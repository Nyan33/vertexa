# Roadmap

## 0.1 — first iteration (this release)

- [x] Exact cubic geometry kernel: intersections, planar arrangement, booleans, fitting
- [x] Flash-style shapes with merge drawing and object drawing
- [x] Animate brush (fill brush) with pressure / tilt, tip shapes and brush modes
- [x] Eraser modes and faucet, paint bucket with gap closing, ink bottle, eyedropper
- [x] Line, rectangle, oval, PolyStar, pen, pencil
- [x] Selection (bend, corner drag, marquee cut), subselection, free transform, lasso
- [x] Timeline: layers, folders, masks, motion guides, keyframes, onion skin, Animate shortcuts
- [x] Symbols: movie clip / graphic, library, edit in place, break apart, groups, colour effects
- [x] Classic tweens (rotation, eases, motion guides) and shape tweens (hints)
- [x] 14 Animate + 13 extra blend modes
- [x] Graphics tablets: pressure, tilt, rotation, eraser end, pressure curve
- [x] Vector brushes: art, pattern, textured and scatter; brushes from a selection
- [x] Filters on movie clips and buttons (all seven Animate filters)
- [x] FLA import: binary Flash 5 – CS4 and XFL (CS5+, Animate)
- [x] Per-instance behaviour (movie clip / graphic / button)
- [x] Button symbols: Up / Over / Down / Hit, Enable Simple Buttons
- [x] 9-slice scaling with exact curve splitting
- [x] Library folders
- [x] GPU rendering through OpenGL with a CPU fallback
- [x] Layer cache, multi-threaded rasterisation, background brush previews
- [x] Layer opacity and blending (Layer Properties)
- [x] Loop range for playback
- [x] `.vtx` files, PNG sequence / SVG / video export

## Next

- **Text tool** (static text with embedded outlines)
- **Gradient Transform tool** and bitmap fills
- **Snapping**: to objects, pixels and guides; rulers, grid and guides
- **Align / Transform / Info panels**
- **Camera layer** and **layer parenting**
- **Bones (IK)** for symbols and shapes
- **Motion tweens** (object-based, with Motion Editor) next to classic tweens
- **Audio layers** with scrubbing
- **Scenes** panel (the model already supports several scenes)
- **FLA import, next steps**: bitmaps, text, sounds and shape tweens of binary files; XFL export
- **Import**: SVG, bitmaps; **export**: SWF, animated GIF / APNG, sprite sheets
- **Performance**: filters on the GPU; drawing the stage straight to the window (no read-back) through Qt RHI, which also brings Vulkan, Metal and Direct3D backends
- **Localisation** (Russian first) and customisable shortcuts
- Flatpak, signed and notarised packages
