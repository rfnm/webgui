// ismmon 802.11b DSSS offline validator: read a 100 MSPS capture, channelize each Wi-Fi channel to
// 25 MSPS, find energy bursts, and decode them with dsss_decode_burst -> real over-the-air SSIDs.
#include "channelizer.h"
#include "wifi.h"
#include "wifi_dsss.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace ismmon;

int main(int argc, char** argv) {
	const char* path = argc > 1 ? argv[1] : "/tmp/wifi.cs16";
	FILE* f = fopen(path, "rb");
	if(!f) { fprintf(stderr, "open %s\n", path); return 1; }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	std::vector<int16_t> raw(sz / 2);
	if(fread(raw.data(), 2, raw.size(), f) != raw.size()) {}
	fclose(f);
	size_t nc = raw.size() / 2;
	std::vector<cf> iq(nc);
	for(size_t i = 0; i < nc; i++) { iq[i] = cf(raw[2 * i] / 32768.0f, raw[2 * i + 1] / 32768.0f); }
	printf("loaded %zu complex (%.1f ms)\n", nc, nc / 100e6 * 1e3);

	std::map<std::string, wifi_packet> all_aps;   // global dedup by BSSID (an AP leaks into adjacent channels)
	std::map<std::string, int> clients;           // data-frame transmitters (devices), dedup by MAC + count
	struct { int ch; double f; } chans[13];
	for(int i = 0; i < 13; i++) { chans[i].ch = i + 1; chans[i].f = 2412e6 + 5e6 * i; }   // ch1..13 = 2412..2472
	for(auto& c : chans) {
		ddc dc(100e6, 25e6, c.f - 2441e6, 0.44);   // -> 25 MSPS (11b ~22 MHz)
		std::vector<cf> ch;
		dc.process(iq.data(), iq.size(), ch);
		double fso = 25e6;
		// Exhaustive slide: no energy gate. Overlapping windows so any beacon is the "first SFD" in
		// some window; dedup APs by BSSID. Catches the weak/distant tail the energy detector skipped.
		int hdr_ok = 0, newaps = 0;
		size_t W = (size_t)(3.5e-3 * fso);     // 3.5 ms window (>= a full 1 Mbps beacon)
		size_t step = (size_t)(0.4e-3 * fso);  // 0.4 ms slide
		for(size_t p = 0; p + W <= ch.size(); p += step) {
			wifi_packet pk;
			if(dsss_decode_burst(&ch[p], W, c.ch, pk)) {
				if(pk.sig_ok) { hdr_ok++; }
				if(pk.has_addr && !pk.ssid.empty()) {
					std::string k = wifi_mac_string(pk.bssid);
					auto it = all_aps.find(k);
					if(it == all_aps.end()) { newaps++; all_aps[k] = pk; }
					else if(pk.rssi_db > it->second.rssi_db) { it->second = pk; }
				} else if(pk.has_addr && pk.frame_type == 0x20) {
					clients[wifi_mac_string(pk.bssid)]++;   // data-frame transmitter = a device
				}
			}
		}
		printf("ch%2d (%.0f MHz): header-ok %3d, new APs %d\n", c.ch, c.f / 1e6, hdr_ok, newaps);
	}
	printf("\n=== %zu unique APs across ch1-13 ===\n", all_aps.size());
	for(auto& kv : all_aps) {
		wifi_packet& p = kv.second;
		printf("   %-9s %s ch%-2d ssid='%s' rate %dM rssi %.0f\n", p.type_name.c_str(), kv.first.c_str(), p.channel, p.ssid.c_str(), p.rate_mbps, p.rssi_db);
	}
	int solid = 0; for(auto& kv : clients) { if(kv.second >= 2) { solid++; } }
	printf("\n=== %d client devices (data-frame TAs seen >=2x; %zu total unique MACs) ===\n", solid, clients.size());
	int shown = 0;
	for(auto& kv : clients) { if(kv.second >= 2 && shown < 20) { printf("   %s  x%d\n", kv.first.c_str(), kv.second); shown++; } }
	return 0;
}
