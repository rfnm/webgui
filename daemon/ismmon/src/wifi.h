// ismmon: 802.11a/g (OFDM) receiver for 2.4 GHz management-frame headers. Consumes a Wi-Fi channel
// channelized to 20 MSPS (100 MSPS / decim 5) and decodes beacons / probe requests/responses ->
// SSID + BSSID + frame type. Chain: L-STF autocorrelation detect + coarse CFO -> L-LTF channel
// estimate + fine CFO -> per-OFDM-symbol (CP strip, 64-FFT, equalize, pilot phase-track, demap) ->
// deinterleave -> Viterbi (K=7, 133/171) -> descramble -> SIGNAL (rate/length) + DATA (MAC). Only
// the 6 Mbps basic rate (BPSK, r=1/2) DATA is decoded (what management frames use); other rates
// still yield the SIGNAL header (rate/length/presence). Conventions per IEEE 802.11-2016 clause 17;
// self-consistent selftest + validated on-air.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dsp.h"

namespace ismmon {

// OFDM (20 MHz) constants
constexpr int WIFI_NFFT = 64;
constexpr int WIFI_CP = 16;
constexpr int WIFI_SYM = 80;       // CP + FFT
constexpr int WIFI_NDATA = 48;     // data subcarriers
constexpr double WIFI_FS = 20.0e6; // channelized sample rate

struct wifi_packet {
	int channel = 0;               // Wi-Fi channel number (1/6/11)
	int rate_mbps = 0;             // from SIGNAL
	int length = 0;                // PSDU length (bytes) from SIGNAL
	bool sig_ok = false;           // SIGNAL parity valid
	bool mac_ok = false;           // a plausible MAC frame was parsed (6 Mbps only)
	double cfo_hz = 0;
	float rssi_db = 0;
	float det_q = 0;               // L-LTF correlation quality (~0.9 real packet, ~0.4 noise) - reject false SIGNALs
	int frame_type = -1;           // 802.11 type/subtype (e.g. 0x08 beacon, 0x04 probe-req, 0x05 probe-resp)
	std::string type_name;
	uint8_t bssid[6] = {0}, sa[6] = {0}, da[6] = {0};
	bool has_addr = false;
	std::string ssid;
	// 802.11n HT (mixed format) - set when the symbol after L-SIG is QBPSK (HT-SIG present)
	bool is_ht = false;            // HT (11n) frame detected via HT-SIG rotation
	bool htsig_ok = false;         // HT-SIG CRC-8 valid
	int mcs = -1;                  // HT MCS index (0..7 single stream)
	int ht_length = 0;             // HT-SIG PSDU length (bytes)
	bool ht_40 = false;            // 40 MHz (CBW). We only decode 20 MHz.
};

// Demodulate one Wi-Fi burst (iq at 20 MSPS, channel baseband). Returns true if an OFDM packet was
// detected + its SIGNAL decoded (inspect .mac_ok / .ssid for a full management-frame decode).
bool wifi_decode_burst(const cf* iq, size_t n, int channel, wifi_packet& out, float det_threshold = 0.6f);

const char* wifi_type_name(int type_subtype);
std::string wifi_mac_string(const uint8_t a[6]);
// FCS-checked 802.11 management-frame parse (shared by the OFDM and DSSS receivers) -> fills addrs,
// type, and SSID; sets out.mac_ok on a valid FCS + management frame.
void wifi_parse_mac(const std::vector<uint8_t>& frame, wifi_packet& out);
uint32_t wifi_crc32(const uint8_t* d, size_t n);

// ---- selftest support (build + OFDM-modulate a frame, so the decoder can be validated offline) ----
std::vector<uint8_t> wifi_build_beacon(const uint8_t bssid[6], const std::string& ssid);
std::vector<cf> wifi_ofdm_modulate(const std::vector<uint8_t>& psdu, int rate_mbps, unsigned scram_seed);

} // namespace ismmon
