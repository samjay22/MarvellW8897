#include "MarvellW8897Debug.hpp"

namespace MarvellW8897Debug {

bool getDebugAbiInfo(UInt64 outInfo[MarvellW8897Regs::kDebugAbiInfoCount])
{
    if (!outInfo) {
        return false;
    }

    outInfo[0] = 1;
    outInfo[1] = 5;
    outInfo[2] = MarvellW8897Regs::kDebugSelectorBitmap;
    outInfo[3] = MarvellW8897Regs::kBarInfoLegacyCount;
    outInfo[4] = MarvellW8897Regs::kBarInfoV2Count;
    outInfo[5] = MarvellW8897Regs::kTransportStateCount;
    return true;
}

} // namespace MarvellW8897Debug
