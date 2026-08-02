// APPS view v4: renders EXCLUSIVELY from appd's /v1/state document - the GUI never infers
// state from systemd, never keeps its own app catalog. The card list, order, panels, RF
// knobs, and config rows all come from the manifests appd scanned on the board; adding an
// app deploys a directory and this file never changes.
//
// The radio is a lease with one owner (appd serializes transitions and enforces settle):
//   activate  -> POST /v1/radio {"owner":"<id>"}     stop -> {"owner":"none"}
// RTSA and VSG are manifest apps too, but their panels ("rtsa"/"vsg") have their own
// top-level tabs, so they are hidden from the app menu; the top strip + the RTSA takeover
// overlay still reflect/steer them through the same lease.
//
// GET /v1/state is also the browser-presence heartbeat: appd idle-stops the radio owner
// when nobody polls (manifest idle_stop_s), so this file polls it while the page is open.
//
// Behavior rules carried from v3 (learned the hard way):
// - every fetch has a timeout + in-flight guard; a slow poll is skipped, never queued
// - state responses apply in order (server seq guard)
// - POSTs never freeze the page; errors land in sticky dismissable boxes
// - DOM is only rebuilt when data changed (fingerprints), never under a text selection
// - only the SELECTED app is polled for live data
"use strict";
(function() {

// panels with their own top-level tabs (or no UI at all) stay out of the app menu
const HIDDEN_PANELS = new Set(["rtsa", "vsg", "none"]);

const ICONS = {
	adsb: '<svg viewBox="0 0 16 16" fill="currentColor" stroke="none"><path d="M8 1.2 L9 5.8 L15 8.6 L9 8.9 L8.8 12.4 L11 14.4 L8 13.5 L5 14.4 L7.2 12.4 L7 8.9 L1 8.6 L7 5.8 Z"/></svg>',
	openwebrx: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.2"><circle cx="8" cy="8" r="6.4"/><ellipse cx="8" cy="8" rx="2.9" ry="6.4"/><path d="M1.8 8 h12.4 M2.7 4.6 h10.6 M2.7 11.4 h10.6"/></svg>',
};

const CONFIG_HINTS = {   // optional helper text keyed by "<key>:<value>"
	"region:us915": "LoRaWAN: sub-band 1, 902.3–903.7 MHz, SF7–10 · Meshtastic: 906.875 MHz",
	"region:eu868": "LoRaWAN: 867.1–868.5 MHz, SF7–12 · Meshtastic: 869.525 MHz",
};

// ---- state ---------------------------------------------------------------------
let state = null;              // last applied /v1/state document
let lastSeq = 0;               // server-seq ordering guard
let stateInFlight = false;
let appdOk = false;
let pending = false;           // a radio/config/rf POST in flight
let selected = null;
let userPicked = false;
let curView = (location.hash.replace("#", "") || "rtsa").split("/")[0];
let viewVisible = curView === "apps";
let cardsKey = "";             // fingerprint of the built card set
const ui = {};                 // per-app elements by app id
const openLogs = new Set();
const cardErr = {};            // id -> {msg, log[], manual} sticky until dismissed/active
const liveBusy = {};

// ---- helpers -------------------------------------------------------------------
function el(tag, cls, text) {
	const e = document.createElement(tag);
	if(cls) { e.className = cls; }
	if(text !== undefined) { e.textContent = text; }
	return e;
}

function fetchJSON(url, opts, timeoutMs) {
	const ctl = new AbortController();
	const timer = setTimeout(() => ctl.abort(), timeoutMs || 8000);
	const o = Object.assign({ signal: ctl.signal, cache: "no-store" }, opts || {});
	return fetch(url, o).then(r =>
		r.json().catch(() => ({})).then(j => {
			if(!r.ok) { const e = new Error(j.err || j.error || ("HTTP " + r.status)); e.log = j.log; throw e; }   // appd says "err" (API.md envelope); "error" kept for stragglers
			return j;
		})
	).finally(() => clearTimeout(timer));
}

function api(path, timeoutMs) {
	return fetchJSON("/api/v1" + path, undefined, timeoutMs);
}

function apiWrite(method, path, body, timeoutMs) {
	return fetchJSON("/api/v1" + path, { method, body: body ? JSON.stringify(body) : undefined }, timeoutMs || 30000);
}

function fmtUp(s) {
	if(s === undefined || s === null) { return ""; }
	if(s < 90) { return Math.round(s) + "s"; }
	if(s < 5400) { return Math.floor(s / 60) + "m " + Math.round(s % 60) + "s"; }
	if(s < 172800) { return Math.floor(s / 3600) + "h " + Math.floor((s % 3600) / 60) + "m"; }
	return Math.floor(s / 86400) + "d " + Math.floor((s % 86400) / 3600) + "h";
}

function fmtMem(b) {
	if(!b && b !== 0) { return ""; }
	if(b < 1048576) { return Math.round(b / 1024) + "K"; }
	if(b < 1073741824) { return (b / 1048576).toFixed(b < 104857600 ? 1 : 0) + "M"; }
	return (b / 1073741824).toFixed(2) + "G";
}

function tile(value, label, warn) {
	const t = el("div", "ap-tile" + (warn ? " warn" : ""));
	t.append(el("div", "v", value), el("div", "l", label));
	return t;
}

function table(headers, rows) {
	const t = el("table", "ap-table");
	const tr0 = el("tr");
	for(const h of headers) { tr0.appendChild(el("th", null, h)); }
	t.appendChild(tr0);
	for(const r of rows) { t.appendChild(r); }
	return t;
}

function tr(cells) {
	const row = el("tr");
	for(const c of cells) { row.appendChild(el("td", null, c === undefined || c === null ? "" : String(c))); }
	return row;
}

function selectionInside(node) {
	const sel = window.getSelection && window.getSelection();
	return !!(sel && !sel.isCollapsed && sel.anchorNode && node.contains(sel.anchorNode));
}

// apps[] accessors over the state doc
function apps() { return (state && state.apps) || []; }
function appOf(id) { return apps().find(a => a.id === id) || null; }
function radioInfo() { return (state && state.radio) || { owner: null, state: "idle", transition: null }; }
function cardApps() { return apps().filter(a => !HIDDEN_PANELS.has(a.panel)); }

// ---- per-app viz modules (app_*.js, loaded before this script) ------------------
// A module self-registers by pushing {id,title,desc,icon,live} onto
// window.__RFNM_APP_MODULES; the manifest's gui.panel selects it by that id.
// .state is the last applied /v1/state doc; every poll outcome is also re-broadcast as the
// "rfnm-state" window event ({ok, state}) so title.js/settings.js render from THE poll
// instead of fetching /v1/state a second time.
window.RFNM_APPS = { el, tile, table, tr, fetchJSON, api, fmtUp, fmtMem, selectionInside, ui, liveBusy, state: null };

function announceState() {
	window.RFNM_APPS.state = state;
	window.dispatchEvent(new CustomEvent("rfnm-state", { detail: { ok: appdOk, state } }));
}

// last explicitly selected app survives reloads (same idea as spectrum.js "rfsp." prefs);
// restored as the DEFAULT only - a live radio owner or an #apps/<id> deep link still wins
const persistSel = id => { try { window.localStorage.setItem("rfap.sel", id); } catch(e) {} };
const recallSel = () => { try { return window.localStorage.getItem("rfap.sel"); } catch(e) { return null; } };

const MODULES = {};
for(const m of window.__RFNM_APP_MODULES || []) {
	MODULES[m.id] = m;
	if(m.icon) { ICONS[m.id] = m.icon; }
}
MODULES.openwebrx = { live: renderOwrx };   // built-in inline panel

// ---- status corners: radio-lease footer (under the menu), vitals + error toast --------
const bar = {
	dot: document.getElementById("ap-holderdot"),
	hold: document.getElementById("ap-holder"),
	err: document.getElementById("aperr"),
	vitals: document.getElementById("apvitals"),
};

// ---- per-app RX antenna + gain control (lives in the pane header of the app using it) --
const ANT_LABEL = { sma_a: "SMA-A", embed: "Embedded" };
(function injectStyle() {
	const s = document.createElement("style");
	s.textContent =
		".ap-rf { display: none; align-items: center; gap: 6px; font-size: 11px; color: #62798f; flex: none; }" +
		".ap-rf .ap-rflabel { text-transform: uppercase; letter-spacing: 1px; font-size: 10px; }" +
		".ap-rfsel { background: #16283f; color: #dae3ec; border: 1px solid #2a4260; border-radius: 5px; font: inherit; font-size: 11px; padding: 2px 6px; cursor: pointer; }" +
		".ap-rfsel:hover { border-color: #3f6f9e; }" +
		".ap-rfslider { width: 104px; vertical-align: middle; accent-color: #10b981; cursor: pointer; }" +
		".ap-rfval { color: #9ab0c4; font-variant-numeric: tabular-nums; min-width: 40px; }" +
		".ap-rfhint { color: #10b981; font-size: 11px; }" +
		".ap-build { color: #475d73; font-size: 10px; margin-left: 8px; }" +
		".ap-agc { display: inline-flex; align-items: center; gap: 4px; cursor: pointer; color: #9ab0c4; }" +
		".ap-agc input { accent-color: #57a3e4; cursor: pointer; margin: 0; }" +
		".ap-btn.stopbtn { border-color: #5c2a28; color: #ef8ba1; }" +
		".ap-btn.stopbtn:hover:not(:disabled) { border-color: #f43f5e; color: #ff9d94; background: #1c0f0e; }" +
		"#appmenu .mitem .mdot.run { width: 8px; height: 8px; }";
	document.head.appendChild(s);
})();

const rfGroup = el("span", "ap-rf ap-rfrow");
const rfSel = el("select", "ap-rfsel");
const rfAgcWrap = el("label", "ap-agc");
const rfAgc = document.createElement("input");
rfAgc.type = "checkbox";
rfAgcWrap.append(rfAgc, el("span", "ap-rflabel", "agc"));
const rfGainLabel = el("span", "ap-rflabel", "gain");
const rfSlider = document.createElement("input");
rfSlider.type = "range";
rfSlider.className = "ap-rfslider";
rfSlider.min = "-24";
rfSlider.max = "30";
rfSlider.step = "1";
const rfVal = el("span", "ap-rfval", "— dB");
const rfHintEl = el("span", "ap-rfhint", "");
rfGroup.append(el("span", "ap-rflabel", "antenna"), rfSel, rfAgcWrap, rfGainLabel, rfSlider, rfVal, rfHintEl);

// AGC on = the device levels itself, the manual slider disappears (it stays the fallback)
function syncGainVisibility() {
	const manual = !rfAgc.checked;
	rfGainLabel.style.display = manual ? "" : "none";
	rfSlider.style.display = manual ? "" : "none";
	rfVal.style.display = manual ? "" : "none";
}

let rfLoadedApp = null;    // app whose values are shown (reload on selection change)
let rfPostBusy = false;
let rfDebounce = null;
let rfHintTimer = null;

function rfHint(msg) {
	rfHintEl.textContent = msg;
	clearTimeout(rfHintTimer);
	if(msg) { rfHintTimer = setTimeout(() => { rfHintEl.textContent = ""; }, 4000); }
}

function updateRfControl() {
	const a = selected && appOf(selected);
	const show = appdOk && a && a.rf;
	rfGroup.style.display = show ? "inline-flex" : "none";
	// the control belongs to the app that uses it: reparent into the selected pane's header
	const slot = selected && ui[selected] && ui[selected].rfSlot;
	if(show && slot && rfGroup.parentNode !== slot) { slot.appendChild(rfGroup); }
	if(!show) { rfHint(""); rfLoadedApp = null; return; }
	if(rfLoadedApp === selected || rfPostBusy) { return; }
	rfLoadedApp = selected;
	const rf = a.rf;
	const ants = rf.antennas || ["sma_a", "embed"];
	if(rfSel.dataset.ants !== ants.join(",")) {
		rfSel.textContent = "";
		for(const x of ants) {
			const o = el("option", null, ANT_LABEL[x] || x);
			o.value = x;
			rfSel.appendChild(o);
		}
		rfSel.dataset.ants = ants.join(",");
	}
	rfSel.value = rf.antenna || ants[0];
	rfApply.style.display = "none";
	const agcOk = !(state && state.board && state.board.agc_available === false);
	rfAgc.checked = agcOk && rf.agc !== false;
	rfAgc.disabled = !agcOk;
	rfAgcWrap.title = agcOk ? "" : "AGC unavailable: kernel peak-detector defect - manual gain until the driver fix lands";
	rfAgcWrap.style.opacity = agcOk ? "" : "0.45";
	if(rf.gain_min !== undefined) { rfSlider.min = String(rf.gain_min); }
	if(rf.gain_max !== undefined) { rfSlider.max = String(rf.gain_max); }
	rfSlider.value = String(rf.gain !== undefined ? rf.gain : 30);
	rfVal.textContent = rfSlider.value + " dB";
	syncGainVisibility();
}

function applyRf() {
	const id = selected;
	if(!id || !appOf(id) || !appOf(id).rf) { return; }
	rfPostBusy = true;
	rfHint("applying…");
	apiWrite("PUT", "/apps/" + id + "/rf", { antenna: rfSel.value, agc: rfAgc.checked, gain: parseInt(rfSlider.value, 10) }).then(d => {
		rfHint(d.restarted ? "applied — restarting" : "saved for next start");
	}).catch(e => {
		rfHint("");
		stripError("antenna/gain change failed: " + e.message);
	}).finally(() => {
		rfPostBusy = false;
		pollState();
	});
}

// STAGED apply (a checkbox click silently restarting the app is bad design) -
// edits accumulate; the button states its consequence and nothing happens until clicked.
const rfApply = el("button", "ap-btn", "apply");
rfApply.style.display = "none";
rfGroup.insertBefore(rfApply, rfHintEl);
function rfMarkDirty() {
	const a = selected && appOf(selected);
	rfApply.style.display = "";
	rfApply.textContent = a && (a.state === "active" || a.state === "activating") ? "apply (restarts " + a.title + ")" : "apply";
}
rfApply.onclick = () => { rfApply.style.display = "none"; applyRf(); };
rfSel.onchange = rfMarkDirty;
rfAgc.onchange = () => { syncGainVisibility(); rfMarkDirty(); };
rfSlider.oninput = () => {
	rfVal.textContent = rfSlider.value + " dB";
	rfMarkDirty();
};

// ---- card skeleton (rebuilt only when the manifest set changes) ----------------------
const menu = document.getElementById("appmenu");
const main = document.getElementById("appmain");

function buildCards() {
	const list = cardApps();
	const key = list.map(a => a.id + ":" + a.title + ":" + (a.config || []).map(c => c.key).join("+")).join("|");
	if(key === cardsKey) { return; }
	cardsKey = key;
	menu.textContent = "";
	main.textContent = "";
	for(const id in ui) { delete ui[id]; }

	for(const a of list) {
		const mitem = el("div", "mitem");
		mitem.id = "appmenu-" + a.id;
		const micon = el("span", "ap-icon");
		micon.innerHTML = ICONS[a.panel] || ICONS[a.id] || "";
		const mdot = el("span", "mdot");
		// the menu dot means "has the RF front-end" - infra apps (radio:none) never get one,
		// a green dot next to Clock Gen/Board I/O would just be noise (they are always running)
		if(a.radio === "none") {
			mdot.style.display = "none";
		} else {
			mdot.title = "green = this app is using the RF front-end";
		}
		mitem.append(micon, el("span", "mlabel", a.title), mdot);
		mitem.onclick = () => { userPicked = true; selectApp(a.id); };
		menu.appendChild(mitem);

		const pane = el("div", "app-pane");
		const phead = el("div", "app-phead");
		const icon = el("span", "ap-icon");
		icon.innerHTML = ICONS[a.panel] || ICONS[a.id] || "";
		const chip = el("span", "ap-chip off", "…");
		const btns = el("span", "ap-btns");
		const bAct = el("button", "ap-btn actbtn", "activate");
		const bStop = el("button", "ap-btn stopbtn", "stop");
		const bOpen = el("button", "ap-btn", "open");
		const bLog = el("button", "ap-btn logbtn", "log");
		bAct.onclick = () => doRadio(a.id);
		bStop.onclick = () => doRadio("none");
		bOpen.onclick = () => {
			const port = (appOf(a.id) || {}).open;
			if(port) { window.open("http://" + location.hostname + ":" + port + "/", "_blank"); }
		};
		bLog.onclick = () => {
			if(openLogs.has(a.id)) { openLogs.delete(a.id); } else { openLogs.add(a.id); refreshLog(a.id); }
			render();
		};
		btns.append(bAct, bStop, bOpen, bLog);
		const meta = el("div", "ap-meta");
		const build = el("span", "ap-build");
		phead.append(icon, el("h3", null, a.title), chip, btns, meta, build);

		// config rows straight from the manifest (segmented control per key)
		const cfgUi = {};
		for(const c of a.config || []) {
			const seg = el("span", "ap-seg");
			const btnsByVal = {};
			for(const v of c.choices) {
				const b = el("button", null, v.toUpperCase());
				b.onclick = () => setConfig(a.id, c.key, v);
				btnsByVal[v] = b;
				seg.appendChild(b);
			}
			const hint = el("span", "ap-hint", "");
			phead.append(seg, hint);
			cfgUi[c.key] = { btns: btnsByVal, hint };
		}

		const rfSlot = el("span", "ap-rfrow");   // the shared RF control reparents here when selected
		phead.appendChild(rfSlot);

		const fail = el("div", "ap-fail");
		fail.style.display = "none";
		const logPre = el("pre", "ap-logpre");
		logPre.style.display = "none";

		const pbody = el("div", "app-pbody");
		const live = el("div", "ap-live");
		live.style.display = "none";
		const idle = el("div", "app-idle");
		idle.append(el("div", null, a.desc || ""));
		idle.append(el("div", "ap-hint", a.radio === "none"
			? "always-on service — its controls appear here while it is running on this board."
			: "select activate to start — one radio at a time, so this stops any other running app."));
		pbody.append(idle, live);

		pane.append(phead, fail, logPre, pbody);
		main.appendChild(pane);

		ui[a.id] = { pane, mitem, mdot, chip, bAct, bStop, bOpen, bLog, metaEl: meta, buildEl: build,
			cfgUi, fail, logPre, live, idle, rfSlot, panel: null };
	}

	if(!selected || !ui[selected]) {
		const dl = window.__APPS_DEEPLINK;
		const last = recallSel();
		if(dl && ui[dl]) {
			userPicked = true;   // a deep link is an explicit choice - don't auto-jump to the radio owner
			selected = dl;
		} else if(last && ui[last]) {
			selected = last;     // restored default, NOT a user pick: the radio owner still auto-selects
		} else {
			selected = list.length ? list[0].id : null;
		}
	}
	if(selected) { selectApp(selected); }
}

// ---- actions ---------------------------------------------------------------------
let stripErrTimer = null;
function stripError(msg) {
	bar.err.textContent = msg;
	bar.err.style.display = "block";
	clearTimeout(stripErrTimer);
	stripErrTimer = setTimeout(() => { bar.err.style.display = "none"; }, 8000);
}

// the lease: POST /v1/radio. owner "none" releases (stops every radio app).
function doRadio(owner) {
	if(pending) { return; }
	pending = true;
	render();
	apiWrite("POST", "/radio", { owner }).then(() => {
		if(owner !== "none") { delete cardErr[owner]; }
	}).catch(e => {
		if(owner !== "none" && ui[owner]) { cardErr[owner] = { msg: "activate failed: " + e.message, log: e.log || [], manual: true }; }
		else { stripError((owner === "none" ? "stop" : "activate") + " failed: " + e.message); }
	}).finally(() => {
		pending = false;
		pollState();
	});
}

function setConfig(id, key, value) {
	if(pending) { return; }
	pending = true;
	render();
	apiWrite("PUT", "/apps/" + id + "/config", { key, value }).catch(e => {
		stripError(key + " change failed: " + e.message);
	}).finally(() => {
		pending = false;
		pollState();
	});
}

function refreshLog(id) {
	api("/apps/" + id + "/log?n=40", 8000).then(d => {
		if(ui[id]) { ui[id].logPre.textContent = (d.log || []).join("\n") || "(journal empty)"; }
	}).catch(() => {});
}

window.RFNM_APPS.selectApp = id => { userPicked = true; selectApp(id); };   // panels may cross-link apps

function selectApp(id) {
	if(!ui[id]) { return; }
	selected = id;
	if(userPicked) { persistSel(id); }
	rfLoadedApp = null;   // reload the RF control for the new app
	for(const a of cardApps()) {
		ui[a.id].pane.classList.toggle("show", a.id === id);
		ui[a.id].mitem.classList.toggle("active", a.id === id);
	}
	render();
	pollLive();
}

// ---- state poll: THE poll (also the appd presence heartbeat) --------------------------
function pollState() {
	if(stateInFlight) { return; }
	stateInFlight = true;
	api("/state", 6000).then(s => {
		if(s.seq && s.seq < lastSeq) { return; }   // stale response
		lastSeq = s.seq || 0;
		appdOk = true;
		state = s;
		announceState();
		buildCards();
		const owner = radioInfo().owner;
		if(!userPicked && owner && ui[owner]) { selectApp(owner); } else { render(); }
	}).catch(() => {
		appdOk = false;
		announceState();
		render();
	}).finally(() => {
		stateInFlight = false;
	});
}

const isCrashing = a => (a.state === "activating" && a.sub === "auto-restart");

// ---- render: strip + cards, straight from the state doc -------------------------------
function render() {
	const r = radioInfo();
	const ownerApp = r.owner ? appOf(r.owner) : null;

	if(!appdOk) {
		bar.dot.className = "ap-dot bad";
		bar.hold.textContent = "appd unreachable";
		bar.hold.title = "";
	}
	else if(r.transition) {
		bar.dot.className = "ap-dot idle";
		bar.hold.textContent = "switching → " + r.transition.to;
		bar.hold.title = r.transition.from + " → " + r.transition.to + " (" + r.transition.phase + ")";
	}
	else if(r.state === "starting" && ownerApp) {
		bar.dot.className = "ap-dot idle";
		bar.hold.textContent = "starting " + ownerApp.title + "…";
		bar.hold.title = "";
	}
	else if(r.state === "crashed" && ownerApp) {
		bar.dot.className = "ap-dot bad";
		bar.hold.textContent = ownerApp.title + " crashed";
		bar.hold.title = "";
	}
	else if(ownerApp) {
		bar.dot.className = "ap-dot";
		bar.hold.textContent = "";
		bar.hold.append("RF: ");
		bar.hold.appendChild(el("b", null, ownerApp.title));
		bar.hold.title = "RF front-end owner: " + ownerApp.title;
	}
	else {
		bar.dot.className = "ap-dot idle";
		bar.hold.textContent = "radio idle";
		bar.hold.title = r.note || "";
	}

	// board vitals (from the same state doc)
	const b = (state && state.board) || {};
	bar.vitals.textContent = "";
	const parts = [];
	if(b.load) { parts.push(["load ", b.load[0].toFixed(2) + "/" + b.ncpu]); }
	if(b.mem_avail !== undefined) { parts.push(["mem ", fmtMem(b.mem_avail) + " free"]); }
	for(const z in b.thermal || {}) { parts.push([z.replace(/-thermal$/, "") + " ", b.thermal[z].toFixed(0) + "°C"]); }
	if(b.uptime !== undefined) { parts.push(["up ", fmtUp(b.uptime)]); }
	parts.forEach((p, i) => {
		if(i) { bar.vitals.append(" · "); }
		bar.vitals.append(p[0]);
		bar.vitals.appendChild(el("b", null, p[1]));
	});

	for(const a of cardApps()) {
		const ce = ui[a.id];
		if(!ce) { continue; }
		const active = a.state === "active";
		const crashing = isCrashing(a);
		const busy = pending || !!r.transition;
		const activating = (a.state === "activating" && !crashing) || (r.transition && r.transition.to === a.id);

		ce.mdot.className = "mdot" + (active ? " run" : crashing || a.state === "failed" ? " fail" : activating ? " wait" : "");

		// one visible primary action per state: stop on the running/crashing app, activate on
		// everything else (state lives in the menu dot, action lives in the header).
		// radio:"none" infrastructure apps (always-on daemons) get NO activate/stop at all -
		// their panel owns the real controls (e.g. Clock Gen toggles the OUT9 output).
		const show = (btn, on) => { btn.style.display = on ? "" : "none"; };
		if(a.radio === "none" && appdOk && a.installed) {
			// always-on services are boot-started (deploy_www wants-symlinks): running is the
			// normal case and not worth a chip - anything else IS the signal
			ce.chip.style.display = active ? "none" : "";
			ce.chip.className = a.state === "failed" ? "ap-chip fail" : "ap-chip off";
			ce.chip.textContent = a.state;
			show(ce.bAct, false); show(ce.bStop, false); show(ce.bOpen, !!a.open && active); show(ce.bLog, true);
			ce.bLog.disabled = false;
			const mod0 = MODULES[a.panel];
			const live0 = active && mod0 && mod0.live;
			ce.live.style.display = live0 ? "block" : "none";
			ce.idle.style.display = live0 ? "none" : "block";
			ce.mdot.className = "mdot" + (active ? " run" : a.state === "failed" ? " fail" : "");
			renderError(a.id);
			continue;
		}
		if(!appdOk || !a.installed) {
			ce.chip.className = "ap-chip off";
			ce.chip.textContent = appdOk ? "not installed" : "unknown";
			show(ce.bAct, false); show(ce.bStop, false); show(ce.bOpen, false); show(ce.bLog, appdOk);
			ce.bLog.disabled = !appdOk;
		}
		else if(crashing) {
			ce.chip.className = "ap-chip fail";
			ce.chip.textContent = "crash-looping";
			show(ce.bAct, false); show(ce.bStop, true); show(ce.bOpen, false); show(ce.bLog, true);
			ce.bStop.disabled = busy;
			ce.bLog.disabled = false;
		}
		else if(activating) {
			ce.chip.className = "ap-chip wait";
			ce.chip.textContent = "starting";
			show(ce.bAct, false); show(ce.bStop, false); show(ce.bOpen, false); show(ce.bLog, true);
			ce.bLog.disabled = false;
		}
		else if(active) {
			ce.chip.className = "ap-chip run";
			ce.chip.textContent = "running";
			show(ce.bAct, false); show(ce.bStop, true); show(ce.bOpen, !!a.open); show(ce.bLog, true);
			ce.bStop.disabled = busy;
			ce.bOpen.disabled = false;
			ce.bLog.disabled = false;
		}
		else if(a.state === "failed") {
			ce.chip.className = "ap-chip fail";
			ce.chip.textContent = "failed";
			show(ce.bAct, true); show(ce.bStop, false); show(ce.bOpen, false); show(ce.bLog, true);
			ce.bAct.disabled = busy;   // activating (any app) also clears the failed unit server-side
			ce.bLog.disabled = false;
		}
		else {
			ce.chip.className = "ap-chip stop";
			ce.chip.textContent = "stopped";
			show(ce.bAct, true); show(ce.bStop, false); show(ce.bOpen, false); show(ce.bLog, true);
			// tx_allowed is a policy string since the path-aware TX policy: "0" forbids, "1"/"embed"/... allow
			ce.bAct.disabled = busy || (a.radio === "tx" && state && state.board && (!state.board.tx_allowed || state.board.tx_allowed === "0"));
			ce.bLog.disabled = false;
		}

		ce.metaEl.textContent = "";
		if(active || a.state === "activating") {
			const bits = [];
			if(a.uptime !== undefined) { bits.push(["up ", fmtUp(a.uptime)]); }
			if(a.cpu !== undefined) { bits.push(["cpu ", Number(a.cpu).toFixed(0) + "%"]); }
			if(a.mem !== undefined) { bits.push(["mem ", fmtMem(a.mem)]); }
			if(a.tasks !== undefined) { bits.push(["", a.tasks + " tasks"]); }
			bits.forEach((x, i) => {
				if(i) { ce.metaEl.append(" · "); }
				ce.metaEl.append(x[0]);
				ce.metaEl.appendChild(el("b", null, x[1]));
			});
		}
		ce.buildEl.textContent = a.build && a.build.commit ? "build " + a.build.commit + (a.build.built ? " · " + a.build.built : "") : "";

		// config rows: reflect current values; disabled while anything is in flight
		for(const c of a.config || []) {
			const cu = ce.cfgUi[c.key];
			if(!cu) { continue; }
			for(const v in cu.btns) {
				cu.btns[v].className = c.value === v ? "on" : "";
				cu.btns[v].disabled = busy || !a.installed;
			}
			cu.hint.textContent = CONFIG_HINTS[c.key + ":" + c.value] ? " " + CONFIG_HINTS[c.key + ":" + c.value] : "";
		}

		if(a.state === "failed") {
			cardErr[a.id] = { msg: "unit failed (" + (a.result || "error") + ")", log: a.log || [] };
		}
		else if(crashing) {
			cardErr[a.id] = { msg: "keeps exiting right after start - last attempt:", log: a.log || [] };
		}
		else if(cardErr[a.id] && !cardErr[a.id].manual) {
			delete cardErr[a.id];
		}
		if(active && cardErr[a.id]) { delete cardErr[a.id]; }
		renderError(a.id);

		ce.bLog.classList.toggle("on", openLogs.has(a.id));
		ce.logPre.style.display = openLogs.has(a.id) ? "block" : "none";

		const mod = MODULES[a.panel];
		const showLive = active && mod && mod.live;
		ce.live.style.display = showLive ? "block" : "none";
		ce.idle.style.display = showLive ? "none" : "block";
		if(!showLive && ce.panel) {
			ce.panel = null;
			ce.live.textContent = "";
		}
	}
	updateTakeover();
	updateRfControl();
	maybeAutoRtsa();
}

// ---- RTSA auto-reacquire -----------------------------------------------------------
// Idle-stop (by design: no browser = no CPU) stops the radio owner once every tab is
// gone. Coming BACK to the RTSA view then used to dead-end on a red conn dot: nothing
// re-took the lease, and the ws client never auto-reconnects (stale-tab protection).
// Looking at the RTSA view IS the intent: if the radio is idle, take it; once rtsa is
// active and the dot is still red, poke the (visible-tab-only) reconnect. Never steals -
// an owner that isn't rtsa keeps the explicit takeover overlay.
let rtsaKick = 0, connKick = 0;
function maybeAutoRtsa() {
	if(curView !== "rtsa" || document.hidden || !appdOk || pending) { return; }
	const r = radioInfo();
	const now = Date.now();
	if(!r.owner && !r.transition && appOf("rtsa")) {
		if(now - rtsaKick > 15000) {
			rtsaKick = now;
			doRadio("rtsa");
		}
		return;
	}
	if(r.owner === "rtsa" && r.state === "active" && now - connKick > 5000) {
		const conn = document.getElementById("conn");
		if(conn && conn.classList.contains("bad")) {
			connKick = now;
			conn.click();   // spectrum.js: reconnect only if the socket is closed
		}
	}
}

function renderError(id) {
	const ce = ui[id];
	const err = cardErr[id];
	if(!err) {
		ce.fail.style.display = "none";
		return;
	}
	const print = err.msg + "|" + (err.log || []).length;
	if(ce.fail.dataset.print !== print) {
		ce.fail.dataset.print = print;
		ce.fail.textContent = "";
		const head = el("div", "ap-err", err.msg);
		const x = el("button", "ap-x", "×");
		x.title = "dismiss";
		x.onclick = () => { delete cardErr[id]; renderError(id); };
		head.appendChild(x);
		ce.fail.appendChild(head);
		for(const line of err.log || []) { ce.fail.appendChild(el("div", null, line)); }
	}
	ce.fail.style.display = "block";
}

// ---- RTSA takeover overlay ----------------------------------------------------------
const rtsaView = document.getElementById("view-rtsa");
const overlay = el("div");
overlay.id = "rfbusy";
overlay.style.cssText = "position:absolute; inset:0; display:none; z-index:20; " +
	"background:rgba(4,5,10,0.78); backdrop-filter:blur(3px); " +
	"flex-direction:column; align-items:center; justify-content:center; gap:12px;";
const busyMsg = el("div", null, "");
busyMsg.style.cssText = "color:#9ab0c4; font-size:13px;";
const busyBtn = el("button", "ap-btn", "stop it & start RTSA");
busyBtn.style.cssText = "font-size:13px; padding:6px 16px;";
busyBtn.onclick = () => doRadio("rtsa");
overlay.append(busyMsg, busyBtn);
rtsaView.appendChild(overlay);

function updateTakeover() {
	const r = radioInfo();
	const ownerApp = r.owner && r.owner !== "rtsa" ? appOf(r.owner) : null;
	busyBtn.disabled = pending || !!r.transition;
	if(ownerApp) {
		busyMsg.textContent = "RF path is in use by " + ownerApp.title;
		overlay.style.display = "flex";
	}
	else {
		overlay.style.display = "none";
	}
}

// ---- OpenWebRX built-in panel ---------------------------------------------------------
function renderOwrx(id) {
	const ce = ui[id];
	if(!ce || ce.panel) { return; }
	ce.live.textContent = "";
	const port = (appOf(id) || {}).open || 8073;
	const btn = el("button", "ap-bigopen", "open receiver → :" + port);
	btn.onclick = () => window.open("http://" + location.hostname + ":" + port + "/", "_blank");
	ce.live.append(btn,
		el("div", "ap-hint", "multi-user: several browsers can listen at once, each with its own demodulator."));
	ce.panel = { built: true };
}

// ---- poll loops -------------------------------------------------------------------------
window.addEventListener("viewchange", e => {
	curView = e.detail;
	viewVisible = curView === "apps";
	if(viewVisible) { pollState(); pollLive(); }
	if(curView === "rtsa") { pollState(); }   // arriving on RTSA: evaluate auto-reacquire now
});

function pollLive() {
	if(!viewVisible || document.hidden || !selected) { return; }
	const a = appOf(selected);
	const mod = a && MODULES[a.panel];
	if(a && mod && mod.live && a.state === "active") { mod.live(a.id); }
}

// /v1/state every 3 s while the page is open - this IS the appd presence heartbeat that
// keeps the radio owner alive (idle_stop_s); it must not be gated on view visibility
setInterval(pollState, 3000);
setInterval(pollLive, 2000);
setInterval(() => { if(viewVisible && !document.hidden) { for(const id of openLogs) { refreshLog(id); } } }, 5000);

pollState();

})();
