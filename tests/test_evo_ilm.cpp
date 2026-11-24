// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Child-process and ILM tests on the PC; this binary also acts as a fake LayerManagerControl.
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

#include "evo_ilm.hpp"
#include "evo_log.hpp"
#include "evo_process.hpp"
#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            exit(1);                                             \
        }                                                        \
    } while (0)
static std::string fileText(const char* path) {
    std::ifstream file(path);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}
static void truncateFile(const char* path) {
    std::ofstream file(path, std::ios::trunc);
    unlink((std::string(path) + ".order").c_str());
}
int main(int argc, char** argv) {
    if (argc > 1) {
        if (!strcmp(argv[1], "--flood")) {
            for (int i = 0; i < 70000; ++i) {
                putchar('x');
            }
            return 0;
        }
        if (!strcmp(argv[1], "--fail")) {
            puts("surface render order: 0x101");
            return 7;
        }
        if (!strcmp(argv[1], "--diagnostic-fail")) {
            puts("partial stdout");
            fputs("detail stderr\n", stderr);
            return 9;
        }
        const char* log = getenv("EVO_TEST_LM_LOG");
        if (!log) {
            return 2;
        }
        std::ofstream file(log, std::ios::app);
        for (int i = 1; i < argc; ++i) {
            file << (i == 1 ? "" : " ") << argv[i];
        }
        file << '\n';
        file.close();
        if (!strcmp(argv[1], "get")) {
            const char* fixture = getenv("EVO_TEST_SCENE_DIR");
            if (fixture && argc > 2) {
                const char* name = !strcmp(argv[2], "screen")  ? "screen.txt"
                                   : !strcmp(argv[2], "layer") ? "layer.txt"
                                                               : "surfaces.txt";
                if (!strcmp(name, "surfaces.txt") && getenv("EVO_TEST_TRUNCATED_SCENE")) {
                    puts("6 Surface(s):\n- Surface 0 (0x0)");
                } else {
                    printf("%s", fileText((std::string(fixture) + "/" + name).c_str()).c_str());
                }
            } else {
                std::string saved = std::string(log) + ".order";
                std::string order =
                    access(saved.c_str(), F_OK) == 0 ? fileText(saved.c_str()) : getenv("EVO_TEST_ORDER");
                printf("surface render order: %s\n", order.c_str());
            }
            if (getenv("EVO_TEST_ERROR_ZERO")) {
                puts("LayerManagerService returned: failed");
            }
            return getenv("EVO_TEST_FAIL_GET") ? 4 : 0;
        }
        if (argc > 4 && getenv("EVO_TEST_FAIL_SET") && !strcmp(argv[4], getenv("EVO_TEST_FAIL_SET"))) {
            return 5;
        }
        if (argc > 5 && !strcmp(argv[4], "render")) {
            if (argc > 7 || getenv("EVO_TEST_PARSER_ERROR")) {
                fputs("Interpreter error: unexpected token.\n", stderr);
                return 0;  // as the car's tool does
            }
            if (!getenv("EVO_TEST_NO_EFFECT")) {
                std::ofstream state((std::string(log) + ".order").c_str());
                if (argc == 7) {
                    state << argv[6];
                }
            }
        }
        return 0;
    }
    char self[PATH_MAX];
    CHECK(realpath(argv[0], self));
    std::string output;
    std::vector<std::string> args(1, "literal $(echo nope); & data");
    CHECK(evo::runCommand("/bin/echo", args, output));
    CHECK(output == args[0] + "\n");
    args[0] = "--fail";
    CHECK(!evo::runCommand(self, args, output));
    CHECK(output.empty());
    args[0] = "--flood";
    CHECK(!evo::runCommand(self, args, output));
    CHECK(output.empty());
    evo::CommandReport report;
    args[0] = "--diagnostic-fail";
    CHECK(!evo::runCommand(self, args, output, 2000, &report));
    CHECK(output.empty());
    CHECK(report.exitCode == 9 && !report.timedOut && !report.truncated);
    CHECK(report.text.find("partial stdout") != std::string::npos &&
          report.text.find("detail stderr") != std::string::npos);
    args[0] = "--flood";
    CHECK(!evo::runCommand(self, args, output, 2000, &report));
    CHECK(output.empty() && report.truncated && report.text.size() == 65536);
    args[0] = "10";
    uint32_t before = evo::monoMs();
    CHECK(!evo::runCommand("/bin/sleep", args, output, 80, &report));
    CHECK(evo::monoMs() - before < 1500);
    CHECK(report.timedOut && report.exitCode == -1);
    int status;
    CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    CHECK(!evo::runCommand("/not-an-entry-aad-command", args, output, 2000, &report));
    CHECK(report.spawnError == ENOENT);
    std::vector<uint32_t> ids;
    CHECK(evo::EvoIlm::parseRenderOrder("surface render order: [0x101, 0x102]\n", ids));
    CHECK(ids.size() == 2 && ids[1] == 0x102);
    CHECK(evo::EvoIlm::parseRenderOrder("surface render order:\n", ids) && ids.empty());
    CHECK(!evo::EvoIlm::parseRenderOrder("surface render order: 0x101, junk", ids) && ids.empty());
    CHECK(!evo::EvoIlm::parseRenderOrder("surface render order: 0x100000000", ids));
    CHECK(!evo::EvoIlm::parseRenderOrder("surface render order: 0x101,0x101", ids));
    CHECK(!evo::EvoIlm::parseRenderOrder("surface render order: 0x", ids));
    CHECK(!evo::EvoIlm::parseRenderOrder("surface render order: [0x101", ids));
    CHECK(!evo::EvoIlm::parseRenderOrder("surface render order: [0x101,]", ids));
    CHECK(evo::EvoIlm::parseRenderOrder("- surface render order: 0(0x0), \r\n- on screen: 0(0x0)", ids));
    CHECK(ids.size() == 1 && ids[0] == 0);
    CHECK(evo::EvoIlm::parseRenderOrder("- surface render order: 131072(0x20000), 132710(0x20666), \n", ids));
    CHECK(ids.size() == 2 && ids[0] == 0x20000 && ids[1] == 0x20666);
    CHECK(
        evo::EvoIlm::parseRenderOrder("- layer render order: 2147483648(0x80000000), 2164260864(0x81000000), \n", ids));
    CHECK(ids.size() == 2 && ids[0] == 0x80000000u && ids[1] == 0x81000000u);
    CHECK(evo::EvoIlm::parseRenderOrder("- surface render order: \n- on screen: 0(0x0)", ids) && ids.empty());
    CHECK(!evo::EvoIlm::parseRenderOrder("surface render order: 131072(0x20001),", ids) && ids.empty());
    CHECK(!evo::EvoIlm::parseRenderOrder("surface render order: 4294967296(0x0),", ids));
    CHECK(!evo::EvoIlm::parseRenderOrder("surface render order: 0(0x0), 0(0x0),", ids));
    CHECK(!evo::EvoIlm::parseRenderOrder("surface render order: 0(0x0), junk", ids));
    CHECK(!evo::EvoIlm::parseRenderOrder("surface render order: 0(0x0),,", ids));
    CHECK(!evo::EvoIlm::parseRenderOrder("surface render order: 0(0x0)1(0x1)", ids));
    CHECK(evo::EvoIlm::parseSurfaceList("2 Surface(s):\n- Surface 0 (0x0)\n- Surface 196610 (0x30002)\n", ids));
    CHECK(ids.size() == 2 && ids[1] == 0x30002);
    CHECK(evo::EvoIlm::parseSurfaceList("0 Surface(s):\n", ids) && ids.empty());
    CHECK(!evo::EvoIlm::parseSurfaceList("2 Surface(s):\n- Surface 0 (0x0)\n", ids));
    CHECK(!evo::EvoIlm::parseSurfaceList("1 Surface(s):\n- Surface 1 (0x2)\n", ids));
    CHECK(!evo::EvoIlm::parseSurfaceList("2 Surface(s):\n- Surface 0 (0x0)\n- Surface 0 (0x0)\n", ids));
    CHECK(!evo::EvoIlm::parseSurfaceList("LayerManagerService returned: failed\n", ids));
    std::string geometry =
        "- visibility: 1\n- opacity: 1\n- orientation: 0 (up is top)\n"
        "- source region: x=0, y=0, w=1280, h=480\n- destination region: x=0, y=0, w=1280, h=480\n";
    CHECK(evo::EvoIlm::validLayerGeometry(geometry, 1280, 480));
    CHECK(!evo::EvoIlm::validLayerGeometry(geometry, 336, 400));
    CHECK(!evo::EvoIlm::validLayerGeometry(geometry + "- visibility: 1\n", 1280, 480));
    std::string hidden = geometry;
    hidden.replace(hidden.find("visibility: 1"), 13, "visibility: 0");
    CHECK(!evo::EvoIlm::validLayerGeometry(hidden, 1280, 480));
    // winston stacking direction, from a copy of the car's /etc/wayland/compositor.conf.
    bool topFirst = false;
    std::string realConf = fileText("tests/fixtures/compositor-entryevo.conf");
    CHECK(realConf.find("genivi-flip-renderorder = 1") != std::string::npos);
    CHECK(evo::EvoIlm::parseRenderOrderFlip(realConf, topFirst) && topFirst);
    CHECK(evo::EvoIlm::parseRenderOrderFlip("genivi-flip-renderorder = 0\n", topFirst) && !topFirst);
    CHECK(evo::EvoIlm::parseRenderOrderFlip("genivi-flip-renderorder=1 # inline\r\n", topFirst) && topFirst);
    CHECK(evo::EvoIlm::parseRenderOrderFlip("plugin = ivi, genivi\n", topFirst) && !topFirst);  // absent
    CHECK(evo::EvoIlm::parseRenderOrderFlip("#genivi-flip-renderorder = 1\n", topFirst) && !topFirst);
    CHECK(!evo::EvoIlm::parseRenderOrderFlip("genivi-flip-renderorder = 2\n", topFirst));
    CHECK(!evo::EvoIlm::parseRenderOrderFlip("genivi-flip-renderorder =\n", topFirst));
    CHECK(!evo::EvoIlm::parseRenderOrderFlip("genivi-flip-renderorder = 1\ngenivi-flip-renderorder = 1\n", topFirst));
    char plainConf[] = "/tmp/entry-aad-conf-XXXXXX";
    int cfd = mkstemp(plainConf);
    CHECK(cfd >= 0);
    close(cfd);
    {
        std::ofstream plain(plainConf, std::ios::trunc);
        plain << "genivi-flip-renderorder = 0\n";
    }
    char log[] = "/tmp/entry-aad-ilm-XXXXXX";
    int fd = mkstemp(log);
    CHECK(fd >= 0);
    close(fd);
    setenv("EVO_TEST_LM_LOG", log, 1);
    setenv("EVO_TEST_ORDER", "0x101, 0x102", 1);
    evo::EvoIlm ilm(self, plainConf);  // standard GENIVI order: last = top
    CHECK(ilm.renderOrderTopFirst(topFirst) && !topFirst);
    setenv("EVO_TEST_SCENE_DIR", "tests/fixtures/ilm-scene", 1);
    CHECK(ilm.validateTarget(0x7eaa0001, 0x80000000, 1280, 480));
    CHECK(!ilm.validateTarget(0x30002, 0x80000000, 1280, 480));  // occupied on another layer
    CHECK(!ilm.validateTarget(0x7eaa0001, 0x3000, 1280, 480));   // absent from screen order
    CHECK(!ilm.validateTarget(0x7eaa0001, 0x80000000, 800, 480));
    CHECK(ilm.surfaceAbsent(0x7eaa0001) && !ilm.surfaceAbsent(0));
    uint32_t panelW = 0, panelH = 0;  // the panel's size, as the service and the supervisor read it
    CHECK(ilm.screenSize(panelW, panelH) && panelW == 1280 && panelH == 480);
    setenv("EVO_TEST_TRUNCATED_SCENE", "1", 1);
    CHECK(!ilm.validateTarget(0x7eaa0001, 0x80000000, 1280, 480) && !ilm.surfaceAbsent(0x7eaa0001));
    unsetenv("EVO_TEST_TRUNCATED_SCENE");
    setenv("EVO_TEST_ERROR_ZERO", "1", 1);
    CHECK(!ilm.validateTarget(0x7eaa0001, 0x80000000, 1280, 480));
    CHECK(fileText(log).find("set ") == std::string::npos);
    unsetenv("EVO_TEST_ERROR_ZERO");
    unsetenv("EVO_TEST_SCENE_DIR");
    truncateFile(log);
    CHECK(ilm.attachSurface(0x3001, 0x3000, 1280, 480));
    CHECK(fileText(log).find("set layer 0x3000 render order 0x101,0x102,0x3001") != std::string::npos);
    CHECK(fileText(log).find("set screen") == std::string::npos);
    setenv("EVO_TEST_ORDER", "0(0x0), ", 1);
    truncateFile(log);
    CHECK(ilm.attachSurface(0x3001, 0x80000000u, 1280, 480));
    CHECK(fileText(log).find("set layer 0x80000000 render order 0x0,0x3001") != std::string::npos);
    truncateFile(log);
    // Opacity of our surface only, clamped, with the car tool's syntax "set surface <id> opacity <v>".
    CHECK(ilm.setSurfaceOpacity(0x3001, 0.25f) && ilm.setSurfaceOpacity(0x3001, 7.f));
    CHECK(fileText(log).find("set surface 0x3001 opacity 0.25") != std::string::npos);
    CHECK(fileText(log).find("set surface 0x3001 opacity 1.00") != std::string::npos);
    setenv("EVO_TEST_ORDER", "0(0x0), 12289(0x3001), 131072(0x20000), ", 1);
    truncateFile(log);
    CHECK(ilm.detachSurface(0x3001, 0x80000000u));
    CHECK(fileText(log).find("set layer 0x80000000 render order 0x0,0x20000") != std::string::npos);
    setenv("EVO_TEST_ORDER", "0x101, 0x3001, 0x2001", 1);
    truncateFile(log);
    CHECK(ilm.detachSurface(0x3001, 0x3000));
    CHECK(fileText(log).find("set layer 0x3000 render order 0x101,0x2001") != std::string::npos);
    setenv("EVO_TEST_ORDER", "0x3001", 1);
    truncateFile(log);
    CHECK(ilm.detachSurface(0x3001, 0x3000));
    CHECK(ilm.surfacesOnLayer(0x3000, ids) && ids.empty());
    setenv("EVO_TEST_ORDER", "0x0", 1);
    truncateFile(log);
    setenv("EVO_TEST_PARSER_ERROR", "1", 1);
    CHECK(!ilm.attachSurface(0x3001, 0x3000, 1280, 480));
    CHECK(ilm.surfacesOnLayer(0x3000, ids) && ids.size() == 1 && ids[0] == 0);
    unsetenv("EVO_TEST_PARSER_ERROR");
    setenv("EVO_TEST_NO_EFFECT", "1", 1);
    truncateFile(log);
    CHECK(!ilm.attachSurface(0x3001, 0x3000, 1280, 480));
    setenv("EVO_TEST_ORDER", "0x0,0x3001", 1);
    truncateFile(log);
    CHECK(!ilm.detachSurface(0x3001, 0x3000));
    unsetenv("EVO_TEST_NO_EFFECT");
    setenv("EVO_TEST_ORDER", "0x101, malformed", 1);
    truncateFile(log);
    CHECK(!ilm.attachSurface(0x3001, 0x3000, 1280, 480));
    CHECK(fileText(log).find("set ") == std::string::npos);
    unsetenv("EVO_TEST_FAIL_GET");
    setenv("EVO_TEST_ORDER", "0(0x0), ", 1);
    setenv("EVO_TEST_ERROR_ZERO", "1", 1);
    truncateFile(log);
    CHECK(!ilm.attachSurface(0x3001, 0x80000000u, 1280, 480));
    CHECK(fileText(log).find("set ") == std::string::npos);
    unsetenv("EVO_TEST_ERROR_ZERO");
    setenv("EVO_TEST_ORDER", "0x101, 0x102", 1);
    setenv("EVO_TEST_ORDER", "0x101, 0x102", 1);
    setenv("EVO_TEST_FAIL_GET", "1", 1);
    truncateFile(log);
    CHECK(!ilm.attachSurface(0x3001, 0x3000, 1280, 480));
    CHECK(fileText(log).find("set ") == std::string::npos);
    unsetenv("EVO_TEST_FAIL_GET");
    setenv("EVO_TEST_FAIL_SET", "source", 1);
    truncateFile(log);
    CHECK(!ilm.attachSurface(0x3001, 0x3000, 1280, 480));
    CHECK(fileText(log).find("set layer") == std::string::npos);
    unsetenv("EVO_TEST_FAIL_SET");
    // The car's direction (first = top): our surface goes FIRST, BMW surfaces keep their relative order.
    evo::EvoIlm hu(self, "tests/fixtures/compositor-entryevo.conf");
    CHECK(hu.renderOrderTopFirst(topFirst) && topFirst);
    setenv("EVO_TEST_ORDER", "0(0x0), ", 1);
    truncateFile(log);
    CHECK(hu.attachSurface(0x7eaa0001, 0x80000000u, 1280, 480));
    CHECK(fileText(log).find("set layer 0x80000000 render order 0x7eaa0001,0x0") != std::string::npos);
    CHECK(fileText(log).find("set screen") == std::string::npos);
    CHECK(hu.surfacesOnLayer(0x80000000u, ids) && ids.size() == 2 && ids[0] == 0x7eaa0001 && ids[1] == 0);
    CHECK(hu.detachSurface(0x7eaa0001, 0x80000000u));
    CHECK(fileText(log).find("set layer 0x80000000 render order 0x0\n") != std::string::npos);
    setenv("EVO_TEST_ORDER", "0x101, 0x102", 1);
    truncateFile(log);
    CHECK(hu.attachSurface(0x3001, 0x3000, 1280, 480));
    CHECK(fileText(log).find("set layer 0x3000 render order 0x3001,0x101,0x102") != std::string::npos);
    // Volume pill: its own surface above Android Auto, placed and unscaled; put back on top after a new session.
    setenv("EVO_TEST_ORDER", "2125070337(0x7eaa0001), 0(0x0), ", 1);
    truncateFile(log);
    CHECK(hu.attachOverlay(0x7eaa0002, 0x80000000u, 460, 392, 360, 60));
    CHECK(fileText(log).find("set surface 0x7eaa0002 visibility 0") != std::string::npos);
    CHECK(fileText(log).find("set surface 0x7eaa0002 source region 0 0 360 60") != std::string::npos);
    CHECK(fileText(log).find("set surface 0x7eaa0002 destination region 460 392 360 60") != std::string::npos);
    CHECK(fileText(log).find("set layer 0x80000000 render order 0x7eaa0002,0x7eaa0001,0x0") != std::string::npos);
    truncateFile(log);
    setenv("EVO_TEST_ORDER", "0x7eaa0002, 0x7eaa0001, 0x0", 1);
    CHECK(hu.raiseSurface(0x7eaa0002, 0x80000000u));
    CHECK(fileText(log).find("set ") == std::string::npos);      // already on top: nothing written
    setenv("EVO_TEST_ORDER", "0x7eaa0001, 0x7eaa0002, 0x0", 1);  // Android Auto attached again by a new session
    CHECK(hu.raiseSurface(0x7eaa0002, 0x80000000u));
    CHECK(fileText(log).find("set layer 0x80000000 render order 0x7eaa0002,0x7eaa0001,0x0") != std::string::npos);
    truncateFile(log);
    CHECK(!hu.attachOverlay(0x7eaa0002, 0x80000000u, 460, 392, 0, 60));  // no size: refused before any command
    CHECK(fileText(log).empty());
    setenv("EVO_TEST_FAIL_GET", "1", 1);
    CHECK(!hu.raiseSurface(0x7eaa0002, 0x80000000u));  // unreadable order: never rewritten
    CHECK(fileText(log).find("set ") == std::string::npos);
    unsetenv("EVO_TEST_FAIL_GET");
    setenv("EVO_TEST_ORDER", "0x101, 0x102", 1);  // the scene the next cases expect
    truncateFile(log);
    // Unknown direction: refuse before any command (a picture hidden behind the HMI is worse than none).
    evo::EvoIlm unknown(self, "/nonexistent/entry-aad/compositor.conf");
    truncateFile(log);
    CHECK(!unknown.renderOrderTopFirst(topFirst));
    CHECK(!unknown.attachSurface(0x3001, 0x3000, 1280, 480));
    CHECK(fileText(log).empty());
    {
        std::ofstream bad(plainConf, std::ios::trunc);
        bad << "genivi-flip-renderorder = maybe\n";
    }
    evo::EvoIlm invalid(self, plainConf);
    truncateFile(log);
    CHECK(!invalid.attachSurface(0x3001, 0x3000, 1280, 480));
    CHECK(fileText(log).empty());
    setenv("EVO_TEST_FAIL_SET", "source", 1);
    const char* diagnostic = getenv("ENTRY_AAD_DIAGNOSTIC");
    if (diagnostic && *diagnostic) {
        unsetenv("EVO_TEST_FAIL_SET");
        setenv("EVO_TEST_FAIL_GET", "1", 1);
        truncateFile(log);
        args[0] = "--scene";
        args.push_back(self);  // evo-check --scene <LayerManagerControl>
        CHECK(!evo::runCommand(diagnostic, args, output, 5000, &report));
        CHECK(output.empty());
        CHECK(report.text.find("surface render order: 0x101, 0x102") != std::string::npos);
        CHECK(report.text.find("code=4") != std::string::npos);
        CHECK(fileText(log).find("get keyboard focus") != std::string::npos);
        CHECK(fileText(log).find("get surface 0x3001") != std::string::npos);
        CHECK(fileText(log).find("set ") == std::string::npos);
        unsetenv("EVO_TEST_FAIL_GET");
        setenv("EVO_TEST_ERROR_ZERO", "1", 1);
        CHECK(!evo::runCommand(diagnostic, args, output, 5000, &report));
        CHECK(output.empty());
        CHECK(report.text.find("[FAILED] code=0") != std::string::npos);
        unsetenv("EVO_TEST_ERROR_ZERO");
    }
    unlink((std::string(log) + ".order").c_str());
    unlink(log);
    unlink(plainConf);
    puts(
        "PASS processus bornes, absence de shell, ILM: conservation des surfaces, sens winston (premier = dessus) "
        "et refus des lectures invalides");
    return 0;
}
