/*
 *   Copyright (c) 2024 Project CHIP Authors
 *   All rights reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */
package com.google.chip.chiptool.voice

import chip.devicecontroller.model.NodeState
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class HubConnectionManagerTest {
  private class FakeTransport : HubTransport {
    val events = mutableListOf<String>()
    val requests = mutableListOf<SubscribeRequest>()
    /** Nodes whose CASE waits on a deferred; others connect at once. */
    val pendingCase = mutableMapOf<Long, CompletableDeferred<Long>>()
    var descriptorReport: NodeState? = null

    override suspend fun connect(nodeId: Long): Long {
      events += "connect:$nodeId"
      return pendingCase[nodeId]?.await() ?: (nodeId * 100)
    }

    override suspend fun readDescriptor(devicePtr: Long, nodeId: Long): NodeState? {
      events += "descriptor:$nodeId"
      return descriptorReport
    }

    override fun subscribe(request: SubscribeRequest) {
      events += "subscribe:${request.nodeId}"
      requests += request
    }

    override fun shutdown(nodeId: Long) {
      events += "shutdown:$nodeId"
    }

    fun lastRequest(nodeId: Long): SubscribeRequest = requests.last { it.nodeId == nodeId }

    fun count(event: String): Int = events.count { it == event }
  }

  private lateinit var registry: CommissionedNodeRegistry
  private lateinit var transport: FakeTransport
  private val dispatcher = StandardTestDispatcher()
  private val testScope = TestScope(dispatcher)

  private val light =
    CommissionedNode(
      nodeId = 3L,
      nodeLabel = "Light",
      roomName = "Living Room",
      endpoints =
        listOf(CommissionedEndpoint(1, 0x010DL, "Extended Color Light", setOf(0x0006L, 0x0008L))),
      descriptorVerified = true,
    )

  private val sensor =
    CommissionedNode(
      nodeId = 1L,
      nodeLabel = "W100",
      roomName = "Bedroom",
      endpoints =
        listOf(
          CommissionedEndpoint(0, 0x0016L, "Root Node", setOf(0x0028L, 0x0046L)),
          CommissionedEndpoint(1, 0x0302L, "Temperature Sensor", setOf(0x0402L, 0x0405L)),
          CommissionedEndpoint(2, 0x000FL, "Generic Switch", setOf(0x003BL)),
        ),
      descriptorVerified = true,
    )

  @Before
  fun setUp() {
    System.setProperty(
      "java.io.tmpdir",
      java.nio.file.Files.createTempDirectory("hub_link").toFile().absolutePath,
    )
    HomeStateCache.clear()
    registry = CommissionedNodeRegistry()
    registry.registerNode(light)
    registry.registerNode(sensor)
    transport = FakeTransport()
    HubConnectionManager.resetForTest(registry, transport, dispatcher)
    HubConnectionManager.clock = { testScope.testScheduler.currentTime + 1_000_000L }
  }

  @After
  fun tearDown() {
    HubConnectionManager.resetForTest(null, null)
    HomeStateCache.clear()
  }

  private fun phase(nodeId: Long) = HubConnectionManager.linkState(nodeId).phase

  @Test
  fun noSubscriptionWithoutLease() = testScope.runTest {
    assertFalse(HubConnectionManager.connectAll())
    assertFalse(HubConnectionManager.connect(3L))
    HubConnectionManager.onNodeCommissioned(3L)
    advanceUntilIdle()
    assertTrue(
      "no transport traffic without a lease: ${transport.events}",
      transport.events.isEmpty(),
    )
    assertEquals(LinkPhase.NOT_CONNECTED, phase(3L))
  }

  @Test
  fun acquireOpensExactlyOneSubscriptionPerNode() = testScope.runTest {
    HubConnectionManager.acquire(null, "hub")
    advanceUntilIdle()

    for (nodeId in listOf(1L, 3L)) {
      assertEquals(1, transport.count("subscribe:$nodeId"))
      val shutdownAt = transport.events.indexOf("shutdown:$nodeId")
      val subscribeAt = transport.events.indexOf("subscribe:$nodeId")
      assertTrue("shutdown before subscribe for $nodeId", shutdownAt in 0 until subscribeAt)
      assertFalse(transport.lastRequest(nodeId).keepSubscriptions)
      assertEquals(LinkPhase.PRIMING, phase(nodeId))
    }
    assertEquals(60, transport.lastRequest(3L).maxInterval)
    assertEquals(300, transport.lastRequest(1L).maxInterval)
    assertEquals(1, transport.lastRequest(1L).eventPaths.size)

    transport.lastRequest(3L).established.onSubscriptionEstablished(42L)
    assertEquals(LinkPhase.LIVE, phase(3L))
    assertEquals(42L, HubConnectionManager.linkState(3L).subscriptionId)
  }

  @Test
  fun onDoneNeverReportsLive() = testScope.runTest {
    HubConnectionManager.acquire(null, "hub")
    advanceUntilIdle()
    val req = transport.lastRequest(3L)

    req.report.onDone()
    assertEquals(LinkPhase.OFFLINE, phase(3L))

    // Establishing again after onDone is still driven only by the established callback.
    req.established.onSubscriptionEstablished(1L)
    assertEquals(LinkPhase.LIVE, phase(3L))
    req.report.onDone()
    assertEquals(LinkPhase.OFFLINE, phase(3L))
  }

  @Test
  fun callbacksFromAnOlderGenerationAreDropped() = testScope.runTest {
    HubConnectionManager.acquire(null, "hub")
    advanceUntilIdle()
    val oldRequest = transport.lastRequest(3L)
    oldRequest.established.onSubscriptionEstablished(1L)

    assertTrue(HubConnectionManager.connectAll())
    advanceUntilIdle()
    val newRequest = transport.lastRequest(3L)
    assertTrue(oldRequest !== newRequest)
    assertEquals(LinkPhase.PRIMING, phase(3L))

    // The old ReadClient reports while being shut down: none of it may touch the new link.
    oldRequest.report.onDone()
    oldRequest.established.onSubscriptionEstablished(1L)
    oldRequest.report.onError(null, null, Exception("old"))
    oldRequest.resubscription.onResubscriptionAttempt(0L, 1000L)
    assertEquals(LinkPhase.PRIMING, phase(3L))

    newRequest.established.onSubscriptionEstablished(2L)
    assertEquals(LinkPhase.LIVE, phase(3L))
    assertEquals(2L, HubConnectionManager.linkState(3L).subscriptionId)
  }

  @Test
  fun connectAllRedoesLiveSubscriptionShutdownFirstWithEventMin() = testScope.runTest {
    HubConnectionManager.acquire(null, "hub")
    advanceUntilIdle()
    transport.lastRequest(3L).established.onSubscriptionEstablished(1L)
    transport.lastRequest(1L).established.onSubscriptionEstablished(2L)
    HomeStateCache.putSwitchEvent(nodeId = 1L, endpointId = 2, eventId = 0x06L, eventNumber = 41L)
    transport.events.clear()

    assertTrue(HubConnectionManager.connectAll())
    advanceUntilIdle()

    for (nodeId in listOf(1L, 3L)) {
      val order =
        transport.events.filter { it.endsWith(":$nodeId") }.map { it.substringBefore(":") }
      assertEquals(listOf("shutdown", "connect", "subscribe"), order)
    }
    assertEquals(42L, transport.lastRequest(1L).eventMin)
    assertNull(transport.lastRequest(3L).eventMin)
  }

  @Test
  fun duplicateConnectAllPressesAreIgnoredWhileInFlight() = testScope.runTest {
    HubConnectionManager.acquire(null, "hub")
    advanceUntilIdle()
    transport.lastRequest(3L).established.onSubscriptionEstablished(1L)
    transport.lastRequest(1L).established.onSubscriptionEstablished(2L)
    val gate = CompletableDeferred<Long>()
    transport.pendingCase[1L] = gate

    assertTrue(HubConnectionManager.connectAll())
    runCurrent()
    assertFalse("second press joins the batch", HubConnectionManager.connectAll())
    assertTrue(HubConnectionManager.connectAllProgress.value!!.active)

    gate.complete(100L)
    advanceUntilIdle()
    assertEquals(2, transport.count("subscribe:1"))
    assertEquals(2, transport.count("subscribe:3"))

    transport.lastRequest(1L).established.onSubscriptionEstablished(3L)
    transport.lastRequest(3L).established.onSubscriptionEstablished(4L)
    val progress = HubConnectionManager.connectAllProgress.value!!
    assertFalse(progress.active)
    assertEquals(2, progress.live)
    assertEquals(2, progress.total)
  }

  @Test
  fun releaseTearsDownEveryNodeAndStopsRetries() = testScope.runTest {
    HubConnectionManager.acquire(null, "hub")
    advanceUntilIdle()
    val req = transport.lastRequest(3L)
    req.established.onSubscriptionEstablished(1L)
    transport.events.clear()

    HubConnectionManager.release("hub")
    advanceUntilIdle()

    assertEquals(listOf("shutdown:1", "shutdown:3"), transport.events.sorted())
    assertEquals(LinkPhase.NOT_CONNECTED, phase(1L))
    assertEquals(LinkPhase.NOT_CONNECTED, phase(3L))
    assertNotNull(HubConnectionManager.linkState(3L).lastHeardMs)

    // Late callbacks from the torn-down ReadClient change nothing and start nothing.
    req.resubscription.onResubscriptionAttempt(0L, 1000L)
    req.report.onDone()
    advanceTimeBy(120_000L)
    assertEquals(LinkPhase.NOT_CONNECTED, phase(3L))
    assertEquals(listOf("shutdown:1", "shutdown:3"), transport.events.sorted())
    assertFalse(HubConnectionManager.connect(3L))
  }

  @Test
  fun reacquireBySameOwnerIsANoOp() = testScope.runTest {
    HubConnectionManager.acquire(null, "hub")
    advanceUntilIdle()
    HubConnectionManager.acquire(null, "hub")
    advanceUntilIdle()
    assertEquals(1, transport.count("subscribe:3"))
    assertEquals(1, transport.count("subscribe:1"))
  }

  @Test
  fun teardownDuringCaseNeverSubscribes() = testScope.runTest {
    val gate = CompletableDeferred<Long>()
    transport.pendingCase[3L] = gate
    HubConnectionManager.acquire(null, "hub")
    runCurrent()
    assertEquals(LinkPhase.CONNECTING, phase(3L))

    HubConnectionManager.release("hub")
    gate.complete(300L)
    advanceUntilIdle()

    assertEquals(0, transport.count("subscribe:3"))
    assertEquals(LinkPhase.NOT_CONNECTED, phase(3L))
  }

  @Test
  fun caseSoftDeadlineIsTenSecondsMainsAndThirtyIcd() = testScope.runTest {
    transport.pendingCase[3L] = CompletableDeferred()
    transport.pendingCase[1L] = CompletableDeferred()
    HubConnectionManager.acquire(null, "hub")
    runCurrent()

    advanceTimeBy(9_000L)
    assertEquals(LinkPhase.CONNECTING, phase(3L))
    advanceTimeBy(2_000L)
    assertEquals(LinkPhase.RECOVERING, phase(3L))
    assertEquals(LinkPhase.CONNECTING, phase(1L))
    advanceTimeBy(20_000L)
    assertEquals(LinkPhase.ASLEEP, phase(1L))

    // A late CASE success still subscribes because the generation did not change.
    transport.pendingCase.getValue(3L).complete(300L)
    runCurrent()
    assertEquals(1, transport.count("subscribe:3"))
    assertEquals(LinkPhase.PRIMING, phase(3L))

    // A hard CASE failure on an ICD keeps the neutral ASLEEP state.
    transport.pendingCase.getValue(1L).completeExceptionally(IllegalStateException("no route"))
    runCurrent()
    assertEquals(LinkPhase.ASLEEP, phase(1L))
    HubConnectionManager.release("hub")
  }

  @Test
  fun hardCaseFailureMarksMainsOffline() = testScope.runTest {
    val gate = CompletableDeferred<Long>()
    transport.pendingCase[3L] = gate
    HubConnectionManager.acquire(null, "hub")
    runCurrent()
    gate.completeExceptionally(IllegalStateException("Unable to get connected device"))
    runCurrent()
    assertEquals(LinkPhase.OFFLINE, phase(3L))
    assertEquals("Couldn't reach", HubConnectionManager.linkState(3L).reason)
    HubConnectionManager.release("hub")
  }

  @Test
  fun resubscribeAttemptsMapToRecoveringOfflineOrAsleep() = testScope.runTest {
    HubConnectionManager.acquire(null, "hub")
    advanceUntilIdle()
    val lightReq = transport.lastRequest(3L)
    lightReq.established.onSubscriptionEstablished(1L)
    lightReq.resubscription.onResubscriptionAttempt(0L, 2000L)
    assertEquals(LinkPhase.RECOVERING, phase(3L))
    lightReq.resubscription.onResubscriptionAttempt(0L, 4000L)
    assertEquals(LinkPhase.OFFLINE, phase(3L))
    lightReq.established.onSubscriptionEstablished(2L)
    assertEquals(LinkPhase.LIVE, phase(3L))

    val sensorReq = transport.lastRequest(1L)
    sensorReq.established.onSubscriptionEstablished(3L)
    sensorReq.resubscription.onResubscriptionAttempt(0L, 2000L)
    assertEquals(LinkPhase.ASLEEP, phase(1L))
  }

  @Test
  fun subscriptionErrorDoesNotShutDownAndMarksOffline() = testScope.runTest {
    HubConnectionManager.acquire(null, "hub")
    advanceUntilIdle()
    val req = transport.lastRequest(3L)
    req.established.onSubscriptionEstablished(1L)
    transport.events.clear()

    req.report.onError(
      chip.devicecontroller.model.ChipAttributePath.newInstance(
        chip.devicecontroller.model.ChipPathId.forId(1L),
        chip.devicecontroller.model.ChipPathId.forId(6L),
        chip.devicecontroller.model.ChipPathId.forId(0L),
      ),
      null,
      Exception("path"),
    )
    assertEquals(LinkPhase.LIVE, phase(3L))

    req.report.onError(null, null, Exception("gave up"))
    advanceUntilIdle()
    assertEquals(LinkPhase.OFFLINE, phase(3L))
    assertTrue("no shutdown on error: ${transport.events}", transport.events.isEmpty())
  }

  @Test
  fun descriptorIsReadBeforeTheSingleSubscribe() = testScope.runTest {
    registry.registerNode(light.copy(descriptorVerified = false))
    HubConnectionManager.acquire(null, "hub")
    advanceUntilIdle()
    val order = transport.events.filter { it.endsWith(":3") }.map { it.substringBefore(":") }
    assertEquals(listOf("shutdown", "connect", "descriptor", "subscribe"), order)
  }

  @Test
  fun cancelStopsPendingNodesAndKeepsLiveOnes() = testScope.runTest {
    HubConnectionManager.acquire(null, "hub")
    advanceUntilIdle()
    transport.pendingCase[1L] = CompletableDeferred()
    assertTrue(HubConnectionManager.connectAll())
    runCurrent()
    transport.lastRequest(3L).established.onSubscriptionEstablished(5L)
    assertEquals(LinkPhase.LIVE, phase(3L))
    assertTrue(HubConnectionManager.connectAllProgress.value!!.active)

    HubConnectionManager.cancelConnectAll()
    runCurrent()
    assertEquals(LinkPhase.LIVE, phase(3L))
    assertEquals(LinkPhase.NOT_CONNECTED, phase(1L))
    assertEquals("Cancelled", HubConnectionManager.linkState(1L).reason)
    val progress = HubConnectionManager.connectAllProgress.value!!
    assertFalse(progress.active)
    assertTrue(progress.cancelled)
    HubConnectionManager.release("hub")
  }

  @Test
  fun perDeviceConnectOnlyStartsWhenNotUp() = testScope.runTest {
    HubConnectionManager.acquire(null, "hub")
    advanceUntilIdle()
    val req = transport.lastRequest(3L)
    req.established.onSubscriptionEstablished(1L)
    assertFalse(HubConnectionManager.connect(3L))

    req.report.onError(null, null, Exception("gone"))
    assertEquals(LinkPhase.OFFLINE, phase(3L))
    assertTrue(HubConnectionManager.connect(3L))
    advanceUntilIdle()
    assertEquals(2, transport.count("subscribe:3"))
  }

  @Test
  fun successfulCommandMarksReachableAndReconnectsOfflineNode() = testScope.runTest {
    HubConnectionManager.acquire(null, "hub")
    advanceUntilIdle()
    transport.lastRequest(3L).report.onError(null, null, Exception("gone"))
    assertEquals(LinkPhase.OFFLINE, phase(3L))

    HubConnectionManager.noteReachable(3L)
    advanceUntilIdle()
    assertEquals(2, transport.count("subscribe:3"))
    assertNotNull(HubConnectionManager.linkState(3L).lastHeardMs)
  }

  @Test
  fun cacheAgeIsZeroWhileLive() = testScope.runTest {
    val now = HubConnectionManager.clock()
    assertEquals(600_000L, HubConnectionManager.cacheAgeMs(3L, now - 600_000L))
    HubConnectionManager.acquire(null, "hub")
    advanceUntilIdle()
    transport.lastRequest(3L).established.onSubscriptionEstablished(1L)
    assertEquals(0L, HubConnectionManager.cacheAgeMs(3L, now - 600_000L))
  }
}
