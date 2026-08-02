// ismmon 802.11b DSSS selftest: build a beacon, DSSS-modulate (1/2 Mbps), add lead/CFO/AWGN, decode,
// assert SSID/BSSID recovered. Self-consistent; on-air conventions validated against the real capture.
#include "wifi.h"
#include "wifi_dsss.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace ismmon;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if(c) { g_pass++; } else { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while(0)

static std::vector<cf> make_burst(const std::vector<cf>& sig, double cfo_hz, double snr_db, int lead, std::mt19937& rng) {
	std::vector<cf> out(lead + sig.size() + 400, cf(0, 0));
	double dph = 2.0 * M_PI * cfo_hz / DSSS_FS;
	for(size_t i = 0; i < sig.size(); i++) {
		double ph = dph * (double)i;
		out[lead + i] = sig[i] * cf((float)cos(ph), (float)sin(ph));
	}
	if(snr_db < 200) {
		double sp = 0; for(auto& x : sig) { sp += std::norm(x); } sp /= sig.size();
		double sigma = std::sqrt(sp / (2.0 * std::pow(10.0, snr_db / 10.0)));
		std::normal_distribution<float> nd(0.0f, (float)sigma);
		for(auto& x : out) { x += cf(nd(rng), nd(rng)); }
	}
	return out;
}

int main() {
	std::mt19937 rng(9);
	uint8_t bssid[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
	std::string ssid = "RFNM-11b-AP";
	std::vector<uint8_t> psdu = wifi_build_beacon(bssid, ssid);
	printf("  beacon PSDU %zu bytes ssid='%s'\n", psdu.size(), ssid.c_str());

	// clean, 1 Mbps
	{
		std::vector<cf> sig = dsss_modulate(psdu, 1);
		std::vector<cf> b = make_burst(sig, 0, 200, 400, rng);
		wifi_packet pk;
		bool ok = dsss_decode_burst(b.data(), b.size(), 6, pk);
		CHECK(ok && pk.sig_ok, "clean 1M: no header (ok %d rate %d len %d)", ok, pk.rate_mbps, pk.length);
		CHECK(pk.rate_mbps == 1, "clean 1M: rate %d", pk.rate_mbps);
		CHECK(pk.mac_ok, "clean 1M: MAC not decoded");
		CHECK(pk.mac_ok && pk.ssid == ssid, "clean 1M: ssid '%s'", pk.ssid.c_str());
		CHECK(pk.mac_ok && memcmp(pk.bssid, bssid, 6) == 0, "clean 1M: bssid %s", wifi_mac_string(pk.bssid).c_str());
		if(pk.mac_ok) { printf("  1M decode: %s '%s' bssid %s\n", pk.type_name.c_str(), pk.ssid.c_str(), wifi_mac_string(pk.bssid).c_str()); }
	}
	// CFO + noise, 1 Mbps
	for(double cfo : { -40e3, 0.0, 50e3 }) {
		std::vector<cf> sig = dsss_modulate(psdu, 1);
		std::vector<cf> b = make_burst(sig, cfo, 15, 400, rng);
		wifi_packet pk;
		bool ok = dsss_decode_burst(b.data(), b.size(), 6, pk);
		CHECK(ok && pk.mac_ok && pk.ssid == ssid, "1M cfo %.0fk/15dB: ssid='%s' mac_ok %d", cfo / 1e3, pk.ssid.c_str(), pk.mac_ok);
	}
	// sensitivity
	printf("  -- sensitivity (60/pt, cfo +30k, 1 Mbps) --\n");
	for(double snr : { 20.0, 12.0, 8.0, 5.0, 3.0 }) {
		int ok = 0;
		for(int t = 0; t < 60; t++) {
			char s[24]; snprintf(s, sizeof(s), "b%d", t);
			std::vector<uint8_t> p = wifi_build_beacon(bssid, s);
			std::vector<cf> sig = dsss_modulate(p, 1);
			std::vector<cf> b = make_burst(sig, 30e3, snr, 400, rng);
			wifi_packet pk;
			if(dsss_decode_burst(b.data(), b.size(), 6, pk) && pk.mac_ok && pk.ssid == s) { ok++; }
		}
		printf("     SNR %5.1f dB : %2d/60\n", snr, ok);
		if(snr >= 12.0) { CHECK(ok >= 57, "1M sensitivity low at %.0f dB: %d/60", snr, ok); }
	}

	printf("\nismmon dsss selftest: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
