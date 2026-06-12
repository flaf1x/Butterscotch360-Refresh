# Butterscotch360-Refresh v0.2

This release is the first hardware-tested refresh where vanilla Undertale and
NXTale both reach a much more usable state on Xbox 360.

## Highlights

- Added a startup splash/progress screen while `data.win` is parsed.
- Added persistent diagnostic logging to `bs360_refresh.log`.
- Added an on-screen diagnostic overlay toggled with `LB + RB`.
- Reworked Xbox 360 audio around streamed OGG music playback.
- Added real `application_surface` support to the D3D9 backend.
- Improved crisp pixel output with point sampling and 720p presentation.
- Added lazy texture-page loading to reduce memory pressure.
- Fixed NXTale texture-page parsing when stale WAD metadata is present.
- Fixed NXTale console-style border rendering.
- Fixed NXTale missing sprites and Undyne spear rotation.
- Fixed duplicated movement by keeping the GameMaker gamepad API disabled by default.

## Xbox 360 Diagnostics

The in-game overlay now shows:

- FPS and frame timing.
- Current room index and room name.
- Instance count and pending room state.
- Application surface and GUI sizing.
- Controller connection and speed-up state.
- Physical and virtual memory usage.
- Room transition hold counter for border flicker debugging.

The overlay can be toggled at runtime with:

```text
LB + RB
```

## Undertale

Vanilla Undertale v1.08 is playable in the tested areas with working text,
music, sound effects, room transitions, and stable pixel output.

## NXTale

NXTale now reaches in-game scenes with:

- Working sprites and character rendering.
- Working console button icons.
- Working dynamic/custom borders.
- Correct spear rotation.
- Less visible border flicker during room transitions.

## Known Notes

- The port is still experimental and may need per-game fixes for other
  GameMaker: Studio titles.
- `os_type` is intentionally kept as Windows by default because that matches the
  tested Undertale/NXTale behavior.
- External audio files should be placed next to the `.xex` and `data.win`.
- Xbox 360 memory limits still matter, especially for games with large texture
  pages or many streamed assets.

## Build

The tested build target is:

```text
Release | Xbox 360
```

The produced binary is:

```text
Release/Butterscotch.xex
```

For testing and releases it is usually renamed to:

```text
Butterscotch360-Refresh.xex
```
