/* SPDX-License-Identifier: GPL-3.0-or-later */
// GFG Temporal Engine inside the MAKO backend.  Selected per context by
// GFG_TEMPORAL_ENGINE (off, observe, shadow, experimental); any refusal keeps
// the LSFG context, so the production path is the fallback for everything.
#pragma once

#include "mako-backend/mako.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

/* keep in step with TE_SCHED_DEFAULT_CAP_MS in src/te_scheduler.h */
#define TE_SCHED_DEFAULT_CAP_MS_VALUE 2.0

struct te_device;
struct te_context;

namespace mako::backend {
    /// True once the backend retains its private Vulkan objects at unload
    /// (see makeLeaking); the engine then keeps its device alive as well.
    [[nodiscard]] bool leakingEnabled() noexcept;
}

namespace mako::backend::gfg_temporal {

    enum class Mode : uint8_t { Off, Observe, Shadow, Experimental };

    [[nodiscard]] Mode parseMode(const char* value) noexcept;
    [[nodiscard]] const char* modeName(Mode mode) noexcept;

    /// MAKO's shared timeline: the producer signals `idx` when a real frame
    /// is ready, generated output i signals idx + 1 + i, and the next real
    /// frame is idx + outputs + 1 (a history-only frame advances by one).
    struct TimelineStep {
        uint64_t wait;
        uint64_t firstSignal;
        uint64_t next;
    };
    [[nodiscard]] constexpr TimelineStep timelineStep(uint64_t idx, size_t outputs) noexcept {
        return {idx, outputs ? idx + 1 : 0, idx + outputs + 1};
    }

    /// Process-wide state: the engine's own Vulkan device on the backend's
    /// physical GPU, created while the backend instance is being created.
    class Runtime {
    public:
        static std::shared_ptr<Runtime> create(Mode mode, VkPhysicalDevice physical,
            PFN_vkGetPhysicalDeviceProperties2 properties2) noexcept;
        ~Runtime();
        Runtime(const Runtime&) = delete;
        Runtime& operator=(const Runtime&) = delete;

        [[nodiscard]] Mode mode() const noexcept { return this->m_mode; }
        [[nodiscard]] te_device* device() const noexcept { return this->m_device; }
        [[nodiscard]] const std::string& unavailableReason() const noexcept { return this->m_reason; }
        [[nodiscard]] const std::string& deviceName() const noexcept { return this->m_deviceName; }
        [[nodiscard]] uint64_t nextContextId() noexcept { return ++this->m_contexts; }
        [[nodiscard]] uint32_t shadowDutyPercent() const noexcept { return this->m_shadowDuty; }
        /// GPU ms per generated frame the engine may use (GFG_TEMPORAL_CAP_MS, 0 = no cap)
        [[nodiscard]] double capMs() const noexcept { return this->m_capMs; }
        /// TE_ALGO_LMC (default) or TE_ALGO_TILES (te.2), from GFG_TEMPORAL_ALGO=lmc|tiles
        [[nodiscard]] uint32_t algorithm() const noexcept { return this->m_algorithm; }
        // LMC cost guard: largest share of the real frame interval one pair may take on the GPU
        [[nodiscard]] double lmcBudget() const noexcept { return this->m_lmcBudget; }
        [[nodiscard]] const std::filesystem::path& statusDirectory() const noexcept { return this->m_statusDir; }
    private:
        explicit Runtime(Mode mode) noexcept;
        Mode m_mode;
        te_device* m_device{nullptr};
        std::string m_reason;
        std::string m_deviceName;
        uint64_t m_contexts{0};
        uint32_t m_shadowDuty{25};
        double m_capMs{TE_SCHED_DEFAULT_CAP_MS_VALUE};
        uint32_t m_algorithm{1};
        double m_lmcBudget{0.25};
        std::filesystem::path m_statusDir;
    };

    /// Descriptor duplicates taken before the LSFG context consumes the
    /// originals.  Unused duplicates are closed on destruction.
    class Prepared {
    public:
        Prepared(std::shared_ptr<Runtime> runtime, std::pair<int, int> sourceFds,
            const std::vector<int>& destFds, int syncFd, VkExtent2D extent,
            FrameEncoding encoding, SourceImageLayout layout) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

        [[nodiscard]] bool wantsExperimental() const noexcept;
        /// Engine context that replaces LSFG, or null (reason recorded).
        [[nodiscard]] std::unique_ptr<ContextBase> openExperimental() noexcept;
        /// Wrap the LSFG context for observe/shadow/fallback reporting.
        [[nodiscard]] std::unique_ptr<ContextBase> attach(std::unique_ptr<ContextBase> primary) noexcept;
    private:
        [[nodiscard]] std::string eligibility() const;
        std::shared_ptr<Runtime> runtime;
        std::vector<int> sources;
        std::vector<int> destinations;
        int sync{-1};
        uint32_t destCount{1};
        VkExtent2D extent{};
        FrameEncoding encoding{};
        SourceImageLayout layout{};
        std::string fallbackReason;
        bool experimentalTried{false};
    };

    /// Status file under <runtime-state>/temporal, replaced atomically and
    /// removed when the context closes.
    class StatusFile {
    public:
        StatusFile(const std::filesystem::path& directory, uint64_t contextId);
        ~StatusFile();
        StatusFile(const StatusFile&) = delete;
        StatusFile& operator=(const StatusFile&) = delete;
        void write(const std::string& json) noexcept;
        [[nodiscard]] uint64_t contextId() const noexcept { return this->id; }
    private:
        std::filesystem::path path;
        uint64_t id;
        int lockFd{-1};
    };

    [[nodiscard]] std::string statusPrefix(uint64_t contextId, std::string_view mode, std::string_view role);
    /// JSON has no NaN/Infinity: a non-finite measurement is written as 0.
    [[nodiscard]] double finite(double value) noexcept;
}
