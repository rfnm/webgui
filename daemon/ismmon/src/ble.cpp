#include "ble.h"

#include <cmath>

namespace ismmon {

// ---- byte <-> bit (LSB-first per byte) ----

bits bytes_to_bits(const uint8_t* bytes, size_t n) {
	bits b;
	b.reserve(n * 8);
	for(size_t i = 0; i < n; i++) {
		for(int k = 0; k < 8; k++) {
			b.push_back((uint8_t)((bytes[i] >> k) & 1));
		}
	}
	return b;
}

std::vector<uint8_t> bits_to_bytes(const bits& b) {
	std::vector<uint8_t> out(b.size() / 8, 0);
	for(size_t i = 0; i < out.size(); i++) {
		uint8_t v = 0;
		for(int k = 0; k < 8; k++) {
			v |= (uint8_t)((b[i * 8 + k] & 1) << k);
		}
		out[i] = v;
	}
	return out;
}

// ---- CRC-24: Galois LFSR, x^24+x^10+x^9+x^6+x^4+x^3+x+1. r[i] holds x^i; init loaded LSB->r[0];
// each data bit XORs the shifted-out x^24 term (r[23]) to form the feedback, which re-enters at
// x^0 and toggles the intermediate polynomial taps. Bits fed in transmit (LSB-first) order. ----

uint32_t ble_crc24(const bits& pdu_bits, uint32_t init) {
	uint8_t r[24];
	for(int i = 0; i < 24; i++) {
		r[i] = (uint8_t)((init >> i) & 1);
	}
	for(uint8_t d : pdu_bits) {
		uint8_t fb = (uint8_t)((d & 1) ^ r[23]);
		for(int i = 23; i >= 1; i--) {
			r[i] = r[i - 1];
		}
		r[0] = fb;
		if(fb) {
			r[1] ^= 1; r[3] ^= 1; r[4] ^= 1; r[6] ^= 1; r[9] ^= 1; r[10] ^= 1;
		}
	}
	// the register is emitted MSB-first (position 23 -> CRC bit 0). VALIDATED against real on-air
	// captures 2026-07-09 (5/6 live BLE packets across ch37/38/39; the 6th had a payload bit error).
	uint32_t out = 0;
	for(int i = 0; i < 24; i++) {
		out |= (uint32_t)(r[i] & 1) << (23 - i);
	}
	return out & 0xFFFFFF;
}

// ---- Data whitening: 7-bit LFSR x^7+x^4+1. r[0]=1, r[1..6]=channel index MSB..LSB. Output bit
// (from r[6]) XORs each data bit; feedback re-enters r[0] and toggles the x^4 tap. Self-inverse. ----

void ble_whiten(bits& b, int chan_index) {
	uint8_t r[7];
	r[0] = 1;
	r[1] = (uint8_t)((chan_index >> 5) & 1);
	r[2] = (uint8_t)((chan_index >> 4) & 1);
	r[3] = (uint8_t)((chan_index >> 3) & 1);
	r[4] = (uint8_t)((chan_index >> 2) & 1);
	r[5] = (uint8_t)((chan_index >> 1) & 1);
	r[6] = (uint8_t)((chan_index >> 0) & 1);
	for(auto& bit : b) {
		uint8_t out = r[6];
		bit ^= out;
		uint8_t fb = r[6];
		r[6] = r[5]; r[5] = r[4]; r[4] = (uint8_t)(r[3] ^ fb); r[3] = r[2]; r[2] = r[1]; r[1] = r[0]; r[0] = fb;
	}
}

// ---- GFSK modulator (continuous-phase Gaussian pulse; handles fractional fs/baud) ----

static std::vector<float> gaussian_phase_pulse(double bt, int L, int steps) {
	// Psi(x): integral of the unit-area Gaussian frequency pulse, normalized 0..1 across [-L/2,L/2].
	auto Q = [](double z) { return 0.5 * erfc(z / std::sqrt(2.0)); };
	const double k = 2.0 * M_PI * bt / std::sqrt(std::log(2.0));
	std::vector<double> g(steps + 1);
	double area = 0;
	for(int i = 0; i <= steps; i++) {
		double x = -L / 2.0 + (double)i * L / steps;
		g[i] = 0.5 * (Q(k * (x - 0.5)) - Q(k * (x + 0.5)));
		area += g[i];
	}
	std::vector<float> psi(steps + 1);
	double acc = 0;
	for(int i = 0; i <= steps; i++) {
		acc += g[i];
		psi[i] = (float)(acc / area);
	}
	return psi;
}

static inline double psi_eval(const std::vector<float>& psi, int L, double x) {
	if(x <= -L / 2.0) { return 0.0; }
	if(x >= L / 2.0) { return 1.0; }
	double f = (x + L / 2.0) / L * (psi.size() - 1);
	int i = (int)f;
	double frac = f - i;
	return psi[i] * (1.0 - frac) + psi[i + 1] * frac;
}

std::vector<cf> gfsk_modulate(const bits& nrz, double fs, double baud, double h, double bt) {
	const int L = 4, STEPS = 4096;
	std::vector<float> psi = gaussian_phase_pulse(bt, L, STEPS);
	int nsym = (int)nrz.size();
	if(nsym == 0) { return {}; }
	double sps = fs / baud;
	std::vector<int> prefix(nsym + 1, 0);
	for(int k = 0; k < nsym; k++) {
		prefix[k + 1] = prefix[k] + (nrz[k] ? 1 : -1);
	}
	size_t nout = (size_t)std::llround((nsym + L) * sps);
	std::vector<cf> out(nout);
	for(size_t n = 0; n < nout; n++) {
		double tsym = (double)n / sps;
		int k0 = (int)std::floor(tsym - L / 2.0);
		int k1 = (int)std::ceil(tsym + L / 2.0);
		if(k0 < 0) { k0 = 0; }
		if(k1 > nsym - 1) { k1 = nsym - 1; }
		double acc = (k0 > 0) ? (double)prefix[k0] : 0.0;
		for(int k = k0; k <= k1; k++) {
			int a = nrz[k] ? 1 : -1;
			acc += a * psi_eval(psi, L, tsym - k);
		}
		double phase = M_PI * h * acc;
		out[n] = cf((float)std::cos(phase), (float)std::sin(phase));
	}
	return out;
}

// ---- Assemble the on-air advertising bit stream ----

bits ble_build_adv_stream(const std::vector<uint8_t>& pdu, int chan_index, uint32_t crc_init) {
	bits pdu_bits = bytes_to_bits(pdu.data(), pdu.size());
	uint32_t crc = ble_crc24(pdu_bits, crc_init);
	std::vector<uint8_t> full = pdu;
	full.push_back((uint8_t)(crc & 0xFF));
	full.push_back((uint8_t)((crc >> 8) & 0xFF));
	full.push_back((uint8_t)((crc >> 16) & 0xFF));
	bits body = bytes_to_bits(full.data(), full.size());
	ble_whiten(body, chan_index);

	uint8_t pre = 0xAA;
	uint8_t aa[4] = { (uint8_t)(BLE_ADV_AA & 0xFF), (uint8_t)((BLE_ADV_AA >> 8) & 0xFF),
	                  (uint8_t)((BLE_ADV_AA >> 16) & 0xFF), (uint8_t)((BLE_ADV_AA >> 24) & 0xFF) };
	bits stream = bytes_to_bits(&pre, 1);
	bits aab = bytes_to_bits(aa, 4);
	stream.insert(stream.end(), aab.begin(), aab.end());
	stream.insert(stream.end(), body.begin(), body.end());
	return stream;
}

bits ble_build_uncoded_stream(const std::vector<uint8_t>& pdu, int chan_index, uint32_t aa,
                              int preamble_bytes, uint32_t crc_init) {
	bits pdu_bits = bytes_to_bits(pdu.data(), pdu.size());
	uint32_t crc = ble_crc24(pdu_bits, crc_init);
	std::vector<uint8_t> full = pdu;
	full.push_back((uint8_t)(crc & 0xFF));
	full.push_back((uint8_t)((crc >> 8) & 0xFF));
	full.push_back((uint8_t)((crc >> 16) & 0xFF));
	bits body = bytes_to_bits(full.data(), full.size());
	ble_whiten(body, chan_index);

	bits stream;
	for(int i = 0; i < preamble_bytes; i++) { uint8_t pre = 0xAA; bits pb = bytes_to_bits(&pre, 1); stream.insert(stream.end(), pb.begin(), pb.end()); }
	uint8_t ab[4] = { (uint8_t)(aa & 0xFF), (uint8_t)((aa >> 8) & 0xFF), (uint8_t)((aa >> 16) & 0xFF), (uint8_t)((aa >> 24) & 0xFF) };
	bits aab = bytes_to_bits(ab, 4);
	stream.insert(stream.end(), aab.begin(), aab.end());
	stream.insert(stream.end(), body.begin(), body.end());
	return stream;
}

// ---- Advertising PDU parse ----

const char* ble_pdu_type_name(int t) {
	switch(t) {
		case 0: return "ADV_IND";
		case 1: return "ADV_DIRECT_IND";
		case 2: return "ADV_NONCONN_IND";
		case 3: return "SCAN_REQ";
		case 4: return "SCAN_RSP";
		case 5: return "CONNECT_IND";
		case 6: return "ADV_SCAN_IND";
		case 7: return "ADV_EXT_IND";
		default: return "ADV_?";
	}
}

std::string ble_adva_string(const uint8_t adva[6]) {
	char s[18];
	snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x",
	         adva[5], adva[4], adva[3], adva[2], adva[1], adva[0]);
	return std::string(s);
}

// parse AD structures [len][type][data...] from AdvData starting at `off`
static void parse_ad_structures(const std::vector<uint8_t>& payload, size_t off, ble_adv_pdu& out) {
	for(size_t i = off; i + 1 < payload.size(); ) {
		int adlen = payload[i];
		if(adlen == 0 || i + 1 + adlen > payload.size()) { break; }
		int adtype = payload[i + 1];
		const uint8_t* d = &payload[i + 2];
		int dl = adlen - 1;
		switch(adtype) {
			case 0x01:
				if(dl >= 1) { out.flags = d[0]; out.has_flags = true; }
				break;
			case 0x08: case 0x09:   // short / complete local name
				out.name.assign((const char*)d, (const char*)d + dl);
				break;
			case 0x0A:
				if(dl >= 1) { out.tx_power = (int8_t)d[0]; out.has_tx_power = true; }
				break;
			case 0x19:
				if(dl >= 2) { out.appearance = (uint16_t)(d[0] | (d[1] << 8)); out.has_appearance = true; }
				break;
			case 0xFF:
				if(dl >= 2) { out.mfr_id = (uint16_t)(d[0] | (d[1] << 8)); out.has_mfr = true; }
				break;
			case 0x02: case 0x03:   // incomplete / complete 16-bit UUIDs
				for(int j = 0; j + 1 < dl; j += 2) { out.uuid16.push_back((uint16_t)(d[j] | (d[j + 1] << 8))); }
				break;
			default:
				break;
		}
		i += 1 + adlen;
	}
}

// Parse the Common Extended Advertising Payload (BLE 5 ADV_EXT_IND / AUX_ADV_IND / AUX_CHAIN_IND).
// payload[0] = ExtHdrLen[6] + AdvMode[2]; then (if ExtHdrLen>0) a flags byte + flag-selected fields
// in fixed order (AdvA,TargetA,CTEInfo,ADI,AuxPtr,SyncInfo,TxPower) + ACAD; AdvData follows.
static void parse_ext_adv(ble_adv_pdu& out) {
	out.is_ext = true;
	if(out.payload.empty()) { return; }
	int ext_hdr_len = out.payload[0] & 0x3F;
	out.adv_mode = (out.payload[0] >> 6) & 0x3;
	size_t adv_data_off = 1 + (size_t)ext_hdr_len;   // AdvData begins right after the extended header
	size_t hdr_end = adv_data_off;                   // fields must stay within the extended header
	if(ext_hdr_len >= 1 && out.payload.size() >= 2) {
		uint8_t flags = out.payload[1];
		size_t p = 2;
		auto fits = [&](size_t need) { return p + need <= out.payload.size() && p + need <= hdr_end; };
		if((flags & 0x01) && fits(6)) { for(int i = 0; i < 6; i++) { out.adva[i] = out.payload[p + i]; } out.has_adva = true; p += 6; }
		if((flags & 0x02) && fits(6)) { for(int i = 0; i < 6; i++) { out.targeta[i] = out.payload[p + i]; } out.has_target = true; p += 6; }
		if((flags & 0x04) && fits(1)) { out.has_cte = true; p += 1; }
		if((flags & 0x08) && fits(2)) { out.adi_did = (uint16_t)(out.payload[p] | ((out.payload[p + 1] & 0x0F) << 8)); out.adi_sid = (uint8_t)((out.payload[p + 1] >> 4) & 0x0F); out.has_adi = true; p += 2; }
		if((flags & 0x10) && fits(3)) { out.has_aux = true; out.aux_chan = out.payload[p] & 0x3F; out.aux_phy = (out.payload[p + 2] >> 5) & 0x7; p += 3; }
		if((flags & 0x20) && fits(18)) { out.has_syncinfo = true; p += 18; }
		if((flags & 0x40) && fits(1)) { out.tx_power = (int8_t)out.payload[p]; out.has_tx_power = true; p += 1; }
	}
	parse_ad_structures(out.payload, adv_data_off, out);
}

bool ble_parse_adv_pdu(const std::vector<uint8_t>& pdu, ble_adv_pdu& out) {
	if(pdu.size() < 2) { return false; }
	out.pdu_type = pdu[0] & 0x0F;
	out.ch_sel = (pdu[0] >> 5) & 1;
	out.tx_add = (pdu[0] >> 6) & 1;
	out.rx_add = (pdu[0] >> 7) & 1;
	// legacy adv uses a 6-bit length; extended/AUX PDUs use the full 8-bit length byte
	out.length = (out.pdu_type == 7) ? pdu[1] : (pdu[1] & 0x3F);
	if((size_t)out.length + 2 > pdu.size()) {
		out.length = (int)pdu.size() - 2;   // truncated burst; parse what we have
	}
	out.payload.assign(pdu.begin() + 2, pdu.begin() + 2 + out.length);

	if(out.pdu_type == 7) {   // ADV_EXT_IND / AUX_ADV_IND / AUX_CHAIN_IND
		parse_ext_adv(out);
		return true;
	}

	// AdvA present for ADV_IND/DIRECT/NONCONN/SCAN_RSP/ADV_SCAN_IND/CONNECT_IND
	int t = out.pdu_type;
	bool has_adva = (t == 0 || t == 1 || t == 2 || t == 4 || t == 5 || t == 6);
	size_t adv_data_off = 0;
	if(has_adva && out.payload.size() >= 6) {
		for(int i = 0; i < 6; i++) { out.adva[i] = out.payload[i]; }
		out.has_adva = true;
		adv_data_off = 6;
		if(t == 1 || t == 5) { adv_data_off = 12; }   // ADV_DIRECT_IND/CONNECT_IND: +TargetA/InitA
	}
	parse_ad_structures(out.payload, adv_data_off, out);
	return true;
}

} // namespace ismmon
