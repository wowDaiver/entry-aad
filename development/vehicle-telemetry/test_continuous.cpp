// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "probe.hpp"
#include "telemetry_model.hpp"
using namespace telemetry;
struct ModelSink : Sink {
    Model model;
    ModelSink() : model(0) {}
    void event(const char* kind, unsigned i, const char* raw, double now) {
        if (!strcmp(kind, "sample")) {
            model.update(i, raw, now);
        } else if (!strcmp(kind, "stopped")) {
            model.stop(now);
        }
    }
};
struct QuietSource : Transport {
    Reply replies[kProperties];
    void* contexts[kProperties];
    double now;
    unsigned requests, cancelled;
    bool respond;
    QuietSource() : now(0), requests(0), cancelled(0), respond(true) {}
    bool available(unsigned) { return true; }
    void subscribe(unsigned i, Reply reply, void* context) {
        replies[i] = reply;
        contexts[i] = context;
    }
    void fetch(unsigned i) {
        static const char* payload[] = {
            "{\"temperature\":{\"engine\":85,\"oil\":84}}", "{\"RPMSpeed\":780}", "{\"speedActual\":0}",
            "{\"averageConsumption\":{\"averageConsumption1\":9.7,\"unit\":1}}", "{\"battery\":87}"};
        ++requests;
        if (respond) {
            replies[i](contexts[i], i, payload[i], now);
        }
    }
    void remove(unsigned) { ++cancelled; }
};
int main() {
    ModelSink sink;
    QuietSource source;
    Probe probe(source, sink);
    assert(probe.start(0));
    for (unsigned sec = 1; sec <= 3600; ++sec) {
        source.now = sec;
        probe.poll(sec);
        assert(probe.active());
        for (unsigned i = 0; i < 6; ++i) {
            assert(sink.model.status(i, sec) == Valid);
        }
    }
    assert(sink.model.reading(BatterySoc).value == 87);
    source.respond = false;
    for (unsigned sec = 3601; sec <= 3640; ++sec) {
        source.now = sec;
        probe.poll(sec);
    }
    assert(probe.active());
    for (unsigned i = 0; i < 6; ++i) {
        assert(sink.model.status(i, 3640) == Stale);
    }
    source.respond = true;
    for (unsigned sec = 3641; sec <= 3645; ++sec) {
        source.now = sec;
        probe.poll(sec);
    }
    for (unsigned i = 0; i < 6; ++i) {
        assert(sink.model.status(i, 3645) == Valid);
    }
    probe.stop(3646);
    unsigned before = source.requests;
    source.now = 3650;
    probe.poll(3650);
    assert(source.requests == before && source.cancelled == 5);
    puts(
        "continuous pipeline: quiet values fresh for one hour; true loss stale; recovery and exit cancellation passed");
}
