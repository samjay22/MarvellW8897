//
//  MarvellW8897Controller.cpp
//  MarvellW8897
//
//  Created by Bryan on 16/11/2025.
//

#include "MarvellW8897Controller.hpp"
#include "MarvellW8897FirmwareImage.hpp"
#include "MarvellW8897MlanHost.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterrupts.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IOOutputQueue.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <kern/kern_types.h>
#include <sys/kpi_mbuf.h>
#include <libkern/crypto/sha2.h>

namespace {

constexpr uint32_t kRegisterBar = 2;
constexpr UInt32 kMaxAutoStartAttempts = 3;
constexpr UInt32 kAutoRetryDelayMs = 10000;
static_assert(sizeof(MarvellW8897FirmwareImage::kSha256) == SHA256_DIGEST_LENGTH,
              "firmware hash size must match SHA-256");

void logPciPowerState(IOPCIDevice* device, const char* stage)
{
    if (!device) return;
    UInt8 capabilityOffset = 0;
    if (device->findPCICapability(kIOPCICapabilityIDPowerManagement,
                                  &capabilityOffset) == 0) {
        IOLog("MarvellW8897: %s PCI power-management capability absent\n", stage);
        return;
    }
    const UInt16 pmcsr = device->configRead16(capabilityOffset + 4);
    const UInt16 command = device->configRead16(kIOPCIConfigCommand);
    IOLog("MarvellW8897: %s PCI PMCSR=%04x state=D%u command=%04x\n",
          stage, pmcsr, pmcsr & 0x3U, command);
}

class PcieFirmwareDevice final : public MarvellW8897FirmwareProtocol::Device {
public:
    PcieFirmwareDevice(IOPCIDevice* pciDevice, const MarvellW8897PciBarState& bars)
        : pciDevice_(pciDevice), bars_(bars)
    {
        buffer_ = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionOut | kIOMemoryPhysicallyContiguous,
            MarvellW8897FirmwareProtocol::kDmaBufferSize, 0xffffffffULL);
        if (!buffer_) return;

        dma_ = IODMACommand::withSpecification(
            kIODMACommandOutputHost64,
            32, // Linux mwifiex_pcie uses DMA_BIT_MASK(32) for 8897.
            MarvellW8897FirmwareProtocol::kDmaBufferSize,
            IODMACommand::kMapped,
            4096,
            MarvellW8897FirmwareProtocol::kDownloadBlockSize);
    }

    ~PcieFirmwareDevice() override
    {
        if (prepared_) finish(false);
        if (dma_) dma_->release();
        if (buffer_) buffer_->release();
    }

    bool valid() const { return buffer_ && dma_ && pciDevice_; }

    bool read32(uint32_t offset, uint32_t& value) override
    {
        return MarvellW8897Transport::readReg32FromBar(bars_, kRegisterBar, offset, value);
    }

    bool write32(uint32_t offset, uint32_t value) override
    {
        return MarvellW8897Transport::writeReg32ToBar(bars_, kRegisterBar, offset, value);
    }

    bool transmit(const uint8_t* data, uint32_t dmaLength) override
    {
        if (!valid() || prepared_ || !data || dmaLength == 0 ||
            dmaLength > MarvellW8897FirmwareProtocol::kDmaBufferSize) {
            return false;
        }

        buffer_->setLength(dmaLength);
        void* bytes = buffer_->getBytesNoCopy();
        if (!bytes) return false;
        bcopy(data, bytes, dmaLength);

        if (dma_->setMemoryDescriptor(buffer_, true) != kIOReturnSuccess) return false;
        prepared_ = true;

        UInt64 offset = 0;
        IODMACommand::Segment64 segment {};
        UInt32 segments = 1;
        if (dma_->gen64IOVMSegments(&offset, &segment, &segments) != kIOReturnSuccess ||
            segments != 1 || segment.fLength < dmaLength) {
            finish(false);
            return false;
        }

        const UInt64 address = segment.fIOVMAddr;
        if (address > 0xffffffffULL ||
            dma_->synchronize(kIODirectionOut) != kIOReturnSuccess) {
            finish(false);
            return false;
        }
        if (!write32(0x0c10, static_cast<UInt32>(address)) ||
            !write32(0x0c14, static_cast<UInt32>(address >> 32)) ||
            !write32(0x0c40, dmaLength) ||
            !write32(0x0c18, 1U << 1)) {
            finish(false);
            return false;
        }
        return true;
    }

    void finish(bool acknowledged) override
    {
        if (!prepared_) return;
        if (!acknowledged && pciDevice_) {
            pciDevice_->setBusMasterEnable(false);
        }
        dma_->clearMemoryDescriptor(true);
        prepared_ = false;
    }

    void delayMicroseconds(uint32_t usec) override
    {
        if (usec >= 1000) IOSleep(usec / 1000);
        else IODelay(usec);
    }

private:
    IOPCIDevice* pciDevice_ { nullptr };
    const MarvellW8897PciBarState& bars_;
    IOBufferMemoryDescriptor* buffer_ { nullptr };
    IODMACommand* dma_ { nullptr };
    bool prepared_ { false };
};

} // namespace

#define super IOEthernetController
OSDefineMetaClassAndStructors(MarvellW8897Controller, IOEthernetController)

void MarvellW8897Controller::releaseMappedBars()
{
    MarvellW8897PciBars::release(fPciBarState);
}

IOService* MarvellW8897Controller::probe(IOService* provider, SInt32* score)
{
    IOService* matched = super::probe(provider, score);

    if (matched) {
        const char* providerName = provider ? provider->getName() : "<null>";
        IOLog("MarvellW8897: probe matched provider=%s score=%d\n",
              providerName,
              score ? *score : -1);
    } else {
        IOLog("MarvellW8897: probe did not match\n");
    }

    return matched;
}

bool MarvellW8897Controller::start(IOService* provider)
{
    if (!super::start(provider)) {
        IOLog("MarvellW8897: super::start() failed\n");
        return false;
    }

    IOLog("MarvellW8897: start() called\n");

    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCIDevice) {
        IOLog("MarvellW8897: provider is not IOPCIDevice\n");
        return false;
    }

    fPCIDevice->retain();

    if (!fPCIDevice->open(this)) {
        IOLog("MarvellW8897: failed to open provider\n");
        fPCIDevice->release();
        fPCIDevice = nullptr;
        return false;
    }
    fDeviceOpen = true;

    fVendorId = fPCIDevice->configRead16(kIOPCIConfigVendorID);
    fDeviceId = fPCIDevice->configRead16(kIOPCIConfigDeviceID);
    fSubVendorId = fPCIDevice->configRead16(kIOPCIConfigSubSystemVendorID);
    fSubSystemId = fPCIDevice->configRead16(kIOPCIConfigSubSystemID);
    fRevisionId = fPCIDevice->configRead8(kIOPCIConfigRevisionID);
    fClassCode = fPCIDevice->configRead32(kIOPCIConfigRevisionID) >> 8;
    fCommand = fPCIDevice->configRead16(kIOPCIConfigCommand);
    fStatus = fPCIDevice->configRead16(kIOPCIConfigStatus);
    fInterruptLine = fPCIDevice->configRead8(kIOPCIConfigInterruptLine);
    fInterruptPin = fPCIDevice->configRead8(kIOPCIConfigInterruptPin);

    const char* providerName = fPCIDevice->getName();
    IOLog("MarvellW8897: provider=%s\n", providerName ? providerName : "<unknown>");
    IOLog("MarvellW8897: PCI IDs vendor=%04x device=%04x subVendor=%04x subSystem=%04x rev=%02x class=%06x\n",
          fVendorId,
          fDeviceId,
          fSubVendorId,
          fSubSystemId,
          fRevisionId,
          fClassCode);
    IOLog("MarvellW8897: PCI cmd=%04x status=%04x intLine=%02x intPin=%02x\n",
          fCommand,
          fStatus,
          fInterruptLine,
          fInterruptPin);
    logPciPowerState(fPCIDevice, "start");

    fPCIDevice->setMemoryEnable(true);
    fPCIDevice->setBusMasterEnable(false);
    fCommand = fPCIDevice->configRead16(kIOPCIConfigCommand);
    fStatus = fPCIDevice->configRead16(kIOPCIConfigStatus);
    IOLog("MarvellW8897: memory enabled; bus mastering deferred (cmd=%04x status=%04x)\n",
          fCommand,
          fStatus);

    if (!MarvellW8897PciBars::mapDeviceBars(fPCIDevice, fPciBarState)) {
        // Keep the service attached so Recovery can report the failure even
        // when its log daemon is unavailable. No firmware work is scheduled.
        setProperty("MarvellBringupStage", "bar-map-failed");
        setProperty("MarvellResourceCount", fPciBarState.resourceCount, 32);
        setProperty("MarvellGetResourcesStatus", fPciBarState.resourceStatus, 32);
        setProperty("MarvellResource0Base", fPciBarState.resources[0].physicalAddress, 64);
        setProperty("MarvellResource0Length", fPciBarState.resources[0].length, 64);
        setProperty("MarvellResource1Base", fPciBarState.resources[1].physicalAddress, 64);
        setProperty("MarvellResource1Length", fPciBarState.resources[1].length, 64);
        setProperty("MarvellBAR0Low", fPciBarState.bars[0].rawLow, 32);
        setProperty("MarvellBAR0High", fPciBarState.bars[0].rawHigh, 32);
        setProperty("MarvellBAR2Low", fPciBarState.bars[2].rawLow, 32);
        setProperty("MarvellBAR2High", fPciBarState.bars[2].rawHigh, 32);
        setProperty("MarvellBAR0MapStatus", fPciBarState.bars[0].mappingStatus);
        setProperty("MarvellBAR2MapStatus", fPciBarState.bars[2].mappingStatus);
        registerService();
        return true;
    }

    setProperty("MarvellResourceCount", fPciBarState.resourceCount, 32);
    setProperty("MarvellBAR0MapStatus", fPciBarState.bars[0].mappingStatus);
    setProperty("MarvellBAR2MapStatus", fPciBarState.bars[2].mappingStatus);
    setProperty("MarvellBAR2MappedResource", fPciBarState.bars[2].mappedResource, 32);
    setProperty("MarvellBAR2MappedLength", fPciBarState.bars[2].length, 64);

    MarvellW8897Transport::reset(fTransportState);
    MarvellW8897Bringup::reset(fBringupResult);

    if (!MarvellW8897Transport::configurePcieProfile(fTransportState, fVendorId, fDeviceId)) {
        IOLog("MarvellW8897: no grounded mwifiex-style PCIe profile for vendor=%04x device=%04x\n",
              fVendorId,
              fDeviceId);
    }

    if (!MarvellW8897Bringup::run(fPciBarState, fTransportState, fBringupResult)) {
        IOLog("MarvellW8897: bring-up failed stage=%s reason=%s bar=%u offset=0x%x reads=0x%08x writes=0x%08x polls=%u\n",
              MarvellW8897Bringup::stageName(fBringupResult.stage),
              MarvellW8897Bringup::failureReasonName(fBringupResult.failureReason),
              fBringupResult.lastBarIndex,
              fBringupResult.lastRegisterOffset,
              fBringupResult.lastValueRead,
              fBringupResult.lastValueWritten,
              fBringupResult.pollIterations);
        setProperty("MarvellBringupStage", MarvellW8897Bringup::stageName(fBringupResult.stage));
        setProperty("MarvellBringupReason", MarvellW8897Bringup::failureReasonName(fBringupResult.failureReason));
        setProperty("MarvellBringupBar", fBringupResult.lastBarIndex, 32);
        setProperty("MarvellBringupOffset", fBringupResult.lastRegisterOffset, 32);
        registerService();
        return true;
    }

    setProperty("MarvellBringupStage", MarvellW8897Bringup::stageName(fBringupResult.stage));
    setProperty("MarvellBringupReason", MarvellW8897Bringup::failureReasonName(fBringupResult.failureReason));

    IOLog("MarvellW8897: bring-up stage=%s reason=%s resetAttempted=%d readyObserved=%d bar=%u offset=0x%x reads=0x%08x writes=0x%08x polls=%u\n",
          MarvellW8897Bringup::stageName(fBringupResult.stage),
          MarvellW8897Bringup::failureReasonName(fBringupResult.failureReason),
          fBringupResult.resetAttempted ? 1 : 0,
          fBringupResult.readyObserved ? 1 : 0,
          fBringupResult.lastBarIndex,
          fBringupResult.lastRegisterOffset,
          fBringupResult.lastValueRead,
          fBringupResult.lastValueWritten,
          fBringupResult.pollIterations);

    fFirmwareLock = IOLockAlloc();
    fNetworkLock = IOLockAlloc();
    if (!fFirmwareLock || !fNetworkLock) {
        if (fNetworkLock) { IOLockFree(fNetworkLock); fNetworkLock = nullptr; }
        if (fFirmwareLock) { IOLockFree(fFirmwareLock); fFirmwareLock = nullptr; }
        fPCIDevice->setBusMasterEnable(false);
        releaseMappedBars();
        fPCIDevice->close(this);
        fDeviceOpen = false;
        fPCIDevice->release();
        fPCIDevice = nullptr;
        return false;
    }

    registerService();
    fAutoStartCall = thread_call_allocate(sAutoStart, this);
    if (fAutoStartCall) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
        thread_call_enter_delayed(fAutoStartCall, deadline);
        IOLog("MarvellW8897: embedded firmware startup scheduled\n");
    } else {
        IOLog("MarvellW8897: could not schedule embedded firmware startup\n");
    }
    return true;
}

void MarvellW8897Controller::stop(IOService* provider)
{
    IOLog("MarvellW8897: stop() called\n");

    IOEthernetInterface* interface = nullptr;
    if (fNetworkLock) {
        IOLockLock(fNetworkLock);
        __atomic_store_n(&fStopping, true, __ATOMIC_RELEASE);
        __atomic_store_n(&fNetworkEnabled, false, __ATOMIC_RELEASE);
        fLinkAssociated = false;
        while (fRxInFlight != 0)
            IOLockSleep(fNetworkLock, &fRxInFlight, THREAD_UNINT);
        interface = __atomic_exchange_n(&fNetworkInterface, nullptr,
                                        __ATOMIC_ACQ_REL);
        IOLockUnlock(fNetworkLock);
    }
    if (interface) {
        setLinkStatus(kIONetworkLinkValid);
        detachInterface(interface, true);
        interface->release();
    }

    if (fAutoStartCall) {
        thread_call_cancel_wait(fAutoStartCall);
        thread_call_free(fAutoStartCall);
        fAutoStartCall = nullptr;
    }

    if (fFirmwareLock) IOLockLock(fFirmwareLock);

    if (fMlanInterrupt) {
        fMlanInterrupt->disable();
        fMlanWorkLoop->removeEventSource(fMlanInterrupt);
        fMlanInterrupt->release();
        fMlanInterrupt = nullptr;
    }
    if (fMlanWorkLoop) {
        fMlanWorkLoop->release();
        fMlanWorkLoop = nullptr;
    }
    fMlanInitStarted = false;

    if (fPCIDevice) fPCIDevice->setBusMasterEnable(false);

    if (fMlanContext) {
        MarvellW8897MlanHost::unregisterCore(fMlanContext);
        fMlanContext = nullptr;
    }

    releaseMappedBars();

    if (fPCIDevice) {
        fPCIDevice->setBusMasterEnable(false);
        fPCIDevice->setMemoryEnable(false);

        if (fDeviceOpen) {
            fPCIDevice->close(this);
            fDeviceOpen = false;
        }

        fPCIDevice->release();
        fPCIDevice = nullptr;
    }

    MarvellW8897Transport::reset(fTransportState);
    MarvellW8897Bringup::reset(fBringupResult);

    if (fFirmwareLock) {
        IOLockUnlock(fFirmwareLock);
        IOLockFree(fFirmwareLock);
        fFirmwareLock = nullptr;
    }

    if (fNetworkLock) {
        IOLockFree(fNetworkLock);
        fNetworkLock = nullptr;
    }

    super::stop(provider);
}

bool MarvellW8897Controller::readReg32FromBar(uint32_t barIndex, uint32_t offset, uint32_t& valueOut)
{
    return MarvellW8897Transport::readReg32FromBar(fPciBarState, barIndex, offset, valueOut);
}

bool MarvellW8897Controller::writeReg32ToBar(uint32_t barIndex, uint32_t offset, uint32_t value)
{
    return MarvellW8897Transport::writeReg32ToBar(fPciBarState, barIndex, offset, value);
}

uint32_t MarvellW8897Controller::readReg32(uint32_t offset)
{
    uint32_t value = 0xffffffffU;
    if (!readReg32FromBar(2, offset, value)) {
        return 0xffffffffU;
    }

    return value;
}

void MarvellW8897Controller::writeReg32(uint32_t offset, uint32_t value)
{
    (void)writeReg32ToBar(2, offset, value);
}

bool MarvellW8897Controller::getPciIdentity(uint64_t outIdentity[10]) const
{
    if (!outIdentity) {
        return false;
    }

    outIdentity[0] = fVendorId;
    outIdentity[1] = fDeviceId;
    outIdentity[2] = fSubVendorId;
    outIdentity[3] = fSubSystemId;
    outIdentity[4] = fRevisionId;
    outIdentity[5] = fClassCode;
    outIdentity[6] = fCommand;
    outIdentity[7] = fStatus;
    outIdentity[8] = fInterruptLine;
    outIdentity[9] = fInterruptPin;
    return true;
}

bool MarvellW8897Controller::getBarInfoLegacy(uint32_t barIndex, uint64_t outInfo[8]) const
{
    return MarvellW8897PciBars::getBarInfoLegacy(fPciBarState, barIndex, outInfo);
}

bool MarvellW8897Controller::getBarInfoV2(uint32_t barIndex, uint64_t outInfo[11]) const
{
    return MarvellW8897PciBars::getBarInfoV2(fPciBarState, barIndex, outInfo);
}

bool MarvellW8897Controller::writeReg32ToBarWithFlush(uint32_t barIndex,
                                                      uint32_t offset,
                                                      uint32_t value,
                                                      uint32_t& readBack)
{
    return MarvellW8897Transport::writeReg32ToBarWithFlush(
        fPciBarState,
        fTransportState,
        barIndex,
        offset,
        value,
        readBack);
}

bool MarvellW8897Controller::getDebugAbiInfo(uint64_t outInfo[6]) const
{
    return MarvellW8897Debug::getDebugAbiInfo(outInfo);
}

bool MarvellW8897Controller::getTransportState(uint64_t outState[8]) const
{
    return MarvellW8897Transport::getTransportState(fTransportState, outState);
}

bool MarvellW8897Controller::getBringupResult(
    uint64_t outState[MarvellW8897Regs::kBringupResultCount]) const
{
    if (!outState) {
        return false;
    }

    // Read-only ABI field order:
    // 0 stage, 1 failureReason, 2 lastBarIndex, 3 lastRegisterOffset,
    // 4 lastValueRead, 5 lastValueWritten, 6 pollIterations,
    // 7 firmwareStatusValue, 8 driverReadyValue, 9 quirkFlags,
    // 10 resetAttempted, 11 readyObserved.
    outState[0] = static_cast<uint64_t>(fBringupResult.stage);
    outState[1] = static_cast<uint64_t>(fBringupResult.failureReason);
    outState[2] = fBringupResult.lastBarIndex;
    outState[3] = fBringupResult.lastRegisterOffset;
    outState[4] = fBringupResult.lastValueRead;
    outState[5] = fBringupResult.lastValueWritten;
    outState[6] = fBringupResult.pollIterations;
    outState[7] = fBringupResult.firmwareStatusValue;
    outState[8] = fBringupResult.driverReadyValue;
    outState[9] = fBringupResult.quirkFlags;
    outState[10] = fBringupResult.resetAttempted ? 1 : 0;
    outState[11] = fBringupResult.readyObserved ? 1 : 0;
    return true;
}

IOReturn MarvellW8897Controller::uploadFirmware(
    const void* image,
    size_t imageSize,
    MarvellW8897FirmwareProtocol::Result& result)
{
    if (!image || imageSize != MarvellW8897FirmwareImage::kSize)
        return kIOReturnBadArgument;

    SHA256_CTX context;
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256_Init(&context);
    SHA256_Update(&context, image, imageSize);
    SHA256_Final(digest, &context);
    if (memcmp(digest, MarvellW8897FirmwareImage::kSha256, sizeof(digest)) != 0) {
        return kIOReturnBadArgument;
    }

    if (!fFirmwareLock) return kIOReturnNotReady;
    IOLockLock(fFirmwareLock);
    if (!fPCIDevice || fFirmwareTransferFailed || fMlanInitStarted) {
        IOLockUnlock(fFirmwareLock);
        return kIOReturnNotReady;
    }

    PcieFirmwareDevice device(fPCIDevice, fPciBarState);
    if (!device.valid()) {
        IOLockUnlock(fFirmwareLock);
        return kIOReturnNoMemory;
    }

    fPCIDevice->setBusMasterEnable(true);
    result = MarvellW8897FirmwareProtocol::upload(
        device, static_cast<const uint8_t*>(image), imageSize);
    const bool ready = result.status == MarvellW8897FirmwareProtocol::Status::Complete ||
                       result.status == MarvellW8897FirmwareProtocol::Status::AlreadyReady;
    if (!ready) {
        fPCIDevice->setBusMasterEnable(false);
        // Both paths below fail before the first firmware DMA submission.
        // They may reflect a device that wakes after the initial boot probe.
        const bool retryableNoTransfer = result.blockCount == 0 &&
            (result.status == MarvellW8897FirmwareProtocol::Status::NotDownloadOwner ||
             result.status == MarvellW8897FirmwareProtocol::Status::HelperTimeout);
        if (!retryableNoTransfer)
            fFirmwareTransferFailed = true;
    } else {
        fTransportState.firmwareReady = 1;
        fBringupResult.readyObserved = true;
        fBringupResult.stage = MarvellW8897BringupStage::ReadyObserved;
        fBringupResult.failureReason = MarvellW8897BringupFailureReason::None;
        fBringupResult.firmwareStatusValue = result.lastFirmwareStatus;
    }
    IOLog("MarvellW8897: firmware upload status=%s bytes=%llu blocks=%u retries=%u fwStatus=%08x\n",
          MarvellW8897FirmwareProtocol::statusName(result.status),
          static_cast<unsigned long long>(result.bytesTransferred),
          result.blockCount,
          result.retryCount,
          result.lastFirmwareStatus);
    IOLockUnlock(fFirmwareLock);
    return kIOReturnSuccess;
}

IOReturn MarvellW8897Controller::registerMlanCore()
{
    if (!fFirmwareLock) return kIOReturnNotReady;
    IOLockLock(fFirmwareLock);
    if (!fPCIDevice || fFirmwareTransferFailed || !fTransportState.firmwareReady) {
        IOLockUnlock(fFirmwareLock);
        return kIOReturnNotReady;
    }
    if (fMlanContext) {
        IOLockUnlock(fFirmwareLock);
        return kIOReturnSuccess;
    }
    fMlanContext = MarvellW8897MlanHost::registerCore(this);
    const bool registered = fMlanContext != nullptr;
    IOLockUnlock(fFirmwareLock);
    return registered ? kIOReturnSuccess : kIOReturnError;
}

void MarvellW8897Controller::sAutoStart(thread_call_param_t owner,
                                       thread_call_param_t)
{
    auto* controller = static_cast<MarvellW8897Controller*>(owner);
    if (!controller || __atomic_load_n(&controller->fStopping, __ATOMIC_ACQUIRE)) return;

    ++controller->fAutoStartAttempts;
    logPciPowerState(controller->fPCIDevice, "auto-start");

    MarvellW8897FirmwareProtocol::Result result {};
    IOReturn status = controller->uploadFirmware(
        MarvellW8897FirmwareImage::kEmbedded,
        MarvellW8897FirmwareImage::kSize, result);
    if (status != kIOReturnSuccess ||
        (result.status != MarvellW8897FirmwareProtocol::Status::Complete &&
         result.status != MarvellW8897FirmwareProtocol::Status::AlreadyReady)) {
        IOLog("MarvellW8897: automatic firmware upload failed, IOReturn=0x%x status=%s\n",
              status, MarvellW8897FirmwareProtocol::statusName(result.status));
        const bool retryableNoTransfer = status == kIOReturnSuccess &&
            result.blockCount == 0 &&
            (result.status == MarvellW8897FirmwareProtocol::Status::NotDownloadOwner ||
             result.status == MarvellW8897FirmwareProtocol::Status::HelperTimeout);
        if (retryableNoTransfer &&
            controller->fAutoStartAttempts < kMaxAutoStartAttempts &&
            !__atomic_load_n(&controller->fStopping, __ATOMIC_ACQUIRE) &&
            controller->fAutoStartCall) {
            uint64_t deadline = 0;
            clock_interval_to_deadline(kAutoRetryDelayMs, kMillisecondScale, &deadline);
            thread_call_enter_delayed(controller->fAutoStartCall, deadline);
            IOLog("MarvellW8897: automatic retry scheduled, attempt=%u/%u\n",
                  controller->fAutoStartAttempts + 1, kMaxAutoStartAttempts);
        }
        return;
    }

    status = controller->registerMlanCore();
    if (status == kIOReturnSuccess) status = controller->startMlanFirmware();
    IOLog("MarvellW8897: automatic MLAN startup result=0x%x\n", status);
}

void MarvellW8897Controller::sMlanInterrupt(OSObject* owner,
                                            IOInterruptEventSource*, int)
{
    auto* controller = OSDynamicCast(MarvellW8897Controller, owner);
    if (controller && controller->fMlanContext)
        MarvellW8897MlanHost::processInterrupt(controller->fMlanContext);
}

IOReturn MarvellW8897Controller::startMlanFirmware()
{
    if (!fFirmwareLock) return kIOReturnNotReady;
    IOLockLock(fFirmwareLock);
    if (!fPCIDevice || !fMlanContext || fFirmwareTransferFailed ||
        !fTransportState.firmwareReady) {
        IOLockUnlock(fFirmwareLock);
        return kIOReturnNotReady;
    }
    if (fMlanInitStarted) {
        IOLockUnlock(fFirmwareLock);
        return kIOReturnSuccess;
    }

    fMlanWorkLoop = IOWorkLoop::workLoop();
    if (!fMlanWorkLoop) {
        IOLockUnlock(fFirmwareLock);
        return kIOReturnNoMemory;
    }
    fMlanInterrupt = IOInterruptEventSource::interruptEventSource(
        this, &MarvellW8897Controller::sMlanInterrupt, fPCIDevice, 0);
    if (!fMlanInterrupt ||
        fMlanWorkLoop->addEventSource(fMlanInterrupt) != kIOReturnSuccess) {
        if (fMlanInterrupt) { fMlanInterrupt->release(); fMlanInterrupt = nullptr; }
        fMlanWorkLoop->release();
        fMlanWorkLoop = nullptr;
        IOLockUnlock(fFirmwareLock);
        return kIOReturnError;
    }

    int interruptType = 0;
    if (fPCIDevice->getInterruptType(0, &interruptType) != kIOReturnSuccess) {
        fMlanWorkLoop->removeEventSource(fMlanInterrupt);
        fMlanInterrupt->release();
        fMlanInterrupt = nullptr;
        fMlanWorkLoop->release();
        fMlanWorkLoop = nullptr;
        IOLockUnlock(fFirmwareLock);
        return kIOReturnError;
    }
    const bool msi = (interruptType & kIOInterruptTypePCIMessaged) != 0;
    const unsigned status = MarvellW8897MlanHost::startFirmware(fMlanContext, msi);
    if (status != 0U && status != 1U) {
        fPCIDevice->setBusMasterEnable(false);
        fFirmwareTransferFailed = true;
        fMlanWorkLoop->removeEventSource(fMlanInterrupt);
        fMlanInterrupt->release();
        fMlanInterrupt = nullptr;
        fMlanWorkLoop->release();
        fMlanWorkLoop = nullptr;
        MarvellW8897MlanHost::unregisterCore(fMlanContext);
        fMlanContext = nullptr;
        IOLockUnlock(fFirmwareLock);
        IOLog("MarvellW8897: MLAN firmware initialization failed\n");
        return kIOReturnError;
    }
    fMlanInitStarted = true;
    fMlanInterrupt->enable();
    // Catch a response that arrived while the event source was disabled.
    MarvellW8897MlanHost::processInterrupt(fMlanContext);
    IOLockUnlock(fFirmwareLock);
    IOLog("MarvellW8897: MLAN firmware initialization started, status=%u irq=%s\n",
          status, msi ? "MSI" : "legacy");
    return kIOReturnSuccess;
}

bool MarvellW8897Controller::getMlanCoreStatus(uint64_t out[6])
{
    if (!out || !fFirmwareLock) return false;
    setProperty("MarvellTxLockDrops", __atomic_load_n(&fTxLockDrops, __ATOMIC_RELAXED), 64);
    setProperty("MarvellRxLockDrops", __atomic_load_n(&fRxLockDrops, __ATOMIC_RELAXED), 64);
    IOLockLock(fFirmwareLock);
    if (!fMlanContext) {
        bzero(out, 6 * sizeof(uint64_t));
        IOLockUnlock(fFirmwareLock);
        return true;
    }
    unsigned long long status[6] = {};
    const bool ok = MarvellW8897MlanHost::getStatus(fMlanContext, status);
    for (unsigned i = 0; i < 6; ++i) out[i] = status[i];
    IOLockUnlock(fFirmwareLock);
    return ok;
}

IOReturn MarvellW8897Controller::requestMlanScan(const uint8_t* ssid, uint32_t length)
{
    if (length > 32 || (length != 0 && !ssid)) return kIOReturnBadArgument;
    if (!fFirmwareLock) return kIOReturnNotReady;
    IOLockLock(fFirmwareLock);
    if (!fMlanContext || !fMlanInitStarted ||
        __atomic_load_n(&fStopping, __ATOMIC_ACQUIRE)) {
        IOLockUnlock(fFirmwareLock);
        return kIOReturnNotReady;
    }
    const unsigned status = MarvellW8897MlanHost::requestScan(fMlanContext, ssid, length);
    IOLockUnlock(fFirmwareLock);
    switch (status) {
    case 0: return kIOReturnSuccess;
    case 1: return kIOReturnBusy;
    case 2: return kIOReturnNotReady;
    default: return kIOReturnError;
    }
}

bool MarvellW8897Controller::getMlanScanStatus(uint64_t out[4])
{
    if (!out || !fFirmwareLock) return false;
    IOLockLock(fFirmwareLock);
    if (!fMlanContext) {
        bzero(out, 4 * sizeof(uint64_t));
        IOLockUnlock(fFirmwareLock);
        return true;
    }
    unsigned long long status[4] = {};
    const bool ok = MarvellW8897MlanHost::getScanStatus(fMlanContext, status);
    for (unsigned i = 0; i < 4; ++i) out[i] = status[i];
    IOLockUnlock(fFirmwareLock);
    return ok;
}

IOReturn MarvellW8897Controller::requestMlanConnect(
    const uint8_t* ssid, uint32_t ssidLength,
    const uint8_t* passphrase, uint32_t passphraseLength)
{
    if (!ssid || ssidLength == 0 || ssidLength > 32 ||
        (passphraseLength != 0 &&
         (passphraseLength < 8 || passphraseLength > 63 || !passphrase)))
        return kIOReturnBadArgument;
    if (!fFirmwareLock) return kIOReturnNotReady;
    IOLockLock(fFirmwareLock);
    if (!fMlanContext || !fMlanInitStarted ||
        __atomic_load_n(&fStopping, __ATOMIC_ACQUIRE)) {
        IOLockUnlock(fFirmwareLock);
        return kIOReturnNotReady;
    }
    const unsigned status = MarvellW8897MlanHost::requestConnect(
        fMlanContext, ssid, ssidLength, passphrase, passphraseLength);
    IOLockUnlock(fFirmwareLock);
    switch (status) {
    case 0: return kIOReturnSuccess;
    case 1: return kIOReturnBusy;
    case 2: return kIOReturnNotReady;
    default: return kIOReturnError;
    }
}

bool MarvellW8897Controller::getMlanConnectStatus(uint64_t out[4])
{
    if (!out || !fFirmwareLock) return false;
    IOLockLock(fFirmwareLock);
    if (!fMlanContext) {
        bzero(out, 4 * sizeof(uint64_t));
        IOLockUnlock(fFirmwareLock);
        return true;
    }
    unsigned long long status[4] = {};
    const bool ok = MarvellW8897MlanHost::getConnectStatus(fMlanContext, status);
    for (unsigned i = 0; i < 4; ++i) out[i] = status[i];
    IOLockUnlock(fFirmwareLock);
    return ok;
}

void MarvellW8897Controller::mlanCoreReady(const uint8_t mac[6], bool associated)
{
    if (!mac || !fNetworkLock) return;
    IOLockLock(fNetworkLock);
    if (__atomic_load_n(&fStopping, __ATOMIC_ACQUIRE)) {
        IOLockUnlock(fNetworkLock);
        return;
    }

    if (!__atomic_load_n(&fMacValid, __ATOMIC_ACQUIRE)) {
        bcopy(mac, fMacAddress, sizeof(fMacAddress));
        __atomic_store_n(&fMacValid, true, __ATOMIC_RELEASE);
    }

    if (!fNetworkInterface) {
        OSDictionary* media = OSDictionary::withCapacity(1);
        // Publish a nonzero nominal link rate until MLAN exposes the actual
        // negotiated rate. macOS uses this value for the interface bandwidth.
        IONetworkMedium* automatic = IONetworkMedium::medium(
            kIOMediumEthernetAuto, 100000000ULL);
        IONetworkInterface* attached = nullptr;
        if (!media || !automatic || !IONetworkMedium::addMedium(media, automatic) ||
            !publishMediumDictionary(media) ||
            !attachInterface(&attached, false)) {
            IOLog("MarvellW8897: could not attach network interface\n");
        } else {
            IOEthernetInterface* ethernet = OSDynamicCast(IOEthernetInterface, attached);
            if (ethernet) {
                __atomic_store_n(&fNetworkInterface, ethernet, __ATOMIC_RELEASE);
                setCurrentMedium(automatic);
                // Matching may call enable(), so publish our pointer first.
                ethernet->registerService();
                IOLog("MarvellW8897: Ethernet-style Wi-Fi interface attached\n");
            } else {
                detachInterface(attached, false);
                attached->release();
                IOLog("MarvellW8897: interface has an unexpected class\n");
            }
        }
        if (automatic) automatic->release();
        if (media) media->release();
    }

    fLinkAssociated = associated;
    if (fNetworkInterface) {
        const IONetworkMedium* automatic = IONetworkMedium::getMediumWithType(
            getMediumDictionary(), kIOMediumEthernetAuto);
        setLinkStatus(kIONetworkLinkValid |
                      (associated ? kIONetworkLinkActive : 0),
                      associated ? automatic : nullptr);
    }
    IOLockUnlock(fNetworkLock);
}

IOReturn MarvellW8897Controller::getHardwareAddress(IOEthernetAddress* address)
{
    if (!address || !__atomic_load_n(&fMacValid, __ATOMIC_ACQUIRE))
        return kIOReturnNotReady;
    bcopy(fMacAddress, address->bytes, sizeof(fMacAddress));
    return kIOReturnSuccess;
}

IOReturn MarvellW8897Controller::enable(IONetworkInterface* interface)
{
    if (!interface ||
        interface != __atomic_load_n(&fNetworkInterface, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&fStopping, __ATOMIC_ACQUIRE) || !fMlanContext)
        return kIOReturnNotReady;
    __atomic_store_n(&fNetworkEnabled, true, __ATOMIC_RELEASE);
    return kIOReturnSuccess;
}

IOReturn MarvellW8897Controller::disable(IONetworkInterface* interface)
{
    if (!interface ||
        (interface != __atomic_load_n(&fNetworkInterface, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&fStopping, __ATOMIC_ACQUIRE)))
        return kIOReturnBadArgument;
    __atomic_store_n(&fNetworkEnabled, false, __ATOMIC_RELEASE);
    return kIOReturnSuccess;
}

UInt32 MarvellW8897Controller::outputPacket(mbuf_t packet, void*)
{
    if (!packet) return kIOReturnOutputDropped;
    const size_t length = mbuf_pkthdr_len(packet);
    if (length < 14 || length > 1514 || !fNetworkLock) {
        freePacket(packet);
        return kIOReturnOutputDropped;
    }
    if (!IOLockTryLock(fNetworkLock)) {
        __atomic_fetch_add(&fTxLockDrops, 1ULL, __ATOMIC_RELAXED);
        freePacket(packet);
        return kIOReturnOutputDropped;
    }
    bool sent = false;
    if (!__atomic_load_n(&fStopping, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&fNetworkEnabled, __ATOMIC_ACQUIRE) &&
        fLinkAssociated && fMlanContext) {
        sent = MarvellW8897MlanHost::sendEthernetFrame(
            fMlanContext, packet, static_cast<unsigned>(length));
    }
    IOLockUnlock(fNetworkLock);
    freePacket(packet);
    return sent ? kIOReturnOutputSuccess : kIOReturnOutputDropped;
}

bool MarvellW8897Controller::receiveEthernetFrame(const uint8_t* bytes, uint32_t length)
{
    if (!bytes || length < 14 || length > 4096 || !fNetworkLock) return false;
    // RX callbacks are drained by unregisterCore before the lock is freed.
    // Allocate and copy before taking the shared TX/link-state lock.
    mbuf_t packet = allocatePacket(length);
    if (!packet) return false;
    if (mbuf_copyback(packet, 0, length, bytes, MBUF_DONTWAIT) != 0) {
        freePacket(packet);
        return false;
    }
    if (!IOLockTryLock(fNetworkLock)) {
        __atomic_fetch_add(&fRxLockDrops, 1ULL, __ATOMIC_RELAXED);
        freePacket(packet);
        return false;
    }
    IOEthernetInterface* interface = nullptr;
    if (!__atomic_load_n(&fStopping, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&fNetworkEnabled, __ATOMIC_ACQUIRE) &&
        fLinkAssociated && fNetworkInterface) {
        interface = fNetworkInterface;
        interface->retain();
        ++fRxInFlight;
    }
    IOLockUnlock(fNetworkLock);
    if (!interface) {
        freePacket(packet);
        return false;
    }
    // Direct handoff avoids sharing the interface's unprotected input queue.
    // stop() waits for this handoff before detaching the interface.
    interface->inputPacket(packet, length, 0);
    IOLockLock(fNetworkLock);
    if (--fRxInFlight == 0) IOLockWakeup(fNetworkLock, &fRxInFlight, false);
    IOLockUnlock(fNetworkLock);
    interface->release();
    return true;
}
