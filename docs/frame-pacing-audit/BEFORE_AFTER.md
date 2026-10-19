# Behavior changes and observation limits

| Trigger | Before | Candidate |
|---|---|---|
|45FPS,90Hz,x3,only slotG2|Requested τ2/3 although slot is midpoint|Requested τ1/2, compact GPU output0, original timeline signal mapping preserved|
|Adaptive G1 late but inside old B extension|Move G1/B after phase chosen|Reject G1 at its immutable deadline; keep baseline B|
|Recovery visibility probe|Could allocate12ms beyond an expired slot and bypass pacing|Must own an unexpired slot; same readiness deadline and output pacing|
|Slow completed sample on repeated bypass|te_poll guard in constructor never collected it there|Nonblocking TE poll at every present entry, including bypass paths|
|Missing G1 in next pair|Could inherit timing before0.1.10 fix|Existing per-pair pace_real reset retained|
|Frames log exceeds32MiB|External truncate removed earlier transition history|Writer-owned complete-line segments,512MiB cap, explicit loss state and SHA256 export|
|Installed launcher imports segment exporter|New module missing from installation tuple|Module installed atomically alongside diagnostic/runtime helpers; extracted-package check|
|Recovery report|Single failure label could hide distinct failure stages|Per-process/generation state plus no-retry/no-output/reentry outcomes|

Archived data is not an after-run. The input x3 retained interval had1 accepted
synthetic in169.599s; x2 had66 in85.485s. This candidate cannot be claimed to
improve those hardware results until the SAME scene is rerun. Software test
results establish specified code behavior and actual LMC/WSI transport only.
