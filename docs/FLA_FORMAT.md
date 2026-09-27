# The binary .fla format (Flash 5 … CS4)

Before CS5, Flash saved documents as an OLE2 compound file. Adobe never
published the format. This page documents what Vertexa decodes, in enough
detail to re-implement it. Layouts marked **CS4** were derived by Vertexa from
a real Flash CS4 document (63 symbols, 382 keyframes, 526 instances) and are
verified by decoding every stream of that file to its exact end. Other layouts
come from earlier reverse engineering of Flash 8 and MX 2004 files, credited in
[REFERENCES.md](REFERENCES.md).

The implementation is `src/core/io/FlaBinary.cpp`. `tests/test_fla.cpp` builds
a document byte by byte from this description.

All integers are little endian. *twip* = 1/20 px.

## Container

An [MS-CFB] compound file (`D0 CF 11 E0 A1 B1 1A E1`). Streams:

| Stream | Content |
|---|---|
| `Contents` | document settings, the library (one record per scene and symbol), publish settings |
| `P n <timestamp>` (`Page n` before Flash 8) | scene timelines |
| `S n <timestamp>` (`Symbol n`) | symbol timelines; `n` is the symbol number that instances refer to |
| `M n <timestamp>` (`Media n`) | bitmaps and sounds |

## MFC archives

Page and symbol streams start with one byte (`01`) followed by one object
written by MFC's `CArchive::WriteObject`:

| Tag (u16) | Meaning |
|---|---|
| `0000` | null object |
| `FFFF` + u16 schema + u16 length + ASCII name | new class, followed by an object of it |
| `8000 \| i` | object of the class at table index `i` |
| `i` (< `7FFF`) | reference to the object at table index `i` |
| `7FFF` + u32 | long form of the two above (bit 31 = class) |

The table is 1-based and **both classes and objects take an index**, in the
order they are read (an object gets its index before its fields are read).
Strings (`CString`): u8 length + Latin-1, or `FF FE FF` + u8 length (or `FF` +
u16) + UTF-16.

## Objects

Every object starts with the `CPicObj` part:

```
u8 schema, u8 flags
children: objects until a null tag
schema >= 1: s32 x, s32 y        transformation point in twips (INT_MIN = unset)
schema >= 3: u8;  schema >= 4: u8
```

### CPicPage — a timeline

`CPicObj` whose children are the layers, **bottom layer first**, then editor
state (current frame and layer).

### CPicLayer

```
CPicObj (children = keyframes)
u8 schema (13 in CS4), CString name
schema <= 3: u8 type
4..30: u8 current-layer flag, u8 locked, u8 hidden
5..30: u32;  6..30: u32 outline colour (RGBA), u32;  8..30: u32
u8 layer type (0 normal, 1 guide, 3 mask, 5 folder)
object: parent layer (mask, guide or folder) or null
7..8: object;  2..5: u8;  3..8: u8;  >= 9: u8;  >= 10: u8;  >= 11: u8 (CS4)
```

### CPicFrame — a keyframe

A frame is a shape (its merge-drawing artwork) with a keyframe tail:

```
CPicObj (children = instances and drawing objects, bottom first)
u8 shape schema, matrix, shape data            (see Shapes)
u8 frame schema (29 in CS4)
u16 duration in frames (0 = zero-length placeholder, occupies no frames)
u16 flags (bit 0: classic motion tween to the next keyframe)
s16 ease (-100 … 100);  u16 sound;  u16 n + n × 8 bytes
u16, u8, u32, s32;  u16
schema >= 23: CString frame label
timeline sub-object:  u32 type, u32 format;
   type >= 1: u32, u32 n, n × u32
   format 1 and type >= 4: (type >= 5: u32) CString
   format 0: u32, u32 n, n × u32
u32, u32, u32, object (shape tween data), u32, object, CString, u32, u32, u32, u32, u32
```

Keyframes are consecutive: each starts where the previous one ended.

### Matrices

Six s32: `a b c d` in 16.16 fixed point, `tx ty` in twips.

## Shapes

```
u8 data schema (5 in CS4), u32 edge count
u16 fills × fill style
u16 lines × line style
edge records until a 00 byte
data schema > 4: s32 n + n × cubic record (CS4)
```

**Fill style:** u32 colour (bytes R G B A), u8 type, u8 flags. Type `& 0x10`:
gradient (`& 3` ≠ 0 radial) → matrix, u8 stop count, (shape schema > 2: u16,
u8), (data schema >= 5: 5 bytes), stops (u8 ratio, u32 colour). The gradient
square is ±16384 twips. Type `& 0x40`: bitmap → matrix, u32 bitmap.

**Line style:** u32 colour, u16 width in twips (0 = hairline), 4 bytes; shape
schema > 2: u8 start cap, u8 end cap, u8 join, u8, u16 miter, fill style.

**Edge record:** a flags byte, then

* `0x40`: style change — three indices, u8 each when `0x80` is set, otherwise
  u16 with bit 15 masked off. Order: **line, right fill, left fill** (left of the
  edge direction in y-up coordinates; checked with `fillAt` in the tests).
* Three coordinate deltas whose encodings are bits 0–1 (move), 2–3 (control)
  and 4–5 (anchor): 0 = none, 1 = s16 pair, 2 = s32 pair, 3 = s16 pair × 128.
  Units are 1/256 twip (1/128 before Flash 8, shape schema ≤ 2).
* Control encoding 0 means a straight edge. **CS4 (shape schema 6): a straight
  edge is followed by one extra byte** (always 0).

Edges are quadratic Béziers; Vertexa converts them exactly to cubics.

**Cubic record (CS4):** the curves as the artist drew them, kept for editing:
4 points (s32 pairs), u8 k + k × (point, u16 on/off-curve), u8 flags;
flags bit 0 and bit 1 each add one more point. Flash renders the quadratic
edges, so Vertexa skips these.

## Symbol instances

`CPicSymbol` (graphic), `CPicSprite` (movie clip), `CPicButton`:

```
CPicObj (the point is the transformation point)
u8 schema (22 in CS4), matrix
u16 first frame, u16 loop (0 loop, 1 play once, 2 single frame)
u8 has colour transform: 8 × s16 (mulR addR mulG addG mulB addB mulA addA; mul 8.8)
u16 colour effect shown in the panel, u16 percent, u32 colour
CString instance name, u32 symbol number
3 bytes, u8 has filters: u32 n + n × filter
u8 blend mode (SWF numbering: 0/1 normal, 2 layer, 3 multiply, 4 screen, 5 lighten,
   6 darken, 7 difference, 8 add, 9 subtract, 10 invert, 11 alpha, 12 erase,
   13 overlay, 14 hard light)
u16, 16 × f32 3D matrix, 24 bytes, s32 x, s32 y 3D centre, 6 bytes
```

Movie clips and buttons continue with u8 schema, a timeline sub-object,
CString, u32, 18 bytes and a CString (component metadata XML).

**Filter (48 bytes, CS4):** u8 enabled, u8 type, 6 bytes, u32 colour (RGBA),
f32 distance, f32 blur x, f32 blur y, f32 angle (radians), u32 inner, u32
knockout, u32 quality (1–3), u32 strength (%), u32 hide object. Type 3 is Glow
(the only type seen so far).

## Contents

* Library: for each scene and symbol, u8 length + UTF-16 stream name, CString
  display name, u32 symbol number, u8 type (0 graphic, 1 button, 2 movie clip).
* Stage: four s32 `{0, width, 0, height}` in twips; 53 bytes later the
  background colour (RGBA), 62 bytes later the frame rate (u16, 8.8).
* Publish settings are key/value CStrings. `PublishHtmlProperties::VersionInfo`
  lists the Flash Player versions the authoring tool knew (CS4 → 10).

## Not decoded yet

Shape tweens, sounds, bitmaps, text fields, and filter types other than Glow in
the binary format. The importer reports these and imports the rest.
