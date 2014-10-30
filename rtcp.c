#include "realtime.h"

/*

	RTCP management

	used for getting absolute PTS (in milliseconds) for each stream


*/

uint64_t ntp_milliseconds(char *buf) {
	uint64_t ms = uwsgi_be32(buf+8) * 1000;
	ms += uwsgi_be32(buf+12) / (1000 * 1000);
	return ms;
}

int rtcp_video_parse(struct realtime_config *rc, char *buf, size_t len) {
	// get NTP date (seconds since 1 january 1900)
	rc->video_rtcp_ntp = ntp_milliseconds(buf);
	uwsgi_log("[video-ntp] %llu\n", rc->video_rtcp_ntp, uwsgi_be32(buf+12));
	// get RTP timestamp relative to the NTP date
	rc->video_rtcp_ts = uwsgi_be32(buf+16);
	return 0;
}

int rtcp_audio_parse(struct realtime_config *rc, char *buf, size_t len) {
        // get NTP date (seconds since 1 january 1900)
        rc->audio_rtcp_ntp = ntp_milliseconds(buf);
	uwsgi_log("[audio-ntp] %llu\n", rc->audio_rtcp_ntp);
        // get RTP timestamp relative to the NTP date
        rc->audio_rtcp_ts = uwsgi_be32(buf+16);
        return 0;
}

uint64_t rtcp_video_ts(struct realtime_config *rc, char *buf) {
	uint32_t ts = uwsgi_be32(buf+4);
	uint64_t delta = ts - rc->video_rtcp_ts;
	uint64_t hz = (rc->video_rtcp_ntp * 90) + delta;
	return (hz/90);
}

uint64_t rtcp_audio_ts(struct realtime_config *rc, char *buf) {
	uint32_t ts = uwsgi_be32(buf+4);
	uint64_t delta = ts - rc->audio_rtcp_ts;
	uint64_t hz = (rc->audio_rtcp_ntp * 44) + delta;
	return (hz/44);
}
