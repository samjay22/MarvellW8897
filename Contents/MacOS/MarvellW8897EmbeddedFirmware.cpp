#include "MarvellW8897FirmwareImage.hpp"

namespace MarvellW8897FirmwareImage {

alignas(16) const uint8_t kEmbedded[kSize] = {
#include "../../firmware/pcie8897_uapsta.inc"
};

static_assert(sizeof(kEmbedded) == kSize, "embedded firmware size mismatch");

} // namespace MarvellW8897FirmwareImage
