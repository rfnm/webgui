// Settings view: board-name registration (your-board-name.rfnm.me; default = board serial).
// Talks to ddnsd via /api/app/ddns/ (see ../ddns/ddnsd.cpp); /set is synchronous, so the
// response is the real registration outcome.
(function() {
	const $ = id => document.getElementById(id);
	const FEEDBACK_URL = "https://api.rfnm.me/fb-0c0bfd43db";
	let boardSerial = "";

	function fmtAge(s) {
		return s < 60 ? s + "s" : s < 3600 ? Math.floor(s / 60) + "m" : Math.floor(s / 3600) + "h";
	}

	function render(st) {
		$("s_en").checked = !!st.enabled;
		if(document.activeElement !== $("s_name")) {
			// running under the serial default: show an empty field (placeholder says so) —
			// what's typed here is the override, and clearing it returns to the default
			$("s_name").value = (st.name && st.name !== st.serial) ? st.name : "";
		}
		$("s_mode").value = st.mode || "local";
		boardSerial = st.serial || "";
		$("s_serial").textContent = st.serial ? st.serial.toUpperCase() : "unknown";
		$("s_ip").textContent = st.local_ip || "unknown";
		const l = st.last || {};
		let msg = "", cls = "hint";
		if(!st.enabled) {
			msg = "disabled";
		} else if(l.http === -1) {
			msg = "not registered yet";
		} else if(l.pending) {
			msg = "registering…";
		} else if(l.ok) {
			msg = (l.api.fqdn || "") + " → " + (l.api.ip || "") + (l.age >= 0 ? "  (updated " + fmtAge(l.age) + " ago)" : "");
			cls = "ok";
		} else {
			msg = "error: " + (l.api && l.api.error ? l.api.error : "http " + l.http);
			cls = "err";
		}
		$("s_stat").textContent = msg;
		$("s_stat").className = cls;
	}

	// Post-registration self-check: DoH proves global DNS, and since this very page is served by
	// the board, fetching our own fqdn succeeds iff the LAN resolver returns the private answer —
	// if DoH sees the name but the local fetch fails, the router's rebind protection is filtering it.
	async function diagnose(fqdn, age) {
		const el = $("s_diag");
		el.textContent = "checking name resolution…";
		let doh = null;
		try {
			const r = await fetch("https://cloudflare-dns.com/dns-query?name=" + encodeURIComponent(fqdn) + "&type=A",
				{ headers: { accept: "application/dns-json" } });
			doh = ((await r.json()).Answer || []).some(a => a.type === 1);
		} catch(e) {}
		let local = false;
		try {
			const ctl = new AbortController();
			const t = setTimeout(() => ctl.abort(), 4000);
			await fetch("http://" + fqdn + "/", { mode: "no-cors", cache: "no-store", signal: ctl.signal });
			clearTimeout(t);
			local = true;
		} catch(e) {}
		if(local) {
			el.textContent = "✓ " + fqdn + " resolves on this network";
			el.className = "ok";
		} else if(doh) {
			el.textContent = "⚠ registered globally, but this network's router blocks private-IP DNS answers (rebind protection) — whitelist rfnm.me in the router, or use the IP address";
			el.className = "err";
		} else if(doh === false) {
			if(age >= 0 && age > 600) {
				el.textContent = "⚠ the API accepted the registration but the name never appeared in global DNS — this board's serial is probably not in the factory database (prototype?), where registrations are silently ignored";
				el.className = "err";
			} else {
				el.textContent = "name registered, DNS still propagating — retry in a minute";
				el.className = "hint";
			}
		} else {
			el.textContent = "";	// DoH unreachable (no internet?): no verdict
		}
	}

	async function refresh() {
		try {
			const st = await (await fetch("/api/app/ddns/config")).json();
			render(st);
			if(st.enabled && st.last && st.last.ok && st.last.api.fqdn) {
				diagnose(st.last.api.fqdn, st.last.age);
			} else {
				$("s_diag").textContent = "";
			}
		} catch(e) {
			$("s_stat").textContent = "ddnsd not reachable";
			$("s_stat").className = "err";
		}
	}

	$("s_save").onclick = async () => {
		$("s_stat").textContent = "registering…";
		$("s_stat").className = "hint";
		const q = "enabled=" + ($("s_en").checked ? 1 : 0)
			+ "&name=" + encodeURIComponent($("s_name").value.trim().toLowerCase())
			+ "&mode=" + $("s_mode").value;
		try {
			const st = await (await fetch("/api/app/ddns/set?" + q)).json();
			if(st.error) {	// 400: validation failed before any registration attempt
				$("s_stat").textContent = "error: " + st.error;
				$("s_stat").className = "err";
				return;
			}
			render(st);
			if(st.enabled && st.last && st.last.ok && st.last.api.fqdn) {
				diagnose(st.last.api.fqdn, st.last.age);
			} else {
				$("s_diag").textContent = "";
			}
		} catch(e) {
			$("s_stat").textContent = "request failed";
			$("s_stat").className = "err";
		}
	};

	// ---- daughterboard cards: GET /api/v1/hwinfo (EEPROM identity + calibration distribution
	// state, rfnm_info-style). Rendered on view entry and after "download calibration now".
	function fmtWhen(t) {
		if(!t) { return ""; }
		const d = Math.floor(Date.now() / 1000 - t);
		return d < 120 ? "just now" : d < 7200 ? Math.floor(d / 60) + " min ago" : d < 172800 ? Math.floor(d / 3600) + " h ago" : Math.floor(d / 86400) + " d ago";
	}

	function calChip(b, hw) {
		if(!b.present) { return ""; }
		const f = ((hw.calib_fetch || {}).boards || []).find(x => x.serial && b.serial && x.serial.toUpperCase() === b.serial.toUpperCase()) || {};
		if(b.calib_file && f.pwr_points > 0) {
			return '<span class="ok">✓ calibrated</span> — ' + f.pwr_points + ' power points, downloaded ' + fmtWhen(b.calib_file.mtime);
		}
		if(b.calib_file) {
			return '<span class="ok">✓ factory data downloaded</span> — no TX power surface (early calibration recipe); VSG dBm uses the fleet-median table';
		}
		if(f.fetch === "unknown-serial") {
			return '<span class="err">✗ serial not in the factory database</span> — prototype board? calibration (and rfnm.me board-name registration) need it registered';
		}
		if(f.fetch === "no-network") {
			return '<span class="err">✗ not downloaded</span> — board could not reach api.rfnm.me';
		}
		return '<span class="err">✗ no calibration table</span> — press download (needs internet)';
	}

	function renderHw(hw) {
		const el = $("s_dgb");
		const boards = (hw.boards || []).filter(b => !b.motherboard);
		if(!boards.length) {
			el.innerHTML = '<span class="hint">no daughterboard EEPROMs visible</span>';
			return;
		}
		el.innerHTML = boards.map(b => {
			if(!b.present) {
				return '<div class="vrow"><label>slot ' + b.slot + '</label><span class="hint">empty</span></div>';
			}
			const t = b.type ? b.type.charAt(0).toUpperCase() + b.type.slice(1) : "?";
			return '<div class="vrow"><label>slot ' + b.slot + '</label><span><b>' + t + '</b> · serial ' + (b.serial || "?")
				+ ' · rev ' + b.rev + '</span></div>'
				+ '<div class="vrow"><label></label><span class="hint">' + calChip(b, hw) + '</span></div>';
		}).join("");
		const v = hw.vsg_tbl;
		if(v && v.src) {
			el.innerHTML += '<div class="vrow"><label>vsg table</label><span class="hint">' + v.points + ' points, '
				+ (v.src === "board" ? "this board's factory surface (" + v.serial + ")" : v.src === "fleet" ? "fleet-median fallback" : v.src) + '</span></div>';
		}
	}

	async function refreshHw() {
		try {
			renderHw(await (await fetch("/api/v1/hwinfo")).json());
		} catch(e) {
			$("s_dgb").innerHTML = '<span class="hint">hwinfo unavailable (old appd?)</span>';
		}
	}

	$("s_cal_fetch").onclick = async () => {
		const stat = $("s_cal_stat");
		stat.textContent = "starting…";
		stat.className = "hint";
		try {
			const r = await fetch("/api/v1/calib/fetch", { method: "POST" });
			const j = await r.json();
			if(!r.ok) { throw new Error(j.err || j.error || "http " + r.status); }
			stat.textContent = "fetching — results appear below in a few seconds";
			setTimeout(refreshHw, 4000);
			setTimeout(() => { refreshHw(); stat.textContent = ""; }, 12000);
		} catch(e) {
			stat.textContent = "failed: " + e.message;
			stat.className = "err";
		}
	};

	// ---- board identity card: rendered from the /api/v1/state doc apps.js already polls
	// (re-broadcast as the "rfnm-state" window event) — this file never polls appd itself.
	// Formatting reuses window.RFNM_APPS.fmtUp/fmtMem so the card and the APPS corner vitals
	// can never drift apart.
	let onSettings = false;

	function renderBoard() {
		if(!onSettings) { return; }
		const A = window.RFNM_APPS;
		const b = A && A.state && A.state.board;
		if(!b) { return; }	// placeholders stay until the first state doc lands; last values stay if appd drops
		$("s_board").textContent = b.name ? b.name.charAt(0).toUpperCase() + b.name.slice(1) : "unknown";
		$("s_up").textContent = b.uptime !== undefined ? A.fmtUp(b.uptime) : "—";
		$("s_load").textContent = b.load ? b.load[0].toFixed(2) + " / " + b.ncpu + " cores" : "—";
		$("s_mem").textContent = b.mem_avail !== undefined ? A.fmtMem(b.mem_avail) + " free of " + A.fmtMem(b.mem_total) : "—";
		const th = Object.keys(b.thermal || {}).map(z => z.replace(/-thermal$/, "") + " " + b.thermal[z].toFixed(0) + "°C");
		$("s_therm").textContent = th.length ? th.join(" · ") : "—";
	}
	window.addEventListener("rfnm-state", renderBoard);

	// deploy stamp from /rfnm/STACK_MANIFEST — the row only appears if nginx actually serves
	// it (it 404s today: the file lives outside the www root), so nothing breaks either way
	let stackTried = false;
	function fetchStack() {
		if(stackTried) { return; }
		stackTried = true;
		const ctl = new AbortController();
		const t = setTimeout(() => ctl.abort(), 5000);
		fetch("/rfnm/STACK_MANIFEST", { cache: "no-store", signal: ctl.signal }).then(r => r.ok ? r.text() : null).then(txt => {
			if(!txt || txt.length > 4096 || txt.trimStart().startsWith("<")) { return; }	// missing, or an HTML error page
			$("s_stack").textContent = txt.trim().split("\n")[0].replace(/^stack manifest\s*/, "deployed ");
			$("s_stack").title = txt.trim();
			$("s_stackrow").style.display = "";
		}).catch(() => {}).finally(() => clearTimeout(t));
	}

	// ---- force firmware update: /api/v1/fwupdate (appd, API.md §2.2). Arming deletes the
	// u-boot rfnm_is_bootable flag and reboots into the eMMC SWUpdate recovery, which
	// reinstalls the firmware (auto-download from bin.rfnm.io, or a USB stick's .swu); a
	// finished install rewrites the flag, so the armed state never survives a real update.
	async function refreshFw() {
		try {
			const st = await (await fetch("/api/v1/fwupdate")).json();
			if(!st.available) { $("s_fw_panel").style.display = "none"; return; }
			$("s_fw_panel").style.display = "";
			$("s_fw_state").style.display = st.armed ? "" : "none";
			$("s_fw_cancel").style.display = st.armed ? "" : "none";
		} catch(e) {
			$("s_fw_panel").style.display = "none";	// appd without the endpoint
		}
	}

	$("s_fw_btn").onclick = async () => {
		if(!confirm("Reboot into firmware update mode?\n\nThis reinstalls the board's firmware from scratch — everything stored on the board is replaced.\n\nThe board reboots into its recovery system (purple LEDs): with internet access it downloads and installs the STANDARD RELEASE firmware by itself; otherwise plug in a USB stick containing a .swu file. If this board runs a beta build, the automatic download takes it back to the older release version — use a USB stick with the beta .swu to stay on beta.\n\nWhen the update finishes, the board reboots back to normal operation.")) { return; }
		const stat = $("s_fw_stat");
		stat.textContent = "arming…";
		stat.className = "hint";
		try {
			const r = await fetch("/api/v1/fwupdate", { method: "POST", body: JSON.stringify({ arm: true, reboot: true }) });
			const j = await r.json();
			if(!r.ok || !j.rebooting) { throw new Error(j.error || "http " + r.status); }
			stat.textContent = "✓ rebooting into the updater — this page is going offline now";
			stat.className = "ok";
		} catch(e) {
			stat.textContent = "failed: " + e.message;
			stat.className = "err";
			refreshFw();
		}
	};

	$("s_fw_cancel").onclick = async () => {
		const stat = $("s_fw_stat");
		stat.textContent = "disarming…";
		stat.className = "hint";
		try {
			const r = await fetch("/api/v1/fwupdate", { method: "POST", body: JSON.stringify({ arm: false }) });
			const j = await r.json();
			if(!r.ok) { throw new Error(j.error || "http " + r.status); }
			stat.textContent = "✓ cancelled — next boot is normal";
			stat.className = "ok";
		} catch(e) {
			stat.textContent = "failed: " + e.message;
			stat.className = "err";
		}
		refreshFw();
	};

	$("s_fb_send").onclick = async () => {
		const msg = $("s_fb").value.trim();
		const stat = $("s_fb_stat");
		if(msg.length < 5) {
			stat.textContent = "write a few words first";
			stat.className = "err";
			return;
		}
		stat.textContent = "sending…";
		stat.className = "hint";
		try {
			const r = await fetch(FEEDBACK_URL, { method: "POST", body: new URLSearchParams({ message: msg, serial: boardSerial }) });
			const j = await r.json();
			if(!j.ok) { throw new Error(j.error || "rejected"); }
			$("s_fb").value = "";
			stat.textContent = "✓ sent — thanks!";
			stat.className = "ok";
		} catch(e) {
			stat.textContent = "could not send (board online?) — " + e.message;
			stat.className = "err";
		}
	};

	window.addEventListener("viewchange", ev => {
		onSettings = ev.detail === "settings";
		if(onSettings) {
			refresh();
			refreshHw();
			refreshFw();
			renderBoard();
			fetchStack();
		}
	});
	if(location.hash.replace("#", "") === "settings") {
		onSettings = true;
		refresh();
		refreshHw();
		refreshFw();
		renderBoard();
		fetchStack();
	}
})();
