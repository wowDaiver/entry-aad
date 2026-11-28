// SPDX-License-Identifier: GPL-3.0-or-later
// A separate process: vmostclient has process-global state already used by HMI.
// Only an authenticated local instrument client can submit the fixed read list.
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "most_protocol.hpp"
#ifndef TELEMETRY_MOST_WORKER_TEST
#include "profile146.hpp"
#include "vendor/kizapp/profile_gate.hpp"
#endif

namespace {
volatile sig_atomic_t stopping = 0;
void stop_signal(int) {
    stopping = 1;
}
double clock_now() {
    timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : t.tv_sec + t.tv_nsec / 1e9;
}
bool nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && !fcntl(fd, F_SETFD, FD_CLOEXEC) && !fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
void log_event(const char* event, unsigned ecu = 0, unsigned did = 0) {
    printf("%.3f MOST %s ecu=%02x did=%04x\n", clock_now(), event, ecu, did);
    fflush(stdout);
}
struct NativeMessage {
    uint16_t address;
    uint8_t block, instance;
    uint16_t function, reserved;
    uint32_t operation;
    uint16_t size, reserved2;
    const uint8_t* data;
    uint32_t channel;
};
#ifndef TELEMETRY_MOST_WORKER_TEST
static_assert(sizeof(NativeMessage) == 24, "owner146 VMOST ARM32 ABI");
#endif

class Worker {
public:
    explicit Worker(const char* library, const char* state_directory)
        : client_(-1),
          native_fd_(-1),
          library_(library),
          state_directory_(state_directory),
          lib_(0),
          connected_(false),
          pending_(false),
          retired_(false),
          mask_(0),
          used_(0),
          output_size_(0),
          output_at_(0),
          deadline_(0),
          reconnect_due_(0),
          state_due_(0),
          good_(0),
          errors_(0),
          all_records_(0),
          connect_(0),
          shadow_(0),
          file_descriptor_(0),
          pump_(0),
          send_(0),
          disconnect_(0) {
        memset(&request_, 0, sizeof(request_));
    }
    ~Worker() {
        drop_client();
        close_native();
    }
    int client() const { return client_; }
    int native_fd() const { return native_fd_; }
    bool wants_output() const { return output_at_ < output_size_; }
    bool busy() const { return client_ >= 0 || pending_ || clock_now() < reconnect_due_; }
    void accept_client(int fd) {
        if (busy() || !nonblocking(fd)) {
            close(fd);
            return;
        }
        client_ = fd;
        mask_ = 0;
        used_ = output_size_ = output_at_ = 0;
        log_event("client-open");
    }
    void drop_client() {
        if (client_ >= 0) {
            close(client_);
            client_ = -1;
            log_event("client-closed");
        }
        used_ = output_size_ = output_at_ = mask_ = 0;
        // Keep the old transaction until its answer arrives. It must never be
        // delivered to a newly opened dashboard, even for the same ECU/DID.
        if (pending_) {
            retired_ = true;
        }
    }
    void read_client() {
        if (client_ < 0) {
            return;
        }
        if (pending_ || wants_output()) {
            drop_client();
            return;
        }
        const ssize_t n = recv(client_, input_ + used_, sizeof(input_) - used_, 0);
        if (!n) {
            drop_client();
            return;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                drop_client();
            }
            return;
        }
        used_ += static_cast<size_t>(n);
        if (used_ < 6) {
            return;
        }
        // No pipelining, arbitrary control words, payload lengths or writes.
        if (input_[0] || input_[1] || input_[2] || (input_[3] != 4 && input_[3] != 5) || used_ > input_[3] + 6u) {
            reject();
            return;
        }
        if (used_ < input_[3] + 6u) {
            return;
        }
        telemetry::MostRead request;
        if (!telemetry::decode_local_read(input_, used_, request) ||
            (request.did != 0xf150 && !(mask_ & (request.ecu == 0x12 ? 1 : 2)))) {
            reject();
            return;
        }
        used_ = 0;
        if (!open_native()) {
            ++errors_;
            drop_client();
            reconnect_due_ = clock_now() + 1;
            return;
        }
        request_ = request;
        pending_ = true;
        retired_ = false;
        deadline_ = clock_now() + (request.did == 0xf150 ? 12 : 5);
        uint8_t data[12];
        const size_t size = telemetry::encode_most_read(request, data);
        NativeMessage message = {0xffff, 6, 0xf0, telemetry::MostFunction, 0, 0, uint16_t(size), 0, data, 0};
        if (send_(&message)) {
            log_event("send-failed", request.ecu, request.did);
            // Treat a failed send as an uncertain outcome, too.
            pending_ = false;
            ++errors_;
            drop_client();
            close_native();
            reconnect_due_ = clock_now() + 30;
        }
    }
    void flush_client() {
        if (client_ < 0 || !wants_output()) {
            return;
        }
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        const ssize_t n = sendto(client_, output_ + output_at_, output_size_ - output_at_, flags, 0, 0);
        if (n > 0) {
            output_at_ += static_cast<size_t>(n);
        } else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            drop_client();
        }
        if (output_at_ == output_size_) {
            output_at_ = output_size_ = 0;
        }
    }
    void process_native() {
        if (connected_) {
            pump_();
        }
    }
    void lost_native() {
        log_event("bus-disconnected");
        ++errors_;
        if (pending_) {
            reconnect_due_ = clock_now() + 30;
        }
        pending_ = retired_ = false;
        drop_client();
        close_native();
    }
    void tick() {
        const double now = clock_now();
        if (pending_ && now >= deadline_) {
            log_event("timeout", request_.ecu, request_.did);
            ++errors_;
            pending_ = false;
            retired_ = false;
            drop_client();
            close_native();
            // This bus has no transaction nonce. Quarantine after an unknown
            // outcome so a late old reply cannot immediately satisfy a retry.
            reconnect_due_ = now + 30;
        }
        if (client_ < 0 && !pending_) {
            close_native();
        }
        if (now >= state_due_) {
            write_state();
            state_due_ = now + 5;
        }
    }
    void finish() {
        drop_client();
        close_native();
        write_state();
    }

private:
    int client_, native_fd_;
    const char *library_, *state_directory_;
    void* lib_;
    bool connected_, pending_, retired_;
    unsigned mask_;
    uint8_t input_[32], output_[32];
    size_t used_, output_size_, output_at_;
    double deadline_, reconnect_due_, state_due_;
    unsigned good_, errors_, all_records_;
    telemetry::MostRead request_;
    int (*connect_)(void (*)(void*, void*), void*, int);
    int (*shadow_)(uint8_t, uint8_t);
    int (*file_descriptor_)(int*);
    int (*pump_)();
    int (*send_)(void*);
    int (*disconnect_)();

    template <typename T>
    bool symbol(const char* name, T& fn) {
        void* p = dlsym(lib_, name);
        static_assert(sizeof(p) == sizeof(fn), "native function pointer size");
        memcpy(&fn, &p, sizeof(p));
        return p != 0;
    }
    bool open_native() {
        if (connected_) {
            return true;
        }
#ifndef TELEMETRY_MOST_WORKER_TEST
        struct stat st;
        if (stat("/var/run/vmost_sockstream", &st) || !S_ISSOCK(st.st_mode)) {
            return false;
        }
#endif
        if (!lib_) {
            lib_ = dlopen(library_, RTLD_NOW | RTLD_LOCAL);
        }
        if (!lib_ || !symbol("vmostConnect", connect_) || !symbol("vmostFblockShadowRegister", shadow_) ||
            !symbol("vmostFileDescriptorGet", file_descriptor_) || !symbol("vmostProcessMessages", pump_) ||
            !symbol("vmostMostMsgSend", send_) || !symbol("vmostDisconnect", disconnect_)) {
            return false;
        }
        if (connect_(&Worker::callback, this, 0)) {
            return false;
        }
        connected_ = true;
        if (shadow_(6, 0xf0) || file_descriptor_(&native_fd_) || native_fd_ < 0) {
            close_native();
            return false;
        }
        log_event("bus-ready");
        return true;
    }
    void close_native() {
        if (connected_) {
            disconnect_();
        }
        connected_ = false;
        native_fd_ = -1;
        // The original library owns process-global GLib objects: keep it loaded.
    }
    void reject() {
        log_event("request-rejected");
        ++errors_;
        drop_client();
    }
    static void callback(void* message, void* context) {
        static_cast<Worker*>(context)->answer(static_cast<NativeMessage*>(message));
    }
    void answer(const NativeMessage* m) {
        if (!pending_ || !m || m->size > 64) {
            return;
        }
        uint8_t response[32];
        size_t n = 0;
        const telemetry::MostAnswer result = telemetry::decode_most_answer(request_, m->block, m->instance, m->function,
                                                                           m->operation, m->data, m->size, response, n);
        if (result == telemetry::IgnoreMostAnswer || result == telemetry::PendingMostAnswer) {
            return;
        }
        pending_ = false;
        if (retired_ || client_ < 0) {
            retired_ = false;
            log_event("retired-answer-discarded");
            return;
        }
        if (result == telemetry::FailedMostAnswer) {
            ++errors_;
            log_event("assistant-unavailable", request_.ecu, request_.did);
            drop_client();
            return;
        }
        const unsigned prefix = request_.obd ? 2 : 3;
        const bool positive = response[8] == (request_.obd ? 0x41 : 0x62);
        if (positive && request_.did == 0xf150 && telemetry::diagnostic_identity(request_.ecu, response + 11, n - 11)) {
            mask_ |= request_.ecu == 0x12 ? 1 : 2;
            log_event("identity-verified", request_.ecu, request_.did);
        }
        if (positive && n == 8 + prefix + request_.size) {
            ++good_;
            unsigned count = 0;
            const telemetry::DiagnosticSource* sources = telemetry::diagnostic_sources(0, true, count);
            for (unsigned i = 0; i < count; ++i) {
                if (sources[i].did == request_.did) {
                    all_records_ |= 1u << i;
                }
            }
        } else {
            ++errors_;
        }
        memcpy(output_, response, n);
        output_size_ = n;
        output_at_ = 0;
        flush_client();
    }
    void write_state() {
        if (!state_directory_) {
            return;
        }
        const int dir = open(state_directory_, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        struct stat st;
        if (dir < 0) {
            return;
        }
        if (fstat(dir, &st) || st.st_uid != geteuid() || (st.st_mode & 0022)) {
            close(dir);
            return;
        }
        int old = openat(dir, "most.status.tmp", O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
        if (old >= 0 && !fstat(old, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() && st.st_nlink == 1) {
            char boot[64] = "unknown", line[512];
            const int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                const ssize_t n = read(fd, boot, sizeof(boot) - 1);
                if (n > 0) {
                    boot[n] = 0;
                }
                close(fd);
            }
            boot[strcspn(boot, "\r\n")] = 0;
            const int n =
                snprintf(line, sizeof(line),
                         "transport=internal-tester-assistant\nboot=%s\nuptime=%.3f\nclient=%d\nidentity_mask=%u\n"
                         "valid_replies=%u\nfailures=%u\nrecords_mask=%08x\npending=%u\n",
                         boot, clock_now(), client_ >= 0, mask_, good_, errors_, all_records_, pending_ ? 1u : 0u);
            if (n > 0 && n < int(sizeof(line)) && !ftruncate(old, 0) && write(old, line, n) == n) {
                close(old);
                old = -1;
                renameat(dir, "most.status.tmp", dir, "most.status");
            }
        }
        if (old >= 0) {
            close(old);
        }
        close(dir);
    }
};

int listener(const char* path, int& lock) {
    if (!path || strlen(path) >= sizeof(sockaddr_un::sun_path)) {
        return -1;
    }
    char parent[sizeof(sockaddr_un::sun_path)];
    strcpy(parent, path);
    char* slash = strrchr(parent, '/');
    if (!slash || slash == parent) {
        return -1;
    }
    *slash = 0;
    if (mkdir(parent, 0700) && errno != EEXIST) {
        return -1;
    }
    struct stat st;
    if (lstat(parent, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077)) {
        return -1;
    }
    char lock_path[sizeof(parent) + 8];
    snprintf(lock_path, sizeof(lock_path), "%s/lock", parent);
    lock = open(lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0 || fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
        (st.st_mode & 0077) || flock(lock, LOCK_EX | LOCK_NB)) {
        return -1;
    }
    if (!lstat(path, &st)) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid() || unlink(path)) {
            return -1;
        }
    } else if (errno != ENOENT) {
        return -1;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    if (!nonblocking(fd) || bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) || chmod(path, 0600) ||
        listen(fd, 2)) {
        close(fd);
        return -1;
    }
    return fd;
}
bool same_user(int fd) {
#ifdef __linux__
    ucred cred = {};
    socklen_t n = sizeof(cred);
    return !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &n) && n == sizeof(cred) && cred.uid == geteuid();
#elif defined(__APPLE__)
    uid_t uid;
    gid_t gid;
    return !getpeereid(fd, &uid, &gid) && uid == geteuid();
#else
    (void)fd;
    return false;
#endif
}
}  // namespace

int main() {
    const char* path = telemetry::MostSocketPath;
    const char* library = "libvmostclient.so.1";
    const char* state_directory = "/var/local/data/VehicleTelemetry146";
#ifdef TELEMETRY_MOST_WORKER_TEST
    path = getenv("TELEMETRY_TEST_SOCKET");
    library = getenv("TELEMETRY_TEST_LIBRARY");
    state_directory = getenv("TELEMETRY_TEST_STATE");
    if (!path || !library) {
        return 2;
    }
#else
    if (!sameBuild("/opt/hmi/bin/HmiMain3D", Hmi146, sizeof(Hmi146)) ||
        !sameBuild("/opt/hmi/lib/libAppFramework.A.so", App146, sizeof(App146)) ||
        !sameBuild("/opt/hmi/lib/libCoreFramework.A.so", Core146, sizeof(Core146))) {
        return 2;
    }
#endif
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    umask(0077);
    int lock = -1;
    const int server = listener(path, lock);
    if (server < 0) {
        perror("local MOST listener");
        return 3;
    }
    log_event("listening");
    Worker worker(library, state_directory);
    while (!stopping) {
        pollfd fds[] = {{server, POLLIN, 0},
                        {worker.client(), short(POLLIN | (worker.wants_output() ? POLLOUT : 0)), 0},
                        {worker.native_fd(), POLLIN, 0}};
        const int rc = poll(fds, 3, 100);
        if (rc < 0 && errno != EINTR) {
            break;
        }
        if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            worker.drop_client();
        } else {
            if (fds[1].revents & POLLIN) {
                worker.read_client();
            }
            if (fds[1].revents & POLLOUT) {
                worker.flush_client();
            }
        }
        if (fds[2].revents & POLLIN) {
            worker.process_native();
        }
        if (fds[2].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            worker.lost_native();
        }
        if (fds[0].revents & POLLIN) {
            const int fd = accept(server, 0, 0);
            if (fd >= 0) {
                if (same_user(fd)) {
                    worker.accept_client(fd);
                } else {
                    close(fd);
                }
            }
        }
        worker.tick();
    }
    worker.finish();
    close(server);
    unlink(path);
    close(lock);
    return 0;
}
