// lrx_probe_cert: continuous-streaming certification for the reorder fix. A plain librfnm
// LOCAL-transport RX client must receive gap-free buffers with NO drops over a sustained run.
// Exit 0 iff samples arrive and dropped==0.
// (Canonical home HERE like lrx_probe since 2026-07-20; the board /rfnm/spectrum/src copy is a
// provenance mirror. v2 port: get_rx_stream_stats -> get_health() rx_pkts_ok/rx_pkts_dropped.)
#include <librfnm/device.h>
#include <librfnm/rx_stream.h>
#include <cstdio>
#include <vector>
#include <librfnm/constants.h>
int main() {
	try {
		rfnm::device dev(rfnm::TRANSPORT_LOCAL);
		size_t bsz; uint8_t bpe;
		dev.set_stream_format(rfnm::STREAM_FORMAT_CS16, &bsz, &bpe);
		if (auto r = dev.set_samp_rate(61440000)) { printf("cert: samp_rate fail %d\n", r); return 2; }
		auto ch = dev.rx_ch_helper(0);
		dev.set_rx_channel_status(ch.id, RFNM_CH_RF_OFF, RFNM_CH_STREAM_OFF, false);
		dev.apply(ch.apply, true, 1000000);
		dev.set_rx_channel_freq(ch.id, RFNM_MHZ_TO_HZ(2450));
		dev.set_rx_channel_gain(ch.id, 30);
		rfnm::rx_stream s(dev, 0x1);
		if (auto r = s.start()) { printf("cert: stream start fail %d\n", r); return 2; }
		std::vector<uint8_t> buf(bsz * 64);
		void *ptrs[1] = { buf.data() };
		size_t got = 0, total = 0; uint64_t t0 = 0;
		int rounds = 0, ok = 0;
		// ~100 reads of 262144 samples @ 61.44 Msps = ~0.43 s of RF; sustained continuous stream
		for (rounds = 0; rounds < 100; rounds++) {
			auto r = s.read(ptrs, 262144, got, t0, 500000);
			if (r == RFNM_API_OK && got > 0) { total += got; ok++; }
		}
		struct rfnm::health h = {};
		dev.get_health(&h);
		s.stop();
		printf("cert: %d/%d reads ok, %zu samples, stream_stats ok=%llu dropped=%llu\n",
				ok, rounds, total, (unsigned long long)h.rx_pkts_ok, (unsigned long long)h.rx_pkts_dropped);
		return (total > 0 && h.rx_pkts_dropped == 0) ? 0 : 1;
	} catch (const std::exception &e) { printf("cert: EXC %s\n", e.what()); return 2; }
}
