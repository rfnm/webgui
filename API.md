# RFNM board machine API — the normative spec

Every RFNM board exposes ONE programmatic interface; the web GUI is just its first client.
This file is the contract for machine clients and for every daemon that serves a piece of it.
Current truth only — semantics changes edit this file in place (history = git log).

Layout: one control door (nginx :80) + declared direct data ports.

```
http://<board>/api/v1/...          appd: discovery, lifecycle, radio lease, rf/config   (§2)
http://<board>/api/app/<id>/...    the app's own domain API (route generated from id)   (§3)
ws://<board>/ws  = ws://<board>:7681/   RFSP v1 binary spectrum stream (rtsa)           (§5.1)
udp://<board>:4991                 VITA 49.2 signal data + context + command (vrt)      (§5.2)
tcp://<board>:30005 / :30003       BEAST / SBS ADS-B feeds (adsb, when net=on)          (§5.3)
http://<board>/adsb/aircraft.json  readsb JSON snapshots (static, 1 Hz)
http://<board>/captures/...        recorder output on the USB disk (static)
```

Rules of the road:
- **Machines enumerate apps, routes, and data ports from `GET /api/v1/state` — never hardcode.**
- Control APIs are loopback-bound behind nginx. Heavy streams run on dedicated direct ports,
  declared in the owning app's manifest and mirrored in the state document (`apps[].data`).
- No auth; nginx is the single choke point if/when that changes.
- All JSON is UTF-8; unknown fields are additive — clients must ignore what they don't know.

## §1 Conventions

- Verbs: `GET` reads, `PUT` writes a document/setting, `POST` performs an action.
- Errors: non-2xx with body `{"err":"<human reason>"}` (appd may add `"log":[...]`). Codes:
  `400` malformed, `403` refused by TX policy, `404` unknown app/route, `409` lease busy /
  mid-transition, `5xx` daemon fault. vsgd's policy-403 carries the full config document
  (§4.3) instead of `{err}` — the refusal reason is in its `last_error` field.
- The radio is single-owner (§2.2): at most ONE radio app runs at a time, fleet-wide per board.

## §2 Control plane — appd at `/api/v1`

| verb | path | body → reply |
|---|---|---|
| GET | `/api/v1/state` | → the whole-board document (below). **Also the presence heartbeat** (§4.1). |
| POST | `/api/v1/radio` | `{"owner":"<id>"\|"none"}` → state's radio object. 403 TX app on TX-forbidden board, 404 unknown, 400 non-radio app, 409 mid-transition/uninstalled. |
| PUT | `/api/v1/apps/<id>/rf` | `{"antenna":"sma_a"\|"embed","gain":<int>,"agc":<bool>}` → 200; restarts the app if running. |
| PUT | `/api/v1/apps/<id>/config` | `{"key":"<manifest config key>","value":"<choice>"}` → 200; 400 on bad key/choice. |
| POST | `/api/v1/apps/<id>/restart` | → 200. |
| GET | `/api/v1/apps/<id>/log?n=40` | → `{"log":["..."]}` journal tail. |
| POST | `/api/v1/rescan` | re-scan manifests, regenerate nginx routes → 200. |
| GET | `/api/v1/fwupdate` | → `{"available":<bool>,"armed":<bool>}` recovery-updater boot flag (§2.2); `available:false` carries `error`. |
| POST | `/api/v1/fwupdate` | `{"arm":<bool>,"reboot":<bool>}` → `{"armed","rebooting"}`; arm clears the u-boot boot flag (reboot=true then reboots into the SWUpdate recovery), arm:false restores it. 500 = env untouched/unverified. |

State document (additive; the load-bearing fields):

```json
{ "seq": 12345,
  "board": { "name": "...", "tx_allowed": "none|embed|all", "thermal": {...}, ... },
  "radio": { "owner": "rtsa|...|", "state": "idle|starting|active|stopping|switching|crashed",
             "since": 0.0, "note": "...", "clients": 1 },
  "apps": [ { "id": "rtsa", "title": "...", "radio": "rx|tx|none",
              "state": "...", "installed": true, "idle_stop": 180,
              "api": { "base": "/api/app/rtsa/", "port": 7687, "version": 1 },
              "data": [ { "kind": "spectrum-ws", "proto": "ws", "port": 7681,
                          "dir": "out", "path": "/ws", "desc": "..." } ],
              "liveness": "clients",
              "rf": { "antenna": "sma_a", "gain": 30, "agc": false },
              "config": [ ... ], "build": { ... } } ] }
```

- `apps[].api` is present when the app serves an HTTP API; `base` is the nginx route.
- `apps[].data` lists the app's direct data ports (`proto` ∈ ws/tcp/udp/http; `dir` is
  board-relative: `out` = board emits). `path` is set when the same stream is also reachable
  through nginx (`/ws`) or as static files (`/adsb/aircraft.json`).
- `radio.clients` appears when the owner declares connection-counted liveness (§4.2): the
  owner's live client count as last polled.

### §2.1 Hardware identity + calibration — `GET /api/v1/hwinfo`

`{"boards":[{"motherboard":true,...} | {"slot":0,"present":true,"type":"lime","id":3,"rev":2,
"serial":"AB12CD34","calib_file":{"size","mtime"}|null}, ...], "calib_fetch":<the
rfnm-calib-fetch status ledger or null>, "vsg_tbl":{"serial","src":"board|fleet","points"}|null}`
— EEPROM identity per board (motherboard + daughterboard slots, rfnm_info-style) plus the
factory-calibration distribution state. `POST /api/v1/calib/fetch` starts the on-board
`rfnm-calib-fetch` oneshot (60 s rate guard, 429 inside it); re-poll hwinfo for the outcome.

Distribution flow: `GET https://api.rfnm.me/calib/<serial>` (public, rate-limited; 404 =
serial not in the factory database) → board stores the raw blob at
`/lib/firmware/rfnm/calib_<SERIAL>.json` and derives vsgd's `/rfnm/vsg/calib_pwr.tbl` from the
primary slot's pwr surface (fleet-median fallback table for boards without one). Runs at boot
+ 6-hourly via `rfnm-calib-fetch.timer`; the §4.3 `dbm` key needs the table.

### §2.2 Force firmware update — `/api/v1/fwupdate`

The u-boot env flag `rfnm_is_bootable` gates normal boot: `rfnm_bootcmd` falls through to
the raw-eMMC SWUpdate recovery whenever the flag is unset/empty (the same path the hardware
boot-pin override takes). appd flips it with `fw_setenv`/`fw_printenv` (`/etc/fw_env.config`,
eMMC offset 0x400000) and verifies every write by readback; a corrupt env (bad CRC) makes the
endpoint refuse (`available:false`) rather than rewrite the boot plumbing from defaults.
`POST {"arm":true,"reboot":true}` is the GUI's one-click path: reply first, reboot ~1 s later.
In recovery the board auto-downloads `http://bin.rfnm.io/rfnmblue.swu` when it has internet
(else: USB stick with a `.swu`, via `swupdate-usb@`); a completed install rewrites the whole
env with `rfnm_is_bootable=1`, so the armed state never survives a successful update. The
download URL is the RELEASE channel, baked into the factory rescue image: a board on a beta
build that force-updates over the network is DOWNGRADED to the release version — beta stays
beta only via a USB-stick `.swu`.
Arming is idempotent; `{"arm":false}` cancels a pending armed state.

## §3 Per-app domain APIs — `/api/app/<id>/`

The route is derived from the manifest `id` (charset `[a-z0-9_-]`). nginx strips the prefix:
an app serves bare paths (`/status`), reachable as `/api/app/<id>/status`. Every app answers:

- `GET /api/app/<id>/api` → `{"app":"<id>","api_version":N,"endpoints":[{"method","path","desc"},...]}`
  — self-description; with `GET /api/v1/state` this makes the whole board machine-discoverable.
- `GET /api/app/<id>/status` → the app's health/telemetry JSON (shape is app-owned, additive).

App-specific surfaces beyond that (current fleet): `rtsa` recorder (`/record`, `/recstatus`,
`/clients`), `vsg` (§4.3 config document + `/arb` + `/signal` (TX render `src=tx` / live RX monitor `src=rx`, fc-centered; NOT named "spectrum" - that is the RTSA app) + `/presets`…), `lora`
`/packets`, `ais` `/messages`, `clockgen`/`boardio` `POST /set`-style writes, `vrt` `/status`.

## §4 Radio lease, keepalive, liveness

### §4.1 The lease

`POST /api/v1/radio {"owner":"<id>"}` stops the current radio app (if any), waits for the
radio-ready gate, starts yours. `{"owner":"none"}` releases. Transitions are serialized —
409 means retry after the current transition. Poll `state.radio.state` for `active`.

### §4.2 Staying alive (and being stopped)

An app whose manifest sets `idle_stop_s > 0` is stopped by appd when BOTH hold:
1. no `GET /api/v1/state` for `idle_stop_s` (presence heartbeat — any client's poll counts,
   and a successful `POST /api/v1/radio` stamps it too), AND
2. if the app declares `liveness:"clients"`: its client count has been 0 for ≥3 consecutive
   5 s polls (a momentary reconnect never kills a session; a broken liveness endpoint counts
   as 0 — liveness can extend a session, never immortalize it).

So a machine client has three equivalent ways to hold a session:
- **hold a data-plane connection** to an app with `liveness:"clients"` (e.g. the RFSP WS) —
  nothing else needed; closing it releases the app like closing a browser tab;
- **poll `GET /api/v1/state`** at least every `idle_stop_s` (comfortably: every 20 s);
- use an app with `idle_stop_s: 0` (e.g. `vrt`) — no idle-stop; release the lease explicitly
  when done (`{"owner":"none"}`). Orphaned owners are visible in `state.radio` and cost one
  POST to clear.

### §4.3 vsg config document (`/api/app/vsg/config`)

Symmetric read-modify-write. `GET` returns the document; `PUT` accepts a FULL or PARTIAL
document (only present keys are applied — merge is inherent); the reply is always the full
post-apply document. Apply-all-report-errors: a policy-refused key keeps its previous value
and the reason lands in `last_error` with HTTP 403 (everything else in the PUT still applied).
Unknown keys are ignored. A change is live once `applied_ver >= cfg_ver` from your PUT reply
(poll `GET /config` or `/status`).

Keys are FLAT and identical to the historical `/set` vocabulary (also what presets store):

| group | keys |
|---|---|
| core | `tx` (0/1; never persisted — boards wake TX-off), `mode`, `rate` (Hz, `0`/`"auto"` = auto ladder), `freq` (Hz), `power` (device TX power knob; on dBm-engine boards — a `/lib/firmware/rfnm/tx_dbm_<serial>.map` present, advertised range then e.g. −50..22 — this IS requested dBm at the SMA, resolved inside rfnm_lime; legacy boards keep the raw setting, Lime −60..52 with PA1 above 0 / PA2 above 20; `/status` surfaces the advertised range as `power_min`/`power_max` once the device is open, 0/0 before — clients clamp their power controls to it), `txpath`, `level` (dBFS), `offset` (Hz **or `"auto"`, the default** — fc semantics since 2026-07-22: `freq` is the CENTER of the emitted waveform; auto parks the LO below it at `max(bw/2+1 MHz, 4 MHz)` so LO leak, DC neighborhood and the IQ image fall outside the occupied band; the resolved value is reported as `offset_hz` + `lo_hz` + `offset_auto` in `/status`; a numeric offset overrides for manual testing — content still airs at `freq`), the monitor DISPLAY always masks capture-self-view families (fs/16 comb, capture DC, and the LO-separation beat harmonics k·(offset−fs/32) for k≥2 — the k=1 line is the real TX LO leak and stays visible; QEC stays raw), `dbm` (number or `null`=off; plain alias that rounds into `power` — the resolver lives in the driver since 2026-07-22) |
| am/fm/pulse | `am_shape`, `am_rate`, `am_depth`, `fm_rate`, `fm_dev`, `pulse_w`, `pulse_p` |
| multitone/sweep/awgn | `mt_tones`, `mt_space`, `sweep_span`, `sweep_time`, `awgn_bw_hz` |
| digital | `const`, `alpha`, `span`, `sps`, `symrate`, `diff`, `oqpsk` |
| fsk | `fsk_bps`, `fsk_h`, `fsk_bt`, `fsk_sps`, `fsk_symrate` |
| ofdm | `ofdm_nfft`, `ofdm_cp`, `ofdm_win`, `ofdm_nsc`, `ofdm_bps`, `ofdm_pilot`, `ofdm_bw_hz` |
| ble | `ble_ch`, `ble_name` (raw UTF-8 string in JSON), `ble_interval_ms` |
| arb | `arb_rate`, `seed` (+ `POST /arb[?fmt=cs16|cf32&file_rate=R]`, raw IQ body ≤512 MB) |
| impairments | `imp_iqgain`, `imp_iqphase`, `imp_dci`, `imp_dcq`, `imp_conj`, `cn_on`, `cn_db`, `pn_dbc`, `pn_bw_hz` |
| monitor | `monitor`, `mon_gain`, `mon_path` |
| read-only meta (GET/reply only) | `cfg_ver`, `applied_ver`, `board`, `tx_policy`, `txpath_locked`, `last_error` |

Types: numbers as JSON numbers; flag-like keys as 0/1 integers; `mode/txpath/am_shape/const/
ble_name/mon_path` as strings. `/status` remains the 1 Hz health feed (field names there may
differ from config keys — `/config` is the RMW surface, `/status` is telemetry).

Monitor semantics (single-client stream contract, 2026-07-22): the RX lane is DECLARED in the
session's bring-up apply — enabling/disabling `monitor` on a live session rebuilds it (~2 s).
Duplex validity is the DRIVER's knowledge (DCS planner + VSPA registry capabilities), not the
daemon's: the bring-up TRIES the duplex apply and falls back to TX-only when the driver refuses,
so `/status` `mon_avail` is learned truth — 1 until the driver refuses duplex at the current
rate/config, 0 after (any config change retries once). `mon_path` `loopback` on a
TX-live session is the chip-internal RFE loopback: a non-diverting TX sensor (TX keeps
radiating). The monitor RX LO is declared fs/32 below the TX carrier so TX LO leak and
receiver DC never share a bin; the published RX PSD is already re-centered on the TX carrier.
Monitor integrity counters in `/status`: `mon_reads/mon_timeouts/mon_samples/mon_gaps/mon_frames`,
`mon_sentinels` (−32768 sentinel samples seen — the duplex-integrity check; was misleadingly
named `mon_sent` before 2026-07-23), and `mon_sick` (1 = the stream never goes contiguous: no
accumulation window completes, published data would be garbage — a known boot-state fault;
heal = driver reload with radio consumers stopped).

`GET/POST /api/app/vsg/qec` — autonomous TX QEC engine (2026-07-23). A daemon-side loop
converges TX LO leak (`imp_dci/dcq`) and TX image (`imp_iqgain/iqphase`) from the chip-LB
monitor's own measurements whenever TX runs with the monitor: from-zero converge on session
start, re-converge on any operating-point change (freq/offset/rate/power/dbm/txpath —
per-state trims), 10 s drift tracking with automatic re-null. LO-only in non-cw modes
(image needs the cw line). It never reparks the user's offset — unusable geometry (offset
< 3.5 MHz) blocks with a reason. No device applies happen in the loop. POST forces a fresh
from-zero re-null (never required; 409 when TX/monitor absent). GET returns `state`
(waiting/converging/tracking/blocked), `why`, live `lo_dbc`/`img_dbc`, last-converge
`*_dbc_before/after`, `floor_dbc`, `lo_skipped`/`img_skipped`, `flip`, `runs`, `at`/
`track_at`, and the applied `imp_*` values (also persisted to `/config`). `/status` carries
`qec`/`qec_why`/`qec_lo_dbc`/`qec_img_dbc`/`qec_runs` for GUI/pollers.

### §4.4 TX policy (safety, enforced twice)

Board policy `tx_allowed ∈ none|embed|all` (from `/rfnm/board.conf`) is enforced by appd at
lease time (403 for `radio:"tx"` apps on `none` boards) AND inside every TX-capable daemon:
vsg refuses `tx=1` on `none` and locks `txpath` to the embedded antenna on `embed`; vrt drops
TX-ingest data and error-acks TX commands (status word `0x3`) unless policy allows, and on
`embed` forces the TX path to the embedded antenna. Machine clients must treat 403 + the
daemon's `last_error`/counters as final — there is no override on the wire.

## §5 Data planes

### §5.1 RFSP v1 — spectrum stream (rtsa)

Binary WebSocket, spec in `PROTOCOL.md` (wire structs: `rfnm_spectrum_protocol.h`). Reachable
as `ws://<board>/ws` (nginx) or directly `ws://<board>:7681/` (same daemon; zero proxy cost).
Machine-consumer obligations:
- **ACK every SPECTRA**: echo its `seq` in a type-17 ACK immediately on receipt — the server's
  flow-control pacer is driven by your ACK stream; silent clients get legacy-brake drops.
- Rows are u8-quantized dB: `db = db_min + v/255*(db_max - db_min)` from each message header.
  Display/monitoring grade by design — measurement-grade consumers use the IQ plane (§5.2).
- permessage-deflate is negotiated-only; a client that never offers it gets plain frames.
  Frames can exceed 64 KB (64-bit WS lengths). Little-endian throughout.
- **Single viewer**: a new WS upgrade disconnects every other RFSP client (the stream follows
  the freshest viewer — tune/zoom state is global). Machine and GUI cannot stream at once.
- Warm-up: wait for the first SPECTRA before sending TUNE/RATE bursts.
- **AGC is on by default** (RT: kernel leveling loop; sweep: per-hop table, rows normalized
  to the reference gain so levels stay absolute). An explicit TUNE gain disables it; TUNE
  flags bit2 toggles it; live gain + state ride the 1 Hz STATUS word (PROTOCOL.md).

### §5.2 VITA 49.2 — IQ samples (vrt)

UDP :4991 both directions (ANSI/VITA-49.2): RX export = signal-data packets (CS16,
big-endian, 360 samples/pkt) + IF-context; TX ingest = signal data addressed to the TX
stream-id; tune/gain via 49.2 command packets with WIF0/EIF0 acks. Wire profile + stream-id
defaults: `apps/vrtd/README.md`. Destination is configured per deployment (`vrt` app.conf /
`--dest`); `GET /api/app/vrt/status` reports counters incl. `tx_forbidden` (§4.4).

### §5.3 ADS-B feeds (adsb)

With manifest config `net=on`: BEAST binary on tcp :30005, SBS/BaseStation on tcp :30003
(standard readsb framing). `net=off` (default) closes both. The JSON snapshots at
`/adsb/aircraft.json` + `/adsb/stats.json` are always on while the app runs. adsb has no
connection-counted liveness — headless consumers keep it alive via the §4.2 heartbeat.

## §6 Clients

`rfnmctl` (host Python kit, the rfnm/rfnmctl repo) is the reference client and the acceptance
harness: `rfnmctl [host] state|lease|hold|rf|config|fft|vsg|vrt|api|accept|discover` — host
is the first positional (IP / host:port / alias); omitted, stdlib UDP broadcast discovery
(librfnm's probe: one 0x00 byte to broadcast:28285, hwinfo reply) uses the single board on
the LAN or refuses with the list. Boards may answer from a DHCP lease address beside their
statically pinned IP — the API serves on both. Its selftest pins this contract (golden RFSP +
hwinfo frames, protocol-header cross-check, stub REST servers).
