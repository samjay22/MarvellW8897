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

#define super IO80211Controller
OSDefineMetaClassAndStructors(MarvellW8897Controller, IO80211Controller)

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
    // A retry must not overlap the previous invocation; stop() can then
    // cancel the retry and wait for an in-flight invocation before teardown.
    fAutoStartCall = thread_call_allocate_with_options(
        sAutoStart, this, THREAD_CALL_PRIORITY_HIGH, THREAD_CALL_OPTIONS_ONCE);
    if (fAutoStartCall) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
        thread_call_enter_delayed(fAutoStartCall, deadline);
        IOLog("MarvellW8897: embedded firmware startup scheduled\n");
    } else {
        IOLog("MarvellW8897: could not schedule embedded firmware startup\n");
    }
    fNativeScanDoneCall = thread_call_allocate_with_options(
        sNativeScanDone, this, THREAD_CALL_PRIORITY_HIGH, THREAD_CALL_OPTIONS_ONCE);
    if (!fNativeScanDoneCall)
        IOLog("MarvellW8897: native Wi-Fi scan notifications unavailable\n");
    return true;
}

void MarvellW8897Controller::stop(IOService* provider)
{
    IOLog("MarvellW8897: stop() called\n");

    IO80211Interface* interface = nullptr;
    if (fNetworkLock) {
        IOLockLock(fNetworkLock);
        __atomic_store_n(&fStopping, true, __ATOMIC_RELEASE);
        __atomic_store_n(&fNetworkEnabled, false, __ATOMIC_RELEASE);
        fLinkAssociated = false;
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

    if (fNativeScanDoneCall) {
        thread_call_cancel_wait(fNativeScanDoneCall);
        thread_call_free(fNativeScanDoneCall);
        fNativeScanDoneCall = nullptr;
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
            controller->fNetworkLock) {
            bool scheduled = false;
            IOLockLock(controller->fNetworkLock);
            if (!__atomic_load_n(&controller->fStopping, __ATOMIC_ACQUIRE) &&
                controller->fAutoStartCall) {
                uint64_t deadline = 0;
                clock_interval_to_deadline(kAutoRetryDelayMs, kMillisecondScale, &deadline);
                thread_call_enter_delayed(controller->fAutoStartCall, deadline);
                scheduled = true;
            }
            IOLockUnlock(controller->fNetworkLock);
            if (scheduled)
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

bool MarvellW8897Controller::getMlanTelemetry(uint64_t out[13])
{
    if (!out || !fFirmwareLock) return false;
    IOLockLock(fFirmwareLock);
    if (!fMlanContext) {
        bzero(out, 13 * sizeof(uint64_t));
        IOLockUnlock(fFirmwareLock);
        return true;
    }
    unsigned long long telemetry[13] = {};
    const bool ok = MarvellW8897MlanHost::getTelemetry(fMlanContext, telemetry);
    for (unsigned i = 0; i < 13; ++i) out[i] = telemetry[i];
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

IOReturn MarvellW8897Controller::requestMlanConnectPmk(
    const uint8_t* ssid, uint32_t ssidLength,
    const uint8_t* pmk, uint32_t pmkLength)
{
    if (!ssid || ssidLength == 0 || ssidLength > 32 || !pmk || pmkLength != 32)
        return kIOReturnBadArgument;
    if (!fFirmwareLock) return kIOReturnNotReady;
    IOLockLock(fFirmwareLock);
    if (!fMlanContext || !fMlanInitStarted ||
        __atomic_load_n(&fStopping, __ATOMIC_ACQUIRE)) {
        IOLockUnlock(fFirmwareLock);
        return kIOReturnNotReady;
    }
    const unsigned status = MarvellW8897MlanHost::requestConnectPmk(
        fMlanContext, ssid, ssidLength, pmk, pmkLength);
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
    IO80211Interface* linkInterface = nullptr;
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
            kIOMediumIEEE80211Auto, 300000000ULL);
        IONetworkInterface* attached = nullptr;
        if (!media || !automatic || !IONetworkMedium::addMedium(media, automatic) ||
            !publishMediumDictionary(media) ||
            !attachInterface(&attached, false)) {
            IOLog("MarvellW8897: could not attach network interface\n");
        } else {
            IO80211Interface* network = OSDynamicCast(IO80211Interface, attached);
            if (network) {
                __atomic_store_n(&fNetworkInterface, network, __ATOMIC_RELEASE);
                setCurrentMedium(automatic);
                // Matching may call enable(), so publish our pointer first.
                network->registerService();
                IOLog("MarvellW8897: native IO80211 interface attached\n");
            } else {
                detachInterface(attached, false);
                attached->release();
                IOLog("MarvellW8897: interface has an unexpected class\n");
            }
        }
        if (automatic) automatic->release();
        if (media) media->release();
    }

    const bool linkChanged = fLinkAssociated != associated;
    fLinkAssociated = associated;
    if (fNetworkInterface) {
        const IONetworkMedium* automatic = IONetworkMedium::getMediumWithType(
            getMediumDictionary(),
            kIOMediumIEEE80211Auto);
        setLinkStatus(kIONetworkLinkValid |
                      (associated ? kIONetworkLinkActive : 0),
                      associated ? automatic : nullptr);
        if (linkChanged) {
            linkInterface = fNetworkInterface;
            linkInterface->retain();
        }
    }
    IOLockUnlock(fNetworkLock);
    // IO80211 clients use this message to refresh the menu-bar and System
    // Settings state. Do not post it while the controller lock is held.
    if (linkInterface) {
        linkInterface->postMessage(APPLE80211_M_LINK_CHANGED);
        linkInterface->release();
    }
}

void MarvellW8897Controller::mlanScanResultsReady()
{
    if (!fNativeScanDoneCall || __atomic_load_n(&fStopping, __ATOMIC_ACQUIRE))
        return;
    thread_call_enter(fNativeScanDoneCall);
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
        sent = MarvellW8897MlanHost::sendDataFrame(
            fMlanContext, packet, static_cast<unsigned>(length));
    }
    IOLockUnlock(fNetworkLock);
    freePacket(packet);
    return sent ? kIOReturnOutputSuccess : kIOReturnOutputDropped;
}

bool MarvellW8897Controller::receiveDataFrame(const uint8_t* bytes, uint32_t length)
{
    if (!bytes || length < 14 || length > 4096 || !fNetworkLock) return false;
    if (!IOLockTryLock(fNetworkLock)) {
        __atomic_fetch_add(&fRxLockDrops, 1ULL, __ATOMIC_RELAXED);
        return false;
    }
    bool delivered = false;
    IO80211Interface* queuedInterface = nullptr;
    if (!__atomic_load_n(&fStopping, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&fNetworkEnabled, __ATOMIC_ACQUIRE) &&
        fLinkAssociated && fNetworkInterface) {
        mbuf_t packet = allocatePacket(length);
        if (packet) {
            if (mbuf_copyback(packet, 0, length, bytes, MBUF_DONTWAIT) == 0) {
                queuedInterface = fNetworkInterface;
                queuedInterface->retain();
                fNetworkInterface->inputPacket(
                    packet, length, IONetworkInterface::kInputOptionQueuePacket);
                delivered = true;
            } else {
                freePacket(packet);
            }
        }
    }
    IOLockUnlock(fNetworkLock);
    if (queuedInterface) {
        queuedInterface->flushInputQueue();
        queuedInterface->release();
    }
    return delivered;
}

void MarvellW8897Controller::sNativeScanDone(thread_call_param_t owner,
                                             thread_call_param_t)
{
    auto* controller = static_cast<MarvellW8897Controller*>(owner);
    if (!controller || !controller->fNetworkLock ||
        __atomic_load_n(&controller->fStopping, __ATOMIC_ACQUIRE))
        return;
    IOLockLock(controller->fNetworkLock);
    IO80211Interface* interface = controller->fNetworkInterface;
    if (interface) interface->retain();
    IOLockUnlock(controller->fNetworkLock);
    if (!interface) return;
    interface->postMessage(APPLE80211_M_SCAN_DONE);
    interface->release();
}

namespace {

UInt32 appleChannelFlags(UInt32 channel)
{
    return APPLE80211_C_FLAG_ACTIVE | APPLE80211_C_FLAG_20MHZ |
        (channel <= 14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ);
}

SInt32 toAppleStatus(IOReturn status)
{
    return static_cast<SInt32>(status);
}

} // namespace

SInt32 MarvellW8897Controller::apple80211Request(unsigned int requestType,
                                                  int requestNumber,
                                                  IO80211Interface* interface,
                                                  void* data)
{
    if (requestType != SIOCGA80211 && requestType != SIOCSA80211)
        return toAppleStatus(kIOReturnBadArgument);
    if (!interface || interface != __atomic_load_n(&fNetworkInterface,
                                                    __ATOMIC_ACQUIRE))
        return toAppleStatus(kIOReturnNotReady);

    switch (requestNumber) {
    case APPLE80211_IOC_SCAN_REQ: {
        if (requestType != SIOCSA80211 || !data)
            return toAppleStatus(kIOReturnBadArgument);
        const auto* request = static_cast<const apple80211_scan_data*>(data);
        if (request->ssid_len > sizeof(request->ssid))
            return toAppleStatus(kIOReturnBadArgument);
        const IOReturn status = requestMlanScan(
            request->ssid_len ? request->ssid : nullptr, request->ssid_len);
        if (status == kIOReturnSuccess && fNetworkLock) {
            IOLockLock(fNetworkLock);
            fNativeScanIndex = 0;
            IOLockUnlock(fNetworkLock);
        }
        return toAppleStatus(status);
    }

    case APPLE80211_IOC_SCAN_RESULT: {
        if (requestType != SIOCGA80211 || !data || !fFirmwareLock || !fNetworkLock)
            return toAppleStatus(kIOReturnBadArgument);
        auto** output = static_cast<apple80211_scan_result**>(data);
        UInt32 index = 0;
        IOLockLock(fNetworkLock);
        index = fNativeScanIndex;
        IOLockUnlock(fNetworkLock);

        MarvellW8897MlanHost::ScanEntry entry {};
        IOLockLock(fFirmwareLock);
        const bool available = fMlanContext &&
            MarvellW8897MlanHost::getScanEntry(fMlanContext, index, entry);
        IOLockUnlock(fFirmwareLock);
        if (!available) return toAppleStatus(kIOReturnNotFound);

        IOLockLock(fNetworkLock);
        bzero(&fNativeScanResult, sizeof(fNativeScanResult));
        fNativeScanResult.version = APPLE80211_VERSION;
        fNativeScanResult.asr_channel.version = APPLE80211_VERSION;
        fNativeScanResult.asr_channel.channel = entry.channel;
        fNativeScanResult.asr_channel.flags = appleChannelFlags(entry.channel);
        fNativeScanResult.asr_rssi = entry.rssiDbm;
        fNativeScanResult.asr_beacon_int = static_cast<int16_t>(entry.beaconInterval);
        fNativeScanResult.asr_cap = static_cast<int16_t>(entry.capabilities);
        bcopy(entry.bssid, fNativeScanResult.asr_bssid,
              sizeof(fNativeScanResult.asr_bssid));
        fNativeScanResult.asr_ssid_len = entry.ssidLength;
        bcopy(entry.ssid, fNativeScanResult.asr_ssid, entry.ssidLength);
        fNativeScanResult.asr_ie_len = 0;
        fNativeScanResult.asr_ie_data = nullptr;
        *output = &fNativeScanResult;
        ++fNativeScanIndex;
        IOLockUnlock(fNetworkLock);
        return toAppleStatus(kIOReturnSuccess);
    }

    case APPLE80211_IOC_ASSOCIATE: {
        if (requestType != SIOCSA80211 || !data)
            return toAppleStatus(kIOReturnBadArgument);
        const auto* association = static_cast<const apple80211_assoc_data*>(data);
        if (association->ad_mode != APPLE80211_AP_MODE_INFRA ||
            association->ad_ssid_len == 0 ||
            association->ad_ssid_len > sizeof(association->ad_ssid))
            return toAppleStatus(kIOReturnBadArgument);

        IOReturn status = kIOReturnUnsupported;
        const bool open = association->ad_auth_upper == APPLE80211_AUTHTYPE_NONE;
        if (open && association->ad_key.key_len == 0) {
            status = requestMlanConnect(association->ad_ssid,
                                        association->ad_ssid_len, nullptr, 0);
        } else if (association->ad_key.key_len == 32) {
            status = requestMlanConnectPmk(association->ad_ssid,
                                           association->ad_ssid_len,
                                           association->ad_key.key, 32);
        }
        if (status == kIOReturnSuccess && fNetworkLock) {
            IOLockLock(fNetworkLock);
            bzero(fNativeSsid, sizeof(fNativeSsid));
            bcopy(association->ad_ssid, fNativeSsid, association->ad_ssid_len);
            fNativeSsidLength = association->ad_ssid_len;
            fNativeAuthLower = association->ad_auth_lower;
            fNativeAuthUpper = association->ad_auth_upper;
            IOLockUnlock(fNetworkLock);
        }
        return toAppleStatus(status);
    }

    case APPLE80211_IOC_SSID: {
        if (requestType != SIOCGA80211 || !data || !fNetworkLock)
            return toAppleStatus(kIOReturnBadArgument);
        auto* ssid = static_cast<apple80211_ssid_data*>(data);
        IOLockLock(fNetworkLock);
        bzero(ssid, sizeof(*ssid));
        ssid->version = APPLE80211_VERSION;
        if (fLinkAssociated) {
            ssid->ssid_len = fNativeSsidLength;
            bcopy(fNativeSsid, ssid->ssid_bytes, fNativeSsidLength);
        }
        IOLockUnlock(fNetworkLock);
        return toAppleStatus(kIOReturnSuccess);
    }

    case APPLE80211_IOC_AUTH_TYPE: {
        if (!data || !fNetworkLock) return toAppleStatus(kIOReturnBadArgument);
        auto* auth = static_cast<apple80211_authtype_data*>(data);
        IOLockLock(fNetworkLock);
        if (requestType == SIOCGA80211) {
            auth->version = APPLE80211_VERSION;
            auth->authtype_lower = fNativeAuthLower;
            auth->authtype_upper = fNativeAuthUpper;
        } else {
            fNativeAuthLower = auth->authtype_lower;
            fNativeAuthUpper = auth->authtype_upper;
        }
        IOLockUnlock(fNetworkLock);
        return toAppleStatus(kIOReturnSuccess);
    }

    case APPLE80211_IOC_STATE: {
        if (requestType != SIOCGA80211 || !data || !fNetworkLock)
            return toAppleStatus(kIOReturnBadArgument);
        auto* state = static_cast<apple80211_state_data*>(data);
        IOLockLock(fNetworkLock);
        state->version = APPLE80211_VERSION;
        state->state = fLinkAssociated ? APPLE80211_S_RUN : APPLE80211_S_INIT;
        IOLockUnlock(fNetworkLock);
        return toAppleStatus(kIOReturnSuccess);
    }

    case APPLE80211_IOC_ASSOCIATE_RESULT: {
        if (requestType != SIOCGA80211 || !data || !fNetworkLock)
            return toAppleStatus(kIOReturnBadArgument);
        auto* result = static_cast<apple80211_assoc_result_data*>(data);
        IOLockLock(fNetworkLock);
        result->version = APPLE80211_VERSION;
        result->result = fLinkAssociated ? APPLE80211_RESULT_SUCCESS :
            APPLE80211_RESULT_UNAVAILABLE;
        IOLockUnlock(fNetworkLock);
        return toAppleStatus(kIOReturnSuccess);
    }

    case APPLE80211_IOC_CARD_CAPABILITIES: {
        if (requestType != SIOCGA80211 || !data)
            return toAppleStatus(kIOReturnBadArgument);
        auto* caps = static_cast<apple80211_capability_data*>(data);
        bzero(caps, sizeof(*caps));
        caps->version = APPLE80211_VERSION;
        // 8897 supports 802.11n plus firmware-handled WPA/WPA2; do not claim
        // WPA3, WEP, monitor mode, AWDL, or hosted-network support.
        caps->capabilities[APPLE80211_CAP_WPA2 / 8] |=
            static_cast<uint8_t>(1U << (APPLE80211_CAP_WPA2 % 8));
        return toAppleStatus(kIOReturnSuccess);
    }

    case APPLE80211_IOC_PHY_MODE: {
        if (requestType != SIOCGA80211 || !data)
            return toAppleStatus(kIOReturnBadArgument);
        auto* mode = static_cast<apple80211_phymode_data*>(data);
        mode->version = APPLE80211_VERSION;
        mode->phy_mode = APPLE80211_MODE_11A | APPLE80211_MODE_11B |
            APPLE80211_MODE_11G | APPLE80211_MODE_11N;
        mode->active_phy_mode = APPLE80211_MODE_11N;
        return toAppleStatus(kIOReturnSuccess);
    }

    case APPLE80211_IOC_OP_MODE: {
        if (requestType != SIOCGA80211 || !data)
            return toAppleStatus(kIOReturnBadArgument);
        auto* mode = static_cast<apple80211_opmode_data*>(data);
        mode->version = APPLE80211_VERSION;
        mode->op_mode = APPLE80211_M_STA;
        return toAppleStatus(kIOReturnSuccess);
    }

    case APPLE80211_IOC_SUPPORTED_CHANNELS:
    case APPLE80211_IOC_HW_SUPPORTED_CHANNELS: {
        if (requestType != SIOCGA80211 || !data)
            return toAppleStatus(kIOReturnBadArgument);
        auto* channels = static_cast<apple80211_sup_channel_data*>(data);
        static const UInt32 kChannels[] = {
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
            36, 40, 44, 48, 149, 153, 157, 161
        };
        bzero(channels, sizeof(*channels));
        channels->version = APPLE80211_VERSION;
        channels->num_channels = sizeof(kChannels) / sizeof(kChannels[0]);
        for (UInt32 i = 0; i < channels->num_channels; ++i) {
            channels->supported_channels[i].version = APPLE80211_VERSION;
            channels->supported_channels[i].channel = kChannels[i];
            channels->supported_channels[i].flags = appleChannelFlags(kChannels[i]);
        }
        return toAppleStatus(kIOReturnSuccess);
    }

    case APPLE80211_IOC_POWER: {
        if (!data) return toAppleStatus(kIOReturnBadArgument);
        auto* power = static_cast<apple80211_power_data*>(data);
        if (requestType == SIOCGA80211) {
            bzero(power, sizeof(*power));
            power->version = APPLE80211_VERSION;
            power->num_radios = 1;
            power->power_state[0] = APPLE80211_POWER_ON;
        }
        return toAppleStatus(kIOReturnSuccess);
    }

    case APPLE80211_IOC_RSN_IE:
        // Firmware owns the embedded WPA2 supplicant and receives the
        // corresponding PMK through APPLE80211_IOC_ASSOCIATE.
        return toAppleStatus(kIOReturnSuccess);

    default:
        return toAppleStatus(kIOReturnUnsupported);
    }
}

SInt32 MarvellW8897Controller::stopDMA()
{
    // Teardown is coordinated by stop(): it first gates outgoing packets,
    // then disables the PCI interrupt source before releasing MLAN state.
    return toAppleStatus(kIOReturnSuccess);
}

UInt32 MarvellW8897Controller::hardwareOutputQueueDepth(IO80211Interface*)
{
    // The NXP core owns the hardware descriptor queues and reports completion
    // asynchronously; do not let IO80211 assume a separate software queue.
    return 0;
}

SInt32 MarvellW8897Controller::performCountryCodeOperation(
    IO80211Interface*, IO80211CountryCodeOp)
{
    // Country selection is firmware-managed. Reporting a fabricated region
    // would be worse than retaining the firmware's regulatory defaults.
    return toAppleStatus(kIOReturnSuccess);
}

SInt32 MarvellW8897Controller::enableFeature(IO80211FeatureCode code, void*)
{
    return code == kIO80211Feature80211n ? toAppleStatus(kIOReturnSuccess) :
        toAppleStatus(kIOReturnUnsupported);
}

bool MarvellW8897Controller::useAppleRSNSupplicant(IO80211Interface*)
{
    // Association uses the 8897 firmware's WPA2 supplicant with the PMK handed
    // over by the macOS association request. Apple should not install a second
    // supplicant/key path for the same radio.
    return false;
}
