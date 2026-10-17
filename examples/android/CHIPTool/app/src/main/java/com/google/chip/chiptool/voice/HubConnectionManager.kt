package com.google.chip.chiptool.voice

import android.content.Context
import chip.devicecontroller.ChipEventStructs
import chip.devicecontroller.ReportCallback
import chip.devicecontroller.ResubscriptionAttemptCallback
import chip.devicecontroller.SubscriptionEstablishedCallback
import chip.devicecontroller.model.ChipAttributePath
import chip.devicecontroller.model.ChipEventPath
import chip.devicecontroller.model.ChipPathId
import chip.devicecontroller.model.EventState
import chip.devicecontroller.model.NodeState
import com.google.chip.chiptool.ChipClient
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicLong
import kotlin.coroutines.resume
import kotlin.math.max
import kotlin.math.min
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withTimeoutOrNull
import matter.tlv.AnonymousTag
import matter.tlv.ContextSpecificTag
import matter.tlv.TlvReader

/** Connection phase of one node, as shown by the Device Hub. */
enum class LinkPhase {
  NOT_CONNECTED,
  CONNECTING,
  PRIMING,
  LIVE,
  RECOVERING,
  ASLEEP,
  OFFLINE,
}

/**
 * Link state of one node. [lastHeardMs] is the liveness watermark: the last time the node was
 * proven reachable (subscription established, report received, successful command or read, or the
 * moment a live subscription was torn down).
 */
data class LinkState(
  val phase: LinkPhase = LinkPhase.NOT_CONNECTED,
  val generation: Long = 0L,
  val sinceMs: Long = 0L,
  val lastHeardMs: Long? = null,
  val reason: String? = null,
  val isIcd: Boolean = false,
  val subscriptionId: Long? = null,
)

/** Progress of one Connect All press. */
data class ConnectAllProgress(
  val total: Int,
  val settled: Int,
  val live: Int,
  val active: Boolean,
  val cancelled: Boolean = false,
)

/** One subscribe request handed to the transport. */
data class SubscribeRequest(
  val nodeId: Long,
  val devicePtr: Long,
  val attributePaths: List<ChipAttributePath>,
  val eventPaths: List<ChipEventPath>,
  val minInterval: Int,
  val maxInterval: Int,
  val keepSubscriptions: Boolean,
  val eventMin: Long?,
  val imTimeoutMs: Int,
  val established: SubscriptionEstablishedCallback,
  val resubscription: ResubscriptionAttemptCallback,
  val report: ReportCallback,
)

/** Seam between the link state machine and the Matter controller. Tests use a fake. */
interface HubTransport {
  /** Opens (or reuses) a CASE session and returns the device pointer. Throws on failure. */
  suspend fun connect(nodeId: Long): Long

  /** Reads the Descriptor cluster once. Returns null if the read failed. */
  suspend fun readDescriptor(devicePtr: Long, nodeId: Long): NodeState?

  fun subscribe(request: SubscribeRequest)

  fun shutdown(nodeId: Long)
}

/**
 * Process-wide owner of the commissioned-node registry and of every node subscription.
 *
 * Subscriptions exist only while a lease is held (the Device Hub is visible). Each node has a link
 * with a generation counter; every SDK callback carries the generation it was created with and is
 * dropped when the link has moved on. The app retries only CASE; once subscribeToPath has been
 * called, the SDK resubscribe logic owns retries until the link is torn down.
 */
object HubConnectionManager {
  private const val TAG = "HubLink"

  const val CASE_DEADLINE_MAINS_MS = 10_000L
  const val CASE_DEADLINE_ICD_MS = 30_000L
  private const val PRIME_TIMEOUT_MAINS_MS = 10_000
  private const val PRIME_TIMEOUT_ICD_MS = 30_000
  private const val MAX_INTERVAL_MAINS_S = 60
  private const val MAX_INTERVAL_ICD_S = 300
  private const val MAX_PARALLEL_CASE = 4
  private const val MAINS_RESUBSCRIBE_ATTEMPTS_BEFORE_OFFLINE = 2
  private const val CASE_RETRY_BASE_MS = 2_000L
  private const val CASE_RETRY_MAX_MS = 60_000L
  private const val CLUSTER_ICD_MANAGEMENT = 0x0046L

  private val VOICE_CLUSTERS =
    listOf(
      0x0402L, // TempMeasurement
      0x0405L, // RelHumidity
      0x0006L, // OnOff
      0x0201L, // Thermostat
      0x0008L, // Level
      0x0045L, // BooleanState
      0x0406L, // Occupancy
    )

  private enum class Mode {
    /** Hub became visible: keep links that are up or on their way up. */
    ENTER,
    /** Connect All: redo every link, joining links that are already connecting. */
    CONNECT_ALL,
    /** Per-device Connect: start a link unless it is up or on its way up. */
    SINGLE,
  }

  private class NodeLink {
    val mutex = Mutex()
    val gen = AtomicLong(0L)
    @Volatile var job: Job? = null
    @Volatile var resubscribeAttempts = 0
  }

  private val stateLock = Any()
  @Volatile private var appContext: Context? = null

  fun appContextOrNull(): Context? = appContext

  @Volatile private var sharedRegistry: CommissionedNodeRegistry? = null
  @Volatile private var transportOverride: HubTransport? = null
  @Volatile private var chipTransport: HubTransport? = null
  @Volatile private var scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
  @Volatile private var caseSemaphore = Semaphore(MAX_PARALLEL_CASE)
  private val links = ConcurrentHashMap<Long, NodeLink>()
  private val leases = mutableSetOf<String>()
  private var batchNodes: Set<Long> = emptySet()

  private val _linkStates = MutableStateFlow<Map<Long, LinkState>>(emptyMap())
  val linkStates: StateFlow<Map<Long, LinkState>> = _linkStates.asStateFlow()

  private val _connectAllProgress = MutableStateFlow<ConnectAllProgress?>(null)
  val connectAllProgress: StateFlow<ConnectAllProgress?> = _connectAllProgress.asStateFlow()

  /** Wall clock, replaceable in tests. */
  @Volatile var clock: () -> Long = { System.currentTimeMillis() }

  // ---------------------------------------------------------------------------------------------
  // Registry
  // ---------------------------------------------------------------------------------------------

  /** Returns the single registry shared by the Hub, the voice engine and commissioning. */
  fun registry(context: Context?): CommissionedNodeRegistry {
    sharedRegistry?.let {
      return it
    }
    synchronized(stateLock) {
      sharedRegistry?.let {
        return it
      }
      val app = context?.applicationContext ?: context
      if (app != null) {
        appContext = app
        HomeStateCache.init(app)
      }
      val reg = CommissionedNodeRegistry(app)
      sharedRegistry = reg
      return reg
    }
  }

  private fun registryOrNull(): CommissionedNodeRegistry? = sharedRegistry

  // ---------------------------------------------------------------------------------------------
  // Public state
  // ---------------------------------------------------------------------------------------------

  fun linkState(nodeId: Long): LinkState = _linkStates.value[nodeId] ?: LinkState()

  fun isHubActive(): Boolean = synchronized(stateLock) { leases.isNotEmpty() }

  /**
   * Age of a cached value, judged by the link's liveness watermark rather than by when the value
   * last changed. Returns 0 while the node is live (the subscription vouches for the value).
   */
  fun cacheAgeMs(nodeId: Long, updatedAtMs: Long): Long {
    val state = linkState(nodeId)
    if (state.phase == LinkPhase.LIVE) return 0L
    val verifiedAt = max(updatedAtMs, state.lastHeardMs ?: 0L)
    return max(0L, clock() - verifiedAt)
  }

  // ---------------------------------------------------------------------------------------------
  // Lease (Hub visibility)
  // ---------------------------------------------------------------------------------------------

  /**
   * Called when the Device Hub becomes visible. Leases are keyed by owner, so re-acquiring after a
   * configuration change is a no-op.
   */
  fun acquire(context: Context?, owner: String) {
    val reg = registry(context)
    val first =
      synchronized(stateLock) {
        val added = leases.add(owner)
        added && leases.size == 1
      }
    if (!first) {
      log("hub acquire owner=$owner ignored (lease already held)")
      return
    }
    val nodeIds = reg.getAllNodes().map { it.nodeId }.sorted()
    log("hub acquire owner=$owner nodes=$nodeIds")
    for (nodeId in nodeIds) startLink(nodeId, Mode.ENTER)
  }

  /** Called when the Device Hub stops being visible. Tears every subscription down at once. */
  fun release(owner: String) {
    val last = synchronized(stateLock) { leases.remove(owner) && leases.isEmpty() }
    if (!last) return
    log("hub release owner=$owner -> teardown")
    teardownAll("hub released")
  }

  // ---------------------------------------------------------------------------------------------
  // Connect All / Connect / Cancel
  // ---------------------------------------------------------------------------------------------

  /**
   * (Re)subscribes every commissioned node. Links that are already connecting are joined; every
   * other link is shut down and subscribed again. Returns false if ignored (no lease, or a Connect
   * All is already in flight).
   */
  fun connectAll(): Boolean {
    val reg = registryOrNull() ?: return false
    val nodeIds = reg.getAllNodes().map { it.nodeId }.sorted()
    synchronized(stateLock) {
      if (leases.isEmpty()) {
        log("connectAll ignored: hub not visible")
        return false
      }
      if (_connectAllProgress.value?.active == true) {
        log("connectAll ignored: already in flight")
        return false
      }
      batchNodes = nodeIds.toSet()
      _connectAllProgress.value =
        ConnectAllProgress(nodeIds.size, 0, 0, active = nodeIds.isNotEmpty())
    }
    log("connectAll start nodes=$nodeIds")
    for (nodeId in nodeIds) startLink(nodeId, Mode.CONNECT_ALL)
    updateProgress()
    return true
  }

  /** Per-device Connect. Returns false if the link was already up or on its way up. */
  fun connect(nodeId: Long): Boolean = startLink(nodeId, Mode.SINGLE)

  /** Stops every node of the current Connect All that has not reached LIVE. */
  fun cancelConnectAll() {
    val nodes =
      synchronized(stateLock) {
        if (_connectAllProgress.value?.active != true) return
        batchNodes
      }
    log("connectAll cancel nodes=$nodes")
    for (nodeId in nodes) {
      if (linkState(nodeId).phase != LinkPhase.LIVE) teardownNode(nodeId, "cancelled")
    }
    synchronized(stateLock) {
      _connectAllProgress.value = _connectAllProgress.value?.copy(active = false, cancelled = true)
    }
  }

  /** A successful command or read proves the node is reachable right now. */
  fun noteReachable(nodeId: Long) {
    val phase =
      synchronized(stateLock) {
        val current = _linkStates.value[nodeId] ?: LinkState()
        put(nodeId, current.copy(lastHeardMs = clock()))
        current.phase
      }
    if (phase == LinkPhase.OFFLINE || phase == LinkPhase.ASLEEP) {
      log("link node=$nodeId reachable via command while $phase -> reconnect")
      startLink(nodeId, Mode.SINGLE)
    }
  }

  fun onNodeCommissioned(nodeId: Long) {
    log("node commissioned node=$nodeId hubActive=${isHubActive()}")
    startLink(nodeId, Mode.SINGLE)
  }

  fun onNodeRemoved(nodeId: Long) {
    teardownNode(nodeId, "node removed")
    synchronized(stateLock) {
      _linkStates.value = _linkStates.value - nodeId
      batchNodes = batchNodes - nodeId
    }
    links.remove(nodeId)
  }

  fun ensureDescriptorDiscovered(nodeId: Long, devicePtr: Long? = null) {
    val reg = registryOrNull() ?: return
    val node = reg.getNode(nodeId) ?: return
    if (node.descriptorVerified) return
    val link = links.getOrPut(nodeId) { NodeLink() }
    val myGen = link.gen.get()
    scope.launch {
      try {
        val ptr = devicePtr ?: transport().connect(nodeId)
        val deadline = if (node.isIcd()) CASE_DEADLINE_ICD_MS else CASE_DEADLINE_MAINS_MS
        val report =
          withTimeoutOrNull(deadline) {
            try {
              transport().readDescriptor(ptr, nodeId)
            } catch (e: CancellationException) {
              throw e
            } catch (e: Exception) {
              null
            }
          }
        if (report != null && link.gen.get() == myGen) {
          val verified = reg.applyDescriptorReport(appContext, nodeId, report)
          log("descriptor node=$nodeId gen=$myGen verified=$verified")
        }
      } catch (e: Exception) {
        log("descriptor discovery failed node=$nodeId gen=$myGen: ${e.message}")
      }
    }
  }

  // ---------------------------------------------------------------------------------------------
  // Link state machine
  // ---------------------------------------------------------------------------------------------

  private fun startLink(nodeId: Long, mode: Mode): Boolean {
    val reg = registryOrNull() ?: return false
    val node = reg.getNode(nodeId) ?: return false
    val link = links.getOrPut(nodeId) { NodeLink() }
    var myGen = -1L
    var kept: LinkState? = null
    synchronized(stateLock) {
      if (leases.isEmpty()) return false
      val current = _linkStates.value[nodeId] ?: LinkState()
      val inFlight = current.phase == LinkPhase.CONNECTING || current.phase == LinkPhase.PRIMING
      val keep =
        when (mode) {
          Mode.ENTER ->
            inFlight || current.phase == LinkPhase.LIVE || current.phase == LinkPhase.RECOVERING
          Mode.CONNECT_ALL -> inFlight
          Mode.SINGLE -> inFlight || current.phase == LinkPhase.LIVE
        }
      if (keep) {
        kept = current
        return@synchronized
      }
      myGen = link.gen.incrementAndGet()
      link.job?.cancel()
      link.resubscribeAttempts = 0
      put(
        nodeId,
        LinkState(
          phase = LinkPhase.CONNECTING,
          generation = myGen,
          sinceMs = clock(),
          lastHeardMs = current.lastHeardMs,
          isIcd = node.isIcd(),
        ),
      )
      link.job = scope.launch { runLink(nodeId, link, myGen) }
    }
    kept?.let {
      log("link node=$nodeId keep ${it.phase} gen=${it.generation} mode=$mode")
      return false
    }
    log("link node=$nodeId CONNECTING gen=$myGen mode=$mode")
    updateProgress()
    return true
  }

  private suspend fun runLink(nodeId: Long, link: NodeLink, myGen: Long) {
    // 1. Shut down whatever an older generation (or another screen) left behind, so that at most
    //    one ReadClient exists for this node.
    link.mutex.withLock {
      if (link.gen.get() != myGen) return
      transport().shutdown(nodeId)
      log("sub_shutdown node=$nodeId gen=$myGen")
    }

    // 2. CASE, retried by the app (no ReadClient exists yet).
    var attempt = 0
    var devicePtr: Long
    while (true) {
      val node = registryOrNull()?.getNode(nodeId) ?: return
      val icd = node.isIcd()
      val deadline = if (icd) CASE_DEADLINE_ICD_MS else CASE_DEADLINE_MAINS_MS
      try {
        devicePtr = connectWithSoftDeadline(nodeId, myGen, icd, deadline)
        break
      } catch (e: CancellationException) {
        throw e
      } catch (e: Exception) {
        val backoff = min(CASE_RETRY_BASE_MS shl min(attempt, 5), CASE_RETRY_MAX_MS)
        signal(nodeId, myGen, "caseFailure") {
          it.copy(
            phase = if (icd) LinkPhase.ASLEEP else LinkPhase.OFFLINE,
            reason = "Couldn't reach",
          )
        }
        log("link node=$nodeId CASE failed gen=$myGen attempt=$attempt retry_in_ms=$backoff")
        attempt++
        delay(backoff)
        if (link.gen.get() != myGen) return
      }
    }

    // 3. Descriptor first (a read, not a subscription) so that one subscribe carries full paths.
    var node = registryOrNull()?.getNode(nodeId) ?: return
    if (!node.descriptorVerified) {
      val deadline = if (node.isIcd()) CASE_DEADLINE_ICD_MS else CASE_DEADLINE_MAINS_MS
      val report =
        withTimeoutOrNull(deadline) {
          try {
            transport().readDescriptor(devicePtr, nodeId)
          } catch (e: CancellationException) {
            throw e
          } catch (e: Exception) {
            null
          }
        }
      if (link.gen.get() != myGen) return
      val verified =
        report != null &&
          registryOrNull()?.applyDescriptorReport(appContext, nodeId, report) == true
      log("descriptor node=$nodeId gen=$myGen verified=$verified")
      node = registryOrNull()?.getNode(nodeId) ?: return
    }

    // 4. One subscription, issued under the node mutex after re-checking the generation so that a
    //    teardown that raced with CASE can never leave an orphan ReadClient.
    link.mutex.withLock {
      if (link.gen.get() != myGen || !isHubActive()) {
        log("drop stale device pointer node=$nodeId gen=$myGen current=${link.gen.get()}")
        return
      }
      val icd = node.isIcd()
      val (paths, eventPaths) = buildPaths(node)
      val maxInterval = if (icd) MAX_INTERVAL_ICD_S else MAX_INTERVAL_MAINS_S
      val eventMin = HomeStateCache.lastSwitchEventNumber(nodeId)?.plus(1)
      signal(nodeId, myGen, "caseSuccess") {
        it.copy(phase = LinkPhase.PRIMING, isIcd = icd, lastHeardMs = clock(), reason = null)
      }
      log(
        "link node=$nodeId PRIMING gen=$myGen paths=${paths.size} eventPaths=${eventPaths.size} " +
          "maxInterval=$maxInterval eventMin=$eventMin"
      )
      try {
        transport()
          .subscribe(
            SubscribeRequest(
              nodeId = nodeId,
              devicePtr = devicePtr,
              attributePaths = paths,
              eventPaths = eventPaths,
              minInterval = 0,
              maxInterval = maxInterval,
              keepSubscriptions = false,
              eventMin = eventMin,
              imTimeoutMs = if (icd) PRIME_TIMEOUT_ICD_MS else PRIME_TIMEOUT_MAINS_MS,
              established = establishedCallback(nodeId, myGen),
              resubscription = resubscriptionCallback(nodeId, myGen, icd),
              report = reportCallback(nodeId, myGen),
            )
          )
      } catch (e: Exception) {
        logE("subscribe failed node=$nodeId gen=$myGen", e)
        signal(nodeId, myGen, "subscribeFailure") {
          it.copy(phase = LinkPhase.OFFLINE, reason = "Subscribe failed")
        }
      }
    }
  }

  /**
   * Runs CASE with a soft UI deadline: when the deadline passes the node is shown as ASLEEP (ICD)
   * or OFFLINE (mains), but the native attempt is allowed to finish and a late success still
   * subscribes if the generation is unchanged.
   */
  private suspend fun connectWithSoftDeadline(
    nodeId: Long,
    gen: Long,
    icd: Boolean,
    deadlineMs: Long,
  ): Long = coroutineScope {
    val case = async { caseSemaphore.withPermit { transport().connect(nodeId) } }
    val early = withTimeoutOrNull(deadlineMs) { case.await() }
    if (early != null) return@coroutineScope early
    signal(nodeId, gen, "caseDeadline") {
      it.copy(
        phase = if (icd) LinkPhase.ASLEEP else LinkPhase.RECOVERING,
        reason = "No response in ${deadlineMs / 1000}s",
      )
    }
    case.await()
  }

  private fun establishedCallback(nodeId: Long, gen: Long) =
    SubscriptionEstablishedCallback { subscriptionId ->
      links[nodeId]?.let { if (it.gen.get() == gen) it.resubscribeAttempts = 0 }
      signal(nodeId, gen, "onSubscriptionEstablished") {
        it.copy(
          phase = LinkPhase.LIVE,
          lastHeardMs = clock(),
          reason = null,
          subscriptionId = subscriptionId,
        )
      }
    }

  private fun resubscriptionCallback(nodeId: Long, gen: Long, icd: Boolean) =
    ResubscriptionAttemptCallback { _, nextResubscribeIntervalMsec ->
      val link = links[nodeId]
      if (link == null || link.gen.get() != gen) {
        logDrop(nodeId, "onResubscriptionAttempt", gen)
        return@ResubscriptionAttemptCallback
      }
      link.resubscribeAttempts++
      val phase =
        when {
          icd -> LinkPhase.ASLEEP
          link.resubscribeAttempts >= MAINS_RESUBSCRIBE_ATTEMPTS_BEFORE_OFFLINE -> LinkPhase.OFFLINE
          else -> LinkPhase.RECOVERING
        }
      signal(nodeId, gen, "onResubscriptionAttempt") {
        it.copy(phase = phase, reason = "Retrying in ${nextResubscribeIntervalMsec / 1000}s")
      }
    }

  private fun reportCallback(nodeId: Long, gen: Long) =
    object : ReportCallback {
      override fun onError(
        attributePath: ChipAttributePath?,
        eventPath: ChipEventPath?,
        ex: Exception,
      ) {
        if (attributePath != null || eventPath != null) {
          log("path error node=$nodeId gen=$gen path=${attributePath ?: eventPath}: ${ex.message}")
          return
        }
        // The SDK gave up resubscribing. Never shut down here: the ReadClient is already gone.
        signal(nodeId, gen, "onError") {
          it.copy(phase = LinkPhase.OFFLINE, reason = ex.message?.take(80) ?: "Subscription error")
        }
      }

      override fun onReport(nodeState: NodeState) {
        if (!isCurrent(nodeId, gen)) {
          logDrop(nodeId, "onReport", gen)
          return
        }
        signal(nodeId, gen, "onReport") { it.copy(lastHeardMs = clock()) }
        processNodeStateReport(nodeId, nodeState)
      }

      override fun onDone() {
        // onDone only means the ReadClient is gone. It never means the node is online.
        signal(nodeId, gen, "onDone") {
          if (it.phase == LinkPhase.NOT_CONNECTED) it
          else it.copy(phase = LinkPhase.OFFLINE, reason = it.reason ?: "Subscription ended")
        }
      }
    }

  private fun isCurrent(nodeId: Long, gen: Long): Boolean = links[nodeId]?.gen?.get() == gen

  /** Applies [transform] if [gen] is still current. Returns false if the callback was dropped. */
  private fun signal(
    nodeId: Long,
    gen: Long,
    kind: String,
    transform: (LinkState) -> LinkState,
  ): Boolean {
    var before = LinkState()
    var after = before
    var droppedAt: Long? = null
    synchronized(stateLock) {
      val link = links[nodeId]
      if (link == null || link.gen.get() != gen) {
        droppedAt = link?.gen?.get() ?: -1L
      } else {
        before = _linkStates.value[nodeId] ?: LinkState()
        var next = transform(before)
        if (next.phase != before.phase) next = next.copy(sinceMs = clock())
        put(nodeId, next)
        after = next
      }
    }
    droppedAt?.let {
      log("drop stale callback node=$nodeId kind=$kind gen=$gen current=$it")
      return false
    }
    if (after.phase != before.phase) {
      val extra =
        when (after.phase) {
          LinkPhase.LIVE -> " subId=${after.subscriptionId}"
          else -> after.reason?.let { " reason=$it" } ?: ""
        }
      if (after.phase != LinkPhase.PRIMING) {
        log("link node=$nodeId ${after.phase} gen=$gen via=$kind$extra")
      }
      updateProgress()
    }
    return true
  }

  private fun teardownAll(reason: String) {
    val nodeIds =
      (links.keys + (registryOrNull()?.getAllNodes()?.map { it.nodeId } ?: emptyList()))
        .toSortedSet()
    for (nodeId in nodeIds) teardownNode(nodeId, reason)
    synchronized(stateLock) {
      batchNodes = emptySet()
      _connectAllProgress.value = null
    }
  }

  private fun teardownNode(nodeId: Long, reason: String) {
    val link = links.getOrPut(nodeId) { NodeLink() }
    val myGen: Long
    synchronized(stateLock) {
      myGen = link.gen.incrementAndGet()
      link.job?.cancel()
      link.job = null
      val current = _linkStates.value[nodeId] ?: LinkState()
      val heard = if (current.phase == LinkPhase.LIVE) clock() else current.lastHeardMs
      put(
        nodeId,
        LinkState(
          phase = LinkPhase.NOT_CONNECTED,
          generation = myGen,
          sinceMs = clock(),
          lastHeardMs = heard,
          reason = if (reason == "cancelled") "Cancelled" else null,
          isIcd = current.isIcd,
        ),
      )
    }
    log("link node=$nodeId NOT_CONNECTED gen=$myGen reason=$reason")
    val transport = transportOrNull() ?: return
    scope.launch {
      link.mutex.withLock {
        if (link.gen.get() != myGen) return@withLock
        transport.shutdown(nodeId)
        log("sub_shutdown node=$nodeId gen=$myGen")
      }
    }
  }

  private fun updateProgress() {
    var finished: ConnectAllProgress? = null
    synchronized(stateLock) {
      val progress = _connectAllProgress.value ?: return
      if (!progress.active) return
      val states = _linkStates.value
      val nodes = batchNodes
      val settled = nodes.count {
        val phase = states[it]?.phase ?: LinkPhase.NOT_CONNECTED
        phase != LinkPhase.CONNECTING && phase != LinkPhase.PRIMING
      }
      val live = nodes.count { states[it]?.phase == LinkPhase.LIVE }
      val next =
        progress.copy(
          total = nodes.size,
          settled = settled,
          live = live,
          active = settled < nodes.size,
        )
      _connectAllProgress.value = next
      if (!next.active) finished = next
    }
    finished?.let { log("connectAll done live=${it.live}/${it.total}") }
  }

  private fun put(nodeId: Long, state: LinkState) {
    _linkStates.value = _linkStates.value + (nodeId to state)
  }

  // ---------------------------------------------------------------------------------------------
  // Paths and reports
  // ---------------------------------------------------------------------------------------------

  private fun CommissionedNode.isIcd(): Boolean =
    getAllSupportedClusterIds().contains(CLUSTER_ICD_MANAGEMENT)

  internal fun buildPaths(
    node: CommissionedNode
  ): Pair<List<ChipAttributePath>, List<ChipEventPath>> {
    val supportedClusters = node.getAllSupportedClusterIds()
    val paths = mutableListOf<ChipAttributePath>()
    val eventPaths = mutableListOf<ChipEventPath>()
    if (supportedClusters.isEmpty()) {
      paths.add(
        ChipAttributePath.newInstance(
          ChipPathId.forWildcard(),
          ChipPathId.forWildcard(),
          ChipPathId.forWildcard(),
        )
      )
      eventPaths.add(
        ChipEventPath.newInstance(
          ChipPathId.forWildcard(),
          ChipPathId.forId(MatterClusterMetaRegistry.CLUSTER_SWITCH),
          ChipPathId.forWildcard(),
          false,
        )
      )
      return paths to eventPaths
    }
    val extraClusters = mutableListOf<Long>()
    if (supportedClusters.contains(MatterClusterMetaRegistry.CLUSTER_POWER_SOURCE)) {
      extraClusters.add(MatterClusterMetaRegistry.CLUSTER_POWER_SOURCE)
    }
    if (supportedClusters.contains(MatterClusterMetaRegistry.CLUSTER_SWITCH)) {
      extraClusters.add(MatterClusterMetaRegistry.CLUSTER_SWITCH)
    }
    val targetClusters =
      (VOICE_CLUSTERS + MatterClusterMetaRegistry.CLUSTER_BASIC_INFORMATION + extraClusters)
        .distinct()
    for (clusterId in targetClusters) {
      paths.add(
        ChipAttributePath.newInstance(
          ChipPathId.forWildcard(),
          ChipPathId.forId(clusterId),
          ChipPathId.forWildcard(),
        )
      )
    }
    for (ep in node.endpoints) {
      if (!ep.serverClusters.contains(MatterClusterMetaRegistry.CLUSTER_SWITCH)) continue
      eventPaths.add(
        ChipEventPath.newInstance(
          ChipPathId.forId(ep.endpointId.toLong()),
          ChipPathId.forId(MatterClusterMetaRegistry.CLUSTER_SWITCH),
          ChipPathId.forWildcard(),
          false,
        )
      )
    }
    return paths to eventPaths
  }

  private fun decodeSwitchEvent(eventId: Long, eventState: EventState): Pair<Int, Int> {
    when (val obj = eventState.value) {
      is ChipEventStructs.SwitchClusterSwitchLatchedEvent -> return obj.newPosition to 1
      is ChipEventStructs.SwitchClusterInitialPressEvent -> return obj.newPosition to 1
      is ChipEventStructs.SwitchClusterLongPressEvent -> return obj.newPosition to 1
      is ChipEventStructs.SwitchClusterShortReleaseEvent -> return obj.previousPosition to 1
      is ChipEventStructs.SwitchClusterLongReleaseEvent -> return obj.previousPosition to 1
      is ChipEventStructs.SwitchClusterMultiPressOngoingEvent ->
        return obj.newPosition to obj.currentNumberOfPressesCounted
      is ChipEventStructs.SwitchClusterMultiPressCompleteEvent ->
        return obj.previousPosition to obj.totalNumberOfPressesCounted
    }
    val tlv = eventState.tlv
    if (tlv != null && tlv.isNotEmpty()) {
      try {
        val reader = TlvReader(tlv)
        reader.enterStructure(AnonymousTag)
        val pos = reader.getUByte(ContextSpecificTag(0)).toInt()
        val count =
          if ((eventId == 0x05L || eventId == 0x06L) && !reader.isEndOfContainer()) {
            reader.getUByte(ContextSpecificTag(1)).toInt()
          } else {
            1
          }
        reader.exitContainer()
        return pos to count
      } catch (_: Throwable) {}
    }
    return 1 to 1
  }

  /**
   * Stamps an event with the device's own epoch timestamp when it has one, so that events replayed
   * by a new subscription keep their real time instead of looking like they just happened.
   */
  internal fun eventTimestampMs(eventState: EventState, nowMs: Long): Long {
    if (eventState.timestampType != EventState.MILLIS_SINCE_EPOCH) return nowMs
    val ts = eventState.timestampValue
    val plausibleFloor = 1_577_836_800_000L // 2020-01-01
    return if (ts in plausibleFloor..(nowMs + 60_000L)) ts else nowMs
  }

  internal fun processNodeStateReport(nodeId: Long, nodeState: NodeState) {
    val now = clock()
    val registry = registryOrNull()
    val beforeNode = registry?.getNode(nodeId)
    if (registry != null && beforeNode == null) {
      return
    }
    if (registry != null && beforeNode?.descriptorVerified != true) {
      val applied = registry.applyDescriptorReport(null, nodeId, nodeState)
      if (!applied) {
        ensureDescriptorDiscovered(nodeId)
      }
    }
    for ((epId, endpointState) in nodeState.endpointStates) {
      for ((clusterId, clusterState) in endpointState.clusterStates) {
        for ((attrId, attributeState) in clusterState.attributeStates) {
          val value =
            attributeState.value
              ?: attributeState.tlv?.let { tlv ->
                MatterClusterMetaRegistry.getCluster(clusterId)?.attributes?.get(attrId)?.let { meta
                  ->
                  UniversalTlvEncoder.decodeAttributeReport(meta, tlv).rawValue
                }
              }
          HomeStateCache.put(
            nodeId,
            epId,
            clusterId,
            attrId,
            value,
            now,
            Source.SUBSCRIPTION_REPORT,
          )
          registry?.recordDiscoveredEndpointCluster(null, nodeId, epId, clusterId, attrId, value)
        }
        if (clusterId == MatterClusterMetaRegistry.CLUSTER_SWITCH) {
          for ((eventId, eventList) in clusterState.eventStates) {
            for (eventState in eventList) {
              val (position, pressCount) = decodeSwitchEvent(eventId, eventState)
              HomeStateCache.putSwitchEvent(
                nodeId = nodeId,
                endpointId = epId,
                eventId = eventId,
                position = position,
                pressCount = pressCount,
                timestampMs = eventTimestampMs(eventState, now),
                eventNumber = eventState.eventNumber,
              )
              log(
                "switch_event node=$nodeId ep=$epId eventId=0x${eventId.toString(16)} " +
                  "pos=$position count=$pressCount num=${eventState.eventNumber}"
              )
            }
          }
        }
      }
      for (clusterId in endpointState.clusterStates.keys) {
        registry?.recordDiscoveredEndpointCluster(null, nodeId, epId, clusterId, null, null)
      }
    }
    val afterNode = registry?.getNode(nodeId)
    if (registry != null && afterNode != null && afterNode != beforeNode) {
      val ctx = appContext
      if (ctx != null) {
        scope.launch { registry.saveFabricToPreferences(ctx) }
      }
    }
  }

  // ---------------------------------------------------------------------------------------------
  // Transport
  // ---------------------------------------------------------------------------------------------

  private fun transport(): HubTransport =
    transportOrNull() ?: throw IllegalStateException("HubConnectionManager has no context")

  private fun transportOrNull(): HubTransport? {
    transportOverride?.let {
      return it
    }
    chipTransport?.let {
      return it
    }
    val ctx = appContext ?: return null
    return ChipTransport(ctx).also { chipTransport = it }
  }

  private class ChipTransport(private val context: Context) : HubTransport {
    override suspend fun connect(nodeId: Long): Long =
      ChipClient.getConnectedDevicePointer(context, nodeId)

    override suspend fun readDescriptor(devicePtr: Long, nodeId: Long): NodeState? =
      suspendCancellableCoroutine { cont ->
        ChipClient.getDeviceController(context)
          .readPath(
            object : ReportCallback {
              override fun onError(
                attributePath: ChipAttributePath?,
                eventPath: ChipEventPath?,
                ex: Exception,
              ) {
                if (attributePath == null && eventPath == null && cont.isActive) cont.resume(null)
              }

              override fun onReport(nodeState: NodeState) {
                if (cont.isActive) cont.resume(nodeState)
              }

              override fun onDone() {
                if (cont.isActive) cont.resume(null)
              }
            },
            devicePtr,
            DESCRIPTOR_PATHS,
            emptyList<ChipEventPath>(),
            false,
            PRIME_TIMEOUT_MAINS_MS,
          )
      }

    override fun subscribe(request: SubscribeRequest) {
      ChipClient.getDeviceController(context)
        .subscribeToPath(
          request.established,
          request.resubscription,
          request.report,
          request.devicePtr,
          request.attributePaths,
          request.eventPaths,
          request.minInterval,
          request.maxInterval,
          request.keepSubscriptions,
          true,
          request.imTimeoutMs,
          request.eventMin,
        )
    }

    override fun shutdown(nodeId: Long) {
      try {
        val controller = ChipClient.getDeviceController(context)
        controller.shutdownSubscriptions(controller.fabricIndex, nodeId)
      } catch (e: Exception) {
        logE("shutdownSubscriptions failed node=$nodeId", e)
      }
    }

    companion object {
      private val DESCRIPTOR_PATHS =
        listOf(
          ChipAttributePath.newInstance(
            ChipPathId.forWildcard(),
            ChipPathId.forId(0x001DL),
            ChipPathId.forId(0x0000L), // DeviceTypeList
          ),
          ChipAttributePath.newInstance(
            ChipPathId.forWildcard(),
            ChipPathId.forId(0x001DL),
            ChipPathId.forId(0x0001L), // ServerList
          ),
          ChipAttributePath.newInstance(
            ChipPathId.forId(0L),
            ChipPathId.forId(0x001DL),
            ChipPathId.forId(0x0003L), // PartsList
          ),
        )
    }
  }

  // ---------------------------------------------------------------------------------------------
  // Logging (android.util.Log is not available in JVM unit tests)
  // ---------------------------------------------------------------------------------------------

  private fun logDrop(nodeId: Long, kind: String, gen: Long) {
    log("drop stale callback node=$nodeId kind=$kind gen=$gen current=${links[nodeId]?.gen?.get()}")
  }

  private fun log(msg: String) {
    try {
      android.util.Log.i(TAG, msg)
    } catch (e: Throwable) {
      println("INFO: $TAG: $msg")
    }
  }

  private fun logE(msg: String, t: Throwable? = null) {
    try {
      android.util.Log.e(TAG, msg, t)
    } catch (e: Throwable) {
      println("ERROR: $TAG: $msg $t")
    }
  }

  // ---------------------------------------------------------------------------------------------
  // Tests
  // ---------------------------------------------------------------------------------------------

  /** Resets all state and installs test doubles. */
  internal fun resetForTest(
    registry: CommissionedNodeRegistry?,
    transport: HubTransport?,
    dispatcher: CoroutineDispatcher = Dispatchers.Default,
  ) {
    scope.cancel()
    scope = CoroutineScope(SupervisorJob() + dispatcher)
    caseSemaphore = Semaphore(MAX_PARALLEL_CASE)
    synchronized(stateLock) {
      leases.clear()
      batchNodes = emptySet()
      links.clear()
      _linkStates.value = emptyMap()
      _connectAllProgress.value = null
      sharedRegistry = registry
      transportOverride = transport
      chipTransport = null
    }
    clock = { System.currentTimeMillis() }
  }

  /** Places [nodeId] in [state] directly (no transport involved). */
  internal fun setLinkStateForTest(nodeId: Long, state: LinkState) {
    synchronized(stateLock) { put(nodeId, state) }
  }
}
