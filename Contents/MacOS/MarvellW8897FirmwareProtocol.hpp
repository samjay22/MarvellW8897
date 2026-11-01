#ifndef _MARVELL_W8897_FIRMWARE_PROTOCOL_HPP
#define _MARVELL_W8897_FIRMWARE_PROTOCOL_HPP

#include <stddef.h>
#include <stdint.h>

namespace MarvellW8897FirmwareProtocol {

// The 88W8897 boot helper requests firmware blocks through scratch register 2.
// A request with bit 0 set asks for the previous block again after a CRC error.
static constexpr uint32_t kMaxHelperRequest = 2312;
static constexpr uint32_t kDownloadBlockSize = 256;
static constexpr uint32_t kDmaBufferSize = 2560;
static constexpr size_t kMaxFirmwareSize = 4U * 1024U * 1024U;

enum class Status : uint32_t {
    Complete,
    AlreadyReady,
    InvalidImage,
    NotDownloadOwner,
    RegisterAccessFailed,
    HelperTimeout,
    InvalidHelperRequest,
    RetryWithoutBlock,
    TooManyRetries,
    DmaTransferFailed,
    DoorbellTimeout,
    FirmwareReadyTimeout,
};

struct Result {
    Status status { Status::InvalidImage };
    size_t bytesTransferred { 0 };
    uint32_t blockCount { 0 };
    uint32_t retryCount { 0 };
    uint32_t lastRequest { 0 };
    uint32_t lastFirmwareStatus { 0 };
};

// The device implementation owns DMA mapping from transmit() until finish().
// If acknowledged is false, the device must stop bus mastering before it
// releases the DMA mapping because the card may still be reading the buffer.
class Device {
public:
    virtual ~Device() = default;
    virtual bool read32(uint32_t offset, uint32_t& value) = 0;
    virtual bool write32(uint32_t offset, uint32_t value) = 0;
    virtual bool transmit(const uint8_t* data, uint32_t dmaLength) = 0;
    virtual void finish(bool acknowledged) = 0;
    virtual void delayMicroseconds(uint32_t usec) = 0;
};

Result upload(Device& device, const uint8_t* image, size_t imageSize);
const char* statusName(Status status);

} // namespace MarvellW8897FirmwareProtocol

#endif
