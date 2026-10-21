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

## External display (front USB-C)

The second display controller (dcpext) drives the front USB-C port in
DisplayPort alt mode. Setup and limits: [display/README.md](../../display/README.md).
Facts established on J613 with macOS 14.8.9 traces under the m1n1 hypervisor:

| Fact | Tag |
|---|---|
| iBoot never powers dcpext on this machine; its firmware segments are preloaded outside RAM and must be reserved before Linux starts (m1n1 patch), then dcpext is powered and started at runtime | [HW] |
| The T8122 ATC PHY's DP AUX sits in the core region at +0x16000/+0x16400 (the ADT's atc-phyN entries); there is no separate lpdptx block | [HW] |
| The crossbar (atcN-dpxbar) must select state 2 for dcpext, as macOS does; the ADT's dp-switch-ufp-endpoint value 1 leaves AUX dead | [HW] |
| HBR2 link setup uses an AUSPLL command register at +0x2000 and a PLL lock bit at +0x7034, not the generic +0xa74 ready wait; the driver replays macOS's sequence, so only HBR2 and the unswapped orientation are supported | [HW] |
| The bandwidth dashboard (PMGR scratch) must be answered to the firmware with its physical address and no DART mapping, as macOS does; a DART address makes the firmware's power-down fault and crash on every unplug | [HW] |
| Tested: USB-C to HDMI adapter (pin D), HP monitor at 2560x1440@60, picture at boot and after plug, unplug and replug, internal panel unaffected | [HW] |
| Not supported: other link rates, swapped orientation, the rear port, Thunderbolt monitors (DP tunnelling) | [HW] |

## Open questions

- Why eDP-1 reports disconnected after the compositor exits, and how to bring
  the panel back without a reboot.
- Whether compressed or GPU-tiled scanout is possible at all (only LINEAR is
  advertised).
