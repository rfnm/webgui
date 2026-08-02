/*
 * RFNM spectrum stream protocol v1 — single source of truth for the on-board daemon.
 * Wire format documented in PROTOCOL.md; JS reference decoder in www/spectrum.js.
 * All fields little-endian on the wire (i.MX8MP and every client we care about are LE).
 */

#ifndef RFNM_SPECTRUM_PROTOCOL_H
#define RFNM_SPECTRUM_PROTOCOL_H

#include <stdint.h>

#define RFSP_MAGIC        0x50534652u /* "RFSP" */
#define RFSP_VERSION      1

#define RFSP_MSG_SPECTRA  1
#define RFSP_MSG_STATUS   2
#define RFSP_MSG_CONFIG   16
#define RFSP_MSG_ACK      17
#define RFSP_MSG_TUNE     18
#define RFSP_MSG_SCALE    19
#define RFSP_MSG_RATE     20   /* client -> server: zoom-driven RF span step (u32 Hz at off 16) */
#define RFSP_MSG_SWEEP    21   /* client -> server: swept full-span mode (see rfsp_sweep) */
#define RFSP_MSG_ANTENNA  22   /* client -> server: select RX RF path / antenna (see rfsp_antenna) */
#define RFSP_MSG_PATHS    23   /* server -> client: available RX paths per daughterboard + the active one */

#define RFSP_MODE_RAW     0
#define RFSP_MODE_AVG     1

struct __attribute__((packed)) rfsp_hdr {
	uint32_t magic;
	uint8_t version;
	uint8_t type;
	uint16_t flags;
	uint32_t seq;
	uint32_t payload_bytes;
};

struct __attribute__((packed)) rfsp_spectra_hdr {
	struct rfsp_hdr h;
	uint32_t n_bins;
	uint16_t n_rows;
	uint16_t mode;
	float db_min;
	float db_max;
	double center_hz;
	double span_hz;
	uint32_t fft_size;
	uint32_t accum_per_row;
	uint32_t source_rows_per_sec;
	uint32_t reserved0;
	/* uint8_t payload[n_rows][n_bins] follows */
};

/* NOTE: the wire STATUS payload is 48 bytes (this struct + the per-client pacer tail
 * documented in PROTOCOL.md; its final word carries the AGC state: byte 0 = live gain
 * as i8, byte 1 bit0 = AGC enabled). Clients feature-detect on payload length. */
struct __attribute__((packed)) rfsp_status {
	struct rfsp_hdr h;
	uint32_t fft_per_sec;
	uint32_t rows_sent_per_sec;
	uint32_t rows_dropped_per_sec;
	float dsp_time_us;
	float cpu_load;
	float dsp_load;
	uint32_t reserved0;
	uint32_t reserved1;
};

struct __attribute__((packed)) rfsp_config {
	struct rfsp_hdr h;
	uint16_t max_rows_per_sec;
	uint16_t mode;
	uint32_t fft_size_req;
	float ema_alpha;
	uint32_t reserved0;
};

struct __attribute__((packed)) rfsp_tune {
	struct rfsp_hdr h;
	double freq_hz;          /* 8-aligned (header is 16 bytes) */
	int32_t gain_db;         /* bit0: manual gain (disables AGC); bit2: gain_db carries the AGC toggle (0/1) */
	uint32_t flags;          /* bit0: gain_db valid; bit1: freq valid; bit2: AGC toggle (ignored when bit0 set) */
};

struct __attribute__((packed)) rfsp_scale {
	struct rfsp_hdr h;
	float db_min;            /* requested quantization window (server clamps span to [5,200] dB) */
	float db_max;
	uint32_t reserved0;
	uint32_t reserved1;
};

struct __attribute__((packed)) rfsp_ack {
	struct rfsp_hdr h;
	uint32_t last_seq_seen;
	uint32_t client_buffered_bytes;
};

/* Swept spectrum analyzer: hop the LO across [start_hz, stop_hz] at a FIXED per-hop
 * sample bandwidth (bw_hz, one of the rate ladder values), stitch the hops into one
 * full-span row. enable=0 returns to real-time (single-LO continuous) mode. */
struct __attribute__((packed)) rfsp_sweep {
	struct rfsp_hdr h;
	double start_hz;         /* 8-aligned (header is 16 bytes) */
	double stop_hz;
	uint32_t bw_hz;          /* per-hop capture bandwidth (== sample rate) */
	uint32_t enable;         /* 1 = sweep, 0 = real-time */
};

/* RX antenna / RF path select. The daughterboard exposes a subset (Lime: embedded + SMA-A);
 * unsupported codes are rejected by the daemon. Applied live (stream quiesced ~100-300ms).
 * channel selects the RX channel the path lives on (daughterboards = different channels);
 * a cross-channel pick makes the daemon move its RX session to that channel. Legacy clients
 * send 0 there, which is the historical single-channel behavior. */
struct __attribute__((packed)) rfsp_antenna {
	struct rfsp_hdr h;
	uint8_t path;            /* 0 = embedded antenna; 1..8 = SMA A..H; 9 = loopback */
	uint8_t channel;         /* RX channel id the path belongs to (from rfsp_paths) */
	uint8_t reserved0[2];
	uint32_t reserved1;
};

/* Server -> client at ws connect (and after any path/channel change or REJECTED switch):
 * the RX path menu. One entry per (daughterboard, path) the hardware exposes, with that
 * channel's tunable range - clients clamp every tune/zoom/sweep decision to the SELECTED
 * path's [fmin, fmax] (the antenna's limits are fixed; nothing widens past them).
 * current_idx points into entry[]. Clients must adopt this as the truth of what the radio
 * is doing NOW - it survives daemon restarts and other viewers' changes. */
#define RFSP_PATHS_MAX 16
struct __attribute__((packed)) rfsp_paths {
	struct rfsp_hdr h;
	char dgb_name[2][32];    /* daughterboard user-readable names ("" = slot empty) */
	uint8_t count;           /* valid entries */
	uint8_t current_idx;     /* index into entry[] of the active path */
	uint8_t reserved0[2];
	struct __attribute__((packed)) {
		uint8_t code;        /* path code, same encoding as rfsp_antenna.path */
		uint8_t channel;     /* RX channel id */
		uint8_t dgb;         /* daughterboard slot 0/1 (indexes dgb_name) */
		uint8_t reserved;
		uint16_t fmin_mhz;   /* channel tunable range, MHz (rounded inward) */
		uint16_t fmax_mhz;
	} entry[RFSP_PATHS_MAX];
};

#ifdef __cplusplus
static_assert(sizeof(struct rfsp_hdr) == 16, "hdr size");
static_assert(sizeof(struct rfsp_spectra_hdr) == 64, "spectra hdr size");
static_assert(sizeof(struct rfsp_sweep) == 40, "sweep msg size");
static_assert(sizeof(struct rfsp_antenna) == 24, "antenna msg size");
static_assert(sizeof(struct rfsp_paths) == 16 + 64 + 4 + 8 * RFSP_PATHS_MAX, "paths msg size");
#else
_Static_assert(sizeof(struct rfsp_hdr) == 16, "hdr size");
_Static_assert(sizeof(struct rfsp_spectra_hdr) == 64, "spectra hdr size");
_Static_assert(sizeof(struct rfsp_sweep) == 40, "sweep msg size");
#endif

#endif /* RFNM_SPECTRUM_PROTOCOL_H */
