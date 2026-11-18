// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <math.h>

namespace telemetry {
// Native child transitions can report parent=false before child=true. Keep a
// short exit grace period rather than starting a new data session per page.
class MenuVisibility {
public:
    MenuVisibility() : mask_(0), until_(0), last_(0) {}
    bool change(unsigned state, bool shown, double now) {
        if (state < 10 || state > 17 || !isfinite(now) || now < last_) {
            return false;
        }
        last_ = now;
        unsigned bit = 1u << (state - 10);
        bool was = mask_ != 0;
        if (shown) {
            mask_ |= bit;
        } else {
            mask_ &= ~bit;
        }
        if (was && !mask_) {
            until_ = now + 0.1;
        }
        return true;
    }
    bool active(double now) const { return isfinite(now) && now >= last_ && (mask_ || now < until_); }
    bool visible() const { return mask_ != 0; }
    void clear() {
        mask_ = 0;
        until_ = 0;
    }

private:
    unsigned mask_;
    double until_, last_;
};
}  // namespace telemetry
