#include "../Contents/MacOS/MarvellW8897FirmwareProtocol.hpp"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <vector>

using namespace MarvellW8897FirmwareProtocol;

class FakeDevice final : public Device {
public:
    FakeDevice() = default;

    bool read32(uint32_t offset, uint32_t& value) override
    {
        if (offset == 0x0c44) {
            ++firmwareStatusReads_;
            if (otherDownloadOwner_ && readyAfterStatusReads_ != 0 &&
                firmwareStatusReads_ >= readyAfterStatusReads_)
                firmwareReady_ = true;
            value = firmwareReady_ ? 0xfedcba00U :
                    (otherDownloadOwner_ ? 0x12345678U : 0);
            return true;
        }
        if (offset == 0x0c40) {
            if (forceHelperTimeout_) {
                value = 0;
            } else if (stopAfterBlocks_ != 0 &&
                       transmissions_.size() >= stopAfterBlocks_) {
                value = 0;
            } else if (requestRetry_ && transmissions_.size() == 1 && !retried_) {
                value = 1025; // low bit requests a resend of the last block
                retried_ = true;
            } else {
                value = 1024;
            }
            currentRequest_ = value;
            return true;
        }
        if (offset == 0x0c1c) {
            value = forceDoorbellTimeout_ ? 2 : 0;
            return true;
        }
        return false;
    }

    bool write32(uint32_t offset, uint32_t value) override
    {
        if (offset == 0x0c34 && value == 0) return true;
        if (offset == 0x0c3c && value == 0x0f) return true;
        if (offset == 0x0cf0 && value == 0xfedcba00U) {
            if (expectedImage_) {
                const size_t expected = stopAfterBlocks_ != 0 ?
                    stopAfterBlocks_ * 1024 : expectedImageSize_;
                assert(expectedOffset_ == expected);
            }
            firmwareReady_ = true;
            return true;
        }
        return false;
    }

    bool transmit(const uint8_t* data, uint32_t dmaLength) override
    {
        assert(!inFlight_);
        assert(dmaLength != 0 && dmaLength <= kDmaBufferSize);
        assert(dmaLength % kDownloadBlockSize == 0);
        if (expectedImage_) {
            if (currentRequest_ & 1U) {
                assert(!transmissions_.empty());
                const auto& previous = transmissions_.back();
                assert(previous.size() == dmaLength);
                assert(memcmp(previous.data(), data, dmaLength) == 0);
            } else {
                assert(expectedOffset_ < expectedImageSize_);
                const size_t remaining = expectedImageSize_ - expectedOffset_;
                const size_t actual = remaining < currentRequest_ ? remaining : currentRequest_;
                const size_t padded = (actual + kDownloadBlockSize - 1) &
                                      ~(static_cast<size_t>(kDownloadBlockSize) - 1);
                assert(dmaLength == padded);
                assert(memcmp(expectedImage_ + expectedOffset_, data, actual) == 0);
                for (size_t i = actual; i < dmaLength; ++i) assert(data[i] == 0);
                expectedOffset_ += actual;
            }
        }
        inFlight_ = true;
        transmissions_.emplace_back(data, data + dmaLength);
        return true;
    }

    void finish(bool) override
    {
        assert(inFlight_);
        inFlight_ = false;
        ++finished_;
    }

    void delayMicroseconds(uint32_t) override {}

    void enableOneRetry() { requestRetry_ = true; }
    void forceHelperTimeout() { forceHelperTimeout_ = true; }
    void forceDoorbellTimeout() { forceDoorbellTimeout_ = true; }
    void stopAfterBlocks(size_t blocks) { stopAfterBlocks_ = blocks; }
    void setFirmwareReady() { firmwareReady_ = true; }
    void setOtherDownloadOwner(size_t readyAfterStatusReads)
    {
        otherDownloadOwner_ = true;
        readyAfterStatusReads_ = readyAfterStatusReads;
    }
    void expectImage(const std::vector<uint8_t>& image)
    {
        expectedImage_ = image.data();
        expectedImageSize_ = image.size();
    }

    size_t finished() const { return finished_; }
    const std::vector<std::vector<uint8_t>>& transmissions() const { return transmissions_; }

private:
    bool inFlight_ { false };
    bool firmwareReady_ { false };
    bool requestRetry_ { false };
    bool retried_ { false };
    bool forceHelperTimeout_ { false };
    bool forceDoorbellTimeout_ { false };
    bool otherDownloadOwner_ { false };
    size_t firmwareStatusReads_ { 0 };
    size_t readyAfterStatusReads_ { 0 };
    size_t stopAfterBlocks_ { 0 };
    uint32_t currentRequest_ { 0 };
    const uint8_t* expectedImage_ { nullptr };
    size_t expectedImageSize_ { 0 };
    size_t expectedOffset_ { 0 };
    size_t finished_ { 0 };
    std::vector<std::vector<uint8_t>> transmissions_;
};

static void testNormalAndRetry()
{
    std::vector<uint8_t> image(2500);
    for (size_t i = 0; i < image.size(); ++i) image[i] = static_cast<uint8_t>(i);
    FakeDevice device;
    device.enableOneRetry();
    device.expectImage(image);

    const Result result = upload(device, image.data(), image.size());
    assert(result.status == Status::Complete);
    assert(result.bytesTransferred == image.size());
    assert(result.retryCount == 1);
    assert(result.blockCount == 4); // 1024, retry, 1024, 452
    assert(device.finished() == result.blockCount);
    const auto& blocks = device.transmissions();
    assert(blocks[0] == blocks[1]);
    assert(blocks[0].size() == 1024);
    assert(blocks[3].size() == 512);
    for (size_t i = 452; i < blocks[3].size(); ++i) assert(blocks[3][i] == 0);
}

static void testTimeoutsAndAlreadyReady()
{
    uint8_t image[16] = {};
    FakeDevice helperTimeout;
    helperTimeout.forceHelperTimeout();
    assert(upload(helperTimeout, image, sizeof(image)).status == Status::HelperTimeout);

    FakeDevice doorbellTimeout;
    doorbellTimeout.forceDoorbellTimeout();
    assert(upload(doorbellTimeout, image, sizeof(image)).status == Status::DoorbellTimeout);
    assert(doorbellTimeout.finished() == 1);

    FakeDevice alreadyReady;
    alreadyReady.setFirmwareReady();
    assert(upload(alreadyReady, image, sizeof(image)).status == Status::AlreadyReady);
    assert(alreadyReady.transmissions().empty());

    FakeDevice otherInterfaceFinishes;
    otherInterfaceFinishes.setOtherDownloadOwner(4);
    assert(upload(otherInterfaceFinishes, image, sizeof(image)).status == Status::AlreadyReady);
    assert(otherInterfaceFinishes.transmissions().empty());

    FakeDevice otherInterfaceStalls;
    otherInterfaceStalls.setOtherDownloadOwner(0);
    assert(upload(otherInterfaceStalls, image, sizeof(image)).status == Status::NotDownloadOwner);
    assert(otherInterfaceStalls.transmissions().empty());

    FakeDevice invalid;
    assert(upload(invalid, nullptr, sizeof(image)).status == Status::InvalidImage);
}

static void testHelperEndsBeforeImageEof()
{
    std::vector<uint8_t> image(4096);
    for (size_t i = 0; i < image.size(); ++i) image[i] = static_cast<uint8_t>(i);
    FakeDevice device;
    device.expectImage(image);
    device.stopAfterBlocks(2);
    const Result result = upload(device, image.data(), image.size());
    assert(result.status == Status::Complete);
    assert(result.bytesTransferred == 2048);
    assert(result.blockCount == 2);
}

static void testRealFirmware(const char* path)
{
    FILE* file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    const long size = ftell(file);
    assert(size == 723540);
    rewind(file);
    std::vector<uint8_t> image(static_cast<size_t>(size));
    assert(fread(image.data(), 1, image.size(), file) == image.size());
    fclose(file);

    FakeDevice device;
    device.expectImage(image);
    const Result result = upload(device, image.data(), image.size());
    assert(result.status == Status::Complete);
    assert(result.bytesTransferred == image.size());
    assert(device.finished() == result.blockCount);
}

int main(int argc, char** argv)
{
    testNormalAndRetry();
    testTimeoutsAndAlreadyReady();
    testHelperEndsBeforeImageEof();
    if (argc > 1) testRealFirmware(argv[1]);
    puts("firmware protocol tests passed");
    return 0;
}
