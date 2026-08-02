// ismmon: NCO + FIR decimator (lifted from aisd/lorad). One instance shifts a BLE/Wi-Fi channel
// from the capture center to baseband and decimates the capture rate down to the per-channel demod
// rate. The FIR only computes at the output rate. The down-mix and FIR are NEON-optimized on
// AArch64 with a scalar fallback that is the numeric reference (bit-identical, what x86 runs).
// in_rate/out_rate must divide evenly (integer decim): 76.8 MSPS -> 2.4 MHz = decim 32.
#pragma once

#include <cmath>
#include <vector>

#include "dsp.h"

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace ismmon {

class ddc {
public:
	// offset_hz is the channel offset from the capture center (positive == above center);
	// cutoff_frac scales the lowpass cutoff (of out_rate) - BLE-1M occupies ~1.1 MHz so at
	// out_rate 2.4 MHz the default 0.45 (~1.08 MHz single-sided) passes it.
	ddc(double in_rate, double out_rate, double offset_hz, double cutoff_frac = 0.45) {
		decim_ = (int)llround(in_rate / out_rate);
		phase_ = cf(1.0f, 0.0f);
		double w = -2.0 * M_PI * offset_hz / in_rate;
		rot_ = cf((float)cos(w), (float)sin(w));
		rot2_ = rot_ * rot_;
		rot3_ = rot2_ * rot_;
		rot4_ = rot2_ * rot2_;

		int taps = 4 * decim_ | 1;
		taps_.resize(taps);
		double fc = cutoff_frac * out_rate / in_rate;
		double sum = 0;
		for(int i = 0; i < taps; i++) {
			double m = i - (taps - 1) / 2.0;
			double s = m == 0 ? 2.0 * fc : sin(2.0 * M_PI * fc * m) / (M_PI * m);
			double win = 0.54 - 0.46 * cos(2.0 * M_PI * i / (taps - 1));
			taps_[i] = (float)(s * win);
			sum += taps_[i];
		}
		for(auto& t : taps_) { t = (float)(t / sum); }
		nt_ = (int)taps_.size();
		buf_.assign((size_t)nt_ * 2, cf(0, 0));
		w_ = nt_ - 1;
	}

	void process(const cf* in, size_t count, std::vector<cf>& out) {
		size_t i = 0;
#if defined(__aarch64__) || defined(__ARM_NEON)
		if(count >= 4) {
			float pr[4] = { phase_.real(), (phase_ * rot_).real(), (phase_ * rot2_).real(), (phase_ * rot3_).real() };
			float pi[4] = { phase_.imag(), (phase_ * rot_).imag(), (phase_ * rot2_).imag(), (phase_ * rot3_).imag() };
			float32x4_t phr = vld1q_f32(pr), phi = vld1q_f32(pi);
			float32x4_t r4r = vdupq_n_f32(rot4_.real()), r4i = vdupq_n_f32(rot4_.imag());
			for(; i + 4 <= count; i += 4) {
				float32x4x2_t v = vld2q_f32(reinterpret_cast<const float*>(in + i));
				float32x4_t xr = vsubq_f32(vmulq_f32(v.val[0], phr), vmulq_f32(v.val[1], phi));
				float32x4_t xi = vaddq_f32(vmulq_f32(v.val[0], phi), vmulq_f32(v.val[1], phr));
				float xrs[4], xis[4];
				vst1q_f32(xrs, xr);
				vst1q_f32(xis, xi);
				for(int k = 0; k < 4; k++) { push_sample(cf(xrs[k], xis[k]), out); }
				float32x4_t nphr = vsubq_f32(vmulq_f32(phr, r4r), vmulq_f32(phi, r4i));
				float32x4_t nphi = vaddq_f32(vmulq_f32(phr, r4i), vmulq_f32(phi, r4r));
				phr = nphr;
				phi = nphi;
			}
			phase_ = cf(vgetq_lane_f32(phr, 0), vgetq_lane_f32(phi, 0));
		}
#endif
		for(; i < count; i++) {
			cf x = in[i] * phase_;
			phase_ *= rot_;
			push_sample(x, out);
		}
		float m = std::abs(phase_);
		if(m > 0) { phase_ /= m; }
	}

private:
	inline void push_sample(cf x, std::vector<cf>& out) {
		buf_[w_] = x;
		buf_[w_ + nt_] = x;
		if(++dphase_ == decim_) {
			dphase_ = 0;
			out.push_back(fir_dot(&buf_[w_]));
		}
		w_ = w_ == 0 ? nt_ - 1 : w_ - 1;
	}

	inline cf fir_dot(const cf* win) const {
#if defined(__aarch64__) || defined(__ARM_NEON)
		float32x4_t ar = vdupq_n_f32(0), ai = vdupq_n_f32(0);
		const float* wf = reinterpret_cast<const float*>(win);
		int t = 0;
		for(; t + 4 <= nt_; t += 4) {
			float32x4x2_t v = vld2q_f32(wf + 2 * t);
			float32x4_t tp = vld1q_f32(&taps_[t]);
			ar = vmlaq_f32(ar, v.val[0], tp);
			ai = vmlaq_f32(ai, v.val[1], tp);
		}
		float32x2_t sr = vadd_f32(vget_low_f32(ar), vget_high_f32(ar));
		float32x2_t si = vadd_f32(vget_low_f32(ai), vget_high_f32(ai));
		float re = vget_lane_f32(vpadd_f32(sr, sr), 0);
		float im = vget_lane_f32(vpadd_f32(si, si), 0);
		for(; t < nt_; t++) {
			re += win[t].real() * taps_[t];
			im += win[t].imag() * taps_[t];
		}
		return cf(re, im);
#else
		float re = 0, im = 0;
		for(int t = 0; t < nt_; t++) {
			re += win[t].real() * taps_[t];
			im += win[t].imag() * taps_[t];
		}
		return cf(re, im);
#endif
	}

	int decim_ = 1;
	int dphase_ = 0;
	int nt_ = 0;
	cf phase_, rot_, rot2_, rot3_, rot4_;
	std::vector<float> taps_;
	std::vector<cf> buf_;
	int w_ = 0;
};

} // namespace ismmon
