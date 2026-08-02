// Board I/O panel: an HTML replica of the motherboard's LEFT EDGE (the bottom-side
// connector/header strip) - silk-accurate rows, PCB styling, click-to-select. No photo:
// pins are DOM elements, so selecting/toggling is direct. Live sources: iod (/api/app/boardio/) for
// GPIO state, clockgend (/api/app/clockgen/) for the REF CLK OUT row. GUARDED pins (lines
// reserved by board policy) and PLANNED pins (DT/module phases pending) are visually distinct
// and not drivable. Full pin table remains below for the flat view.
"use strict";
(function() {

const APP = () => window.RFNM_APPS;

const ICON = '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.2"><rect x="1.5" y="4" width="13" height="8" rx="1.2"/><path d="M4 4 V2.2 M6.7 4 V2.2 M9.4 4 V2.2 M12 4 V2.2 M4 13.8 V12 M6.7 13.8 V12 M9.4 13.8 V12 M12 13.8 V12"/></svg>';

window.__RFNM_APP_MODULES = window.__RFNM_APP_MODULES || [];
window.__RFNM_APP_MODULES.push({
	id: "boardio", title: "Board I/O", icon: ICON,
	desc: "The bottom-edge header, live: GPIO visualize/toggle now; PWM3, GPT1 capture, PPS and clock I/O as their driver phases land.",
	live: render,
});

// The strip, silk-exact from the board render (top -> bottom). kind: coax | pwr | pin | gap.
// io = iod pin id (live/controllable); live:"clkgen" pulls the Clock Gen state instead.
const STRIP = [
	{ kind: "coax", key: "ref_in",  silk: "REF CLK IN",  note: "Si5510 external reference input - lock badge lands with the driver attrs (0x16/0x12)" },
	{ kind: "coax", key: "ref_out", silk: "REF CLK OUT", note: "Si5510 OUT9 - driven by the Clock Gen app", live: "clkgen" },
	{ kind: "gap" },
	{ kind: "pin", key: "gnd0", silk: "GND", gnd: true },
	{ kind: "pin", key: "aux_p", silk: "AUX CLKOUT P", planned: true, note: "differential Si5510 output - which OUTn = schematic question (parked)" },
	{ kind: "pin", key: "aux_n", silk: "AUX CLKOUT N", planned: true, note: "differential Si5510 output - which OUTn = schematic question (parked)" },
	{ kind: "pin", key: "gnd1", silk: "GND", gnd: true },
	{ kind: "pin", key: "pps_out", silk: "PPS OUT", io: "pps_out", note: "source TBD: LA9310 phytimer C14 already emits 1 PPS - scope this pin (see phytimer-pps study)" },
	{ kind: "pin", key: "gnd2", silk: "GND", gnd: true },
	{ kind: "pin", key: "pps_in", silk: "PPS IN", io: "gpio4_27", note: "PPS_IN select A (SAI2_MCLK) - input-only until the schematic confirms the net" },
	{ kind: "gap" },
	{ kind: "pin", key: "u4tx", silk: "UART4 TX", io: "uart4_tx", note: "uart4 is unused by Linux - pad freely switchable UART4 <-> GPIO5_29" },
	{ kind: "pin", key: "u4rx", silk: "UART4 RX", io: "uart4_rx", note: "uart4 is unused by Linux - pad freely switchable UART4 <-> GPIO5_28" },
	{ kind: "pin", key: "u2tx", silk: "UART2 TX", io: "uart2_tx", note: "free GPIO - the Linux console actually lives on the SD1 pads (DTS)" },
	{ kind: "pin", key: "u2rx", silk: "UART2 RX", io: "uart2_rx", note: "free GPIO - the Linux console actually lives on the SD1 pads (DTS)" },
	{ kind: "pin", key: "ulatx", silk: "UART LA TX", uart: true, note: "LA9310-side console UART - not an i.MX pad" },
	{ kind: "pin", key: "ularx", silk: "UART LA RX", uart: true, note: "LA9310-side console UART - not an i.MX pad" },
	{ kind: "pin", key: "gnd3", silk: "GND", gnd: true },
	{ kind: "gap" },
	{ kind: "pin", key: "gpt1_cmp3", silk: "GPT1 CMP3", io: "gpt1_cmp3", note: "the SPDIF_EXT_CLK pad turned out to be the ETH PHY reset on this board (DTS) - which pad really feeds this header pin needs the schematic" },
	{ kind: "pin", key: "i2c4_sda", silk: "I2C4 SDA", io: "i2c4_sda", note: "I2C4 - controller not in the board DTS yet; pad usable as GPIO5_21 meanwhile" },
	{ kind: "pin", key: "i2c4_scl", silk: "I2C4 SCL", io: "i2c4_scl", note: "I2C4 - controller not in the board DTS yet; pad usable as GPIO5_20 meanwhile" },
	{ kind: "pin", key: "gpio4_26", silk: "GPIO4 26", io: "gpio4_26" },
	{ kind: "pin", key: "gpio4_30", silk: "GPIO4 30", io: "gpio4_30" },
	{ kind: "pin", key: "pwm3", silk: "PWM3", io: "pwm3", note: "PWM3_OUT on the SPDIF_TX pad (best guess pending schematic) - live since the 2026-07-10 DT" },
	{ kind: "pin", key: "gpt1_capt1", silk: "GPT1 CAPT1", io: "gpt1_capt1", note: "GPT1 CAPTURE1 (SAI3_TXC) - timestamping module pending; pad usable as GPIO5_00 meanwhile" },
];

const css = `
.bio-wrap { display: flex; gap: 18px; align-items: flex-start; flex-wrap: wrap; }
.bio-edge { flex: none; width: 288px; position: relative; padding: 14px 12px 16px 10px;
	background:
		repeating-linear-gradient(0deg, rgba(255,255,255,0.014) 0 2px, transparent 2px 4px),
		radial-gradient(140% 90% at 110% 10%, rgba(64,120,200,0.20) 0%, transparent 60%),
		linear-gradient(105deg, #16325c 0%, #1c4076 48%, #152e57 100%);
	border: 1px solid #2a4d85; border-radius: 10px;
	box-shadow: inset 0 0 40px rgba(5,12,26,0.6), 0 2px 14px rgba(0,0,0,0.35); }
.bio-edge::before { content: ""; position: absolute; left: 0; top: 10px; bottom: 10px; width: 5px;
	background: linear-gradient(180deg, #d8b968, #8a6f2f 30%, #d8b968 60%, #8a6f2f);
	border-radius: 3px 0 0 3px; opacity: 0.55; }   /* plated board edge */
.bio-brand { color: #e8f1fc; font-size: 10px; letter-spacing: 3px; opacity: 0.6; margin: 0 0 10px 12px;
	font-weight: bold; }
.bio-hdr { color: #7fa0cc; font-size: 9px; letter-spacing: 2.4px; margin: 8px 0 3px 12px; opacity: 0.85; }
.bio-row { display: flex; align-items: center; gap: 9px; padding: 3px 6px 3px 10px; border-radius: 6px;
	cursor: pointer; border: 1px solid transparent; position: relative; }
.bio-row.static { cursor: default; opacity: 0.8; }
.bio-row:not(.static):hover { background: rgba(255,255,255,0.07); }
.bio-row.sel { background: rgba(16,185,129,0.12); border-color: rgba(16,185,129,0.8);
	box-shadow: 0 0 10px rgba(16,185,129,0.15); }
.bio-pad { flex: none; width: 13px; height: 13px; border-radius: 50%;
	background: radial-gradient(circle at 50% 50%, #16283f 0 24%, #e6c878 30% 62%, #96772f 66% 100%);
	box-shadow: 0 0 3px rgba(0,0,0,0.6); border: 0; }   /* drilled gold pad */
.bio-pad.gndp { background: radial-gradient(circle at 50% 50%, #16283f 0 24%, #8b97ab 30% 62%, #525f75 66% 100%); }
.bio-pad.hi { background: radial-gradient(circle at 50% 50%, #0c2517 0 22%, #45e88b 28% 66%, #1d9a55 70% 100%);
	box-shadow: 0 0 9px rgba(16,185,129,0.9); }
.bio-pad.lo { background: radial-gradient(circle at 50% 50%, #0a111f 0 24%, #2c4266 30% 62%, #1a2940 66% 100%); }
.bio-pad.guard { outline: 2px solid rgba(255,212,121,0.85); outline-offset: 1.5px; }
.bio-pad.plan { background: transparent; border: 1.5px dashed #7a8aa5; box-shadow: none; border-radius: 50%; }
.bio-coax { flex: none; width: 28px; height: 28px; display: flex; }
.bio-coax svg { display: block; filter: drop-shadow(0 1px 3px rgba(0,0,0,0.55)); }
.bio-coax.on svg { filter: drop-shadow(0 0 7px rgba(16,185,129,0.9)); }
.bio-coax.on svg .ckl { stroke: rgba(16,185,129,0.9); }
.bio-silk { color: #eef5fd; font-size: 11.5px; letter-spacing: 0.7px; white-space: nowrap;
	font-weight: 500; text-shadow: 0 0 3px rgba(0,0,0,0.7); }
.bio-row.static .bio-silk { color: #9cb2c8; font-weight: 400; }
.bio-live { margin-left: auto; font-size: 10px; color: #10b981; white-space: nowrap; font-variant-numeric: tabular-nums; }
.bio-live.alt { color: #b8c6dc; opacity: 0.75; }
.bio-gap { height: 7px; margin: 3px 10px 3px 12px; border-bottom: 1px solid rgba(216,185,104,0.18); }
.bio-pwr { display: grid; grid-template-columns: auto auto; gap: 1px 12px; margin: 3px 0 3px 42px; }
.bio-pwr span { color: #8fa8cc; font-size: 9.5px; letter-spacing: 0.6px; }
.bio-muxchip { display: inline-flex; align-items: center; gap: 5px; background: #16283f; color: #9ab0c4;
	border: 1px solid #2a4260; border-radius: 5px; font-size: 11px; padding: 3px 10px; cursor: pointer; }
.bio-muxchip.cur { color: #10b981; border-color: #10b981; cursor: default; }
.bio-muxchip.grey { opacity: 0.65; }
.bio-muxchip.grey:hover { opacity: 1; border-color: #3f6f9e; }
.bio-detail { flex: 1 1 340px; min-width: 300px; background: #0c1220; border: 1px solid #2a4260;
	border-radius: 8px; padding: 14px 16px; }
.bio-detail h4 { margin: 0 0 4px; color: #dae3ec; font-size: 15px; }
.bio-kv { font-size: 12px; color: #9ab0c4; margin: 2px 0; }
.bio-kv b { color: #dae3ec; font-weight: normal; font-variant-numeric: tabular-nums; }
.bio-note { font-size: 11.5px; color: #62798f; margin-top: 8px; line-height: 1.5; }
.bio-ctl { margin-top: 12px; display: flex; gap: 6px; }
.bio-btn { background: #16283f; color: #9ab0c4; border: 1px solid #2a4260; border-radius: 5px;
	font: inherit; font-size: 11px; padding: 3px 12px; cursor: pointer; }
.bio-btn.on { color: #10b981; border-color: #10b981; }
.bio-btn:disabled { opacity: 0.35; cursor: not-allowed; }
.bio-err { color: #ef8ba1; font-size: 12px; min-height: 15px; margin-top: 6px; }
.bio-grid { margin-top: 14px; display: grid; grid-template-columns: repeat(auto-fill, minmax(210px, 1fr)); gap: 8px; }
.bio-tile { background: #0c1220; border: 1px solid #1e2a3f; border-radius: 8px; padding: 8px 10px; cursor: pointer; }
.bio-tile:hover { border-color: #3f6f9e; }
.bio-tile.sel { border-color: #10b981; background: rgba(16,185,129,0.05); }
.bio-tile.guard { border-left: 3px solid rgba(255,212,121,0.7); }
.bio-tile.plan { border-style: dashed; opacity: 0.6; cursor: default; }
.bio-tile-h { display: flex; align-items: center; gap: 7px; }
.bio-tile-name { color: #dae3ec; font-size: 12px; font-weight: 600; letter-spacing: 0.3px; }
.bio-tile-state { margin-left: auto; color: #62798f; font-size: 10.5px; white-space: nowrap; }
.bio-tile-role { color: #4d5e78; font-size: 10.5px; margin-top: 4px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.bio-legend { font-size: 11px; color: #62798f; display: flex; gap: 16px; flex-wrap: wrap; margin-top: 10px; }
`;
(function() { const s = document.createElement("style"); s.textContent = css; document.head.appendChild(s); })();

let busy = false;
let selKey = "gpio4_26";

function pinById(pins, id) { return (pins || []).find(p => p.id === id) || null; }
function stripEntry(key) { return STRIP.find(e => e.key === key) || null; }

function ensurePanel(id) {
	const a = APP(), ce = a.ui[id];
	if(ce.panel) { return ce.panel; }
	ce.live.textContent = "";

	const wrap = a.el("div", "bio-wrap");
	const edge = a.el("div", "bio-edge");
	const rows = {};
	for(const e of STRIP) {
		if(e.kind === "gap") {
			edge.appendChild(a.el("div", "bio-gap"));
			continue;
		}

		const row = a.el("div", "bio-row" + (e.gnd || e.uart ? " static" : ""));
		const pad = a.el("span", e.kind === "coax" ? "bio-coax" : "bio-pad" + (e.gnd ? " gndp" : ""));
		if(e.kind === "coax") {
			pad.innerHTML = '<svg viewBox="0 0 30 30" width="28" height="28">' +
				'<circle cx="15" cy="15" r="13.4" fill="#1c2740" stroke="#0a111f" stroke-width="1"/>' +
				'<circle cx="15" cy="15" r="12" fill="none" stroke="#8e99ad" stroke-width="2.6" stroke-dasharray="2.4 1.6"/>' +
				'<circle cx="15" cy="15" r="9" fill="#d9d2c0" stroke="#9a927e" stroke-width="1.4"/>' +
				'<circle cx="15" cy="15" r="5.6" fill="#f4efe2" stroke="#b7ae97" stroke-width="1"/>' +
				'<circle cx="15" cy="15" r="1.9" fill="#c9a54a" stroke="#8a6f2f" stroke-width="0.8"/>' +
				'<circle class="ckl" cx="15" cy="15" r="13" fill="none" stroke="rgba(16,185,129,0)" stroke-width="1.6"/></svg>';
		}
		row.append(pad, a.el("span", "bio-silk", e.silk));
		const liveTag = a.el("span", "bio-live", "");
		row.appendChild(liveTag);
		if(!e.gnd && !e.uart) {
			row.onclick = () => { selKey = e.key; render(id); };
		}
		edge.appendChild(row);
		rows[e.key] = { row, pad, liveTag, e };
	}

	const detail = a.el("div", "bio-detail");
	const legend = a.el("div", "bio-legend");
	legend.innerHTML = "<span><b style='color:#10b981'>&#9632;</b> high</span><span><b style='color:#3f6f9e'>&#9632;</b> low</span>" +
		"<span><b style='color:#c9a54a'>&#9632;</b> pad</span>" +
		"<span><b style='color:#e4b664'>&#9633;</b> guarded (reserved - read-only)</span>" +
		"<span style='opacity:0.7'><b>&#9633;</b> planned (DT/module pending)</span>";
	const grid = a.el("div", "bio-grid");
	const right = a.el("div");
	right.style.cssText = "flex:1 1 340px; min-width:300px;";
	right.append(detail, grid, legend);
	wrap.append(edge, right);
	ce.live.appendChild(wrap);
	ce.panel = { rows, detail, grid, print: "" };
	return ce.panel;
}

function setPwm(body, err) {
	if(busy) { return; }
	busy = true;
	APP().fetchJSON("/api/app/boardio/pwm", { method: "POST", body: JSON.stringify(body) }, 8000)
		.catch(e => { err.textContent = e.message; setTimeout(() => { err.textContent = ""; }, 5000); })
		.finally(() => { busy = false; });
}

function setMux(ioId, to, err) {
	if(busy) { return; }
	busy = true;
	APP().fetchJSON("/api/app/boardio/mux", { method: "POST", body: JSON.stringify({ id: ioId, to }) }, 8000)
		.catch(e => { err.textContent = e.message; setTimeout(() => { err.textContent = ""; }, 5000); })
		.finally(() => { busy = false; });
}

function setPin(ioId, body, err) {
	if(busy) { return; }
	busy = true;
	APP().fetchJSON("/api/app/boardio/set", { method: "POST", body: JSON.stringify(Object.assign({ id: ioId }, body)) }, 8000)
		.catch(e => { err.textContent = e.message; setTimeout(() => { err.textContent = ""; }, 5000); })
		.finally(() => { busy = false; });
}

function renderDetail(a, p, pins, clk, pwm) {
	const d = p.detail;
	d.textContent = "";
	const e = stripEntry(selKey);
	if(!e) { return; }
	const pin = e.io ? pinById(pins, e.io) : null;
	d.appendChild(a.el("h4", null, e.silk));
	if(e.key === "ref_out") {
		d.appendChild(a.el("div", "bio-kv")).innerHTML = "This connector is <b>Si5510 OUT9</b> - driven by an app: <b>Clock Gen</b> (1-200.099 MHz, 1 kHz grid).<br>Output now: <b>" +
			(clk && clk.enabled ? (clk.freq_khz / 1000).toFixed(3) + " MHz" : "off") + "</b>";
		const ctl = a.el("div", "bio-ctl");
		const go = a.el("button", "bio-btn on", "open Clock Gen \u25B8");
		go.onclick = () => { if(a.selectApp) { a.selectApp("clockgen"); } };
		ctl.appendChild(go);
		d.appendChild(ctl);
	} else if(e.key === "ref_in") {
		d.appendChild(a.el("div", "bio-kv")).innerHTML = "Si5510 lock to external ref: <b>n/a</b> (driver attrs 0x16/0x12 pending)";
	} else if(e.key === "pwm3" && pwm && pwm.found) {
		const kv = a.el("div", "bio-kv");
		const freq = pwm.period_ns > 0 ? (1e9 / pwm.period_ns) : 0;
		const duty = pwm.period_ns > 0 ? (100 * pwm.duty_ns / pwm.period_ns) : 0;
		kv.innerHTML = "state: <b>" + (pwm.enable ? "ENABLED" : "off") + "</b>" +
			(pwm.period_ns > 0 ? " · <b>" + (freq >= 1000 ? (freq / 1000).toFixed(3) + " kHz" : freq.toFixed(1) + " Hz") + "</b> · duty <b>" + duty.toFixed(1) + "%</b>" : "");
		d.appendChild(kv);
		const err = a.el("div", "bio-err", "");
		const ctl = a.el("div", "bio-ctl");
		const fIn = document.createElement("input");
		fIn.className = "ckg-freq";
		fIn.style.width = "110px";
		fIn.type = "number";
		fIn.placeholder = "freq Hz";
		fIn.value = pwm.period_ns > 0 ? Math.round(1e9 / pwm.period_ns) : 1000;
		const dIn = document.createElement("input");
		dIn.className = "ckg-freq";
		dIn.style.width = "80px";
		dIn.type = "number";
		dIn.placeholder = "duty %";
		dIn.min = "0"; dIn.max = "100";
		dIn.value = pwm.period_ns > 0 ? Math.round(100 * pwm.duty_ns / pwm.period_ns) : 50;
		const bGo = a.el("button", "bio-btn on", "apply + enable");
		const bOff = a.el("button", "bio-btn", "disable");
		bGo.onclick = () => {
			const f = parseFloat(fIn.value), dc = parseFloat(dIn.value);
			if(!(f > 0) || !(dc >= 0 && dc <= 100)) { err.textContent = "need freq > 0 Hz and duty 0-100%"; return; }
			const period = Math.round(1e9 / f);
			setPwm({ period_ns: period, duty_ns: Math.round(period * dc / 100), enable: 1 }, err);
		};
		bOff.onclick = () => setPwm({ enable: 0, duty_ns: -1, period_ns: 0 }, err);
		ctl.append(fIn, dIn, bGo, bOff);
		d.append(ctl, err);
	} else if(pin && !pin.planned) {
		const kv = a.el("div", "bio-kv");
		kv.innerHTML = "pad: <b>" + pin.label + "</b>" +
			(pin.muxable ? "<br>pad function now: <b>" + (pin.mux_gpio ? pin.gpio_name + " (GPIO)" : pin.mux === -1 ? "?" : (pin.mux === 5 ? "GPIO" : pin.primary)) + "</b>" : "") +
			((pin.mux_gpio || !pin.muxable) ? "<br>direction: <b>" + pin.dir + "</b> · value: <b>" + (pin.val < 0 ? "-" : pin.val) + "</b>" : "") +
			(pin.guarded ? " · <b style='color:#e4b664'>GUARDED</b>" : "");
		d.appendChild(kv);
		if(pin.muxable) {
			// the "grey pinmux alternative": current function solid, the other selectable
			const err0 = a.el("div", "bio-err", "");
			const mux = a.el("div", "bio-ctl");
			const cPri = a.el("button", "bio-muxchip" + (!pin.mux_gpio ? " cur" : " grey"), pin.primary);
			const cGpio = a.el("button", "bio-muxchip" + (pin.mux_gpio ? " cur" : " grey"), pin.gpio_name + " (GPIO)");
			cPri.onclick = () => { if(pin.mux_gpio) { setMux(pin.id, "primary", err0); } };
			cGpio.onclick = () => { if(!pin.mux_gpio) { setMux(pin.id, "gpio", err0); } };
			mux.append(cPri, cGpio);
			d.append(mux, err0);
		}
		if(pin.muxable && !pin.mux_gpio) {
			if(e.note) { d.appendChild(a.el("div", "bio-note", e.note)); }
			return;
		}
		const err = a.el("div", "bio-err", "");
		const ctl = a.el("div", "bio-ctl");
		const bIn = a.el("button", "bio-btn" + (pin.dir === "in" ? " on" : ""), "input");
		const b0 = a.el("button", "bio-btn" + (pin.dir === "out" && pin.val === 0 ? " on" : ""), "drive 0");
		const b1 = a.el("button", "bio-btn" + (pin.dir === "out" && pin.val === 1 ? " on" : ""), "drive 1");
		bIn.onclick = () => setPin(pin.id, { dir: "in" }, err);
		b0.onclick = () => setPin(pin.id, { dir: "out", val: 0 }, err);
		b1.onclick = () => setPin(pin.id, { dir: "out", val: 1 }, err);
		b0.disabled = b1.disabled = pin.guarded;
		ctl.append(bIn, b0, b1);
		d.append(ctl, err);
		if(pin.guarded) { d.appendChild(a.el("div", "bio-note", "Guarded: this line is reserved on this board - view-only from here.")); }
	} else {
		d.appendChild(a.el("div", "bio-kv", pin ? "pad: " + pin.label : ""));
	}
	if(e.note) { d.appendChild(a.el("div", "bio-note", e.note)); }
}

function render(id) {
	const a = APP();
	if(a.liveBusy[id]) { return; }
	a.liveBusy[id] = true;
	const p = ensurePanel(id);
	Promise.all([
		a.fetchJSON("/api/app/boardio/status", undefined, 6000),
		a.fetchJSON("/api/app/clockgen/status", undefined, 6000).catch(() => null),
	]).then(([st, clk]) => {
		const pins = st.pins || [];
		const print = JSON.stringify(pins) + "|" + JSON.stringify(clk) + "|" + JSON.stringify(st.pwm3) + "|" + selKey;
		if(p.print === print) { return; }
		p.print = print;

		for(const key in p.rows) {
			const { row, pad, liveTag, e } = p.rows[key];
			row.classList.toggle("sel", key === selKey);
			if(e.kind === "coax") {
				if(e.live === "clkgen") {
					const on = clk && clk.enabled;
					pad.className = "bio-coax" + (on ? " on" : "");
					liveTag.textContent = on ? (clk.freq_khz / 1000).toFixed(3) + " MHz" : "";
				}
				continue;
			}
			if(e.gnd || e.uart) { continue; }
			if(e.key === "pwm3" && st.pwm3 && st.pwm3.found) {
				pad.className = "bio-pad" + (st.pwm3.enable ? " hi" : " lo");
				liveTag.textContent = st.pwm3.enable && st.pwm3.period_ns > 0 ? Math.round(1e9 / st.pwm3.period_ns) + " Hz" : "";
				continue;
			}
			const pin = e.io ? pinById(pins, e.io) : null;
			let cls = "bio-pad";
			liveTag.textContent = "";   // state lives in the pad color + tooltip + detail card
			let tip = e.silk;
			if(pin && !pin.planned && pin.muxable && !pin.mux_gpio) {
				tip = e.silk + " — pad on " + pin.primary;
			} else if(pin && !pin.planned) {
				cls += pin.val === 1 ? " hi" : pin.val === 0 ? " lo" : "";
				if(pin.guarded) { cls += " guard"; }
				tip = e.silk + " — " + (pin.muxable ? pin.gpio_name + ", " : "") + pin.dir + (pin.val >= 0 ? " = " + pin.val : "");
			} else if(!pin || pin.planned || e.planned) {
				cls += " plan";
				tip = e.silk + " — pending";
			}
			row.title = tip;
			pad.className = cls;
		}

		renderDetail(a, p, pins, clk, st.pwm3);

		if(!a.selectionInside(p.grid)) {
			p.grid.textContent = "";
			for(const pin of pins) {
				const se = STRIP.find(x => x.io === pin.id);
				const t = a.el("div", "bio-tile" + (se && se.key === selKey ? " sel" : "") +
					(pin.planned ? " plan" : "") + (pin.guarded ? " guard" : ""));
				const head = a.el("div", "bio-tile-h");
				const dot = a.el("span", "bio-pad" + (pin.planned ? " plan" : pin.val === 1 ? " hi" : pin.val === 0 ? " lo" : "") + (pin.guarded ? " guard" : ""));
				dot.style.cssText = "width:9px;height:9px;";
				head.append(dot, a.el("span", "bio-tile-name", pin.silk));
				const state = pin.planned ? "pending" :
					(pin.muxable && !pin.mux_gpio) ? pin.primary :
					(pin.muxable ? pin.gpio_name + " · " : "") + pin.dir + (pin.val >= 0 ? " · " + pin.val : "");
				head.appendChild(a.el("span", "bio-tile-state", state));
				const role = a.el("div", "bio-tile-role", pin.label);
				t.append(head, role);
				t.title = pin.label;
				t.onclick = () => { if(se) { selKey = se.key; render(id); } };
				p.grid.appendChild(t);
			}
		}
	}).catch(() => {}).finally(() => { a.liveBusy[id] = false; });
}

})();
