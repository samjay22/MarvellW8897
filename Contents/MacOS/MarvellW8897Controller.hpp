//
//  MarvellW8897Controller.hpp
//  MarvellW8897
//
//  Created by Bryan on 16/11/2025.
//

#ifndef _MARVELL_W8897_CONTROLLER_HPP
#define _MARVELL_W8897_CONTROLLER_HPP

#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
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

class MarvellW8897Controller : public IOEthernetController
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
    IOEthernetInterface*       fNetworkInterface { nullptr };
    uint8_t                    fMacAddress[6] {};
    bool                       fMacValid { false };
    bool                       fNetworkEnabled { false };
    bool                       fLinkAssociated { false };
    uint64_t                   fTxLockDrops { 0 };
    uint64_t                   fRxLockDrops { 0 };
    uint32_t                   fRxInFlight { 0 };

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
    IOReturn requestMlanScan(const uint8_t* ssid, uint32_t length);
    bool getMlanScanStatus(uint64_t out[4]);
    IOReturn requestMlanConnect(const uint8_t* ssid, uint32_t ssidLength,
                                const uint8_t* passphrase, uint32_t passphraseLength);
    bool getMlanConnectStatus(uint64_t out[4]);
    static void sMlanInterrupt(OSObject* owner, IOInterruptEventSource* source, int count);
    static void sAutoStart(thread_call_param_t owner, thread_call_param_t);
    void mlanCoreReady(const uint8_t mac[6], bool associated);
    bool receiveEthernetFrame(const uint8_t* bytes, uint32_t length);
    IOReturn getHardwareAddress(IOEthernetAddress* address) APPLE_KEXT_OVERRIDE;
    IOReturn enable(IONetworkInterface* interface) APPLE_KEXT_OVERRIDE;
    IOReturn disable(IONetworkInterface* interface) APPLE_KEXT_OVERRIDE;
    UInt32 outputPacket(mbuf_t packet, void* param) APPLE_KEXT_OVERRIDE;

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
};

#endif
