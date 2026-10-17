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

import java.io.File
import java.nio.file.Files
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Before
import org.junit.Test

class HomeStateCacheTest {
  private lateinit var tempDir: File

  @Before
  fun setUp() {
    System.setProperty(
      "java.io.tmpdir",
      Files.createTempDirectory("chiptool_cache").toFile().absolutePath,
    )
    HomeStateCache.init(null)
  }

  @After
  fun tearDown() {
    HomeStateCache.clear()
  }

  @Test
  fun testMonotonicUpdate() {
    HomeStateCache.put(1L, 1, 0x0402L, 0L, 2200, 1000L, Source.SUBSCRIPTION_REPORT)
    val first = HomeStateCache.get(1L, 1, 0x0402L, 0L)
    assertNotNull(first)
    assertEquals(2200, first!!.value)

    // Attempt an older timestamp update - should be ignored
    HomeStateCache.put(1L, 1, 0x0402L, 0L, 2100, 500L, Source.SUBSCRIPTION_REPORT)
    val ignored = HomeStateCache.get(1L, 1, 0x0402L, 0L)
    assertEquals(2200, ignored!!.value)

    // Newer timestamp should update
    HomeStateCache.put(1L, 1, 0x0402L, 0L, 2300, 2000L, Source.UNICAST_READ)
    val second = HomeStateCache.get(1L, 1, 0x0402L, 0L)
    assertEquals(2300, second!!.value)
  }

  @Test
  fun testSnapshotRoundTrip() {
    HomeStateCache.put(2L, 1, 0x0402L, 0L, 2500, 3000L, Source.SUBSCRIPTION_REPORT)
    val file = File(System.getProperty("java.io.tmpdir"), "home_state_snapshot.json")
    file.writeText(
      """
      [
        {"nodeId":3,"endpointId":1,"clusterId":1026,"attributeId":0,"value":2100,"updatedAtMs":12345,"source":"SUBSCRIPTION_REPORT"}
      ]
      """
        .trimIndent()
    )

    HomeStateCache.init(null)
    Thread.sleep(300)
    val loaded = HomeStateCache.get(3L, 1, 1026L, 0L)
    assertNotNull(loaded)
    assertEquals(2100.0, loaded!!.value)
    assertEquals(Source.SNAPSHOT, loaded.source)
  }

  @Test
  fun testSnapshotNeverOverwritesNewerValue() {
    HomeStateCache.put(4L, 1, 0x0006L, 0L, false, 50_000L, Source.SUBSCRIPTION_REPORT)
    val file = File(System.getProperty("java.io.tmpdir"), "home_state_snapshot.json")
    file.writeText(
      """
      [
        {"nodeId":4,"endpointId":1,"clusterId":6,"attributeId":0,"value":true,"updatedAtMs":10000,"source":"SUBSCRIPTION_REPORT"}
      ]
      """
        .trimIndent()
    )

    HomeStateCache.init(null)
    Thread.sleep(300)
    val current = HomeStateCache.get(4L, 1, 0x0006L, 0L)
    assertEquals(false, current!!.value)
    assertEquals(Source.SUBSCRIPTION_REPORT, current.source)
  }

  @Test
  fun testConcurrentUpdates() {
    val threads =
      (1..8).map { i ->
        Thread {
          for (j in 1..100) {
            HomeStateCache.put(
              1L,
              1,
              0x0402L,
              0L,
              j,
              System.currentTimeMillis(),
              Source.SUBSCRIPTION_REPORT,
            )
          }
        }
      }
    threads.forEach { it.start() }
    threads.forEach { it.join() }
    val result = HomeStateCache.get(1L, 1, 0x0402L, 0L)
    assertNotNull(result)
  }
}
