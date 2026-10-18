# Temporal Engine quality bench

Drop-frame protocol on procedural 3D scenes, so generator changes are judged
offline before anything reaches a device.

1. `render.sh <godot 4.4.1> <frames dir>` renders every scene in `scenes/` at
   1280x800 and a fixed 90 FPS (Godot Movie Maker, GL compatibility renderer).
2. `bench.py <frames dir>` keeps every 3rd frame (a 30 FPS game on a 90 Hz
   panel), asks each generator for the two frames in between and scores them
   against the frames that were really rendered (NVIDIA FLIP overall, inside
   the motion mask and inside the HUD, 95th percentile, temporal error, PSNR).
3. `errmap.py` draws output and FLIP error side by side for one frame.

Generators: `repeat`, `blend`, `te2` (the shipped algorithm, through its
executable reference), `fsr3` (FidelityFX frame interpolation without game
vectors, reimplemented from the SDK shaders) and `lmc` (layered motion
compensation, the next candidate).

Scenes: fast camera flick with pillars (`pan`), swinging sword (`sword`),
glossy spheres with a moving light (`specular`), sparks, smoke and glass
(`particles`), walk through thin posts (`walk`), third-person orbit (`orbit`)
and the same with a sword carried on the back (`carry`).  Every scene draws a
HUD: minimap with a moving dot, quest text, health bar, timer, crosshair.

Requires numpy, scipy, Pillow and `flip-evaluator` (BSD-3, NVIDIA).
