# RFNM spectrum stream protocol v1 (binary, little-endian)

One WebSocket at `/ws` (nginx proxies to the daemon at `127.0.0.1:7681`; also reachable
direct on `:7681` — a declared data port; the control plane around it is `API.md`). Machine
consumers: ACK every SPECTRA and mind the single-viewer policy (API.md 5.1). Every message is a
binary frame; there is no JSON anywhere. All integers/floats little-endian. The C definitions
live in `rfnm_spectrum_protocol.h` (shared by the daemon); `www/spectrum.js` is the reference
decoder.

## Generic header (16 bytes, every message both directions)

| off | type | field | value |
|---|---|---|---|
| 0 | u32 | magic | 0x50534652 ("RFSP" as bytes) |
| 4 | u8  | version | 1 |
| 5 | u8  | type | see below |
| 6 | u16 | flags | 0 |
| 8 | u32 | seq | per-sender monotonic |
| 12 | u32 | payload_bytes | bytes following the type-specific header |

## Server → client

### type 1 — SPECTRA (the hot path)
Type-specific header at offset 16 (48 bytes, f64 fields 8-aligned by construction):

| off | type | field |
|---|---|---|
| 16 | u32 | n_bins |
| 20 | u16 | n_rows (spectra in this batch) |
| 22 | u16 | mode (0 = raw, 1 = device-averaged) |
| 24 | f32 | db_min (u8 0 maps here) |
| 28 | f32 | db_max (u8 255 maps here) |
| 32 | f64 | center_hz |
| 40 | f64 | span_hz |
| 48 | u32 | fft_size |
| 52 | u32 | accum_per_row (FFTs folded into each row; 1 in raw mode) |
| 56 | u32 | source_rows_per_sec (produced rate pre-throttle, for drop display) |
| 60 | u32 | reserved0 |
| 64 | u8[n_rows][n_bins] | payload: dB quantized, `db = db_min + v/255*(db_max-db_min)`; row 0 oldest |

`n_bins` constant within a connection unless a CONFIG round-trip changes it (client re-inits
textures on change; server bumps `seq` normally).

### type 2 — STATUS (1 Hz)
| off | type | field |
|---|---|---|
| 16 | u32 | fft_per_sec (DSP output rate) |
| 20 | u32 | rows_sent_per_sec |
| 24 | u32 | rows_dropped_per_sec (throttle drops) |
| 28 | f32 | dsp_time_us (per FFT) |
| 32 | f32 | cpu_load (0..1) |
| 36 | f32 | dsp_load (0..1) |
| 40 | u32 | reserved0 |
| 44 | u32 | reserved1 |
| 48 | u32 | link_limited (1 = flow control active for THIS client; payload is per-client) |
| 52 | u32 | link_deliv_kbps (measured delivery rate) |
| 56 | u32 | link_rtt_min_ms |
| 60 | u32 | agc: byte 0 = live RX gain (i8), byte 1 bit0 = AGC enabled |

Fields 48+ were added with the WAN flow control; older servers send a 32-byte payload —
clients must feature-detect on payload length. The AGC word is zero on pre-AGC servers;
the gain byte is the daemon's live leveling gain in real-time mode and the REFERENCE
gain (the level the composed row is normalized to) in sweep mode.

### type 23 — PATHS (RX path menu; at connect and after every path/channel change)
| off | type | field |
|---|---|---|
| 16 | char[2][32] | daughterboard user-readable names ("" = slot empty) |
| 80 | u8  | count (valid entries, max 16) |
| 81 | u8  | current_idx (index into entry[] of the active path) |
| 82 | u8[2] | reserved |
| 84 | entry[16] | each 8 B: u8 path code (ANTENNA encoding), u8 rx channel, u8 dgb slot, u8 reserved, u16 fmin_mhz, u16 fmax_mhz (the channel's tunable band, rounded inward) |

The daemon enumerates every RX channel's `path_possible` set at init and announces it with
the daughterboard each channel lives on, so clients can label entries
"<daughterboard> — <path>" when two boards are fitted. Clients must adopt `current_idx` as
the truth of what the radio is doing NOW (the daemon persists the selection across its own
restarts) and must NOT push a persisted antenna preference at connect. The SELECTED entry's
[fmin, fmax] are the fixed limits for every client tune/zoom/sweep decision; the daemon
re-announces after REJECTED switches too, so client menus always snap back to reality.

## Client → server

### type 16 — CONFIG
| off | type | field |
|---|---|---|
| 16 | u16 | max_rows_per_sec (0 = unthrottled) |
| 18 | u16 | mode (0 raw, 1 avg) |
| 20 | u32 | fft_size_req (0 = keep) |
| 24 | f32 | ema_alpha (avg mode) |
| 28 | u32 | reserved0 |

### type 17 — ACK (one per received SPECTRA message; server flow-control input)
| off | type | field |
|---|---|---|
| 16 | u32 | seq of the SPECTRA message just processed (echo of its header seq) |
| 20 | u32 | reserved (was `ws.bufferedAmount` — the client SEND queue, no downstream info) |

Flow-control rule (server, daemon/pacer.h; validated in an offline simulation of the
pump/txq/sndbuf/bottleneck/ACK chain):
from the ACK stream the server derives, per client, the delivery rate (windowed acked
bytes/s), rtt_min (min-filtered, drift-bounded against self-queuing) and in-flight bytes;
unacked data is capped at 1.3 x BDP (2.5x during BBR-style STARTUP bandwidth discovery, 1.5x
for one measurement window every 5 s to probe freed headroom; 256 KB floor pre-measurement).
Window-blocked modes skip their cursors forward — pace by dropping, never by queueing stale
lines. On a LAN in-flight never approaches the cap, so behavior is identical to no flow
control. A client that never ACKs gets the legacy brakes only (1 MB txq cap, whole-message
drops). ACKs MUST be per-message: batching them inflates apparent in-flight past any sane
window. App-limited guard (BBR-style): a measurement window in which the sender never hit
the cap measures the OFFERED LOAD, not the link — such samples may only raise the
delivery estimate (a sweep's ~0.5 Mbit/s of rows must not poison the estimate a returning
RT stream sizes its window from), EXCEPT windows containing a txq-cap drop: a drop is hard
overrun evidence, so those windows may lower the estimate even without blocking. The STATUS
`link_limited` flag reflects RECENT blocking or drops (~5 s), never a lifetime latch. Never block the FFT loop on the socket — drop whole rows and count them in STATUS.

### type 18 — TUNE (client → server)
| off | type | field |
|---|---|---|
| 16 | f64 | freq_hz (new RX LO) |
| 24 | i32 | gain_db |
| 28 | u32 | flags (bit0: gain valid, bit1: freq valid, bit2: AGC toggle — gain_db carries 0/1) |

Server retunes the RX channel live (stream keeps running; expect a few glitched rows).
The new center appears in subsequent SPECTRA headers — clients re-derive their axes from it.
AGC is ON by default (daemon `--no-agc` opts out). An explicit gain (bit0) is the manual
override: it disables AGC and applies that gain (bit2 is ignored in the same message).
Toggling AGC off (bit2, gain_db=0) freezes the gain where the loop left it; toggling on
re-enables leveling from the current gain. AGC state and the live gain ride the 1 Hz STATUS
word. Mid-sweep toggles apply at the next real-time entry.

### type 19 — SCALE (client → server)
| off | type | field |
|---|---|---|
| 16 | f32 | db_min |
| 20 | f32 | db_max |
| 24 | u32 | reserved0 |
| 28 | u32 | reserved1 |

CyberEther Scale-block equivalent: the server re-quantizes the u8 rows into this window at the
source (span clamped to [5,200] dB), so the full 8-bit resolution always covers the displayed
range. Global (last writer wins); the applied window is reflected in SPECTRA `db_min/db_max`.

### type 20 — RATE (client → server)
| off | type | field |
|---|---|---|
| 16 | u32 | sample_rate_hz (RF span; must be one of the 2× ladder values) |

Zoom-driven RF span step: the server changes the sample rate (100M/50M/25M/12.5M/6.25M/3.125M
only; other values ignored). The stream is quiesced around the change; the new span appears in
subsequent SPECTRA headers.

### type 21 — SWEEP (client → server)
| off | type | field |
|---|---|---|
| 16 | f64 | start_hz |
| 24 | f64 | stop_hz |
| 32 | u32 | bw_hz (fixed per-hop sample bandwidth; must be a rate-ladder value) |
| 36 | u32 | enable (1 = swept mode, 0 = real-time) |

Swept spectrum analyzer. With `enable=1` (and `stop_hz > start_hz`, `bw_hz` a ladder value) the
server hands the RX thread to a hop ladder: it steps the LO across `[start_hz, stop_hz]` at the
fixed per-hop bandwidth `bw_hz`, FFTs each hop's central usable band, and stitches them into one
full-span row published once per completed pass. That row ships as an ordinary SPECTRA message
(`mode=0`, `n_rows=1`) whose `center_hz`/`span_hz` cover the whole sweep, so the client renders
it with the normal center/span-driven waterfall + trace. `enable=0` returns to the single-LO
real-time stream, restoring the pre-sweep rate/LO — with one refinement: TUNE/RATE messages
already queued behind the disable override the restore targets, so a sweep→RT morph costs one
reclock instead of restore-then-step. Re-sending SWEEP with new params re-arms the ladder
mid-sweep; with `bw_hz` unchanged a re-arm is just a new hop plan (no reclock), which is what
makes the GUI's zoom-following sweep range cheap. The reference GUI always sends
`bw_hz = 50 MHz` (speed/level sweet spot; its per-hop bandwidth knob was removed 2026-07-12).

While AGC is enabled, sweep mode runs a per-hop gain table: each hop's gain is applied in
the same confirmed apply as its LO retune, attacked/decayed per pass from that hop's own
captured window (the kernel leveling loop is parked for the sweep — it cannot attribute
ring samples to hops). The composed row is normalized back to the REFERENCE gain (the last
manual/`--gain` value), so displayed levels stay absolute: a hop that attacked down shows
the same signal level, with an honestly higher noise floor. A re-arm (range/bw edit)
reseeds the table at the reference gain and re-converges within a few passes.

### type 22 — ANTENNA (client → server)
| off | type | field |
|---|---|---|
| 16 | u8 | path code: 0 = embedded antenna; 1..8 = SMA A..H; 9 = loopback |
| 17 | u8 | rx channel id (from PATHS; legacy clients send 0) |

Same-channel picks apply live (stream keeps running). Cross-channel picks (a path on the other
daughterboard) move the whole RX session to that channel (~0.5 s stream rebuild). The result is
announced to every client via a fresh PATHS message.
