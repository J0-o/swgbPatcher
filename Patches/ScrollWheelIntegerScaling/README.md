# Scroll-Wheel Gameplay Zoom

This independent kpatch adds smoothly animated nearest-neighbor zoom to the gameplay view.
The world view is scaled after `view::draw_view` and before the game draws its HUD,
so menus, panels, and the minimap keep their native size.

- Wheel up: smoothly transition to 2x zoom-in.
- Wheel down: step back toward native 1x.
- Each step uses a short eased transition while retaining crisp nearest-neighbor sampling.
- The zoom is centered near the mouse pointer.
- Mouse coordinates inside the world view are remapped to the sampled source area.
- The patch has no D3D11 dependency and can run with the D3D11 renderer patch.

This first implementation intentionally provides integer zoom-in only. True zoom-out
requires changing the engine camera extent and object culling, not scaling a completed frame.

Version 0.1.1 patches the proven main-loop call site directly because `DDRAW.dll` can
replace the executable's `PeekMessageA` import after patch initialization.

Version 0.1.2 disables the engine's incremental world-buffer reuse while zoomed and
requests one clean redraw after returning to 1x. Reusing a previously scaled frame
caused rapidly accumulating terrain corruption while the fixed HUD remained intact.

Version 0.2.0 animates between the existing integer zoom targets over 180 ms. Intermediate
frames use fractional scale values with nearest-neighbor sampling; the HUD remains unscaled
and mouse coordinates follow the current animated scale.

Version 0.2.1 performs the transition with 16.16 fixed-point arithmetic so the injected
render hooks do not disturb the legacy engine's floating-point state during gameplay loading.

Version 0.2.2 preserves all registers and flags around the injected frame-finalizer helper.
The overwritten instruction did not alter registers, and the game immediately reuses EDX and
ECX for a virtual call; allowing a normal C++ helper to clobber them caused the loading crash.

Version 0.2.3 limits zoom to the useful 1x and 2x targets.

Version 0.2.4 keeps the source pixel beneath the mouse at the same screen coordinate
throughout the transition instead of sliding that point toward the view center.

Version 0.2.5 ignores additional wheel input at the 1x and 2x limits without replacing
the current zoom anchor or restarting its transition.

Version 0.2.8 preserves the zoom-in anchor when returning to 1x, so moving the mouse
while zoomed no longer moves the world before the zoom-out transition begins.

Version 0.2.9 immediately returns to 1x when F10 opens the options menu or the player
clicks the fixed HUD outside the world view. This keeps native menu button coordinates
aligned with the mouse without hard-coding resolution-specific UI positions.

Version 0.2.10 uses the normal eased zoom-out transition when opening the native menu.
World-coordinate mouse remapping is bypassed during that transition so the menu buttons
remain aligned while the zoomed background returns to 1x.

Version 0.2.11 replaces the F10 and fixed-HUD input heuristics with the game's shared
popup-active state. Any modal in-game popup now resets zoom immediately to 1x, while
ordinary HUD controls no longer cancel zoom.
