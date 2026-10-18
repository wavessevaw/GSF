/* SPDX-License-Identifier: GPL-3.0-or-later */
// Tests of the Temporal Engine adapter inside the MAKO backend on a real
// Vulkan device (lavapipe in CI).  The engine itself is covered by
// engine/gfg-temporal/tests/te_tests.c; here: mode parsing, the MAKO timeline
// mapping, and that every refusal keeps the LSFG context in charge, reports
// why, and neither leaks nor steals the caller's descriptors.
#include "gfg_temporal.hpp"

extern "C" {
#include "te_engine.h"
}

#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <vulkan/vulkan.h>

using namespace mako::backend;
using namespace mako::backend::gfg_temporal;

namespace {
    int failures = 0, checks = 0;
    void expect(bool ok, const std::string& what) {
        ++checks;
        if (!ok) {
            ++failures;
            std::cerr << "FAIL " << what << '\n';
        }
    }

    struct FakeContext final : ContextBase {
        int frames = 0, history = 0;
        size_t lastCount = 0;
        void scheduleFrames(std::span<const float> timestamps) override { ++frames; lastCount = timestamps.size(); }
        void scheduleFrameHistory() override { ++history; }
        [[nodiscard]] bool waitForIdle(uint64_t) const override { return true; }
    };

    int openFds() {
        int n = 0;
        if (DIR* d = opendir("/proc/self/fd")) {
            while (readdir(d)) ++n;
            closedir(d);
        }
        return n;
    }

    std::vector<std::string> statusFiles(const std::filesystem::path& dir) {
        std::vector<std::string> out;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
            if (entry.path().extension() != ".json") continue;  // skip the liveness lock
            std::ifstream in(entry.path());
            std::stringstream text;
            text << in.rdbuf();
            out.push_back(text.str());
        }
        return out;
    }
}

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("gfg-temporal-adapter-" + std::to_string(::getpid()));
    std::filesystem::create_directories(root);
    setenv("MAKO_CONFIG", (root / "conf.toml").c_str(), 1);
    const auto statusDir = root / "runtime-state" / "temporal";

    expect(parseMode(nullptr) == Mode::Off && parseMode("") == Mode::Off && parseMode("on") == Mode::Off,
           "unknown values mean off");
    expect(parseMode("observe") == Mode::Observe && parseMode("shadow") == Mode::Shadow &&
           parseMode("experimental") == Mode::Experimental, "modes parse");
    constexpr auto pair = timelineStep(1, 2);
    static_assert(pair.wait == 1 && pair.firstSignal == 2 && pair.next == 4);
    constexpr auto history = timelineStep(5, 0);
    static_assert(history.wait == 5 && history.firstSignal == 0 && history.next == 6);

    VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance{};
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::cerr << "no Vulkan instance\n";
        return 2;
    }
    uint32_t count = 1;
    VkPhysicalDevice physical{};
    vkEnumeratePhysicalDevices(instance, &count, &physical);

    expect(Runtime::create(Mode::Off, physical, vkGetPhysicalDeviceProperties2) == nullptr, "off creates nothing");
    {
        unsetenv("GFG_TEMPORAL_ALGO");
        auto lmc = Runtime::create(Mode::Observe, physical, vkGetPhysicalDeviceProperties2);
        expect(lmc && lmc->algorithm() == TE_ALGO_LMC, "layered motion is the default algorithm");
        setenv("GFG_TEMPORAL_ALGO", "tiles", 1);
        auto tiles = Runtime::create(Mode::Observe, physical, vkGetPhysicalDeviceProperties2);
        expect(tiles && tiles->algorithm() == TE_ALGO_TILES, "GFG_TEMPORAL_ALGO=tiles selects te.2");
        setenv("GFG_TEMPORAL_ALGO", "something", 1);
        auto other = Runtime::create(Mode::Observe, physical, vkGetPhysicalDeviceProperties2);
        expect(other && other->algorithm() == TE_ALGO_LMC, "unknown GFG_TEMPORAL_ALGO keeps the default");
        unsetenv("GFG_TEMPORAL_ALGO");
    }
    {
        Prepared none(nullptr, {-1, -1}, {}, -1, {64, 64}, FrameEncoding::Sdr8, SourceImageLayout::Default);
        auto fake = std::make_unique<FakeContext>();
        auto* raw = fake.get();
        auto attached = none.attach(std::move(fake));
        expect(attached.get() == raw, "without a runtime the LSFG context is returned unchanged");
    }

    for (const Mode mode : {Mode::Observe, Mode::Shadow, Mode::Experimental}) {
        auto runtime = Runtime::create(mode, physical, vkGetPhysicalDeviceProperties2);
        expect(runtime != nullptr, std::string("runtime for ") + modeName(mode));
        if (!runtime) continue;
        std::cout << "  " << modeName(mode) << ": device=" << runtime->deviceName() << " engine="
                  << (runtime->device() ? "ready" : runtime->unavailableReason()) << '\n';
        int src[2], dst[2], sync[2];
        expect(pipe(src) == 0 && pipe(dst) == 0 && pipe(sync) == 0, "descriptors");
        const int before = openFds();
        {
            // pipes are not images: an available engine refuses the import, a
            // missing one refuses earlier; either way LSFG keeps the work
            Prepared prepared(runtime, {src[0], src[1]}, {dst[0]}, sync[0], {64, 64}, FrameEncoding::Sdr8,
                              SourceImageLayout::Default);
            expect(prepared.wantsExperimental() == (mode == Mode::Experimental), "experimental wish");
            if (prepared.wantsExperimental()) expect(prepared.openExperimental() == nullptr, "refused engine context");
            auto fake = std::make_unique<FakeContext>();
            auto* raw = fake.get();
            auto attached = prepared.attach(std::move(fake));
            expect(attached != nullptr, "context returned");
            const float ts[2] = {1.0f / 3, 2.0f / 3};
            attached->scheduleFrames(ts);
            attached->scheduleFrameHistory();
            expect(raw->frames == 1 && raw->lastCount == 2 && raw->history == 1, "calls reach the LSFG context");
            expect(attached->waitForIdle(0), "readiness comes from the LSFG context");
            const auto files = statusFiles(statusDir);
            expect(files.size() == 1, "one status record per context");
            if (!files.empty()) {
                const auto& s = files.front();
                const char* role = mode == Mode::Observe ? "\"role\":\"observer\"" : "\"role\":\"fallback\"";
                expect(s.find(role) != std::string::npos, std::string("role in ") + s);
                expect(s.find("\"active\":false") != std::string::npos, "not active");
                expect(s.find(std::string("\"mode\":\"") + modeName(mode) + "\"") != std::string::npos, "mode recorded");
                expect(mode == Mode::Observe || s.find("\"fallback_reason\":null") == std::string::npos,
                       "a refusal names its reason");
            }
        }
        expect(statusFiles(statusDir).empty(), "status record removed with the context");
        {
            std::error_code ec;
            bool locks = false;
            for (const auto& e : std::filesystem::directory_iterator(statusDir, ec)) locks |= e.path().extension() == ".lock";
            expect(!locks, "liveness lock removed with the context");
        }
        expect(openFds() == before, "no descriptor leaked (" + std::to_string(openFds()) + " vs " +
               std::to_string(before) + ")");
        for (int fd : {src[0], src[1], dst[0], sync[0]})
            expect(fcntl(fd, F_GETFD) != -1, "caller's descriptor still open");
        for (int fd : {src[0], src[1], dst[0], dst[1], sync[0], sync[1]}) close(fd);
    }
    vkDestroyInstance(instance, nullptr);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::cout << "gfg-temporal adapter tests: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
