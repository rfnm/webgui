// SigMF sidecar generator (header-only) — emit a SigMF v1.2.0 .sigmf-meta JSON document
// describing an IQ recording. Pure string building into a caller buffer (no allocation, no I/O),
// so it is trivially host-testable while the recorder that calls it (spectrumd) needs a board.
// spectrumd writes this next to its raw .cs16/.cf32 capture as a Non-Conforming Dataset (the
// core:dataset field names the data file), leaving the recorded IQ byte-for-byte unchanged.
//
// This is a self-contained vendored copy of the same generator that lives in libvsg
// (r/vsg/src/sigmf.c); spectrumd does not link libvsg, so a ~header keeps it standalone.
// Spec: https://github.com/sigmf/SigMF (v1.2.0).
#ifndef RTSA_SIGMF_H
#define RTSA_SIGMF_H

#include <cstdint>
#include <cstdio>
#include <cstring>

struct rtsa_sigmf_meta {
	const char *datatype;     // "ci16_le" (cs16) or "cf32_le" (cf32); default ci16_le
	const char *dataset;      // basename of the raw data file (Non-Conforming Dataset); optional
	uint64_t sample_rate;     // Hz
	int64_t frequency;        // capture centre, Hz
	uint32_t num_channels;    // default 1
	int gain_db;
	const char *rx_path;      // "embed" / "sma_a" / ...; optional
	const char *datetime;     // ISO-8601 UTC capture start; optional
	const char *description;  // optional
	const char *hw;           // optional; default "RFNM Lime (LA9310 / i.MX8MP)"
	const char *author;       // optional; default "RFNM"
	const char *recorder;     // optional; default "spectrumd"
};

namespace rtsa_sigmf_detail {

// Bounded writer: appends fragments until the buffer is full, then latches ok=0 and drops every
// later append, so a truncated document never emits a corrupt tail. One NUL byte is always kept in
// reserve, so out stays NUL-terminatable at w.len for both the full and the truncated case.
struct wb {
	char *p;
	size_t cap;
	size_t len;
	int ok;
};

static inline void wb_raw(struct wb *w, const char *s) {
	if(!w->ok) {
		return;
	}
	size_t n = strlen(s);
	if(w->len + n + 1 > w->cap) {
		w->ok = 0;
		return;
	}
	memcpy(w->p + w->len, s, n);
	w->len += n;
}

// Append a JSON string value (no surrounding quotes), escaping the characters JSON forbids raw.
static inline void wb_json(struct wb *w, const char *s) {
	for(; *s; s++) {
		unsigned char c = (unsigned char)*s;
		char tmp[8];
		if(c == '"') {
			wb_raw(w, "\\\"");
		} else if(c == '\\') {
			wb_raw(w, "\\\\");
		} else if(c == '\n') {
			wb_raw(w, "\\n");
		} else if(c == '\r') {
			wb_raw(w, "\\r");
		} else if(c == '\t') {
			wb_raw(w, "\\t");
		} else if(c < 0x20) {
			snprintf(tmp, sizeof(tmp), "\\u%04x", c);
			wb_raw(w, tmp);
		} else {
			tmp[0] = (char)c;
			tmp[1] = 0;
			wb_raw(w, tmp);
		}
	}
}

// Field separator: newline + indent before the first field, comma + newline + indent before the
// rest — so optional fields drop out with no dangling comma to reason about.
static inline void sep(struct wb *w, int *first, const char *indent) {
	wb_raw(w, *first ? "\n" : ",\n");
	*first = 0;
	wb_raw(w, indent);
}

static inline void v_str(struct wb *w, const char *key, const char *val) {
	wb_raw(w, "\"");
	wb_raw(w, key);
	wb_raw(w, "\": \"");
	wb_json(w, val);
	wb_raw(w, "\"");
}

static inline void v_u64(struct wb *w, const char *key, uint64_t val) {
	char t[32];
	snprintf(t, sizeof(t), "%llu", (unsigned long long)val);
	wb_raw(w, "\"");
	wb_raw(w, key);
	wb_raw(w, "\": ");
	wb_raw(w, t);
}

static inline void v_i64(struct wb *w, const char *key, int64_t val) {
	char t[32];
	snprintf(t, sizeof(t), "%lld", (long long)val);
	wb_raw(w, "\"");
	wb_raw(w, key);
	wb_raw(w, "\": ");
	wb_raw(w, t);
}

static inline void v_int(struct wb *w, const char *key, int val) {
	char t[16];
	snprintf(t, sizeof(t), "%d", val);
	wb_raw(w, "\"");
	wb_raw(w, key);
	wb_raw(w, "\": ");
	wb_raw(w, t);
}

} // namespace rtsa_sigmf_detail

// Emit the .sigmf-meta JSON for m into out[cap]. Returns the JSON length (excluding the NUL) on
// success, or -1 if the buffer was too small (out is still left NUL-terminated at the truncation).
static inline int rtsa_sigmf_meta_json(const struct rtsa_sigmf_meta *m, char *out, size_t cap) {
	using namespace rtsa_sigmf_detail;
	if(!m || !out || cap == 0) {
		return -1;
	}
	struct wb w = { out, cap, 0, 1 };
	const char *hw = m->hw ? m->hw : "RFNM Lime (LA9310 / i.MX8MP)";
	const char *author = m->author ? m->author : "RFNM";
	const char *recorder = m->recorder ? m->recorder : "spectrumd";
	uint32_t nch = m->num_channels ? m->num_channels : 1;
	int first;

	wb_raw(&w, "{\n  \"global\": {");
	first = 1;
	sep(&w, &first, "    ");
	v_str(&w, "core:datatype", m->datatype ? m->datatype : "ci16_le");
	sep(&w, &first, "    ");
	v_str(&w, "core:version", "1.2.0");
	sep(&w, &first, "    ");
	v_u64(&w, "core:sample_rate", m->sample_rate);
	sep(&w, &first, "    ");
	v_u64(&w, "core:num_channels", nch);
	if(m->dataset) {
		sep(&w, &first, "    ");
		v_str(&w, "core:dataset", m->dataset);
	}
	sep(&w, &first, "    ");
	v_str(&w, "core:hw", hw);
	sep(&w, &first, "    ");
	v_str(&w, "core:author", author);
	sep(&w, &first, "    ");
	v_str(&w, "core:recorder", recorder);
	if(m->description) {
		sep(&w, &first, "    ");
		v_str(&w, "core:description", m->description);
	}
	sep(&w, &first, "    ");
	v_int(&w, "rfnm:gain_db", m->gain_db);
	if(m->rx_path) {
		sep(&w, &first, "    ");
		v_str(&w, "rfnm:rx_path", m->rx_path);
	}

	wb_raw(&w, "\n  },\n  \"captures\": [\n    {");
	first = 1;
	sep(&w, &first, "      ");
	v_u64(&w, "core:sample_start", 0);
	sep(&w, &first, "      ");
	v_i64(&w, "core:frequency", m->frequency);
	if(m->datetime) {
		sep(&w, &first, "      ");
		v_str(&w, "core:datetime", m->datetime);
	}
	wb_raw(&w, "\n    }\n  ],\n  \"annotations\": []\n}\n");

	out[w.len] = 0;
	return w.ok ? (int)w.len : -1;
}

#endif // RTSA_SIGMF_H
