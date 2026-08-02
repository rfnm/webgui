// ismmon: BLE LE 1M link-layer PHY primitives, isolated so every on-air convention that
// could differ between implementations lives in one place (interop fixes stay one-liners).
//
// Conventions (BLE Core spec Vol 6 Part B; proven SELF-CONSISTENT by the selftest -
// absolute polynomial/bit-order ground truth is confirmed against real captures in Phase 2):
//   - LE 1M: 1 Msym/s GFSK, modulation index h=0.5, Gaussian BT=0.5. bit 1 = +deviation.
//   - On air, everything is transmitted LSB-first within each byte. A `bits` vector here is
//     one bit/element in TRANSMIT order: bits[0] = first bit on air = LSB of byte 0.
//   - Advertising access address 0x8E89BED6, sent little-endian byte order (D6 BE 89 8E),
//     each byte LSB-first. Preamble 0xAA (LE 1M, AA LSB = 0).
//   - Whitening (7-bit LFSR x^7+x^4+1) covers PDU+CRC only (not preamble/AA), keyed by the
//     channel INDEX (37/38/39 for the advertising channels).
//   - CRC-24 (x^24+x^10+x^9+x^6+x^4+x^3+x+1), init 0x555555 for advertising, over the PDU
//     (header+payload), transmitted after the payload (before whitening is applied to it).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dsp.h"

namespace ismmon {

typedef std::vector<uint8_t> bits;   // 1 bit/element, TRANSMIT order (bits[0] = LSB of byte 0)

// ---- BLE LE 1M PHY constants ----
constexpr double BLE_SYM_RATE = 1.0e6;
constexpr double BLE_H = 0.5;
constexpr double BLE_BT = 0.5;
constexpr uint32_t BLE_ADV_AA = 0x8E89BED6u;
constexpr uint32_t BLE_ADV_CRC_INIT = 0x555555u;

inline double ble_adv_freq(int chan_index) {
	switch(chan_index) {
		case 37: return 2402e6;
		case 38: return 2426e6;
		case 39: return 2480e6;
	}
	return 0;
}

// ---- byte <-> bit (LSB-first per byte, transmit order) ----
bits bytes_to_bits(const uint8_t* bytes, size_t n);
std::vector<uint8_t> bits_to_bytes(const bits& b);   // b.size() a multiple of 8

// ---- CRC-24 over PDU bits (transmit order) ----
uint32_t ble_crc24(const bits& pdu_bits, uint32_t init = BLE_ADV_CRC_INIT);

// ---- Data whitening (in place; self-inverse - same call de-whitens) over PDU+CRC bits ----
void ble_whiten(bits& b, int chan_index);

// ---- GFSK modulator (selftest / bench only; the daemon is RX) ----
// NRZ bits (0/1) -> complex baseband at fs (Hz). Continuous-phase Gaussian pulse. Amplitude 1.
std::vector<cf> gfsk_modulate(const bits& nrz, double fs, double baud = BLE_SYM_RATE,
                              double h = BLE_H, double bt = BLE_BT);

// ---- Assemble the full on-air advertising bit stream ----
// pdu = header(2 bytes) + payload; returns preamble + AA + whitened(PDU + CRC), NRZ, ready for
// gfsk_modulate. chan_index keys the whitening (37/38/39).
bits ble_build_adv_stream(const std::vector<uint8_t>& pdu, int chan_index, uint32_t crc_init = BLE_ADV_CRC_INIT);

// General uncoded stream builder (selftest/bench): preamble_bytes 0xAA (1 = LE 1M, 2 = LE 2M) + the
// given 32-bit access address (LE bytes) + whitened(PDU + CRC-24), as NRZ symbols for gfsk_modulate.
bits ble_build_uncoded_stream(const std::vector<uint8_t>& pdu, int chan_index, uint32_t aa,
                              int preamble_bytes, uint32_t crc_init = BLE_ADV_CRC_INIT);

// ---- Advertising PDU parse ----
struct ble_adv_pdu {
	int pdu_type = 0;
	bool tx_add = false, rx_add = false, ch_sel = false;
	int length = 0;
	uint8_t adva[6] = {0};     // advertiser address, on-air (LSB) byte order
	bool has_adva = false;
	std::vector<uint8_t> payload;
	// parsed AD structures from AdvData
	uint8_t flags = 0; bool has_flags = false;
	std::string name;
	int8_t tx_power = 0; bool has_tx_power = false;
	uint16_t appearance = 0; bool has_appearance = false;
	uint16_t mfr_id = 0; bool has_mfr = false;
	std::vector<uint16_t> uuid16;
	// ---- Extended advertising (ADV_EXT_IND / AUX_ADV_IND / AUX_CHAIN_IND, pdu_type 7) ----
	bool is_ext = false;              // parsed as an extended-advertising PDU
	int adv_mode = 0;                 // 0=non-conn/non-scan, 1=connectable, 2=scannable
	bool has_target = false; uint8_t targeta[6] = {0};
	bool has_adi = false; uint16_t adi_did = 0; uint8_t adi_sid = 0;
	bool has_aux = false;             // AuxPtr present (payload continues on a secondary channel)
	int aux_chan = 0; int aux_phy = 0;
	bool has_cte = false, has_syncinfo = false;
};
const char* ble_pdu_type_name(int t);
std::string ble_adva_string(const uint8_t adva[6]);   // MSB-first colon notation for display
bool ble_parse_adv_pdu(const std::vector<uint8_t>& pdu, ble_adv_pdu& out);

} // namespace ismmon
