#include "gfsk_rx.h"
#include "ble_coded.h"

#include <cmath>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace ismmon {

#ifdef __ARM_NEON
// 4-lane atan2 approximation (max err ~0.0038 rad) for the FM discriminator. The demod uses
// mean-removed soft-symbol correlation + integrate-and-dump, which is insensitive to this error
// (validated: the aarch64/NEON build passes the same BLE selftest as the x86 scalar reference).
static inline float32x4_t neon_atan2(float32x4_t y, float32x4_t x) {
	const float32x4_t pi = vdupq_n_f32((float)M_PI), hpi = vdupq_n_f32((float)M_PI_2), z = vdupq_n_f32(0.f);
	float32x4_t ax = vabsq_f32(x), ay = vabsq_f32(y);
	float32x4_t mx = vmaxq_f32(ax, ay), mn = vminq_f32(ax, ay);
	float32x4_t a = vdivq_f32(mn, vmaxq_f32(mx, vdupq_n_f32(1e-20f)));
	float32x4_t s = vmulq_f32(a, a);
	float32x4_t poly = vmlaq_f32(vdupq_n_f32(0.15931422f), s, vdupq_n_f32(-0.0464964749f));
	poly = vmlaq_f32(vdupq_n_f32(-0.327622764f), s, poly);
	float32x4_t r = vmulq_f32(a, vmlaq_f32(vdupq_n_f32(1.f), poly, s));   // a*(1 + poly*s) = atan(a)
	r = vbslq_f32(vcgtq_f32(ay, ax), vsubq_f32(hpi, r), r);               // ay>ax: pi/2 - r
	r = vbslq_f32(vcltq_f32(x, z), vsubq_f32(pi, r), r);                  // x<0:   pi - r
	r = vbslq_f32(vcltq_f32(y, z), vnegq_f32(r), r);                      // y<0:   -r
	return r;
}
#endif

// preamble (0xAA) + advertising access address (0x8E89BED6, little-endian bytes), as +-1 NRZ
// symbols in transmit order - the 40-symbol sync template.
static const std::vector<float>& sync_template() {
	static std::vector<float> P;
	if(P.empty()) {
		uint8_t pre = 0xAA;
		uint8_t aa[4] = { (uint8_t)(BLE_ADV_AA & 0xFF), (uint8_t)((BLE_ADV_AA >> 8) & 0xFF),
		                  (uint8_t)((BLE_ADV_AA >> 16) & 0xFF), (uint8_t)((BLE_ADV_AA >> 24) & 0xFF) };
		bits b = bytes_to_bits(&pre, 1);
		bits ab = bytes_to_bits(aa, 4);
		b.insert(b.end(), ab.begin(), ab.end());
		P.resize(b.size());
		for(size_t i = 0; i < b.size(); i++) { P[i] = b[i] ? 1.0f : -1.0f; }
	}
	return P;
}

static inline double interp(const std::vector<double>& cum, double x) {
	if(x <= 0) { return cum.front(); }
	if(x >= (double)(cum.size() - 1)) { return cum.back(); }
	long i = (long)std::floor(x);
	double f = x - (double)i;
	return cum[i] * (1.0 - f) + cum[i + 1] * f;
}

// FM discriminator + running phase integral, shared by 1M / 2M / Coded. cum[i] = integral of the
// instantaneous frequency (integrate-and-dump reads two points of it); mag_sum = sum|iq|. NEON on
// AArch64, scalar reference elsewhere (identical math to the original inline 1M loop).
static void fm_discriminator(const cf* iq, size_t n, std::vector<double>& cum, double& mag_sum) {
	cum.assign(n, 0.0);
	mag_sum = 0;
#ifdef __ARM_NEON
	{
		std::vector<float> d(n, 0.f);
		float32x4_t magv = vdupq_n_f32(0.f);
		size_t i = 1;
		for(; i + 4 <= n; i += 4) {
			float32x4x2_t cur = vld2q_f32((const float*)(iq + i));       // re_i, im_i
			float32x4x2_t prv = vld2q_f32((const float*)(iq + i - 1));   // re_{i-1}, im_{i-1}
			float32x4_t pre = vaddq_f32(vmulq_f32(cur.val[0], prv.val[0]), vmulq_f32(cur.val[1], prv.val[1]));
			float32x4_t pim = vsubq_f32(vmulq_f32(cur.val[1], prv.val[0]), vmulq_f32(cur.val[0], prv.val[1]));
			vst1q_f32(&d[i], neon_atan2(pim, pre));
			magv = vaddq_f32(magv, vsqrtq_f32(vaddq_f32(vmulq_f32(cur.val[0], cur.val[0]), vmulq_f32(cur.val[1], cur.val[1]))));
		}
		mag_sum = vaddvq_f32(magv);
		for(; i < n; i++) {
			cf p = iq[i] * std::conj(iq[i - 1]);
			d[i] = (float)std::atan2((double)p.imag(), (double)p.real());
			mag_sum += std::abs(iq[i]);
		}
		for(size_t k = 1; k < n; k++) { cum[k] = cum[k - 1] + (double)d[k]; }
	}
#else
	for(size_t i = 1; i < n; i++) {
		cf p = iq[i] * std::conj(iq[i - 1]);
		cum[i] = cum[i - 1] + std::atan2((double)p.imag(), (double)p.real());
		mag_sum += std::abs(iq[i]);
	}
#endif
}

// +-1 sync template = preamble bytes (LSB-first) then the 32-bit AA (little-endian bytes, LSB-first).
static std::vector<float> make_sync_template(const uint8_t* pre, int npre, uint32_t aa) {
	uint8_t ab[4] = { (uint8_t)(aa & 0xFF), (uint8_t)((aa >> 8) & 0xFF), (uint8_t)((aa >> 16) & 0xFF), (uint8_t)((aa >> 24) & 0xFF) };
	bits b = bytes_to_bits(pre, npre);
	bits abb = bytes_to_bits(ab, 4);
	b.insert(b.end(), abb.begin(), abb.end());
	std::vector<float> P(b.size());
	for(size_t i = 0; i < b.size(); i++) { P[i] = b[i] ? 1.f : -1.f; }
	return P;
}

// I[j] = interp(cum, o + j*sps) for j in [0,npts). Consecutive soft symbols share an interp point, so
// a caller wanting `SYNC` symbols gets SYNC+1 interps instead of 2*SYNC. NEON does 2 lanes (double)
// with a scalar gather where the points are strictly in-bounds (the sync search guarantees this via
// o_max); the scalar interp handles edges and IS the numeric reference — bit-identical (same floor,
// same cum[i]*(1-f)+cum[i+1]*f, no FMA).
static inline void interp_seq(const std::vector<double>& cum, double o, double sps, int npts, double* I) {
	int j = 0;
#ifdef __ARM_NEON
	// NEON the safe prefix: both lanes need 0 <= x < N-1 (so i and i+1 are in bounds). j+1 < lim keeps
	// the last lane's point below N-1; the remaining tail (and any edge) goes through scalar interp.
	double lim = ((double)((int)cum.size() - 1) - o) / sps;
	if(o >= 0.0 && sps > 0.0) {
		const float64x2_t one = vdupq_n_f64(1.0);
		// 4 points/iter as two independent lanes so the two cum[] gathers issue back-to-back and their
		// load latency overlaps (the in-order A53 can't reorder across them). Numerics identical to the
		// 2-wide path (same floor, same cum[i]*(1-f)+cum[i+1]*f, no FMA) - still bit-exact vs scalar.
		for(; j + 4 <= npts && (double)(j + 3) < lim; j += 4) {
			float64x2_t x0 = { o + (double)j * sps, o + (double)(j + 1) * sps };
			float64x2_t x1 = { o + (double)(j + 2) * sps, o + (double)(j + 3) * sps };
			float64x2_t fl0 = vrndmq_f64(x0), fl1 = vrndmq_f64(x1);
			float64x2_t f0 = vsubq_f64(x0, fl0), f1 = vsubq_f64(x1, fl1);
			long a0 = (long)vgetq_lane_f64(fl0, 0), a1 = (long)vgetq_lane_f64(fl0, 1);
			long b0 = (long)vgetq_lane_f64(fl1, 0), b1 = (long)vgetq_lane_f64(fl1, 1);
			float64x2_t c0 = { cum[a0], cum[a1] }, c0n = { cum[a0 + 1], cum[a1 + 1] };
			float64x2_t c1 = { cum[b0], cum[b1] }, c1n = { cum[b0 + 1], cum[b1 + 1] };
			vst1q_f64(&I[j], vaddq_f64(vmulq_f64(c0, vsubq_f64(one, f0)), vmulq_f64(c0n, f0)));
			vst1q_f64(&I[j + 2], vaddq_f64(vmulq_f64(c1, vsubq_f64(one, f1)), vmulq_f64(c1n, f1)));
		}
		for(; j + 2 <= npts && (double)(j + 1) < lim; j += 2) {
			float64x2_t x = { o + (double)j * sps, o + (double)(j + 1) * sps };
			float64x2_t fl = vrndmq_f64(x);                       // floor (toward -inf), == std::floor
			float64x2_t f = vsubq_f64(x, fl);
			long i0 = (long)vgetq_lane_f64(fl, 0), i1 = (long)vgetq_lane_f64(fl, 1);
			float64x2_t ci = { cum[i0], cum[i1] }, ci1 = { cum[i0 + 1], cum[i1 + 1] };
			vst1q_f64(&I[j], vaddq_f64(vmulq_f64(ci, vsubq_f64(one, f)), vmulq_f64(ci1, f)));
		}
	}
#endif
	for(; j < npts; j++) { I[j] = interp(cum, o + (double)j * sps); }
}

// Slide the +-1 sync template P over the phase-integral cum at 0.5-sample steps; return the best
// normalized (mean-removed, scale-invariant) correlation + its offset. The dominant BLE cost after the
// discriminator; shared by 1M / 2M / Coded. NEON: 2-wide-double interp gather + num/den reductions.
static void sync_correlate(const std::vector<double>& cum, const std::vector<float>& P, double sps,
                           double o_max, double& best_score, double& best_o) {
	const int SYNC = (int)P.size();
	best_score = 0; best_o = -1;
	if(SYNC < 1 || SYNC > 128) { return; }
	double I[129];
	for(double o = 0; o <= o_max; o += 0.5) {
		interp_seq(cum, o, sps, SYNC + 1, I);
		// mean of the soft symbols s[j]=I[j+1]-I[j] telescopes to (I[SYNC]-I[0])/SYNC - the whole sum
		// loop is redundant (and this closed form is one subtraction, so slightly more accurate).
		double mean = (I[SYNC] - I[0]) / SYNC;
		double num = 0, den = 0; int j = 0;
#ifdef __ARM_NEON
		// 4-wide with TWO independent num/den accumulators to hide the FMA latency on the in-order A53
		// (a single accumulator chain issues 1 FMA every ~4 cyc; two chains ~double the throughput), vfmaq.
		const float64x2_t vm = vdupq_n_f64(mean);
		float64x2_t vnum0 = vdupq_n_f64(0.0), vnum1 = vdupq_n_f64(0.0);
		float64x2_t vden0 = vdupq_n_f64(0.0), vden1 = vdupq_n_f64(0.0);
		for(; j + 4 <= SYNC; j += 4) {
			float64x2_t s0 = vsubq_f64(vsubq_f64(vld1q_f64(&I[j + 1]), vld1q_f64(&I[j])), vm);
			float64x2_t s1 = vsubq_f64(vsubq_f64(vld1q_f64(&I[j + 3]), vld1q_f64(&I[j + 2])), vm);
			float64x2_t p0 = { (double)P[j], (double)P[j + 1] };
			float64x2_t p1 = { (double)P[j + 2], (double)P[j + 3] };
			vnum0 = vfmaq_f64(vnum0, s0, p0);
			vnum1 = vfmaq_f64(vnum1, s1, p1);
			vden0 = vfmaq_f64(vden0, s0, s0);
			vden1 = vfmaq_f64(vden1, s1, s1);
		}
		for(; j + 2 <= SYNC; j += 2) {
			float64x2_t s0 = vsubq_f64(vsubq_f64(vld1q_f64(&I[j + 1]), vld1q_f64(&I[j])), vm);
			float64x2_t p0 = { (double)P[j], (double)P[j + 1] };
			vnum0 = vfmaq_f64(vnum0, s0, p0);
			vden0 = vfmaq_f64(vden0, s0, s0);
		}
		float64x2_t vnum = vaddq_f64(vnum0, vnum1), vden = vaddq_f64(vden0, vden1);
		num = vgetq_lane_f64(vnum, 0) + vgetq_lane_f64(vnum, 1);
		den = vgetq_lane_f64(vden, 0) + vgetq_lane_f64(vden, 1);
#endif
		for(; j < SYNC; j++) { double v = (I[j + 1] - I[j]) - mean; num += v * (double)P[j]; den += v * v; }
		double score = (den > 0) ? num / std::sqrt(den * SYNC) : 0;
		if(score > best_score) { best_score = score; best_o = o; }
	}
}

bool demod_ble_burst(const cf* iq, size_t n, int channel, double sps, ble_packet& out,
                     double corr_threshold) {
	const int SYNC = 40;                 // preamble(8) + AA(32)
	const double PH = M_PI * BLE_H;       // ideal per-symbol phase magnitude (+-)
	if(n < (size_t)((SYNC + 8) * sps) || sps < 1.5) { return false; }

	// FM discriminator + running phase integral (integrate-and-dump reads two points of it)
	std::vector<double> cum;
	double mag_sum = 0;
	fm_discriminator(iq, n, cum, mag_sum);

	const std::vector<float>& P = sync_template();

	// slide the sync template over the burst at half-sample steps; score = scale-invariant
	// correlation of the mean-removed soft symbols with the +-1 template (mean removal kills CFO)
	double best_score = 0, best_o = -1;
	double o_max = (double)n - (SYNC + 2) * sps;
	sync_correlate(cum, P, sps, o_max, best_score, best_o);

	out.channel = channel;
	out.sync_corr = best_score;
	out.rssi_db = 20.0f * std::log10((float)(mag_sum / (n > 1 ? n - 1 : 1)) + 1e-9f);
	if(best_score < corr_threshold || best_o < 0) { return false; }

	// feed-forward CFO from the known sync word: cfo = mean(soft_j - P[j]*PH) over preamble+AA
	double cfo = 0;
	{
		double Ic[SYNC + 1];
		interp_seq(cum, best_o, sps, SYNC + 1, Ic);
		for(int j = 0; j < SYNC; j++) { cfo += (Ic[j + 1] - Ic[j]) - P[j] * PH; }
	}
	cfo /= SYNC;
	out.cfo_hz = cfo / (2.0 * M_PI) * BLE_SYM_RATE;

	// slice post-AA symbols against the CFO bias -> whitened PDU+CRC bits
	int navail = (int)((((double)(n - 1) - best_o) / sps) - SYNC);
	int nbits = navail;
	if(nbits > (2 + 37 + 3) * 8) { nbits = (2 + 37 + 3) * 8; }   // legacy adv max
	nbits -= nbits % 8;
	if(nbits < 16) { return true; }   // sync found but too short for a header

	bits body(nbits);
	{
		std::vector<double> Ib(nbits + 1);
		interp_seq(cum, best_o + (double)SYNC * sps, sps, nbits + 1, Ib.data());
		for(int k = 0; k < nbits; k++) { body[k] = ((Ib[k + 1] - Ib[k]) - cfo) > 0 ? 1 : 0; }
	}
	ble_whiten(body, channel);   // de-whiten (self-inverse)

	std::vector<uint8_t> bytes = bits_to_bytes(body);
	if(bytes.size() < 2) { return true; }
	int length = bytes[1] & 0x3F;
	size_t need = (size_t)(2 + length + 3);   // header + payload + CRC
	if(bytes.size() < need) { return true; }  // truncated burst

	std::vector<uint8_t> pdu(bytes.begin(), bytes.begin() + 2 + length);
	out.pdu = pdu;
	out.crc_calc = ble_crc24(bytes_to_bits(pdu.data(), pdu.size()), BLE_ADV_CRC_INIT);
	out.crc_rx = (uint32_t)bytes[2 + length] | ((uint32_t)bytes[3 + length] << 8) | ((uint32_t)bytes[4 + length] << 16);
	out.crc_ok = (out.crc_calc == out.crc_rx);
	if(out.crc_ok) { ble_parse_adv_pdu(pdu, out.adv); }
	return true;
}

// ---- generalized uncoded demod (LE 2M; parameterized preamble/AA/rate) ----
static bool demod_uncoded(const cf* iq, size_t n, int channel, double sps,
                          const std::vector<float>& P, double sym_rate, int phy, uint32_t aa,
                          ble_packet& out, double corr_threshold) {
	const int SYNC = (int)P.size();
	const double PH = M_PI * BLE_H;
	out.channel = channel; out.phy = phy; out.aa = aa;
	if(n < (size_t)((SYNC + 8) * sps) || sps < 1.5) { return false; }
	std::vector<double> cum;
	double mag_sum;
	fm_discriminator(iq, n, cum, mag_sum);
	out.rssi_db = 20.f * std::log10((float)(mag_sum / (n > 1 ? n - 1 : 1)) + 1e-9f);

	double best_score = 0, best_o = -1;
	double o_max = (double)n - (SYNC + 2) * sps;
	sync_correlate(cum, P, sps, o_max, best_score, best_o);
	out.sync_corr = best_score;
	if(best_score < corr_threshold || best_o < 0) { return false; }

	double cfo = 0;
	{
		std::vector<double> Ic(SYNC + 1);
		interp_seq(cum, best_o, sps, SYNC + 1, Ic.data());
		for(int j = 0; j < SYNC; j++) { cfo += (Ic[j + 1] - Ic[j]) - P[j] * PH; }
	}
	cfo /= SYNC;
	out.cfo_hz = cfo / (2.0 * M_PI) * sym_rate;

	int navail = (int)((((double)(n - 1) - best_o) / sps) - SYNC);
	int nbits = navail;
	int cap = (2 + 257 + 3) * 8;   // header + max payload + CRC
	if(nbits > cap) { nbits = cap; }
	nbits -= nbits % 8;
	if(nbits < 16) { return true; }
	bits body(nbits);
	{
		std::vector<double> Ib(nbits + 1);
		interp_seq(cum, best_o + (double)SYNC * sps, sps, nbits + 1, Ib.data());
		for(int k = 0; k < nbits; k++) { body[k] = ((Ib[k + 1] - Ib[k]) - cfo) > 0 ? 1 : 0; }
	}
	ble_whiten(body, channel);
	std::vector<uint8_t> bytes = bits_to_bytes(body);
	if(bytes.size() < 2) { return true; }
	int length = bytes[1];   // full 8-bit length (extended/data PDUs); legacy adv RFU bits are 0
	size_t need = (size_t)(2 + length + 3);
	if(bytes.size() < need) { return true; }
	std::vector<uint8_t> pdu(bytes.begin(), bytes.begin() + 2 + length);
	out.pdu = pdu;
	out.crc_calc = ble_crc24(bytes_to_bits(pdu.data(), pdu.size()), BLE_ADV_CRC_INIT);
	out.crc_rx = (uint32_t)bytes[2 + length] | ((uint32_t)bytes[3 + length] << 8) | ((uint32_t)bytes[4 + length] << 16);
	out.crc_ok = (out.crc_calc == out.crc_rx);
	if(out.crc_ok) { ble_parse_adv_pdu(pdu, out.adv); }
	return true;
}

bool demod_ble_burst_2m(const cf* iq, size_t n, int channel, double sps, ble_packet& out,
                        double corr_threshold, uint32_t aa) {
	uint8_t pre[2] = { 0xAA, 0xAA };   // LE 2M: 16-symbol preamble (0xAAAA) for AA LSB = 0
	std::vector<float> P = make_sync_template(pre, 2, aa);
	return demod_uncoded(iq, n, channel, sps, P, 2.0e6, BLE_PHY_2M, aa, out, corr_threshold);
}

// ---- LE Coded PHY demod (S=8 / S=2, Long Range) ----
bool demod_ble_coded(const cf* iq, size_t n, int channel, double sps, ble_packet& out,
                     double corr_threshold, uint32_t aa) {
	const double PH = M_PI * BLE_H;   // coded PHY: 1 Msym/s GFSK h=0.5
	out.channel = channel; out.phy = BLE_PHY_CODED_S8; out.aa = aa;
	const bits& pre = ble_coded_preamble();
	const int PRE = (int)pre.size();
	if(n < (size_t)((PRE + 300) * sps) || sps < 1.5) { return false; }

	std::vector<double> cum;
	double mag_sum;
	fm_discriminator(iq, n, cum, mag_sum);
	out.rssi_db = 20.f * std::log10((float)(mag_sum / (n > 1 ? n - 1 : 1)) + 1e-9f);

	std::vector<float> P(PRE);
	for(int j = 0; j < PRE; j++) { P[j] = pre[j] ? 1.f : -1.f; }

	double best_score = 0, best_o = -1;
	double o_max = (double)n - (PRE + 2) * sps;
	sync_correlate(cum, P, sps, o_max, best_score, best_o);
	out.sync_corr = best_score;
	if(best_score < corr_threshold || best_o < 0) { return false; }

	double cfo = 0;
	{
		std::vector<double> Ic(PRE + 1);
		interp_seq(cum, best_o, sps, PRE + 1, Ic.data());
		for(int j = 0; j < PRE; j++) { cfo += (Ic[j + 1] - Ic[j]) - P[j] * PH; }
	}
	cfo /= PRE;
	out.cfo_hz = cfo / (2.0 * M_PI) * 1.0e6;

	const int F1_CODED = 74, F1_SYM = F1_CODED * 4;   // FEC1 = AA(32)+CI(2)+TERM(3) = 37 info -> 74 coded -> 296 sym @ S=8

	// try the found preamble origin and +-8/16 symbol shifts (the coded preamble is period-8, so the
	// correlation peak can be an octet off; the FEC1 AA check disambiguates).
	for(int shift : { 0, 8, -8, 16, -16 }) {
		double origin = best_o + shift * sps;
		if(origin < 0) { continue; }
		int total_syms = (int)((((double)(n - 1) - origin) / sps) - PRE);
		if(total_syms < F1_SYM + 8) { continue; }
		// precompute all soft symbols for this origin once (shared interp points -> total_syms+1 interps
		// instead of 2 per symbol); softsym(k) reads the array.
		std::vector<double> Is(total_syms + 1);
		interp_seq(cum, origin + (double)PRE * sps, sps, total_syms + 1, Is.data());
		auto softsym = [&](int k) -> double { return Is[k + 1] - Is[k] - cfo; };

		// FEC1: despread S=8 (4 symbols -> 1 soft coded bit: pattern 1->[+,+,-,-]) -> Viterbi (terminated)
		std::vector<float> sc1(F1_CODED);
		for(int c = 0; c < F1_CODED; c++) {
			double g = 0;
			for(int q = 0; q < 4; q++) { double v = softsym(c * 4 + q); g += (q < 2) ? v : -v; }
			sc1[c] = (float)g;
		}
		bits f1 = ble_viterbi_decode(sc1, true);
		uint32_t got = 0;
		for(int i = 0; i < 32; i++) { got |= ((uint32_t)(f1[i] & 1)) << i; }
		if(got != aa) { continue; }   // wrong alignment / not this AA

		int ci = (f1[32] & 1) | ((f1[33] & 1) << 1);
		int S = (ci == 0) ? 8 : 2;    // FLAG: CI 0->S8, 1->S2
		out.coded_s = S;
		out.phy = (S == 8) ? BLE_PHY_CODED_S8 : BLE_PHY_CODED_S2;

		int f2_start = F1_SYM;
		int f2_syms = total_syms - f2_start;
		int f2_coded = (S == 8) ? (f2_syms / 4) : f2_syms;
		int cap = (2 + 257 + 3 + 3) * 8 * 2;   // generous coded-bit cap
		if(f2_coded > cap) { f2_coded = cap; }
		if(f2_coded < 16) { return true; }
		std::vector<float> sc2(f2_coded);
		for(int c = 0; c < f2_coded; c++) {
			if(S == 8) {
				double g = 0;
				for(int q = 0; q < 4; q++) { double v = softsym(f2_start + c * 4 + q); g += (q < 2) ? v : -v; }
				sc2[c] = (float)g;
			} else {
				sc2[c] = (float)softsym(f2_start + c);
			}
		}
		bits f2 = ble_viterbi_decode(sc2, false);   // whiten(PDU+CRC) + TERM2 + trailing garbage
		ble_whiten(f2, channel);
		std::vector<uint8_t> bytes = bits_to_bytes(f2);
		if(bytes.size() < 2) { return true; }
		int length = bytes[1];
		size_t need = (size_t)(2 + length + 3);
		if(bytes.size() < need) { return true; }
		std::vector<uint8_t> pdu(bytes.begin(), bytes.begin() + 2 + length);
		out.pdu = pdu;
		out.crc_calc = ble_crc24(bytes_to_bits(pdu.data(), pdu.size()), BLE_ADV_CRC_INIT);
		out.crc_rx = (uint32_t)bytes[2 + length] | ((uint32_t)bytes[3 + length] << 8) | ((uint32_t)bytes[4 + length] << 16);
		out.crc_ok = (out.crc_calc == out.crc_rx);
		if(out.crc_ok) { ble_parse_adv_pdu(pdu, out.adv); }
		return true;
	}
	return true;   // preamble found but FEC1 AA never matched
}

} // namespace ismmon
