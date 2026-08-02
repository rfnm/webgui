// Clock Gen panel: Si5510 OUT9 siggen on the REF CLK OUT connector, via clockgend
// (/api/app/clockgen/). The daemon always runs (radio:"none"); the toggle here is the OUT9 OUTPUT.
// Reference badge: ext_ref (external 10 MHz on REF CLK IN disciplining the board, auto-
// revertive) + ref_locked (reference-PLL lock); both are null on boards whose kernel
// lacks the si5510 status attrs (Lime today) -> dim "n/a".
"use strict";
(function() {

const APP = () => window.RFNM_APPS;

const ICON = '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.3"><path d="M1.5 8 h2 l1.5 -4 3 8 1.5 -4 h5"/><circle cx="13" cy="8" r="1.1" fill="currentColor" stroke="none"/></svg>';

window.__RFNM_APP_MODULES = window.__RFNM_APP_MODULES || [];
window.__RFNM_APP_MODULES.push({
	id: "clockgen", title: "Clock Gen", icon: ICON,
	desc: "Si5510 OUT9 signal generator on the REF CLK OUT connector: 1 MHz - 200.099 MHz, 1 kHz grid.",
	live: render,
});

const css = `
.ckg-wrap { display: flex; flex-direction: column; gap: 14px; max-width: 560px; }
.ckg-row { display: flex; align-items: center; gap: 10px; }
.ckg-freq { background: #16283f; color: #dae3ec; border: 1px solid #2a4260; border-radius: 6px;
	font: inherit; font-size: 15px; padding: 6px 10px; width: 170px; font-variant-numeric: tabular-nums; }
.ckg-freq:focus { border-color: #10b981; outline: none; }
.ckg-unit { color: #62798f; font-size: 12px; text-transform: uppercase; letter-spacing: 1px; }
.ckg-hint { color: #62798f; font-size: 11px; }
.ckg-err { color: #ef8ba1; font-size: 12px; min-height: 15px; }
.ckg-ref { display: inline-flex; align-items: center; gap: 7px; font-size: 11px; letter-spacing: 1px; text-transform: uppercase;
	padding: 3px 10px; border-radius: 5px; cursor: default; }
.ckg-ref .ckg-dot { width: 8px; height: 8px; border-radius: 50%; flex: none; }
.ckg-ref.ext { color: #10b981; background: #0d2a23; border: 1px solid #1d5244; }
.ckg-ref.ext .ckg-dot { background: #10b981; }
.ckg-ref.int { color: #9ab0c4; background: #16283f; border: 1px solid #2a4260; }
.ckg-ref.int .ckg-dot { background: #62798f; }
.ckg-ref.na { color: #475d73; background: #15243a; border: 1px solid #1c2c40; }
.ckg-ref.na .ckg-dot { background: #2a3549; }
.ckg-ref.unlock { color: #ef8ba1; background: #1a0f0d; border: 1px solid #4a241e; }
.ckg-ref.unlock .ckg-dot { background: #ef8ba1; }
`;
(function() { const s = document.createElement("style"); s.textContent = css; document.head.appendChild(s); })();

let busy = false;

function ensurePanel(id) {
	const a = APP(), ce = a.ui[id];
	if(ce.panel) { return ce.panel; }
	ce.live.textContent = "";

	const wrap = a.el("div", "ckg-wrap");
	const tiles = a.el("div", "ap-tiles");
	const refRow = a.el("div", "ckg-row");
	const refBadge = a.el("span", "ckg-ref na");
	const refDot = a.el("span", "ckg-dot");
	const refTxt = a.el("span", null, "n/a");
	refBadge.append(refDot, refTxt);
	refRow.append(a.el("span", "ckg-unit", "reference"), refBadge);
	const row = a.el("div", "ckg-row");
	const freq = document.createElement("input");
	freq.className = "ckg-freq";
	freq.type = "number";
	freq.min = "1";
	freq.max = "200.099";
	freq.step = "0.001";
	freq.placeholder = "frequency";
	const bSet = a.el("button", "ap-btn", "set + enable");
	const bOff = a.el("button", "ap-btn stopbtn", "output off");
	const err = a.el("div", "ckg-err", "");
	row.append(freq, a.el("span", "ckg-unit", "MHz"), bSet, bOff);
	wrap.append(tiles, refRow, row, err,
		a.el("div", "ckg-hint", "1.000 - 200.099 MHz on a 1 kHz grid. Retuning is safe while radio apps stream. Output resets to OFF on reboot."));
	ce.live.appendChild(wrap);

	const post = body => {
		if(busy) { return; }
		busy = true;
		err.textContent = "";
		a.fetchJSON("/api/app/clockgen/set", { method: "POST", body: JSON.stringify(body) }, 10000).then(() => {
			render(id);
		}).catch(e => {
			err.textContent = e.message;
		}).finally(() => { busy = false; });
	};
	bSet.onclick = () => {
		const mhz = parseFloat(freq.value);
		if(isNaN(mhz)) { err.textContent = "enter a frequency in MHz"; return; }
		post({ freq_khz: Math.round(mhz * 1000) });
	};
	bOff.onclick = () => post({ enabled: false });

	ce.panel = { tiles, refBadge, refTxt, freq, err, print: "" };
	return ce.panel;
}

function render(id) {
	const a = APP();
	if(a.liveBusy[id]) { return; }
	a.liveBusy[id] = true;
	const p = ensurePanel(id);
	a.fetchJSON("/api/app/clockgen/status", undefined, 6000).then(st => {
		const print = JSON.stringify(st);
		if(p.print === print) { return; }
		p.print = print;
		p.tiles.textContent = "";
		p.tiles.append(
			a.tile(st.enabled ? "ON" : "OFF", "OUT9 output", !st.sysfs_ok),
			a.tile(st.enabled ? (st.freq_khz / 1000).toFixed(3) + " MHz" : "—", "frequency"),
		);
		let cls = "na", txt = "n/a", tip = "reference status: driver support pending on this board";
		if(st.ref_locked === 0) {
			cls = "unlock";
			txt = "reference PLL unlocked";
			tip = "the Si5510 reference PLL reports unlocked (Device-API 0x16) - board clocks are not trustworthy right now";
		} else if(st.ext_ref === 1) {
			cls = "ext";
			txt = "external 10 MHz";
			tip = "locked to the 10 MHz reference on the REF CLK IN connector (selection is automatic + auto-revertive: unplug it and the board falls back to the internal TCXO by itself)";
		} else if(st.ext_ref === 0) {
			cls = "int";
			txt = "internal TCXO";
			tip = "running on the onboard TCXO - plug a 10 MHz reference into REF CLK IN to discipline the board; switchover is automatic";
		}
		p.refBadge.className = "ckg-ref " + cls;
		p.refBadge.title = tip;
		p.refTxt.textContent = txt;
		if(!st.sysfs_ok) { p.err.textContent = "siggen sysfs missing - is the rev4 si5510 driver running?"; }
		if(document.activeElement !== p.freq && st.enabled) { p.freq.value = (st.freq_khz / 1000).toFixed(3); }
	}).catch(() => {}).finally(() => { a.liveBusy[id] = false; });
}

})();
