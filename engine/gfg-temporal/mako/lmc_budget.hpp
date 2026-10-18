/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace mako::backend::gfg_temporal {

// GPU share and throughput are separate constraints. React in measured
// milliseconds, not pair counts. A repeat is limited to one pair: a block of
// 120 repeated pairs is a multi-second interruption, not a quality tier.
struct LmcBudget {
    enum class Action { keep, cheaper, better, repeat_one };
    double over_ms{0}, under_ms{0};
    std::array<double, 3> gpu_share{-1, -1, -1};
    std::array<double, 3> ready_share{-1, -1, -1};
    double limit{0.25};
    uint64_t deadline_misses{0};

    void observe(double gpu_ms, double ready_ms, double interval_ms,
                 double budget, int measured_tier) {
        if (!std::isfinite(gpu_ms) || !std::isfinite(ready_ms) || !std::isfinite(interval_ms)
            || !std::isfinite(budget) || gpu_ms < 0 || ready_ms <= 0 || interval_ms <= 0 || budget <= 0)
            return;
        limit = budget;
        const auto t = static_cast<size_t>(std::clamp(measured_tier, 0, 2));
        const double gpu = gpu_ms / interval_ms, ready = ready_ms / interval_ms;
        gpu_share[t] = gpu_share[t] < 0 ? gpu : gpu_share[t] + 0.2 * (gpu - gpu_share[t]);
        ready_share[t] = ready_share[t] < 0 ? ready : ready_share[t] + 0.2 * (ready - ready_share[t]);
        const double step = std::min(interval_ms, 250.0);
        if (ready > 1.0) ++deadline_misses;
        over_ms = gpu > budget || ready > 1.0 ? over_ms + step : 0;
        under_ms = gpu < 0.7 * budget && ready < 0.8 ? under_ms + step : 0;
    }

    Action plan(unsigned tier) {
        if (over_ms >= 250.0) {
            over_ms = under_ms = 0;
            return tier < 2 ? Action::cheaper : Action::repeat_one;
        }
        if (tier > 0 && tier <= 2 && under_ms >= 10000.0) {
            const auto previous = tier - 1;
            const bool fits = gpu_share[previous] < 0
                || (gpu_share[previous] < 0.9 * limit && ready_share[previous] < 0.9);
            if (fits || under_ms >= 60000.0) {
                over_ms = under_ms = 0;
                return Action::better;
            }
        }
        return Action::keep;
    }
};
}
