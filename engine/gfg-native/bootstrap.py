#!/usr/bin/env python3
"""Deterministic transitional no-DLL bootstrap for GFG Temporal Engine.

This operates on the *already patched*, pinned GPL-3.0-or-later MAKO source.
It is deliberately not a permanent rendering architecture. It replaces the
DLL-dependent backend startup when Experimental TE is selected, retaining the
open WSI transport until GFG's own presentation layer can replace it.

Fail closed on source drift. Never load or extract Lossless.dll on this path.
"""
from __future__ import annotations

import argparse
from pathlib import Path


def checked_replace(source: str, before: str, after: str, label: str) -> str:
    count = source.count(before)
    if count != 1:
        raise ValueError(f"{label}: expected exactly one upstream anchor, found {count}")
    return source.replace(before, after, 1)


def patch_backend(source: str) -> str:
    source = checked_replace(source, """    this->m_impl = std::make_unique<InstanceImpl>(
        selectFunc, shaderDllPath, allowLowPrecision
    );
    // GFG Temporal Engine: its own device on the same GPU, created here so the
    // caller's layer-disabling environment also covers it.  Off unless asked.
    const auto& backendVulkan = this->m_impl->getVulkan();
    this->m_temporal = gfg_temporal::Runtime::create(
        gfg_temporal::parseMode(std::getenv("GFG_TEMPORAL_ENGINE")),
        backendVulkan.physdev(), backendVulkan.fi().GetPhysicalDeviceProperties2);
""", """    const auto temporalMode = gfg_temporal::parseMode(std::getenv("GFG_TEMPORAL_ENGINE"));
    if (temporalMode == gfg_temporal::Mode::Experimental) {
        // No InstanceImpl, shader registry, or Lossless.dll access on this path.
        // The temporary open Vulkan instance selects the SAME physical GPU as
        // the game; Temporal Engine owns its separate compute VkDevice.
        vk::Vulkan bootstrap{
            "gfg-temporal-bootstrap", vk::version{1, 0, 0},
            "gfg-temporal-bootstrap", vk::version{1, 0, 0},
            selectFunc, false, std::nullopt, std::nullopt
        };
        this->m_temporal = gfg_temporal::Runtime::create(
            temporalMode, bootstrap.physdev(), bootstrap.fi().GetPhysicalDeviceProperties2);
        if (!this->m_temporal || !this->m_temporal->device())
            throw backend::error("Independent GFG Temporal Engine is unavailable");
        std::clog << "GFG Native: experimental compute backend started without Lossless.dll" << std::endl;
        return;
    }
    this->m_impl = std::make_unique<InstanceImpl>(
        selectFunc, shaderDllPath, allowLowPrecision
    );
    const auto& backendVulkan = this->m_impl->getVulkan();
    this->m_temporal = gfg_temporal::Runtime::create(
        temporalMode, backendVulkan.physdev(), backendVulkan.fi().GetPhysicalDeviceProperties2);
""", "exclusive native bootstrap")

    source = checked_replace(source, """bool Instance::supportsPackedHdr10Transport() const {
    const auto& vk = this->m_impl->getVulkan();
""", """bool Instance::supportsPackedHdr10Transport() const {
    if (!this->m_impl) return false; // TE-only mode does not support HDR
    const auto& vk = this->m_impl->getVulkan();
""", "HDR capability in native mode")

    source = checked_replace(source, """bool Instance::supportsSourceImageLayout(const SourceImageLayout layout) const {
    const auto format = nativeSourceImageFormat(FrameEncoding::Sdr8, layout);
""", """bool Instance::supportsSourceImageLayout(const SourceImageLayout layout) const {
    if (!this->m_impl) return false; // use existing RGBA8 transport initially
    const auto format = nativeSourceImageFormat(FrameEncoding::Sdr8, layout);
""", "source transport in native mode")

    source = checked_replace(source, """            return *result;
        }
    }
    const auto& vulkan = this->m_impl->getVulkan();
""", """            return *result;
        }
        // A rejected native context MUST NOT fall back to an LSFG/DLL context.
        // The outer renderer must retain the game's real-frame presentation.
        throw backend::error("GFG native context unavailable; LSFG fallback disabled");
    }
    const auto& vulkan = this->m_impl->getVulkan();
""", "exclusive native context")
    return source


def patch_renderer(source: str) -> str:
    source = checked_replace(source, """    const auto& global = this->config.get().global();

    std::optional<std::filesystem::path> scalingShaderDll;
    if (info.spatialScalingActive) {
""", """    const auto& global = this->config.get().global();
    const char* nativeChoice = std::getenv("GFG_TEMPORAL_ENGINE");
    const bool nativeRequested = nativeChoice &&
        std::string_view(nativeChoice) == "experimental";

    std::optional<std::filesystem::path> scalingShaderDll;
    if (info.spatialScalingActive && !nativeRequested) {
""", "disable optional licensed scaler discovery")

    source = checked_replace(source, """            std::string dll{};
            if (global.dll.has_value())
                dll = *global.dll;
            else
                dll = ls::findShaderDll();

            const auto applicationDevice = identifyApplicationDevice(vk);
""", """            std::string dll{};
            if (!nativeRequested) {
                if (global.dll.has_value())
                    dll = *global.dll;
                else
                    dll = ls::findShaderDll();
            }

            const auto applicationDevice = identifyApplicationDevice(vk);
""", "skip shader DLL lookup on exclusive native path")

    source = checked_replace(source, """            if (frameGenerationRequested && !info.spatialScalingActive)
                throw ls::error("failed to create backend instance", e);
""", """            if (frameGenerationRequested && !info.spatialScalingActive &&
                    !nativeRequested)
                throw ls::error("failed to create backend instance", e);
            // Native capability failure must leave real-frame presentation
            // available rather than make swapchain creation fail.
""", "real-frame fallback on native bootstrap failure")
    return source


def transform(root: Path) -> None:
    targets = (
        (root / "engine/mako-backend/src/mako.cpp", patch_backend),
        (root / "engine/mako-render/src/instance.cpp", patch_renderer),
    )
    # Stage every edit before writing any file: one failed anchor changes nothing.
    staged = []
    for path, patch in targets:
        staged.append((path, patch(path.read_text(encoding="utf-8"))))
    for path, content in staged:
        path.write_text(content, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    transform(args.source.resolve())
    print("GFG Native: DLL-independent TE bootstrap applied (MAKO WSI remains transitional)")


if __name__ == "__main__":
    main()
