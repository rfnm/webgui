// ismmon BLE-reach selftest: hardware-free, deterministic. Validates the LE 2M PHY, the LE Coded
// PHY (S=8 and S=2), and Extended Advertising PDU parsing by synthesizing on-air streams
// (gfsk_modulate) with CFO + AWGN and asserting the full RX chain recovers the PDU/fields. Proves
// the novel DSP + FEC; on-air interop (exact conv polynomials / patterns) is flagged in ble_coded.
#include "ble.h"
#include "ble_coded.h"
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

static std::vector<uint8_t> make_adv_ind(const uint8_t adva[6], const std::string& name, int pdu_type = 0) {
	std::vector<uint8_t> pl;
	for(int i = 0; i < 6; i++) { pl.push_back(adva[i]); }
	pl.push_back(2); pl.push_back(0x01); pl.push_back(0x06);
	if(!name.empty()) {
		pl.push_back((uint8_t)(1 + name.size())); pl.push_back(0x09);
		for(char c : name) { pl.push_back((uint8_t)c); }
	}
	std::vector<uint8_t> pdu;
	pdu.push_back((uint8_t)(pdu_type & 0x0F));
	pdu.push_back((uint8_t)pl.size());
	pdu.insert(pdu.end(), pl.begin(), pl.end());
	return pdu;
}

// ADV_EXT_IND / AUX_ADV_IND (pdu_type 7): ext header flags AdvA+ADI+AuxPtr+TxPower, then AdvData=name.
static std::vector<uint8_t> make_ext_adv(const uint8_t adva[6], const std::string& name) {
	std::vector<uint8_t> ext;
	uint8_t flags = 0x01 | 0x08 | 0x10 | 0x40;   // AdvA, ADI, AuxPtr, TxPower
	ext.push_back(flags);
	for(int i = 0; i < 6; i++) { ext.push_back(adva[i]); }         // AdvA
	ext.push_back(0x23); ext.push_back(0x41);                      // ADI: DID=0x123, SID=4
	ext.push_back(9); ext.push_back(0x00); ext.push_back(0x20);    // AuxPtr: chan 9, aux_phy 1 (2M)
	ext.push_back((uint8_t)0xF4);                                  // TxPower = -12 dBm
	std::vector<uint8_t> payload;
	payload.push_back((uint8_t)(ext.size() & 0x3F));               // ExtHdrLen (AdvMode=0)
	payload.insert(payload.end(), ext.begin(), ext.end());
	payload.push_back((uint8_t)(1 + name.size())); payload.push_back(0x09);   // AdvData: Complete Name
	for(char c : name) { payload.push_back((uint8_t)c); }
	std::vector<uint8_t> pdu;
	pdu.push_back(0x07);                                           // ADV_EXT_IND
	pdu.push_back((uint8_t)payload.size());
	pdu.insert(pdu.end(), payload.begin(), payload.end());
	return pdu;
}

static std::vector<cf> mod_stream(const bits& stream, double sps, double baud, double cfo_hz,
                                  double snr_db, int lead_syms, std::mt19937& rng) {
	double fs = sps * baud;
	std::vector<cf> sig = gfsk_modulate(stream, fs, baud);
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

int main() {
	std::mt19937 rng(9991);
	uint8_t adva[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 };

	// ---- FEC round-trip (conv encode -> Viterbi), bit level ----
	{
		bits in; for(int i = 0; i < 60; i++) { in.push_back(rng() & 1); }
		for(int i = 0; i < 3; i++) { in.push_back(0); }   // TERM
		bits coded = ble_conv_encode(in);
		std::vector<float> soft(coded.size());
		for(size_t i = 0; i < coded.size(); i++) { soft[i] = coded[i] ? 1.f : -1.f; }
		bits dec = ble_viterbi_decode(soft, true);
		CHECK(dec == in, "conv/Viterbi round-trip failed");
		// one bit error is corrected by the code
		soft[10] = -soft[10];
		CHECK(ble_viterbi_decode(soft, true) == in, "Viterbi did not correct a single coded-bit error");
	}

	// ================= LE 2M PHY =================
	printf("  -- LE 2M --\n");
	for(double sps : { 2.0, 2.5, 4.0 }) {
		std::vector<uint8_t> pdu = make_adv_ind(adva, "RFNM-2M");
		bits stream = ble_build_uncoded_stream(pdu, 38, BLE_ADV_AA, 2);
		std::vector<cf> b = mod_stream(stream, sps, 2e6, 0, 200, 12, rng);
		ble_packet pkt;
		bool ok = demod_ble_burst_2m(b.data(), b.size(), 38, sps, pkt);
		CHECK(ok && pkt.crc_ok && pkt.pdu == pdu, "2M clean sps=%.1f (corr %.2f crc %d)", sps, pkt.sync_corr, pkt.crc_ok);
		CHECK(pkt.phy == 2, "2M phy flag = %d", pkt.phy);
	}
	for(double cfo : { -250e3, -120e3, 0.0, 120e3, 250e3 }) {
		std::vector<uint8_t> pdu = make_adv_ind(adva, "2M-CFO");
		bits stream = ble_build_uncoded_stream(pdu, 38, BLE_ADV_AA, 2);
		std::vector<cf> b = mod_stream(stream, 2.5, 2e6, cfo, 28, 12, rng);
		ble_packet pkt;
		bool ok = demod_ble_burst_2m(b.data(), b.size(), 38, 2.5, pkt);
		CHECK(ok && pkt.crc_ok, "2M CFO %.0f kHz failed (corr %.2f est %.0f kHz)", cfo / 1e3, pkt.sync_corr, pkt.cfo_hz / 1e3);
	}
	printf("  -- 2M sensitivity (100/pt, sps=2.5, cfo +100k) --\n");
	for(double snr : { 20.0, 16.0, 14.0, 12.0, 10.0 }) {
		int ok = 0;
		for(int t = 0; t < 100; t++) {
			uint8_t a[6]; for(int i = 0; i < 6; i++) { a[i] = rng() & 0xFF; } a[5] |= 0xC0;
			std::vector<uint8_t> pdu = make_adv_ind(a, "s");
			bits stream = ble_build_uncoded_stream(pdu, 38, BLE_ADV_AA, 2);
			std::vector<cf> b = mod_stream(stream, 2.5, 2e6, 100e3, snr, 10, rng);
			ble_packet pkt;
			if(demod_ble_burst_2m(b.data(), b.size(), 38, 2.5, pkt) && pkt.crc_ok && pkt.pdu == pdu) { ok++; }
		}
		printf("     SNR %5.1f dB : %3d/100\n", snr, ok);
		if(snr >= 14.0) { CHECK(ok >= 95, "2M sensitivity low at %.0f dB: %d/100", snr, ok); }
	}

	// ================= LE Coded PHY (S=8 / S=2) =================
	printf("  -- LE Coded --\n");
	for(int S : { 8, 2 }) {
		std::vector<uint8_t> pdu = make_adv_ind(adva, S == 8 ? "LR-S8" : "LR-S2", 7);
		bits sym = ble_build_coded_stream(pdu, 38, S, BLE_ADV_AA);
		std::vector<cf> b = mod_stream(sym, 4.0, 1e6, 0, 200, 20, rng);
		ble_packet pkt;
		bool ok = demod_ble_coded(b.data(), b.size(), 38, 4.0, pkt);
		CHECK(ok && pkt.crc_ok && pkt.pdu == pdu, "coded S=%d clean (corr %.2f coded_s %d crc %d)", S, pkt.sync_corr, pkt.coded_s, pkt.crc_ok);
		CHECK(pkt.coded_s == S, "coded S detect %d != %d", pkt.coded_s, S);
	}
	for(int S : { 8, 2 }) {
		for(double cfo : { -100e3, 0.0, 100e3 }) {
			std::vector<uint8_t> pdu = make_adv_ind(adva, "LR-CFO", 7);
			bits sym = ble_build_coded_stream(pdu, 39, S, BLE_ADV_AA);
			std::vector<cf> b = mod_stream(sym, 4.0, 1e6, cfo, 25, 20, rng);
			ble_packet pkt;
			bool ok = demod_ble_coded(b.data(), b.size(), 39, 4.0, pkt);
			CHECK(ok && pkt.crc_ok && pkt.coded_s == S, "coded S=%d CFO %.0fk failed (corr %.2f crc %d)", S, cfo / 1e3, pkt.sync_corr, pkt.crc_ok);
		}
	}
	printf("  -- Coded S=8 sensitivity (long range; 80/pt, sps=4, cfo +60k) --\n");
	for(double snr : { 12.0, 8.0, 4.0, 2.0, 0.0 }) {
		int ok = 0;
		for(int t = 0; t < 80; t++) {
			uint8_t a[6]; for(int i = 0; i < 6; i++) { a[i] = rng() & 0xFF; } a[5] |= 0xC0;
			std::vector<uint8_t> pdu = make_adv_ind(a, "lr", 7);
			bits sym = ble_build_coded_stream(pdu, 38, 8, BLE_ADV_AA);
			std::vector<cf> b = mod_stream(sym, 4.0, 1e6, 60e3, snr, 20, rng);
			ble_packet pkt;
			if(demod_ble_coded(b.data(), b.size(), 38, 4.0, pkt) && pkt.crc_ok && pkt.pdu == pdu) { ok++; }
		}
		printf("     SNR %5.1f dB : %2d/80\n", snr, ok);
		if(snr >= 8.0) { CHECK(ok >= 76, "coded S=8 sensitivity low at %.0f dB: %d/80", snr, ok); }
	}

	// ================= Extended Advertising parse =================
	printf("  -- Extended advertising --\n");
	{
		std::vector<uint8_t> pdu = make_ext_adv(adva, "RFNM-Ext");
		ble_adv_pdu p;
		bool ok = ble_parse_adv_pdu(pdu, p);
		CHECK(ok && p.is_ext, "ext parse: not flagged extended");
		CHECK(p.pdu_type == 7, "ext parse: pdu_type %d", p.pdu_type);
		CHECK(p.has_adva && memcmp(p.adva, adva, 6) == 0, "ext parse: AdvA (%s)", ble_adva_string(p.adva).c_str());
		CHECK(p.has_adi && p.adi_did == 0x123 && p.adi_sid == 4, "ext parse: ADI did %03x sid %d", p.adi_did, p.adi_sid);
		CHECK(p.has_aux && p.aux_chan == 9 && p.aux_phy == 1, "ext parse: AuxPtr chan %d phy %d", p.aux_chan, p.aux_phy);
		CHECK(p.has_tx_power && p.tx_power == -12, "ext parse: TxPower %d", p.tx_power);
		CHECK(p.name == "RFNM-Ext", "ext parse: name '%s'", p.name.c_str());
	}
	// full chain: an AUX_ADV_IND carried on LE 2M, decoded then extended-parsed
	{
		std::vector<uint8_t> pdu = make_ext_adv(adva, "AUX-2M");
		bits stream = ble_build_uncoded_stream(pdu, 6, BLE_ADV_AA, 2);
		std::vector<cf> b = mod_stream(stream, 4.0, 2e6, 40e3, 30, 12, rng);
		ble_packet pkt;
		bool ok = demod_ble_burst_2m(b.data(), b.size(), 6, 4.0, pkt);
		CHECK(ok && pkt.crc_ok, "ext-over-2M decode failed (corr %.2f)", pkt.sync_corr);
		CHECK(pkt.crc_ok && pkt.adv.is_ext && pkt.adv.name == "AUX-2M", "ext-over-2M parse name '%s' ext %d", pkt.adv.name.c_str(), pkt.adv.is_ext);
		CHECK(pkt.crc_ok && pkt.adv.has_adva && memcmp(pkt.adv.adva, adva, 6) == 0, "ext-over-2M AdvA");
	}

	printf("\nismmon reach selftest: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
