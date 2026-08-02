// ismmon Phase-1 selftest: hardware-free, deterministic. Synthesizes BLE LE 1M advertising
// bursts (gfsk_modulate), adds carrier offset + AWGN + timing lead, and asserts the full RX
// chain (discriminator -> sync -> feed-forward CFO removal -> slice -> de-whiten -> CRC-24 ->
// parse) recovers the PDU bit-exact. Proves the novel DSP; on-air interop is validated in Phase 2.
#include "ble.h"
#include "gfsk_rx.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace ismmon;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { if(cond) { g_pass++; } else { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while(0)

static std::vector<uint8_t> make_adv_ind(const uint8_t adva[6], const std::string& name,
                                         uint16_t mfr = 0, const std::vector<uint8_t>& mfrdata = {}) {
	std::vector<uint8_t> pl;
	for(int i = 0; i < 6; i++) { pl.push_back(adva[i]); }
	pl.push_back(2); pl.push_back(0x01); pl.push_back(0x06);   // Flags: LE General Disc + BR/EDR not supported
	if(!name.empty()) {
		pl.push_back((uint8_t)(1 + name.size())); pl.push_back(0x09);
		for(char c : name) { pl.push_back((uint8_t)c); }
	}
	if(mfr) {
		pl.push_back((uint8_t)(3 + mfrdata.size())); pl.push_back(0xFF);
		pl.push_back((uint8_t)(mfr & 0xFF)); pl.push_back((uint8_t)((mfr >> 8) & 0xFF));
		for(uint8_t b : mfrdata) { pl.push_back(b); }
	}
	std::vector<uint8_t> pdu;
	pdu.push_back(0x00);                     // ADV_IND, public addr
	pdu.push_back((uint8_t)pl.size());
	pdu.insert(pdu.end(), pl.begin(), pl.end());
	return pdu;
}

static std::vector<cf> make_burst(const std::vector<uint8_t>& pdu, int chan, double sps,
                                  double cfo_hz, double snr_db, int lead_syms, std::mt19937& rng) {
	bits stream = ble_build_adv_stream(pdu, chan);
	double fs = sps * BLE_SYM_RATE;
	std::vector<cf> sig = gfsk_modulate(stream, fs);
	int lead = (int)(lead_syms * sps), trail = (int)(24 * sps);
	std::vector<cf> out(lead + sig.size() + trail, cf(0, 0));
	double dph = 2.0 * M_PI * cfo_hz / fs;
	for(size_t i = 0; i < sig.size(); i++) {
		double ph = dph * (double)i;
		out[lead + i] = sig[i] * cf((float)std::cos(ph), (float)std::sin(ph));
	}
	if(snr_db < 200) {
		double sigma = std::sqrt(1.0 / (2.0 * std::pow(10.0, snr_db / 10.0)));
		std::normal_distribution<float> nd(0.0f, (float)sigma);
		for(auto& x : out) { x += cf(nd(rng), nd(rng)); }
	}
	return out;
}

static bool decode_ok(const std::vector<uint8_t>& pdu, int chan, double sps, double cfo, double snr,
                      int lead, std::mt19937& rng, ble_packet& pkt) {
	std::vector<cf> burst = make_burst(pdu, chan, sps, cfo, snr, lead, rng);
	bool found = demod_ble_burst(burst.data(), burst.size(), chan, sps, pkt);
	return found && pkt.crc_ok && pkt.pdu == pdu;
}

int main() {
	std::mt19937 rng(12345);
	uint8_t adva[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 };   // C6.. = random static

	// --- PHY primitive round-trips ---
	{
		bits b; for(int i = 0; i < 200; i++) { b.push_back(rng() & 1); }
		bits b2 = b; ble_whiten(b2, 38); ble_whiten(b2, 38);
		CHECK(b2 == b, "whitening not self-inverse");

		std::vector<uint8_t> pdu = make_adv_ind(adva, "X");
		bits pb = bytes_to_bits(pdu.data(), pdu.size());
		uint32_t c1 = ble_crc24(pb), c2 = ble_crc24(pb);
		CHECK(c1 == c2 && c1 <= 0xFFFFFF, "crc24 unstable/oversized (%06x)", c1);

		std::vector<uint8_t> rt = bits_to_bytes(bytes_to_bits(pdu.data(), pdu.size()));
		CHECK(rt == pdu, "bytes<->bits round-trip broken");
	}

	// --- clean decode across oversampling ratios (2.0 = ismmond's 100 MSPS / decim 50) ---
	for(double sps : { 2.0, 2.4, 3.2, 4.0, 8.0 }) {
		std::vector<uint8_t> pdu = make_adv_ind(adva, "RFNM");
		ble_packet pkt;
		bool ok = decode_ok(pdu, 38, sps, 0, 200, 12, rng, pkt);
		CHECK(ok, "clean decode failed at sps=%.1f (corr %.2f crc_ok %d)", sps, pkt.sync_corr, pkt.crc_ok);
	}
	// --- sps 2.0 with CFO (ismmond operating point) ---
	for(double cfo : { -150e3, 0.0, 120e3 }) {
		std::vector<uint8_t> pdu = make_adv_ind(adva, "R2");
		ble_packet pkt;
		bool ok = decode_ok(pdu, 38, 2.0, cfo, 28, 12, rng, pkt);
		CHECK(ok, "sps=2.0 CFO %.0f kHz failed (corr %.2f)", cfo / 1e3, pkt.sync_corr);
	}

	// --- CFO sweep (BLE tolerance +-150 kHz) at the realistic low oversampling ---
	for(double cfo : { -150e3, -100e3, -50e3, 0.0, 50e3, 100e3, 150e3 }) {
		std::vector<uint8_t> pdu = make_adv_ind(adva, "RFNM");
		ble_packet pkt;
		bool ok = decode_ok(pdu, 38, 2.4, cfo, 30, 12, rng, pkt);
		CHECK(ok, "CFO %.0f kHz failed at sps=2.4 (corr %.2f est_cfo %.0f kHz)",
		      cfo / 1e3, pkt.sync_corr, pkt.cfo_hz / 1e3);
	}

	// --- all three advertising channels (whitening keyed by index) ---
	for(int ch : { 37, 38, 39 }) {
		std::vector<uint8_t> pdu = make_adv_ind(adva, "CH");
		ble_packet pkt;
		bool ok = decode_ok(pdu, ch, 2.4, 40e3, 25, 12, rng, pkt);
		CHECK(ok, "channel %d decode failed (corr %.2f)", ch, pkt.sync_corr);
	}

	// --- timing lead robustness (sync search must find the burst) ---
	for(int lead : { 3, 40, 150 }) {
		std::vector<uint8_t> pdu = make_adv_ind(adva, "LEAD");
		ble_packet pkt;
		bool ok = decode_ok(pdu, 38, 4.0, 30e3, 25, lead, rng, pkt);
		CHECK(ok, "lead=%d decode failed (corr %.2f)", lead, pkt.sync_corr);
	}

	// --- field parse: name + manufacturer data ---
	{
		std::vector<uint8_t> pdu = make_adv_ind(adva, "RFNM-Lime", 0x004C, { 0x02, 0x15 });
		std::mt19937 r2(7);
		std::vector<cf> burst = make_burst(pdu, 38, 4.0, 20e3, 30, 12, r2);
		ble_packet pkt;
		bool found = demod_ble_burst(burst.data(), burst.size(), 38, 4.0, pkt);
		CHECK(found && pkt.crc_ok, "parse-case decode failed");
		CHECK(pkt.adv.pdu_type == 0, "pdu_type != ADV_IND (%d)", pkt.adv.pdu_type);
		CHECK(pkt.adv.has_adva && memcmp(pkt.adv.adva, adva, 6) == 0, "AdvA mismatch (%s)", ble_adva_string(pkt.adv.adva).c_str());
		CHECK(pkt.adv.name == "RFNM-Lime", "name mismatch ('%s')", pkt.adv.name.c_str());
		CHECK(pkt.adv.has_mfr && pkt.adv.mfr_id == 0x004C, "mfr id mismatch (%04x)", pkt.adv.mfr_id);
		CHECK(pkt.adv.has_flags && pkt.adv.flags == 0x06, "flags mismatch (%02x)", pkt.adv.flags);
	}

	// --- a CRC error must be rejected (flip one payload symbol's bit region via a bad pdu) ---
	{
		std::vector<uint8_t> pdu = make_adv_ind(adva, "RFNM");
		std::mt19937 r3(3);
		std::vector<cf> burst = make_burst(pdu, 38, 4.0, 0, 200, 12, r3);
		// corrupt a chunk in the payload region (well after the AA)
		size_t mid = burst.size() / 2;
		for(size_t i = mid; i < mid + (size_t)(8 * 4.0); i++) { burst[i] = -burst[i]; }
		ble_packet pkt;
		demod_ble_burst(burst.data(), burst.size(), 38, 4.0, pkt);
		CHECK(!pkt.crc_ok, "corrupted burst wrongly passed CRC");
	}

	// --- sensitivity sweep (report the SNR floor; assert reliable >= 14 dB) ---
	printf("  -- sensitivity (100 trials/pt, sps=2.4, cfo=+80kHz) --\n");
	for(double snr : { 25.0, 20.0, 16.0, 14.0, 12.0, 10.0, 8.0 }) {
		int ok = 0;
		for(int t = 0; t < 100; t++) {
			uint8_t a[6]; for(int i = 0; i < 6; i++) { a[i] = rng() & 0xFF; }
			a[5] |= 0xC0;
			std::vector<uint8_t> pdu = make_adv_ind(a, "s");
			ble_packet pkt;
			if(decode_ok(pdu, 38, 2.4, 80e3, snr, 10, rng, pkt)) { ok++; }
		}
		printf("     SNR %5.1f dB : %3d/100\n", snr, ok);
		if(snr >= 14.0) { CHECK(ok >= 95, "sensitivity too low at %.0f dB: %d/100", snr, ok); }
	}

	printf("\nismmon selftest: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
