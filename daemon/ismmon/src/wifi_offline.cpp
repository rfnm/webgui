// ismmon Wi-Fi offline validator: read a whole-band 100 MSPS capture, channelize a Wi-Fi channel to
// 20 MSPS, and scan it with wifi_decode_burst — validates the OFDM decoder's conventions against REAL
// over-the-air 802.11 (the way the BLE CRC was validated). usage: wifi_offline <capture.cs16>
#include "channelizer.h"
#include "wifi.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace ismmon;

static std::map<std::string, int> g_clients;   // legacy OFDM data-frame transmitters (client/AP devices), all channels
static int g_htsig_ok = 0;                      // real HT frames whose HT-SIG CRC-8 validated
static std::map<int, int> g_mcs;                // MCS histogram of those HT frames

int main(int argc, char** argv) {
	const char* path = argc > 1 ? argv[1] : "/tmp/wifi.cs16";
	FILE* f = fopen(path, "rb");
	if(!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	std::vector<int16_t> raw(sz / 2);
	if(fread(raw.data(), 2, raw.size(), f) != raw.size()) { /* short */ }
	fclose(f);
	size_t ncplx = raw.size() / 2;
	std::vector<cf> iq(ncplx);
	for(size_t i = 0; i < ncplx; i++) { iq[i] = cf(raw[2 * i] / 32768.0f, raw[2 * i + 1] / 32768.0f); }
	printf("loaded %zu complex samples (%.1f ms @100 MSPS)\n", ncplx, ncplx / 100e6 * 1e3);

	struct { int ch; double f; } chans[] = { { 1, 2412e6 }, { 6, 2437e6 }, { 11, 2462e6 } };
	for(auto& c : chans) {
		double off = c.f - 2441e6;
		ddc dc(100e6, 20e6, off, 0.48);   // -> 20 MSPS, pass the ~16.5 MHz OFDM
		std::vector<cf> ch;
		dc.process(iq.data(), iq.size(), ch);
		// scan overlapping windows. Gate on the L-LTF match quality (det_q) to reject the false SIGNALs
		// that a weak 1-bit parity admits on noise; then tally legacy vs HT (11n) among REAL packets.
		std::map<std::string, wifi_packet> aps;
		std::map<int, int> rate_hist;
		int strong = 0, ht_seen = 0, legacy_seen = 0, ssid_seen = 0;
		int qh[10] = { 0 };
		const int W = 4096, STEP = 800;
		const float QT = getenv("WIFI_QT") ? atof(getenv("WIFI_QT")) : 0.55f;
		std::map<int, int> ht_rate, leg_rate;
		for(size_t p = 0; p + W <= ch.size(); p += STEP) {
			wifi_packet pk;
			if(wifi_decode_burst(&ch[p], W, c.ch, pk)) {
				if(pk.det_q > 0) { qh[std::min(9, (int)(pk.det_q * 10))]++; }
				if(pk.sig_ok && pk.det_q > QT) {
					strong++;
					rate_hist[pk.rate_mbps]++;
					if(pk.is_ht) { ht_seen++; ht_rate[pk.rate_mbps]++; if(pk.htsig_ok) { g_htsig_ok++; g_mcs[pk.mcs]++; } } else { legacy_seen++; leg_rate[pk.rate_mbps]++; }
				}
				if(pk.has_addr && !pk.ssid.empty() && pk.det_q > QT) { ssid_seen++; aps[wifi_mac_string(pk.bssid)] = pk; }
				if(pk.det_q > QT && pk.has_addr && pk.frame_type == 0x20) { g_clients[wifi_mac_string(pk.bssid)]++; }   // legacy data-frame TA = device
			}
		}
		(void)ht_rate; (void)leg_rate;
		printf("\nch%d (%.0f MHz): strong SIGNAL (det_q>%.2f): %d  [legacy %d, HT %d]  SSID %d  uniqueAP %zu\n",
		       c.ch, c.f / 1e6, QT, strong, legacy_seen, ht_seen, ssid_seen, aps.size());
		printf("   det_q histogram [0..1 in 0.1]:");
		for(int i = 0; i < 10; i++) { printf(" %d", qh[i]); }
		printf("\n   legacy rate hist:");
		for(auto& kv : leg_rate) { printf(" %d:%d", kv.first, kv.second); }
		printf("   |  HT L-SIG rate hist:");
		for(auto& kv : ht_rate) { printf(" %d:%d", kv.first, kv.second); }
		printf("\n");
		for(auto& kv : aps) {
			wifi_packet& p = kv.second;
			printf("   %-8s %s ssid='%s' %s rssi %.0f q%.2f\n", p.type_name.c_str(),
			       kv.first.c_str(), p.ssid.c_str(), p.is_ht ? "HT" : "legacy", p.rssi_db, p.det_q);
		}
	}
	// client devices from legacy OFDM data-frame transmitters (MAC header is unencrypted); >=2 sightings
	printf("\n=== HT (11n): %d real frames with a CRC-8-valid HT-SIG; MCS histogram:", g_htsig_ok);
	for(auto& kv : g_mcs) { printf(" MCS%d:%d", kv.first, kv.second); }
	printf(" ===\n");
	int solid = 0; for(auto& kv : g_clients) { if(kv.second >= 2) { solid++; } }
	printf("=== %d legacy-OFDM data-frame devices (TA seen >=2x; %zu unique MACs) ===\n", solid, g_clients.size());
	int shown = 0;
	for(auto& kv : g_clients) { if(kv.second >= 2 && shown < 25) { printf("   %s  x%d\n", kv.first.c_str(), kv.second); shown++; } }
	return 0;
}
