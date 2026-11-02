# Galactic Battlegrounds native D3D11 renderer

`RendererD3D11MenuWindow.kpatch` is the project's native renderer patch.

Version 0.9.2 intercepts the game's imported `DirectDrawCreate` and returns a native DirectDraw v1 compatibility object. CPU-backed 8-bit surfaces, palettes, clippers, locks, GDI DC access, and color-keyed blits are implemented inside the patch. The primary surface is uploaded as an 8-bit index texture and a D3D11 pixel shader resolves it through a separate 256-color palette texture. Palette update batches are presented without per-update vertical-sync waits so partially updated palettes do not become visible frames. The 800x600 main-menu window can be resized or maximized; DXGI scales the complete image to the client area and the window procedure maps mouse movement and button coordinates back into the game's fixed 800x600 coordinate space. Menu centering is posted to the window message queue so it runs after the game finishes its synchronous return-to-menu positioning. Gameplay display modes remain native-size and borderless. The patch owns a native display palette and propagates it to CPU surfaces without an explicit palette so their GDI DCs resolve RGB text colors correctly.

The 800x600 menu uses an exact 800x600 client area with a normal border and movable title bar. It is initially centered in the monitor work area and may be resized afterward. Other display modes remain borderless at their native pixel dimensions.

No call is made to the imported DirectDraw implementation. The log includes an explicit `ZERO FORWARDING` marker and an original-call count of zero. Windows may still load a local `ddraw.dll` while resolving the executable's imports before KPM injection, but the patched rendering path never calls it.

Successful startup creates `Game\patches\gb-d3d11-bootstrap.log`.
