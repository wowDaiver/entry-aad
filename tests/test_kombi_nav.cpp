// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Tests platform/kombi_nav.cpp end to end: the arrows in the service, with their threads, files and a fake KOMBI.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "evo_log.hpp"
#include "kombi_fake_kombi.hpp"
#include "kombi_most.hpp"
#include "kombi_nav.hpp"
#include "kombi_test_util.hpp"

namespace {

using namespace evo::kombi;
using kombitest::Bytes;
using kombitest::FakeKombi;
using kombitest::Step;

int g_bmw = -1;
int bmwProbe() {
    return g_bmw;
}

bool exists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

void touch(const std::string& path) {
    FILE* f = fopen(path.c_str(), "w");
    CHECK(f);
    fclose(f);
}

// A temporary folder for the setting and marker files, and the arrows' settings for the fake KOMBI.
struct Place {
    char dir[64];
    std::string off, marker, single;
    Place() {
        snprintf(dir, sizeof(dir), "/tmp/kombi-navXXXXXX");
        CHECK(mkdtemp(dir));
        off = std::string(dir) + "/kombi-off";
        marker = std::string(dir) + "/kombi-nav.shown";
        single = std::string(dir) + "/kombi-single-maneuver";
    }
    ~Place() {
        unlink(off.c_str());
        unlink(marker.c_str());
        unlink(single.c_str());
        rmdir(dir);
    }
    ArrowsConfig config(uint16_t port) const {
        ArrowsConfig c;
        c.clusterAddress = "127.0.0.1";
        c.clusterPort = port;
        c.offFile = off;
        c.markerFile = marker;
        c.singleFile = single;
        c.guide.approachMiles = 0;
        return c;
    }
};

// The messages of one moment, as the session hands them over.
void tell(Arrows& arrows, const Step& step, int64_t metres, const char* text, int unit) {
    const Bytes status = kombitest::status(1), state = kombitest::state(step),
                position = kombitest::position(metres, text, unit);
    Arrows::onNavStatus(kNavStatus, &status[0], status.size(), &arrows);
    Arrows::onNavStatus(kNavState, &state[0], state.size(), &arrows);
    Arrows::onNavStatus(kNavPosition, &position[0], position.size(), &arrows);
}

// The configured file selects both the graph and its log once, before start(), even if the file changes meanwhile.
void routeModeSelectedOnce() {
    for (int single = 0; single < 2; ++single) {
        FakeKombi k;
        Place p;
        if (single) {
            touch(p.single);
        }
        Arrows arrows(p.config(k.port()));
        if (single) {
            CHECK(unlink(p.single.c_str()) == 0);
        } else {
            touch(p.single);
        }

        FILE* log = tmpfile();
        CHECK(log);
        fflush(stdout);
        const int saved = dup(STDOUT_FILENO);
        CHECK(saved >= 0 && dup2(fileno(log), STDOUT_FILENO) >= 0);
        arrows.start();
        fflush(stdout);
        CHECK(dup2(saved, STDOUT_FILENO) >= 0);
        close(saved);
        rewind(log);
        std::string lines;
        char line[1024];
        while (fgets(line, sizeof(line), log)) {
            lines += line;
        }
        fclose(log);
        CHECK(lines.find(single ? "route graph: next maneuver only (14 requests)"
                                : "route graph: whole captured route") != std::string::npos);
        CHECK(lines.find(single ? "route graph: whole captured route" : "route graph: next maneuver only") ==
              std::string::npos);

        const size_t count = single ? 14 : 97;
        tell(arrows, Step(7, "TEST ST"), 800, "0.5", kUnitMilesTenths);
        CHECK(k.waitFor(count) && k.count() == count);
        arrows.stop();
        CHECK(k.count() == count + 6 && !exists(p.marker));
    }
}

void endToEnd() {
    FakeKombi k;
    Place p;
    {
        Arrows arrows(p.config(k.port()));
        CHECK(arrows.settingOn());  // on by default
        arrows.start();
        tell(arrows, Step(7, "TEST ST"), 800, "0.5", kUnitMilesTenths);
        Arrows::onNavStatus(0x8001, nullptr, 0, &arrows);  // a message the arrows don't read
        CHECK(k.waitFor(97) && k.count() == 97);
        usleep(300000);
        CHECK(exists(p.marker));  // an arrow may be up
        tell(arrows, Step(7, "TEST ST"), 700, "0.4", kUnitMilesTenths);
        CHECK(k.waitFor(98) && k.count() == 98);
        arrows.onSessionEnded();  // the phone left: cleared at once
        CHECK(k.waitFor(104));
        usleep(400000);
        CHECK(k.count() == 104 && !exists(p.marker));
        tell(arrows, Step(7, "TEST ST"), 800, "0.5", kUnitMilesTenths);  // next session, new route
        CHECK(k.waitFor(104 + 97));
        touch(p.off);  // the menu turns the arrows off: cleared within a second
        CHECK(k.waitFor(104 + 97 + 6, 3.0) && !arrows.settingOn());
        arrows.stop();  // nothing up: no cancel at exit
        CHECK(k.count() == 104 + 97 + 6);
    }
    unlink(p.off.c_str());
    size_t base = k.count();
    {
        Arrows arrows(p.config(k.port()));  // exit with an arrow up: cancelled, marker gone
        arrows.start();
        tell(arrows, Step(8, "MAIN ST"), 300, "0.2", kUnitMilesTenths);
        CHECK(k.waitFor(base + 97));
        arrows.stop();
        CHECK(k.count() == base + 97 + 6 && !exists(p.marker));
    }
    base = k.count();
    touch(p.marker);  // left by a killed run
    touch(p.off);     // the cancel goes even when off
    {
        Arrows arrows(p.config(k.port()));
        arrows.start();
        CHECK(k.waitFor(base + 6));
        tell(arrows, Step(8, "MAIN ST"), 300, "0.2", kUnitMilesTenths);
        usleep(1200000);
        arrows.stop();
        CHECK(k.count() == base + 6 && !exists(p.marker));
    }
    const std::vector<kombitest::Seen> seen = k.seen();
    for (size_t i = 0; i < seen.size(); ++i) {
        CHECK(isAllowedMethod(seen[i].method));
    }
}

// While the BMW navigation guides nothing is written and nothing cancelled; when it ends the arrow comes back whole.
// Unknown never stops the arrows.
void bmwNavigationWins() {
    FakeKombi k;
    Place p;
    g_bmw = -1;
    {
        Arrows arrows(p.config(k.port()));
        arrows.start(&bmwProbe);
        tell(arrows, Step(7, "TEST ST"), 800, "0.5", kUnitMilesTenths);
        CHECK(k.waitFor(97) && k.count() == 97);
        g_bmw = 1;
        usleep(2600000);
        tell(arrows, Step(7, "TEST ST"), 700, "0.4", kUnitMilesTenths);
        usleep(600000);
        CHECK(k.count() == 97 && !exists(p.marker));  // the factory graph owns the models
        g_bmw = 0;
        CHECK(k.waitFor(97 * 2, 5.0) && k.count() == 97 * 2);
        arrows.stop();
        CHECK(k.count() == 97 * 2 + 6 && !exists(p.marker));
    }
    g_bmw = -1;
}

// A refused request: the arrows stay off until the next session.
void offUntilNextSession() {
    FakeKombi k;
    Place p;
    k.refuse(1);
    Arrows arrows(p.config(k.port()));
    arrows.start();
    tell(arrows, Step(7, "TEST ST"), 800, "0.5", kUnitMilesTenths);
    CHECK(k.waitFor(1 + 6));
    tell(arrows, Step(7, "TEST ST"), 700, "0.4", kUnitMilesTenths);
    usleep(1000000);
    CHECK(k.count() == 1 + 6);
    arrows.onSessionEnded();
    usleep(600000);
    tell(arrows, Step(7, "TEST ST"), 700, "0.4", kUnitMilesTenths);
    CHECK(k.waitFor(1 + 6 + 97));
    arrows.stop();
    CHECK(k.count() == 1 + 6 + 97 + 6);
}

// The MOST cluster's side of the arrows: what reaches vmostd.
class CountingPort : public evo::VmostPort {
public:
    CountingPort() : sent(0) {}
    int send(evo::VmostMsgOut&) override {
        ++sent;
        return 0;
    }
    int shadow(uint8_t, uint8_t, bool) override { return 0; }
    void pump(int) override {}
    int sent;
};

// Ticks the MOST cluster's calls for about a second, as the controller reader would.
int mostCalls(MostHud& most) {
    CountingPort port;
    for (int i = 0; i < 100; ++i) {
        most.tick(evo::monoMs(), port);
        usleep(10000);
    }
    return port.sent;
}

// The Ethernet KOMBI tells the MOST cluster it took an arrow: on a G-series car the MOST cluster stays alone, even with
// the phone guiding.
void mostAloneWithEthernet() {
    FakeKombi k;
    Place p;
    MostHudConfig mc;
    mc.markerFile = std::string(p.dir) + "/kombi-most.shown";
    mc.offFile = p.off;
    mc.ethernetFirstS = 0;
    MostHud most(mc);
    Arrows arrows(p.config(k.port()));
    arrows.attachMost(&most);
    arrows.start();
    tell(arrows, Step(7, "TEST ST"), 800, "0.5", kUnitMilesTenths);
    CHECK(k.waitFor(97));
    const Bytes status = kombitest::status(1), state = kombitest::state(Step(7, "TEST ST")),
                position = kombitest::position(800, "0.5", kUnitMilesTenths);
    MostHud::onNavStatus(kNavStatus, &status[0], status.size(), &most);
    MostHud::onNavStatus(kNavState, &state[0], state.size(), &most);
    MostHud::onNavStatus(kNavPosition, &position[0], position.size(), &most);
    CHECK(mostCalls(most) == 0 && most.phase() == MostHud::kIdle);
    arrows.stop();
}

}  // namespace

int main() {
    routeModeSelectedOnce();
    endToEnd();
    bmwNavigationWins();
    offUntilNextSession();
    mostAloneWithEthernet();
    puts(
        "PASS kombi nav: route mode and log, menu setting, session end, exit, marker of a killed run, BMW navigation, "
        "refusal, MOST cluster left alone by an Ethernet KOMBI");
    return 0;
}
