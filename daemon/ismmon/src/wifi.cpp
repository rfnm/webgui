#include "wifi.h"
#include "wifi_ht.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace ismmon {

// ---- NEON complex vector multiply: out[i] = a[i]*b[i]. A53: fused multiply-add/sub (4 ops per complex
// mul vs 6) — decode-equivalent to the scalar (ar*br-ai*bi, ...) reference, selftest-gated (x86 = the
// scalar A/B reference; the FMA's single-rounding differs from scalar mul+add by <1 ULP). ----
static inline void cvmul(const cf* a, const cf* b, cf* out, int n) {
	int i = 0;
#ifdef __ARM_NEON
	for(; i + 4 <= n; i += 4) {
		float32x4x2_t av = vld2q_f32((const float*)(a + i)), bv = vld2q_f32((const float*)(b + i));
		float32x4_t re = vfmsq_f32(vmulq_f32(av.val[0], bv.val[0]), av.val[1], bv.val[1]);
		float32x4_t im = vfmaq_f32(vmulq_f32(av.val[0], bv.val[1]), av.val[1], bv.val[0]);
		float32x4x2_t o = { re, im };
		vst2q_f32((float*)(out + i), o);
	}
#endif
	for(; i < n; i++) { out[i] = a[i] * b[i]; }
}

// ---- derotate x[i] *= exp(-j*rad*i) over the burst (coarse/fine CFO removal). The per-sample cos/sin
// is the single biggest OFDM cost on a long frame; NEON runs a 4-phase NCO (advance by step^4) with an
// exact renorm every 512 samples to bound recurrence drift. x86 keeps the exact per-sample trig (the
// numeric reference); aarch64 correctness is gated by the ofdm selftest. ----
static void derotate(cf* x, size_t n, double rad) {
	size_t i = 0;
#ifdef __ARM_NEON
	// A53: fused multiply-add/sub (fewer ops than mul+add/sub), 2x unrolled with TWO independent phase
	// chains (a: samples i..i+3, b: i+4..i+7) so the NCO's loop-carried multiply latency is hidden by
	// the other chain's rotate on the in-order dual-issue pipe. Both advance by step^8. Exact renorm
	// every 512 samples bounds recurrence drift; prefetch the read/write stream.
	float pr[8], pi[8];
	for(int k = 0; k < 8; k++) { pr[k] = (float)cos(-rad * k); pi[k] = (float)sin(-rad * k); }
	float32x4_t phr_a = vld1q_f32(pr), phi_a = vld1q_f32(pi);
	float32x4_t phr_b = vld1q_f32(pr + 4), phi_b = vld1q_f32(pi + 4);
	float32x4_t sr = vdupq_n_f32((float)cos(-rad * 8)), si = vdupq_n_f32((float)sin(-rad * 8));   // step^8
	size_t blk = 0;
	for(; i + 8 <= n; i += 8) {
		__builtin_prefetch(x + i + 32, 1, 1);
		float32x4x2_t xa = vld2q_f32((const float*)(x + i));
		float32x4x2_t oa = { vfmsq_f32(vmulq_f32(xa.val[0], phr_a), xa.val[1], phi_a),
		                     vfmaq_f32(vmulq_f32(xa.val[0], phi_a), xa.val[1], phr_a) };
		vst2q_f32((float*)(x + i), oa);
		float32x4x2_t xb = vld2q_f32((const float*)(x + i + 4));
		float32x4x2_t ob = { vfmsq_f32(vmulq_f32(xb.val[0], phr_b), xb.val[1], phi_b),
		                     vfmaq_f32(vmulq_f32(xb.val[0], phi_b), xb.val[1], phr_b) };
		vst2q_f32((float*)(x + i + 4), ob);
		float32x4_t nra = vfmsq_f32(vmulq_f32(phr_a, sr), phi_a, si), nia = vfmaq_f32(vmulq_f32(phr_a, si), phi_a, sr);
		float32x4_t nrb = vfmsq_f32(vmulq_f32(phr_b, sr), phi_b, si), nib = vfmaq_f32(vmulq_f32(phr_b, si), phi_b, sr);
		phr_a = nra; phi_a = nia; phr_b = nrb; phi_b = nib;
		if((++blk & 63) == 0) {   // exact renorm every 512 samples (64 iters x 8)
			for(int k = 0; k < 8; k++) { pr[k] = (float)cos(-rad * (double)(i + 8 + k)); pi[k] = (float)sin(-rad * (double)(i + 8 + k)); }
			phr_a = vld1q_f32(pr); phi_a = vld1q_f32(pi);
			phr_b = vld1q_f32(pr + 4); phi_b = vld1q_f32(pi + 4);
		}
	}
#endif
	for(; i < n; i++) { double a = -rad * (double)i; x[i] *= cf((float)cos(a), (float)sin(a)); }
}

// ---- 64-tap complex correlation + energy for the L-LTF search: (cre,cim)=sum x*lconj, en=sum|x|^2.
// NEON 4-lane accumulate + horizontal reduce; the scalar fallback matches the original per-sample sum
// exactly (x86 = the numeric reference). ----
static inline void corr_e64(const cf* x, const cf* lconj, float& cre, float& cim, float& en) {
#ifdef __ARM_NEON
	float32x4_t cr = vdupq_n_f32(0), ci = vdupq_n_f32(0), e = vdupq_n_f32(0);
	for(int k = 0; k < 64; k += 4) {
		float32x4x2_t xv = vld2q_f32((const float*)(x + k));
		float32x4x2_t lv = vld2q_f32((const float*)(lconj + k));
		cr = vfmsq_f32(vfmaq_f32(cr, xv.val[0], lv.val[0]), xv.val[1], lv.val[1]);   // cr += xr*lr - xi*li
		ci = vfmaq_f32(vfmaq_f32(ci, xv.val[0], lv.val[1]), xv.val[1], lv.val[0]);   // ci += xr*li + xi*lr
		e = vfmaq_f32(vfmaq_f32(e, xv.val[0], xv.val[0]), xv.val[1], xv.val[1]);     // e  += xr^2 + xi^2
	}
	cre = vaddvq_f32(cr); cim = vaddvq_f32(ci); en = vaddvq_f32(e);
#else
	float cr = 0, ci = 0, e = 0;
	for(int k = 0; k < 64; k++) {
		cr += x[k].real() * lconj[k].real() - x[k].imag() * lconj[k].imag();
		ci += x[k].real() * lconj[k].imag() + x[k].imag() * lconj[k].real();
		e += x[k].real() * x[k].real() + x[k].imag() * x[k].imag();
	}
	cre = cr; cim = ci; en = e;
#endif
}

// ---- small radix-2 FFT (N power of two). NEON butterflies (groups of 4); twiddles precomputed by the
// SAME recurrence as scalar, twiddle complex-mul via FMA (fewer A53 ops) -> decode-equivalent to the
// scalar reference (x86 = scalar A/B reference), selftest-gated. ----
static void fft(std::vector<cf>& a, bool inv) {
	int n = (int)a.size();
	for(int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for(; j & bit; bit >>= 1) { j ^= bit; }
		j ^= bit;
		if(i < j) { std::swap(a[i], a[j]); }
	}
	for(int len = 2; len <= n; len <<= 1) {
		double ang = (inv ? 2.0 : -2.0) * M_PI / len;
		cf wl((float)cos(ang), (float)sin(ang));
		int half = len / 2;
#ifdef __ARM_NEON
		if(half >= 4) {
			static thread_local std::vector<cf> tw;
			tw.resize(half);
			cf w(1, 0);
			for(int k = 0; k < half; k++) { tw[k] = w; w *= wl; }
			for(int i = 0; i < n; i += len) {
				cf* A = &a[i]; cf* B = &a[i + half];
				int k = 0;
				for(; k + 4 <= half; k += 4) {
					float32x4x2_t u = vld2q_f32((const float*)(A + k));
					float32x4x2_t vr = vld2q_f32((const float*)(B + k));
					float32x4x2_t tv = vld2q_f32((const float*)(tw.data() + k));
					float32x4_t vre = vfmsq_f32(vmulq_f32(vr.val[0], tv.val[0]), vr.val[1], tv.val[1]);
					float32x4_t vim = vfmaq_f32(vmulq_f32(vr.val[0], tv.val[1]), vr.val[1], tv.val[0]);
					float32x4x2_t o1 = { vaddq_f32(u.val[0], vre), vaddq_f32(u.val[1], vim) };
					float32x4x2_t o2 = { vsubq_f32(u.val[0], vre), vsubq_f32(u.val[1], vim) };
					vst2q_f32((float*)(A + k), o1);
					vst2q_f32((float*)(B + k), o2);
				}
				for(; k < half; k++) { cf u = A[k], v = B[k] * tw[k]; A[k] = u + v; B[k] = u - v; }
			}
			continue;
		}
#endif
		for(int i = 0; i < n; i += len) {
			cf w(1, 0);
			for(int k = 0; k < half; k++) {
				cf u = a[i + k], v = a[i + k + half] * w;
				a[i + k] = u + v;
				a[i + k + half] = u - v;
				w *= wl;
			}
		}
	}
	if(inv) { for(auto& x : a) { x /= (float)n; } }
}

// ---- subcarrier maps (built once) ----
struct WifiMaps {
	int data_bin[WIFI_NDATA];   // FFT bin for each of the 48 data subcarriers (in tx order)
	int pilot_bin[4];           // FFT bins for pilots at k=-21,-7,7,21
	float pilot_val[4] = { 1, 1, 1, -1 };
	cf ltf[64];                 // L-LTF freq-domain (subcarriers set, others 0)
	cf stf[64];                 // L-STF freq-domain
	float pilot_pol[127];       // pilot polarity sequence
	WifiMaps() {
		auto binof = [](int k) { return (k + 64) % 64; };
		int d = 0;
		for(int k = -26; k <= 26; k++) {
			if(k == 0 || k == -21 || k == -7 || k == 7 || k == 21) { continue; }
			data_bin[d++] = binof(k);
		}
		int pk[4] = { -21, -7, 7, 21 };
		for(int i = 0; i < 4; i++) { pilot_bin[i] = binof(pk[i]); }
		// L-LTF frequency sequence, subcarriers -26..26
		static const int Lseq[53] = {
			1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,  // -26..-1
			0,                                                             // DC
			1,-1,-1,1,1,-1,1,-1,1,-1,-1,-1,-1,-1,1,1,-1,-1,1,-1,1,-1,1,1,1,1 }; // 1..26
		for(int i = 0; i < 64; i++) { ltf[i] = cf(0, 0); stf[i] = cf(0, 0); }
		for(int k = -26; k <= 26; k++) { ltf[binof(k)] = cf((float)Lseq[k + 26], 0); }
		// L-STF: subcarriers at multiples of 4 in [-24,24], value sqrt(13/6)*(+-1+-j)
		static const int sidx[12] = { -24,-20,-16,-12,-8,-4,4,8,12,16,20,24 };
		static const int sre[12]  = {  1,  1,  1,  1, 1, 1,-1,-1, 1, 1, 1, 1 };
		static const int sim[12]  = {  1,  1,  1,  1, 1, 1,-1,-1, 1, 1, 1, 1 };
		// standard STF (values +-(1+j)); signs per the spec
		static const int SR[12] = { 1,-1,1,-1,-1,1, -1,-1,1,1,1,1 };
		float g = (float)std::sqrt(13.0 / 6.0);
		for(int i = 0; i < 12; i++) { (void)sre; (void)sim; stf[binof(sidx[i])] = cf(SR[i] * g, SR[i] * g); }
		// pilot polarity: scrambler seed all-ones, 0->+1, 1->-1
		uint8_t s = 0x7f;
		for(int i = 0; i < 127; i++) {
			uint8_t fb = (uint8_t)(((s >> 6) ^ (s >> 3)) & 1);
			pilot_pol[i] = fb ? -1.0f : 1.0f;
			s = (uint8_t)(((s << 1) | fb) & 0x7f);
		}
	}
};
static const WifiMaps& maps() { static WifiMaps m; return m; }

// ---- scrambler (x^7+x^4+1) ----
static void scramble(std::vector<uint8_t>& bits, uint8_t seed) {
	uint8_t s = seed & 0x7f;
	for(auto& b : bits) {
		uint8_t fb = (uint8_t)(((s >> 6) ^ (s >> 3)) & 1);
		b ^= fb;
		s = (uint8_t)(((s << 1) | fb) & 0x7f);
	}
}

// ---- convolutional code K=7, g0=0133, g1=0171 ----
static inline int parity(uint32_t x) { return __builtin_parity(x); }
static void conv_encode(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
	uint32_t sr = 0;
	out.clear();
	out.reserve(in.size() * 2);
	for(uint8_t u : in) {
		sr = ((sr << 1) | (u & 1)) & 0x7f;
		out.push_back((uint8_t)parity(sr & 0x5b));   // g0 = 133 oct
		out.push_back((uint8_t)parity(sr & 0x79));   // g1 = 171 oct
	}
}

// soft-decision Viterbi (K=7, 64 states), tail-terminated (traceback from state 0).
// Trellis butterfly: predecessors {j, j+32} feed next-states {2j, 2j+1} (top state bit s5 is dropped
// by the &0x3f next-state mask). s5 flips bit 6 of the shift register, which is set in BOTH generators
// (g0=0133, g1=0171), so it flips both parities -> the branch metric of predecessor j+32 is exactly
// -(that of j). On A53 this becomes a branch-free vector add-compare-select with a backward-gather
// traceback (no scatter, no per-lane NEON->GPR extracts). Decode-EQUIVALENT to the scalar forward-
// scatter reference (gated by wifi_ofdm_selftest): every state has exactly 2 predecessors so npm is
// always max(cand_j, cand_{j+32}); unreached states stay at ~-1e30 and never join the survivor path;
// ties resolve to j (the lower predecessor), matching the scalar's first-writer-wins.
static std::vector<uint8_t> viterbi(const std::vector<float>& soft /* pairs a,b */) {
	int nbits = (int)soft.size() / 2;
	const int NS = 64;
	std::vector<std::array<uint8_t, NS>> tb(nbits);
	std::vector<float> pm(NS, -1e30f), npm(NS);
	pm[0] = 0;
	// precompute outputs for (state,u) + the +-1 sign tables / index table the NEON path needs
	// ([[maybe_unused]]: nxt is scalar-only, the sign/index tables are NEON-only)
	[[maybe_unused]] static int outA[NS][2], outB[NS][2], nxt[NS][2];
	[[maybe_unused]] static float SA0[NS], SB0[NS], SA1[NS], SB1[NS];
	[[maybe_unused]] static uint32_t IDX[NS];
	static bool init = false;
	if(!init) {
		for(int s = 0; s < NS; s++) {
			for(int u = 0; u < 2; u++) {
				uint32_t sr = (((uint32_t)s << 1) | u) & 0x7f;
				outA[s][u] = parity(sr & 0x5b);
				outB[s][u] = parity(sr & 0x79);
				nxt[s][u] = (int)(sr & 0x3f);
			}
			SA0[s] = outA[s][0] ? 1.f : -1.f; SB0[s] = outB[s][0] ? 1.f : -1.f;
			SA1[s] = outA[s][1] ? 1.f : -1.f; SB1[s] = outB[s][1] ? 1.f : -1.f;
			IDX[s] = (uint32_t)s;
		}
		init = true;
	}
#ifdef __ARM_NEON
	alignas(16) float bm0[NS], bm1[NS];
	for(int t = 0; t < nbits; t++) {
		float32x4_t rav = vdupq_n_f32(soft[2 * t]), rbv = vdupq_n_f32(soft[2 * t + 1]);
		for(int j = 0; j < NS; j += 4) {   // bm = ra*sgn(outA) + rb*sgn(outB), for u=0 and u=1
			vst1q_f32(bm0 + j, vfmaq_f32(vmulq_f32(rbv, vld1q_f32(SB0 + j)), rav, vld1q_f32(SA0 + j)));
			vst1q_f32(bm1 + j, vfmaq_f32(vmulq_f32(rbv, vld1q_f32(SB1 + j)), rav, vld1q_f32(SA1 + j)));
		}
		const uint32x4_t k32 = vdupq_n_u32(32);
		for(int g = 0; g < 8; g++) {   // 4 butterflies/group -> npm[8g..8g+7]
			int j = 4 * g;
			float32x4_t pmj = vld1q_f32(pm.data() + j), pmj32 = vld1q_f32(pm.data() + 32 + j);
			float32x4_t b0 = vld1q_f32(bm0 + j), b1 = vld1q_f32(bm1 + j);
			float32x4_t ea = vaddq_f32(pmj, b0), eb = vsubq_f32(pmj32, b0);   // ns=2j  from {j, j+32}
			float32x4_t oa = vaddq_f32(pmj, b1), ob = vsubq_f32(pmj32, b1);   // ns=2j+1
			float32x4x2_t st = { vmaxq_f32(ea, eb), vmaxq_f32(oa, ob) };
			vst2q_f32(npm.data() + 8 * g, st);                               // interleave even/odd
			uint32x4_t base = vld1q_u32(IDX + j);
			uint32x4_t pe = vaddq_u32(base, vbicq_u32(k32, vcgeq_f32(ea, eb)));   // ea>=eb -> +0 (j) else +32
			uint32x4_t po = vaddq_u32(base, vbicq_u32(k32, vcgeq_f32(oa, ob)));
			uint8x8_t pe8 = vmovn_u16(vcombine_u16(vmovn_u32(pe), vmovn_u32(pe)));
			uint8x8_t po8 = vmovn_u16(vcombine_u16(vmovn_u32(po), vmovn_u32(po)));
			vst1_u8(&tb[t][8 * g], vzip1_u8(pe8, po8));
		}
		pm.swap(npm);
	}
#else
	for(int t = 0; t < nbits; t++) {
		float ra = soft[2 * t], rb = soft[2 * t + 1];
		std::fill(npm.begin(), npm.end(), -1e30f);
		for(int s = 0; s < NS; s++) {
			if(pm[s] < -1e29f) { continue; }
			for(int u = 0; u < 2; u++) {
				float bm = ra * (outA[s][u] ? 1.f : -1.f) + rb * (outB[s][u] ? 1.f : -1.f);
				int ns = nxt[s][u];
				float m = pm[s] + bm;
				if(m > npm[ns]) { npm[ns] = m; tb[t][ns] = (uint8_t)s; }
			}
		}
		pm.swap(npm);
	}
#endif
	// traceback from state 0 (tail bits force it)
	std::vector<uint8_t> bits(nbits);
	int s = 0;
	// if state 0 isn't the best (unterminated capture), pick the best
	if(pm[0] < -1e29f) { s = (int)(std::max_element(pm.begin(), pm.end()) - pm.begin()); }
	for(int t = nbits - 1; t >= 0; t--) {
		int ps = tb[t][s];
		// the input bit u that took ps->s is the LSB of s's shift (s = ((ps<<1)|u)&0x3f)
		bits[t] = (uint8_t)(s & 1);
		s = ps;
	}
	return bits;
}

// ---- interleaver (BPSK, N_CBPS=48; second perm is identity for BPSK) ----
static const int NCBPS = 48;
static int interleave_perm(int k) { return 3 * (k % 16) + k / 16; }   // coded bit k -> subcarrier pos
static void deinterleave(const float* in48, float* out48) {
	for(int k = 0; k < NCBPS; k++) { out48[k] = in48[interleave_perm(k)]; }
}
static void interleave(const std::vector<uint8_t>& in48, std::vector<uint8_t>& out48) {
	out48.assign(NCBPS, 0);
	for(int k = 0; k < NCBPS; k++) { out48[interleave_perm(k)] = in48[k]; }
}

// ---- general legacy rate parameters + QAM demap + interleaver + puncturing (all 8 rates) ----
struct RParams { int nbpsc, ncbps, ndbps, num, den; };   // bits/subcarrier, coded/data bits per sym, code rate num/den
static RParams rate_params(int rate) {
	switch(rate) {
		case 6:  return { 1, 48, 24, 1, 2 };
		case 9:  return { 1, 48, 36, 3, 4 };
		case 12: return { 2, 96, 48, 1, 2 };
		case 18: return { 2, 96, 72, 3, 4 };
		case 24: return { 4, 192, 96, 1, 2 };
		case 36: return { 4, 192, 144, 3, 4 };
		case 48: return { 6, 288, 192, 2, 3 };
		case 54: return { 6, 288, 216, 3, 4 };
	}
	return { 1, 48, 24, 1, 2 };
}
// soft demap in "level" domain (>0 => bit 1). 802.11 Gray mapping, I bits then Q bits.
static void demap_soft(cf z, int nbpsc, float* b) {
	float I = z.real(), Q = z.imag();
	if(nbpsc == 1) { b[0] = I; return; }
	if(nbpsc == 2) { b[0] = I; b[1] = Q; return; }
	if(nbpsc == 4) {
		float xi = I * 3.16228f, xq = Q * 3.16228f;   // *sqrt(10)
		b[0] = xi; b[1] = 2.0f - std::fabs(xi);
		b[2] = xq; b[3] = 2.0f - std::fabs(xq);
		return;
	}
	// 64QAM
	float xi = I * 6.48074f, xq = Q * 6.48074f;   // *sqrt(42)
	b[0] = xi; b[1] = 4.0f - std::fabs(xi); b[2] = 2.0f - std::fabs(4.0f - std::fabs(xi));
	b[3] = xq; b[4] = 4.0f - std::fabs(xq); b[5] = 2.0f - std::fabs(4.0f - std::fabs(xq));
}
static cf qam_map(const uint8_t* b, int nbpsc) {
	if(nbpsc == 1) { return cf(b[0] ? 1.f : -1.f, 0); }
	if(nbpsc == 2) { return cf((b[0] ? 1 : -1) * 0.70711f, (b[1] ? 1 : -1) * 0.70711f); }
	if(nbpsc == 4) {
		float i = (b[0] ? 1 : -1) * (b[1] ? 1 : 3), q = (b[2] ? 1 : -1) * (b[3] ? 1 : 3);
		return cf(i / 3.16228f, q / 3.16228f);
	}
	auto lvl = [](const uint8_t* c) { float m = c[1] ? (c[2] ? 3.f : 1.f) : (c[2] ? 5.f : 7.f); return (c[0] ? 1.f : -1.f) * m; };
	return cf(lvl(b) / 6.48074f, lvl(b + 3) / 6.48074f);
}
// general interleaver (IEEE 802.11-2016 17.3.5.7). coded-bit index k -> interleaved position j.
static int intlv_map(int k, int ncbps, int nbpsc) {
	int i = (ncbps / 16) * (k % 16) + k / 16;                 // first permutation
	int s = std::max(nbpsc / 2, 1);
	return s * (i / s) + (i + ncbps - (16 * i) / ncbps) % s;  // second permutation
}
// puncture a rate-1/2 coded stream (A0 B0 A1 B1 ...) down to num/den; RX inserts 0-erasures to invert.
static bool punct_keep(int idx_in_period, int num, int den) {
	if(num == 1 && den == 2) { return true; }
	if(num == 3 && den == 4) { int m = idx_in_period % 6; return m == 0 || m == 1 || m == 2 || m == 5; }   // keep A0 B0 A1 B2
	if(num == 2 && den == 3) { int m = idx_in_period % 4; return m != 3; }                                  // drop B1
	return true;
}
static int punct_period(int num, int den) { return (num == 3 && den == 4) ? 6 : (num == 2 && den == 3) ? 4 : 2; }

static const int RATE_CODE[8] = { 0xB, 0xF, 0x5, 0x7, 0x9, 0xB, 0xD, 0xF };   // unused table guard
static int rate_from_code(int c) {
	switch(c) { case 0xD: return 6; case 0xF: return 9; case 0x5: return 12; case 0x7: return 18;
		case 0x9: return 24; case 0xB: return 36; case 0x1: return 48; case 0x3: return 54; }
	return 0;
}
static int code_from_rate(int r) {
	switch(r) { case 6: return 0xD; case 9: return 0xF; case 12: return 0x5; case 18: return 0x7;
		case 24: return 0x9; case 36: return 0xB; case 48: return 0x1; case 54: return 0x3; }
	return 0xD;
}
[[maybe_unused]] static int ndbps_of(int rate) { return rate * 4; }   // legacy helper (superseded by rate_params)

// ---- one OFDM symbol: 48 data soft bits (BPSK), after equalize + pilot phase correction ----
static void demod_symbol(const cf* sym /*64 freq bins, equalized*/, int sym_idx, float* out48) {
	const WifiMaps& m = maps();
	// pilot phase estimate
	cf acc(0, 0);
	for(int i = 0; i < 4; i++) {
		float ref = m.pilot_val[i] * m.pilot_pol[sym_idx % 127];
		acc += sym[m.pilot_bin[i]] * ref;   // sym*conj(ref); ref real -> just scale
	}
	float ph = std::atan2((float)acc.imag(), (float)acc.real());
	cf rot((float)cos(-ph), (float)sin(-ph));
	for(int d = 0; d < WIFI_NDATA; d++) {
		cf z = sym[m.data_bin[d]] * rot;
		out48[d] = z.real();   // BPSK soft (0->-1,1->+1)
	}
}

// return the 48 equalized + pilot-phase-corrected complex data subcarriers (for QAM demap)
static void demod_symbol_cplx(const cf* sym, int sym_idx, cf* out48) {
	const WifiMaps& m = maps();
	cf acc(0, 0);
	for(int i = 0; i < 4; i++) { float ref = m.pilot_val[i] * m.pilot_pol[sym_idx % 127]; acc += sym[m.pilot_bin[i]] * ref; }
	float ph = std::atan2((float)acc.imag(), (float)acc.real());
	cf rot((float)cos(-ph), (float)sin(-ph));
	for(int d = 0; d < WIFI_NDATA; d++) { out48[d] = sym[m.data_bin[d]] * rot; }
}

// ---- CRC-32 (802.11 FCS) ----
uint32_t wifi_crc32(const uint8_t* d, size_t n) {
	uint32_t c = 0xFFFFFFFF;
	for(size_t i = 0; i < n; i++) {
		c ^= d[i];
		for(int k = 0; k < 8; k++) { c = (c >> 1) ^ (0xEDB88320 & (-(int)(c & 1))); }
	}
	return ~c;
}

const char* wifi_type_name(int ts) {
	switch(ts) { case 0x08: return "Beacon"; case 0x04: return "ProbeReq"; case 0x05: return "ProbeResp";
		case 0x00: return "AssocReq"; case 0x01: return "AssocResp"; case 0x0B: return "Auth";
		case 0x0C: return "Deauth"; case 0x0A: return "Disassoc"; }
	return "Mgmt?";
}
std::string wifi_mac_string(const uint8_t a[6]) {
	char s[18];
	snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
	return s;
}

// ---- parse a management MAC frame -> fill out ----
// Addresses + SSID are extracted FIRST (they sit at fixed early offsets); mac_ok is set only when the
// full-frame FCS-32 verifies. This matters because a beacon body is ~300 bytes and one bit error
// anywhere fails the FCS -> gating the SSID on the FCS would drop every AP but the strongest few. The
// SSID is validated by structure (well-formed IE, <=32 printable bytes) instead, so a header-CRC-valid
// frame with a clean front still yields its SSID/BSSID (what real sniffers do). Confidence: caller
// gates on a valid PHY header (PLCP CRC-16 / OFDM SIGNAL) + non-empty printable SSID.
void wifi_parse_mac(const std::vector<uint8_t>& f, wifi_packet& out) {
	if(f.size() < 24) { return; }
	int type = (f[0] >> 2) & 3, subtype = (f[0] >> 4) & 0xF;
	if(type == 2) {
		// Data frame: the MAC header (addresses) is NOT encrypted. addr2 = transmitter (TA) = a real
		// station or AP that just spoke - a device. Direction bits pick which side is the client.
		if(f.size() < 16) { return; }
		int tods = f[1] & 1, fromds = (f[1] >> 1) & 1;
		memcpy(out.sa, &f[10], 6);   // TA (transmitter)
		memcpy(out.da, &f[4], 6);    // RA (receiver)
		memcpy(out.bssid, &f[10], 6);   // key the device table on the transmitter
		out.has_addr = true;
		out.frame_type = 0x20;
		out.type_name = (fromds && !tods) ? "Data(AP)" : (tods && !fromds) ? "Data(STA)" : "Data";
		return;
	}
	if(type != 0) { return; }   // management or data only
	out.frame_type = subtype;
	out.type_name = wifi_type_name(subtype);
	memcpy(out.da, &f[4], 6);
	memcpy(out.sa, &f[10], 6);
	memcpy(out.bssid, &f[16], 6);
	out.has_addr = true;
	size_t end = f.size() >= 4 ? f.size() - 4 : f.size();   // exclude a trailing FCS if present
	// tagged params (SSID = tag 0). Beacon/ProbeResp carry 12 bytes of fixed params first.
	size_t p = 24;
	if(subtype == 0x08 || subtype == 0x05) { p += 12; }
	while(p + 2 <= end) {
		int tag = f[p], len = f[p + 1];
		if(p + 2 + (size_t)len > end) { break; }
		if(tag == 0) {
			bool printable = true;
			for(int i = 0; i < len; i++) { uint8_t c = f[p + 2 + i]; if(c < 0x20 || c > 0x7e) { printable = false; break; } }
			if(len <= 32 && printable) { out.ssid.assign((const char*)&f[p + 2], (const char*)&f[p + 2] + len); }
			break;
		}
		p += 2 + len;
	}
	// full-frame FCS -> mac_ok (SSID/BSSID above are usable even without it)
	if(f.size() >= 28) {
		uint32_t rx = (uint32_t)f[f.size() - 4] | ((uint32_t)f[f.size() - 3] << 8) | ((uint32_t)f[f.size() - 2] << 16) | ((uint32_t)f[f.size() - 1] << 24);
		if(wifi_crc32(f.data(), f.size() - 4) == rx) { out.mac_ok = true; }
	}
}

// 802.11n HT (mixed-format, 20 MHz, 1 spatial stream) decode - defined below wifi_decode_burst.
static void decode_ht(const std::vector<cf>& x, int ltf_start, int sig_start, const std::vector<cf>& H, wifi_packet& out, int n);

// ================= receiver =================
bool wifi_decode_burst(const cf* iq, size_t n, int channel, wifi_packet& out, float det_threshold) {
	out.channel = channel;
	const WifiMaps& m = maps();
	if(n < 400) { return false; }
	// rssi
	double ms = 0; for(size_t i = 0; i < n; i++) { ms += std::abs(iq[i]); }
	out.rssi_db = 20.0f * std::log10((float)(ms / n) + 1e-9f);

	// --- L-STF autocorrelation (lag 16, Schmidl-Cox) to find the packet + coarse CFO ---
	const int L = 16;
	if(n < (size_t)(3 * L)) { return false; }
	std::vector<float> M(n, 0), C(n, 0);
	double Pr = 0, Pi = 0, Cc = 0;   // double accumulators - the float sliding sum drifts to M>1 over long bursts
	for(int k = 0; k < L; k++) { cf t = iq[k] * std::conj(iq[k + L]); Pr += t.real(); Pi += t.imag(); Cc += std::norm(iq[k + L]); }
	double sumC = 0; size_t cntC = 0;
	for(size_t p = 0; p + 2 * L <= n; p++) {
		double mag = std::sqrt(Pr * Pr + Pi * Pi);
		M[p] = Cc > 1e-9 ? (float)std::min(1.0, mag / Cc) : 0;   // clamp: |P| <= Cc analytically
		C[p] = (float)Cc;
		sumC += Cc; cntC++;
		cf a = iq[p + L] * std::conj(iq[p + 2 * L]), b = iq[p] * std::conj(iq[p + L]);
		Pr += (double)a.real() - b.real(); Pi += (double)a.imag() - b.imag();
		Cc += (double)std::norm(iq[p + 2 * L]) - std::norm(iq[p + L]);
	}
	// gate on AVERAGE energy, not the peak: the L-STF has lower PAPR than QAM DATA, so a peak-relative
	// gate (0.25*maxC) wrongly excludes the real preamble when the data is 16/64-QAM. STF power ~= data
	// average power (matched by design), so 0.5*meanC admits the STF while rejecting noise regions.
	float gateC = (float)(0.5 * sumC / (cntC ? cntC : 1));
	double best = 0; int best_pos = -1;
	for(size_t p = 0; p + 2 * L <= n; p++) { if(C[p] > gateC && M[p] > best) { best = M[p]; best_pos = (int)p; } }
	if(best < det_threshold || best_pos < 0) { return false; }
	// coarse CFO from the autocorr phase over the STF plateau
	cf cf_acc(0, 0);
	for(int i = std::max(0, best_pos); i < std::min((int)n - L, best_pos + 100); i++) { cf_acc += iq[i] * std::conj(iq[i + L]); }
	double cfo_rad = std::atan2((double)cf_acc.imag(), (double)cf_acc.real()) / L;
	out.cfo_hz = cfo_rad / (2 * M_PI) * WIFI_FS;

	// derotate the whole burst by the coarse CFO
	std::vector<cf> x(iq, iq + n);
	derotate(x.data(), n, cfo_rad);

	// --- find L-LTF: after the STF (~10 short syms = 160 samples), the 2 LTS (with 32-sample GI) ---
	// search a window for the LTF by correlating with the known time-domain LTS
	std::vector<cf> ltf_freq(64);
	for(int i = 0; i < 64; i++) { ltf_freq[i] = m.ltf[i]; }
	std::vector<cf> lts_t = ltf_freq; fft(lts_t, true);   // time-domain LTS (64)
	int ltf_start = -1; double lbest = 0; int pmax = -1;
	int s0 = std::max(0, best_pos), s1 = std::min((int)n - 64, best_pos + 340);
	std::vector<double> lc(std::max(1, s1 - s0), 0);
	std::vector<cf> lconj(64);
	for(int k = 0; k < 64; k++) { lconj[k] = std::conj(lts_t[k]); }
	for(int p = s0; p < s1; p++) {
		float cre, cim, en;
		corr_e64(&x[p], lconj.data(), cre, cim, en);
		cf c(cre, cim);
		double mtr = (en > 1e-6f) ? std::abs(c) / std::sqrt(en) : 0;
		lc[p - s0] = mtr;
		if(mtr > lbest) { lbest = mtr; pmax = p; }
	}
	if(pmax < 0) { return false; }
	out.det_q = (float)lbest;   // L-LTF match quality: ~0.9 for a real packet, ~0.4 for noise
	// the two LTS are identical; if a peak 64 samples earlier is also strong, THAT is LTS1
	ltf_start = (pmax - 64 >= s0 && lc[pmax - 64 - s0] > 0.7 * lbest) ? pmax - 64 : pmax;
	if(ltf_start + 128 + WIFI_SYM > (int)n) { return false; }

	// fine CFO from the two identical LTS (64 samples apart) - refines the coarse STF estimate
	cf fine(0, 0);
	for(int k = 0; k < 64; k++) { fine += x[ltf_start + 64 + k] * std::conj(x[ltf_start + k]); }
	double fine_rad = std::atan2((double)fine.imag(), (double)fine.real()) / 64.0;
	derotate(x.data(), n, fine_rad);
	out.cfo_hz += fine_rad / (2 * M_PI) * WIFI_FS;

	// channel estimate from the 2 LTS
	std::vector<cf> H(64, cf(0, 0));
	for(int r = 0; r < 2; r++) {
		std::vector<cf> sym(x.begin() + ltf_start + r * 64, x.begin() + ltf_start + r * 64 + 64);
		fft(sym, false);
		for(int k = 0; k < 64; k++) { H[k] += sym[k] * m.ltf[k]; }   // divide out the known +-1 LTS -> channel
	}
	for(int k = 0; k < 64; k++) { H[k] *= 0.5f; }
	// Smooth the channel estimate across adjacent subcarriers - the channel is smooth over ~1 MHz, so a
	// 3-tap frequency-order average denoises H (~1 dB on the DATA decode at low SNR) without distorting
	// it. Edge-aware; DC (k=0) skipped. Off by WIFI_NOHSMOOTH for A/B.
	if(!getenv("WIFI_NOHSMOOTH")) {
		auto bof = [](int k) { return (k + 64) % 64; };
		std::vector<cf> Hs = H;
		for(int k = -26; k <= 26; k++) {
			if(k == 0) { continue; }
			cf a = H[bof(k)]; int c = 1;
			if(k - 1 >= -26 && k - 1 != 0) { a += H[bof(k - 1)]; c++; }
			if(k + 1 <= 26 && k + 1 != 0) { a += H[bof(k + 1)]; c++; }
			Hs[bof(k)] = a / (float)c;
		}
		H.swap(Hs);
	}
	auto eqval = [&](int bin) { return (std::norm(H[bin]) > 1e-9f) ? cf(H[bin].real(), -H[bin].imag()) / std::norm(H[bin]) : cf(0, 0); };
	cf eqc[64];   // precomputed equalizer coeffs (conj(H)/|H|^2); per-symbol equalize becomes one NEON cvmul
	for(int k = 0; k < 64; k++) { eqc[k] = eqval(k); }

	// SIGNAL symbol starts right after the 2 LTS
	int sig_start = ltf_start + 128;
	auto demod_at = [&](int start, int sym_idx, float* out48) {
		std::vector<cf> sym(x.begin() + start + WIFI_CP, x.begin() + start + WIFI_CP + 64);
		fft(sym, false);
		std::vector<cf> eq(64);
		cvmul(sym.data(), eqc, eq.data(), 64);
		demod_symbol(eq.data(), sym_idx, out48);
	};

	// --- SIGNAL: 48 coded soft bits, deinterleave, Viterbi (tail), parse rate/length ---
	float sig48[48], sigde[48];
	demod_at(sig_start, 0, sig48);
	deinterleave(sig48, sigde);
	std::vector<float> sigsoft(sigde, sigde + 48);
	std::vector<uint8_t> sigbits = viterbi(sigsoft);   // 24 bits
	if(sigbits.size() < 24) { return false; }
	int rate_code = (sigbits[0]) | (sigbits[1] << 1) | (sigbits[2] << 2) | (sigbits[3] << 3);
	int length = 0; for(int i = 0; i < 12; i++) { length |= sigbits[5 + i] << i; }
	int par = 0; for(int i = 0; i < 17; i++) { par ^= sigbits[i]; }
	out.sig_ok = (par == sigbits[17]) && rate_from_code(rate_code) > 0 && length > 0 && length < 4096;
	out.rate_mbps = rate_from_code(rate_code);
	out.length = length;
	if(getenv("WIFI_DBG")) {
		fprintf(stderr, "[dbg] STFbest=%.2f pos=%d | LTFstart=%d lbest=%.2f | rate_code=%x rate=%d len=%d par=%d/%d sig_ok=%d\n",
		        best, best_pos, ltf_start, lbest, rate_code, out.rate_mbps, length, par, sigbits[17], out.sig_ok);
		fprintf(stderr, "[dbg] sigbits:"); for(int i = 0; i < 24; i++) { fprintf(stderr, "%d", sigbits[i]); } fprintf(stderr, "\n");
	}
	if(!out.sig_ok) { return true; }   // detected + attempted; header invalid

	// --- HT (11n mixed-format) detection. An HT frame always signals L-SIG rate 6, and the symbol after
	//     L-SIG is HT-SIG1 = QBPSK (BPSK rotated +90 onto Q). So only a rate-6 L-SIG can be HT; decide by
	//     I-vs-Q energy of the equalized data subcarriers (legacy BPSK-6 -> energy on I; HT-SIG -> Q). ---
	if(out.rate_mbps == 6) {
		int hs = sig_start + WIFI_SYM;
		if(hs + WIFI_SYM <= (int)n) {
			std::vector<cf> sym(x.begin() + hs + WIFI_CP, x.begin() + hs + WIFI_CP + 64);
			fft(sym, false);
			double ei = 0, eq = 0;
			for(int d = 0; d < WIFI_NDATA; d++) { cf z = sym[m.data_bin[d]] * eqval(m.data_bin[d]); ei += (double)z.real() * z.real(); eq += (double)z.imag() * z.imag(); }
			out.is_ht = eq > ei;
		}
	}
	if(out.is_ht) { decode_ht(x, ltf_start, sig_start, H, out, (int)n); return true; }

	// --- legacy DATA: all 8 rates (BPSK/QPSK/16QAM/64QAM + puncturing) -> MAC header ---
	(void)&demod_at;
	RParams rp = rate_params(out.rate_mbps);
	int nsym = (16 + 8 * length + 6 + rp.ndbps - 1) / rp.ndbps;
	if(sig_start + WIFI_SYM + nsym * WIFI_SYM > (int)n) { nsym = ((int)n - sig_start - WIFI_SYM) / WIFI_SYM; }
	if(nsym <= 0) { return true; }
	std::vector<float> kept;   // deinterleaved (kept) soft bits, concatenated across all data symbols
	kept.reserve((size_t)nsym * rp.ncbps);
	for(int s = 0; s < nsym; s++) {
		int base = sig_start + WIFI_SYM + s * WIFI_SYM + WIFI_CP;
		std::vector<cf> sym(x.begin() + base, x.begin() + base + 64);
		fft(sym, false);
		std::vector<cf> eq(64);
		cvmul(sym.data(), eqc, eq.data(), 64);
		cf data48[48];
		demod_symbol_cplx(eq.data(), 1 + s, data48);
		if(getenv("WIFI_DIAG") && out.det_q > 0.6f && s < 14) {
			cf pacc(0, 0);
			for(int i = 0; i < 4; i++) { float ref = m.pilot_val[i] * m.pilot_pol[(1 + s) % 127]; pacc += eq[m.pilot_bin[i]] * ref; }
			double dmag = 0, dre = 0;
			for(int d = 0; d < 48; d++) { dmag += std::abs(data48[d]); dre += std::abs(data48[d].real()) - std::abs(data48[d].imag()); }
			fprintf(stderr, "[diag] dq=%.2f r=%d L=%d s%2d: pilotPh=%+.2f |pil|=%.2f dMag=%.2f ImQ=%+.2f\n",
			        out.det_q, out.rate_mbps, out.length, s, std::atan2((double)pacc.imag(), (double)pacc.real()), std::abs(pacc) / 4, dmag / 48, dre / 48);
		}
		std::vector<float> recv(rp.ncbps);
		for(int d = 0; d < WIFI_NDATA; d++) {
			float bb[6];
			demap_soft(data48[d], rp.nbpsc, bb);
			for(int q = 0; q < rp.nbpsc; q++) { recv[d * rp.nbpsc + q] = bb[q]; }
		}
		for(int k = 0; k < rp.ncbps; k++) { kept.push_back(recv[intlv_map(k, rp.ncbps, rp.nbpsc)]); }
	}
	// depuncture: reinsert 0-erasures at the punctured positions -> full rate-1/2 coded soft stream
	std::vector<float> coded;
	coded.reserve(kept.size() * 2);
	int pp = punct_period(rp.num, rp.den);
	for(int i = 0, ki = 0; ki < (int)kept.size(); i++) {
		if(punct_keep(i % pp, rp.num, rp.den)) { coded.push_back(kept[ki++]); }
		else { coded.push_back(0.0f); }
	}
	std::vector<uint8_t> databits = viterbi(coded);
	if((int)databits.size() < 16) { return true; }
	// descramble: SERVICE[0:7]=0 so scrambler_out[0:7]=databits[0:7]; reconstruct the LFSR state after
	// those 7 outputs, then continue the sequence and XOR (SERVICE bits themselves are 0).
	std::vector<uint8_t> ds(databits.begin(), databits.end());
	uint8_t s = 0;
	for(int i = 0; i < 7; i++) { s = (uint8_t)((s << 1) | (databits[i] & 1)); }
	for(int i = 0; i < 7; i++) { ds[i] = 0; }
	for(size_t i = 7; i < ds.size(); i++) {
		uint8_t fb = (uint8_t)(((s >> 6) ^ (s >> 3)) & 1);
		ds[i] ^= fb;
		s = (uint8_t)(((s << 1) | fb) & 0x7f);
	}
	// bytes: skip SERVICE(16 bits = 2 bytes), take length bytes
	int total_bytes = (int)ds.size() / 8;
	std::vector<uint8_t> bytes(total_bytes, 0);
	for(int i = 0; i < total_bytes; i++) { for(int b = 0; b < 8; b++) { bytes[i] |= ds[i * 8 + b] << b; } }
	if((int)bytes.size() < 2 + length) { return true; }
	std::vector<uint8_t> mac(bytes.begin() + 2, bytes.begin() + 2 + length);
	if(getenv("WIFI_MAC") && mac.size() >= 16) {
		fprintf(stderr, "MAC r%d q%.2f fc=%02x%02x a1=%02x:%02x:%02x:%02x:%02x:%02x a2=%02x:%02x:%02x:%02x:%02x:%02x\n",
		        out.rate_mbps, out.det_q, mac[0], mac[1], mac[4], mac[5], mac[6], mac[7], mac[8], mac[9], mac[10], mac[11], mac[12], mac[13], mac[14], mac[15]);
	}
	if(getenv("WIFI_DBG6")) {
		fprintf(stderr, "[dbg6] len=%d nsym=%d mac[0..17]:", length, nsym);
		for(int i = 0; i < 18 && i < (int)mac.size(); i++) { fprintf(stderr, " %02x", mac[i]); }
		if(mac.size() >= 4) {
			uint32_t fc = wifi_crc32(mac.data(), mac.size() - 4);
			uint32_t fr = mac[mac.size() - 4] | (mac[mac.size() - 3] << 8) | (mac[mac.size() - 2] << 16) | ((uint32_t)mac[mac.size() - 1] << 24);
			fprintf(stderr, " | fcs_calc=%08x rx=%08x", fc, fr);
		}
		fprintf(stderr, "\n");
	}
	wifi_parse_mac(mac, out);
	return true;
}

// ================= 802.11n HT (mixed format, 20 MHz, 1 spatial stream) =================
static void map_symbol(const std::vector<uint8_t>& b48, int sym_idx, std::vector<cf>& freq);
static void ofdm_symbol_time(const std::vector<cf>& freq, std::vector<cf>& out);
// HT-SIG CRC-8 (poly x^8+x^2+x+1 = 0x07, init 0xFF, MSB-first, ones-complemented) over 34 bits.
static uint8_t ht_crc8(const uint8_t* bits, int nbits) {
	uint8_t c = 0xff;
	for(int i = 0; i < nbits; i++) {
		uint8_t x = (uint8_t)(((c >> 7) & 1) ^ (bits[i] & 1));
		c = (uint8_t)(c << 1);
		if(x) { c ^= 0x07; }
	}
	return (uint8_t)~c;
}
// MCS 0..7 (single stream, 20 MHz): modulation bits/subcarrier + code rate. NCBPS = 52*nbpsc.
static RParams ht_mcs_params(int mcs) {
	switch(mcs) {
		case 0: return { 1, 52, 26, 1, 2 };    // BPSK 1/2
		case 1: return { 2, 104, 52, 1, 2 };   // QPSK 1/2
		case 2: return { 2, 104, 78, 3, 4 };   // QPSK 3/4
		case 3: return { 4, 208, 104, 1, 2 };  // 16QAM 1/2
		case 4: return { 4, 208, 156, 3, 4 };  // 16QAM 3/4
		case 5: return { 6, 312, 208, 2, 3 };  // 64QAM 2/3
		case 6: return { 6, 312, 234, 3, 4 };  // 64QAM 3/4
		case 7: return { 6, 312, 260, 5, 6 };  // 64QAM 5/6
	}
	return { 1, 52, 26, 1, 2 };
}
// HT 20 MHz maps: 52 data subcarriers (-28..28 excl 0,+-7,+-21) + 4 pilots; HT-LTF freq sequence.
struct HtMaps {
	int data_bin[52];
	int pilot_bin[4];
	float pilot_val[4] = { 1, 1, 1, -1 };
	cf htltf[64];
	HtMaps() {
		auto binof = [](int k) { return (k + 64) % 64; };
		int d = 0;
		for(int k = -28; k <= 28; k++) { if(k == 0 || k == 7 || k == -7 || k == 21 || k == -21) { continue; } data_bin[d++] = binof(k); }
		int pk[4] = { -21, -7, 7, 21 };
		for(int i = 0; i < 4; i++) { pilot_bin[i] = binof(pk[i]); }
		// HT-LTF (20 MHz, 1 SS): the L-LTF sequence (-26..26) extended to +-27,+-28 (IEEE 802.11-2016 Eq 21-46).
		static const int L[53] = { 1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,
		                           1,-1,-1,1,1,-1,1,-1,1,-1,-1,-1,-1,-1,1,1,-1,-1,1,-1,1,-1,1,1,1,1 };
		for(int i = 0; i < 64; i++) { htltf[i] = cf(0, 0); }
		for(int k = -26; k <= 26; k++) { if(k) { htltf[binof(k)] = cf((float)L[k < 0 ? k + 26 : k + 25], 0); } }
		int ext[4] = { -28, -27, 27, 28 }, ev[4] = { 1, 1, -1, -1 };
		for(int i = 0; i < 4; i++) { htltf[binof(ext[i])] = cf((float)ev[i], 0); }
	}
};
static const HtMaps& htmaps() { static HtMaps m; return m; }

// decode HT-SIG (2 QBPSK OFDM symbols after L-SIG) -> MCS / length / CRC-8; then HT-LTF channel est +
// HT-DATA (MCS0 landed; higher MCS wired via ht_mcs_params). x is CFO-corrected; H is the L-LTF channel.
static void decode_ht(const std::vector<cf>& x, int ltf_start, int sig_start, const std::vector<cf>& H, wifi_packet& out, int n) {
	const WifiMaps& m = maps();
	auto eqval = [&](int bin) { return (std::norm(H[bin]) > 1e-9f) ? cf(H[bin].real(), -H[bin].imag()) / std::norm(H[bin]) : cf(0, 0); };
	cf eqc[64];
	for(int k = 0; k < 64; k++) { eqc[k] = eqval(k); }
	// HT-SIG: 2 symbols at sig_start+WIFI_SYM and +2*WIFI_SYM, QBPSK (derotate by -90 deg => *(-j)).
	int hs0 = sig_start + WIFI_SYM;
	if(hs0 + 2 * WIFI_SYM > n) { return; }
	std::vector<float> coded;
	for(int s = 0; s < 2; s++) {
		std::vector<cf> sym(x.begin() + hs0 + s * WIFI_SYM + WIFI_CP, x.begin() + hs0 + s * WIFI_SYM + WIFI_CP + 64);
		fft(sym, false);
		std::vector<cf> eq(64);
		cvmul(sym.data(), eqc, eq.data(), 64);
		cf data48[48];
		demod_symbol_cplx(eq.data(), 1 + s, data48);   // pilots are un-rotated in HT-SIG -> phase ref stays valid
		float sd[48], dd[48];
		for(int d = 0; d < WIFI_NDATA; d++) { sd[d] = data48[d].imag(); }   // QBPSK: bit is on the Q axis
		deinterleave(sd, dd);
		for(int k = 0; k < 48; k++) { coded.push_back(dd[k]); }
	}
	std::vector<uint8_t> bits = viterbi(coded);   // 48 bits (HT-SIG1[24] + HT-SIG2[24])
	if((int)bits.size() < 48) { return; }
	int mcs = 0; for(int i = 0; i < 7; i++) { mcs |= bits[i] << i; }
	int cbw = bits[7];
	int htlen = 0; for(int i = 0; i < 16; i++) { htlen |= bits[8 + i] << i; }
	uint8_t crc_in[34];
	for(int i = 0; i < 24; i++) { crc_in[i] = bits[i]; }
	for(int i = 0; i < 10; i++) { crc_in[24 + i] = bits[24 + i]; }
	uint8_t crc = ht_crc8(crc_in, 34);
	uint8_t crc_rx = 0; for(int i = 0; i < 8; i++) { crc_rx |= bits[24 + 10 + i] << (7 - i); }   // c7..c0 MSB-first
	out.mcs = mcs;
	out.ht_40 = cbw;
	out.ht_length = htlen;
	out.htsig_ok = (crc == crc_rx) && mcs <= 7 && !cbw && htlen > 0 && htlen < 4096;
	if(getenv("WIFI_HTDBG")) { fprintf(stderr, "[ht] mcs=%d cbw=%d len=%d crc=%02x/%02x ok=%d\n", mcs, cbw, htlen, crc, crc_rx, out.htsig_ok); }
	if(!out.htsig_ok) { return; }
	// TODO(HT-DATA): HT-STF(1) + HT-LTF(1) at hs0+2*WIFI_SYM, re-estimate H over the 56 HT subcarriers
	// (htmaps()), then demod HT-DATA with these params + the HT interleaver (NCOL=13) + descramble -> MAC.
	RParams hp = ht_mcs_params(out.mcs);   // modulation/coding for the detected MCS (MCS0 scaffolded)
	(void)hp; (void)ltf_start; (void)m; (void)htmaps;
}

// ---- HT modulator (selftest) ----
std::vector<cf> wifi_ht_modulate(const std::vector<uint8_t>& psdu, int mcs, unsigned scram_seed) {
	const WifiMaps& m = maps();
	std::vector<cf> out;
	std::vector<cf> stf_t(m.stf, m.stf + 64); fft(stf_t, true);
	for(int i = 0; i < 160; i++) { out.push_back(stf_t[i % 16]); }
	std::vector<cf> ltf_t(m.ltf, m.ltf + 64); fft(ltf_t, true);
	for(int i = 0; i < 32; i++) { out.push_back(ltf_t[64 - 32 + i]); }
	for(int r = 0; r < 2; r++) { for(int i = 0; i < 64; i++) { out.push_back(ltf_t[i]); } }
	// L-SIG: rate 6, length = a legacy-duration spoof (any valid value; HT-SIG carries the real length)
	std::vector<uint8_t> sig(24, 0);
	int rc = code_from_rate(6);
	for(int i = 0; i < 4; i++) { sig[i] = (rc >> i) & 1; }
	int lsig_len = 40; for(int i = 0; i < 12; i++) { sig[5 + i] = (lsig_len >> i) & 1; }
	int par = 0; for(int i = 0; i < 17; i++) { par ^= sig[i]; }
	sig[17] = (uint8_t)par;
	std::vector<uint8_t> sigc; conv_encode(sig, sigc);
	std::vector<uint8_t> sigi; interleave(sigc, sigi);
	std::vector<cf> f; map_symbol(sigi, 0, f); ofdm_symbol_time(f, out);
	// HT-SIG: 48 bits (MCS/CBW/HT-Length + flags + CRC-8 + tail), BCC, interleave, BPSK then QBPSK (*j)
	std::vector<uint8_t> hb(48, 0);
	for(int i = 0; i < 7; i++) { hb[i] = (mcs >> i) & 1; }
	int htlen = (int)psdu.size();
	for(int i = 0; i < 16; i++) { hb[8 + i] = (htlen >> i) & 1; }
	hb[24 + 2] = 1;   // reserved bit = 1
	uint8_t crc = ht_crc8(hb.data(), 34);
	for(int i = 0; i < 8; i++) { hb[24 + 10 + i] = (crc >> (7 - i)) & 1; }
	std::vector<uint8_t> hc; conv_encode(hb, hc);   // 96 coded bits
	for(int s = 0; s < 2; s++) {
		std::vector<uint8_t> blk(hc.begin() + s * 48, hc.begin() + s * 48 + 48), bi;
		interleave(blk, bi);
		std::vector<cf> ff; map_symbol(bi, 1 + s, ff);
		for(auto& z : ff) { z = cf(-z.imag(), z.real()); }   // rotate +90 deg (QBPSK) - data goes to Q
		// keep pilots un-rotated (real) so RX phase tracking works
		const WifiMaps& mm = maps();
		for(int i = 0; i < 4; i++) { ff[mm.pilot_bin[i]] = cf(mm.pilot_val[i] * mm.pilot_pol[(1 + s) % 127], 0); }
		ofdm_symbol_time(ff, out);
	}
	// HT-STF + HT-LTF (so the frame is structurally complete; HT-DATA decode is pending)
	const HtMaps& hm = htmaps();
	std::vector<cf> hltf_t(hm.htltf, hm.htltf + 64); fft(hltf_t, true);
	for(int i = 0; i < 16; i++) { out.push_back(hltf_t[i % 16]); }   // HT-STF (approx)
	for(int i = 0; i < 16; i++) { out.push_back(hltf_t[64 - 16 + i]); }   // HT-LTF GI
	for(int i = 0; i < 64; i++) { out.push_back(hltf_t[i]); }
	(void)scram_seed;
	return out;
}

// ================= modulator (selftest/bench only) =================
std::vector<uint8_t> wifi_build_beacon(const uint8_t bssid[6], const std::string& ssid) {
	std::vector<uint8_t> f;
	f.push_back(0x80); f.push_back(0x00);       // FC: mgmt beacon
	f.push_back(0); f.push_back(0);             // duration
	for(int i = 0; i < 6; i++) { f.push_back(0xff); }   // DA broadcast
	for(int i = 0; i < 6; i++) { f.push_back(bssid[i]); }   // SA
	for(int i = 0; i < 6; i++) { f.push_back(bssid[i]); }   // BSSID
	f.push_back(0); f.push_back(0);             // seq
	for(int i = 0; i < 8; i++) { f.push_back(0); }         // timestamp
	f.push_back(0x64); f.push_back(0x00);       // beacon interval
	f.push_back(0x01); f.push_back(0x04);       // capability
	f.push_back(0); f.push_back((uint8_t)ssid.size());     // SSID tag
	for(char c : ssid) { f.push_back((uint8_t)c); }
	f.push_back(0x01); f.push_back(0x04); f.push_back(0x82); f.push_back(0x84); f.push_back(0x8b); f.push_back(0x96);   // supported rates
	uint32_t fcs = wifi_crc32(f.data(), f.size());
	f.push_back(fcs & 0xff); f.push_back((fcs >> 8) & 0xff); f.push_back((fcs >> 16) & 0xff); f.push_back((fcs >> 24) & 0xff);
	return f;
}

static void map_symbol(const std::vector<uint8_t>& b48, int sym_idx, std::vector<cf>& freq) {
	const WifiMaps& m = maps();
	freq.assign(64, cf(0, 0));
	for(int d = 0; d < WIFI_NDATA; d++) { freq[m.data_bin[d]] = cf(b48[d] ? 1.f : -1.f, 0); }
	for(int i = 0; i < 4; i++) { freq[m.pilot_bin[i]] = cf(m.pilot_val[i] * m.pilot_pol[sym_idx % 127], 0); }
}
// map NCBPS interleaved bits -> 64 freq bins via QAM (nbpsc bits per data subcarrier) + pilots
static void map_symbol_qam(const std::vector<uint8_t>& bits, int nbpsc, int sym_idx, std::vector<cf>& freq) {
	const WifiMaps& m = maps();
	freq.assign(64, cf(0, 0));
	for(int d = 0; d < WIFI_NDATA; d++) { freq[m.data_bin[d]] = qam_map(&bits[(size_t)d * nbpsc], nbpsc); }
	for(int i = 0; i < 4; i++) { freq[m.pilot_bin[i]] = cf(m.pilot_val[i] * m.pilot_pol[sym_idx % 127], 0); }
}
static void ofdm_symbol_time(const std::vector<cf>& freq, std::vector<cf>& out) {
	std::vector<cf> t = freq; fft(t, true);
	for(int i = 0; i < WIFI_CP; i++) { out.push_back(t[64 - WIFI_CP + i]); }   // CP
	for(int i = 0; i < 64; i++) { out.push_back(t[i]); }
}

std::vector<cf> wifi_ofdm_modulate(const std::vector<uint8_t>& psdu, int rate_mbps, unsigned scram_seed) {
	const WifiMaps& m = maps();
	std::vector<cf> out;
	// preamble: STF (10 short = 160 samples from stf freq, repeated) + LTF (GI32 + 2x LTS)
	std::vector<cf> stf_t(m.stf, m.stf + 64); fft(stf_t, true);
	for(int i = 0; i < 160; i++) { out.push_back(stf_t[i % 16]); }   // STF short-symbol periodic (approx)
	std::vector<cf> ltf_t(m.ltf, m.ltf + 64); fft(ltf_t, true);
	for(int i = 0; i < 32; i++) { out.push_back(ltf_t[64 - 32 + i]); }   // GI2
	for(int r = 0; r < 2; r++) { for(int i = 0; i < 64; i++) { out.push_back(ltf_t[i]); } }

	// SIGNAL (24 bits, rate 1/2, BPSK, not scrambled)
	std::vector<uint8_t> sig(24, 0);
	int rc = code_from_rate(rate_mbps);
	for(int i = 0; i < 4; i++) { sig[i] = (rc >> i) & 1; }
	for(int i = 0; i < 12; i++) { sig[5 + i] = ((int)psdu.size() >> i) & 1; }
	int par = 0; for(int i = 0; i < 17; i++) { par ^= sig[i]; }
	sig[17] = (uint8_t)par;
	std::vector<uint8_t> sigc; conv_encode(sig, sigc);
	std::vector<uint8_t> sigi; interleave(sigc, sigi);
	std::vector<cf> f; map_symbol(sigi, 0, f); ofdm_symbol_time(f, out);

	// DATA: SERVICE(16=0) + PSDU + tail(6=0), scrambled (SERVICE+PSDU; tail stays 0 after)
	RParams rp = rate_params(rate_mbps);
	int nsym = (16 + 8 * (int)psdu.size() + 6 + rp.ndbps - 1) / rp.ndbps;
	std::vector<uint8_t> data(nsym * rp.ndbps, 0);
	for(int i = 0; i < 8 * (int)psdu.size(); i++) { data[16 + i] = (psdu[i / 8] >> (i % 8)) & 1; }
	int scr_len = 16 + 8 * (int)psdu.size();
	std::vector<uint8_t> scr(data.begin(), data.begin() + scr_len);
	scramble(scr, (uint8_t)scram_seed);
	for(int i = 0; i < scr_len; i++) { data[i] = scr[i]; }
	std::vector<uint8_t> datac; conv_encode(data, datac);   // rate-1/2 coded
	// puncture to the code rate, then segment into NCBPS-bit symbols, interleave, QAM-map
	int pp = punct_period(rp.num, rp.den);
	std::vector<uint8_t> punc;
	punc.reserve(datac.size());
	for(int i = 0; i < (int)datac.size(); i++) { if(punct_keep(i % pp, rp.num, rp.den)) { punc.push_back(datac[i]); } }
	for(int s = 0; s < nsym; s++) {
		std::vector<uint8_t> intl(rp.ncbps, 0);
		for(int k = 0; k < rp.ncbps; k++) { intl[intlv_map(k, rp.ncbps, rp.nbpsc)] = punc[(size_t)s * rp.ncbps + k]; }
		std::vector<cf> ff; map_symbol_qam(intl, rp.nbpsc, 1 + s, ff); ofdm_symbol_time(ff, out);
	}
	return out;
}

} // namespace ismmon
