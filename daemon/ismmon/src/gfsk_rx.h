// ismmon: BLE LE 1M burst demodulator. Consumes complex baseband already channelized to one
// BLE channel (sps = out_rate / 1 Msym/s). Non-coherent: FM discriminator -> integrate-and-dump
// per symbol -> slide the known preamble+AA over the soft symbols for sync/timing -> feed-forward
// CFO removal from the known sync word -> slice -> de-whiten -> CRC-24 -> parse. Stateless per
// burst: the Phase-2 detector localizes each burst in time/frequency and hands us the slice.
#pragma once

#include <cstdint>
#include <vector>

#include "ble.h"
#include "dsp.h"

namespace ismmon {

struct ble_packet {
	int channel = 0;           // channel index (37/38/39)
	double sync_corr = 0;      // normalized preamble+AA correlation at the winning alignment
	double cfo_hz = 0;         // estimated carrier offset
	float rssi_db = 0;         // rough burst level, 20log10(mean|iq|)
	bool crc_ok = false;
	std::vector<uint8_t> pdu;  // de-whitened PDU bytes (header + payload), CRC stripped
	uint32_t crc_rx = 0, crc_calc = 0;
	ble_adv_pdu adv;           // parsed (valid when it is an advertising PDU)
	int phy = 1;               // 1=LE 1M, 2=LE 2M, 8=Coded S=8, 2... (see ble_phy_kind); default 1M
	int coded_s = 0;           // Coded PHY spreading actually decoded (8 or 2), 0 if uncoded
	uint32_t aa = 0x8E89BED6u; // access address the burst was decoded against
};

// Demodulate one BLE LE 1M burst. Returns true if the sync word was found (inspect .crc_ok for a
// clean decode). corr_threshold ~0.6 is a scale-invariant correlation coefficient in [0,1].
// sps = samples/symbol = out_rate / 1 Msym/s. Unchanged/validated against real captures.
bool demod_ble_burst(const cf* iq, size_t n, int channel, double sps, ble_packet& out,
                     double corr_threshold = 0.6);

// ---- BLE-reach extensions (LE 2M / LE Coded) - additive, share the 1M discriminator core ----
// out.phy records which PHY decoded (set on all paths).
enum ble_phy_kind { BLE_PHY_1M = 1, BLE_PHY_2M = 2, BLE_PHY_CODED_S8 = 8, BLE_PHY_CODED_S2 = 9 };

// LE 2M: 2 Msym/s GFSK h=0.5, 16-symbol (0xAAAA) preamble. sps = out_rate / 2 Msym/s. Same AA /
// whitening / CRC-24 / PDU parse as 1M. aa lets a caller follow a non-advertising access address.
bool demod_ble_burst_2m(const cf* iq, size_t n, int channel, double sps, ble_packet& out,
                        double corr_threshold = 0.6, uint32_t aa = 0x8E89BED6u);

// LE Coded (Long Range): 1 Msym/s symbols, 80-symbol preamble, FEC block 1 (AA+CI+TERM1) always S=8,
// FEC block 2 (PDU+CRC+TERM2) at the CI-selected S=8 or S=2. Demod -> pattern despread -> Viterbi ->
// dewhiten -> CRC-24 -> parse. sps = out_rate / 1 Msym/s. Sets out.coded_s (8 or 2) on success.
bool demod_ble_coded(const cf* iq, size_t n, int channel, double sps, ble_packet& out,
                     double corr_threshold = 0.6, uint32_t aa = 0x8E89BED6u);

} // namespace ismmon
