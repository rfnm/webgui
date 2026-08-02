// ismmon: per-channel burst detector. Consumes the FFT-magnitude rows the spectrum engine already
// produces (u8-dB, one value per bin, fftshifted so bin 0 = lowest freq) and flags bursts where a
// channel's band power rises above its rolling noise floor. Each completed burst -> (channel,
// row_start, row_end); the daemon maps rows to sample times and pulls the raw IQ span from the
// history ring for DDC + demod. FFT-source-agnostic: works on GPU or CPU rows identically.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace ismmon {

// Peak (max) u8 value over row[lo..hi] inclusive. NEON on aarch64 (bit-identical to the scalar max -
// max is exact, no float); runs per channel on EVERY FFT row in the live daemon, so it's genuinely hot.
static inline int detector_band_peak(const uint8_t* row, int lo, int hi) {
	int band = 0;
	int b = lo;
#if defined(__aarch64__) || defined(__ARM_NEON)
	// Two independent max accumulators (32 bytes/iter) hide load latency on the in-order A53; one
	// horizontal reduce at the end. max is associative + exact -> bit-identical to the scalar max.
	uint8x16_t vm0 = vdupq_n_u8(0), vm1 = vdupq_n_u8(0);
	for(; b + 32 <= hi + 1; b += 32) { vm0 = vmaxq_u8(vm0, vld1q_u8(row + b)); vm1 = vmaxq_u8(vm1, vld1q_u8(row + b + 16)); }
	for(; b + 16 <= hi + 1; b += 16) { vm0 = vmaxq_u8(vm0, vld1q_u8(row + b)); }
	band = (int)vmaxvq_u8(vmaxq_u8(vm0, vm1));
#endif
	for(; b <= hi; b++) { if(row[b] > band) { band = row[b]; } }
	return band;
}

// A channel of interest = a contiguous bin range in the u8-dB row (inclusive).
struct ChannelBand {
	int id;          // logical id (e.g. BLE channel index 37/38/39, or a Wi-Fi channel number)
	int bin_lo;
	int bin_hi;
};

struct BurstEvent {
	int channel_id;
	uint64_t row_start;   // first row above threshold
	uint64_t row_end;     // last row above threshold (inclusive)
	float peak_db;        // peak band level (u8 units) during the burst
	float floor_db;       // noise floor estimate at trigger time (u8 units)
};

class Detector {
public:
	// margin_u8: band must exceed floor by this (u8-dB units) to trigger. hang_rows: rows below
	// threshold tolerated before closing a burst. max_rows: force-close an over-long burst.
	Detector(std::vector<ChannelBand> chans, float margin_u8 = 18.0f, int hang_rows = 2,
	         uint64_t max_rows = 64)
		: chans_(std::move(chans)), margin_(margin_u8), hang_(hang_rows), max_rows_(max_rows) {
		st_.resize(chans_.size());
	}

	// Close any open burst (call at a time discontinuity, e.g. a dropped FFT batch, so a burst never
	// spans the gap). Keeps the per-channel noise floor.
	void flush(const std::function<void(const BurstEvent&)>& on_burst) {
		for(size_t c = 0; c < chans_.size(); c++) {
			ChState& s = st_[c];
			if(s.active) { emit(chans_[c].id, s, s.last_hot, on_burst); s.active = false; }
		}
	}

	// Feed one u8-dB FFT row (nbins values). Calls on_burst for each burst that completes on this row.
	void feed_row(uint64_t row_idx, const uint8_t* row, int nbins,
	              const std::function<void(const BurstEvent&)>& on_burst) {
		for(size_t c = 0; c < chans_.size(); c++) {
			const ChannelBand& cb = chans_[c];
			ChState& s = st_[c];
			int lo = cb.bin_lo < 0 ? 0 : cb.bin_lo;
			int hi = cb.bin_hi >= nbins ? nbins - 1 : cb.bin_hi;
			if(lo > hi) { continue; }
			int band = detector_band_peak(row, lo, hi);   // peak-in-band (NEON on aarch64)

			if(!s.have_floor) { s.floor = (float)band; s.have_floor = true; }
			float thresh = s.floor + margin_;

			if(!s.active) {
				if((float)band > thresh) {
					s.active = true; s.start = row_idx; s.peak = (float)band;
					s.trig_floor = s.floor; s.hang = 0;
				} else {
					s.floor += 0.02f * ((float)band - s.floor);   // track noise floor only while idle
				}
			} else {
				if((float)band > s.peak) { s.peak = (float)band; }
				if((float)band > thresh) {
					s.last_hot = row_idx; s.hang = 0;
				} else if(++s.hang > hang_) {
					emit(cb.id, s, row_idx, on_burst);
					s.active = false;
				}
				if(row_idx - s.start >= max_rows_) {   // force-close runaway (e.g. continuous Wi-Fi)
					emit(cb.id, s, row_idx, on_burst);
					s.active = false;
				}
			}
		}
	}

private:
	struct ChState {
		bool have_floor = false, active = false;
		float floor = 0, trig_floor = 0, peak = 0;
		uint64_t start = 0, last_hot = 0;
		int hang = 0;
	};
	void emit(int id, ChState& s, uint64_t row_idx, const std::function<void(const BurstEvent&)>& cb) {
		BurstEvent e;
		e.channel_id = id;
		e.row_start = s.start;
		e.row_end = s.last_hot ? s.last_hot : row_idx;
		e.peak_db = s.peak;
		e.floor_db = s.trig_floor;
		cb(e);
	}
	std::vector<ChannelBand> chans_;
	float margin_;
	int hang_;
	uint64_t max_rows_;
	std::vector<ChState> st_;
};

} // namespace ismmon
