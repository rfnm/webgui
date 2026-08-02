/* sweep_eq_test — host-side (x86) unit test for the sweep composition + static per-hop
 * response correction in sweep_comp.h. No board, no librfnm, no GPU: N_FFT and a scalar
 * cw_convert are defined here, pffft builds for x86.
 *
 * The contract under test (#94 design): the per-hop correction is a CONSTANT loaded from a
 * table — flat composition from the very first pass when a curve matches the armed bw, the
 * honest dome when none does, and the correction NEVER moves under the received data (the
 * v1 adaptive estimator re-fitted the shape from a cross-hop median of live data; broadband
 * ambient world poisoned it into a placement-dependent sawtooth). Real signals,
 * genuine per-hop level differences and broadband world structure must all SURVIVE (a
 * flatness cal, not an AGC), dropped hops hold-fill, and mixed-gain passes compose at the
 * reference-gain level. */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define N_FFT 4096

/* scalar stand-in for spectrumd's NEON cw_convert (same math) */
static void cw_convert(const int16_t *in, const float *win, float *out, int n) {
	for (int i = 0; i < n; i++) {
		out[2 * i] = static_cast<float>(in[2 * i]) * win[i];
		out[2 * i + 1] = static_cast<float>(in[2 * i + 1]) * win[i];
	}
}

#include "sweep_comp.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t g_seed = 0x52464e4d;
static float frnd() { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5; return static_cast<float>(g_seed & 0xFFFFFF) / 16777216.0f; }

static double u8_db(uint8_t v, float dbMin, float dbScale) { return static_cast<double>(v) / dbScale + dbMin; }

/* output bin for an absolute frequency (same mapping as SweepComp::compose) */
static int fbin(const SweepComp &c, double f) { return static_cast<int>(llround((f - c.a) / c.span * (N_FFT - 1))); }

/* mean dB over an output-bin range */
static double zone_db(const uint8_t *row, int j0, int j1, float dbMin, float dbScale) {
	double s = 0; int n = 0;
	for (int j = j0; j <= j1; j++) { if (j >= 0 && j < N_FFT) { s += u8_db(row[j], dbMin, dbScale); n++; } }
	return n ? s / n : 0.0;
}

/* hump metric: mean over hops of (hop-center zone dB - hop-edge zone dB); the raw dome
 * makes this strongly positive, a corrected composite makes it ~0 */
static double hump_metric(const SweepComp &c, const uint8_t *row, const std::vector<size_t> &hops, float dbMin, float dbScale, double *worst) {
	const double h = SWEEP_USABLE * static_cast<double>(c.bw) / 2.0;   /* kept half-width, Hz */
	double sum = 0; *worst = 0;
	for (size_t k : hops) {
		double lo = c.los[k];
		double ctr = zone_db(row, fbin(c, lo - 0.16 * h), fbin(c, lo + 0.16 * h), dbMin, dbScale);
		double edg = 0.5 * (zone_db(row, fbin(c, lo - 0.92 * h), fbin(c, lo - 0.70 * h), dbMin, dbScale) +
				zone_db(row, fbin(c, lo + 0.70 * h), fbin(c, lo + 0.92 * h), dbMin, dbScale));
		double m = ctr - edg;
		sum += m;
		if (std::fabs(m) > std::fabs(*worst)) { *worst = m; }
	}
	return sum / static_cast<double>(hops.size());
}

static float dome_db_at(float x) {   /* the synthetic per-hop response, x = d/half_use in [-1,1] */
	return -5.0f * powf(std::fabs(x), 2.2f) + 0.8f * x;   /* asymmetric, ~5.4 dB deep */
}

/* inject one pass: dome * world(kf) * noise. World on the absolute-frequency axis in hop
 * units (kf = k + 0.5x): floor below hop 5, +2.5 dB/hop rising edge hops 5-10, plateau
 * above — broadband occupancy with in-hop slope, the structure that poisoned the v1
 * estimator. hop 0 at 3x level (genuine step: must SURVIVE), hop 7 carrying a +30 dB
 * signal at d=+200..+240, hop `skip` (if >=0) dropped. world_on=false = flat world. */
static void inject_pass(SweepComp &c, int skip, bool world_on) {
	std::vector<float> pw(c.nkeep);
	for (size_t k = 0; k < c.nhops(); k++) {
		if (static_cast<int>(k) == skip) { continue; }
		for (int s = 0; s < c.nkeep; s++) {
			int d = s - c.half_use;
			float x = static_cast<float>(d) / static_cast<float>(c.half_use);
			float w_db = 0.0f;
			if (world_on) {
				float kf = static_cast<float>(k) + 0.5f * x;
				if (kf >= 5.0f && kf <= 10.0f) { w_db = (kf - 5.0f) * 2.5f; }
				else if (kf > 10.0f) { w_db = 12.5f; }
			}
			float v = 1e6f * powf(10.0f, (dome_db_at(x) + w_db) / 10.0f) * (0.85f + 0.3f * frnd());
			if (k == 0) { v *= 3.0f; }
			if (k == 7 && d >= 200 && d <= 240) { v *= 1000.0f; }
			pw[s] = v;
		}
		c.add_hop_pow(k, pw.data());
	}
}

static const char *write_table(uint32_t bw) {   /* table = the injected dome, exact */
	const char *path = "/tmp/sweep_resp_test.tbl";
	FILE *f = fopen(path, "w");
	fprintf(f, "# unit-test curve = the injected dome\nBW %u\n", bw);
	for (int g = 0; g < SWEEP_EQ_SEG; g++) {
		float x = ((static_cast<float>(g) + 0.5f) / SWEEP_EQ_SEG) * 2.0f - 1.0f;
		fprintf(f, "%.4f\n", dome_db_at(x));
	}
	fclose(f);
	return path;
}

int main() {
	const float dbMin = 30.0f, dbScale = 255.0f / 80.0f;
	uint8_t row[N_FFT];
	std::vector<size_t> plain;   /* hops without signal/level special-casing (20-hop plans) */
	for (size_t k = 1; k < 20; k++) { if (k != 7 && k != 12) { plain.push_back(k); } }

	/* ---- A: hop plan + LO band containment ---- */
	{
		SweepComp c(1000e6, 1400e6, 25000000u);
		CHECK(c.nhops() == 20, "hop plan: got %zu hops", c.nhops());
		SweepComp lc(10e6, 3500e6, 50000000u);
		CHECK(lc.nhops() == 88, "full-span plan: got %zu hops", lc.nhops());
		for (size_t k = 0; k < lc.nhops(); k++) {
			CHECK(lc.los[k] >= 10e6 && lc.los[k] <= 3500e6, "full span: LO %zu = %.3f MHz outside [10,3500]", k, lc.los[k] / 1e6);
		}
		CHECK(std::fabs(lc.los.back() - (3500e6 - 0.5 * 0.8 * 50e6)) < 1.0, "full span: last LO %.3f MHz != stop - usable/2", lc.los.back() / 1e6);
		SweepComp fc2(10.000052e6, 3500.000052e6, 50000000u);
		for (size_t k = 0; k < fc2.nhops(); k++) {
			CHECK(fc2.los[k] >= 10.000052e6 && fc2.los[k] <= 3500.000052e6, "float span: LO %zu = %.6f MHz escapes the range", k, fc2.los[k] / 1e6);
		}
		SweepComp nc(1000e6, 1010e6, 50000000u);
		CHECK(nc.nhops() == 1 && nc.los[0] == 1005e6, "narrow span: %zu hops, lo %.1f", nc.nhops(), nc.los[0] / 1e6);
	}

	/* ---- B: control, no table -> the honest dome shows (else this test can't catch a
	 * regression) and the correction gain is exactly 1 ---- */
	{
		SweepComp raw(1000e6, 1400e6, 25000000u);
		CHECK(!raw.resp_on, "control: resp_on with no table loaded");
		for (int s = 0; s < raw.nkeep; s++) { CHECK(raw.gain[s] == 1.0f, "control: gain != 1 with no table"); if (raw.gain[s] != 1.0f) { break; } }
		raw.begin_pass();
		inject_pass(raw, -1, false);
		raw.compose(row, dbMin, dbScale);
		double worst = 0, hm = hump_metric(raw, row, plain, dbMin, dbScale, &worst);
		printf("control (no table): hump metric %.2f dB (worst %.2f)\n", hm, worst);
		CHECK(hm > 2.0, "control: dome not visible, hump metric %.2f dB", hm);
	}

	/* ---- C: table loaded -> flat from pass 1 (connect case, hop 12 dropped), signal and
	 * genuine level step survive, and the correction is IMMOBILE under hostile data ---- */
	CHECK(sweep_resp_load(write_table(25000000u)) == 1, "C: table load failed");
	SweepComp comp(1000e6, 1400e6, 25000000u);
	CHECK(comp.resp_on, "C: curve not adopted at matching bw");
	{
		SweepComp oc(1000e6, 1400e6, 50000000u);
		CHECK(!oc.resp_on, "C: curve adopted at NON-matching bw");
	}
	comp.begin_pass();
	inject_pass(comp, 12, false);
	comp.compose(row, dbMin, dbScale);
	for (int j = 0; j < N_FFT; j++) { CHECK(row[j] != 0, "pass 1: black notch at bin %d", j); if (row[j] == 0) { break; } }
	double worst = 0, hm = hump_metric(comp, row, plain, dbMin, dbScale, &worst);
	printf("pass 1 (connect, table): hump metric %.2f dB (worst %.2f)\n", hm, worst);
	CHECK(std::fabs(hm) < 0.7, "pass 1: hump metric %.2f dB (want ~0)", hm);
	CHECK(std::fabs(worst) < 1.2, "pass 1: worst per-hop hump %.2f dB", worst);
	{
		double mn = 1e9, mx = -1e9;
		const double h = SWEEP_USABLE * 25e6 / 2.0;
		for (size_t k : plain) {
			for (int j = fbin(comp, comp.los[k] - 0.9 * h); j <= fbin(comp, comp.los[k] + 0.9 * h); j++) {
				if (j < 0 || j >= N_FFT) { continue; }
				double db = u8_db(row[j], dbMin, dbScale);
				if (db < mn) { mn = db; }
				if (db > mx) { mx = db; }
			}
		}
		CHECK(mx - mn < 2.0, "pass 1: floor ripple %.2f dB p-p", mx - mn);
		double sig = -1e9;
		for (int j = fbin(comp, comp.los[7] + 200.0 / N_FFT * 25e6) - 4; j <= fbin(comp, comp.los[7] + 240.0 / N_FFT * 25e6) + 4; j++) {
			double db = u8_db(row[j], dbMin, dbScale);
			if (db > sig) { sig = db; }
		}
		CHECK(sig > mn + 25.0, "pass 1: signal flattened away (%.1f dB over floor)", sig - mn);
		double hop0 = zone_db(row, fbin(comp, comp.los[0] - 0.5 * h), fbin(comp, comp.los[0] + 0.5 * h), dbMin, dbScale);
		CHECK(hop0 - (mn + mx) / 2.0 > 3.0, "pass 1: hop-0 +4.8 dB level step eaten (%.2f dB)", hop0 - (mn + mx) / 2.0);
	}
	{
		/* broadband structured world (the #94 poison input): correction must not move, the
		 * world must pass through exactly, flatness holds in the floor hops */
		std::vector<float> g0(comp.gain);
		comp.begin_pass();
		inject_pass(comp, -1, true);
		comp.compose(row, dbMin, dbScale);
		double moved = 0;
		for (int s = 0; s < comp.nkeep; s++) { moved += std::fabs(static_cast<double>(comp.gain[s]) - g0[s]); }
		CHECK(moved == 0.0, "C: correction moved under structured world (sum |dgain| %.3g)", moved);
		const std::vector<size_t> floorh = {1, 2, 3};
		double wf = 0, hf = hump_metric(comp, row, floorh, dbMin, dbScale, &wf);
		CHECK(std::fabs(hf) < 0.7, "C: floor-hop hump %.2f dB under structured world", hf);
		const double h = SWEEP_USABLE * 25e6 / 2.0;
		double tilt = 0;
		for (size_t k : floorh) {
			tilt += zone_db(row, fbin(comp, comp.los[k] + 0.35 * h), fbin(comp, comp.los[k] + 0.85 * h), dbMin, dbScale)
			      - zone_db(row, fbin(comp, comp.los[k] - 0.85 * h), fbin(comp, comp.los[k] - 0.35 * h), dbMin, dbScale);
		}
		tilt /= static_cast<double>(floorh.size());
		printf("structured world (table): floor-hop tilt %.2f dB\n", tilt);
		CHECK(std::fabs(tilt) < 0.5, "C: floor-hop tilt %.2f dB (v1's poison was ~-0.8 here)", tilt);
		double flo = zone_db(row, fbin(comp, comp.los[2] - 0.5 * h), fbin(comp, comp.los[2] + 0.5 * h), dbMin, dbScale);
		double plat = zone_db(row, fbin(comp, comp.los[13] - 0.5 * h), fbin(comp, comp.los[13] + 0.5 * h), dbMin, dbScale);
		CHECK(std::fabs((plat - flo) - 12.5) < 0.5, "C: world step distorted (%.2f dB, want 12.5)", plat - flo);
	}

	/* ---- D: tiny sweep (2 hops): the constant correction needs no minimum hop count ---- */
	{
		SweepComp tc(1000e6, 1040e6, 25000000u);
		CHECK(tc.nhops() == 2, "tiny plan: got %zu hops", tc.nhops());
		CHECK(tc.resp_on, "tiny plan: curve not adopted");
		tc.begin_pass();
		inject_pass(tc, -1, false);
		tc.compose(row, dbMin, dbScale);
		for (int j = 0; j < N_FFT; j++) { CHECK(row[j] != 0, "tiny sweep: black notch at bin %d", j); if (row[j] == 0) { break; } }
		std::vector<size_t> both = {0, 1};
		double w2 = 0, m2 = hump_metric(tc, row, both, dbMin, dbScale, &w2);
		CHECK(std::fabs(m2) < 1.0, "tiny sweep: hump %.2f dB with table", m2);
	}

	/* ---- E: RFNM_SWEEP_NOEQ=1 ignores the table (bench A/B door) ---- */
	{
		putenv(const_cast<char *>("RFNM_SWEEP_NOEQ=1"));
		SweepComp nc(1000e6, 1400e6, 25000000u);
		CHECK(!nc.resp_on, "NOEQ: table still applied");
		unsetenv("RFNM_SWEEP_NOEQ");
	}

	/* ---- F: real FFT path (add_hop), white-noise IQ, no table -> flat (no correction to
	 * disturb a flat input) ---- */
	g_sweep_resp.clear();
	{
		SweepComp fc(1000e6, 1400e6, 25000000u);
		std::vector<int16_t> iq(static_cast<size_t>(N_FFT) * 2);
		const float fdbMin = 40.0f, fdbScale = 255.0f / 100.0f;
		std::vector<size_t> all;
		for (size_t k = 0; k < fc.nhops(); k++) { all.push_back(k); }
		uint8_t frow[N_FFT];
		for (int pass = 1; pass <= 3; pass++) {
			fc.begin_pass();
			for (size_t k = 0; k < fc.nhops(); k++) {
				for (int i = 0; i < 2 * N_FFT; i++) { iq[i] = static_cast<int16_t>((frnd() + frnd() + frnd() + frnd() - 2.0f) * 500.0f); }
				fc.add_hop(k, iq.data());
			}
			fc.compose(frow, fdbMin, fdbScale);
			for (int j = 0; j < N_FFT; j++) { CHECK(frow[j] != 0, "fft pass %d: black notch at bin %d", pass, j); if (frow[j] == 0) { break; } }
			double w = 0, m = hump_metric(fc, frow, all, fdbMin, fdbScale, &w);
			CHECK(std::fabs(m) < 1.0, "fft pass %d: hump metric %.2f dB", pass, m);
		}
	}

	/* ---- G: per-hop sweep-AGC gain normalization — hops captured at different RF gains must
	 * compose at the REFERENCE-gain level (display honesty), with the table applied ---- */
	CHECK(sweep_resp_load(write_table(25000000u)) == 1, "G: table reload failed");
	{
		SweepComp rc(1000e6, 1400e6, 25000000u);
		rc.begin_pass();
		uint32_t seed_save = g_seed;
		inject_pass(rc, -1, false);
		uint8_t rrow[N_FFT];
		rc.compose(rrow, dbMin, dbScale);

		SweepComp mc(1000e6, 1400e6, 25000000u);
		std::vector<int> delta(mc.nhops(), 0);
		delta[3] = -18;
		delta[9] = 12;
		mc.begin_pass();
		g_seed = seed_save;                                      /* identical noise draw as the baseline */
		{
			std::vector<float> pw(mc.nkeep);
			for (size_t k = 0; k < mc.nhops(); k++) {
				mc.set_hop_gain_delta(k, static_cast<float>(delta[k]));
				float lin = powf(10.0f, static_cast<float>(delta[k]) / 10.0f);
				for (int s = 0; s < mc.nkeep; s++) {
					int d = s - mc.half_use;
					float x = static_cast<float>(d) / static_cast<float>(mc.half_use);
					float v = 1e6f * powf(10.0f, dome_db_at(x) / 10.0f) * (0.85f + 0.3f * frnd());
					if (k == 0) { v *= 3.0f; }
					if (k == 7 && d >= 200 && d <= 240) { v *= 1000.0f; }
					pw[s] = v * lin;
				}
				mc.add_hop_pow(k, pw.data());
			}
		}
		uint8_t mrow[N_FFT];
		mc.compose(mrow, dbMin, dbScale);
		const double h = SWEEP_USABLE * 25e6 / 2.0;
		for (size_t k = 0; k < mc.nhops(); k++) {
			double zr = zone_db(rrow, fbin(rc, rc.los[k] - 0.5 * h), fbin(rc, rc.los[k] + 0.5 * h), dbMin, dbScale);
			double zm = zone_db(mrow, fbin(mc, mc.los[k] - 0.5 * h), fbin(mc, mc.los[k] + 0.5 * h), dbMin, dbScale);
			CHECK(std::fabs(zm - zr) < 0.5, "agc norm: hop %zu (delta %+d dB) level moved %.2f dB vs baseline", k, delta[k], zm - zr);
		}
		double w2 = 0, m2 = hump_metric(mc, mrow, plain, dbMin, dbScale, &w2);
		CHECK(std::fabs(m2) < 0.7, "agc norm: hump metric %.2f dB under mixed gains (table on)", m2);
	}

	if (g_fail) { printf("sweep_eq_test: %d FAILURE(S)\n", g_fail); return 1; }
	printf("sweep_eq_test: all checks passed\n");
	return 0;
}
