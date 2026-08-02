// ismmon 802.11n HT (high-throughput, mixed format) support. The HT receiver lives inside wifi.cpp's
// wifi_decode_burst (it reuses the OFDM front-end: L-STF/L-LTF sync, channel est, FFT, Viterbi). This
// header exposes only the HT MODULATOR used by the selftest to synthesize a 20 MHz, 1-spatial-stream
// HT frame (L-STF/L-LTF/L-SIG + HT-SIG + HT-STF + HT-LTF + HT-DATA) at a given MCS.
#pragma once

#include <cstdint>
#include <vector>

#include "dsp.h"
#include "wifi.h"

namespace ismmon {

// Synthesize an 802.11n HT-mixed-format frame carrying `psdu` at `mcs` (0..7, single stream, 20 MHz).
std::vector<cf> wifi_ht_modulate(const std::vector<uint8_t>& psdu, int mcs, unsigned scram_seed);

} // namespace ismmon
