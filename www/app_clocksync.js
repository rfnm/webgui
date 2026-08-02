// Clock-sync visualizer (cellsyncd). Disciplines the RFNM local oscillator to LTE
// downlink carriers; reports the ppb/ppm offset of the local clock and of every
// measured carrier. Data: GET /api/app/clocksync/status (see cellsyncd main.cpp).
//
// This is a GPSDO-style clock-discipline instrument:
//  - a hero readout of the local-clock residual in ppb AND ppm AND absolute Hz at the
//    effective LO, with a lock badge and a CFO↔timing "observables agree" confidence badge
//  - a canvas strip-chart of the residual converging toward zero, with a ±sigma
//    envelope and the DCO actuator trace (its own scale); the x-axis is annotated in
//    elapsed loop-seconds and the derived update period. History is module-global so it
//    survives tab switches and is dropped only on a real daemon restart.
//  - a DCO actuator-rail gauge (±100000 steps / ±10 ppm) with amber/red bands near the
//    rails — the whole failure history is "wrong sign → runs to the rail", so seeing the
//    actuator sit centered is the primary health signal
//  - discipline metric tiles (steer, stability, median, counters)
//  - a per-carrier diverging bar chart + table; CFO-vs-timing agreement per cell, with
//    timing-invalid rows greyed out
"use strict";
(function() {

// ---- module state ----------------------------------------------------------------
// hist is a capped ring of loop-update snapshots {res, sig, dco, t}; one entry per new
// loop update (d.updates change) so the x-axis tracks real estimates, not poll jitter.
// It is module-global and deliberately survives panel rebuilds (tab switches) — it is
// dropped only when render() detects a real daemon restart (uptime/updates regression).
const S = { hist: [], maxHist: 300, chartFloor: 0.3, yv: 0, lastUp: null, lastUpdates: null, cellPrint: "", cadence: 0 };

// ---- number formatting -----------------------------------------------------------
function fnum(x) { const v = Number(x); return isFinite(v) ? v : NaN; }

// signed, with a typographic minus; "—" when not finite
function fmtSigned(v, d) {
	if(!isFinite(v)) { return "—"; }
	return (v < 0 ? "−" : "+") + Math.abs(v).toFixed(d);
}

// ppb → ppm string, precision scaled to magnitude (general audience wants ppm)
function fmtPpm(ppb) {
	if(!isFinite(ppb)) { return "—"; }
	const ppm = ppb / 1000, a = Math.abs(ppm);
	let s;
	if(a === 0) { s = "0"; }
	else if(a >= 0.1) { s = a.toFixed(3); }
	else if(a >= 0.0001) { s = a.toFixed(5); }
	else { s = a.toExponential(1); }
	return (ppm < 0 ? "−" : "") + s + " ppm";
}

// signed absolute frequency error, precision scaled to magnitude
function fmtHz(hz) {
	if(!isFinite(hz)) { return "—"; }
	const a = Math.abs(hz);
	const d = a < 1 ? 3 : (a < 10 ? 2 : (a < 100 ? 1 : 0));
	return (hz < 0 ? "−" : "+") + a.toFixed(d) + " Hz";
}

function f1(x) { const v = Number(x); return isFinite(v) ? v.toFixed(1) : "—"; }
function bigInt(x) { return (Number(x) || 0).toLocaleString(); }

// guard: the /api/app/clocksync/ port (7686) is shared, so a mis-routed response (e.g. AIS JSON)
// must never render as bogus clock data. Require the clock-shaped fields before drawing.
function looksLikeClock(d) {
	return !!d && typeof d === "object" && typeof d.mode === "string"
		&& ("residual_ppb" in d) && ("dco_steps" in d) && ("updates" in d) && Array.isArray(d.cells);
}

// ---- one-time CSS (prefixed cs-, injected into <head> once) -----------------------
function injectStyle() {
	if(document.getElementById("cs-style")) { return; }
	const st = document.createElement("style");
	st.id = "cs-style";
	st.textContent = `
	.cs-hero { display:flex; align-items:flex-start; justify-content:space-between; gap:16px; flex-wrap:wrap;
		background:linear-gradient(180deg,#0c1224,#0a0f1c); border:1px solid #1c2c40; border-radius:8px; padding:13px 16px; margin-bottom:10px; }
	.cs-hero-left { min-width:0; }
	.cs-hero-main { display:flex; align-items:baseline; gap:11px; flex-wrap:wrap; }
	.cs-badge { font-size:11px; letter-spacing:1px; text-transform:uppercase; padding:3px 10px; border-radius:5px;
		display:inline-flex; align-items:center; gap:6px; white-space:nowrap; align-self:center; }
	.cs-badge.lock { color:#10b981; background:#0d2a23; border:1px solid #1d5244; }
	.cs-badge.acq  { color:#e4b664; background:#201c0c; border:1px solid #4f421f; animation:cs-pulse 1.2s infinite; }
	.cs-dot { width:8px; height:8px; border-radius:50%; flex:none; }
	.cs-badge.lock .cs-dot { background:#10b981; }
	.cs-badge.acq  .cs-dot { background:#e4b664; }
	.cs-state { font-size:10px; color:#9ab0c4; text-transform:uppercase; letter-spacing:1px;
		border:1px dashed #2a4260; border-radius:4px; padding:2px 7px; align-self:center; }
	.cs-hero-val { font-size:36px; font-variant-numeric:tabular-nums; color:#dae3ec; line-height:1; letter-spacing:-0.5px; }
	.cs-hero-val.good { color:#8be6b0; }
	.cs-hero-unit { font-size:14px; color:#62798f; }
	.cs-hero-ppm { font-size:13px; color:#8fbfe6; font-variant-numeric:tabular-nums; }
	.cs-hero-hz { font-size:13px; color:#ffcf8f; font-variant-numeric:tabular-nums; }
	.cs-hero-sub { color:#9ab0c4; font-size:11px; margin-top:6px; line-height:1.5; }
	.cs-hero-badges { margin-top:9px; display:flex; gap:8px; flex-wrap:wrap; align-items:center; }
	.cs-agree { font-size:10px; letter-spacing:0.5px; text-transform:uppercase; padding:3px 9px; border-radius:5px;
		display:inline-flex; align-items:center; gap:6px; white-space:nowrap; font-variant-numeric:tabular-nums; }
	.cs-agree .cs-dot { width:7px; height:7px; }
	.cs-agree.ok   { color:#10b981; background:#0d2a23; border:1px solid #1d5244; }
	.cs-agree.ok   .cs-dot { background:#10b981; }
	.cs-agree.warn { color:#e8a94f; background:#1a1206; border:1px solid #4a3410; }
	.cs-agree.warn .cs-dot { background:#e8a94f; box-shadow:0 0 5px #e8a94f; }
	.cs-agree.na   { color:#6b7c96; background:#0d1421; border:1px solid #1c2c40; }
	.cs-agree.na   .cs-dot { background:#465671; box-shadow:none; }
	.cs-agree small { color:#62798f; text-transform:none; letter-spacing:0; }
	.cs-hero-meta { text-align:right; color:#62798f; font-size:11px; line-height:1.65; font-variant-numeric:tabular-nums; white-space:nowrap; }
	.cs-hero-meta b { color:#9ab0c4; font-weight:normal; }
	.cs-chart { width:100%; height:186px; background:#0a1019; border:1px solid #1c2c40; border-radius:8px; display:block; }
	.cs-legend { display:flex; gap:15px; flex-wrap:wrap; color:#62798f; font-size:10px; margin:6px 2px 12px; }
	.cs-legend span { display:inline-flex; align-items:center; gap:5px; }
	.cs-sw { width:15px; height:3px; border-radius:2px; display:inline-block; flex:none; }
	.cs-sw.dash { height:0; border-top:2px dashed rgba(16,185,129,0.75); border-radius:0; }
	.cs-sec { color:#62798f; font-size:10px; text-transform:uppercase; letter-spacing:1px; margin:14px 0 3px; }
	.cs-cap { color:#62798f; font-size:10px; margin-bottom:9px; }
	.cs-cap b { color:#10b981; font-weight:normal; }
	.cs-amb { color:#e8a94f; }
	.cs-red { color:#f66a85; }
	.cs-rail-wrap { margin:2px 0 4px; }
	.cs-rail { position:relative; height:22px; background:#15243a; border:1px solid #16222f; border-radius:5px; overflow:hidden; }
	.cs-rail-zone { position:absolute; top:0; bottom:0; }
	.cs-rail-zone.amb { background:rgba(255,179,71,0.30); }
	.cs-rail-zone.red { background:rgba(255,107,107,0.42); }
	.cs-rail-zero { position:absolute; left:50%; top:0; bottom:0; width:1px; background:#33445f; }
	.cs-rail-tick { position:absolute; top:0; bottom:0; width:1px; background:#243352; }
	.cs-rail-marker { position:absolute; top:-3px; bottom:-3px; width:2px; margin-left:-1px; background:#7fb6dd;
		box-shadow:0 0 8px rgba(127,208,255,0.95); transition:left .3s ease; }
	.cs-rail-marker::before { content:""; position:absolute; left:50%; top:-3px; width:7px; height:7px; margin-left:-3.5px;
		border-radius:50%; background:#7fb6dd; box-shadow:0 0 6px rgba(127,208,255,0.95); }
	.cs-rail-marker.amb, .cs-rail-marker.amb::before { background:#e8a94f; }
	.cs-rail-marker.red, .cs-rail-marker.red::before { background:#f66a85; }
	.cs-rail-marker.amb { background:#e8a94f; box-shadow:0 0 7px rgba(255,179,71,0.9); }
	.cs-rail-marker.red { background:#f66a85; box-shadow:0 0 8px rgba(255,107,107,0.95); }
	.cs-rail-labels { display:flex; justify-content:space-between; color:#3e6188; font-size:9px; margin-top:3px; font-variant-numeric:tabular-nums; }
	.cs-rail-read { color:#9ab0c4; font-size:11px; margin-top:5px; font-variant-numeric:tabular-nums; }
	.cs-rail-read b { color:#dae3ec; font-weight:normal; }
	.cs-bars { display:flex; flex-direction:column; gap:5px; margin-bottom:12px; }
	.cs-bar-row { display:grid; grid-template-columns:104px 1fr 132px; align-items:center; gap:10px; }
	.cs-bar-pci { display:flex; align-items:center; gap:6px; color:#dae3ec; font-size:12px; font-variant-numeric:tabular-nums; }
	.cs-bw { font-size:9px; padding:1px 5px; border-radius:3px; border:1px solid #2a4260; color:#62798f; }
	.cs-bw.wide { color:#8fbfe6; border-color:#2a4260; }
	.cs-bar-snr { color:#62798f; font-size:10px; font-variant-numeric:tabular-nums; }
	.cs-track { position:relative; height:17px; background:#15243a; border:1px solid #16222f; border-radius:4px; overflow:hidden; }
	.cs-track .zero { position:absolute; left:50%; top:0; bottom:0; width:1px; background:#2a3752; }
	.cs-fill { position:absolute; top:2px; bottom:2px; border-radius:2px; background:#33445f; }
	.cs-fill.contrib { background:#10b981; }
	.cs-bar-val { text-align:right; font-variant-numeric:tabular-nums; }
	.cs-bar-val .p1 { color:#dae3ec; font-size:11px; }
	.cs-bar-val .p2 { color:#62798f; font-size:10px; }
	.cs-table-wrap { overflow-x:auto; }
	.ap-table tr.cs-contrib td { color:#cfe6d8; }
	.ap-table tr.cs-contrib td:first-child { box-shadow:inset 2px 0 0 #10b981; }
	.ap-table tr.cs-tim-na td { color:#4d5a70; }
	@keyframes cs-pulse { 0%,100% { opacity:1; } 50% { opacity:0.55; } }
	`;
	document.head.appendChild(st);
}

// ---- panel skeleton (built once, cached on ui[id].panel) -------------------------
function mkLeg(A, cls, color, text) {
	const s = A.el("span");
	const sw = A.el("span", cls);
	if(color) { sw.style.background = color; }
	s.append(sw, document.createTextNode(text));
	return s;
}

function ensurePanel(id) {
	const A = window.RFNM_APPS, ce = A.ui[id];
	if(ce.panel) { return ce.panel; }
	injectStyle();
	ce.live.textContent = "";

	// hero: lock badge + big residual (ppb) + ppm + absolute Hz + narrative + agreement,
	// radio facts on the right
	const hero = A.el("div", "cs-hero");
	const left = A.el("div", "cs-hero-left");
	const main = A.el("div", "cs-hero-main");
	const badge = A.el("span", "cs-badge acq");
	const badgeDot = A.el("span", "cs-dot");
	const badgeTxt = A.el("span", null, "ACQUIRING");
	badge.append(badgeDot, badgeTxt);
	const statePill = A.el("span", "cs-state", "");
	statePill.style.display = "none";
	const heroVal = A.el("span", "cs-hero-val", "—");
	const heroUnit = A.el("span", "cs-hero-unit", "ppb");
	const heroPpm = A.el("span", "cs-hero-ppm", "");
	const heroHz = A.el("span", "cs-hero-hz", "");
	main.append(badge, statePill, heroVal, heroUnit, heroPpm, heroHz);
	const heroSub = A.el("div", "cs-hero-sub", "");
	// CFO↔timing agreement badge (two independent observables that should read the same)
	const heroBadges = A.el("div", "cs-hero-badges");
	const agree = A.el("span", "cs-agree na");
	const agreeDot = A.el("span", "cs-dot");
	const agreeTxt = A.el("span", null, "CFO / TIMING — waiting");
	agree.append(agreeDot, agreeTxt);
	heroBadges.append(agree);
	left.append(main, heroSub, heroBadges);
	const heroMeta = A.el("div", "cs-hero-meta");
	hero.append(left, heroMeta);

	// strip-chart of the residual converging toward zero
	const chart = document.createElement("canvas");
	chart.className = "cs-chart";
	const legend = A.el("div", "cs-legend");
	legend.append(
		mkLeg(A, "cs-sw", "#7fb6dd", "residual (clock error)"),
		mkLeg(A, "cs-sw", "rgba(87,163,228,0.38)", "±σ uncertainty"),
		mkLeg(A, "cs-sw dash", null, "target = 0"),
		mkLeg(A, "cs-sw", "rgba(255,212,121,0.75)", "DCO steer (own scale)"));

	// DCO actuator-rail gauge - scale comes from the daemon's dco_range_steps (servo authority, default ±20000 = ±2 ppm)
	const railSec = A.el("div", "cs-sec", "DCO actuator rail");
	const railCap = A.el("div", "cs-cap");
	railCap.append(document.createTextNode("actuator position within the servo authority · "),
		A.el("span", "cs-amb", "amber ≥ ±70k"), document.createTextNode(" · "),
		A.el("span", "cs-red", "red ≥ ±90k"), document.createTextNode(" — a wrong-sign loop runs to the rail"));
	const railWrap = A.el("div", "cs-rail-wrap");
	const rail = A.el("div", "cs-rail");
	const zRedL = A.el("div", "cs-rail-zone red"); zRedL.style.left = "0%"; zRedL.style.width = "5%";
	const zAmbL = A.el("div", "cs-rail-zone amb"); zAmbL.style.left = "5%"; zAmbL.style.width = "10%";
	const zAmbR = A.el("div", "cs-rail-zone amb"); zAmbR.style.left = "85%"; zAmbR.style.width = "10%";
	const zRedR = A.el("div", "cs-rail-zone red"); zRedR.style.left = "95%"; zRedR.style.width = "5%";
	const tickL = A.el("div", "cs-rail-tick"); tickL.style.left = "15%";
	const tickR = A.el("div", "cs-rail-tick"); tickR.style.left = "85%";
	const railZero = A.el("div", "cs-rail-zero");
	const railMarker = A.el("div", "cs-rail-marker");
	railMarker.style.left = "50%";
	rail.append(zRedL, zAmbL, zAmbR, zRedR, tickL, tickR, railZero, railMarker);
	const railLabels = A.el("div", "cs-rail-labels");
	railLabels.append(A.el("span", null, "−100k"), A.el("span", null, "0"), A.el("span", null, "+100k"));
	const railRead = A.el("div", "cs-rail-read");
	railWrap.append(rail, railLabels, railRead);

	const tiles = A.el("div", "ap-tiles");

	const secTitle = A.el("div", "cs-sec", "measured carriers");
	const cap = A.el("div", "cs-cap");
	cap.append(document.createTextNode("frequency offset of each cell from center · "),
		A.el("b", null, "green"), document.createTextNode(" = contributing to the disciplined solution"));
	const carriers = A.el("div");
	const bars = A.el("div", "cs-bars");
	const tblWrap = A.el("div", "cs-table-wrap");
	carriers.append(bars, tblWrap);

	const hint = A.el("div", "ap-hint");

	ce.live.append(hero, chart, legend, railSec, railCap, railWrap, tiles, secTitle, cap, carriers, hint);
	// A fresh DOM was just built — force the carrier bars/table to repopulate. Do NOT reset
	// the strip-chart ring (S.hist/yv/lastUp/lastUpdates): it is module-global so the chart
	// survives a hidden→shown rebuild; render() drops it only on a real daemon restart.
	S.cellPrint = "";
	ce.panel = { badge, badgeDot, badgeTxt, statePill, heroVal, heroUnit, heroPpm, heroHz, heroSub,
		agree, agreeTxt, heroMeta, chart, railMarker, railRead, tiles, bars, tblWrap, carriers, hint };
	return ce.panel;
}

// ---- strip-chart -----------------------------------------------------------------
// Left/gridded axis = residual ppb (symmetric about zero); a ±sigma envelope hugs the
// trace; a dashed green line marks the target (0). The DCO actuator rides its own
// auto-fit vertical scale (labelled at the right edge) so its settling is visible even
// when the residual and the steer live at very different magnitudes. The x-axis is
// annotated in elapsed loop-seconds (updates × derived period) with the update period.
function drawChart(cv, acquiring) {
	const dpr = window.devicePixelRatio || 1;
	const w = cv.clientWidth, h = cv.clientHeight;
	if(!w || !h) { return; }
	if(cv.width !== Math.round(w * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr); }
	const g = cv.getContext("2d");
	g.setTransform(dpr, 0, 0, dpr, 0, 0);
	g.clearRect(0, 0, w, h);
	g.font = "10px ui-monospace, monospace";
	g.textBaseline = "middle";
	const padL = 48, padR = 50, padT = 12, padB = 18;
	const pw = w - padL - padR, ph = h - padT - padB;
	if(pw <= 12 || ph <= 12) { return; }
	const midY = padT + ph / 2;
	const hist = S.hist;

	if(hist.length < 2) {
		g.fillStyle = "#3e6188";
		g.textAlign = "center";
		g.fillText(acquiring ? "acquiring — searching for an LTE downlink carrier…" : "waiting for the loop to produce estimates…", w / 2, h / 2);
		g.textAlign = "left";
		return;
	}

	// residual vertical range: fit |res|+sigma, floor so a locked clock still has scale,
	// smoothed (expand fast, contract slowly) so it doesn't pump
	let ymax = S.chartFloor;
	for(const p of hist) { const e = Math.abs(p.res) + Math.abs(p.sig); if(e > ymax) { ymax = e; } }
	ymax *= 1.18;
	S.yv = S.yv ? S.yv + (ymax - S.yv) * (ymax > S.yv ? 0.5 : 0.05) : ymax;
	const Y = Math.max(S.yv, S.chartFloor);
	const dec = Y < 1 ? 2 : (Y < 10 ? 1 : 0);
	const yPx = v => midY - (Math.max(-Y, Math.min(Y, v)) / Y) * (ph / 2);
	const n = hist.length;
	const xPx = i => padL + (n === 1 ? pw / 2 : (i / (n - 1)) * pw);

	// residual gridlines + left labels
	g.strokeStyle = "#16222f";
	g.fillStyle = "#3e6188";
	g.textAlign = "right";
	for(const f of [1, 0.5, -0.5, -1]) {
		const y = yPx(f * Y);
		g.beginPath(); g.moveTo(padL, y); g.lineTo(padL + pw, y); g.stroke();
		g.fillText((f > 0 ? "+" : "−") + (f * Y * (f < 0 ? -1 : 1)).toFixed(dec), padL - 5, y);
	}

	// DCO auto-fit scale (bottom→top of plot), independent of the residual axis
	let dmin = Infinity, dmax = -Infinity;
	for(const p of hist) { if(p.dco < dmin) { dmin = p.dco; } if(p.dco > dmax) { dmax = p.dco; } }
	if(!isFinite(dmin) || !isFinite(dmax)) { dmin = -1; dmax = 1; }
	if(dmax - dmin < 0.2) { const c = (dmax + dmin) / 2; dmin = c - 0.5; dmax = c + 0.5; }
	else { const pad = (dmax - dmin) * 0.15; dmin -= pad; dmax += pad; }
	const dPx = v => padT + ph - ((v - dmin) / (dmax - dmin)) * ph;

	// DCO actuator trace (thin amber)
	g.strokeStyle = "rgba(255,212,121,0.5)";
	g.lineWidth = 1;
	g.beginPath();
	hist.forEach((p, i) => { const x = xPx(i), y = dPx(p.dco); if(i) { g.lineTo(x, y); } else { g.moveTo(x, y); } });
	g.stroke();
	g.fillStyle = "rgba(255,212,121,0.7)";
	g.textAlign = "left";
	g.fillText(dmax.toFixed(2), padL + pw + 5, padT + 6);
	g.fillText(dmin.toFixed(2), padL + pw + 5, padT + ph - 6);

	// ±sigma envelope (translucent), then dashed target line, then residual trace
	g.fillStyle = "rgba(87,163,228,0.13)";
	g.beginPath();
	hist.forEach((p, i) => { const x = xPx(i), y = yPx(p.res + p.sig); if(i) { g.lineTo(x, y); } else { g.moveTo(x, y); } });
	for(let i = n - 1; i >= 0; i--) { g.lineTo(xPx(i), yPx(hist[i].res - hist[i].sig)); }
	g.closePath();
	g.fill();

	g.strokeStyle = "rgba(16,185,129,0.55)";
	g.lineWidth = 1;
	g.setLineDash([4, 4]);
	g.beginPath(); g.moveTo(padL, midY); g.lineTo(padL + pw, midY); g.stroke();
	g.setLineDash([]);

	g.strokeStyle = "#7fb6dd";
	g.lineWidth = 1.6;
	g.beginPath();
	hist.forEach((p, i) => { const x = xPx(i), y = yPx(p.res); if(i) { g.lineTo(x, y); } else { g.moveTo(x, y); } });
	g.stroke();
	const lx = xPx(n - 1), ly = yPx(hist[n - 1].res);
	g.fillStyle = "#7fb6dd";
	g.beginPath(); g.arc(lx, ly, 2.6, 0, 2 * Math.PI); g.fill();

	// x-axis caption: elapsed loop-seconds (from the derived update period) + cadence.
	// One history point == one loop update, so window ≈ (n-1) × period; this is robust to
	// tab-switch gaps (it is anchored to update count, not wall clock).
	const cad = S.cadence;
	const winTxt = (cad > 0) ? ("−" + Math.round((n - 1) * cad) + " s") : (n + " updates");
	g.fillStyle = "#3e6188";
	g.textAlign = "left";
	g.fillText(winTxt + " →", padL, h - 7);
	if(cad > 0) {
		g.textAlign = "center";
		g.fillText("≈ " + cad.toFixed(1) + " s / update", padL + pw / 2, h - 7);
	}
	g.textAlign = "right";
	g.fillText("now", padL + pw, h - 7);
}

// ---- per-carrier diverging bar ---------------------------------------------------
function buildBar(A, c, maxAbs, maxW) {
	const off = Number(c.offset_ppb) || 0;
	const wgt = Number(c.weight) || 0;
	const contrib = wgt > 0;
	const row = A.el("div", "cs-bar-row");

	const pci = A.el("div", "cs-bar-pci");
	pci.append(document.createTextNode(String(c.pci)));
	pci.appendChild(A.el("span", "cs-bw" + (c.wide ? " wide" : ""), c.wide ? "20M" : "narrow"));
	pci.appendChild(A.el("span", "cs-bar-snr", (isFinite(Number(c.snr_db)) ? Number(c.snr_db).toFixed(0) : "–") + " dB"));

	const track = A.el("div", "cs-track");
	track.appendChild(A.el("div", "zero"));
	const fill = A.el("div", "cs-fill" + (contrib ? " contrib" : ""));
	const frac = Math.max(-1, Math.min(1, off / maxAbs));
	let left, width;
	if(frac >= 0) { left = 50; width = frac * 50; }
	else { width = -frac * 50; left = 50 - width; }
	fill.style.left = left + "%";
	fill.style.width = Math.max(width, 0.6) + "%";
	if(contrib && maxW > 0) { fill.style.opacity = (0.45 + 0.55 * (wgt / maxW)).toFixed(2); }
	track.appendChild(fill);

	const val = A.el("div", "cs-bar-val");
	val.append(A.el("div", "p1", fmtSigned(off, 3) + " ppb"), A.el("div", "p2", fmtPpm(off)));

	row.append(pci, track, val);
	row.title = "PCI " + c.pci + " · weight " + Math.round(wgt) + " · cfo " + (Number(c.cfo_hz) || 0).toFixed(1) + " Hz";
	return row;
}

// ---- poll/render -----------------------------------------------------------------
function render(id) {
	const A = window.RFNM_APPS;
	if(A.liveBusy[id]) { return; }
	A.liveBusy[id] = true;
	const p = ensurePanel(id);
	A.fetchJSON("/api/app/clocksync/status", undefined, 6000).then(d => {
		// shape guard: never render a mis-routed (non-clock) response on the shared port
		if(!looksLikeClock(d)) {
			p.hint.textContent = "received a response with no clock fields on this port — ignoring (waiting for cellsyncd /status)";
			return;
		}
		const up = fnum(d.uptime);
		const upd = Number(d.updates) || 0;
		// daemon restart: uptime jumped back, or the update counter regressed → drop history
		if((isFinite(S.lastUp) && isFinite(up) && up < S.lastUp - 1) || (S.lastUpdates !== null && upd < S.lastUpdates)) {
			S.hist.length = 0; S.yv = 0; S.lastUpdates = null; S.cellPrint = "";
		}
		S.lastUp = up;
		// derived loop update period (≈ steer period_s), used for the chart's elapsed-seconds axis
		if(upd > 0 && isFinite(up) && up > 0) { S.cadence = up / upd; }

		const acquiring = (d.mode || "acquire") === "acquire";
		const res = fnum(d.residual_ppb);
		const sigRaw = fnum(d.sigma_ppb);
		const sig = isFinite(sigRaw) ? Math.abs(sigRaw) : NaN;
		const dco = fnum(d.dco_ppb);
		const freq = Number(d.freq) || 0;
		const offHz = Number(d.offset) || 0;
		const fLO = freq + offHz;                       // effective local-oscillator frequency (Hz)
		const resHz = (isFinite(res) && fLO) ? res * fLO * 1e-9 : NaN;   // absolute clock error in Hz
		const steerHz = (isFinite(dco) && fLO) ? dco * fLO * 1e-9 : NaN; // what the DCO is steering, in Hz

		// one history point per NEW loop update (keeps the x-axis on real estimates)
		if(!acquiring && upd > 0 && upd !== S.lastUpdates && isFinite(res)) {
			S.hist.push({ res, sig: isFinite(sig) ? sig : 0, dco: isFinite(dco) ? dco : 0, t: Date.now() });
			if(S.hist.length > S.maxHist) { S.hist.shift(); }
		}
		S.lastUpdates = upd;

		const locked = !acquiring && isFinite(res) && Math.abs(res) <= Math.max(2 * (isFinite(sig) ? sig : 0), 1);

		// ---- hero badge + big readout ----
		if(acquiring) {
			p.badge.className = "cs-badge acq";
			p.badgeTxt.textContent = "ACQUIRING";
			p.statePill.style.display = "none";
		} else {
			p.badge.className = "cs-badge lock";
			p.badgeTxt.textContent = locked ? "LOCKED" : "TRACKING";
			p.statePill.style.display = "";
			p.statePill.textContent = "loop: " + String(d.mode);
		}
		if(isFinite(res)) {
			p.heroVal.textContent = fmtSigned(res, 3);
			p.heroVal.className = "cs-hero-val" + (locked ? " good" : "");
			p.heroPpm.textContent = "= " + fmtPpm(res);
			p.heroHz.textContent = (fLO ? "· " + fmtHz(resHz) + " @ " + (fLO / 1e6).toFixed(3) + " MHz" : "");
		} else {
			p.heroVal.textContent = "—";
			p.heroVal.className = "cs-hero-val";
			p.heroPpm.textContent = "";
			p.heroHz.textContent = "";
		}
		if(acquiring) {
			p.heroSub.textContent = "searching for an LTE downlink carrier to lock — point the antenna toward a cell tower";
		} else if(isFinite(res)) {
			p.heroSub.textContent = "local clock is " + fmtSigned(res, 3) + " ppb (" + fmtHz(resHz) + ") from the LTE network — " +
				(locked ? "disciplined and holding" : "loop converging");
		} else {
			p.heroSub.textContent = "locked to the network; waiting for the first residual estimate…";
		}

		// ---- CFO↔timing agreement badge --------------------------------------------
		// offset_ppb (from CFO) and tim_ppb (from timing slope) are independent observables
		// of the same clock error and, with the daemon's sign convention, should read the
		// same. Their agreement on the strongest weighted cell is the confidence signal.
		{
			let topW = null;
			for(const c of (Array.isArray(d.cells) ? d.cells : [])) {
				if((Number(c.weight) || 0) > 0 && (topW === null || (Number(c.weight) || 0) > (Number(topW.weight) || 0))) { topW = c; }
			}
			// global weighted CFO−timing delta (over weighted, timing-valid cells) for the tooltip
			let sw = 0, so = 0, stt = 0, nt = 0;
			for(const c of (Array.isArray(d.cells) ? d.cells : [])) {
				const w = Number(c.weight) || 0;
				if(w > 0 && c.tim_valid) { sw += w; so += w * (Number(c.offset_ppb) || 0); stt += w * (Number(c.tim_ppb) || 0); nt++; }
			}
			const gDelta = sw > 0 ? Math.abs(so / sw - stt / sw) : NaN;
			const good = Math.max(3, 1.5 * (isFinite(sig) ? sig : 0));    // "agree" threshold, ppb
			if(!topW) {
				p.agree.className = "cs-agree na";
				p.agreeTxt.textContent = "CFO / TIMING — no weighted cell yet";
				p.agree.title = "no carrier is currently weighted into the disciplined solution";
			} else if(!topW.tim_valid) {
				p.agree.className = "cs-agree na";
				p.agreeTxt.textContent = "CFO / TIMING — timing n/a (PCI " + topW.pci + ")";
				p.agree.title = "strongest weighted cell PCI " + topW.pci + " has no valid timing slope yet (steering on CFO alone)";
			} else {
				const off = Number(topW.offset_ppb) || 0, tim = Number(topW.tim_ppb) || 0;
				const delta = Math.abs(off - tim);
				const ok = delta <= good;
				p.agree.className = "cs-agree " + (ok ? "ok" : "warn");
				p.agreeTxt.textContent = (ok ? "observables agree ±" : "observables diverge Δ") + delta.toFixed(1) + " ppb";
				const gtxt = isFinite(gDelta) ? gDelta.toFixed(1) + " ppb over " + nt + " cell" + (nt === 1 ? "" : "s") : "n/a";
				p.agree.title = "PCI " + topW.pci + ": CFO offset " + fmtSigned(off, 1) + " ppb vs timing " + fmtSigned(tim, 1) +
					" ppb (should match by identity CFO/f_LO = −timing-slope). Global weighted CFO−timing Δ = " + gtxt +
					". Threshold ±" + good.toFixed(1) + " ppb.";
			}
		}

		// ---- hero meta (radio facts) ----
		p.heroMeta.textContent = "";
		const metaRow = (v, l) => { const r = A.el("div"); r.append(A.el("b", null, v), document.createTextNode(" " + l)); p.heroMeta.appendChild(r); };
		metaRow((freq / 1e6).toFixed(3) + " MHz", "LTE downlink");
		metaRow((fLO / 1e6).toFixed(3) + " MHz", "effective LO (" + fmtSigned(offHz / 1e3, 0) + " kHz offset)");
		metaRow(((Number(d.rate) || 0) / 1e6).toFixed(2) + " MSps", "capture rate");
		metaRow(fmtSigned(fnum(d.coarse_cfo), 0) + " Hz", "coarse CFO");
		metaRow(bigInt(d.samples), "samples");
		metaRow(A.fmtUp(Number(d.uptime) || 0), "uptime");

		// ---- DCO actuator-rail gauge ----
		{
			const steps = Number(d.dco_steps) || 0;
			const mag = Math.abs(steps);
			// display rail is ±1 ppm (10 ppm-wide gauges make real trims unreadable);
			// the marker clamps at the edge if the servo (authority from the daemon, default
			// ±2 ppm) ever walks past the display range
			const R = 10000;
			const auth = Number(d.dco_range_steps) || 100000;
			const pos = Math.max(0, Math.min(100, (steps + R) / (2 * R) * 100));
			p.railMarker.style.left = pos + "%";
			p.railMarker.className = "cs-rail-marker" + (mag > 0.9 * auth ? " red" : (mag > 0.7 * auth ? " amb" : ""));
			p.railRead.textContent = "";
			const b = t => A.el("b", null, t);
			p.railRead.append(
				document.createTextNode("DCO at "), b(bigInt(steps) + " steps"),
				document.createTextNode(" · rail ±1 ppm shown, authority ±" + (auth / 10000).toFixed(1) + " ppm · "), b(fmtSigned(dco, 1) + " ppb"),
				document.createTextNode(" → "), b(fmtHz(steerHz)),
				document.createTextNode(fLO ? " @ " + (fLO / 1e6).toFixed(3) + " MHz · " : " · "),
				b(bigInt(Math.max(0, auth - mag)) + " steps"), document.createTextNode(" of headroom to the authority rail"));
		}

		// ---- discipline metric tiles ----
		let trend = "";
		if(S.hist.length >= 6 && isFinite(dco)) {
			const dd = dco - S.hist[S.hist.length - 6].dco;
			trend = " " + (Math.abs(dd) < 0.02 ? "→" : (dd > 0 ? "↑" : "↓"));
		}
		p.tiles.textContent = "";
		p.tiles.append(
			A.tile(isFinite(dco) ? fmtSigned(dco, 2) + " ppb" + trend : "—", "clock steer"),
			A.tile(isFinite(steerHz) ? fmtHz(steerHz) : "—", "steer @ f_LO"),
			A.tile(isFinite(sig) ? "±" + sig.toFixed(3) : "—", "stability σ ppb"),
			A.tile(fmtSigned(fnum(d.median_ppb), 3), "network median ppb"),
			A.tile(bigInt(d.updates), "loop updates"),
			A.tile(bigInt(d.acquisitions), "acquisitions"),
			A.tile(bigInt(d.dco_writes), "dco writes"),
			A.tile(bigInt(d.dco_steps), "dco steps ·0.1ppb"));

		// ---- strip-chart ----
		drawChart(p.chart, acquiring);

		// ---- per-carrier bars + table (rebuild only on change, never under a selection) ----
		const cells = Array.isArray(d.cells) ? d.cells : [];
		const sorted = cells.slice().sort((a, b) =>
			((Number(b.weight) || 0) - (Number(a.weight) || 0)) || ((Number(b.snr_db) || 0) - (Number(a.snr_db) || 0)));
		let maxAbs = 1, maxW = 0;
		for(const c of sorted) {
			maxAbs = Math.max(maxAbs, Math.abs(Number(c.offset_ppb) || 0));
			maxW = Math.max(maxW, Number(c.weight) || 0);
		}
		const print = sorted.map(c => [c.pci, c.wide ? 1 : 0, f1(c.snr_db), f1(c.cfo_hz),
			(Number(c.offset_ppb) || 0).toFixed(3), c.tim_valid ? f1(c.tim_ppb) : "x",
			Math.round(Number(c.weight) || 0), Math.round(Number(c.age) || 0)].join(",")).join(";");
		if(print !== S.cellPrint && !A.selectionInside(p.carriers)) {
			S.cellPrint = print;
			p.bars.textContent = "";
			for(const c of sorted.slice(0, 16)) { p.bars.appendChild(buildBar(A, c, maxAbs, maxW)); }
			p.tblWrap.textContent = "";
			if(sorted.length) {
				const rows = sorted.map(c => {
					const off = Number(c.offset_ppb) || 0;
					const tv = !!c.tim_valid;
					const dtxt = tv ? fmtSigned(off - (Number(c.tim_ppb) || 0), 1) : "—";
					const row = A.tr([c.pci, c.wide ? "20 MHz" : "narrow", f1(c.snr_db), f1(c.cfo_hz),
						fmtSigned(off, 3), fmtPpm(off), tv ? f1(c.tim_ppb) : "—", dtxt,
						Math.round(Number(c.weight) || 0), (Number(c.age) || 0).toFixed(1) + "s"]);
					if((Number(c.weight) || 0) > 0) { row.classList.add("cs-contrib"); }
					if(!tv) { row.classList.add("cs-tim-na"); }
					return row;
				});
				p.tblWrap.appendChild(A.table(
					["PCI", "bw", "SNR dB", "CFO Hz", "offset ppb", "offset ppm", "tim ppb", "Δ cfo−tim", "weight", "age"], rows));
			}
		}
		p.hint.textContent = cells.length
			? (maxW > 0 ? "" : "carriers seen but none yet weighted into the solution — needs higher SNR / stable timing")
			: (acquiring ? "acquiring — point the antenna at an LTE cell tower to lock a downlink carrier" : "no carriers currently measured");
	}).catch(() => {}).finally(() => { A.liveBusy[id] = false; });
}

(window.__RFNM_APP_MODULES = window.__RFNM_APP_MODULES || []).push({
	id: "clocksync", title: "Clock sync",
	desc: "cellsyncd disciplines the RFNM oscillator to LTE downlink carriers: the ppb/ppm/Hz offset of the local clock and of every measured carrier.",
	icon: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.2"><circle cx="8" cy="8" r="6.4"/><path d="M8 3.6 V8 l3.1 1.9"/></svg>',
	live: render,
});

})();
