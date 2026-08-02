// RFNM dynamic-DNS client daemon: registers <board-name>.rfnm.me -> this board's IP against the
// rfnm.me API. A board with no stored name self-registers as <serial>.rfnm.me (default-on distribution).
// HTTP control on 127.0.0.1:7685 (appd generates nginx route /api/app/ddns/ -> here) for the web GUI settings view:
//   GET  /config                                  current config + last registration status (JSON)
//   GET  /set?enabled=1&name=<board-name>&mode=local|public   apply config, register/release now, return status
//   (empty name + enabled=1 is valid: EEPROM keeps '', the board runs under its serial)
// Registration is re-run at startup and whenever `ip -4 monitor addr` reports an address change.
// Config persists in the motherboard EEPROM user block, owned entirely by the kernel
// (i2c-rfnm-bootconfig.c settings framework): we read/write named settings under
// /sys/.../rfnm_settings/ and never touch raw bytes — settings survive rootfs reflashes.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <map>

#define DDNSD_PORT 7685
#define UCFG_DIR "/sys/bus/i2c/devices/0-0050/rfnm_settings/"
#define API_BASE "http://api.rfnm.me"
#define BOOT_RETRIES 5
#define RETRY_DELAY_S 10
#define SLOW_RETRY_DELAY_S 300

struct ddns_cfg {
	bool enabled = false;
	std::string name;
	std::string mode = "local";
};

struct ddns_status {
	time_t when = 0;		// last attempt, 0 = never
	int http = -1;			// -1 never attempted, 0 transport failure, else HTTP code
	bool ok = false;
	std::string api_body;	// raw server JSON (or transport error text)
	bool pending = false;
};

static std::mutex g_mtx;				// guards g_cfg + g_stat
static ddns_cfg g_cfg;
static ddns_status g_stat;
static std::mutex g_kick_mtx;
static std::condition_variable g_kick_cv;
static uint32_t g_kick = 0;
static std::atomic<bool> g_shutdown{false};
static std::mutex g_curl_mtx;			// one API call at a time

static bool valid_name(const std::string &n) {
	if(n.empty() || n.size() > 16) {	// EEPROM nickname field is 16 chars
		return false;
	}
	for(char c : n) {
		if(!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
			return false;
		}
	}
	return n.front() != '-' && n.back() != '-';
}

// motherboard EEPROM serial (the 8-char sticker serial, same as librfnm's motherboard.serial),
// via the U-Boot-populated bootconfig block: rfnm-shared.h struct rfnm_bootconfig at 0xA3400000,
// motherboard_eeprom.serial_number at offset 4 + (4+6+6+1+1+5) = 27
#define RFNM_BOOTCONFIG_PHYADDR 0xA3400000UL
#define RFNM_MB_SERIAL_OFFSET 27

static std::string board_serial(void) {
	static std::string cached;
	if(!cached.empty()) {
		return cached;
	}
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if(fd < 0) {
		return "";
	}
	void *m = mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd, RFNM_BOOTCONFIG_PHYADDR);
	close(fd);
	if(m == MAP_FAILED) {
		return "";
	}
	const volatile char *p = (const volatile char *)m + RFNM_MB_SERIAL_OFFSET;	// byte reads only: device memory
	std::string s;
	for(int i = 0; i < 8; i++) {
		char c = (char)tolower((unsigned char)p[i]);
		if(!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
			s.clear();
			break;
		}
		s += c;
	}
	munmap(m, 4096);
	cached = s;
	return cached;
}

static std::string local_ip(void) {
	FILE *p = popen("ip -4 route get 1.1.1.1 2>/dev/null", "r");
	if(!p) {
		return "";
	}
	char line[512] = {};
	if(!fgets(line, sizeof(line), p)) {
		pclose(p);
		return "";
	}
	pclose(p);
	const char *src = strstr(line, " src ");
	if(!src) {
		return "";
	}
	src += 5;
	std::string ip;
	while(*src && *src != ' ' && *src != '\n') {
		char c = *src++;
		if(!((c >= '0' && c <= '9') || c == '.')) {
			return "";
		}
		ip += c;
	}
	struct in_addr ia;
	if(inet_pton(AF_INET, ip.c_str(), &ia) != 1) {
		return "";
	}
	return ip;
}

// the kernel owns the EEPROM block; we only read/write named settings under rfnm_settings/
static bool setting_read(const char *name, std::string *out) {
	FILE *f = fopen((std::string(UCFG_DIR) + name).c_str(), "r");
	if(!f) {
		return false;
	}
	char buf[64] = {};
	size_t r = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	while(r && (buf[r - 1] == '\n' || buf[r - 1] == ' ')) {
		buf[--r] = 0;
	}
	*out = buf;
	return true;
}

static bool setting_write(const char *name, const std::string &val) {
	FILE *f = fopen((std::string(UCFG_DIR) + name).c_str(), "w");
	if(!f) {
		return false;
	}
	std::string s = val + "\n";
	size_t w = fwrite(s.data(), 1, s.size(), f);
	int ret = fclose(f);	// kernel-side validation/eeprom errors surface on write/close
	return w == s.size() && ret == 0;
}

static void cfg_load(void) {
	std::string en, pub, nick;
	if(!setting_read("ddns_enabled", &en) || !setting_read("ddns_public", &pub) || !setting_read("ddns_nickname", &nick)) {
		return;		// settings framework not available: stay at defaults (disabled)
	}
	std::lock_guard<std::mutex> lk(g_mtx);
	g_cfg.name = valid_name(nick) ? nick : "";
	g_cfg.enabled = en == "1";
	g_cfg.mode = pub == "1" ? "public" : "local";
	// Default-on distribution (2026-07-19): the kernel setting ddns_enabled defaults to 1 on a
	// virgin EEPROM block; with no nickname stored the board self-names as its serial, so a new
	// board is reachable at <serial>.rfnm.me out of the box. An explicit user 0 persists off.
	if(g_cfg.enabled && g_cfg.name.empty()) {
		g_cfg.name = board_serial();
		if(!valid_name(g_cfg.name)) {
			g_cfg.name = "";
			g_cfg.enabled = false;
		}
	}
}

// nickname first, enabled last: a failure mid-sequence can't leave "enabled with a wrong name"
static bool cfg_save(void) {
	ddns_cfg cfg;
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		cfg = g_cfg;
	}
	return setting_write("ddns_nickname", cfg.name)
		&& setting_write("ddns_public", cfg.mode == "public" ? "1" : "0")
		&& setting_write("ddns_enabled", cfg.enabled ? "1" : "0");
}

// curl exit code -> a diagnosis a human can act on. The old blanket "no response (network
// down?)" collapsed four distinct failures (2026-07-19 field report) - never regress this.
static std::string transport_err(int ec) {
	switch(ec) {
	case 127: return "curl is not installed on this image";
	case 6:   return "DNS failure: cannot resolve api.rfnm.me (check the network's DNS server)";
	case 7:   return "cannot connect to api.rfnm.me (network egress blocked?)";
	case 28:  return "connection to api.rfnm.me timed out (firewall?)";
	case 35:
	case 60:  return "TLS failure talking to api.rfnm.me (board clock or CA store?)";
	default:  return "no response from api.rfnm.me (curl exit " + std::to_string(ec) + ")";
	}
}

// one curl attempt against one scheme; returns HTTP code, 0 = transport failure (+*ec set)
static int curl_once(const std::string &base, const std::string &endpoint, const std::string &form, std::string *body_out, int *ec) {
	std::string cmd = "curl -s -m 15 -w '\\n%{http_code}' --data '" + form + "' " + base + endpoint + " 2>/dev/null";
	FILE *p = popen(cmd.c_str(), "r");
	if(!p) {
		*ec = -1;
		*body_out = "popen failed";
		return 0;
	}
	std::string out;
	char buf[1024];
	size_t r;
	while((r = fread(buf, 1, sizeof(buf), p)) > 0) {
		out.append(buf, r);
	}
	int st = pclose(p);
	*ec = (st >= 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
	size_t nl = out.rfind('\n');
	if(nl == std::string::npos) {
		*body_out = transport_err(*ec);
		return 0;
	}
	int code = atoi(out.c_str() + nl + 1);
	*body_out = out.substr(0, nl);
	while(!body_out->empty() && (body_out->back() == '\n' || body_out->back() == '\r' || body_out->back() == ' ')) {
		body_out->pop_back();	// server body ends with \n; status_json's embed-as-object check needs it gone
	}
	if(code == 0) {
		*body_out = transport_err(*ec);
	}
	return code;
}

// API call with scheme fallback: https first (some networks only allow 443 out), plain http
// second (covers images without a CA store; the server answers both, no redirect). Endpoint/
// args are validated ([a-z0-9-.=&] only) before shelling out.
static int api_call(const std::string &endpoint, const std::string &form, std::string *body_out) {
	int ec_https = 0;
	int code = curl_once("https://api.rfnm.me", endpoint, form, body_out, &ec_https);
	if(code != 0) {
		return code;
	}
	if(ec_https == 127) {
		return 0;	// no curl: the http retry can only fail identically
	}
	std::string https_err = *body_out;
	int ec_http = 0;
	code = curl_once(API_BASE, endpoint, form, body_out, &ec_http);
	if(code == 0) {
		*body_out = "https: " + https_err + "; http: " + *body_out;
	}
	return code;
}

// one registration (or release) attempt; updates g_stat, returns HTTP code (0 = transport failure)
static int do_register(void) {
	ddns_cfg cfg;
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		cfg = g_cfg;
		g_stat.pending = true;
	}
	std::string serial = board_serial();
	std::string body;
	int code;
	{
		std::lock_guard<std::mutex> lk(g_curl_mtx);
		if(serial.empty()) {
			code = 0;
			body = "no board serial in devicetree";
		} else if(!cfg.enabled || cfg.name.empty()) {
			code = api_call("/release", "serial=" + serial, &body);
			if(code == 404) {	// nothing registered = released, fine
				code = 200;
				body = "{\"ok\":true}";
			}
		} else {
			std::string form = "serial=" + serial + "&name=" + cfg.name + "&mode=" + cfg.mode;
			if(cfg.mode == "local") {
				std::string ip = local_ip();
				if(ip.empty()) {
					std::lock_guard<std::mutex> lk2(g_mtx);
					g_stat = { time(nullptr), 0, false, "no local IPv4 address", false };
					return 0;
				}
				form += "&ip=" + ip;
			}
			code = api_call("/update", form, &body);
		}
	}
	std::lock_guard<std::mutex> lk(g_mtx);
	g_stat = { time(nullptr), code, code == 200, body, false };
	return code;
}

static void worker_thread(void) {
	uint32_t seen = 0;
	while(!g_shutdown) {
		{
			std::unique_lock<std::mutex> lk(g_kick_mtx);
			g_kick_cv.wait(lk, [&] { return g_kick != seen || g_shutdown; });
			if(g_shutdown) {
				return;
			}
			seen = g_kick;
		}
		sleep(2);	// debounce address-change bursts (dhcp renew = del + add)
		{
			std::lock_guard<std::mutex> lk(g_kick_mtx);
			seen = g_kick;
		}
		{
			std::lock_guard<std::mutex> lk(g_mtx);
			if(!g_cfg.enabled || g_cfg.name.empty()) {
				continue;
			}
		}
		// retry until the server is reached (4xx/5xx = config/server issue, retrying won't help):
		// quick 10 s retries first, then every 5 min — a board whose internet comes up late (or
		// behind a resolver with stale delegation) must still register without a reboot. Once a
		// call gets through this goes quiet; it is not a periodic re-registration timer.
		for(int attempt = 0; !g_shutdown; attempt++) {
			int code = do_register();
			if(code != 0) {
				break;
			}
			int delay = attempt < BOOT_RETRIES ? RETRY_DELAY_S : SLOW_RETRY_DELAY_S;
			bool rekicked = false;
			for(int s = 0; s < delay && !g_shutdown && !rekicked; s++) {
				sleep(1);
				std::lock_guard<std::mutex> lk(g_kick_mtx);
				rekicked = g_kick != seen;
			}
			if(rekicked) {
				break;	// new event queued, restart cycle
			}
		}
	}
}

static void kick(void) {
	{
		std::lock_guard<std::mutex> lk(g_kick_mtx);
		g_kick++;
	}
	g_kick_cv.notify_all();
}

static void monitor_thread(void) {
	while(!g_shutdown) {
		FILE *p = popen("ip -4 monitor addr 2>/dev/null", "r");
		if(!p) {
			sleep(30);
			continue;
		}
		char line[512];
		while(fgets(line, sizeof(line), p)) {
			kick();
		}
		pclose(p);
		sleep(5);	// ip died? reopen
	}
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

static std::string status_json(void) {
	std::lock_guard<std::mutex> lk(g_mtx);
	std::string api = g_stat.api_body;
	std::string api_field;
	if(!api.empty() && api[0] == '{' && api.back() == '}') {
		api_field = api;	// server JSON, embed as object
	} else {
		api_field = "{\"ok\":false,\"error\":\"" + json_escape(api) + "\"}";
	}
	std::string j = "{\"enabled\":" + std::string(g_cfg.enabled ? "true" : "false")
		+ ",\"name\":\"" + json_escape(g_cfg.name) + "\""
		+ ",\"mode\":\"" + g_cfg.mode + "\""
		+ ",\"serial\":\"" + json_escape(board_serial()) + "\""
		+ ",\"local_ip\":\"" + json_escape(local_ip()) + "\""
		+ ",\"last\":{\"http\":" + std::to_string(g_stat.http)
		+ ",\"ok\":" + (g_stat.ok ? "true" : "false")
		+ ",\"pending\":" + (g_stat.pending ? "true" : "false")
		+ ",\"age\":" + std::to_string(g_stat.when ? (long)(time(nullptr) - g_stat.when) : -1)
		+ ",\"api\":" + api_field + "}}";
	return j;
}

static std::string url_decode(const std::string &s) {
	std::string o;
	for(size_t i = 0; i < s.size(); i++) {
		if(s[i] == '%' && i + 2 < s.size()) {
			char h[3] = { s[i + 1], s[i + 2], 0 };
			o += (char)strtol(h, nullptr, 16);
			i += 2;
		} else if(s[i] == '+') {
			o += ' ';
		} else {
			o += s[i];
		}
	}
	return o;
}

static std::map<std::string, std::string> parse_query(const std::string &q) {
	std::map<std::string, std::string> kv;
	size_t pos = 0;
	while(pos < q.size()) {
		size_t amp = q.find('&', pos);
		if(amp == std::string::npos) {
			amp = q.size();
		}
		std::string pair = q.substr(pos, amp - pos);
		size_t eq = pair.find('=');
		if(eq != std::string::npos) {
			kv[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
		}
		pos = amp + 1;
	}
	return kv;
}

static void http_client(int fd) {
	std::string req;
	char buf[4096];
	while(req.find("\r\n\r\n") == std::string::npos) {
		ssize_t r = recv(fd, buf, sizeof(buf), 0);
		if(r <= 0) {
			close(fd);
			return;
		}
		req.append(buf, (size_t)r);
		if(req.size() > 65536) {
			close(fd);
			return;
		}
	}
	size_t sp1 = req.find(' ');
	size_t sp2 = req.find(' ', sp1 + 1);
	std::string uri = req.substr(sp1 + 1, sp2 - sp1 - 1);
	std::string path = uri;
	std::string query;
	size_t qm = uri.find('?');
	if(qm != std::string::npos) {
		path = uri.substr(0, qm);
		query = uri.substr(qm + 1);
	}

	std::string body;
	int code = 200;

	if(path == "/config") {
		body = status_json();
	} else if(path == "/set") {
		auto kv = parse_query(query);
		std::string name = kv.count("name") ? kv["name"] : "";
		std::string mode = kv.count("mode") ? kv["mode"] : "local";
		bool enabled = kv.count("enabled") && kv["enabled"] == "1";
		if(enabled && !name.empty() && !valid_name(name)) {
			code = 400;
			body = "{\"ok\":false,\"error\":\"invalid board name: 1-16 chars, a-z 0-9 and interior hyphens\"}";
		} else if(mode != "local" && mode != "public") {
			code = 400;
			body = "{\"ok\":false,\"error\":\"invalid mode\"}";
		} else {
			{
				std::lock_guard<std::mutex> lk(g_mtx);
				g_cfg.enabled = enabled;
				g_cfg.name = name;
				g_cfg.mode = mode;
			}
			if(!cfg_save()) {
				code = 500;
				body = "{\"ok\":false,\"error\":\"failed to persist settings to EEPROM\"}";
			} else {
				{
					// empty name saved with enabled=1: EEPROM keeps '' (stays on the default-name
					// policy), the running config self-names as the serial — same as cfg_load()
					std::lock_guard<std::mutex> lk(g_mtx);
					if(g_cfg.enabled && g_cfg.name.empty()) {
						g_cfg.name = board_serial();
						if(!valid_name(g_cfg.name)) {
							g_cfg.name = "";
						}
					}
				}
				do_register();	// synchronous: GUI gets the real outcome
				body = status_json();
			}
		}
	} else {
		code = 404;
		body = "{\"err\":\"not found\"}";
	}

	char hdr[256];
	snprintf(hdr, sizeof(hdr), "HTTP/1.1 %d OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", code, body.size());
	send(fd, hdr, strlen(hdr), MSG_NOSIGNAL);
	send(fd, body.data(), body.size(), MSG_NOSIGNAL);
	close(fd);
}

static int selftest(void) {
	if(!valid_name("limey") || valid_name("") || valid_name("-bad") || valid_name("bad-") || valid_name("Has Space") || valid_name("x'; drop") || valid_name("seventeen-chars-x")) {
		return 1;
	}
	if(json_escape("a\"b\\c") != "a\\\"b\\\\c") {
		return 1;
	}
	auto kv = parse_query("enabled=1&name=my%2Dboard&mode=local");
	if(kv["name"] != "my-board" || kv["enabled"] != "1") {
		return 1;
	}
	// transport diagnosis map: each historical blind spot must stay named (2026-07-19)
	if(transport_err(127).find("not installed") == std::string::npos
		|| transport_err(6).find("DNS failure") == std::string::npos
		|| transport_err(7).find("egress blocked") == std::string::npos
		|| transport_err(28).find("timed out") == std::string::npos
		|| transport_err(35).find("TLS") == std::string::npos
		|| transport_err(3).find("curl exit 3") == std::string::npos) {
		return 1;
	}
	printf("selftest ok\n");
	return 0;
}

int main(int argc, char **argv) {
	if(argc > 1 && !strcmp(argv[1], "--selftest")) {
		return selftest();
	}
	signal(SIGPIPE, SIG_IGN);
	cfg_load();

	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(DDNSD_PORT);
	if(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) || listen(lfd, 16)) {
		perror("bind/listen");
		return 1;
	}
	printf("ddnsd: listening on 127.0.0.1:%d\n", DDNSD_PORT);

	std::thread worker(worker_thread);
	std::thread monitor(monitor_thread);
	monitor.detach();
	kick();		// boot registration (worker retries while the network comes up)

	while(!g_shutdown) {
		int fd = accept(lfd, nullptr, nullptr);
		if(fd < 0) {
			continue;
		}
		struct timeval tv = { 5, 0 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		http_client(fd);
	}
	worker.join();
	return 0;
}
