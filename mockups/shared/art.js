// A small original character drawn as vector shapes, used as the "artwork" on every stage.
// VX.art({ ghost: '#hex' }) draws an onion-skin ghost (outlines only, tinted).
window.VX = window.VX || {};

VX.art = function (o = {}) {
  const g = o.ghost;
  const ink = g || '#141414';
  const P = g
    ? { skin: 'none', hood: 'none', pants: 'none', cap: 'none', hair: 'none', shoe: 'none', mic: 'none', dark: 'none', blush: 'none', tongue: 'none', white: 'none', shadow: 'none' }
    : { skin: '#ffd8b4', hood: '#ff6f5e', pants: '#2b2f5c', cap: '#2ec4b6', hair: '#3a5bd9', shoe: '#ffffff', mic: '#3b3b44', dark: '#141414', blush: '#ff8fa3', tongue: '#e04b5f', white: '#ffffff', shadow: 'rgba(20,20,40,.13)' };
  const sw = g ? 3 : 6;
  // limb = wide ink stroke under a narrower colour stroke -> outlined tube
  const limb = (d, w, c) => g
    ? `<path d="${d}" fill="none" stroke="${ink}" stroke-width="${sw}" stroke-linecap="round" stroke-linejoin="round" opacity=".9"/>`
    : `<path d="${d}" fill="none" stroke="${ink}" stroke-width="${w + 12}" stroke-linecap="round" stroke-linejoin="round"/>` +
      `<path d="${d}" fill="none" stroke="${c}" stroke-width="${w}" stroke-linecap="round" stroke-linejoin="round"/>`;
  const S = `stroke="${ink}" stroke-width="${sw}" stroke-linejoin="round" stroke-linecap="round"`;
  return `
<svg class="vx-art" viewBox="0 0 360 480" xmlns="http://www.w3.org/2000/svg">
  <ellipse cx="184" cy="452" rx="104" ry="13" fill="${P.shadow}"/>
  ${limb('M156 330 L146 424', 30, P.pants)}
  ${limb('M212 330 L230 422', 30, P.pants)}
  <path d="M112 428 Q114 410 146 412 Q170 414 168 430 Q166 442 140 442 Q112 442 112 428 Z" fill="${P.shoe}" ${S}/>
  <path d="M206 428 Q208 410 236 412 Q262 414 262 430 Q262 442 234 442 Q206 442 206 428 Z" fill="${P.shoe}" ${S}/>
  ${limb('M136 246 Q96 252 66 232', 26, P.hood)}
  <circle cx="58" cy="228" r="15" fill="${P.skin}" ${S}/>
  <path d="M126 228 Q180 204 236 228 L250 334 Q182 356 112 334 Z" fill="${P.hood}" ${S}/>
  <path d="M146 300 Q182 312 218 300 L214 330 Q182 338 150 330 Z" fill="none" ${S}/>
  <path d="M168 232 L164 270 M196 232 L200 270" fill="none" stroke="${ink}" stroke-width="${g ? 2 : 4}" stroke-linecap="round"/>
  <circle cx="164" cy="274" r="4" fill="${P.white}" ${S} stroke-width="${g ? 2 : 3}"/>
  <circle cx="200" cy="274" r="4" fill="${P.white}" ${S} stroke-width="${g ? 2 : 3}"/>
  ${limb('M234 246 Q288 270 262 206', 26, P.hood)}
  <g transform="rotate(-22 252 188)">
    <rect x="244" y="182" width="16" height="46" rx="7" fill="${P.mic}" ${S}/>
    <circle cx="252" cy="170" r="17" fill="${P.mic}" ${S}/>
    ${g ? '' : `<path d="M240 166 H264 M238 174 H266" stroke="#6b6b78" stroke-width="2.5"/>`}
  </g>
  <circle cx="262" cy="206" r="15" fill="${P.skin}" ${S}/>
  <path d="M120 120 L90 150 L114 148 L94 176 L122 162 L112 190 L140 160 Z" fill="${P.hair}" ${S}/>
  <circle cx="180" cy="150" r="72" fill="${P.skin}" ${S}/>
  <path d="M108 138 Q112 74 180 70 Q248 72 254 132 Q182 112 108 138 Z" fill="${P.cap}" ${S}/>
  <path d="M110 134 Q76 124 52 142 Q86 156 124 144 Z" fill="${P.cap}" ${S}/>
  <path d="M150 96 Q180 86 214 96" fill="none" stroke="${ink}" stroke-width="${g ? 2 : 4}" stroke-linecap="round"/>
  <ellipse cx="160" cy="152" rx="9" ry="14" fill="${P.dark}" ${g ? S : ''}/>
  <ellipse cx="206" cy="150" rx="9" ry="14" fill="${P.dark}" ${g ? S : ''}/>
  ${g ? '' : `<circle cx="157" cy="146" r="3.4" fill="#fff"/><circle cx="203" cy="144" r="3.4" fill="#fff"/>`}
  ${g ? '' : `<ellipse cx="138" cy="176" rx="12" ry="7" fill="${P.blush}" opacity=".55"/><ellipse cx="228" cy="174" rx="12" ry="7" fill="${P.blush}" opacity=".55"/>`}
  <path d="M164 186 Q184 214 204 184 Q184 194 164 186 Z" fill="${P.dark}" ${S}/>
  ${g ? '' : `<path d="M174 198 Q184 206 194 197 Q184 194 174 198 Z" fill="${P.tongue}"/>`}
</svg>`;
};

// Fill every [data-art] element: data-art="main" | "ghost:#hex:dx:dy:rot"
VX.paintArt = function (root = document) {
  root.querySelectorAll('[data-art]').forEach(el => {
    const spec = el.getAttribute('data-art').split(':');
    if (spec[0] === 'ghost') {
      el.innerHTML = VX.art({ ghost: spec[1] });
      el.style.transform = `translate(${spec[2] || 0}px, ${spec[3] || 0}px) rotate(${spec[4] || 0}deg)`;
    } else {
      el.innerHTML = VX.art();
    }
  });
};
document.addEventListener('DOMContentLoaded', () => VX.paintArt());
