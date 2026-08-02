// ismmon: BLE LE Coded PHY (Long Range) forward-error-correction primitives, isolated so the FEC
// conventions live in one place. LE Coded transmits 1 Msym/s GFSK (h=0.5) symbols; each info bit is
// spread to 2 symbols (S=2, 500 kb/s) or 8 symbols (S=8, 125 kb/s) via a rate-1/2 convolutional code
// plus, for S=8, a 4-symbol pattern mapper. Packet = 80-sym preamble + FEC block 1 (AA + Coding
// Indicator + TERM1, always S=8) + FEC block 2 (PDU + CRC + TERM2, S per the CI).
//
// SPEC-CONVENTION FLAGS (self-consistent here; confirm vs Bluetooth Core Spec Vol 6 Part B 3.3
// before on-air use): the two convolutional generator polynomials + output order, the S=8 pattern
// mapper (0->0011 / 1->1100), the CI value->scheme mapping (0->S8, 1->S2), and the 80-symbol
// preamble pattern (0b00111100 x10). All are single-constant fixes if a capture disagrees.
#pragma once

#include <cstdint>
#include <vector>

#include "ble.h"

namespace ismmon {

// rate-1/2 convolutional encode. `in` includes the 3 trailing TERM (flush) bits. Returns 2*in.size()
// coded bits (c0,c1 per input bit, in that order).
bits ble_conv_encode(const bits& in);

// soft-decision Viterbi over coded soft values (soft[2k],soft[2k+1] = c0,c1 LLRs, + = bit 1).
// terminated=true tracebacks from state 0 (the block ends with TERM flush); false = best state.
bits ble_viterbi_decode(const std::vector<float>& soft, bool terminated);

// S=8 pattern mapper: each coded bit -> 4 symbols. (0->0011, 1->1100.)
bits ble_pattern_map_s8(const bits& coded);

// the 80-symbol LE Coded preamble (0/1 symbols).
const bits& ble_coded_preamble();

// Build the full on-air symbol stream (0/1 symbols at 1 Msym/s) for a coded packet:
// preamble + FEC1(AA,CI,TERM1 @S8) + FEC2(whiten(PDU+CRC),TERM2 @ ci_s). ci_s is 8 or 2.
bits ble_build_coded_stream(const std::vector<uint8_t>& pdu, int chan_index, int ci_s,
                            uint32_t aa = BLE_ADV_AA, uint32_t crc_init = BLE_ADV_CRC_INIT);

} // namespace ismmon
