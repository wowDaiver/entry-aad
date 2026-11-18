// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller.hpp"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trial_guard.hpp"
#ifdef TELEMETRY_GRAPHICS
#include "dashboard/native_image.hpp"
#include "dashboard/renderer.hpp"
#ifdef TELEMETRY_EMULATOR_ONLY
#include "dashboard/demo_fixture.hpp"
#include "dashboard/recording.hpp"
#endif
#include <time.h>

#include <new>
#ifndef TELEMETRY_DASHBOARD_FONT
#define TELEMETRY_DASHBOARD_FONT "/opt/hmi/data/ro/mini/font/MINI_TA_RG.ttf"
#endif
#endif

namespace telemetry {
namespace {
bool emulator_demo() {
#ifdef TELEMETRY_EMULATOR_ONLY
    const char* setting = getenv("VEHICLE_TELEMETRY_DEMO");
    return setting && !strcmp(setting, "battery-and-temperatures");
#else
    return false;
#endif
}
// The native const-char overload decodes Latin-1. Build UTF-16 explicitly for
// Cyrillic labels and pass the native Unicode overload; no locale conversion.
struct HmiLine {
    unsigned short text[192];
    unsigned used;
    HmiLine() : used(0) { text[0] = 0; }
    void add(const char16_t* value) {
        for (; *value && used + 1 < sizeof(text) / sizeof(text[0]); ++value) {
            text[used++] = static_cast<unsigned short>(*value);
        }
        text[used] = 0;
    }
    void ascii(const char* value) {
        for (; *value && used + 1 < sizeof(text) / sizeof(text[0]); ++value) {
            text[used++] = static_cast<unsigned char>(*value) < 128 ? static_cast<unsigned char>(*value) : '?';
        }
        text[used] = 0;
    }
    void publish(bmw::PluginApp& app, unsigned model) {
        bmw::BasicString<unsigned short> value(text);
        app.updateModel(model, value);
    }
};
}  // namespace
Controller::Controller(bmw::PluginApp& app, double now)
    : app_(app),
      model_(now),
      transport_(app),
      probe_(transport_, *this),
      egs_(0, true),
      shown_(false),
      render_due_(0)
#ifdef TELEMETRY_GRAPHICS
      ,
      dashboard_(0),
      dashboard_shown_(false),
      dashboard_force_(false),
      recording_(false),
      demo_opened_(0),
      demo_updated_(-1),
      recording_time_(0)
#endif
{
}
Controller::~Controller() {
    hide(model_.last_time());
#ifdef TELEMETRY_GRAPHICS
    delete dashboard_;
#endif
}
bool Controller::show(double now) {
    if (shown_) {
        return true;
    }
    render_due_ = 0;
    trial_log("menu-show");
    if (emulator_demo()) {
        model_ = Model(now);
        shown_ = true;
#ifdef TELEMETRY_GRAPHICS
        demo_opened_ = now;
        demo_updated_ = -1;
        recording_ = false;
        panel_extras_ = PanelExtras();
        demo(now);
#endif
        render(now);
        return true;
    }
    if (!isfinite(now) || now < model_.last_time()) {
        return false;
    }
    model_ = Model(now);
    shown_ = true;
    if (!probe_.start(now)) {
        shown_ = false;
        return false;
    }
    model_.enable_diagnostics(0x12, model_.last_time());
    model_.enable_diagnostics(0x18, model_.last_time());
    egs_.start(model_.last_time());
    render(now < model_.last_time() ? model_.last_time() : now);
    return true;
}
void Controller::hide(double now) {
    if (!isfinite(now) || now < model_.last_time()) {
        now = model_.last_time();
    }
    shown_ = false;
    egs_.stop();
    probe_.stop(now);
    model_.stop(now);
}
void Controller::tick(double now) {
    if (shown_ && emulator_demo()) {
#ifdef TELEMETRY_GRAPHICS
        double previous = demo_updated_;
        demo(now);
        if (previous == demo_updated_) {
            return;
        }
#endif
        render(now);
        return;
    }
    if (!shown_) {
        return;
    }
    probe_.poll(now);
    if (now < model_.last_time()) {
        now = model_.last_time();
    }
    EgsReader* readers[] = {&egs_};
    for (unsigned i = 0; i < 1; ++i) {
        readers[i]->poll(now);
        DiagnosticReply reply = {};
        if (!readers[i]->take_record(reply)) {
            continue;
        }
        bool valid = reply.valid && model_.diagnostic(reply.ecu, reply.did, reply.data, reply.size, now);
        if (!valid) {
            if (reply.ecu) {
                model_.diagnostic_unavailable(reply.ecu, reply.did, now);
            } else {
                model_.diagnostic_unavailable(0x12, 0, now);
                model_.diagnostic_unavailable(0x18, 0, now);
            }
        }
        char raw[33] = {};
        for (unsigned n = 0; n < reply.size && n < 16; ++n) {
            snprintf(raw + n * 2, sizeof(raw) - n * 2, "%02x", reply.data[n]);
        }
        if (!valid) {
            snprintf(raw, sizeof(raw), "err=%u", reply.failure);
        }
        trial_log(valid ? "diagnostic" : "diagnostic-unavailable", (reply.ecu << 16) | reply.did, raw);
    }
    render(now < model_.last_time() ? model_.last_time() : now);
}
void Controller::event(const char* kind, unsigned property, const char* payload, double now) {
    // Only the five fixed non-identifying CDS properties reach this bounded log.
    trial_log(kind, property, payload);
    if (!strcmp(kind, "sample") || !strcmp(kind, "invalid_payload")) {
        model_.update(property, payload, now);
    } else if (!strcmp(kind, "unavailable")) {
        model_.unavailable(property, now);
    } else if (!strcmp(kind, "stopped")) {
        model_.stop(now);
    }
}
#ifdef TELEMETRY_GRAPHICS
void Controller::dashboard(bool shown, double now) {
    dashboard_shown_ = shown;
    dashboard_force_ = shown;
    if (shown_) {
        render(now);
    }
}
void Controller::prime_dashboard(double now) {
    bool previous = dashboard_shown_;
    dashboard_shown_ = true;
    dashboard_force_ = true;
    try {
        render(now);
    } catch (...) {
        dashboard_shown_ = previous;
        throw;
    }
    dashboard_shown_ = previous;
}
void Controller::demo(double now) {
#ifdef TELEMETRY_EMULATOR_ONLY
    // Emulator-only caller. These are test fixtures, never recorded vehicle values.
    double second = floor(now - demo_opened_);
    if (second == demo_updated_) {
        return;
    }
    demo_updated_ = second;
    const char* scenario = "live";
    FILE* f = fopen("/tmp/telemetry-dashboard-scenario", "r");
    char name[24] = {};
    if (f) {
        if (fgets(name, sizeof(name), f)) {
            name[strcspn(name, "\r\n")] = 0;
            scenario = name;
        }
        fclose(f);
    }
    if (!strcmp(scenario, "recorded")) {
        if (!recording_) {
            recording_time_ = now;
            model_ = Model(now);
            panel_extras_ = PanelExtras();
            FILE* capture = fopen("/tmp/telemetry-dashboard-recording", "r");
            bool loaded = load_recording(capture, model_, now);
            if (capture) {
                fclose(capture);
            }
            fprintf(stderr, "VEHICLE_TELEMETRY_VM offline recording loaded=%u\n", loaded ? 1 : 0);
        }
        recording_ = true;
        return;
    }
    if (recording_) {
        model_ = Model(now);
        panel_extras_ = PanelExtras();
        recording_ = false;
    }
    panel_demo(model_, panel_extras_, scenario, now, second);
#else
    (void)now;
#endif
}
#endif
void Controller::render(double now) {
    double interval = 1;
#ifdef TELEMETRY_GRAPHICS
    if (dashboard_shown_) {
        interval = .2;
    }
#endif
    if (now < render_due_) {
        return;
    }
    render_due_ = now + interval;
#ifdef TELEMETRY_GRAPHICS
    // The recorded-data viewer holds one archived snapshot, not a stream of new
    // measurements. Host replay tests exercise elapsed age separately.
    if (recording_) {
        now = recording_time_;
    }
    if (dashboard_shown_) {
        if (!dashboard_) {
            dashboard_ = new DashboardRenderer(DashboardWidth, DashboardHeight, TELEMETRY_DASHBOARD_FONT);
        }
        timespec begin = {};
        clock_gettime(CLOCK_MONOTONIC, &begin);
        const std::vector<unsigned char>& bytes =
            dashboard_->render(model_, now, emulator_demo(), panel_extras_, recording_);
        if (!dashboard_->changed() && !dashboard_force_) {
            return;
        }
        dashboard_force_ = false;
        alignas(bmw::Image) unsigned char storage[sizeof(bmw::Image)] = {};
        bmw::Image* image = new (storage) bmw::Image(bytes.data(), bytes.size());
        try {
            app_.updateModel(120, *image);
        } catch (...) {
            image->~Image();
            throw;
        }
        image->~Image();
        timespec end = {};
        clock_gettime(CLOCK_MONOTONIC, &end);
        double ms = (end.tv_sec - begin.tv_sec) * 1000. + (end.tv_nsec - begin.tv_nsec) / 1e6;
        fprintf(stderr, "VEHICLE_TELEMETRY_VM dashboard image=%zu submitted time=%.3f render_publish_ms=%.1f\n",
                bytes.size(), now, ms);
        return;
    }
#endif
    const char16_t* labels[] = {u"Темп. двигателя*",
                                u"Масло",
                                u"Обороты",
                                u"Скорость",
                                u"Средний расход",
                                u"Заряд АКБ*",
                                u"Наддув",
                                u"Темп. впуска",
                                u"Темп. выхлопа",
                                u"Напряжение",
                                u"Давл. топлива",
                                u"Давл. в рейке",
                                u"Нагрузка ДВС",
                                u"Темп. АКПП",
                                u"Давление масла",
                                u"Ток АКБ",
                                u"Темп. АКБ",
                                u"Передача",
                                u"Режим АКПП",
                                u"Блокировка ГТ",
                                u"Входной вал",
                                u"Выходной вал",
                                u"Обороты турбины",
                                u"Обороты ДВС (АКПП)",
                                u"Передача (внутр.)",
                                u"Момент ДВС",
                                u"Заряд АКБ (DDE)",
                                u"Рычаг АКПП",
                                u"Ручник",
                                u"Счётчик рекуперации",
                                u"Рекуперация"};
    static_assert(sizeof(labels) / sizeof(labels[0]) == MetricCount, "metric labels must be complete");
    const char16_t* states[] = {u"Получение данных", u"", u"Последние данные", u"Нет данных"};
    const char16_t* demo_values[MetricCount] = {
        u"92 °C (тест)", u"104 °C (тест)", 0, 0, 0, u"78 % (тест)", 0, u"29 °C (тест)", 0, u"12.6 В (тест)", 0, 0, 0,
        u"67 °C (тест)"};
    for (unsigned i = 0; i < MetricCount; ++i) {
        if (!metric_model(i)) {
            continue;
        }
        unsigned source = i == BatterySoc ? model_.preferred_soc_metric(now) : i;
        HmiLine line;
        Status status = model_.status(source, now);
        const Reading& row = model_.reading(source);
        line.add(source == BatterySocDde ? u"Заряд АКБ" : labels[i]);
        line.add(u": ");
        if (emulator_demo() && demo_values[i]
#ifdef TELEMETRY_GRAPHICS
            && false  // Graphic/text fallback share the same Model in this variant.
#endif
        ) {
            line.add(demo_values[i]);
            line.publish(app_, metric_model(i));
            continue;
        }
        if (row.present && (status == Valid || status == Stale)) {
            if (row.unit == SelectorPosition || row.unit == DriveMode || row.unit == LockState ||
                row.unit == GearCode) {
                line.add(enum_value(row.unit, row.value));
            } else {
                char number[32];
                snprintf(number, sizeof(number),
                         row.unit == Bars       ? "%.3f "
                         : row.unit == Amperes  ? "%+.1f "
                         : row.unit == GearCode ? "%.0f"
                                                : "%.1f ",
                         row.value);
                line.ascii(number);
                if (row.unit == Celsius) {
                    line.add(u"°C");
                } else if (row.unit == Volts) {
                    line.add(u"В");
                } else if (row.unit == Amperes) {
                    line.add(u"А");
                } else {
                    line.ascii(unit_name(row.unit));
                }
            }
            if (status == Stale) {
                line.add(u" ");
                line.add(states[status]);
            }
        } else {
            line.add(states[status]);
        }
        line.publish(app_, metric_model(i));
    }
    HmiLine note;
    const char16_t* note_text = emulator_demo() ? u"Эмулятор: тестовые данные" : u"* Датчики со * требуют проверки";
#ifdef TELEMETRY_GRAPHICS
    if (recording_) {
        note_text = u"Офлайн: запись ответов блоков";
    }
#endif
    note.add(note_text);
    note.publish(app_, 114);
}
}  // namespace telemetry
