// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Android Auto Bluetooth SPP link over the car's Alps "Evolution" Bluetooth stack, through D-Bus.

// D-Bus interface of the Alps stack, as the BMW Bluetooth services use it:
//   service org.alps.evolution, object /org/alps/evolution/hli
//   org.alps.evo.SPP: BtApplSppSetUuidReq(y instance, q length, ay data)
//                     BtApplSppDataReq(y instance, q length, ay data)
//                     signals BtApplSppSetUuidCfm(y instance, y status), BtApplSppDataInd(y instance, q length, ay
//                     data), BtApplSppDataCfm(y instance, y status), BtApplSppSendInd(y instance),
//                     BtApplSppDataSendFcInd(y, y)
//   org.alps.evo.DM:  signals BtApplDeviceConnectInd(y status, ay address, t function, y instance)
//                     BtApplDeviceDisconnectInd(y status, ay address, y reason, t function, y instance)
//   SetUuid data (as btmbt's CDBUSBluetoothSender::sendSPPBtApplSppSetUuidReq builds it):
//     [type][initiator UUID][type][UUID of the local SDP record], type 1/2/3 = 16/32/128-bit UUID
//   No virtual serial port: the kernel has no evo_bvd (bvd_tty), so the data goes through D-Bus.
//
// The link is handed to the core (wireless_setup) as a file descriptor: one end of a socketpair whose other end is
// served by the D-Bus thread (DataInd -> write, read -> DataReq).
#ifndef ENTRY_AAD_PLATFORM_ALPS_SPP_HPP
#define ENTRY_AAD_PLATFORM_ALPS_SPP_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace evo {

// Android Auto UUID the phone looks for in the car's SDP records.
static const char* const kAaUuid = "4de17a00-52cb-11e6-bdf4-0800200c9a66";

// "4de17a00-52cb-..." -> 16 big-endian bytes. false if the string is not a 128-bit UUID.
bool parseUuid128(const char* s, uint8_t out[16]);
// BtApplSppSetUuidReq payload: [3][initiator uuid][3][record uuid] (34 bytes for two 128-bit UUIDs).
std::vector<uint8_t> buildSetUuidPayload(const uint8_t initUuid[16], const uint8_t recUuid[16]);
// Whether BtApplReadConfigurationReq answers with this slot's own value. The Alps stack keeps one copy of each SPP
// setting per slot (id + 0x50 * slot, libevo_btappl.so configuration table) and gen_SetConfig writes that copy, but
// gen_GetConfig adds the offset for slot 1 only: slots 2 to 5 are answered with slot 0's value (seen on the car: the
// interface of slot 5 written to 1 read back as 0). There a write is confirmed by its own status, never read back.
inline bool slotConfigurationReadable(unsigned slot) {
    return slot <= 1;
}

class AlpsSppLink {
public:
    AlpsSppLink(const AlpsSppLink&) = delete;
    AlpsSppLink& operator=(const AlpsSppLink&) = delete;
    AlpsSppLink();
    ~AlpsSppLink();

    // Connects to the system bus, subscribes to the SPP/DM signals, starts the dispatch thread.
    bool start(uint8_t instance);
    void stop();

    // Publishes the UUID on the instance (BtApplSppSetUuidReq) and waits for BtApplSppSetUuidCfm.
    // Returns its status (0 expected), or -1 when no confirmation arrives in time.
    int registerUuid(const char* uuid, int timeoutMs);
    // Exact, validated UUID pair for restoration (16/32/128-bit UUIDs).
    int registerUuidPayload(const std::vector<uint8_t>& payload, int timeoutMs);
    // Writes 0x6203, then reads it back where the stack can (slotConfigurationReadable).
    bool setDataInterface(uint8_t value, int timeoutMs);
    // Only this SPP instance (function=1); never disconnect all BMW profiles.
    bool requestSppDisconnect();

    // Waits for the phone to open the link (DeviceConnectInd or first DataInd on our instance).
    // Returns a fd (read/write of a transparent byte stream), or -1 after timeoutMs.
    // The fd is borrowed: do not close() it; closeLink() closes it once I/O has ended.
    // A disconnection reads as EOF; the fd is never recycled under the reader.
    int waitLink(int timeoutMs);
    void closeLink();
    void interruptLink();  // wakes the reader (EOF) without closing its fd; then join, then closeLink/stop
    bool linkUp();

    // Monitor mode: logs SPP/DM metadata; refuses UUID registration, DataReq and configuration writes.
    // Explicit readConfiguration/introspect calls stay allowed (read-only).
    void setMonitorOnly(bool on);  // before start() only
    // Read-only call: BtApplReadConfigurationReq(instance, id) -> BtApplReadConfigurationCfm.
    bool readConfiguration(uint8_t instance, uint16_t id, std::vector<uint8_t>& out, int timeoutMs);
    // Standard D-Bus introspection (read-only).
    std::string introspect(const char* path);

    // Checks that configuration 0x6203 == 1. Writes it only when allowWrite is set, waits for the confirmation,
    // then reads the configuration back. Refused in monitor mode. On a slot the stack cannot read back
    // (slotConfigurationReadable), it is written to 1 and the write's confirmation is the proof.
    bool ensureDataInterface(bool allowWrite, int timeoutMs);

    bool running();

private:
    struct Impl;
    Impl* p_;
};

}  // namespace evo

#endif
