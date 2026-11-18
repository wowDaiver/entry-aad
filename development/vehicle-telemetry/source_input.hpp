// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "diagnostic_schedule.hpp"
#include "source_mapping.hpp"

namespace telemetry {
// Pure ingestion boundary for offline replay. No sockets, polling or demo values.
// Each capture starts a new instance: identity from another file cannot leak in.
class SourceInput {
public:
    explicit SourceInput(Model& model) : model_(model), dde_(false), egs_(false) {}
    bool identity(unsigned ecu, const unsigned char* data, unsigned size) {
        bool valid = diagnostic_identity(ecu, data, size);
        if (ecu == 0x12) {
            dde_ = valid;
        } else if (ecu == 0x18) {
            egs_ = valid;
        }
        if (!valid && (ecu == 0x12 || ecu == 0x18)) {
            model_.diagnostic_unavailable(ecu, 0, model_.last_time());
        }
        return valid;
    }
    bool record(unsigned ecu, unsigned key, const unsigned char* data, unsigned size, double now) {
        if (!verified(ecu)) {
            return false;
        }
        if (!bound(ecu, key)) {
            return false;
        }
        if (model_.diagnostic(ecu, key, data, size, now)) {
            return true;
        }
        model_.diagnostic_unavailable(ecu, key, now);
        return false;
    }
    bool unavailable(unsigned ecu, unsigned key, double now) {
        if (!verified(ecu) || !bound(ecu, key)) {
            return false;
        }
        model_.diagnostic_unavailable(ecu, key, now);
        return true;
    }
    bool cds(unsigned property, const char* json, double now) { return model_.update(property, json, now); }

private:
    Model& model_;
    bool dde_, egs_;
    bool verified(unsigned ecu) const { return ecu == 0x12 ? dde_ : ecu == 0x18 ? egs_ : false; }
    bool bound(unsigned ecu, unsigned key) const {
        for (unsigned i = 0; i < MetricCount; ++i) {
            const SourceBinding& b = *source_binding(i);
            if ((b.kind == UdsSource || b.kind == ObdSource) && b.ecu == ecu && b.key == key) {
                return true;
            }
        }
        return false;
    }
};
}  // namespace telemetry
