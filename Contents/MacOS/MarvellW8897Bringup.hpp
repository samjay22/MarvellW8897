#ifndef _MARVELL_W8897_BRINGUP_HPP
#define _MARVELL_W8897_BRINGUP_HPP

#include "MarvellW8897PciBars.hpp"
#include "MarvellW8897Transport.hpp"

// Internal-only bring-up staging for conservative post-Phase-A orchestration.
// This state is intentionally not exported through the UserClient ABI.
enum class MarvellW8897BringupStage : UInt32 {
    NotStarted = 0,
    PciEnabled,
    BarsMapped,
    SanityChecked,
    ResetAttempted,
    ResetObserved,
    ReadyObserved,
    TimedOut,
    Unsupported,
    Failed,
};

enum class MarvellW8897BringupFailureReason : UInt32 {
    None = 0,
    InvalidBar,
    InvalidOffset,
    ReadFailed,
    WriteFailed,
    VerifyMismatch,
    Timeout,
    NoGroundedResetSemantic,
    NoTransportProfile,
    UnavailableRegisterBar,
};

struct MarvellW8897BringupResult {
    MarvellW8897BringupStage         stage { MarvellW8897BringupStage::NotStarted };
    MarvellW8897BringupFailureReason failureReason { MarvellW8897BringupFailureReason::None };
    bool                             resetAttempted { false };
    bool                             readyObserved { false };
    UInt32                           lastBarIndex { MarvellW8897Regs::kInvalidResourceIndex };
    UInt32                           lastRegisterOffset { 0 };
    UInt32                           lastValueRead { 0 };
    UInt32                           lastValueWritten { 0 };
    UInt32                           pollIterations { 0 };
    UInt32                           quirkFlags { 0 };
    UInt32                           firmwareStatusValue { 0 };
    UInt32                           driverReadyValue { 0 };
};

namespace MarvellW8897Bringup {

void reset(MarvellW8897BringupResult& result);
const char* stageName(MarvellW8897BringupStage stage);
const char* failureReasonName(MarvellW8897BringupFailureReason reason);

bool pollReg32MaskedEq(const MarvellW8897PciBarState& pciBars,
                       MarvellW8897BringupResult& result,
                       UInt32 barIndex,
                       UInt32 offset,
                       UInt32 mask,
                       UInt32 expected,
                       UInt32 maxPolls,
                       UInt32 delayUsec);

bool pollReg32Eq(const MarvellW8897PciBarState& pciBars,
                 MarvellW8897BringupResult& result,
                 UInt32 barIndex,
                 UInt32 offset,
                 UInt32 expected,
                 UInt32 maxPolls,
                 UInt32 delayUsec);

bool writeReg32AndVerify(const MarvellW8897PciBarState& pciBars,
                         MarvellW8897BringupResult& result,
                         UInt32 barIndex,
                         UInt32 offset,
                         UInt32 value,
                         UInt32 verifyMask,
                         UInt32 maxPolls,
                         UInt32 delayUsec);

bool run(const MarvellW8897PciBarState& pciBars,
         MarvellW8897TransportState& transportState,
         MarvellW8897BringupResult& result);

} // namespace MarvellW8897Bringup

#endif
