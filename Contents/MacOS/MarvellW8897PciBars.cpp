#include "MarvellW8897PciBars.hpp"
#include <IOKit/IOLib.h>

namespace {

void logBarInfo(UInt32 barIndex, const MarvellW8897BarInfo& bar)
{
    const char* type = bar.isContinuation ? "CONT" : (bar.isMemory ? "MMIO" : "IO");
    const char* width = bar.isContinuation ? "-" : (bar.is64Bit ? "64" : "32");

    IOLog("MarvellW8897: BARslot%u rawLo=%08x rawHi=%08x cfg=%016llx present=%d type=%s width=%s continuation=%d\n",
          barIndex,
          bar.rawLow,
          bar.rawHigh,
          static_cast<unsigned long long>(bar.configValue),
          bar.present ? 1 : 0,
          type,
          width,
          bar.isContinuation ? 1 : 0);
}

} // namespace

namespace MarvellW8897PciBars {

void reset(MarvellW8897PciBarState& state)
{
    for (UInt32 i = 0; i < MarvellW8897Regs::kMaxResources; ++i) {
        state.resources[i] = MarvellW8897ResourceInfo{};
    }

    for (UInt32 i = 0; i < MarvellW8897Regs::kMaxBars; ++i) {
        state.bars[i] = MarvellW8897BarInfo{};
    }

    state.resourceCount = 0;
    state.resourceStatus = 0;
}

void release(MarvellW8897PciBarState& state)
{
    for (UInt32 i = 0; i < MarvellW8897Regs::kMaxBars; ++i) {
        if (state.bars[i].directMap) {
            state.bars[i].directMap->unmap();
            state.bars[i].directMap->release();
        }
    }
    reset(state);
}

bool mapDeviceBars(IOPCIDevice* pciDevice, MarvellW8897PciBarState& state)
{
    if (!pciDevice) {
        return false;
    }

    reset(state);

    // Read the BAR configuration before asking IOPCIFamily for its assigned
    // memory resources. A 64-bit BAR occupies two configuration slots.
    for (UInt32 bar = 0; bar < MarvellW8897Regs::kMaxBars; ++bar) {
        MarvellW8897BarInfo& info = state.bars[bar];
        const UInt32 barRegOffset = kIOPCIConfigBaseAddress0 + (bar * sizeof(UInt32));
        const bool preMarkedContinuation = info.isContinuation;

        info.rawLow = pciDevice->configRead32(barRegOffset);

        if (preMarkedContinuation) {
            info.rawHigh = 0;
            info.configValue = static_cast<UInt64>(info.rawLow);
            info.isMemory = false;
            info.is64Bit = false;
            info.present = false;
            logBarInfo(bar, info);
            continue;
        }

        info.isMemory = ((info.rawLow & 0x1U) == 0U);
        info.is64Bit = info.isMemory && ((info.rawLow & 0x6U) == 0x4U);

        if (info.is64Bit && (bar + 1) < MarvellW8897Regs::kMaxBars) {
            info.rawHigh = pciDevice->configRead32(barRegOffset + sizeof(UInt32));
            info.configValue =
                (static_cast<UInt64>(info.rawHigh) << 32) | static_cast<UInt64>(info.rawLow);
            state.bars[bar + 1].isContinuation = true;
        } else {
            info.rawHigh = 0;
            info.configValue = static_cast<UInt64>(info.rawLow);
        }

        if (info.isMemory) {
            const UInt64 addr = info.configValue & ~0xFULL;
            info.present = (addr != 0);
        } else {
            const UInt32 ioAddr = info.rawLow & ~0x3U;
            info.present = (ioAddr != 0);
        }

        logBarInfo(bar, info);
    }

    const IOReturn resourceStatus = pciDevice->getResources();
    state.resourceStatus = static_cast<UInt32>(resourceStatus);
    IOLog("MarvellW8897: PCI getResources returned 0x%x\n", resourceStatus);

    state.resourceCount = pciDevice->getDeviceMemoryCount();
    if (state.resourceCount > MarvellW8897Regs::kMaxResources) {
        state.resourceCount = MarvellW8897Regs::kMaxResources;
    }

    bool foundAnyResource = false;
    for (UInt32 resourceIndex = 0; resourceIndex < state.resourceCount; ++resourceIndex) {
        IODeviceMemory* memory = pciDevice->getDeviceMemoryWithIndex(resourceIndex);
        if (!memory) continue;

        MarvellW8897ResourceInfo& resource = state.resources[resourceIndex];
        resource.length = memory->getLength();
        resource.physicalAddress = memory->getPhysicalAddress();

        IOLog("MarvellW8897: resource%u paddr=%016llx len=%llu\n",
              resourceIndex,
              static_cast<unsigned long long>(resource.physicalAddress),
              static_cast<unsigned long long>(resource.length));

        foundAnyResource = true;
    }

    if (!foundAnyResource) {
        IOLog("MarvellW8897: no indexed IOKit memory resources found\n");
    }

    for (UInt32 bar = 0; bar < MarvellW8897Regs::kMaxBars; ++bar) {
        MarvellW8897BarInfo& info = state.bars[bar];
        if (!info.present || !info.isMemory || info.isContinuation) continue;

        // Match the assigned IOKit resource by its physical base address,
        // not its enumeration index. On this Surface BAR0 and BAR2 are both
        // 64-bit apertures; their resource indices need not equal BAR slots.
        const UInt64 expectedBase = info.configValue & ~0xFULL;
        bool indexedResourceMatched = false;
        for (UInt32 resourceIndex = 0; resourceIndex < state.resourceCount; ++resourceIndex) {
            MarvellW8897ResourceInfo& resource = state.resources[resourceIndex];
            if (resource.length == 0 || resource.claimed ||
                (resource.physicalAddress & ~0xFULL) != expectedBase) {
                continue;
            }

            indexedResourceMatched = true;

            IOMemoryMap* map = pciDevice->mapDeviceMemoryWithIndex(resourceIndex);
            if (!map || !map->getVirtualAddress() || map->getLength() == 0) {
                if (map) { map->unmap(); map->release(); }
                IOLog("MarvellW8897: BARslot%u resource%u could not be mapped\n",
                      bar, resourceIndex);
                continue;
            }

            info.directMap = map;
            info.mappedPtr = reinterpret_cast<volatile void*>(map->getVirtualAddress());
            info.length = map->getLength();
            info.mappedResource = resourceIndex;
            resource.claimed = true;
            info.mappingStatus = "indexed-resource";
            IOLog("MarvellW8897: BARslot%u mapped by resource%u (exact base match)\n",
                  bar, resourceIndex);
            break;
        }

        if (info.directMap) continue;

        // The register-selected API itself identifies the aperture assigned
        // to this BAR. Its descriptor's physical address can differ from the
        // raw PCI configuration value after platform resource translation,
        // so do not reject a valid descriptor for that difference.
        const UInt8 barRegister = static_cast<UInt8>(
            kIOPCIConfigBaseAddress0 + bar * sizeof(UInt32));
        IODeviceMemory* registerMemory = pciDevice->getDeviceMemoryWithRegister(barRegister);
        if (!registerMemory) {
            info.mappingStatus = indexedResourceMatched
                                     ? "indexed-map-failed-no-register"
                                     : "no-register-descriptor";
            IOLog("MarvellW8897: BARslot%u has no register descriptor\n", bar);
            continue;
        }
        IOLog("MarvellW8897: BARslot%u register descriptor paddr=%016llx len=%llu\n",
              bar,
              static_cast<unsigned long long>(registerMemory->getPhysicalAddress()),
              static_cast<unsigned long long>(registerMemory->getLength()));
        info.directMap = pciDevice->mapDeviceMemoryWithRegister(barRegister);
        if (!info.directMap || !info.directMap->getVirtualAddress() ||
            info.directMap->getLength() == 0) {
            if (info.directMap) { info.directMap->unmap(); info.directMap->release(); }
            info.directMap = nullptr;
            info.mappingStatus = "register-map-failed";
            IOLog("MarvellW8897: BARslot%u has no direct register mapping\n", bar);
            continue;
        }
        info.mappingStatus = "register-descriptor";
        info.mappedPtr = reinterpret_cast<volatile void*>(
            info.directMap->getVirtualAddress());
        info.length = info.directMap->getLength();
        for (UInt32 resourceIndex = 0; resourceIndex < state.resourceCount; ++resourceIndex) {
            MarvellW8897ResourceInfo& resource = state.resources[resourceIndex];
            if (resource.length == 0 || resource.claimed) {
                continue;
            }

            const UInt64 resourceBase = resource.physicalAddress & ~0xFULL;
            if (resourceBase != expectedBase) {
                continue;
            }

            info.mappedResource = resourceIndex;
            resource.claimed = true;

            IOLog("MarvellW8897: BARslot%u direct map -> resource%u (exact base match)\n",
                  bar, resourceIndex);
            break;
        }
    }

    bool mappedAnyBar = false;
    for (UInt32 bar = 0; bar < MarvellW8897Regs::kMaxBars; ++bar) {
        const MarvellW8897BarInfo& info = state.bars[bar];
        if (!info.directMap) {
            continue;
        }

        IOLog("MarvellW8897: BARslot%u active resource=%u len=%llu\n",
              bar,
              info.mappedResource,
              static_cast<unsigned long long>(info.length));
        mappedAnyBar = true;
    }

    if (!mappedAnyBar) {
        IOLog("MarvellW8897: no PCI BAR could be mapped by register\n");
        // Retain the raw BAR and resource metadata for the controller's
        // I/O Registry diagnostic properties. No mapping needs releasing.
        return false;
    }

    return true;
}

bool isMmioAccessValid(const MarvellW8897PciBarState& state,
                       UInt32 barIndex,
                       UInt32 offset,
                       UInt32 accessWidth)
{
    if (barIndex >= MarvellW8897Regs::kMaxBars) {
        IOLog("MarvellW8897: MMIO invalid BAR index %u\n", barIndex);
        return false;
    }

    const MarvellW8897BarInfo& bar = state.bars[barIndex];
    if (bar.isContinuation) {
        IOLog("MarvellW8897: MMIO BAR%u is 64-bit continuation BAR\n", barIndex);
        return false;
    }

    if (!bar.present) {
        IOLog("MarvellW8897: MMIO BAR%u not present\n", barIndex);
        return false;
    }

    if (!bar.isMemory) {
        IOLog("MarvellW8897: MMIO BAR%u is not memory BAR\n", barIndex);
        return false;
    }

    if (!bar.directMap) {
        IOLog("MarvellW8897: MMIO BAR%u has no direct register mapping\n", barIndex);
        return false;
    }

    if (!bar.mappedPtr || bar.length == 0) {
        IOLog("MarvellW8897: MMIO BAR%u not mapped\n", barIndex);
        return false;
    }

    if ((offset & (accessWidth - 1)) != 0) {
        IOLog("MarvellW8897: MMIO BAR%u unaligned access offset=0x%x width=%u\n",
              barIndex,
              offset,
              accessWidth);
        return false;
    }

    const UInt64 end = static_cast<UInt64>(offset) + static_cast<UInt64>(accessWidth);
    if (end > bar.length) {
        IOLog("MarvellW8897: MMIO BAR%u OOB access offset=0x%x width=%u len=%llu\n",
              barIndex,
              offset,
              accessWidth,
              static_cast<unsigned long long>(bar.length));
        return false;
    }

    return true;
}

bool getBarInfoLegacy(const MarvellW8897PciBarState& state,
                      UInt32 barIndex,
                      UInt64 outInfo[MarvellW8897Regs::kBarInfoLegacyCount])
{
    if (!outInfo || barIndex >= MarvellW8897Regs::kMaxBars) {
        return false;
    }

    const MarvellW8897BarInfo& bar = state.bars[barIndex];
    outInfo[0] = barIndex;
    outInfo[1] = bar.rawLow;
    outInfo[2] = bar.rawHigh;
    outInfo[3] = bar.configValue;
    outInfo[4] = bar.present ? 1 : 0;
    outInfo[5] = bar.isMemory ? 1 : 0;
    outInfo[6] = bar.is64Bit ? 1 : 0;
    outInfo[7] = bar.isContinuation ? 1 : 0;
    return true;
}

bool getBarInfoV2(const MarvellW8897PciBarState& state,
                  UInt32 barIndex,
                  UInt64 outInfo[MarvellW8897Regs::kBarInfoV2Count])
{
    if (!outInfo || barIndex >= MarvellW8897Regs::kMaxBars) {
        return false;
    }

    const MarvellW8897BarInfo& bar = state.bars[barIndex];
    outInfo[0] = barIndex;
    outInfo[1] = bar.rawLow;
    outInfo[2] = bar.rawHigh;
    outInfo[3] = bar.configValue;
    outInfo[4] = bar.present ? 1 : 0;
    outInfo[5] = bar.isMemory ? 1 : 0;
    outInfo[6] = bar.is64Bit ? 1 : 0;
    outInfo[7] = bar.isContinuation ? 1 : 0;
    outInfo[8] = bar.mappedResource;
    outInfo[9] = bar.length;
    outInfo[10] = bar.directMap ? bar.directMap->getPhysicalAddress() : 0;
    return true;
}

} // namespace MarvellW8897PciBars
