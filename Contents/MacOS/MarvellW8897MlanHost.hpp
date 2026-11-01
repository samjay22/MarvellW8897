// SPDX-License-Identifier: GPL-2.0
// macOS host adapter for NXP's GPL-2.0 MLAN core.
#ifndef MARVELL_W8897_MLAN_HOST_HPP
#define MARVELL_W8897_MLAN_HOST_HPP
#include <sys/kpi_mbuf.h>

class MarvellW8897Controller;

namespace MarvellW8897MlanHost {

// Registration allocates MLAN's software state. Firmware and data-path startup
// remain separate so a bad PCIe transfer cannot run during a normal boot.
struct Context;

// A bounded copy of the fields that the macOS 802.11 layer needs from an
// NXP-owned BSSDescriptor_t.  This deliberately contains no NXP pointers or
// variable-length IE storage, so callers cannot retain firmware-owned memory.
struct ScanEntry {
    unsigned char bssid[6];
    unsigned char ssid[32];
    unsigned ssidLength;
    unsigned channel;
    int rssiDbm;
    unsigned beaconInterval;
    unsigned capabilities;
    bool privacy;
};

Context* registerCore(MarvellW8897Controller* controller);
void unregisterCore(Context* context);
// Called only after the caller has installed an IOKit PCI interrupt source.
// Returns NXP's immediate MLAN_STATUS_* value (0 success, 1 pending).
unsigned startFirmware(Context* context, bool msiInterrupt);
bool processInterrupt(Context* context);
bool getStatus(Context* context, unsigned long long out[6]);
// Read-only accounting for correlation of PCIe interrupts, deferred MLAN
// work, and frames crossing the macOS network boundary. This is deliberately
// separate from getStatus so the existing six-word ABI remains stable.
bool getTelemetry(Context* context, unsigned long long out[13]);
// The caller retains the mbuf. MLAN owns its copy after a pending send.
bool sendDataFrame(Context* context, mbuf_t packet, unsigned length);
// A scan is asynchronous; getScanStatus reports phase, MLAN completion, and
// the number of BSS entries obtained from a separate persistent GET request.
unsigned requestScan(Context* context, const unsigned char* ssid, unsigned length);
bool getScanStatus(Context* context, unsigned long long out[4]);
bool getScanEntry(Context* context, unsigned index, ScanEntry& entry);
// Manual open or WPA2-PSK/CCMP association, using the 8897 embedded supplicant.
// Passphrase length is 0 for open or 8-63 bytes for WPA2-PSK.
unsigned requestConnect(Context* context, const unsigned char* ssid, unsigned ssidLength,
                        const unsigned char* passphrase, unsigned passphraseLength);
// Join using a pre-derived 32-byte WPA/WPA2 PMK. macOS's native 802.11
// association interface supplies this form rather than a user passphrase.
unsigned requestConnectPmk(Context* context, const unsigned char* ssid,
                           unsigned ssidLength, const unsigned char* pmk,
                           unsigned pmkLength);
bool getConnectStatus(Context* context, unsigned long long out[4]);

} // namespace MarvellW8897MlanHost
#endif
