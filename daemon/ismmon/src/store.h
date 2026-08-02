// ismmon: decoded-packet store — a rolling device table (keyed by advertiser address) plus a
// recent-packet ring, serialized to JSON for the /ismapi/ HTTP surface (nginx -> the app page).
#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <sys/time.h>

#include "gfsk_rx.h"
#include "wifi.h"

namespace ismmon {

static inline double wall_now() {
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	return (double)tv.tv_sec + (double)tv.tv_usec * 1e-6;
}

static inline std::string now_hms() {
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	struct tm tmv;
	localtime_r(&tv.tv_sec, &tmv);
	char b[32];
	snprintf(b, sizeof(b), "%02d:%02d:%02d.%03d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(tv.tv_usec / 1000));
	return b;
}

static inline std::string json_escape(const std::string& s) {
	std::string o;
	o.reserve(s.size() + 4);
	for(unsigned char c : s) {
		switch(c) {
			case '"': o += "\\\""; break;
			case '\\': o += "\\\\"; break;
			case '\n': o += "\\n"; break;
			case '\r': o += "\\r"; break;
			case '\t': o += "\\t"; break;
			default:
				if(c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
				else { o += (char)c; }
		}
	}
	return o;
}

static inline std::string hexstr(const std::vector<uint8_t>& b) {
	static const char* H = "0123456789abcdef";
	std::string o;
	o.reserve(b.size() * 2);
	for(uint8_t x : b) { o += H[x >> 4]; o += H[x & 0xF]; }
	return o;
}

struct DevInfo {
	std::string addr, addr_type, name;
	int last_type = 0;
	int channel = 0;
	std::string pdu_str = "?";
	uint64_t count = 0;
	float rssi = 0;
	double cfo = 0;
	uint16_t mfr = 0; bool has_mfr = false;
	uint8_t flags = 0; bool has_flags = false;
	uint16_t appearance = 0; bool has_appearance = false;
	double first_seen = 0, last_seen = 0;
};

struct PktEntry {
	std::string time, type, addr, name, hex;
	int channel = 0, len = 0;
	float rssi = 0;
	double cfo = 0;
};

class Store {
public:
	void add(const ble_packet& p) {
		std::lock_guard<std::mutex> lk(m_);
		total_++;
		std::string addr = p.adv.has_adva ? ble_adva_string(p.adv.adva) : std::string("(no-addr)");
		double t = wall_now();
		DevInfo& d = devs_[addr];
		if(d.count == 0) {
			d.addr = addr; d.first_seen = t;
			d.addr_type = p.adv.tx_add ? "random" : "public";
		}
		d.count++; d.last_seen = t; d.rssi = p.rssi_db; d.cfo = p.cfo_hz; d.last_type = p.adv.pdu_type; d.channel = p.channel;
		d.pdu_str = ble_pdu_type_name(p.adv.pdu_type);
		if(!p.adv.name.empty()) { d.name = p.adv.name; }
		if(p.adv.has_mfr) { d.mfr = p.adv.mfr_id; d.has_mfr = true; }
		if(p.adv.has_flags) { d.flags = p.adv.flags; d.has_flags = true; }
		if(p.adv.has_appearance) { d.appearance = p.adv.appearance; d.has_appearance = true; }

		PktEntry e;
		e.time = now_hms();
		e.channel = p.channel;
		e.type = ble_pdu_type_name(p.adv.pdu_type);
		e.addr = addr; e.rssi = p.rssi_db; e.cfo = p.cfo_hz;
		e.len = (int)p.pdu.size(); e.name = p.adv.name; e.hex = hexstr(p.pdu);
		pkts_.push_back(std::move(e));
		if(pkts_.size() > MAXPKT) { pkts_.pop_front(); }
	}

	// Wi-Fi (802.11) management frame -> same device/packet tables (addr=BSSID, name=SSID)
	void add_wifi(const wifi_packet& p) {
		std::lock_guard<std::mutex> lk(m_);
		total_++;
		std::string addr = p.has_addr ? wifi_mac_string(p.bssid) : std::string("(wifi)");
		double t = wall_now();
		DevInfo& d = devs_[addr];
		if(d.count == 0) { d.addr = addr; d.first_seen = t; d.addr_type = "wifi"; }
		d.count++; d.last_seen = t; d.rssi = p.rssi_db; d.cfo = p.cfo_hz; d.channel = p.channel;
		d.pdu_str = std::string("wifi/") + p.type_name;
		if(!p.ssid.empty()) { d.name = p.ssid; }
		PktEntry e;
		e.time = now_hms();
		e.channel = p.channel;
		e.type = std::string("wifi/") + p.type_name;
		e.addr = addr; e.rssi = p.rssi_db; e.cfo = p.cfo_hz;
		e.len = p.length; e.name = p.ssid;
		pkts_.push_back(std::move(e));
		if(pkts_.size() > MAXPKT) { pkts_.pop_front(); }
	}

	std::string devices_json() {
		std::lock_guard<std::mutex> lk(m_);
		double now = wall_now();
		std::string o = "{\"devices\":[";
		bool first = true;
		for(auto& kv : devs_) {
			DevInfo& d = kv.second;
			char b[512];
			snprintf(b, sizeof(b),
			         "%s{\"addr\":\"%s\",\"type\":\"%s\",\"name\":\"%s\",\"pdu\":\"%s\",\"channel\":%d,\"count\":%llu,"
			         "\"rssi\":%.1f,\"cfo_khz\":%.0f,\"age\":%.1f",
			         first ? "" : ",", d.addr.c_str(), d.addr_type.c_str(), json_escape(d.name).c_str(),
			         d.pdu_str.c_str(), d.channel, (unsigned long long)d.count, d.rssi, d.cfo / 1e3,
			         now - d.last_seen);
			o += b;
			if(d.has_mfr) { snprintf(b, sizeof(b), ",\"mfr\":\"0x%04x\"", d.mfr); o += b; }
			if(d.has_flags) { snprintf(b, sizeof(b), ",\"flags\":\"0x%02x\"", d.flags); o += b; }
			if(d.has_appearance) { snprintf(b, sizeof(b), ",\"appearance\":%u", d.appearance); o += b; }
			o += "}";
			first = false;
		}
		o += "]}";
		return o;
	}

	std::string packets_json() {
		std::lock_guard<std::mutex> lk(m_);
		std::string o = "{\"packets\":[";
		bool first = true;
		for(auto it = pkts_.rbegin(); it != pkts_.rend(); ++it) {
			char b[640];
			snprintf(b, sizeof(b),
			         "%s{\"time\":\"%s\",\"ch\":%d,\"type\":\"%s\",\"addr\":\"%s\",\"rssi\":%.1f,"
			         "\"cfo_khz\":%.0f,\"len\":%d,\"name\":\"%s\",\"hex\":\"%s\"}",
			         first ? "" : ",", it->time.c_str(), it->channel, it->type.c_str(), it->addr.c_str(),
			         it->rssi, it->cfo / 1e3, it->len, json_escape(it->name).c_str(), it->hex.c_str());
			o += b;
			first = false;
		}
		o += "]}";
		return o;
	}

	uint64_t total() { std::lock_guard<std::mutex> lk(m_); return total_; }
	size_t device_count() { std::lock_guard<std::mutex> lk(m_); return devs_.size(); }

private:
	std::mutex m_;
	std::map<std::string, DevInfo> devs_;
	std::deque<PktEntry> pkts_;
	uint64_t total_ = 0;
	static constexpr size_t MAXPKT = 300;
};

} // namespace ismmon
