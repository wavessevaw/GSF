# Steam Deck15W qualification: NOT TESTED

The user's reported test settings are90Hz,15W and AUTO GPU clock. They have not
been independently read from hardware in this executor. No Deck endpoint,
Steam session or DRM render node is available. Both supplied traces are app582160
(AC Origins), build6a5d66ec6384be371d42742ee054bda1617d5a7c. The earlier mention
of Ghost of Tsushima does not turn these into Tsushima logs.

G3 requires matched baseline without GSF and candidate runs, at least3min each,
with the same scene/settings and thermal state. Record requested AND observed
refresh, power and GPU clock independently; preserve AUTO/15W. Separate30→60x2,
45→90x2,30→90x3 and capacity-limited60→90 modes. Keep normal-no-shadow as the
control; do not impose a fixed source FPS as a product requirement.

Use a source frame identifier and independently observed physical output IDs,
e.g. a sufficiently fast external camera with unambiguous markers. WSI IDs,
MangoHud/Steam FPS counters and GOOGLE presentation timestamps alone do not
prove unique OLED images. Report native rate, GPU dispatch/readiness, WSI accept,
compositor feedback and physical uniqueness separately. Measure B delay against
baseline and recoveries after repeated load transitions, not just startup.

No 15W performance, physical90FPS, quality, power attribution or latency guarantee
is claimed. This is the remaining hardware gate, not an automatic conclusion
that RDNA2/Proton/Gamescope can never support generation.
