// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Starting, reading, telling and stopping the service from the iDrive app (service_link.hpp).
#include "service_link.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../platform/service_contract.hpp"
#include "app_log.hpp"
#include "log_profile.hpp"
#include "spawn_attr.hpp"

namespace evo {
namespace app {

namespace {

time_t g_lastLaunch = 0;
pid_t g_launchPid = 0;
pid_t g_stopPid = 0;
time_t g_stopTime = 0;
int g_stopStage = 0;

bool pidAlive(int pid) {
    if (pid <= 0) {
        return false;
    }
    if (kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

void* reaper(void* p) {
    pid_t pid = static_cast<pid_t>(reinterpret_cast<intptr_t>(p));
    int status = 0;
    pid_t r;
    do {
        r = waitpid(pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    if (r == pid) {
        alog("service supervisor pid %d ended (%s %d)%s", static_cast<int>(pid), WIFEXITED(status) ? "exit" : "signal",
             WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status),
             WIFEXITED(status) && WEXITSTATUS(status) == 127 ? " : 127 = start refused before/at exec" : "");
        __sync_bool_compare_and_swap(&g_launchPid, pid, 0);  // a reused pid must not look like a starting service
    }
    return nullptr;
}

void loopbackControlPort(struct sockaddr_in* a) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons(contract::kControlPort);
    a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

bool controlPortBusy() {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    struct sockaddr_in a;
    loopbackControlPort(&a);
    bool busy = false;
    // Connected: with no service on the port the kernel answers "refused" at once, instead of a 200 ms silence on every
    // status poll of the app's thread.
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a)) == 0 && send(fd, "ping", 4, 0) == 4) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        if (select(fd + 1, &rf, nullptr, nullptr, &tv) > 0) {
            char buf[8];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            busy = (n >= 4 && memcmp(buf, "pong", 4) == 0);
        }
    }
    close(fd);
    return busy;
}

}  // namespace

// ---- launching the service ------------------------------------------------------------------------------------------
void launchService() {
    time_t now = time(nullptr);
    if (g_lastLaunch && now - g_lastLaunch < 3) {
        alog("launch ignored (pressed again within 3 s)");
        return;
    }
    g_lastLaunch = now;
    struct stat sst;
    if (stat(contract::kSupervisor, &sst) != 0) {
        alog("launch: %s not found", contract::kSupervisor);
        return;
    }
    if (g_launchPid > 0 && pidAlive(g_launchPid)) {
        alog("launch: supervisor pid %d still running", static_cast<int>(g_launchPid));
        return;
    }
    unlink(contract::kServiceStop);  // a STOP left from an earlier run would end the new one

    // The service's whole environment: where its libraries are and how to reach the compositor. No settings.
    static const char* const kServiceEnv[] = {"PATH=/bin:/usr/bin:/sbin:/usr/sbin:/usr/local/bin",
                                              "LD_LIBRARY_PATH=/usr/local/lib:/opt/hmi/lib:/usr/lib:/lib",
                                              "XDG_RUNTIME_DIR=/run/system/0",
                                              "WAYLAND_DISPLAY=wayland-0",
                                              "HOME=/tmp",
                                              nullptr};
    char* const* envp = const_cast<char* const*>(kServiceEnv);

    char* argv[] = {const_cast<char*>(contract::kSupervisor), const_cast<char*>("--service"), nullptr};
    int in = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int out = open(serviceLogPath(contract::kMaintenance, contract::kServiceLog),
                   O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (in < 0 || out < 0) {
        alog("launch: cannot open /dev/null or %s (errno %d)", contract::kServiceLog, errno);
        if (in >= 0) {
            close(in);
        }
        if (out >= 0) {
            close(out);
        }
        return;
    }
    {  // header line so the service log is never empty, even if the child dies before exec
        char head[160];
        int hn = snprintf(head, sizeof(head), "=== %ld: the iDrive app starts %s --service (%s)\n",
                          static_cast<long>(time(nullptr)), contract::kSupervisor, ENTRY_AA_APP_VERSION);
        if (hn > 0) {
            ssize_t w =
                write(out, head, static_cast<size_t>(hn) < sizeof(head) ? static_cast<size_t>(hn) : sizeof(head) - 1);
            (void)w;
        }
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in, 0);
    posix_spawn_file_actions_adddup2(&fa, out, 1);
    posix_spawn_file_actions_adddup2(&fa, out, 2);
    posix_spawnattr_t at;
    int arc = initServiceSpawnAttr(&at);  // spawn_attr.hpp: no sigfillset (glibc 2.15 -> exit 127)
    if (arc != 0) {
        alog("launch: spawn attributes refused (%s)", strerror(arc));
        posix_spawn_file_actions_destroy(&fa);
        close(in);
        close(out);
        return;
    }
    pid_t pid = -1;
    int rc = posix_spawn(&pid, contract::kSupervisor, &fa, &at, argv, envp);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    close(in);
    close(out);
    if (rc != 0) {
        alog("launch: spawn %s failed (%s)", contract::kSupervisor, strerror(rc));
        return;
    }
    g_launchPid = pid;
    alog("launch: started %s --service pid %d (log %s)", contract::kSupervisor, static_cast<int>(pid),
         contract::kServiceLog);
    pthread_t t;
    pthread_attr_t ta;
    pthread_attr_init(&ta);
    pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &ta, reaper, reinterpret_cast<void*>(static_cast<intptr_t>(pid))) != 0) {
        alog("launch: no reaper thread");
    }
    pthread_attr_destroy(&ta);
}

// ---- control messages and state -------------------------------------------------------------------------------------
void sendToService(const char* message) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return;
    }
    struct sockaddr_in a;
    loopbackControlPort(&a);
    sendto(fd, message, strlen(message), 0, reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
    close(fd);
}

ServiceState readServiceState() {
    ServiceState d;
    FILE* f = fopen(contract::kStateFile, "r");
    if (f) {
        char st[24] = "";
        int pid = 0, ov = 0;
        int n = fscanf(f, "%23s %d %d", st, &pid, &ov);
        fclose(f);
        if (n >= 2 && pidAlive(pid)) {
            strcpy(d.state, st);
            d.pid = pid;
            d.overlay = (n >= 3) ? ov : 0;
            return d;
        }
        unlink(contract::kStateFile);  // left by a service that is gone
    }
    if (g_launchPid > 0 && pidAlive(g_launchPid)) {  // supervisor in its preflight: show "connecting"
        strcpy(d.state, "connecting");
        d.pid = -1;
        return d;
    }
    if (controlPortBusy()) {
        strcpy(d.state, "running");
        d.pid = -1;
    }
    return d;
}

// ---- stopping -------------------------------------------------------------------------------------------------------
void beginStop() {
    ServiceState d = readServiceState();
    if (d.pid == 0) {
        alog("stop: service not running");
        return;
    }
    g_stopPid = d.pid > 0 ? static_cast<pid_t>(d.pid) : -1;
    g_stopTime = time(nullptr);
    g_stopStage = 0;
    sendToService("quit");
    int fd = open(contract::kServiceStop, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);  // the supervisor stops its child too
    if (fd >= 0) {
        close(fd);
    }
    alog("stop: sent 'quit' (service pid %d) and STOP to the supervisor", d.pid);
}

void serviceStop() {
    if (!g_stopPid) {
        return;
    }
    bool gone = g_stopPid > 0 ? !pidAlive(static_cast<int>(g_stopPid))
                              : !(g_launchPid > 0 && pidAlive(g_launchPid)) && !controlPortBusy();
    if (gone) {
        alog("service (pid %d) has exited", static_cast<int>(g_stopPid));
        g_stopPid = 0;
        return;
    }
    long waited = static_cast<long>(time(nullptr) - g_stopTime);
    if (g_stopPid < 0) {
        if (waited >= 20) {
            alog("service (pid unknown) still running 20 s after stop: giving up");
            g_stopPid = 0;
        }
        return;
    }
    // Only the session process is signalled; the supervisor always finishes its Bluetooth restore by itself.
    if (g_stopStage == 0 && waited >= 3) {
        alog("service pid %d still running 3 s after 'quit': SIGTERM", static_cast<int>(g_stopPid));
        kill(g_stopPid, SIGTERM);
        g_stopStage = 1;
    } else if (g_stopStage == 1 && waited >= 15) {
        alog("service pid %d still running 15 s after 'quit': SIGKILL", static_cast<int>(g_stopPid));
        kill(g_stopPid, SIGKILL);
        g_stopStage = 2;
    } else if (g_stopStage == 2 && waited >= 20) {
        alog("service pid %d survived SIGKILL; giving up", static_cast<int>(g_stopPid));
        g_stopPid = 0;
    }
}

bool stopInProgress() {
    return g_stopPid != 0;
}

bool supervisorRunning() {
    int fd = open(contract::kWirelessLock, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;  // no supervisor has run since the unit started
    }
    const bool held = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);  // gives the lock back when it was free
    return held;
}

}  // namespace app
}  // namespace evo
