# Lossy-link test hazards

Most Matter tests run over loss-free transports, so every message arrives on
the first try and exchanges finish before the next request starts. When frames
are lost, MRP retransmissions and timeouts stretch exchange lifetimes, and
state that is normally short-lived stays alive long enough to interact with the
next request. This page lists the hazards that are covered by deterministic
unit tests, and how they relate to the Cirque RF channel simulator.

## How the tests inject loss

The unit tests below run on the host with `LoopbackMessagingContext` and drop
specific frames through `LoopbackTransport` (`mNumMessagesToAllowBeforeDropping`,
`mNumMessagesToDrop`, `mDroppedMessageCount`). Each lossy test has a control
that runs the same steps without the drop and expects the hazard not to occur.
They do not use the Cirque simulator; the drop indices encode the current
message order of each exchange.

The Cirque RF channel simulator (`third_party/cirque/repo`, see its
`docs/rf_channel_simulation.md`) produces the same kinds of loss end to end:
`regime_b_far_nlos_fading` gives isolated single-frame drops and
`regime_f_bursty_body_shadowing` gives correlated bursts of consecutive drops.

## 1. Interaction Model `Status::Busy` from a held `ReadHandler`

-   **Code:**
    [`InteractionModelEngine::EnsureResourceForRead`](../../src/app/InteractionModelEngine.cpp#L1517-L1523)
    returns `Status::Busy` when the requesting fabric already uses its
    guaranteed number of read handlers or paths.
-   **Loss pattern:** a non-final chunk of a chunked `ReportData` is lost. The
    server `ReadHandler` keeps waiting for the client's `StatusResponse`, so a
    retried Read on the same fabric finds the fabric quota in use.
-   **Condition:** quotas are only enforced when pools are statically allocated
    or `chip_im_force_fabric_quota_check` is set; the test forces them with
    `SetForceHandlerQuota(true)` and a capacity of one read handler.
-   **Test:** `TestReadInteraction.TestReadHandler_LossyChunkedReportHoldsHandlerRetryGetsBusy`
    and `..._IdealControl` in
    [`src/app/tests/TestReadInteraction.cpp`](../../src/app/tests/TestReadInteraction.cpp).

## 2. CASE responder Busy while waiting for Sigma3

-   **Code:**
    [`CASEServer::OnMessageReceived`](../../src/protocols/secure_channel/CASEServer.cpp#L85-L128)
    answers a new Sigma1 with a Busy status report while its `CASESession` is
    not in `kInitialized`, unless the background-work watchdog clears a stuck
    handshake. In `kSentSigma2` the minimum wait it reports is
    `CASESession::ComputeSigma2ResponseTimeout`.
-   **Loss pattern:** the responder's Sigma2 is lost, so the responder stays
    in `kSentSigma2` and a second initiator's Sigma1 receives Busy. MRP on the
    responder normally retransmits Sigma2, so on a real link this window is
    bounded by the retransmission schedule.
-   **Test:** `TestCASESession.LossySigma2KeepsResponderBusyForSecondInitiator`
    and `..._IdealControl` in
    [`src/protocols/secure_channel/tests/TestCASESession.cpp`](../../src/protocols/secure_channel/tests/TestCASESession.cpp).
    The test also checks the requested Busy delay; without the drop it fails.

## 3. Timed interaction rejected with `Status::Timeout`

-   **Code:**
    [`TimedHandler::HandleTimedRequestAction`](../../src/app/TimedHandler.cpp#L129-L140)
    sets `mTimeLimit` to the time the `TimedRequest` is answered plus the
    requested timeout, and
    [`TimedHandler::OnMessageReceived`](../../src/app/TimedHandler.cpp#L61-L72)
    answers the following action with `Status::Timeout` if it arrives after
    `mTimeLimit`.
-   **Loss pattern:** the first transmission of the `InvokeRequest` is lost and
    its MRP retransmission arrives after the timed window. Any loss of the
    `StatusResponse` to the `TimedRequest` shortens the window the same way.
-   **Test:** `TestTimedHandler.TestInvokeDelayedByLostFrameTimesOut` and
    `..._IdealControl` in
    [`src/app/tests/TestTimedHandler.cpp`](../../src/app/tests/TestTimedHandler.cpp).

## 4. CASE session marked defunct after MRP exhaustion

-   **Code:**
    [`ReliableMessageMgr::ExecuteActions`](../../src/messaging/ReliableMessageMgr.cpp#L186-L218)
    gives up after `CHIP_CONFIG_RMP_DEFAULT_MAX_RETRANS` retransmissions. If
    the exchange does not expect a response and the session is a CASE session,
    it calls `MarkAsDefunct()` and `NotifySessionHang()`.
-   **Loss pattern:** a burst of loss covers all `1 + MAX_RETRANS`
    transmissions of a message that does not expect a response. Losing one
    transmission fewer lets the last retransmission through and the session
    stays healthy.
-   **Test:** `TestReliableMessageProtocol.CheckBurstLossExhaustsRetransMarksCaseDefunct`
    and `..._BoundaryControl` in
    [`src/messaging/tests/TestReliableMessageProtocol.cpp`](../../src/messaging/tests/TestReliableMessageProtocol.cpp).
    `CheckApplicationResponseNeverComes` covers the response-expected path,
    where the exchange response timeout marks the session defunct instead.

## Related Cirque test

`src/test_driver/linux-cirque/AndroidBleThreadSilabsLockRfRssiTest.py`
commissions the Silabs lock app over BLE and Thread from Android CHIPTool and
checks the RSSI, SINR, LQI and drop statistics reported by the Cirque RF
simulator for several presets. Its Oracle RF-5 compares `ideal` with
`regime_b_far_nlos_fading` (zero drops under `ideal`, lower and more variable
RSSI under `regime_b`). It does not assert on the four hazards above.
