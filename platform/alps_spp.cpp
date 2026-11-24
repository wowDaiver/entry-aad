// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Alps Bluetooth SPP link exposed as a POSIX byte stream (socketpair served by a D-Bus thread).
#include "alps_spp.hpp"

#include <dbus/dbus.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "evo_log.hpp"

namespace evo {
namespace {
const char* const service = "org.alps.evolution";
const char* const path = "/org/alps/evolution/hli";
const char* const spp = "org.alps.evo.SPP";
const char* const dm = "org.alps.evo.DM";
const size_t maxRx = 256 * 1024;
pthread_once_t dbusOnce = PTHREAD_ONCE_INIT;
bool dbusReady = false;
void initDbus() {
    dbusReady = dbus_threads_init_default() != 0;
}
struct Lock {
    pthread_mutex_t* m;
    explicit Lock(pthread_mutex_t& v) : m(&v) { pthread_mutex_lock(m); }
    ~Lock() { pthread_mutex_unlock(m); }
};
timespec deadline(int ms) {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        ++t.tv_sec;
        t.tv_nsec -= 1000000000L;
    }
    return t;
}
bool appendBytes(DBusMessage* m, uint8_t inst, const uint8_t* data, size_t n, const uint16_t* id = nullptr) {
    if (!m || n > 65535) {
        return false;
    }
    DBusMessageIter it, a;
    dbus_message_iter_init_append(m, &it);
    dbus_uint16_t len = static_cast<dbus_uint16_t>(n);
    if (!dbus_message_iter_append_basic(&it, DBUS_TYPE_BYTE, &inst) ||
        (id && !dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT16, id)) ||
        !dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT16, &len) ||
        !dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "y", &a)) {
        return false;
    }
    if (n && !dbus_message_iter_append_fixed_array(&a, DBUS_TYPE_BYTE, &data, static_cast<int>(n))) {
        dbus_message_iter_abandon_container(&it, &a);
        return false;
    }
    return dbus_message_iter_close_container(&it, &a) != 0;
}
bool sendRequest(DBusConnection* conn, DBusMessage* m, bool valid) {
    bool ok = false;
    if (m) {
        dbus_message_set_no_reply(m, TRUE);  // Alps confirms through a separate signal.
        ok = valid && dbus_connection_send(conn, m, nullptr);
        dbus_message_unref(m);
    }
    return ok;
}
bool byteArray(DBusMessageIter& it, const uint8_t*& p, int& n) {
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY ||
        dbus_message_iter_get_element_type(&it) != DBUS_TYPE_BYTE) {
        return false;
    }
    DBusMessageIter a;
    dbus_message_iter_recurse(&it, &a);
    p = nullptr;
    n = 0;
    dbus_message_iter_get_fixed_array(&a, &p, &n);
    return n >= 0;
}
}  // namespace

struct AlpsSppLink::Impl {
    DBusConnection* conn;
    std::string owner;
    uint8_t inst;
    bool monitor, active, filterInstalled, dispatchStarted, pumpStarted;
    pthread_t dispatchThread, pumpThread;
    pthread_mutex_t mu, requestMu;
    pthread_cond_t cv;
    int pair[2];
    bool link, borrowed, requireConnect, remoteClosed, txPending, txAllowed, uuidWaiting, cfgWaiting;
    bool uuidPoisoned, cfgPoisoned, cfgWrite;
    int uuidStatus, cfgStatus;
    uint8_t cfgInst;
    uint16_t cfgId;
    uint32_t txSince;
    std::vector<uint8_t> rx, cfgData;
    size_t rxOffset;
    uint8_t phone[6];  // the device whose Android Auto link last opened, to tell it from the others in the log
    bool phoneKnown;

    Impl()
        : conn(nullptr),
          inst(0),
          monitor(false),
          active(false),
          filterInstalled(false),
          dispatchStarted(false),
          pumpStarted(false),
          link(false),
          borrowed(false),
          requireConnect(false),
          remoteClosed(false),
          txPending(false),
          txAllowed(true),
          uuidWaiting(false),
          cfgWaiting(false),
          uuidPoisoned(false),
          cfgPoisoned(false),
          cfgWrite(false),
          uuidStatus(-1),
          cfgStatus(-1),
          cfgInst(0),
          cfgId(0),
          txSince(0),
          rxOffset(0),
          phoneKnown(false) {
        memset(phone, 0, sizeof(phone));
        pair[0] = pair[1] = -1;
        pthread_mutex_init(&mu, nullptr);
        pthread_mutex_init(&requestMu, nullptr);
        pthread_condattr_t a;
        pthread_condattr_init(&a);
        pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
        pthread_cond_init(&cv, &a);
        pthread_condattr_destroy(&a);
    }
    ~Impl() {
        pthread_cond_destroy(&cv);
        pthread_mutex_destroy(&mu);
        pthread_mutex_destroy(&requestMu);
    }

    // mu held. Retain the borrowed end until closeLink: never close a descriptor in use by WPP.
    void breakLink() {
        if (link) {
            requireConnect = true;
        }
        if (pair[0] >= 0) {
            shutdown(pair[0], SHUT_RDWR);
            close(pair[0]);
            pair[0] = -1;
        }
        if (!borrowed && pair[1] >= 0) {
            close(pair[1]);
            pair[1] = -1;
        }
        link = false;
        txPending = false;
        rx.clear();
        rxOffset = 0;
        pthread_cond_broadcast(&cv);
    }
    bool openLink() {
        if (link) {
            return true;
        }
        if (pair[1] >= 0 || monitor || !active || requireConnect) {
            return false;
        }
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
            return false;
        }
        if (fcntl(pair[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(pair[1], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(pair[0], F_SETFL, O_NONBLOCK) < 0) {
            breakLink();
            return false;
        }
        link = true;
        remoteClosed = false;
        txPending = false;
        txAllowed = true;
        ELOG("SPP", "link open, instance %u", inst);
        pthread_cond_broadcast(&cv);
        return true;
    }
    bool sendBytes(const char* member, const uint8_t* data, size_t n, const uint16_t* id = nullptr) {
        DBusMessage* m = dbus_message_new_method_call(owner.c_str(), path, id ? dm : spp, member);
        return sendRequest(conn, m, appendBytes(m, inst, data, n, id));
    }
    static DBusHandlerResult filter(DBusConnection*, DBusMessage* m, void* arg) {
        Impl* p = static_cast<Impl*>(arg);
        Lock lock(p->mu);
        if (dbus_message_is_signal(m, DBUS_INTERFACE_LOCAL, "Disconnected")) {
            p->active = false;
            p->breakLink();
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        if (dbus_message_is_signal(m, DBUS_INTERFACE_DBUS, "NameOwnerChanged") &&
            dbus_message_has_sender(m, DBUS_SERVICE_DBUS)) {
            const char *name, *oldOwner, *newOwner;
            if (dbus_message_get_args(m, nullptr, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &oldOwner,
                                      DBUS_TYPE_STRING, &newOwner, DBUS_TYPE_INVALID) &&
                !strcmp(name, service) && p->owner != newOwner) {
                ELOG("SPP", "Alps service replaced or lost: the link must restart");
                p->active = false;
                p->breakLink();
            }
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        if (!p->active || !dbus_message_has_path(m, path) || !dbus_message_has_sender(m, p->owner.c_str()) ||
            dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_SIGNAL) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        p->signal(m);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    void signal(DBusMessage* m) {
        const char* member = dbus_message_get_member(m);
        if (!member) {
            return;
        }
        // Metadata only: no SPP payload or Wi-Fi passphrase in diagnostics.
        if (monitor) {
            DBusMessageIter meta;
            if (dbus_message_has_interface(m, spp) && dbus_message_iter_init(m, &meta) &&
                dbus_message_iter_get_arg_type(&meta) == DBUS_TYPE_BYTE) {
                uint8_t instance;
                dbus_message_iter_get_basic(&meta, &instance);
                ELOG("SPP", "observed %s.%s (%s) instance=%u", dbus_message_get_interface(m), member,
                     dbus_message_get_signature(m), instance);
            } else {
                ELOG("SPP", "observed %s.%s (%s)", dbus_message_get_interface(m), member,
                     dbus_message_get_signature(m));
            }
        }
        if (dbus_message_has_interface(m, dm)) {
            if (!strcmp(member, "BtApplReadConfigurationCfm") || !strcmp(member, "BtApplSetConfigurationCfm")) {
                const bool writing = !strcmp(member, "BtApplSetConfigurationCfm");
                if (!dbus_message_has_signature(m, writing ? "yyq" : "yyqqay")) {
                    return;
                }
                DBusMessageIter it;
                dbus_message_iter_init(m, &it);
                uint8_t status, instance;
                dbus_uint16_t id, len = 0;
                dbus_message_iter_get_basic(&it, &status);
                dbus_message_iter_next(&it);
                dbus_message_iter_get_basic(&it, &instance);
                dbus_message_iter_next(&it);
                dbus_message_iter_get_basic(&it, &id);
                if (!cfgWaiting || cfgWrite != writing || cfgInst != instance || cfgId != id) {
                    return;
                }
                cfgData.clear();
                if (!writing) {
                    dbus_message_iter_next(&it);
                    dbus_message_iter_get_basic(&it, &len);
                    dbus_message_iter_next(&it);
                    const uint8_t* data;
                    int n;
                    if (!byteArray(it, data, n) || n != len) {
                        return;
                    }
                    if (n) {
                        cfgData.assign(data, data + n);
                    }
                }
                cfgStatus = status;
                cfgWaiting = false;
                pthread_cond_broadcast(&cv);
                return;
            }
            const bool connecting = !strcmp(member, "BtApplDeviceConnectInd");
            if (!connecting && strcmp(member, "BtApplDeviceDisconnectInd")) {
                return;
            }
            if (!dbus_message_has_signature(m, connecting ? "yayty" : "yayyty")) {
                return;
            }
            DBusMessageIter it;
            dbus_message_iter_init(m, &it);
            uint8_t status, instance;
            dbus_uint64_t function;
            dbus_message_iter_get_basic(&it, &status);
            dbus_message_iter_next(&it);
            const uint8_t* addr;
            int n;
            if (!byteArray(it, addr, n) || n != 6) {
                return;
            }
            dbus_message_iter_next(&it);
            uint8_t reason = 0;
            if (!connecting) {
                dbus_message_iter_get_basic(&it, &reason);
                dbus_message_iter_next(&it);
            }
            dbus_message_iter_get_basic(&it, &function);
            dbus_message_iter_next(&it);
            dbus_message_iter_get_basic(&it, &instance);
            // The BMW Bluetooth service (btmbt) reads function == 1 as SPP in these signals.
            const bool ours = !monitor && instance == inst && function == 1 && (status & 0x7f) == 0;
            if (ours && connecting) {
                memcpy(phone, addr, sizeof(phone));
                phoneKnown = true;
            }
            // Every Bluetooth connection the car makes, accepts or loses, with any device: the radio is shared with
            // the Wi-Fi. No address is written: only whether the device is the Android Auto phone. A disconnection
            // comes with the reason the car's Bluetooth service gives (btmbt: 2 is the other side closing).
            const char* who =
                !phoneKnown ? "" : (memcmp(phone, addr, sizeof(phone)) ? ", another device" : ", the phone");
            if (connecting) {
                ELOG("SPP", "%s instance=%u function=%llu status=%u%s", member, instance, (unsigned long long)function,
                     status, who);
            } else {
                ELOG("SPP", "%s instance=%u function=%llu status=%u reason=%u%s", member, instance,
                     (unsigned long long)function, status, reason, who);
            }
            if (!ours) {
                return;
            }
            if (connecting) {
                requireConnect = false;
                openLink();
            } else {
                // Preserve bytes already received before exposing EOF to the WPP reader.
                remoteClosed = true;
                requireConnect = true;
                txPending = false;
                txAllowed = false;
                if (rxOffset == rx.size()) {
                    breakLink();
                }
            }
            return;
        }
        if (!dbus_message_has_interface(m, spp)) {
            return;
        }
        DBusMessageIter it;
        if (!dbus_message_iter_init(m, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_BYTE) {
            return;
        }
        uint8_t instance;
        dbus_message_iter_get_basic(&it, &instance);
        if (instance != inst) {
            return;
        }
        if (!strcmp(member, "BtApplSppSetUuidCfm") && dbus_message_has_signature(m, "yy")) {
            dbus_message_iter_next(&it);
            uint8_t status;
            dbus_message_iter_get_basic(&it, &status);
            if (uuidWaiting) {
                uuidStatus = status;
                uuidWaiting = false;
                pthread_cond_broadcast(&cv);
            }
            return;
        }
        if (monitor) {
            return;
        }
        if (!strcmp(member, "BtApplSppDataInd") && dbus_message_has_signature(m, "yqay")) {
            dbus_uint16_t len;
            dbus_message_iter_next(&it);
            dbus_message_iter_get_basic(&it, &len);
            dbus_message_iter_next(&it);
            const uint8_t* data;
            int n;
            if (!byteArray(it, data, n) || n != len || !n) {
                return;
            }
            if (remoteClosed || !openLink()) {
                return;
            }
            if (rx.size() - rxOffset + n > maxRx) {
                ELOG("SPP", "receive buffer full: closing the stream, nothing truncated");
                breakLink();
                return;
            }
            if (rxOffset) {
                rx.erase(rx.begin(), rx.begin() + rxOffset);
                rxOffset = 0;
            }
            rx.insert(rx.end(), data, data + n);
            pthread_cond_broadcast(&cv);
        } else if (link && !strcmp(member, "BtApplSppDataCfm") && dbus_message_has_signature(m, "yy")) {
            uint8_t status;
            dbus_message_iter_next(&it);
            dbus_message_iter_get_basic(&it, &status);
            if (!txPending) {
                return;
            }
            // As in the car's own SPP client (libNmeTransport): a successful DataCfm does not finish a send,
            // SendInd does. DataCfm only reports failures to the waiter.
            if (status) {
                ELOG("SPP", "DataCfm error %u", status);
                breakLink();
            }
            pthread_cond_broadcast(&cv);
        } else if (link && !strcmp(member, "BtApplSppSendInd") && dbus_message_has_signature(m, "y")) {
            if (txPending) {
                txPending = false;
            }
            pthread_cond_broadcast(&cv);  // Completion does not override a flow-control pause.
        } else if (link && !strcmp(member, "BtApplSppDataSendFcInd") && dbus_message_has_signature(m, "yy")) {
            uint8_t status;
            dbus_message_iter_next(&it);
            dbus_message_iter_get_basic(&it, &status);
            // Same rule as libNmeTransport: a non-zero status pauses sending, zero resumes it and wakes the sender.
            txAllowed = status == 0;
            ELOG("SPP", "flow control status=%u (%s)", status, txAllowed ? "ready" : "paused");
            pthread_cond_broadcast(&cv);
        }
    }
    static void* dispatch(void* arg) {
        Impl* p = static_cast<Impl*>(arg);
        for (;;) {
            {
                Lock lock(p->mu);
                if (!p->active) {
                    break;
                }
            }
            if (!dbus_connection_read_write_dispatch(p->conn, 50)) {
                Lock lock(p->mu);
                p->active = false;
                p->breakLink();
                break;
            }
            usleep(1000);  // Let synchronous readers/producers acquire libdbus's IO lock.
        }
        return nullptr;
    }
    static void* pump(void* arg) {
        Impl* p = static_cast<Impl*>(arg);
        Lock lock(p->mu);
        while (p->active) {
            if (p->link) {
                if (p->rxOffset < p->rx.size()) {
                    ssize_t n =
                        send(p->pair[0], &p->rx[p->rxOffset], p->rx.size() - p->rxOffset, MSG_DONTWAIT | MSG_NOSIGNAL);
                    if (n > 0) {
                        p->rxOffset += static_cast<size_t>(n);
                    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        p->breakLink();
                    }
                    if (p->rxOffset == p->rx.size()) {
                        p->rx.clear();
                        p->rxOffset = 0;
                        if (p->remoteClosed) {
                            p->breakLink();
                        }
                    }
                }
                if (p->txPending && monoMs() - p->txSince >= 5000) {
                    ELOG("SPP", "no SendInd for 5 s: closing");
                    p->breakLink();
                }
                if (p->link && !p->remoteClosed && !p->txPending && p->txAllowed) {
                    uint8_t buf[1024];
                    ssize_t n = recv(p->pair[0], buf, sizeof(buf), MSG_DONTWAIT);
                    if (n > 0) {
                        p->txPending = true;
                        p->txSince = monoMs();
                        if (!p->sendBytes("BtApplSppDataReq", buf, static_cast<size_t>(n))) {
                            p->breakLink();
                        }
                    } else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                        p->breakLink();
                    }
                }
            }
            timespec t = deadline(10);
            pthread_cond_timedwait(&p->cv, &p->mu, &t);
        }
        return nullptr;
    }
    // requestMu held by caller; only one Alps config transaction at a time.
    bool configuration(uint8_t instance, uint16_t id, bool writing, std::vector<uint8_t>& out, int ms,
                       uint8_t value = 1) {
        Lock lock(mu);
        out.clear();
        if (!active || cfgPoisoned || ms <= 0 || (writing && monitor)) {
            return false;
        }
        cfgInst = instance;
        cfgId = id;
        cfgWrite = writing;
        cfgStatus = -1;
        cfgWaiting = true;
        cfgData.clear();
        bool sent;
        if (writing) {
            sent = sendBytes("BtApplSetConfigurationReq", &value, 1, &id);
        } else {
            DBusMessage* m = dbus_message_new_method_call(owner.c_str(), path, dm, "BtApplReadConfigurationReq");
            bool ok =
                m && dbus_message_append_args(m, DBUS_TYPE_BYTE, &instance, DBUS_TYPE_UINT16, &id, DBUS_TYPE_INVALID);
            sent = sendRequest(conn, m, ok);
        }
        if (!sent) {
            cfgWaiting = false;
            return false;
        }
        timespec t = deadline(ms);
        while (active && cfgWaiting) {
            if (pthread_cond_timedwait(&cv, &mu, &t) == ETIMEDOUT) {
                break;
            }
        }
        // Cfm carries no transaction serial: never reuse a timed-out request slot.
        if (cfgWaiting) {
            cfgPoisoned = true;
            cfgWaiting = false;
            return false;
        }
        if (!active || cfgStatus != 0) {
            if (active) {
                ELOG("SPP", "configuration %s of 0x%04x on instance %u refused (status %d)", writing ? "write" : "read",
                     static_cast<unsigned>(id), static_cast<unsigned>(instance), cfgStatus);
            }
            return false;
        }
        out = cfgData;
        return true;
    }
};

AlpsSppLink::AlpsSppLink() : p_(new Impl) {}
AlpsSppLink::~AlpsSppLink() {
    stop();
    delete p_;
}
void AlpsSppLink::setMonitorOnly(bool on) {
    Lock lock(p_->mu);
    if (!p_->conn) {
        p_->monitor = on;
    }
}
bool AlpsSppLink::running() {
    Lock lock(p_->mu);
    return p_->active;
}
bool AlpsSppLink::linkUp() {
    Lock lock(p_->mu);
    return p_->link;
}

bool AlpsSppLink::start(uint8_t instance) {
    // Lifecycle start/stop is serialized by the owning thread.
    if (p_->conn) {
        return false;
    }
    pthread_once(&dbusOnce, initDbus);
    if (!dbusReady) {
        return false;
    }
    DBusError e;
    dbus_error_init(&e);
    // Private connection: stop must not close another component's shared system bus.
    p_->conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &e);
    if (!p_->conn) {
        ELOG("SPP", "system bus unreachable: %s", e.message ? e.message : "error");
        dbus_error_free(&e);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(p_->conn, FALSE);
    DBusMessage* q =
        dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetNameOwner");
    DBusMessage* reply = nullptr;
    if (q && dbus_message_append_args(q, DBUS_TYPE_STRING, &service, DBUS_TYPE_INVALID)) {
        reply = dbus_connection_send_with_reply_and_block(p_->conn, q, 2000, &e);
    }
    if (q) {
        dbus_message_unref(q);
    }
    const char* owner = nullptr;
    if (reply && dbus_message_get_args(reply, nullptr, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID)) {
        p_->owner = owner;
    }
    if (reply) {
        dbus_message_unref(reply);
    }
    if (dbus_error_is_set(&e) || p_->owner.empty()) {
        ELOG("SPP", "Alps service missing");
        dbus_error_free(&e);
        stop();
        return false;
    }
    if (!dbus_connection_add_filter(p_->conn, Impl::filter, p_, nullptr)) {
        stop();
        return false;
    }
    p_->filterInstalled = true;
    const char* interfaces[] = {spp, dm};
    for (size_t i = 0; i < 2; ++i) {
        std::string rule =
            std::string("type='signal',sender='") + service + "',path='" + path + "',interface='" + interfaces[i] + "'";
        dbus_bus_add_match(p_->conn, rule.c_str(), &e);
        if (dbus_error_is_set(&e)) {
            dbus_error_free(&e);
            stop();
            return false;
        }
    }
    dbus_bus_add_match(p_->conn,
                       "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='"
                       "NameOwnerChanged',arg0='org.alps.evolution'",
                       &e);
    if (dbus_error_is_set(&e)) {
        dbus_error_free(&e);
        stop();
        return false;
    }
    {
        Lock lock(p_->mu);
        p_->inst = instance;
        p_->active = true;
        p_->uuidPoisoned = p_->cfgPoisoned = p_->requireConnect = p_->remoteClosed = false;
    }
    if (pthread_create(&p_->dispatchThread, nullptr, Impl::dispatch, p_) != 0) {
        stop();
        return false;
    }
    p_->dispatchStarted = true;
    if (!p_->monitor) {
        if (pthread_create(&p_->pumpThread, nullptr, Impl::pump, p_) != 0) {
            stop();
            return false;
        }
        p_->pumpStarted = true;
    }
    return true;
}
void AlpsSppLink::stop() {
    {
        Lock lock(p_->mu);
        p_->active = false;
        p_->breakLink();
    }
    if (p_->dispatchStarted) {
        pthread_join(p_->dispatchThread, nullptr);
        p_->dispatchStarted = false;
    }
    if (p_->pumpStarted) {
        pthread_join(p_->pumpThread, nullptr);
        p_->pumpStarted = false;
    }
    Lock request(p_->requestMu);
    if (p_->conn) {
        if (p_->filterInstalled) {
            dbus_connection_remove_filter(p_->conn, Impl::filter, p_);
            p_->filterInstalled = false;
        }
        dbus_connection_close(p_->conn);
        dbus_connection_unref(p_->conn);
        p_->conn = nullptr;
    }
    p_->owner.clear();
    // Caller must finish any WPP reader before stop; closeLink releases its end.
    closeLink();
}
int AlpsSppLink::registerUuid(const char* uuid, int ms) {
    uint8_t u[16];
    if (!parseUuid128(uuid, u) || ms <= 0) {
        return -1;
    }
    return registerUuidPayload(buildSetUuidPayload(u, u), ms);
}
int AlpsSppLink::registerUuidPayload(const std::vector<uint8_t>& data, int ms) {
    const size_t sizes[] = {0, 2, 4, 16};
    size_t pos = 0;
    for (int i = 0; i < 2; ++i) {
        if (pos >= data.size() || data[pos] < 1 || data[pos] > 3) {
            return -1;
        }
        pos += 1 + sizes[data[pos]];
    }
    if (pos != data.size() || ms <= 0) {
        return -1;
    }
    Lock request(p_->requestMu);
    Lock lock(p_->mu);
    if (!p_->active || p_->monitor || p_->uuidPoisoned || p_->link) {
        return -1;
    }
    p_->uuidWaiting = true;
    p_->uuidStatus = -1;
    if (!p_->sendBytes("BtApplSppSetUuidReq", &data[0], data.size())) {
        p_->uuidWaiting = false;
        return -1;
    }
    timespec t = deadline(ms);
    while (p_->active && p_->uuidWaiting) {
        if (pthread_cond_timedwait(&p_->cv, &p_->mu, &t) == ETIMEDOUT) {
            break;
        }
    }
    if (p_->uuidWaiting) {
        p_->uuidPoisoned = true;
        p_->uuidWaiting = false;
        return -1;
    }
    return p_->active ? p_->uuidStatus : -1;
}
int AlpsSppLink::waitLink(int ms) {
    Lock lock(p_->mu);
    if (ms < 0 || p_->monitor || p_->borrowed) {
        return -1;
    }
    timespec t = deadline(ms);
    while (p_->active && !p_->link) {
        if (pthread_cond_timedwait(&p_->cv, &p_->mu, &t) == ETIMEDOUT) {
            break;
        }
    }
    if (!p_->active || !p_->link) {
        return -1;
    }
    p_->borrowed = true;
    return p_->pair[1];
}
void AlpsSppLink::closeLink() {
    Lock lock(p_->mu);
    p_->breakLink();
    if (p_->pair[1] >= 0) {
        close(p_->pair[1]);
        p_->pair[1] = -1;
    }
    p_->borrowed = false;
}
void AlpsSppLink::interruptLink() {
    Lock lock(p_->mu);
    p_->breakLink();
}
bool AlpsSppLink::readConfiguration(uint8_t instance, uint16_t id, std::vector<uint8_t>& out, int ms) {
    Lock request(p_->requestMu);
    return p_->configuration(instance, id, false, out, ms);
}
bool AlpsSppLink::ensureDataInterface(bool allowWrite, int ms) {
    Lock request(p_->requestMu);
    std::vector<uint8_t> data;
    if (!slotConfigurationReadable(p_->inst)) {
        // Its read would give slot 0's value: written, and confirmed by the write's own status.
        return allowWrite && p_->configuration(p_->inst, 0x6203, true, data, ms);
    }
    if (!p_->configuration(p_->inst, 0x6203, false, data, ms) || data.size() != 1) {
        return false;
    }
    if (data[0] == 1) {
        return true;
    }
    if (!allowWrite || data[0] != 0) {
        return false;
    }
    if (!p_->configuration(p_->inst, 0x6203, true, data, ms)) {
        return false;
    }
    return p_->configuration(p_->inst, 0x6203, false, data, ms) && data.size() == 1 && data[0] == 1;
}
bool AlpsSppLink::setDataInterface(uint8_t value, int ms) {
    if (value > 1) {
        return false;
    }
    Lock request(p_->requestMu);
    std::vector<uint8_t> data;
    if (!p_->configuration(p_->inst, 0x6203, true, data, ms, value)) {
        return false;
    }
    if (!slotConfigurationReadable(p_->inst)) {
        return true;
    }
    return p_->configuration(p_->inst, 0x6203, false, data, ms) && data.size() == 1 && data[0] == value;
}
bool AlpsSppLink::requestSppDisconnect() {
    Lock request(p_->requestMu);
    Lock lock(p_->mu);
    if (!p_->active || p_->monitor) {
        return false;
    }
    dbus_uint64_t function = 1;
    DBusMessage* m = dbus_message_new_method_call(p_->owner.c_str(), path, dm, "BtApplDeviceDisconnectReq");
    bool valid =
        m && dbus_message_append_args(m, DBUS_TYPE_UINT64, &function, DBUS_TYPE_BYTE, &p_->inst, DBUS_TYPE_INVALID);
    return sendRequest(p_->conn, m, valid);
}
std::string AlpsSppLink::introspect(const char* objectPath) {
    Lock request(p_->requestMu);
    {
        Lock lock(p_->mu);
        if (!p_->active || !objectPath || !dbus_validate_path(objectPath, nullptr)) {
            return "";
        }
    }
    DBusMessage* m =
        dbus_message_new_method_call(p_->owner.c_str(), objectPath, DBUS_INTERFACE_INTROSPECTABLE, "Introspect");
    if (!m) {
        return "";
    }
    DBusError e;
    dbus_error_init(&e);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(p_->conn, m, 2000, &e);
    dbus_message_unref(m);
    std::string result;
    const char* xml;
    if (r && dbus_message_get_args(r, nullptr, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID)) {
        result = xml;
    }
    if (r) {
        dbus_message_unref(r);
    }
    dbus_error_free(&e);
    return result;
}
}  // namespace evo
