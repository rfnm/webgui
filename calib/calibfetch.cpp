// rfnm-calib-fetch: oneshot factory-calibration distributor. For every daughterboard EEPROM
// found in sysfs it GETs https://api.rfnm.me/calib/<serial> and stores the raw blob at
// /lib/firmware/rfnm/calib_<SERIAL>.json (atomic tmp+rename; the kernel request_firmware
// consumer comes later).
// From the PRIMARY slot's blob it derives vsgd's /rfnm/vsg/calib_pwr.tbl
// (dBm at SMA, -6 dBFS CW anchor); boards with no pwr surface
// (v0 recipe) fall back to the shipped fleet-median table. A per-board table (SRC board) is
// never overwritten by fleet data. Outcome ledger -> /rfnm/calib/status.json for appd's
// GET /api/v1/hwinfo and the web GUI daughterboard panel.
//
// Exit 0 = every present daughterboard resolved (fetched, kept current, or known-unregistered);
// exit 2 = transport failure with nothing cached (the systemd timer retries).
#include <dirent.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <map>
#include <string>
#include <vector>

#define DEF_SYSFS "/sys/bus/i2c/devices"
#define DEF_API "api.rfnm.me"   // scheme-less = https-then-http fallback; a scheme pins it
#define DEF_FWDIR "/lib/firmware/rfnm"
#define DEF_STATUS "/rfnm/calib/status.json"
#define DEF_VSGTBL "/rfnm/vsg/calib_pwr.tbl"

// rfnm-shared.h RFNM_BOARD_ID_TO_USER_READABLE_NAME
static const char *board_name(int id) {
	static const char *n[] = { "", "breakout", "granita", "lime", "motherboard", "yucca" };
	return (id >= 1 && id <= 5) ? n[id] : "unknown";
}

struct binfo {
	int adapter = -1;
	int id = 0;
	int rev = 0;
	std::string serial;
	std::string mac;
	std::string fetch;		// ok | current | unknown-serial | http-N | no-network | invalid-reply | no-eeprom
	int pwr_points = 0;
};

static std::string read_file(const std::string &path) {
	FILE *f = fopen(path.c_str(), "r");
	if(!f) {
		return "";
	}
	std::string out;
	char buf[65536];
	size_t r;
	while((r = fread(buf, 1, sizeof(buf), f)) > 0) {
		out.append(buf, r);
	}
	fclose(f);
	return out;
}

static bool write_atomic(const std::string &path, const std::string &data) {
	std::string tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "w");
	if(!f) {
		return false;
	}
	size_t w = fwrite(data.data(), 1, data.size(), f);
	int c = fclose(f);
	if(w != data.size() || c != 0) {
		unlink(tmp.c_str());
		return false;
	}
	return rename(tmp.c_str(), path.c_str()) == 0;
}

// "board id 3 revision 2 serial AB12CD34 mac-addr aa:bb:.." (mac only on adapter 0)
static bool parse_board_info(const std::string &line, binfo *b) {
	char serial[32] = {};
	char mac[32] = {};
	int n = sscanf(line.c_str(), "board id %d revision %d serial %31s mac-addr %31s", &b->id, &b->rev, serial, mac);
	if(n < 3) {
		return false;
	}
	for(char *p = serial; *p; p++) {
		if(!isalnum((unsigned char)*p)) {
			return false;
		}
	}
	b->serial = serial;
	if(n == 4) {
		b->mac = mac;
	}
	return b->serial.size() == 8;
}

// curl exit -> compact status tag (mirrors ddnsd's transport_err; ledger-friendly one-worders)
static const char *transport_tag(int ec) {
	switch(ec) {
	case 127: return "curl-missing";
	case 6:   return "dns-failure";
	case 7:   return "connect-refused";
	case 28:  return "timeout";
	case 35:
	case 60:  return "tls-failure";
	default:  return "no-network";
	}
}

// GET via curl (the ddnsd pattern: boards ship curl, no TLS lib linked here); code 0 = transport
// failure with *ec = curl exit
// ---- identity via appd (primary source): GET 127.0.0.1:7683/v1/hwinfo serves the EEPROM
// scan appd made once at its own startup; direct sysfs is the fallback/test hook.
// Post-#76 the kernel serves probe-time cached identity from those nodes (never the
// i2c wire), so either path is safe at any rate.
static void boards_from_hwinfo_json(const std::string &body, std::vector<binfo> *boards) {
	size_t arr = body.find("\"boards\":[");
	if(arr == std::string::npos) {
		return;
	}
	size_t p = arr + 10;
	while(p < body.size() && body[p] != ']') {
		if(body[p] != '{') {
			p++;
			continue;
		}
		size_t q = p;
		int d = 0;
		do {
			if(body[q] == '{') {
				d++;
			} else if(body[q] == '}') {
				d--;
			}
			q++;
		} while(q < body.size() && d > 0);
		std::string obj = body.substr(p, q - p);
		p = q;
		auto geti = [&obj](const char *key, int def) -> int {
			size_t k = obj.find(std::string("\"") + key + "\":");
			return k == std::string::npos ? def : atoi(obj.c_str() + k + strlen(key) + 3);
		};
		auto gets = [&obj](const char *key) -> std::string {
			size_t k = obj.find(std::string("\"") + key + "\":\"");
			if(k == std::string::npos) {
				return "";
			}
			k += strlen(key) + 4;
			size_t e = obj.find('"', k);
			return e == std::string::npos ? "" : obj.substr(k, e - k);
		};
		binfo b;
		b.adapter = obj.find("\"motherboard\":true") != std::string::npos ? 0 : geti("slot", -1) + 1;
		if(b.adapter < 0) {
			continue;
		}
		if(obj.find("\"present\":false") != std::string::npos) {
			b.fetch = "no-eeprom";
			boards->push_back(b);
			continue;
		}
		b.id = geti("id", 0);
		b.rev = geti("rev", 0);
		b.serial = gets("serial");
		b.mac = gets("mac");
		boards->push_back(b);
	}
}

static int http_get(const std::string &url, std::string *body, int *ec) {
	*ec = -1;
	for(char c : url) {
		if(!(isalnum((unsigned char)c) || strchr(":/.-_", c))) {
			return 0;
		}
	}
	std::string cmd = "curl -s -m 25 -w '\\n%{http_code}' '" + url + "' 2>/dev/null";
	FILE *p = popen(cmd.c_str(), "r");
	if(!p) {
		return 0;
	}
	std::string out;
	char buf[8192];
	size_t r;
	while((r = fread(buf, 1, sizeof(buf), p)) > 0) {
		out.append(buf, r);
		if(out.size() > 4u * 1024 * 1024) {
			break;		// blobs are ~100 KB; runaway = broken
		}
	}
	int st = pclose(p);
	*ec = (st >= 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
	size_t nl = out.rfind('\n');
	if(nl == std::string::npos) {
		return 0;
	}
	*body = out.substr(0, nl);
	return atoi(out.c_str() + nl + 1);
}

// scheme fallback: https first (443-only networks), plain http second (no-CA images)
static int http_get_fallback(const std::string &host_path, std::string *body, int *ec) {
	int code = http_get("https://" + host_path, body, ec);
	if(code != 0 || *ec == 127) {
		return code;
	}
	int ec2 = -1;
	code = http_get("http://" + host_path, body, &ec2);
	if(code == 0 && *ec == 0) {
		*ec = ec2;	// prefer a meaningful exit over a zero
	}
	return code;
}

// scan the raw blob for ["pwr", setting, freq_mhz, dbm] rows (machine-generated, stable shape)
static std::map<int, std::map<double, double>> scan_pwr(const std::string &blob) {
	std::map<int, std::map<double, double>> pwr;
	size_t pos = 0;
	while((pos = blob.find("[\"pwr\",", pos)) != std::string::npos) {
		const char *p = blob.c_str() + pos + 7;
		char *end;
		double s = strtod(p, &end);
		if(end == p || *end != ',') {
			pos++;
			continue;
		}
		p = end + 1;
		double f = strtod(p, &end);
		if(end == p || *end != ',') {
			pos++;
			continue;
		}
		p = end + 1;
		double d = strtod(p, &end);
		if(end == p || *end != ']') {
			pos++;
			continue;
		}
		pwr[(int)s][f] = d;
		pos = (size_t)(end - blob.c_str());
	}
	return pwr;
}

static bool ci_contains(const std::string &hay, const std::string &needle) {
	auto low = [](std::string s) { for(char &c : s) { c = (char)tolower((unsigned char)c); } return s; };
	return low(hay).find(low(needle)) != std::string::npos;
}

// emit calib_table.py --vsg format byte-compatibly (sorted by setting then freq)
static std::string emit_vsg_tbl(const std::string &serial, const char *src, const std::map<int, std::map<double, double>> &pwr) {
	std::string out = "# RFNM VSG TX power table v1 (calib_table.py --vsg semantics: dBm at SMA, -6 dBFS CW)\nSERIAL " + serial + "\nSRC " + src + "\n";
	char line[96];
	for(const auto &s : pwr) {
		for(const auto &fq : s.second) {
			snprintf(line, sizeof(line), "PWR %.1f %d %.2f\n", fq.first, s.first, fq.second);
			out += line;
		}
	}
	return out;
}

static std::string json_escape(const std::string &s) {
	std::string o;
	for(char c : s) {
		if(c == '"' || c == '\\') {
			o += '\\';
			o += c;
		} else if((unsigned char)c >= 0x20) {
			o += c;
		}
	}
	return o;
}

static int selftest(void);

int main(int argc, char **argv) {
	std::string sysfs = DEF_SYSFS, api = DEF_API, fwdir = DEF_FWDIR, status_path = DEF_STATUS, vsgtbl = DEF_VSGTBL;
	for(int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if(a == "--selftest") {
			return selftest();
		} else if(a == "--sysfs" && i + 1 < argc) {
			sysfs = argv[++i];
		} else if(a == "--api" && i + 1 < argc) {
			api = argv[++i];
		} else if(a == "--out" && i + 1 < argc) {
			fwdir = argv[++i];
		} else if(a == "--status" && i + 1 < argc) {
			status_path = argv[++i];
		} else if(a == "--vsg-tbl" && i + 1 < argc) {
			vsgtbl = argv[++i];
		} else {
			fprintf(stderr, "usage: rfnm-calib-fetch [--sysfs d] [--api url] [--out d] [--status f] [--vsg-tbl f] [--selftest]\n");
			return 1;
		}
	}

	std::vector<binfo> boards;
	bool sysfs_hook = sysfs != DEF_SYSFS;   // an explicit --sysfs is a test hook: walk that dir
	if(!sysfs_hook) {
		std::string hw;
		int hwec = 0;
		if(http_get("http://127.0.0.1:7683/v1/hwinfo", &hw, &hwec) == 200) {
			boards_from_hwinfo_json(hw, &boards);
		}
	}
	// fallback / test hook: the sysfs nodes directly. Post-#76 kernel contract: they
	// serve the probe-time cached identity, never the wire - safe at any rate.
	for(int a = 0; (sysfs_hook || boards.empty()) && a <= 2; a++) {
		binfo b;
		b.adapter = a;
		char path[256];
		snprintf(path, sizeof(path), "%s/%d-0050/rfnm_show_board_info", sysfs.c_str(), a);
		std::string line = read_file(path);
		if(line.empty() || !parse_board_info(line, &b)) {
			if(a > 0 && !line.empty()) {
				b.fetch = "no-eeprom";	// slot answered but unparsable ("Failed to read eeprom" = empty slot)
			}
			if(a == 0 || !line.empty()) {
				// motherboard row kept even without parse; silent-absent adapters skipped
			}
			if(!line.empty()) {
				boards.push_back(b);
			}
			continue;
		}
		boards.push_back(b);
	}

	mkdir(fwdir.c_str(), 0755);
	bool transport_fail = false;
	binfo *primary = nullptr;
	for(auto &b : boards) {
		if(b.adapter == 0 || b.serial.empty()) {
			continue;		// motherboard identity is ddnsd's business; only DGBs have calib
		}
		std::string upper = b.serial;
		for(char &c : upper) {
			c = (char)toupper((unsigned char)c);
		}
		std::string dst = fwdir + "/calib_" + upper + ".json";
		std::string body;
		int ec = -1;
		int code = api.find("://") == std::string::npos ? http_get_fallback(api + "/calib/" + upper, &body, &ec)
		                                                : http_get(api + "/calib/" + upper, &body, &ec);
		if(code == 200 && body.find("\"ok\":true") != std::string::npos && ci_contains(body, "\"serial\":\"" + upper + "\"")) {
			b.fetch = write_atomic(dst, body) ? "ok" : "write-failed";
		} else if(code == 200) {
			b.fetch = "invalid-reply";
		} else if(code == 404) {
			b.fetch = "unknown-serial";	// not factory-registered (prototype) - retried daily, not an error
		} else if(code == 0) {
			b.fetch = access(dst.c_str(), R_OK) == 0 ? "current" : transport_tag(ec);	// keep cached blob
			if(b.fetch != "current") {
				transport_fail = true;
			}
		} else {
			char e[24];
			snprintf(e, sizeof(e), "http-%d", code);
			b.fetch = e;
			transport_fail = true;
		}
		std::string blob = read_file(dst);
		if(!blob.empty()) {
			auto pwr = scan_pwr(blob);
			b.pwr_points = 0;
			for(const auto &s : pwr) {
				b.pwr_points += (int)s.second.size();
			}
		}
		if(b.adapter == 1) {
			primary = &b;
		}
		printf("rfnm-calib-fetch: slot %d %s %s: %s (%d pwr points)\n", b.adapter - 1, board_name(b.id), upper.c_str(), b.fetch.c_str(), b.pwr_points);
	}

	// vsgd table from the PRIMARY slot (dgb 0 = the slot vsgd drives)
	std::string vsg_src, vsg_serial;
	int vsg_points = 0;
	if(primary && !primary->serial.empty()) {
		std::string upper = primary->serial;
		for(char &c : upper) {
			c = (char)toupper((unsigned char)c);
		}
		auto pwr = scan_pwr(read_file(fwdir + "/calib_" + upper + ".json"));
		std::string existing = read_file(vsgtbl);
		if(pwr.size() >= 2) {
			std::string tbl = emit_vsg_tbl(upper, "board", pwr);
			if(existing != tbl && write_atomic(vsgtbl, tbl)) {
				printf("rfnm-calib-fetch: wrote %s (SRC board, serial %s)\n", vsgtbl.c_str(), upper.c_str());
			}
			vsg_src = "board";
			vsg_serial = upper;
			for(const auto &s : pwr) {
				vsg_points += (int)s.second.size();
			}
		} else if(existing.find("\nSRC board") != std::string::npos) {
			vsg_src = "board";		// keep: never clobber a per-board table with fleet data
			vsg_serial = "existing";
		} else {
			std::string fleet = read_file(fwdir + "/fleet_median_vsg_" + board_name(primary->id) + ".tbl");
			if(!fleet.empty()) {
				if(existing != fleet && write_atomic(vsgtbl, fleet)) {
					printf("rfnm-calib-fetch: wrote %s (SRC fleet median, %s)\n", vsgtbl.c_str(), board_name(primary->id));
				}
				vsg_src = "fleet";
			} else {
				vsg_src = existing.empty() ? "none" : "existing";
			}
		}
	}

	std::string st = "{\"when\":" + std::to_string((long)time(nullptr)) + ",\"boards\":[";
	bool first = true;
	for(const auto &b : boards) {
		if(!first) {
			st += ",";
		}
		first = false;
		st += "{" + std::string(b.adapter == 0 ? "" : "\"slot\":" + std::to_string(b.adapter - 1) + ",") + "\"type\":\"" + board_name(b.id) + "\",\"id\":" + std::to_string(b.id)
			+ ",\"rev\":" + std::to_string(b.rev) + ",\"serial\":\"" + json_escape(b.serial) + "\""
			+ (b.mac.empty() ? "" : ",\"mac\":\"" + json_escape(b.mac) + "\"")
			+ (b.adapter == 0 ? "" : ",\"fetch\":\"" + json_escape(b.fetch) + "\",\"pwr_points\":" + std::to_string(b.pwr_points)) + "}";
	}
	st += "],\"vsg_tbl\":{\"src\":\"" + vsg_src + "\",\"serial\":\"" + json_escape(vsg_serial) + "\",\"points\":" + std::to_string(vsg_points) + "}}\n";
	char sdir[256];
	snprintf(sdir, sizeof(sdir), "%s", status_path.c_str());
	char *slash = strrchr(sdir, '/');
	if(slash) {
		*slash = 0;
		mkdir(sdir, 0755);
	}
	write_atomic(status_path, st);
	return transport_fail ? 2 : 0;
}

// ---- selftest: parse + conversion pinned against golden shapes (no network, no sysfs) ----
static int selftest(void) {
	int fails = 0;
	auto chk = [&](bool ok, const char *what) {
		printf("%s %s\n", ok ? "PASS" : "FAIL", what);
		if(!ok) {
			fails++;
		}
	};
	binfo b;
	chk(parse_board_info("board id 3 revision 2 serial AB12CD34 mac-addr aa:bb:cc:dd:ee:ff\n", &b) && b.id == 3 && b.rev == 2 && b.serial == "AB12CD34" && b.mac == "aa:bb:cc:dd:ee:ff", "mb line parse");
	binfo d;
	chk(parse_board_info("board id 5 revision 1 serial XY98ZW76\n", &d) && d.id == 5 && d.mac.empty(), "dgb line parse");
	binfo e;
	chk(!parse_board_info("Failed to read eeprom\n", &e), "empty-slot line rejected");
	chk(!parse_board_info("board id 3 revision 2 serial BAD!SER\n", &e), "junk serial rejected");
	{
		std::vector<binfo> hb;
		boards_from_hwinfo_json("{\"boards\":[{\"motherboard\":true,\"present\":true,\"type\":\"motherboard\",\"id\":4,\"rev\":1,\"serial\":\"XY98ZW76\",\"mac\":\"00:04:9f:00:00:01\"},"
			"{\"slot\":0,\"present\":true,\"type\":\"lime\",\"id\":3,\"rev\":1,\"serial\":\"QQ11RR22\",\"calib_file\":null},{\"slot\":1,\"present\":false}],\"calib_fetch\":null}", &hb);
		chk(hb.size() == 3 && hb[0].adapter == 0 && hb[0].serial == "XY98ZW76" && hb[0].mac == "00:04:9f:00:00:01"
			&& hb[1].adapter == 1 && hb[1].id == 3 && hb[1].serial == "QQ11RR22"
			&& hb[2].adapter == 2 && hb[2].fetch == "no-eeprom" && hb[2].serial.empty(), "appd hwinfo identity parse");
	}
	chk(std::string(board_name(3)) == "lime" && std::string(board_name(5)) == "yucca" && std::string(board_name(9)) == "unknown", "board id names");
	// golden fragment in the live /calib reply shape (2026-07-19)
	std::string blob = "{\"ok\":true,\"type\":\"lime\",\"serial\":\"AB12CD34\",\"cal\":[{\"cal_version\":2},[\"loop\",50,42.23],[\"pwr\",40,50,17.681822],[\"pwr\",20,50,17.79],[\"pwr\",-13,75,3.45],[\"pwr\",40,75,18.1]]}";
	auto pwr = scan_pwr(blob);
	chk(pwr.size() == 3 && pwr[40].size() == 2 && pwr[40][50] > 17.68 && pwr[40][50] < 17.69 && pwr[-13][75] == 3.45, "pwr scan");
	std::string tbl = emit_vsg_tbl("AB12CD34", "board", pwr);
	chk(tbl.find("SERIAL AB12CD34\nSRC board\nPWR 75.0 -13 3.45\nPWR 50.0 20 17.79\nPWR 50.0 40 17.68\nPWR 75.0 40 18.10\n") != std::string::npos, "tbl emit ordering + format");
	chk(scan_pwr("{\"cal\":[[\"pwr\",broken]]}").empty() && scan_pwr("").empty(), "malformed pwr rows ignored");
	chk(ci_contains("\"serial\":\"AB12CD34\"", "\"serial\":\"ab12cd34\""), "ci serial match");
	chk(std::string(transport_tag(127)) == "curl-missing" && std::string(transport_tag(6)) == "dns-failure"
		&& std::string(transport_tag(7)) == "connect-refused" && std::string(transport_tag(28)) == "timeout"
		&& std::string(transport_tag(60)) == "tls-failure" && std::string(transport_tag(3)) == "no-network", "transport tags");
	printf(fails ? "SELFTEST FAIL (%d)\n" : "SELFTEST OK\n", fails);
	return fails ? 1 : 0;
}
