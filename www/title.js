// Live tab title + favicon (title.js): the browser tab says what the board is doing, so a
// wall of RFNM tabs (or a backgrounded one) is readable at a glance. Renders EXCLUSIVELY
// from the /api/v1/state document apps.js already polls - re-broadcast here as the
// "rfnm-state" window event - this file never fetches anything.
//   radio idle:         "<view> · RFNM"           dark tab icon, no dot
//   radio app active:   "● <app> · RFNM"          green icon dot = RF front-end in use
//   TX owner (vsg):     "● <app> · RFNM"          RED icon dot = the board is transmitting
// The favicon = the transparent rfnm.com mark (light + dark scheme variants) with the dot
// composited on a 32x32 canvas; the static links in <head> cover the pre-JS moment.
"use strict";
(function() {

const VIEW_LABEL = { rtsa: "RTSA", vsg: "VSG", apps: "APPS", settings: "Settings" };
const TAB_LABEL = { rtsa: "RTSA", vsg: "VSG" };   // hidden-panel owners read as their top-level tabs, not their manifest titles

let curView = (location.hash.replace("#", "") || "rtsa").split("/")[0];
let state = null;
let appdOk = false;

// ---- favicon: the rfnm.com mark, transparent, one variant per browser color scheme
// (dark mark for light tab bars, white mark for dark ones) + status dot when it means
// something; the dot sits in a punched-out transparent ring so it reads on any tab color.
const iconCache = {};   // "<scheme>:<kind>" -> data URL
const marks = {
	light: { img: new Image(), ready: false, src: "favicon.png?v=app2" },
	dark: { img: new Image(), ready: false, src: "favicon-dark-mode.png?v=app1" }
};
for(const s in marks) {
	marks[s].img.onload = () => { marks[s].ready = true; for(const k in iconCache) { delete iconCache[k]; } update(); };
	marks[s].img.src = marks[s].src;
}
function favicon(scheme, kind) {   // kind: "idle" | "rx" | "tx"; null until the mark image is in
	const key = scheme + ":" + kind;
	if(iconCache[key]) { return iconCache[key]; }
	const m = marks[scheme];
	if(!m || !m.ready) { return null; }
	const cv = document.createElement("canvas");
	cv.width = cv.height = 32;
	const g = cv.getContext("2d");
	g.drawImage(m.img, 0, 0, 32, 32);
	if(kind !== "idle") {
		const col = kind === "tx" ? "#f43f5e" : "#10b981";
		g.globalCompositeOperation = "destination-out";
		g.beginPath();
		g.arc(24, 8, 7.5, 0, 2 * Math.PI);
		g.fill();
		g.globalCompositeOperation = "source-over";
		g.beginPath();
		g.arc(24, 8, 5.5, 0, 2 * Math.PI);
		g.fillStyle = col;
		g.fill();
	}
	iconCache[key] = cv.toDataURL("image/png");
	return iconCache[key];
}

// the two media-gated links from index.html; the browser applies whichever matches
const iconLinks = [];
document.querySelectorAll('link[rel="icon"]').forEach(el => {
	iconLinks.push([el, (el.media || "").indexOf("light") >= 0 ? "light" : "dark"]);
});

function update() {
	const r = (state && state.radio) || null;
	const ownerApp = r && r.owner ? ((state.apps || []).find(a => a.id === r.owner) || null) : null;
	const live = !!(appdOk && ownerApp && r.state === "active");   // dot ONLY when the front-end is really held
	const tx = live && ownerApp.radio === "tx";
	const name = live ? (TAB_LABEL[ownerApp.id] || ownerApp.title) : VIEW_LABEL[curView];
	const title = (live ? "● " : "") + (name ? name + " · RFNM" : "RFNM");
	if(document.title !== title) { document.title = title; }
	const kind = tx ? "tx" : live ? "rx" : "idle";
	for(const [el, scheme] of iconLinks) {
		const href = favicon(scheme, kind);
		if(href && el.href !== href) { el.href = href; }
	}
}

window.addEventListener("viewchange", e => { curView = e.detail; update(); });
window.addEventListener("rfnm-state", e => {
	appdOk = !!(e.detail && e.detail.ok);
	state = e.detail ? e.detail.state : null;
	update();
});
update();

})();
