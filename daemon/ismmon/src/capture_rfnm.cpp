#include "capture.h"

#include <librfnm/device.h>
#include <librfnm/rx_stream.h>

#include <cstdio>
#include <memory>
#include <vector>

namespace ismmon {

// Resolve an antenna name to an RF path; fall back to the channel's preferred path (never
// crash-loops on a bad --antenna). "embed"/"sma"/... via string_to_rf_path.
static enum rfnm_rf_path resolve_rx_path(const struct rfnm_api_rx_ch* rx, const std::string& antenna) {
	std::string avail;
	for(int i = 0; i < 10; i++) {
		if(rx->path_possible[i] == RFNM_PATH_NULL) { continue; }
		avail += " " + rfnm::device::rf_path_to_string(rx->path_possible[i]);
	}
	fprintf(stderr, "ismmond: antenna paths available:%s; preferred %s\n",
	        avail.c_str(), rfnm::device::rf_path_to_string(rx->path_preferred).c_str());
	if(antenna.empty()) { return rx->path_preferred; }
	enum rfnm_rf_path wanted = rfnm::device::string_to_rf_path(antenna);
	if(wanted == RFNM_PATH_NULL) {
		fprintf(stderr, "ismmond: antenna '%s' not understood, using preferred\n", antenna.c_str());
		return rx->path_preferred;
	}
	for(int i = 0; i < 10; i++) {
		if(rx->path_possible[i] == wanted) { return wanted; }
	}
	fprintf(stderr, "ismmond: antenna '%s' not offered by this channel, using preferred\n", antenna.c_str());
	return rx->path_preferred;
}

bool run_capture(double center_hz, double rate_hz, int gain_db, int agc, const std::string& antenna,
                 std::atomic<bool>& stop, std::atomic<uint64_t>& samples,
                 const std::function<void(const int16_t*, size_t)>& cb) {
	std::unique_ptr<rfnm::device> dev;
	try {
		dev.reset(new rfnm::device(rfnm::TRANSPORT_LOCAL, ""));
	} catch(const std::exception& e) {
		fprintf(stderr, "ismmond: device open failed: %s\n", e.what());
		return false;
	}

	rfnm_api_failcode err = dev->set_stream_format(rfnm::STREAM_FORMAT_CS16);
	if(err) { fprintf(stderr, "ismmond: set_stream_format: %s\n", rfnm::device::failcode_to_string(err)); return false; }
	err = dev->set_samp_rate((uint64_t)rate_hz, 2000000);
	if(err) { fprintf(stderr, "ismmond: set_samp_rate(%.0f): %s\n", rate_hz, rfnm::device::failcode_to_string(err)); return false; }
	err = dev->rx_disable_stale_channels(20000000);
	if(err) { fprintf(stderr, "ismmond: rx_disable_stale_channels: %s\n", rfnm::device::failcode_to_string(err)); return false; }

	const struct rfnm_api_rx_ch* rx = dev->get_rx_channel(0);
	int gain = gain_db;
	if(gain < rx->gain_range.min) { gain = rx->gain_range.min; }
	if(gain > rx->gain_range.max) { fprintf(stderr, "ismmond: gain %d clamped to max %d\n", gain_db, rx->gain_range.max); gain = rx->gain_range.max; }
	enum rfnm_rf_path path = resolve_rx_path(rx, antenna);

	dev->set_rx_channel_status(0, RFNM_CH_RF_OFF, RFNM_CH_STREAM_AUTO);
	dev->set_rx_channel_freq(0, (int64_t)center_hz);
	dev->set_rx_channel_gain(0, (int8_t)gain);
	dev->set_rx_channel_agc(0, agc ? RFNM_AGC_DEFAULT : RFNM_AGC_OFF);
	dev->set_rx_channel_path(0, path);
	err = dev->apply(rfnm::rx_channel_apply_flags[0], true, 20000000);
	if(err) { fprintf(stderr, "ismmond: apply: %s\n", rfnm::device::failcode_to_string(err)); return false; }

	std::unique_ptr<rfnm::rx_stream> stream(dev->rx_stream_create(rfnm::channel_flags[0]));
	err = stream->start();
	if(err) { fprintf(stderr, "ismmond: stream start: %s\n", rfnm::device::failcode_to_string(err)); return false; }

	std::vector<int16_t> raw(1 << 21);   // up to 1M complex per read
	void* buffs[1] = { raw.data() };
	int dry_reads = 0;
	while(!stop) {
		size_t n = 0;
		uint64_t ts = 0;
		rfnm_api_failcode r = stream->read(buffs, raw.size() / 2, n, ts, 100000);
		if(r != RFNM_API_OK || !n) {
			if(++dry_reads >= 300) {
				fprintf(stderr, "ismmond: no samples for 30 s (last: %s), giving up\n", rfnm::device::failcode_to_string(r));
				stream->stop();
				return false;
			}
			continue;
		}
		dry_reads = 0;
		samples += n;
		cb(raw.data(), n);
	}
	stream->stop();
	return true;
}

} // namespace ismmon
