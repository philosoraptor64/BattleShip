# VS results screen solid cream on Metal (missing SetColorWriteMask)

**Status:** FIXED (libultraship Metal backend), 2026-11-07. Verified on macOS 27 (Apple Silicon) and iOS 27 (iPhone 14 Pro).

## Symptom
After a VS match, the end-of-match wipe started, then the results screen stayed solid near-white (~RGB 254,254,245) for its whole duration. Inside the wipe's hexagon the same cream showed instead of the results scene. The game logic was fine: scene 24 (`nSCKindVSResults`) ran normally and logs matched a correctly rendering run.

## Diagnosis
- Reproduced on Metal on **both** macOS and iOS; the OpenGL backend (`SSB64_RENDER_BACKEND=opengl`, Apple debug override added in `port/port.cpp`) rendered the results correctly.
- Skipping the framebuffer photo capture made no difference. Skipping all results cameras still showed cream; skipping only the transition (`lbTransitionSetupTransition` + `lbTransitionMakeCamera`) fixed it.
- `lbTransitionMakeCamera` sets `COBJ_FLAG_ZBUFFER`, so the camera clears depth every frame (and keeps doing so after the wipe pieces are ejected) by redirecting the color image to the Z buffer and filling `GPACK_ZDZ(G_MAXFBZ,0)` = 0xFFFC. In Fast3D that fill goes through the rectangle path, which relies on `GfxRenderingAPI::SetColorWriteMask(false)` to suppress color output (see `color_image_redirect_z_emulation_2026-07-30.md`).
- OpenGL and DirectX 11 override `SetColorWriteMask`; **Metal used the no-op default**, so each depth clear painted 0xFFFC as color over everything drawn before it.

## Fix
`gfx_metal.cpp`: implement `SetColorWriteMask`. Metal bakes the write mask into the pipeline state, so each shader program now retains its `RenderPipelineDescriptor` and lazily builds `pipeline_state_variants_no_color[msaa]` with `ColorWriteMaskNone`; the draw path selects it while color writes are off (rebinding when either the program or the mask changes) and, like the OpenGL and DirectX fixes, switches the encoder to `DepthClipModeClip` for those draws and back to `Clamp` afterwards. If a no-color pipeline can't be built the draw is dropped rather than painting color.

## Audit hook
`GfxRenderingAPI` has optional virtuals with conservative defaults (`SetColorWriteMask`, `ClearColorRegion`, `FbNeedsSampleVFlip`). A backend that skips one fails silently. When a scene renders wrong only on one backend, check which of these that backend leaves at the default. As of this fix Metal still uses the default `ClearColorRegion` (full clear; see `intro_explosion_widescreen_persistence_2026-08-04.md` for the transition that is protected from it).
