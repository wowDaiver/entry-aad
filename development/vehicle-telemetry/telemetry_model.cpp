// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry_model.hpp"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "diagnostic_records.hpp"
#include "diagnostic_schedule.hpp"
#include "source_mapping.hpp"

namespace telemetry {
namespace {
// Bounded JSON subset reader: full object/array syntax, scalar validation,
// duplicate keys rejected. CDS keys must be literal ASCII (escaped keys fail
// closed); unrelated string values may contain JSON escapes. No heap allocation.
struct Node {
    enum Type { Object, Array, Number, String, Boolean, Null } type;
    int child, next, key, key_end;
    double number;
    bool integer_literal;
};
class Json {
public:
    Node nodes[96];
    explicit Json(const char* text) : text_(text), at_(0), count_(0), size_(0) {}
    bool parse() {
        if (!text_) {
            return false;
        }
        size_ = strnlen(text_, kMaxPayload + 1);
        if (size_ > kMaxPayload) {
            return false;
        }
        int root = value(0);
        space();
        return root == 0 && at_ == size_ && nodes[0].type == Node::Object;
    }
    int field(int object, const char* key) const {
        if (object < 0 || nodes[object].type != Node::Object) {
            return -1;
        }
        for (int n = nodes[object].child; n >= 0; n = nodes[n].next) {
            if (nodes[n].key >= 0 && equal(nodes[n].key, nodes[n].key_end, key)) {
                return n;
            }
        }
        return -1;
    }
    bool number(int node, double& out) const {
        if (node < 0 || nodes[node].type != Node::Number) {
            return false;
        }
        out = nodes[node].number;
        return true;
    }
    bool integer(int node, double& out) const { return number(node, out) && nodes[node].integer_literal; }

private:
    const char* text_;
    size_t at_;
    int count_;
    size_t size_;
    void space() {
        while (at_ < size_ && (text_[at_] == ' ' || text_[at_] == '\r' || text_[at_] == '\n' || text_[at_] == '\t')) {
            ++at_;
        }
    }
    bool equal(int begin, int end, const char* s) const {
        return end - begin == static_cast<int>(strlen(s)) && !memcmp(text_ + begin, s, end - begin);
    }
    int node(Node::Type type) {
        if (count_ == 96) {
            return -1;
        }
        int n = count_++;
        nodes[n].type = type;
        nodes[n].child = nodes[n].next = -1;
        nodes[n].key = nodes[n].key_end = -1;
        nodes[n].number = 0;
        nodes[n].integer_literal = false;
        return n;
    }
    bool quoted(bool key, int& begin, int& end) {
        if (at_ >= size_ || text_[at_++] != '"') {
            return false;
        }
        begin = static_cast<int>(at_);
        while (at_ < size_) {
            unsigned char c = text_[at_++];
            if (c == '"') {
                end = static_cast<int>(at_ - 1);
                return true;
            }
            if (c < 32 || c > 127) {
                return false;
            }
            if (c == '\\') {
                if (key || at_ == size_) {
                    return false;
                }
                char esc = text_[at_++];
                if (esc == 'u') {
                    for (unsigned i = 0; i < 4; ++i) {
                        if (at_ == size_) {
                            return false;
                        }
                        char hex = text_[at_++];
                        if (!((hex >= '0' && hex <= '9') || (hex >= 'a' && hex <= 'f') || (hex >= 'A' && hex <= 'F'))) {
                            return false;
                        }
                    }
                } else if (!strchr("\"\\/bfnrt", esc)) {
                    return false;
                }
            }
        }
        return false;
    }
    int value(unsigned depth) {
        space();
        if (depth > 8 || at_ == size_) {
            return -1;
        }
        char c = text_[at_];
        if (c == '{' || c == '[') {
            bool object = c == '{';
            int n = node(object ? Node::Object : Node::Array);
            if (n < 0) {
                return -1;
            }
            ++at_;
            space();
            char close = object ? '}' : ']';
            if (at_ < size_ && text_[at_] == close) {
                ++at_;
                return n;
            }
            int previous = -1;
            for (;;) {
                int begin = -1, end = -1;
                if (object) {
                    if (!quoted(true, begin, end)) {
                        return -1;
                    }
                    for (int old = nodes[n].child; old >= 0; old = nodes[old].next) {
                        if (end - begin == nodes[old].key_end - nodes[old].key &&
                            !memcmp(text_ + begin, text_ + nodes[old].key, end - begin)) {
                            return -1;
                        }
                    }
                    space();
                    if (at_ == size_ || text_[at_++] != ':') {
                        return -1;
                    }
                }
                int child = value(depth + 1);
                if (child < 0) {
                    return -1;
                }
                nodes[child].key = begin;
                nodes[child].key_end = end;
                if (previous < 0) {
                    nodes[n].child = child;
                } else {
                    nodes[previous].next = child;
                }
                previous = child;
                space();
                if (at_ == size_) {
                    return -1;
                }
                char delim = text_[at_++];
                if (delim == close) {
                    return n;
                }
                if (delim != ',') {
                    return -1;
                }
                space();
            }
        }
        if (c == '"') {
            int begin, end;
            if (!quoted(false, begin, end)) {
                return -1;
            }
            return node(Node::String);
        }
        const char* words[] = {"true", "false", "null"};
        for (unsigned i = 0; i < 3; ++i) {
            size_t len = strlen(words[i]);
            if (size_ - at_ >= len && !memcmp(text_ + at_, words[i], len)) {
                at_ += len;
                return node(i == 2 ? Node::Null : Node::Boolean);
            }
        }
        size_t begin = at_;
        if (text_[at_] == '-') {
            ++at_;
        }
        if (at_ == size_) {
            return -1;
        }
        if (text_[at_] == '0') {
            ++at_;
        } else if (text_[at_] >= '1' && text_[at_] <= '9') {
            while (at_ < size_ && text_[at_] >= '0' && text_[at_] <= '9') {
                ++at_;
            }
        } else {
            return -1;
        }
        if (at_ < size_ && text_[at_] == '.') {
            size_t start = ++at_;
            while (at_ < size_ && text_[at_] >= '0' && text_[at_] <= '9') {
                ++at_;
            }
            if (at_ == start) {
                return -1;
            }
        }
        if (at_ < size_ && (text_[at_] == 'e' || text_[at_] == 'E')) {
            ++at_;
            if (at_ < size_ && (text_[at_] == '+' || text_[at_] == '-')) {
                ++at_;
            }
            size_t start = at_;
            while (at_ < size_ && text_[at_] >= '0' && text_[at_] <= '9') {
                ++at_;
            }
            if (at_ == start) {
                return -1;
            }
        }
        char* tail = 0;
        double number = strtod(text_ + begin, &tail);
        if (tail != text_ + at_ || !isfinite(number)) {
            return -1;
        }
        int n = node(Node::Number);
        if (n >= 0) {
            nodes[n].number = number;
            nodes[n].integer_literal = true;
            for (size_t j = begin; j < at_; ++j) {
                if (text_[j] == '.' || text_[j] == 'e' || text_[j] == 'E') {
                    nodes[n].integer_literal = false;
                }
            }
        }
        return n;
    }
};
const unsigned prop[6] = {0, 0, 1, 2, 3, 4};
const double low[6] = {-48, -48, 0, 0, 0, 0};
const double high[6] = {206, 206, 16383, 1023, 409.2, 100};
const Unit units[6] = {Celsius, Celsius, Revolutions, Kmh, NoUnit, Percent};
}  // namespace
Model::Model(double now) : opened_at_(now), last_time_(now), stopped_(false) {
    if (!isfinite(now) || now < 0) {
        opened_at_ = last_time_ = 0;
        stopped_ = true;
    }
    for (unsigned i = 0; i < MetricCount; ++i) {
        rows_[i].value = rows_[i].received_at = 0;
        rows_[i].unit = i < 6 ? units[i] : NoUnit;
        rows_[i].error = i < 6 ? Waiting : SourceUnknown;
        rows_[i].present = false;
    }
}
bool Model::clock(double now) {
    if (!isfinite(now) || now < last_time_) {
        return false;
    }
    last_time_ = now;
    return true;
}
void Model::fail(unsigned property, Error error) {
    for (unsigned i = 0; i < 6; ++i) {
        if (prop[i] == property) {
            rows_[i].error = error;
        }
    }
}
bool Model::update(unsigned property, const char* raw, double now) {
    if (stopped_ || property >= kProperties || !clock(now)) {
        return false;
    }
    Json json(raw);
    if (!json.parse()) {
        fail(property, BadJson);
        return false;
    }
    if (json.field(0, "error") >= 0) {
        fail(property, SourceError);
        return false;
    }
    for (unsigned i = 0; i < 6; ++i) {
        if (prop[i] != property) {
            continue;
        }
        int node = -1;
        Unit unit = units[i];
        if (i < 2) {
            node = json.field(json.field(0, "temperature"), i == 0 ? "engine" : "oil");
        } else if (i == Rpm) {
            node = json.field(0, "RPMSpeed");
        } else if (i == Speed) {
            node = json.field(0, "speedActual");
        } else if (i == BatterySoc) {
            node = json.field(0, "battery");
        } else {
            node = json.field(json.field(0, "averageConsumption"), "averageConsumption1");
        }
        double number = 0;
        if (node < 0) {
            rows_[i].error = MissingField;
            continue;
        }
        if (!json.number(node, number) || number < low[i] || number > high[i] ||
            ((i == Rpm || i == BatterySoc) && floor(number) != number)) {
            rows_[i].error = BadValue;
            continue;
        }
        if (i == AverageConsumption) {
            double code = 0;
            if (!json.integer(json.field(json.field(0, "averageConsumption"), "unit"), code) || code < 1 || code > 4 ||
                floor(code) != code) {
                rows_[i].error = UnknownUnit;
                continue;
            }
            const Unit consumption[4] = {Litres100km, MpgUk, MpgUs, KmL};
            unit = consumption[static_cast<unsigned>(code) - 1];
        }
        Reading& row = rows_[i];
        row.value = number;
        row.unit = unit;
        row.received_at = now;
        row.error = NoError;
        row.present = true;
    }
    return true;
}
void Model::unavailable(unsigned property, double now) {
    if (property < kProperties && !stopped_ && clock(now)) {
        fail(property, NotAvailable);
    }
}
void Model::enable_gearbox(double now) {
    if (stopped_ || !clock(now)) {
        return;
    }
    rows_[GearboxTemperature].unit = Celsius;
    rows_[GearboxTemperature].error = Waiting;
}
void Model::gearbox(double value, bool valid, double now) {
    if (stopped_ || !clock(now)) {
        return;
    }
    Reading& r = rows_[GearboxTemperature];
    if (!valid || !isfinite(value) || value < -40 || value > 205) {
        r.error = NotAvailable;
        return;
    }
    r.value = value;
    r.received_at = now;
    r.unit = Celsius;
    r.error = NoError;
    r.present = true;
}
bool Model::diagnostic(unsigned ecu, unsigned did, const unsigned char* data, unsigned size, double now) {
    if (stopped_ || !isfinite(now) || now < last_time_) {
        return false;
    }
    DiagnosticRecord record;
    if (!decode_diagnostic(ecu, did, data, size, record)) {
        return false;
    }
    if (!clock(now)) {
        return false;
    }
    const Reading previous_charge = rows_[RecuperatedCharge];
    for (unsigned i = 0; i < record.count; ++i) {
        const DiagnosticSample& s = record.samples[i];
        Reading& r = rows_[s.metric];
        if (!s.valid) {
            r.error = NotAvailable;
            continue;
        }
        r.value = s.value;
        r.unit = s.unit;
        r.received_at = now;
        r.error = NoError;
        r.present = true;
    }
    if (ecu == 0x12 && did == 0x40c0) {
        const Reading& charge = rows_[RecuperatedCharge];
        Reading& active = rows_[RecuperationActive];
        active.unit = BooleanValue;
        const double dt = now - previous_charge.received_at;
        // A single sample, reset/wrap, reconnect/error, or long gap cannot
        // establish activity. Duplicate timestamps never refresh the result.
        if (charge.error != NoError || !previous_charge.present || previous_charge.error != NoError || dt > 3 ||
            charge.value < previous_charge.value) {
            active.error = NotAvailable;
        } else if (dt > 0) {
            active.value = charge.value > previous_charge.value ? 1 : 0;
            active.received_at = now;
            active.error = NoError;
            active.present = true;
        }
    }
    return true;
}
void Model::enable_diagnostics(unsigned ecu, double now) {
    if (stopped_ || !clock(now)) {
        return;
    }
    unsigned count = 0;
    const DiagnosticSource* sources = diagnostic_sources(ecu, true, count);
    const unsigned char zero[16] = {};
    for (unsigned i = 0; i < count; ++i) {
        DiagnosticRecord record;
        if (!decode_diagnostic(ecu, sources[i].did, zero, sources[i].size, record)) {
            continue;
        }
        for (unsigned n = 0; n < record.count; ++n) {
            Reading& r = rows_[record.samples[n].metric];
            r.unit = record.samples[n].unit;
            if (!r.present) {
                r.error = Waiting;
            }
        }
    }
    if (ecu == 0x12 && !rows_[RecuperationActive].present) {
        rows_[RecuperationActive].unit = BooleanValue;
        rows_[RecuperationActive].error = Waiting;
    }
}
void Model::diagnostic_unavailable(unsigned ecu, unsigned did, double now) {
    if (stopped_ || !clock(now)) {
        return;
    }
    unsigned count = 0;
    const DiagnosticSource* sources = diagnostic_sources(ecu, true, count);
    const unsigned char zero[16] = {};
    for (unsigned i = 0; i < count; ++i) {
        if (did && sources[i].did != did) {
            continue;
        }
        DiagnosticRecord record;
        if (!decode_diagnostic(ecu, sources[i].did, zero, sources[i].size, record)) {
            continue;
        }
        for (unsigned n = 0; n < record.count; ++n) {
            rows_[record.samples[n].metric].error = NotAvailable;
        }
    }
    if (ecu == 0x12 && (!did || did == 0x40c0)) {
        rows_[RecuperationActive].error = NotAvailable;
    }
}
void Model::stop(double now) {
    if (!clock(now)) {
        return;
    }
    stopped_ = true;
    for (unsigned i = 0; i < MetricCount; ++i) {
        if (rows_[i].error != SourceUnknown || rows_[i].present) {
            rows_[i].error = Stopped;
        }
    }
}
Status Model::status(unsigned i, double now) const {
    if (i >= MetricCount || !isfinite(now) || now < last_time_) {
        return Unavailable;
    }
    const Reading& r = rows_[i];
    if (!r.present) {
        return r.error == Waiting && now - opened_at_ < 5 ? Loading : Unavailable;
    }
    double age = (i == Rpm || i == Speed) ? 2
                 : (i == OilPressure || i == BatteryCurrent || i == GearSelector || i == GearMode ||
                    i == ConverterLock || i == ConverterRpm || i == OutputShaftRpm || i == TurbineRpm ||
                    i == EgsEngineRpm || i == TransmissionGearCode || i == EngineLoad || i == EngineTorque ||
                    i == GearLever || i == ParkingBrake || i == RecuperatedCharge)
                     ? 5
                 : i == RecuperationActive ? 3
                                           : 30;
    return r.error != NoError || now - r.received_at >= age ? Stale : Valid;
}
const Reading& Model::reading(unsigned i) const {
    return rows_[i < MetricCount ? i : MetricCount - 1];
}
unsigned Model::preferred_soc_metric(double now) const {
    const Reading& dde = rows_[BatterySocDde];
    const Reading& cds = rows_[BatterySoc];
    if (dde.present && status(BatterySocDde, now) == Valid) {
        return BatterySocDde;
    }
    if (cds.present && status(BatterySoc, now) == Valid) {
        return BatterySoc;
    }
    return dde.present && (!cds.present || dde.received_at >= cds.received_at) ? BatterySocDde : BatterySoc;
}
const char* status_name(Status s) {
    const char* names[] = {"loading", "valid", "stale", "unavailable"};
    return names[s];
}
const char* metric_name(unsigned i) {
    const char* names[] = {"engine_temperature",
                           "oil_temperature",
                           "rpm",
                           "speed",
                           "average_consumption",
                           "battery_soc",
                           "boost",
                           "intake_temperature",
                           "exhaust_temperature",
                           "voltage",
                           "fuel_pressure",
                           "rail_pressure",
                           "engine_load",
                           "gearbox_temperature",
                           "oil_pressure",
                           "battery_current",
                           "battery_temperature",
                           "gear_selector",
                           "gear_mode",
                           "converter_lock",
                           "converter_rpm",
                           "output_shaft_rpm",
                           "turbine_rpm",
                           "egs_engine_rpm",
                           "transmission_gear_code",
                           "engine_torque",
                           "battery_soc_dde",
                           "gear_lever",
                           "parking_brake",
                           "recuperated_charge",
                           "recuperation_active"};
    return i < MetricCount ? names[i] : "unknown";
}
const char* unit_name(Unit u) {
    const char* names[] = {"",  "C", "rpm", "km/h", "l/100km", "mpg_uk", "mpg_us", "km/l", "%", "bar",
                           "A", "V", "",    "",     "",        "",       "Nm",     "",     "",  "As"};
    return unsigned(u) < sizeof(names) / sizeof(names[0]) ? names[u] : "";
}
unsigned metric_model(unsigned i) {
    const SourceBinding* b = source_binding(i);
    return b ? b->hmi_model : 0;
}
const char16_t* enum_value(Unit unit, double value) {
    if (!isfinite(value) || value < 0 || value > 255 || floor(value) != value) {
        return u"?";
    }
    unsigned n = static_cast<unsigned>(value);
    if (unit == SelectorPosition) {
        const char16_t* names[] = {u"P", u"R", u"N", u"1", u"2", u"3", u"4", u"5", u"6", u"7"};
        return n < 10 ? names[n] : n == 16 ? u"8" : u"?";
    }
    if (unit == DriveMode) {
        const char16_t* names[] = {u"P", u"R", u"N", u"D", u"M", u"S"};
        return n < 6 ? names[n] : u"?";
    }
    if (unit == LeverCode) {
        const char16_t* names[] = {u"Исходное", u"Кнопка P", u"R", u"N", u"D", u"M−", u"M+", u"M/S"};
        return n < 8 ? names[n] : u"?";
    }
    if (unit == GearCode) {
        const char16_t* names[] = {u"?", u"1", u"2", u"3", u"4", u"5", u"6", u"7", u"8"};
        return n < 9 ? names[n] : n == 255 ? u"R" : u"?";
    }
    if (unit == LockState) {
        const char16_t* names[] = {u"Открыта", u"Регулирование (низк.)", u"Регулирование (выс.)", u"Замкнута (низк.)",
                                   u"Замкнута (выс.)"};
        const char16_t* transitions[] = {u"Открывается", u"К регулир. (низк.)", u"К регулир. (выс.)",
                                         u"К замыканию (низк.)", u"К замыканию (выс.)"};
        return n < 5 ? names[n] : n >= 8 && n <= 12 ? transitions[n - 8] : u"?";
    }
    return u"?";
}
}  // namespace telemetry
