// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <stdlib.h>

#include <string>

#include "persistent_guard.hpp"
static const char* A = "00000000-0000-0000-0000-000000000001";
static const char* B = "00000000-0000-0000-0000-000000000002";
static const char* C = "00000000-0000-0000-0000-000000000003";
struct Fixture {
    std::string dir;
    Fixture() {
#ifdef __APPLE__
        char pattern[] = "/private/tmp/telemetry-persistent-XXXXXX";
#else
        char pattern[] = "/tmp/telemetry-persistent-XXXXXX";
#endif
        char* p = mkdtemp(pattern);
        assert(p);
        dir = p;
        put("persistent.enabled", telemetry::PersistentToken);
    }
    void put(const char* name, const char* value) {
        int fd = open((dir + "/" + name).c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0);
        assert(write(fd, value, strlen(value)) == static_cast<ssize_t>(strlen(value)));
        close(fd);
    }
    bool exists(const char* name) {
        struct stat st = {};
        return !lstat((dir + "/" + name).c_str(), &st);
    }
    ~Fixture() {
        const char* names[] = {"persistent.enabled", "persistent.disabled", "persistent.state", "persistent.state.new",
                               "persistent.lock"};
        for (unsigned i = 0; i < 5; ++i) {
            unlink((dir + "/" + names[i]).c_str());
        }
        rmdir(dir.c_str());
    }
};
int main() {
    {
        Fixture f;
        for (unsigned i = 0; i < 100; ++i) {
            telemetry::PersistentGuard g;
            assert(g.start(f.dir.c_str(), i % 2 ? A : B, 100 + i));
            assert(g.healthy());
            g.clean();
        }
        assert(!f.exists("persistent.disabled"));
    }
    {
        Fixture f;
        {
            telemetry::PersistentGuard g;
            assert(g.start(f.dir.c_str(), A, 100));
            assert(g.healthy());
        }
        {
            telemetry::PersistentGuard g;
            assert(g.start(f.dir.c_str(), B, 101));
            assert(g.healthy());
        }
        assert(!f.exists("persistent.disabled"));
    }
    {
        Fixture f;
        {
            telemetry::PersistentGuard g;
            assert(g.start(f.dir.c_str(), A, 100));
        }
        {
            telemetry::PersistentGuard g;
            assert(g.start(f.dir.c_str(), B, 101));
        }
        telemetry::PersistentGuard g;
        assert(!g.start(f.dir.c_str(), C, 102));
        assert(f.exists("persistent.disabled"));
    }
    {
        Fixture f;
        {
            telemetry::PersistentGuard g;
            assert(g.start(f.dir.c_str(), A, 100));
            assert(g.healthy());
        }
        {
            telemetry::PersistentGuard g;
            assert(g.start(f.dir.c_str(), A, 101));
            assert(g.healthy());
        }
        telemetry::PersistentGuard g;
        assert(!g.start(f.dir.c_str(), A, 102));
        assert(f.exists("persistent.disabled"));
    }
    {
        Fixture f;
        telemetry::PersistentGuard first, second;
        assert(first.start(f.dir.c_str(), A, 100));
        assert(!second.start(f.dir.c_str(), A, 101));
        assert(!f.exists("persistent.disabled"));
        first.clean();
        first.release();
        assert(second.start(f.dir.c_str(), A, 101));
        second.fail("native-error");
        assert(f.exists("persistent.disabled"));
    }
    {
        Fixture f;
        f.put("persistent.state", "bad state\n");
        telemetry::PersistentGuard g;
        assert(!g.start(f.dir.c_str(), A, 100));
        assert(f.exists("persistent.disabled"));
    }
    {
        Fixture f;
        unlink((f.dir + "/persistent.enabled").c_str());
        assert(!symlink("/etc/passwd", (f.dir + "/persistent.enabled").c_str()));
        telemetry::PersistentGuard g;
        assert(!g.start(f.dir.c_str(), A, 100));
    }
    {
        Fixture f;
        f.put("persistent.state.new", "interrupted\n");
        telemetry::PersistentGuard g;
        assert(!g.start(f.dir.c_str(), A, 100));
        assert(f.exists("persistent.disabled"));
    }
    puts(
        "persistent guard: 100 normal restarts, healthy power cycles, early-crash fallback, same-boot failures, "
        "locking and unsafe-state checks passed");
}
