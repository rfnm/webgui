/* sweep_comp.h — shared sweep composition + per-hop passband correction.
 *
 * Used by both spectrumd sweep engines (drain + schedule_rx): they capture N_FFT samples per
 * hop and hand them here; SweepComp owns the hop plan and the per-hop power spectra for one
 * pass, and composes one corrected full-span u8 row.
 *
 * The per-hop RX passband is NOT flat across the kept SWEEP_USABLE band — the decimation
 * chain's transition starts right at +-0.4 Fs (the RFIC LPF's in-band share is ~0.2 dB,
 * measured) — so a raw composite shows one ~3 dB dome per hop. That dome is a
 * CONSTANT of the rate plan, so the correction is a constant: a per-bw curve loaded from
 * a table (sweep_resp_load below), minted OFFLINE from a RAW grid-shifted ladder
 * (pair-difference solver - the ambient world cancels per absolute bin).
 * No table for the armed bw = no correction (the honest dome shows). An in-daemon
 * self-cal episode existed briefly and was deliberately deleted:
 * user-visible benefit ~nil over one fleet table (board-to-board curve spread measured
 * 0.13 dB rms), and a public cal-rewriting endpoint is the wrong surface for a
 * manufacturing operation. A decimator/eld change re-runs the table mint.
 *
 * There is deliberately NO live estimator here. The v1 equalizer re-fitted the "passband
 * shape" every pass as a cross-hop per-offset MEDIAN of the received data; R(offset) and
 * world(f) are confounded in non-overlapping hop data (each abs frequency is seen at ONE
 * offset), so with broadband ambient occupancy the median sampled the WORLD's slope and
 * stamped its inverse onto every hop — a placement-dependent 40 MHz sawtooth (measured:
 * artifact 1.45 dB and 2.0 dB fc-spread vs 0.35/0.60 for the constant curve). Correcting
 * a compile-time constant with a runtime fit of unidentifiable data was the defect; do not
 * reintroduce one. Adaptivity, when wanted, belongs in an identifiable calibration episode
 * (overlapped hops: every f seen at two offsets, the world cancels per-bin), whose output
 * is frozen into the same table this header loads.
 *
 * Self-contained on purpose so the math is host-unit-testable (tests/sweep_eq_test.cpp):
 * the includer must define N_FFT and cw_convert() before this header. */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include "pffft.h"
}

#define SWEEP_USABLE      0.80f       /* central fraction of each hop kept (drop the LPF skirts) */
#define SWEEP_DC_GUARD_HZ 80000.0     /* drop +-this around each hop LO (kill the DC/LO leakage spike) */
#define SWEEP_EQ_SEG      64          /* response-curve segments across the kept band (the shape is smooth) */
#define SWEEP_EQ_CLAMP_DB 12.0f      /* table sanity bound: the real dome is ~3 dB */

/* Factory-calibration correction table (.tbl format: rows C(f) on the factory grid,
 * decomposed against the factory loop/pwr0 planes). Rows are C(f) =
 * loop - pwr0 on the factory grid, keyed by the era FE leg. v1 applies the ZERO-MEDIAN
 * shape (flatness only - the absolute anchor waits on the DELTA(f) reference measurement, which
 * arrives as a data update in the same file). The equalizer above corrects the per-hop
 * passband dome from the data itself; this corrects the cross-hop FE shape from the
 * factory tables - independent, multiplicative, no interaction.
 * Interpolation NEVER crosses a leg boundary (the response is discontinuous there):
 * bracketing rows of different legs fall back to nearest-row. Island rows ship excluded
 * (dropped at load; the gap interpolates between its wideband-leg neighbors). */
struct SweepCalRow { float f_mhz, c_db; int leg; };
struct SweepCalTable {
	std::vector<SweepCalRow> rows;      /* sorted by f, excluded rows dropped, c median-normalized */
	char serial[16] = {0};
	bool loaded = false;
};
inline SweepCalTable g_sweep_cal;

/* parse the .tbl (header lines + "ROW f c leg excluded"); returns rows kept */
inline int sweep_cal_load(const char *path) {
	FILE *fh = fopen(path, "r");
	if (!fh) { return 0; }
	char line[128];
	std::vector<float> cs;
	while (fgets(line, sizeof(line), fh)) {
		float f, c;
		int leg, exc;
		if (sscanf(line, "ROW %f %f %d %d", &f, &c, &leg, &exc) == 4) {
			if (exc) { continue; }
			g_sweep_cal.rows.push_back({f, c, leg});
			cs.push_back(c);
		} else {
			sscanf(line, "SERIAL %15s", g_sweep_cal.serial);
		}
	}
	fclose(fh);
	if (cs.size() < 2) { g_sweep_cal.rows.clear(); return 0; }
	std::nth_element(cs.begin(), cs.begin() + static_cast<long>(cs.size() / 2), cs.end());
	const float med = cs[cs.size() / 2];
	for (auto &r : g_sweep_cal.rows) { r.c_db -= med; }
	std::sort(g_sweep_cal.rows.begin(), g_sweep_cal.rows.end(),
			  [](const SweepCalRow &x, const SweepCalRow &y) { return x.f_mhz < y.f_mhz; });
	g_sweep_cal.loaded = true;
	return static_cast<int>(g_sweep_cal.rows.size());
}

/* correction (dB, median-normalized) at absolute frequency f_mhz */
inline float sweep_cal_db(float f_mhz) {
	const auto &rs = g_sweep_cal.rows;
	if (rs.empty()) { return 0.0f; }
	if (f_mhz <= rs.front().f_mhz) { return rs.front().c_db; }      /* hold edges */
	if (f_mhz >= rs.back().f_mhz) { return rs.back().c_db; }
	size_t hi = 1;
	while (rs[hi].f_mhz < f_mhz) { hi++; }
	const SweepCalRow &r0 = rs[hi - 1], &r1 = rs[hi];
	if (r0.leg != r1.leg) {                                          /* never interp across a leg edge */
		return (f_mhz - r0.f_mhz < r1.f_mhz - f_mhz) ? r0.c_db : r1.c_db;
	}
	float t = (f_mhz - r0.f_mhz) / (r1.f_mhz - r0.f_mhz);
	return r0.c_db + t * (r1.c_db - r0.c_db);
}

/* Per-rate static response curves. File: blocks of "BW <hz>" + SWEEP_EQ_SEG lines of dB
 * (segment centers, low->high offset across the kept band); zero-median enforced on load
 * (flatness only, never a level shift). Ship path /rfnm/spectrum/resp.tbl beside the cal
 * table; RFNM_SWEEP_RESP overrides for manual testing. */
struct SweepResp { uint32_t bw; float db[SWEEP_EQ_SEG]; };
inline std::vector<SweepResp> g_sweep_resp;

inline int sweep_resp_load(const char *path) {
	FILE *fh = fopen(path, "r");
	if (!fh) { return 0; }
	char line[128];
	SweepResp cur;
	int nseg = -1;
	while (fgets(line, sizeof(line), fh)) {
		uint32_t bw;
		float v;
		if (sscanf(line, "BW %u", &bw) == 1) {
			if (nseg == SWEEP_EQ_SEG) { g_sweep_resp.push_back(cur); }
			cur.bw = bw;
			nseg = 0;
		} else if (nseg >= 0 && nseg < SWEEP_EQ_SEG && sscanf(line, "%f", &v) == 1) {
			cur.db[nseg++] = v;
		}
	}
	if (nseg == SWEEP_EQ_SEG) { g_sweep_resp.push_back(cur); }
	fclose(fh);
	for (auto &r : g_sweep_resp) {   /* zero-median: flatness only, never a level shift */
		float tmp[SWEEP_EQ_SEG];
		memcpy(tmp, r.db, sizeof(tmp));
		std::nth_element(tmp, tmp + SWEEP_EQ_SEG / 2, tmp + SWEEP_EQ_SEG);
		const float med = tmp[SWEEP_EQ_SEG / 2];
		for (int g = 0; g < SWEEP_EQ_SEG; g++) { r.db[g] -= med; }
	}
	return static_cast<int>(g_sweep_resp.size());
}

struct SweepComp {
	double a, span, center;
	uint32_t bw;
	std::vector<double> los;             /* hop LO ladder */
	int half_use, guard, nkeep;
	bool eq_on;                          /* RFNM_SWEEP_NOEQ=1 ignores the response table (bench A/B) */
	bool resp_on;                        /* a curve matching this bw is loaded and applied */
	PFFFT_Setup *setup;
	float *win, *fin, *fout, *work;
	std::vector<float> hoppow;           /* nhops x nkeep kept |X|^2, row-major (worst case ~18 MB at 3.125M full span) */
	std::vector<uint8_t> hopok;
	std::vector<float> hopnorm;          /* per-hop linear gain normalization (sweep AGC): a hop captured at
	                                      * ref+delta dB RF gain stores its power scaled by 10^(-delta/10), so
	                                      * every consumer downstream (compose, cal) sees the REFERENCE-gain
	                                      * level — display stays absolute across mixed-gain passes. 1.0 = no AGC. */
	std::vector<float> gain;             /* per-fine-bin linear response correction — a CONSTANT, built once
	                                      * in the ctor from the loaded curve (never from live data, #94) */
	std::vector<double> acc;             /* linear |X|^2 accumulated per output bin */
	std::vector<uint32_t> cnt;           /* fine bins that landed in each output bin */
	std::vector<float> calgain;          /* per-OUTPUT-bin factory-cal linear gain (absolute f), 1.0 = off */
	bool cal_on = false;

	SweepComp(double a_, double b_, uint32_t bw_) : a(a_), bw(bw_) {
		double usable = SWEEP_USABLE * static_cast<double>(bw);
		center = (a_ + b_) / 2.0;
		span = b_ - a_;
		/* clamp every LO inside [a,b] (the caller clamps a/b to the tunable band): the raw
		 * ladder places the last hop's center up to usable/2 PAST the stop to cover the tail,
		 * which walks the LO out of the band - a full-span Lime sweep commanded LO 3510 on a
		 * 3500-max device, a deterministic reject + hold-filled top edge on every pass.
		 * Overlapping the previous hop is fine: compose() averages by absolute frequency. */
		double lo_min = a_ + usable / 2.0, lo_max = b_ - usable / 2.0;
		if (lo_max < lo_min) { lo_min = lo_max = center; }   /* span narrower than one hop: single centered hop */
		for (double f = lo_min; f - usable / 2.0 < b_; f += usable) {
			los.push_back(f < lo_max ? f : lo_max);
			if (f >= lo_max) { break; }
		}
		if (los.empty()) { los.push_back(center); }
		half_use = static_cast<int>(SWEEP_USABLE * N_FFT / 2.0);   /* fftshifted bins kept each side of DC */
		guard = static_cast<int>(SWEEP_DC_GUARD_HZ * N_FFT / static_cast<double>(bw));   /* LO-leakage bins each side of DC */
		nkeep = 2 * half_use;
		eq_on = getenv("RFNM_SWEEP_NOEQ") == nullptr;
		setup = pffft_new_setup(N_FFT, PFFFT_COMPLEX);
		win = static_cast<float *>(pffft_aligned_malloc(N_FFT * 4));
		fin = static_cast<float *>(pffft_aligned_malloc(N_FFT * 8));
		fout = static_cast<float *>(pffft_aligned_malloc(N_FFT * 8));
		work = static_cast<float *>(pffft_aligned_malloc(N_FFT * 8));
		for (int i = 0; i < N_FFT; i++) { win[i] = 0.5f * (1.0f - cosf(2.0f * static_cast<float>(M_PI) * i / (N_FFT - 1))); }
		hoppow.resize(los.size() * static_cast<size_t>(nkeep));
		hopok.resize(los.size());
		hopnorm.assign(los.size(), 1.0f);
		gain.assign(nkeep, 1.0f);
		acc.resize(N_FFT);
		cnt.resize(N_FFT);
		/* per-fine-bin response correction from the loaded curve for this bw: clamp each
		 * segment, linear-interp segment centers to bins, gain = 10^(-db/10). Built ONCE -
		 * whatever error the curve has is the SAME error at every sweep placement. */
		resp_on = false;
		if (eq_on) {
			for (const auto &r : g_sweep_resp) {
				if (r.bw != bw_) { continue; }
				float corr[SWEEP_EQ_SEG];
				for (int g = 0; g < SWEEP_EQ_SEG; g++) {
					float v = r.db[g];
					if (v > SWEEP_EQ_CLAMP_DB) { v = SWEEP_EQ_CLAMP_DB; } else if (v < -SWEEP_EQ_CLAMP_DB) { v = -SWEEP_EQ_CLAMP_DB; }
					corr[g] = v;
				}
				for (int s = 0; s < nkeep; s++) {
					float x = (static_cast<float>(s) + 0.5f) * SWEEP_EQ_SEG / static_cast<float>(nkeep) - 0.5f;   /* segment-center coordinates */
					int i0 = static_cast<int>(x);
					if (x < 0.0f) { i0 = 0; x = 0.0f; } else if (i0 > SWEEP_EQ_SEG - 2) { i0 = SWEEP_EQ_SEG - 2; x = static_cast<float>(SWEEP_EQ_SEG - 1); }
					float t = x - static_cast<float>(i0);
					float cdb = corr[i0] + t * (corr[i0 + 1] - corr[i0]);
					gain[s] = expf(-0.2302585093f * cdb);      /* 10^(-cdb/10) */
				}
				resp_on = true;
				break;
			}
		}
		/* factory-cal per-output-bin gains: output bin j sits at absolute f = a + j/(N-1)*span
		 * regardless of hop layout, so the correction is exact per bin and hop-independent */
		cal_on = g_sweep_cal.loaded && getenv("RFNM_SWEEP_NOCAL") == nullptr;
		if (cal_on) {
			calgain.resize(N_FFT);
			for (int j = 0; j < N_FFT; j++) {
				float f_mhz = static_cast<float>((a + static_cast<double>(j) / (N_FFT - 1) * span) / 1e6);
				calgain[j] = expf(-0.2302585093f * sweep_cal_db(f_mhz));   /* 10^(-c/10) */
			}
		}
	}
	~SweepComp() {
		pffft_aligned_free(win); pffft_aligned_free(fin); pffft_aligned_free(fout); pffft_aligned_free(work);
		pffft_destroy_setup(setup);
	}
	SweepComp(const SweepComp &) = delete;
	SweepComp &operator=(const SweepComp &) = delete;

	size_t nhops() const { return los.size(); }

	void begin_pass() { std::fill(hopok.begin(), hopok.end(), 0); }

	/* sweep AGC: hop k's NEXT capture runs at ref+delta dB RF gain — must be set BEFORE the
	 * capture is added so its stored power lands normalized back to the reference gain */
	void set_hop_gain_delta(size_t k, float delta_db) { hopnorm[k] = expf(-0.2302585093f * delta_db); }   /* 10^(-delta/10) */

	/* FFT one hop's N_FFT CS16 samples, store the kept (fftshifted central) power bins */
	void add_hop(size_t k, const int16_t *iq) {
		cw_convert(iq, win, fin, N_FFT);
		pffft_transform_ordered(setup, fin, fout, work, PFFFT_FORWARD);
		const int c = N_FFT / 2;
		const float nrm = hopnorm[k];
		float *hp = hoppow.data() + k * static_cast<size_t>(nkeep);
		for (int m = c - half_use; m < c + half_use; m++) {
			int i = m ^ (N_FFT / 2);                       /* fftshifted m -> raw pffft bin */
			float re = fout[2 * i], im = fout[2 * i + 1];
			hp[m - (c - half_use)] = (re * re + im * im) * nrm;
		}
		hopok[k] = 1;
	}

	/* inject a hop's kept power spectrum directly (host unit test seam; same normalization) */
	void add_hop_pow(size_t k, const float *pw) {
		float *hp = hoppow.data() + k * static_cast<size_t>(nkeep);
		const float nrm = hopnorm[k];
		for (int s = 0; s < nkeep; s++) { hp[s] = pw[s] * nrm; }
		hopok[k] = 1;
	}

	/* composite this pass into one full-span u8 row: response-correct (constant gain[]),
	 * power-detect, dB->u8 (same fast-log2 + fp16 scale offset as cw_mag_u8 so sweep
	 * quantization matches the real-time engines'), hold-fill seam/guard/dropped-hop gaps */
	void compose(uint8_t *rowbuf, float dbMin, float dbScale) {
		std::fill(acc.begin(), acc.end(), 0.0);
		std::fill(cnt.begin(), cnt.end(), 0u);
		for (size_t k = 0; k < los.size(); k++) {
			if (!hopok[k]) { continue; }                   /* short/dropped hop: hold-fill covers it */
			const float *hp = hoppow.data() + k * static_cast<size_t>(nkeep);
			for (int s = 0; s < nkeep; s++) {
				int d = s - half_use;
				if (d >= -guard && d <= guard) { continue; }   /* skip the LO/DC leakage */
				double f = los[k] + static_cast<double>(d) / N_FFT * static_cast<double>(bw);
				int j = static_cast<int>(llround((f - a) / span * (N_FFT - 1)));
				if (j >= 0 && j < N_FFT) { acc[j] += static_cast<double>(hp[s]) * gain[s]; cnt[j]++; }
			}
		}
		memset(rowbuf, 0, N_FFT);
		for (int j = 0; j < N_FFT; j++) {
			if (!cnt[j]) { continue; }
			float mag2 = static_cast<float>(acc[j] / cnt[j]) * (cal_on ? calgain[j] : 1.0f) + 1e-12f;
			int e;
			float mm = frexpf(mag2, &e);
			float db = 3.0102999566f * (static_cast<float>(e) + 2.0f * (mm - 0.75f));
			float v = (db - dbMin) * dbScale;
			rowbuf[j] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
		}
		/* hold-fill the occasional empty bin at a hop seam (no fine bin rounded into it) so the
		 * trace has no black notches; forward-hold then back-fill any leading run */
		int last = -1;
		for (int j = 0; j < N_FFT; j++) {
			if (cnt[j]) { last = rowbuf[j]; }
			else if (last >= 0) { rowbuf[j] = static_cast<uint8_t>(last); }
		}
		int f = 0;
		while (f < N_FFT && !cnt[f]) { f++; }
		for (int j = 0; j < f && f < N_FFT; j++) { rowbuf[j] = rowbuf[f]; }
	}
};
