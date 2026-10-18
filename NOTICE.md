# GSF third-party and upstream notices

Read this file alongside [LICENSE.md](LICENSE.md), which contains the
full GNU GPLv3 text and the **verbatim historical third-party notices**,
and [COPYRIGHT.md](COPYRIGHT.md), which describes the GSF extraction.

## Provenance categories

| Component | Current relationship to GSF | Attribution / applicable rights |
| --- | --- | --- |
| GFG Extreme | Source of GSF's native and Temporal Engine tree | GPL-3.0-or-later in the covered files; historic copyrights retained |
| MAKO | Upstream for parts of GFG and the optional fusion/renderer adapter | Existing author notices and GPL-3.0-or-later lineage; see `engine/gfg-fusion/source.json` |
| lsfg-vk v2 renderer | Historic lineage of MAKO renderer, **not a required GSF runtime component** | GPL-3.0-or-later upstream lineage described in LICENSE.md |
| Original Decky LSFG-VK plugin, toml++, Bjoern Hoehrmann decoder, Steam Deck Homebrew | Notices inherited from original GFG licensing bundle; **not a claim that all implementations ship in the standalone engine** | BSD-3-Clause / MIT text preserved in LICENSE.md |
| AFMF-Linux by serialexperimentslainnnn | External reference studied in `docs/GSF_AFMF_LINUX_SOURCE_AUDIT_20261018.md`; **not vendored into the GSF source tree at extraction** | Its own MIT license applies to any separately imported upstream code in future |
| AMD FidelityFX Optical Flow / FSR3 | Discussed and referenced as an algorithm or external benchmark; **not a blanket permission to redistribute shaders** | Verify exact vendor source and its applicable license/NOTICE before any import |
| Vulkan-Headers / SPIR-V tools / Mesa / Gamescope | Build/runtime ecosystem dependencies, not code reassigned to GSF | Their independent licenses apply; no project affiliation implied |

**Important:** preserving historical notices is conservative.
It must not be read as a declaration that every third-party component in
GFG Extreme was imported into this smaller GSF extraction.

## Mandatory rules when importing third-party code

1. Record upstream **URL, revision, author, license expression, and the
   actual source files** imported, alongside any required notices.
2. Ensure compatibility with GPL-3.0-or-later and preserve both original
   license text and SPDX identifiers, where present.
3. Include corresponding source and required license notices with
   distributed binary builds; document any dynamically downloaded
   components and their own terms.
4. Do not remove upstream authors, use license changes to claim sole
   ownership, or claim an external vendor endorses GSF.
5. Before first public binary release, review the entire actual
   *distribution payload*, especially generated shaders, MAKO adapter,
   binaries, SDK code and optional dependencies. This document is an
   inventory, **not a completed provenance audit**.

See the complete retained upstream text in
[LICENSE.md](LICENSE.md#third-party-notices).
