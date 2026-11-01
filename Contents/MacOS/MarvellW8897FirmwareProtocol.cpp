#include "MarvellW8897FirmwareProtocol.hpp"
#include <string.h>

namespace MarvellW8897FirmwareProtocol {
namespace {

constexpr uint32_t kScratch2 = 0x0c40;
constexpr uint32_t kScratch3 = 0x0c44;
constexpr uint32_t kScratch12 = 0x0cf0;
constexpr uint32_t kCpuInterruptStatus = 0x0c1c;
constexpr uint32_t kHostInterruptMask = 0x0c34;
constexpr uint32_t kHostInterruptStatusMask = 0x0c3c;
constexpr uint32_t kDoorbellBit = 1U << 1;
constexpr uint32_t kHostInterruptBits = 0x0f;
constexpr uint32_t kFirmwareReady = 0xfedcba00;
constexpr uint32_t kMaxPolls = 100;
constexpr uint32_t kOtherInterfaceReadyPolls = 150;
constexpr uint32_t kOwnFirmwareReadyPolls = 300;
constexpr uint32_t kMaxCrcRetries = 2;

bool readValid(Device& device, uint32_t offset, uint32_t& value)
{
    return device.read32(offset, value) && value != 0xffffffffU;
}

bool waitForHelperRequest(Device& device, uint32_t& request)
{
    for (uint32_t i = 0; i < kMaxPolls; ++i) {
        if (!readValid(device, kScratch2, request)) {
            return false;
        }
        if (request != 0) {
            return true;
        }
        device.delayMicroseconds(20);
    }
    return true; // request remains zero: caller reports HelperTimeout.
}

bool waitForDoorbellClear(Device& device, bool& readFailed)
{
    readFailed = false;
    for (uint32_t i = 0; i < kMaxPolls; ++i) {
        uint32_t status = 0;
        if (!readValid(device, kCpuInterruptStatus, status)) {
            readFailed = true;
            return false;
        }
        if ((status & kDoorbellBit) == 0) {
            return true;
        }
        device.delayMicroseconds(20);
    }
    return false;
}

Status waitForFirmwareReady(Device& device, Result& result,
                            uint32_t polls, Status readyStatus,
                            Status timeoutStatus)
{
    for (uint32_t i = 0; i < polls; ++i) {
        uint32_t firmwareStatus = 0;
        if (!readValid(device, kScratch3, firmwareStatus))
            return Status::RegisterAccessFailed;
        result.lastFirmwareStatus = firmwareStatus;
        if (firmwareStatus == kFirmwareReady) return readyStatus;
        device.delayMicroseconds(100000);
    }
    return timeoutStatus;
}

} // namespace

Result upload(Device& device, const uint8_t* image, size_t imageSize)
{
    Result result;
    if (!image || imageSize == 0 || imageSize > kMaxFirmwareSize) {
        return result;
    }

    uint32_t firmwareStatus = 0;
    if (!readValid(device, kScratch3, firmwareStatus)) {
        result.status = Status::RegisterAccessFailed;
        return result;
    }
    result.lastFirmwareStatus = firmwareStatus;
    if (firmwareStatus == kFirmwareReady) {
        result.status = Status::AlreadyReady;
        return result;
    }
    if (firmwareStatus != 0) {
        // Like mwifiex, wait for the other interface of this combo chip to
        // finish loading the shared firmware before declaring failure.
        result.status = waitForFirmwareReady(
            device, result, kOtherInterfaceReadyPolls,
            Status::AlreadyReady, Status::NotDownloadOwner);
        return result;
    }

    if (!device.write32(kHostInterruptMask, 0)) {
        result.status = Status::RegisterAccessFailed;
        return result;
    }

    // One reusable transfer buffer is retained for an explicit CRC retry.
    uint8_t block[kDmaBufferSize];
    uint32_t previousDmaLength = 0;
    uint32_t crcRetriesForBlock = 0;

    while (result.bytesTransferred < imageSize) {
        uint32_t request = 0;
        if (!waitForHelperRequest(device, request)) {
            result.status = Status::RegisterAccessFailed;
            return result;
        }
        if (request == 0) {
            if (result.bytesTransferred == 0) {
                result.status = Status::HelperTimeout;
                return result;
            }
            // The helper may stop after the Wi-Fi portion of a combo image.
            // Linux then checks firmware-ready rather than requiring EOF.
            break;
        }
        result.lastRequest = request;

        uint32_t dmaLength = 0;
        size_t dataLength = 0;
        const bool retry = (request & 1U) != 0;
        if (retry) {
            if (previousDmaLength == 0) {
                result.status = Status::RetryWithoutBlock;
                return result;
            }
            if (++crcRetriesForBlock > kMaxCrcRetries) {
                result.status = Status::TooManyRetries;
                return result;
            }
            dmaLength = previousDmaLength;
            ++result.retryCount;
        } else {
            if (request > kMaxHelperRequest) {
                result.status = Status::InvalidHelperRequest;
                return result;
            }
            const size_t remaining = imageSize - result.bytesTransferred;
            dataLength = remaining < request ? remaining : request;
            dmaLength = static_cast<uint32_t>(
                (dataLength + kDownloadBlockSize - 1) & ~(kDownloadBlockSize - 1));
            if (dmaLength == 0 || dmaLength > kDmaBufferSize) {
                result.status = Status::InvalidHelperRequest;
                return result;
            }
            memset(block, 0, dmaLength);
            memcpy(block, image + result.bytesTransferred, dataLength);
            previousDmaLength = dmaLength;
            crcRetriesForBlock = 0;
        }

        if (!device.transmit(block, dmaLength)) {
            result.status = Status::DmaTransferFailed;
            return result;
        }
        bool readFailed = false;
        const bool acknowledged = waitForDoorbellClear(device, readFailed);
        device.finish(acknowledged);
        if (!acknowledged) {
            result.status = readFailed ? Status::RegisterAccessFailed
                                       : Status::DoorbellTimeout;
            return result;
        }

        if (!retry) {
            result.bytesTransferred += dataLength;
        }
        ++result.blockCount;
    }

    if (!device.write32(kHostInterruptStatusMask, kHostInterruptBits) ||
        !device.write32(kScratch12, kFirmwareReady)) {
        result.status = Status::RegisterAccessFailed;
        return result;
    }

    result.status = waitForFirmwareReady(
        device, result, kOwnFirmwareReadyPolls,
        Status::Complete, Status::FirmwareReadyTimeout);
    return result;
}

const char* statusName(Status status)
{
    switch (status) {
        case Status::Complete: return "complete";
        case Status::AlreadyReady: return "already-ready";
        case Status::InvalidImage: return "invalid-image";
        case Status::NotDownloadOwner: return "not-download-owner";
        case Status::RegisterAccessFailed: return "register-access-failed";
        case Status::HelperTimeout: return "helper-timeout";
        case Status::InvalidHelperRequest: return "invalid-helper-request";
        case Status::RetryWithoutBlock: return "retry-without-block";
        case Status::TooManyRetries: return "too-many-retries";
        case Status::DmaTransferFailed: return "dma-transfer-failed";
        case Status::DoorbellTimeout: return "doorbell-timeout";
        case Status::FirmwareReadyTimeout: return "firmware-ready-timeout";
    }
    return "unknown";
}

} // namespace MarvellW8897FirmwareProtocol
