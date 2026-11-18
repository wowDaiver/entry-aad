#pragma once
#include <stddef.h>

namespace telemetry {
// Budget per monotonic one-second window, not a lifetime/session limit.
enum { kProperties = 5, kMaxPayload = 1024, kMaxSamples = 100 };
struct Property {
    const char* name;
    unsigned interval_ms;
};
extern const Property properties[kProperties];
typedef void (*Reply)(void*, unsigned, const char*, double);
struct Transport {
    virtual ~Transport() {}
    virtual bool available(unsigned) = 0;
    virtual void subscribe(unsigned, Reply, void*) = 0;
    virtual void fetch(unsigned) = 0;
    virtual void remove(unsigned) = 0;
};
struct Sink {
    virtual ~Sink() {}
    // payload is borrowed for this call only; nullptr for non-sample events.
    virtual void event(const char* kind, unsigned property, const char* payload, double now) = 0;
};
// Called only on the HMI dispatcher thread. Transport must isolate retired
// subscriptions on restart (NativeCds uses generation cookies). Caller owns lifetime,
// drives poll() at least once/second and calls stop() before PluginApp finalize.
// No requests, threads, allocation, filesystem or firmware writes in constructor.
class Probe {
public:
    Probe(Transport& transport, Sink& sink);
    bool start(double now);
    void poll(double now);
    void stop(double now);
    bool active() const { return active_; }
    unsigned samples() const { return samples_; }

private:
    static void receive(void*, unsigned, const char*, double);
    Transport& transport_;
    Sink& sink_;
    bool active_, subscribed_[kProperties];
    unsigned samples_;
    double window_, last_, fetched_at_[kProperties];
    Probe(const Probe&);
    Probe& operator=(const Probe&);
};
}  // namespace telemetry
