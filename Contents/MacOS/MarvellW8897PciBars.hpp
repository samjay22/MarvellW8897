#ifndef _MARVELL_W8897_PCIBARS_HPP
#define _MARVELL_W8897_PCIBARS_HPP

#include "MarvellW8897Regs.hpp"
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

struct MarvellW8897BarInfo {
    IOMemoryMap*   directMap { nullptr };
    volatile void* mappedPtr { nullptr };
    UInt64       length { 0 };
    UInt32       mappedResource { MarvellW8897Regs::kInvalidResourceIndex };
    UInt32       rawLow { 0 };
    UInt32       rawHigh { 0 };
    UInt64       configValue { 0 };
    bool           isMemory { false };
    bool           is64Bit { false };
    bool           isContinuation { false };
    bool           present { false };
    const char*    mappingStatus { "not-attempted" };
};

struct MarvellW8897ResourceInfo {
    UInt64       length { 0 };
    UInt64       physicalAddress { 0 };
    bool           claimed { false };
};

struct MarvellW8897PciBarState {
    MarvellW8897BarInfo      bars[MarvellW8897Regs::kMaxBars] {};
    MarvellW8897ResourceInfo resources[MarvellW8897Regs::kMaxResources] {};
    UInt32                 resourceCount { 0 };
    UInt32                 resourceStatus { 0 };
};

namespace MarvellW8897PciBars {

void reset(MarvellW8897PciBarState& state);
void release(MarvellW8897PciBarState& state);
bool mapDeviceBars(IOPCIDevice* pciDevice, MarvellW8897PciBarState& state);
bool isMmioAccessValid(const MarvellW8897PciBarState& state,
                       UInt32 barIndex,
                       UInt32 offset,
                       UInt32 accessWidth);
bool getBarInfoLegacy(const MarvellW8897PciBarState& state,
                      UInt32 barIndex,
                      UInt64 outInfo[MarvellW8897Regs::kBarInfoLegacyCount]);
bool getBarInfoV2(const MarvellW8897PciBarState& state,
                  UInt32 barIndex,
                  UInt64 outInfo[MarvellW8897Regs::kBarInfoV2Count]);

} // namespace MarvellW8897PciBars

#endif
