// SPDX-License-Identifier: GPL-3.0-or-later
// Host-only bus double. Python injects native messages through a private socket.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
struct Message {
    uint16_t address;
    uint8_t block, instance;
    uint16_t function, reserved;
    uint32_t operation;
    uint16_t size, reserved2;
    const uint8_t* data;
    uint32_t channel;
};
static int fd = -1;
static void (*callback)(void*, void*);
static void* context;
extern "C" int vmostDisconnect() {
    if (fd >= 0) {
        close(fd);
    }
    fd = -1;
    unlink(getenv("TELEMETRY_FAKE_BUS"));
    return 0;
}
extern "C" int vmostConnect(void (*cb)(void*, void*), void* ctx, int) {
    callback = cb;
    context = ctx;
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un a = {};
    a.sun_family = AF_UNIX;
    const char* path = getenv("TELEMETRY_FAKE_BUS");
    if (!path || strlen(path) >= sizeof(a.sun_path)) {
        return -1;
    }
    strcpy(a.sun_path, path);
    unlink(path);
    return bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
}
extern "C" int vmostFblockShadowRegister(uint8_t block, uint8_t instance) {
    return block == 6 && instance == 0xf0 ? 0 : -1;
}
extern "C" int vmostFileDescriptorGet(int* out) {
    *out = fd;
    return 0;
}
extern "C" int vmostMostMsgSend(void* raw) {
    const Message* m = static_cast<Message*>(raw);
    if (m->block != 6 || m->instance != 0xf0 || m->function != 0x50 || m->operation != 0 || m->size > 12) {
        return -1;
    }
    FILE* f = fopen(getenv("TELEMETRY_FAKE_LOG"), "ab");
    if (!f) {
        return -1;
    }
    fputc(m->size, f);
    fwrite(m->data, 1, m->size, f);
    fclose(f);
    return 0;
}
extern "C" int vmostProcessMessages() {
    uint8_t p[128];
    const ssize_t n = recv(fd, p, sizeof(p), 0);
    if (n < 10) {
        return 0;
    }
    Message m = {0x100,
                 p[0],
                 p[1],
                 uint16_t(p[2] | unsigned(p[3]) << 8),
                 0,
                 unsigned(p[4]) | unsigned(p[5]) << 8 | unsigned(p[6]) << 16 | unsigned(p[7]) << 24,
                 uint16_t(p[8] | unsigned(p[9]) << 8),
                 0,
                 p + 10,
                 0};
    if (m.size == n - 10) {
        callback(&m, context);
    }
    return 1;  // Pump's return value is not an error contract.
}
