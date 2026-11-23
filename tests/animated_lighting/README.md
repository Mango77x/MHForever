# Standalone animated lighting test

This synthetic fixture checks normal blending with two opposing bone rotations in OpenGL. It samples the center of a triangle whose screen position stays fixed, so the pixel changes come from animated normals. It exercises per-vertex and per-fragment lighting at 0°, +60°, −60°, +60°, and 0° to catch stale pose data.

## Run

Use Python 3 on Linux with Mesa EGL and desktop OpenGL 3.3. No game data, window, or Python packages are required.

```sh
LIBGL_ALWAYS_SOFTWARE=1 python3 verify_animation.py
```

Expected result: `PASS: 10 framebuffer checks`. This fixture runs its own shaders and does not exercise the MHForever renderer. It can serve as a reference for an equivalent fixture in that backend. It measures pixel correctness, not frame time or shadow performance.
