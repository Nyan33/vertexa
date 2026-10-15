# Vertexa Alpha 2.0

Vertexa has been rewritten from scratch: C17 and sokol (Direct3D 11) instead of C++20 and Qt 6, with its own
interface drawn entirely on the GPU. Documents use a new format (`.vxa`). Windows only for now.

## What's new compared with the old Vertexa

**Interface**
- A new dark interface: a timeline on a pixel grid, panels, a palette and a custom title bar.
- A start screen with recent projects and thumbnails, and a splash screen while loading.
- Documents in tabs: drag to reorder, close with the middle button or `Ctrl+W`, a prompt before losing changes.
- Settings: auto-save and recovery copies, undo steps, interface size, graphics card choice, stage
  anti-aliasing and resolution, rebindable shortcuts, a theme color editor with presets.
- A custom cursor set: an arrow shaped like the V of the logo, macOS-style hands, tool cursors.

**Drawing**
- A Flash-style vector brush: stabilizer, pressure curve with editable points, tilt, tapers, three tip shapes.
- Clip Studio Paint brush import (`.sut`).
- Trim Edge (`C`): cuts a stroke back to the nearest intersections with a click or a drag.
- Umoupen-style selection: the vector itself is picked, whole or just the piece between intersections;
  the box and the lasso cut strokes.
- Free Transform with a movable transformation point, Subselection of nodes.

**Animation**
- Shape tweens between any two drawings, classic tweens.
- A camera layer with a frame on the stage (move, zoom, rotate) and export through the camera.
- Audio layers (WAV, MP3, OGG) with waveforms and playback in sync; the sound is stored in the file.
- Multiple selection of frames and layers (Ctrl / Shift), dragging frames, layers and folders.

**Stage and compositing**
- All 14 Animate blend modes on layers, symbols and bitmaps; masks.
- Filters (Blur, Glow, Drop Shadow, Adjust Color) and color effects (brightness, tint, advanced).
- Gradient and bitmap fills.
- Animate's see-through layers (Shift-click the eye): shown on the stage, left out of exports.

**Images**
- PNG, JPEG and other images can be imported (`Ctrl+R` or drop on the stage); a library with thumbnails.

**FLA import**
- Bitmaps from FLA files (JPEG and Animate's lossless formats), gradients, filters, color effects,
  blend modes, masks and see-through layers.
- File paths with non-Latin characters.

**FNF export**
- The Export Symbols window: the symbols picked in the library become the character's animations.
- Sparrow, FNF V-Slice and Psych Engine formats; ID, scale, position, animation name, prefix, FPS, loop and
  offsets with a live preview (feet origin, idle reference, nudging with the arrow keys).
- One shared frame box, as V-Slice expects, so the character doesn't jump between animations.
  The settings are saved in the `.vxa`.
- PNG sequence export.

**Performance**
- Geometry is built only for the visible part of the stage, so zooming in doesn't drop the frame rate.
