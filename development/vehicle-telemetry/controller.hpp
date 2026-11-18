// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "egs_reader.hpp"
#include "native_cds.hpp"
#include "telemetry_model.hpp"
#ifdef TELEMETRY_GRAPHICS
#include "dashboard/panel_data.hpp"
#endif

namespace telemetry {
#ifdef TELEMETRY_GRAPHICS
class DashboardRenderer;
#endif
// Fixed storage across unlimited visits. NativeCds generation cookies reject
// late callbacks without keeping a heap allocation for every old visit.
class Controller : public Sink {
public:
    Controller(bmw::PluginApp&, double);
    ~Controller();
    bool show(double);
    void hide(double);
    void tick(double);
#ifdef TELEMETRY_GRAPHICS
    void dashboard(bool, double);
    void prime_dashboard(double);
#endif
    void event(const char*, unsigned, const char*, double);
    const Model& model() const { return model_; }

private:
    bmw::PluginApp& app_;
    Model model_;
    NativeCds transport_;
    Probe probe_;
    EgsReader egs_;
    bool shown_;
    double render_due_;
#ifdef TELEMETRY_GRAPHICS
    DashboardRenderer* dashboard_;
    bool dashboard_shown_, dashboard_force_, recording_;
    double demo_opened_, demo_updated_, recording_time_;
    PanelExtras panel_extras_;
    void demo(double);
#endif
    void render(double);
    Controller(const Controller&);
    Controller& operator=(const Controller&);
};
}  // namespace telemetry
