// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Builds the car's ServiceDiscoveryResponse: identity, driver side, fuel, main display, touchscreen and audio channels.
#include "sdr_builder.hpp"

#include <cstdio>

#include "aa_protocol.hpp"
#include "pb.hpp"

namespace aa {
namespace {

typedef std::vector<uint8_t> Bytes;

// Field numbers along the paths edited here.
const uint32_t kServiceField = 1;        // top level: one per channel
const uint32_t kDriverSideField = 6;     // top level: DriverPosition
const uint32_t kHeadUnitInfoField = 17;  // top level: copies of the identity strings
const uint32_t kChannelIdField = 1;      // in a service
const uint32_t kMediaSinkField = 3;      // in a service
const uint32_t kInputSourceField = 4;    // in a service
const uint32_t kAudioTypeField = 2;      // in a media sink
const uint32_t kVideoConfigField = 4;    // in a media sink
const uint32_t kResolutionField = 1;     // in a video config: the canvas, 1 800x480, 2 1280x720, 3 1920x1080
const uint32_t kWidthMarginField = 3;    // in a video config, with kHeightMarginField
const uint32_t kHeightMarginField = 4;
const uint32_t kDensityField = 5;  // in a video config, with kRealDensityField
const uint32_t kRealDensityField = 9;
const uint32_t kUiConfigField = 11;    // in a video config: the insets as its field 1, then theme and the like
const uint32_t kInsetsField = 1;       // top, bottom, left, right
const uint32_t kTouchscreenField = 2;  // in an input source
const uint32_t kWidthField = 1;        // in a touchscreen, with kHeightField
const uint32_t kHeightField = 2;
const uint32_t kSensorSourceField = 2;   // in a service
const uint32_t kSourceFuelTypes = 3;     // in a sensor source, repeated
const uint32_t kSourceEvConnectors = 4;  // in a sensor source, repeated

const uint32_t kDensity = 140;
const uint64_t kGuidanceAudio = 1;
const uint8_t kSystemAudio = 2;

// Each identity string: its field at the top level, its field in the head unit info, its value.
struct IdentityField {
    uint32_t top;
    uint32_t info;
    std::string SdrIdentity::*value;
};
const IdentityField kIdentity[] = {{2, 1, &SdrIdentity::carMake}, {3, 2, &SdrIdentity::carModel},
                                   {4, 3, &SdrIdentity::carYear}, {5, 4, &SdrIdentity::vehicleId},
                                   {7, 5, &SdrIdentity::huMake},  {8, 6, &SdrIdentity::huModel},
                                   {9, 7, &SdrIdentity::swBuild}, {10, 8, &SdrIdentity::swVersion}};

// A string field of the identity, replaced by its value; false for any other field.
bool replaceIdentity(const pb::Field& field, Bytes& out, const SdrIdentity& id, uint32_t IdentityField::*column) {
    if (field.type != pb::kBytes) {
        return false;
    }
    for (size_t i = 0; i < sizeof(kIdentity) / sizeof(kIdentity[0]); ++i) {
        if (kIdentity[i].*column == field.number) {
            pb::appendString(out, field.number, id.*(kIdentity[i].value));
            return true;
        }
    }
    return false;
}

// A sub-message rewritten by `edit` and written back under its own number; false (nothing written) when malformed.
template <typename Edit>
bool rewriteInner(const pb::Field& field, Bytes& out, Edit edit) {
    Bytes inner;
    if (!pb::rewriteMessage(field.data, field.size, inner, edit)) {
        return false;
    }
    pb::appendMessage(out, field.number, inner);
    return true;
}

// The channel a top-level service describes; none when the field is not a service.
bool isChannel(const pb::Field& field, uint8_t channel) {
    uint64_t id = 0;
    return pb::isMessage(field, kServiceField) && pb::findVarint(field.data, field.size, kChannelIdField, id) &&
           id == channel;
}

// The services of one channel rewritten by `edit`, the rest copied. False when the description is malformed; else
// `changed` tells whether such a service was rewritten.
template <typename Edit>
bool rewriteChannel(Bytes& sdr, uint8_t channel, Edit edit, bool& changed) {
    Bytes out;
    changed = false;
    const bool ok = pb::rewriteMessage(sdr.data(), sdr.size(), out, [&](const pb::Field& service, Bytes& to) -> bool {
        const bool done = isChannel(service, channel) && rewriteInner(service, to, edit);
        changed = changed || done;
        return done;
    });
    if (ok && changed) {
        sdr.swap(out);
    }
    return ok;
}

// The video config of the main display: the canvas, both densities set, and the UI config written last, its insets
// replaced. The template's two margins are what the reference car has always sent on its 1280x720 canvas and stay
// on that canvas; on another one they are zero, the insets alone placing the UI.
bool tuneVideoConfig(const pb::Field& config, Bytes& out, const DisplayPlan& display) {
    Bytes ui;
    Bytes body;
    bool templateCanvas = true;
    const bool ok = pb::rewriteMessage(config.data, config.size, body, [&](const pb::Field& field, Bytes& to) -> bool {
        if (field.type == pb::kVarint && field.number == kResolutionField) {
            templateCanvas = field.value == display.resolution;
            pb::appendUint(to, field.number, display.resolution);
            return true;
        }
        if (field.type == pb::kVarint && !templateCanvas &&
            (field.number == kWidthMarginField || field.number == kHeightMarginField)) {
            pb::appendUint(to, field.number, 0);
            return true;
        }
        if (field.type == pb::kVarint && (field.number == kDensityField || field.number == kRealDensityField)) {
            pb::appendUint(to, field.number, kDensity);
            return true;
        }
        if (pb::isMessage(field, kUiConfigField)) {
            ui.assign(field.data, field.data + field.size);  // rebuilt below
            return true;
        }
        return false;
    });
    if (!ok) {
        return false;
    }
    Bytes insets;
    pb::appendUint(insets, 1, 0);  // top
    pb::appendUint(insets, 2, display.bottomInset());
    pb::appendUint(insets, 3, display.leftInset());
    pb::appendUint(insets, 4, display.rightInset());
    Bytes rebuilt;
    pb::appendMessage(rebuilt, kInsetsField, insets);
    // The template's other UI settings stay, after the insets.
    pb::rewriteMessage(ui.data(), ui.size(), rebuilt,
                       [](const pb::Field& field, Bytes&) -> bool { return field.number == kInsetsField; });
    pb::appendMessage(body, kUiConfigField, rebuilt);
    pb::appendMessage(out, config.number, body);
    return true;
}

}  // namespace

SdrIdentity::SdrIdentity()
    : carMake("BMW"),
      // No model name is stored on the head unit, only its series (car_series.hpp): "iDrive" until the coding or the
      // vehicle file gives one.
      carModel("iDrive"),
      carYear("2019"),
      // The phone keeps its per-car settings under this id: never change it.
      vehicleId("3b9e7d52-1c4f-4a8e-b6d0-5f2a9c81e4b3"),
      huMake("BMW"),
      huModel("EntryNav2"),
      swBuild("entry-aad"),
      swVersion("0.0.0"),
      driverPosition(DriverPosition::kLeft),
      fuel(FuelType::kUnleaded) {}

bool validCarModel(const std::string& s) {
    if (s.empty() || s.size() > 24) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' ||
                             c == '.' || c == '_' || c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

bool validCarYear(const std::string& s) {
    bool ok = s.size() == 4 && (s[0] == '1' || s[0] == '2');
    for (size_t i = 1; ok && i < 4; ++i) {
        ok = s[i] >= '0' && s[i] <= '9';
    }
    return ok;
}

bool SdrIdentity::loadVehicleFile(const char* path) {
    FILE* file = fopen(path, "r");
    if (!file) {
        return false;
    }
    char text[64];
    while (fgets(text, sizeof(text), file)) {
        std::string line(text);
        while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r')) {
            line.erase(line.size() - 1);
        }
        if (line.compare(0, 6, "model=") == 0 && validCarModel(line.substr(6))) {
            carModel = line.substr(6);
        } else if (line.compare(0, 5, "year=") == 0 && validCarYear(line.substr(5))) {
            carYear = line.substr(5);
        } else if (line.compare(0, 5, "side=") == 0) {
            const std::string side = line.substr(5);
            if (side == "right") {
                driverPosition = DriverPosition::kRight;
            } else if (side == "left") {
                driverPosition = DriverPosition::kLeft;
            }
        } else if (line.compare(0, 5, "fuel=") == 0) {
            const std::string kind = line.substr(5);
            if (kind == "diesel") {
                fuel = FuelType::kDiesel;
            } else if (kind == "petrol") {
                fuel = FuelType::kUnleaded;
            }
        }
    }
    fclose(file);
    return true;
}

std::vector<uint8_t> buildServiceDiscovery(const uint8_t* base, size_t baseLen, const SdrIdentity& id) {
    Bytes out;
    bool sideWritten = false;
    const bool ok = pb::rewriteMessage(base, baseLen, out, [&](const pb::Field& field, Bytes& to) -> bool {
        if (pb::isMessage(field, kHeadUnitInfoField)) {
            return rewriteInner(field, to, [&](const pb::Field& info, Bytes& into) -> bool {
                return replaceIdentity(info, into, id, &IdentityField::info);
            });
        }
        if (field.type == pb::kVarint && field.number == kDriverSideField) {
            pb::appendUint(to, field.number, static_cast<uint64_t>(id.driverPosition));
            sideWritten = true;
            return true;
        }
        return replaceIdentity(field, to, id, &IdentityField::top);
    });
    if (!ok) {
        return Bytes(base, base + baseLen);
    }
    if (!sideWritten) {
        pb::appendUint(out, kDriverSideField, static_cast<uint64_t>(id.driverPosition));
    }
    return out;
}

bool declareFuel(std::vector<uint8_t>& sdr, FuelType fuel) {
    bool changed = false;
    const bool ok = rewriteChannel(
        sdr, channel::kSensors,
        [&](const pb::Field& part, Bytes& to) -> bool {
            if (!pb::isMessage(part, kSensorSourceField)) {
                return false;
            }
            // Every fuel and connector of the template dropped, then the car's one fuel.
            Bytes source;
            if (!pb::rewriteMessage(part.data, part.size, source, [](const pb::Field& field, Bytes&) -> bool {
                    return field.number == kSourceFuelTypes || field.number == kSourceEvConnectors;
                })) {
                return false;
            }
            pb::appendUint(source, kSourceFuelTypes, static_cast<uint64_t>(fuel));
            pb::appendMessage(to, part.number, source);
            return true;
        },
        changed);
    if (ok && changed) {
        printf("[SDR] sensor source: fuel %s, no EV connector\n", fuel == FuelType::kDiesel ? "diesel" : "petrol");
    }
    return ok && changed;
}

bool tuneMainDisplay(std::vector<uint8_t>& sdr, const DisplayPlan& display) {
    bool changed = false;
    const bool ok = rewriteChannel(
        sdr, channel::kVideo,
        [&](const pb::Field& field, Bytes& to) -> bool {
            return pb::isMessage(field, kMediaSinkField) &&
                   rewriteInner(field, to, [&](const pb::Field& sink, Bytes& into) -> bool {
                       return pb::isMessage(sink, kVideoConfigField) && tuneVideoConfig(sink, into, display);
                   });
        },
        changed);
    if (ok && changed) {
        // The margins as sent (top, bottom, left, right): the left one is the MINI's surround.
        printf("[SDR] main VideoConfig: canvas %ux%u for a %ux%u panel, dpi=%u insets=(0,%u,%u,%u)\n",
               display.canvasWidth, display.canvasHeight, display.width, display.height, kDensity,
               display.bottomInset(), display.leftInset(), display.rightInset());
    }
    return ok && changed;
}

bool declareTouchscreen(std::vector<uint8_t>& sdr, const DisplayPlan& display) {
    bool changed = false;
    const bool ok = rewriteChannel(
        sdr, channel::kInput,
        [&](const pb::Field& field, Bytes& to) -> bool {
            return pb::isMessage(field, kInputSourceField) &&
                   rewriteInner(field, to, [&](const pb::Field& source, Bytes& into) -> bool {
                       return pb::isMessage(source, kTouchscreenField) &&
                              rewriteInner(source, into, [&](const pb::Field& size, Bytes& sized) -> bool {
                                  if (size.type != pb::kVarint ||
                                      (size.number != kWidthField && size.number != kHeightField)) {
                                      return false;
                                  }
                                  pb::appendUint(
                                      sized, size.number,
                                      size.number == kWidthField ? display.canvasWidth : display.canvasHeight);
                                  return true;
                              });
                   });
        },
        changed);
    if (ok && changed) {
        printf("[SDR] touchscreen declared %ux%u\n", display.canvasWidth, display.canvasHeight);
    }
    return ok && changed;
}

int removeMediaChannel(std::vector<uint8_t>& sdr) {
    int removed = 0;
    Bytes out;
    const bool ok = pb::rewriteMessage(sdr.data(), sdr.size(), out, [&](const pb::Field& service, Bytes&) -> bool {
        if (!isChannel(service, channel::kMediaAudio)) {
            return false;
        }
        printf("[SDR] dropped channel %u (media over Bluetooth)\n", channel::kMediaAudio);
        ++removed;
        return true;
    });
    if (!ok) {
        return 0;
    }
    if (removed) {
        sdr.swap(out);
    }
    return removed;
}

// In place: GUIDANCE (1) and SYSTEM_AUDIO (2) are both one-byte values.
int retypeGuidanceSink(std::vector<uint8_t>& sdr) {
    int changed = 0;
    pb::FieldReader services(sdr.data(), sdr.size());
    pb::Field service;
    while (services.next(service)) {
        if (!isChannel(service, channel::kGuidanceAudio)) {
            continue;
        }
        pb::FieldReader parts(service.data, service.size);
        pb::Field sink;
        while (parts.next(sink)) {
            if (!pb::isMessage(sink, kMediaSinkField)) {
                continue;
            }
            pb::FieldReader settings(sink.data, sink.size);
            pb::Field type;
            while (settings.next(type)) {
                if (type.type == pb::kVarint && type.number == kAudioTypeField && type.value == kGuidanceAudio &&
                    type.end - type.begin == 2) {
                    sdr[static_cast<size_t>(sink.data - sdr.data()) + type.end - 1] = kSystemAudio;
                    ++changed;
                    printf("[SDR] channel 8 audio sink: GUIDANCE -> SYSTEM_AUDIO (nav prompts over Bluetooth)\n");
                }
            }
        }
    }
    return changed;
}

namespace {
DiscoveryTrim g_trim = nullptr;
}  // namespace

void setDiscoveryTrim(DiscoveryTrim trim) {
    g_trim = trim;
}

std::vector<uint8_t> buildCarServiceDiscovery(const uint8_t* base, size_t baseLen, const char* vehicleFile,
                                              const char* softwareVersion, const DisplayPlan& display, const char* make,
                                              DriverPosition codedSide, const char* codedModel) {
    SdrIdentity identity;
    identity.driverPosition = codedSide;  // before the file, whose "side=" and "model=" win
    if (codedModel && validCarModel(codedModel)) {
        identity.carModel = codedModel;
    }
    if (vehicleFile) {
        identity.loadVehicleFile(vehicleFile);
    }
    if (make && *make) {
        identity.carMake = identity.huMake = make;
    }
    if (softwareVersion && *softwareVersion) {
        identity.swVersion = softwareVersion;
    }
    std::vector<uint8_t> sdr = buildServiceDiscovery(base, baseLen, identity);
    printf("[SDR] ServiceDiscovery identity: %s %s %s, %s-hand drive (BT channel: none)\n", identity.carMake.c_str(),
           identity.carModel.c_str(), identity.carYear.c_str(),
           identity.driverPosition == DriverPosition::kRight ? "right" : "left");
    declareFuel(sdr, identity.fuel);
    tuneMainDisplay(sdr, display);
    declareTouchscreen(sdr, display);
    removeMediaChannel(sdr);
    if (g_trim) {
        g_trim(sdr);
    }
    retypeGuidanceSink(sdr);
    return sdr;
}

}  // namespace aa
