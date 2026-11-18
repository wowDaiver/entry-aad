#include "probe.hpp"

#include <math.h>
#include <string.h>

namespace telemetry {
const Property properties[kProperties] = {{"engine.temperature", 5000},
                                          {"engine.RPMSpeed", 200},
                                          {"driving.speedActual", 200},
                                          {"driving.averageConsumption", 5000},
                                          {"sensors.battery", 5000}};
Probe::Probe(Transport& transport, Sink& sink)
    : transport_(transport), sink_(sink), active_(false), samples_(0), window_(0), last_(0) {
    memset(subscribed_, 0, sizeof(subscribed_));
    memset(fetched_at_, 0, sizeof(fetched_at_));
}
bool Probe::start(double now) {
    if (active_ || !isfinite(now) || now < last_) {
        return false;
    }
    active_ = true;
    samples_ = 0;
    window_ = last_ = now;
    try {
        for (unsigned i = 0; i < kProperties && active_; ++i) {
            if (!transport_.available(i)) {
                sink_.event("unavailable", i, 0, now);
                continue;
            }
            // Publish context before subscribing: transports may deliver synchronously.
            subscribed_[i] = true;
            fetched_at_[i] = now;
            transport_.subscribe(i, &Probe::receive, this);
            if (active_) {
                transport_.fetch(i);
            }
        }
    } catch (...) {
        stop(now);
        return false;
    }
    return true;
}
void Probe::receive(void* context, unsigned i, const char* raw, double now) {
    Probe& self = *static_cast<Probe*>(context);
    if (!self.active_ || i >= kProperties || !self.subscribed_[i]) {
        return;
    }
    if (!isfinite(now) || now < self.last_) {
        return;
    }
    self.last_ = now;
    if (now - self.window_ >= 1) {
        self.window_ = now;
        self.samples_ = 0;
    }
    if (self.samples_ >= kMaxSamples) {
        return;
    }
    ++self.samples_;  // malformed payloads consume the budget too
    if (!raw || strnlen(raw, kMaxPayload + 1) > kMaxPayload) {
        self.sink_.event("invalid_payload", i, 0, now);
        return;
    }
    self.sink_.event("sample", i, raw, now);
}
void Probe::poll(double now) {
    if (!active_) {
        return;
    }
    if (!isfinite(now) || now < last_) {
        stop(isfinite(now) && now >= last_ ? now : last_);
        return;
    }
    last_ = now;
    if (now - window_ >= 1) {
        window_ = now;
        samples_ = 0;
    }
    // CDS change notifications may stay quiet when a value is unchanged.
    // Refresh snapshots at a bounded rate so those values remain current.
    // No catch-up burst after a delayed dispatcher tick.
    try {
        for (unsigned i = 0; i < kProperties && active_; ++i) {
            if (!subscribed_[i] || now - fetched_at_[i] < properties[i].interval_ms / 1000.0) {
                continue;
            }
            fetched_at_[i] = now;  // publish before potentially synchronous callback
            transport_.fetch(i);
        }
    } catch (...) {
        stop(now);
    }
}
void Probe::stop(double now) {
    if (!active_) {
        return;
    }
    if (!isfinite(now) || now < last_) {
        now = last_;
    }
    active_ = false;  // reject reentrant/late callbacks before unsubscribing
    for (unsigned i = 0; i < kProperties; ++i) {
        if (!subscribed_[i]) {
            continue;
        }
        subscribed_[i] = false;
        try {
            transport_.remove(i);
        } catch (...) {
        }  // attempt every cancellation
    }
    try {
        sink_.event("stopped", kProperties, 0, now);
    } catch (...) {
    }
}
}  // namespace telemetry
