# Architecture

Vertexa is four libraries stacked on top of each other, each only depending on
the ones below it:

```
 vertexa (executable)
   └─ vx_app     Qt Widgets: Editor, StageView, tools, panels, dialogs
       └─ vx_render   rasteriser, blend modes, filters, renderer (CPU / OpenGL), SVG
           └─ vx_core     document model, shape graph, timelines, tweens, .vtx, FLA import
               └─ vx_geom     exact 2D geometry (standard C++ only)
```

`vx_geom` has no dependencies at all; `vx_core` uses only Qt Core (for JSON and
strings). Everything below `vx_app` builds and is tested without a display.

## vx_geom: the exact geometry kernel

The goal is Animate-grade precision: drawing, erasing and filling must never
leave slivers, gaps or wobbles. Three rules make that possible:

1. **Cubic Beziers in double precision everywhere.** Lines are degenerate
   cubics, arcs are split into ≤ 90° cubic pieces. Nothing in the document is
   flattened into polygons; flattening happens only in the rasteriser.
2. **Exact intersections.** `Intersect.cpp` finds curve/curve intersections by
   bounding-box subdivision down to flat pieces, then polishes every hit with
   Newton iterations on the original curves to ~1e-12. Overlapping (coincident)
   pieces are detected explicitly and reported as overlaps instead of thousands
   of points. `Polynomial.cpp` solves quadratics and cubics robustly (line hits,
   extrema, inflections).
3. **Topological labelling instead of sampling.** `Arrangement.cpp` builds a
   planar map (DCEL) from any set of input curves: curves are split at all
   intersections, vertices are merged within a tolerance, and faces are traced
   by turning around vertices by tangent angle (with curvature tie-breaks for
   tangential curves). Each input curve carries a *layer* and optional labels,
   and every face gets its value per layer:
   - **winding layers** sum the signed crossings of their source curves (non-zero
     / even-odd regions, booleans);
   - **label layers** read Flash's `fillStyle0/fillStyle1` from the edges
     around the face.

   Inputs can be put in a *group* known not to cross itself: shape graphs
   emitted by an arrangement are marked planar (a hash of their edges, so any
   later edit drops the mark), and merging into such a drawing only
   intersects the new curves with it, not its edges with each other.

   Because values are propagated across edges and not sampled at points, the
   result is correct even for hair-thin faces.

On top of that:

| File | What it does |
|---|---|
| `Region` | Filled areas as closed cubic contours: union, intersection, difference, xor, normalisation |
| `Fit` | Schneider curve fitting with corner detection and Newton reparameterisation |
| `Outline` | The Animate brush: the exact swept area of a round, elliptic or polygonal tip moving with variable size/angle — tangent envelopes + circular joins/caps, unioned through the arrangement, then refitted within 0.08 screen px |
| `Smooth` | Pointer smoothing / stabiliser for pen input |
| `Affine` | Matrices plus Flash-style decomposition (scale, skew, rotation) used by tweens |

## vx_core: the document model

### Shapes: `ShapeGraph`

Shapes use Flash's XFL model: a list of edges, each an exact cubic with a
left fill, right fill and stroke style (`GEdge{c, fillL, fillR, stroke}`). No
explicit polygons are stored, so "merge drawing" is natural — two shapes on a
layer are simply one graph.

`ShapeOps.cpp` implements all editing on this graph through the arrangement:

- `overlay` — merge drawing (paint normal / fills / behind / selection / inside),
- `erase` — the eraser modes and the faucet,
- `paintBucket` — face lookup with gap closing (small / medium / large gaps are
  4 / 10 / 24 screen px),
- `inkBottle`, `recolorFaces`, `restrokeEdges`,
- selection helpers (click a face, a stroke run, connected parts, marquee /
  lasso areas) and `liftSelection` / `cutByRegion` that split a shape exactly
  along a selection border,
- `bendEdge`, `moveVertex`, `moveHandle` for the Selection and Subselection tools,
- `normalizeGraph` — merges collinear pieces, drops edges with the same fill on
  both sides, re-labels faces.

`renderData()` converts a graph into fill paths (closed contours per style)
and stroke paths, cached on the graph until it is modified.

### Vector brushes: `VectorBrush`

The Paint Brush never produces pixels: `vectorBrushStroke()` turns a centre
line with half widths (`vectorBrushPath()` applies size, pressure curve and
minimum size) into coloured regions, and `vectorBrushGraph()` makes them an
ordinary shape graph that the tool merges like any other drawing.

- *Art* and *Pattern* brushes build a moving frame along the resampled path
  (arc length + smoothed normals) and warp the artwork's contours into it:
  the artwork's x runs along the path, its y across the local width. Pattern
  brushes split the path into tiles whose length follows the artwork's aspect.
  Warped contours are flattened finely and refitted with `fitCurves`.
- *Textured* brushes take the exact swept area, move its boundary along the
  normal by canvas-anchored value noise (`brushNoise`) and punch grain holes
  on a hashed canvas grid (one hole per cell, so holes never overlap; holes
  clear of the edge are added as reversed contours, only edge-crossing ones go
  through a boolean subtraction).
- *Scatter* brushes place hashed ellipse dabs along the frame and union them.
- `makeArtBrush()` / `linesToFills()` make a brush from a selection; brushes
  made that way are stored in `Document::brushes` and saved in the `.vtx`.

### Symbols: buttons and 9-slice

- `instanceSymbolFrame()` maps an instance to a frame of its symbol: graphics
  follow their loop options, movie clips the playback frame, buttons the frame
  of their state (Up, Over, Down). `buttonAt()` finds the topmost button whose
  Hit frame (or Up frame when Hit is empty) contains a point, through nested
  symbols. It returns `EvalItem::source`, the keyframe element an evaluated
  item comes from, which stays the same across tween evaluation. The stage's
  hot button and its state travel to the renderer in `RenderOptions`.
- `Scale9` — 9-slice scaling. For an instance scaled by (sx, sy) the symbol
  space is remapped piecewise-linearly so the corners keep their size. Edges
  are split exactly where they cross the guides (cubic roots), so each piece
  lies in one cell and is mapped by one affine map. The renderer and SVG
  export apply it to the symbol's own shapes; nested symbols scale normally.

### Filters: `Filter`

`Filter` holds the seven Animate filters with their parameters and SWF
numbering; `lerpFilters` interpolates filter lists in classic tweens and
`filterMargin` tells the renderer how far a filter reaches outside the
object.

### Import: `io/`

- `Cfb` — an OLE2 / Compound File Binary reader (FAT, DIFAT, mini stream,
  directory tree).
- `FlaBinary` — binary `.fla` (Flash 5 – CS4): walks the MFC `CArchive` object
  streams (`CPicPage` → `CPicLayer` → `CPicFrame` → `CPicShape` /
  `CPicSymbol` …) and maps them onto the Vertexa model. The layouts are in
  [FLA_FORMAT.md](FLA_FORMAT.md).
- `Zip` + `Xfl` — a small ZIP reader with its own inflate, and the XFL reader
  (`DOMDocument.xml`, `LIBRARY/*.xml`, edge strings, gradients, filters,
  tweens). Which side of an edge `fillStyle0` is on is decided per document by
  a topology vote.
- `importFla()` detects the format and returns an `ImportReport` listing
  anything that could not be imported exactly.

### Elements, timelines, documents

- `Element.h` — immutable elements shared through `shared_ptr<const Element>`:
  `ShapeElement` (merge shape or drawing object), `InstanceElement` (symbol
  instance with its own behaviour, colour effect, blend mode, filters and
  graphic loop), `GroupElement`, `MorphElement` (baked shape tween frame).
- `Timeline.h` — layers (normal, guide, mask, folder; parent links), keyframe
  spans, tween settings, labels.
- `Document.h` — scenes, symbols (movie clip, graphic, button), vector brushes
  made in the document, stage settings.
  A `Document` is a value: copying it is cheap because elements are shared,
  which makes undo a simple snapshot.

### Commands and evaluation

- `TimelineOps` — F5/F6/F7 & co, copy/paste/move frames, reverse, layer drag.
- `DocumentOps` — convert to symbol, break apart, group, create classic/shape
  tweens (Animate's auto-conversion to "Tween N" symbols; when the next
  keyframe holds the same drawing under an affine map, found from matching
  edges, it becomes an instance of the same symbol with that matrix), swap
  symbol.
- `Tween` — classic tween matrices: decompose both keyframe matrices, interpolate
  scale/skew/rotation/translation with the selected rotation mode and ease,
  follow a `GuidePath` (arc-length parameterised motion guide) with optional
  orient-to-path.
- `ShapeTween` — shape tween: matches fills/strokes between keyframes, resamples
  contours to a common point structure, aligns them with shape hints, and blends
  in distributive or angular mode.
- `Easing` — classic ease, 27 Penner presets, custom Bezier curves.
- `Evaluate` — resolves "what is on this layer at this frame" into
  `EvalItem`s with final matrices, colour transforms and blend modes, including
  graphic-symbol frame mapping and motion guides.
- `Serialize` — the `.vtx` file: a JSON document
  (`{"format":"vertexa","version":…,"stage":…,"scenes":[…],"symbols":[…]}`)
  that stores curves losslessly, plus the clipboard format.

## vx_render

- `Raster` — a scanline rasteriser with exact area coverage (accumulation buffer
  in the style of font-rs). All fills of one shape are rasterised in a single
  pass with a *coverage-weighted colour sum*, so the edge shared by two fills
  gets full coverage and never shows the background through a seam.
  Curves whose control points lie outside the clip are replaced by their
  chords (only their end points matter for the winding inside it), and only
  segments crossing the clipped rows are sorted, so a zoomed-in view or a
  band of the frame costs what it shows.
- `Blend` — premultiplied compositing with all Animate blend modes plus the
  Krita/W3C ones. As in Flash, a blended movie clip is rendered into its own
  buffer first, and Alpha / Erase only act inside a parent set to Layer.
- `Filters` — the filter stack on an isolated buffer: separable box blurs (one
  pass per quality step, like Flash), shadow / glow / bevel built from the
  blurred alpha, gradient variants through a 256-entry ramp, colour matrix for
  Adjust Color.
- `Renderer` — walks evaluated items, renders nested symbols with isolation,
  masks, outline mode, onion skins and the edit-in-place dimming. A movie clip
  with filters or a blend mode is rendered into a buffer bounded by its
  symbol bounds plus the filter margin, filtered, colour-transformed and then
  composited. It draws through a `Surface`, which decides where the pixels
  are made: `CpuSurface` wraps a `QImage` and uses the rasteriser above,
  `GlSurface` draws on the GPU.
  Large CPU frames are drawn in horizontal bands on a thread pool: each band
  is an image over the target's own rows and walks the document itself, so
  nothing drawn for one band (a filter margin, an isolated layer) reaches
  another. Shape graphs build their render data and topology behind a
  mutex, as several threads read them. Strokes (other than hairlines and
  non-scaling ones, which QPainter strokes on screen) are filled from their
  outlines through the same rasteriser: the outline of each chain is made
  once per scale bucket and kept by content (FNV hash of the curves, the
  pen and the bucket), so redrawing a drawing after a stroke was merged into
  it only outlines the chains that changed, and bands don't stroke again.
- `LayerCache` — stage rendering that keeps the pixels of layers that do not
  change. A key of what each top-level layer draws (elements, matrices,
  colour effects, filters, nested symbol frames; shape graphs by their
  immutable, pinned pointers) is computed on every render. Consecutive
  layers whose keys have not changed for a few renders are drawn once into a
  cropped image and composited afterwards; changing layers (animated, being
  edited) and layers that blend with what lies below are drawn every time,
  over the pixels below them (on the GPU the target is uploaded first). New
  cached runs are assembled from older ones, so a layer that stops changing
  joins its neighbours without redrawing them. A change of view, size or
  options starts afresh.
- `RhiRenderer` — the GPU path of the stage, on Qt RHI (Vulkan, Metal,
  Direct3D 11/12, OpenGL; built with Qt 6.7+ and Qt Shader Tools). Same
  techniques as `GlRenderer` below, with shaders written once in GLSL 4.40
  and compiled by `qsb` into SPIR-V, GLSL, HLSL and MSL (`render/shaders`).
  An `RhiSurface` records its draws and turns them into a render pass when
  its pixels are needed (composited into a parent, shown or read); passes
  are recorded in order, so render targets go back to a pool as soon as no
  pending draw reads them (spares not asked for in 120 frames, or beyond
  512 MB, are freed; depth-stencil buffers are shared by the targets of a
  size). Every RHI resource is freed with `deleteLater()`: Direct3D 11 runs
  a frame's commands only when the frame ends. Uniforms and transient vertices live in per-frame
  dynamic buffers, each draw in its own slice; gradients use rows of a ramp
  atlas; shape geometry is cached per shape and scale bucket and kept alive
  by the draws that use it. A surface that must read its own pixels (a blend
  mode needing the destination, a colour transform) copies its texture and
  starts a new pass from the copy. Filtered instances are drawn and filtered
  on the CPU (`Surface::makeFilterLayer`) and uploaded, so nothing is ever
  read back. `present()` shows the frame on a QRhiWidget's target with the
  stage backdrop (margin, drop shadow, paper) drawn by a shader.
- `GlRenderer` — the OpenGL path (3.3 core, or ES 3.0), on an offscreen
  context of the GUI thread:
  - fills and strokes use *stencil-then-cover* in a multisampled framebuffer:
    triangle fans of each contour count the non-zero winding in the stencil
    buffer (two-sided increment / decrement), then one quad paints the
    covered samples with the fill colour or a gradient (a 256-entry ramp
    texture) and resets the stencil. Neighbouring fills are flattened from
    the same edges, so every sample belongs to exactly one of them and no
    seam shows;
  - flattened geometry is cached per shape in shape space (half-octave scale
    buckets, vertex buffers, 256 MB LRU budget) and placed by the vertex
    shader, so panning and playback reuse it; fans are split into short
    chunks to keep stencil overdraw low;
  - isolated layers are pooled framebuffers; blend modes, masks and colour
    transforms are shaders that port the CPU formulas (fixed-function
    blending for Normal / Layer / Alpha / Erase). Filters still run on the
    CPU on the filtered instance's own pixels;
  - `renderAccelerated()` falls back to the CPU renderer when there is no
    usable context, when the driver is a software one (llvmpipe, SwiftShader…
    unless `VERTEXA_GPU=force`) or when GPU rendering is turned off.
- `SvgExport` — writes a frame as SVG with exact cubic paths and SVG filter
  equivalents.

## vx_app

- `Editor` — the application model: the current `Document`, the editing
  context (scene → symbol → nested instance stack for edit in place), frame,
  layer, element selection, tool settings, playback and onion skin.
  Every change goes through `Editor::edit(label, fn)`: `fn` mutates a copy of the
  document, and if it returns `true` the old/new snapshots become one undo step.
  Live previews (dragging, drawing) use `setPreview()` without touching history.
- `StageCanvas` — with RHI, the stage is a QRhiWidget that draws the frame
  with `RhiRenderer` straight into the window, under a transparent
  `StageOverlay` where tools paint their feedback with QPainter; both let
  input through to the `StageView`. The backend is chosen once per run by
  trying the preferred APIs on a real device (software devices are skipped).
- `StageView` — the canvas: zoom/pan with animated transitions, tablet input
  (pressure, tilt, rotation, eraser end), rendering through the `LayerCache`,
  selection overlays, tool cursors, the right-click menu (it first selects
  what is under the pointer). The rendered frame, the stage and its
  cached backdrop are composed into one opaque image when the frame changes,
  so repainting under tool feedback is a single copy.
- `CrashHandler` — logs and crash reports. A Qt message handler writes
  every message (and the `vx.*` breadcrumbs: edits, undo, tools, files,
  renderer) to the run's log and to a fixed ring buffer. Crashes are caught
  by signal handlers on an alternate stack (SIGSEGV, SIGBUS, SIGFPE, SIGILL,
  SIGABRT) on Linux and macOS, by an unhandled-exception filter plus the CRT's
  abort / pure-call / invalid-parameter hooks on Windows, by a terminate
  handler (the message of an uncaught exception) and by the message handler
  for qFatal. The handler only uses what was prepared at start (paths, the
  header with version, system, Qt and context, the ring buffer) and plain
  system calls: it writes the report — header, crash, stack
  (`backtrace_symbols_fd`; `StackWalk64` with the PDB and a minidump on
  Windows), the log — then, with a timer against hangs, saves unsaved work
  through a callback and adds a demangled stack. Each run holds a lock file;
  the next start finds the stale ones (`uncleanSessions`) and `MainWindow`
  shows `CrashDialog` with the report and the recovered work. Unsaved work
  is also written every two minutes, off the GUI thread.
- `PropertiesPanel` rebuilds only when what it shows changes: moving the
  playhead rebuilds just the Frame section, and edits that do not touch the
  shown values (painting, moving other objects) leave it alone.
- `tools/` — one class per tool; `DrawTools` (brush, eraser, pencil, paint
  brush), `BasicTools` (shapes, pen, bucket, ink bottle, eyedropper, hand, zoom),
  `SelectTools` (selection, subselection, free transform, lasso).
  Freehand strokes are inked piece by piece into an image of the view (`Ink`;
  a new sample repaints only its own area of the stage) and, once released,
  go through a `StrokeQueue`: the shape is built and merged into the layer's
  drawing on a worker thread, and committed on the GUI thread in the order
  the strokes were drawn, on the frame they were drawn on. The ink stays up
  until then. Free Transform selects like the Selection tool (the drawing
  under the pointer, a marquee in empty space); the transformation point of
  a symbol, group or drawing object is stored in it, that of anything else
  in the editor's selection (`Editor::selectionPivot`), which transforms
  carry along.
- `panels/` — Tools, Timeline, Properties (with the filter stack and Animate's
  grouped blend list), Library, Color, Brushes (vector brush presets, per-kind
  editor, art / pattern brush from selection).
- `Theme`, `Icons`, `Widgets` — the design system: Inter / Inter Display,
  flat dark and light palettes with a single warm accent, procedural icons and
  small animated controls (scrubbable numbers, segmented buttons, colour
  picker, curve editor).

## Tests

`tests/` has one executable per library. `test_geom` and `test_shape` check the
kernel (intersections, arrangement labelling, booleans, fitting, the brush
outline, merge/erase/bucket semantics); `test_core` covers timelines, tweens,
symbols and serialization; `test_brush` the vector brushes; `test_fla` the
OLE2 and ZIP readers and both FLA importers on synthetic files (set
`VERTEXA_FLA_SAMPLES` to a folder of real `.fla` files to import those too);
`test_render` compares rendered pixels, filters included; `test_gpu`
(opt-in with `VERTEXA_TEST_GPU=1`, CI runs it under Xvfb with Mesa and on
Windows with WARP) renders the same scenes with every GPU path (OpenGL, RHI
on Vulkan, OpenGL and Direct3D 11 / 12) and the CPU and compares them; `test_stage_gpu` does the same for the whole
stage shown through `StageCanvas`; `test_crash` runs itself again to crash
in several ways (invalid access, abort, uncaught exception, qFatal, a worker
thread, a killed run) and checks the report, the log, the saved work and what
the next start finds; `test_app`
drives the real `StageView` with synthetic mouse events (drawing, erasing,
selecting, bending, tweening, entering symbols) on the offscreen platform.

```bash
ctest --test-dir build --output-on-failure
```
