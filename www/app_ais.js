// AIS visualizer (app_ais.js): full-screen live ship map + roster for aisd.
// Self-registers into the APPS view. Reads /api/app/ais/status and /api/app/ais/messages (served by
// nginx from aisd on 127.0.0.1:7686). Marine AIS on channels A/B (161.975 / 162.025 MHz).
//
// Basemap is progressive, exactly like the ADS-B map: with internet it loads real
// Web-Mercator tiles (Carto dark or Esri satellite); offline it falls back to a self-hosted
// Natural Earth vector coastline (geodata.js, lazy-loaded) on the same <canvas>. Vessels,
// trails and range rings are all projected in Web Mercator so everything lines up. There is
// no receiver position for AIS, so the view auto-centres on the fleet median.
//
// Data contract (/api/app/ais/messages):
//   { total, messages:[ { time:"HH:MM:SS", ch:"A"|"B", type:int, mmsi:u32, level:dBFS,
//                         // structured decode (aisd ais_parse) — present-only:
//                         lat, lon, sog, cog, hdg, shiptype, nav_status, name, callsign,
//                         destination,
//                         info:string, nmea:string } ] }
// The structured keys are consumed directly; if a field is absent (e.g. an aisd predating
// the structured-JSON change) we fall back to regex-parsing the legacy `info` string, so
// this GUI works against both.
"use strict";
(function() {

// ---- module state (owned by this file; reset when the panel is (re)built) --------------
const S = {
	ships: new Map(),        // mmsi -> merged record (static + position + trail)
	center: null,            // {lat,lon} smoothed map centre (fleet median)
	radiusKm: 3,             // hysteresis zoom radius (km); rings are labelled in nm
	manual: null, drag: null, dragged: false,   // manual = {lat, lon, z}: user view
	status: null,            // last /status payload
	lastTotal: null, lastT: null, rate: null,   // /status.total -> smoothed msgs/s
	lastMsgTotal: null,      // /messages.total, watched for a daemon restart
	messages: [],            // last /messages ring (feed + type breakdown)
	selected: null,          // selected mmsi
	filter: "",
	basemap: "map",
	tiles: new Map(), tileOk: 0, tileErr: 0, redraw: false,
	sortKey: "age", sortDir: 1,
	tblPrint: "", chanPrint: "", feedPrint: "", typePrint: "", feedText: "",
	geoBox: null, geoLoading: false, cv: null, panelRef: null,
};

const PLOT_AGE = 900;      // s: a fix older than this drops off the map
const ROSTER_AGE = 1800;   // s: a ship older than this drops off the roster
const PRUNE_AGE = 3600;    // s: forget the ship entirely (bounds memory)
const TRAIL_MAX = 64;      // trail points kept per ship
const RAD = Math.PI / 180;
const POS_TYPES = new Set([1, 2, 3, 4, 9, 18, 19, 21]);   // message types that carry a fix

// ---- basemap tile sources (loaded by the viewer's browser; https on an http page is OK) --
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

// ---- class colour / kind / ship-type -----------------------------------------------------
function colorFor(type) {
	if(type === 1 || type === 2 || type === 3 || type === 5) { return { css: "#10b981", rgb: "55,214,122" }; }   // Class A
	if(type === 18 || type === 19 || type === 24) { return { css: "#5da8dc", rgb: "96,190,255" }; }              // Class B
	if(type === 4) { return { css: "#f0be5a", rgb: "240,190,90" }; }                                             // base station
	if(type === 21) { return { css: "#c58cff", rgb: "197,140,255" }; }                                           // aid to navigation
	if(type === 9) { return { css: "#ff85b3", rgb: "255,133,179" }; }                                            // SAR aircraft
	return { css: "#96a5be", rgb: "150,165,190" };                                                               // other
}

function kindOf(type) {
	if(type === 1 || type === 2 || type === 3) { return "Class A"; }
	if(type === 5) { return "Class A stat"; }
	if(type === 18) { return "Class B"; }
	if(type === 19) { return "Class B+"; }
	if(type === 24) { return "Class B stat"; }
	if(type === 4) { return "Base stn"; }
	if(type === 21) { return "AtoN"; }
	if(type === 9) { return "SAR air"; }
	return "type " + type;
}

// AIS ship-and-cargo type code (0..99) -> short label
function shipTypeLabel(code) {
	if(!code) { return null; }
	if(code >= 20 && code <= 29) { return "WIG"; }
	if(code === 30) { return "Fishing"; }
	if(code === 31 || code === 32) { return "Towing"; }
	if(code === 33) { return "Dredger"; }
	if(code === 34) { return "Dive vessel"; }
	if(code === 35) { return "Military"; }
	if(code === 36) { return "Sailing"; }
	if(code === 37) { return "Pleasure craft"; }
	if(code >= 40 && code <= 49) { return "High-speed craft"; }
	if(code === 50) { return "Pilot"; }
	if(code === 51) { return "Search & rescue"; }
	if(code === 52) { return "Tug"; }
	if(code === 53) { return "Port tender"; }
	if(code === 54) { return "Anti-pollution"; }
	if(code === 55) { return "Law enforcement"; }
	if(code === 58) { return "Medical transport"; }
	if(code >= 60 && code <= 69) { return "Passenger"; }
	if(code >= 70 && code <= 79) { return "Cargo"; }
	if(code >= 80 && code <= 89) { return "Tanker"; }
	if(code >= 90 && code <= 99) { return "Other"; }
	return "type " + code;
}

// ---- field helpers -----------------------------------------------------------------------
// legacy fallback: parse the free-text info string (pre-structured-JSON aisd)
function parseInfo(info) {
	const o = {};
	if(!info) { return o; }
	let m;
	const qre = /(name|call|dest)="([^"]*)"/g;
	while((m = qre.exec(info)) !== null) { if(m[2]) { o[m[1]] = m[2]; } }
	m = /pos=(-?\d+\.\d+),(-?\d+\.\d+)/.exec(info);
	if(m) { o.lat = parseFloat(m[1]); o.lon = parseFloat(m[2]); }
	m = /sog=(-?\d+(?:\.\d+)?)kn/.exec(info);
	if(m) { o.sog = parseFloat(m[1]); }
	m = /cog=(-?\d+(?:\.\d+)?)/.exec(info);
	if(m) { o.cog = parseFloat(m[1]); }
	m = /hdg=(\d+)/.exec(info);
	if(m) { o.hdg = parseInt(m[1], 10); }
	m = /status=([a-z][a-z-]*)/.exec(info);
	if(m) { o.status = m[1]; }
	return o;
}

// prefer structured keys; fall back to the parsed info string per field
function fieldsOf(m) {
	const p = parseInfo(m.info);
	const num = (a, b) => (typeof a === "number" && isFinite(a)) ? a : ((typeof b === "number" && isFinite(b)) ? b : null);
	return {
		lat: num(m.lat, p.lat),
		lon: num(m.lon, p.lon),
		sog: num(m.sog, p.sog),
		cog: num(m.cog, p.cog),
		hdg: num(m.hdg, typeof p.hdg === "number" ? p.hdg : null),
		name: m.name || p.name || null,
		callsign: m.callsign || p.call || null,
		dest: m.destination || p.dest || null,
		nav: m.nav_status || p.status || null,
		shiptype: (typeof m.shiptype === "number" && m.shiptype > 0) ? m.shiptype : null,
	};
}

function hasPos(o) {
	return typeof o.lat === "number" && typeof o.lon === "number" && isFinite(o.lat) && isFinite(o.lon) &&
		Math.abs(o.lat) <= 90 && Math.abs(o.lon) <= 180 && !(o.lat === 0 && o.lon === 0);
}

function courseOf(s) {
	if(typeof s.cog === "number" && s.cog >= 0 && s.cog < 360) { return s.cog; }
	if(typeof s.hdg === "number" && s.hdg >= 0 && s.hdg < 360) { return s.hdg; }
	return null;
}

function fmtAge(sec) {
	if(sec === null || sec === undefined) { return "—"; }
	if(sec < 60) { return Math.round(sec) + "s"; }
	if(sec < 3600) { return Math.floor(sec / 60) + "m"; }
	return Math.floor(sec / 3600) + "h" + Math.floor((sec % 3600) / 60) + "m";
}

function distKm(from, ll) {
	const kx = 111.320 * Math.cos(from.lat * RAD), ky = 110.574;
	const dx = (ll.lon - from.lon) * kx, dy = (ll.lat - from.lat) * ky;
	return Math.hypot(dx, dy);
}

// ---- Web Mercator (shared with the ADS-B map) --------------------------------------------
const merc = { x: lon => (lon + 180) / 360, y: lat => (1 - Math.asinh(Math.tan(lat * RAD)) / Math.PI) / 2 };

// fractional zoom so the wanted radius (km) fits half the smaller canvas dim at centre lat
function viewOf(cv) {
	const c = S.manual || S.center;
	if(!c) { return null; }
	const w = cv.clientWidth, h = cv.clientHeight;
	const halfPx = Math.min(w, h) / 2 - 18;
	const mppWant = (S.radiusKm * 1000) / Math.max(halfPx, 1);
	let z = Math.log2(156543.03 * Math.cos(c.lat * RAD) / mppWant);
	z = S.manual ? S.manual.z : Math.max(3, Math.min(15, z));   // harbours are tight, allow deeper zoom than ADS-B
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
function zoomAt(cv, V, ev) {
	const [mx, my] = mapPos(cv, ev);
	const [fx, fy] = pointerFrac(V, mx, my);
	const px = ev.deltaMode === 1 ? ev.deltaY * 16 : ev.deltaY;
	const z = Math.max(3, Math.min(17, V.z - Math.max(-0.6, Math.min(0.6, px * 0.002))));
	S.manual = centreFor(fx, fy, mx - V.cx, my - V.cy, z);
}
function dragStart(cv, V, ev) {
	const [mx, my] = mapPos(cv, ev);
	const [fx, fy] = pointerFrac(V, mx, my);
	S.drag = { x: mx, y: my, fx, fy, z: V.z, cx: V.cx, cy: V.cy };
	S.dragged = false;
	cv.setPointerCapture(ev.pointerId);
}
function dragMove(cv, ev) {
	const d = S.drag;
	if(!d) { return false; }
	const [mx, my] = mapPos(cv, ev);
	if(!S.dragged && Math.hypot(mx - d.x, my - d.y) < 4) { return false; }
	S.dragged = true;
	cv.style.cursor = "grabbing";
	S.manual = centreFor(d.fx, d.fy, mx - d.cx, my - d.cy, d.z);
	return true;
}
function dragEnd(cv) {
	S.drag = null;
	cv.style.cursor = "";
}

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
		if(S.tiles.size > 400) { const k = S.tiles.keys().next().value; S.tiles.delete(k); }
	}
	return img;
}

function scheduleRedraw() {
	if(S.redraw || !S.cv) { return; }
	S.redraw = true;
	requestAnimationFrame(() => { S.redraw = false; if(S.cv) { drawShips(S.cv); } });
}

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
				try { g.drawImage(img, sx(tx * 256), sy(ty * 256), dsz, dsz); ok = true; } catch(e) { /* incomplete */ }
			}
		}
	}
	if(set.filter) { g.filter = "none"; }
	return ok;
}

// ---- coastline (offline fallback) --------------------------------------------------------
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
	const dlon = (S.radiusKm * 1.6) / kx, dlat = (S.radiusKm * 1.6) / ky;
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

// ---- style (scoped to .ais-*) ------------------------------------------------------------
function ensureStyle() {
	if(document.getElementById("ais-style")) { return; }
	const st = document.createElement("style");
	st.id = "ais-style";
	st.textContent = `
.ais-wrap { display: flex; flex-direction: column; gap: 10px; }
.ais-mapwrap { position: relative; height: min(56vh, 540px); min-height: 360px; }
.ais-map { width: 100%; height: 100%; background: #0a1019; border: 1px solid #1c2c40; border-radius: 8px; display: block; cursor: crosshair; }
.ais-ctl { position: absolute; top: 8px; left: 8px; display: flex; gap: 6px; align-items: center;
	background: rgba(8,11,18,0.82); border: 1px solid #1c2c40; border-radius: 7px; padding: 5px 8px; backdrop-filter: blur(4px); }
.ais-ctl input[type=text], .ais-ctl select { background: #16283f; color: #dae3ec; border: 1px solid #2a4260; border-radius: 5px; font: inherit; padding: 2px 7px; }
.ais-ctl input[type=text] { width: 122px; }
.ais-attr { position: absolute; bottom: 6px; right: 8px; color: #6f87a0; font-size: 9.5px; background: rgba(4,6,12,0.6); padding: 1px 6px; border-radius: 4px; pointer-events: none; }
.ais-legend { position: absolute; bottom: 8px; left: 8px; display: flex; gap: 9px; align-items: center;
	background: rgba(8,11,18,0.78); border: 1px solid #1c2c40; border-radius: 7px; padding: 4px 9px; color: #9ab0c4; font-size: 10px; flex-wrap: wrap; max-width: calc(100% - 130px); }
.ais-legend .sw { display: inline-block; width: 9px; height: 9px; border-radius: 2px; margin-right: 3px; vertical-align: middle; }
.ais-detail { position: absolute; top: 8px; right: 8px; width: 244px; max-height: calc(100% - 16px); overflow-y: auto;
	background: rgba(8,11,18,0.94); border: 1px solid #2a4260; border-radius: 8px; padding: 10px 12px; box-shadow: 0 6px 24px rgba(0,0,0,0.5); }
.ais-detail h4 { color: #cfe2f2; font-size: 14px; font-weight: normal; margin-bottom: 2px; display: flex; align-items: center; gap: 6px; }
.ais-detail .sub { color: #62798f; font-size: 11px; margin-bottom: 8px; }
.ais-detail dl { display: grid; grid-template-columns: auto 1fr; gap: 2px 10px; font-size: 11px; }
.ais-detail dt { color: #62798f; }
.ais-detail dd { color: #dae3ec; text-align: right; font-variant-numeric: tabular-nums; word-break: break-word; }
.ais-detail .cx { position: absolute; top: 7px; right: 9px; color: #9ab0c4; cursor: pointer; background: none; border: none; font: inherit; font-size: 15px; line-height: 1; }
.ais-detail .cx:hover { color: #f43f5e; }
.ais-lower { display: flex; gap: 14px; align-items: flex-start; flex-wrap: wrap; }
.ais-tblcol { flex: 1 1 520px; min-width: 0; }
.ais-sidecol { flex: 1 1 320px; min-width: 280px; display: flex; flex-direction: column; gap: 8px; }
.ais-secthead { color: #62798f; font-size: 10px; text-transform: uppercase; letter-spacing: 1px; margin: 0 0 5px; display: flex; align-items: center; gap: 8px; }
.ais-tblscroll { max-height: 320px; overflow-y: auto; }
.ais-tbl { border-collapse: collapse; width: 100%; }
.ais-tbl th { position: sticky; top: 0; background: #0d1725; color: #62798f; text-align: left; font-weight: normal; font-size: 10px;
	text-transform: uppercase; letter-spacing: 1px; padding: 3px 10px 3px 0; cursor: pointer; user-select: none; white-space: nowrap; }
.ais-tbl th.sorted { color: #10b981; }
.ais-tbl td { color: #dae3ec; padding: 2px 10px 2px 0; border-top: 1px solid #16222f; font-variant-numeric: tabular-nums; white-space: nowrap; }
.ais-tbl tr.row { cursor: pointer; }
.ais-tbl tr.row:hover td { background: #15243a; }
.ais-tbl tr.sel td { background: #0d2a23; }
.ais-tbl td .sdot { display: inline-block; width: 7px; height: 7px; border-radius: 2px; margin-right: 5px; }
.ais-tbl td.mut { color: #4a5a72; }
.ais-types { display: flex; flex-wrap: wrap; gap: 6px; }
.ais-tchip { background: #15243a; border: 1px solid #1c2c40; border-radius: 6px; padding: 3px 9px; font-size: 11px; color: #9ab0c4; display: flex; align-items: center; gap: 5px; }
.ais-tchip .sw { width: 8px; height: 8px; border-radius: 2px; }
.ais-tchip .n { color: #dae3ec; font-variant-numeric: tabular-nums; }
.ais-feed { background: #0a1019; border: 1px solid #1c2c40; border-radius: 8px; height: 300px; overflow-y: auto; padding: 8px 10px;
	font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 11px; line-height: 1.5; }
.ais-feed .ln { color: #9cb2c8; white-space: pre-wrap; word-break: break-all; }
.ais-feed .ln .t { color: #48597a; }
.ais-copy { margin-left: auto; background: #16283f; color: #9ab0c4; border: 1px solid #2a4260; border-radius: 5px; font: inherit; font-size: 10px; padding: 1px 9px; cursor: pointer; }
.ais-copy:hover { color: #dae3ec; }
.ais-mut { color: #4a5a72; }
`;
	document.head.appendChild(st);
}

// ---- panel skeleton (built once; cached on ce.panel) -------------------------------------
function ensurePanel(id) {
	const A = window.RFNM_APPS, ce = A.ui[id];
	if(ce.panel) { return ce.panel; }
	ensureStyle();
	ce.live.textContent = "";

	const wrap = A.el("div", "ais-wrap");
	const tiles = A.el("div", "ap-tiles");
	const chans = A.el("div", "ap-chanstrip");

	const mapwrap = A.el("div", "ais-mapwrap");
	const map = document.createElement("canvas");
	map.className = "ais-map";
	const ctl = A.el("div", "ais-ctl");
	const search = document.createElement("input");
	search.type = "text";
	search.placeholder = "filter name / MMSI";
	search.oninput = () => { S.filter = search.value.trim().toLowerCase(); buildTable(id, true); };
	const bmSel = document.createElement("select");
	for(const k of ["map", "sat", "coast"]) { const o = document.createElement("option"); o.value = k; o.textContent = TILESETS[k].label; bmSel.appendChild(o); }
	bmSel.value = S.basemap;
	bmSel.onchange = () => { S.basemap = bmSel.value; drawShips(map); };
	const fit = document.createElement("button");
	fit.type = "button";
	fit.textContent = "Auto";
	fit.title = "back to the automatic view (or double-click the map)";
	fit.onclick = () => { S.manual = null; drawShips(map); };
	ctl.append(search, bmSel, fit);
	const attr = A.el("div", "ais-attr");
	const legend = A.el("div", "ais-legend");
	const detail = A.el("div", "ais-detail");
	detail.style.display = "none";
	map.onclick = ev => {
		if(S.dragged) { S.dragged = false; return; }   // the click that ends a drag selects nothing
		hitTest(id, map, ev);
	};
	map.ondblclick = () => { S.manual = null; drawShips(map); };
	map.addEventListener("wheel", ev => {
		const V = viewOf(map);
		if(!V) { return; }
		ev.preventDefault();
		zoomAt(map, V, ev);
		drawShips(map);
	}, { passive: false });
	map.addEventListener("pointerdown", ev => {
		const V = viewOf(map);
		if(V && ev.button === 0 && ev.pointerType !== "touch") { dragStart(map, V, ev); }
	});
	map.addEventListener("pointermove", ev => { if(dragMove(map, ev)) { drawShips(map); } });
	map.addEventListener("pointerup", () => dragEnd(map));
	map.addEventListener("pointercancel", () => dragEnd(map));
	mapwrap.append(map, ctl, legend, attr, detail);

	const lower = A.el("div", "ais-lower");
	const tblcol = A.el("div", "ais-tblcol");
	tblcol.appendChild(A.el("div", "ais-secthead", "vessels"));
	const tblscroll = A.el("div", "ais-tblscroll");
	const tblWrap = A.el("div");
	tblscroll.appendChild(tblWrap);
	tblcol.appendChild(tblscroll);

	const sidecol = A.el("div", "ais-sidecol");
	sidecol.appendChild(A.el("div", "ais-secthead", "message types"));
	const types = A.el("div", "ais-types");
	const feedhead = A.el("div", "ais-secthead", "raw AIVDM feed");
	const copyBtn = A.el("button", "ais-copy", "copy");
	feedhead.appendChild(copyBtn);
	const feed = A.el("div", "ais-feed");
	copyBtn.onclick = () => {
		const txt = S.feedText || "";
		if(navigator.clipboard && navigator.clipboard.writeText) {
			navigator.clipboard.writeText(txt).then(() => { copyBtn.textContent = "copied"; setTimeout(() => { copyBtn.textContent = "copy"; }, 1200); }).catch(() => {});
		}
	};
	sidecol.append(types, feedhead, feed);

	lower.append(tblcol, sidecol);
	const hint = A.el("div", "ap-hint");
	wrap.append(tiles, chans, mapwrap, lower, hint);
	ce.live.append(wrap);

	// fresh panel -> fresh state
	S.ships.clear();
	S.center = null; S.radiusKm = 3; S.status = null;
	S.lastTotal = S.lastT = S.rate = null; S.lastMsgTotal = null;
	S.messages = []; S.selected = null; S.filter = "";
	S.tblPrint = S.chanPrint = S.feedPrint = S.typePrint = ""; S.feedText = "";
	S.tiles = new Map(); S.geoBox = null;

	ce.panel = { tiles, chans, map, legend, attr, detail, tblWrap, types, feed, copyBtn, hint };
	S.panelRef = ce.panel; S.cv = map;
	return ce.panel;
}

// ---- roster merge (idempotent: trails rebuilt from the ring each poll) --------------------
function newShip(mmsi) {
	return {
		mmsi, name: null, callsign: null, dest: null, navStatus: null,
		lat: null, lon: null, sog: null, cog: null, hdg: null, shiptype: 0,
		classType: null, lastType: 0, chan: null, level: null,
		lastSeen: Date.now(), lastKey: null, newKey: null, posSeen: null, trail: [],
	};
}

function updateShips(msgs) {
	for(const [, sh] of S.ships) { sh._nt = []; }
	// oldest -> newest so the newest field values win and the trail runs forward
	for(const m of msgs) {
		const mmsi = m.mmsi;
		if(!mmsi) { continue; }
		let sh = S.ships.get(mmsi);
		if(!sh) { sh = newShip(mmsi); sh._nt = []; S.ships.set(mmsi, sh); }
		const f = fieldsOf(m);
		sh.lastType = m.type;
		sh.chan = m.ch;
		if(POS_TYPES.has(m.type)) { sh.classType = m.type; }
		if(typeof m.level === "number") { sh.level = m.level; }
		if(f.name) { sh.name = f.name; }
		if(f.callsign) { sh.callsign = f.callsign; }
		if(f.dest) { sh.dest = f.dest; }
		if(f.nav) { sh.navStatus = f.nav; }
		if(f.shiptype) { sh.shiptype = f.shiptype; }
		if(typeof f.sog === "number") { sh.sog = f.sog; }
		if(typeof f.cog === "number") { sh.cog = f.cog; }
		if(typeof f.hdg === "number") { sh.hdg = f.hdg; }
		if(hasPos(f)) {
			sh.lat = f.lat; sh.lon = f.lon; sh.posSeen = Date.now();
			const last = sh._nt[sh._nt.length - 1];
			if(!last || Math.abs(last.lat - f.lat) > 1e-5 || Math.abs(last.lon - f.lon) > 1e-5) { sh._nt.push({ lat: f.lat, lon: f.lon }); }
		}
		sh.newKey = m.time + "|" + m.type + "|" + (m.info || "");
		sh._seen = true;
	}
	const nowMs = Date.now();
	for(const [mmsi, sh] of S.ships) {
		if(sh._nt && sh._nt.length) { sh.trail = sh._nt.slice(-TRAIL_MAX); }
		if(sh._seen && sh.newKey !== sh.lastKey) { sh.lastKey = sh.newKey; sh.lastSeen = nowMs; }
		if((nowMs - sh.lastSeen) / 1000 > PRUNE_AGE) { S.ships.delete(mmsi); }
		delete sh._nt; delete sh._seen;
	}
}

// ---- live render (polled ~2s) ------------------------------------------------------------
function render(id) {
	const A = window.RFNM_APPS;
	if(A.liveBusy[id]) { return; }
	A.liveBusy[id] = true;
	const p = ensurePanel(id);
	S.cv = p.map;

	A.fetchJSON("/api/app/ais/status", undefined, 6000).then(st => {
		S.status = st;
		const t = performance.now() / 1000;
		if(S.lastTotal !== null && st.total < S.lastTotal) { S.lastTotal = S.lastT = S.rate = null; }   // restart
		if(S.lastTotal !== null && t > S.lastT) {
			const inst = (st.total - S.lastTotal) / (t - S.lastT);
			S.rate = S.rate === null ? inst : 0.7 * S.rate + 0.3 * inst;
		}
		S.lastTotal = st.total; S.lastT = t;
		updateChans(p, st);
	}).catch(() => {});

	A.fetchJSON("/api/app/ais/messages", undefined, 6000).then(d => {
		const msgs = Array.isArray(d.messages) ? d.messages : (Array.isArray(d) ? d : []);
		const total = typeof d.total === "number" ? d.total : msgs.length;
		if(S.lastMsgTotal !== null && total < S.lastMsgTotal) {   // daemon restarted: drop roster
			S.ships.clear(); S.center = null; S.radiusKm = 3;
		}
		S.lastMsgTotal = total;
		S.messages = msgs;
		updateShips(msgs);
		updateTiles(p, total);
		drawShips(p.map);
		updateLegend(p);
		buildTable(id);
		buildTypes(p);
		buildFeed(id);
		const nowMs = Date.now();
		const tracked = [...S.ships.values()].filter(s => (nowMs - s.lastSeen) / 1000 <= ROSTER_AGE).length;
		p.hint.textContent = tracked ? "" :
			"listening… no AIS bursts decoded yet (needs a VHF marine antenna with a water/sky view — indoor reception is luck; the decode chain is proven by aisd --selftest)";
	}).catch(() => {}).finally(() => { A.liveBusy[id] = false; });
}

function updateTiles(p, total) {
	const A = window.RFNM_APPS, st = S.status || {};
	const nowMs = Date.now();
	const all = [...S.ships.values()].filter(s => (nowMs - s.lastSeen) / 1000 <= ROSTER_AGE);
	const plotted = all.filter(s => hasPos(s) && (nowMs - s.lastSeen) / 1000 <= PLOT_AGE);
	p.tiles.textContent = "";
	p.tiles.append(
		A.tile(st.center ? (st.center / 1e6).toFixed(3) + " MHz" : "—", "center"),
		A.tile((total || 0).toLocaleString(), "messages"),
		A.tile(S.rate === null ? "…" : S.rate.toFixed(1), "msgs/s"),
		A.tile(String(plotted.length), "ships plotted"),
		A.tile(String(all.length), "ships tracked"),
		A.tile(A.fmtUp(st.uptime), "uptime"));
	if((st.queue_drops || 0) > 0) { p.tiles.append(A.tile(String(st.queue_drops), "drops", true)); }
}

function updateChans(p, st) {
	const A = window.RFNM_APPS;
	const print = JSON.stringify(st.chans || []);
	if(print === S.chanPrint) { return; }
	S.chanPrint = print;
	p.chans.textContent = "";
	for(const ch of st.chans || []) {
		const cls = ch.messages > 0 ? " hot" : (ch.drops > 0 ? " drop" : "");
		const box = A.el("div", "ap-chan" + cls);
		box.appendChild(A.el("div", "f", (ch.freq / 1e6).toFixed(3)));
		let s = "ch " + ch.ch + " · " + ch.messages + " msg";
		if(typeof ch.last_level === "number") { s += " · " + ch.last_level.toFixed(1) + " dBFS"; }
		if(ch.drops > 0) { s += " · " + ch.drops + " drop"; }
		box.appendChild(A.el("div", "c", s));
		box.title = "channel " + ch.ch + " @ " + (ch.freq / 1e6).toFixed(3) + " MHz · queue " + (ch.queue || 0);
		p.chans.appendChild(box);
	}
}

function updateLegend(p) {
	const A = window.RFNM_APPS;
	p.legend.textContent = "";
	const key = (label, color) => {
		const sp = A.el("span");
		const sw = A.el("span", "sw");
		sw.style.background = color;
		sp.append(sw, document.createTextNode(label));
		return sp;
	};
	p.legend.append(key("Class A", "#10b981"), key("Class B", "#5da8dc"), key("base", "#f0be5a"), key("AtoN", "#c58cff"), key("SAR", "#ff85b3"));
	const radiusNm = S.radiusKm / 1.852;
	const sc = A.el("span");
	const b = A.el("b", null, radiusNm.toFixed(radiusNm < 10 ? 1 : 0) + " nm");
	b.style.color = "#9ab0c4";
	sc.append("range ", b, " · centre = fleet median");
	p.legend.appendChild(sc);
}

// ---- ship map ----------------------------------------------------------------------------
function drawShips(cv) {
	const dpr = window.devicePixelRatio || 1;
	const w = cv.clientWidth, h = cv.clientHeight;
	if(!w || !h) { return; }
	if(cv.width !== Math.round(w * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr); }
	const g = cv.getContext("2d");
	g.setTransform(dpr, 0, 0, dpr, 0, 0);
	g.clearRect(0, 0, w, h);
	g.font = "10px ui-monospace, monospace";

	const nowMs = Date.now();
	const pos = [...S.ships.values()].filter(s => hasPos(s) && (nowMs - s.lastSeen) / 1000 <= PLOT_AGE);

	if(pos.length) {
		const lats = pos.map(s => s.lat).sort((a, b) => a - b);
		const lons = pos.map(s => s.lon).sort((a, b) => a - b);
		const med = { lat: lats[lats.length >> 1], lon: lons[lons.length >> 1] };
		if(!S.center) { S.center = med; }
		else { S.center.lat += (med.lat - S.center.lat) * 0.15; S.center.lon += (med.lon - S.center.lon) * 0.15; }
	}
	const V = viewOf(cv);
	const stn = S.center || (V && V.c);
	const [sx, sy] = V ? V.toXY(stn.lon, stn.lat) : [0, 0];   // rings stay on the fleet, not on a panned view
	if(!V) {
		g.fillStyle = "#3e6188"; g.textAlign = "center";
		g.fillText("waiting for a position fix…", w / 2, h / 2);
		g.textAlign = "left";
		return;
	}

	// zoom-to-fit with hysteresis
	let need = 0.6;
	for(const s of pos) { need = Math.max(need, distKm(V.c, s)); }
	need *= 1.18;
	S.radiusKm = need > S.radiusKm ? need : S.radiusKm + (need - S.radiusKm) * 0.08;

	// basemap: tiles if online + selected, else coastline
	let drewBase = false;
	const set = TILESETS[S.basemap];
	if(S.basemap !== "coast" && navigator.onLine !== false) { drewBase = drawTiles(g, V, set); }
	let attrTxt = set.attr;
	if(!drewBase) { drewBase = drawCoast(g, V); attrTxt = (S.basemap === "coast" ? "" : "offline · ") + TILESETS.coast.attr; }
	if(S.panelRef && S.panelRef.attr) { S.panelRef.attr.textContent = attrTxt; }

	// range rings (nautical miles) + crosshair
	const radiusNm = S.radiusKm / 1.852;
	let step = 0.5;
	for(const s of [0.5, 1, 2, 5, 10, 20, 40, 80]) { step = s; if(radiusNm / s <= 4) { break; } }
	g.strokeStyle = "rgba(120,150,190,0.22)"; g.fillStyle = "rgba(150,175,205,0.6)";
	for(let r = step; r <= radiusNm; r += step) {
		const rp = r * 1852 / V.mpp;
		g.beginPath(); g.arc(sx, sy, rp, 0, 2 * Math.PI); g.stroke();
		g.fillText((r < 1 ? r : r) + " nm", sx + 3, sy - rp + 11);
	}
	g.strokeStyle = "rgba(150,175,205,0.5)"; g.beginPath();
	g.moveTo(sx - 6, sy); g.lineTo(sx + 6, sy); g.moveTo(sx, sy - 6); g.lineTo(sx, sy + 6); g.stroke();
	g.fillStyle = "rgba(150,175,205,0.6)"; g.fillText("N", V.cx - 3, 12);

	// trails (stored lon/lat, projected here)
	for(const s of pos) {
		if(!s.trail || s.trail.length < 2) { continue; }
		g.beginPath(); g.lineWidth = 1.4;
		g.strokeStyle = "rgba(" + colorFor(s.classType || s.lastType).rgb + ",0.45)";
		s.trail.forEach((pt, i) => { const xy = V.toXY(pt.lon, pt.lat); if(i) { g.lineTo(xy[0], xy[1]); } else { g.moveTo(xy[0], xy[1]); } });
		g.stroke();
	}
	g.lineWidth = 1;

	// vessels
	S._hit = [];
	for(const s of pos) {
		const [x, y] = V.toXY(s.lon, s.lat);
		if(x < -30 || x > w + 30 || y < -30 || y > h + 30) { continue; }
		S._hit.push({ mmsi: s.mmsi, x, y });
		const age = (nowMs - s.lastSeen) / 1000;
		const alpha = age > 400 ? 0.4 : (age > 120 ? 0.66 : 1.0);
		const col = colorFor(s.classType || s.lastType);
		const sel = s.mmsi === S.selected;
		const crs = courseOf(s);
		g.save();
		g.translate(x, y);
		if(crs !== null) {
			g.rotate(crs * RAD);
			g.beginPath();
			g.moveTo(0, -8); g.lineTo(3.6, -1); g.lineTo(3.6, 6); g.lineTo(-3.6, 6); g.lineTo(-3.6, -1); g.closePath();
		} else {
			g.beginPath();
			g.moveTo(0, -5); g.lineTo(5, 0); g.lineTo(0, 5); g.lineTo(-5, 0); g.closePath();
		}
		g.fillStyle = "rgba(" + col.rgb + "," + alpha + ")"; g.fill();
		g.lineWidth = 0.8; g.strokeStyle = "rgba(0,0,0,0.55)"; g.stroke();
		if(sel) { g.strokeStyle = "#fff"; g.lineWidth = 1.4; g.stroke(); }
		g.restore();
		if(sel) { g.strokeStyle = "rgba(255,255,255,0.55)"; g.beginPath(); g.arc(x, y, 14, 0, 2 * Math.PI); g.stroke(); }
		const label = s.name || String(s.mmsi);
		g.lineWidth = 3; g.strokeStyle = "rgba(4,6,12,0.7)"; g.strokeText(label, x + 9, y + 3);
		g.fillStyle = sel ? "#fff" : "rgba(210,222,240," + alpha + ")"; g.fillText(label, x + 9, y + 3);
	}
	g.lineWidth = 1;
}

// ---- interaction -------------------------------------------------------------------------
function hitTest(id, cv, ev) {
	if(!S._hit) { return; }
	const rect = cv.getBoundingClientRect();
	const mx = ev.clientX - rect.left, my = ev.clientY - rect.top;
	let best = null, bd = 20 * 20;
	for(const hc of S._hit) {
		const d = (hc.x - mx) ** 2 + (hc.y - my) ** 2;
		if(d < bd) { bd = d; best = hc.mmsi; }
	}
	S.selected = best;
	drawShips(cv);
	buildTable(id, true);
	updateDetail(id);
}

function fld(dl, k, v) {
	if(v === undefined || v === null || v === "") { return; }
	const A = window.RFNM_APPS;
	dl.append(A.el("dt", null, k), A.el("dd", null, String(v)));
}

function updateDetail(id) {
	const A = window.RFNM_APPS, p = A.ui[id].panel;
	if(!p) { return; }
	const d = p.detail;
	const s = S.ships.get(S.selected);
	if(!s) { d.style.display = "none"; return; }
	d.style.display = "block";
	d.textContent = "";
	const close = A.el("button", "cx", "×");
	close.onclick = () => { S.selected = null; d.style.display = "none"; drawShips(p.map); buildTable(id, true); };
	const h = A.el("h4");
	const sw = A.el("span", "sw");
	sw.style.cssText = "width:10px;height:10px;border-radius:2px;background:" + colorFor(s.classType || s.lastType).css;
	h.append(sw, document.createTextNode(s.name || String(s.mmsi)));
	const sub = A.el("div", "sub", [String(s.mmsi), kindOf(s.classType || s.lastType), s.chan ? ("ch " + s.chan) : ""].filter(Boolean).join(" · "));
	const dl = A.el("dl");
	const nowMs = Date.now();
	fld(dl, "call sign", s.callsign);
	fld(dl, "destination", s.dest);
	const stl = shipTypeLabel(s.shiptype);
	fld(dl, "ship type", stl ? stl + " (" + s.shiptype + ")" : undefined);
	fld(dl, "nav status", s.navStatus);
	if(hasPos(s)) {
		fld(dl, "position", s.lat.toFixed(5) + ", " + s.lon.toFixed(5));
		if(S.center) { fld(dl, "distance", (distKm(S.center, s) / 1.852).toFixed(2) + " nm"); }
	}
	fld(dl, "speed", typeof s.sog === "number" ? s.sog.toFixed(1) + " kn" : undefined);
	fld(dl, "course", typeof s.cog === "number" ? Math.round(s.cog) + "°" : undefined);
	fld(dl, "heading", typeof s.hdg === "number" ? Math.round(s.hdg) + "°" : undefined);
	fld(dl, "level", typeof s.level === "number" ? s.level.toFixed(1) + " dBFS" : undefined);
	fld(dl, "last msg", "type " + s.lastType);
	fld(dl, "seen", fmtAge((nowMs - s.lastSeen) / 1000) + " ago");
	d.append(close, h, sub, dl);
}

// ---- roster table (sortable + filterable) ------------------------------------------------
const COLS = [
	{ k: "name", h: "vessel", get: s => (s.name || String(s.mmsi)).toLowerCase() },
	{ k: "mmsi", h: "MMSI", get: s => s.mmsi },
	{ k: "kind", h: "class", get: s => kindOf(s.classType || s.lastType) },
	{ k: "sog", h: "kn", get: s => typeof s.sog === "number" ? s.sog : null, fmt: s => typeof s.sog === "number" ? s.sog.toFixed(1) : "" },
	{ k: "cog", h: "cog", get: s => courseOf(s), fmt: s => { const c = courseOf(s); return c === null ? "" : Math.round(c) + "°"; } },
	{ k: "nav", h: "status", get: s => s.navStatus || "" },
	{ k: "dist", h: "nm", get: s => s._dist ?? null, fmt: s => s._dist !== undefined ? s._dist.toFixed(1) : "" },
	{ k: "age", h: "age", get: s => (Date.now() - s.lastSeen) / 1000, fmt: s => fmtAge((Date.now() - s.lastSeen) / 1000) },
];

function buildTable(id, force) {
	const A = window.RFNM_APPS, ce = A.ui[id];
	if(!ce.panel) { return; }
	const p = ce.panel, nowMs = Date.now();
	const all = [...S.ships.values()].filter(s => (nowMs - s.lastSeen) / 1000 <= ROSTER_AGE);
	for(const s of all) { s._dist = (S.center && hasPos(s)) ? distKm(S.center, s) / 1.852 : undefined; }
	let view = all;
	if(S.filter) { view = all.filter(s => ((s.name || "") + " " + s.mmsi).toLowerCase().includes(S.filter)); }
	const col = COLS.find(c => c.k === S.sortKey) || COLS[7];
	view = view.slice().sort((x, y) => {
		let vx = col.get(x), vy = col.get(y);
		if(vx === null || vx === undefined) { vx = S.sortDir > 0 ? Infinity : -Infinity; }
		if(vy === null || vy === undefined) { vy = S.sortDir > 0 ? Infinity : -Infinity; }
		if(typeof vx === "string") { return S.sortDir * vx.localeCompare(vy); }
		return S.sortDir * (vx - vy);
	});
	const full = view;
	view = view.slice(0, 120);

	const print = S.sortKey + S.sortDir + "|" + S.filter + "|" + S.selected + "|" +
		JSON.stringify(view.map(s => [s.mmsi, col.fmt ? col.fmt(s) : col.get(s), Math.round((nowMs - s.lastSeen) / 1000)]));
	if(!force && (print === S.tblPrint || A.selectionInside(p.tblWrap))) { return; }
	S.tblPrint = print;

	const t = A.el("table", "ais-tbl");
	const tr0 = A.el("tr");
	for(const c of COLS) {
		const th = A.el("th", c.k === S.sortKey ? "sorted" : null, c.h + (c.k === S.sortKey ? (S.sortDir > 0 ? " ▲" : " ▼") : ""));
		th.onclick = () => {
			if(S.sortKey === c.k) { S.sortDir *= -1; }
			else { S.sortKey = c.k; S.sortDir = (c.k === "sog" || c.k === "cog") ? -1 : 1; }
			buildTable(id, true);
		};
		tr0.appendChild(th);
	}
	t.appendChild(tr0);
	for(const s of view) {
		const row = A.el("tr", "row" + (s.mmsi === S.selected ? " sel" : ""));
		const first = A.el("td");
		const dot = A.el("span", "sdot");
		dot.style.background = colorFor(s.classType || s.lastType).css;
		first.append(dot, document.createTextNode(s.name || String(s.mmsi)));
		row.appendChild(first);
		for(let i = 1; i < COLS.length; i++) {
			const c = COLS[i];
			const v = c.fmt ? c.fmt(s) : c.get(s);
			const empty = v === "" || v === null || v === undefined;
			row.appendChild(A.el("td", empty ? "mut" : null, empty ? "—" : String(v)));
		}
		row.onclick = () => { S.selected = S.selected === s.mmsi ? null : s.mmsi; drawShips(p.map); buildTable(id, true); updateDetail(id); };
		t.appendChild(row);
	}
	p.tblWrap.textContent = "";
	p.tblWrap.appendChild(t);
	if(full.length > view.length) { p.tblWrap.appendChild(A.el("div", "ap-hint", "+" + (full.length - view.length) + " more (filter to narrow)")); }
	updateDetail(id);
}

// ---- message-type breakdown --------------------------------------------------------------
const TYPE_ORDER = [1, 2, 3, 4, 5, 18, 19, 24, 21, 9];
function buildTypes(p) {
	const A = window.RFNM_APPS;
	const counts = new Map();
	for(const m of S.messages) { counts.set(m.type, (counts.get(m.type) || 0) + 1); }
	const keys = [...counts.keys()].sort((a, b) => {
		const ia = TYPE_ORDER.indexOf(a), ib = TYPE_ORDER.indexOf(b);
		return (ia < 0 ? 99 + a : ia) - (ib < 0 ? 99 + b : ib);
	});
	const print = JSON.stringify(keys.map(k => [k, counts.get(k)]));
	if(print === S.typePrint) { return; }
	S.typePrint = print;
	p.types.textContent = "";
	if(!keys.length) { p.types.appendChild(A.el("span", "ais-mut", "—")); return; }
	for(const k of keys) {
		const chip = A.el("div", "ais-tchip");
		const sw = A.el("span", "sw");
		sw.style.background = colorFor(k).css;
		chip.append(sw, document.createTextNode("T" + k), A.el("span", "n", String(counts.get(k))));
		chip.title = kindOf(k) + " — " + counts.get(k) + " in buffer";
		p.types.appendChild(chip);
	}
}

// ---- raw AIVDM feed ----------------------------------------------------------------------
function buildFeed(id) {
	const A = window.RFNM_APPS, ce = A.ui[id];
	if(!ce.panel) { return; }
	const p = ce.panel;
	const lines = [];
	for(let i = S.messages.length - 1; i >= 0 && lines.length < 250; i--) {
		const m = S.messages[i];
		if(!m.nmea) { continue; }
		const parts = String(m.nmea).split("\n");
		for(let j = parts.length - 1; j >= 0 && lines.length < 250; j--) { if(parts[j]) { lines.push({ t: m.time, s: parts[j] }); } }
	}
	const print = lines.map(l => l.t + l.s).join("|");
	if(print === S.feedPrint || A.selectionInside(p.feed)) { return; }
	S.feedPrint = print;
	p.feed.textContent = "";
	if(!lines.length) { p.feed.appendChild(A.el("div", "ais-mut", "no sentences yet")); S.feedText = ""; return; }
	S.feedText = lines.map(l => l.s).join("\n");
	for(const l of lines) {
		const div = A.el("div", "ln");
		div.append(A.el("span", "t", l.t + "  "), document.createTextNode(l.s));
		p.feed.appendChild(div);
	}
}

// ---- self-registration -------------------------------------------------------------------
(window.__RFNM_APP_MODULES = window.__RFNM_APP_MODULES || []).push({
	id: "ais", title: "AIS receiver",
	desc: "aisd decoding marine AIS (channels A/B, 161.975 / 162.025 MHz) into a live ship map, roster and raw AIVDM feed.",
	icon: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.2"><path d="M2.5 10 h11 l-1.4 3 H3.9 Z M8 10 V3 M8 3 L12 6 M4.6 8 h6.8"/></svg>',
	live: render,
});

})();
