#ifndef _MARVELL_W8897_REGS_HPP
#define _MARVELL_W8897_REGS_HPP

// Shared kext headers must not rely on userspace transitive includes for integer
// definitions. Keep the kernel-proven Apple IOKit fixed-width typedefs available
// explicitly in this shared header.
#include <IOKit/IOTypes.h>

namespace MarvellW8897Regs {

static constexpr UInt32 kMaxBars = 6;
static constexpr UInt32 kMaxResources = 8;
static constexpr UInt32 kInvalidResourceIndex = 0xFFFFFFFFU;

static constexpr UInt32 kBarInfoLegacyCount = 8;
static constexpr UInt32 kBarInfoV2Count = 11;
static constexpr UInt32 kTransportStateCount = 8;
static constexpr UInt32 kBringupResultCount = 12;
static constexpr UInt32 kDebugAbiInfoCount = 6;

static constexpr UInt64 kDebugSelectorBitmap =
    (1ULL << 0) |
    (1ULL << 1) |
    (1ULL << 2) |
    (1ULL << 3) |
    (1ULL << 4) |
    (1ULL << 5) |
    (1ULL << 6) |
    (1ULL << 7) |
    (1ULL << 8) |
    (1ULL << 9) |
    (1ULL << 10) |
    (1ULL << 11) |
    (1ULL << 12) |
    (1ULL << 13) |
    (1ULL << 14) |
    (1ULL << 15) |
    (1ULL << 16);

} // namespace MarvellW8897Regs

#endif
