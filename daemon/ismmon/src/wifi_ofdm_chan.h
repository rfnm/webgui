// ismmon: OFDM-local channelizer. The shared BLE/DSSS ddc (channelizer.h) uses a short 4*decim+1-tap
// Hamming FIR that is fine for the narrow BLE/DSSS bands but rolls off across the 802.11 OFDM edge
// subcarriers (±8.125 MHz), degrading the L-LTF correlation (det_q ~0.6 on real frames). This is a
// longer, sharper windowed-sinc decimator tuned to pass the whole OFDM band flat. Batch (offline
// analysis + the daemon's rate-limited Wi-Fi scan); scalar — not on any continuous hot path.
#pragma once

#include <cmath>
#include <vector>

#include "dsp.h"

namespace ismmon {

// Mix a channel at off_hz to baseband and decimate in_rate->out_rate with a taps_mult*decim+1-tap
// Blackman-Harris windowed-sinc lowpass. cutoff_hz = single-sided passband edge (Hz).
static inline void channelize_ofdm(const cf* in, size_t n, double off_hz, double in_rate, double out_rate,
                                   double cutoff_hz, int taps_mult, std::vector<cf>& out) {
	int decim = (int)std::llround(in_rate / out_rate);
	int taps = (taps_mult * decim) | 1;
	std::vector<float> h(taps);
	double fc = cutoff_hz / in_rate;   // normalized cutoff (of in_rate)
	double sum = 0;
	for(int i = 0; i < taps; i++) {
		double m = i - (taps - 1) / 2.0;
		double s = (m == 0) ? 2.0 * fc : std::sin(2.0 * M_PI * fc * m) / (M_PI * m);
		double w = 0.35875 - 0.48829 * std::cos(2 * M_PI * i / (taps - 1))
		         + 0.14128 * std::cos(4 * M_PI * i / (taps - 1)) - 0.01168 * std::cos(6 * M_PI * i / (taps - 1));
		h[i] = (float)(s * w);
		sum += h[i];
	}
	for(auto& t : h) { t = (float)(t / sum); }

	// incremental NCO down-mix (renormalized periodically to hold precision over millions of samples)
	std::vector<cf> mix(n);
	double w0 = -2.0 * M_PI * off_hz / in_rate;
	cf ph(1.f, 0.f), rot((float)std::cos(w0), (float)std::sin(w0));
	for(size_t i = 0; i < n; i++) {
		mix[i] = in[i] * ph;
		ph *= rot;
		if((i & 1023) == 1023) { float mg = std::abs(ph); if(mg > 0) { ph /= mg; } }
	}
	// FIR + decimate
	out.clear();
	out.reserve(n / decim + 1);
	for(size_t p = (size_t)taps; p + decim < n; p += decim) {
		cf acc(0, 0);
		for(int t = 0; t < taps; t++) { acc += mix[p - t] * h[t]; }
		out.push_back(acc);
	}
}

} // namespace ismmon
