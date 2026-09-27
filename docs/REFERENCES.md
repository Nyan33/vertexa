# References

Vertexa is written from scratch. The projects and papers below were studied for
behaviour, file formats and algorithms; no source code was copied from them.

## Applications

| Project | What we look at |
|---|---|
| **Adobe Animate / Flash** | The behaviour Vertexa reproduces: merge vs. object drawing, the fill-based brush and its modes, eraser modes, paint bucket gap closing, selection bending, symbols and edit in place, classic / shape tweens, blend modes, timeline shortcuts. |
| [**Krita**](https://invent.kde.org/graphics/krita) | Texture brush design: auto and predefined tips, spacing, sensor curves, the "Pattern" (texture) option with multiply / subtract / height modes, wash vs. build-up, `.gbr` brush tips, blend modes beyond Flash, tablet handling and pressure curves. |
| [**OpenToonz**](https://github.com/opentoonz/opentoonz) | A production 2D animation tool with vector levels and region filling (gap closing, "autoclose"), Xsheet/timeline workflows and tablet input on all platforms. |
| [**Flare**](https://github.com/Flare-Animate/Flare) ([site](https://flare-animate.github.io/website/)) | An open-source animation editor built around Flash/Animate workflows. Its documentation refers to the Adobe Flash API and to similar projects; we use it to check feature coverage and naming. |
| [**Ruffle**](https://github.com/ruffle-rs/ruffle) | Flash player in Rust: a reference for how SWF shapes (edge lists with fillStyle0/1) turn into filled regions, and how Flash applies blend modes, colour transforms and masks. |
| [**fla-viewer**](https://github.com/lifeart/fla-viewer) | Reads XFL/FLA documents: a reference for the DOMDocument / DOMTimeline / DOMLayer / DOMFrame / DOMShape structure that Vertexa's model mirrors (for future import). |
| [**Paper.js**](https://github.com/paperjs/paper.js) | Robust boolean operations and curve fitting on cubic Beziers (winding contributions, Newton reparameterisation in fitting). |

## Specifications

- Adobe, *SWF File Format Specification* v19 — shape records, fill/line styles,
  matrices and colour transforms.
- XFL (the unzipped `.fla` format) — `DOMShape` edges with `fillStyle0`,
  `fillStyle1`, `strokeStyle`, `DOMSymbolInstance`, tweens and eases.
- W3C, *Compositing and Blending Level 1* — the separable and non-separable
  blend mode formulas.
- Qt 6, `QTabletEvent` and the Windows Ink / WinTab platform options.

## Papers and algorithms

- P. J. Schneider, *An Algorithm for Automatically Fitting Digitized Curves*,
  Graphics Gems (1990) — stroke and outline fitting.
- T. W. Sederberg, T. Nishita, *Curve intersection using Bézier clipping* (1990)
  and bounding-box subdivision methods — curve/curve intersection.
- M. de Berg et al., *Computational Geometry* — doubly connected edge lists and
  planar arrangements.
- R. Penner, *Robert Penner's Easing Equations* — preset eases.
- R. Levien, [*font-rs*](https://github.com/raphlinus/font-rs) — accumulation-
  buffer scanline rasterisation with exact area coverage.
- J. F. Blinn, *Compositing* (premultiplied alpha).

## Assets

- [Inter](https://rsms.me/inter/) and Inter Display by Rasmus Andersson —
  SIL Open Font License 1.1 (`resources/fonts/OFL.txt`).
- All icons, brush tips and paper textures are generated procedurally in code.
