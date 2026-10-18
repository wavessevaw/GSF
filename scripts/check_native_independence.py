#!/usr/bin/env python3
"""Verify native path cannot silently reopen DLL-based LSFG in GFG bootstrap.

Usage: python3 scripts/check_native_independence.py --source /tmp/patched-mako
The source is the compiled patched MAKO *transitional WSI*, not the final native
presentation layer. Checking absence of files opened requires a Deck trace.
"""
import argparse
from pathlib import Path
import sys

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--source", type=Path, required=True)
    args = p.parse_args()
    backend = (args.source / "engine/mako-backend/src/mako.cpp").read_text()
    renderer = (args.source / "engine/mako-render/src/instance.cpp").read_text()
    required = [
        'temporalMode == gfg_temporal::Mode::Experimental',
        'GFG native context unavailable; LSFG fallback disabled',
        'if (!this->m_impl) return false',
        'GFG Native: experimental compute backend started without Lossless.dll',
    ]
    required_renderer = [
        'if (info.spatialScalingActive && !nativeRequested)',
        'if (!nativeRequested) {',
        'frameGenerationRequested && !info.spatialScalingActive &&',
    ]
    failures = [text for text in required if text not in backend]
    failures += [text for text in required_renderer if text not in renderer]
    if failures:
        print("GFG Native bootstrap contract failed:", failures, file=sys.stderr)
        return 1
    print("GFG Native bootstrap contract: PASS (source-level, not device verified)")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
