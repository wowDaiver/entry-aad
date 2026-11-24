// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Integration test of the SPP link against a fake Alps service on a PRIVATE D-Bus, never the HU bus.
#include <dbus/dbus.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "alps_spp.hpp"
#include "evo_process.hpp"
#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            exit(1);                                             \
        }                                                        \
    } while (0)
namespace {
const char* const path = "/org/alps/evolution/hli";
const char* const spp = "org.alps.evo.SPP";
const char* const dm = "org.alps.evo.DM";
void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
class FakeAlps {
public:
    DBusConnection* conn;
    std::atomic<bool> active, suppressUuid, suppressConfig;
    std::atomic<int> uuidRequests, dataRequests, writes, reads;
    uint8_t config;
    std::mutex mu;
    std::vector<uint8_t> sent, lastUuid;
    std::thread worker;
    FakeAlps()
        : conn(0),
          active(true),
          suppressUuid(false),
          suppressConfig(false),
          uuidRequests(0),
          dataRequests(0),
          writes(0),
          reads(0),
          config(0) {
        DBusError e;
        dbus_error_init(&e);
        conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &e);
        CHECK(conn);
        CHECK(dbus_bus_request_name(conn, "org.alps.evolution", DBUS_NAME_FLAG_DO_NOT_QUEUE, &e) ==
              DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
        worker = std::thread(&FakeAlps::loop, this);
    }
    ~FakeAlps() {
        active = false;
        worker.join();
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
    }
    void send(DBusMessage* m) {
        CHECK(m);
        CHECK(dbus_connection_send(conn, m, 0));
        dbus_message_unref(m);
    }
    void status(const char* member, uint8_t instance, uint8_t value) {
        DBusMessage* m = dbus_message_new_signal(path, spp, member);
        CHECK(m);
        CHECK(dbus_message_append_args(m, DBUS_TYPE_BYTE, &instance, DBUS_TYPE_BYTE, &value, DBUS_TYPE_INVALID));
        send(m);
    }
    void ready(uint8_t instance) {
        DBusMessage* m = dbus_message_new_signal(path, spp, "BtApplSppSendInd");
        CHECK(m);
        CHECK(dbus_message_append_args(m, DBUS_TYPE_BYTE, &instance, DBUS_TYPE_INVALID));
        send(m);
    }
    void device(bool connecting, uint8_t instance, dbus_uint64_t function = 1, uint8_t statusCode = 0) {
        DBusMessage* m =
            dbus_message_new_signal(path, dm, connecting ? "BtApplDeviceConnectInd" : "BtApplDeviceDisconnectInd");
        CHECK(m);
        uint8_t address[] = {1, 2, 3, 4, 5, 6}, reason = 0;
        const uint8_t* a = address;
        CHECK(dbus_message_append_args(m, DBUS_TYPE_BYTE, &statusCode, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &a, 6,
                                       DBUS_TYPE_INVALID));
        if (!connecting) {
            CHECK(dbus_message_append_args(m, DBUS_TYPE_BYTE, &reason, DBUS_TYPE_INVALID));
        }
        CHECK(dbus_message_append_args(m, DBUS_TYPE_UINT64, &function, DBUS_TYPE_BYTE, &instance, DBUS_TYPE_INVALID));
        send(m);
    }
    void data(uint8_t instance, const std::vector<uint8_t>& bytes, bool badLength = false) {
        DBusMessage* m = dbus_message_new_signal(path, spp, "BtApplSppDataInd");
        CHECK(m);
        dbus_uint16_t n = static_cast<dbus_uint16_t>(bytes.size() + (badLength ? 1 : 0));
        const uint8_t* p = bytes.empty() ? 0 : &bytes[0];
        CHECK(dbus_message_append_args(m, DBUS_TYPE_BYTE, &instance, DBUS_TYPE_UINT16, &n, DBUS_TYPE_ARRAY,
                                       DBUS_TYPE_BYTE, &p, static_cast<int>(bytes.size()), DBUS_TYPE_INVALID));
        send(m);
    }
    void configReply(uint8_t instance, dbus_uint16_t id) {
        uint8_t statusCode = 0;
        dbus_uint16_t len = 1;
        const uint8_t* p = &config;
        DBusMessage* m = dbus_message_new_signal(path, dm, "BtApplReadConfigurationCfm");
        CHECK(m);
        CHECK(dbus_message_append_args(m, DBUS_TYPE_BYTE, &statusCode, DBUS_TYPE_BYTE, &instance, DBUS_TYPE_UINT16, &id,
                                       DBUS_TYPE_UINT16, &len, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &p, 1,
                                       DBUS_TYPE_INVALID));
        send(m);
    }
    bool awaitCount(std::atomic<int>& value, int expected) {
        for (int i = 0; i < 400 && value < expected; ++i) {
            sleepMs(5);
        }
        return value == expected;
    }
    void loop() {
        while (active) {
            dbus_connection_read_write(conn, 0);
            DBusMessage* m;
            while ((m = dbus_connection_pop_message(conn)) != 0) {
                if (dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
                    handle(m);
                }
                dbus_message_unref(m);
            }
            sleepMs(1);  // The service thread alone performs IO; producers only queue messages.
        }
    }
    void handle(DBusMessage* m) {
        const char* member = dbus_message_get_member(m);
        CHECK(member);
        if (!strcmp(member, "Introspect")) {
            DBusMessage* r = dbus_message_new_method_return(m);
            const char* xml = "<node/>";
            CHECK(dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID));
            send(r);
            return;
        }
        uint8_t instance;
        dbus_uint16_t len, id;
        const uint8_t* bytes;
        int n;
        if (!strcmp(member, "BtApplReadConfigurationReq")) {
            CHECK(dbus_message_get_args(m, 0, DBUS_TYPE_BYTE, &instance, DBUS_TYPE_UINT16, &id, DBUS_TYPE_INVALID));
            ++reads;
            if (!suppressConfig) {
                configReply(instance + 1, id);
                configReply(instance, id + 1);
                configReply(instance, id);
            }
        } else if (!strcmp(member, "BtApplSetConfigurationReq")) {
            CHECK(dbus_message_get_args(m, 0, DBUS_TYPE_BYTE, &instance, DBUS_TYPE_UINT16, &id, DBUS_TYPE_UINT16, &len,
                                        DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &bytes, &n, DBUS_TYPE_INVALID));
            CHECK(id == 0x6203 && n == 1 && len == 1 && bytes[0] <= 1);
            ++writes;
            config = bytes[0];
            uint8_t statusCode = 0;
            DBusMessage* r = dbus_message_new_signal(path, dm, "BtApplSetConfigurationCfm");
            CHECK(dbus_message_append_args(r, DBUS_TYPE_BYTE, &statusCode, DBUS_TYPE_BYTE, &instance, DBUS_TYPE_UINT16,
                                           &id, DBUS_TYPE_INVALID));
            send(r);
        } else if (!strcmp(member, "BtApplDeviceDisconnectReq")) {
            dbus_uint64_t function;
            CHECK(
                dbus_message_get_args(m, 0, DBUS_TYPE_UINT64, &function, DBUS_TYPE_BYTE, &instance, DBUS_TYPE_INVALID));
            CHECK(function == 1 && instance == 2);
            device(false, instance);
        } else {
            CHECK(dbus_message_has_interface(m, spp));
            CHECK(dbus_message_has_signature(m, "yqay"));
            CHECK(dbus_message_get_args(m, 0, DBUS_TYPE_BYTE, &instance, DBUS_TYPE_UINT16, &len, DBUS_TYPE_ARRAY,
                                        DBUS_TYPE_BYTE, &bytes, &n, DBUS_TYPE_INVALID));
            CHECK(n == len);
            if (!strcmp(member, "BtApplSppSetUuidReq")) {
                const uint8_t generic[] = {1, 0x11, 1, 1, 0x11, 1};
                CHECK((n == 34 && bytes[0] == 3 && bytes[17] == 3) || (n == 6 && !memcmp(bytes, generic, 6)));
                {
                    std::lock_guard<std::mutex> lock(mu);
                    lastUuid.assign(bytes, bytes + n);
                }
                ++uuidRequests;
                if (!suppressUuid) {
                    status("BtApplSppSetUuidCfm", instance + 1, 9);
                    status("BtApplSppSetUuidCfm", instance, 0);
                }
            } else {
                CHECK(!strcmp(member, "BtApplSppDataReq"));
                CHECK(n > 0 && n <= 1024);
                {
                    std::lock_guard<std::mutex> lock(mu);
                    sent.insert(sent.end(), bytes, bytes + n);
                }
                ++dataRequests;  // tests control ACK/flow independently
            }
        }
    }
};
bool readable(int fd, int timeout = 1000) {
    pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, timeout) > 0;
}
std::vector<uint8_t> receive(int fd, size_t n) {
    std::vector<uint8_t> result(n);
    size_t offset = 0;
    while (offset < n) {
        CHECK(readable(fd));
        ssize_t got = read(fd, &result[offset], n - offset);
        CHECK(got > 0);
        offset += got;
    }
    return result;
}
}  // namespace
int main() {
    // Refuse to run unless the test runner explicitly supplies an isolated bus.
    CHECK(getenv("ENTRY_AAD_TEST_BUS") && !strcmp(getenv("ENTRY_AAD_TEST_BUS"), "1"));
    CHECK(getenv("DBUS_SESSION_BUS_ADDRESS") && getenv("DBUS_SYSTEM_BUS_ADDRESS"));
    CHECK(!strcmp(getenv("DBUS_SESSION_BUS_ADDRESS"), getenv("DBUS_SYSTEM_BUS_ADDRESS")));
    CHECK(dbus_threads_init_default());
    FakeAlps fake;
    if (const char* diagnostic = getenv("ENTRY_AAD_DIAGNOSTIC")) {
        std::string out;
        std::vector<std::string> args(1, "--bluetooth");
        CHECK(evo::runCommand(diagnostic, args, out, 5000));
        CHECK(out.find("instance=0 configuration=0x6203 value=0") != std::string::npos);
        CHECK(out.find("instance=1 configuration=0x6203 value=0") != std::string::npos);
        args[0] = "--monitor";
        args.push_back("1");
        std::thread signals([&] {
            std::vector<uint8_t> payload(4, 0x42);
            for (int i = 0; i < 5; ++i) {
                sleepMs(100);
                fake.data(1, payload);
            }
        });
        evo::CommandReport observed;
        bool observedOk = evo::runCommand(diagnostic, args, out, 3500, &observed);
        signals.join();
        CHECK(observedOk);
        CHECK(out.find("Watch over") != std::string::npos);
        CHECK(out.find("BtApplSppDataInd (yqay) instance=1") != std::string::npos);
        args.resize(1);
        args[0] = "--run";
        CHECK(!evo::runCommand(diagnostic, args, out));
        CHECK(fake.uuidRequests == 0 && fake.writes == 0 && fake.dataRequests == 0);
        puts("PASS evo-check: lecture/observation seules, duree bornee, mode actif refuse");
    }
    std::vector<uint8_t> bytes(4);
    bytes[0] = 0;
    bytes[1] = 0xff;
    bytes[2] = 0x80;
    bytes[3] = 1;
    {
        evo::AlpsSppLink monitor;
        monitor.setMonitorOnly(true);
        CHECK(monitor.start(2));
        CHECK(monitor.registerUuid(evo::kAaUuid, 200) == -1);
        CHECK(monitor.introspect(path) == "<node/>");
        std::vector<uint8_t> cfg;
        CHECK(monitor.readConfiguration(2, 0x6203, cfg, 1000));
        CHECK(cfg.size() == 1 && cfg[0] == 0);
        CHECK(!monitor.ensureDataInterface(true, 1000));
        CHECK(!monitor.setDataInterface(0, 1000));
        CHECK(!monitor.requestSppDisconnect());
        fake.device(true, 2);
        fake.data(2, bytes);
        CHECK(monitor.waitLink(50) == -1);
        CHECK(fake.uuidRequests == 0 && fake.dataRequests == 0 && fake.writes == 0);
    }
    {
        evo::AlpsSppLink link;
        CHECK(link.start(2));
        CHECK(!link.ensureDataInterface(false, 1000));
        CHECK(fake.writes == 0);
        // Slot 2: the stack answers its reads with slot 0's value, so nothing is read, the write's status decides.
        const int readsBefore = fake.reads;
        CHECK(link.ensureDataInterface(true, 1000));
        CHECK(fake.writes == 1 && fake.reads == readsBefore && fake.config == 1);
        CHECK(link.registerUuid(evo::kAaUuid, 1000) == 0);
        fake.device(true, 1);
        fake.device(true, 2, 2);
        fake.device(true, 2, 1, 1);
        fake.data(2, bytes, true);
        CHECK(link.waitLink(100) == -1);
        fake.data(2, bytes);  // First DataInd also establishes a link; preserve bytes before waitLink.
        int fd = link.waitLink(1000);
        CHECK(fd >= 0);
        CHECK(link.waitLink(0) == -1);
        CHECK(receive(fd, bytes.size()) == bytes);
        std::vector<uint8_t> tx(2500);
        for (size_t i = 0; i < tx.size(); ++i) {
            tx[i] = static_cast<uint8_t>(i);
        }
        CHECK(write(fd, &tx[0], tx.size()) == static_cast<ssize_t>(tx.size()));
        CHECK(fake.awaitCount(fake.dataRequests, 1));
        fake.ready(3);
        fake.status("BtApplSppDataCfm", 2, 0);
        sleepMs(80);
        CHECK(fake.dataRequests == 1);
        fake.status("BtApplSppDataSendFcInd", 2, 1);
        fake.ready(2);
        sleepMs(80);
        CHECK(fake.dataRequests == 1);
        fake.ready(2);
        sleepMs(40);
        CHECK(fake.dataRequests == 1);  // SendInd cannot clear an FC pause.
        fake.status("BtApplSppDataSendFcInd", 2, 0);
        CHECK(fake.awaitCount(fake.dataRequests, 2));
        fake.ready(2);
        CHECK(fake.awaitCount(fake.dataRequests, 3));  // success may have no DataCfm
        fake.ready(2);
        {
            std::lock_guard<std::mutex> lock(fake.mu);
            CHECK(fake.sent == tx);
        }
        fake.device(false, 2, 2);
        sleepMs(40);
        CHECK(link.linkUp());  // A2DP is not SPP.
        fake.data(2, bytes);
        fake.device(false, 2);
        CHECK(receive(fd, bytes.size()) == bytes);
        CHECK(readable(fd));
        uint8_t v;
        CHECK(read(fd, &v, 1) == 0);
        CHECK(fcntl(fd, F_GETFD) >= 0);
        link.closeLink();
        CHECK(fcntl(fd, F_GETFD) == -1);
        fake.device(true, 2);
        int again = link.waitLink(1000);
        CHECK(again >= 0);
        link.interruptLink();
        CHECK(readable(again));
        CHECK(read(again, &v, 1) == 0);
        CHECK(fcntl(again, F_GETFD) >= 0);
        link.closeLink();
        CHECK(link.waitLink(20) == -1);
        const uint8_t generic[] = {1, 0x11, 1, 1, 0x11, 1};
        CHECK(link.registerUuidPayload(std::vector<uint8_t>(generic, generic + 6), 1000) == 0);
        uint8_t bmwInit[16], bmwRecv[16];
        CHECK(evo::parseUuid128("00000000-deca-fade-deca-deafdecacaff", bmwInit));
        CHECK(evo::parseUuid128("00000000-deca-fade-deca-deafdecacafe", bmwRecv));
        const std::vector<uint8_t> bmwPair = evo::buildSetUuidPayload(bmwInit, bmwRecv);
        CHECK(link.registerUuidPayload(bmwPair, 1000) == 0);
        {
            std::lock_guard<std::mutex> lock(fake.mu);
            CHECK(fake.lastUuid == bmwPair && fake.lastUuid[16] == 0xff && fake.lastUuid[33] == 0xfe);
        }
        int before = fake.uuidRequests;
        CHECK(link.registerUuidPayload(std::vector<uint8_t>(generic, generic + 5), 1000) == -1);
        CHECK(fake.uuidRequests == before);
        CHECK(link.setDataInterface(0, 1000));
        CHECK(fake.config == 0);
        CHECK(!link.setDataInterface(2, 1000));
        fake.device(true, 2);
        CHECK(link.waitLink(1000) >= 0);
        CHECK(link.requestSppDisconnect());
        for (int i = 0; i < 100 && link.linkUp(); ++i) {
            sleepMs(10);
        }
        CHECK(!link.linkUp());
        link.closeLink();
    }
    {
        // A slot the stack reads back (0 or 1): read, written, read back; nothing written when already 1.
        CHECK(evo::slotConfigurationReadable(0) && evo::slotConfigurationReadable(1));
        CHECK(!evo::slotConfigurationReadable(2) && !evo::slotConfigurationReadable(5));
        evo::AlpsSppLink link;
        CHECK(link.start(1));
        fake.config = 0;
        const int reads = fake.reads, writes = fake.writes;
        CHECK(link.ensureDataInterface(true, 1000));
        CHECK(fake.writes == writes + 1 && fake.reads == reads + 2 && fake.config == 1);
        CHECK(link.ensureDataInterface(true, 1000) && fake.writes == writes + 1 && fake.reads == reads + 3);
        CHECK(link.setDataInterface(0, 1000) && fake.config == 0 && fake.reads == reads + 4);
    }
    {
        evo::AlpsSppLink link;
        CHECK(link.start(2));
        fake.suppressUuid = true;
        CHECK(link.registerUuid(evo::kAaUuid, 80) == -1);
        int before = fake.uuidRequests;
        fake.status("BtApplSppSetUuidCfm", 2, 0);
        sleepMs(30);
        CHECK(link.registerUuid(evo::kAaUuid, 80) == -1);
        CHECK(fake.uuidRequests == before);
        fake.suppressConfig = true;
        std::vector<uint8_t> cfg;
        CHECK(!link.readConfiguration(2, 0x6203, cfg, 80));
        int readBefore = fake.reads;
        fake.configReply(2, 0x6203);
        sleepMs(30);
        CHECK(!link.readConfiguration(2, 0x6203, cfg, 80));
        CHECK(fake.reads == readBefore);
    }
    {
        evo::AlpsSppLink link;
        CHECK(link.start(2));
        fake.device(true, 2);
        int fd = link.waitLink(1000);
        CHECK(fd >= 0);
        CHECK(write(fd, &bytes[0], bytes.size()) == static_cast<ssize_t>(bytes.size()));
        CHECK(fake.awaitCount(fake.dataRequests, 4));
        fake.status("BtApplSppDataCfm", 2, 7);
        CHECK(readable(fd));
        uint8_t v;
        CHECK(read(fd, &v, 1) == 0);
        link.closeLink();
    }
    {
        evo::AlpsSppLink link;
        CHECK(link.start(2));
        std::atomic<int> result(-2);
        std::thread waiter([&] { result = link.waitLink(5000); });
        sleepMs(20);
        link.stop();
        waiter.join();
        CHECK(result == -1);
    }
    {
        evo::AlpsSppLink link;
        CHECK(link.start(2));
        fake.device(true, 2);
        int fd = link.waitLink(1000);
        CHECK(fd >= 0);
        CHECK(write(fd, &bytes[0], bytes.size()) == static_cast<ssize_t>(bytes.size()));
        CHECK(fake.awaitCount(fake.dataRequests, 5));
        fake.status("BtApplSppDataCfm", 2, 0);
        // An accepted request without SendInd must fail within a bounded time, not queue more writes.
        CHECK(readable(fd, 6500));
        uint8_t v;
        CHECK(read(fd, &v, 1) == 0);
        link.closeLink();
    }
    {
        evo::AlpsSppLink link;
        CHECK(link.start(2));
        fake.device(true, 2);
        int fd = link.waitLink(1000);
        CHECK(fd >= 0);
        std::vector<uint8_t> flood(60000, 0x42);
        for (int i = 0; i < 20; ++i) {
            fake.data(2, flood);
        }
        for (int i = 0; i < 400 && link.linkUp(); ++i) {
            sleepMs(5);
        }
        CHECK(!link.linkUp());
        CHECK(fcntl(fd, F_GETFD) >= 0);
        link.closeLink();
    }
    {
        evo::AlpsSppLink link;
        CHECK(link.start(2));
        DBusError e;
        dbus_error_init(&e);
        CHECK(dbus_bus_release_name(fake.conn, "org.alps.evolution", &e) == DBUS_RELEASE_NAME_REPLY_RELEASED);
        for (int i = 0; i < 400 && link.running(); ++i) {
            sleepMs(5);
        }
        CHECK(!link.running());
        CHECK(link.waitLink(10) == -1);
        evo::AlpsSppLink unavailable;
        CHECK(!unavailable.start(2));
        unavailable.stop();
    }
    puts("PASS D-Bus SPP: observation, signatures, instance/profil, HLI, flux, ACK, EOF, delais et arret");
    return 0;
}
