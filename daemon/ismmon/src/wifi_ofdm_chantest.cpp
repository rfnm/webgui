// A/B the shared ddc vs a sharper OFDM-local channelizer: does a flatter passband raise det_q +
// make the SIGNAL length plausible on real ch1 OFDM frames?  (diagnostic, not a selftest)
#include "channelizer.h"
#include "wifi.h"
#include "wifi_ofdm_chan.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace ismmon;

static void scan(const std::vector<cf>& ch, const char* label) {
	int hist[11] = { 0 }, strong = 0, plausible = 0, dqmax10 = 0;
	for(size_t p = 0; p + 4096 <= ch.size(); p += 1024) {
		wifi_packet pk;
		if(wifi_decode_burst(&ch[p], 4096, 1, pk)) {
			int b = (int)(pk.det_q * 10); if(b < 0) { b = 0; } if(b > 10) { b = 10; }
			hist[b]++;
			if(b > dqmax10) { dqmax10 = b; }
			if(pk.det_q > 0.6f) { strong++; if(pk.sig_ok && pk.length < 2300) { plausible++; } }
		}
	}
	printf("%-26s strong(dq>.6)=%-3d plausibleLen=%-3d maxdq~%.1f | hist[0..1]:", label, strong, plausible, dqmax10 / 10.0);
	for(int i = 0; i < 11; i++) { printf(" %d", hist[i]); }
	printf("\n");
}

int main(int argc, char** argv) {
	const char* path = argc > 1 ? argv[1] : "wifi.cs16";
	FILE* f = fopen(path, "rb");
	if(!f) { fprintf(stderr, "open %s\n", path); return 1; }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	std::vector<int16_t> raw(sz / 2);
	if(fread(raw.data(), 2, raw.size(), f) != raw.size()) {}
	fclose(f);
	size_t nc = raw.size() / 2;
	std::vector<cf> iq(nc);
	for(size_t i = 0; i < nc; i++) { iq[i] = cf(raw[2 * i] / 32768.0f, raw[2 * i + 1] / 32768.0f); }
	printf("loaded %zu samples; ch1 (off %.0f MHz)\n", nc, (2412e6 - 2441e6) / 1e6);
	double off = 2412e6 - 2441e6;

	{ ddc dc(100e6, 20e6, off, 0.48); std::vector<cf> a; dc.process(iq.data(), iq.size(), a); scan(a, "shared ddc 0.48/21tap"); }
	{ std::vector<cf> b; channelize_ofdm(iq.data(), iq.size(), off, 100e6, 20e6, 9.5e6, 8, b);  scan(b, "ofdm 9.5MHz/41tap BH"); }
	{ std::vector<cf> b; channelize_ofdm(iq.data(), iq.size(), off, 100e6, 20e6, 9.0e6, 12, b); scan(b, "ofdm 9.0MHz/61tap BH"); }
	{ std::vector<cf> b; channelize_ofdm(iq.data(), iq.size(), off, 100e6, 20e6, 8.7e6, 16, b); scan(b, "ofdm 8.7MHz/81tap BH"); }
	return 0;
}
