// VITA-49 bridge monitor (vrtd). A "network appliance" telemetry page for the RFNM->wire
// VRT bridge: RX IQ + context stream out as VITA-49.0 signal-data/context packets, TX VRT
// ingested back into the radio, and a VITA-49.2 control channel. Data: GET /api/app/vrt/status.
//
// NOTE on the data contract: the /status JSON (vrtd main.cpp ~L127) puts the radio config
// (rx_freq, tx_freq, rate, rx_gain, tx_power, rx_on, tx_on) at the TOP LEVEL and uses
// "radio" as a board-description STRING (rad->describe()), NOT a nested object. We read the
// top-level fields and stay tolerant of a nested {radio:{...}} shape too.
"use strict";
(function() {

// ---- module state (survives across polls) ----------------------------------------
const S = {
	prev: null,          // last counters + timestamp, for delta rates
	rate: null,          // EWMA rates {pktsOut, ctxOut, sampOut, pktsIn, sampIn} or null
	hist: [],            // throughput ring: {rx, tx} in MB/s
	ctlUntil: 0,         // control lane stays lit until this timestamp (recent command)
	prevInteg: null,     // last integrity counters, to detect a rising fault
};

// ---- formatting helpers ------------------------------------------------------------
function n0(v) { return (typeof v === "number" && isFinite(v)) ? v : 0; }
function fMHz(hz) { return (typeof hz === "number" && isFinite(hz)) ? (hz / 1e6).toFixed(3) + " MHz" : "…"; }
function msps(hz) { return (typeof hz === "number" && isFinite(hz)) ? (hz / 1e6).toFixed(3) + " MSps" : "…"; }
function dbf(v, unit) { return (typeof v === "number" && isFinite(v)) ? v.toFixed(1) + " " + unit : "…"; }
function cnt(v) { return n0(v).toLocaleString(); }
function mbps(sps) { return (Math.max(0, n0(sps)) * 4 / 1e6).toFixed(2); }   // CS16 = 4 bytes/sample
function ew(prev, inst) { return (prev === null || prev === undefined) ? inst : 0.7 * prev + 0.3 * inst; }

function fRate(x) {
	if(x === null || x === undefined) { return "…"; }
	const v = Math.max(0, x);
	if(v >= 1e6) { return (v / 1e6).toFixed(2) + "M"; }
	if(v >= 1e4) { return (v / 1e3).toFixed(1) + "k"; }
	if(v >= 1e3) { return Math.round(v).toLocaleString(); }
	return v.toFixed(v < 10 ? 1 : 0);
}

// ---- the signal-flow diagram (built once, updated in place) ------------------------
const SVG =
	'<svg viewBox="0 0 760 250" preserveAspectRatio="xMidYMid meet">' +
	'<defs>' +
	'<marker id="vrtArrR" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto"><path d="M0 0 L6 3 L0 6 Z" fill="#3f6f9e"/></marker>' +
	'<marker id="vrtArrL" markerWidth="8" markerHeight="8" refX="0" refY="3" orient="auto"><path d="M6 0 L0 3 L6 6 Z" fill="#3f6f9e"/></marker>' +
	'</defs>' +
	// nodes
	'<rect id="vrt-nradio" class="vrt-node" x="24" y="55" width="150" height="140" rx="9"/>' +
	'<rect class="vrt-node vrt-wire" x="586" y="55" width="150" height="140" rx="9"/>' +
	'<text class="vrt-ntitle" x="99" y="92" text-anchor="middle">RFNM RADIO</text>' +
	'<text id="vrt-rxline" class="vrt-nsub" x="99" y="118" text-anchor="middle">RX …</text>' +
	'<text id="vrt-txline" class="vrt-nsub" x="99" y="142" text-anchor="middle">TX …</text>' +
	'<text id="vrt-rateline" class="vrt-nsub" x="99" y="170" text-anchor="middle">…</text>' +
	'<text class="vrt-ntitle" x="661" y="100" text-anchor="middle">VRT SINK</text>' +
	'<text class="vrt-nsub" x="661" y="124" text-anchor="middle">UDP · VITA-49</text>' +
	'<text class="vrt-nsub" x="661" y="152" text-anchor="middle">your receiver</text>' +
	// RX export lane (radio -> wire)
	'<line class="vrt-base" x1="176" y1="90" x2="582" y2="90" marker-end="url(#vrtArrR)"/>' +
	'<line id="vrt-fl-rx" class="vrt-fl rx fwd" x1="176" y1="90" x2="582" y2="90"/>' +
	'<text class="vrt-llabel" x="182" y="80">VRT DATA + CONTEXT</text>' +
	'<text id="vrt-rxrate" class="vrt-lrate" x="182" y="104">idle</text>' +
	// control lane (wire -> radio, commands in)
	'<line class="vrt-base" x1="182" y1="125" x2="584" y2="125" marker-start="url(#vrtArrL)"/>' +
	'<line id="vrt-fl-ctl" class="vrt-fl ctl rev" x1="182" y1="125" x2="584" y2="125"/>' +
	'<text class="vrt-llabel" x="182" y="116">VITA-49.2 CONTROL</text>' +
	'<text id="vrt-ctlrate" class="vrt-lrate" x="182" y="140">0 cmds</text>' +
	// TX ingest lane (wire -> radio)
	'<line class="vrt-base" x1="176" y1="160" x2="584" y2="160" marker-start="url(#vrtArrL)"/>' +
	'<line id="vrt-fl-tx" class="vrt-fl tx rev" x1="176" y1="160" x2="584" y2="160"/>' +
	'<text class="vrt-llabel" x="182" y="154">TX VRT INGEST</text>' +
	'<text id="vrt-txrate" class="vrt-lrate" x="182" y="174">idle</text>' +
	'</svg>';

const STYLE =
	".vrt-flow { max-width: 860px; margin: 2px 0 8px; }" +
	".vrt-flow svg { width: 100%; height: auto; display: block; }" +
	".vrt-base { stroke: #232f45; stroke-width: 2; fill: none; }" +
	".vrt-node { fill: #15243a; stroke: #2a4260; stroke-width: 1.4; transition: stroke .4s; }" +
	".vrt-node.on { stroke: #1d5244; }" +
	".vrt-wire { fill: #0a0f1c; }" +
	".vrt-ntitle { fill: #dae3ec; font: 600 13px ui-monospace, monospace; letter-spacing: 1px; }" +
	".vrt-nsub { fill: #62798f; font: 11px ui-monospace, monospace; }" +
	".vrt-llabel { fill: #9ab0c4; font: 10px ui-monospace, monospace; letter-spacing: 1px; }" +
	".vrt-lrate { fill: #62798f; font: 11px ui-monospace, monospace; }" +
	".vrt-lrate.on { fill: #cfe2f2; }" +
	".vrt-fl { fill: none; stroke-width: 2.6; stroke-dasharray: 9 9; opacity: 0; transition: opacity .35s; }" +
	".vrt-fl.on { opacity: 0.95; }" +
	".vrt-fl.rx { stroke: #10b981; filter: drop-shadow(0 0 3px rgba(16,185,129,.55)); }" +
	".vrt-fl.tx { stroke: #5da8dc; filter: drop-shadow(0 0 3px rgba(87,163,228,.55)); }" +
	".vrt-fl.ctl { stroke: #e4b664; stroke-width: 2; }" +
	".vrt-fl.fwd.on { animation: vrtFwd 1.3s linear infinite; }" +
	".vrt-fl.rev.on { animation: vrtRev 1.6s linear infinite; }" +
	"@keyframes vrtFwd { from { stroke-dashoffset: 0; } to { stroke-dashoffset: -180; } }" +
	"@keyframes vrtRev { from { stroke-dashoffset: 0; } to { stroke-dashoffset: 180; } }" +
	".vrt-health { display: flex; align-items: center; gap: 9px; padding: 7px 12px; border-radius: 8px;" +
	" margin-bottom: 10px; font: 12px ui-monospace, monospace; letter-spacing: .5px; }" +
	".vrt-health.ok { background: #0d2a23; border: 1px solid #1d5244; color: #10b981; }" +
	".vrt-health.bad { background: #1a0e0e; border: 1px solid #7c2440; color: #f43f5e; animation: apPulse 1.1s infinite; }" +
	".vrt-bdot { width: 8px; height: 8px; border-radius: 50%; flex: none; }" +
	".vrt-health.ok .vrt-bdot { background: #10b981; }" +
	".vrt-health.bad .vrt-bdot { background: #f43f5e; }" +
	".vrt-sec { color: #62798f; font-size: 10px; text-transform: uppercase; letter-spacing: 1.5px; margin: 12px 0 6px; }" +
	".vrt-spark { width: 100%; max-width: 860px; height: 122px; display: block; margin-top: 4px;" +
	" background: #0a1019; border: 1px solid #1c2c40; border-radius: 8px; }";

function injectStyle() {
	if(document.getElementById("vrt-style")) { return; }
	const s = document.createElement("style");
	s.id = "vrt-style";
	s.textContent = STYLE;
	document.head.appendChild(s);
}

// ---- throughput sparkline ----------------------------------------------------------
function drawSpark(cv, hist) {
	const dpr = window.devicePixelRatio || 1;
	const w = cv.clientWidth, h = cv.clientHeight;
	if(!w || !h) { return; }
	if(cv.width !== Math.round(w * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr); }
	const g = cv.getContext("2d");
	g.setTransform(dpr, 0, 0, dpr, 0, 0);
	g.clearRect(0, 0, w, h);
	const left = 6, right = w - 6, top = 8, bottom = h - 14;
	g.strokeStyle = "#16222f";
	g.beginPath(); g.moveTo(left, bottom); g.lineTo(right, bottom); g.stroke();
	g.font = "10px ui-monospace, monospace";

	if(hist.length < 2) {
		g.fillStyle = "#3e6188";
		g.textAlign = "center";
		g.fillText("accumulating throughput…", (left + right) / 2, (top + bottom) / 2);
		g.textAlign = "left";
		return;
	}
	let peak = 0;
	for(const p of hist) { if(p.rx > peak) { peak = p.rx; } if(p.tx > peak) { peak = p.tx; } }
	const mx = peak > 0 ? peak * 1.15 : 1;
	const n = hist.length;
	const X = i => left + (right - left) * (i / (n - 1));
	const Y = v => bottom - (bottom - top) * Math.min(1, Math.max(0, v) / mx);
	const hasTx = hist.some(p => p.tx > 1e-4);

	// RX export: filled area + line
	g.beginPath();
	g.moveTo(X(0), bottom);
	hist.forEach((p, i) => g.lineTo(X(i), Y(p.rx)));
	g.lineTo(X(n - 1), bottom);
	g.closePath();
	g.fillStyle = "rgba(55, 214, 122, 0.15)";
	g.fill();
	g.beginPath();
	hist.forEach((p, i) => { const x = X(i), y = Y(p.rx); if(i) { g.lineTo(x, y); } else { g.moveTo(x, y); } });
	g.strokeStyle = "#10b981"; g.lineWidth = 1.6; g.stroke();

	// TX ingest: line only (when present)
	if(hasTx) {
		g.beginPath();
		hist.forEach((p, i) => { const x = X(i), y = Y(p.tx); if(i) { g.lineTo(x, y); } else { g.moveTo(x, y); } });
		g.strokeStyle = "#5da8dc"; g.lineWidth = 1.4; g.stroke();
	}

	const cur = hist[n - 1];
	g.fillStyle = "#62798f";
	g.fillText("RX export MB/s", left + 2, top + 4);
	g.textAlign = "right";
	g.fillText("peak " + peak.toFixed(2), right - 2, top + 4);
	g.textAlign = "left";
	g.fillStyle = "#10b981";
	g.fillText(cur.rx.toFixed(2) + " MB/s", left + 2, bottom - 4);
	if(hasTx) {
		g.textAlign = "right";
		g.fillStyle = "#5da8dc";
		g.fillText("TX " + cur.tx.toFixed(2), right - 2, bottom - 4);
		g.textAlign = "left";
	}
}

// ---- panel construction ------------------------------------------------------------
function ensurePanel(id) {
	const A = window.RFNM_APPS, ce = A.ui[id];
	if(ce.panel) { return ce.panel; }
	injectStyle();
	ce.live.textContent = "";

	const flow = A.el("div", "vrt-flow");
	flow.innerHTML = SVG;
	const q = sel => flow.querySelector(sel);
	const refs = {
		nodeRadio: q("#vrt-nradio"), rxLine: q("#vrt-rxline"), txLine: q("#vrt-txline"), rateLine: q("#vrt-rateline"),
		flRx: q("#vrt-fl-rx"), flTx: q("#vrt-fl-tx"), flCtl: q("#vrt-fl-ctl"),
		rxRate: q("#vrt-rxrate"), txRate: q("#vrt-txrate"), ctlRate: q("#vrt-ctlrate") };

	const banner = A.el("div", "vrt-health ok");
	const tilesCfg = A.el("div", "ap-tiles");
	const tilesThru = A.el("div", "ap-tiles");
	const tilesHealth = A.el("div", "ap-tiles");
	const spark = document.createElement("canvas");
	spark.className = "vrt-spark";
	const grid = A.el("div");
	const hint = A.el("div", "ap-hint");

	ce.live.append(
		flow, banner,
		A.el("div", "vrt-sec", "radio"), tilesCfg,
		A.el("div", "vrt-sec", "throughput"), tilesThru,
		A.el("div", "vrt-sec", "wire integrity"), tilesHealth,
		spark, grid, hint);

	// reset the module state whenever the panel is (re)built
	S.prev = null; S.rate = null; S.hist = []; S.ctlUntil = 0; S.prevInteg = null;

	ce.panel = { refs, banner, tilesCfg, tilesThru, tilesHealth, spark, grid, hint, print: "" };
	return ce.panel;
}

// ---- live render (polled every 2s while active) ------------------------------------
function render(id) {
	const A = window.RFNM_APPS;
	if(A.liveBusy[id]) { return; }
	A.liveBusy[id] = true;
	const p = ensurePanel(id);

	A.fetchJSON("/api/app/vrt/status", undefined, 6000).then(d => {
		// tolerate both the real top-level shape and a nested {radio:{...}} object
		const r = (d.radio && typeof d.radio === "object") ? d.radio : d;
		const desc = (typeof d.radio === "string") ? d.radio : "";
		const now = performance.now() / 1000;

		const cur = { t: now,
			data_out: n0(d.data_out), ctx_out: n0(d.ctx_out), samples_out: n0(d.samples_out),
			data_in: n0(d.data_in), samples_in: n0(d.samples_in), cmds_in: n0(d.cmds_in) };

		// counter reset (daemon restart -> counters go backwards) => drop rate state
		if(S.prev && (cur.data_out < S.prev.data_out || cur.samples_out < S.prev.samples_out ||
				cur.data_in < S.prev.data_in || cur.cmds_in < S.prev.cmds_in)) {
			S.prev = null; S.rate = null; S.hist = []; S.ctlUntil = 0;
		}

		let dCmd = 0;
		if(S.prev && now > S.prev.t) {
			const dt = now - S.prev.t;
			const inst = {
				pktsOut: (cur.data_out - S.prev.data_out) / dt,
				ctxOut: (cur.ctx_out - S.prev.ctx_out) / dt,
				sampOut: (cur.samples_out - S.prev.samples_out) / dt,
				pktsIn: (cur.data_in - S.prev.data_in) / dt,
				sampIn: (cur.samples_in - S.prev.samples_in) / dt };
			S.rate = S.rate || { pktsOut: null, ctxOut: null, sampOut: null, pktsIn: null, sampIn: null };
			for(const k in inst) { S.rate[k] = ew(S.rate[k], inst[k]); }
			dCmd = cur.cmds_in - S.prev.cmds_in;
			S.hist.push({ rx: Math.max(0, inst.sampOut) * 4 / 1e6, tx: Math.max(0, inst.sampIn) * 4 / 1e6 });
			if(S.hist.length > 180) { S.hist.shift(); }
		}
		if(dCmd > 0) { S.ctlUntil = now + 3; }
		S.prev = cur;

		const haveRate = !!S.rate;
		const rxOn = !!r.rx_on, txOn = !!r.tx_on;
		const rxFlow = haveRate ? (S.rate.pktsOut > 0.2) : rxOn;
		const txFlow = haveRate ? (S.rate.pktsIn > 0.2) : txOn;
		const ctlLit = now < S.ctlUntil;

		// ---- flow diagram ----
		const rf = p.refs;
		rf.nodeRadio.classList.toggle("on", rxOn || txOn);
		rf.rxLine.textContent = "RX  " + fMHz(r.rx_freq);
		rf.rxLine.setAttribute("fill", rxOn ? "#10b981" : "#62798f");
		rf.txLine.textContent = "TX  " + fMHz(r.tx_freq);
		rf.txLine.setAttribute("fill", txOn ? "#5da8dc" : "#62798f");
		rf.rateLine.textContent = msps(r.rate);
		rf.flRx.classList.toggle("on", rxFlow);
		rf.flTx.classList.toggle("on", txFlow);
		rf.flCtl.classList.toggle("on", ctlLit);
		rf.rxRate.classList.toggle("on", rxFlow);
		rf.txRate.classList.toggle("on", txFlow);
		rf.ctlRate.classList.toggle("on", ctlLit);
		rf.rxRate.textContent = haveRate ? (fRate(S.rate.pktsOut) + " pkt/s · " + mbps(S.rate.sampOut) + " MB/s") : "idle";
		rf.txRate.textContent = haveRate && S.rate.pktsIn > 0.2 ? (fRate(S.rate.pktsIn) + " pkt/s · " + mbps(S.rate.sampIn) + " MB/s") : "idle";
		rf.ctlRate.textContent = cnt(d.cmds_in) + " cmds · " + cnt(d.acks_out) + " acks";

		// ---- integrity / health ----
		const integ = { g: n0(d.seq_gaps_in), dr: n0(d.tx_drops), rj: n0(d.cmds_rejected), ml: n0(d.malformed_in) };
		const rising = S.prevInteg && (integ.g > S.prevInteg.g || integ.dr > S.prevInteg.dr ||
			integ.rj > S.prevInteg.rj || integ.ml > S.prevInteg.ml);
		S.prevInteg = integ;
		const issues = [];
		if(integ.g > 0) { issues.push("seq gaps"); }
		if(integ.dr > 0) { issues.push("tx drops"); }
		if(integ.rj > 0) { issues.push("cmds rejected"); }
		if(integ.ml > 0) { issues.push("malformed"); }
		p.banner.textContent = "";
		p.banner.className = "vrt-health " + (issues.length ? "bad" : "ok");
		p.banner.append(A.el("span", "vrt-bdot"),
			A.el("span", null, issues.length ?
				("DEGRADED — " + issues.join(" · ") + (rising ? "  (rising)" : "")) :
				"BRIDGE HEALTHY — VRT stream clean, no gaps or drops"));

		// ---- tiles ----
		p.tilesCfg.textContent = "";
		p.tilesCfg.append(
			A.tile(fMHz(r.rx_freq), "rx freq"),
			A.tile(fMHz(r.tx_freq), "tx freq"),
			A.tile(msps(r.rate), "sample rate"),
			A.tile(dbf(r.rx_gain, "dB"), "rx gain"),
			A.tile(dbf(r.tx_power, "dBm"), "tx power"),
			A.tile(rxOn ? "ON" : "off", "rx"),
			A.tile(txOn ? "ON" : "off", "tx"));

		p.tilesThru.textContent = "";
		p.tilesThru.append(
			A.tile(haveRate ? fRate(S.rate.pktsOut) + "/s" : "…", "data pkts out"),
			A.tile(haveRate ? fRate(S.rate.ctxOut) + "/s" : "…", "context out"),
			A.tile(haveRate ? mbps(S.rate.sampOut) : "…", "export MB/s"),
			A.tile(haveRate ? fRate(S.rate.pktsIn) + "/s" : "…", "tx pkts in"),
			A.tile(haveRate ? mbps(S.rate.sampIn) : "…", "ingest MB/s"));

		p.tilesHealth.textContent = "";
		p.tilesHealth.append(
			A.tile(cnt(integ.g), "seq gaps in", integ.g > 0),
			A.tile(cnt(integ.dr), "tx drops", integ.dr > 0),
			A.tile(cnt(integ.rj), "cmds rejected", integ.rj > 0),
			A.tile(cnt(integ.ml), "malformed in", integ.ml > 0),
			A.tile(cnt(d.cmds_in), "cmds in"),
			A.tile(cnt(d.acks_out), "acks out"));

		// ---- throughput sparkline ----
		drawSpark(p.spark, S.hist);

		// ---- raw counters table ----
		const rows = [
			["data packets out", d.data_out], ["context packets out", d.ctx_out], ["samples out", d.samples_out],
			["tx data packets in", d.data_in], ["tx samples in", d.samples_in],
			["seq gaps in", d.seq_gaps_in], ["tx drops", d.tx_drops],
			["commands in", d.cmds_in], ["commands rejected", d.cmds_rejected],
			["acks out", d.acks_out], ["malformed in", d.malformed_in]];
		const print = JSON.stringify(rows);
		if(print !== p.print && !A.selectionInside(p.grid)) {
			p.print = print;
			p.grid.textContent = "";
			p.grid.appendChild(A.table(["counter", "value"], rows.map(row => A.tr([row[0], cnt(row[1])]))));
		}

		p.hint.textContent = (desc ? desc + " · " : "") +
			"VITA-49.0 signal-data + context out on the wire, 49.2 control in. Point a GNU Radio / SigMF VRT sink at this device. " +
			"Throughput assumes CS16 (4 bytes/sample).";
	}).catch(() => {}).finally(() => { A.liveBusy[id] = false; });
}

(window.__RFNM_APP_MODULES = window.__RFNM_APP_MODULES || []).push({
	id: "vrtd", title: "VITA-49 bridge",
	desc: "vrtd streaming RX IQ + context as VITA-49 and ingesting TX VRT, with a 49.2 control channel.",
	icon: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.2"><path d="M2 8 h3 l2-4 2 8 2-4 h3"/></svg>',
	live: render,
});

})();
