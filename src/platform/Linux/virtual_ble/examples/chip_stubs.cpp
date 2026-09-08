
#include <functional>
#include <lib/support/CHIPMem.h>
#include <lib/support/logging/CHIPLogging.h>
#include <system/SystemLayer.h>
#include <ble/Ble.h>
#include <ble/BleLayer.h>

namespace chip {
namespace Platform {
    CriticalFailure MemoryInit(void * buf, size_t bufSize) { return CriticalFailure(CHIP_NO_ERROR); }
    void MemoryShutdown() {}
}
namespace DeviceLayer {
    class SystemLayerImpl : public System::Layer {
    public:
        CriticalFailure Init() override { return CriticalFailure(CHIP_NO_ERROR); }
        void Shutdown() override {}
        bool IsInitialized() const override { return true; }
        CriticalFailure StartTimer(System::Clock::Timeout aDelay, System::TimerCompleteCallback aFF, void * aAppState) override { return CriticalFailure(CHIP_NO_ERROR); }
        CHIP_ERROR ExtendTimerTo(System::Clock::Timeout aDelay, System::TimerCompleteCallback aComplete, void * aAppState) override { return CHIP_NO_ERROR; }
        bool IsTimerActive(System::TimerCompleteCallback onComplete, void * appState) override { return false; }
        System::Clock::Timeout GetRemainingTime(System::TimerCompleteCallback onComplete, void * appState) override { return System::Clock::Timeout::zero(); }
        void CancelTimer(System::TimerCompleteCallback aFF, void * aAppState) override {}
        CriticalFailure ScheduleWork(System::TimerCompleteCallback aComplete, void * aAppState) override { return CriticalFailure(CHIP_NO_ERROR); }
        CriticalFailure ScheduleLambdaBridge(LambdaBridge && bridge) override { return CriticalFailure(CHIP_NO_ERROR); }
    };
    static SystemLayerImpl sSystemLayer;
    System::Layer & SystemLayer() { return sSystemLayer; }
}
namespace Ble {
    BleLayer::BleLayer() {}
    CHIP_ERROR BleLayer::Init(BlePlatformDelegate * pPlatformDelegate, BleConnectionDelegate * pConnDelegate,
                               BleApplicationDelegate * pAppDelegate, System::Layer * pSystemLayer) { return CHIP_NO_ERROR; }
    void BleLayer::Shutdown() {}
    bool BleLayer::HandleWriteReceived(BLE_CONNECTION_OBJECT connObj, const ChipBleUUID * svcId, const ChipBleUUID * charId, System::PacketBufferHandle && pBuf) { return true; }
    bool BleLayer::HandleIndicationReceived(BLE_CONNECTION_OBJECT connObj, const ChipBleUUID * svcId, const ChipBleUUID * charId, System::PacketBufferHandle && pBuf) { return true; }
    bool BleLayer::HandleSubscribeReceived(BLE_CONNECTION_OBJECT connObj, const ChipBleUUID * svcId, const ChipBleUUID * charId) { return true; }
    void BleLayer::HandleConnectionError(BLE_CONNECTION_OBJECT connObj, CHIP_ERROR err) {}
}
namespace Logging {
    void Log(uint8_t module, uint8_t category, const char * msg, ...) {}
}
}
