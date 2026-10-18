# GSF copyright and source provenance

GSF is an open-source GPU frame-generation research project. Its
[software license is GPL-3.0-or-later](LICENSE.md), subject to preserved
notices for identified third-party material. **Licensing is not a transfer
of copyright title:** each author retains copyright in their respective
original contributions unless they have executed a separate lawful
assignment. The project does not claim ownership of third-party code.

## Attribution and audit trail

- **Original GFG Extreme / MAKO lineage:** `Copyright (C) 2025-2026
  Eugenio Segala and contributors.` Preserve this upstream notice without
  presenting it as the sole author of all later GSF work.
- **GSF work first published in 2026:** copyright remains with the
  respective GSF contributors who authored their additions. The GitHub
  account `wavessevaw` administers the GSF repository; this does not
  by itself establish who personally owns copyright in each file.
- **Native Vulkan code, Temporal Engine, LMC shaders, docs and tests:**
  originated from GFG Extreme experimental development and were copied
  **byte-for-byte by Git blob** at the migration snapshot
  [`1b600fbc`](https://github.com/wavessevaw/GFG-Extreme/tree/1b600fbc4e6a03a9616fd65ee2f5b16f5224da00).
  All 234 selected source blobs matched. Historic authorship belongs to
  original authors, not to the extraction tool or to this new repo.
- **MAKO-linked adapter and fusion patch:** see
  `engine/gfg-temporal/mako`, `engine/gfg-fusion/source.json`
  and the original MAKO source; their separate lineage must remain
  attributable. The GSF project does not own the MAKO project.
- **Original code before the extraction:** full historical commits,
  PR discussion and provenance remain in the
  [GFG Extreme repository](https://github.com/wavessevaw/GFG-Extreme/pulls);
  see [MIGRATION.md](MIGRATION.md). New GSF Git history is not a
  reconstructed author-by-author history.

## Ownership and licensing of future contributions

Unless an individual file explicitly carries a different compatible
third-party license:
- New independently authored GSF source contributions accepted into
  this GPL-covered project are submitted under
  `SPDX-License-Identifier: GPL-3.0-or-later`.
- Contributors **retain their own copyrights**. The project is not
  requesting a contributor license assignment or contributor license
  agreement.
- Contributions must include source provenance and any mandatory
  notices when incorporating someone else's work. Use a commit
  `Signed-off-by` DCO line to record authorship/permission.
- A project name, logo, domain or repo ownership does not establish
  exclusive ownership of an algorithm or a right to forbid GPL-authorized
  use of the software.

## Corrections and infringement reports

If you identify omitted attribution, an incompatible imported license,
or source published without the copyright holder's authorization,
open an issue in this repository with the source file, precise upstream
URL/revision, applicable license and evidence. Please avoid posting
personal contact information or confidential material in public issues.

GSF does not represent that its source has passed a formal legal title
or patent clearance audit. Before a commercial distribution or dual
license offer, have a qualified lawyer review the actual file-level
copyright and license chain.
