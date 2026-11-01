#ifndef _MARVELL_W8897_FIRMWARE_IMAGE_HPP
#define _MARVELL_W8897_FIRMWARE_IMAGE_HPP

#include <stddef.h>
#include <stdint.h>

namespace MarvellW8897FirmwareImage {

// SHA-256 of the 88W8897 image verified against this Surface's Ubuntu copy.
static constexpr size_t kSize = 723540;
static constexpr uint8_t kSha256[32] = {
    0xcf, 0x4f, 0x51, 0xf4, 0x1b, 0xd7, 0xef, 0x4d,
    0x7f, 0xe6, 0x5f, 0xb7, 0x6b, 0x8a, 0x2a, 0x08,
    0x97, 0xbc, 0x70, 0xa0, 0x74, 0x2b, 0xc4, 0xae,
    0xa1, 0x3d, 0x93, 0xb0, 0x3f, 0xff, 0xd0, 0x3a,
};

// This test kext embeds the image so Recovery startup does not depend on
// mounting the EFI volume from user space.
extern const uint8_t kEmbedded[kSize];


} // namespace MarvellW8897FirmwareImage

#endif
