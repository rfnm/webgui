#include "ble_coded.h"

#include <algorithm>
#include <array>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace ismmon {

// Convolutional code, constraint length K=4 (3 memory bits). Tap masks over the 4-bit window
// [a_i, a_{i-1}, a_{i-2}, a_{i-3}] (a_i = bit 3, the current input = octal MSB, per the MATLAB
// poly2trellis convention). VERIFIED vs Core Spec Vol 6 Part B 3.3.1: generators g0 = 1+x+x^2+x^3
// (octal 17) and g1 = 1+x^2+x^3 (octal 13); c0 (from g0) is transmitted first, then c1. MATLAB's
// spec-compliant BLE FEC = poly2trellis(4, [17 13]). Sources: bluetooth.com "going the distance",
// EDN/Silabs/AllAboutCircuits, arXiv 2305.10138. [was 0b1101 = octal 15 = 1+x+x^3: tapped delay-1
// instead of delay-2 -> would have failed on-air; corrected 2026-07-09.]
static const int CONV_G0 = 0b1111;   // c0 = a_i ^ a_{i-1} ^ a_{i-2} ^ a_{i-3}   (g0 = 1+x+x^2+x^3, octal 17)
static const int CONV_G1 = 0b1011;   // c1 = a_i          ^ a_{i-2} ^ a_{i-3}   (g1 = 1+x^2+x^3,   octal 13)

static inline int par(int x) { return __builtin_popcount((unsigned)x) & 1; }

bits ble_conv_encode(const bits& in) {
	bits out;
	out.reserve(in.size() * 2);
	int s0 = 0, s1 = 0, s2 = 0;   // a_{i-1}, a_{i-2}, a_{i-3}
	for(uint8_t a : in) {
		int w = ((a & 1) << 3) | (s0 << 2) | (s1 << 1) | s2;
		out.push_back((uint8_t)par(w & CONV_G0));
		out.push_back((uint8_t)par(w & CONV_G1));
		s2 = s1; s1 = s0; s0 = a & 1;
	}
	return out;
}

bits ble_viterbi_decode(const std::vector<float>& soft, bool terminated) {
	int nbits = (int)soft.size() / 2;
	const int NS = 8;   // 3 memory bits: state = (a_{i-1},a_{i-2},a_{i-3})
	// Backward trellis: each next-state ns has exactly 2 predecessors p0<p1; the input bit is u = ns>>2
	// and the +-1 output signs come from the (predecessor,u) window. Gathering (vs the forward scatter)
	// makes the ACS branch-free and vectorizable across the 8 states. Equivalences that keep this
	// bit-for-bit == the old forward scan: bm = ra*Sa+rb*Sb (then + pm) as before; a tie picks p0 (the
	// forward scan visited s=0..7 with strict '>', so the lower predecessor won ties); an unreached
	// predecessor stays at -1e30 and loses, matching the old `continue`.
	static int P0[NS], P1[NS];
	static float S0a[NS], S0b[NS], S1a[NS], S1b[NS];
	static bool init = false;
	if(!init) {
		for(int ns = 0; ns < NS; ns++) {
			int u = ns >> 2, p0 = (ns & 3) << 1, p1 = p0 | 1;
			P0[ns] = p0; P1[ns] = p1;
			int w0 = (u << 3) | p0, w1 = (u << 3) | p1;   // [a_i, s2, s1, s0]
			S0a[ns] = par(w0 & CONV_G0) ? 1.f : -1.f; S0b[ns] = par(w0 & CONV_G1) ? 1.f : -1.f;
			S1a[ns] = par(w1 & CONV_G0) ? 1.f : -1.f; S1b[ns] = par(w1 & CONV_G1) ? 1.f : -1.f;
		}
		init = true;
	}
	std::vector<std::array<uint8_t, NS>> tb(nbits);
	float pm[NS], npm[NS];
	for(int i = 0; i < NS; i++) { pm[i] = -1e30f; }
	pm[0] = 0.f;
	for(int t = 0; t < nbits; t++) {
		float ra = soft[2 * t], rb = soft[2 * t + 1];
		int s = 0;
#ifdef __ARM_NEON
		for(; s + 4 <= NS; s += 4) {
			float g0[4], g1[4];
			for(int i = 0; i < 4; i++) { g0[i] = pm[P0[s + i]]; g1[i] = pm[P1[s + i]]; }
			float32x4_t bm0 = vaddq_f32(vmulq_n_f32(vld1q_f32(&S0a[s]), ra), vmulq_n_f32(vld1q_f32(&S0b[s]), rb));
			float32x4_t bm1 = vaddq_f32(vmulq_n_f32(vld1q_f32(&S1a[s]), ra), vmulq_n_f32(vld1q_f32(&S1b[s]), rb));
			float32x4_t c0 = vaddq_f32(vld1q_f32(g0), bm0), c1 = vaddq_f32(vld1q_f32(g1), bm1);
			uint32x4_t ge = vcgeq_f32(c0, c1);   // c0 >= c1 -> predecessor p0 wins (incl. ties)
			vst1q_f32(&npm[s], vbslq_f32(ge, c0, c1));
			uint32_t m[4]; vst1q_u32(m, ge);
			for(int i = 0; i < 4; i++) { tb[t][s + i] = (uint8_t)(m[i] ? P0[s + i] : P1[s + i]); }
		}
#endif
		for(; s < NS; s++) {
			float bm0 = ra * S0a[s] + rb * S0b[s], bm1 = ra * S1a[s] + rb * S1b[s];
			float c0 = pm[P0[s]] + bm0, c1 = pm[P1[s]] + bm1;
			if(c0 >= c1) { npm[s] = c0; tb[t][s] = (uint8_t)P0[s]; }
			else { npm[s] = c1; tb[t][s] = (uint8_t)P1[s]; }
		}
		for(int i = 0; i < NS; i++) { pm[i] = npm[i]; }
	}
	int s = 0;
	if(!terminated || pm[0] < -1e29f) {
		float best = pm[0];
		for(int i = 1; i < NS; i++) { if(pm[i] > best) { best = pm[i]; s = i; } }   // first argmax (== std::max_element)
	}
	bits out(nbits);
	for(int t = nbits - 1; t >= 0; t--) {
		int ps = tb[t][s];
		out[t] = (uint8_t)((s >> 2) & 1);   // input bit u took ps->s: s = (u<<2)|(ps>>1) => u = s>>2
		s = ps;
	}
	return out;
}

static const int S8_PAT0[4] = { 0, 0, 1, 1 };   // VERIFIED vs spec: S=8 pattern mapper 0->0011, 1->1100
static const int S8_PAT1[4] = { 1, 1, 0, 0 };

bits ble_pattern_map_s8(const bits& coded) {
	bits out;
	out.reserve(coded.size() * 4);
	for(uint8_t c : coded) {
		const int* p = (c & 1) ? S8_PAT1 : S8_PAT0;
		for(int i = 0; i < 4; i++) { out.push_back((uint8_t)p[i]); }
	}
	return out;
}

const bits& ble_coded_preamble() {
	static bits pre;
	if(pre.empty()) {
		static const int pat[8] = { 0, 0, 1, 1, 1, 1, 0, 0 };   // VERIFIED: coded preamble 00111100 x10 = 80 sym
		for(int r = 0; r < 10; r++) { for(int i = 0; i < 8; i++) { pre.push_back((uint8_t)pat[i]); } }
	}
	return pre;
}

bits ble_build_coded_stream(const std::vector<uint8_t>& pdu, int chan_index, int ci_s,
                            uint32_t aa, uint32_t crc_init) {
	bits sym = ble_coded_preamble();   // 80

	// FEC block 1: AA(32, LE bytes LSB-first) + CI(2) + TERM1(3), always S=8
	uint8_t ab[4] = { (uint8_t)(aa & 0xFF), (uint8_t)((aa >> 8) & 0xFF), (uint8_t)((aa >> 16) & 0xFF), (uint8_t)((aa >> 24) & 0xFF) };
	bits fec1 = bytes_to_bits(ab, 4);
	int civ = (ci_s == 2) ? 1 : 0;                       // VERIFIED: CI=0 -> S8, CI=1 -> S2
	fec1.push_back((uint8_t)(civ & 1));
	fec1.push_back((uint8_t)((civ >> 1) & 1));
	for(int i = 0; i < 3; i++) { fec1.push_back(0); }    // TERM1
	bits fec1_sym = ble_pattern_map_s8(ble_conv_encode(fec1));
	sym.insert(sym.end(), fec1_sym.begin(), fec1_sym.end());

	// FEC block 2: whiten(PDU + CRC24) + TERM2(3), coded at S=ci_s
	bits pdu_bits = bytes_to_bits(pdu.data(), pdu.size());
	uint32_t crc = ble_crc24(pdu_bits, crc_init);
	std::vector<uint8_t> full = pdu;
	full.push_back((uint8_t)(crc & 0xFF));
	full.push_back((uint8_t)((crc >> 8) & 0xFF));
	full.push_back((uint8_t)((crc >> 16) & 0xFF));
	bits body = bytes_to_bits(full.data(), full.size());
	ble_whiten(body, chan_index);
	for(int i = 0; i < 3; i++) { body.push_back(0); }    // TERM2 (not whitened)
	bits fec2_coded = ble_conv_encode(body);
	bits fec2_sym = (ci_s == 8) ? ble_pattern_map_s8(fec2_coded) : fec2_coded;
	sym.insert(sym.end(), fec2_sym.begin(), fec2_sym.end());
	return sym;
}

} // namespace ismmon
