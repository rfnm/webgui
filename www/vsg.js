// VSG view: drives the local vsgd daemon through the /api/app/vsg/ nginx proxy.
// All controls push partial PUT /config documents (debounced; flat /set-vocabulary keys); status polls at 1 Hz while the view is
// active. TX toggling hands the radio between vsgd and the RTSA spectrum daemon server-side.
"use strict";
(function() {
	const $ = (id) => document.getElementById(id);
	const API = "/api/app/vsg";
	let active = (location.hash.replace("#", "") === "vsg");
	let pollTimer = null;
	let debTimer = null;
	let lastStatus = null;
	let qecMsgHold = 0;	// keep a button error visible past the next status renders
	let currentMode = "cw";
	let txpathLocked = false;	// daemon says the board policy pins the TX path — never send txpath then
	let lastPos = null;		// previous poll's pos_placed/pos_dropped (placement % is a delta)
	let lastPlanNote = null;	// rate-plan clamp note, toasted once per distinct message

	const MODES = [
		["cw", "CW"], ["am", "AM"], ["fm", "FM"], ["pulse", "Pulse"],
		["multitone", "Multitone"], ["sweep", "Ramp sweep"], ["awgn", "AWGN"],
		["digmod", "Digital mod"], ["fsk", "FSK / GMSK"], ["ofdm", "OFDM"], ["ble", "BLE beacon"], ["arb", "Player"],
	];
	(function buildModeList() {
		const nav = $("vsgmodes");
		for(const [id, label] of MODES) {
			const el = document.createElement("div");
			el.className = "mitem" + (id === currentMode ? " active" : "");
			el.id = "mitem-" + id;
			el.textContent = label;
			el.onclick = () => { setMode(id); if(id === "ble") { bleFreqFromCh(); } push(); };
			nav.appendChild(el);
		}
	})();

	// BLE advertising channels live at fixed frequencies — picking one tunes the carrier.
	// (The burst ring pre-renders at 16 MS/s and resamples at build time, so any DAC rate is
	// fine — the old "cap the rate at 16 MS/s" hack died with the transparent-rate daemon.)
	const BLE_CH_MHZ = { "37": 2402, "38": 2426, "39": 2480 };
	function bleFreqFromCh() {
		const mhz = BLE_CH_MHZ[$("v_ble_ch").value];
		if(mhz) {
			$("v_freq").value = String(mhz);
			window.NumField.normalize($("v_freq"));
		}
	}

	function setMode(id) {
		currentMode = id;
		for(const [m] of MODES) {
			document.getElementById("mitem-" + m).classList.toggle("active", m === id);
		}
		showGroup(id);
		const label = (MODES.find((x) => x[0] === id) || ["", "?"])[1];
		$("vparam_title").textContent = label + " parameters";
		if(id === "arb") {
			refreshFiles();
		}
		prevState.peak = null;	// peak hold clears on mode change
		if(id === "digmod" || id === "ofdm") {
			fetchConstellation();
		}
		drawPrev();
	}

	function fmtHz(v) {
		if(v >= 1e6) {
			return (v / 1e6).toFixed(2) + " MHz";
		}
		if(v >= 1e3) {
			return (v / 1e3).toFixed(1) + " kHz";
		}
		return v + " Hz";
	}

	// unit-suffix parsing ("2.45G" = absolute Hz, plain number = MHz / MS/s, comma decimals):
	// shared with the RTSA page via numfield.js — keep the two pages' fields identical.
	const { parseFreqMHz } = window.NumField;

	// unit fields (numfield.js pattern: letter lives in the text, data-base="1" = bare Hz);
	// hzTxt hands the daemon a plain base-unit number — "auto"/garbage/empty pass through raw
	const UFIELDS = ["v_freq", "v_offset", "v_am_rate", "v_fm_rate", "v_fm_dev", "v_mt_space", "v_sweep_span",
		"v_awgn_bw", "v_symrate", "v_fsk_symrate", "v_ofdm_bw", "v_arb_rate", "v_pn_bw"];
	function hzTxt(id) {
		const v = window.NumField.readAbs($(id));
		return isFinite(v) ? String(v) : $(id).value.trim();
	}

	// PUT the (partial) config document; the reply is the post-apply DOCUMENT, not status —
	// poll() right after so health/plan fields render from the normal /status path
	async function apiPut(doc) {
		try {
			await fetch(API + "/config", { method: "PUT", body: JSON.stringify(doc), cache: "no-store" });
		} catch(e) { /* vsgd down — the next poll reports it */ }
		return poll();
	}

	async function api(pathAndQuery) {
		try {
			const r = await fetch(API + pathAndQuery);
			const j = await r.json();
			const wasDown = lastStatus === null;
			lastStatus = j;
			if(wasDown) {
				refreshPresets();   // daemon just came up: the offline "preset list failed" is stale
			}
			render(j);
			return j;
		} catch(e) {
			// vsgd is not running: the TX button becomes the way to start it (via the appd lease)
			lastStatus = null;
			const tx = $("vtx");
			tx.className = "down";
			tx.textContent = "VSG offline — start";
			tx.disabled = false;
			$("vstat").innerHTML = '<span class="err">generator not running — the button hands it the radio</span>';
			return null;
		}
	}

	// ---- signal view (TX render + live RX monitor; served by /signal, fc-centered) ----
	let prevVer = -1;
	let prevRxVer = -1;
	let lastRxT = 0;
	const prevState = { domain: "spec", psd: null, rate: 61440000, env: null, envRate: 61440000, rx: null, peak: null, hover: null };

	const RXHINT_STALL = $("v_rx_hint").textContent;	// default = benign rebuild-transient wording
	function rxHint(show, sick) {
		$("v_rx_hint").textContent = sick
			? "monitor data broken — RX stream not contiguous (board needs a driver reload)"
			: RXHINT_STALL;
		$("v_rx_hint").style.display = show ? "" : "none";
		// a stalled pane must not impersonate a live one: dim the last frame while no fresh
		// RX data arrives (plan rebuild after a freq/rate/power change takes a few seconds)
		$("vprev_rx").style.opacity = show ? "0.35" : "";
	}

	async function fetchPreview() {
		try {
			if(prevState.domain === "time") {
				const r = await fetch(API + "/signal?domain=time");
				const p = await r.json();
				if(p.env && p.env.length) {
					prevState.env = p.env;
					prevState.envRate = p.rate || prevState.envRate;
				}
			} else {
				const r = await fetch(API + "/signal");
				const p = await r.json();
				if(p.db && p.db.length) {
					prevState.psd = p.db;
					prevState.rate = p.rate || prevState.rate;
					if($("v_pk_hold").checked) {
						if(!prevState.peak || prevState.peak.length !== p.db.length) {
							prevState.peak = [...p.db];
						} else {
							for(let i = 0; i < p.db.length; i++) {
								if(p.db[i] > prevState.peak[i]) { prevState.peak[i] = p.db[i]; }
							}
						}
					}
				}
			}
			drawPrev();
		} catch(e) { /* preview is best-effort */ }
	}

	async function fetchRxPreview() {
		try {
			const r = await fetch(API + "/signal?src=rx");
			const p = await r.json();
			prevState.rx = (p.db && p.db.length) ? p.db : null;
			drawRxPane();
			if(!prevState.rx) {
				rxHint(true);
			}
			drawPrev();
		} catch(e) {
			rxHint(true);
		}
	}

	function prevScale(arr) {
		const srt = [...arr].sort((a, b) => a - b);
		const floor = srt[Math.floor(srt.length * 0.25)];	// percentile floor: outliers can't stretch the axis
		let mx = srt[srt.length - 1];
		let mn = Math.max(floor - 12, mx - 90);
		mx += 6; mn -= 6;
		return [mn, mx];
	}

	function ringMs(rate) {
		return (lastStatus && lastStatus.arb_len && rate) ? lastStatus.arb_len / rate * 1e3 : 0;
	}

	function drawPrev() {
		const cv = $("vprev");
		const ctx = cv.getContext("2d");
		const W = cv.width, H = cv.height;
		ctx.fillStyle = "#04070c";
		ctx.fillRect(0, 0, W, H);
		const timeDom = (prevState.domain === "time");
		const dataFull = timeDom ? prevState.env : prevState.psd;
		if(!dataFull || !dataFull.length) {
			return;
		}
		// spectrum panes crop 5% per side: hides the anti-alias rolloff shoulders and the
		// unsampled fs/32 sliver (owner: never show the cliff, zoom instead)
		const CROP = timeDom ? 0 : Math.round(dataFull.length * 0.10);
		const spec = (a) => (CROP && a) ? a.slice(CROP, a.length - CROP) : a;
		const data = spec(dataFull);
		const [mn, mx] = prevScale(data);
		const rateFull = timeDom ? prevState.envRate : prevState.rate;
		const rate = rateFull * (data.length / dataFull.length);
		// grid
		ctx.strokeStyle = "#141c2c";
		ctx.fillStyle = "#62798f";
		ctx.font = "10px ui-monospace, monospace";
		for(let g = 0; g <= 4; g++) {
			const y = g * H / 4;
			ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
			if(g < 4) {	// bottom row label would clip against the canvas edge
				ctx.fillText((mx - g * (mx - mn) / 4).toFixed(0) + (timeDom ? " dBFS" : " dB"), 4, y + 11);
			}
		}
		for(let g = 0; g <= 4; g++) {
			const x = g * W / 4;
			ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
			if(timeDom) {
				const ms = ringMs(rate);
				const lbl = ms > 0 ? (g * ms / 4).toFixed(2) + "ms" : (g * 25) + "%";
				ctx.fillText(lbl, Math.min(x + 3, W - 56), H - 5);
			} else {
				const mhz = (g - 2) * rate / 4 / 1e6;
				ctx.fillText((mhz > 0 ? "+" : "") + mhz.toFixed(1) + "M", Math.min(x + 3, W - 44), H - 5);
			}
		}
		if(!timeDom) {
			// center marker
			ctx.strokeStyle = "#2a4260";
			ctx.beginPath(); ctx.moveTo(W / 2, 0); ctx.lineTo(W / 2, H); ctx.stroke();
		}
		const yOf = (db) => Math.min(H - 1, Math.max(1, (mx - db) / (mx - mn) * H));
		const trace = (arr, color, width) => {
			ctx.strokeStyle = color;
			ctx.lineWidth = width;
			ctx.beginPath();
			for(let i = 0; i < arr.length; i++) {
				const x = i * W / (arr.length - 1);
				const y = yOf(arr[i]);
				if(i === 0) { ctx.moveTo(x, y); } else { ctx.lineTo(x, y); }
			}
			ctx.stroke();
		};
		if(!timeDom && prevState.peak && $("v_pk_hold").checked) {
			trace(spec(prevState.peak), "#b8873a", 1.0);
		}
		// RTSA-style main trace: gradient fill anchored at the trace top fading to the baseline,
		// then a brighter stroke with a soft glow on top
		{
			let yTop = H;
			for(let i = 0; i < data.length; i++) {
				const y = yOf(data[i]);
				if(y < yTop) { yTop = y; }
			}
			const grad = ctx.createLinearGradient(0, yTop, 0, H);
			grad.addColorStop(0, "rgba(55, 214, 122, 0.35)");
			grad.addColorStop(1, "rgba(55, 214, 122, 0)");
			ctx.fillStyle = grad;
			ctx.beginPath();
			ctx.moveTo(0, H);
			for(let i = 0; i < data.length; i++) {
				ctx.lineTo(i * W / (data.length - 1), yOf(data[i]));
			}
			ctx.lineTo(W, H);
			ctx.closePath();
			ctx.fill();
			ctx.shadowColor = "#10b981";
			ctx.shadowBlur = 4;
			trace(data, "#4ef08e", 1.5);
			ctx.shadowBlur = 0;
			ctx.shadowColor = "transparent";
		}
		ctx.fillStyle = "#4ef08e";
		ctx.font = "10px ui-monospace, monospace";
		ctx.fillText("preview", W - 50, 12);

		// hover crosshair + tooltip: drawn from cached data, no refetch
		if(prevState.hover) {
			const hx = Math.max(0, Math.min(W - 1, prevState.hover.x));
			const bin = Math.max(0, Math.min(data.length - 1, Math.round(hx / W * (data.length - 1))));
			const db = data[bin];
			const y = yOf(db);
			ctx.strokeStyle = "#3f6f9e";
			ctx.lineWidth = 1;
			ctx.beginPath(); ctx.moveTo(hx, 0); ctx.lineTo(hx, H); ctx.stroke();
			ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
			let lbl;
			if(timeDom) {
				const ms = ringMs(rate);
				lbl = (ms > 0 ? (bin / (data.length - 1) * ms).toFixed(3) + " ms, " : "") + db.toFixed(1) + " dBFS";
			} else {
				const foff = (bin / (data.length - 1) - 0.5) * rate;
				lbl = (foff >= 0 ? "+" : "") + (foff / 1e6).toFixed(3) + " MHz, " + db.toFixed(1) + " dB";
			}
			ctx.font = "11px ui-monospace, monospace";
			const tw = ctx.measureText(lbl).width;
			ctx.fillStyle = "#ecf2f8";
			ctx.fillText(lbl, Math.max(2, Math.min(hx + 8, W - tw - 4)), Math.max(14, y - 6));
		}
		updatePrevReadout();
	}

	// live-RX pane: its own graph below the TX render, appears while the monitor runs
	// (owner: two graphs, no overlay). Same house style, own dB scale, same 5% crop.
	let rxHover = null;
	function drawRxPane() {
		const cv = $("vprev_rx");
		const on = !!(lastStatus && lastStatus.monitor === 1 && lastStatus.tx === 1 && prevState.rx);
		cv.style.display = on ? "" : "none";
		if(!on) {
			return;
		}
		const ctx = cv.getContext("2d");
		const W = cv.width, H = cv.height;
		ctx.fillStyle = "#04070c";
		ctx.fillRect(0, 0, W, H);
		const CROP = Math.round(prevState.rx.length * 0.10);
		const data = prevState.rx.slice(CROP, prevState.rx.length - CROP).map((v) => v <= -100 ? NaN : v);
		const rate = prevState.rate * (data.length / prevState.rx.length);
		const fin = data.filter((v) => !isNaN(v));
		if(!fin.length) {
			return;
		}
		const [mn, mx] = prevScale(fin);
		ctx.strokeStyle = "#141c2c";
		ctx.fillStyle = "#62798f";
		ctx.font = "10px ui-monospace, monospace";
		for(let g = 0; g <= 3; g++) {
			const y = g * H / 3;
			ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
			if(g < 3) {
				ctx.fillText((mx - g * (mx - mn) / 3).toFixed(0) + " dB", 4, y + 11);
			}
		}
		for(let g = 0; g <= 4; g++) {
			const x = g * W / 4;
			ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
			const mhz = (g - 2) * rate / 4 / 1e6;
			ctx.fillText((mhz > 0 ? "+" : "") + mhz.toFixed(1) + "M", Math.min(x + 3, W - 44), H - 5);
		}
		ctx.strokeStyle = "#2a4260";
		ctx.beginPath(); ctx.moveTo(W / 2, 0); ctx.lineTo(W / 2, H); ctx.stroke();
		const yOf = (db) => Math.min(H - 1, Math.max(1, (mx - db) / (mx - mn) * H));
		let yTop = H;
		for(const v of data) { if(!isNaN(v)) { const y = yOf(v); if(y < yTop) { yTop = y; } } }
		const grad = ctx.createLinearGradient(0, yTop, 0, H);
		grad.addColorStop(0, "rgba(74, 163, 232, 0.30)");
		grad.addColorStop(1, "rgba(74, 163, 232, 0)");
		ctx.fillStyle = grad;
		ctx.beginPath();
		ctx.moveTo(0, H);
		for(let i = 0; i < data.length; i++) {
			ctx.lineTo(i * W / (data.length - 1), isNaN(data[i]) ? H : yOf(data[i]));
		}
		ctx.lineTo(W, H);
		ctx.closePath();
		ctx.fill();
		ctx.shadowColor = "#4aa3e8";
		ctx.shadowBlur = 4;
		ctx.strokeStyle = "#6cacd8";
		ctx.lineWidth = 1.5;
		ctx.beginPath();
		let pen = 0;
		for(let i = 0; i < data.length; i++) {
			if(isNaN(data[i])) { pen = 0; continue; }
			const x = i * W / (data.length - 1), y = yOf(data[i]);
			if(!pen) { ctx.moveTo(x, y); pen = 1; } else { ctx.lineTo(x, y); }
		}
		ctx.stroke();
		ctx.shadowBlur = 0;
		ctx.shadowColor = "transparent";
		ctx.fillStyle = "#6cacd8";
		ctx.font = "10px ui-monospace, monospace";
		ctx.fillText("measured", W - 58, 12);
		if(rxHover) {
			const hx = Math.max(0, Math.min(W - 1, rxHover.x));
			const bin = Math.max(0, Math.min(data.length - 1, Math.round(hx / W * (data.length - 1))));
			if(!isNaN(data[bin])) {
				const y = yOf(data[bin]);
				ctx.strokeStyle = "#3f6f9e";
				ctx.lineWidth = 1;
				ctx.beginPath(); ctx.moveTo(hx, 0); ctx.lineTo(hx, H); ctx.stroke();
				ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
				const foff = (bin / (data.length - 1) - 0.5) * rate;
				const lbl = (foff >= 0 ? "+" : "") + (foff / 1e6).toFixed(3) + " MHz, " + data[bin].toFixed(1) + " dB";
				ctx.font = "11px ui-monospace, monospace";
				const tw = ctx.measureText(lbl).width;
				ctx.fillStyle = "#ecf2f8";
				ctx.fillText(lbl, Math.max(2, Math.min(hx + 8, W - tw - 4)), Math.max(14, y - 6));
			}
		}
	}

	function updatePrevReadout() {
		const p = prevState.psd;
		if(!p || !p.length) {
			return;
		}
		const rate = prevState.rate;
		const binHz = rate / p.length;	// PSD is 4096-pt averaged into 512 bins: rate/512 per displayed bin
		let pkI = 0;
		for(let i = 1; i < p.length; i++) {
			if(p[i] > p[pkI]) { pkI = i; }
		}
		// OBW99: span containing 99% of total linear power, grown outward from the power centroid
		let tot = 0, cen = 0;
		const lin = new Array(p.length);
		for(let i = 0; i < p.length; i++) {
			lin[i] = Math.pow(10, p[i] / 10);
			tot += lin[i];
			cen += lin[i] * i;
		}
		let lo = Math.max(0, Math.min(p.length - 1, Math.round(cen / tot)));
		let hi = lo;
		let acc = lin[lo];
		while(acc < 0.99 * tot && (lo > 0 || hi < p.length - 1)) {
			const nl = lo > 0 ? lin[lo - 1] : -1;
			const nh = hi < p.length - 1 ? lin[hi + 1] : -1;
			if(nl >= nh) { lo--; acc += nl; } else { hi++; acc += nh; }
		}
		const obw = (hi - lo + 1) * binHz / 1e6;
		const pkOff = (pkI / (p.length - 1) - 0.5) * rate / 1e6;
		$("v_prev_ro").textContent =
			`peak ${p[pkI].toFixed(1)} dB @ ${(pkOff >= 0 ? "+" : "")}${pkOff.toFixed(2)} MHz · OBW99 ${obw.toFixed(2)} MHz · bin ${(binHz / 1e3).toFixed(1)} kHz`;
	}

	(function bindPrevInput() {
		const cv = $("vprev");
		{
			const rcv = $("vprev_rx");
			rcv.addEventListener("mousemove", (e) => {
				const r = rcv.getBoundingClientRect();
				rxHover = { x: (e.clientX - r.left) * rcv.width / r.width };
				drawRxPane();
			});
			rcv.addEventListener("mouseleave", () => { rxHover = null; drawRxPane(); });
		}
		cv.addEventListener("mousemove", (e) => {
			const r = cv.getBoundingClientRect();
			prevState.hover = { x: (e.clientX - r.left) * cv.width / r.width };
			drawPrev();
		});
		cv.addEventListener("mouseleave", () => {
			prevState.hover = null;
			drawPrev();
		});
		$("v_pk_hold").addEventListener("change", () => {
			prevState.peak = ($("v_pk_hold").checked && prevState.psd) ? [...prevState.psd] : null;
			drawPrev();
		});
		$("v_prev_dom").addEventListener("change", () => {
			prevState.domain = $("v_prev_dom").value;
			fetchPreview();
		});
	})();

	// ---- constellation (digmod/ofdm only) ----
	let constBusy = false;
	async function fetchConstellation() {
		if(constBusy) {
			return;
		}
		constBusy = true;
		try {
			const r = await fetch(API + "/constellation");
			const j = await r.json();
			drawConstellation(j.n ? j.iq : null);
		} catch(e) { /* best-effort */ }
		constBusy = false;
	}
	function drawConstellation(iq) {
		const cv = $("vconst");
		const ctx = cv.getContext("2d");
		const W = cv.width, H = cv.height;
		ctx.fillStyle = "#04070c";
		ctx.fillRect(0, 0, W, H);
		ctx.strokeStyle = "#2a4260";
		ctx.beginPath(); ctx.moveTo(W / 2, 0); ctx.lineTo(W / 2, H); ctx.stroke();
		ctx.beginPath(); ctx.moveTo(0, H / 2); ctx.lineTo(W, H / 2); ctx.stroke();
		if(!iq || !iq.length) {
			return;
		}
		ctx.fillStyle = "#10b981";
		const scale = W / 2 / 1.4;	// points come normalized to ±1; keep headroom
		for(let k = 0; k + 1 < iq.length; k += 2) {
			ctx.fillRect(W / 2 + iq[k] * scale - 1.5, H / 2 - iq[k + 1] * scale - 1.5, 3, 3);
		}
	}

	// transient error toast (policy-rejected config etc.) — same pattern as the APPS-view #aperr toast
	let lastErrToasted = null;
	let vsgErrTimer = null;
	function vsgToast(msg) {
		const t = $("vsgerr");
		t.textContent = msg;
		t.style.display = "block";
		clearTimeout(vsgErrTimer);
		vsgErrTimer = setTimeout(() => { t.style.display = "none"; }, 8000);
	}

	function render(s) {
		if(!s) {
			return;
		}
		// power field clamps to the DEVICE-advertised contract (dBm-engine boards say -50..+22,
		// legacy raw boards their raw span); the HTML range is only the pre-connect fallback
		if(s.power_max > s.power_min) {
			$("v_power").dataset.min = String(s.power_min);
			$("v_power").dataset.max = String(s.power_max);
		}
		// TX path: live selector when free, amber badge when board policy forbids TX outright
		// (embed-only boards: the FE has no embed TX leg, so the daemon refuses TX - #68)
		txpathLocked = !!s.txpath_locked;
		if(s.txpath) {
			const badge = $("v_txpath_badge");
			$("v_txpath_wrap").style.display = txpathLocked ? "none" : "";
			badge.style.display = txpathLocked ? "inline-block" : "none";
		}
		// daemon-side rejection (e.g. txpath forbidden by policy): toast once per distinct message
		if(s.last_error && s.last_error.length) {
			if(s.last_error !== lastErrToasted) {
				lastErrToasted = s.last_error;
				vsgToast(s.last_error);
			}
		} else {
			lastErrToasted = null;	// cleared by a clean apply — a recurrence toasts again
		}
		// autonomous QEC loop readout (daemon-side; the button only forces a re-null)
		if(s.qec !== undefined && performance.now() > qecMsgHold) {
			let t;
			if(s.qec === "tracking" || s.qec === "converging") {
				t = `auto ${s.qec} · LO ${(+s.qec_lo_dbc).toFixed(1)} dBc` +
					(+s.qec_img_dbc < 0 ? ` · img ${(+s.qec_img_dbc).toFixed(1)} dBc` : "");
			} else if(s.qec === "blocked") {
				t = "blocked: " + (s.qec_why || "see /qec");
			} else {
				t = s.qec_why ? `auto — ${s.qec_why}` : "auto — waiting";
			}
			$("v_qec_ro").textContent = t;
		}
		drawRxPane();	// visibility follows monitor/tx state even with no new rx data
		if(s.preview_ver !== undefined && s.preview_ver !== prevVer) {
			prevVer = s.preview_ver;
			fetchPreview();
			if(currentMode === "digmod" || currentMode === "ofdm") {
				fetchConstellation();
			}
		}
		if(s.monitor === 1) {
			if(s.mon_sick === 1) {
				rxHint(true, true);	// daemon's own stream-health verdict: say it, don't guess
			} else if(s.preview_rx_ver > 0 && s.preview_rx_ver !== prevRxVer) {
				prevRxVer = s.preview_rx_ver;
				lastRxT = performance.now();
				rxHint(false);
				fetchRxPreview();
			} else if(!(s.preview_rx_ver > 0) || performance.now() - lastRxT > 3000) {
				rxHint(true);	// monitor on but the RX preview is not advancing
			}
		} else {
			rxHint(false);
			if(prevState.rx) {
				prevState.rx = null;
				drawPrev();
			}
		}
		const tx = $("vtx");
		tx.className = s.tx ? "on" : "off";
		tx.textContent = s.tx ? "TX ON" : "TX OFF";
		tx.disabled = !!s.rendering;
		let line = `mode ${s.mode} · DAC ${(s.rate / 1e6).toFixed(2)} MS/s${s.rate_auto ? " (auto)" : ""}`;
		if(s.interp > 1) {
			line += ` · render ${(s.fs_int / 1e6).toFixed(2)} ×${s.interp}`;
		}
		if(s.rendering) {
			line += " · rendering…";
		}
		if(s.tx) {
			line += ` · air ${s.msps.toFixed(2)} MS/s`;
			// kernel placement truth (pos_placed/pos_dropped are cumulative): delta since last poll
			if(lastPos && s.pos_placed !== undefined) {
				const dp = s.pos_placed - lastPos.p;
				const dd = s.pos_dropped - lastPos.d;
				if(dp + dd > 0) {
					const pct = 100 * dp / (dp + dd);
					line += ` · placed ${pct.toFixed(0)}%`;
					if(pct < 90) {
						line += ' <span class="err">(TX dropping)</span>';
					}
				}
			}
			if(s.msps > 0 && s.msps < s.rate / 1e6 * 0.95) {
				line += ' · <span class="err">UNDERRUN</span>';
			}
		}
		lastPos = s.pos_placed !== undefined ? { p: s.pos_placed, d: s.pos_dropped } : null;
		if(currentMode === "digmod" || currentMode === "fsk") {
			$("v_symrate_ro").textContent = s.sym_eff > 0 ?
				`achieved ${(s.sym_eff / 1e6).toFixed(4)} Msym/s (render ${(s.fs_int / 1e6).toFixed(2)} MS/s ×${s.interp})` :
				"narrow rates render low and LPF-interpolate up";
		}
		if(s.plan_note && s.plan_note.length && s.plan_note !== lastPlanNote) {
			lastPlanNote = s.plan_note;
			vsgToast(s.plan_note);
		}
		if(s.err && s.err.length) {
			line += ` · <span class="err">${s.err}</span>`;
		}
		if(s.arb_len) {
			$("v_arb_info").textContent = `${s.arb_len} samples loaded (${(s.arb_len / s.rate * 1e3).toFixed(1)} ms loop)`;
		}
		if(s.preset_last) {
			$("v_pre_last").textContent = "last: " + s.preset_last;
		}
		$("vstat").innerHTML = line;
	}

	function showGroup(mode) {
		document.querySelectorAll("#view-vsg .pgroup").forEach((el) => {
			el.classList.toggle("show", el.dataset.mode === mode);
		});
	}

	const numTxt = (v) => String(v).trim().replace(",", ".");

	// (no "rate" key anywhere: the GUI has no rate control — the daemon defaults to auto and
	// the GUI simply never touches the knob; manual rates stay machine-API-only. owner ruling.)
	function collect() {
		const o = {};
		o["mode"] = currentMode;
		const fm = parseFreqMHz($("v_freq").value);
		if(isFinite(fm)) {
			o["freq"] = String(fm * 1e6);
		}
		if(!txpathLocked) {	// a policy-locked path is the daemon's business — sending it would only bounce
			o["txpath"] = $("v_txpath").value;
		}
		o["power"] = numTxt($("v_power").value);
		o["level"] = numTxt($("v_level").value);
		o["offset"] = hzTxt("v_offset");
		o["am_shape"] = $("v_am_shape").value;
		o["am_rate"] = hzTxt("v_am_rate");
		o["am_depth"] = $("v_am_depth").value;
		o["fm_rate"] = hzTxt("v_fm_rate");
		o["fm_dev"] = hzTxt("v_fm_dev");
		o["pulse_w"] = $("v_pulse_w").value;
		o["pulse_p"] = $("v_pulse_p").value;
		o["mt_tones"] = $("v_mt_tones").value;
		o["mt_space"] = hzTxt("v_mt_space");
		o["sweep_span"] = hzTxt("v_sweep_span");
		o["sweep_time"] = $("v_sweep_time").value;
		o["awgn_bw_hz"] = hzTxt("v_awgn_bw");
		o["const"] = $("v_const").value;
		o["alpha"] = $("v_alpha").value;
		o["symrate"] = hzTxt("v_symrate");
		o["diff"] = $("v_diff").checked ? 1 : 0;
		o["oqpsk"] = $("v_oqpsk").checked ? 1 : 0;
		o["fsk_bps"] = $("v_fsk_bps").value;
		o["fsk_h"] = $("v_fsk_h").value;
		o["fsk_bt"] = $("v_fsk_bt").value;
		o["fsk_symrate"] = hzTxt("v_fsk_symrate");
		o["ofdm_nfft"] = $("v_ofdm_nfft").value;
		o["ofdm_cp"] = $("v_ofdm_cp").value;
		o["ofdm_win"] = $("v_ofdm_win").value;
		o["ofdm_nsc"] = $("v_ofdm_nsc").value;
		o["ofdm_bps"] = $("v_ofdm_bps").value;
		o["ofdm_pilot"] = $("v_ofdm_pilot").value;
		o["ofdm_bw_hz"] = hzTxt("v_ofdm_bw");
		o["ble_ch"] = $("v_ble_ch").value;
		o["ble_name"] = $("v_ble_name").value.slice(0, 20);	// URLSearchParams percent-encodes it
		o["ble_interval_ms"] = $("v_ble_interval").value;
		if($("v_arb_rate").value.trim()) {
			o["arb_rate"] = hzTxt("v_arb_rate");
		} else {
			o["arb_rate"] = "0";
		}
		// impairments / playback block: live on the daemon side, no re-render
		o["imp_iqgain"] = $("v_imp_iqgain").value;
		o["imp_iqphase"] = $("v_imp_iqphase").value;
		o["imp_dci"] = $("v_imp_dci").value;
		o["imp_dcq"] = $("v_imp_dcq").value;
		o["imp_conj"] = $("v_imp_conj").checked ? 1 : 0;
		o["cn_on"] = $("v_cn_on").checked ? 1 : 0;
		o["cn_db"] = $("v_cn_db").value;
		o["pn_dbc"] = $("v_pn_on").checked ? numTxt($("v_pn_dbc").value) : "0";	// 0 = off on the daemon side
		const pnbw = window.NumField.readAbs($("v_pn_bw"));	// data-base="1": plain number = Hz
		if(isFinite(pnbw) && pnbw > 0) {
			o["pn_bw_hz"] = String(Math.round(pnbw));
		}
		o["monitor"] = $("v_monitor").checked ? 1 : 0;
		o["mon_gain"] = $("v_mon_gain").value;
		o["mon_path"] = $("v_mon_path").value;
		// fm shape shares am_shape server-side; send whichever group is live
		if(currentMode === "fm") {
			o["am_shape"] = $("v_fm_shape").value;
		}
		return o;
	}

	// DELTA pushes (the 07-23 "stuck live RX" root cause): a push must carry ONLY the keys the
	// user actually changed. The old full-collect() push let any stale form — a long-lived tab,
	// a view that never synced — silently overwrite daemon truth for every unrelated key
	// (receipted: a freq commit carried a stale monitor:0 and killed the live-RX monitor).
	// Baseline = the form as of the last sync/push; vsgd PUT /config accepts partials.
	let formBaseline = null;
	function deltaDoc() {
		const full = collect();
		if(!formBaseline) {
			formBaseline = full;	// never synced: nothing to diff against, clobber nothing
			return {};
		}
		const d = {};
		for(const k in full) {
			if(String(full[k]) !== String(formBaseline[k])) {
				d[k] = full[k];
			}
		}
		Object.assign(formBaseline, d);
		return d;
	}

	function push() {
		clearTimeout(debTimer);
		debTimer = setTimeout(() => {
			apiPut(deltaDoc());
		}, 250);
	}

	// wire every input in the view to a debounced push; purely local controls stay out of it
	const LOCAL_ONLY = new Set(["v_pk_hold", "v_prev_dom", "v_pre_name", "v_pre_sel"]);
	document.querySelectorAll("#view-vsg select, #view-vsg input:not([type=file])").forEach((el) => {
		if(LOCAL_ONLY.has(el.id)) {
			return;
		}
		el.addEventListener("change", push);
		el.addEventListener("input", () => {
			if(el.id === "v_imp_iqgain") {
				$("v_imp_iqgain_ro").textContent = parseFloat(el.value).toFixed(2);
			} else if(el.id === "v_imp_iqphase") {
				$("v_imp_iqphase_ro").textContent = parseFloat(el.value).toFixed(1);
			} else if(el.id === "v_ble_name") {
				$("v_ble_name_ct").textContent = el.value.length + "/20";
			}
		});
	});

	$("v_cn_on").addEventListener("change", () => {
		$("v_cn_db").disabled = !$("v_cn_on").checked;
	});

	$("v_pn_on").addEventListener("change", () => {
		const off = !$("v_pn_on").checked;
		$("v_pn_dbc").disabled = off;
		$("v_pn_bw").disabled = off;
	});

	$("v_ble_ch").addEventListener("change", bleFreqFromCh);	// the generic change→push above fires too; freq lands before the debounce

	// mouse-wheel tuning on the freq field (1 MHz, shift = 10, ctrl = 0.1): shared numfield.js
	// binding — it dispatches "change" after the wheel settles, which lands on push() above.
	window.NumField.bindWheel($("v_freq"));
	// unit letter in the text + normalize-on-commit ("123445k" reads back as "123.445M", "3G"
	// stays "3G", Hz-base fields show "75k"/"10M"): shared numfield.js unit-field pattern on
	// every freq/rate/bandwidth field — collect() converts back to plain numbers via hzTxt
	for(const id of UFIELDS) {
		window.NumField.bindUnit($(id));
	}
	// baseline from the (normalized) HTML defaults: a page that never reached the daemon diffs
	// against these, so even its first push carries only what the user really touched
	formBaseline = collect();

	$("vtx").onclick = async () => {
		const tx = $("vtx");
		if(tx.className === "down") {
			// vsgd not running: hand it the radio lease through appd, then resume polling
			tx.disabled = true;
			$("vstat").textContent = "starting the generator…";
			try {
				await fetch("/api/v1/radio", { method: "POST", body: JSON.stringify({ owner: "vsg" }), cache: "no-store" });
			} catch(e) { /* appd unreachable — the next poll reports it */ }
			tx.disabled = false;
			setTimeout(poll, 1000);
			return;
		}
		const want = lastStatus && lastStatus.tx ? 0 : 1;
		await apiPut(Object.assign(deltaDoc(), { tx: String(want) }));
	};

	// ▲▼ steppers: shared numfield.js binding (data-step / data-min / data-max, fires "change")
	window.NumField.bindSteppers("#view-vsg");

	$("v_selftest").onclick = async () => {
		const btn = $("v_selftest");
		btn.disabled = true;
		$("v_selftest_ro").textContent = "selftest running…";
		try {
			const r = await fetch(API + "/selftest");
			const j = await r.json();
			if(j.err) {
				$("v_selftest_ro").textContent = "err: " + j.err;
			} else {
				$("v_selftest_ro").textContent = `ok: rx_peak ${(+j.rx_peak_db).toFixed(1)} dB · corr ${(+j.corr_db).toFixed(1)} dB` + (j.note ? ` · ${j.note}` : "");
			}
		} catch(e) {
			$("v_selftest_ro").textContent = "selftest failed (daemon unreachable)";
		}
		btn.disabled = false;
	};

	// ---- QEC is autonomous (a daemon-side loop converges + tracks whenever TX runs with
	// the monitor); the button only forces a fresh from-zero re-null. The readout follows
	// the loop from the 1 Hz status poll (render()), so there is nothing to poll here. ----
	$("v_qec").onclick = async () => {
		try {
			const r = await fetch(API + "/qec", { method: "POST" });
			const j = await r.json();
			if(j.err) {
				$("v_qec_ro").textContent = "err: " + j.err;
				qecMsgHold = performance.now() + 4000;
			}
		} catch(e) {
			$("v_qec_ro").textContent = "qec failed (daemon unreachable)";
			qecMsgHold = performance.now() + 4000;
		}
	};

	// ---- presets ----
	async function refreshPresets(sel) {
		try {
			const r = await fetch(API + "/presets");
			const j = await r.json();
			const box = $("v_pre_sel");
			box.innerHTML = "";
			for(const n of (j.presets || [])) {
				box.add(new Option(n, n));
			}
			if(sel && (j.presets || []).includes(sel)) {
				box.value = sel;
			}
			if(j.last) {
				$("v_pre_last").textContent = "last: " + j.last;
			}
			if($("v_pre_stat").textContent === "preset list failed") {
				$("v_pre_stat").textContent = "";   // clear the stale offline complaint, keep save/load/del feedback
			}
		} catch(e) {
			$("v_pre_stat").textContent = "preset list failed";
		}
	}
	$("v_pre_save").onclick = async () => {
		const name = $("v_pre_name").value.trim() || $("v_pre_sel").value;
		if(!name) {
			$("v_pre_stat").textContent = "name?";
			return;
		}
		try {
			const r = await fetch(API + "/preset?op=save&name=" + encodeURIComponent(name));
			const j = await r.json();
			$("v_pre_stat").textContent = j.ok ? "saved " + name : ("save failed: " + j.err);
		} catch(e) {
			$("v_pre_stat").textContent = "save failed";
		}
		refreshPresets(name);
	};
	$("v_pre_load").onclick = async () => {
		const name = $("v_pre_sel").value;
		if(!name) {
			return;
		}
		try {
			const r = await fetch(API + "/preset?op=load&name=" + encodeURIComponent(name));
			const j = await r.json();
			$("v_pre_stat").textContent = j.ok ? "loaded " + name : ("load failed: " + j.err);
			if(j.ok) {
				$("v_pre_last").textContent = "last: " + name;
				const s = await api("/status");
				if(s) {
					populateForm(s);
				}
			}
		} catch(e) {
			$("v_pre_stat").textContent = "load failed";
		}
	};
	$("v_pre_del").onclick = async () => {
		const name = $("v_pre_sel").value;
		if(!name) {
			return;
		}
		try {
			const r = await fetch(API + "/preset?op=del&name=" + encodeURIComponent(name));
			const j = await r.json();
			$("v_pre_stat").textContent = j.ok ? "deleted " + name : ("del failed: " + j.err);
		} catch(e) {
			$("v_pre_stat").textContent = "del failed";
		}
		refreshPresets();
	};

	// sync the form from a /status snapshot (initial load and after preset load)
	function populateForm(s) {
		setMode(s.mode);
		$("v_freq").value = (s.freq / 1e6).toString();
		$("v_power").value = s.power;
		$("v_level").value = s.level_db;
		$("v_offset").value = s.offset_hz;
		// (no rate control: the GUI always runs rate auto — a machine-API manual rate shows in
		// the plan line and flips back to auto on the next GUI push)
		// only mirror paths the selector actually offers (an old config may still say "embed")
		if(s.txpath && [...$("v_txpath").options].some((o) => o.value === s.txpath)) { $("v_txpath").value = s.txpath; }
		if(s.imp_iqgain !== undefined) { $("v_imp_iqgain").value = s.imp_iqgain; $("v_imp_iqgain_ro").textContent = (+s.imp_iqgain).toFixed(2); }
		if(s.imp_iqphase !== undefined) { $("v_imp_iqphase").value = s.imp_iqphase; $("v_imp_iqphase_ro").textContent = (+s.imp_iqphase).toFixed(1); }
		if(s.imp_dci !== undefined) { $("v_imp_dci").value = s.imp_dci; }
		if(s.imp_dcq !== undefined) { $("v_imp_dcq").value = s.imp_dcq; }
		if(s.imp_conj !== undefined) { $("v_imp_conj").checked = !!s.imp_conj; }
		if(s.cn_on !== undefined) { $("v_cn_on").checked = !!s.cn_on; $("v_cn_db").disabled = !s.cn_on; }
		if(s.cn_db !== undefined) { $("v_cn_db").value = s.cn_db; }
		if(s.pn_dbc !== undefined) {
			const pnOn = +s.pn_dbc !== 0;
			$("v_pn_on").checked = pnOn;
			if(pnOn) { $("v_pn_dbc").value = s.pn_dbc; }	// off reports 0 — keep the -40 default staged
			$("v_pn_dbc").disabled = !pnOn;
			$("v_pn_bw").disabled = !pnOn;
		}
		if(s.pn_bw_hz !== undefined && +s.pn_bw_hz > 0) { $("v_pn_bw").value = String(Math.round(+s.pn_bw_hz)); }
		if(s.ble_name !== undefined) { $("v_ble_name").value = String(s.ble_name).slice(0, 20); $("v_ble_name_ct").textContent = $("v_ble_name").value.length + "/20"; }
		if(s.monitor !== undefined) { $("v_monitor").checked = !!s.monitor; }
		if(s.mon_gain !== undefined) { $("v_mon_gain").value = s.mon_gain; }
		if(s.mon_path) { $("v_mon_path").value = s.mon_path; }
		if(s.preset_last) { $("v_pre_last").textContent = "last: " + s.preset_last; }
		// per-mode params: best effort, the status keys mirror the config keys
		const map = {
			am_shape: "v_am_shape", am_rate: "v_am_rate", am_depth: "v_am_depth", fm_rate: "v_fm_rate", fm_dev: "v_fm_dev",
			pulse_w: "v_pulse_w", pulse_p: "v_pulse_p", mt_tones: "v_mt_tones", mt_space: "v_mt_space",
			sweep_span: "v_sweep_span", sweep_time: "v_sweep_time", awgn_bw_hz: "v_awgn_bw",
			const: "v_const", alpha: "v_alpha", symrate: "v_symrate", fsk_bps: "v_fsk_bps", fsk_h: "v_fsk_h", fsk_bt: "v_fsk_bt", fsk_symrate: "v_fsk_symrate",
			ofdm_nfft: "v_ofdm_nfft", ofdm_cp: "v_ofdm_cp", ofdm_win: "v_ofdm_win", ofdm_nsc: "v_ofdm_nsc", ofdm_bps: "v_ofdm_bps", ofdm_pilot: "v_ofdm_pilot", ofdm_bw_hz: "v_ofdm_bw",
			ble_ch: "v_ble_ch", ble_interval_ms: "v_ble_interval",
		};
		for(const k in map) {
			if(s[k] !== undefined) {
				$(map[k]).value = s[k];
			}
		}
		if(s.diff !== undefined) { $("v_diff").checked = !!(+s.diff); }
		if(s.oqpsk !== undefined) { $("v_oqpsk").checked = !!(+s.oqpsk); }
		if(currentMode === "fm" && s.am_shape !== undefined) { $("v_fm_shape").value = s.am_shape; }
		// status writes are plain numbers — put the unit letters back on
		for(const id of UFIELDS) {
			window.NumField.normalize($(id));
		}
		formBaseline = collect();	// synced: future pushes diff against daemon-derived truth
	}

	$("v_arb_up").onclick = async () => {
		const f = $("v_arb_file").files[0];
		if(!f) {
			$("v_arb_info").textContent = "pick a file first";
			return;
		}
		const fmt = f.name.toLowerCase().endsWith(".cf32") ? "?fmt=cf32" : "";
		$("v_arb_info").textContent = "uploading " + (f.size / 1e6).toFixed(1) + " MB…";
		try {
			const r = await fetch(API + "/arb" + fmt, { method: "POST", body: f });
			const j = await r.json();
			$("v_arb_info").textContent = j.ok ? `${j.arb_len} samples loaded${fmt ? " (cf32 converted)" : ""}` : ("upload failed: " + j.err);
			setMode("arb");
			push();
			poll();
		} catch(e) {
			$("v_arb_info").textContent = "upload failed";
		}
	};

	async function refreshFiles() {
		try {
			const r = await fetch(API + "/files");
			const j = await r.json();
			$("v_usb_stat").textContent = j.usb ? `mounted, ${j.free_gb} GB free` : "no drive mounted";
			const box = $("v_usb_files");
			box.innerHTML = "";
			for(const f of (j.files || [])) {
				const row = document.createElement("div");
				row.className = "vrow";
				const btn = document.createElement("button");
				btn.textContent = "play";
				btn.onclick = async () => {
					const frv = window.NumField.readAbs($("v_arb_rate"));
					let fr = isFinite(frv) ? String(Math.round(frv)) : "";
					// SigMF-compatible playback: if the user didn't type a rate and the capture has a
					// .sigmf-meta sidecar, play it back at the rate recorded in the sidecar.
					if(!fr && f.sigmf) {
						try {
							const meta = await (await fetch("/captures/" + encodeURIComponent(f.name.replace(/\.[^.]+$/, "") + ".sigmf-meta"))).json();
							const sr = meta && meta.global && meta.global["core:sample_rate"];
							if(sr) { fr = String(Math.round(sr)); $("v_arb_rate").value = fr; window.NumField.normalize($("v_arb_rate")); }
						} catch(e) { /* sidecar unreadable — fall back to the DAC rate */ }
					}
					const q = "/play?file=" + encodeURIComponent(f.name) + (fr ? "&file_rate=" + fr : "");
					const rr = await fetch(API + q);
					const jj = await rr.json();
					$("v_arb_info").textContent = jj.ok ? `${f.name}: ${jj.arb_len} samples loaded from USB${fr && f.sigmf ? " @ " + (fr / 1e6).toFixed(3) + " MS/s (SigMF)" : ""}` : ("play failed: " + jj.err);
					setMode("arb");
					push();
					poll();
				};
				const lbl = document.createElement("span");
				lbl.textContent = `${f.name}  (${(f.bytes / 1e6).toFixed(1)} MB)`;
				row.appendChild(btn);
				row.appendChild(lbl);
				if(f.sigmf) {
					const tag = document.createElement("span");
					tag.className = "hint";
					tag.textContent = "SigMF";
					tag.title = "has a .sigmf-meta sidecar — download it alongside the data file (both at /captures/)";
					tag.style.marginLeft = "6px";
					row.appendChild(tag);
				}
				box.appendChild(row);
			}
			if(!(j.files || []).length) {
				box.innerHTML = '<div class="vrow hint">no .cs16/.cf32 files on the drive</div>';
			}
		} catch(e) {
			$("v_usb_stat").textContent = "file list failed";
		}
	}
	$("v_usb_refresh").onclick = refreshFiles;

	function poll() {
		if(!active) {
			return;
		}
		api("/status");
	}

	function setActive(a) {
		active = a;
		clearInterval(pollTimer);
		pollTimer = null;
		if(a) {
			poll();
			pollTimer = setInterval(poll, 1000);
			refreshPresets();
			// populate the form once from daemon state so reloads reflect reality
			if(!lastStatus) {
				api("/status").then((s) => {
					if(!s) {
						return;
					}
					populateForm(s);
				});
			}
		}
	}

	window.addEventListener("viewchange", (e) => setActive(e.detail === "vsg"));
	showGroup(currentMode);
	setActive(active);
	fetchPreview();	// first paint even before the first status poll
})();
