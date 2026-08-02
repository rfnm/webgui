// ismmon: 802.11b DSSS receiver (long preamble, 1 Mbps DBPSK / 2 Mbps DQPSK) for 2.4 GHz beacons -
// the rate this bench's APs actually beacon at. Consumes a Wi-Fi channel at 25 MSPS (100/decim 4;
// 11 Mchip Barker -> ~2.27 samples/chip). Chain: Barker matched-filter despread -> symbol timing ->
// CFO removal -> differential (D)PSK -> self-sync descramble (x^7+x^4+1) -> SFD -> PLCP header
// (rate/length + CRC-16) -> PSDU -> shared MAC parse -> SSID/BSSID. Reuses wifi_packet + wifi_parse_mac.
#pragma once

#include <cstdint>
#include <vector>

#include "dsp.h"
#include "wifi.h"

namespace ismmon {

constexpr double DSSS_FS = 25.0e6;

// Decode one 802.11b burst (iq at 25 MSPS, channel baseband). Returns true if the PLCP header
// decoded (CRC-16 ok); inspect .mac_ok / .ssid for the management-frame decode.
bool dsss_decode_burst(const cf* iq, size_t n, int channel, wifi_packet& out);

// selftest / bench: DSSS-modulate a PSDU at 1 or 2 Mbps (long preamble) to 25 MSPS complex baseband.
std::vector<cf> dsss_modulate(const std::vector<uint8_t>& psdu, int rate_mbps);

} // namespace ismmon
