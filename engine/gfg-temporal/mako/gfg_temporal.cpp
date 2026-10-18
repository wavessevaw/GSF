/* SPDX-License-Identifier: GPL-3.0-or-later */
// GFG Temporal Engine adapter for the MAKO backend.  See gfg_temporal.hpp and
// docs/TEMPORAL_ENGINE.md.  Nothing here runs unless GFG_TEMPORAL_ENGINE asks
// for it, and every failure leaves the LSFG context in charge.
#include "gfg_temporal.hpp"
#include "lmc_budget.hpp"

#include "mako-common/configuration/config.hpp"

extern "C" {
#include "te_engine.h"
#include "te_scheduler.h"
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <locale>
#include <sstream>
#include <string_view>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace mako::backend::gfg_temporal {
namespace {
    using Clock = std::chrono::steady_clock;
    constexpr uint64_t previousWorkTimeoutNs = 250'000'000ULL; // same bound as the LSFG context


    const char* lmcPassName(int k) noexcept {
        static const char* const names[TE_LMC_PROF_COUNT] = {"search_b", "search_a", "layers", "fit_wait",
                                                             "field", "masked", "orphan", "field2"};
        return k >= 0 && k < TE_LMC_PROF_COUNT ? names[k] : "?";
    }

    constexpr auto publishInterval = std::chrono::milliseconds(1000);

    uint64_t processStartTicks() {
        std::ifstream input("/proc/self/stat");
        std::string line;
        std::getline(input, line);
        const auto end = line.rfind(')');
        if (end == std::string::npos || end + 2 >= line.size()) return 0;
        std::istringstream fields(line.substr(end + 2));
        std::string field;
        for (unsigned int n = 3; n <= 22; ++n)
            if (!(fields >> field)) return 0;
        try {
            return std::stoull(field);
        } catch (...) {
            return 0;
        }
    }

    int64_t unixMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string jsonString(std::string_view value) {
        std::string out = "\"";
        for (const char c : value) {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
            else out += c;
        }
        return out + "\"";
    }

    int duplicate(int fd) {
        return fd >= 0 ? ::fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;
    }

    void closeAll(std::vector<int>& fds) {
        for (int& fd : fds)
            if (fd >= 0) { static_cast<void>(::close(fd)); fd = -1; }
    }

    const char* reasonName(te_sched_reason reason) {
        switch (reason) {
            case TE_REASON_NONE: return "none";
            case TE_REASON_WARMUP: return "warmup";
            case TE_REASON_OVER_BUDGET: return "over-budget";
            case TE_REASON_MISSES: return "deadline-misses";
            case TE_REASON_ADAPTIVE_NOT_WORTH: return "adaptive-not-worth";
            case TE_REASON_SCENE_CUT: return "scene-cut";
        }
        return "unknown";
    }

    /// Engine-backed context.  Experimental: it is the frame generator and
    /// signals the shared timeline exactly like the LSFG context.  Shadow:
    /// it waits on the same timeline but never signals, writes only its own
    /// images, and drops pairs instead of queueing behind the game.
    class TemporalContext final : public ContextBase {
    public:
        TemporalContext(std::shared_ptr<Runtime> runtime, te_context* context, bool shadow,
                VkExtent2D extent) :
            runtime(std::move(runtime)), ctx(context), shadow(shadow), extent(extent),
            status(this->runtime->statusDirectory(), this->runtime->nextContextId()),
            lmc(this->runtime->algorithm() == TE_ALGO_LMC) {
            te_scheduler_init(&this->scheduler);
            this->scheduler.cap_ms = this->runtime->capMs();
            this->opened = Clock::now();
            this->publish(true);
        }

        ~TemporalContext() override {
            if (!this->ctx) return;
            if (leakingEnabled()) return; // keep the private device alive, as the backend does
            te_context_destroy(this->ctx);
        }

        TemporalContext(const TemporalContext&) = delete;
        TemporalContext& operator=(const TemporalContext&) = delete;

        void scheduleFrames(std::span<const float> timestamps) override {
            std::array<float, TE_MAX_OUTPUTS> even{};
            const uint32_t capacity = te_context_outputs(this->ctx);
            if (timestamps.empty()) {
                for (uint32_t i = 0; i < capacity; ++i)
                    even[i] = static_cast<float>(i + 1) / static_cast<float>(capacity + 1);
                timestamps = std::span<const float>(even.data(), capacity);
            }
            if (timestamps.size() > capacity)
                throw backend::error("Too many interpolation timestamps for this context");
            this->submit(timestamps);
        }

        void scheduleFrameHistory() override { this->submit({}); }

        [[nodiscard]] bool waitForIdle(uint64_t timeoutNs) const override {
            return te_context_idle(this->ctx, timeoutNs) != 0;
        }

    private:
        void observeInterval() {
            const auto now = Clock::now();
            if (this->lastCall.time_since_epoch().count() != 0) {
                const double ms = std::chrono::duration<double, std::milli>(now - this->lastCall).count();
                if (ms > 0.0 && ms < 1000.0)
                    this->intervalMs = this->intervalMs > 0.0 ? this->intervalMs + (ms - this->intervalMs) * 0.1 : ms;
            }
            this->lastCall = now;
        }

        void collect() {
            te_poll(this->ctx);
            te_pair_stats st{};
            if (te_last_stats(this->ctx, &st) != VK_SUCCESS) return;
            this->outputsCompleted = st.outputs_completed_total;
            if (st.sequence == this->lastSequence) return;
            this->lastSequence = st.sequence;
            this->stats = st;
            this->haveStats = true;
            if (st.held || !st.timed || st.outputs == 0) return;
            if (this->lmc) {
                // LMC: tiles[0] = layer count, orphan blocks of the B and A grids
                double pair = st.stage_ms[TE_STAGE_PYRAMID] + st.stage_ms[TE_STAGE_ANALYZE] + st.stage_ms[TE_STAGE_CLASSIFY];
                double frame = 0.0, select = 0.0;
                for (uint32_t i = 0; i < st.outputs && i < TE_MAX_OUTPUTS; ++i) {
                    pair += st.frame_ms[i][TE_FRAME_TOTAL];
                    frame += st.frame_ms[i][TE_FRAME_TOTAL];
                    select += st.frame_ms[i][TE_FRAME_LISTS];
                }
                this->cpuFitMs = this->ema(this->cpuFitMs, st.lmc_fit_ms);
                this->pairWithFitMs = this->ema(this->pairWithFitMs, st.ready_ms[st.outputs - 1]);
                this->firstReadyMs = this->ema(this->firstReadyMs, st.ready_ms[0]);
                this->recordPairTime(pair);
                for (int k = 0; k < TE_STAGE_COUNT; ++k) this->stageMs[k] = this->ema(this->stageMs[k], st.stage_ms[k]);
                this->frameMs = this->ema(this->frameMs, frame / st.outputs);
                this->listsMs = this->ema(this->listsMs, select / st.outputs);
                this->lmcLayers = this->ema(this->lmcLayers, st.tiles[0][0]);
                this->lmcOrphansB = this->ema(this->lmcOrphansB, st.tiles[0][1]);
                this->lmcOrphansA = this->ema(this->lmcOrphansA, st.tiles[0][2]);
                for (int k = 0; k < TE_LMC_PROF_COUNT; ++k)
                    if (st.lmc_ms[k] >= 0.0) this->lmcMs[k] = this->ema(this->lmcMs[k], st.lmc_ms[k]);
                this->lmcBudget.observe(pair, st.ready_ms[st.outputs - 1], this->intervalMs,
                                       this->runtime->lmcBudget(), st.lmc_tier);
                return;
            }
            te_sched_observation o{};
            o.timed = st.timed;
            o.profiled = st.profiled;
            o.outputs = st.outputs;
            o.tile_total = st.tile_total;
            o.scene_cut = st.scene_cut;
            o.fixed_ms = st.stage_ms[TE_STAGE_PYRAMID] + st.stage_ms[TE_STAGE_ANALYZE] + st.stage_ms[TE_STAGE_CLASSIFY];
            o.classify_ms = st.stage_ms[TE_STAGE_CLASSIFY];
            double pair = o.fixed_ms, frame = 0.0, lists = 0.0;
            for (uint32_t i = 0; i < st.outputs && i < TE_MAX_OUTPUTS; ++i) {
                o.lists_ms[i] = st.frame_ms[i][TE_FRAME_LISTS];
                o.reuse_ms[i] = st.frame_ms[i][TE_FRAME_REUSE];
                o.light_ms[i] = st.frame_ms[i][TE_FRAME_LIGHT];
                o.full_ms[i] = st.frame_ms[i][TE_FRAME_FULL];
                o.total_ms[i] = st.frame_ms[i][TE_FRAME_TOTAL];
                for (int c = 0; c < 3; ++c) o.tiles[i][c] = st.tiles[i][c];
                pair += st.frame_ms[i][TE_FRAME_TOTAL];
                frame += st.frame_ms[i][TE_FRAME_TOTAL];
                lists += st.frame_ms[i][TE_FRAME_LISTS];
            }
            te_scheduler_observe(&this->scheduler, &o, this->decision.budget_ms);
            this->recordPairTime(pair);
            for (int c = 0; c < 3; ++c) {
                double share = 0.0;
                for (uint32_t i = 0; i < st.outputs; ++i) share += static_cast<double>(st.tiles[i][c]) / st.tile_total;
                this->share[c] = share / st.outputs;
            }
            this->sceneCuts += st.scene_cut;
            for (int k = 0; k < TE_STAGE_COUNT; ++k) this->stageMs[k] = this->ema(this->stageMs[k], st.stage_ms[k]);
            this->frameMs = this->ema(this->frameMs, frame / st.outputs);
            this->listsMs = this->ema(this->listsMs, lists / st.outputs);
            if (st.profiled) {
                for (int c = 0; c < 3; ++c) {
                    double t = 0.0;
                    for (uint32_t i = 0; i < st.outputs; ++i) t += st.frame_ms[i][TE_FRAME_REUSE + c];
                    this->classMs[c] = t / st.outputs;
                }
            }
            this->confidence = st.confidence;
            this->promoted = st.promoted;
        }

        double ema(double old, double value) const { return old > 0.0 ? old + (value - old) * 0.2 : value; }

        void recordPairTime(double ms) {
            this->window[this->windowNext] = ms;
            this->windowNext = (this->windowNext + 1) % this->window.size();
            this->windowCount = std::min<size_t>(this->windowCount + 1, this->window.size());
        }

        [[nodiscard]] double percentile(double p) const {
            if (!this->windowCount) return 0.0;
            std::vector<double> v(this->window.begin(), this->window.begin() + static_cast<long>(this->windowCount));
            std::sort(v.begin(), v.end());
            const auto index = static_cast<size_t>(std::ceil(p * static_cast<double>(v.size()))) - 1;
            return v[std::min(index, v.size() - 1)];
        }

        void submit(std::span<const float> timestamps) {
            this->observeInterval();
            const auto step = timelineStep(this->idx, timestamps.size());
            const uint32_t slot = static_cast<uint32_t>(this->fidx % 2);
            if (this->shadow) {
                // never delay the game: a busy engine skips this frame
                if (!te_context_idle(this->ctx, 0)) {
                    this->dropped++;
                    this->skipped = true;
                    this->idx = step.next;
                    this->fidx++;
                    this->publish(false);
                    return;
                }
            } else if (!te_context_idle(this->ctx, previousWorkTimeoutNs)) {
                std::cerr << "MAKO Renderer: Temporal Engine work did not complete within 250 ms\n";
                throw backend::error("Timeout waiting for previous Temporal Engine work");
            }
            this->collect();
            te_sched_input in{.real_interval_ms = this->intervalMs, .gpu_busy = -1.0,
                              .requested = static_cast<uint32_t>(timestamps.size())};
            if (!timestamps.empty() && this->lmc && this->holdPairs == 0) {
                const auto action = this->lmcBudget.plan(this->tier);
                if (action == LmcBudget::Action::cheaper) {
                    this->tier++;
                    this->tierSteps++;
                } else if (action == LmcBudget::Action::repeat_one) {
                    this->holdPairs = 1;
                    this->costHolds++;
                } else if (action == LmcBudget::Action::better) {
                    this->tier--;
                    this->tierSteps++;
                }
            }
            if (!timestamps.empty() && !this->lmc) {
                this->decision = te_scheduler_decide(&this->scheduler, &in);
                // over the cost cap even at LIGHT-only: hold (copy the real frame,
                // no analysis) for a while, then measure again
                if (this->decision.action == TE_SCHED_FALLBACK && this->holdPairs == 0) {
                    this->holdPairs = 120;
                    this->costHolds++;
                }
            }
            te_submit s{};
            s.slot = slot;
            s.count = static_cast<uint32_t>(timestamps.size());
            for (size_t i = 0; i < timestamps.size(); ++i) s.timestamps[i] = timestamps[i];
            s.wait_value = step.wait;
            s.signal_first = this->shadow ? 0 : step.firstSignal;
            s.discontinuity = this->skipped ? 1 : 0;
            if (this->holdPairs > 0 && s.count) {
                s.hold = 1;
                // measure afresh when the hold ends (the averages are from before it)
                if (--this->holdPairs == 0) this->scheduler.samples = 0;
            }
            s.lmc_tier = static_cast<int>(this->tier);
            s.profile = this->decision.profile;
            s.light_scale = this->decision.light_scale;
            s.force_full = this->decision.force_full;
            s.force_light = this->decision.force_light;
            if (this->shadow && s.count) {
                // duty cycle: runs of 16 measured pairs, history-only in between
                const uint32_t duty = this->runtime->shadowDutyPercent();
                const uint64_t period = duty >= 100 ? 1 : 16ULL * 100 / std::max<uint32_t>(duty, 1);
                if (duty < 100 && (this->pairsSeen++ % period) >= 16) s.count = 0;
            }
            const VkResult r = te_submit_frame(this->ctx, &s);
            if (r != VK_SUCCESS) {
                this->lastError = te_last_error();
                if (!this->shadow)
                    throw backend::error(std::string("Temporal Engine submit failed: ") + this->lastError);
                this->dropped++;
                this->skipped = true;
            } else {
                this->skipped = false;
                this->holdingCurrent = s.count > 0 && s.hold != 0;
                this->submits++;
                this->outputsDispatched += s.count;
                if (s.count) this->pairsSubmitted++;
                else this->historyFrames++;
            }
            this->idx = step.next;
            this->fidx++;
            this->publish(false);
        }

        void publish(bool force) noexcept {
            const auto now = Clock::now();
            if (!force && now - this->lastPublish < publishInterval) return;
            this->lastPublish = now;
            try {
                std::ostringstream o;
                o.imbue(std::locale::classic());
                o << statusPrefix(this->status.contextId(), this->shadow ? "shadow" : "experimental", "engine")
                  << ",\"active\":true,\"algorithm\":" << (this->lmc ? "\"lmc\"" : "\"tiles\"")
                  << ",\"device\":" << jsonString(this->runtime->deviceName())
                  << ",\"width\":" << this->extent.width << ",\"height\":" << this->extent.height
                  << ",\"outputs_capacity\":" << te_context_outputs(this->ctx)
                  << ",\"vram_bytes\":" << te_context_memory_bytes(this->ctx)
                  << ",\"real_interval_ms\":" << finite(this->intervalMs)
                  << ",\"counters\":{\"pairs_submitted\":" << this->pairsSubmitted
                  << ",\"history_frames\":" << this->historyFrames
                  << ",\"outputs_dispatched\":" << this->outputsDispatched
                  << ",\"outputs_gpu_completed\":" << this->outputsCompleted
                  << ",\"dropped\":" << this->dropped
                  << ",\"scene_cuts\":" << this->sceneCuts
                  << ",\"promoted_tiles\":" << this->promoted
                  << ",\"cost_holds\":" << this->costHolds << ",\"lmc_tier\":" << this->tier
                  << ",\"tier_steps\":" << this->tierSteps
                  << ",\"holding\":" << (this->holdingCurrent ? 1 : 0)
                  << ",\"presented\":null}"
                  << ",\"gpu_ms\":{\"pyramid\":" << finite(this->stageMs[TE_STAGE_PYRAMID])
                  << ",\"analyze\":" << finite(this->stageMs[TE_STAGE_ANALYZE])
                  << ",\"classify\":" << finite(this->stageMs[TE_STAGE_CLASSIFY])
                  << ",\"lists\":" << finite(this->listsMs)
                  << ",\"reuse\":" << finite(this->classMs[0]) << ",\"light\":" << finite(this->classMs[1])
                  << ",\"full\":" << finite(this->classMs[2])
                  << ",\"merge\":0,\"frame\":" << finite(this->frameMs)
                  << ",\"pair_p50\":" << finite(this->percentile(0.50)) << ",\"pair_p95\":" << finite(this->percentile(0.95))
                  << ",\"pair_p99\":" << finite(this->percentile(0.99)) << ",\"timed\":" << ((this->haveStats && this->stats.timed) ? "true" : "false")
                  << '}'
                  << ",\"shares\":{\"reuse\":" << finite(this->share[0]) << ",\"light\":" << finite(this->share[1])
                  << ",\"full\":" << finite(this->share[2]) << '}'
                  << ",\"confidence\":" << finite(this->confidence)
                  << ",\"lmc\":{\"layers\":" << finite(this->lmcLayers) << ",\"orphans_b\":" << finite(this->lmcOrphansB)
                  << ",\"cpu_fit_ms\":" << finite(this->cpuFitMs)
                  << ",\"pair_with_fit_ms\":" << finite(this->pairWithFitMs)
                  << ",\"first_ready_ms\":" << finite(this->firstReadyMs)
                  << ",\"deadline_misses\":" << this->lmcBudget.deadline_misses
                  << ",\"orphans_a\":" << finite(this->lmcOrphansA) << ",\"select_ms\":" << finite(this->listsMs)
                  << ",\"budget\":" << finite(this->runtime->lmcBudget()) << ",\"passes_ms\":{";
                for (int k = 0; k < TE_LMC_PROF_COUNT; ++k)
                    o << (k ? "," : "") << '"' << lmcPassName(k) << "\":" << finite(this->lmcMs[k]);
                o << "}}"
                  << ",\"scheduler\":{\"level\":" << this->decision.level
                  << ",\"light_scale\":" << finite(this->decision.light_scale)
                  << ",\"force_full\":" << this->decision.force_full
                  << ",\"force_light\":" << this->decision.force_light
                  << ",\"cap_ms\":" << finite(this->scheduler.cap_ms)
                  << ",\"budget_ms\":" << finite(this->decision.budget_ms)
                  << ",\"predicted_ms\":" << finite(this->decision.predicted_ms)
                  << ",\"recommended_outputs\":" << this->decision.outputs
                  << ",\"recommendation\":" << jsonString(this->lmc ? (this->holdingCurrent ? "hold" : "keep")
                        : this->decision.action == TE_SCHED_FALLBACK ? "fallback"
                        : (this->decision.outputs < this->stats.outputs ? "fewer-outputs" : "keep"))
                  << ",\"reason\":" << jsonString(reasonName(this->decision.reason))
                  << ",\"misses\":" << this->scheduler.total_misses
                  << ",\"gpu_busy_known\":false}"
                  << ",\"last_error\":" << (this->lastError.empty() ? "null" : jsonString(this->lastError))
                  << "}\n";
                this->status.write(o.str());
                // the same facts in the renderer log every 10 s: readable even when
                // no status reader is available
                if (!this->shadow && now - this->lastLog >= std::chrono::seconds(10)) {
                    this->lastLog = now;
                    std::ostringstream l;
                    l.imbue(std::locale::classic());
                    l.setf(std::ios::fixed);
                    l.precision(2);
                    l << "MAKO Renderer: Temporal Engine stats algo=" << (this->lmc ? "lmc" : "tiles")
                      << " level=" << this->decision.level
                      << " reason=" << reasonName(this->decision.reason)
                      << " holding=" << (this->holdingCurrent ? 1 : 0) << " cost_holds=" << this->costHolds
                      << " tier=" << this->tier << " tier_steps=" << this->tierSteps
                      << " cpu_fit_ms=" << finite(this->cpuFitMs)
                      << " pair_with_fit_ms=" << finite(this->pairWithFitMs)
                      << " first_ready_ms=" << finite(this->firstReadyMs)
                      << " deadline_misses=" << this->lmcBudget.deadline_misses
                      << " pairs=" << this->pairsSubmitted << " dropped=" << this->dropped
                      << " interval_ms=" << finite(this->intervalMs)
                      << " fixed_ms=" << finite(this->stageMs[TE_STAGE_PYRAMID] + this->stageMs[TE_STAGE_ANALYZE]
                                                + this->stageMs[TE_STAGE_CLASSIFY])
                      << " frame_ms=" << finite(this->frameMs) << " pair_p95_ms=" << finite(this->percentile(0.95))
                      << " cap_ms=" << finite(this->scheduler.cap_ms)
                      << " predicted_ms=" << finite(this->decision.predicted_ms)
                      << " shares=" << finite(this->share[0]) << '/' << finite(this->share[1]) << '/'
                      << finite(this->share[2]) << " confidence=" << finite(this->confidence)
                      << " scene_cuts=" << this->sceneCuts << " pyramid_ms=" << finite(this->stageMs[TE_STAGE_PYRAMID])
                      << " analyze_ms=" << finite(this->stageMs[TE_STAGE_ANALYZE])
                      << " fields_ms=" << finite(this->stageMs[TE_STAGE_CLASSIFY])
                      << " layers=" << finite(this->lmcLayers) << " orphans=" << finite(this->lmcOrphansB) << '/'
                      << finite(this->lmcOrphansA);
                    if (this->lmc) {
                        l << " lmc_ms=";
                        for (int k = 0; k < TE_LMC_PROF_COUNT; ++k)
                            l << (k ? "/" : "") << finite(this->lmcMs[k]);
                        l << " lmc_passes=search_b/search_a/layers/fit_wait/field/masked/orphan/field2";
                    }
                    l << '\n';
                    std::clog << l.str();
                }
            } catch (...) {
            }
        }

        std::shared_ptr<Runtime> runtime;
        te_context* ctx;
        bool shadow;
        VkExtent2D extent;
        StatusFile status;
        bool lmc;                  // layered motion path: no tile scheduler; its own cost guard
        double lmcLayers{0.0}, lmcOrphansB{0.0}, lmcOrphansA{0.0};
        double lmcMs[TE_LMC_PROF_COUNT]{};
        LmcBudget lmcBudget;
        bool holdingCurrent{false};
        double cpuFitMs{0.0};
        double pairWithFitMs{0.0}; // timestamp span, including fit/queue gaps; not display latency
        double firstReadyMs{0.0};
        uint32_t tier{0};          // LMC cost tier (te_submit.lmc_tier)
        uint32_t tierSteps{0};     // tier changes since the context started
        te_scheduler scheduler{};
        te_sched_decision decision{};
        te_pair_stats stats{};
        bool haveStats{false};
        uint64_t idx{1};
        uint64_t fidx{0};
        bool skipped{false};
        uint64_t submits{0}, lastSequence{0};
        uint64_t pairsSubmitted{0}, historyFrames{0}, outputsDispatched{0}, outputsCompleted{0};
        uint64_t dropped{0}, sceneCuts{0}, pairsSeen{0};
        uint32_t promoted{0};
        uint32_t holdPairs{0};
        uint64_t costHolds{0};
        double intervalMs{0.0};
        double stageMs[TE_STAGE_COUNT]{};
        double classMs[3]{};
        double share[3]{};
        double frameMs{0.0}, listsMs{0.0}, confidence{0.0};
        std::array<double, 240> window{};
        size_t windowNext{0}, windowCount{0};
        std::string lastError;
        Clock::time_point opened{}, lastCall{}, lastPublish{}, lastLog{};
    };

    /// LSFG context plus optional shadow engine and a status record.
    class ObservedContext final : public ContextBase {
    public:
        ObservedContext(std::unique_ptr<ContextBase> primary, std::unique_ptr<ContextBase> shadow,
                std::unique_ptr<StatusFile> record) :
            primary(std::move(primary)), shadow(std::move(shadow)), record(std::move(record)) {}

        void scheduleFrames(std::span<const float> timestamps) override {
            this->primary->scheduleFrames(timestamps);   // presentation first
            if (this->shadow) {
                try {
                    this->shadow->scheduleFrames(timestamps);
                } catch (const std::exception& e) {
                    std::cerr << "MAKO Renderer: Temporal Engine shadow stopped: " << e.what() << '\n';
                    this->shadow.reset();
                }
            }
        }

        void scheduleFrameHistory() override {
            this->primary->scheduleFrameHistory();
            if (this->shadow) {
                try {
                    this->shadow->scheduleFrameHistory();
                } catch (const std::exception& e) {
                    std::cerr << "MAKO Renderer: Temporal Engine shadow stopped: " << e.what() << '\n';
                    this->shadow.reset();
                }
            }
        }

        [[nodiscard]] bool waitForIdle(uint64_t timeoutNs) const override {
            // the shadow never gates presentation; it is drained on destruction
            return this->primary->waitForIdle(timeoutNs);
        }

    private:
        std::unique_ptr<ContextBase> primary;
        std::unique_ptr<ContextBase> shadow;
        std::unique_ptr<StatusFile> record;
    };
}

Mode parseMode(const char* value) noexcept {
    if (!value) return Mode::Off;
    const std::string_view v(value);
    if (v == "observe") return Mode::Observe;
    if (v == "shadow") return Mode::Shadow;
    if (v == "experimental") return Mode::Experimental;
    return Mode::Off;
}

const char* modeName(Mode mode) noexcept {
    switch (mode) {
        case Mode::Off: return "off";
        case Mode::Observe: return "observe";
        case Mode::Shadow: return "shadow";
        case Mode::Experimental: return "experimental";
    }
    return "off";
}

/// JSON has no NaN/Infinity: a non-finite measurement is written as 0.
double finite(double value) noexcept { return std::isfinite(value) ? value : 0.0; }

std::string statusPrefix(uint64_t contextId, std::string_view mode, std::string_view role) {
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << "{\"schema\":1,\"engine\":\"gfg-temporal\",\"version\":" << jsonString(TE_VERSION)
      << ",\"pid\":" << ::getpid() << ",\"process_start_ticks\":" << processStartTicks()
      << ",\"context\":" << contextId << ",\"updated_unix_ms\":" << unixMs()
      << ",\"mode\":" << jsonString(mode) << ",\"role\":" << jsonString(role);
    return o.str();
}

Runtime::Runtime(Mode mode) noexcept : m_mode(mode) {}

std::shared_ptr<Runtime> Runtime::create(Mode mode, VkPhysicalDevice physical,
        PFN_vkGetPhysicalDeviceProperties2 properties2) noexcept {
    if (mode == Mode::Off) return nullptr;
    std::shared_ptr<Runtime> rt;
    try {
        rt.reset(new Runtime(mode));
        rt->m_statusDir = ls::findConfigurationFile().parent_path() / "runtime-state" / "temporal";
        std::error_code error;
        std::filesystem::create_directories(rt->m_statusDir, error);
        if (const char* duty = std::getenv("GFG_TEMPORAL_SHADOW_DUTY")) {
            const long value = std::strtol(duty, nullptr, 10);
            if (value >= 1 && value <= 100) rt->m_shadowDuty = static_cast<uint32_t>(value);
        }
        if (const char* cap = std::getenv("GFG_TEMPORAL_CAP_MS")) {
            const double value = std::strtod(cap, nullptr);
            if (value >= 0.0 && value <= 50.0) rt->m_capMs = value;
        }
        if (const char* algo = std::getenv("GFG_TEMPORAL_ALGO"))
            rt->m_algorithm = std::string_view(algo) == "tiles" ? TE_ALGO_TILES : TE_ALGO_LMC;
        if (const char* budget = std::getenv("GFG_TEMPORAL_LMC_BUDGET")) {
            const double value = std::strtod(budget, nullptr);
            if (value >= 0.05 && value <= 1.0) rt->m_lmcBudget = value;
        }
    } catch (...) {
        return nullptr;
    }
    te_device_desc desc{};
    if (physical && properties2) {
        VkPhysicalDeviceIDProperties id{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
        properties2(physical, &props);
        std::memcpy(desc.device_uuid, id.deviceUUID, VK_UUID_SIZE);
        desc.match_uuid = 1;
        rt->m_deviceName = props.properties.deviceName;
    } else {
        rt->m_reason = "backend-device-unknown";
        return rt;
    }
    te_device* device = nullptr;
    if (te_device_create(&desc, &device) != VK_SUCCESS) {
        rt->m_reason = std::string("device: ") + te_last_error();
        return rt;
    }
    int memory = 0, semaphore = 0;
    te_device_supports_fd_import(device, &memory, &semaphore);
    if (!memory || !semaphore) {
        rt->m_reason = !memory ? "no-external-memory-fd" : "no-external-semaphore-fd";
        te_device_destroy(device);
        return rt;
    }
    rt->m_device = device;
    std::clog << "MAKO Renderer: Temporal Engine " << TE_VERSION << " mode=" << modeName(mode)
              << " device=" << rt->m_deviceName << '\n';
    return rt;
}

Runtime::~Runtime() {
    if (this->m_device && !leakingEnabled()) te_device_destroy(this->m_device);
}

StatusFile::StatusFile(const std::filesystem::path& directory, uint64_t contextId) : id(contextId) {
    this->path = directory / (std::to_string(::getpid()) + "-" + std::to_string(processStartTicks()) + "-" +
                              std::to_string(contextId) + ".json");
    // Liveness that survives a private PID namespace (Steam Runtime): the
    // reader checks whether this lock is held, as for the renderer's own status.
    auto lock = this->path;
    lock += ".lock";
    this->lockFd = ::open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (this->lockFd >= 0 && ::flock(this->lockFd, LOCK_EX | LOCK_NB) != 0) {
        ::close(this->lockFd);
        this->lockFd = -1;
    }
}

StatusFile::~StatusFile() {
    std::error_code error;
    std::filesystem::remove(this->path, error);
    auto lock = this->path;
    lock += ".lock";
    std::filesystem::remove(lock, error);
    if (this->lockFd >= 0) ::close(this->lockFd);
}

void StatusFile::write(const std::string& json) noexcept {
    try {
        auto temp = this->path;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::trunc);
            if (!out) return;
            out << json;
            if (!out) return;
        }
        std::error_code error;
        std::filesystem::rename(temp, this->path, error);
    } catch (...) {
    }
}

Prepared::Prepared(std::shared_ptr<Runtime> runtime, std::pair<int, int> sourceFds,
        const std::vector<int>& destFds, int syncFd, VkExtent2D extent, FrameEncoding encoding,
        SourceImageLayout layout) noexcept :
    runtime(std::move(runtime)), extent(extent), encoding(encoding), layout(layout) {
    if (!this->runtime) return;
    const Mode mode = this->runtime->mode();
    if (mode != Mode::Shadow && mode != Mode::Experimental) return;
    try {
        this->sources = {duplicate(sourceFds.first), duplicate(sourceFds.second)};
        this->destCount = static_cast<uint32_t>(std::min<size_t>(destFds.size(), TE_MAX_OUTPUTS));
        if (mode == Mode::Experimental)
            for (const int fd : destFds) this->destinations.push_back(duplicate(fd));
        this->sync = duplicate(syncFd);
    } catch (...) {
        closeAll(this->sources);
        closeAll(this->destinations);
    }
}

Prepared::~Prepared() {
    closeAll(this->sources);
    closeAll(this->destinations);
    if (this->sync >= 0) static_cast<void>(::close(this->sync));
}

std::string Prepared::eligibility() const {
    if (!this->runtime) return "off";
    if (!this->runtime->device()) return this->runtime->unavailableReason();
    if (this->encoding != FrameEncoding::Sdr8) return "encoding-not-sdr8";
    if (this->layout != SourceImageLayout::Default && this->layout != SourceImageLayout::Rgba8 &&
            this->layout != SourceImageLayout::Bgra8)
        return "source-layout";
    return "";
}

bool Prepared::wantsExperimental() const noexcept {
    return this->runtime && this->runtime->mode() == Mode::Experimental;
}

std::unique_ptr<ContextBase> Prepared::openExperimental() noexcept {
    this->experimentalTried = true;
    try {
        this->fallbackReason = this->eligibility();
        if (!this->fallbackReason.empty()) return nullptr;
        if (this->sources.size() != 2 || this->sources[0] < 0 || this->sources[1] < 0 || this->sync < 0 ||
                std::find(this->destinations.begin(), this->destinations.end(), -1) != this->destinations.end()) {
            this->fallbackReason = "descriptor-duplicate-failed";
            return nullptr;
        }
        if (this->destinations.empty() || this->destinations.size() > TE_MAX_OUTPUTS) {
            this->fallbackReason = "output-count";
            return nullptr;
        }
        te_context_desc desc;
        te_context_desc_default(&desc, this->extent.width, this->extent.height,
            static_cast<uint32_t>(this->destinations.size()));
        desc.source_fds[0] = this->sources[0];
        desc.source_fds[1] = this->sources[1];
        for (size_t i = 0; i < this->destinations.size(); ++i) desc.dest_fds[i] = this->destinations[i];
        desc.sync_fd = this->sync;
        desc.source_bgra = this->layout == SourceImageLayout::Bgra8;
        desc.params.algorithm = this->runtime->algorithm();
        // te_context_create owns every descriptor from here, success or not
        this->sources.clear();
        this->destinations.clear();
        this->sync = -1;
        te_context* ctx = nullptr;
        if (te_context_create(this->runtime->device(), &desc, &ctx) != VK_SUCCESS) {
            this->fallbackReason = std::string("context: ") + te_last_error();
            return nullptr;
        }
        return std::make_unique<TemporalContext>(this->runtime, ctx, false, this->extent);
    } catch (const std::exception& e) {
        this->fallbackReason = std::string("exception: ") + e.what();
        return nullptr;
    }
}

std::unique_ptr<ContextBase> Prepared::attach(std::unique_ptr<ContextBase> primary) noexcept {
    if (!this->runtime) return primary;
    try {
        const Mode mode = this->runtime->mode();
        std::unique_ptr<ContextBase> shadow;
        std::string reason = this->experimentalTried ? this->fallbackReason : this->eligibility();
        if (mode == Mode::Shadow && reason.empty()) {
            if (this->sources.size() == 2 && this->sources[0] >= 0 && this->sources[1] >= 0 && this->sync >= 0) {
                te_context_desc desc;
                te_context_desc_default(&desc, this->extent.width, this->extent.height, this->destCount);
                desc.source_fds[0] = this->sources[0];
                desc.source_fds[1] = this->sources[1];
                desc.sync_fd = this->sync;
                desc.source_bgra = this->layout == SourceImageLayout::Bgra8;
                desc.outputs = this->destCount; // private images; the shadow never touches the game's
                desc.params.algorithm = this->runtime->algorithm();
                this->sources.clear();
                this->sync = -1;
                te_context* ctx = nullptr;
                if (te_context_create(this->runtime->device(), &desc, &ctx) == VK_SUCCESS)
                    shadow = std::make_unique<TemporalContext>(this->runtime, ctx, true, this->extent);
                else
                    reason = std::string("context: ") + te_last_error();
            } else {
                reason = "descriptor-duplicate-failed";
            }
        }
        std::unique_ptr<StatusFile> record;
        if (!shadow) {
            // observe, or a refused shadow/experimental context: report why LSFG kept the work
            record = std::make_unique<StatusFile>(this->runtime->statusDirectory(), this->runtime->nextContextId());
            std::ostringstream o;
            o.imbue(std::locale::classic());
            o << statusPrefix(record->contextId(), modeName(mode), mode == Mode::Observe ? "observer" : "fallback")
              << ",\"active\":false,\"device\":" << jsonString(this->runtime->deviceName())
              << ",\"width\":" << this->extent.width << ",\"height\":" << this->extent.height
              << ",\"eligible\":" << (reason.empty() ? "true" : "false")
              << ",\"fallback_reason\":" << (reason.empty() ? "null" : jsonString(reason)) << "}\n";
            record->write(o.str());
            if (!reason.empty() && mode != Mode::Observe)
                std::cerr << "MAKO Renderer: Temporal Engine not used for this context (" << reason
                          << "); LSFG keeps frame generation\n";
        }
        return std::make_unique<ObservedContext>(std::move(primary), std::move(shadow), std::move(record));
    } catch (...) {
        return primary;
    }
}
}
