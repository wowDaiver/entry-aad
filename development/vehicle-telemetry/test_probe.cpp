#include <assert.h>
#include <stdio.h>

#include <limits>
#include <string>
#include <vector>

#include "probe.hpp"
using namespace telemetry;
struct Record {
    std::string kind;
    unsigned property;
    std::string payload;
};
struct Journal : Sink {
    std::vector<Record> rows;
    void event(const char* k, unsigned i, const char* p, double) {
        Record r = {k, i, p ? p : ""};
        rows.push_back(r);
    }
};
struct Fake : Transport {
    Reply replies[kProperties];
    void* contexts[kProperties];
    bool supported[kProperties];
    unsigned added, fetched, removed;
    bool immediate, late_on_remove, throw_remove, reply_on_fetch, throw_fetch;
    double clock;
    Fake()
        : added(0),
          fetched(0),
          removed(0),
          immediate(false),
          late_on_remove(false),
          throw_remove(false),
          reply_on_fetch(false),
          throw_fetch(false),
          clock(0) {
        for (unsigned i = 0; i < kProperties; ++i) {
            supported[i] = true;
            replies[i] = 0;
            contexts[i] = 0;
        }
    }
    bool available(unsigned i) { return supported[i]; }
    void subscribe(unsigned i, Reply r, void* c) {
        replies[i] = r;
        contexts[i] = c;
        ++added;
        if (immediate) {
            r(c, i, "{\"battery\":0}", 0);
        }
    }
    void fetch(unsigned i) {
        assert(replies[i]);
        ++fetched;
        if (throw_fetch) {
            throw 1;
        }
        if (reply_on_fetch) {
            replies[i](contexts[i], i, "{\"battery\":87}", clock);
        }
    }
    void remove(unsigned i) {
        ++removed;
        if (throw_remove) {
            throw 1;
        }
        if (late_on_remove) {
            replies[i](contexts[i], i, "{\"battery\":99}", 29);
        }
    }
    void send(unsigned i, const char* raw, double now) { replies[i](contexts[i], i, raw, now); }
};
int main() {
    {
        Fake t;
        Journal j;
        Probe p(t, j);
        t.supported[1] = false;
        assert(p.start(0));
        assert(t.added == 4 && t.fetched == 4);
        assert(j.rows.size() == 1 && j.rows[0].kind == "unavailable");
        t.send(4, "{\"battery\":0}", 1);
        assert(p.samples() == 1);
        t.late_on_remove = true;
        p.stop(2);
        assert(!p.active() && t.removed == 4 && p.samples() == 1);
        t.send(4, "{}", 3);
        assert(p.samples() == 1);
        p.stop(4);
        assert(t.removed == 4);
        assert(p.start(5));
        p.stop(6);
        assert(t.removed == 8);
    }
    {
        Fake t;
        Journal j;
        Probe p(t, j);
        t.immediate = true;
        assert(p.start(0));
        assert(p.samples() == 5);  // callbacks during subscribe
        p.poll(29.999);
        assert(p.active());
        t.send(4, "{}", 30);
        assert(p.samples() == 1);
        p.poll(30);
        assert(p.active() && t.removed == 0);
        p.stop(31);
        assert(t.removed == 5);
    }
    {
        Fake t;
        Journal j;
        Probe p(t, j);
        assert(p.start(0));
        std::string too_long(1025, 'x');
        t.send(0, too_long.c_str(), 1);
        assert(j.rows.back().kind == "invalid_payload");
        t.send(0, 0, 1);
        assert(j.rows.back().kind == "invalid_payload");
        for (unsigned i = 2; i < 100; ++i) {
            t.send(0, "{}", 1);
        }
        t.send(0, "{}", 1);
        assert(p.samples() == 100);
        p.poll(1);
        assert(p.active() && t.removed == 0);
        assert(j.rows.size() == 100);
        t.send(0, "{}", 2);
        assert(p.samples() == 1 && j.rows.size() == 101);
        p.stop(3);
        assert(t.removed == 5);
    }
    {
        Fake t;
        Journal j;
        Probe p(t, j);
        assert(!p.start(std::numeric_limits<double>::quiet_NaN()));
        assert(!p.start(-1));
        assert(p.start(10));
        t.send(4, "{}", 9);
        assert(p.samples() == 0);
        p.poll(9);
        assert(!p.active() && t.removed == 5);
    }
    {
        Fake t;
        Journal j;
        Probe p(t, j);
        assert(p.start(0));
        t.throw_remove = true;
        p.stop(1);
        assert(!p.active() && t.removed == 5);
    }
    {
        Fake t;
        Journal j;
        Probe p(t, j);
        t.reply_on_fetch = true;
        assert(p.start(0));
        // One hour visible, well beyond both old limits. Quiet properties are
        // still refreshed; subscription count and callback contexts stay fixed.
        for (unsigned sec = 1; sec <= 3600; ++sec) {
            t.clock = sec;
            p.poll(sec);
            assert(p.active());
        }
        assert(t.added == 5 && t.removed == 0);
        assert(t.fetched == 5 + 2 * 3600 + 3 * 720);
        unsigned before = t.fetched;
        Reply late = t.replies[4];
        void* ctx = t.contexts[4];
        p.stop(3601);
        size_t count = j.rows.size();
        t.clock = 3602;
        p.poll(3602);
        late(ctx, 4, "{\"battery\":88}", 3602);
        assert(t.fetched == before && t.removed == 5 && j.rows.size() == count);
    }
    {
        Fake t;
        Journal j;
        Probe p(t, j);
        assert(p.start(0));
        p.poll(3600);
        assert(p.active() && t.fetched == 10);  // no catch-up burst
        t.throw_fetch = true;
        p.poll(3601);
        assert(!p.active() && t.removed == 5);
    }
    puts("probe: continuous hour, periodic quiet-value refresh, flood recovery, cancellation and clocks passed");
}
