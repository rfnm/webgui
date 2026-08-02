// numfield.js — shared instrument-style numeric field helpers (RTSA + VSG pages).
// Parsing convention (one convention everywhere, people think in MHz): a plain number means
// MHz / MS-per-s ("millions" of the field's on-screen unit), comma or dot decimals accepted
// ("868,3" = 868.3); a k/M/G suffix means absolute Hz ("2.45G", "185k"). Steppers (.vstep
// buttons with data-for), wheel tuning and arrow-key tuning (data-step; shift = x10, ctrl = /10)
// write a plain number back and dispatch a normal "change" event so each page's apply plumbing
// runs unchanged.
// Unit-field pattern (THE way to mark up a frequency / sample-rate field): the label carries no
// unit ("frequency", not "frequency MHz"); the unit letter lives IN the field text, hard against
// the digits ("2450M", "3G") — the page calls NumField.bindUnit(input) and every commit rewrites
// the typed text as mantissa+letter ("123445k" -> "123.445M", "3g" -> "3G", plain "2450" ->
// "2450M"). A typed unit sticks when its mantissa is sane; silly mantissas rescale to the natural
// letter. Suffixed text is absolute under the parsing convention above, so the committed text
// parses identically everywhere (apply plumbing, wheel, steppers) with no per-field unit state.
// Fields whose bare-number unit is Hz (vsg offset / mod rates / bandwidths) mark it with
// data-base="1": plain "75000" still means 75 kHz and displays as "75k"; sub-1k values stay bare.
// Daemons never see the letters — pages convert with readAbs() before sending.
"use strict";
(function() {
	function parseSuffixed(txt) {
		const m = String(txt).trim().replace(",", ".").match(/^([+-]?[0-9]*\.?[0-9]+(?:[eE][+-]?[0-9]+)?)\s*([gGmMkK])?$/);
		if(!m) {
			return { v: NaN, hz: false };
		}
		if(!m[2]) {
			return { v: parseFloat(m[1]), hz: false };
		}
		const u = m[2].toLowerCase() === "k" ? "k" : m[2].toUpperCase();
		const mul = { G: 1e9, M: 1e6, k: 1e3 }[u];
		return { v: parseFloat(m[1]) * mul, hz: true, u: u };
	}
	function parseFreqMHz(txt) {
		const p = parseSuffixed(txt);
		return p.hz ? p.v / 1e6 : p.v;
	}
	function parseRateHz(txt) {
		const p = parseSuffixed(txt);
		return p.hz ? p.v : p.v * 1e6;	// plain number = MS/s, suffix = absolute Hz
	}
	// current field value in its display unit ("millions"): suffixed absolute Hz folds back to M
	function dispVal(inp) {
		const p = parseSuffixed(inp.value);
		return p.hz ? p.v / 1e6 : p.v;
	}
	function clampTo(inp, v) {
		if(inp.dataset.min !== undefined) { v = Math.max(v, parseFloat(inp.dataset.min)); }
		if(inp.dataset.max !== undefined) { v = Math.min(v, parseFloat(inp.dataset.max)); }
		return v;
	}
	// keyboard tuning on a stepper-bound field: ArrowUp/Down step by data-step (shift = x10,
	// ctrl = /10, same modifiers as the wheel), clamp, write a plain number back; "change"
	// fires once keys settle (200 ms) so held-key auto-repeat coalesces into one apply
	function bindArrows(inp) {
		if(inp.dataset.arrowsBound) {
			return;
		}
		inp.dataset.arrowsBound = "1";
		let t = null;
		inp.addEventListener("keydown", (e) => {
			if(e.key !== "ArrowUp" && e.key !== "ArrowDown") {
				return;
			}
			e.preventDefault();
			let v = dispVal(inp);
			if(!isFinite(v)) {
				v = 0;
			}
			const base = parseFloat(inp.dataset.step) || 1;
			const step = (e.shiftKey ? base * 10 : (e.ctrlKey ? base / 10 : base)) * (e.key === "ArrowUp" ? 1 : -1);
			v = clampTo(inp, Math.round((v + step) * 1e4) / 1e4);
			inp.value = String(v);
			clearTimeout(t);
			t = setTimeout(() => { inp.dispatchEvent(new Event("change")); }, 200);
		});
	}
	// wire every .vstep under rootSel to its data-for input: step by data-step, clamp, change;
	// the same pass gives each referenced input arrow-key tuning
	function bindSteppers(rootSel) {
		document.querySelectorAll(rootSel + " .vstep").forEach((b) => {
			const binp = document.getElementById(b.dataset.for);
			if(binp) {
				bindArrows(binp);
			}
			b.addEventListener("click", () => {
				const inp = document.getElementById(b.dataset.for);
				if(!inp) {
					return;
				}
				const step = (parseFloat(inp.dataset.step) || 1) * (b.classList.contains("vdn") ? -1 : 1);
				let v = dispVal(inp);
				if(!isFinite(v)) {
					v = 0;
				}
				v = clampTo(inp, Math.round((v + step) * 1e6) / 1e6);
				inp.value = String(v);
				inp.dispatchEvent(new Event("change"));
			});
		});
	}
	// unit-field normalize: rewrite the field as mantissa+unit-letter. The typed letter sticks
	// ("3G" stays 3G); a plain number stays in the field's base unit (default M, data-base="1"
	// = Hz); a mantissa outside [1, 10000) — or a bare Hz value >= 1000 — rescales to the
	// largest letter that keeps it >= 1 ("123445k" -> "123.445M", "0.5G" -> "500M", Hz-base
	// "75000" -> "75k"). Steppers/wheel write plain base-unit numbers and dispatch "change",
	// so their writes flow through here and pick the letter back up.
	function normalize(inp) {
		const base = parseFloat(inp.dataset.base) || 1e6;
		const p = parseSuffixed(inp.value);
		if(!isFinite(p.v)) {
			return;	// unparseable text ("auto", empty, garbage) stays as typed
		}
		const abs = p.hz ? p.v : p.v * base;
		const a = Math.abs(abs);
		let u = p.u || (base === 1e6 ? "M" : "");
		let mul = u ? { G: 1e9, M: 1e6, k: 1e3 }[u] : 1;
		if(a > 0 && (a / mul >= 10000 || a / mul < 1 || (!u && a >= 1000))) {
			u = base === 1e6 ? "M" : "";
			mul = base;
			for(const [su, sm] of [["G", 1e9], ["M", 1e6], ["k", 1e3]]) {
				if(a / sm >= 1) { u = su; mul = sm; break; }
			}
		}
		inp.value = String(Math.round((abs / mul) * 1e6) / 1e6) + u;
	}
	// absolute value of a unit-field's text (suffix = absolute, plain = base unit): pages hand
	// the daemon plain numbers via this — the unit letter is strictly a GUI affair
	function readAbs(inp) {
		const p = parseSuffixed(inp.value);
		return p.hz ? p.v : p.v * (parseFloat(inp.dataset.base) || 1e6);
	}
	// unit-field binding: normalized text is suffixed = absolute under the shared convention, so
	// the page's own "change" handler parses raw and normalized text to the same value — listener
	// order doesn't matter. Normalizing at bind time gives the initial HTML value its letter.
	function bindUnit(inp) {
		normalize(inp);
		inp.addEventListener("change", () => normalize(inp));
	}
	// wheel tuning on the field itself: data-step per tick, shift = x10, ctrl = /10; the value
	// updates per tick but "change" fires once the wheel settles (200 ms) so apply paths see one hit
	function bindWheel(inp) {
		let t = null;
		inp.addEventListener("wheel", (e) => {
			e.preventDefault();
			let v = dispVal(inp);
			if(!isFinite(v)) {
				return;
			}
			const base = parseFloat(inp.dataset.step) || 1;
			const step = (e.shiftKey ? base * 10 : (e.ctrlKey ? base / 10 : base)) * (e.deltaY < 0 ? 1 : -1);
			v = clampTo(inp, Math.round((v + step) * 1e4) / 1e4);
			inp.value = String(v);
			clearTimeout(t);
			t = setTimeout(() => { inp.dispatchEvent(new Event("change")); }, 200);
		}, { passive: false });
	}
	window.NumField = { parseSuffixed, parseFreqMHz, parseRateHz, bindSteppers, bindWheel, bindArrows, bindUnit, normalize, readAbs };
})();
