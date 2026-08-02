// Per-client WAN flow control for the spectrumd websocket stream.
//
// Problem: the default live stream is ~14 MB/s by design (LAN-first). On a slow/far link
// the kernel socket buffer + txq legally absorb ~1.8 MB before any server-side signal
// fires, which at 2 Mbit/s is seven seconds of waterfall lag. The browser's
// ws.bufferedAmount (what the old ACK carried) is its SEND queue - always ~0 on a
// receive-heavy stream - and carries no downstream information at all.
//
// Design (validated in an offline simulator that #includes this header and
// models the pump/txq/sndbuf/bottleneck/ACK chain at 1..300 ms RTT, 2..1000 Mbit/s):
//
//   The client ACKs EVERY spectra message (echoing the header seq; ~30 B upstream each,
//   a few hundred/s at worst). From a send ring (seq -> cumulative wire bytes, send time)
//   the server keeps, per client:
//     deliv_Bps  = EWMA of acked wire bytes / ACK spacing = what the pipe actually carries
//     rtt_min_ms = min over a 10 s window of (t_ack - t_send)  (BBR-style, immune to the
//                  queueing delay our own backlog adds)
//   and enforces ONE rule: unacked bytes may not exceed  WIN_BDP_MULT x deliv x rtt_min
//   (floor WIN_FLOOR until the pipe is measured). That makes the stream self-clocking -
//   each ACK releases the next message, throughput converges to the link rate with
//   in-flight pinned at ~2.5 bandwidth-delay products, i.e. waterfall lag ~2.5 RTTs.
//
//   Earlier iterations used an AIMD rate controller (cut/probe/drain) on top; the
//   simulator showed it only fights the window (probes to 1.0 under a full window, then
//   crashes to a stale fit - +-6x pace oscillation). Deleted: the window IS the controller.
//
// LAN invariant: with per-message ACKs the in-flight on a fast link is ~1 RTT of data
// (tens of KB), far under WIN_FLOOR - the window never engages, send behavior is
// byte-identical to the pre-pacer code (sim: pace 1.0, 6899 of 6900 lines/s, 0 blocks).
//
// Legacy clients (never ACK): the window gives up after 3 s and the pre-existing brakes
// (1 MB txq cap + whole-message drops) apply exactly as before.
//
// Pump contract: gate every spectra send on !win_blocked(now); when blocked, advance the
// mode cursor (drop unsent rows, stay fresh) exactly like the existing credit-cap skips.
// Pure logic, no I/O; times are double milliseconds from any monotonic clock.
#pragma once
#include <stdint.h>
#include <string.h>

struct Pacer {
	static constexpr int    RING = 1024;             // sent-seq ring (~4 s of full-rate history)
	static constexpr double RTT_INIT_MS = 100.0;
	static constexpr double RTT_WIN_MS = 10000.0;    // rtt_min sliding window
	// in-flight cap in bandwidth-delay products. Self-clocking only needs BDP + 1 message to
	// keep the pipe full; anything above that is a STANDING queue that (a) is pure waterfall
	// lag and (b) inflates every rtt sample, ratcheting the window open (sim: 2.5 here ->
	// in-flight pinned at 1-2 MB regardless of link rate, 4.5 s lag at 2 Mbit).
	// But window<->deliv is circular (the flow measures its own cap), so a cruise multiplier
	// alone discovers bandwidth at only ~7%/window (sim: 50 Mbit stuck at 21% utilization).
	// BBR's answer, used here: STARTUP phase at 2.5x until the delivery rate plateaus, then
	// CRUISE at 1.3x with a brief 1.5x probe window every few seconds to sniff freed headroom.
	static constexpr double WIN_BDP_MULT = 1.3;
	static constexpr double WIN_STARTUP_MULT = 2.5;
	static constexpr double WIN_PROBE_MULT = 1.5;
	static constexpr double PROBE_EVERY_MS = 5000.0;
	static constexpr double WIN_FLOOR_BYTES = 262144.0;  // pre-measurement cap (never binds a LAN)
	static constexpr double WIN_MIN_BYTES = 184320.0;    // ~3 messages: self-clock never starves
	static constexpr double STARTUP_GIVEUP_MS = 3000.0;  // no ACKs by then -> legacy client
	static constexpr double DELIV_WIN_MS = 700.0;    // delivery-rate measurement window

	struct Sent { uint32_t seq; uint64_t cum; double t_ms; };
	Sent ring[RING] = {};
	int ring_n = 0, ring_w = 0;
	uint64_t cum_sent = 0;          // wire bytes of all SPECTRA sent (post-compression)
	uint64_t cum_acked = 0;
	double win_t0_ms = 0;           // delivery-rate window start
	uint64_t win_cum0 = 0;
	double deliv_Bps = 0;           // 0 = pipe not measured yet
	double rtt_min_ms = RTT_INIT_MS;
	bool rtt_measured = false;      // first real sample REPLACES the init (an init below the
	                                // true rtt undersizes the window and spirals deliv down)
	double rtt_min_stamp = 0;
	double rtt_last_ms = RTT_INIT_MS;
	double t0_ms = -1;              // first tick (connect time)
	bool acks_seen = false;
	uint32_t drops = 0;             // txq-cap message losses (legacy brake evidence, lifetime)
	bool drop_pending = false;      // set by on_drop, stamped by the next tick
	double last_drop_ms = -1;       // recency of the legacy brake (limited() looks at THIS, not the lifetime count)
	bool win_had_block = false;     // any window-blocked tick since win_t0 (app-limited detector)
	bool win_had_drop = false;      // any txq-cap drop since win_t0: a drop is hard evidence the
	                                // pipe is overrun, so the window may LOWER the estimate even
	                                // without blocking (an offered-load ~= link-rate stream never
	                                // blocks, and a startup-burst overestimate would stick while
	                                // drops keep the limited() chip lit with the stale number)
	double blocked_ema = 0;         // fraction of recent ticks spent window-blocked (indicator)
	double last_tick_ms = 0;
	bool startup = true;            // bandwidth-discovery phase (2.5x window)
	int plateau_cnt = 0;
	double probe_at_ms = 0;         // next CRUISE probe
	bool probing = false;
	double probe_end_ms = 0;

	void on_send(uint32_t seq, size_t wire_bytes, double now_ms) {
		cum_sent += wire_bytes;
		ring[ring_w] = {seq, cum_sent, now_ms};
		ring_w = (ring_w + 1) % RING;
		if(ring_n < RING) { ring_n++; }
	}

	void on_drop() { drops++; drop_pending = true; win_had_drop = true; }

	void on_ack(uint32_t seq, double now_ms) {
		acks_seen = true;
		for(int k = 0; k < ring_n; k++) {           // newest-first: ACKs echo recent messages
			int i = (ring_w - 1 - k + RING) % RING;
			if(ring[i].seq != seq) { continue; }
			double rtt = now_ms - ring[i].t_ms;
			if(rtt >= 0) {
				rtt_last_ms = rtt;
				if(!rtt_measured || rtt < rtt_min_ms) { rtt_min_ms = rtt; rtt_measured = true; rtt_min_stamp = now_ms; }
				else if(now_ms - rtt_min_stamp > RTT_WIN_MS) {
					// gentle upward drift only: hard-refreshing to the current sample lets
					// our own standing queue ratchet the estimate (and the window) open
					rtt_min_ms = rtt < rtt_min_ms * 1.05 ? rtt : rtt_min_ms * 1.05;
					rtt_min_stamp = now_ms;
				}
			}
			if(ring[i].cum > cum_acked) {
				cum_acked = ring[i].cum;
				// windowed delivery rate: bytes acked over ~DELIV_WIN. Per-ack instantaneous
				// rates are useless - ACKs arrive clustered, and averaging instantaneous
				// rates biases low (sim: 20/50 Mbit stuck at ~70% utilization on an EWMA)
				if(win_t0_ms <= 0) { win_t0_ms = now_ms; win_cum0 = cum_acked; }
				else if(now_ms - win_t0_ms >= DELIV_WIN_MS) {
					double r = (cum_acked - win_cum0) * 1000.0 / (now_ms - win_t0_ms);
					// BBR-style app-limited guard: a window in which the sender never hit the
					// cap measures the OFFERED LOAD, not the link. A sweep's ~0.5 Mbit/s of
					// rows decayed deliv to junk ("link-limited 0.5 Mbit/s" while the link
					// carries 20), and the return to RT then started on a strangled window.
					// App-limited samples may only RAISE the estimate; the startup/probe
					// machinery pauses with them (probing an unfilled pipe learns nothing).
					if(win_had_block || win_had_drop || r > deliv_Bps) {
						double prev = deliv_Bps;
						deliv_Bps = deliv_Bps > 0 ? (deliv_Bps + r) * 0.5 : r;
						if(startup) {
							// leave STARTUP once the rate stops growing (2 flat windows)
							plateau_cnt = (prev > 0 && r < prev * 1.10) ? plateau_cnt + 1 : 0;
							if(plateau_cnt >= 2) { startup = false; probe_at_ms = now_ms + PROBE_EVERY_MS; }
						} else if(probing && now_ms >= probe_end_ms) {
							probing = false;
							probe_at_ms = now_ms + PROBE_EVERY_MS;
							if(prev > 0 && r > prev * 1.15) { startup = true; plateau_cnt = 0; }   // headroom found
						} else if(!probing && now_ms >= probe_at_ms) {
							probing = true;
							probe_end_ms = now_ms + DELIV_WIN_MS;
						}
					}
					win_t0_ms = now_ms;
					win_cum0 = cum_acked;
					win_had_block = false;
					win_had_drop = false;
				}
			}
			return;
		}
	}

	double window_bytes() const {
		if(deliv_Bps <= 0) { return WIN_FLOOR_BYTES; }
		double mult = startup ? WIN_STARTUP_MULT : (probing ? WIN_PROBE_MULT : WIN_BDP_MULT);
		double w = deliv_Bps * mult * rtt_min_ms / 1000.0;
		// once measured, trust the BDP even below the startup floor (a 2 Mbit link wants
		// ~100 KB) - but never starve below ~3 messages or the self-clock stalls
		return w < WIN_MIN_BYTES ? WIN_MIN_BYTES : w;
	}

	bool win_blocked(double now_ms) const {
		if(t0_ms < 0) { return false; }
		if(!acks_seen && now_ms - t0_ms > STARTUP_GIVEUP_MS) { return false; }   // legacy client
		return static_cast<double>(cum_sent - cum_acked) > window_bytes();
	}

	// call once per pump iteration (before the send loops); maintains the indicator state
	void tick(double now_ms) {
		if(t0_ms < 0) { t0_ms = now_ms; last_tick_ms = now_ms; }
		if(drop_pending) { last_drop_ms = now_ms; drop_pending = false; }
		double dt = now_ms - last_tick_ms;
		last_tick_ms = now_ms;
		double a = dt / 1000.0;                     // ~1 s time constant for the indicator
		if(a > 1.0) { a = 1.0; }
		bool blk = win_blocked(now_ms);
		if(blk) { win_had_block = true; }
		blocked_ema += ((blk ? 1.0 : 0.0) - blocked_ema) * a;
		if(blocked_ema < 0) { blocked_ema = 0; }
	}

	// "we are bandwidth-limited": the window did real work RECENTLY, or the legacy brake
	// dropped RECENTLY. `drops > 0` used to sit here and LATCH: one early loss kept the
	// GUI's link-limited chip on for the connection's whole life - including all through
	// a 0.5 Mbit/s sweep ("that sounds like it's made up", 2026-07-12).
	bool limited() const { return blocked_ema > 0.05 || (last_drop_ms >= 0 && last_tick_ms - last_drop_ms < 5000.0); }
	// achievable fraction of the full stream, permille (STATUS field for the GUI)
	uint32_t rate_permille(double target_Bps) const {
		if(!limited() || target_Bps <= 0 || deliv_Bps <= 0) { return 1000; }
		double f = deliv_Bps / target_Bps;
		return f >= 1.0 ? 1000 : static_cast<uint32_t>(f * 1000.0);
	}
	uint32_t deliv_kbps() const { return static_cast<uint32_t>(deliv_Bps * 8.0 / 1000.0); }
};
