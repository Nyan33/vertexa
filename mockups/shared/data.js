// Shared demo content for every mockup, so the concepts are compared on the same scene.
window.VX = window.VX || {};

VX.doc = { name: 'pico_sing.vxa', scene: 'Pico — singLEFT', fps: 24, frame: 9, total: 24, zoom: 100, w: 1280, h: 720 };

VX.tools = [
  { k: 'V', n: 'Selection' }, { k: 'A', n: 'Subselection' }, { k: 'Q', n: 'Free Transform' }, { k: 'L', n: 'Lasso' },
  { k: 'P', n: 'Pen' }, { k: 'T', n: 'Text' }, { k: 'N', n: 'Line' }, { k: 'R', n: 'Rectangle' },
  { k: 'O', n: 'Oval' }, { k: 'Y', n: 'Pencil' }, { k: 'B', n: 'Brush' }, { k: 'E', n: 'Eraser' },
  { k: 'K', n: 'Paint Bucket' }, { k: 'S', n: 'Ink Bottle' }, { k: 'I', n: 'Eyedropper' }, { k: 'M', n: 'Bone' },
  { k: 'H', n: 'Hand' }, { k: 'Z', n: 'Zoom' },
];
VX.activeTool = 'B';

// keys: keyframe positions (1-based). blank: blank keyframes. tw: [from,to,kind] kind = 'classic' | 'shape'
VX.layers = [
  { name: 'Sketch', kind: 'guide', color: '#9b9b9b', keys: [1], len: 24, hidden: true },
  { name: 'Mic', kind: 'normal', color: '#ff5d73', keys: [1, 5, 9, 13, 17, 21], len: 24 },
  { name: 'Arm L', kind: 'normal', color: '#ffb347', keys: [1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23], len: 24, active: true },
  { name: 'Head', kind: 'normal', color: '#46c2ff', keys: [1, 7, 13, 19], tw: [[1, 7, 'classic'], [13, 19, 'classic']], len: 24 },
  { name: 'Body', kind: 'normal', color: '#8b7bff', keys: [1, 13], tw: [[1, 13, 'shape']], len: 24 },
  { name: 'Legs', kind: 'normal', color: '#3ddc97', keys: [1, 9, 17], blank: [], len: 24 },
  { name: 'Shadow', kind: 'normal', color: '#7d8590', keys: [1], len: 24, locked: true },
];
VX.labels = [{ at: 1, text: 'singLEFT' }, { at: 13, text: 'hold' }];

VX.library = [
  { n: 'pico_head', t: 'Graphic', uses: 24 },
  { n: 'pico_arm_L', t: 'Graphic', uses: 12 },
  { n: 'pico_body', t: 'Graphic', uses: 2 },
  { n: 'mic', t: 'Movie Clip', uses: 6 },
  { n: 'shoe', t: 'Graphic', uses: 6 },
  { n: 'btn_play', t: 'Button', uses: 1 },
  { n: 'ref_pose.png', t: 'Bitmap', uses: 1 },
];
VX.libraryFolders = ['characters', 'props', 'refs'];

VX.brushes = [
  { n: 'Flash Classic', sub: 'fill brush · CS6', vec: true },
  { n: 'Animate Fluid', sub: 'variable width', vec: true },
  { n: 'Dry Ink', sub: 'textured · .sut', vec: true },
  { n: 'Taper Ink', sub: 'pressure taper', vec: true },
  { n: 'Soft Pencil', sub: 'grain · raster', vec: false },
  { n: 'G-Pen', sub: 'CSP import', vec: true },
  { n: 'Airbrush', sub: 'raster', vec: false },
  { n: 'Chalk', sub: 'grain · raster', vec: false },
];

VX.swatches = ['#111111', '#ffffff', '#ffd2ad', '#f7a07c', '#ff5d73', '#c2304a', '#ffb347', '#ffe066',
  '#3ddc97', '#1b998b', '#46c2ff', '#2f6fdb', '#8b7bff', '#5b3fd6', '#2b2f5c', '#7d8590'];

// FNF-specific workspace data
VX.anims = [
  { n: 'idle', frames: 14, fps: 24, loop: true, off: [0, 0], arrow: null },
  { n: 'singLEFT', frames: 24, fps: 24, loop: false, off: [-38, 4], arrow: 'left', active: true },
  { n: 'singDOWN', frames: 22, fps: 24, loop: false, off: [-6, -52], arrow: 'down' },
  { n: 'singUP', frames: 20, fps: 24, loop: false, off: [-14, 41], arrow: 'up' },
  { n: 'singRIGHT', frames: 24, fps: 24, loop: false, off: [-52, -2], arrow: 'right' },
  { n: 'singLEFTmiss', frames: 18, fps: 24, loop: false, off: [-30, 10], arrow: 'left' },
  { n: 'singDOWNmiss', frames: 18, fps: 24, loop: false, off: [-8, -40], arrow: 'down' },
  { n: 'hey', frames: 16, fps: 24, loop: false, off: [4, 6], arrow: null },
];

// helpers
VX.isKey = (l, f) => l.keys.includes(f);
VX.tweenAt = (l, f) => (l.tw || []).find(t => f >= t[0] && f < t[1]);
VX.h = (html) => { const t = document.createElement('template'); t.innerHTML = html.trim(); return t.content; };
