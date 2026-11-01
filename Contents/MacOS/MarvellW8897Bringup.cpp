#include "MarvellW8897Bringup.hpp"
#include <IOKit/IOLib.h>

namespace {

// mwifiex maps PCI BAR2 as its register window for 88W8897. BAR0 is a
// separate memory region and must not be used for the scratch registers.
constexpr UInt32 kBringupBarIndex = 2;
constexpr UInt32 kRegisterWidth = sizeof(UInt32);

bool validateAccess(const MarvellW8897PciBarState& pciBars,
                    MarvellW8897BringupResult& result,
                    UInt32 barIndex,
                    UInt32 offset)
{
    result.lastBarIndex = barIndex;
    result.lastRegisterOffset = offset;

    if (barIndex >= MarvellW8897Regs::kMaxBars) {
        result.failureReason = MarvellW8897BringupFailureReason::InvalidBar;
        result.stage = MarvellW8897BringupStage::Failed;
        return false;
    }

    if (!MarvellW8897PciBars::isMmioAccessValid(pciBars, barIndex, offset, kRegisterWidth)) {
        result.failureReason = (barIndex == kBringupBarIndex)
                                   ? MarvellW8897BringupFailureReason::UnavailableRegisterBar
                                   : MarvellW8897BringupFailureReason::InvalidOffset;
        result.stage = MarvellW8897BringupStage::Failed;
        return false;
    }

    return true;
}

} // namespace

namespace MarvellW8897Bringup {

void reset(MarvellW8897BringupResult& result)
{
    result = MarvellW8897BringupResult{};
}

const char* stageName(MarvellW8897BringupStage stage)
{
    switch (stage) {
        case MarvellW8897BringupStage::NotStarted:
            return "not-started";
        case MarvellW8897BringupStage::PciEnabled:
            return "pci-enabled";
        case MarvellW8897BringupStage::BarsMapped:
            return "bars-mapped";
        case MarvellW8897BringupStage::SanityChecked:
            return "sanity-checked";
        case MarvellW8897BringupStage::ResetAttempted:
            return "reset-attempted";
        case MarvellW8897BringupStage::ResetObserved:
            return "reset-observed";
        case MarvellW8897BringupStage::ReadyObserved:
            return "ready-observed";
        case MarvellW8897BringupStage::TimedOut:
            return "timed-out";
        case MarvellW8897BringupStage::Unsupported:
            return "unsupported";
        case MarvellW8897BringupStage::Failed:
            return "failed";
    }

    return "unknown";
}

const char* failureReasonName(MarvellW8897BringupFailureReason reason)
{
    switch (reason) {
        case MarvellW8897BringupFailureReason::None:
            return "none";
        case MarvellW8897BringupFailureReason::InvalidBar:
            return "invalid-bar";
        case MarvellW8897BringupFailureReason::InvalidOffset:
            return "invalid-offset";
        case MarvellW8897BringupFailureReason::ReadFailed:
            return "read-failed";
        case MarvellW8897BringupFailureReason::WriteFailed:
            return "write-failed";
        case MarvellW8897BringupFailureReason::VerifyMismatch:
            return "verify-mismatch";
        case MarvellW8897BringupFailureReason::Timeout:
            return "timeout";
        case MarvellW8897BringupFailureReason::NoGroundedResetSemantic:
            return "no-grounded-reset-semantic";
        case MarvellW8897BringupFailureReason::NoTransportProfile:
            return "no-transport-profile";
        case MarvellW8897BringupFailureReason::UnavailableRegisterBar:
            return "unavailable-register-bar";
    }

    return "unknown";
}

bool pollReg32MaskedEq(const MarvellW8897PciBarState& pciBars,
                       MarvellW8897BringupResult& result,
                       UInt32 barIndex,
                       UInt32 offset,
                       UInt32 mask,
                       UInt32 expected,
                       UInt32 maxPolls,
                       UInt32 delayUsec)
{
    if (!validateAccess(pciBars, result, barIndex, offset)) {
        return false;
    }

    if (maxPolls == 0) {
        result.failureReason = MarvellW8897BringupFailureReason::Timeout;
        result.stage = MarvellW8897BringupStage::TimedOut;
        return false;
    }

    result.pollIterations = 0;

    for (UInt32 i = 0; i < maxPolls; ++i) {
        UInt32 value = 0;
        if (!MarvellW8897Transport::readReg32FromBar(pciBars, barIndex, offset, value)) {
            result.failureReason = MarvellW8897BringupFailureReason::ReadFailed;
            result.stage = MarvellW8897BringupStage::Failed;
            result.pollIterations = i + 1;
            return false;
        }

        result.lastValueRead = value;
        result.pollIterations = i + 1;

        if ((value & mask) == expected) {
            return true;
        }

        if ((i + 1) < maxPolls && delayUsec != 0) {
            IODelay(delayUsec);
        }
    }

    result.failureReason = MarvellW8897BringupFailureReason::Timeout;
    result.stage = MarvellW8897BringupStage::TimedOut;
    return false;
}

bool pollReg32Eq(const MarvellW8897PciBarState& pciBars,
                 MarvellW8897BringupResult& result,
                 UInt32 barIndex,
                 UInt32 offset,
                 UInt32 expected,
                 UInt32 maxPolls,
                 UInt32 delayUsec)
{
    return pollReg32MaskedEq(pciBars,
                             result,
                             barIndex,
                             offset,
                             0xFFFFFFFFU,
                             expected,
                             maxPolls,
                             delayUsec);
}

bool writeReg32AndVerify(const MarvellW8897PciBarState& pciBars,
                         MarvellW8897BringupResult& result,
                         UInt32 barIndex,
                         UInt32 offset,
                         UInt32 value,
                         UInt32 verifyMask,
                         UInt32 maxPolls,
                         UInt32 delayUsec)
{
    if (!validateAccess(pciBars, result, barIndex, offset)) {
        return false;
    }

    result.lastValueWritten = value;

    if (!MarvellW8897Transport::writeReg32ToBar(pciBars, barIndex, offset, value)) {
        result.failureReason = MarvellW8897BringupFailureReason::WriteFailed;
        result.stage = MarvellW8897BringupStage::Failed;
        return false;
    }

    const bool matched = pollReg32MaskedEq(pciBars,
                                           result,
                                           barIndex,
                                           offset,
                                           verifyMask,
                                           value & verifyMask,
                                           maxPolls,
                                           delayUsec);
    if (!matched && result.failureReason == MarvellW8897BringupFailureReason::Timeout) {
        result.failureReason = MarvellW8897BringupFailureReason::VerifyMismatch;
        result.stage = MarvellW8897BringupStage::Failed;
    }

    return matched;
}

bool run(const MarvellW8897PciBarState& pciBars,
         MarvellW8897TransportState& transportState,
         MarvellW8897BringupResult& result)
{
    reset(result);
    result.stage = MarvellW8897BringupStage::PciEnabled;

    const MarvellW8897TransportPcieProfile* profile =
        MarvellW8897Transport::getPcieProfile(transportState);
    if (!profile) {
        result.failureReason = MarvellW8897BringupFailureReason::NoTransportProfile;
        result.stage = MarvellW8897BringupStage::Unsupported;
        IOLog("MarvellW8897: bring-up has no configured mwifiex-style PCIe profile\n");
        return true;
    }

    const UInt32 sanityRegisters[] = {
        profile->firmwareStatusReg,
        profile->driverReadyReg,
        profile->txReadPointerReg,
        profile->txWritePointerReg,
        profile->rxReadPointerReg,
        profile->rxWritePointerReg,
        profile->eventReadPointerReg,
        profile->eventWritePointerReg,
    };

    for (UInt32 reg : sanityRegisters) {
        if (!validateAccess(pciBars, result, kBringupBarIndex, reg)) {
            IOLog("MarvellW8897: bring-up failed before sanity stage: bar=%u offset=0x%x reason=%s\n",
                  result.lastBarIndex,
                  result.lastRegisterOffset,
                  failureReasonName(result.failureReason));
            return false;
        }
    }

    result.stage = MarvellW8897BringupStage::BarsMapped;
    result.pollIterations = 0;
    result.lastValueWritten = 0;
    result.quirkFlags = profile->quirkFlags;
    result.stage = MarvellW8897BringupStage::SanityChecked;

    transportState.firmwareReady = 0;
    result.resetAttempted = false;
    result.readyObserved = false;
    result.failureReason = MarvellW8897BringupFailureReason::NoGroundedResetSemantic;
    result.stage = MarvellW8897BringupStage::Unsupported;

    IOLog("MarvellW8897: PCIe profile mapped; fw=%s quirks=0x%x fwStatusReg=0x%x drvRdyReg=0x%x txWr=0x%x evtWr=0x%x MMIO deferred until manual firmware upload (%s)\n",
          profile->defaultFirmwareName ? profile->defaultFirmwareName : "<none>",
          profile->quirkFlags,
          profile->firmwareStatusReg,
          profile->driverReadyReg,
          profile->txWritePointerReg,
          profile->eventWritePointerReg,
          failureReasonName(result.failureReason));

    return true;
}

} // namespace MarvellW8897Bringup
