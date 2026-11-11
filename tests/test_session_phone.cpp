// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// A fake phone plays whole Android Auto sessions over a socketpair; what the head unit sends must match the golden.

// The phone side is written from the protocol, not from the code under test: it is the TLS server, as a real phone
// is, with a throwaway certificate, and it drives the session step by step. Recorded, in order: every frame each side
// sends (channel, flags, decrypted payload) and, separately, every session callback. Masked: the TLS handshake records
// and the head unit's clock (input and microphone timestamps); nothing else varies.
//   test-session-phone <golden>            compare (tests/fixtures/session_phone.golden)
//   test-session-phone --write <golden>    write the golden from the current code
#include <fcntl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "aa_session.hpp"

namespace {

typedef std::vector<uint8_t> Bytes;

// Throwaway identity of the fake phone (RSA 2048, self-signed). The head unit does not check it.
const char* const kPhoneCert =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDITCCAgmgAwIBAgIUKdxfXiryyn2X0r/ZqKwMFDYIqM8wDQYJKoZIhvcNAQEL\n"
    "BQAwHzEdMBsGA1UEAwwUZW50cnktYWFkIHRlc3QgcGhvbmUwIBcNMjYxMTA5MTg1\n"
    "ODM5WhgPMjEyNjEwMTYxODU4MzlaMB8xHTAbBgNVBAMMFGVudHJ5LWFhZCB0ZXN0\n"
    "IHBob25lMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA9rY5pHLazMgQ\n"
    "jeA70hLgb1wBrscFVoPpfVA6SYBTNy09i2zDiJUxcp61Zlkk8CVX+SDjdHSAm8Lv\n"
    "oLraVtA1brRDwTKCAUmWYCPR/EWmfU8CXqfkfL/gG/6CTKtyMja/ST6i+0zkql3T\n"
    "Kv7ug85vs18HToTTOBqcPmu63UVl2Orm5Na9tZJVW7DhowkmB6nH58cOhKbxyyu2\n"
    "d9hJFcCTWzzLo/gX4K4DeWTCuR/S15r/k4cKOlEpZJGN/Rd1A3Ofo4IjIhQ9Ltmd\n"
    "5NsEJ4rN8Nuxhdh6x/Dwc+D9IFWM2OcwqXo8l1k0iFlrEogsX4ounVbD2CxcG1Lx\n"
    "c1VrNGmWvQIDAQABo1MwUTAdBgNVHQ4EFgQUhAcWWiujJiAssHYJ8GLUhIaPF6Iw\n"
    "HwYDVR0jBBgwFoAUhAcWWiujJiAssHYJ8GLUhIaPF6IwDwYDVR0TAQH/BAUwAwEB\n"
    "/zANBgkqhkiG9w0BAQsFAAOCAQEALRAjPIffrs7qO7PyVpAi7AASXu0j8xIyjZlR\n"
    "SvlH/Jy9SGesh71oh0uuDFriGBZHHLHzjooOw9jN9pj/c0Gm9n+0XgbexbrLZqk+\n"
    "v/N/h0IWVdIma6KFUcuCs7nk/eXNFZbNuyTHHDrMQr09QFFSRUTl/nEd/n01gYLu\n"
    "F4P3RMUCkxKukSu+dfI7b5U23qW0j834CSAeH91C5WYEhey+shV2YKGfvXg5S1px\n"
    "DMMx+xpIPY2shegIiVt7dExjlTsxa5kGiGNKU411rEz0Vbvi6KqY8YChcSuaurcq\n"
    "oC1T80dfox2MVesFx9Es5C3lqhLM4dRvHUJbH6sTmsIdcEWVDg==\n"
    "-----END CERTIFICATE-----\n";
const char* const kPhoneKey =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQD2tjmkctrMyBCN\n"
    "4DvSEuBvXAGuxwVWg+l9UDpJgFM3LT2LbMOIlTFynrVmWSTwJVf5ION0dICbwu+g\n"
    "utpW0DVutEPBMoIBSZZgI9H8RaZ9TwJep+R8v+Ab/oJMq3IyNr9JPqL7TOSqXdMq\n"
    "/u6Dzm+zXwdOhNM4Gpw+a7rdRWXY6ubk1r21klVbsOGjCSYHqcfnxw6EpvHLK7Z3\n"
    "2EkVwJNbPMuj+BfgrgN5ZMK5H9LXmv+Thwo6USlkkY39F3UDc5+jgiMiFD0u2Z3k\n"
    "2wQnis3w27GF2HrH8PBz4P0gVYzY5zCpejyXWTSIWWsSiCxfii6dVsPYLFwbUvFz\n"
    "VWs0aZa9AgMBAAECggEAEIS670vRYoyyrw0odDRXWp1qb21b6iMC6yUIAgvZhoQ8\n"
    "7qzlmquCmA4QTz4AvyOA9GlDUXTLd6JhsP1RySY/rZ3cO39KPRPUQbOkR6PSNemo\n"
    "WvYa0k92jelIiCGij8IipKDRAMpxNQb7MJHJHoIf8H6YWqWCDuuZMoS8CjyvSQqv\n"
    "q2eM6TI/hc9DT+QeRZecOFeCxLuABshwUnfZsV/3UHbCdj/nWdOIbYvZgxs2aSCd\n"
    "siXFqHIIIPf1Li2YekRm2glAuXj7KWfuj3L8R6czWI6zIg/7Z6vHk5fUzgQuE3Jm\n"
    "Gv6LKoax4kxxG/b87k8Bj1zTBXPAC9F7QfMev0zSAQKBgQD9PVKDUZToC3IKI0bv\n"
    "se9vXOfCNJjNghNK4gFsKJ+T4GwRHsIh/OXiaqkrwH65OVtq2DD+fUNEFSet/PSy\n"
    "x4pgIJwLvOKokqkvENIwfE1VPfb62B8g3QfHsjEuwv2nTE1WAGLScxRY1mlhybVZ\n"
    "b5zhaCO2lTVDrqW/mu3+2Qh44QKBgQD5Zq/ZRIjvwqofqGWsjOzhS+E3h3ak+BdO\n"
    "PsjaDe+3z99CgzSzR7n+7GmAKZfpDwUTSXxUL2usqUDpNeF+zMdYVsl1v6OoikXn\n"
    "g28OUpebt6y5bzvzBkMMUqPhoELjeIEG6xh0lKRHrzwluyqEN86BV9Td/ULZ6IDo\n"
    "1e6ADTpNXQKBgQCwlIvuqXpNe32feA3Eoaw1WPD5yDcsRaEyO/mF86WiNYDlRy9T\n"
    "B8c8Y+dmbKlweoA2Nl2jdrDqzl2GwrXV27Gvac2lmaXqqaly+iW2TVaXM8f+DalW\n"
    "/winxKbq1miMuXXLnCVMXZY4g/mjiET+AgfAXR9Z9xzW/vbwbhW5+JghwQKBgD91\n"
    "5aD/U2jOu8M6ckSpx0y8ph4fFg18I1IBn9H8Ydb/yet+v1YvFiDLvzyhWMowzDXQ\n"
    "KRayyk8w8hdlYm+FY+QSkUio7tff91GlnAik83boEcdNio+YNE9aZ+Dtipee/hXo\n"
    "lFx1xieHa2KecX8bRCydUMj7vJr+Em8soHds5jzxAoGAZnJMe3l+haOLZR20lBeE\n"
    "7XEJqPpRH2ia5n2YuA+qg8ssW+q+2qMb/pDNdVzPXJRAXYV71VDYZPne5NRz+uHI\n"
    "dBv+uAju7y608gbvHWtZHGubPZZ+Tyj05jI522T2cYC/g+zGooq4sMUMlbsTcAMc\n"
    "N77cd+Eoqg8A2aUSmLO1few=\n"
    "-----END PRIVATE KEY-----\n";

// Frame flags and the Android key codes the steps use, written out here rather than taken from the code under test.
const uint8_t kPlain = 0x03;      // whole message, not encrypted
const uint8_t kSealed = 0x0b;     // whole message, encrypted
const uint8_t kSealedCtl = 0x0f;  // whole message, encrypted, control message on a service channel
const uint32_t kKeyHome = 3, kKeyBack = 4, kKeyUp = 19, kRotary = 65536;

const int kWaitMs = 4000;  // per frame; under the head unit's 5 s silence limit, ample under QEMU

Bytes bytes(std::initializer_list<int> list) {
    Bytes out;
    for (int v : list) {
        out.push_back(static_cast<uint8_t>(v));
    }
    return out;
}

Bytes operator+(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

Bytes text(const char* s) {
    return Bytes(s, s + strlen(s));
}

Bytes counting(size_t n, uint8_t first) {
    Bytes out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>(first + i);
    }
    return out;
}

std::string hex(const uint8_t* p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += digits[p[i] >> 4];
        s += digits[p[i] & 15];
    }
    return s;
}

std::string hex(const Bytes& b) {
    return b.empty() ? std::string("-") : hex(&b[0], b.size());
}

std::string format(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    return line;
}

// A message as the golden shows it: id, then body; TLS records and the head unit's clock replaced by a marker.
std::string describe(uint8_t channel, const Bytes& m, bool fromHeadUnit) {
    if (m.size() < 2) {
        return hex(m);
    }
    const unsigned id = (m[0] << 8) | m[1];
    std::string out = hex(&m[0], 2) + " ";
    size_t body = 2;
    if (channel == 0 && id == 0x0003) {
        return out + "<tls>";
    }
    if (fromHeadUnit && channel == 7 && id == 0x8001 && m.size() > 3 && m[2] == 0x08) {
        body = 3;
        while (body < m.size() && (m[body] & 0x80)) {
            ++body;
        }
        ++body;
        out += "08<time>";
    } else if (fromHeadUnit && channel == 14 && id == 0x0000 && m.size() >= 10) {
        body = 10;
        out += "<time>";
    }
    return body < m.size() ? out + hex(&m[body], m.size() - body) : out + "-";
}

// ---- what the session thread reports ------------------------------------------------------------------------------

pthread_mutex_t g_eventsMutex = PTHREAD_MUTEX_INITIALIZER;
std::vector<std::string> g_events;

void event(const std::string& line) {
    pthread_mutex_lock(&g_eventsMutex);
    g_events.push_back(line);
    pthread_mutex_unlock(&g_eventsMutex);
}

std::vector<std::string> takeEvents() {
    pthread_mutex_lock(&g_eventsMutex);
    std::vector<std::string> out;
    out.swap(g_events);
    pthread_mutex_unlock(&g_eventsMutex);
    return out;
}

// Holds the session thread inside the video callback, so that car input posted meanwhile is queued and sent together.
class Pause {
public:
    Pause() : armed_(false), inside_(false) {
        pthread_mutex_init(&mutex_, nullptr);
        pthread_cond_init(&cond_, nullptr);
    }
    void arm() { set(armed_, true); }
    // Session thread, from the video callback.
    void hold() {
        pthread_mutex_lock(&mutex_);
        if (armed_) {
            inside_ = true;
            pthread_cond_broadcast(&cond_);
            while (armed_) {
                pthread_cond_wait(&cond_, &mutex_);
            }
            inside_ = false;
        }
        pthread_mutex_unlock(&mutex_);
    }
    bool waitInside() {
        pthread_mutex_lock(&mutex_);
        timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += kWaitMs / 1000;
        int rc = 0;
        while (!inside_ && rc == 0) {
            rc = pthread_cond_timedwait(&cond_, &mutex_, &until);
        }
        const bool in = inside_;
        pthread_mutex_unlock(&mutex_);
        return in;
    }
    void release() { set(armed_, false); }

private:
    void set(bool& flag, bool value) {
        pthread_mutex_lock(&mutex_);
        flag = value;
        pthread_cond_broadcast(&cond_);
        pthread_mutex_unlock(&mutex_);
    }
    pthread_mutex_t mutex_;
    pthread_cond_t cond_;
    bool armed_, inside_;
};

Pause g_pause;

void onEnded(void*) {
    event("ended");
}
void onVideo(uint32_t frame, const uint8_t* data, size_t len, uint64_t pts, void*) {
    event(format("video frame=%u pts=%llu ", frame, static_cast<unsigned long long>(pts)) + hex(data, len));
    g_pause.hold();
}
void onCodec(const uint8_t* data, size_t len, void*) {
    event("codec " + hex(data, len));
}
void onFocus(bool projected, uint32_t reason, void*) {
    event(format("focus projected=%d reason=%u", projected ? 1 : 0, reason));
}
void onMic(bool open, void*) {
    event(format("mic open=%d", open ? 1 : 0));
}
void onNav(uint16_t id, const uint8_t* body, size_t len, void*) {
    event(format("nav %04x ", id) + (len ? hex(body, len) : std::string("-")));
}

// The golden was recorded on an M340i of 2019: the model and year come from a vehicle file, as on that car.
char g_vehicleFile[96];
void writeVehicleFile() {
    snprintf(g_vehicleFile, sizeof(g_vehicleFile), "/tmp/entry-session-vehicle-%d", int(getpid()));
    FILE* f = fopen(g_vehicleFile, "w");
    if (!f || fputs("model=M340i\nyear=2019\n", f) < 0 || fclose(f) != 0) {
        fprintf(stderr, "FAIL vehicle file\n");
        exit(1);
    }
}

void watch(aa::AASession& session, bool touch) {
    session.setTouchForwarding(touch);
    session.setSoftwareVersion("1.3.33");   // the version the golden was recorded with; the service announces its own
    session.setVehicleFile(g_vehicleFile);  // and the car it was recorded on
    session.setDisconnectCallback(onEnded);
    session.setVideoFrameCallback(onVideo);
    session.setCodecConfigCallback(onCodec);
    session.setVideoFocusCallback(onFocus);
    session.setMicRequestCallback(onMic);
    session.setNavStatusCallback(onNav);
}

bool waitEnded(aa::AASession& session) {
    for (int i = 0; i < kWaitMs / 10 && session.isActive(); ++i) {
        usleep(10000);
    }
    return !session.isActive();
}

// ---- the phone ----------------------------------------------------------------------------------------------------

int acceptAnyCertificate(int, X509_STORE_CTX*) {
    return 1;
}

// TLS server side through memory buffers, as the head unit runs its client side.
class PhoneTls {
public:
    PhoneTls() : ctx_(SSL_CTX_new(SSLv23_server_method())), ssl_(nullptr), in_(nullptr), out_(nullptr) {
        BIO* certText = BIO_new_mem_buf(const_cast<char*>(kPhoneCert), -1);
        X509* cert = PEM_read_bio_X509(certText, nullptr, nullptr, nullptr);
        BIO_free(certText);
        BIO* keyText = BIO_new_mem_buf(const_cast<char*>(kPhoneKey), -1);
        EVP_PKEY* key = PEM_read_bio_PrivateKey(keyText, nullptr, nullptr, nullptr);
        BIO_free(keyText);
        if (!ctx_ || !cert || !key || SSL_CTX_use_certificate(ctx_, cert) != 1 ||
            SSL_CTX_use_PrivateKey(ctx_, key) != 1) {
            fprintf(stderr, "FAIL phone TLS identity\n");
            exit(1);
        }
        X509_free(cert);
        EVP_PKEY_free(key);
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, acceptAnyCertificate);  // asks for the car's certificate, as a phone
        ssl_ = SSL_new(ctx_);
        in_ = BIO_new(BIO_s_mem());
        out_ = BIO_new(BIO_s_mem());
        SSL_set_bio(ssl_, in_, out_);
        SSL_set_accept_state(ssl_);
    }
    ~PhoneTls() {
        SSL_free(ssl_);
        SSL_CTX_free(ctx_);
    }
    Bytes step(const Bytes& records) {
        BIO_write(in_, &records[0], static_cast<int>(records.size()));
        SSL_do_handshake(ssl_);
        return pending();
    }
    Bytes seal(const Bytes& plain) {
        SSL_write(ssl_, &plain[0], static_cast<int>(plain.size()));
        return pending();
    }
    bool open(const Bytes& records, Bytes& plain) {
        BIO_write(in_, &records[0], static_cast<int>(records.size()));
        plain.clear();
        uint8_t buf[4096];
        int n;
        while ((n = SSL_read(ssl_, buf, sizeof(buf))) > 0) {
            plain.insert(plain.end(), buf, buf + n);
        }
        return !plain.empty();
    }

private:
    Bytes pending() {
        Bytes out;
        uint8_t buf[4096];
        int n;
        while ((n = BIO_read(out_, buf, sizeof(buf))) > 0) {
            out.insert(out.end(), buf, buf + n);
        }
        return out;
    }
    SSL_CTX* ctx_;
    SSL* ssl_;
    BIO* in_;
    BIO* out_;
};

class Phone {
public:
    Phone(int fd, std::vector<std::string>& log) : fd_(fd), log_(log), quiet_(false), pings_(0), lost_(false) {}

    // A whole message; a flag with 0x08 encrypts it.
    void send(uint8_t channel, uint8_t flags, const Bytes& message) {
        if (!quiet_) {
            log_.push_back(format("ph ch=%u fl=%02x ", channel, flags) + describe(channel, message, false));
        }
        frame(channel, flags, (flags & 0x08) ? tls_.seal(message) : message, nullptr);
    }
    // One frame of a fragmented message, as is: 0x01 FIRST (announcing total), 0x00 MIDDLE, 0x02 LAST, 0x08 encrypted.
    void piece(uint8_t channel, uint8_t flags, const Bytes& data, uint32_t total = 0) {
        if (!quiet_) {
            const std::string shown = data.size() <= 64 ? hex(data) : format("<%u bytes>", unsigned(data.size()));
            log_.push_back(format("ph ch=%u fl=%02x piece ", channel, flags) + shown);
        }
        const bool first = (flags & 0x03) == 0x01;
        frame(channel, flags, (flags & 0x08) ? tls_.seal(data) : data, first ? &total : nullptr);
    }
    void raw(const char* what, const Bytes& data) {
        log_.push_back(std::string("ph raw ") + what + " " + hex(data));
        writeAll(data);
    }
    // A ping, then every head unit frame up to its answer: whatever was sent or posted before has been handled.
    void settle() {
        if (lost_) {
            return;
        }
        ++pings_;
        const Bytes ping = bytes({0x00, 0x0b, 0x08, 0x80 | (pings_ & 0x7f), 0x7f});
        send(0, kPlain, ping);
        const Bytes answer = bytes({0x00, 0x0c, 0x08, 0x80 | (pings_ & 0x7f), 0x7f});
        uint8_t channel = 0, flags = 0;
        Bytes message;
        do {
            if (receive(channel, flags, message) <= 0) {
                return;
            }
        } while (channel != 0 || message != answer);
    }
    // Version exchange, then TLS as the server, up to the head unit's AuthComplete.
    void handshake() {
        uint8_t channel = 0, flags = 0;
        Bytes message;
        receive(channel, flags, message);  // VersionRequest
        send(0, kPlain, bytes({0x00, 0x02, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00}));
        for (int flight = 0; flight < 2 && !lost_; ++flight) {
            if (receive(channel, flags, message) <= 0 || message.size() < 3) {
                return;
            }
            send(0, kPlain, bytes({0x00, 0x03}) + tls_.step(Bytes(message.begin() + 2, message.end())));
        }
        receive(channel, flags, message);  // AuthComplete
    }
    void discover() {
        send(0, kSealed, bytes({0x00, 0x05, 0x22, 0x05}) + text("Phone"));
        settle();
    }
    // The phone shuts its side, then records what the head unit still sends until it closes.
    void leave() {
        log_.push_back("ph shutdown");
        shutdown(fd_, SHUT_WR);
        untilClosed();
    }
    // Records what the head unit still sends until it closes the link.
    void untilClosed() {
        uint8_t channel = 0, flags = 0;
        Bytes message;
        while (!lost_ && receive(channel, flags, message) > 0) {
        }
    }
    void setQuiet(bool quiet) { quiet_ = quiet; }

private:
    void frame(uint8_t channel, uint8_t flags, const Bytes& payload, const uint32_t* total) {
        Bytes f =
            bytes({channel, flags, static_cast<int>(payload.size() >> 8), static_cast<int>(payload.size() & 255)});
        if (total) {
            f = f + bytes({static_cast<int>(*total >> 24), static_cast<int>((*total >> 16) & 255),
                           static_cast<int>((*total >> 8) & 255), static_cast<int>(*total & 255)});
        }
        writeAll(f + payload);
    }
    void writeAll(const Bytes& data) {
        size_t done = 0;
        while (done < data.size()) {
            const ssize_t n = write(fd_, &data[done], data.size() - done);
            if (n <= 0) {
                log_.push_back("ph <write failed>");
                return;
            }
            done += static_cast<size_t>(n);
        }
    }
    // 1 complete, 0 the head unit closed, -1 nothing within kWaitMs.
    int readExact(uint8_t* p, size_t n) {
        size_t done = 0;
        while (done < n) {
            pollfd pfd = {fd_, POLLIN, 0};
            if (poll(&pfd, 1, kWaitMs) <= 0) {
                return -1;
            }
            const ssize_t got = read(fd_, p + done, n - done);
            if (got <= 0) {
                return 0;
            }
            done += static_cast<size_t>(got);
        }
        return 1;
    }
    int receive(uint8_t& channel, uint8_t& flags, Bytes& message) {
        uint8_t header[8];
        int r = readExact(header, 4);
        if (r > 0) {
            channel = header[0];
            flags = header[1];
            if ((flags & 0x03) == 0x01) {
                r = readExact(header + 4, 4);
            }
        }
        Bytes payload(r > 0 ? (header[2] << 8) | header[3] : 0);
        if (r > 0 && !payload.empty()) {
            r = readExact(&payload[0], payload.size());
        }
        if (r <= 0) {
            log_.push_back(r == 0 ? "hu <closed>" : "hu <silent>");
            lost_ = true;
            return r;
        }
        message = payload;
        if ((flags & 0x08) && !tls_.open(payload, message)) {
            log_.push_back(format("hu ch=%u fl=%02x <undecryptable>", channel, flags));
            return 1;
        }
        log_.push_back(format("hu ch=%u fl=%02x ", channel, flags) + describe(channel, message, true));
        return 1;
    }

    int fd_;
    std::vector<std::string>& log_;
    PhoneTls tls_;
    bool quiet_;
    int pings_;
    bool lost_;  // the head unit closed or went silent: later steps record nothing more
};

// One session on a fresh socketpair, the phone end for the test.
struct Link {
    aa::AASession session;
    int phoneFd;
    explicit Link(bool touch = true) : phoneFd(-1) {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            fprintf(stderr, "FAIL socketpair\n");
            exit(1);
        }
        watch(session, touch);
        phoneFd = fds[1];
        if (!session.start(fds[0])) {
            fprintf(stderr, "FAIL session start\n");
            exit(1);
        }
    }
    ~Link() {
        session.stop();
        close(phoneFd);
    }
};

void ended(Link& link, std::vector<std::string>& log) {
    log.push_back(format("session ended=%d", waitEnded(link.session) ? 1 : 0));
}

aa::TouchEventOut touch(int action, int index, std::initializer_list<int> points) {
    aa::TouchEventOut e;
    memset(&e, 0, sizeof(e));
    e.action = action;
    e.index = index;
    e.n = 0;
    for (size_t i = 0; i + 2 < points.size(); i += 3) {
        const int* point = points.begin() + i;
        e.p[e.n].x = point[0];
        e.p[e.n].y = point[1];
        e.p[e.n].id = point[2];
        ++e.n;
    }
    return e;
}

// A video message on channel 6 whose callback holds the session thread while the car posts input.
void pauseOnVideo(Phone& phone, std::vector<std::string>& log, const char* what) {
    log.push_back(std::string("-- held: ") + what);
    g_pause.arm();
    phone.send(6, kSealed, bytes({0x00, 0x00, 0, 0, 0, 0, 0, 0, 0x10, 0x00, 0x00, 0x00, 0x00, 0x01, 0x41}));
    if (!g_pause.waitInside()) {
        fprintf(stderr, "FAIL the session thread never reached the video callback\n");
        exit(1);
    }
}

void resume(Phone& phone) {
    g_pause.release();
    phone.settle();
}

// ---- scenarios ----------------------------------------------------------------------------------------------------

// What a phone does in a normal drive, then ByeBye.
void fullSession(std::vector<std::string>& log) {
    Link link;
    aa::AASession& s = link.session;
    Phone phone(link.phoneFd, log);
    phone.handshake();
    phone.discover();

    log.push_back("-- car key before the input channel is bound, car focus before the video setup");
    s.postKeyEvent(kKeyHome, true);
    s.postVideoFocus(false);
    phone.settle();

    for (int channel = 1; channel <= 14; ++channel) {
        phone.send(static_cast<uint8_t>(channel), kSealedCtl, bytes({0x00, 0x07, 0x08, 0x00, 0x10, channel}));
    }
    phone.settle();

    log.push_back("-- audio and video setup, start indications");
    phone.send(6, kSealed, bytes({0x80, 0x00, 0x08, 0x03}));
    phone.send(8, kSealed, bytes({0x80, 0x00, 0x08, 0x01}));
    phone.send(2, kSealed, bytes({0x80, 0x00, 0x08, 0x03}));
    phone.send(3, kSealed, bytes({0x80, 0x00}));
    phone.send(14, kSealed, bytes({0x80, 0x00, 0x08, 0x01}));
    phone.send(6, kSealed, bytes({0x80, 0x01, 0x08, 0x05, 0x10, 0x00}));
    phone.send(8, kSealed, bytes({0x80, 0x01, 0x08, 0x07}));
    phone.send(2, kSealed, bytes({0x80, 0x01, 0x10, 0x00}));
    phone.send(4, kSealed, bytes({0x80, 0x01, 0x08, 0xa1, 0x02}));
    phone.settle();

    log.push_back("-- media: codec, video, fragmented video, cut-short video, audio");
    phone.send(6, kSealed, bytes({0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x00, 0x00, 0x01, 0x68}));
    phone.send(6, kSealed, bytes({0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06}) + counting(24, 0x10));
    const Bytes big = bytes({0x00, 0x00, 0, 0, 0, 0, 0, 0, 0x77, 0x77}) + counting(300, 0x20);
    const Bytes a(big.begin(), big.begin() + 100), b(big.begin() + 100, big.begin() + 200),
        c(big.begin() + 200, big.end());
    phone.piece(6, 0x09, a, static_cast<uint32_t>(big.size()));
    phone.piece(6, 0x08, b);
    phone.piece(6, 0x0a, c);
    phone.send(6, kSealed, bytes({0x00, 0x00, 0x01, 0x02}));
    phone.send(6, kSealed, bytes({0x00, 0x01}));
    phone.send(8, kSealed, bytes({0x00, 0x00, 0, 0, 0, 0, 0, 0, 0x30, 0x39}) + counting(32, 0x40));
    phone.send(8, kSealed, bytes({0x00, 0x00, 0x01}));
    phone.send(9, kSealed, bytes({0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 1}) + counting(4, 0));
    phone.send(4, kSealed, bytes({0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 1, 0x65}));
    phone.send(1, kSealed, bytes({0x00, 0x01, 0x02}));
    phone.send(8, kSealed, bytes({0x80, 0x02}));
    phone.send(9, kSealed, bytes({0x80, 0x02}));
    phone.send(2, kSealed, bytes({0x80, 0x02}));
    phone.settle();

    log.push_back("-- video focus asked by the phone");
    phone.send(6, kSealed, bytes({0x80, 0x07, 0x08, 0x00, 0x10, 0x02, 0x18, 0x01}));
    phone.send(6, kSealed, bytes({0x80, 0x07, 0x10, 0x01, 0x18, 0x02}));
    phone.send(6, kSealed, bytes({0x80, 0x07}));
    phone.send(6, kSealed, bytes({0x80, 0x08, 0x08, 0x01}));
    phone.send(2, kSealed, bytes({0x80, 0x07, 0x10, 0x02}));
    phone.settle();

    log.push_back("-- input bindings; the next drain also sends the focus kept from before the video setup");
    phone.send(3, kSealed, bytes({0x80, 0x02, 0x0a, 0x01, 0x03}));
    phone.send(5, kSealed, bytes({0x80, 0x02}));
    phone.send(7, kSealed, bytes({0x80, 0x02, 0x0a, 0x03, 0x03, 0x04, 0x13}));
    phone.settle();
    s.postKeyEvent(kKeyHome, true);
    phone.settle();
    s.postKeyEvent(kKeyHome, false);
    s.postRelativeEvent(kRotary, -1);
    phone.settle();
    s.postTouchEvent(touch(0, 0, {100, 200, 0}));
    phone.settle();

    pauseOnVideo(phone, log, "keys, rotary steps, touches, focus");
    s.postRelativeEvent(kRotary, 1);
    s.postRelativeEvent(kRotary, 2);
    s.postKeyEvent(kKeyBack, true);
    s.postRelativeEvent(kRotary, 1);
    s.postRelativeEvent(kRotary + 1, 1);
    s.postTouchEvent(touch(2, 0, {110, 200, 0}));
    s.postTouchEvent(touch(2, 0, {120, 210, 0}));
    s.postTouchEvent(touch(5, 1, {120, 210, 0, 600, 300, 1}));
    s.postTouchEvent(touch(2, 1, {121, 211, 0, 601, 301, 1}));
    s.postTouchEvent(touch(6, 1, {121, 211, 0, -5, 470, 1}));
    s.postTouchEvent(touch(1, 0, {121, 211, 0}));
    s.postTouchEvent(touch(1, 0, {}));
    s.postVideoFocus(true);
    s.postVideoFocus(false);
    resume(phone);

    pauseOnVideo(phone, log, "70 keys: 64 kept");
    for (int i = 0; i < 70; ++i) {
        s.postKeyEvent(kKeyUp, true);
    }
    resume(phone);

    pauseOnVideo(phone, log, "300 touches: 256 kept");
    for (int i = 0; i < 300; ++i) {
        s.postTouchEvent(touch(0, 0, {3, 7, 0}));
    }
    resume(phone);

    log.push_back("-- sensors: driving status, night mode, unknown and malformed requests");
    s.postNightMode(true);
    phone.send(1, kSealed, bytes({0x80, 0x01, 0x08, 0x0d, 0x10, 0x00}));
    phone.send(1, kSealed, bytes({0x80, 0x01, 0x08, 0x0a}));
    phone.settle();
    s.postNightMode(false);
    phone.settle();
    s.postNightMode(false);
    phone.send(1, kSealed, bytes({0x80, 0x01, 0x08, 0x63}));
    phone.send(1, kSealed, bytes({0x80, 0x01, 0x10, 0x05}));
    phone.send(1, kSealed, bytes({0x80, 0x01, 0x08}));
    phone.send(1, kSealed, bytes({0x80, 0x01, 0x08, 0x8a, 0x80, 0x80, 0x80, 0x10}));  // 10 past 32 bits
    phone.send(1, kSealed, bytes({0x80, 0x01, 0x08, 0x0a}));
    phone.send(1, kSealed, bytes({0x80, 0x02, 0x08, 0x00}));
    phone.settle();
    pauseOnVideo(phone, log, "night, day, night");
    s.postNightMode(true);
    s.postNightMode(false);
    s.postNightMode(true);
    resume(phone);

    log.push_back("-- control channel");
    phone.send(0, kSealed, bytes({0x00, 0x12, 0x08, 0x01}));
    phone.send(0, kSealed, bytes({0x00, 0x12, 0x08, 0x04}));
    phone.send(0, kSealed, bytes({0x00, 0x12, 0x08, 0x03}));
    phone.send(0, kSealed, bytes({0x00, 0x12, 0x08, 0x02}));
    phone.send(0, kSealed, bytes({0x00, 0x12}));
    phone.send(0, kSealed, bytes({0x00, 0x0d, 0x08, 0x02}));
    phone.send(0, kSealed, bytes({0x00, 0x0d}));
    phone.send(0, kSealed, bytes({0x00, 0x11, 0x08, 0x01}));
    phone.send(0, kSealed, bytes({0x00, 0x11, 0x08, 0x02}));
    phone.send(0, kSealed, bytes({0x00, 0x99, 0x08, 0x01}));
    phone.send(0, kSealed, bytes({0x00, 0x0b, 0x08, 0x96, 0x01, 0x10, 0x00}));
    phone.send(0, kSealed, bytes({0x00, 0x0b, 0x10, 0x01}));
    phone.send(0, kSealed, bytes({0x00, 0x0b, 0x08}));
    phone.send(0, kSealed, bytes({0x00, 0x0b, 0x08, 0x80, 0x00}));
    phone.send(0, kSealed, bytes({0x00, 0x0b, 0x08}) + Bytes(15, 0xff));
    phone.send(0, kSealed, bytes({0x00, 0x0b, 0x08, 0xff, 0xff}));
    phone.send(0, kSealed, bytes({0x00, 0x0c, 0x08, 0x01}));
    phone.send(200, kSealed, bytes({0x12, 0x34}));
    phone.send(0, kSealed, bytes({0x00, 0x07, 0x08, 0x00}));
    phone.settle();

    log.push_back("-- microphone");
    s.postMicData(&counting(32, 0)[0], 32);
    phone.send(14, kSealed, bytes({0x80, 0x05, 0x08, 0x01, 0x20, 0x05}));
    phone.settle();
    s.postMicData(&counting(160, 0x50)[0], 160);
    phone.settle();
    pauseOnVideo(phone, log, "microphone chunks, empty and oversized ones refused");
    s.postMicData(&counting(16, 1)[0], 16);
    s.postMicData(&counting(16, 2)[0], 16);
    s.postMicData(nullptr, 16);
    s.postMicData(&counting(16, 3)[0], 0);
    s.postMicData(&Bytes(8193, 7)[0], 8193);
    s.postMicData(&Bytes(8192, 8)[0], 8192);
    s.postMicData(&counting(16, 4)[0], 16);
    resume(phone);
    pauseOnVideo(phone, log, "70 microphone chunks: 64 kept");
    for (int i = 0; i < 70; ++i) {
        s.postMicData(&counting(2, 0x11)[0], 2);
    }
    resume(phone);
    phone.send(14, kSealed, bytes({0x80, 0x04, 0x08, 0x00, 0x10, 0x01}));
    phone.send(14, kSealed, bytes({0x80, 0x05, 0x08, 0x00}));
    phone.settle();
    s.postMicData(&counting(16, 5)[0], 16);
    phone.send(14, kSealed, bytes({0x80, 0x05, 0x08, 0x01}));
    phone.settle();

    pauseOnVideo(phone, log, "one of everything the car queues");
    s.postTouchEvent(touch(0, 0, {5, 6, 2}));
    s.postKeyEvent(kKeyBack, false);
    s.postVideoFocus(true);
    s.postNightMode(false);
    s.postMicData(&counting(8, 9)[0], 8);
    resume(phone);
    phone.send(14, kSealed, bytes({0x80, 0x05, 0x08, 0x00}));
    phone.settle();

    log.push_back("-- navigation and media status");
    phone.send(10, kSealed, bytes({0x80, 0x03, 0x08, 0x01}));
    phone.send(10, kSealed, bytes({0x80, 0x06, 0x0a, 0x02, 0x08, 0x05}));
    phone.send(10, kSealed, bytes({0x80, 0x07, 0x08, 0x02}));
    phone.send(10, kSealed, bytes({0x80, 0x00}));
    phone.send(10, kSealed, bytes({0x80, 0x02}));
    phone.send(10, kSealed, bytes({0x80, 0x03, 0x08, 0x02}));
    phone.send(10, kSealed, bytes({0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 1}));
    phone.send(12, kSealed, bytes({0x80, 0x01, 0x08, 0x02}));
    phone.send(12, kSealed, bytes({0x80, 0x01, 0x08, 0x02}));
    phone.send(12, kSealed, bytes({0x80, 0x01, 0x08, 0x01}));
    phone.send(12, kSealed, bytes({0x80, 0x03, 0x0a, 0x00}));
    phone.send(12, kSealed, bytes({0x80, 0x00}));
    phone.settle();

    log.push_back("-- the car takes the screen back and gives it again");
    s.postVideoFocus(false);
    phone.settle();
    s.postVideoFocus(true);
    phone.settle();

    log.push_back("-- location last: a fix stays fresh 5 s");
    phone.send(1, kSealed, bytes({0x80, 0x01, 0x08, 0x01, 0x10, 0xe0, 0xd4, 0x03}));
    phone.settle();
    aa::CarLocation fix;
    fix.valid = true;
    fix.latitudeE6 = 48123456;
    fix.longitudeE6 = -2123456;
    fix.sampledMs = aa::locationClockMs();
    fix.hasSpeed = fix.hasBearing = fix.hasAltitude = fix.hasAccuracy = true;
    fix.speedMps = 13.5;
    fix.bearingDeg = 271.25;
    fix.altitudeM = -12.3;
    fix.accuracyM = 4.5;
    fix.utcSeconds = 1793750000;
    s.postCarLocation(fix);
    phone.settle();
    s.postCarLocation(fix);
    phone.settle();

    log.push_back("-- ByeBye");
    phone.send(0, kSealed, bytes({0x00, 0x0f, 0x08, 0x01}));
    phone.untilClosed();
    ended(link, log);
}

// Fragments in odd orders, short messages, a frame that does not decrypt, then the phone leaves.
void fragmentEdges(std::vector<std::string>& log) {
    Link link;
    Phone phone(link.phoneFd, log);
    phone.handshake();
    phone.send(6, kSealed, bytes({0x80, 0x00}));
    phone.send(6, kSealed, bytes({0x80, 0x01, 0x08, 0x03}));
    phone.settle();
    const Bytes video = bytes({0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0x2a}) + counting(20, 0x60);
    const Bytes head(video.begin(), video.begin() + 12), tail(video.begin() + 12, video.end());
    log.push_back("-- LAST without FIRST");
    phone.piece(6, 0x0a, video);
    phone.settle();
    log.push_back("-- MIDDLE then LAST without FIRST");
    phone.piece(6, 0x08, head);
    phone.piece(6, 0x0a, tail);
    phone.settle();
    log.push_back("-- FIRST announcing more than came");
    phone.piece(6, 0x09, head, 10000);
    phone.piece(6, 0x0a, tail);
    phone.settle();
    log.push_back("-- FIRST announcing 0");
    phone.piece(6, 0x09, head, 0);
    phone.piece(6, 0x0a, tail);
    phone.settle();
    log.push_back("-- a whole message between FIRST and LAST");
    phone.piece(6, 0x09, head, static_cast<uint32_t>(video.size()));
    phone.send(6, kSealed, bytes({0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x99}));
    phone.piece(6, 0x0a, tail);
    phone.settle();
    log.push_back("-- a new FIRST replaces an unfinished one; fragments of two channels interleaved");
    phone.piece(6, 0x09, bytes({0x01, 0x02, 0x03}), 3);
    phone.piece(6, 0x09, head, static_cast<uint32_t>(video.size()));
    phone.piece(8, 0x09, bytes({0x00, 0x00, 0, 0, 0}), 12);
    phone.piece(6, 0x0a, tail);
    phone.piece(8, 0x0a, bytes({0, 0, 0, 0, 0, 0x55, 0x66}));
    phone.settle();
    log.push_back("-- empty frames are skipped");
    phone.raw("FIRST of 0 bytes", bytes({0x06, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10}));
    phone.raw("whole of 0 bytes", bytes({0x00, 0x03, 0x00, 0x00}));
    phone.settle();
    log.push_back("-- messages shorter than an id");
    phone.send(0, kSealed, bytes({0x05}));
    phone.send(6, kSealed, bytes({0x05}));
    phone.send(8, kSealed, bytes({0x05}));
    phone.settle();
    log.push_back("-- a second version response restarts TLS");
    phone.send(0, kPlain, bytes({0x00, 0x02, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00}));
    phone.settle();
    log.push_back("-- garbage where an encrypted frame should be");
    phone.raw("sealed garbage", bytes({0x06, 0x0b, 0x00, 0x08, 1, 2, 3, 4, 5, 6, 7, 8}));
    phone.settle();
    phone.leave();
    ended(link, log);
}

// Without TLS: what still works, an overflowing reassembly, a frame cut short.
void plainEdges(std::vector<std::string>& log) {
    {
        Link link;
        Phone phone(link.phoneFd, log);
        phone.settle();
        phone.send(0, kPlain, bytes({0x00, 0x02, 0x00, 0x01}));
        phone.send(0, kPlain, bytes({0x00, 0x03}));
        phone.send(0, kPlain, bytes({0x00, 0x03, 0x16}));
        phone.send(0, kPlain, bytes({0x00, 0x05}));
        phone.send(0, kPlain, bytes({0x00, 0x12, 0x08, 0x01}));
        phone.send(1, kPlain, bytes({0x80, 0x01, 0x08, 0x0d}));
        phone.send(6, kPlain, bytes({0x80, 0x00}));
        phone.send(6, kPlain, bytes({0x80, 0x07, 0x10, 0x02}));
        phone.send(7, kPlain, bytes({0x80, 0x02}));
        phone.send(14, kPlain, bytes({0x80, 0x05, 0x08, 0x01}));
        phone.send(6, kPlain, bytes({0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x67}));
        phone.send(6, kPlain, bytes({0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0x07, 0x65}));
        phone.send(10, kPlain, bytes({0x80, 0x03, 0x08, 0x01}));
        phone.settle();
        link.session.postKeyEvent(kKeyHome, true);
        link.session.postVideoFocus(true);
        link.session.postMicData(&counting(4, 0)[0], 4);
        phone.settle();
        phone.send(0, kPlain, bytes({0x00, 0x0f}));
        phone.untilClosed();
        ended(link, log);
    }
    log.push_back("-- reassembly past 8 MiB");
    Link link;
    Phone phone(link.phoneFd, log);
    phone.settle();
    const Bytes chunk(65535, 0);
    phone.piece(6, 0x01, chunk, 9000000);
    log.push_back("ph 128 MIDDLE pieces of 65535 bytes");
    phone.setQuiet(true);
    for (int i = 0; i < 128; ++i) {
        phone.piece(6, 0x00, chunk);
    }
    phone.setQuiet(false);
    phone.piece(6, 0x02, bytes({0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x01}));
    phone.settle();
    log.push_back("-- a payload cut short");
    phone.raw("header of 100 bytes, 10 sent", bytes({0x06, 0x03, 0x00, 0x64}) + Bytes(10, 0));
    phone.leave();
    ended(link, log);
}

void cutHeaders(std::vector<std::string>& log) {
    {
        Link link;
        Phone phone(link.phoneFd, log);
        phone.settle();
        phone.raw("half a header", bytes({0x00, 0x0b}));
        phone.leave();
        ended(link, log);
    }
    Link link;
    Phone phone(link.phoneFd, log);
    phone.settle();
    phone.raw("FIRST header without its total", bytes({0x06, 0x09, 0x00, 0x10, 0x00, 0x00}));
    phone.leave();
    ended(link, log);
}

// The car side: a second start while a session runs, stop(), a phone already gone, touch forwarding off.
void lifecycle(std::vector<std::string>& log) {
    int a[2], b[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, a) != 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, b) != 0) {
        fprintf(stderr, "FAIL socketpair\n");
        exit(1);
    }
    {
        aa::AASession session;
        watch(session, true);
        session.start(a[0]);
        Phone first(a[1], log);
        first.handshake();
        log.push_back("-- start again while the session runs");
        session.start(b[0]);
        first.untilClosed();
        Phone second(b[1], log);
        second.handshake();
        second.settle();
        log.push_back("-- stop by the car");
        session.stop();
        second.untilClosed();
        log.push_back(format("session active=%d", session.isActive() ? 1 : 0));
    }
    close(a[1]);
    close(b[1]);
    log.push_back("-- the phone is gone before the first frame");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, a) != 0) {
        fprintf(stderr, "FAIL socketpair\n");
        exit(1);
    }
    close(a[1]);
    {
        aa::AASession session;
        watch(session, true);
        session.start(a[0]);
        log.push_back(format("session ended=%d", waitEnded(session) ? 1 : 0));
    }
    log.push_back("-- touch forwarding off: keys still go");
    Link link(false);
    Phone phone(link.phoneFd, log);
    phone.handshake();
    phone.send(7, kSealed, bytes({0x80, 0x02}));
    phone.settle();
    link.session.postTouchEvent(touch(0, 0, {1, 2, 0}));
    link.session.postKeyEvent(kKeyHome, true);
    phone.settle();
    phone.leave();
    ended(link, log);
}

struct Scenario {
    const char* name;
    void (*run)(std::vector<std::string>&);
};

const Scenario kScenarios[] = {{"full session", fullSession},
                               {"fragment edges", fragmentEdges},
                               {"plain edges", plainEdges},
                               {"cut headers", cutHeaders},
                               {"lifecycle", lifecycle}};

void timedOut(int) {
    static const char msg[] = "FAIL test-session-phone: no progress\n";
    ssize_t w = write(2, msg, sizeof(msg) - 1);
    (void)w;
    _exit(1);
}

// Identical consecutive lines as one, followed by their count.
std::vector<std::string> collapse(const std::vector<std::string>& lines) {
    std::vector<std::string> out;
    for (size_t i = 0; i < lines.size();) {
        size_t same = 1;
        while (i + same < lines.size() && lines[i + same] == lines[i]) {
            ++same;
        }
        out.push_back(same > 1 ? lines[i] + format(" (x%u)", static_cast<unsigned>(same)) : lines[i]);
        i += same;
    }
    return out;
}

bool readLines(const char* path, std::vector<std::string>& lines) {
    FILE* f = fopen(path, "r");
    if (!f) {
        return false;
    }
    std::string line;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line += static_cast<char>(c);
        }
    }
    fclose(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const bool writing = argc == 3 && !strcmp(argv[1], "--write");
    const char* golden = writing ? argv[2] : (argc == 2 ? argv[1] : "tests/fixtures/session_phone.golden");
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, timedOut);
    writeVehicleFile();
    alarm(300);
    SSL_library_init();
    SSL_load_error_strings();
    // The session's own log would bury the verdict: it goes to /dev/null while the scenarios run.
    fflush(stdout);
    const int console = dup(1);
    const int sink = open("/dev/null", O_WRONLY);
    if (console < 0 || sink < 0 || dup2(sink, 1) < 0) {
        return 1;
    }
    std::vector<std::string> out;
    for (size_t i = 0; i < sizeof(kScenarios) / sizeof(kScenarios[0]); ++i) {
        out.push_back(std::string("== ") + kScenarios[i].name);
        kScenarios[i].run(out);
        const std::vector<std::string> events = takeEvents();
        for (size_t k = 0; k < events.size(); ++k) {
            out.push_back("cb " + events[k]);
        }
    }
    fflush(stdout);
    dup2(console, 1);
    close(sink);
    out = collapse(out);
    if (writing) {
        FILE* f = fopen(golden, "w");
        for (size_t i = 0; f && i < out.size(); ++i) {
            fprintf(f, "%s\n", out[i].c_str());
        }
        if (!f || fclose(f) != 0) {
            printf("FAIL cannot write %s\n", golden);
            return 1;
        }
        printf("WROTE %s (%u lines)\n", golden, static_cast<unsigned>(out.size()));
        return 0;
    }
    std::vector<std::string> want;
    if (!readLines(golden, want)) {
        printf("FAIL cannot read %s\n", golden);
        return 1;
    }
    for (size_t i = 0; i < out.size() || i < want.size(); ++i) {
        const std::string got = i < out.size() ? out[i] : "<missing>";
        const std::string expected = i < want.size() ? want[i] : "<missing>";
        if (got != expected) {
            printf("FAIL session, golden line %u\n  got:  %s\n  want: %s\n", static_cast<unsigned>(i + 1), got.c_str(),
                   expected.c_str());
            return 1;
        }
    }
    printf(
        "PASS session with a fake phone: %u lines as the golden (TLS handshake, discovery, every channel, car input "
        "queues, odd fragments, cut frames, start/stop)\n",
        static_cast<unsigned>(out.size()));
    return 0;
}
