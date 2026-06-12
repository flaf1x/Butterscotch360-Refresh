# Butterscotch360-Refresh v0.2

This release note only lists changes made after the previous public release
notes. The earlier release already covered the initial refreshed Xbox 360
project files, D3D9/XAudio2 backend, external OGG playback, diagnostic log,
startup splash/progress screen, and the first Undertale fixes.

## Highlights

- Added an on-screen diagnostic overlay toggled with `LB + RB`.
- Added runtime memory information to the diagnostic overlay.
- Added room transition diagnostics to help catch one-frame rendering issues.
- Added real Xbox 360 D3D9 `application_surface` support.
- Added lazy texture-page loading to reduce Xbox 360 memory pressure.
- Added safer texture-page parsing for games with stale WAD metadata.
- Added NXTale compatibility detection without changing the default `os_type`
  away from Windows.
- Fixed NXTale missing sprites caused by texture metadata/stride mismatch.
- Fixed NXTale console button icons.
- Fixed NXTale console-style/dynamic border rendering.
- Fixed NXTale Undyne spear rotation.
- Fixed duplicated controller movement by keeping the GameMaker gamepad API
  disabled by default unless explicitly enabled.
- Reduced visible border flicker during room transitions.

## Diagnostic Overlay

Press:

```text
LB + RB
```

The overlay currently shows:

- FPS and frame timing.
- Current room index and room name.
- Room size, instance count, and pending room state.
- Application surface and GUI dimensions.
- Controller connection and speed-up state.
- Physical and virtual memory usage.
- Room transition hold counter.

## NXTale

NXTale is now substantially more usable on Xbox 360:

- Characters and important sprites render correctly.
- Console button prompts render.
- Dynamic/custom borders render in the expected console-style layout.
- Undyne spear attacks point correctly.
- The game keeps `os_type` as Windows by default, matching the tested behavior.

## Notes

- Vanilla Undertale v1.08 remains the primary tested baseline.
- NXTale is tested, but still experimental.
- External audio files should still be placed next to the `.xex` and `data.win`.
- `splash.png` remains optional and works the same way as in the previous
  release.
- Xbox 360 memory limits still matter, especially for larger texture pages or
  games with many streamed assets.

## Known Issues

- The port is still not generally optimized for 4:3.
- Other GameMaker: Studio games may need per-game compatibility fixes.
- If you hit a crash or rendering/audio issue, please attach
  `bs360_refresh.log` and a screenshot if possible.
