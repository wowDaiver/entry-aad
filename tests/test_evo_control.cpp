// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Control protocol between the iDrive app and the Android Auto service (the real channel, the app's own calls, a
// real state file).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <vector>

#include "control_address.hpp"
#include "evo_control.hpp"
#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            exit(1);                                             \
        }                                                        \
    } while (0)
static std::vector<std::string> seen;
static void onCmd(const evo::ControlCommand& c, void*) {
    char b[48];
    snprintf(b, sizeof(b), "%s%s%d", c.verb.c_str(), c.hasArg ? ":" : "", c.hasArg ? c.arg : 0);
    seen.push_back(c.hasArg ? std::string(b) : c.verb);
}
static long nowMs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
static std::string text(const char* p) {
    std::ifstream f(p);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}
int main() {
    evo::ControlCommand c;
    CHECK(evo::parseControlCommand("gear 2", 6, c) && c.verb == "gear" && c.hasArg && c.arg == 2);
    CHECK(evo::parseControlCommand("show\n", 5, c) && c.verb == "show" && !c.hasArg);
    CHECK(evo::parseControlCommand("await", 5, c) && c.verb == "await" && !c.hasArg);  // a phone waits for "show"
    CHECK(!evo::parseControlCommand("await 1", 7, c));
    CHECK(evo::parseControlCommand("drivemode 0 3 1", 15, c) == false);                 // 3 args: refused
    CHECK(evo::parseControlCommand("call 1", 6, c) && c.verb == "call" && c.arg == 1);  // phone keys during a call
    CHECK(evo::parseControlCommand("call", 4, c) == false);
    CHECK(evo::parseControlCommand("popup 1", 7, c) && c.verb == "popup" && c.arg == 1);  // iDrive popup
    CHECK(evo::parseControlCommand("popup", 5, c) == false);
    CHECK(!evo::parseControlCommand("gear", 4, c) && !evo::parseControlCommand("show 1", 6, c));
    CHECK(!evo::parseControlCommand("reboot", 6, c) && !evo::parseControlCommand("", 0, c));
    CHECK(!evo::parseControlCommand("gear 2 x", 8, c));
    char name[48];
    snprintf(name, sizeof(name), "entry-aad-control-test-%d", (int)getpid());
    char state[] = "/tmp/entry-aad-state-test-XXXXXX";
    int sfd = mkstemp(state);
    CHECK(sfd >= 0);
    close(sfd);
    unlink(state);
    {  // The address: abstract (first byte 0), the name after it, nothing beyond; a name too long is cut.
        sockaddr_un a;
        const socklen_t length = evo::controlAddress("abc", a);
        CHECK(a.sun_family == AF_UNIX && a.sun_path[0] == 0 && !memcmp(a.sun_path + 1, "abc", 3));
        CHECK(length == offsetof(sockaddr_un, sun_path) + 4);
        const std::string huge(400, 'x');
        CHECK(evo::controlAddress(huge.c_str(), a) == sizeof(a));
    }
    {  // No service: the app is told at once, it does not wait for an answer that cannot come.
        const long before = nowMs();
        CHECK(!evo::controlAnswers(name, 2000) && !evo::sendControl(name, "show"));
        CHECK(nowMs() - before < 1000);
    }
    // The fault seen on a car: another program was given UDP port 52790 on 127.0.0.1, the old channel, and the
    // service gave up. Holding that port (when this machine lets us) no longer matters.
    const int stranger = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in old;
    memset(&old, 0, sizeof(old));
    old.sin_family = AF_INET;
    old.sin_port = htons(52790);
    old.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)bind(stranger, (sockaddr*)&old, sizeof(old));
    evo::EvoControl ctl;
    CHECK(ctl.start(name, state, onCmd, 0));
    close(stranger);
    evo::EvoControl second;
    CHECK(!second.start(name, state, onCmd, 0));  // one service only
    CHECK(evo::controlAnswers(name, 2000));       // ping -> pong, through the app's own call
    CHECK(evo::sendControl(name, "show") && evo::sendControl(name, "gear 2") && evo::sendControl(name, "bogus") &&
          evo::sendControl(name, "hide") && evo::sendControl(name, "quit"));
    usleep(400000);
    CHECK(seen.size() == 4 && seen[0] == "show" && seen[1] == "gear:2" && seen[2] == "hide" && seen[3] == "quit");
    {  // A sender with no address of its own cannot be answered: the service must not stumble on its ping.
        const int mute = socket(AF_UNIX, SOCK_DGRAM, 0);
        sockaddr_un to;
        const socklen_t length = evo::controlAddress(name, to);
        CHECK(sendto(mute, "ping", 4, 0, (sockaddr*)&to, length) == 4);
        close(mute);
        CHECK(evo::controlAnswers(name, 2000));
    }
    ctl.writeState("connecting", 0);
    char expect[64];
    snprintf(expect, sizeof(expect), "connecting %d 0\n", (int)getpid());
    CHECK(text(state) == expect);
    ctl.writeState("connected", 1);
    snprintf(expect, sizeof(expect), "connected %d 1\n", (int)getpid());
    CHECK(text(state) == expect);
    ctl.stop();
    CHECK(access(state, F_OK) != 0);  // removed on stop: the app shows "Android Auto" again
    CHECK(!evo::controlAnswers(name, 2000) && !evo::sendControl(name, "show"));  // the name is free again
    {  // A holder of the name that never answers is not taken for a service, and the wait is the one asked for.
        const int silent = socket(AF_UNIX, SOCK_DGRAM, 0);
        sockaddr_un a;
        const socklen_t length = evo::controlAddress(name, a);
        CHECK(bind(silent, (sockaddr*)&a, length) == 0);
        const long before = nowMs();
        CHECK(!evo::controlAnswers(name, 150));
        CHECK(nowMs() - before >= 100 && nowMs() - before < 1500);
        evo::EvoControl refused;
        CHECK(!refused.start(name, state, onCmd, 0));
        close(silent);
    }
    evo::EvoControl again;
    CHECK(again.start(name, state, onCmd, 0) && evo::controlAnswers(name, 2000));  // a new service can start
    again.stop();
    puts(
        "PASS controle appli iDrive : canal sans port (nom abstrait), ping/pong et commandes par les appels de l "
        "appli, "
        "commandes inconnues refusees, un seul service, reponse immediate sans service, nom libere a l arret, "
        "fichier d'etat atomique et supprime a l'arret");
    return 0;
}
