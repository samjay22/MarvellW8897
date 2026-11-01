//
//  MarvellW8897Controller.hpp
//  MarvellW8897
//
//  Created by Bryan on 16/11/2025.
//

#ifndef _MARVELL_W8897_CONTROLLER_HPP
#define _MARVELL_W8897_CONTROLLER_HPP

#include <Availability.h>
#ifndef __IO80211_TARGET
#define __IO80211_TARGET __MAC_15_0
#endif
#include <IOKit/80211/IO80211Controller.h>
#include <IOKit/80211/IO80211Interface.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread_call.h>
#include "MarvellW8897Bringup.hpp"
#include "MarvellW8897Debug.hpp"
#include "MarvellW8897FirmwareProtocol.hpp"
#include "MarvellW8897PciBars.hpp"
#include "MarvellW8897Transport.hpp"

namespace MarvellW8897MlanHost { struct Context; }
class IOWorkLoop;
class IOInterruptEventSource;

class MarvellW8897Controller : public IO80211Controller
{
    OSDeclareDefaultStructors(MarvellW8897Controller)

private:
    IOPCIDevice*               fPCIDevice { nullptr };
    bool                       fDeviceOpen { false };
    MarvellW8897PciBarState     fPciBarState {};
    MarvellW8897TransportState  fTransportState {};
    MarvellW8897BringupResult   fBringupResult {};
    IOLock*                    fFirmwareLock { nullptr };
    bool                       fFirmwareTransferFailed { false };
    MarvellW8897MlanHost::Context* fMlanContext { nullptr };
    IOWorkLoop*                fMlanWorkLoop { nullptr };
    IOInterruptEventSource*    fMlanInterrupt { nullptr };
    bool                       fMlanInitStarted { false };
    thread_call_t              fAutoStartCall { nullptr };
    UInt32                     fAutoStartAttempts { 0 };
    IOLock*                    fNetworkLock { nullptr };
    bool                       fStopping { false };
    IO80211Interface*           fNetworkInterface { nullptr };
    uint8_t                    fMacAddress[6] {};
    bool                       fMacValid { false };
    bool                       fNetworkEnabled { false };
    bool                       fLinkAssociated { false };
    uint64_t                   fTxLockDrops { 0 };
    uint64_t                   fRxLockDrops { 0 };

    void releaseMappedBars();

public:
    IOService* probe(IOService* provider, SInt32* score) APPLE_KEXT_OVERRIDE;
    bool start(IOService* provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService* provider) APPLE_KEXT_OVERRIDE;

    uint32_t readReg32(uint32_t offset);
    void     writeReg32(uint32_t offset, uint32_t value);
    bool     readReg32FromBar(uint32_t barIndex, uint32_t offset, uint32_t& valueOut);
    bool     writeReg32ToBar(uint32_t barIndex, uint32_t offset, uint32_t value);

    bool getPciIdentity(uint64_t outIdentity[10]) const;
    bool getBarInfoLegacy(uint32_t barIndex, uint64_t outInfo[8]) const;
    bool getBarInfoV2(uint32_t barIndex, uint64_t outInfo[11]) const;
    bool getTransportState(uint64_t outState[8]) const;
    bool getBringupResult(uint64_t outState[MarvellW8897Regs::kBringupResultCount]) const;
    bool getDebugAbiInfo(uint64_t outInfo[6]) const;
    IOReturn uploadFirmware(const void* image,
                            size_t imageSize,
                            MarvellW8897FirmwareProtocol::Result& result);
    IOReturn registerMlanCore();
    IOReturn startMlanFirmware();
    bool getMlanCoreStatus(uint64_t out[6]);
    bool getMlanTelemetry(uint64_t out[13]);
    IOReturn requestMlanScan(const uint8_t* ssid, uint32_t length);
    bool getMlanScanStatus(uint64_t out[4]);
    IOReturn requestMlanConnect(const uint8_t* ssid, uint32_t ssidLength,
                                const uint8_t* passphrase, uint32_t passphraseLength);
    IOReturn requestMlanConnectPmk(const uint8_t* ssid, uint32_t ssidLength,
                                   const uint8_t* pmk, uint32_t pmkLength);
    bool getMlanConnectStatus(uint64_t out[4]);
    static void sMlanInterrupt(OSObject* owner, IOInterruptEventSource* source, int count);
    static void sAutoStart(thread_call_param_t owner, thread_call_param_t);
    void mlanCoreReady(const uint8_t mac[6], bool associated);
    // MLAN calls this only after its scan table is fully materialized. It
    // notifies the native Wi-Fi picker without exposing NXP-owned memory.
    void mlanScanResultsReady();
    // Delivers a decapsulated 802.3 payload from the 8897 data path to the
    // IO80211 interface. The radio is represented only as Wi-Fi to macOS.
    bool receiveDataFrame(const uint8_t* bytes, uint32_t length);
    IOReturn getHardwareAddress(IOEthernetAddress* address) APPLE_KEXT_OVERRIDE;
    IOReturn enable(IONetworkInterface* interface) APPLE_KEXT_OVERRIDE;
    IOReturn disable(IONetworkInterface* interface) APPLE_KEXT_OVERRIDE;
    UInt32 outputPacket(mbuf_t packet, void* param) APPLE_KEXT_OVERRIDE;

    static void sNativeScanDone(thread_call_param_t owner, thread_call_param_t);
    SInt32 apple80211Request(unsigned int requestType, int requestNumber,
                             IO80211Interface* interface, void* data) APPLE_KEXT_OVERRIDE;
    SInt32 stopDMA() APPLE_KEXT_OVERRIDE;
    UInt32 hardwareOutputQueueDepth(IO80211Interface* interface) APPLE_KEXT_OVERRIDE;
    SInt32 performCountryCodeOperation(IO80211Interface* interface,
                                       IO80211CountryCodeOp operation) APPLE_KEXT_OVERRIDE;
    SInt32 enableFeature(IO80211FeatureCode code, void* data) APPLE_KEXT_OVERRIDE;
    bool useAppleRSNSupplicant(IO80211Interface* interface) APPLE_KEXT_OVERRIDE;

    bool writeReg32ToBarWithFlush(uint32_t barIndex, uint32_t offset, uint32_t value, uint32_t& readBack);

private:
    uint16_t fVendorId { 0 };
    uint16_t fDeviceId { 0 };
    uint16_t fSubVendorId { 0 };
    uint16_t fSubSystemId { 0 };
    uint8_t  fRevisionId { 0 };
    uint32_t fClassCode { 0 };
    uint16_t fCommand { 0 };
    uint16_t fStatus { 0 };
    uint8_t  fInterruptLine { 0 };
    uint8_t  fInterruptPin { 0 };
    thread_call_t fNativeScanDoneCall { nullptr };
    uint32_t fNativeScanIndex { 0 };
    uint8_t fNativeSsid[32] {};
    uint32_t fNativeSsidLength { 0 };
    uint32_t fNativeAuthLower { 0 };
    uint32_t fNativeAuthUpper { 0 };
    apple80211_scan_result fNativeScanResult {};
};

#endif
