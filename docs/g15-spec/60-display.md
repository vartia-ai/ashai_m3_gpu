# Display

Tags follow the [README](README.md#how-facts-are-tagged). The panel is driven
by the DCP display coprocessor through Linux's `apple-drm` KMS driver, a
separate device from the GPU. This section covers what matters for putting GPU
frames on screen.

## Devices

| Fact | Tag |
|---|---|
| `apple-drm` is its own DRM device (card1 on the test machine); the GPU is `asahi` (card0 and renderD128) | [HW] |
| The internal panel is eDP-1, 2560x1664 at 60 Hz, with one CRTC | [HW] |
| Mesa's kmsro already pairs a display device named `apple` with the asahi render node; a Mesa build with GBM enabled is enough | [HW] |

## Scanout

| Fact | Tag |
|---|---|
| Primary plane formats: XR30, AR30, XR24, AR24, XB24, AB24 (plus YUV); overlay planes: AR30, AR24, AB24 (plus YUV) | [HW] |
| Every plane accepts the LINEAR modifier only; GPU-tiled buffers cannot be scanned out directly | [HW] |
| Dumb buffers have 64-byte aligned pitches; a 2560-wide 32-bit buffer is 10240 bytes per row | [HW] |
| A GBM scanout surface allocated through kmsro is imported into the GPU and rendered as a linear BGRA8/BGRX8 target; glmark2-es2-drm's 2560x1664 AR24 framebuffer was scanned out from plane 0 | [HW] |

## Session behaviour

| Fact | Tag |
|---|---|
| A DRM client must be master to modeset. With a compositor running, switching to another VT (`chvt`) makes logind revoke the compositor's master while the panel stays powered and connected; a root client on that VT can then become master | [HW] |
| After the last compositor exits (for example when its display manager is stopped), eDP-1 reports "disconnected" and a forced modeset fails with EINVAL until reboot | [HW] |
| `DRM_CAP_ASYNC_PAGE_FLIP` is not supported | [HW] |

## Open questions

- Why eDP-1 reports disconnected after the compositor exits, and how to bring
  the panel back without a reboot.
- Whether compressed or GPU-tiled scanout is possible at all (only LINEAR is
  advertised).
