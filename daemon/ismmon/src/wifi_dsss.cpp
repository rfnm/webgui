#include "wifi_dsss.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace ismmon {

static const int BARKER[11] = { 1, -1, 1, 1, -1, 1, 1, 1, -1, -1, -1 };
static const int SPS = 25;   // samples/symbol at 25 MSPS, 1 Msym/s

// Barker symbol waveform (rect chips) at 25 samples/symbol
static const std::vector<float>& barker_wave() {
	static std::vector<float> h;
	if(h.empty()) {
		h.resize(SPS);
		for(int n = 0; n < SPS; n++) { h[n] = (float)BARKER[(int)(n * 11 / SPS)]; }
	}
	return h;
}

// Barker matched-filter despread: y[p] = sum_{k<SPS} iq[p+k]*h[k], real taps h. This is THE Wi-Fi hot
// loop (O(n*25) per window, run over every window of every scan). NEON on aarch64 (the board);
// bit-for-bit-equivalent scalar reference elsewhere (x86 stays the A/B reference - do not vectorise it).
static void despread_barker(const cf* iq, size_t n, const float* h, cf* y) {
	size_t p = 0;
#ifdef __ARM_NEON
	// CACHE-BLOCKED for the i.MX8MP Cortex-A53. The previous version deinterleaved the WHOLE window into
	// re[]/im[] float buffers (~2*n*4B ~= 700 KB at a 3.5 ms window) - far over the A53 cluster's ~512 KB
	// shared L2 - so the FIR re-streamed re[]/im[] from DRAM. Now we tile: each tile deinterleaves only
	// its input span (T outputs + SPS-1 halo) into a ~16 KB on-stack scratch that stays L1-resident
	// (32 KB L1D), then runs the SAME 4-output real FIR (vmlaq, bit-identical to the old NEON) over it.
	// Broadcast taps are hoisted out of the per-group loop. The 4-wide output set [0,Q4) and the scalar
	// tail split are IDENTICAL to the old loop, so output is bit-for-bit unchanged.
	constexpr int T = 2048;                 // outputs per tile (multiple of 4); scratch 2*(T+SPS)*4B ~16 KB
	float re[T + SPS + 4], im[T + SPS + 4];
	float32x4_t hb[SPS];                    // hoisted broadcast taps (computed once, not per output group)
	for(int k = 0; k < SPS; k++) { hb[k] = vdupq_n_f32(h[k]); }
	// total 4-wide outputs [0,Q4): Q4 = largest (mult-of-4)+4 with (Q4-4)+SPS+3 < n == the old guard.
	size_t Q4 = (n >= (size_t)(SPS + 4)) ? (((n - (size_t)SPS - 4) / 4) * 4 + 4) : 0;
	while(p < Q4) {
		size_t tout = ((size_t)T < Q4 - p) ? (size_t)T : (Q4 - p);   // both multiples of 4
		size_t span = tout + SPS - 1;                                // input samples this tile (p+span<=n)
		size_t i = 0;
		for(; i + 4 <= span; i += 4) {
			__builtin_prefetch((const char*)(iq + p + i) + 320, 0, 0);
			float32x4x2_t v = vld2q_f32((const float*)(iq + p + i));
			vst1q_f32(&re[i], v.val[0]);
			vst1q_f32(&im[i], v.val[1]);
		}
		for(; i < span; i++) { re[i] = iq[p + i].real(); im[i] = iq[p + i].imag(); }
		for(size_t j = 0; j < tout; j += 4) {
			float32x4_t ar = vdupq_n_f32(0.f), ai = vdupq_n_f32(0.f);
			for(int k = 0; k < SPS; k++) {
				ar = vmlaq_f32(ar, vld1q_f32(&re[j + k]), hb[k]);
				ai = vmlaq_f32(ai, vld1q_f32(&im[j + k]), hb[k]);
			}
			float arr[4], aii[4];
			vst1q_f32(arr, ar);
			vst1q_f32(aii, ai);
			y[p + j] = cf(arr[0], aii[0]); y[p + j + 1] = cf(arr[1], aii[1]);
			y[p + j + 2] = cf(arr[2], aii[2]); y[p + j + 3] = cf(arr[3], aii[3]);
		}
		p += tout;
	}
#endif
	for(; p + SPS <= n; p++) {
		cf a(0, 0);
		for(int k = 0; k < SPS; k++) { a += iq[p + k] * h[k]; }
		y[p] = a;
	}
}

// 802.11b self-synchronising scrambler / descrambler, g(x)=x^7+x^4+1.
// scramble: s[n]=d[n]^s[n-4]^s[n-7];  descramble: d[n]=s[n]^s[n-4]^s[n-7].
static void dsss_scramble(std::vector<uint8_t>& d, uint8_t init) {
	uint8_t s[7];
	for(int i = 0; i < 7; i++) { s[i] = (init >> i) & 1; }   // s[-1..-7]
	for(auto& b : d) {
		uint8_t o = (uint8_t)(b ^ s[3] ^ s[6]);
		b = o;
		for(int i = 6; i > 0; i--) { s[i] = s[i - 1]; }
		s[0] = o;
	}
}
static std::vector<uint8_t> dsss_descramble(const std::vector<uint8_t>& s) {
	std::vector<uint8_t> d(s.size());
	uint8_t r[7] = { 1, 1, 1, 1, 1, 1, 1 };   // self-sync: fills from received bits
	for(size_t i = 0; i < s.size(); i++) {
		d[i] = (uint8_t)(s[i] ^ r[3] ^ r[6]);
		for(int k = 6; k > 0; k--) { r[k] = r[k - 1]; }
		r[0] = s[i];
	}
	return d;
}

// 802.11 PLCP CRC-16 = CRC-16/X-25 (poly 0x1021 reflected = 0x8408, init 0xFFFF, refin/refout,
// xorout 0xFFFF). Validated against real on-air PLCP headers 2026-07-09.
static uint16_t crc16_ccitt(const std::vector<uint8_t>& bytes) {
	uint16_t c = 0xFFFF;
	for(uint8_t b : bytes) {
		c ^= b;
		for(int i = 0; i < 8; i++) { c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1); }
	}
	return (uint16_t)(c ^ 0xFFFF);
}

static uint8_t bits_to_byte_lsb(const uint8_t* b) {
	uint8_t v = 0;
	for(int i = 0; i < 8; i++) { v |= (b[i] & 1) << i; }
	return v;
}

// long-preamble SFD, 0xF3A0 (16 bits). Search the descrambled stream for it (try MSB- and LSB-first).
static const uint8_t SFD_BITS[16] = { 0,0,0,0,0,1,0,1,1,1,0,0,1,1,1,1 };   // 0xF3A0, on-air LSB-first (validated vs real captures)

std::vector<cf> dsss_modulate(const std::vector<uint8_t>& psdu, int rate_mbps) {
	const std::vector<float>& h = barker_wave();
	// build the PLCP bit stream (SYNC ones + SFD + SIGNAL + SERVICE + LENGTH + CRC16 + PSDU)
	std::vector<uint8_t> bits;
	for(int i = 0; i < 128; i++) { bits.push_back(1); }               // SYNC = scrambled ones
	for(int i = 0; i < 16; i++) { bits.push_back(SFD_BITS[i]); }        // SFD
	uint8_t sig = rate_mbps == 2 ? 0x14 : 0x0A;                        // 1 or 2 Mbps
	int len_us = rate_mbps == 2 ? (int)psdu.size() * 4 : (int)psdu.size() * 8;
	std::vector<uint8_t> hdr = { sig, 0x00, (uint8_t)(len_us & 0xff), (uint8_t)((len_us >> 8) & 0xff) };
	uint16_t crc = crc16_ccitt(hdr);
	auto push_byte_lsb = [&](uint8_t v) { for(int i = 0; i < 8; i++) { bits.push_back((v >> i) & 1); } };
	for(uint8_t b : hdr) { push_byte_lsb(b); }
	push_byte_lsb((uint8_t)(crc & 0xff));
	push_byte_lsb((uint8_t)((crc >> 8) & 0xff));
	int hdr_bits = (int)bits.size();                                  // preamble+header end (all DBPSK 1 Mbps)
	for(uint8_t b : psdu) { push_byte_lsb(b); }

	// scramble whole stream (self-sync scrambler, init all-ones)
	dsss_scramble(bits, 0x7f);

	// modulate: header region DBPSK (1 sym/bit); PSDU region DBPSK(1Mbps) or DQPSK(2Mbps)
	std::vector<cf> out;
	double phase = 0;
	auto emit = [&](double dphi) { phase += dphi; cf v((float)cos(phase), (float)sin(phase)); for(int n = 0; n < SPS; n++) { out.push_back(v * h[n]); } };
	int i = 0;
	for(; i < hdr_bits; i++) { emit(bits[i] ? M_PI : 0); }            // DBPSK
	if(rate_mbps == 2) {
		for(; i + 1 < (int)bits.size(); i += 2) {                    // DQPSK dibits
			int dib = (bits[i] << 1) | bits[i + 1];
			static const double dq[4] = { 0, M_PI / 2, M_PI, 3 * M_PI / 2 };
			emit(dq[dib]);
		}
	} else {
		for(; i < (int)bits.size(); i++) { emit(bits[i] ? M_PI : 0); }
	}
	return out;
}

// NEON complex-magnitude helpers (aarch64); scalar std::abs is the numeric reference elsewhere.
// sum_abs -> RSSI; fill_mag -> the symbol-timing search reads precomputed |y| (strided adds, no sqrt).
static double sum_abs(const cf* v, size_t n) {
	size_t i = 0;
	double s = 0;
#ifdef __ARM_NEON
	// Two independent accumulators (8 samples/iter) hide the A53's multi-cycle NEON sqrt latency on the
	// in-order pipe; single horizontal reduce at the end. (RSSI-only; not a decode-critical value.)
	float32x4_t a0 = vdupq_n_f32(0.f), a1 = vdupq_n_f32(0.f);
	for(; i + 8 <= n; i += 8) {
		float32x4x2_t z0 = vld2q_f32((const float*)(v + i));
		float32x4x2_t z1 = vld2q_f32((const float*)(v + i + 4));
		a0 = vaddq_f32(a0, vsqrtq_f32(vaddq_f32(vmulq_f32(z0.val[0], z0.val[0]), vmulq_f32(z0.val[1], z0.val[1]))));
		a1 = vaddq_f32(a1, vsqrtq_f32(vaddq_f32(vmulq_f32(z1.val[0], z1.val[0]), vmulq_f32(z1.val[1], z1.val[1]))));
	}
	for(; i + 4 <= n; i += 4) {
		float32x4x2_t z = vld2q_f32((const float*)(v + i));
		a0 = vaddq_f32(a0, vsqrtq_f32(vaddq_f32(vmulq_f32(z.val[0], z.val[0]), vmulq_f32(z.val[1], z.val[1]))));
	}
	s = (double)vaddvq_f32(vaddq_f32(a0, a1));
#endif
	for(; i < n; i++) { s += std::abs(v[i]); }
	return s;
}
static void fill_mag(const cf* v, size_t n, float* mag) {
	size_t i = 0;
#ifdef __ARM_NEON
	// 2x unrolled (independent sqrt chains) + prefetch the streaming input. Elementwise -> bit-identical.
	for(; i + 8 <= n; i += 8) {
		__builtin_prefetch((const char*)(v + i) + 320, 0, 0);
		float32x4x2_t z0 = vld2q_f32((const float*)(v + i));
		float32x4x2_t z1 = vld2q_f32((const float*)(v + i + 4));
		vst1q_f32(mag + i, vsqrtq_f32(vaddq_f32(vmulq_f32(z0.val[0], z0.val[0]), vmulq_f32(z0.val[1], z0.val[1]))));
		vst1q_f32(mag + i + 4, vsqrtq_f32(vaddq_f32(vmulq_f32(z1.val[0], z1.val[0]), vmulq_f32(z1.val[1], z1.val[1]))));
	}
	for(; i + 4 <= n; i += 4) {
		float32x4x2_t z = vld2q_f32((const float*)(v + i));
		vst1q_f32(mag + i, vsqrtq_f32(vaddq_f32(vmulq_f32(z.val[0], z.val[0]), vmulq_f32(z.val[1], z.val[1]))));
	}
#endif
	for(; i < n; i++) { mag[i] = std::abs(v[i]); }
}

bool dsss_decode_burst(const cf* iq, size_t n, int channel, wifi_packet& out) {
	out.channel = channel;
	const std::vector<float>& h = barker_wave();
	if(n < (size_t)(200 * SPS)) { return false; }
	double ms = sum_abs(iq, n);
	out.rssi_db = 20.0f * std::log10((float)(ms / n) + 1e-9f);

	// matched-filter despread (NEON on aarch64)
	std::vector<cf> y(n);
	despread_barker(iq, n, h.data(), y.data());
	// symbol timing: pick t0 in [0,SPS) maximizing mean |y| at symbol centres. Precompute |y| once
	// (NEON), then the strided per-t0 sums are cheap adds (no sqrt) - the main post-despread DSSS cost.
	int nsym = (int)((n - SPS) / SPS);
	if(nsym < 200) { return false; }
	std::vector<float> mag(n);
	fill_mag(y.data(), n, mag.data());
	int best_t = 0; double best = 0;
	for(int t0 = 0; t0 < SPS; t0++) {
		double s = 0; for(int k = 0; k < nsym; k++) { s += mag[t0 + (size_t)k * SPS]; }
		if(s > best) { best = s; best_t = t0; }
	}
	std::vector<cf> sym(nsym);
	for(int k = 0; k < nsym; k++) { sym[k] = y[best_t + (size_t)k * SPS]; }

	// CFO: fold the DBPSK preamble differential (0/pi) over the first ~90 symbols (the SYNC region);
	// clamp to a real range (a whole-burst estimate aliases on DQPSK data -> garbage MHz values).
	cf acc(0, 0);
	int ne = std::min(nsym - 1, 90);
	for(int k = 1; k <= ne; k++) { cf d = sym[k] * std::conj(sym[k - 1]); acc += d * d; }
	double cfo = std::atan2((double)acc.imag(), (double)acc.real()) / 2.0;
	double maxcfo = 2 * M_PI * 80e3 / 1e6;   // +-80 kHz / 1 Msym
	if(cfo > maxcfo || cfo < -maxcfo) { cfo = 0; }
	cf derot((float)cos(-cfo), (float)sin(-cfo));

	// DBPSK bits over the whole burst: bit = Re(sym[k]*conj(sym[k-1])*derot) < 0. NEON computes the
	// differential real part 4-wide; scalar fallback is the reference.
	std::vector<uint8_t> raw(nsym, 0);
	{
		int k = 1;
#ifdef __ARM_NEON
		float32x4_t vDr = vdupq_n_f32(derot.real()), vDi = vdupq_n_f32(derot.imag()), vz = vdupq_n_f32(0.f);
		for(; k + 4 <= nsym; k += 4) {
			float32x4x2_t a = vld2q_f32((const float*)(sym.data() + k));       // sym[k..k+3]
			float32x4x2_t b = vld2q_f32((const float*)(sym.data() + k - 1));   // sym[k-1..k+2]
			float32x4_t pr = vaddq_f32(vmulq_f32(a.val[0], b.val[0]), vmulq_f32(a.val[1], b.val[1]));   // Re(a*conj(b))
			float32x4_t pi = vsubq_f32(vmulq_f32(a.val[1], b.val[0]), vmulq_f32(a.val[0], b.val[1]));   // Im(a*conj(b))
			float32x4_t re = vsubq_f32(vmulq_f32(pr, vDr), vmulq_f32(pi, vDi));   // Re(p*derot)
			uint32_t m[4];
			vst1q_u32(m, vcltq_f32(re, vz));
			raw[k] = m[0] ? 1 : 0; raw[k + 1] = m[1] ? 1 : 0; raw[k + 2] = m[2] ? 1 : 0; raw[k + 3] = m[3] ? 1 : 0;
		}
#endif
		for(; k < nsym; k++) { cf d = sym[k] * std::conj(sym[k - 1]) * derot; raw[k] = (d.real() < 0) ? 1 : 0; }
	}
	std::vector<uint8_t> desc = dsss_descramble(raw);
	if(getenv("DSSS_DBG")) {
		int bestrun = 0, runpos = 0, cur = 0;   // longest run of ones = the descrambled SYNC field
		for(int i = 0; i < nsym; i++) { if(desc[i]) { cur++; if(cur > bestrun) { bestrun = cur; runpos = i - cur + 1; } } else { cur = 0; } }
		int e = runpos + bestrun;
		fprintf(stderr, "[dsss] nsym=%d ones-run=%d@%d, 72 bits after run:", nsym, bestrun, runpos);
		for(int i = e; i < e + 72 && i < nsym; i++) { fprintf(stderr, "%d", desc[i]); }
		fprintf(stderr, "\n");
	}

	// find the SFD (after the SYNC). Search descrambled bits for SFD_BITS (and its bit-inverse, since
	// DBPSK has a 180 deg ambiguity that the differential + descrambler mostly resolve but be safe).
	int sfd_end = -1;
	bool inv = false;
	for(int p = 0; p + 16 <= nsym && sfd_end < 0; p++) {
		int m1 = 0, m0 = 0;
		for(int i = 0; i < 16; i++) { m1 += (desc[p + i] == SFD_BITS[i]); m0 += (desc[p + i] == (SFD_BITS[i] ^ 1)); }
		if(m1 >= 15) { sfd_end = p + 16; }
		else if(m0 >= 15) { sfd_end = p + 16; inv = true; }   // 180-deg DBPSK polarity
	}
	if(getenv("DSSS_DBG2")) { fprintf(stderr, "[dsss2] sfd_end=%d inv=%d\n", sfd_end, inv); }
	if(sfd_end < 0 || sfd_end + 48 > nsym) { return false; }
	if(inv) { for(auto& b : desc) { b ^= 1; } }

	// PLCP header: 48 bits LSB-first per octet
	uint8_t hb[6];
	for(int i = 0; i < 6; i++) { hb[i] = bits_to_byte_lsb(&desc[sfd_end + i * 8]); }
	std::vector<uint8_t> hdr = { hb[0], hb[1], hb[2], hb[3] };
	uint16_t crc_rx = (uint16_t)(hb[4] | (hb[5] << 8));
	if(getenv("DSSS_DBG2")) { fprintf(stderr, "[dsss2] hb=%02x %02x %02x %02x crc_rx=%04x calc=%04x\n", hb[0], hb[1], hb[2], hb[3], crc_rx, crc16_ccitt(hdr)); }
	if(crc16_ccitt(hdr) != crc_rx) {
		// try the 180-deg inverted stream (resolve DBPSK polarity)
		for(auto& b : desc) { b ^= 1; }
		for(int i = 0; i < 6; i++) { hb[i] = bits_to_byte_lsb(&desc[sfd_end + i * 8]); }
		hdr = { hb[0], hb[1], hb[2], hb[3] };
		crc_rx = (uint16_t)(hb[4] | (hb[5] << 8));
		if(crc16_ccitt(hdr) != crc_rx) { return false; }
	}
	int sig = hb[0];
	out.rate_mbps = (sig == 0x0A) ? 1 : (sig == 0x14) ? 2 : (sig == 0x37) ? 5 : (sig == 0x6E) ? 11 : 0;
	int len_us = hb[2] | (hb[3] << 8);
	out.sig_ok = (out.rate_mbps > 0 && len_us > 0);
	if(!out.sig_ok) { return true; }
	out.length = (out.rate_mbps == 2) ? len_us / 4 : len_us / 8;   // octets (1/2 Mbps)

	// PSDU. Header ends at sfd_end+48 (symbol index). 1 Mbps DBPSK: already in `desc`. 2 Mbps DQPSK
	// re-demods the PSDU symbols as dibits.
	std::vector<uint8_t> psdu_bits;
	int p0 = sfd_end + 48;
	if(out.rate_mbps == 1) {
		for(int i = p0; i < p0 + out.length * 8 && i < nsym; i++) { psdu_bits.push_back(desc[i]); }
	} else if(out.rate_mbps == 2) {
		std::vector<uint8_t> db;
		for(int k = p0; k < nsym; k++) {
			cf d = sym[k] * std::conj(sym[k - 1]) * derot;
			double ph = std::atan2((double)d.imag(), (double)d.real());
			if(ph < 0) { ph += 2 * M_PI; }
			int dib = (int)std::lround(ph / (M_PI / 2)) & 3;
			static const int demap[4] = { 0, 1, 2, 3 };
			int v = demap[dib];
			db.push_back((v >> 1) & 1); db.push_back(v & 1);
		}
		std::vector<uint8_t> dd = dsss_descramble(db);   // note: re-descramble misaligned; best-effort for 2 Mbps
		for(int i = 0; i < (int)dd.size() && i < out.length * 8; i++) { psdu_bits.push_back(dd[i]); }
	} else {
		return true;   // 5.5/11 Mbps CCK not decoded
	}
	int nbytes = (int)psdu_bits.size() / 8;
	std::vector<uint8_t> mac(nbytes);
	for(int i = 0; i < nbytes; i++) { mac[i] = bits_to_byte_lsb(&psdu_bits[i * 8]); }
	if(getenv("DSSS_FC") && nbytes >= 1) { fprintf(stderr, "FC %02x n %d\n", mac[0], nbytes); }
	if(nbytes >= 28) { wifi_parse_mac(mac, out); }
	return true;
}

} // namespace ismmon
