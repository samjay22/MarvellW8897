#include "../Contents/MacOS/MarvellW8897FirmwareProtocol.hpp"
#include "../Contents/MacOS/MarvellW8897FirmwareImage.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
#include <string>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr unsigned kUploadSelector = 9;
constexpr unsigned kRegisterCoreSelector = 10;
constexpr unsigned kStartCoreSelector = 11;
constexpr unsigned kCoreStatusSelector = 12;
constexpr unsigned kScanSelector = 13;
constexpr unsigned kScanStatusSelector = 14;
constexpr unsigned kConnectSelector = 15;
constexpr unsigned kConnectStatusSelector = 16;
struct ScanInput {
    uint32_t ssidLength;
    uint8_t ssid[32];
};
static_assert(sizeof(ScanInput) == 36, "scan ABI must stay fixed");
struct ConnectInput {
    uint32_t ssidLength;
    uint32_t passphraseLength;
    uint8_t ssid[32];
    uint8_t passphrase[64];
};
static_assert(sizeof(ConnectInput) == 104, "connect ABI must stay fixed");

void secureZero(void* bytes, size_t length)
{
    auto* cursor = static_cast<volatile uint8_t*>(bytes);
    while (length--) *cursor++ = 0;
}

bool readPassphrase(std::string& result)
{
    const bool tty = isatty(STDIN_FILENO) != 0;
    termios previous {};
    if (tty) {
        if (tcgetattr(STDIN_FILENO, &previous) != 0) return false;
        termios hidden = previous;
        hidden.c_lflag &= ~ECHO;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) return false;
    }
    if (tty) std::cerr << "WPA2 passphrase: " << std::flush;
    const bool read = static_cast<bool>(std::getline(std::cin, result));
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &previous);
        std::cerr << "\n";
    }
    return read && result.size() >= 8 && result.size() <= 63;
}
static_assert(sizeof(MarvellW8897FirmwareImage::kSha256) == CC_SHA256_DIGEST_LENGTH,
              "firmware hash size must match SHA-256");

bool readFirmware(const char* path, std::vector<uint8_t>& image)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "cannot open firmware image\n";
        return false;
    }
    if (file.tellg() != std::streampos(MarvellW8897FirmwareImage::kSize)) {
        std::cerr << "firmware image has an unexpected size\n";
        return false;
    }
    file.seekg(0);
    image.resize(MarvellW8897FirmwareImage::kSize);
    file.read(reinterpret_cast<char*>(image.data()),
              static_cast<std::streamsize>(image.size()));
    if (!file || file.gcount() != static_cast<std::streamsize>(image.size())) {
        std::cerr << "firmware image could not be read completely\n";
        return false;
    }

    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(image.data(), static_cast<CC_LONG>(image.size()), digest);
    if (std::memcmp(digest, MarvellW8897FirmwareImage::kSha256, sizeof(digest)) != 0) {
        std::cerr << "firmware image SHA-256 does not match this Surface\n";
        return false;
    }
    return true;
}

bool findInterface(io_registry_entry_t entry, std::string& name, unsigned depth)
{
    if (depth == 0) return false;
    io_iterator_t children = IO_OBJECT_NULL;
    if (IORegistryEntryGetChildIterator(entry, kIOServicePlane, &children) != KERN_SUCCESS)
        return false;
    bool found = false;
    io_registry_entry_t child = IO_OBJECT_NULL;
    while (!found && (child = IOIteratorNext(children)) != IO_OBJECT_NULL) {
        if (IOObjectConformsTo(child, "IONetworkInterface")) {
            CFTypeRef property = IORegistryEntryCreateCFProperty(
                child, CFSTR("IOBSDName"), kCFAllocatorDefault, 0);
            if (property) {
                if (CFGetTypeID(property) == CFStringGetTypeID()) {
                    char device[32] = {};
                    if (CFStringGetCString(static_cast<CFStringRef>(property), device,
                                           sizeof(device), kCFStringEncodingUTF8)) {
                        name = device;
                        found = true;
                    }
                }
                CFRelease(property);
            }
        }
        if (!found) found = findInterface(child, name, depth - 1);
        IOObjectRelease(child);
    }
    IOObjectRelease(children);
    return found;
}

} // namespace

int main(int argc, char** argv)
{
    const bool statusOnly = argc == 2 && std::strcmp(argv[1], "--status") == 0;
    const bool scanStatusOnly = argc == 2 && std::strcmp(argv[1], "--scan-status") == 0;
    const bool scanOnly = (argc == 2 || argc == 3) &&
                          std::strcmp(argv[1], "--scan") == 0;
    const bool connectStatusOnly = argc == 2 && std::strcmp(argv[1], "--connect-status") == 0;
    const bool interfaceOnly = argc == 2 && std::strcmp(argv[1], "--interface") == 0;
    const bool connectWpa2 = argc == 3 && std::strcmp(argv[1], "--connect") == 0;
    const bool connectOpen = argc == 3 && std::strcmp(argv[1], "--connect-open") == 0;
    const bool connectOnly = connectWpa2 || connectOpen;
    const bool startCore = argc == 3 && std::strcmp(argv[1], "--start-core") == 0;
    const bool registerCore = startCore ||
        (argc == 3 && std::strcmp(argv[1], "--register-core") == 0);
    const bool uploadOnly = argc == 2 && argv[1][0] != '-';
    if (!statusOnly && !scanStatusOnly && !scanOnly && !connectStatusOnly &&
        !interfaceOnly &&
        !connectOnly && !uploadOnly && !registerCore) {
        std::cerr << "usage: sudo marvell_fwupload [--register-core|--start-core] /path/to/pcie8897_uapsta.bin\n"
                  << "       marvell_fwupload --status\n"
                  << "       sudo marvell_fwupload --scan [SSID]\n"
                  << "       marvell_fwupload --scan-status\n"
                  << "       sudo marvell_fwupload --connect SSID\n"
                  << "       sudo marvell_fwupload --connect-open SSID\n"
                  << "       marvell_fwupload --connect-status\n"
                  << "       marvell_fwupload --interface\n";
        return 2;
    }

    std::vector<uint8_t> image;
    if (!statusOnly && !scanStatusOnly && !scanOnly &&
        !connectStatusOnly && !interfaceOnly && !connectOnly &&
        !readFirmware(argv[registerCore ? 2 : 1], image)) {
        std::cerr << "firmware file is missing or does not match the verified Surface image\n";
        return 2;
    }

    io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("MarvellW8897Controller"));
    if (service == IO_OBJECT_NULL) {
        std::cerr << "MarvellW8897Controller is not loaded\n";
        return 1;
    }

    if (interfaceOnly) {
        std::string name;
        const bool found = findInterface(service, name, 4);
        IOObjectRelease(service);
        if (!found) {
            std::cerr << "MarvellW8897 network interface is not registered\n";
            return 1;
        }
        std::cout << name << "\n";
        return 0;
    }

    io_connect_t connection = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) {
        std::cerr << "cannot open MarvellW8897Controller: " << kr << "\n";
        return 1;
    }

    if (statusOnly) {
        uint64_t coreStatus[6] = {};
        uint32_t count = 6;
        kr = IOConnectCallMethod(connection, kCoreStatusSelector,
                                 nullptr, 0, nullptr, 0,
                                 coreStatus, &count, nullptr, nullptr);
        IOServiceClose(connection);
        if (kr != KERN_SUCCESS || count != 6) {
            std::cerr << "cannot read MLAN core status: " << kr << "\n";
            return 1;
        }
        std::cout << "registered=" << coreStatus[0]
                  << " initComplete=" << coreStatus[1]
                  << " initStatus=" << coreStatus[2]
                  << " hwSpecStatus=" << coreStatus[3]
                  << " lastEvent=0x" << std::hex << coreStatus[4] << std::dec
                  << " eventCount=" << coreStatus[5] << "\n";
        return 0;
    }

    if (scanStatusOnly) {
        uint64_t scanStatus[4] = {};
        uint32_t count = 4;
        kr = IOConnectCallMethod(connection, kScanStatusSelector,
                                 nullptr, 0, nullptr, 0,
                                 scanStatus, &count, nullptr, nullptr);
        IOServiceClose(connection);
        if (kr != KERN_SUCCESS || count != 4) {
            std::cerr << "cannot read scan status: " << kr << "\n";
            return 1;
        }
        std::cout << "scanPhase=" << scanStatus[0]
                  << " mlanStatus=" << scanStatus[1]
                  << " resultPhase=" << scanStatus[2]
                  << " bssCount=" << scanStatus[3] << "\n";
        return 0;
    }

    if (connectStatusOnly) {
        uint64_t connectStatus[4] = {};
        uint32_t count = 4;
        kr = IOConnectCallMethod(connection, kConnectStatusSelector,
                                 nullptr, 0, nullptr, 0,
                                 connectStatus, &count, nullptr, nullptr);
        IOServiceClose(connection);
        if (kr != KERN_SUCCESS || count != 4) {
            std::cerr << "cannot read connection status: " << kr << "\n";
            return 1;
        }
        std::cout << "connectPhase=" << connectStatus[0]
                  << " mlanStatus=" << connectStatus[1]
                  << " commandPending=" << connectStatus[2]
                  << " associated=" << connectStatus[3] << "\n";
        return 0;
    }

    if (connectOnly) {
        ConnectInput input {};
        const size_t ssidLength = std::strlen(argv[2]);
        if (ssidLength == 0 || ssidLength > sizeof(input.ssid)) {
            IOServiceClose(connection);
            std::cerr << "SSID must be 1 to 32 bytes\n";
            return 2;
        }
        input.ssidLength = static_cast<uint32_t>(ssidLength);
        std::memcpy(input.ssid, argv[2], ssidLength);
        std::string passphrase;
        if (connectWpa2) {
            if (!readPassphrase(passphrase)) {
                IOServiceClose(connection);
                std::cerr << "passphrase must be 8 to 63 bytes\n";
                secureZero(passphrase.data(), passphrase.size());
                return 2;
            }
            input.passphraseLength = static_cast<uint32_t>(passphrase.size());
            std::memcpy(input.passphrase, passphrase.data(), passphrase.size());
            secureZero(passphrase.data(), passphrase.size());
        }
        kr = IOConnectCallMethod(connection, kConnectSelector,
                                 nullptr, 0, &input, sizeof(input),
                                 nullptr, nullptr, nullptr, nullptr);
        secureZero(&input, sizeof(input));
        IOServiceClose(connection);
        if (kr != KERN_SUCCESS) {
            std::cerr << "MLAN association request failed: " << kr << "\n";
            return 1;
        }
        std::cout << "association requested; use --connect-status to track it\n";
        return 0;
    }

    if (scanOnly) {
        ScanInput scan {};
        if (argc == 3) {
            const size_t length = std::strlen(argv[2]);
            if (length == 0 || length > sizeof(scan.ssid)) {
                IOServiceClose(connection);
                std::cerr << "SSID must be 1 to 32 bytes\n";
                return 2;
            }
            scan.ssidLength = static_cast<uint32_t>(length);
            std::memcpy(scan.ssid, argv[2], length);
        }
        kr = IOConnectCallMethod(connection, kScanSelector,
                                 nullptr, 0, &scan, sizeof(scan),
                                 nullptr, nullptr, nullptr, nullptr);
        IOServiceClose(connection);
        if (kr != KERN_SUCCESS) {
            std::cerr << "MLAN scan request failed: " << kr << "\n";
            return 1;
        }
        std::cout << "scan requested; use --scan-status for completion and BSS count\n";
        return 0;
    }

    uint64_t output[6] = {};
    uint32_t outputCount = 6;
    kr = IOConnectCallMethod(connection, kUploadSelector,
                             nullptr, 0,
                             image.data(), image.size(),
                             output, &outputCount,
                             nullptr, nullptr);
    if (kr != KERN_SUCCESS || outputCount != 6) {
        IOServiceClose(connection);
        std::cerr << "firmware upload method failed: " << kr << "\n";
        return 1;
    }

    const auto status = static_cast<MarvellW8897FirmwareProtocol::Status>(output[0]);
    std::cout << "status=" << MarvellW8897FirmwareProtocol::statusName(status)
              << " bytes=" << output[1]
              << " blocks=" << output[2]
              << " retries=" << output[3]
              << " lastRequest=" << output[4]
              << " firmwareStatus=0x" << std::hex << output[5] << std::dec << "\n";

    if (status != MarvellW8897FirmwareProtocol::Status::Complete &&
        status != MarvellW8897FirmwareProtocol::Status::AlreadyReady) {
        IOServiceClose(connection);
        return 1;
    }
    if (registerCore) {
        kr = IOConnectCallMethod(connection, kRegisterCoreSelector,
                                 nullptr, 0, nullptr, 0,
                                 nullptr, nullptr, nullptr, nullptr);
        if (kr != KERN_SUCCESS) {
            IOServiceClose(connection);
            std::cerr << "MLAN core registration failed: " << kr << "\n";
            return 1;
        }
        std::cout << "NXP MLAN core registered; PCIe ring startup still requires --start-core\n";
    }
    if (startCore) {
        kr = IOConnectCallMethod(connection, kStartCoreSelector,
                                 nullptr, 0, nullptr, 0,
                                 nullptr, nullptr, nullptr, nullptr);
        if (kr != KERN_SUCCESS) {
            IOServiceClose(connection);
            std::cerr << "MLAN firmware initialization failed to start: " << kr << "\n";
            return 1;
        }
        std::cout << "MLAN firmware initialization started; use --status to inspect completion\n";
    }
    IOServiceClose(connection);
    return 0;
}
