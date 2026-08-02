/*
 * RFNM spectrum visualizer engine.
 *
 * Rendering is a WebGL2 port of CyberEther's visualization shaders (MIT, (c) Luigi F. Cruz,
 * github.com/luigifcruz/CyberEther):
 *   - waterfall:  resources/shaders/waterfall/signal.frag.glsl (ring-buffer sampler,
 *                 optional 9-tap vertical Gaussian, colormap LUT)
 *   - lineplot:   resources/shaders/global/thicklinestrip.comp + lineplot/signal.{vert,frag}.glsl
 *                 (thick segment quads carrying +-dir "normals", alpha via fwidth smoothstep,
 *                 Turbo LUT indexed by amplitude) — the compute-shader quad expansion is done
 *                 in the vertex shader with one instance per segment instead.
 * On top of that, a phosphor persistence pass (decay-multiply + MAX-blend row accumulation)
 * exploits the high-rate feed: every received spectrum row is drawn, not just one per frame.
 *
 * Data plane: binary WebSocket per PROTOCOL.md. No JSON. Zero allocation on the hot path:
 * incoming ArrayBuffers are parsed with a DataView and their payload views are handed
 * straight to texSubImage2D.
 */

"use strict";
const N_FFT_DEFAULT = 4096;

/* ------------------------------------------------------------------ protocol */

const RFSP = {
	MAGIC: 0x50534652,
	VERSION: 1,
	SPECTRA: 1,
	STATUS: 2,
	CONFIG: 16,
	ACK: 17,
	ANTENNA: 22,
	PATHS: 23,
	HDR: 16,
	SPECTRA_HDR: 64,
};

/* ------------------------------------------------------------ mode-morph math (pure, headless-testable) */

const MorphMath = {
	/* smallest hardware rate that still covers the visible span (rates sorted wide -> narrow) */
	pickRtRateIdx(rates, visSpanHz) {
		let idx = 0;
		for (let i = 0; i < rates.length; i++) {
			if (rates[i] >= visSpanHz - 1) { idx = i; } else { break; }
		}
		return idx;
	},
	/* visible window -> sweep range: at least minMHz wide, clamped to the SELECTED PATH's
	 * band [loMHz, hiMHz] (the antenna's limits are fixed - nothing ever widens past them;
	 * the clamp slides the window rather than shrinking it, so the width survives at the
	 * band edges until the band itself is the limit). 0.1 MHz resolution. */
	viewSweepRange(cvMHz, visMHz, minMHz, loMHz, hiMHz) {
		const lo = isFinite(loMHz) ? loMHz : 10, hi = isFinite(hiMHz) ? hiMHz : 3500;
		const half = Math.min(Math.max(visMHz, minMHz), hi - lo) / 2;
		let a = cvMHz - half, b = cvMHz + half;
		if (a < lo) { b = Math.min(hi, b + (lo - a)); a = lo; }
		if (b > hi) { a = Math.max(lo, a - (b - hi)); b = hi; }
		return { a: Math.round(a * 10) / 10, b: Math.round(b * 10) / 10 };
	},
};

/* ------------------------------------------------------------------ colormaps */

/* Turbo (Google) polynomial approximation — the CyberEther default LUT for both views. */
function turbo(t) {
	const r = 0.13572138 + t * (4.61539260 + t * (-42.66032258 + t * (132.13108234 + t * (-152.94239396 + t * 59.28637943))));
	const g = 0.09140261 + t * (2.19418839 + t * (4.84296658 + t * (-14.18503333 + t * (4.27729857 + t * 2.82956604))));
	const b = 0.10667330 + t * (12.64194608 + t * (-60.58204836 + t * (110.36276771 + t * (-89.90310912 + t * 27.34824973))));
	return [r, g, b];
}

/* Inferno / Viridis via control-point interpolation (matplotlib anchor colors). */
const INFERNO_STOPS = [[0, 0, 0.016], [0.16, 0.042, 0.343], [0.34, 0.106, 0.4], [0.55, 0.184, 0.322], [0.75, 0.306, 0.161], [0.93, 0.522, 0.058], [0.99, 0.792, 0.227], [0.988, 0.998, 0.645]];
const VIRIDIS_STOPS = [[0.267, 0.005, 0.329], [0.283, 0.141, 0.458], [0.254, 0.265, 0.53], [0.207, 0.372, 0.553], [0.164, 0.471, 0.558], [0.128, 0.567, 0.551], [0.135, 0.659, 0.518], [0.267, 0.749, 0.441], [0.478, 0.821, 0.318], [0.741, 0.873, 0.15], [0.993, 0.906, 0.144]];

function stops(t, table) {
	const x = t * (table.length - 1), i = Math.min(table.length - 2, Math.floor(x)), f = x - i;
	return [0, 1, 2].map(c => table[i][c] * (1 - f) + table[i + 1][c] * f);
}

const COLORMAPS = ["Turbo", "Inferno", "Viridis", "Phosphor"];

function colormapPixel(map, t) {
	switch (map) {
	case 0: return turbo(t);
	case 1: return stops(t, INFERNO_STOPS);
	case 2: return stops(t, VIRIDIS_STOPS);
	default: return [t * t * 0.35, t, t * t * 0.45]; /* phosphor green */
	}
}

function buildLutTexture(gl) {
	const w = 256, h = COLORMAPS.length, px = new Uint8Array(w * h * 4);
	for (let m = 0; m < h; m++) {
		for (let x = 0; x < w; x++) {
			const c = colormapPixel(m, x / (w - 1));
			const o = (m * w + x) * 4;
			px[o] = Math.max(0, Math.min(255, c[0] * 255));
			px[o + 1] = Math.max(0, Math.min(255, c[1] * 255));
			px[o + 2] = Math.max(0, Math.min(255, c[2] * 255));
			px[o + 3] = 255;
		}
	}
	const tex = gl.createTexture();
	gl.bindTexture(gl.TEXTURE_2D, tex);
	gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, px);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
	return tex;
}

/* ------------------------------------------------------------------ shaders */

const VS_QUAD = `#version 300 es
layout(location=0) in vec2 aPos;
out vec2 vUV;
void main() { vUV = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }`;

/* Waterfall — port of CyberEther waterfall/signal.frag.glsl. The float SSBO ring buffer
 * becomes an R8 ring texture; samplerXY's wrap logic maps onto the row ring via uHead.
 * X pan/zoom applied in UV space (CyberEther does it in the vertex shader). */
const FS_WATERFALL = `#version 300 es
precision highp float;
uniform sampler2D uRing;   /* R8, nBins x H ring */
uniform sampler2D uLut;
uniform float uHead;       /* newest row (texel row index) */
uniform float uRows;       /* ring height in rows */
uniform float uView;       /* rows displayed (view depth; CE waterfall height) */
uniform float uZoomX, uOffX;
uniform float uLutRow, uLutRows;
uniform float uFloor, uCeil;    /* display window remap inside u8 range, 0..1 */
uniform bool uInterp;
in vec2 vUV;
out vec4 outColor;

float ringSample(float x, float yBack) {
	/* yBack: rows back in time from head; wraps like CyberEther's samplerXY */
	float row = mod(uHead - yBack + uRows * 4.0, uRows);
	return texture(uRing, vec2(x, (row + 0.5) / uRows)).r;
}

void main() {
	float x = (vUV.x - 0.5) / uZoomX + 0.5 + uOffX;
	if (x < 0.0 || x > 1.0) { discard; }   /* over-pan: unexplored spectrum is dark, not smeared */
	float yBack = (1.0 - vUV.y) * (uView - 1.0);
	float mag;
	if (uInterp) {
		mag  = ringSample(x, yBack - 4.0) * 0.0162162162;
		mag += ringSample(x, yBack - 3.0) * 0.0540540541;
		mag += ringSample(x, yBack - 2.0) * 0.1216216216;
		mag += ringSample(x, yBack - 1.0) * 0.1945945946;
		mag += ringSample(x, yBack      ) * 0.2270270270;
		mag += ringSample(x, yBack + 1.0) * 0.1945945946;
		mag += ringSample(x, yBack + 2.0) * 0.1216216216;
		mag += ringSample(x, yBack + 3.0) * 0.0540540541;
		mag += ringSample(x, yBack + 4.0) * 0.0162162162;
	} else {
		mag = ringSample(x, yBack);
	}
	mag = clamp((mag - uFloor) / max(uCeil - uFloor, 1e-4), 0.0, 1.0);
	outColor = texture(uLut, vec2(mag, (uLutRow + 0.5) / uLutRows));
}`;

/* Lineplot — thicklinestrip.comp done per-instance in the vertex shader; one instance per
 * segment, 4 strip vertices. Rows are fetched from the batch texture so the same program
 * draws the live trace (uRow = newest) and the phosphor accumulation (all rows). */
const VS_LINE = `#version 300 es
precision highp float;
uniform sampler2D uRows;    /* R8, nBins x batchRows */
uniform int uNBins;
uniform int uRow;
uniform vec2 uThickness;    /* NDC half-thickness, x/y */
uniform float uZoomX, uOffX;
uniform float uFloor, uCeil;
out vec2 vTexCoord;
out vec2 vNormal;

vec2 pointAt(int i) {
	float v = texelFetch(uRows, ivec2(i, uRow), 0).r;
	v = clamp((v - uFloor) / max(uCeil - uFloor, 1e-4), 0.0, 1.0);
	float x = (float(i) / float(uNBins - 1) - 0.5 - uOffX) * uZoomX * 2.0;
	return vec2(x, v * 2.0 - 1.0);
}

void main() {
	int seg = gl_InstanceID;
	int corner = gl_VertexID;            /* 0..3 of a triangle strip quad */
	vec2 A = clamp(pointAt(seg), -1.0, 1.0);
	vec2 B = clamp(pointAt(seg + 1), -1.0, 1.0);
	vec2 dir = (B - A);
	dir = dir / max(length(dir), 1e-6);
	vec2 off = vec2(-dir.y, dir.x) * uThickness;
	vec2 p = (corner < 2) ? A : B;
	float side = (corner == 1 || corner == 3) ? 1.0 : -1.0;
	p += off * side;
	vNormal = dir * side;                /* CyberEther: +-dir stored as the "normal" */
	vTexCoord = vec2((p.y + 1.0) * 0.5, 0.0);
	gl_Position = vec4(p, 0.0, 1.0);
}`;

/* Exact port of lineplot/signal.frag.glsl (LUT row select added for the shared LUT). */
const FS_LINE = `#version 300 es
precision highp float;
uniform sampler2D uLut;
uniform float uLutRow, uLutRows;
uniform float uAlpha;
in vec2 vTexCoord;
in vec2 vNormal;
out vec4 outColor;
void main() {
	vec3 color = texture(uLut, vec2(vTexCoord.x, (uLutRow + 0.5) / uLutRows)).rgb;
	float distance = (1.0 - length(vNormal));
	float width = fwidth(distance);
	float edgeSharpness = 0.75;
	float alpha = smoothstep(0.5 - edgeSharpness * width, 0.5 + edgeSharpness * width, distance);
	outColor = vec4(color, alpha * uAlpha);
}`;

/* Distance-field trace: the spectrum is a function y(x), so instead of jointed
 * segment quads (wedge gaps on steep peaks) each fragment measures its vertical
 * distance to the trace band [min,max] over its pixel footprint. Continuous by
 * construction; fwidth-free AA in pixel space; CE-style height-gradient color. */
const FS_TRACE = `#version 300 es
precision highp float;
uniform sampler2D uTrace;   /* R8; sampled LINEAR along x */
uniform sampler2D uLut;
uniform float uRowY;        /* y texcoord of the row inside uTrace */
uniform float uZoomX, uOffX;
uniform float uFloor, uCeil;
uniform float uLutRow, uLutRows;
uniform float uThickPx;
uniform float uAlphaMul;
uniform vec2 uViewPx;
in vec2 vUV;
out vec4 outColor;

float traceVal(float sx) {
	float v = texture(uTrace, vec2(clamp(sx, 0.0, 1.0), uRowY)).r;
	return clamp((v - uFloor) / max(uCeil - uFloor, 1e-4), 0.0, 1.0);
}

void main() {
	float sx = (vUV.x - 0.5) / uZoomX + 0.5 + uOffX;
	float dx = 0.75 / (uViewPx.x * uZoomX);      /* ~pixel footprint in source coords */
	float v0 = traceVal(sx - dx);
	float v1 = traceVal(sx);
	float v2 = traceVal(sx + dx);
	float vmin = min(v1, min(v0, v2));
	float vmax = max(v1, max(v0, v2));
	float y = vUV.y;
	float dPx = (y < vmin ? (vmin - y) : (y > vmax ? (y - vmax) : 0.0)) * uViewPx.y;
	/* CyberEther line profile: distance field across a 2*hw band with a smoothstep
	 * shoulder at half-width (soft glowing core, matches their signal.frag exactly) */
	float hw = max(uThickPx, 1.0);
	float d01 = 1.0 - clamp(dPx / hw, 0.0, 1.0);
	float e = 0.75 / hw;
	float alpha = smoothstep(0.5 - e, 0.5 + e, d01);
	if (alpha <= 0.003) { discard; }
	vec3 color = uAlphaMul < 0.99
		? vec3(0.75, 0.78, 0.85)   /* peak-hold trace: neutral, unobtrusive */
		: texture(uLut, vec2(clamp(y, 0.02, 0.98), (uLutRow + 0.5) / uLutRows)).rgb;
	outColor = vec4(color, alpha * uAlphaMul);
}`;

/* Phosphor accumulation variants: intensity-only into an R channel FBO. */
const FS_LINE_ACC = `#version 300 es
precision highp float;
in vec2 vTexCoord;
in vec2 vNormal;
out vec4 outColor;
void main() {
	float distance = (1.0 - length(vNormal));
	float width = fwidth(distance);
	float alpha = smoothstep(0.5 - 0.75 * width, 0.5 + 0.75 * width, distance);
	outColor = vec4(alpha, 0.0, 0.0, 1.0);
}`;

const FS_DECAY = `#version 300 es
precision highp float;
uniform float uDecay;
out vec4 outColor;
void main() { outColor = vec4(uDecay, uDecay, uDecay, 1.0); }`;

/* Spectrum background: dB/freq grid + phosphor glow under the live trace. */
const FS_SPECTRUM_BG = `#version 300 es
precision highp float;
uniform sampler2D uAcc;
uniform sampler2D uLut;
uniform float uLutRow, uLutRows;
uniform vec2 uGridDiv;     /* divisions x,y */
uniform vec2 uViewPx;
in vec2 vUV;
out vec4 outColor;
uniform float uPhosOn;
uniform float uHistNorm;   /* 1 / log2(1 + expected steady-state max count) */
void main() {
	vec3 bg = vec3(0.0);
	/* CE-style grid: hairline neutral gray */
	vec2 g = fract(vUV * uGridDiv);
	vec2 dpx = uGridDiv / uViewPx;
	float line = 0.0;
	line = max(line, 1.0 - smoothstep(0.0, dpx.x, min(g.x, 1.0 - g.x)));
	line = max(line, 1.0 - smoothstep(0.0, dpx.y, min(g.y, 1.0 - g.y)));
	bg += vec3(0.13) * line * 0.55;
	if (uPhosOn > 0.5) {
		/* gr-fosphor: acc holds hit counts; log-compress to density, colormap BY density */
		float acc = texture(uAcc, vUV).r;
		float d = clamp(log2(1.0 + acc) * uHistNorm, 0.0, 1.0);
		vec3 ph = texture(uLut, vec2(d * 0.92 + 0.05, (uLutRow + 0.5) / uLutRows)).rgb;
		bg = max(bg, ph * smoothstep(0.0, 0.02, d));
	}
	outColor = vec4(bg, 1.0);
}`;

/* ------------------------------------------------------------------ engine */

class SpectrumEngine {
	constructor(canvas, overlay, hud) {
		this.canvas = canvas;
		this.overlay = overlay;
		this.hud = hud;
		this.gl = canvas.getContext("webgl2", { antialias: false, alpha: false, depth: false, stencil: false, powerPreference: "high-performance", preserveDrawingBuffer: false });
		if (!this.gl) { throw new Error("WebGL2 required"); }
		/* real-GPU drivers can reset the context under texture-realloc churn (mode morphs);
		 * without this the canvas dies silently and permanently - the "it always crashes on
		 * MY machine" class that headless verification never sees. A clean reload restores
		 * every layer; mode/range/antenna come back from persistence + the PATHS announce. */
		canvas.addEventListener("webglcontextlost", (e) => {
			e.preventDefault();
			setTimeout(() => location.reload(), 300);
		});

		/* view state */
		this.nBins = 0;
		this.wfRows = 2048;
		this.batchCap = 256;
		this.zoomX = 1.0;
		this._zoomAnim = null;
		this._splitAnim = null;
		this._tunePending = null;
		this._maxPane = null;
		this._splitPrev = 0.42;
		this._detentT = 0;
		this._charge = 0;          /* wheel ticks accumulated against the detent stop */
		this._chargeT = 0;
		this._chargeDir = 0;
		this.hwRates = [100000000, 50000000, 25000000, 12500000, 6250000, 3125000];
		this.offX = 0.0;
		this.floorN = 0.0;      /* display window inside u8 range, normalized */
		this.ceilN = 1.0;
		this.colormap = 0;
		this.decay = 0.94;
		this.fosphor = false;        /* CyberEther mode by default: averaged trace, no phosphor */
		this.wfDecim = 1;            /* waterfall rows folded per displayed row (speed control) */
		this.mouse = null;           /* {x,y} in canvas px, for the cursor tooltip */
		this.averaging = 1;          /* frames EMA'd on the per-frame fold; 1 = pure live (CE) */
		this.interp = true;
		this.splitY = 0.42;     /* spectrum fraction of canvas height */
		this.thicknessPx = 1.0;      /* CE: thickness*3px soft-profile half-width */
		this.paused = false;
		this.dirty = true;

		/* meta from stream */
		this.meta = { dbMin: -120, dbMax: 0, centerHz: 0, spanHz: 0, fftSize: 0, accum: 1, srcRows: 0, mode: 0 };
		this.stats = { rows: 0, bytes: 0, batches: 0, lastT: performance.now(), rowsSec: 0, mbps: 0, fps: 0, frames: 0, status: null };

		this._initGL();
		this._bindInput();
		new ResizeObserver(() => this._resize()).observe(canvas.parentElement);
		this._resize();
		const loop = () => { this._frame(); requestAnimationFrame(loop); };
		requestAnimationFrame(loop);
	}

	_program(vs, fs) {
		const gl = this.gl;
		const mk = (t, src) => {
			const s = gl.createShader(t);
			gl.shaderSource(s, src);
			gl.compileShader(s);
			if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) { throw new Error(gl.getShaderInfoLog(s) + "\n" + src); }
			return s;
		};
		const p = gl.createProgram();
		gl.attachShader(p, mk(gl.VERTEX_SHADER, vs));
		gl.attachShader(p, mk(gl.FRAGMENT_SHADER, fs));
		gl.linkProgram(p);
		if (!gl.getProgramParameter(p, gl.LINK_STATUS)) { throw new Error(gl.getProgramInfoLog(p)); }
		const u = {};
		const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
		for (let i = 0; i < n; i++) { const info = gl.getActiveUniform(p, i); u[info.name] = gl.getUniformLocation(p, info.name); }
		return { p, u };
	}

	_initGL() {
		const gl = this.gl;
		this.extF = gl.getExtension("EXT_color_buffer_float");
		this.lut = buildLutTexture(gl);

		const quad = gl.createBuffer();
		gl.bindBuffer(gl.ARRAY_BUFFER, quad);
		gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
		this.quadVAO = gl.createVertexArray();
		gl.bindVertexArray(this.quadVAO);
		gl.bindBuffer(gl.ARRAY_BUFFER, quad);
		gl.enableVertexAttribArray(0);
		gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
		this.emptyVAO = gl.createVertexArray(); /* line programs fetch everything themselves */

		this.pWf = this._program(VS_QUAD, FS_WATERFALL);
		this.pLine = this._program(VS_LINE, FS_LINE);
		this.pTrace = this._program(VS_QUAD, FS_TRACE);
		this.pLineAcc = this._program(VS_LINE, FS_LINE_ACC);
		this.pDecay = this._program(VS_QUAD, FS_DECAY);
		this.pBg = this._program(VS_QUAD, FS_SPECTRUM_BG);
		gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
		gl.disable(gl.DEPTH_TEST);
		gl.clearColor(0.012, 0.014, 0.02, 1);
	}

	_allocStream(nBins) {
		const gl = this.gl;
		this.nBins = nBins;
		const maxT = gl.getParameter(gl.MAX_TEXTURE_SIZE);
		if (nBins > maxT) { throw new Error(`nBins ${nBins} > MAX_TEXTURE_SIZE ${maxT}`); }

		const mkR8 = (w, h) => {
			const t = gl.createTexture();
			gl.bindTexture(gl.TEXTURE_2D, t);
			gl.texStorage2D(gl.TEXTURE_2D, 1, gl.R8, w, h);
			gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
			gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
			gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
			gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT);
			return t;
		};
		if (this.ringTex) { gl.deleteTexture(this.ringTex); gl.deleteTexture(this.rowsTex); }
		this.wfBins = nBins;
		this.ringTex = mkR8(nBins, this.wfRows);
		this.rowsTex = mkR8(nBins, this.batchCap);
		this.rowsBins = nBins;
		if (this.avgTex) { gl.deleteTexture(this.avgTex); }
		this.avgTex = mkR8(nBins, 1);
		if (this.holdTex) { gl.deleteTexture(this.holdTex); }
		this.holdTex = mkR8(nBins, 1);
		this.holdF = new Float32Array(nBins);
		this.holdU8 = new Uint8Array(nBins);
		this.avgF = new Float32Array(nBins);
		this.avgU8 = new Uint8Array(nBins);
		this.peakF = new Float32Array(nBins);
		this.peakU8 = new Uint8Array(nBins);
		if (this.peakTex) { gl.deleteTexture(this.peakTex); }
		this.peakTex = mkR8(nBins, 1);
		this.head = 0;          /* newest ring row */
		this.lastRow = 0;       /* newest row index inside rowsTex */
		this.pendingRows = 0;   /* rows uploaded to rowsTex since last accumulation */
		this._allocAcc();
		this.dirty = true;
	}

	_allocWf(bins) {
		const gl = this.gl;
		const mk = (w, h) => {
			const t = gl.createTexture();
			gl.bindTexture(gl.TEXTURE_2D, t);
			gl.texStorage2D(gl.TEXTURE_2D, 1, gl.R8, w, h);
			gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
			gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
			gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
			gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT);
			return t;
		};
		if (this.ringTex) { gl.deleteTexture(this.ringTex); gl.deleteTexture(this.rowsTex); }
		this.wfBins = bins;
		this.ringTex = mk(bins, this.wfRows);
		this.rowsTex = mk(bins, this.batchCap);
		this.rowsBins = bins;   /* live plane width; this.nBins stays the TRACE plane width */
		this.head = 0;
		this.lastRow = 0;
		this.pendingRows = 0;
		this.wfFold = new Uint8Array(bins);
		this.wfStage = new Uint8Array(32 * bins);
		this.wfFoldN = 0;
		this.rowStamp = new Float64Array(this.wfRows);   /* wall-clock ms per ring row */
		this.rowSeq = 0;                                 /* monotonic staged-row counter */
		this._lineRateEma = 0;
		this._lastBatchT = 0;
		this.wfBox = null;                               /* inspectrum-style measure box */
		this.dirty = true;
	}

	_applyWfLines(buf, bins, k) {
		const gl = this.gl;
		if (bins !== this.wfBins) { this._allocWf(bins); }
		this.lastLiveT = performance.now();
		const payload = new Uint8Array(buf, RFSP.SPECTRA_HDR, k * bins);
		if (this.wfStage.length < k * bins) { this.wfStage = new Uint8Array(k * bins); }
		let staged = 0;
		for (let r = 0; r < k; r++) {
			const src = payload.subarray(r * bins, (r + 1) * bins);
			if (this.wfDecim <= 1) {
				this.wfStage.set(src, staged * bins);
				staged++;
				continue;
			}
			if (this.wfFoldN === 0) { this.wfFold.set(src); }
			else { const f = this.wfFold; for (let i = 0; i < bins; i++) { if (src[i] > f[i]) { f[i] = src[i]; } } }
			if (++this.wfFoldN >= this.wfDecim) { this.wfStage.set(this.wfFold, staged * bins); staged++; this.wfFoldN = 0; }
		}
		this.stagedTotal = (this.stagedTotal || 0) + staged;
		if (staged > 0) {
			const nowT = performance.now();
			const dtB = this._lastBatchT ? nowT - this._lastBatchT : 0;
			this._lastBatchT = nowT;
			if (dtB > 0 && dtB < 1000) {
				const rInst = staged / dtB * 1000;
				this._lineRateEma = this._lineRateEma ? this._lineRateEma * 0.9 + rInst * 0.1 : rInst;
			}
			const msRow = 1000 / Math.max(this._lineRateEma || 240, 1);
			for (let i = 0; i < staged; i++) {
				this.rowStamp[(this.head + 1 + i) % this.wfRows] = nowT - (staged - 1 - i) * msRow;
			}
			this.rowSeq += staged;
			gl.bindTexture(gl.TEXTURE_2D, this.ringTex);
			let done = 0;
			while (done < staged) {
				const to = (this.head + 1) % this.wfRows;
				const n = Math.min(staged - done, this.wfRows - to);
				gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, to, bins, n, gl.RED, gl.UNSIGNED_BYTE,
					this.wfStage.subarray(done * bins, (done + n) * bins));
				this.head = (to + n - 1) % this.wfRows;
				done += n;
			}
		}
		/* newest line -> live row for fosphor. Source the PARSED staging buffer: the
		 * wire payload has per-line headers (stride != bins), so subarraying it raw
		 * shifted the glow right by (k-1)*hdr bytes and clipped it — the "phosphor
		 * ends at half the screen" bug. */
		if (staged > 0) {
			const nrows = Math.min(staged, this.batchCap);
			gl.bindTexture(gl.TEXTURE_2D, this.rowsTex);
			gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, bins, nrows, gl.RED, gl.UNSIGNED_BYTE,
				this.wfStage.subarray((staged - nrows) * bins, staged * bins));
			this.lastRow = nrows - 1;
			this.pendingRows = nrows;
		}
		/* slow-link trace source: the fold pairs that feed the exact-level trace are the
		 * FIRST thing the pacer drops on a constrained link - the lineplot froze/stuttered
		 * while the waterfall (these live lines) stayed perfect (2026-07-12 report). The
		 * live lines own the trace (max-of-2 server fold: floor ~1 dB hot vs the mean
		 * plane) unless pairs PROVE a healthy cadence (arbitration in _applyMsg); pairs
		 * going stale hand it back after ONE <=300 ms gap per degradation event - not the
		 * per-pair 400 ms flap-freeze this replaces. */
		if (this._traceLive === false && this.lastPairT && this.lastLiveT - this.lastPairT > 300) {
			this._traceLive = true;
			this._pairsOk = 0;
		}
		if (staged > 0 && this._traceLive !== false) {
			if (!this.frameSum || this.frameSum.length !== this.nBins) {
				this.frameSum = new Float64Array(this.nBins);
				this.frameMax = new Uint8Array(this.nBins);
				this.frameRows = 0;
			}
			const cs = this.frameSum, fm = this.frameMax;
			const ex = Math.max(1, Math.round(this.nBins / bins));
			for (let r = 0; r < staged; r++) {
				const src = this.wfStage.subarray(r * bins, (r + 1) * bins);
				for (let i = 0; i < bins; i++) {
					const v = src[i];
					const j = i * ex;
					for (let k = 0; k < ex; k++) {
						cs[j + k] += v;
						if (v > fm[j + k]) { fm[j + k] = v; }
					}
				}
			}
			this.frameRows += staged;
		}
	}

	clearAcc() {
		const gl = this.gl;
		if (!this.accFb) { return; }
		gl.bindFramebuffer(gl.FRAMEBUFFER, this.accFb);
		gl.clearColor(0, 0, 0, 1);
		gl.clear(gl.COLOR_BUFFER_BIT);
		gl.bindFramebuffer(gl.FRAMEBUFFER, null);
	}

	_allocAcc() {
		const gl = this.gl;
		if (this.accFb) { gl.deleteFramebuffer(this.accFb); gl.deleteTexture(this.accTex); }
		const w = Math.max(64, Math.floor(this.canvas.width));
		const h = Math.max(64, Math.floor(this.canvas.height * this.splitY));
		this.accW = w; this.accH = h;
		this.accTex = gl.createTexture();
		gl.bindTexture(gl.TEXTURE_2D, this.accTex);
		gl.texStorage2D(gl.TEXTURE_2D, 1, this.extF ? gl.R16F : gl.R8, w, h);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
		this.accFb = gl.createFramebuffer();
		gl.bindFramebuffer(gl.FRAMEBUFFER, this.accFb);
		gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.accTex, 0);
		gl.clear(gl.COLOR_BUFFER_BIT);
		gl.bindFramebuffer(gl.FRAMEBUFFER, null);
	}

	/* ---------------- data ingest (hot path, no allocation) ---------------- */

	ingestSpectra(dv, buf) {
		/* parse + queue only; ALL GL uploads happen at frame start (rAF) — uploading
		 * mid-message into in-flight textures caused driver ghost-copy stalls (~28 fps) */
		const nBins = dv.getUint32(16, true);
		const nRows = dv.getUint16(20, true);
		if (buf.byteLength < RFSP.SPECTRA_HDR + nRows * nBins) { return; }
		const msgMode = dv.getUint16(22, true);
		this.meta.mode = msgMode;
		this.meta.dbMin = dv.getFloat32(24, true);
		this.meta.dbMax = dv.getFloat32(28, true);
		this.meta.centerHz = dv.getFloat64(32, true);
		const newSpan = dv.getFloat64(40, true);
		if (this._hwPending && this.meta.spanHz > 0) {
			if (Math.abs(newSpan - this._hwPending.to) < 1e5) {
				const landedWiden = !!this._hwPending.widen;
				const landedMorph = !!this._hwPending.morph;
				const landedQuiet = !!this._hwPending.quiet;   /* sweep-range re-arm: no mode-flip FX */
				if (this._hwPending.fit) {
					/* deliberate zoom-out: show the WHOLE new span (hiding it behind a rescaled
					 * zoom made wheel-outs feel dead until you wheeled the reveal too) */
					this.zoomX = 1;
					this.offX = 0;
				} else {
					/* span landed: rescale zoom so the visible window is unchanged */
					this.zoomX = Math.max(1, Math.min(64, this.zoomX * newSpan / this._hwPending.from));
					this.offX = this._hwPending.widen ? (this._hwPending.cv - this.meta.centerHz) / newSpan : 0;
				}
				this._clampPan();
				this._hwPending = null;
				this._detentT = performance.now();
				if (this.fosphor) { this.clearAcc(); }
				if (this.onHwLanded && !landedQuiet) { this.onHwLanded(landedWiden, landedMorph); }
				if (this.onZoomLod) { const z = this.zoomX; setTimeout(() => this.onZoomLod(z), 50); }
			} else if (this._hwPending.cv !== undefined) {
				/* transition window: LO may retune BEFORE the span changes — keep the view
				 * pinned to the target center or the content visibly shifts sideways */
				this.offX = (this._hwPending.cv - this.meta.centerHz) / newSpan;
				this._clampPan();
			}
		}
		if (this._tunePending) {
			const tp = this._tunePending;
			if (Math.abs(this.meta.centerHz - tp.c0) > 1e3) {
				/* LO moved: re-express the dragged view against the new center */
				this.offX = (tp.cv - this.meta.centerHz) / newSpan;
				this._clampPan();
				this._tunePending = null;
			} else if (performance.now() - tp.t > 3000) {
				this._tunePending = null;
				this._clampPan();
			}
		}
		this.meta.spanHz = newSpan;
		if (msgMode !== 3) { this.meta.fftSize = dv.getUint32(48, true); }
		this.meta.accum = dv.getUint32(52, true);
		this.meta.srcRows = dv.getUint32(56, true);
		if (this.sweepMode && msgMode === 0 && nRows === 1) {
			/* sweep rows arrive once per completed pass: their cadence IS the refresh rate */
			const tsw = performance.now();
			if (this._swRowT) {
				const dt = tsw - this._swRowT;
				if (dt > 5 && dt < 30000) { this._swHz = this._swHz ? this._swHz * 0.7 + (1000 / dt) * 0.3 : 1000 / dt; }
			}
			this._swRowT = tsw;
		}
		if (this.paused) { return; }
		if (!this.msgQ) { this.msgQ = []; }
		this.msgQ.push({ buf, nBins, nRows, mode: msgMode });
		if (this.msgQ.length > 8) { this.msgQ.shift(); }   /* hidden tab etc: keep newest */
		this.stats.rows += nRows;
		this.stats.bytes += buf.byteLength;
		this.stats.batches++;
		this.dirty = true;
	}

	_applyMsg(buf, nBins, nRows, msgMode) {
		if (msgMode === 3) {
			/* live mode: every produced line, bins pre-folded server-side */
			if (!this.nBins) { this._allocStream(N_FFT_DEFAULT); }
			this._applyWfLines(buf, nBins, nRows);
			return;
		}
		if (nBins !== this.nBins) { this._allocStream(nBins); }
		const gl = this.gl;
		const payload = new Uint8Array(buf, RFSP.SPECTRA_HDR, nRows * nBins);

		/* server fold packets (mode 2): row0 = mean plane, row1 = max plane. While the live
		 * (mode 3) line stream is flowing these ONLY feed the trace (frameSum) — they must not
		 * touch the wf planes at all: the live plane is narrower (2048), and reallocating on
		 * every width flip wiped the waterfall ~12x/s (the "RT continuously resets" outage,
		 * 2026-07-12). wf realloc/upload happens only when this message actually draws. */
		if (msgMode === 2 && nRows >= 2) {
			const liveWf = this.lastLiveT && (performance.now() - this.lastLiveT < 500);
			if (!liveWf && nBins !== this.wfBins) { this._allocWf(nBins); }
			/* trace-source arbitration (the other half lives in _applyWfLines): pairs own the
			 * trace only after PROVING a healthy cadence — 4 consecutive gaps under 150 ms —
			 * so a trickle of pairs surviving flow control can never wrestle the trace away
			 * from the live lines. (app21's flat 400 ms staleness gate flapped: every stray
			 * pair froze the trace for the next 400 ms — the "intermittent stutter".) */
			{
				const tp = performance.now();
				const gap = this.lastPairT ? tp - this.lastPairT : 1e9;
				this._pairsOk = gap < 150 ? (this._pairsOk || 0) + 1 : 0;
				if (this._pairsOk >= 4) { this._traceLive = false; }
				this.lastPairT = tp;
			}
			const usePairs = !liveWf || this._traceLive === false;
			for (let pair = 0; pair * 2 + 1 < nRows; pair++) {
			const mean = payload.subarray((2 * pair) * nBins, (2 * pair + 1) * nBins);
			const mx = payload.subarray((2 * pair + 1) * nBins, (2 * pair + 2) * nBins);
			if (!this.frameSum || this.frameSum.length !== nBins) {
				this.frameSum = new Float64Array(nBins);
				this.frameMax = new Uint8Array(nBins);
				this.frameRows = 0;
			}
			if (usePairs) {
				const cs = this.frameSum, fm = this.frameMax;
				for (let i = 0; i < nBins; i++) {
					cs[i] += mean[i];
					if (mx[i] > fm[i]) { fm[i] = mx[i]; }
				}
				this.frameRows += 1;
			}
			if (!liveWf) {
				/* waterfall line = the max plane (every produced FFT contributes: full POI) */
				if (!this.wfFold || this.wfStage.length < nBins) {
					this.wfFold = new Uint8Array(nBins);
					this.wfStage = new Uint8Array(nBins);
					this.wfFoldN = 0;
				}
				let staged = 0;
				if (this.wfDecim <= 1) {
					this.wfStage.set(mx, 0);
					staged = 1;
				} else {
					if (this.wfFoldN === 0) { this.wfFold.set(mx); }
					else { const f = this.wfFold; for (let i = 0; i < nBins; i++) { if (mx[i] > f[i]) { f[i] = mx[i]; } } }
					if (++this.wfFoldN >= this.wfDecim) { this.wfStage.set(this.wfFold, 0); staged = 1; this.wfFoldN = 0; }
				}
				this.stagedTotal = (this.stagedTotal || 0) + staged;
				if (staged > 0) {
					gl.bindTexture(gl.TEXTURE_2D, this.ringTex);
					const to = (this.head + 1) % this.wfRows;
					gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, to, nBins, 1, gl.RED, gl.UNSIGNED_BYTE, this.wfStage.subarray(0, nBins));
					this.head = to;
				}
				/* live row for fosphor mode = the mean plane */
				gl.bindTexture(gl.TEXTURE_2D, this.rowsTex);
				gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, nBins, 1, gl.RED, gl.UNSIGNED_BYTE, mean);
				this.lastRow = 0;
				this.pendingRows = 1;
			}
			}
			return;
		}
		/* raw/avg/sweep rows always draw into the wf planes: size them to this message
		 * (RT live lines can have left them narrower — uploads would GL_INVALID_VALUE
		 * and the waterfall would silently freeze on stale rows) */
		if (nBins !== this.wfBins) { this._allocWf(nBins); }

		/* waterfall: fold wfDecim rows per line into a staging buffer, ONE batched upload
		 * per message (per-row texSubImage2D at high rates caused the stutter regression) */
		if (!this.wfFold || this.wfStage.length < nRows * nBins) {
			this.wfFold = new Uint8Array(nBins);
			this.wfStage = new Uint8Array(Math.max(1, nRows) * nBins);
			this.wfFoldN = 0;
		}
		let staged = 0;
		for (let r = 0; r < nRows; r++) {
			const src = payload.subarray(r * nBins, (r + 1) * nBins);
			if (this.wfDecim <= 1) {
				this.wfStage.set(src, staged * nBins);
				staged++;
				continue;
			}
			if (this.wfFoldN === 0) { this.wfFold.set(src); }
			else {
				const f = this.wfFold;
				for (let i = 0; i < nBins; i++) { if (src[i] > f[i]) { f[i] = src[i]; } }
			}
			if (++this.wfFoldN >= this.wfDecim) {
				this.wfStage.set(this.wfFold, staged * nBins);
				staged++;
				this.wfFoldN = 0;
			}
		}
		this.stagedTotal = (this.stagedTotal || 0) + staged;
		if (staged > 0) {
			gl.bindTexture(gl.TEXTURE_2D, this.ringTex);
			let done = 0;
			while (done < staged) {
				const to = (this.head + 1) % this.wfRows;
				const n = Math.min(staged - done, this.wfRows - to);
				gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, to, nBins, n, gl.RED, gl.UNSIGNED_BYTE,
					this.wfStage.subarray(done * nBins, (done + n) * nBins));
				this.head = (to + n - 1) % this.wfRows;
				done += n;
			}
		}

		/* batch texture for the live/phosphor passes */
		const rows = Math.min(nRows, this.batchCap);
		gl.bindTexture(gl.TEXTURE_2D, this.rowsTex);
		gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, nBins, rows, gl.RED, gl.UNSIGNED_BYTE, payload.subarray((nRows - rows) * nBins));
		this.lastRow = rows - 1;
		this.pendingRows = rows;

		/* real-time-analyzer fold: accumulate EVERY row of this frame's messages into
		 * per-frame sum + max (one pass); the frame fold happens in _frame */
		{
			if (!this.frameSum || this.frameSum.length !== nBins) {
				this.frameSum = new Float64Array(nBins);
				this.frameMax = new Uint8Array(nBins);
				this.frameRows = 0;
			}
			const cs = this.frameSum, fm = this.frameMax;
			for (let r = 0; r < nRows; r++) {
				const off = r * nBins;
				for (let i = 0; i < nBins; i++) {
					const v = payload[off + i];
					cs[i] += v;
					if (v > fm[i]) { fm[i] = v; }
				}
			}
			this.frameRows += nRows;
		}

	}

	/* ---------------- rendering ---------------- */

	_frame() {
		const now = performance.now();
		/* failsafe: a hw-step/morph whose span never lands (lost apply, wedged capture) must not
		 * leave zoom input dead forever — wide sweeps can take seconds per pass, so be generous */
		if (this._hwPending && this._hwPendingT && now - this._hwPendingT > 10000) {
			this._hwPending = null;
			this._detentT = now;
		}
		if (this._zoomAnim) {
			const a = this._zoomAnim, t = Math.min(1, (now - a.t0) / a.dur);
			const k = 1 - Math.pow(1 - t, 3);
			this.zoomX = a.from + (a.to - a.from) * k;
			if (a.toOff !== undefined) { this.offX = a.fromOff + (a.toOff - a.fromOff) * k; }
			this.dirty = true;
			if (t >= 1) {
				this.zoomX = a.to;
				if (a.toOff !== undefined) { this.offX = a.toOff; }
				this._zoomAnim = null;
				this._detentT = now;
				if (a.then) { a.then(); }
				if (this.onZoomLod) { this.onZoomLod(this.zoomX); }
			}
		}
		if (this._splitAnim) {
			const a = this._splitAnim, t = Math.min(1, (now - a.t0) / a.dur);
			this.splitY = a.from + (a.to - a.from) * (1 - Math.pow(1 - t, 3));
			this.dirty = true;
			if (t >= 1) {
				this.splitY = a.to;
				this._splitAnim = null;
				this._allocAcc();
				if (this.onSplit) { this.onSplit(); }
			}
		}
		if (now - this.stats.lastT >= 1000) {
			const dt = (now - this.stats.lastT) / 1000;
			this.stats.rowsSec = Math.round(this.stats.rows / dt);
			this.stats.mbps = (this.stats.bytes * 8 / dt / 1e6).toFixed(1);
			this.stats.fps = Math.round(this.stats.frames / dt);
			this.stats.rows = this.stats.bytes = this.stats.frames = 0;
			this.stats.lastT = now;
			this._drawHud();
		}
		if (this.msgQ && this.msgQ.length) {
			for (const m of this.msgQ) { this._applyMsg(m.buf, m.nBins, m.nRows, m.mode); }
			this.msgQ.length = 0;
			this.lastDataT = now;
		}
		/* render every vsync while the stream is live (message-gated rendering beats
		 * against vsync and lands on ~every other frame = the stutter); gate only
		 * when idle >0.5s and nothing changed */
		const live = this.lastDataT && now - this.lastDataT < 500;
		if ((!this.dirty && !live) || !this.nBins) { return; }
		this.dirty = false;
		/* glass-smooth waterfall: scroll at a rate-tracked fractional pace instead of
		 * jumping by whole bursts as messages land (burst jitter = micro-stutter) */
		{
			const dtF = Math.min(0.1, Math.max(0.001, (now - (this._lastFrameT || now)) / 1000));
			this._lastFrameT = now;
			const tot = this.stagedTotal || 0;
			if (this._dispRows === undefined) { this._dispRows = tot; this._rateRows = 0; this._lastTot = tot; this._lastRateT = now; }
			const dtR = (now - this._lastRateT) / 1000;
			if (dtR > 0.25) {
				const inst = (tot - this._lastTot) / dtR;
				this._rateRows += 0.3 * (inst - this._rateRows);
				this._lastTot = tot;
				this._lastRateT = now;
			}
			this._dispRows += this._rateRows * dtF;
			if (this._dispRows > tot) { this._dispRows = tot; }
			const maxLag = Math.max(32, this._rateRows * 0.1);
			if (tot - this._dispRows > maxLag) { this._dispRows = tot - maxLag; }
			this.smoothHead = (this.head - (tot - this._dispRows) + this.wfRows * 4) % this.wfRows;
			this._scrollLog = this._scrollLog || [];
			this._scrollLog.push(this._rateRows * dtF);
			if (this._scrollLog.length > 240) { this._scrollLog.shift(); }
		}
		this.stats.frames++;
		const gl = this.gl;
		const W = this.canvas.width, H = this.canvas.height;
		const specH = Math.floor(H * this.splitY);

		/* 1. phosphor accumulation: decay-multiply then MAX-blend every pending row */
		gl.bindFramebuffer(gl.FRAMEBUFFER, this.accFb);
		gl.viewport(0, 0, this.accW, this.accH);
		gl.enable(gl.BLEND);
		if (this.fosphor && this.pendingRows > 0) {
			gl.blendFunc(gl.ZERO, gl.SRC_COLOR);   /* dst *= decay */
			gl.blendEquation(gl.FUNC_ADD);
			gl.useProgram(this.pDecay.p);
			gl.uniform1f(this.pDecay.u.uDecay, this.decay);
			gl.bindVertexArray(this.quadVAO);
			gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

			gl.blendFunc(gl.ONE, gl.ONE);   /* additive: acc = per-(bin,dB-cell) hit count */
			gl.useProgram(this.pLineAcc.p);
			this._lineUniforms(this.pLineAcc, this.accW, this.accH, 1.1);
			gl.bindVertexArray(this.emptyVAO);
			const rb = (this.rowsBins || this.nBins) - 1;
			for (let r = this.lastRow - this.pendingRows + 1; r <= this.lastRow; r++) {
				gl.uniform1i(this.pLineAcc.u.uRow, r);
				gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, rb);
			}
			this.pendingRows = 0;
		}

		/* 2. spectrum view: bg(grid+phosphor) + live trace */
		gl.bindFramebuffer(gl.FRAMEBUFFER, null);
		const P = this._plotRect();
		gl.viewport(P.l, H - specH + P.b, P.w, P.ph);
		gl.disable(gl.BLEND);
		gl.useProgram(this.pBg.p);
		gl.activeTexture(gl.TEXTURE0);
		gl.bindTexture(gl.TEXTURE_2D, this.accTex);
		gl.uniform1i(this.pBg.u.uAcc, 0);
		gl.activeTexture(gl.TEXTURE1);
		gl.bindTexture(gl.TEXTURE_2D, this.lut);
		gl.uniform1i(this.pBg.u.uLut, 1);
		gl.uniform1f(this.pBg.u.uLutRow, this.colormap);
		gl.uniform1f(this.pBg.u.uLutRows, COLORMAPS.length);
		gl.uniform2f(this.pBg.u.uGridDiv, 10.0, 8.0);
		gl.uniform1f(this.pBg.u.uPhosOn, this.fosphor ? 1.0 : 0.0);
		{
			/* steady-state max hits/cell ~ (lines per frame) / (1 - decay); the log norm
			 * keeps the top of the colormap reachable at any line rate or persistence */
			const lpf = Math.max(1, (this.stats.rowsSec || 240) / 60);
			const peak = Math.max(4, lpf / Math.max(1 - this.decay, 0.02));
			gl.uniform1f(this.pBg.u.uHistNorm, 1.0 / Math.log2(1.0 + peak));
		}
		gl.uniform2f(this.pBg.u.uViewPx, P.w, P.ph);
		gl.bindVertexArray(this.quadVAO);
		gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

		gl.enable(gl.BLEND);
		gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
		{
			/* per-frame fold (real-time analyzer): this frame's rows -> fresh mean, EMA'd
			 * over FRAMES by the avg knob (avg 1 = pure live short-window, CE-like);
			 * frame max feeds a decaying peak trace so no burst can hide between frames */
			const dtF = Math.min(0.1, (now - (this._lastTraceT || now)) / 1000 + 1e-4);
			this._lastTraceT = now;
			const af = this.avgF, au = this.avgU8;
			const pf = this.peakF, pu = this.peakU8;
			if (this.frameRows > 0) {
				const inv = 1.0 / this.frameRows;
				const a = 1.0 / Math.max(1, this.averaging);
				const cs = this.frameSum, fm = this.frameMax;
				const pkDecay = 90 * dtF;   /* peak trace falls fast: bursts flash, no ceiling crust */
				for (let i = 0; i < this.nBins; i++) {
					af[i] += a * (cs[i] * inv - af[i]);
					const pv = pf[i] - pkDecay;
					pf[i] = fm[i] > pv ? fm[i] : pv;
				}
				if (this.holdOn && this.holdF) {
					const hf = this.holdF;
					for (let i = 0; i < this.nBins; i++) { if (fm[i] > hf[i]) { hf[i] = fm[i]; } }
					this._holdDirty = true;
				}
				cs.fill(0);
				fm.fill(0);
				this.frameRows = 0;
			} else {
				const pkDecay = 90 * dtF;
				for (let i = 0; i < this.nBins; i++) { pf[i] -= pkDecay; if (pf[i] < 0) { pf[i] = 0; } }
			}
			/* the trace IS the fold state: it steps to new data the instant it lands and
			 * holds between arrivals. No display-side inertia ever - a glide here shows
			 * values that were never measured and lags the data by design ("garbage slow
			 * animation", user verdict 2026-07-12; the avg knob's EMA above is the only
			 * legitimate smoothing, and it is per data frame, not per render frame). */
			for (let i = 0; i < this.nBins; i++) {
				au[i] = af[i];
				pu[i] = pf[i];
			}
			gl.bindTexture(gl.TEXTURE_2D, this.avgTex);
			gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, this.nBins, 1, gl.RED, gl.UNSIGNED_BYTE, au);
			gl.bindTexture(gl.TEXTURE_2D, this.peakTex);
			gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, this.nBins, 1, gl.RED, gl.UNSIGNED_BYTE, pu);
		}
		gl.useProgram(this.pTrace.p);
		if (this.holdOn && this.holdTex) {
			if (this._holdDirty) {
				const hf = this.holdF, hu = this.holdU8;
				for (let i = 0; i < this.nBins; i++) { hu[i] = hf[i]; }
				gl.activeTexture(gl.TEXTURE0);
				gl.bindTexture(gl.TEXTURE_2D, this.holdTex);
				gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, this.nBins, 1, gl.RED, gl.UNSIGNED_BYTE, hu);
				this._holdDirty = false;
			}
			gl.activeTexture(gl.TEXTURE0);
			gl.bindTexture(gl.TEXTURE_2D, this.holdTex);
			gl.uniform1i(this.pTrace.u.uTrace, 0);
			gl.uniform1f(this.pTrace.u.uRowY, 0.5);
			gl.uniform1f(this.pTrace.u.uAlphaMul, 0.55);
			gl.uniform1f(this.pTrace.u.uThickPx, 1.0 * (window.devicePixelRatio || 1));
			gl.bindVertexArray(this.quadVAO);
			gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
		}
		gl.uniform1f(this.pTrace.u.uAlphaMul, 1.0);
		gl.activeTexture(gl.TEXTURE0);
		if (this.fosphor) {
			gl.bindTexture(gl.TEXTURE_2D, this.rowsTex);      /* live latest row */
			gl.uniform1f(this.pTrace.u.uRowY, (this.lastRow + 0.5) / this.batchCap);
		} else {
			gl.bindTexture(gl.TEXTURE_2D, this.avgTex);       /* CE-style averaged trace */
			gl.uniform1f(this.pTrace.u.uRowY, 0.5);
		}
		gl.uniform1i(this.pTrace.u.uTrace, 0);
		gl.activeTexture(gl.TEXTURE1);
		gl.bindTexture(gl.TEXTURE_2D, this.lut);
		gl.uniform1i(this.pTrace.u.uLut, 1);
		gl.uniform1f(this.pTrace.u.uZoomX, this.zoomX);
		gl.uniform1f(this.pTrace.u.uOffX, this.offX);
		gl.uniform1f(this.pTrace.u.uFloor, this.floorN);
		gl.uniform1f(this.pTrace.u.uCeil, this.ceilN);
		gl.uniform1f(this.pTrace.u.uLutRow, this.colormap);
		gl.uniform1f(this.pTrace.u.uLutRows, COLORMAPS.length);
		gl.uniform1f(this.pTrace.u.uThickPx, this.thicknessPx * 3.0 * (window.devicePixelRatio || 1));
		gl.uniform2f(this.pTrace.u.uViewPx, P.w, P.ph);
		gl.bindVertexArray(this.quadVAO);
		gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

		/* 3. waterfall */
		gl.disable(gl.BLEND);
		gl.viewport(P.l, 0, P.w, H - specH);
		gl.useProgram(this.pWf.p);
		gl.activeTexture(gl.TEXTURE0);
		gl.bindTexture(gl.TEXTURE_2D, this.ringTex);
		gl.uniform1i(this.pWf.u.uRing, 0);
		gl.activeTexture(gl.TEXTURE1);
		gl.bindTexture(gl.TEXTURE_2D, this.lut);
		gl.uniform1i(this.pWf.u.uLut, 1);
		gl.uniform1f(this.pWf.u.uHead, this.smoothHead !== undefined ? this.smoothHead : this.head);
		gl.uniform1f(this.pWf.u.uRows, this.wfRows);
		gl.uniform1f(this.pWf.u.uView, Math.min(this.wfView || 512, this.wfRows));
		gl.uniform1f(this.pWf.u.uZoomX, this.zoomX);
		gl.uniform1f(this.pWf.u.uOffX, this.offX);
		gl.uniform1f(this.pWf.u.uLutRow, this.colormap);
		gl.uniform1f(this.pWf.u.uLutRows, COLORMAPS.length);
		gl.uniform1f(this.pWf.u.uFloor, this.floorN);
		gl.uniform1f(this.pWf.u.uCeil, this.ceilN);
		gl.uniform1i(this.pWf.u.uInterp, this.interp ? 1 : 0);
		gl.bindVertexArray(this.quadVAO);
		gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

		this._drawAxes(specH);
		this._drawWfBox();
	}

	_lineUniforms(prog, vw, vh, px) {
		const gl = this.gl;
		gl.uniform1i(prog.u.uNBins, this.rowsBins || this.nBins);
		gl.uniform2f(prog.u.uThickness, px / vw, px / vh);
		gl.uniform1f(prog.u.uZoomX, this.zoomX);
		gl.uniform1f(prog.u.uOffX, this.offX);
		gl.uniform1f(prog.u.uFloor, this.floorN);
		gl.uniform1f(prog.u.uCeil, this.ceilN);
		gl.activeTexture(gl.TEXTURE0);
		gl.bindTexture(gl.TEXTURE_2D, this.rowsTex);
		gl.uniform1i(prog.u.uRows, 0);
	}

	/* ---------------- overlay: axes text ---------------- */

	_drawAxes(specHpx) {
		const ctx = this.overlay.getContext("2d");
		const W = this.overlay.width, H = this.overlay.height;
		const dpr = window.devicePixelRatio || 1;
		const P = this._plotRect();
		ctx.clearRect(0, 0, W, H);
		ctx.font = `${11 * dpr}px ui-monospace, monospace`;
		const m = this.meta;
		if (!m.spanHz) { return; }
		/* plot frame */
		ctx.strokeStyle = "rgba(120,130,150,0.45)";
		ctx.lineWidth = 1;
		ctx.strokeRect(P.l + 0.5, P.t + 0.5, P.w - 1, P.ph - 1);
		/* frequency tick labels in the bottom margin (CE: under the plot) */
		ctx.fillStyle = "rgba(200,210,225,0.60)";
		const visSpan = m.spanHz / this.zoomX;
		const visCenter = m.centerHz + m.spanHz * this.offX;
		ctx.textAlign = "center";
		for (let i = 1; i < 10; i++) {
			const f = visCenter - visSpan / 2 + visSpan * i / 10;
			const x = P.l + (i / 10) * P.w;
			const label = (Math.abs(f) >= 1e9) ? (f / 1e9).toFixed(3) : (f / 1e6).toFixed(2);
			ctx.fillText(label, x, P.t + P.ph + 14 * dpr);
		}
		/* axis titles */
		ctx.fillStyle = "rgba(200,210,225,0.40)";
		ctx.fillText(Math.abs(visCenter) >= 1e9 ? "Frequency (GHz)" : "Frequency (MHz)", P.l + P.w / 2, P.t + P.ph + 28 * dpr);
		ctx.save();
		ctx.translate(13 * dpr, P.t + P.ph / 2);
		ctx.rotate(-Math.PI / 2);
		ctx.textAlign = "center";
		ctx.fillText("Amplitude (dB)", 0, 0);
		ctx.restore();
		/* dB tick labels in the left margin */
		const dbLo = m.dbMin + (m.dbMax - m.dbMin) * this.floorN;
		const dbHi = m.dbMin + (m.dbMax - m.dbMin) * this.ceilN;
		ctx.fillStyle = "rgba(200,210,225,0.60)";
		ctx.textAlign = "right";
		for (let i = 0; i <= 8; i++) {
			const db = dbHi - (dbHi - dbLo) * i / 8;
			const y = P.t + (i / 8) * P.ph;
			ctx.fillText(db.toFixed(0), P.l - 6 * dpr, y + 4 * dpr);
		}
		/* separator between spectrum and waterfall */
		ctx.fillStyle = "rgba(120,130,150,0.5)";
		ctx.fillRect(0, specHpx - 1, W, 1);
		/* CE cursor readout: dot rides the trace, freq+dB text beside it */
		if (this.mouse && this.avgU8) {
			const mx = this.mouse.x, my = this.mouse.y;
			const fx = (mx - P.l) / P.w;
			if (fx >= 0 && fx <= 1 && my >= P.t && my <= P.t + P.ph) {
				const f = visCenter - visSpan / 2 + visSpan * fx;
				const srcX = Math.max(0, Math.min(1, (fx - 0.5) / this.zoomX + 0.5 + this.offX));
				const bin = Math.max(0, Math.min(this.nBins - 1, Math.round(srcX * (this.nBins - 1))));
				const u2db = (m.dbMax - m.dbMin) / 255;
				const v = this.avgF[bin];
				const vn = Math.max(0, Math.min(1, (v / 255 - this.floorN) / (this.ceilN - this.floorN)));
				const ty = P.t + (1 - vn) * P.ph;
				const label = (f / 1e6).toFixed(3) + " MHz, " + (m.dbMin + v * u2db).toFixed(2) + " dB";
				ctx.beginPath();
				ctx.arc(mx, ty, 3.5 * dpr, 0, Math.PI * 2);
				ctx.fillStyle = "rgba(235,240,250,0.95)";
				ctx.fill();
				ctx.font = `${12 * dpr}px ui-monospace, monospace`;
				ctx.textAlign = "left";
				ctx.fillStyle = "rgba(225,232,245,0.92)";
				const tw = ctx.measureText(label).width;
				const bx = Math.min(mx + 14 * dpr, P.l + P.w - tw - 6 * dpr);
				ctx.fillText(label, bx, Math.max(P.t + 14 * dpr, ty - 10 * dpr));
			}
		}
	}

	toggleMax(pane) {
		/* maximize one pane inside the browser window: splitY -> 1 (lineplot) or 0
		 * (waterfall); toggling again restores the previous split */
		if (this._maxPane === pane) {
			this._splitAnim = { from: this.splitY, to: this._splitPrev, t0: performance.now(), dur: 200 };
			this._maxPane = null;
		} else {
			if (!this._maxPane) { this._splitPrev = this.splitY; }
			this._splitAnim = { from: this.splitY, to: pane === "tr" ? 1 : 0, t0: performance.now(), dur: 200 };
			this._maxPane = pane;
		}
		this.dirty = true;
	}

	/* waterfall coordinate mapping: seq = monotonic row id, newest = rowSeq-1 */
	_wfGeom() {
		const P = this._plotRect();
		const dpr = window.devicePixelRatio || 1;
		const wfTop = P.specH / dpr;                       /* CSS px, canvas-relative */
		const wfH = (P.H - P.specH) / dpr;
		const view = Math.min(this.wfView || 512, this.wfRows);
		const lag = this.smoothHead !== undefined ? (this.head - this.smoothHead + this.wfRows) % this.wfRows : 0;
		const newestSeq = this.rowSeq - 1 - lag;
		return { P, dpr, wfTop, wfH, view, newestSeq, l: P.l / dpr, w: P.w / dpr };
	}

	wfSeqToY(seq, g) {
		return g.wfTop + (g.newestSeq - seq) / Math.max(g.view - 1, 1) * g.wfH;
	}

	wfYToSeq(yCss, g) {
		return Math.round(g.newestSeq - (yCss - g.wfTop) / g.wfH * Math.max(g.view - 1, 1));
	}

	wfFreqToX(f, g) {
		const m = this.meta;
		const srcX = (f - m.centerHz) / m.spanHz + 0.5;
		return g.l + ((srcX - 0.5 - this.offX) * this.zoomX + 0.5) * g.w;
	}

	wfXToFreq(xCss, g) {
		const m = this.meta;
		const srcX = ((xCss - g.l) / g.w - 0.5) / this.zoomX + 0.5 + this.offX;
		return m.centerHz + m.spanHz * (srcX - 0.5);
	}

	wfBoxHit(xCss, yCss) {
		/* returns null | {mode:'move'|'n'|'s'|'e'|'w'|'ne'|'nw'|'se'|'sw'} */
		const b = this.wfBox;
		if (!b) { return null; }
		const g = this._wfGeom();
		const x0 = this.wfFreqToX(Math.min(b.fLo, b.fHi), g), x1 = this.wfFreqToX(Math.max(b.fLo, b.fHi), g);
		const y0 = this.wfSeqToY(Math.max(b.sLo, b.sHi), g), y1 = this.wfSeqToY(Math.min(b.sLo, b.sHi), g);
		const T = 7;
		if (xCss < x0 - T || xCss > x1 + T || yCss < y0 - T || yCss > y1 + T) { return null; }
		const w = Math.abs(xCss - x0) < T ? "w" : (Math.abs(xCss - x1) < T ? "e" : "");
		const n = Math.abs(yCss - y0) < T ? "n" : (Math.abs(yCss - y1) < T ? "s" : "");
		if (n || w) { return { mode: (n + w) || "move" }; }
		return { mode: "move" };
	}

	_rateIdx() {
		const sp = this.meta.spanHz;
		if (!sp) { return -1; }
		return this.hwRates.findIndex(r => Math.abs(r - sp) < 1e5);
	}

	_magnet(target, then) {
		this._zoomAnim = { from: this.zoomX, to: target, t0: performance.now(), dur: 140, then };
	}

	_drawWfBox() {
		const b = this.wfBox;
		if (!b || !this.rowSeq) { return; }
		const ctx = this.overlay.getContext("2d");
		const dpr = window.devicePixelRatio || 1;
		const g = this._wfGeom();
		const x0 = this.wfFreqToX(Math.min(b.fLo, b.fHi), g) * dpr, x1 = this.wfFreqToX(Math.max(b.fLo, b.fHi), g) * dpr;
		const sTop = Math.max(b.sLo, b.sHi), sBot = Math.min(b.sLo, b.sHi);
		const y0 = this.wfSeqToY(sTop, g) * dpr, y1 = this.wfSeqToY(sBot, g) * dpr;
		const stale = sBot < this.rowSeq - this.wfRows;   /* rows scrolled out of the ring */
		ctx.save();
		ctx.beginPath();
		ctx.rect(g.l * dpr, g.wfTop * dpr, g.w * dpr, g.wfH * dpr);
		ctx.clip();
		ctx.fillStyle = "rgba(87,163,228,0.10)";
		ctx.fillRect(x0, y0, x1 - x0, y1 - y0);
		ctx.strokeStyle = stale ? "rgba(150,150,150,0.5)" : "rgba(87,163,228,0.9)";
		ctx.lineWidth = dpr;
		ctx.strokeRect(x0, y0, x1 - x0, y1 - y0);
		if (b.n > 1) {
			ctx.strokeStyle = "rgba(87,163,228,0.45)";
			ctx.setLineDash([4 * dpr, 4 * dpr]);
			for (let i = 1; i < b.n; i++) {
				const y = y0 + (y1 - y0) * i / b.n;
				ctx.beginPath(); ctx.moveTo(x0, y); ctx.lineTo(x1, y); ctx.stroke();
			}
			ctx.setLineDash([]);
		}
		/* measurements from the row stamps (true line rate, not assumed) */
		const tTop = this.rowStamp[((sTop % this.wfRows) + this.wfRows) % this.wfRows];
		const tBot = this.rowStamp[((sBot % this.wfRows) + this.wfRows) % this.wfRows];
		const dtMs = stale ? (sTop - sBot) * (1000 / Math.max(this._lineRateEma || 240, 1)) : Math.abs(tTop - tBot);
		const dfHz = Math.abs(b.fHi - b.fLo);
		const fmtF = (hz) => hz >= 1e6 ? (hz / 1e6).toFixed(hz >= 30e6 ? 1 : 3) + " MHz" : (hz / 1e3).toFixed(1) + " kHz";
		const fmtT = (ms) => ms >= 1000 ? (ms / 1000).toFixed(2) + " s" : ms >= 1 ? ms.toFixed(2) + " ms" : (ms * 1000).toFixed(0) + " \u00b5s";
		let label = "\u0394f " + fmtF(dfHz) + " \u00b7 \u0394t " + fmtT(dtMs) + " \u00b7 fc " + ((Math.min(b.fLo, b.fHi) + dfHz / 2) / 1e6).toFixed(3) + " MHz";
		if (b.n > 1) { label += " \u00b7 " + b.n + " \u00d7 " + fmtT(dtMs / b.n); }
		ctx.font = `${11 * dpr}px ui-monospace, monospace`;
		ctx.textAlign = "left";
		ctx.textBaseline = "alphabetic";
		const tw = ctx.measureText(label).width + 12 * dpr;
		const tx = Math.max(g.l * dpr + 2, Math.min(x0, g.P.W - tw - 4));
		const ty = Math.min((g.wfTop + g.wfH) * dpr - 6 * dpr, y1 + 15 * dpr);
		ctx.fillStyle = "rgba(8,12,20,0.88)";
		ctx.fillRect(tx, ty - 12 * dpr, tw, 16 * dpr);
		ctx.fillStyle = stale ? "#999" : "#8fbfe6";
		ctx.fillText(label, tx + 6 * dpr, ty);
		ctx.restore();
		if (this.onWfBoxRect) { this.onWfBoxRect({ x: x1 / dpr, y: y0 / dpr }); }
	}

	_drawHud() {
		const s = this.stats, m = this.meta;
		const chip = this._srEl || (this._srEl = document.getElementById("srchip"));
		if (chip && m.spanHz) {
			let txt;
			if (this._hwPending) {
				/* mode transition: meta still carries the OLD span - a lingering sweep row's
				 * width rendered through the RT formula was the "SR 167 MS/s" garbage. Show
				 * the commanded TARGET until it lands. */
				const toM = this._hwPending.to / 1e6;
				txt = "⇄ " + (this.sweepMode
					? "SWEEP " + (toM >= 100 ? toM.toFixed(0) : toM.toFixed(1)) + " MHz"
					: "SR " + (toM >= 10 ? toM.toFixed(0) : toM.toFixed(3).replace(/\.?0+$/, "")) + " MS/s");
			} else if (this.sweepMode) {
				/* sweep speed = scan rate + refresh rate (GHz/s · fps), not a bare Hz */
				const hz = this._swHz || 0;
				const gps = hz * m.spanHz / 1e9;
				txt = hz > 0
					? "SWEEP " + (gps >= 10 ? gps.toFixed(0) : gps.toFixed(1)) + " GHz/s · " + hz.toFixed(1) + " fps"
					: "SWEEP …";
			} else {
				const ms = m.spanHz / 1e6;
				txt = "SR " + (ms >= 10 ? ms.toFixed(0) : ms.toFixed(3).replace(/\.?0+$/, "")) + " MS/s";
			}
			if (chip.firstChild && chip.firstChild.nodeType === 3) { chip.firstChild.textContent = txt; }
			else { chip.textContent = txt; }
		}
		const st = s.status;
		const modeLabel = this.sweepMode ? "SWEEP"
			: ((this.lastLiveT && performance.now() - this.lastLiveT < 500) ? "LIVE" : (["RAW", "AVG", "FOLD"][m.mode] || m.mode));
		/* AGC readout: live gain in RT (the daemon's leveling readback); in sweep the gain is
		 * per-hop so just name the mode */
		const agc = st && st.agcOn ? (this.sweepMode ? " \u00b7 AGC" : ` \u00b7 AGC ${st.agcGain} dB`) : "";
		/* compact readout; the full numbers live on the tooltip (the old always-on firehose
		 * of stats was most of the visual noise in the bar) */
		this.hud.textContent = `${s.fps} fps \u00b7 ${s.mbps} Mb/s \u00b7 ${modeLabel}${agc}`;
		this.hud.title = `${s.rowsSec} rows/s` + (st ? ` \u00b7 dsp ${st.fftPerSec} FFT/s \u00b7 ${st.drop} skipped/s` : "");
	}

	/* ---------------- input ---------------- */

	_bindInput() {
		const cv = this.canvas;
		cv.addEventListener("wheel", (e) => {
			e.preventDefault();
			const r = cv.getBoundingClientRect();
			const inSpectrum = (e.clientY - r.top) / r.height < this.splitY;
			if (e.shiftKey && inSpectrum) {
				/* dB window zoom */
				const k = e.deltaY > 0 ? 1.12 : 1 / 1.12;
				const mid = (this.floorN + this.ceilN) / 2, half = (this.ceilN - this.floorN) / 2 * k;
				this.floorN = Math.max(0, mid - half);
				this.ceilN = Math.min(1, mid + half);
			} else {
				/* Magnetic detents on the hardware 2x decimation ladder. Display zoom is
				 * free between notches; the notches live at zoomX=2.0 (span halves: next
				 * profile) and zoomX=1.0 (full span: previous profile). Crossing into the
				 * capture band eases the view onto the notch and commits ONE rate step;
				 * input during the swap/snap is absorbed, so fast wheeling can never
				 * chain profiles invisibly. */
				const now = performance.now();
				/* trackpad-proportional input: normalize to DETENT UNITS (one mouse notch
				 * ~= 1.0; pixel-mode streams ~= deltaY/100). A trackpad swipe is dozens of
				 * small events plus a momentum tail - a full 1.15x notch per EVENT
				 * compounded to ~16x per swipe and machine-gunned the discrete stops
				 * ("100 -> 160 -> 250 -> 3000 MHz" on one small scroll, 2026-07-12).
				 * Analog zoom scales by the FRACTIONAL exponent; the discrete stops
				 * (detent charge, rate step, sweep widen/morph) consume WHOLE accumulated
				 * notches, so any input device pays the same physical scroll per action. */
				const uRaw = e.deltaMode === 1 ? e.deltaY / 3 : e.deltaMode === 2 ? e.deltaY : e.deltaY / 100;
				const u = Math.min(3, Math.abs(uRaw));
				if (!u) { this.dirty = true; return; }
				const dirOut = uRaw > 0;
				this._lastWheelT = now;
				const k = Math.pow(1.15, dirOut ? -u : u);
				/* the zoom NEVER goes dead: display zoom stays live through a pending rate step
				 * or morph (the landing rescale carries the extra zoom through: zoomX scales by
				 * newSpan/from, so the visible window the user reached is preserved). Chained
				 * hardware steps stay impossible - onHwStep self-blocks while _hwPending, and
				 * during a morph the stale meta span is off-ladder so the detents disarm. The
				 * old absorb read as "the GUI crashed" on slow links (morphs take ~2 s there). */
				if (this._zoomAnim) { this.dirty = true; return; }
				const P = this._plotRect();
				const dpr = window.devicePixelRatio || 1;
				/* the 2x-ladder detents are an RT concept; a sweep span that happens to equal a
				 * ladder rate must not arm them (sweep ranges are dynamic now) */
				const idx = this.sweepMode ? -1 : this._rateIdx();
				const canNarrow = idx >= 0 && idx < this.hwRates.length - 1;
				const canWiden = idx > 0;
				const fx = ((e.clientX - r.left) * dpr - P.l) / P.w - 0.5;
				const before = fx / this.zoomX + this.offX;
				/* Detents are STOPS, not triggers: the view parks on the profile boundary
				 * (zoomX 2.0 in, 1.0 out) and the rate step commits only after CHARGE_N
				 * further ticks against the stop — you rest on the magnet, then push
				 * through. The charge drains if you pause, and any reverse tick clears it. */
				const CHARGE_N = 4;
				const chargeTick = (dir) => {
					/* charge in NOTCH units (u): a mouse click adds ~1, a trackpad adds its
					 * physical scroll distance - every device pays the same travel to push
					 * through, and the charge indicator fills fractionally while you scroll */
					if (this._chargeDir !== dir || now - this._chargeT > 650) { this._charge = 0; }
					this._chargeDir = dir;
					this._chargeT = now;
					this._charge += u;
					if (this.onDetentCharge) { this.onDetentCharge(Math.min(1, this._charge / CHARGE_N), dir); }
					if (this._charge >= CHARGE_N) {
						this._charge = 0;
						if (this.onDetentCharge) { this.onDetentCharge(0, dir); }
						if (this.onHwStep) { this.onHwStep(dir > 0 ? idx + 1 : idx - 1, dir < 0); }
					}
				};
				if (k > 1) {
					this._chargeDir === -1 && (this._charge = 0);
					const nz = this.zoomX * k;
					if (canNarrow && this.zoomX >= 1.995) {
						chargeTick(1);
					} else if (canNarrow && nz >= 1.72) {
						this._magnet(2.0, null);   /* pull onto the stop; committing costs more */
					} else {
						this.zoomX = Math.min(canNarrow ? 1.999 : 64, nz);
						this.offX = before - fx / this.zoomX;
					}
				} else {
					this._chargeDir === 1 && (this._charge = 0);
					const nz = this.zoomX * k;
					if (nz > 1.12 || (!canWiden && nz > 1.001)) {
						this.zoomX = Math.max(1, nz);
						this.offX = before - fx / this.zoomX;
					} else if (this.zoomX > 1.005) {
						this._magnet(1.0, null);   /* snap onto full span: the stop */
					} else if (canWiden) {
						chargeTick(-1);
					} else if (this.onZoomOutStop) {
						this.onZoomOutStop(u);     /* parked at the widest hw span: sweep-morph territory */
					}
				}
				this._clampPan();
				if (this.onZoomLod) { this.onZoomLod(this.zoomX); }
			}
			this.dirty = true;
		}, { passive: false });
		let drag = null;
		cv.addEventListener("pointerdown", (e) => {
			const r0 = cv.getBoundingClientRect();
			const xC = e.clientX - r0.left, yC = e.clientY - r0.top;
			const inWf = yC / r0.height >= this.splitY;
			if (inWf && this.paused && !e.shiftKey && this.rowSeq > 0) {
				/* frozen waterfall: measure box (inspectrum-style) */
				const g = this._wfGeom();
				const hit = this.wfBoxHit(xC, yC);
				if (hit) {
					this._boxDrag = { mode: hit.mode, x: xC, y: yC, b: { ...this.wfBox } };
				} else {
					const f = this.wfXToFreq(xC, g), sq = this.wfYToSeq(yC, g);
					this.wfBox = { fLo: f, fHi: f, sLo: sq, sHi: sq, n: 0 };
					this._boxDrag = { mode: "create", x: xC, y: yC, b: { ...this.wfBox } };
				}
				cv.setPointerCapture(e.pointerId);
				this.dirty = true;
				return;
			}
			drag = { x: e.clientX, y: e.clientY, offX: this.offX, f: this.floorN, c: this.ceilN, spec: yC / r0.height < this.splitY && e.shiftKey };
			cv.setPointerCapture(e.pointerId);
		});
		cv.addEventListener("pointermove", (e) => {
			if (this._boxDrag) {
				const r0 = cv.getBoundingClientRect();
				const xC = e.clientX - r0.left, yC = e.clientY - r0.top;
				const g = this._wfGeom();
				const d = this._boxDrag, b = this.wfBox;
				const f = this.wfXToFreq(xC, g), sq = this.wfYToSeq(yC, g);
				if (d.mode === "create") {
					b.fHi = f; b.sHi = sq;
				} else if (d.mode === "move") {
					const df = f - this.wfXToFreq(d.x, g), ds = sq - this.wfYToSeq(d.y, g);
					b.fLo = d.b.fLo + df; b.fHi = d.b.fHi + df;
					b.sLo = d.b.sLo + ds; b.sHi = d.b.sHi + ds;
				} else {
					if (d.mode.includes("w")) { b.fLo = f; }
					if (d.mode.includes("e")) { b.fHi = f; }
					if (d.mode.includes("n")) { b.sHi = sq; }
					if (d.mode.includes("s")) { b.sLo = sq; }
				}
				this.dirty = true;
				return;
			}
			if (!drag) { return; }
			const r = cv.getBoundingClientRect();
			if (drag.spec) {
				const dv = (e.clientY - drag.y) / (r.height * this.splitY) * (drag.c - drag.f);
				this.floorN = Math.max(0, drag.f + dv);
				this.ceilN = Math.min(1, drag.c + dv);
			} else {
				this.offX = drag.offX - (e.clientX - drag.x) * (window.devicePixelRatio || 1) / this._plotRect().w / this.zoomX;
				this._clampPanSoft();
			}
			this.dirty = true;
		});
		cv.addEventListener("pointerup", () => {
			if (this._boxDrag) {
				const b = this.wfBox;
				if (b && (Math.abs(this.wfFreqToX(b.fHi, this._wfGeom()) - this.wfFreqToX(b.fLo, this._wfGeom())) < 4 || b.sLo === b.sHi)) { this.wfBox = null; }
				this._boxDrag = null;
				if (this.onWfBox) { this.onWfBox(this.wfBox); }
				this.dirty = true;
				return;
			}
			if (drag && !drag.spec) { this._dragRetuneCommit(); }
			drag = null;
		});
		cv.addEventListener("pointercancel", () => { if (drag && !drag.spec) { this._dragRetuneCommit(); } drag = null; });
		cv.addEventListener("pointermove", (e) => {
			const r = cv.getBoundingClientRect();
			const dpr = window.devicePixelRatio || 1;
			this.mouse = { x: (e.clientX - r.left) * dpr, y: (e.clientY - r.top) * dpr };
			this.dirty = true;
		});
		cv.addEventListener("pointerleave", () => { this.mouse = null; this.dirty = true; });
		cv.addEventListener("dblclick", (e) => {
			const m = this.meta;
			if (!m.spanHz) { return; }
			const r = cv.getBoundingClientRect();
			const inSpectrum = (e.clientY - r.top) / r.height < this.splitY;
			const P = this._plotRect();
			const dpr = window.devicePixelRatio || 1;
			if (inSpectrum) {
				/* zoom OUT one step, anchored at the click (mirror of the wheel notches) */
				if ((this._hwPending && !this.sweepMode) || this._zoomAnim) { return; }
				if (this.zoomX > 1.001) {
					const fx = ((e.clientX - r.left) * dpr - P.l) / P.w - 0.5;
					const target = Math.max(1, this.zoomX / 2);
					const before = fx / this.zoomX + this.offX;
					let toOff = before - fx / target;
					const lim = 0.5 - 0.5 / target;
					toOff = Math.max(-lim, Math.min(lim, toOff));
					this._zoomAnim = { from: this.zoomX, to: target, fromOff: this.offX, toOff: toOff, t0: performance.now(), dur: 180, then: null };
				} else if (this.sweepMode) {
					if (this.onZoomOutStop) { this.onZoomOutStop(); }   /* at the commanded range: widen the sweep */
				} else {
					const idx = this._rateIdx();
					if (idx > 0) {
						if (this.onHwStep) { this.onHwStep(idx - 1, true); }
					}
				}
				return;
			}
			/* waterfall double-click keeps the tune gesture */
			if (!this.onTune) { return; }
			const fx = Math.max(0, Math.min(1, ((e.clientX - r.left) * dpr - P.l) / P.w));
			const visSpan = m.spanHz / this.zoomX;
			const visCenter = m.centerHz + m.spanHz * this.offX;
			const f = visCenter - visSpan / 2 + visSpan * fx;
			this.offX = 0;
			this.onTune(f);
		});
		document.addEventListener("visibilitychange", () => { this.dirty = this.dirty || !document.hidden; });
	}

	_plotRect() {
		const dpr = window.devicePixelRatio || 1;
		const W = this.canvas.width, H = this.canvas.height;
		const specH = Math.floor(H * this.splitY);
		const l = Math.round(56 * dpr), r = Math.round(12 * dpr);
		const t = Math.round(6 * dpr), b = Math.round(34 * dpr);
		return { W, H, specH, l, r, t, b, w: W - l - r, ph: Math.max(1, specH - t - b) };
	}

	_clampPan() {
		const lim = 0.5 - 0.5 / this.zoomX;
		this.offX = Math.max(-lim, Math.min(lim, this.offX));
	}

	_clampPanSoft() {
		/* drag may overshoot the captured window by up to 45% of the view — the
		 * overshoot is the retune request the release commits */
		const ext = (0.5 - 0.5 / this.zoomX) + 0.45;
		this.offX = Math.max(-ext, Math.min(ext, this.offX));
		/* ...but the visible EDGES never pass the antenna band (center-only clamping still
		 * let half a screen of no-man's-land show at the edges — round-4 report). The edge
		 * window extends to the current capture where it already sticks out past the band
		 * (an RT LO parked at the limit captures beyond it — that content is real). A view
		 * wider than the window (full-band sweep) pins dead center: no drag at all. */
		const m = this.meta, L = this.freqLimits;
		if (L && m.spanHz) {
			const vis = m.spanHz / this.zoomX;
			const edgeLo = Math.min(L.lo, m.centerHz - m.spanHz / 2);
			const edgeHi = Math.max(L.hi, m.centerHz + m.spanHz / 2);
			let loB = (edgeLo + vis / 2 - m.centerHz) / m.spanHz;
			let hiB = (edgeHi - vis / 2 - m.centerHz) / m.spanHz;
			if (loB > hiB) { loB = hiB = (loB + hiB) / 2; }
			this.offX = Math.max(loB, Math.min(hiB, this.offX));
		}
	}

	_dragRetuneCommit() {
		const m = this.meta;
		const lim = 0.5 - 0.5 / this.zoomX;
		if (!m.spanHz || Math.abs(this.offX) <= lim + 1e-6) { return; }
		if (this.sweepMode) {
			/* pan overshoot in sweep mode slides the sweep range to the view, not the LO */
			if (this.onSweepView) { this.onSweepView(); }
			return;
		}
		if (this._hwPending) { return; }
		const cv = m.centerHz + m.spanHz * this.offX;
		this._tunePending = { cv: cv, c0: m.centerHz, t: performance.now() };
		if (this.onTune) { this.onTune(cv); }
	}

	_resize() {
		const dpr = window.devicePixelRatio || 1;
		const el = this.canvas.parentElement;
		const w = Math.floor(el.clientWidth * dpr), h = Math.floor(el.clientHeight * dpr);
		if (w === this.canvas.width && h === this.canvas.height) { return; }
		this.canvas.width = this.overlay.width = w;
		this.canvas.height = this.overlay.height = h;
		if (this.nBins) { this._allocAcc(); }
		this.dirty = true;
	}
}

/* ------------------------------------------------------------------ transport */

class SpectrumClient {
	constructor(engine, url) {
		this.engine = engine;
		this.url = url;
		this.backoff = 500;
		this.ackBuf = new ArrayBuffer(24);
		this.cfgBuf = new ArrayBuffer(32);
		this.txSeq = 0;
		/* Connect the spectrum WS only while the RTSA view is actually on screen, so an
		 * unused/backgrounded tab releases the radio (spectrumd parks its RX when it has
		 * no client) instead of streaming to nobody. viewchange fires on load too. */
		window.addEventListener("viewchange", () => this._syncView());
		document.addEventListener("visibilitychange", () => this._syncView());
		window.addEventListener("focus", () => this._syncView());
		this._syncView();
	}

	_syncView() {
		const onRtsa = (location.hash.replace("#", "") || "rtsa") === "rtsa" && !document.hidden;
		if (onRtsa) {
			this._kicks = 0;   /* user-driven (re)entry: eligible to take the radio again */
			if (!this.ws || this.ws.readyState >= 2) { this._connect(); }
		} else if (this.ws && this.ws.readyState < 2) {
			this.closing = true;   /* deliberate close: no auto-reconnect */
			this.ws.close();       /* leaving RTSA / tab hidden: drop the client so the radio idles */
		}
	}

	/* Auto-reconnect with backoff, but ONLY for the focused foreground RTSA view: a daemon
	 * restart (watchdog blip, deploy) self-heals in a few seconds instead of leaving a dead
	 * red dot. An unfocused window stays down after a server-side kick (single-viewer policy:
	 * the newest/focused viewer owns the radio) and reconnects when refocused via _syncView. */
	_scheduleReconnect() {
		clearTimeout(this._rct);
		this._rct = setTimeout(() => {
			const onRtsa = (location.hash.replace("#", "") || "rtsa") === "rtsa" && !document.hidden && document.hasFocus();
			if (!onRtsa) { return; }
			if (!this.ws || this.ws.readyState >= 2) { this._connect(); }
		}, this.backoff);
		this.backoff = Math.min(this.backoff * 2, 8000);
	}

	_hdr(dv, type, payloadBytes) {
		dv.setUint32(0, RFSP.MAGIC, true);
		dv.setUint8(4, RFSP.VERSION);
		dv.setUint8(5, type);
		dv.setUint16(6, 0, true);
		dv.setUint32(8, this.txSeq++, true);
		dv.setUint32(12, payloadBytes, true);
	}

	sendScale(dbMin, dbMax) {
		if (!this.ws || this.ws.readyState !== WebSocket.OPEN) { return; }
		const buf = new ArrayBuffer(32);
		const dv = new DataView(buf);
		this._hdr(dv, 19 /* SCALE */, 16);
		dv.setFloat32(16, dbMin, true);
		dv.setFloat32(20, dbMax, true);
		this.ws.send(buf);
	}

	sendTune(freqHz, gainDb) {
		if (!this.ws || this.ws.readyState !== WebSocket.OPEN) { return; }
		const buf = new ArrayBuffer(32);
		const dv = new DataView(buf);
		this._hdr(dv, 18 /* TUNE */, 16);
		let flags = 0;
		if (freqHz != null) { dv.setFloat64(16, freqHz, true); flags |= 2; }
		if (gainDb != null) { dv.setInt32(24, gainDb | 0, true); flags |= 1; }
		dv.setUint32(28, flags, true);
		this.ws.send(buf);
	}

	sendRate(rateHz) {
		if (!this.ws || this.ws.readyState !== WebSocket.OPEN) { return; }
		const b = new ArrayBuffer(20);
		const dv = new DataView(b);
		this._hdr(dv, 20, 4);
		dv.setUint32(16, rateHz, true);
		this.ws.send(b);
	}

	sendSweep(startHz, stopHz, bwHz, enable) {
		if (!this.ws || this.ws.readyState !== WebSocket.OPEN) { return; }
		const b = new ArrayBuffer(40);
		const dv = new DataView(b);
		this._hdr(dv, 21 /* SWEEP */, 24);
		dv.setFloat64(16, startHz, true);
		dv.setFloat64(24, stopHz, true);
		dv.setUint32(32, bwHz >>> 0, true);
		dv.setUint32(36, enable ? 1 : 0, true);
		this.ws.send(b);
	}

	sendAntenna(pathCode, channel) {
		if (!this.ws || this.ws.readyState !== WebSocket.OPEN) { return; }
		const b = new ArrayBuffer(24);
		const dv = new DataView(b);
		this._hdr(dv, RFSP.ANTENNA, 8);
		dv.setUint8(16, pathCode & 0xff);   /* 0 = embed, 1..8 = SMA A..H, 9 = loopback */
		dv.setUint8(17, (channel || 0) & 0xff);
		this.ws.send(b);
	}

	sendConfig(cfg) {
		if (!this.ws || this.ws.readyState !== WebSocket.OPEN) { return; }
		const dv = new DataView(this.cfgBuf);
		this._hdr(dv, RFSP.CONFIG, 16);
		/* zoom LOD overrides: lodRate>0 caps lines/s; lodBins selects live line width */
		dv.setUint16(16, (cfg.lodRate || cfg.maxRowsPerSec) | 0, true);
		dv.setUint16(18, cfg.mode | 0, true);
		dv.setUint32(20, (cfg.lodBins || cfg.fftSizeReq) | 0, true);
		dv.setFloat32(24, cfg.emaAlpha || 0.1, true);
		dv.setUint32(28, 0, true);
		this.ws.send(this.cfgBuf);
	}

	_connect() {
		const ws = new WebSocket(this.url);
		ws.binaryType = "arraybuffer";
		this.ws = ws;
		ws.onopen = () => { this.backoff = 500; this._openT = performance.now(); document.getElementById("conn").className = "ok"; this.engine._hwPending = null; this.engine._zoomAnim = null; if (ui) { ui.pushConfig(); ui.pushScaleIfSet(); ui.pushSweepIfOn(); } };
		ws.onclose = () => {
			document.getElementById("conn").className = "bad";
			if (this.closing) { this.closing = false; return; }   /* deliberate (left the view / hidden tab) */
			/* kicked right after connecting = another viewer took the radio (single-viewer
			 * policy). Two viewers auto-reconnecting kick each other in an endless war
			 * (observed 2026-07-12 eve: two tabs re-arming/disarming the sweep 2x/s) —
			 * after two quick kicks this tab stands down; refocusing it re-arms (_syncView). */
			if (this._openT && performance.now() - this._openT < 5000) { this._kicks = (this._kicks || 0) + 1; }
			else if (this._openT) { this._kicks = 0; }
			this._openT = 0;
			if (this._kicks >= 2) { return; }
			this._scheduleReconnect();
		};
		document.getElementById("conn").onclick = () => {
			if (!this.ws || this.ws.readyState >= 2) { this._connect(); }
		};
		ws.onerror = () => ws.close();
		ws.onmessage = (ev) => {
			const buf = ev.data;
			if (!(buf instanceof ArrayBuffer) || buf.byteLength < RFSP.HDR) { return; }
			const dv = new DataView(buf);
			if (dv.getUint32(0, true) !== RFSP.MAGIC || dv.getUint8(4) !== RFSP.VERSION) { return; }
			const type = dv.getUint8(5);
			if (type === RFSP.SPECTRA) {
				this.engine.ingestSpectra(dv, buf);
				/* ACK EVERY message, echoing its seq: the server's WAN flow control derives
				   in-flight bytes / delivery rate / RTT from these (pacer.h). ~30 B each;
				   batching them breaks the measurement (in-flight granularity >> window). */
				const adv = new DataView(this.ackBuf);
				this._hdr(adv, RFSP.ACK, 8);
				adv.setUint32(16, dv.getUint32(8, true), true);
				adv.setUint32(20, 0, true);
				ws.send(this.ackBuf);
			} else if (type === RFSP.STATUS) {
				this.engine.stats.status = {
					fftPerSec: dv.getUint32(16, true),
					sent: dv.getUint32(20, true),
					drop: dv.getUint32(24, true),
					dspTimeUs: dv.getFloat32(28, true),
				};
				/* pacer tail (48-byte STATUS payload; older servers send 32 - feature-detect) */
				if (buf.byteLength >= 16 + 48) {
					this._linkUpdate(dv.getUint32(48, true) === 1, dv.getUint32(52, true), dv.getUint32(56, true));
					/* AGC word (last tail word, zero on pre-AGC servers): byte 0 = live gain
					 * (i8; the daemon stepper's gain in RT, reference gain in sweep), byte 1 bit0 = on */
					const ag = dv.getUint32(60, true);
					this.engine.stats.status.agcOn = (ag & 0x100) !== 0;
					this.engine.stats.status.agcGain = (ag << 24) >> 24;
				}
			} else if (type === RFSP.PATHS && buf.byteLength >= 16 + 68) {
				/* RX path menu: daughterboard names + (code, channel, dgb, band) entries + active one */
				const td = this._td || (this._td = new TextDecoder());
				const name = (off) => {
					const bytes = new Uint8Array(buf, off, 32);
					let n = 0;
					while (n < 32 && bytes[n]) { n++; }
					return td.decode(bytes.subarray(0, n));
				};
				const count = Math.min(dv.getUint8(80), 16);
				const entries = [];
				for (let i = 0; i < count; i++) {
					const o = 84 + 8 * i;
					entries.push({ code: dv.getUint8(o), ch: dv.getUint8(o + 1), dgb: dv.getUint8(o + 2),
						lo: dv.getUint16(o + 4, true) || 10, hi: dv.getUint16(o + 6, true) || 3500 });
				}
				if (this.onPaths) { this.onPaths({ names: [name(16), name(48)], entries, curIdx: Math.min(dv.getUint8(81), Math.max(0, count - 1)) }); }
			}
		};
	}

	/* "we are bandwidth-limited" advisory: a quiet chip in the top bar, nothing modal.
	   Hysteresis: appears after 2 s limited, stays until 5 s clean (no flicker). */
	_linkUpdate(limited, kbps, rttMs) {
		const now = performance.now();
		if (this._lnk === undefined) { this._lnk = { since: 0, cleanSince: 0, shown: false }; }
		const L = this._lnk;
		if (limited) { L.cleanSince = 0; if (!L.since) { L.since = now; } }
		else { L.since = 0; if (!L.cleanSince) { L.cleanSince = now; } }
		const el = document.getElementById("lnk");
		if (!el) { return; }
		if (!L.shown && limited && now - L.since > 2000) { L.shown = true; }
		if (L.shown && !limited && now - L.cleanSince > 5000) { L.shown = false; }
		if (L.shown) {
			el.textContent = "↯ link-limited" + (kbps > 100 ? " · " + (kbps / 1000).toFixed(kbps < 10000 ? 1 : 0) + " Mbit/s" : "");
			el.title = "The network between you and the board carries less than the full waterfall stream, so the line rate " +
				"is automatically paced to what the link delivers (~" + (kbps / 1000).toFixed(1) + " Mbit/s, RTT ~" + rttMs + " ms). " +
				"Spectrum resolution and trace accuracy are unaffected; on the local network the full rate returns by itself.";
			el.style.display = "";
		} else {
			el.style.display = "none";
		}
	}
}

/* ------------------------------------------------------------------ ui glue */

let ui = null;

window.addEventListener("DOMContentLoaded", () => {
	const engine = new SpectrumEngine(
		document.getElementById("gl"),
		document.getElementById("ovl"),
		document.getElementById("stats"));
	window.eng = engine;   /* debug/inspection hook */
	const wsUrl = (location.protocol === "https:" ? "wss://" : "ws://") + (location.host || "localhost:7681") + "/ws";
	const client = new SpectrumClient(engine, wsUrl);

	const $ = (id) => document.getElementById(id);
	const store = window.localStorage;
	const persist = (k, v) => { try { store.setItem("rfsp." + k, v); } catch (e) {} };
	const recall = (k, d) => { const v = store.getItem("rfsp." + k); return v === null ? d : v; };

	/* RX path state (fed by the daemon's PATHS announce). lim = the SELECTED path's band in
	 * MHz — the fixed limits every tune / zoom / sweep decision is clamped to. */
	const rxPaths = { entries: [], names: [], curIdx: -1, lim: { lo: 10, hi: 3500 } };
	const fmtM = (v) => String(Math.round(v * 1000) / 1000);
	const setFields = (r) => {   /* reflect a commanded sweep range into the center+span fields */
		if (document.activeElement !== $("freq")) { $("freq").value = fmtM((r.a + r.b) / 2); NumField.normalize($("freq")); }
		if (document.activeElement !== $("span")) { $("span").value = fmtM(r.b - r.a); NumField.normalize($("span")); }
	};
	const cmapSel = $("cmap");
	COLORMAPS.forEach((n, i) => cmapSel.add(new Option(n, i)));
	cmapSel.onchange = () => { engine.colormap = +cmapSel.value; engine.dirty = true; persist("cmap", cmapSel.value); };
	$("decay").oninput = (e) => { engine.decay = +e.target.value; persist("decay", e.target.value); };
	$("interp").onchange = (e) => { engine.interp = e.target.checked; engine.dirty = true; };
	$("fosphor").onchange = (e) => { engine.fosphor = e.target.checked; if (!engine.fosphor) { engine.clearAcc(); } engine.dirty = true; persist("fosphor", e.target.checked ? 1 : 0); };
	$("avgn").onchange = (e) => { engine.averaging = Math.max(1, Math.min(64, +e.target.value)); persist("avgn", engine.averaging); };
	const pushScale = () => {
		const mn = parseFloat($("dbmin").value), mx = parseFloat($("dbmax").value);
		if (mx - mn >= 5) {
			client.sendScale(mn, mx);
			engine.floorN = 0; engine.ceilN = 1;   /* server window IS the display window now */
			engine.dirty = true;
			persist("dbmin", mn); persist("dbmax", mx);
		}
	};
	const tuneFromField = () => {
		const mhz = NumField.parseFreqMHz($("freq").value);
		if (!isFinite(mhz)) { return; }
		const c = Math.max(rxPaths.lim.lo, Math.min(rxPaths.lim.hi, mhz));   /* the antenna's band is the limit */
		if (engine.sweepMode) {
			modeCtl.rearm(c * 1e6, (modeCtl.cmdB - modeCtl.cmdA) * 1e6 || engine.meta.spanHz);   /* slide the range to the new center */
			return;
		}
		engine.offX = 0;
		engine._tunePending = null;
		engine.dirty = true;
		client.sendTune(c * 1e6, null);
	};
	$("freq").addEventListener("keydown", (e) => { if (e.key === "Enter") { tuneFromField(); e.target.blur(); } });
	$("freq").addEventListener("change", tuneFromField);
	$("dbmin").addEventListener("keydown", (e) => { if (e.key === "Enter") { pushScale(); e.target.blur(); } });
	$("dbmax").addEventListener("keydown", (e) => { if (e.key === "Enter") { pushScale(); e.target.blur(); } });
	$("dbauto").onclick = () => {
		/* CE-style framing: noise floor sits ~23% above the bottom, peaks get ~20% headroom */
		const m = engine.meta;
		if (!engine.avgU8) { return; }
		const srt = Array.from(engine.avgU8).sort((a, b) => a - b);
		const u2db = (m.dbMax - m.dbMin) / 255;
		const flo = m.dbMin + srt[Math.floor(srt.length * 0.20)] * u2db;
		const pk = m.dbMin + srt[Math.floor(srt.length * 0.999)] * u2db;
		const span = Math.max(60, (pk - flo) / 0.57);   /* floor 23% up, peak at ~80% */
		const lo = flo - 0.23 * span;
		$("dbmin").value = lo.toFixed(0);
		$("dbmax").value = (lo + span).toFixed(0);
		pushScale();
	};
	/* restore persisted prefs */
	$("decay").value = recall("decay", "0.94"); engine.decay = +$("decay").value;
	$("fosphor").checked = recall("fosphor", "0") === "1"; engine.fosphor = $("fosphor").checked;
	$("avgn").value = recall("avgn", "1"); engine.averaging = +$("avgn").value;
	cmapSel.value = recall("cmap", "2"); engine.colormap = +cmapSel.value;   /* Viridis default */
	$("pause").onclick = (e) => { engine.paused = !engine.paused; e.target.textContent = engine.paused ? "resume" : "pause"; };
	$("reset").onclick = () => { engine.zoomX = 1; engine.offX = 0; engine._zoomAnim = null; engine._hwPending = null; engine.floorN = 0; engine.ceilN = 1; engine.dirty = true; };

	const freqBox = $("freq");
	const doTune = (hz) => {
		if (engine.sweepMode || !(hz > 0)) { return; }
		hz = Math.max(rxPaths.lim.lo * 1e6, Math.min(rxPaths.lim.hi * 1e6, hz));
		client.sendTune(hz, null);
		freqBox.value = (hz / 1e6).toFixed(3);
		NumField.normalize(freqBox);
	};
	engine.onTune = doTune;
	/* (Enter/change tune lives on the single tuneFromField handler above; a second keydown
	 * handler here used to double-fire TUNE per Enter press) */
	let lastCenter = 0, lastSpanHz = 0;
	setInterval(() => {
		const m = engine.meta;
		if (m.centerHz && m.centerHz !== lastCenter && document.activeElement !== freqBox) {
			lastCenter = m.centerHz;
			freqBox.value = (lastCenter / 1e6).toFixed(3);
			NumField.normalize(freqBox);
		}
		const sEl = $("span");
		if (m.spanHz && m.spanHz !== lastSpanHz && document.activeElement !== sEl) {
			lastSpanHz = m.spanHz;
			sEl.value = fmtM(m.spanHz / 1e6);
			NumField.normalize(sEl);
		}
	}, 500);

	const savedMin = recall("dbmin", null), savedMax = recall("dbmax", null);
	if (savedMin !== null && savedMax !== null) {
		$("dbmin").value = savedMin; $("dbmax").value = savedMax;
		setTimeout(pushScale, 1200);
	} else {
		/* first visit: auto-range once as soon as spectra actually FLOW (CE-like floor-low
		 * framing). Must wait for real rows: a sweep pass can take >2.5s to produce its first
		 * row at connect, and auto-ranging an all-zero trace locks in (and persists) a garbage
		 * scale window computed from the meta defaults. */
		const autoOnce = () => {
			if (recall("dbmin", null) !== null) { return; }   /* user set a scale meanwhile */
			if (engine.avgU8 && engine.stats.batches > 0 && engine.avgU8.some((v) => v > 0)) { $("dbauto").click(); return; }
			setTimeout(autoOnce, 500);
		};
		setTimeout(autoOnce, 2500);
	}

	const wfLabel = () => { $("wfspeed").textContent = (engine.wfView || 512) + "r"; };
	const wfFaster = () => { engine.wfView = Math.max(128, (engine.wfView || 512) >> 1); wfLabel(); persist("wfview", engine.wfView); };
	const wfSlower = () => { engine.wfView = Math.min(2048, (engine.wfView || 512) << 1); wfLabel(); persist("wfview", engine.wfView); };
	$("wfplus").onclick = wfFaster;
	$("wfminus").onclick = wfSlower;
	engine.wfView = +recall("wfview", "512"); wfLabel();
	document.addEventListener("keydown", (e) => {
		if ((e.key === "Escape" || e.key === "x") && engine.wfBox && document.activeElement.tagName !== "INPUT") {
			engine.wfBox = null; engine.dirty = true; if (engine.onWfBox) { engine.onWfBox(null); }
			return;
		}
		if (e.target.tagName === "INPUT" || e.target.tagName === "SELECT") { return; }
		if (e.code === "Space") {
			e.preventDefault();
			engine.paused = !engine.paused;
			$("pause").textContent = engine.paused ? "resume" : "pause";
		} else if (e.key === "+" || e.key === "=") { wfFaster(); }
		else if (e.key === "-" || e.key === "_") { wfSlower(); }
	});

	/* zoom drives the RADIO: display zoom past 2.2x commits a hardware span step
	 * (LO -> view center, sample rate/2 within the no-reclock 100M/2^k ladder) and the
	 * display rescales when the new span arrives. Hardware coarse, display fine. */
	let lodLast = "";
	const HW_RATES = engine.hwRates;
	engine.onZoomLod = (z) => {
		if (engine.sweepMode) { modeCtl.sweepView(); return; }   /* sweep: the view drives the commanded range (and the <100 MHz auto-RT morph) */
		modeCtl.rtZoom(z);
		/* transport LOD: narrower RF span raises engine duty (same FFT/s over fewer
		 * windows), so cap line rate by SPAN level — bandwidth steps DOWN per zoom */
		const m0 = engine.meta;
		const hwIdx = m0.spanHz ? Math.max(0, Math.round(Math.log2(100e6 / m0.spanHz))) : 0;
		const spanCap = hwIdx === 0 ? 0 : Math.max(1500, 4000 - hwIdx * 500);
		const lod = z >= 8 ? [512, Math.min(1500, spanCap || 1500)] : (z >= 2 ? [1024, spanCap ? Math.min(3000, spanCap) : 3000] : [2048, spanCap]);
		const key = lod.join();
		if (key !== lodLast) {
			lodLast = key;
			engine.lodBins = lod[0];
			engine.lodRate = lod[1];
			ui.pushConfig();
		}
	};
	/* magnetic detent commits: one hardware profile per notch. Narrow retunes the LO
	 * to the view center first (zoom into what you are looking at); widen must be
	 * invisible (view keeps center+width, wheel-out then reveals outward). */
	engine.onHwStep = (toIdx, widen) => {
		if (engine.sweepMode) { return; }   /* sweep owns the LO ladder; no zoom-driven span steps */
		const m = engine.meta;
		if (!m.spanHz || engine._hwPending) { return; }
		const cv = m.centerHz + m.spanHz * engine.offX;
		engine._hwPending = { from: m.spanHz, to: HW_RATES[toIdx], cv: cv, widen: widen };
		engine._hwPendingT = performance.now();
		edgeFx.arm();
		if (!widen) { client.sendTune(cv, null); }
		client.sendRate(HW_RATES[toIdx]);
	};
	engine.onHwLanded = (widen, morph) => {
		edgeFx.fire(widen);
		if (!morph) { srChip.pulse(widen); }   /* a mode morph is not a x2/÷2 ladder step — the segment flash tells that story */
	};
	engine.onZoomOutStop = (u) => modeCtl.outStop(u);
	engine.onSweepView = () => modeCtl.sweepView();

	engine.onDetentCharge = (frac, dir) => {
		[document.getElementById("edgeL"), document.getElementById("edgeR")].forEach(el => {
			if (!el) { return; }
			el.classList.remove("snapin", "snapout", "arming");
			el.style.opacity = frac > 0 ? (0.15 + 0.5 * frac).toFixed(2) : "";
		});
	};

	/* rate-change edge effect + sample-rate chip */
	const edgeFx = {
		els: [document.getElementById("edgeL"), document.getElementById("edgeR")],
		arm() { this.els.forEach(el => { if (el) { el.style.opacity = ""; el.classList.remove("snapin", "snapout"); el.classList.add("arming"); } }); },
		fire(widen) {
			this.els.forEach(el => {
				if (!el) { return; }
				el.style.opacity = "";
				el.classList.remove("arming", "snapin", "snapout");
				void el.offsetWidth;   /* restart the CSS animation */
				el.classList.add(widen ? "snapout" : "snapin");
			});
		},
	};
	/* pane maximize icons: appear when the pointer is in the pane, fade after idle */
	const maxIcons = {
		tr: document.getElementById("maxTr"),
		wf: document.getElementById("maxWf"),
		t: null,
		place() {
			if (this.wf) { this.wf.style.top = "calc(" + (engine.splitY * 100).toFixed(2) + "% + 8px)"; }
		},
		poke(x, y) {
			this.place();
			[this.tr, this.wf].forEach(el => {
				if (!el) { return; }
				const b = el.getBoundingClientRect();
				const d = Math.hypot(x - (b.left + b.width / 2), y - (b.top + b.height / 2));
				el.style.opacity = d < 40 ? 0.92 : Math.max(0, 0.92 * (1 - (d - 40) / 140)).toFixed(2);
			});
			clearTimeout(this.t);
			this.t = setTimeout(() => { [this.tr, this.wf].forEach(el => el && (el.style.opacity = "")); }, 1500);
		},
	};
	engine.onSplit = () => {
		maxIcons.place();
		if (maxIcons.tr) { maxIcons.tr.textContent = engine._maxPane === "tr" ? "\u2929" : "\u26f6"; maxIcons.tr.title = engine._maxPane === "tr" ? "restore split" : "maximize lineplot"; }
		if (maxIcons.wf) { maxIcons.wf.textContent = engine._maxPane === "wf" ? "\u2929" : "\u26f6"; maxIcons.wf.title = engine._maxPane === "wf" ? "restore split" : "maximize waterfall"; }
	};
	if (maxIcons.tr) { maxIcons.tr.onclick = () => { engine.toggleMax("tr"); engine.onSplit(); }; }
	if (maxIcons.wf) { maxIcons.wf.onclick = () => { engine.toggleMax("wf"); engine.onSplit(); }; }
	engine.canvas.addEventListener("pointermove", (e) => { maxIcons.poke(e.clientX, e.clientY); });
	maxIcons.place();

	/* measure-box divider tools: [-] n [+] [x], parked at the box top-right corner */
	const boxTools = {
		el: document.getElementById("boxTools"),
		n: document.getElementById("boxN"),
		sync() {
			const b = engine.wfBox;
			if (!this.el) { return; }
			this.el.style.display = b ? "flex" : "none";
			if (b && this.n) { this.n.textContent = b.n > 1 ? b.n : "1"; }
		},
	};
	if (boxTools.el) {
		document.getElementById("boxMinus").onclick = () => { const b = engine.wfBox; if (b) { b.n = Math.max(1, (b.n || 1) - 1); engine.dirty = true; boxTools.sync(); } };
		document.getElementById("boxPlus").onclick = () => { const b = engine.wfBox; if (b) { b.n = Math.min(64, (b.n || 1) + 1); engine.dirty = true; boxTools.sync(); } };
		document.getElementById("boxClose").onclick = () => { engine.wfBox = null; engine.dirty = true; boxTools.sync(); };
	}
	engine.onWfBox = () => boxTools.sync();

	/* max-hold + peak table */
	$("hold").onchange = (e) => {
		engine.holdOn = e.target.checked;
		if (engine.holdOn && engine.holdF) { engine.holdF.fill(0); engine._holdDirty = true; }
		engine.dirty = true;
	};
	/* peaks: compact top-5 panel at the right edge under the bar; header click folds it */
	const pkEl = document.getElementById("pkTable");
	let pkFold = recall("pkfold", "0") === "1";
	const pkRender = (rows) => {
		pkEl.innerHTML = "";
		const hd = document.createElement("div");
		hd.textContent = (pkFold ? "▸" : "▾") + " PEAKS" + (pkFold && rows.length ? " " + rows.length : "");
		hd.style.cssText = "cursor:pointer; color:#9ab0c4; letter-spacing:1px; font-size:9.5px; user-select:none;";
		hd.title = pkFold ? "expand the peak list" : "collapse to a chip";
		hd.onclick = () => { pkFold = !pkFold; persist("pkfold", pkFold ? "1" : "0"); pkRender(rows); };
		pkEl.appendChild(hd);
		if (pkFold) { return; }
		rows.forEach(rw => {
			const d = document.createElement("div");
			d.textContent = rw.txt;
			pkEl.appendChild(d);
		});
		if (!rows.length) {
			const d = document.createElement("div");
			d.textContent = "(no peaks)";
			d.style.color = "#62798f";
			pkEl.appendChild(d);
		}
	};
	$("pktbl").onchange = (e) => { if (pkEl) { pkEl.style.display = e.target.checked ? "block" : "none"; } };
	setInterval(() => {
		if (!pkEl || pkEl.style.display === "none" || !engine.avgF || !engine.meta.spanHz) { return; }
		const m = engine.meta, af = engine.avgF, n = engine.nBins;
		const u2db = (m.dbMax - m.dbMin) / 255;
		const sorted = Float32Array.from(af).sort();
		const med = sorted[n >> 1];
		const pks = [];
		/* plateau-safe: local max with >= left / > right (one point per flat top),
		 * then a minimum separation with keep-strongest */
		const minSep = Math.max(8, n >> 8);
		for (let i = 1; i < n - 1; i++) {
			const v = af[i];
			if (v > med + 15 && v >= af[i - 1] && v > af[i + 1]) {
				if (pks.length && i - pks[pks.length - 1].i < minSep) {
					if (v > pks[pks.length - 1].v) { pks[pks.length - 1] = { i, v }; }
				} else { pks.push({ i, v }); }
			}
		}
		/* merge neighbors with a shallow saddle (same physical peak, noisy top) */
		for (let k = pks.length - 2; k >= 0; k--) {
			const a = pks[k], b = pks[k + 1];
			let saddle = 255;
			for (let i = a.i; i <= b.i; i++) { if (af[i] < saddle) { saddle = af[i]; } }
			if (saddle > Math.min(a.v, b.v) - 12) {
				pks.splice(k, 2, a.v >= b.v ? a : b);
			}
		}
		pks.sort((a, b) => b.v - a.v);
		const rows = pks.slice(0, 5).map(p => {
			const f = m.centerHz + m.spanHz * (p.i / (n - 1) - 0.5);
			return { f, txt: (f / 1e6).toFixed(3).padStart(9) + " MHz " + (m.dbMin + p.v * u2db).toFixed(1).padStart(6) + " dB" };
		});
		pkRender(rows);
	}, 500);
	engine.onWfBoxRect = (r) => {
		if (!boxTools.el || !engine.wfBox) { return; }
		boxTools.el.style.left = Math.min(r.x + 6, engine.canvas.clientWidth - 120) + "px";
		boxTools.el.style.top = Math.max(r.y - 30, 4) + "px";
	};

	/* IQ record: spectrumd backend (/api/app/rtsa/record) taps the LIVE stream \u2014 records exactly
	 * what's on screen at the current LO / span / gain, with no device handoff and no pause.
	 * Red dot in the bar persists while the modal is closed. */
	const recUi = {
		modal: document.getElementById("recModal"),
		dot: document.getElementById("recDot"),
		stat: document.getElementById("rec_stat"),
		active: false,
		lastName: "",
		rate: 0,
		timer: null,
		open() {
			const m = engine.meta;
			this.rate = m.spanHz || 0;
			$("rec_view").textContent = m.spanHz
				? "current view: " + (m.centerHz / 1e6).toFixed(3) + " MHz @ " + (m.spanHz / 1e6).toFixed(3) + " MS/s"
				: "current view: (waiting for the spectrum\u2026)";
			this.size();
			this.modal.style.display = "block";
		},
		close() { this.modal.style.display = "none"; },
		size() {
			const bytes = this.rate * (+$("rec_secs").value) * ($("rec_fmt").value === "cf32" ? 8 : 4);
			$("rec_size").textContent = this.rate ? "\u2248 " + (bytes / 1e9).toFixed(2) + " GB" : "";
		},
		async start() {
			const name = encodeURIComponent($("rec_name").value || "capture1");
			const q = "/api/app/rtsa/record?secs=" + (+$("rec_secs").value) + "&name=" + name +
				($("rec_fmt").value === "cf32" ? "&fmt=cf32" : "");
			try {
				const r = await fetch(q);
				const j = await r.json();
				if (!j.ok) { this.stat.textContent = "failed: " + (j.err || r.status); return; }
				this.lastName = j.name;
				this.active = true;
				this.dot.classList.add("live");
				this.stat.textContent = "recording " + j.name + "\u2026";
				this.poll();
			} catch (e) { this.stat.textContent = "failed: " + e; }
		},
		async stop() {
			if (!this.active) { this.close(); return; }
			try { await fetch("/api/app/rtsa/record?stop=1"); } catch (e) { /* status poll reports the result */ }
		},
		poll() {
			clearTimeout(this.timer);
			this.timer = setTimeout(async () => {
				try {
					const s = await (await fetch("/api/app/rtsa/recstatus")).json();
					if (s.rec_state === 1) {
						const pct = s.rec_want ? " (" + (100 * s.rec_got / s.rec_want).toFixed(0) + "%)" : "";
						this.stat.textContent = "recording\u2026 " + (s.rec_got / 1e6).toFixed(1) + " Msamples" + pct;
						this.poll();
						return;
					}
					this.active = false;
					this.dot.classList.remove("live");
					if (s.rec_state === 2) {
						this.stat.innerHTML = "done: " + (s.rec_got / 1e6).toFixed(1) + " Msamples \u2014 " +
							"<a href=\"/captures/" + this.lastName + "\" download style=\"color:#8fbfe6\">" + this.lastName + "</a> (on the USB drive)";
					} else {
						this.stat.textContent = s.rec_summary || "stopped";
					}
				} catch (e) { this.poll(); }
			}, 600);
		},
	};
	document.getElementById("recBtn").onclick = () => recUi.open();
	$("rec_close").onclick = () => recUi.close();
	$("rec_go").onclick = () => recUi.start();
	$("rec_stop").onclick = () => recUi.stop();
	["rec_secs", "rec_fmt"].forEach(id => { $(id).onchange = () => recUi.size(); });
	document.getElementById("recModal").addEventListener("pointerdown", (e) => { if (e.target.id === "recModal") { recUi.close(); } });

	const srChip = {
		el: document.getElementById("srchip"),
		delta: document.getElementById("srdelta"),
		t: null,
		pulse(widen) {
			if (!this.el) { return; }
			if (this.delta) { this.delta.textContent = widen ? "\u00d72" : "\u00f72"; }
			this.el.classList.remove("pulse");
			void this.el.offsetWidth;
			this.el.classList.add("pulse");
			clearTimeout(this.t);
			this.t = setTimeout(() => { if (this.delta) { this.delta.textContent = ""; } }, 900);
		},
	};

	ui = {
		pushScaleIfSet() {
			const mn = parseFloat($("dbmin").value), mx = parseFloat($("dbmax").value);
			if (mx - mn >= 5 && recall("dbmin", null) !== null) { setTimeout(pushScale, 600); }
		},
		pushConfig() {
			client.sendConfig({
				maxRowsPerSec: +$("rate").value,
				mode: +$("mode").value,
				lodBins: engine.lodBins, lodRate: engine.lodRate,
				fftSizeReq: 4096,   /* engine GPU kernel is 4096-pt; the old dropdown was decorative */
				emaAlpha: 0.1,
			});
		},
		pushSweepIfOn() { if (engine.sweepMode) { applySweep((modeCtl.cmdA + modeCtl.cmdB) / 2, modeCtl.cmdB - modeCtl.cmdA); } },   /* reconnect: resume the same range */
	};
	["rate", "mode"].forEach(id => { $(id).onchange = ui.pushConfig; });
	/* RX daughterboard + antenna path: TWO dropdowns fed by the daemon's PATHS announce
	 * (device truth at connect and after every change — including REJECTED switches, which
	 * re-announce so these snap back; nothing is pushed at connect). One daughterboard =
	 * the board dropdown simply has a single option. The SELECTED path's band becomes
	 * rxPaths.lim, the fixed limits for every tune/zoom/sweep decision. Antenna changes
	 * while sweeping drop the sweep first (the daemon applies path switches in RT) and
	 * re-arm inside the new band when the switch's announce comes back. */
	{
		const PATH_NAMES = ["EMBED", "SMA A", "SMA B", "SMA C", "SMA D", "SMA E", "SMA F", "SMA G", "SMA H", "LOOPBACK"];
		const dsel = $("dgb"), asel = $("ant");
		let reenterSweep = null;
		const fillPathSel = (dgb, curIdx) => {
			asel.innerHTML = "";
			rxPaths.entries.forEach((e, i) => {
				if (e.dgb === dgb) { asel.add(new Option(PATH_NAMES[e.code] || "?", i)); }
			});
			if (curIdx !== undefined && rxPaths.entries[curIdx] && rxPaths.entries[curIdx].dgb === dgb) { asel.value = String(curIdx); }
			asel.disabled = asel.options.length <= 1;
		};
		client.onPaths = (p) => {
			rxPaths.entries = p.entries;
			rxPaths.names = p.names;
			rxPaths.curIdx = p.curIdx;
			const cur = p.entries[p.curIdx];
			if (cur) {
				rxPaths.lim = { lo: cur.lo, hi: cur.hi };
				freqBox.dataset.min = String(cur.lo);
				freqBox.dataset.max = String(cur.hi);
				engine.freqLimits = { lo: cur.lo * 1e6, hi: cur.hi * 1e6 };   /* drag stops at the band edges */
			}
			const dgbs = [...new Set(p.entries.map(e => e.dgb))];
			dsel.innerHTML = "";
			dgbs.forEach(d => dsel.add(new Option(p.names[d] || ("BOARD " + (d + 1)), d)));
			dsel.value = String(cur ? cur.dgb : dgbs[0]);
			dsel.disabled = dgbs.length <= 1;
			fillPathSel(cur ? cur.dgb : dgbs[0], p.curIdx);
			if (reenterSweep) {
				/* post-switch: resume the sweep inside the (possibly new) band */
				const want = reenterSweep.spanMHz;
				reenterSweep = null;
				if (engine.sweepMode) {
					const cv = Math.max(rxPaths.lim.lo, Math.min(rxPaths.lim.hi, engine.meta.centerHz / 1e6 || 0));
					applySweep(cv, want);
				}
			}
		};
		const commitAntenna = (idx) => {
			const e = rxPaths.entries[idx];
			if (!e || idx === rxPaths.curIdx) { return; }
			if (engine.sweepMode) {
				reenterSweep = { spanMHz: (modeCtl.cmdB - modeCtl.cmdA) || 400 };
				client.sendSweep(0, 0, 0, false);   /* the daemon applies path switches in RT */
			}
			client.sendAntenna(e.code, e.ch);
		};
		asel.onchange = () => commitAntenna(+asel.value);
		dsel.onchange = () => {
			fillPathSel(+dsel.value);
			commitAntenna(+asel.value);   /* first path of the picked board */
		};
	}
	/* rows/s only applies to raw/avg/fold pulls; the live path is LOD/credit-driven */
	function rateVisible() {
		if (engine.sweepMode) { return; }
		const on = +$("mode").value !== 3;
		$("rate").style.display = on ? "" : "none";
		$("rateLbl").style.display = on ? "" : "none";
	}
	$("mode").addEventListener("change", rateVisible);
	rateVisible();

	/* ---- Real Time / Sweep top-menu toggle ----
	 * Real Time = the single-LO live analyzer (default). Sweep hands the daemon a
	 * start/stop range at a FIXED 50 MHz per-hop bandwidth (the bw knob is gone: 50M is the
	 * validated speed/level sweet spot); the daemon hops the LO and stitches one full-span
	 * row per pass, positioned by the SPECTRA header center/span so the same renderer draws
	 * it. The ZOOM is the mode control: see modeCtl below. */
	function applySweep(centerMHz, spanMHz) {
		/* no/invalid args = survey the WHOLE band of the selected antenna (the manual-click
		 * default); everything is clamped to the path's band either way */
		let c = centerMHz, s = spanMHz;
		if (!isFinite(c) || !isFinite(s) || s <= 0) {
			c = (rxPaths.lim.lo + rxPaths.lim.hi) / 2;
			s = rxPaths.lim.hi - rxPaths.lim.lo;
		}
		const r = MorphMath.viewSweepRange(c, s, 100, rxPaths.lim.lo, rxPaths.lim.hi);
		if (!(r.b > r.a)) { $("stats").textContent = "no sweepable range inside the antenna band"; return false; }
		engine.sweepBwHz = modeCtl.SWEEP_BW;
		modeCtl.cmdA = r.a;
		modeCtl.cmdB = r.b;
		setFields(r);
		client.sendSweep(r.a * 1e6, r.b * 1e6, modeCtl.SWEEP_BW, true);
		persist("sw_center", (r.a + r.b) / 2); persist("sw_span", r.b - r.a);
		return true;
	}
	function setSweepMode(on, opts) {
		opts = opts || {};
		engine.sweepMode = on;
		engine._swHz = 0;
		engine._swRowT = 0;
		$("rt_rt").classList.toggle("on", !on);
		$("rt_sweep").classList.toggle("on", on);
		/* center+span are shared between the modes now; only the row-mode knobs + rec are RT-only */
		["mode", "rate", "rateLbl"].forEach(id => { const e = $(id); if (e) { e.style.display = on ? "none" : ""; } });
		const rb = $("recBtn"); if (rb) { rb.style.display = on ? "none" : ""; }
		if (!opts.keepView) {   /* a zoom-morph hands the view over seamlessly via _hwPending; manual toggles reset it */
			engine._hwPending = null; engine._tunePending = null; engine._zoomAnim = null;
			engine.zoomX = 1; engine.offX = 0;
		}
		engine.dirty = true;
		if (on) {
			if (!applySweep(opts.center, opts.span)) { setSweepMode(false); return; }   /* no range: stay in RT */
		} else {
			client.sendSweep(0, 0, 0, false);
			rateVisible();
		}
		persist("sweepMode", on ? 1 : 0);
	}

	/* ---- the zoom IS the mode control ----
	 * One continuous ladder from 3500 MHz down to kHz: in SWEEP, every settled view change
	 * (wheel, pan overshoot) re-commands the sweep range to what is on screen — narrower
	 * range = faster passes; once the view fits a single RT capture (< 100 MHz) it morphs to
	 * REAL TIME (LO to the view center, rate off the 2x ladder). In RT, wheeling out at the
	 * widest span (100M, zoomX 1) morphs back to sweep, ALWAYS — no memory of how the mode
	 * was entered (the old rtByZoom gate left manual/reloaded sessions permanently stuck).
	 * Every transition rides engine._hwPending: the old trace holds on screen and rescales
	 * in place when the first row of the new span lands. Flips are >= GAP_MS apart and each
	 * sends ONE sweep verb (G15: rapid enable/disable churn can wedge the ADC); range
	 * re-arms within sweep are cheap (same bw, new ladder — no reclock daemon-side). */
	const modeCtl = {
		RT_MAX_HZ: 100e6,          /* view narrower than this = RT territory */
		SWEEP_BW: 50000000,        /* fixed per-hop capture bandwidth */
		GAP_MS: 900, SETTLE_MS: 250, REARM_MS: 250, OUT_TICKS: 2, TICK_MS: 650, WIDEN_X: 1.15,
		lastFlip: 0, outN: 0, outT: 0, rearmT: null, widenSpan: 0,
		cmdA: NaN, cmdB: NaN,      /* last commanded sweep range, MHz (applySweep/rearm write it) */
		cancel() {
			clearTimeout(this.rearmT);
			this.rearmT = null; this.outN = 0; this.widenSpan = 0;
			if (engine.onDetentCharge) { engine.onDetentCharge(0, -1); }
		},
		ready() { return performance.now() - this.lastFlip >= this.GAP_MS && engine.meta.spanHz > 0; },
		visWin() {
			const m = engine.meta;
			return { cv: m.centerHz + m.spanHz * engine.offX, span: m.spanHz / engine.zoomX };
		},
		/* SWEEP: settled view changes re-command the range (or morph to RT below 100 MHz) */
		sweepView() {
			if (!engine.sweepMode) { return; }
			if (this.widenSpan) { return; }   /* a widen is charging: onZoomLod fires right after outStop and must not stomp its timer */
			clearTimeout(this.rearmT);
			this.rearmT = setTimeout(() => {
				this.rearmT = null;
				if (!engine.sweepMode) { return; }
				const w = this.visWin();
				if (w.span < this.RT_MAX_HZ) {
					if (this.ready()) { this.toRT(); } else { this.sweepView(); }   /* not ready yet: re-poll */
					return;
				}
				this.rearm(w.cv, w.span);
			}, this.REARM_MS);
		},
		/* SWEEP, parked at the commanded range: further zoom-out widens around the view
		 * center — WIDEN_X per NOTCH of physical scroll, same 1.15/notch rate as every
		 * other zoom (out must not feel hotter than in), commanded on a LEADING-EDGE
		 * throttle: a re-arm fires every REARM_MS DURING the scroll, so a sustained swipe
		 * is a smooth ladder of ~1.2x steps on screen. The old trailing debounce let the
		 * whole gesture compound into ONE leap ("250 -> 3000 MHz", 2026-07-12) — every
		 * event reset the timer, nothing fired until the fingers stopped. Capped at the
		 * selected antenna's band (the limits are FIXED per path). */
		sweepWiden(u) {
			const bandHz = (rxPaths.lim.hi - rxPaths.lim.lo) * 1e6;
			const span0 = this.widenSpan || (isFinite(this.cmdB - this.cmdA) ? (this.cmdB - this.cmdA) * 1e6 : engine.meta.spanHz);
			const fresh = !this.widenSpan;
			this.widenSpan = Math.min(bandHz, span0 * Math.pow(this.WIDEN_X, u || 1));
			if (fresh) { clearTimeout(this.rearmT); this.rearmT = null; }   /* take over a pending view re-arm */
			if (this.rearmT) { return; }   /* this widen's throttle tick is already scheduled */
			this.rearmT = setTimeout(() => {
				this.rearmT = null;
				/* ALWAYS clear the charge: a widenSpan left set when the mode flipped under the
				 * timer made sweepView() early-return forever - zoom-in in sweep went dead until
				 * a zoom-out "recovered" it (2026-07-12 late report) */
				const span = this.widenSpan;
				this.widenSpan = 0;
				if (!engine.sweepMode) { return; }
				this.rearm(this.visWin().cv, span, true);
			}, this.REARM_MS);
		},
		rearm(cvHz, spanHz, fit) {
			const r = MorphMath.viewSweepRange(cvHz / 1e6, spanHz / 1e6, this.RT_MAX_HZ / 1e6, rxPaths.lim.lo, rxPaths.lim.hi);
			if (Math.abs(r.a - this.cmdA) < 0.05 && Math.abs(r.b - this.cmdB) < 0.05) { return; }   /* already commanded */
			this.cmdA = r.a;
			this.cmdB = r.b;
			setFields(r);
			persist("sw_center", (r.a + r.b) / 2); persist("sw_span", r.b - r.a);
			client.sendSweep(r.a * 1e6, r.b * 1e6, this.SWEEP_BW, true);
			engine._swHz = 0;   /* new range = new pass cadence + scan rate: re-measure */
			engine._swRowT = 0;
			engine._hwPending = { from: engine.meta.spanHz, to: (r.b - r.a) * 1e6, cv: cvHz, widen: true, morph: true, quiet: true, fit: !!fit };
			engine._hwPendingT = performance.now();
		},
		/* RT: zoom-in disarms a charging sweep exit */
		rtZoom(z) {
			if (z > 1.01 && this.outN) { this.cancel(); }
		},
		/* RT, wheel-out parked at the widest hw span: charge OUT_TICKS notches (fractional
		 * for trackpads) then morph to sweep */
		outStop(u) {
			if (engine.sweepMode) { this.sweepWiden(u); return; }   /* (dblclick zoom-out routes here too) */
			if (!this.ready() || engine._hwPending) { return; }
			const now = performance.now();
			if (now - this.outT > this.TICK_MS) { this.outN = 0; }
			this.outT = now;
			this.outN += u || 1;
			if (engine.onDetentCharge) { engine.onDetentCharge(Math.min(1, this.outN / this.OUT_TICKS), -1); }
			if (this.outN < this.OUT_TICKS) { return; }
			this.cancel();
			this.toSweep();
		},
		toRT(idxOverride) {
			const m = engine.meta;
			const w = this.visWin();
			const idx = idxOverride !== undefined ? idxOverride : MorphMath.pickRtRateIdx(HW_RATES, w.span);
			const cv = Math.max(rxPaths.lim.lo * 1e6, Math.min(rxPaths.lim.hi * 1e6, w.cv));
			this.lastFlip = performance.now();
			this.cancel();
			setSweepMode(false, { keepView: true });   /* sends the single SWEEP-disable */
			engine._hwPending = { from: m.spanHz, to: HW_RATES[idx], cv: cv, widen: false, morph: true };
			engine._hwPendingT = performance.now();
			client.sendTune(cv, null);
			client.sendRate(HW_RATES[idx]);
			freqBox.value = (cv / 1e6).toFixed(3);
			NumField.normalize(freqBox);
			edgeFx.arm();
			this.flash();
		},
		toSweep(spanOverrideMHz) {
			const m = engine.meta;
			const w = this.visWin();
			/* zoom-out morph: comfortably wider than the RT capture so it visibly opens up;
			 * an explicit span (the span field) is honored as-is. Both clamp to the band. */
			const wantM = spanOverrideMHz !== undefined ? spanOverrideMHz : (w.span * 1.6) / 1e6;
			const minM = spanOverrideMHz !== undefined ? 100.1 : 160;
			const r = MorphMath.viewSweepRange(w.cv / 1e6, wantM, minM, rxPaths.lim.lo, rxPaths.lim.hi);
			this.lastFlip = performance.now();
			setSweepMode(true, { keepView: true, center: (r.a + r.b) / 2, span: r.b - r.a });
			if (!engine.sweepMode) { return; }         /* no sweepable range: stayed in RT */
			engine._hwPending = { from: m.spanHz, to: (r.b - r.a) * 1e6, cv: w.cv, widen: true, morph: true, fit: true };
			engine._hwPendingT = performance.now();
			edgeFx.arm();
			this.flash();
		},
		flash() {   /* the segmented control IS the mode indicator: pulse the side that just took over */
			const btn = engine.sweepMode ? $("rt_sweep") : $("rt_rt");
			btn.classList.remove("morphed");
			void btn.offsetWidth;
			btn.classList.add("morphed");
			setTimeout(() => btn.classList.remove("morphed"), 500);
		},
	};

	/* manual mode clicks: Real Time hands over AT THE VIEW CENTER (not wherever the LO
	 * happened to park pre-sweep); Sweep runs the field range. Clicking the active side
	 * re-applies/no-ops instead of churning the radio. */
	$("rt_rt").onclick = () => {
		if (!engine.sweepMode) { return; }
		modeCtl.lastFlip = performance.now();
		modeCtl.cancel();
		modeCtl.toRT();
	};
	$("rt_sweep").onclick = () => {
		modeCtl.cancel();
		if (engine.sweepMode) { return; }
		modeCtl.lastFlip = performance.now();
		setSweepMode(true);   /* manual Sweep = survey the selected antenna's whole band */
	};
	/* span field: ONE control for "how much spectrum am I looking at" — up to 100 MHz is a
	 * real-time capture (snapped onto the 2x rate ladder), more is a sweep of that width.
	 * Steppers walk the ladder in RT and scale the range x1.25 in sweep; crossing 100
	 * morphs, exactly like the wheel. */
	const spanBox = $("span");
	const commitSpan = (mhz) => {
		if (!isFinite(mhz) || mhz <= 0) { return; }
		const w = modeCtl.visWin();
		const cv = engine.meta.spanHz ? w.cv : (rxPaths.lim.lo + rxPaths.lim.hi) / 2 * 1e6;
		if (mhz <= 100.001) {
			const idx = MorphMath.pickRtRateIdx(HW_RATES, mhz * 1e6);
			if (engine.sweepMode) { modeCtl.toRT(idx); }
			else if (engine.meta.spanHz && Math.abs(HW_RATES[idx] - engine.meta.spanHz) > 1e5 && !engine._hwPending) {
				engine._hwPending = { from: engine.meta.spanHz, to: HW_RATES[idx], cv: cv, widen: HW_RATES[idx] > engine.meta.spanHz, morph: false };
				engine._hwPendingT = performance.now();
				edgeFx.arm();
				if (HW_RATES[idx] < engine.meta.spanHz) { client.sendTune(cv, null); }
				client.sendRate(HW_RATES[idx]);
			}
			return;
		}
		const s = Math.min(mhz, rxPaths.lim.hi - rxPaths.lim.lo);
		if (engine.sweepMode) { modeCtl.rearm(cv, s * 1e6, true); }
		else { modeCtl.toSweep(s); }
	};
	spanBox.addEventListener("change", () => commitSpan(NumField.parseFreqMHz(spanBox.value)));
	spanBox.addEventListener("keydown", (e) => { if (e.key === "Enter") { spanBox.blur(); } });
	$("spanUp").onclick = () => {
		if (engine.sweepMode) { commitSpan(((modeCtl.cmdB - modeCtl.cmdA) || 160) * 1.25); }
		else {
			const i = engine._rateIdx();
			if (i > 0) { engine.onHwStep(i - 1, true); }
			else { commitSpan(160); }   /* already at the widest capture: open into sweep */
		}
	};
	$("spanDn").onclick = () => {
		if (engine.sweepMode) { commitSpan((modeCtl.cmdB - modeCtl.cmdA) / 1.25); }
		else {
			const i = engine._rateIdx();
			if (i >= 0 && i < HW_RATES.length - 1) { engine.onHwStep(i + 1, false); }
		}
	};
	NumField.bindSteppers("#view-rtsa");
	NumField.bindWheel($("freq"));
	/* unit letter in the field text (2450M / 61.44M), normalize-on-commit: shared numfield.js
	 * unit-field pattern — programmatic freq/span writes above re-normalize after writing */
	NumField.bindUnit($("freq"));
	NumField.bindUnit($("span"));
	/* restore the persisted mode + range (values ride setSweepMode's opts; a missing/garbage
	 * range falls back to the antenna's full band once PATHS arrives) */
	if (recall("sweepMode", "0") === "1") {
		setSweepMode(true, { center: parseFloat(recall("sw_center", "NaN")), span: parseFloat(recall("sw_span", "NaN")) });
	}
});
