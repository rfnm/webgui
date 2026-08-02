// ISM Monitor (app_ismmon.js): full-screen live view for ismmond, the RX-only 2.4 GHz air monitor.
// Whole-band capture @ 100 MSPS / 2441 MHz -> GPU FFT burst-detect -> per-burst GFSK demod of BLE
// LE 1M advertising on ch37/38/39. Shows a live device table (keyed by advertiser address) and a
// scrolling packet log (click a row for the raw PDU hex). Data: /api/app/ismmon/status, /api/app/ismmon/devices,
// /api/app/ismmon/packets. Decode is proven by ismmond --selftest (GPU pipeline) and validated on-air.
"use strict";
(function() {

const APP = () => window.RFNM_APPS;

const ICON = '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.3">' +
	'<path d="M8 2.2v11.6M8 2.2l3 2.2-3 2.2M8 13.8l3-2.2-3-2.2" stroke-linejoin="round"/>' +
	'<path d="M3.2 4.8a6 6 0 000 6.4M12.8 4.8a6 6 0 010 6.4" opacity="0.7"/></svg>';

window.__RFNM_APP_MODULES = window.__RFNM_APP_MODULES || [];
window.__RFNM_APP_MODULES.push({
	id: "ismmon", title: "ISM Monitor", icon: ICON,
	desc: "RX-only 2.4 GHz air monitor: whole-band capture, GPU FFT burst-detect. Decodes BLE advertising (ch37/38/39) + Wi-Fi 802.11b beacons/clients (ch1-13) - live device table + packet log.",
	live: render,
});

const css = `
.ism-wrap { display: flex; flex-direction: column; gap: 10px; height: 100%; }
.ism-main { flex: 1 1 auto; display: flex; gap: 10px; min-height: 0; }
.ism-col { display: flex; flex-direction: column; gap: 6px; min-height: 0; }
.ism-col.devs { flex: 1 1 46%; } .ism-col.pkts { flex: 1 1 54%; }
.ism-h { color: #62798f; font-size: 10px; text-transform: uppercase; letter-spacing: 1px; display: flex; justify-content: space-between; }
.ism-h b { color: #9ab0c4; font-weight: normal; }
.ism-tblwrap { flex: 1 1 auto; overflow-y: auto; min-height: 0; border: 1px solid #16222f; border-radius: 6px; }
.ism-tblwrap .ap-table th { position: sticky; top: 0; background: #0d1725; z-index: 1; }
.ism-tblwrap .ap-table td { white-space: nowrap; }
.ism-addr { font-variant-numeric: tabular-nums; color: #cfe2f2; }
.ism-name { color: #10b981; } .ism-noname { color: #4a6d8f; }
.ism-ctl { display: flex; gap: 8px; align-items: center; }
.ism-ctl input[type=text] { background: #16283f; color: #dae3ec; border: 1px solid #2a4260; border-radius: 5px; font: inherit; padding: 3px 9px; width: 200px; }
.ism-tgl { background: #16283f; color: #9ab0c4; border: 1px solid #2a4260; border-radius: 5px; font: inherit; padding: 3px 12px; cursor: pointer; }
.ism-tgl.on { background: #0d2a23; color: #10b981; border-color: #1d5244; }
.ism-spacer { flex: 1 1 auto; }
.ism-count { color: #62798f; font-size: 11px; white-space: nowrap; }
.ism-ch { display: inline-block; padding: 0 6px; border-radius: 4px; font-size: 10px; background: #15243a; border: 1px solid #1c2c40; color: #9ab0c4; }
.ism-pill { display:inline-block; padding: 0 6px; border-radius: 10px; font-size: 10px; background: rgba(95,143,220,0.12); color: #8fb0e0; }
`;
if(!document.getElementById("ism-style")) {
	const st = document.createElement("style");
	st.id = "ism-style";
	st.textContent = css;
	document.head.appendChild(st);
}

const ST = {};
function freshState() {
	return { lastTotal: null, filter: "", paused: false, packets: [], pktPrint: "", devPrint: "",
		tilePrint: "", expanded: new Set() };
}
function stateOf(id) { if(!ST[id]) { ST[id] = freshState(); } return ST[id]; }

// signal-strength colour ramp (weak red -> strong green), by RSSI dB
const RAMP = [[-95, [226, 69, 60]], [-80, [255, 140, 66]], [-65, [255, 212, 121]], [-45, [155, 229, 100]], [-25, [55, 214, 122]]];
function rssiColor(v) {
	const s = Number(v);
	if(!isFinite(s)) { return [140, 160, 190]; }
	if(s <= RAMP[0][0]) { return RAMP[0][1]; }
	for(let i = 1; i < RAMP.length; i++) {
		if(s <= RAMP[i][0]) { const [a0, c0] = RAMP[i - 1], [a1, c1] = RAMP[i], t = (s - a0) / (a1 - a0); return c0.map((v2, k) => Math.round(v2 + (c1[k] - v2) * t)); }
	}
	return RAMP[RAMP.length - 1][1];
}
const rgba = (c, a) => "rgba(" + c[0] + "," + c[1] + "," + c[2] + "," + a + ")";

function render(id) {
	const a = APP();
	if(a.liveBusy[id]) { return; }
	a.liveBusy[id] = true;
	const p = ensurePanel(id);
	const st = stateOf(id);
	const done = () => { a.liveBusy[id] = false; };

	a.fetchJSON("/api/app/ismmon/status", undefined, 6000).then(status => {
		if(st.lastTotal !== null && status.total < st.lastTotal) { st.expanded.clear(); }
		st.lastTotal = status.total;
		updateTiles(id, p, st, status);
	}).catch(() => {});

	a.fetchJSON("/api/app/ismmon/devices", undefined, 6000).then(d => { buildDeviceTable(id, d.devices || []); }).catch(() => {});

	a.fetchJSON("/api/app/ismmon/packets", undefined, 6000).then(d => {
		if(!st.paused) { st.packets = d.packets || []; }
		buildPacketTable(id);
	}).catch(() => {}).finally(done);
}

function ensurePanel(id) {
	const a = APP(), ce = a.ui[id];
	if(ce.panel) { return ce.panel; }
	ST[id] = freshState();
	ce.live.textContent = "";
	const pbody = ce.live.parentElement;
	if(pbody) { pbody.style.display = "flex"; pbody.style.flexDirection = "column"; }
	ce.live.style.flex = "1 1 auto";
	ce.live.style.minHeight = "0";
	const st = stateOf(id);

	const wrap = a.el("div", "ism-wrap");
	const tiles = a.el("div", "ap-tiles");
	const main = a.el("div", "ism-main");

	// device column
	const devCol = a.el("div", "ism-col devs");
	const devH = a.el("div", "ism-h");
	const devHl = a.el("span", null, "Devices");
	const devHc = a.el("b", null, "0");
	devH.append(devHl, devHc);
	const devWrap = a.el("div", "ism-tblwrap");
	devCol.append(devH, devWrap);

	// packet column
	const pktCol = a.el("div", "ism-col pkts");
	const ctl = a.el("div", "ism-ctl");
	const filter = document.createElement("input");
	filter.type = "text";
	filter.placeholder = "filter (addr, name, type, hex…)";
	filter.oninput = () => { st.filter = filter.value.trim().toLowerCase(); buildPacketTable(id, true); };
	const bPause = a.el("button", "ism-tgl", "pause");
	bPause.onclick = () => { st.paused = !st.paused; bPause.classList.toggle("on", st.paused); bPause.textContent = st.paused ? "paused" : "pause"; };
	const spacer = a.el("span", "ism-spacer");
	const count = a.el("span", "ism-count", "");
	ctl.append(filter, bPause, spacer, count);
	const pktWrap = a.el("div", "ism-tblwrap");
	pktCol.append(ctl, pktWrap);

	main.append(devCol, pktCol);
	wrap.append(tiles, main);
	ce.live.append(wrap);
	ce.panel = { tiles, devWrap, devHc, pktWrap, count };
	return ce.panel;
}

function updateTiles(id, p, st, s) {
	const a = APP();
	const bits = [
		["band", (s.freq / 1e6).toFixed(0) + " MHz · " + (s.rate / 1e6).toFixed(0) + " MSps"],
		["coverage", (s.coverage_pct || 0).toFixed(0) + "% · FFT " + (s.fft_coverage_pct || 0).toFixed(0) + "%"],
		["devices", String(s.devices || 0)],
		["decoded", String(s.decoded || 0)],
		["bursts", String(s.bursts || 0)],
		["uptime", a.fmtUp(s.uptime)],
	];
	const print = JSON.stringify(bits);
	if(print === st.tilePrint) { return; }
	st.tilePrint = print;
	p.tiles.textContent = "";
	for(const b of bits) { p.tiles.append(a.tile(b[1], b[0])); }
}

const DCOLS = [
	{ h: "address", node: (a, d) => { const s = a.el("span", "ism-addr"); const dot = a.el("span"); dot.style.cssText = "display:inline-block;width:7px;height:7px;border-radius:2px;margin-right:6px;background:" + rgba(rssiColor(d.rssi), 1); s.append(dot, document.createTextNode(d.addr)); return s; } },
	{ h: "kind", get: d => d.type === "wifi" ? (/Beacon|ProbeResp/.test(d.pdu || "") ? "AP" : "STA") : (d.type === "random" ? "rnd" : "pub") },
	{ h: "ch", node: (a, d) => a.el("span", "ism-ch", d.channel ? String(d.channel) : "—") },
	{ h: "name", node: (a, d) => d.name ? a.el("span", "ism-name", d.name) : a.el("span", "ism-noname", "—") },
	{ h: "type", get: d => d.pdu },
	{ h: "mfr", get: d => d.mfr || "" },
	{ h: "rssi", get: d => (d.rssi).toFixed(0) },
	{ h: "cfo", get: d => (d.cfo_khz > 0 ? "+" : "") + d.cfo_khz + "k" },
	{ h: "cnt", get: d => d.count },
	{ h: "age", get: (a, d) => a.fmtUp(d.age) },
];

function buildDeviceTable(id, devs) {
	const a = APP(), ce = a.ui[id];
	if(!ce.panel) { return; }
	const p = ce.panel, st = stateOf(id);
	devs = devs.slice().sort((x, y) => x.age - y.age);   // most-recently-heard first
	p.devHc.textContent = String(devs.length);
	const print = JSON.stringify(devs.map(d => [d.addr, d.name, d.pdu, d.channel, d.mfr, Math.round(d.rssi), d.cfo_khz, d.count, Math.round(d.age)]));
	if(print === st.devPrint || a.selectionInside(p.devWrap)) { return; }
	st.devPrint = print;
	if(!devs.length) {
		p.devWrap.textContent = "";
		const h = a.el("div", "ap-hint", "listening for BLE advertisers + Wi-Fi APs/clients… (decode proven by ismmond --selftest)");
		h.style.padding = "12px";
		p.devWrap.append(h);
		return;
	}
	const rows = devs.map(d => {
		const row = a.el("tr");
		for(const c of DCOLS) {
			const td = a.el("td");
			if(c.node) { td.append(c.node(a, d)); }
			else { td.textContent = String(c.get.length === 2 ? c.get(a, d) : c.get(d)); }
			row.append(td);
		}
		return row;
	});
	p.devWrap.textContent = "";
	p.devWrap.append(a.table(DCOLS.map(c => c.h), rows));
}

const PCOLS = [
	{ h: "time", get: pk => pk.time },
	{ h: "ch", node: (a, pk) => a.el("span", "ism-ch", String(pk.ch)) },
	{ h: "type", get: pk => pk.type },
	{ h: "address", get: pk => pk.addr },
	{ h: "rssi", get: pk => (pk.rssi).toFixed(0) },
	{ h: "name", node: (a, pk) => pk.name ? a.el("span", "ism-name", pk.name) : a.el("span", "ism-noname", "—") },
	{ h: "len", get: pk => pk.len },
];
function pktKey(pk) { return pk.time + "|" + (pk.hex || "").slice(0, 16); }

function buildPacketTable(id, force) {
	const a = APP(), ce = a.ui[id];
	if(!ce.panel) { return; }
	const p = ce.panel, st = stateOf(id);
	let pkts = st.packets;
	if(st.filter) {
		pkts = pkts.filter(pk => (pk.addr + " " + (pk.name || "") + " " + pk.type + " " + (pk.hex || "") + " ch" + pk.ch).toLowerCase().includes(st.filter));
	}
	p.count.textContent = pkts.length + (st.paused ? " pkts · paused" : " pkts");
	const print = JSON.stringify(pkts.map(pk => [pk.time, pk.ch, pk.type, pk.addr, Math.round(pk.rssi), pk.name, pk.len])) + "|" + [...st.expanded].join(",") + "|" + st.filter;
	if(!force && (print === st.pktPrint || a.selectionInside(p.pktWrap))) { return; }
	st.pktPrint = print;
	if(!pkts.length) {
		p.pktWrap.textContent = "";
		const h = a.el("div", "ap-hint", st.filter ? "no packets match the filter" : "no packets yet — advertising bursts appear here as they decode");
		h.style.padding = "12px";
		p.pktWrap.append(h);
		return;
	}
	const keys = new Set(pkts.map(pktKey));
	for(const k of st.expanded) { if(!keys.has(k)) { st.expanded.delete(k); } }
	const rows = [];
	for(const pk of pkts) {
		const key = pktKey(pk);
		const row = a.el("tr", "exp-toggle");
		for(const c of PCOLS) {
			const td = a.el("td");
			if(c.node) { td.append(c.node(a, pk)); }
			else { td.textContent = String(c.get.length === 2 ? c.get(a, pk) : c.get(pk)); }
			row.append(td);
		}
		row.onclick = () => { if(st.expanded.has(key)) { st.expanded.delete(key); } else { st.expanded.add(key); } buildPacketTable(id, true); };
		rows.push(row);
		if(st.expanded.has(key)) {
			const hexRow = a.el("tr", "ap-hexrow");
			const td = a.el("td", null, pk.hex || "(empty)");
			td.colSpan = PCOLS.length;
			hexRow.append(td);
			rows.push(hexRow);
		}
	}
	p.pktWrap.textContent = "";
	p.pktWrap.append(a.table(PCOLS.map(c => c.h), rows));
	if(!st.paused) { p.pktWrap.scrollTop = 0; }
}

})();
