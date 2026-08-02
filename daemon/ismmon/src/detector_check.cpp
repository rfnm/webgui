// A/B self-check for detector_band_peak (detector.h): the NEON path (aarch64) must equal the scalar
// max over random u8 rows and random [lo,hi] sub-bands. Deterministic LCG (no time-based RNG). On x86
// both sides are scalar (a smoke test); on aarch64 it proves the vmaxq_u8/vmaxvq_u8 path is exact.
#include "detector.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace ismmon;

static int ref_peak(const uint8_t* row, int lo, int hi) {
	int b = 0;
	for(int i = lo; i <= hi; i++) { if(row[i] > b) { b = row[i]; } }
	return b;
}

int main() {
	uint32_t seed = 0x1234567u;
	auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7fff; };
	const int NBINS = 4096;
	std::vector<uint8_t> row(NBINS);
	int tests = 0, fails = 0;
	for(int t = 0; t < 5000; t++) {
		for(auto& x : row) { x = (uint8_t)(rnd() & 0xff); }
		int lo = rnd() % (NBINS - 1);
		int hi = lo + (rnd() % 130);          // sub-bands from 1 to ~130 bins (covers BLE/Wi-Fi widths)
		if(hi >= NBINS) { hi = NBINS - 1; }
		if(lo > hi) { continue; }
		tests++;
		int a = detector_band_peak(row.data(), lo, hi);
		int r = ref_peak(row.data(), lo, hi);
		if(a != r) { fails++; if(fails <= 5) { printf("  MISMATCH lo=%d hi=%d peak=%d ref=%d\n", lo, hi, a, r); } }
	}
	// also exercise widths that straddle the 16-lane boundary explicitly
	for(int w = 0; w < 40; w++) {
		for(int i = 0; i < NBINS; i++) { row[i] = (uint8_t)((i * 7 + w * 3) & 0xff); }
		int lo = 100, hi = 100 + w;
		tests++;
		if(detector_band_peak(row.data(), lo, hi) != ref_peak(row.data(), lo, hi)) { fails++; }
	}
	printf("detector band-peak A/B: %d tests, %d fails\n", tests, fails);
	return fails ? 1 : 0;
}
