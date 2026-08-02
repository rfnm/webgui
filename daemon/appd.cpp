// appd v3: RFNM on-board app manager (nginx /api/ -> 127.0.0.1:7683). C++17, no library
// dependencies; systemctl/journalctl/nginx-reload are the only side effects.
//
// The registry IS the filesystem: every app deploys /rfnm/apps/<id>/app.json (manifest) and
// appd scans them at boot + on POST /api/v1/rescan. The radio has exactly one owner at a
// time, acquired through the lease endpoint; RTSA and VSG are ordinary manifest apps.
// Transitions are serialized in one worker with an appd-enforced teardown settle, a boot
// readiness gate (no radio consumer before the LA9310 "hard reset complete" dmesg marker),
// a per-board TX policy (/rfnm/board.conf), and the idle-stop law (webgui/API.md 4.2):
// GET state is the presence heartbeat, and an owner declaring liveness:"clients" is also
// held alive by its live client count - stop only when BOTH are gone.
//
// API (JSON over HTTP; nginx forwards /api/v1/* verbatim, bare /v1/* works on loopback):
//   GET  /api/v1/state                 - THE state document (board, radio lease, apps[] with
//                                        health/rf/config/data/build); the presence heartbeat
//   POST /api/v1/radio                 - {"owner":"<id>"|"none"}: the lease. 409 while a
//                                        transition runs, 403 TX apps on TX-forbidden boards
//   PUT  /api/v1/apps/<id>/rf          - {"antenna","gain","agc"}: rf.conf keys, restart if running
//   PUT  /api/v1/apps/<id>/config      - {"key","value"}: manifest-declared setting -> app.conf env
//   POST /api/v1/apps/<id>/restart     - restart (radio:"none" daemons, or the current owner)
//   GET  /api/v1/apps/<id>/log?n=40    - journal tail
//   POST /api/v1/rescan                - re-scan manifests, regenerate the nginx app include
//   GET  /api/v1/fwupdate              - recovery-updater boot flag state {"available","armed"}
//   POST /api/v1/fwupdate              - {"arm":bool,"reboot":bool}: clear/restore the u-boot
//                                        rfnm_is_bootable flag (+optional reboot into recovery)
// Per-app domain APIs are proxied at /api/app/<id>/ -> the manifest api.port (gen_nginx).
//
// Manifest (/rfnm/apps/<id>/app.json), all fields beyond id/title/unit/radio optional:
//   { "id":"lora", "title":"LoRa / Meshtastic", "desc":"...", "unit":"rfnm-lora",
//     "radio":"rx",                        // rx | tx | none
//     "settle_s":2, "idle_stop_s":180,     // teardown settle; 0 = never idle-stop
//     "api":{"port":7684,"version":1,"nginx_extra":""},   // route = /api/app/<id>/ (from id)
//     "liveness":{"mode":"clients","port":7687,"path":"/clients"},  // {"clients":N} endpoint;
//                                          // port defaults to api.port, path to /clients
//     "data":[{"kind":"spectrum-ws","proto":"ws","port":7681,"dir":"out","path":"/ws","desc":"..."}],
//     "rf":{"prefix":"LORA","antenna":"sma_a","gain":30},
//     "config":[{"key":"mode","env":"LORAD_MODE","choices":["lorawan","meshtastic"],"restart":true,
//                "flags_env":"LORAD_FLAGS","values":["","--x"]}],  // optional: choice i also
//                                          // writes flags_env=values[i] (units word-split it)
//     "gui":{"panel":"lora","order":40}, "open":0,
//     "build":{"commit":"...","built":"...","librfnm":"..."} }
//
// Build (host cross-compile, static aarch64):
//   aarch64-linux-gnu-g++ -O2 -static -std=c++17 -Wall -Wextra -pthread appd.cpp -o appd

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static const char *APPS_DIR = "/rfnm/apps";
static const char *BOARD_CONF = "/rfnm/board.conf";
static const char *RF_CONF = "/rfnm/apps/rf.conf";
static const char *NGINX_INCLUDE = "/rfnm/apps/nginx-apps.conf";
static const char *SHOW_PROPS = "Id,LoadState,ActiveState,SubState,MainPID,ActiveEnterTimestampMonotonic,Result";

// ---- tiny JSON parser (manifests + request bodies; objects/arrays/str/num/bool/null) ----
struct jv {
	enum { NUL, BOO, NUM, STR, OBJ, ARR } t = NUL;
	bool b = false;
	double n = 0;
	std::string s;
	std::vector<std::pair<std::string, jv>> o;
	std::vector<jv> a;
	const jv *get(const std::string &k) const {
		if(t != OBJ) {
			return nullptr;
		}
		for(const auto &kv : o) {
			if(kv.first == k) {
				return &kv.second;
			}
		}
		return nullptr;
	}
	std::string str(const std::string &k, const std::string &def = "") const {
		const jv *v = get(k);
		return v && v->t == STR ? v->s : def;
	}
	double num(const std::string &k, double def = 0) const {
		const jv *v = get(k);
		return v && v->t == NUM ? v->n : def;
	}
	bool boolean(const std::string &k, bool def = false) const {
		const jv *v = get(k);
		return v && v->t == BOO ? v->b : def;
	}
};

static void jskip(const std::string &s, size_t &p) {
	while(p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) {
		p++;
	}
}

static bool jparse_val(const std::string &s, size_t &p, jv &out, int depth);

static bool jparse_str(const std::string &s, size_t &p, std::string &out) {
	if(p >= s.size() || s[p] != '"') {
		return false;
	}
	p++;
	out.clear();
	while(p < s.size()) {
		char c = s[p];
		if(c == '"') {
			p++;
			return true;
		}
		if(c == '\\') {
			if(p + 1 >= s.size()) {
				return false;
			}
			char e = s[p + 1];
			p += 2;
			switch(e) {
				case '"': out += '"'; break;
				case '\\': out += '\\'; break;
				case '/': out += '/'; break;
				case 'n': out += '\n'; break;
				case 't': out += '\t'; break;
				case 'r': out += '\r'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'u': {   // decode BMP escapes to UTF-8; good enough for manifests
					if(p + 4 > s.size()) {
						return false;
					}
					unsigned v = (unsigned)strtoul(s.substr(p, 4).c_str(), nullptr, 16);
					p += 4;
					if(v < 0x80) {
						out += (char)v;
					} else if(v < 0x800) {
						out += (char)(0xC0 | (v >> 6));
						out += (char)(0x80 | (v & 0x3F));
					} else {
						out += (char)(0xE0 | (v >> 12));
						out += (char)(0x80 | ((v >> 6) & 0x3F));
						out += (char)(0x80 | (v & 0x3F));
					}
					break;
				}
				default: return false;
			}
		} else {
			out += c;
			p++;
		}
	}
	return false;
}

static bool jparse_val(const std::string &s, size_t &p, jv &out, int depth) {
	if(depth > 16) {
		return false;
	}
	jskip(s, p);
	if(p >= s.size()) {
		return false;
	}
	char c = s[p];
	if(c == '"') {
		out.t = jv::STR;
		return jparse_str(s, p, out.s);
	}
	if(c == '{') {
		out.t = jv::OBJ;
		p++;
		jskip(s, p);
		if(p < s.size() && s[p] == '}') {
			p++;
			return true;
		}
		while(p < s.size()) {
			jskip(s, p);
			std::string key;
			if(!jparse_str(s, p, key)) {
				return false;
			}
			jskip(s, p);
			if(p >= s.size() || s[p] != ':') {
				return false;
			}
			p++;
			jv val;
			if(!jparse_val(s, p, val, depth + 1)) {
				return false;
			}
			out.o.emplace_back(std::move(key), std::move(val));
			jskip(s, p);
			if(p < s.size() && s[p] == ',') {
				p++;
				continue;
			}
			if(p < s.size() && s[p] == '}') {
				p++;
				return true;
			}
			return false;
		}
		return false;
	}
	if(c == '[') {
		out.t = jv::ARR;
		p++;
		jskip(s, p);
		if(p < s.size() && s[p] == ']') {
			p++;
			return true;
		}
		while(p < s.size()) {
			jv val;
			if(!jparse_val(s, p, val, depth + 1)) {
				return false;
			}
			out.a.push_back(std::move(val));
			jskip(s, p);
			if(p < s.size() && s[p] == ',') {
				p++;
				continue;
			}
			if(p < s.size() && s[p] == ']') {
				p++;
				return true;
			}
			return false;
		}
		return false;
	}
	if(c == 't' && s.compare(p, 4, "true") == 0) {
		out.t = jv::BOO;
		out.b = true;
		p += 4;
		return true;
	}
	if(c == 'f' && s.compare(p, 5, "false") == 0) {
		out.t = jv::BOO;
		out.b = false;
		p += 5;
		return true;
	}
	if(c == 'n' && s.compare(p, 4, "null") == 0) {
		out.t = jv::NUL;
		p += 4;
		return true;
	}
	char *end = nullptr;
	double v = strtod(s.c_str() + p, &end);
	if(end == s.c_str() + p) {
		return false;
	}
	out.t = jv::NUM;
	out.n = v;
	p = (size_t)(end - s.c_str());
	return true;
}

static bool jparse(const std::string &s, jv &out) {
	size_t p = 0;
	if(!jparse_val(s, p, out, 0)) {
		return false;
	}
	jskip(s, p);
	return p == s.size();
}

// ---- registry: manifests scanned from /rfnm/apps/<id>/app.json --------------------------
struct cfg_def {
	std::string key, env;
	std::vector<std::string> choices;
	std::string flags_env;                 // optional: choice i also writes flags_env=values[i]
	std::vector<std::string> values;       // parallel to choices (validated same length at scan)
	bool restart = false;
};

// declared direct data port (heavy streams bypass nginx; surfaced in /state for discovery)
struct data_port {
	std::string kind, proto, dir, path, desc;
	int port = 0;
};

struct app_mf {
	std::string id, title, desc, unit, radio, panel, nginx_extra;
	std::string rf_prefix, rf_ant;
	int rf_gain = 30;
	bool rf_agc = true;
	bool has_rf = false;
	int port = 0, open = 0, order = 100, settle_s = 2, idle_stop_s = 0;
	int api_version = 0;
	std::string live_mode, live_path;      // liveness: "clients" -> GET live_port/live_path
	int live_port = 0;
	std::vector<data_port> data;
	std::vector<cfg_def> config;
	std::string b_commit, b_built, b_librfnm;
	std::string dir;
};

static std::mutex g_reg_mtx;
static std::vector<app_mf> g_reg;
static std::string g_board_name = "unknown";
// /rfnm/board.conf TX_ALLOWED as a raw policy string: "0" = no TX apps, "1" = unrestricted,
// anything else (e.g. "embed" = embedded antenna only) grants the lease and passes through to
// /v1/state so the GUI can badge it - the path policy itself is enforced device-side by vsgd
static std::string g_tx_policy = "0";
static bool g_agc_available = true;   // /rfnm/board.conf AGC_AVAILABLE=0 while the kernel peak-detector defect is open

// ---- child processes: capture stdout+stderr, hard deadline, never throw ----------------
static bool run_cmd(const std::vector<std::string> &argv, int timeout_ms, std::string &out, int &exit_code) {
	out.clear();
	exit_code = -1;
	int fds[2];
	if(pipe(fds) < 0) {
		return false;
	}
	pid_t pid = fork();
	if(pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return false;
	}
	if(pid == 0) {
		close(fds[0]);
		dup2(fds[1], 1);
		dup2(fds[1], 2);
		close(fds[1]);
		std::vector<char *> av;
		for(const auto &a : argv) {
			av.push_back(const_cast<char *>(a.c_str()));
		}
		av.push_back(nullptr);
		execvp(av[0], av.data());
		_exit(127);
	}
	close(fds[1]);
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	char buf[4096];
	bool timed_out = false;
	while(true) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long elapsed = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
		long left = timeout_ms - elapsed;
		if(left <= 0) {
			timed_out = true;
			break;
		}
		struct pollfd pfd = { fds[0], POLLIN, 0 };
		int pr = poll(&pfd, 1, (int)left);
		if(pr <= 0) {
			if(pr == 0) {
				timed_out = true;
			}
			break;
		}
		ssize_t n = read(fds[0], buf, sizeof(buf));
		if(n <= 0) {
			break;   // child closed its end (exited)
		}
		if(out.size() < 262144) {
			out.append(buf, (size_t)n);
		}
	}
	close(fds[0]);
	if(timed_out) {
		kill(pid, SIGKILL);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if(timed_out) {
		out += "\n(command timed out)";
		return false;
	}
	exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
	return true;
}

static int systemctl(std::string &out, std::initializer_list<std::string> args, int timeout_ms = 30000) {
	std::vector<std::string> argv = { "systemctl" };
	argv.insert(argv.end(), args);
	int code = -1;
	run_cmd(argv, timeout_ms, out, code);
	return code;
}

// ---- systemd unit state: one batched round-trip -----------------------------------------
using props_t = std::map<std::string, std::string>;

static std::map<std::string, props_t> show_units(const std::vector<app_mf> &reg) {
	std::vector<std::string> argv = { "systemctl", "show", "-p", SHOW_PROPS, "--" };
	for(const auto &a : reg) {
		argv.push_back(a.unit + ".service");
	}
	std::string out;
	int code = -1;
	run_cmd(argv, 10000, out, code);
	std::map<std::string, props_t> res;
	props_t cur;
	std::istringstream is(out);
	std::string line;
	auto flush = [&] {
		auto it = cur.find("Id");
		if(it != cur.end()) {
			std::string unit = it->second;
			size_t dot = unit.rfind(".service");
			if(dot != std::string::npos) {
				unit.resize(dot);
			}
			res[unit] = cur;
		}
		cur.clear();
	};
	while(std::getline(is, line)) {
		if(line.empty()) {
			flush();
			continue;
		}
		size_t eq = line.find('=');
		if(eq != std::string::npos) {
			cur[line.substr(0, eq)] = line.substr(eq + 1);
		}
	}
	flush();
	return res;
}

static std::string prop(const std::map<std::string, props_t> &m, const std::string &unit, const char *key) {
	auto it = m.find(unit);
	if(it == m.end()) {
		return "";
	}
	auto p = it->second.find(key);
	return p == it->second.end() ? "" : p->second;
}

static std::vector<std::string> journal_tail(const std::string &unit, int n = 8) {
	std::string out;
	int code = -1;
	run_cmd({ "journalctl", "-u", unit, "-n", std::to_string(n), "--no-pager", "-o", "short-iso" }, 10000, out, code);
	std::vector<std::string> lines;
	std::istringstream is(out);
	std::string line;
	while(std::getline(is, line)) {
		if(!line.empty() && line.rfind("-- ", 0) != 0) {
			lines.push_back(line);
		}
	}
	return lines;
}

// ---- small file helpers -------------------------------------------------------------------
static bool read_file(const std::string &path, std::string &out) {
	std::ifstream f(path);
	if(!f) {
		return false;
	}
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

static bool write_file_atomic(const std::string &path, const std::string &content) {
	std::string tmp = path + ".tmp";
	std::ofstream f(tmp);
	if(!f) {
		return false;
	}
	f << content;
	f.close();
	return rename(tmp.c_str(), path.c_str()) == 0;
}

static bool read_u64(const std::string &path, unsigned long long &v) {
	std::string s;
	if(!read_file(path, s)) {
		return false;
	}
	return sscanf(s.c_str(), "%llu", &v) == 1;
}

static double mono_now() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// env-style KEY=value files (rf.conf, app.conf): read one key / rewrite a set of keys
static std::string env_get(const std::string &path, const std::string &key) {
	std::string s;
	if(!read_file(path, s)) {
		return "";
	}
	std::istringstream is(s);
	std::string line;
	while(std::getline(is, line)) {
		if(line.rfind(key + "=", 0) == 0) {
			std::string v = line.substr(key.size() + 1);
			while(!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) {
				v.pop_back();
			}
			return v;
		}
	}
	return "";
}

static std::mutex g_conf_mtx;   // rf.conf / app.conf writers

static bool env_set(const std::string &path, const std::vector<std::pair<std::string, std::string>> &kvs) {
	std::lock_guard<std::mutex> lk(g_conf_mtx);
	std::string s;
	read_file(path, s);   // keep unrelated lines + comments
	std::string out;
	std::istringstream is(s);
	std::string line;
	while(std::getline(is, line)) {
		bool replaced = false;
		for(const auto &kv : kvs) {
			if(line.rfind(kv.first + "=", 0) == 0) {
				replaced = true;
				break;
			}
		}
		if(!replaced) {
			out += line + "\n";
		}
	}
	for(const auto &kv : kvs) {
		out += kv.first + "=" + kv.second + "\n";
	}
	return write_file_atomic(path, out);
}

// cpu usage of the whole unit cgroup; %-of-one-core needs a delta between polls
static std::mutex g_cpu_mtx;
static std::map<std::string, std::pair<unsigned long long, double>> g_cpu_prev;

static double cpu_percent(const std::string &unit, unsigned long long usec) {
	double now = mono_now();
	std::lock_guard<std::mutex> lk(g_cpu_mtx);
	auto it = g_cpu_prev.find(unit);
	std::pair<unsigned long long, double> prev = it == g_cpu_prev.end() ? std::make_pair(0ULL, -1.0) : it->second;
	g_cpu_prev[unit] = { usec, now };
	if(prev.second < 0 || usec < prev.first || now - prev.second < 0.2) {
		return -1;
	}
	return (double)(usec - prev.first) / ((now - prev.second) * 1e6) * 100.0;
}

// ---- JSON output helpers --------------------------------------------------------------------
static std::string jesc(const std::string &s) {
	std::string o;
	o.reserve(s.size() + 8);
	size_t i = 0;
	while(i < s.size()) {
		unsigned char c = (unsigned char)s[i];
		if(c == '"' || c == '\\') {
			o += '\\';
			o += (char)c;
			i++;
		} else if(c < 0x20) {
			char b[8];
			snprintf(b, sizeof(b), "\\u%04x", c);
			o += b;
			i++;
		} else if(c < 0x80) {
			o += (char)c;
			i++;
		} else {
			int cont = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
			bool ok = cont > 0 && c != 0xC0 && c != 0xC1 && c <= 0xF4 && i + (size_t)cont < s.size();
			for(int k = 1; ok && k <= cont; k++) {
				ok = ((unsigned char)s[i + k] & 0xC0) == 0x80;
			}
			if(ok) {
				o.append(s, i, (size_t)cont + 1);
				i += (size_t)cont + 1;
			} else {
				o += '?';
				i++;
			}
		}
	}
	return o;
}

static void jkv(std::string &j, const char *key, const std::string &val, bool &first) {
	j += first ? "" : ",";
	first = false;
	j += "\"";
	j += key;
	j += "\":\"" + jesc(val) + "\"";
}

static void jkv_raw(std::string &j, const char *key, const std::string &raw, bool &first) {
	j += first ? "" : ",";
	first = false;
	j += "\"";
	j += key;
	j += "\":" + raw;
}

static std::string jarr(const std::vector<std::string> &lines) {
	std::string j = "[";
	for(size_t i = 0; i < lines.size(); i++) {
		j += (i ? "," : "") + ("\"" + jesc(lines[i]) + "\"");
	}
	return j + "]";
}

static std::string fmt(const char *f, double v) {
	char b[64];
	snprintf(b, sizeof(b), f, v);
	return b;
}

// ---- registry scan + nginx include generation -------------------------------------------
static void scan_registry() {
	std::vector<app_mf> reg;
	if(DIR *d = opendir(APPS_DIR)) {
		while(struct dirent *e = readdir(d)) {
			if(e->d_name[0] == '.') {
				continue;
			}
			std::string dir = std::string(APPS_DIR) + "/" + e->d_name;
			std::string txt;
			if(!read_file(dir + "/app.json", txt)) {
				continue;
			}
			jv m;
			if(!jparse(txt, m) || m.t != jv::OBJ) {
				fprintf(stderr, "appd: bad manifest %s/app.json\n", dir.c_str());
				continue;
			}
			app_mf a;
			a.id = m.str("id");
			a.title = m.str("title");
			a.desc = m.str("desc");
			a.unit = m.str("unit");
			a.radio = m.str("radio", "none");
			a.settle_s = (int)m.num("settle_s", 2);
			a.idle_stop_s = (int)m.num("idle_stop_s", 0);
			a.open = (int)m.num("open", 0);
			a.dir = dir;
			if(a.id.empty() || a.title.empty() || a.unit.empty() || (a.radio != "rx" && a.radio != "tx" && a.radio != "none")) {
				fprintf(stderr, "appd: manifest %s missing id/title/unit or bad radio\n", dir.c_str());
				continue;
			}
			// the id becomes an nginx location (/api/app/<id>/) - reject anything outside the charset
			if(a.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) {
				fprintf(stderr, "appd: manifest %s id '%s' outside [a-z0-9_-]\n", dir.c_str(), a.id.c_str());
				continue;
			}
			if(const jv *api = m.get("api")) {
				a.port = (int)api->num("port", 0);
				a.api_version = (int)api->num("version", 0);
				a.nginx_extra = api->str("nginx_extra");
			}
			if(const jv *lv = m.get("liveness")) {
				a.live_mode = lv->str("mode");
				a.live_port = (int)lv->num("port", a.port);
				a.live_path = lv->str("path", "/clients");
			}
			if(const jv *dp = m.get("data")) {
				for(const auto &e : dp->a) {
					data_port d;
					d.kind = e.str("kind");
					d.proto = e.str("proto");
					d.port = (int)e.num("port", 0);
					d.dir = e.str("dir", "out");
					d.path = e.str("path");
					d.desc = e.str("desc");
					if(!d.kind.empty() && !d.proto.empty() && d.port > 0) {
						a.data.push_back(std::move(d));
					}
				}
			}
			if(const jv *rf = m.get("rf")) {
				a.has_rf = true;
				a.rf_prefix = rf->str("prefix");
				a.rf_ant = rf->str("antenna", "sma_a");
				a.rf_gain = (int)rf->num("gain", 30);
				a.rf_agc = rf->boolean("agc", true);
			}
			if(const jv *gui = m.get("gui")) {
				a.panel = gui->str("panel", "generic");
				a.order = (int)gui->num("order", 100);
			}
			if(const jv *b = m.get("build")) {
				a.b_commit = b->str("commit");
				a.b_built = b->str("built");
				a.b_librfnm = b->str("librfnm");
			}
			if(const jv *cfg = m.get("config")) {
				for(const auto &c : cfg->a) {
					cfg_def cd;
					cd.key = c.str("key");
					cd.env = c.str("env");
					cd.restart = c.boolean("restart");
					cd.flags_env = c.str("flags_env");
					if(const jv *ch = c.get("choices")) {
						for(const auto &x : ch->a) {
							if(x.t == jv::STR) {
								cd.choices.push_back(x.s);
							}
						}
					}
					if(const jv *vs = c.get("values")) {
						for(const auto &x : vs->a) {
							if(x.t == jv::STR) {
								cd.values.push_back(x.s);
							}
						}
					}
					if(!cd.flags_env.empty() && cd.values.size() != cd.choices.size()) {
						fprintf(stderr, "appd: manifest %s config '%s': values[] must pair choices[] - dropped\n", dir.c_str(), cd.key.c_str());
						continue;
					}
					if(!cd.key.empty() && !cd.env.empty() && !cd.choices.empty()) {
						a.config.push_back(std::move(cd));
					}
				}
			}
			reg.push_back(std::move(a));
		}
		closedir(d);
	}
	std::sort(reg.begin(), reg.end(), [](const app_mf &x, const app_mf &y) { return x.order < y.order; });

	// materialize rf.conf defaults so ${*_ANT}/${*_GAIN}/${*_AGC} in units never expand empty
	for(const auto &a : reg) {
		if(a.has_rf && env_get(RF_CONF, a.rf_prefix + "_ANT").empty()) {
			env_set(RF_CONF, { { a.rf_prefix + "_ANT", a.rf_ant }, { a.rf_prefix + "_GAIN", std::to_string(a.rf_gain) },
			                   { a.rf_prefix + "_AGC", a.rf_agc ? "1" : "0" } });
		} else if(a.has_rf && env_get(RF_CONF, a.rf_prefix + "_AGC").empty()) {
			env_set(RF_CONF, { { a.rf_prefix + "_AGC", a.rf_agc ? "1" : "0" } });   // upgrade path: pre-AGC rf.conf
		}
	}

	std::string name = env_get(BOARD_CONF, "BOARD");
	std::string tx = env_get(BOARD_CONF, "TX_ALLOWED");
	std::string agc_avail = env_get(BOARD_CONF, "AGC_AVAILABLE");
	{
		std::lock_guard<std::mutex> lk(g_reg_mtx);
		g_reg = std::move(reg);
		if(!name.empty()) {
			g_board_name = name;
		}
		// back-compat: ""/"0"/"no" refuse TX, "yes" was an accepted truthy spelling; any other
		// value ("embed", ...) is a policy string that still grants the TX lease
		g_tx_policy = (tx.empty() || tx == "no") ? "0" : (tx == "yes" ? "1" : tx);
		g_agc_available = agc_avail != "0" && agc_avail != "no";
	}
}

static std::vector<app_mf> registry() {
	std::lock_guard<std::mutex> lk(g_reg_mtx);
	return g_reg;
}

static bool find_mf(const std::string &id, app_mf &out) {
	std::lock_guard<std::mutex> lk(g_reg_mtx);
	for(const auto &a : g_reg) {
		if(a.id == id) {
			out = a;
			return true;
		}
	}
	return false;
}

// one generated nginx include for every app HTTP API; the site config `include`s this file
static void gen_nginx() {
	// one location per app: /api/app/<id>/ (longest-prefix beats the static /api/ -> appd
	// route). Trailing-slash proxy_pass strips the prefix, so apps serve bare paths.
	std::string conf = "# generated by appd from /rfnm/apps/*/app.json - do not edit\n";
	for(const auto &a : registry()) {
		if(a.port <= 0) {
			continue;
		}
		std::string timeout = a.nginx_extra.find("proxy_read_timeout") == std::string::npos ? "proxy_read_timeout 60s; " : "";
		conf += "location /api/app/" + a.id + "/ { proxy_pass http://127.0.0.1:" + std::to_string(a.port) + "/; " + timeout + a.nginx_extra + " }\n";
	}
	std::string cur;
	read_file(NGINX_INCLUDE, cur);
	if(cur == conf) {
		return;
	}
	if(!write_file_atomic(NGINX_INCLUDE, conf)) {
		fprintf(stderr, "appd: cannot write %s\n", NGINX_INCLUDE);
		return;
	}
	std::string out;
	int code = -1;
	run_cmd({ "nginx", "-s", "reload" }, 10000, out, code);
	if(code != 0) {
		fprintf(stderr, "appd: nginx reload failed: %s\n", out.c_str());
	}
}

// ---- radio lease ------------------------------------------------------------------------
static std::mutex g_r_mtx;
static std::condition_variable g_r_cv;
static std::string g_owner;            // "" = none
static std::string g_rstate = "idle";  // idle|starting|active|stopping|switching|crashed
static std::string g_note;             // human-readable last event ("idle-stopped lora", errors)
static double g_since = 0;
static bool g_busy = false;            // worker mid-transition
static std::string g_t_from, g_t_to, g_t_phase;
static bool g_has_req = false;
static std::string g_req_owner;
static bool g_req_restart = false;
static double g_last_ui = 0;           // last GET state / lease POST (the presence heartbeat)
static int g_owner_clients = -1;       // owner's last liveness poll (-1 = unknown/no liveness)

// boot gate: no radio consumer before the LA9310 driver finished its hard reset
static bool g_radio_ready = false;

static void wait_radio_ready() {
	if(g_radio_ready) {
		return;
	}
	for(int i = 0; i < 60; i++) {
		std::string out;
		int code = -1;
		// clean boots print 'daughterboard initialized' (marker fixed 2026-07-07); only a
		// mid-run LA9310 recovery prints 'hard reset complete' - accept either
		run_cmd({ "sh", "-c", "dmesg | grep -cE 'daughterboard initialized|hard reset complete'" }, 5000, out, code);
		if(atoi(out.c_str()) > 0) {
			sleep(2);   // marker + settle
			g_radio_ready = true;
			return;
		}
		std::string up;
		if(read_file("/proc/uptime", up) && atof(up.c_str()) > 300) {
			g_radio_ready = true;   // long-running board; the marker rotated out of the ring
			return;
		}
		sleep(2);
	}
	fprintf(stderr, "appd: radio-ready marker never appeared; proceeding\n");
	g_radio_ready = true;
}

// stop every radio-consuming unit (except `keep` if nonempty), reset-failed the stopped ones.
// returns false + fills err/log on a stop failure.
static bool stop_radio_units(const std::string &keep, std::string &err, std::vector<std::string> &log) {
	auto reg = registry();
	auto shown = show_units(reg);
	for(const auto &a : reg) {
		if(a.radio == "none" || a.id == keep) {
			continue;
		}
		std::string st = prop(shown, a.unit, "ActiveState");
		std::string sub = prop(shown, a.unit, "SubState");
		if(st == "active" || st == "activating" || sub == "auto-restart") {
			std::string out;
			if(systemctl(out, { "stop", a.unit }) != 0) {
				err = "stopping " + a.title + " failed: " + out;
				log = journal_tail(a.unit);
				return false;
			}
			systemctl(out, { "reset-failed", a.unit }, 5000);
		}
	}
	return true;
}

// the single transition worker: every lease change (and owner restart) runs here, serialized
static void radio_worker() {
	while(true) {
		std::string want;
		bool restart;
		{
			std::unique_lock<std::mutex> lk(g_r_mtx);
			g_r_cv.wait(lk, [] { return g_has_req; });
			want = g_req_owner;
			restart = g_req_restart;
			g_has_req = false;
			g_busy = true;
			g_t_from = g_owner;
			g_t_to = want.empty() ? "none" : want;
		}
		auto set_phase = [](const char *p) {
			std::lock_guard<std::mutex> lk(g_r_mtx);
			g_t_phase = p;
			g_rstate = "switching";
		};
		auto finish = [](const std::string &owner, const std::string &state, const std::string &note) {
			std::lock_guard<std::mutex> lk(g_r_mtx);
			g_owner = owner;
			g_rstate = state;
			g_note = note;
			g_since = mono_now();
			g_busy = false;
			g_t_phase.clear();
			g_owner_clients = -1;   // stale liveness never carries across owners
		};

		app_mf target;
		bool have_target = !want.empty() && find_mf(want, target);
		{
			// same-owner no-op (idempotent lease re-acquire without restart)
			std::lock_guard<std::mutex> lk(g_r_mtx);
			if(!restart && want == g_owner && g_rstate == "active") {
				g_busy = false;
				g_t_phase.clear();
				continue;
			}
		}

		set_phase("stopping");
		std::string err;
		std::vector<std::string> log;
		if(!stop_radio_units("", err, log)) {
			finish("", "idle", err);
			continue;
		}
		if(!have_target) {
			finish("", "idle", want.empty() ? g_note : "unknown app " + want);
			if(!want.empty()) {
				fprintf(stderr, "appd: lease request for unknown app %s\n", want.c_str());
			}
			continue;
		}

		set_phase("settling");
		wait_radio_ready();
		int settle = target.settle_s < 2 ? 2 : target.settle_s;
		sleep((unsigned)settle);

		set_phase("starting");
		std::string out;
		if(systemctl(out, { "--no-block", "start", target.unit }, 10000) != 0) {
			finish("", "idle", "start " + target.title + " failed: " + out);
			continue;
		}
		{
			std::lock_guard<std::mutex> lk(g_r_mtx);
			g_owner = target.id;
			g_rstate = "starting";
			g_since = mono_now();
		}
		// track to active/crashed for up to 25 s; still-activating is left as "starting"
		// and state_json keeps promoting/demoting from the live unit state afterwards
		std::string result = "starting";
		for(int i = 0; i < 50; i++) {
			usleep(500 * 1000);
			auto shown = show_units({ target });
			std::string st = prop(shown, target.unit, "ActiveState");
			std::string sub = prop(shown, target.unit, "SubState");
			if(st == "active") {
				result = "active";
				break;
			}
			if(st == "failed" || sub == "auto-restart") {
				result = "crashed";
				break;
			}
		}
		finish(target.id, result, result == "crashed" ? target.title + " crashed right after start" : "");
	}
}

// submit a transition; returns false if the worker is busy (caller replies 409)
static bool submit_transition(const std::string &owner, bool restart, std::string &err) {
	std::lock_guard<std::mutex> lk(g_r_mtx);
	if(g_busy || g_has_req) {
		err = "transition in progress (" + g_t_from + " -> " + g_t_to + ", " + g_t_phase + ")";
		return false;
	}
	g_has_req = true;
	g_req_owner = owner;
	g_req_restart = restart;
	g_r_cv.notify_one();
	return true;
}

static bool send_all(int fd, const char *p, size_t n);   // defined in the HTTP section below

// minimal loopback HTTP GET for liveness polls; true + body iff the app answered 200.
// Every fleet daemon replies Connection: close, so read-to-EOF is the whole protocol.
static bool http_get_loopback(int port, const std::string &path, int timeout_ms, std::string &body) {
	body.clear();
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if(fd < 0) {
		return false;
	}
	struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons((uint16_t)port);
	if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return false;
	}
	std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
	if(!send_all(fd, req.c_str(), req.size())) {
		close(fd);
		return false;
	}
	std::string resp;
	char buf[2048];
	while(resp.size() < 65536) {
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if(n <= 0) {
			break;
		}
		resp.append(buf, (size_t)n);
	}
	close(fd);
	size_t eol = resp.find("\r\n");
	if(eol == std::string::npos || resp.substr(0, eol).find(" 200 ") == std::string::npos) {
		return false;
	}
	size_t split = resp.find("\r\n\r\n");
	if(split == std::string::npos) {
		return false;
	}
	body = resp.substr(split + 4);
	return true;
}

// idle-stop (API.md 4.2): the presence heartbeat (GET state / lease POST) gates stopping;
// an owner declaring liveness:"clients" is ALSO held by its live client count - stop only
// when the heartbeat expired AND the count has been confirmed 0 (3 consecutive polls, plus
// a 30 s post-transition grace). Poll failures count as 0: a broken liveness endpoint can
// extend nothing (falls back to today's heartbeat behavior), never immortalize an app.
static void idle_watcher() {
	std::string last_owner;
	int zero_streak = 0;
	while(true) {
		sleep(5);
		std::string owner;
		double since;
		{
			std::lock_guard<std::mutex> lk(g_r_mtx);
			if(g_busy || g_has_req || g_owner.empty() || (g_rstate != "active" && g_rstate != "starting")) {
				last_owner.clear();
				continue;
			}
			owner = g_owner;
			since = g_since;
		}
		if(owner != last_owner) {
			last_owner = owner;
			zero_streak = 0;
			std::lock_guard<std::mutex> lk(g_r_mtx);
			g_owner_clients = -1;
		}
		app_mf mf;
		if(!find_mf(owner, mf) || mf.idle_stop_s <= 0) {
			continue;
		}
		bool counted = mf.live_mode == "clients" && mf.live_port > 0;
		if(counted) {
			std::string body;
			int clients = -1;
			jv v;
			if(http_get_loopback(mf.live_port, mf.live_path, 2000, body) && jparse(body, v) && v.t == jv::OBJ) {
				clients = (int)v.num("clients", 0);
			}
			zero_streak = clients > 0 ? 0 : zero_streak + 1;
			std::lock_guard<std::mutex> lk(g_r_mtx);
			g_owner_clients = clients;
		}
		double idle;
		{
			std::lock_guard<std::mutex> lk(g_r_mtx);
			idle = mono_now() - g_last_ui;
		}
		if(idle <= mf.idle_stop_s) {
			continue;
		}
		if(counted && (zero_streak < 3 || mono_now() - since < 30)) {
			continue;
		}
		std::string err;
		if(submit_transition("", false, err)) {
			std::lock_guard<std::mutex> lk(g_r_mtx);
			g_note = "idle-stopped " + mf.title + " (no " + (counted ? "client or poll" : "poll") + " for " + std::to_string((int)idle) + " s)";
			printf("appd: %s\n", g_note.c_str());
			fflush(stdout);
		}
	}
}

// ---- state document -----------------------------------------------------------------------
static std::atomic<unsigned long long> g_seq(0);

static std::string radio_json_locked() {
	std::string j = "{";
	bool f = true;
	if(g_owner.empty()) {
		jkv_raw(j, "owner", "null", f);
	} else {
		jkv(j, "owner", g_owner, f);
	}
	jkv(j, "state", g_rstate, f);
	jkv_raw(j, "since", fmt("%.1f", g_since > 0 ? mono_now() - g_since : 0), f);
	if(g_owner_clients >= 0) {
		jkv_raw(j, "clients", std::to_string(g_owner_clients), f);
	}
	if(g_busy) {
		std::string t = "{";
		bool tf = true;
		jkv(t, "from", g_t_from.empty() ? "none" : g_t_from, tf);
		jkv(t, "to", g_t_to, tf);
		jkv(t, "phase", g_t_phase, tf);
		jkv_raw(j, "transition", t + "}", f);
	} else {
		jkv_raw(j, "transition", "null", f);
	}
	if(!g_note.empty()) {
		jkv(j, "note", g_note, f);
	}
	return j + "}";
}

static std::string state_json() {
	auto reg = registry();
	auto shown = show_units(reg);
	double mono = mono_now();

	// reconcile the lease with reality: adopt a radio unit that is already running (appd
	// restart / manual systemctl start), promote starting->active, catch crashes and
	// out-of-band stops (someone systemctl-stopped the owner behind appd's back)
	{
		std::lock_guard<std::mutex> lk(g_r_mtx);
		g_last_ui = mono;
		if(!g_busy && g_owner.empty()) {
			for(const auto &a : reg) {
				if(a.radio != "none" && prop(shown, a.unit, "ActiveState") == "active") {
					g_owner = a.id;
					g_rstate = "active";
					g_since = mono;
					g_note = "adopted running " + a.title;
					break;
				}
			}
		}
		if(!g_busy && !g_owner.empty()) {
			std::string unit;
			for(const auto &a : reg) {
				if(a.id == g_owner) {
					unit = a.unit;
				}
			}
			std::string st = prop(shown, unit, "ActiveState");
			std::string sub = prop(shown, unit, "SubState");
			if(st == "active") {
				g_rstate = "active";
			} else if(st == "failed" || sub == "auto-restart") {
				g_rstate = "crashed";
			} else if(st == "inactive" && g_rstate != "starting") {
				g_note = g_owner + " stopped outside appd";
				g_owner.clear();
				g_rstate = "idle";
			}
		}
	}

	std::string j = "{";
	bool f = true;
	jkv_raw(j, "seq", std::to_string(++g_seq), f);

	// board vitals (merged - there is no separate /system endpoint anymore)
	{
		std::string b = "{";
		bool bf = true;
		jkv(b, "name", g_board_name, bf);
		jkv(b, "tx_allowed", g_tx_policy, bf);   // raw policy string "0"|"1"|"embed" (GUI: "0" = greyed, "embed" = badge)
		jkv_raw(b, "agc_available", g_agc_available ? "true" : "false", bf);
		std::string s;
		if(read_file("/proc/loadavg", s)) {
			double l1 = 0, l5 = 0, l15 = 0;
			if(sscanf(s.c_str(), "%lf %lf %lf", &l1, &l5, &l15) == 3) {
				jkv_raw(b, "load", "[" + fmt("%.2f", l1) + "," + fmt("%.2f", l5) + "," + fmt("%.2f", l15) + "]", bf);
			}
		}
		jkv_raw(b, "ncpu", std::to_string(sysconf(_SC_NPROCESSORS_ONLN)), bf);
		if(read_file("/proc/uptime", s)) {
			double up = 0;
			if(sscanf(s.c_str(), "%lf", &up) == 1) {
				jkv_raw(b, "uptime", fmt("%.1f", up), bf);
			}
		}
		if(read_file("/proc/meminfo", s)) {
			unsigned long long total = 0, avail = 0;
			const char *p = strstr(s.c_str(), "MemTotal:");
			if(p) {
				sscanf(p, "MemTotal: %llu", &total);
			}
			p = strstr(s.c_str(), "MemAvailable:");
			if(p) {
				sscanf(p, "MemAvailable: %llu", &avail);
			}
			jkv_raw(b, "mem_total", std::to_string(total * 1024), bf);
			jkv_raw(b, "mem_avail", std::to_string(avail * 1024), bf);
		}
		std::string th = "{";
		bool tf = true;
		std::vector<std::string> zones;
		if(DIR *d = opendir("/sys/class/thermal")) {
			while(struct dirent *e = readdir(d)) {
				if(strncmp(e->d_name, "thermal_zone", 12) == 0) {
					zones.push_back(e->d_name);
				}
			}
			closedir(d);
		}
		std::sort(zones.begin(), zones.end());
		for(const auto &z : zones) {
			std::string type, temp;
			if(read_file("/sys/class/thermal/" + z + "/type", type) && read_file("/sys/class/thermal/" + z + "/temp", temp)) {
				while(!type.empty() && (type.back() == '\n' || type.back() == '\r')) {
					type.pop_back();
				}
				th += (tf ? "" : ",") + ("\"" + jesc(type) + "\":" + fmt("%.1f", atof(temp.c_str()) / 1000.0));
				tf = false;
			}
		}
		jkv_raw(b, "thermal", th + "}", bf);
		jkv_raw(j, "board", b + "}", f);
	}

	{
		std::lock_guard<std::mutex> lk(g_r_mtx);
		jkv_raw(j, "radio", radio_json_locked(), f);
	}

	std::string apps = "[";
	bool af = true;
	for(const auto &a : reg) {
		apps += af ? "" : ",";
		af = false;
		apps += "{";
		bool pf = true;
		jkv(apps, "id", a.id, pf);
		jkv(apps, "title", a.title, pf);
		jkv(apps, "desc", a.desc, pf);
		jkv(apps, "radio", a.radio, pf);
		jkv(apps, "panel", a.panel.empty() ? "generic" : a.panel, pf);
		jkv_raw(apps, "order", std::to_string(a.order), pf);
		jkv_raw(apps, "idle_stop", std::to_string(a.idle_stop_s), pf);
		if(a.open) {
			jkv_raw(apps, "open", std::to_string(a.open), pf);
		}
		if(a.port > 0) {
			std::string api = "{";
			bool xf = true;
			jkv(api, "base", "/api/app/" + a.id + "/", xf);
			jkv_raw(api, "port", std::to_string(a.port), xf);
			if(a.api_version > 0) {
				jkv_raw(api, "version", std::to_string(a.api_version), xf);
			}
			jkv_raw(apps, "api", api + "}", pf);
		}
		if(!a.data.empty()) {
			std::string dl = "[";
			bool df = true;
			for(const auto &d : a.data) {
				dl += df ? "" : ",";
				df = false;
				dl += "{";
				bool xf = true;
				jkv(dl, "kind", d.kind, xf);
				jkv(dl, "proto", d.proto, xf);
				jkv_raw(dl, "port", std::to_string(d.port), xf);
				jkv(dl, "dir", d.dir, xf);
				if(!d.path.empty()) {
					jkv(dl, "path", d.path, xf);
				}
				if(!d.desc.empty()) {
					jkv(dl, "desc", d.desc, xf);
				}
				dl += "}";
			}
			jkv_raw(apps, "data", dl + "]", pf);
		}
		if(!a.live_mode.empty()) {
			jkv(apps, "liveness", a.live_mode, pf);
		}
		bool installed = prop(shown, a.unit, "LoadState") == "loaded";
		std::string state = prop(shown, a.unit, "ActiveState");
		if(state.empty()) {
			state = "unknown";
		}
		jkv_raw(apps, "installed", installed ? "true" : "false", pf);
		jkv(apps, "state", state, pf);
		jkv(apps, "sub", prop(shown, a.unit, "SubState"), pf);
		if(installed && (state == "active" || state == "activating")) {
			unsigned long long ts = strtoull(prop(shown, a.unit, "ActiveEnterTimestampMonotonic").c_str(), nullptr, 10);
			if(ts) {
				double up = mono - (double)ts / 1e6;
				jkv_raw(apps, "uptime", fmt("%.1f", up < 0 ? 0 : up), pf);
			}
			std::string base = "/sys/fs/cgroup/system.slice/" + a.unit + ".service/";
			unsigned long long mem = 0, tasks = 0;
			if(read_u64(base + "memory.current", mem)) {
				jkv_raw(apps, "mem", std::to_string(mem), pf);
			}
			if(read_u64(base + "pids.current", tasks)) {
				jkv_raw(apps, "tasks", std::to_string(tasks), pf);
			}
			std::string cs;
			if(read_file(base + "cpu.stat", cs)) {
				unsigned long long usec = 0;
				if(sscanf(cs.c_str(), "usage_usec %llu", &usec) == 1) {
					double pct = cpu_percent(a.unit, usec);
					if(pct >= 0) {
						jkv_raw(apps, "cpu", fmt("%.1f", pct), pf);
					}
				}
			}
		}
		if(a.has_rf) {
			std::string ant = env_get(RF_CONF, a.rf_prefix + "_ANT");
			std::string gain = env_get(RF_CONF, a.rf_prefix + "_GAIN");
			std::string agc = env_get(RF_CONF, a.rf_prefix + "_AGC");
			bool agc_on = agc.empty() ? a.rf_agc : agc == "1";
			std::string r = "{";
			bool rf2 = true;
			jkv(r, "antenna", ant.empty() ? a.rf_ant : ant, rf2);
			jkv_raw(r, "gain", gain.empty() ? std::to_string(a.rf_gain) : gain, rf2);
			jkv_raw(r, "agc", agc_on ? "true" : "false", rf2);
			jkv_raw(r, "antennas", "[\"sma_a\",\"embed\"]", rf2);
			jkv_raw(r, "gain_min", "-24", rf2);
			jkv_raw(r, "gain_max", "30", rf2);
			jkv_raw(apps, "rf", r + "}", pf);
		}
		if(!a.config.empty()) {
			std::string c = "[";
			bool cf = true;
			for(const auto &cd : a.config) {
				c += cf ? "" : ",";
				cf = false;
				c += "{";
				bool xf = true;
				jkv(c, "key", cd.key, xf);
				std::string cur = env_get(a.dir + "/app.conf", cd.env);
				jkv(c, "value", cur.empty() ? cd.choices[0] : cur, xf);
				jkv_raw(c, "choices", jarr(cd.choices), xf);
				c += "}";
			}
			jkv_raw(apps, "config", c + "]", pf);
		}
		if(!a.b_commit.empty() || !a.b_built.empty()) {
			std::string b = "{";
			bool xf = true;
			jkv(b, "commit", a.b_commit, xf);
			jkv(b, "built", a.b_built, xf);
			jkv(b, "librfnm", a.b_librfnm, xf);
			jkv_raw(apps, "build", b + "}", pf);
		}
		// a crash-looping unit (Restart=on-failure) never latches "failed" - it cycles
		// activating/auto-restart forever, so surface its journal the same way
		if(state == "failed" || prop(shown, a.unit, "SubState") == "auto-restart") {
			jkv(apps, "result", prop(shown, a.unit, "Result"), pf);
			jkv_raw(apps, "log", jarr(journal_tail(a.unit)), pf);
		}
		apps += "}";
	}
	jkv_raw(j, "apps", apps + "]", f);
	return j + "}";
}

// ---- HTTP -----------------------------------------------------------------------------------
static bool send_all(int fd, const char *p, size_t n) {
	while(n) {
		ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
		if(w <= 0) {
			return false;
		}
		p += w;
		n -= (size_t)w;
	}
	return true;
}

static void reply(int fd, int code, const std::string &body) {
	const char *msg = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" :
	                  code == 404 ? "Not Found" : code == 409 ? "Conflict" : "Internal Server Error";
	char hdr[256];
	snprintf(hdr, sizeof(hdr),
	         "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
	         "Content-Length: %zu\r\nConnection: close\r\n\r\n", code, msg, body.size());
	if(send_all(fd, hdr, strlen(hdr))) {
		send_all(fd, body.data(), body.size());
	}
}

static void reply_err(int fd, int code, const std::string &msg, const std::vector<std::string> &log = {}) {
	std::string j = "{\"error\":\"" + jesc(msg) + "\"";
	if(!log.empty()) {
		j += ",\"log\":" + jarr(log);
	}
	reply(fd, code, j + "}");
}

static std::vector<std::string> split_path(const std::string &p, std::string &query) {
	std::string path = p;
	size_t q = path.find('?');
	if(q != std::string::npos) {
		query = path.substr(q + 1);
		path.resize(q);
	}
	std::vector<std::string> parts;
	std::istringstream is(path);
	std::string seg;
	while(std::getline(is, seg, '/')) {
		if(!seg.empty()) {
			parts.push_back(seg);
		}
	}
	return parts;
}

// restart a running app through the lease worker (radio apps) or plain systemctl (radio none)
static void do_restart(int fd, const app_mf &mf) {
	auto shown = show_units({ mf });
	bool running = prop(shown, mf.unit, "ActiveState") == "active" || prop(shown, mf.unit, "ActiveState") == "activating";
	if(!running) {
		reply(fd, 200, "{\"restarted\":false}");
		return;
	}
	if(mf.radio == "none") {
		std::string out;
		if(systemctl(out, { "--no-block", "restart", mf.unit }, 10000) != 0) {
			reply_err(fd, 500, "restart failed: " + out, journal_tail(mf.unit));
			return;
		}
		reply(fd, 200, "{\"restarted\":true}");
		return;
	}
	std::string err;
	if(!submit_transition(mf.id, true, err)) {
		reply_err(fd, 409, err);
		return;
	}
	reply(fd, 200, "{\"restarted\":true}");
}

static double g_calib_kick = 0;   // POST /v1/calib/fetch rate guard (mono seconds)

// ---- force firmware update (API.md §2.2) ------------------------------------------------
// The u-boot env flag rfnm_is_bootable gates normal boot: rfnm_bootcmd falls through to
// rfnm_run_rescue (the raw-eMMC SWUpdate recovery — auto-downloads rfnmblue.swu from
// bin.rfnm.io, or installs a USB stick's .swu) whenever the flag is unset/empty, and a
// completed install rewrites the whole env with the flag back at 1. So forcing an update
// is just deleting the flag (fw_setenv, /etc/fw_env.config → eMMC 0x400000) and rebooting.
static std::mutex g_fw_mtx;   // one env writer at a time so the write+readback pair is atomic

// flag state: 1 = set (normal boot), 0 = absent (update mode armed), -1 = unavailable (err says why)
static int fw_flag_read(std::string &err) {
	std::string out;
	int code = -1;
	if(!run_cmd({ "fw_printenv", "-n", "rfnm_is_bootable" }, 8000, out, code)) {
		err = "fw_printenv did not run";
		return -1;
	}
	if(out.find("Bad CRC") != std::string::npos) {
		// fw_setenv on a corrupt env rewrites it from bare defaults, stripping the rfnm_*
		// boot plumbing — never modify an environment that can't be read back
		err = "bootloader environment unreadable (bad CRC)";
		return -1;
	}
	if(code == 0) {
		// u-boot's gate is `test -n "$rfnm_is_bootable"`: an empty value is just as armed as
		// a deleted one — and this fw_setenv leaves an empty variable behind on "delete"
		std::string v = out;
		v.erase(v.find_last_not_of(" \t\r\n") + 1);
		return v.empty() ? 0 : 1;
	}
	if(code == 127) {
		err = "fw_printenv not installed";
		return -1;
	}
	if(out.find("not defined") != std::string::npos || out.find("not found") != std::string::npos) {
		return 0;
	}
	err = out.empty() ? "fw_printenv failed" : out.substr(0, 200);
	return -1;
}

static std::string fw_json_locked() {
	std::string err;
	int st = fw_flag_read(err);
	if(st < 0) {
		return "{\"available\":false,\"error\":\"" + jesc(err) + "\"}";
	}
	return std::string("{\"available\":true,\"armed\":") + (st == 0 ? "true" : "false") + "}";
}

static std::string fw_json() {
	std::lock_guard<std::mutex> lk(g_fw_mtx);
	return fw_json_locked();
}

static void handle_write(int fd, const std::vector<std::string> &parts, const std::string &body) {
	// POST /v1/radio {"owner":"<id>"|"none"}
	if(parts.size() == 2 && parts[1] == "radio") {
		jv req;
		if(!jparse(body, req) || req.t != jv::OBJ) {
			reply_err(fd, 400, "bad JSON body");
			return;
		}
		std::string owner = req.str("owner", "none");
		const jv *ov = req.get("owner");
		if(ov && ov->t == jv::NUL) {
			owner = "none";
		}
		if(owner == "none") {
			owner = "";
		}
		app_mf mf;
		if(!owner.empty()) {
			if(!find_mf(owner, mf)) {
				reply_err(fd, 404, "unknown app");
				return;
			}
			if(mf.radio == "none") {
				reply_err(fd, 400, mf.title + " is not a radio app");
				return;
			}
			if(mf.radio == "tx" && g_tx_policy == "0") {
				reply_err(fd, 403, "TX apps are forbidden on this board (" + g_board_name + ")");
				return;
			}
			auto shown = show_units({ mf });
			if(prop(shown, mf.unit, "LoadState") != "loaded") {
				reply_err(fd, 409, mf.title + " is not installed");
				return;
			}
		}
		std::string err;
		if(!submit_transition(owner, false, err)) {
			reply_err(fd, 409, err);
			return;
		}
		std::lock_guard<std::mutex> lk(g_r_mtx);
		g_last_ui = mono_now();   // a lease action IS presence: the full idle window from here
		reply(fd, 200, "{\"radio\":" + radio_json_locked() + "}");
		return;
	}

	// POST /v1/calib/fetch: start the rfnm-calib-fetch oneshot (60 s rate guard)
	if(parts.size() == 3 && parts[1] == "calib" && parts[2] == "fetch") {
		double now = mono_now();
		if(g_calib_kick && now - g_calib_kick < 60) {
			reply_err(fd, 429, "calibration fetch already started - re-poll /v1/hwinfo");
			return;
		}
		std::string out;
		if(systemctl(out, { "--no-block", "start", "rfnm-calib-fetch" }, 10000) != 0) {
			reply_err(fd, 500, "could not start rfnm-calib-fetch: " + out);
			return;
		}
		g_calib_kick = now;
		reply(fd, 200, "{\"started\":true}");
		return;
	}

	// POST /v1/fwupdate {"arm":bool[,"reboot":bool]}: flip the recovery-boot flag (§2.2)
	if(parts.size() == 2 && parts[1] == "fwupdate") {
		jv req;
		if(!jparse(body, req) || req.t != jv::OBJ || !req.get("arm")) {
			reply_err(fd, 400, "body must be {\"arm\":bool[,\"reboot\":bool]}");
			return;
		}
		bool arm = req.boolean("arm", false);
		bool do_reboot = arm && req.boolean("reboot", false);
		std::lock_guard<std::mutex> lk(g_fw_mtx);
		std::string err, out;
		int code = -1;
		if(fw_flag_read(err) < 0) {
			reply_err(fd, 500, err);
			return;
		}
		std::vector<std::string> cmd = { "fw_setenv", "rfnm_is_bootable" };   // no value = delete
		if(!arm) {
			cmd.push_back("1");
		}
		if(!run_cmd(cmd, 15000, out, code) || code != 0) {
			reply_err(fd, 500, "fw_setenv failed: " + out.substr(0, 200));
			return;
		}
		// require the flip to have actually landed on the eMMC before claiming anything
		if(fw_flag_read(err) != (arm ? 0 : 1)) {
			reply_err(fd, 500, "flag readback mismatch after write" + (err.empty() ? std::string() : ": " + err));
			return;
		}
		printf("appd: fwupdate %s\n", arm ? (do_reboot ? "ARMED, rebooting into SWUpdate recovery" : "ARMED (next boot = SWUpdate recovery)") : "disarmed");
		fflush(stdout);
		reply(fd, 200, std::string("{\"armed\":") + (arm ? "true" : "false") + ",\"rebooting\":" + (do_reboot ? "true" : "false") + "}");
		if(do_reboot) {
			// detached: let the HTTP response land before systemd tears the network down
			std::thread([] {
				usleep(1200 * 1000);
				std::string o;
				int c = -1;
				run_cmd({ "sync" }, 20000, o, c);
				run_cmd({ "systemctl", "reboot" }, 30000, o, c);
			}).detach();
		}
		return;
	}

	// POST /v1/rescan
	if(parts.size() == 2 && parts[1] == "rescan") {
		scan_registry();
		gen_nginx();
		reply(fd, 200, "{\"apps\":" + std::to_string(registry().size()) + "}");
		return;
	}

	if(parts.size() != 4 || parts[1] != "apps") {
		reply_err(fd, 404, "not found");
		return;
	}
	app_mf mf;
	if(!find_mf(parts[2], mf)) {
		reply_err(fd, 404, "unknown app");
		return;
	}

	// PUT /v1/apps/<id>/rf {"antenna","gain"}
	if(parts[3] == "rf") {
		if(!mf.has_rf) {
			reply_err(fd, 400, mf.title + " has no RF settings");
			return;
		}
		jv req;
		if(!jparse(body, req) || req.t != jv::OBJ) {
			reply_err(fd, 400, "bad JSON body");
			return;
		}
		std::string ant = req.str("antenna");
		if(ant != "sma_a" && ant != "embed") {
			reply_err(fd, 400, "antenna must be sma_a or embed");
			return;
		}
		int gain = (int)req.num("gain", 30);
		gain = gain < -24 ? -24 : (gain > 30 ? 30 : gain);
		bool agc = req.boolean("agc", mf.rf_agc);
		if(!env_set(RF_CONF, { { mf.rf_prefix + "_ANT", ant }, { mf.rf_prefix + "_GAIN", std::to_string(gain) },
		                       { mf.rf_prefix + "_AGC", agc ? "1" : "0" } })) {
			reply_err(fd, 500, "could not write rf.conf");
			return;
		}
		do_restart(fd, mf);
		return;
	}

	// PUT /v1/apps/<id>/config {"key","value"}
	if(parts[3] == "config") {
		jv req;
		if(!jparse(body, req) || req.t != jv::OBJ) {
			reply_err(fd, 400, "bad JSON body");
			return;
		}
		std::string key = req.str("key"), value = req.str("value");
		const cfg_def *def = nullptr;
		for(const auto &cd : mf.config) {
			if(cd.key == key) {
				def = &cd;
			}
		}
		if(!def) {
			reply_err(fd, 400, "unknown config key");
			return;
		}
		auto it = std::find(def->choices.begin(), def->choices.end(), value);
		if(it == def->choices.end()) {
			reply_err(fd, 400, key + " must be one of the declared choices");
			return;
		}
		std::vector<std::pair<std::string, std::string>> kv = { { def->env, value } };
		if(!def->flags_env.empty()) {
			// the human choice doubles as an index into values[]: units word-split the flag string
			kv.push_back({ def->flags_env, def->values[(size_t)(it - def->choices.begin())] });
		}
		if(!env_set(mf.dir + "/app.conf", kv)) {
			reply_err(fd, 500, "could not write app.conf");
			return;
		}
		if(def->restart) {
			do_restart(fd, mf);
		} else {
			reply(fd, 200, "{\"restarted\":false}");
		}
		return;
	}

	// POST /v1/apps/<id>/restart
	if(parts[3] == "restart") {
		do_restart(fd, mf);
		return;
	}
	reply_err(fd, 404, "not found");
}

// ---- GET /v1/hwinfo: EEPROM identity per board (i2c-rfnm-bootconfig sysfs, rfnm_info-style)
// + factory-calibration distribution state (rfnm-calib-fetch status ledger, per-DGB blob files,
// vsgd table header). API.md section 2.
static const char *hw_board_name(int id) {
	static const char *n[] = { "", "breakout", "granita", "lime", "motherboard", "yucca" };
	return (id >= 1 && id <= 5) ? n[id] : "unknown";
}

// Identity comes from the rfnm_show_board_info sysfs nodes, read ONCE at startup.
// Kernel contract: those nodes serve the PROBE-TIME cached identity and
// never touch the i2c wire (the DGB buses stop being i2c once the radio stack runs -
// the M7 owns I2C3_SDA as its GPT3 clock), so reading them is always
// safe at any rate. Identity cannot change at runtime; per-DGB calib_file stats
// stay live (a fetch changes them).
struct hw_board {
	int adapter;
	bool present;
	int id, rev;
	std::string serial, mac;
};
static std::vector<struct hw_board> g_hw_scan;

static void hw_scan_once(void) {
	for(int a = 0; a <= 2; a++) {
		std::string line;
		if(!read_file("/sys/bus/i2c/devices/" + std::to_string(a) + "-0050/rfnm_show_board_info", line)) {
			continue;
		}
		struct hw_board b = { a, false, 0, 0, "", "" };
		char serial[32] = {}, mac[32] = {};
		int n = sscanf(line.c_str(), "board id %d revision %d serial %31s mac-addr %31s", &b.id, &b.rev, serial, mac);
		if(n >= 3) {
			b.present = true;
			b.serial = serial;
			for(char &c : b.serial) {
				c = (char)toupper((unsigned char)c);
			}
			if(n == 4) {
				b.mac = mac;
			}
		} else if(a == 0) {
			continue;   // unreadable motherboard line: omit, matching the old shape
		}
		g_hw_scan.push_back(b);
	}
}

static std::string hwinfo_json(void) {
	std::string out = "{\"boards\":[";
	bool first = true;
	for(const auto &b : g_hw_scan) {
		if(!b.present) {
			out += std::string(first ? "" : ",") + "{\"slot\":" + std::to_string(b.adapter - 1) + ",\"present\":false}";
			first = false;
			continue;
		}
		out += std::string(first ? "" : ",") + "{" + (b.adapter == 0 ? std::string("\"motherboard\":true") : "\"slot\":" + std::to_string(b.adapter - 1))
			+ ",\"present\":true,\"type\":\"" + hw_board_name(b.id) + "\",\"id\":" + std::to_string(b.id) + ",\"rev\":" + std::to_string(b.rev)
			+ ",\"serial\":\"" + jesc(b.serial) + "\"";
		if(!b.mac.empty()) {
			out += ",\"mac\":\"" + jesc(b.mac) + "\"";
		}
		if(b.adapter > 0) {
			struct stat sb;
			std::string blob = "/lib/firmware/rfnm/calib_" + b.serial + ".json";
			if(stat(blob.c_str(), &sb) == 0) {
				out += ",\"calib_file\":{\"size\":" + std::to_string((long)sb.st_size) + ",\"mtime\":" + std::to_string((long)sb.st_mtime) + "}";
			} else {
				out += ",\"calib_file\":null";
			}
		}
		out += "}";
		first = false;
	}
	out += "]";
	std::string st;
	jv stv;
	if(read_file("/rfnm/calib/status.json", st) && jparse(st, stv) && stv.t == jv::OBJ) {
		while(!st.empty() && (st.back() == '\n' || st.back() == ' ')) {
			st.pop_back();
		}
		out += ",\"calib_fetch\":" + st;   // the fetcher's own ledger, embedded verbatim (validated JSON)
	} else {
		out += ",\"calib_fetch\":null";
	}
	std::string tbl;
	if(read_file("/rfnm/vsg/calib_pwr.tbl", tbl)) {
		std::string ser, src;
		size_t p = tbl.find("\nSERIAL ");
		if(p != std::string::npos) {
			ser = tbl.substr(p + 8, tbl.find('\n', p + 8) - p - 8);
		}
		p = tbl.find("\nSRC ");
		if(p != std::string::npos) {
			src = tbl.substr(p + 5, tbl.find('\n', p + 5) - p - 5);
		}
		size_t rows = 0;
		for(p = 0; (p = tbl.find("\nPWR ", p)) != std::string::npos; p += 5) {
			rows++;
		}
		out += ",\"vsg_tbl\":{\"serial\":\"" + jesc(ser) + "\",\"src\":\"" + jesc(src) + "\",\"points\":" + std::to_string(rows) + "}";
	} else {
		out += ",\"vsg_tbl\":null";
	}
	return out + "}";
}

static void handle_conn(int fd) {
	struct timeval tv = { 2, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	std::string req;
	char buf[2048];
	size_t hdr_end = std::string::npos;
	while(req.size() < 16384) {
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if(n <= 0) {
			break;
		}
		req.append(buf, (size_t)n);
		hdr_end = req.find("\r\n\r\n");
		if(hdr_end != std::string::npos) {
			break;
		}
	}
	if(hdr_end == std::string::npos) {
		close(fd);
		return;
	}
	char method[8] = { 0 }, rawpath[512] = { 0 };
	sscanf(req.c_str(), "%7s %511s", method, rawpath);
	size_t clen = 0;
	{
		std::string hdrs = req.substr(0, hdr_end);
		for(auto &ch : hdrs) {
			ch = (char)tolower((unsigned char)ch);
		}
		size_t cl = hdrs.find("content-length:");
		if(cl != std::string::npos) {
			clen = (size_t)strtoul(hdrs.c_str() + cl + 15, nullptr, 10);
		}
	}
	if(clen > 4096) {
		reply_err(fd, 400, "body too large");
		close(fd);
		return;
	}
	std::string body = req.substr(hdr_end + 4);
	while(body.size() < clen) {
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if(n <= 0) {
			break;
		}
		body.append(buf, (size_t)n);
	}

	std::string query;
	std::vector<std::string> parts = split_path(rawpath, query);
	if(!parts.empty() && parts[0] == "api") {
		parts.erase(parts.begin());   // canonical /api/v1/* (nginx forwards the full URI); bare /v1/* stays for loopback curls
	}
	bool v1 = !parts.empty() && parts[0] == "v1";

	if(strcmp(method, "GET") == 0 && v1) {
		if(parts.size() == 2 && parts[1] == "state") {
			reply(fd, 200, state_json());
		} else if(parts.size() == 2 && parts[1] == "hwinfo") {
			reply(fd, 200, hwinfo_json());
		} else if(parts.size() == 2 && parts[1] == "fwupdate") {
			reply(fd, 200, fw_json());
		} else if(parts.size() == 4 && parts[1] == "apps" && parts[3] == "log") {
			app_mf mf;
			if(!find_mf(parts[2], mf)) {
				reply_err(fd, 404, "unknown app");
			} else {
				int n = 40;
				size_t np = query.find("n=");
				if(np != std::string::npos) {
					n = atoi(query.c_str() + np + 2);
				}
				n = n < 1 ? 1 : (n > 200 ? 200 : n);
				reply(fd, 200, "{\"log\":" + jarr(journal_tail(mf.unit, n)) + "}");
			}
		} else {
			reply_err(fd, 404, "not found");
		}
	} else if((strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) && v1) {
		handle_write(fd, parts, body);
	} else {
		reply_err(fd, 404, "not found");
	}
	close(fd);
}

int main(int argc, char **argv) {
	int port = argc > 1 ? atoi(argv[1]) : 7683;
	signal(SIGPIPE, SIG_IGN);

	scan_registry();
	hw_scan_once();   // the one EEPROM sysfs pass this process ever makes (kernel i2c wedge defect)
	gen_nginx();
	std::thread(radio_worker).detach();
	std::thread(idle_watcher).detach();
	{
		std::lock_guard<std::mutex> lk(g_r_mtx);
		g_last_ui = mono_now();   // boot grace: nothing runs yet, but don't log idle churn
	}

	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons((uint16_t)port);
	if(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(lfd, 16) < 0) {
		perror("appd: bind/listen");
		return 1;
	}
	printf("appd: %zu apps, board %s (tx policy %s), listening on 127.0.0.1:%d\n",
	       registry().size(), g_board_name.c_str(), g_tx_policy.c_str(), port);
	fflush(stdout);

	while(true) {
		int fd = accept(lfd, nullptr, nullptr);
		if(fd < 0) {
			continue;
		}
		// one thread per connection: a long synchronous transition must never block polls
		std::thread(handle_conn, fd).detach();
	}
}
