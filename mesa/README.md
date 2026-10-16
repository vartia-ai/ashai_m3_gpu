# Mesa patch for G15 rendering

`g15-render.patch` is the complete G15 render-submission and driver change set
as one diff against Gravity Mesa commit `87ac51d411f0e58a9433567ec2666f457be89e1d`
(the Asahi Gallium driver with Apple9/G15 compiler support). It adds:

- G15 device selection and the G15 GPU VA layout;
- the Gallium render flush for G15 (VDM/PPP streams, attachments, render
  targets up to 8192x8192, linear and GPU-tiled RGBA8/BGRA8, D32F/D16 depth,
  S8 stencil, depth-only passes, mip levels);
- Mesa's own background/reload and end-of-tile programs;
- external textures (needed by Hyprland), and per-render primitive/draw caps
  that keep tile lists within the hardware limit described in
  `docs/g15-spec/70-known-limits.md`;
- environment switches (`ASAHI_G15_*`) for experiments and diagnostics.

The public patch omits one private diagnostic (loading macOS-compiled
fragment shaders from local files); otherwise it equals the development branch.

Apply with `git apply g15-render.patch` on that commit. The driver talks to the
G15 kernel driver's UAPI (`DRM_ASAHI_*` G15 extensions), which is not
published yet; without it the patch builds but cannot render.

Set `ASAHI_G15_GRAPHICS=1` to enable G15 rendering.
