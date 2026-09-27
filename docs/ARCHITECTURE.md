# Architecture

Vertexa is four libraries stacked on top of each other, each only depending on
the ones below it:

```
 vertexa (executable)
   └─ vx_app     Qt Widgets: Editor, StageView, tools, panels, dialogs
       └─ vx_render   rasteriser, blend modes, filters, renderer, SVG
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
  tweens (Animate's auto-conversion to "Tween N" symbols), swap symbol.
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
  composited.
- `SvgExport` — writes a frame as SVG with exact cubic paths and SVG filter
  equivalents.

## vx_app

- `Editor` — the application model: the current `Document`, the editing
  context (scene → symbol → nested instance stack for edit in place), frame,
  layer, element selection, tool settings, playback and onion skin.
  Every change goes through `Editor::edit(label, fn)`: `fn` mutates a copy of the
  document, and if it returns `true` the old/new snapshots become one undo step.
  Live previews (dragging, drawing) use `setPreview()` without touching history.
- `StageView` — the canvas: zoom/pan with animated transitions, tablet input
  (pressure, tilt, rotation, eraser end), cached rendering, selection overlays,
  tool cursors.
- `tools/` — one class per tool; `DrawTools` (brush, eraser, pencil, paint
  brush), `BasicTools` (shapes, pen, bucket, ink bottle, eyedropper, hand, zoom),
  `SelectTools` (selection, subselection, free transform, lasso).
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
`test_render` compares rendered pixels, filters included; `test_app`
drives the real `StageView` with synthetic mouse events (drawing, erasing,
selecting, bending, tweening, entering symbols) on the offscreen platform.

```bash
ctest --test-dir build --output-on-failure
```
