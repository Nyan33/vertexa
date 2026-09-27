# Architecture

Vertexa is four libraries stacked on top of each other, each only depending on
the ones below it:

```
 vertexa (executable)
   └─ vx_app     Qt Widgets: Editor, StageView, tools, panels, dialogs
       └─ vx_render   rasteriser, blend modes, dab engine, renderer, SVG
           └─ vx_core     document model, shape graph, timelines, tweens, .vtx
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

### Elements, timelines, documents

- `Element.h` — immutable elements shared through `shared_ptr<const Element>`:
  `ShapeElement` (merge shape or drawing object), `InstanceElement` (symbol
  instance with colour effect, blend mode, graphic loop), `GroupElement`,
  `PaintElement` (texture-brush strokes as data), `MorphElement` (baked shape
  tween frame).
- `Timeline.h` — layers (normal, guide, mask, folder; parent links), keyframe
  spans, tween settings, labels.
- `Document.h` — scenes, symbols (movie clip, graphic, button), stage settings.
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
- `DabEngine` — a Krita-like brush engine: stamps tip images along a stroke with
  spacing, pressure curves for size / opacity / flow, rotation from tilt or
  stroke direction, size / opacity / rotation jitter, scatter, wash vs.
  build-up, and textures anchored to the canvas (multiply, subtract, height).
- `BrushResources` — built-in procedural tips and paper textures, `.gbr`/image
  import.
- `Renderer` — walks evaluated items, renders nested symbols with isolation,
  masks, outline mode, onion skins and the edit-in-place dimming; caches
  rendered paint strokes by element and sub-pixel matrix.
- `SvgExport` — writes a frame as SVG with exact cubic paths.

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
- `panels/` — Tools, Timeline, Properties, Library, Color, Brushes.
- `Theme`, `Icons`, `Widgets` — the design system: Inter / Inter Display,
  flat dark and light palettes with a single warm accent, procedural icons and
  small animated controls (scrubbable numbers, segmented buttons, colour
  picker, curve editor).

## Tests

`tests/` has one executable per library. `test_geom` and `test_shape` check the
kernel (intersections, arrangement labelling, booleans, fitting, the brush
outline, merge/erase/bucket semantics); `test_core` covers timelines, tweens,
symbols and serialization; `test_render` compares rendered pixels; `test_app`
drives the real `StageView` with synthetic mouse events (drawing, erasing,
selecting, bending, tweening, entering symbols) on the offscreen platform.

```bash
ctest --test-dir build --output-on-failure
```
