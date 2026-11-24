// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The ServiceDiscoveryResponse that describes this car to the phone, built from Google's DHU template.
#ifndef ENTRY_AAD_CORE_SDR_BUILDER_HPP
#define ENTRY_AAD_CORE_SDR_BUILDER_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "display_plan.hpp"

namespace aa {

// The side the driver sits on, as Android Auto numbers it (ServiceDiscoveryResponse field 6): the phone lays its
// screen out for that side.
enum class DriverPosition : uint8_t { kLeft = 0, kRight = 1 };
// The car's fuel, as Android Auto numbers it (SensorSourceService field 3). Diesel is FUEL_TYPE_DIESEL_2, the
// ordinary road diesel (DIESEL_1 is a lighter winter grade).
enum class FuelType : uint8_t { kUnleaded = 1, kDiesel = 4 };

// What the phone is told about the car and the head unit.
struct SdrIdentity {
    std::string carMake, carModel, carYear, vehicleId;
    std::string huMake, huModel, swBuild, swVersion;
    DriverPosition driverPosition;
    FuelType fuel;
    // A left-hand drive petrol BMW "iDrive" of 2019 on an EntryNav2, swVersion "0.0.0": what is announced until the
    // vehicle file and the program give their own.
    SdrIdentity();
    // The car as written at installation, one setting per line: "model=320d xDrive", "year=2021", "side=left" or
    // "side=right", "fuel=petrol" or "fuel=diesel". A missing file or an invalid line keeps the default values. False
    // if unreadable.
    bool loadVehicleFile(const char* path);
};
// Accepted model and year: 1 to 24 of A-Z a-z 0-9 space . _ -, and 1xxx or 2xxx.
bool validCarModel(const std::string& s);
bool validCarYear(const std::string& s);

// The steps of buildCarServiceDiscovery, in its order; each prints what it changed.
// The identity strings replaced, at the top level and in the head unit info (field 17), and the driver's side
// written; everything else untouched.
std::vector<uint8_t> buildServiceDiscovery(const uint8_t* base, size_t baseLen, const SdrIdentity& id);
// The sensor source (channel 1) with the car's one fuel and no EV connector, instead of the template's unleaded plus
// electric and its Tesla Supercharger connector. False when there is no sensor source or it is malformed.
bool declareFuel(std::vector<uint8_t>& sdr, FuelType fuel);
// Main display (channel 6): the plan's canvas, density 140 (seen on the car: the rail icons no longer overlap) and
// the insets that keep the phone's UI in the part of the canvas the panel shows 1:1. On the reference car: the
// 1280x720 canvas and a bottom inset of 240, its top 480 rows shown.
bool tuneMainDisplay(std::vector<uint8_t>& sdr, const DisplayPlan& display = DisplayPlan());
// The touchscreen (channel 7) with the size of the phone's canvas: touch coordinates go through unscaled.
bool declareTouchscreen(std::vector<uint8_t>& sdr, const DisplayPlan& display = DisplayPlan());
// No media sink (channel 9): media plays over the car's Bluetooth. The phone refuses the session without an audio
// sink or without the microphone, so 8 (speech and guidance) and 14 stay. Returns the descriptors removed.
int removeMediaChannel(std::vector<uint8_t>& sdr);
// The channel 8 sink as SYSTEM_AUDIO instead of GUIDANCE: with neither a guidance nor a media sink the phone mixes
// the prompts into its own audio, which plays over Bluetooth. Returns the fields changed (0 or 1).
int retypeGuidanceSink(std::vector<uint8_t>& sdr);

// The platform's last word on the description, after removeMediaChannel (platform/evo_sdr_sensors.cpp registers
// one: the sensor list cut to what the car feeds). Returns what it removed.
using DiscoveryTrim = int (*)(std::vector<uint8_t>& sdr);
void setDiscoveryTrim(DiscoveryTrim trim);

// What the phone is told about this car: the template adapted by every step above. vehicleFile: the model, year,
// side and fuel (SdrIdentity::loadVehicleFile), given by the platform; null keeps the default ones. softwareVersion:
// the version of the program, announced as the head unit's software version; null keeps "0.0.0". display: the panel
// and the canvas chosen for it; the reference car's when not given. make: the car's and the head unit's maker as
// iDrive is coded ("MINI"); null keeps "BMW". codedSide: the driver's side as the car is coded, used unless the
// vehicle file says "side=". codedModel: the car's series as iDrive is coded ("G20"), used unless the vehicle file
// says "model="; null or invalid keeps "iDrive".
std::vector<uint8_t> buildCarServiceDiscovery(const uint8_t* base, size_t baseLen, const char* vehicleFile,
                                              const char* softwareVersion, const DisplayPlan& display = DisplayPlan(),
                                              const char* make = nullptr,
                                              DriverPosition codedSide = DriverPosition::kLeft,
                                              const char* codedModel = nullptr);

}  // namespace aa

#endif  // ENTRY_AAD_CORE_SDR_BUILDER_HPP
