# Remaining limits

1. The confirmed blocker is scoped to the current zero-extra-buffer direct WSI
   schedule when delivery+compute exceeds the remaining G slot. The retained
   trace supports that inequality, not a global Linux/RDNA2 impossibility claim.
2. Changing admission/shadow thresholds cannot overcome the causal window. An
   independent retained-source/output owner is required before enabling any
   controlled buffer. Its resource, latency and loss policy is not finished.
3. The private-owner proof uses real LMC and real WSI but is test-only: worker
   waits, grayscale inputs, one queue family, output reanchor and limited app
   contract. No game can be qualified from it. It is excluded from Decky builds.
4. Live forecasting still lacks full GPU copy/acquire budget measurement. A
   successful readiness wait is not proof the later WSI call met its slot.
   Late acquired images require explicit release/retirement; consuming only
   the semaphore cannot safely return them. This resource contract remains open.
5. WSI acceptance is not compositor display and not panel scanout. Refresh-query
   failure retains truthful provenance; no hardcoded90Hz is inserted into the
   launcher. CI's explicit90Hz test override is never a hardware observation.
6. The old logs lost their beginning. A30FPS startup claim cannot be reconstructed.
   New detail may stop at the configured cap or low disk; manifest records that
   loss instead of silently truncating history. game.log's separate existing
   console cap is not a substitute for retained frames telemetry.
7. Local /proc PID sampling refers to a different namespace and is invalid for
   the stress child. Its recorded304KiB samples are excluded. In-process getrusage
   in CI reports peak RSS; a stable peak alone is not an exhaustive leak proof.
8. G3 and matched15W B-over-baseline/input→display latency are unmeasured. The
   package is an audit candidate with safety/correctness fixes, not release-ready
   continuous2x/3x on Steam Deck.
