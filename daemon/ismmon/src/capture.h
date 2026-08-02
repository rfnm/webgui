// ismmon: local-transport RX capture (aisd/lorad "as-flown" sequence). Delivers interleaved CS16
// blocks (the GPU FFT reads CS16 directly; the IQ history ring / demod convert per burst).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace ismmon {

// cb receives interleaved CS16: `n` complex samples = 2*n int16 values at `iq`.
// agc nonzero enables the device's hardware AGC (gain_db stays the starting gain).
// Returns false on open/apply/stream failure or a 30 s dry-read (device wedge).
bool run_capture(double center_hz, double rate_hz, int gain_db, int agc, const std::string& antenna,
                 std::atomic<bool>& stop, std::atomic<uint64_t>& samples,
                 const std::function<void(const int16_t* iq, size_t n)>& cb);

} // namespace ismmon
