# Keyboard shortcuts

Vertexa follows Adobe Animate's default shortcut set. On macOS `Ctrl` is `⌘`
and `Alt` is `⌥`. `F1` (Help → Keyboard Shortcuts) lists every command in the
app with its current key.

## Tools

| Tool | Key | | Tool | Key |
|---|---|---|---|---|
| Selection | `V` | | Brush (fill brush) | `B` |
| Subselection | `A` | | Paint Brush (vector brushes) | `Y` |
| Free Transform | `Q` | | Pencil | `Shift+Y` |
| Lasso | `L` | | Eraser | `E` |
| Pen | `P` | | Paint Bucket | `K` |
| Line | `N` | | Ink Bottle | `S` |
| Rectangle | `R` | | Eyedropper | `I` |
| Oval | `O` | | Hand | `H` (hold `Space` for a temporary hand) |
| PolyStar | `Shift+R` | | Zoom | `Z` (`Alt`-click zooms out) |

| Action | Key |
|---|---|
| Object Drawing mode | `J` |
| Swap fill / stroke colours | `X` |
| Default colours | `D` |
| Smaller / bigger brush | `[` / `]` |

### Modifiers on the stage

| Where | Modifier | Effect |
|---|---|---|
| Line, PolyStar | `Shift` | snap angle to 45° / 15° |
| Rectangle, Oval | `Shift` | square / circle |
| Rectangle, Oval | `Alt` | draw from the centre |
| Selection | `Shift` | add to selection; constrain moves to an axis |
| Selection | `Alt`+drag | duplicate while moving |
| Selection | drag an unselected edge / corner | bend the curve / move the corner |
| Selection | double-click a fill or line | select connected fills and lines |
| Selection | double-click an instance / group | edit in place; double-click empty space goes back |
| Subselection | `Alt`+drag a handle | break a smooth point into a corner |
| Free Transform | `Shift` | proportional scale, 15° rotation steps |
| Free Transform | `Alt` | scale around the pivot |
| Pen | `Enter` / `Esc` / `Backspace` | finish / cancel / remove last point |
| Stage | `Ctrl`+wheel, pinch | zoom at the cursor |
| Stage | wheel, `Shift`+wheel | scroll vertically / horizontally |
| Stage | `Esc` | cancel the current drag and deselect |

## Edit

| Action | Key |
|---|---|
| Undo | `Ctrl+Z` |
| Redo | `Ctrl+Y`, `Ctrl+Shift+Z` |
| Cut / Copy | `Ctrl+X` / `Ctrl+C` |
| Paste in Center | `Ctrl+V` |
| Paste in Place | `Ctrl+Shift+V` |
| Duplicate | `Ctrl+D` |
| Clear | `Delete`, `Backspace` |
| Select All / Deselect All | `Ctrl+A` / `Ctrl+Shift+A` |
| Edit Symbols (toggle) | `Ctrl+E` |
| Nudge | arrow keys; `Shift`+arrow moves 10 px |

## Modify

| Action | Key |
|---|---|
| Convert to Symbol | `F8` |
| Break Apart | `Ctrl+B` |
| Group / Ungroup | `Ctrl+G` / `Ctrl+Shift+G` |
| Rotate 90° CW / CCW | `Ctrl+Shift+9` / `Ctrl+Shift+7` |
| Bring to Front / Forward | `Ctrl+Shift+Up` / `Ctrl+Up` |
| Send Backward / to Back | `Ctrl+Down` / `Ctrl+Shift+Down` |
| Document settings | `Ctrl+J` |

Flip Horizontal / Vertical and Combine Objects (Union, Intersect, Punch,
Crop) are in the Modify menu without a default key, as in Animate.

## Timeline

| Action | Key |
|---|---|
| Play / Stop | `Enter` |
| Loop Playback | `Alt+Shift+L` (drag the bracket on the ruler to choose the frames; with frames selected, the loop takes the selection) |
| Previous / Next frame | `,` / `.` |
| First / Last frame | `Shift+,` / `Shift+.` |
| Insert Frame | `F5` |
| Remove Frames | `Shift+F5` |
| Insert Keyframe | `F6` |
| Clear Keyframe | `Shift+F6` |
| Insert Blank Keyframe | `F7` |
| Create Classic Tween | `Ctrl+Alt+T` |
| Create Shape Tween | `Ctrl+Alt+Y` |
| Add Shape Hint | `Ctrl+Shift+H` |
| Clear Frames | `Alt+Backspace` |
| Cut / Copy / Paste Frames | `Ctrl+Alt+X` / `Ctrl+Alt+C` / `Ctrl+Alt+V` |
| Layer Properties (name, type, opacity, blending) | double-click the layer icon; Modify ▸ Timeline ▸ Layer Properties… |
| Select All Frames | `Ctrl+Alt+A` |
| New Layer | `Ctrl+Alt+N` |
| New Symbol | `Ctrl+F8` |
| Onion Skin | `Alt+Shift+O` |

In the timeline panel itself:

| Gesture | Effect |
|---|---|
| Click / `Shift`+click frames | select a frame / extend to a range |
| Drag selected frames | move them (keyframes and spans travel together) |
| Drag a layer name | reorder (children travel along); dropping right under a folder, mask or guide puts it inside |
| Double-click a layer name | rename |
| `Alt`+click the eye / lock | hide / lock all other layers |
| `Ctrl`+wheel | change the frame width |
| wheel, `Shift`+wheel | scroll layers / frames |
| Right-click | frame and layer context menus (tweens, keyframes, reverse, labels, layer types…) |

## View and file

| Action | Key |
|---|---|
| Zoom In / Out | `Ctrl+=` / `Ctrl+-` |
| 100% / Fit stage | `Ctrl+1` / `Ctrl+2` |
| New / Open / Save / Save As | `Ctrl+N` / `Ctrl+O` / `Ctrl+S` / `Ctrl+Shift+S` |
| Import FLA / XFL | `Ctrl+R` (Open also accepts `.fla` and `.xfl`) |
| Enable Simple Buttons | `Ctrl+Alt+B` |
| Export PNG sequence | `Ctrl+Alt+Shift+S` |
| Export video (FFmpeg) | `Ctrl+Alt+Shift+E` |
| Keyboard shortcuts | `F1` |
