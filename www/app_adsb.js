// ADS-B visualizer (app_adsb.js): full-screen live map + table for readsb.
// Self-registers into the APPS view. Reads /adsb/aircraft.json, /adsb/stats.json and
// /adsb/receiver.json (all served by nginx from readsb's /run/rfnm-adsb).
//
// Basemap is progressive: when the viewer's browser has internet it loads real Web-Mercator
// tiles (dark street or satellite); with no internet it falls back to a self-hosted Natural
// Earth vector coastline (geodata.js, lazy-loaded) drawn on the same <canvas>. Aircraft,
// trails and range rings are all projected in Web Mercator so everything lines up.
"use strict";
(function() {

const A = () => window.RFNM_APPS;
const ICON = '<svg viewBox="0 0 16 16" fill="currentColor" stroke="none"><path d="M8 1.2 L9 5.8 L15 8.6 L9 8.9 L8.8 12.4 L11 14.4 L8 13.5 L5 14.4 L7.2 12.4 L7 8.9 L1 8.6 L7 5.8 Z"/></svg>';

window.__RFNM_APP_MODULES = window.__RFNM_APP_MODULES || [];
window.__RFNM_APP_MODULES.push({
	id: "adsb", title: "ADS-B", icon: ICON,
	desc: "readsb decoding 1090 MHz Mode-S/ADS-B at 2.4 MSps with a native RFNM driver.",
	live: render,
});

// ---- basemap tile sources (loaded by the viewer's browser; https on an http page is OK) --
// no crossOrigin: we only draw tiles, never read pixels, so canvas-taint doesn't matter and
// we avoid CORS failures on servers that don't send the header.
const SUBS = ["a", "b", "c", "d"];
const TILESETS = {
	map: { name: "map", label: "Dark map", dark: true,
		url: (z, x, y) => "https://tile.openstreetmap.org/" + z + "/" + x + "/" + y + ".png",
		filter: "invert(1) hue-rotate(180deg) brightness(0.9)",
		attr: "© OpenStreetMap contributors" },
	sat: { name: "sat", label: "Satellite", dark: true,
		url: (z, x, y) => "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/" + z + "/" + y + "/" + x,
		attr: "Imagery © Esri, Maxar, Earthstar Geographics" },
	coast: { name: "coast", label: "Coastline (offline)", dark: true, attr: "Coastline: Natural Earth" },
};

// ---- styles (scoped to .adsb-*) --------------------------------------------------------
const css = `
.adsb-wrap { display: flex; flex-direction: column; gap: 10px; height: 100%; }
.adsb-mapwrap { position: relative; flex: 1 1 auto; min-height: 340px; }
.adsb-map { width: 100%; height: 100%; background: #0a1019; border: 1px solid #1c2c40;
	border-radius: 8px; display: block; cursor: crosshair; }
.adsb-ctl { position: absolute; top: 8px; left: 8px; display: flex; gap: 6px; align-items: center;
	background: rgba(8,11,18,0.82); border: 1px solid #1c2c40; border-radius: 7px; padding: 5px 8px;
	backdrop-filter: blur(4px); }
.adsb-ctl input[type=text], .adsb-ctl select { background: #16283f; color: #dae3ec; border: 1px solid #2a4260;
	border-radius: 5px; font: inherit; padding: 2px 7px; }
.adsb-ctl input[type=text] { width: 118px; }
.adsb-attr { position: absolute; bottom: 6px; right: 8px; color: #6f87a0; font-size: 9.5px;
	background: rgba(4,6,12,0.6); padding: 1px 6px; border-radius: 4px; pointer-events: none; }
.adsb-legend { position: absolute; bottom: 8px; left: 8px; display: flex; gap: 9px; align-items: center;
	background: rgba(8,11,18,0.78); border: 1px solid #1c2c40; border-radius: 7px; padding: 4px 9px;
	color: #9ab0c4; font-size: 10px; flex-wrap: wrap; max-width: calc(100% - 120px); }
.adsb-legend .sw { display: inline-block; width: 9px; height: 9px; border-radius: 2px; margin-right: 3px;
	vertical-align: middle; }
.adsb-detail { position: absolute; top: 8px; right: 8px; width: 236px; max-height: calc(100% - 16px);
	overflow-y: auto; background: rgba(8,11,18,0.92); border: 1px solid #2a4260; border-radius: 8px;
	padding: 10px 12px; box-shadow: 0 6px 24px rgba(0,0,0,0.5); }
.adsb-detail h4 { color: #cfe2f2; font-size: 14px; font-weight: normal; margin-bottom: 2px;
	display: flex; align-items: center; gap: 6px; }
.adsb-detail .sub { color: #62798f; font-size: 11px; margin-bottom: 8px; }
.adsb-detail dl { display: grid; grid-template-columns: auto 1fr; gap: 2px 10px; font-size: 11px; }
.adsb-detail dt { color: #62798f; }
.adsb-detail dd { color: #dae3ec; text-align: right; font-variant-numeric: tabular-nums; }
.adsb-detail .cx { position: absolute; top: 7px; right: 9px; color: #9ab0c4; cursor: pointer;
	background: none; border: none; font: inherit; font-size: 15px; line-height: 1; }
.adsb-detail .cx:hover { color: #f43f5e; }
.adsb-tblwrap { flex: none; max-height: 40%; overflow-y: auto; }
.adsb-tbl { border-collapse: collapse; width: 100%; }
.adsb-tbl th { position: sticky; top: 0; background: #0d1725; color: #62798f; text-align: left;
	font-weight: normal; font-size: 10px; text-transform: uppercase; letter-spacing: 1px;
	padding: 3px 10px 3px 0; cursor: pointer; user-select: none; white-space: nowrap; }
.adsb-tbl th.sorted { color: #10b981; }
.adsb-tbl td { color: #dae3ec; padding: 2px 10px 2px 0; border-top: 1px solid #16222f;
	font-variant-numeric: tabular-nums; white-space: nowrap; }
.adsb-tbl tr.row { cursor: pointer; }
.adsb-tbl tr.row:hover td { background: #15243a; }
.adsb-tbl tr.sel td { background: #0d2a23; }
.adsb-tbl td .adot { display: inline-block; width: 7px; height: 7px; border-radius: 2px; margin-right: 5px; }
`;
if(!document.getElementById("adsb-style")) {
	const st = document.createElement("style");
	st.id = "adsb-style";
	st.textContent = css;
	document.head.appendChild(st);
}

// ---- altitude color ramp (aviation-style) ----------------------------------------------
const RAMP = [
	[0, [226, 69, 60]], [3000, [255, 140, 66]], [8000, [255, 212, 121]],
	[16000, [155, 229, 100]], [24000, [55, 214, 122]], [34000, [95, 191, 255]], [45000, [180, 150, 255]],
];
function altColor(alt) {
	if(alt === "ground" || alt === undefined || alt === null) { return [140, 160, 190]; }
	const a = Number(alt);
	if(!isFinite(a)) { return [140, 160, 190]; }
	for(let i = 1; i < RAMP.length; i++) {
		if(a <= RAMP[i][0]) {
			const [a0, c0] = RAMP[i - 1], [a1, c1] = RAMP[i];
			const t = (a - a0) / (a1 - a0);
			return c0.map((v, k) => Math.round(v + (c1[k] - v) * t));
		}
	}
	return RAMP[RAMP.length - 1][1];
}
const rgba = (c, a) => "rgba(" + c[0] + "," + c[1] + "," + c[2] + "," + a + ")";
const LEGEND = [["ground", [140, 160, 190]], ["0", altColor(0)], ["10k", altColor(10000)],
	["20k", altColor(20000)], ["30k", altColor(30000)], ["40k+", altColor(45000)]];

// ---- module state ----------------------------------------------------------------------
const S = {
	center: null, radius: 20, home: null, homeTried: false,
	trails: new Map(), lastMsgs: null, lastT: null, rate: null,
	ac: [], stats: null, now: 0, messages: 0, selected: null, filter: "",
	basemap: "map", tiles: new Map(), tileErr: 0, tileOk: 0, redraw: false,
	sortKey: "dist", sortDir: 1, tblPrint: "", geoBox: null, geoLoading: false, cv: null,
	manual: null, drag: null, dragged: false,   // manual = {lat, lon, z}: user view, null = auto-fit
};

// ---- Web Mercator ----------------------------------------------------------------------
const RAD = Math.PI / 180;
const merc = { x: lon => (lon + 180) / 360, y: lat => (1 - Math.asinh(Math.tan(lat * RAD)) / Math.PI) / 2 };

// fractional zoom so the wanted radius (km) fits half the smaller canvas dimension at center lat
function viewOf(cv) {
	const c = S.manual || S.center;
	if(!c) { return null; }
	const w = cv.clientWidth, h = cv.clientHeight;
	const halfPx = Math.min(w, h) / 2 - 18;
	const mppWant = (S.radius * 1000) / Math.max(halfPx, 1);
	let z = Math.log2(156543.03 * Math.cos(c.lat * RAD) / mppWant);
	z = S.manual ? S.manual.z : Math.max(3, Math.min(12, z));
	const world = 256 * Math.pow(2, z);
	const cwx = merc.x(c.lon) * world, cwy = merc.y(c.lat) * world;
	const cx = w / 2, cy = h / 2;
	return {
		w, h, cx, cy, z, world, c,
		mpp: 156543.03 * Math.cos(c.lat * RAD) / Math.pow(2, z),
		toXY: (lon, lat) => [cx + merc.x(lon) * world - cwx, cy + merc.y(lat) * world - cwy],
	};
}

// ---- manual view: wheel zoom around the pointer, drag pan, double click or Auto = auto-fit --
const unmerc = (fx, fy) => ({
	lon: fx * 360 - 180,
	lat: Math.atan(Math.sinh(Math.PI * (1 - 2 * Math.max(0.001, Math.min(0.999, fy))))) / RAD,
});
// view centre that puts world fraction (fx, fy) at pixel offset (dx, dy) from the centre, at zoom z
function centreFor(fx, fy, dx, dy, z) {
	const world = 256 * Math.pow(2, z);
	return { ...unmerc(fx - dx / world, fy - dy / world), z };
}
function pointerFrac(V, mx, my) {
	return [merc.x(V.c.lon) + (mx - V.cx) / V.world, merc.y(V.c.lat) + (my - V.cy) / V.world];
}
function mapPos(cv, ev) {
	const r = cv.getBoundingClientRect();
	return [ev.clientX - r.left, ev.clientY - r.top];
}
function onWheel(cv, ev) {
	ev.preventDefault();
	const V = viewOf(cv);
	if(!V) { return; }
	const [mx, my] = mapPos(cv, ev);
	const [fx, fy] = pointerFrac(V, mx, my);
	const px = ev.deltaMode === 1 ? ev.deltaY * 16 : ev.deltaY;
	const z = Math.max(3, Math.min(17, V.z - Math.max(-0.6, Math.min(0.6, px * 0.002))));
	S.manual = centreFor(fx, fy, mx - V.cx, my - V.cy, z);
	draw(cv);
}
function onDown(cv, ev) {
	if(ev.button !== 0 || ev.pointerType === "touch") { return; }
	const V = viewOf(cv);
	if(!V) { return; }
	const [mx, my] = mapPos(cv, ev);
	const [fx, fy] = pointerFrac(V, mx, my);
	S.drag = { x: mx, y: my, fx, fy, z: V.z, cx: V.cx, cy: V.cy };
	S.dragged = false;
	cv.setPointerCapture(ev.pointerId);
}
function onMove(cv, ev) {
	const d = S.drag;
	if(!d) { return; }
	const [mx, my] = mapPos(cv, ev);
	if(!S.dragged && Math.hypot(mx - d.x, my - d.y) < 4) { return; }
	S.dragged = true;
	cv.style.cursor = "grabbing";
	S.manual = centreFor(d.fx, d.fy, mx - d.cx, my - d.cy, d.z);
	draw(cv);
}
function onUp(cv) {
	S.drag = null;
	cv.style.cursor = "";
}

// ---- tiles -----------------------------------------------------------------------------
function getTile(set, z, x, y) {
	const key = set.name + "/" + z + "/" + x + "/" + y;
	let img = S.tiles.get(key);
	if(img === undefined) {
		img = new Image();
		img._ok = false; img._err = false;
		img.onload = () => { img._ok = true; S.tileOk++; scheduleRedraw(); };
		img.onerror = () => { img._err = true; S.tileErr++; scheduleRedraw(); };
		img.src = set.url(z, x, y);
		S.tiles.set(key, img);
		if(S.tiles.size > 400) { const k = S.tiles.keys().next().value; S.tiles.delete(k); }   // simple cap
	}
	return img;
}

function scheduleRedraw() {
	if(S.redraw || !S.cv) { return; }
	S.redraw = true;
	requestAnimationFrame(() => { S.redraw = false; if(S.cv) { draw(S.cv); } });
}

// tiles at integer zoom round(view.z), scaled to the fractional zoom. Returns whether any drew.
function drawTiles(g, view, set) {
	const tz = Math.max(0, Math.round(view.z));
	const scale = Math.pow(2, view.z - tz);
	const tworld = 256 * Math.pow(2, tz), nt = Math.pow(2, tz);
	const cwx = merc.x(view.c.lon) * tworld, cwy = merc.y(view.c.lat) * tworld;
	const sx = px => (px - cwx) * scale + view.cx, sy = py => (py - cwy) * scale + view.cy;
	const tx0 = Math.floor((cwx - view.cx / scale) / 256), tx1 = Math.floor((cwx + view.cx / scale) / 256);
	const ty0 = Math.floor((cwy - view.cy / scale) / 256), ty1 = Math.floor((cwy + view.cy / scale) / 256);
	let ok = false;
	if(set.filter) { g.filter = set.filter; }
	for(let tx = tx0; tx <= tx1; tx++) {
		for(let ty = ty0; ty <= ty1; ty++) {
			if(ty < 0 || ty >= nt) { continue; }
			const X = ((tx % nt) + nt) % nt;
			const img = getTile(set, tz, X, ty);
			if(img._ok) {
				const dsz = 256 * scale + 1;
				try { g.drawImage(img, sx(tx * 256), sy(ty * 256), dsz, dsz); ok = true; } catch(e) { /* taint/incomplete */ }
			}
		}
	}
	if(set.filter) { g.filter = "none"; }
	return ok;
}

// ---- coastline (offline fallback) ------------------------------------------------------
function ensureGeo() {
	if(window.__RFNM_GEO || S.geoLoading) { return; }
	S.geoLoading = true;
	const s = document.createElement("script");
	s.src = "geodata.js?v=1";
	s.onload = () => {
		const co = (window.__RFNM_GEO || {}).coast || [];
		S.geoBox = co.map(fl => {
			let x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
			for(let i = 0; i < fl.length; i += 2) {
				if(fl[i] < x0) { x0 = fl[i]; } if(fl[i] > x1) { x1 = fl[i]; }
				if(fl[i + 1] < y0) { y0 = fl[i + 1]; } if(fl[i + 1] > y1) { y1 = fl[i + 1]; }
			}
			return [x0, y0, x1, y1];
		});
		scheduleRedraw();
	};
	document.head.appendChild(s);
}

function drawCoast(g, view) {
	ensureGeo();
	if(!window.__RFNM_GEO || !S.geoBox) { return false; }
	const co = window.__RFNM_GEO.coast;
	const kx = 111.320 * Math.cos(view.c.lat * RAD), ky = 110.574;
	const dlon = (S.radius * 1.6) / kx, dlat = (S.radius * 1.6) / ky;
	const vx0 = view.c.lon - dlon, vx1 = view.c.lon + dlon, vy0 = view.c.lat - dlat, vy1 = view.c.lat + dlat;
	g.strokeStyle = "#2a4260"; g.lineWidth = 1; g.beginPath();
	for(let k = 0; k < co.length; k++) {
		const b = S.geoBox[k];
		if(b[2] < vx0 || b[0] > vx1 || b[3] < vy0 || b[1] > vy1) { continue; }
		const fl = co[k];
		for(let i = 0; i < fl.length; i += 2) {
			const xy = view.toXY(fl[i], fl[i + 1]);
			if(i === 0) { g.moveTo(xy[0], xy[1]); } else { g.lineTo(xy[0], xy[1]); }
		}
	}
	g.stroke();
	return true;
}

// ---- helpers ---------------------------------------------------------------------------
function validPos(a) {
	return typeof a.lat === "number" && typeof a.lon === "number" &&
		Math.abs(a.lat) <= 90 && Math.abs(a.lon) <= 180 && (a.seen_pos === undefined || a.seen_pos < 300);
}
function distBearing(from, a) {
	const kx = 111.320 * Math.cos(from.lat * RAD), ky = 110.574;
	const dx = (a.lon - from.lon) * kx, dy = (a.lat - from.lat) * ky;
	return { dist: Math.hypot(dx, dy), brg: (Math.atan2(dx, dy) * 180 / Math.PI + 360) % 360 };
}

// ---- panel skeleton --------------------------------------------------------------------
function ensurePanel(id) {
	const a = A(), ce = a.ui[id];
	if(ce.panel) { return ce.panel; }
	ce.live.textContent = "";

	const wrap = a.el("div", "adsb-wrap");
	const tiles = a.el("div", "ap-tiles");

	const mapwrap = a.el("div", "adsb-mapwrap");
	const map = document.createElement("canvas");
	map.className = "adsb-map";
	const ctl = a.el("div", "adsb-ctl");
	const search = document.createElement("input");
	search.type = "text";
	search.placeholder = "filter flight / hex";
	search.oninput = () => { S.filter = search.value.trim().toLowerCase(); buildTable(id, true); draw(map); };
	const bmSel = document.createElement("select");
	for(const k of ["map", "sat", "coast"]) { const o = document.createElement("option"); o.value = k; o.textContent = TILESETS[k].label; bmSel.appendChild(o); }
	bmSel.value = S.basemap;
	bmSel.onchange = () => { S.basemap = bmSel.value; draw(map); };
	const fit = document.createElement("button");
	fit.type = "button";
	fit.textContent = "Auto";
	fit.title = "back to the automatic view (or double-click the map)";
	fit.onclick = () => { S.manual = null; draw(map); };
	ctl.append(search, bmSel, fit);
	const attr = a.el("div", "adsb-attr");
	const legend = a.el("div", "adsb-legend");
	legend.append(a.el("span", null, "altitude:"));
	for(const [lbl, c] of LEGEND) {
		const sp = a.el("span");
		const sw = a.el("span", "sw");
		sw.style.background = rgba(c, 1);
		sp.append(sw, document.createTextNode(lbl));
		legend.append(sp);
	}
	const detail = a.el("div", "adsb-detail");
	detail.style.display = "none";
	map.onclick = ev => {
		if(S.dragged) { S.dragged = false; return; }   // the click that ends a drag selects nothing
		hitTest(id, map, ev);
	};
	map.ondblclick = () => { S.manual = null; draw(map); };
	map.addEventListener("wheel", ev => onWheel(map, ev), { passive: false });
	map.addEventListener("pointerdown", ev => onDown(map, ev));
	map.addEventListener("pointermove", ev => onMove(map, ev));
	map.addEventListener("pointerup", () => onUp(map));
	map.addEventListener("pointercancel", () => onUp(map));
	mapwrap.append(map, ctl, legend, attr, detail);

	const tblwrap = a.el("div", "adsb-tblwrap");
	wrap.append(tiles, mapwrap, tblwrap);
	ce.live.append(wrap);
	ce.panel = { tiles, map, detail, tblwrap, attr };
	S.cv = map;
	return ce.panel;
}

// ---- live fetch ------------------------------------------------------------------------
function render(id) {
	const a = A();
	if(a.liveBusy[id]) { return; }
	a.liveBusy[id] = true;
	const p = ensurePanel(id);
	S.cv = p.map;
	const done = () => { a.liveBusy[id] = false; };

	if(!S.homeTried) {
		S.homeTried = true;
		a.fetchJSON("/adsb/receiver.json", undefined, 5000).then(r => {
			if(typeof r.lat === "number" && typeof r.lon === "number") { S.home = { lat: r.lat, lon: r.lon }; }
		}).catch(() => {});
	}

	a.fetchJSON("/adsb/aircraft.json", undefined, 6000).then(d => {
		const ac = d.aircraft || [];
		const now = d.now || (Date.now() / 1000);
		if(S.lastMsgs !== null && d.messages < S.lastMsgs) { S.lastMsgs = S.lastT = null; S.rate = null; }
		if(S.lastMsgs !== null && now > S.lastT) {
			const inst = (d.messages - S.lastMsgs) / (now - S.lastT);
			S.rate = S.rate === null ? inst : 0.7 * S.rate + 0.3 * inst;
		}
		S.lastMsgs = d.messages; S.lastT = now;
		S.ac = ac; S.now = now; S.messages = d.messages || 0;
		updateTiles(p);
		draw(p.map);
		buildTable(id);
	}).catch(() => {}).finally(done);
	a.fetchJSON("/adsb/stats.json", undefined, 6000).then(s => { S.stats = s; }).catch(() => {});
}

function updateTiles(p) {
	const a = A();
	const withPos = S.ac.filter(validPos);
	p.tiles.textContent = "";
	p.tiles.append(
		a.tile(String(S.ac.length), "aircraft"),
		a.tile(String(withPos.length), "with position"),
		a.tile(S.rate === null ? "…" : S.rate.toFixed(1), "msgs/s"),
		a.tile((S.messages || 0).toLocaleString(), "messages"));
	if(S.stats) {
		const l = S.stats.last1min || {}, loc = l.local || {};
		if(loc.signal !== undefined) { p.tiles.append(a.tile(loc.signal.toFixed(1) + " dBFS", "signal")); }
		else if(loc.noise !== undefined) { p.tiles.append(a.tile(loc.noise.toFixed(1) + " dBFS", "noise")); }
		if(loc.samples_dropped !== undefined) { p.tiles.append(a.tile(String(loc.samples_dropped), "dropped", loc.samples_dropped > 0)); }
		if(S.stats.gain_db !== undefined && S.stats.gain_db > -50 && S.stats.gain_db < 100) { p.tiles.append(a.tile(S.stats.gain_db + " dB", "rf gain")); }
	}
	if(S.home) { p.tiles.append(a.tile("home set", "range from")); }
}

// ---- map drawing -----------------------------------------------------------------------
function draw(cv) {
	const dpr = window.devicePixelRatio || 1;
	const w = cv.clientWidth, h = cv.clientHeight;
	if(!w || !h) { return; }
	if(cv.width !== Math.round(w * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr); }
	const g = cv.getContext("2d");
	g.setTransform(dpr, 0, 0, dpr, 0, 0);
	g.clearRect(0, 0, w, h);
	g.font = "10px ui-monospace, monospace";

	const pos = S.ac.filter(validPos);
	if(S.home) { S.center = S.home; }
	else if(pos.length) {
		const lats = pos.map(a => a.lat).sort((x, y) => x - y);
		const lons = pos.map(a => a.lon).sort((x, y) => x - y);
		const med = { lat: lats[lats.length >> 1], lon: lons[lons.length >> 1] };
		if(!S.center) { S.center = med; }
		else { S.center.lat += (med.lat - S.center.lat) * 0.15; S.center.lon += (med.lon - S.center.lon) * 0.15; }
	}
	const V = viewOf(cv);
	if(!V) {
		g.fillStyle = "#3e6188"; g.textAlign = "center";
		g.fillText("waiting for aircraft positions…", w / 2, h / 2);
		g.textAlign = "left";
		return;
	}

	// zoom-to-fit with hysteresis (drives V.z on the next frame)
	let need = 20;
	const stn = S.center || V.c;
	const [sx, sy] = V.toXY(stn.lon, stn.lat);   // rings belong to the station, not to a panned view
	for(const a of pos) { need = Math.max(need, distBearing(stn, a).dist); }
	need *= 1.15;
	S.radius = need > S.radius ? need : S.radius + (need - S.radius) * 0.08;

	// basemap: tiles if online + selected, else coastline
	let drewBase = false;
	const set = TILESETS[S.basemap];
	if(S.basemap !== "coast" && navigator.onLine !== false) { drewBase = drawTiles(g, V, set); }
	let attrTxt = set.attr;
	if(!drewBase) { drewBase = drawCoast(g, V); attrTxt = (S.basemap === "coast" ? "" : "offline · ") + TILESETS.coast.attr; }
	const p = A().ui.adsb && A().ui.adsb.panel;
	if(p && p.attr) { p.attr.textContent = attrTxt; }

	// range rings (true km circles about the station/center)
	let step = 5;
	for(const s of [5, 10, 25, 50, 100, 200, 400]) { step = s; if(S.radius / s <= 4) { break; } }
	g.strokeStyle = "rgba(120,150,190,0.22)"; g.fillStyle = "rgba(150,175,205,0.6)";
	for(let r = step; r <= S.radius; r += step) {
		const rp = r * 1000 / V.mpp;
		g.beginPath(); g.arc(sx, sy, rp, 0, 2 * Math.PI); g.stroke();
		g.fillText(r + " km", sx + 3, sy - rp + 11);
	}
	g.strokeStyle = "rgba(150,175,205,0.5)"; g.beginPath();
	g.moveTo(sx - 6, sy); g.lineTo(sx + 6, sy); g.moveTo(sx, sy - 6); g.lineTo(sx, sy + 6); g.stroke();
	g.fillStyle = "rgba(150,175,205,0.6)"; g.fillText("N", V.cx - 3, 12);
	if(S.home) { g.fillStyle = "#5da8dc"; g.beginPath(); g.arc(sx, sy, 3.5, 0, 2 * Math.PI); g.fill(); }

	// trails (stored in lon/lat, projected here so they survive zoom/pan)
	for(const a of pos) {
		let t = S.trails.get(a.hex);
		if(!t) { t = { pts: [], seen: S.now }; S.trails.set(a.hex, t); }
		t.seen = S.now;
		const last = t.pts[t.pts.length - 1];
		if(!last || Math.abs(last.lon - a.lon) > 0.001 || Math.abs(last.lat - a.lat) > 0.001) {
			t.pts.push({ lon: a.lon, lat: a.lat, alt: a.alt_baro });
			if(t.pts.length > 120) { t.pts.shift(); }
		}
	}
	for(const [hex, t] of S.trails) { if(S.now - t.seen > 300) { S.trails.delete(hex); } }
	for(const t of S.trails.values()) {
		if(t.pts.length < 2) { continue; }
		g.beginPath();
		g.lineWidth = 1.4;
		g.strokeStyle = rgba(altColor(t.pts[t.pts.length - 1].alt), 0.5);
		t.pts.forEach((p2, i) => { const xy = V.toXY(p2.lon, p2.lat); if(i) { g.lineTo(xy[0], xy[1]); } else { g.moveTo(xy[0], xy[1]); } });
		g.stroke();
	}
	g.lineWidth = 1;

	// aircraft
	S._hit = [];
	for(const a of pos) {
		const [x, y] = V.toXY(a.lon, a.lat);
		if(x < -20 || x > w + 20 || y < -20 || y > h + 20) { continue; }
		S._hit.push({ hex: a.hex, x, y });
		const age = a.seen_pos || 0;
		const alpha = age > 30 ? 0.45 : (age > 15 ? 0.72 : 1.0);
		const col = altColor(a.alt_baro);
		const sel = a.hex === S.selected;
		g.save();
		g.translate(x, y);
		g.rotate((a.track || 0) * RAD);
		g.beginPath();
		g.moveTo(0, -7); g.lineTo(5, 6); g.lineTo(0, 3); g.lineTo(-5, 6); g.closePath();
		g.fillStyle = rgba(col, alpha); g.fill();
		g.lineWidth = 0.8; g.strokeStyle = "rgba(0,0,0,0.55)"; g.stroke();
		if(sel) { g.strokeStyle = "#fff"; g.lineWidth = 1.4; g.stroke(); }
		g.restore();
		if(sel) { g.strokeStyle = "rgba(255,255,255,0.55)"; g.beginPath(); g.arc(x, y, 13, 0, 2 * Math.PI); g.stroke(); }
		const label = (a.flight || "").trim() || a.hex;
		g.lineWidth = 3; g.strokeStyle = "rgba(4,6,12,0.7)"; g.strokeText(label, x + 9, y + 3);
		g.fillStyle = sel ? "#fff" : rgba([210, 222, 240], alpha); g.fillText(label, x + 9, y + 3);
	}
	g.lineWidth = 1;
}

// ---- interaction -----------------------------------------------------------------------
function hitTest(id, cv, ev) {
	if(!S._hit) { return; }
	const rect = cv.getBoundingClientRect();
	const mx = ev.clientX - rect.left, my = ev.clientY - rect.top;
	let best = null, bd = 18 * 18;
	for(const h of S._hit) {
		const d = (h.x - mx) ** 2 + (h.y - my) ** 2;
		if(d < bd) { bd = d; best = h.hex; }
	}
	S.selected = best;
	draw(cv);
	buildTable(id, true);
	updateDetail(id);
}

function fld(dl, k, v) {
	if(v === undefined || v === null || v === "") { return; }
	const a = A();
	dl.append(a.el("dt", null, k), a.el("dd", null, String(v)));
}

function updateDetail(id) {
	const a = A(), p = a.ui[id].panel;
	if(!p) { return; }
	const d = p.detail;
	const ac = S.ac.find(x => x.hex === S.selected);
	if(!ac) { d.style.display = "none"; return; }
	d.style.display = "block";
	d.textContent = "";
	const close = a.el("button", "cx", "×");
	close.onclick = () => { S.selected = null; d.style.display = "none"; draw(p.map); buildTable(id, true); };
	const h = a.el("h4");
	const sw = a.el("span", "sw");
	sw.style.cssText = "width:10px;height:10px;border-radius:2px;background:" + rgba(altColor(ac.alt_baro), 1);
	h.append(sw, document.createTextNode((ac.flight || "").trim() || ac.hex));
	const sub = a.el("div", "sub", [ac.hex, ac.r, ac.t].filter(Boolean).join(" · "));
	const dl = a.el("dl");
	const homeRef = S.home || S.center;
	if(homeRef && validPos(ac)) {
		const db = distBearing(homeRef, ac);
		fld(dl, "distance", db.dist.toFixed(1) + " km");
		fld(dl, "bearing", db.brg.toFixed(0) + "°");
	}
	fld(dl, "altitude", ac.alt_baro === "ground" ? "ground" : (ac.alt_baro !== undefined ? ac.alt_baro + " ft" : undefined));
	fld(dl, "geom alt", ac.alt_geom !== undefined ? ac.alt_geom + " ft" : undefined);
	fld(dl, "vert rate", ac.baro_rate !== undefined ? ac.baro_rate + " fpm" : undefined);
	fld(dl, "ground spd", ac.gs !== undefined ? Math.round(ac.gs) + " kt" : undefined);
	fld(dl, "IAS", ac.ias !== undefined ? ac.ias + " kt" : undefined);
	fld(dl, "TAS", ac.tas !== undefined ? ac.tas + " kt" : undefined);
	fld(dl, "track", ac.track !== undefined ? Math.round(ac.track) + "°" : undefined);
	fld(dl, "heading", ac.mag_heading !== undefined ? Math.round(ac.mag_heading) + "°" : undefined);
	fld(dl, "squawk", ac.squawk);
	fld(dl, "category", ac.category);
	fld(dl, "emergency", ac.emergency && ac.emergency !== "none" ? ac.emergency : undefined);
	fld(dl, "nav alt", ac.nav_altitude_mcp !== undefined ? ac.nav_altitude_mcp + " ft" : undefined);
	fld(dl, "position", validPos(ac) ? ac.lat.toFixed(4) + ", " + ac.lon.toFixed(4) : undefined);
	fld(dl, "NIC / RC", ac.nic !== undefined ? ac.nic + " / " + (ac.rc !== undefined ? ac.rc + " m" : "?") : undefined);
	fld(dl, "RSSI", ac.rssi !== undefined ? ac.rssi.toFixed(1) + " dBFS" : undefined);
	fld(dl, "messages", ac.messages);
	fld(dl, "seen", ac.seen !== undefined ? ac.seen.toFixed(0) + " s ago" : undefined);
	d.append(close, h, sub, dl);
}

// ---- table (sortable + filterable) -----------------------------------------------------
const COLS = [
	{ k: "flight", h: "flight", get: a => (a.flight || "").trim() || a.hex },
	{ k: "alt", h: "alt", get: a => a.alt_baro === "ground" ? -1 : (a.alt_baro ?? null), fmt: a => a.alt_baro === "ground" ? "gnd" : (a.alt_baro !== undefined ? a.alt_baro : "") },
	{ k: "spd", h: "spd", get: a => a.gs ?? null, fmt: a => a.gs !== undefined ? Math.round(a.gs) : "" },
	{ k: "trk", h: "trk", get: a => a.track ?? null, fmt: a => a.track !== undefined ? Math.round(a.track) + "°" : "" },
	{ k: "dist", h: "dist", get: a => a._dist ?? null, fmt: a => a._dist !== undefined ? a._dist.toFixed(0) : "" },
	{ k: "rssi", h: "rssi", get: a => a.rssi ?? null, fmt: a => a.rssi !== undefined ? a.rssi.toFixed(1) : "" },
	{ k: "seen", h: "seen", get: a => a.seen ?? 999, fmt: a => a.seen !== undefined ? a.seen.toFixed(0) + "s" : "" },
];

function buildTable(id, force) {
	const a = A(), ce = a.ui[id];
	if(!ce.panel) { return; }
	const p = ce.panel;
	const homeRef = S.home || S.center;
	const rows = S.ac.slice();
	for(const r of rows) { r._dist = (homeRef && validPos(r)) ? distBearing(homeRef, r).dist : undefined; }
	let view = rows;
	if(S.filter) { view = rows.filter(r => ((r.flight || "") + " " + r.hex).toLowerCase().includes(S.filter)); }
	const col = COLS.find(c => c.k === S.sortKey) || COLS[4];
	view.sort((x, y) => {
		let vx = col.get(x), vy = col.get(y);
		if(vx === null || vx === undefined) { vx = S.sortDir > 0 ? Infinity : -Infinity; }
		if(vy === null || vy === undefined) { vy = S.sortDir > 0 ? Infinity : -Infinity; }
		if(typeof vx === "string") { return S.sortDir * vx.localeCompare(vy); }
		return S.sortDir * (vx - vy);
	});
	view = view.slice(0, 200);

	const print = S.sortKey + S.sortDir + "|" + S.filter + "|" + S.selected + "|" +
		JSON.stringify(view.map(r => [r.hex, col.fmt ? col.fmt(r) : col.get(r), r.seen]));
	if(!force && (print === S.tblPrint || a.selectionInside(p.tblwrap))) { return; }
	S.tblPrint = print;

	const t = a.el("table", "adsb-tbl");
	const tr0 = a.el("tr");
	for(const c of COLS) {
		const th = a.el("th", c.k === S.sortKey ? "sorted" : null, c.h + (c.k === S.sortKey ? (S.sortDir > 0 ? " ▲" : " ▼") : ""));
		th.onclick = () => {
			if(S.sortKey === c.k) { S.sortDir *= -1; } else { S.sortKey = c.k; S.sortDir = (c.k === "flight" || c.k === "seen" || c.k === "dist") ? 1 : -1; }
			buildTable(id, true);
		};
		tr0.appendChild(th);
	}
	t.appendChild(tr0);
	for(const r of view) {
		const row = a.el("tr", "row" + (r.hex === S.selected ? " sel" : ""));
		const first = a.el("td");
		const dot = a.el("span", "adot");
		dot.style.background = rgba(altColor(r.alt_baro), 1);
		first.append(dot, document.createTextNode((r.flight || "").trim() || r.hex));
		row.appendChild(first);
		for(let i = 1; i < COLS.length; i++) { row.appendChild(a.el("td", null, COLS[i].fmt ? COLS[i].fmt(r) : (COLS[i].get(r) ?? ""))); }
		row.onclick = () => { S.selected = S.selected === r.hex ? null : r.hex; draw(p.map); buildTable(id, true); updateDetail(id); };
		t.appendChild(row);
	}
	p.tblwrap.textContent = "";
	p.tblwrap.appendChild(t);
	if(rows.length > view.length) { p.tblwrap.appendChild(a.el("div", "ap-hint", "+" + (rows.length - view.length) + " more (filter to narrow)")); }
	updateDetail(id);
}

})();
