// League Recorder's media tool: audio jobs on recordings, with the FFmpeg libraries OBS bundles (it
// lives next to them in OBS's bin\64bit folder). The core runs it once per job.
//
//   league-recorder-media.exe tracks <in.mp4> <out folder> <name prefix>
//       Copies each audio track into its own file, <prefix>-<n>.m4a (n counts the audio tracks from
//       0), without re-encoding: the app plays them separately so tracks can be switched on and off.
//       Prints "track <n> <offset>" for each: seconds from the start of the video to the track's
//       first sample (the files themselves start at 0).
//   league-recorder-media.exe export <in.mp4> <out.mp4> <n,n,...>
//       Copies the video as it is and mixes the listed audio tracks into one AAC track (a single
//       listed track is copied as it is): a file with only the sounds you want, for uploading or editing.
//   league-recorder-media.exe thumbnail <in.mp4> <out.jpg> <seconds> <width>
//       The frame at <seconds> as a JPEG <width> pixels wide: for videos Windows can't decode (e.g. AV1
//       without Microsoft's AV1 Video Extension), so recordings still get thumbnails.
//   league-recorder-media.exe encode <in.mp4> <out.mp4> [from=<s>] [to=<s>] [height=<px>] [fps=<n>] kbps=<video kbps>
//                             [akbps=<audio kbps>] [tracks=<n,n,...>] [separate=1] [fragmented=1]
//       Part of the video encoded again as H.264 (the graphics card's encoder where there is one, prints "encoder <name>")
//       and AAC: smaller (height, fps, bitrate) and/or cut to the exact frame. The listed audio tracks are mixed into
//       one, or with separate=1 each kept as its own. fragmented=1 writes it the way the recordings are (a fragment at
//       each keyframe), else with its index in front.
//   league-recorder-media.exe pixels <in.mp4> <out.rgb> <x,y,w,h[;x,y,w,h...]> <seconds,seconds,...> [keyframes]
//       One or more areas of the frame at each time, as raw RGB one after another (each area's times, then the
//       next area's): for reading the game's HUD (which abilities were ready) after a game. With keyframes, each
//       time is read at the keyframe at or before it, decoded on its own (prints "keyframe <n> <seconds>"). Decoded on the graphics card where it can be (prints
//       "decoder graphics card", or "decoder processor: <why>"); LR_MEDIA_CPU=1 keeps it on the processor.
//
// stdout: "progress <0-1>" lines while working, then "done"; on failure "error <message>" and exit code 1.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

static const int RATE = 48000;

struct Failure {
	std::string message;
};

static std::string ff_error(int code)
{
	char buf[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, buf, sizeof(buf));
	return buf;
}

/** FFmpeg calls that return < 0 on failure */
static int check(int code, const char *what)
{
	if (code < 0)
		throw Failure{std::string(what) + ": " + ff_error(code)};
	return code;
}

static std::string utf8(const wchar_t *w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 1)
		WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

static std::wstring wide(const std::string &s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1)
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
	return w;
}

static void say(const std::string &line)
{
	fputs((line + "\n").c_str(), stdout);
	fflush(stdout);
}

// ---------- input ----------

struct Input {
	AVFormatContext *ctx = nullptr;
	int video = -1;
	std::vector<int> audio; // stream indexes of the audio tracks, in order
	double duration = 0;    // seconds, for progress

	explicit Input(const std::string &path)
	{
		check(avformat_open_input(&ctx, path.c_str(), nullptr, nullptr), "Couldn't open the video");
		check(avformat_find_stream_info(ctx, nullptr), "Couldn't read the video");
		for (unsigned i = 0; i < ctx->nb_streams; i++) {
			AVMediaType type = ctx->streams[i]->codecpar->codec_type;
			if (type == AVMEDIA_TYPE_VIDEO && video < 0)
				video = (int)i;
			else if (type == AVMEDIA_TYPE_AUDIO)
				audio.push_back((int)i);
		}
		if (ctx->duration > 0)
			duration = ctx->duration / (double)AV_TIME_BASE;
	}
	~Input() { avformat_close_input(&ctx); }

	/** Seconds of the stream's first timestamp (0 if unknown) */
	double start(int stream) const
	{
		AVStream *s = ctx->streams[stream];
		return s->start_time == AV_NOPTS_VALUE ? 0 : s->start_time * av_q2d(s->time_base);
	}
};

/** Reports progress at most every 1% */
struct Progress {
	double total;
	int last = -1;
	void at(double seconds)
	{
		if (total <= 0)
			return;
		int pct = (int)std::min(100.0, std::max(0.0, seconds / total * 100));
		if (pct == last)
			return;
		last = pct;
		char line[32];
		snprintf(line, sizeof(line), "progress %.2f", pct / 100.0);
		say(line);
	}
};

static AVFormatContext *open_output(const std::string &path, const char *format)
{
	AVFormatContext *out = nullptr;
	check(avformat_alloc_output_context2(&out, nullptr, format, path.c_str()), "Couldn't create the output");
	return out;
}

static AVStream *copy_stream(AVFormatContext *out, AVStream *from)
{
	AVStream *s = avformat_new_stream(out, nullptr);
	if (!s)
		throw Failure{"Couldn't add a stream"};
	check(avcodec_parameters_copy(s->codecpar, from->codecpar), "Couldn't copy the stream");
	s->codecpar->codec_tag = 0;
	s->time_base = from->time_base;
	return s;
}

static void begin(AVFormatContext *out, const std::string &path, const char *movflags)
{
	check(avio_open(&out->pb, path.c_str(), AVIO_FLAG_WRITE), "Couldn't write the output");
	AVDictionary *opts = nullptr;
	if (movflags)
		av_dict_set(&opts, "movflags", movflags, 0);
	int r = avformat_write_header(out, &opts);
	av_dict_free(&opts);
	check(r, "Couldn't start the output");
}

static void finish(AVFormatContext *&out)
{
	if (!out)
		return;
	if (out->pb) {
		av_write_trailer(out);
		avio_closep(&out->pb);
	}
	avformat_free_context(out);
	out = nullptr;
}

// ---------- tracks: each audio track to its own file ----------

static void run_tracks(const std::string &in_path, const std::string &folder, const std::string &prefix)
{
	Input in(in_path);
	if (in.audio.empty())
		throw Failure{"The video has no audio"};
	double video_start = in.video >= 0 ? in.start(in.video) : 0;

	std::vector<AVFormatContext *> outs(in.audio.size(), nullptr);
	std::vector<int64_t> first(in.audio.size(), AV_NOPTS_VALUE);
	try {
		for (size_t n = 0; n < in.audio.size(); n++) {
			std::string path = folder + "\\" + prefix + "-" + std::to_string(n) + ".m4a";
			outs[n] = open_output(path, "mp4");
			copy_stream(outs[n], in.ctx->streams[in.audio[n]]);
			// moov at the front, so the player can start without reading to the end
			begin(outs[n], path, "+faststart");
		}
		// Only audio is read: the demuxer skips over the video's data
		for (unsigned i = 0; i < in.ctx->nb_streams; i++)
			if (std::find(in.audio.begin(), in.audio.end(), (int)i) == in.audio.end())
				in.ctx->streams[i]->discard = AVDISCARD_ALL;

		Progress progress{in.duration};
		AVPacket *pkt = av_packet_alloc();
		while (av_read_frame(in.ctx, pkt) >= 0) {
			auto it = std::find(in.audio.begin(), in.audio.end(), pkt->stream_index);
			if (it != in.audio.end()) {
				size_t n = it - in.audio.begin();
				AVStream *src = in.ctx->streams[pkt->stream_index];
				if (first[n] == AV_NOPTS_VALUE)
					first[n] = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
				progress.at((pkt->dts - first[n]) * av_q2d(src->time_base));
				// Each file starts at 0; its offset from the video is reported instead
				if (pkt->pts != AV_NOPTS_VALUE)
					pkt->pts -= first[n];
				if (pkt->dts != AV_NOPTS_VALUE)
					pkt->dts -= first[n];
				pkt->stream_index = 0;
				av_packet_rescale_ts(pkt, src->time_base, outs[n]->streams[0]->time_base);
				pkt->pos = -1;
				check(av_interleaved_write_frame(outs[n], pkt), "Couldn't write a track");
			}
			av_packet_unref(pkt);
		}
		av_packet_free(&pkt);
		for (size_t n = 0; n < outs.size(); n++) {
			finish(outs[n]);
			AVStream *src = in.ctx->streams[in.audio[n]];
			double offset = first[n] == AV_NOPTS_VALUE ? 0 : first[n] * av_q2d(src->time_base) - video_start;
			char line[64];
			snprintf(line, sizeof(line), "track %zu %.4f", n, offset);
			say(line);
		}
	} catch (...) {
		for (auto *&out : outs)
			finish(out);
		throw;
	}
}

// ---------- export: video copied, chosen audio tracks mixed ----------

/** One audio track being decoded into 48 kHz stereo float for the mix */
struct Track {
	int stream = -1;
	AVCodecContext *dec = nullptr;
	SwrContext *swr = nullptr;
	std::deque<float> left, right;
	int64_t start = AV_NOPTS_VALUE; // sample index (at 48 kHz) of the first buffered sample
	int64_t next = AV_NOPTS_VALUE;  // sample index just after the last buffered sample
	bool ended = false;

	~Track()
	{
		avcodec_free_context(&dec);
		swr_free(&swr);
	}

	/** Samples from `at` (zeros outside what was decoded); drops everything before `at + count` */
	float take(int64_t at, int count, int channel, int i) const
	{
		int64_t pos = at + i;
		if (start == AV_NOPTS_VALUE || pos < start || pos >= next)
			return 0.0f;
		const std::deque<float> &q = channel == 0 ? left : right;
		return q[(size_t)(pos - start)];
	}

	void drop_before(int64_t at)
	{
		if (start == AV_NOPTS_VALUE)
			return;
		while (start < at && !left.empty()) {
			left.pop_front();
			right.pop_front();
			start++;
		}
		if (left.empty())
			start = next;
	}

	/** Append a decoded frame at its timestamp, filling gaps with silence */
	void push(AVFrame *frame, AVRational tb)
	{
		AVChannelLayout stereo;
		av_channel_layout_default(&stereo, 2);
		if (!swr) {
			check(swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLTP, RATE, &frame->ch_layout, (AVSampleFormat)frame->format,
						  frame->sample_rate, 0, nullptr),
			      "Couldn't set up the audio conversion");
			check(swr_init(swr), "Couldn't set up the audio conversion");
		}
		int max_out = swr_get_out_samples(swr, frame->nb_samples);
		std::vector<float> l(max_out > 0 ? max_out : 1), r(l.size());
		uint8_t *planes[2] = {reinterpret_cast<uint8_t *>(l.data()), reinterpret_cast<uint8_t *>(r.data())};
		int got = check(swr_convert(swr, planes, (int)l.size(), (const uint8_t **)frame->extended_data, frame->nb_samples),
				"Couldn't convert the audio");
		int64_t at = frame->pts != AV_NOPTS_VALUE ? av_rescale_q(frame->pts, tb, AVRational{1, RATE}) : next;
		if (start == AV_NOPTS_VALUE) {
			start = next = at;
		} else if (at > next) {
			// A gap: silence until this frame (small jitter of a few samples is just appended)
			if (at - next > 32)
				for (; next < at; next++) {
					left.push_back(0);
					right.push_back(0);
				}
		}
		for (int i = 0; i < got; i++) {
			left.push_back(l[i]);
			right.push_back(r[i]);
		}
		next += got;
	}
};

static void run_export(const std::string &in_path, const std::string &out_path, const std::vector<int> &chosen)
{
	Input in(in_path);
	if (in.video < 0)
		throw Failure{"The file has no video"};
	for (int n : chosen)
		if (n < 0 || n >= (int)in.audio.size())
			throw Failure{"There's no audio track " + std::to_string(n)};

	AVFormatContext *out = open_output(out_path, "mp4");
	AVCodecContext *enc = nullptr;
	AVFrame *frame = nullptr;
	AVPacket *pkt = av_packet_alloc();
	std::vector<Track *> tracks;
	try {
		AVStream *video_out = copy_stream(out, in.ctx->streams[in.video]);
		AVStream *audio_out = nullptr;
		bool copy_audio = chosen.size() == 1;
		int copy_from = copy_audio ? in.audio[chosen[0]] : -1;
		if (copy_audio) {
			audio_out = copy_stream(out, in.ctx->streams[copy_from]);
		} else if (!chosen.empty()) {
			for (int n : chosen) {
				auto *t = new Track();
				tracks.push_back(t);
				t->stream = in.audio[n];
				AVCodecParameters *par = in.ctx->streams[t->stream]->codecpar;
				const AVCodec *codec = avcodec_find_decoder(par->codec_id);
				if (!codec)
					throw Failure{"No decoder for an audio track"};
				t->dec = avcodec_alloc_context3(codec);
				check(avcodec_parameters_to_context(t->dec, par), "Couldn't read an audio track");
				t->dec->pkt_timebase = in.ctx->streams[t->stream]->time_base;
				check(avcodec_open2(t->dec, codec, nullptr), "Couldn't open an audio track");
			}
			const AVCodec *aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
			if (!aac)
				throw Failure{"No AAC encoder"};
			enc = avcodec_alloc_context3(aac);
			av_channel_layout_default(&enc->ch_layout, 2);
			enc->sample_rate = RATE;
			enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
			enc->bit_rate = 192000;
			enc->time_base = AVRational{1, RATE};
			if (out->oformat->flags & AVFMT_GLOBALHEADER)
				enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			check(avcodec_open2(enc, aac, nullptr), "Couldn't start the audio encoder");
			audio_out = avformat_new_stream(out, nullptr);
			check(avcodec_parameters_from_context(audio_out->codecpar, enc), "Couldn't set up the audio");
			audio_out->time_base = enc->time_base;
			frame = av_frame_alloc();
		}
		begin(out, out_path, nullptr);

		for (unsigned i = 0; i < in.ctx->nb_streams; i++) {
			bool used = (int)i == in.video || (int)i == copy_from ||
				    std::any_of(tracks.begin(), tracks.end(), [&](Track *t) { return t->stream == (int)i; });
			if (!used)
				in.ctx->streams[i]->discard = AVDISCARD_ALL;
		}

		int64_t mix_at = AV_NOPTS_VALUE; // next sample index to encode
		auto encode = [&](AVFrame *f) {
			check(avcodec_send_frame(enc, f), "Couldn't encode the audio");
			AVPacket *p = av_packet_alloc();
			while (avcodec_receive_packet(enc, p) >= 0) {
				p->stream_index = audio_out->index;
				av_packet_rescale_ts(p, enc->time_base, audio_out->time_base);
				check(av_interleaved_write_frame(out, p), "Couldn't write the audio");
			}
			av_packet_free(&p);
		};
		// Mix and encode every whole frame the tracks have all reached (or `all` at the end)
		auto mix = [&](bool all) {
			if (tracks.empty())
				return;
			if (mix_at == AV_NOPTS_VALUE) {
				// Start once every track has begun (or ended without audio)
				for (Track *t : tracks)
					if (t->start == AV_NOPTS_VALUE && !t->ended)
						return;
				for (Track *t : tracks)
					if (t->start != AV_NOPTS_VALUE && (mix_at == AV_NOPTS_VALUE || t->start < mix_at))
						mix_at = t->start;
				if (mix_at == AV_NOPTS_VALUE)
					return;
			}
			int size = enc->frame_size > 0 ? enc->frame_size : 1024;
			for (;;) {
				int64_t ready = INT64_MAX;
				int64_t latest = INT64_MIN;
				for (Track *t : tracks) {
					int64_t end = t->next == AV_NOPTS_VALUE ? mix_at : t->next;
					latest = std::max(latest, end);
					if (!t->ended)
						ready = std::min(ready, end);
				}
				if (all)
					ready = latest;
				if (ready - mix_at < size && !(all && ready > mix_at))
					return;
				int count = (int)std::min<int64_t>(size, ready - mix_at);
				av_frame_unref(frame);
				frame->nb_samples = size;
				frame->format = AV_SAMPLE_FMT_FLTP;
				frame->sample_rate = RATE;
				av_channel_layout_default(&frame->ch_layout, 2);
				check(av_frame_get_buffer(frame, 0), "Couldn't make an audio frame");
				for (int ch = 0; ch < 2; ch++) {
					float *dst = reinterpret_cast<float *>(frame->data[ch]);
					for (int i = 0; i < size; i++) {
						float sum = 0;
						if (i < count)
							for (Track *t : tracks)
								sum += t->take(mix_at, size, ch, i);
						dst[i] = std::max(-1.0f, std::min(1.0f, sum));
					}
				}
				frame->pts = mix_at;
				encode(frame);
				mix_at += count;
				for (Track *t : tracks)
					t->drop_before(mix_at);
				if (count < size)
					return;
			}
		};

		AVFrame *decoded = av_frame_alloc();
		auto drain = [&](Track *t) {
			while (avcodec_receive_frame(t->dec, decoded) >= 0) {
				t->push(decoded, in.ctx->streams[t->stream]->time_base);
				av_frame_unref(decoded);
			}
		};

		Progress progress{in.duration};
		double video_start = in.start(in.video);
		while (av_read_frame(in.ctx, pkt) >= 0) {
			AVStream *src = in.ctx->streams[pkt->stream_index];
			if (pkt->stream_index == in.video) {
				if (pkt->dts != AV_NOPTS_VALUE)
					progress.at(pkt->dts * av_q2d(src->time_base) - video_start);
				pkt->stream_index = video_out->index;
				av_packet_rescale_ts(pkt, src->time_base, video_out->time_base);
				pkt->pos = -1;
				check(av_interleaved_write_frame(out, pkt), "Couldn't write the video");
			} else if (pkt->stream_index == copy_from) {
				pkt->stream_index = audio_out->index;
				av_packet_rescale_ts(pkt, src->time_base, audio_out->time_base);
				pkt->pos = -1;
				check(av_interleaved_write_frame(out, pkt), "Couldn't write the audio");
			} else {
				for (Track *t : tracks) {
					if (t->stream != pkt->stream_index)
						continue;
					// A broken packet is skipped rather than failing the whole export
					if (avcodec_send_packet(t->dec, pkt) >= 0)
						drain(t);
				}
				mix(false);
			}
			av_packet_unref(pkt);
		}
		for (Track *t : tracks) {
			avcodec_send_packet(t->dec, nullptr);
			drain(t);
			t->ended = true;
		}
		mix(true);
		if (enc)
			encode(nullptr);
		av_frame_free(&decoded);
		finish(out);
	} catch (...) {
		finish(out);
		DeleteFileW(wide(out_path).c_str());
		for (Track *t : tracks)
			delete t;
		avcodec_free_context(&enc);
		av_frame_free(&frame);
		av_packet_free(&pkt);
		throw;
	}
	for (Track *t : tracks)
		delete t;
	avcodec_free_context(&enc);
	av_frame_free(&frame);
	av_packet_free(&pkt);
}

// ---------- encode: part of a video encoded again ----------
//
// For the Clip Vault: a copy small enough to paste into Discord (a lower bitrate, and smaller or fewer frames), and a
// trim cut to the exact frame instead of the keyframe before it. H.264 on the graphics card where there's an encoder
// for it (NVIDIA, AMD, Intel), else x264 on the processor; audio as AAC, the chosen tracks mixed into one or each kept.

struct EncodeOptions {
	double from = 0;           // seconds into the video
	double to = 0;             // 0: to the end
	int height = 0;            // 0: as it is (never made bigger)
	double max_fps = 0;        // 0: as it is
	int video_kbps = 0;
	int audio_kbps = 128;
	std::vector<int> tracks;   // audio tracks, by their order in the file
	bool separate = false;     // each track kept as its own (else mixed into one)
	bool fragmented = false;   // a fragment at every keyframe, like the recordings (else the index in front)
};

static const char *ENCODERS[] = {"h264_nvenc", "h264_amf", "h264_qsv", "libx264"};

/** The first H.264 encoder that opens with these settings (LR_MEDIA_ENCODER picks one, for tests) */
static AVCodecContext *open_video_encoder(int width, int height, AVRational time_base, AVRational fps, int kbps, bool global_header,
					   std::string &name)
{
	char wanted[64] = {};
	GetEnvironmentVariableA("LR_MEDIA_ENCODER", wanted, sizeof(wanted));
	std::string tried;
	for (const char *candidate : ENCODERS) {
		if (wanted[0] && strcmp(wanted, candidate) != 0)
			continue;
		const AVCodec *codec = avcodec_find_encoder_by_name(candidate);
		if (!codec)
			continue;
		AVPixelFormat format = AV_PIX_FMT_NONE;
		for (const AVPixelFormat *f = codec->pix_fmts; f && *f != AV_PIX_FMT_NONE; f++)
			if (*f == AV_PIX_FMT_YUV420P || (*f == AV_PIX_FMT_NV12 && format == AV_PIX_FMT_NONE))
				format = *f;
		if (format == AV_PIX_FMT_NONE)
			continue;
		AVCodecContext *enc = avcodec_alloc_context3(codec);
		enc->width = width;
		enc->height = height;
		enc->pix_fmt = format;
		enc->time_base = time_base;
		enc->framerate = fps;
		enc->bit_rate = (int64_t)kbps * 1000;
		enc->rc_max_rate = enc->bit_rate;
		enc->rc_buffer_size = (int)std::min<int64_t>(INT_MAX, enc->bit_rate * 2);
		enc->gop_size = std::max(1, (int)(2 * av_q2d(fps) + 0.5));
		if (global_header)
			enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		// No B-frames on the graphics card's encoders: their timestamps for them don't fit the MP4 muxer (NVENC gave
		// "pts < dts")
		if (strcmp(candidate, "libx264") != 0)
			enc->max_b_frames = 0;
		// A steady bitrate, so the file comes out the size it was asked for
		if (!strcmp(candidate, "h264_nvenc")) {
			av_opt_set(enc->priv_data, "preset", "p5", 0);
			av_opt_set(enc->priv_data, "rc", "cbr", 0);
		} else if (!strcmp(candidate, "h264_amf")) {
			av_opt_set(enc->priv_data, "rc", "cbr", 0);
			av_opt_set(enc->priv_data, "quality", "quality", 0);
		} else if (!strcmp(candidate, "libx264")) {
			av_opt_set(enc->priv_data, "preset", "faster", 0);
		}
		int r = avcodec_open2(enc, codec, nullptr);
		if (r >= 0) {
			name = candidate;
			return enc;
		}
		tried += std::string(tried.empty() ? "" : "; ") + candidate + ": " + ff_error(r);
		avcodec_free_context(&enc);
	}
	throw Failure{"No video encoder could start" + (tried.empty() ? std::string() : " (" + tried + ")")};
}

static AVCodecContext *open_aac(int kbps, bool global_header)
{
	const AVCodec *aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!aac)
		throw Failure{"No AAC encoder"};
	AVCodecContext *enc = avcodec_alloc_context3(aac);
	av_channel_layout_default(&enc->ch_layout, 2);
	enc->sample_rate = RATE;
	enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
	enc->bit_rate = (int64_t)kbps * 1000;
	enc->time_base = AVRational{1, RATE};
	if (global_header)
		enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	check(avcodec_open2(enc, aac, nullptr), "Couldn't start the audio encoder");
	return enc;
}

static void run_encode(const std::string &in_path, const std::string &out_path, const EncodeOptions &o)
{
	Input in(in_path);
	if (in.video < 0)
		throw Failure{"The file has no video"};
	for (int n : o.tracks)
		if (n < 0 || n >= (int)in.audio.size())
			throw Failure{"There's no audio track " + std::to_string(n)};
	AVStream *vs = in.ctx->streams[in.video];
	double video_start = in.start(in.video);
	double to = o.to > o.from ? o.to : (in.duration > 0 ? in.duration : 1e9);
	AVRational tb = vs->time_base;

	AVFormatContext *out = open_output(out_path, "mp4");
	bool global_header = out->oformat->flags & AVFMT_GLOBALHEADER;
	AVCodecContext *dec = nullptr, *venc = nullptr;
	std::vector<AVCodecContext *> aencs;
	std::vector<Track *> tracks;
	SwsContext *sws = nullptr;
	AVFrame *frame = av_frame_alloc(), *scaled = av_frame_alloc(), *audio = av_frame_alloc();
	AVPacket *pkt = av_packet_alloc(), *got = av_packet_alloc();
	try {
		const AVCodec *codec = avcodec_find_decoder(vs->codecpar->codec_id);
		if (!codec)
			throw Failure{"No decoder for the video"};
		dec = avcodec_alloc_context3(codec);
		check(avcodec_parameters_to_context(dec, vs->codecpar), "Couldn't read the video");
		dec->pkt_timebase = tb;
		dec->thread_count = 0;
		check(avcodec_open2(dec, codec, nullptr), "Couldn't open the video");

		// The size and rate it's made at
		int src_w = vs->codecpar->width, src_h = vs->codecpar->height;
		int out_h = o.height > 0 && o.height < src_h ? o.height : src_h;
		out_h &= ~1;
		int out_w = (int)(src_w * (double)out_h / src_h / 2 + 0.5) * 2;
		AVRational fps = av_guess_frame_rate(in.ctx, vs, nullptr);
		if (fps.num <= 0 || fps.den <= 0)
			fps = AVRational{60, 1};
		bool fewer = o.max_fps > 0 && av_q2d(fps) > o.max_fps + 0.01;
		if (fewer)
			fps = av_d2q(o.max_fps, 1000);
		std::string encoder;
		venc = open_video_encoder(out_w, out_h, tb, fps, o.video_kbps, global_header, encoder);
		say("encoder " + encoder);
		AVStream *vout = avformat_new_stream(out, nullptr);
		check(avcodec_parameters_from_context(vout->codecpar, venc), "Couldn't set up the video");
		vout->time_base = tb;

		// The audio: decoded tracks (Track, as the export mixes them), and an encoder for the mix or for each
		for (int n : o.tracks) {
			auto *t = new Track();
			tracks.push_back(t);
			t->stream = in.audio[n];
			AVCodecParameters *par = in.ctx->streams[t->stream]->codecpar;
			const AVCodec *ac = avcodec_find_decoder(par->codec_id);
			if (!ac)
				throw Failure{"No decoder for an audio track"};
			t->dec = avcodec_alloc_context3(ac);
			check(avcodec_parameters_to_context(t->dec, par), "Couldn't read an audio track");
			t->dec->pkt_timebase = in.ctx->streams[t->stream]->time_base;
			check(avcodec_open2(t->dec, ac, nullptr), "Couldn't open an audio track");
		}
		size_t outputs = tracks.empty() ? 0 : o.separate ? tracks.size() : 1;
		std::vector<AVStream *> aouts;
		for (size_t i = 0; i < outputs; i++) {
			aencs.push_back(open_aac(o.audio_kbps, global_header));
			AVStream *s = avformat_new_stream(out, nullptr);
			check(avcodec_parameters_from_context(s->codecpar, aencs.back()), "Couldn't set up the audio");
			s->time_base = AVRational{1, RATE};
			aouts.push_back(s);
		}
		begin(out, out_path, o.fragmented ? "frag_keyframe+empty_moov+default_base_moof" : "+faststart");

		for (unsigned i = 0; i < in.ctx->nb_streams; i++) {
			bool used = (int)i == in.video || std::any_of(tracks.begin(), tracks.end(), [&](Track *t) { return t->stream == (int)i; });
			if (!used)
				in.ctx->streams[i]->discard = AVDISCARD_ALL;
		}
		// From the keyframe at or before the start: the frames before it are decoded and dropped
		int64_t seek_to = (int64_t)((o.from + video_start) / av_q2d(tb));
		if (o.from > 0)
			av_seek_frame(in.ctx, in.video, seek_to, AVSEEK_FLAG_BACKWARD);

		auto write = [&](AVCodecContext *enc, AVFrame *f, AVStream *s) {
			check(avcodec_send_frame(enc, f), "Couldn't encode");
			while (avcodec_receive_packet(enc, got) >= 0) {
				got->stream_index = s->index;
				av_packet_rescale_ts(got, enc->time_base, s->time_base);
				check(av_interleaved_write_frame(out, got), "Couldn't write the output");
			}
		};

		// The audio, in step with the video: every whole frame all tracks have reached up to `until` (48 kHz samples
		// on the video's clock), or all of it at the end
		int64_t s_first = (int64_t)llround((o.from + video_start) * RATE);
		int64_t s_end = (int64_t)llround((to + video_start) * RATE);
		int64_t s_next = s_first;
		int size = aencs.empty() ? 1024 : (aencs[0]->frame_size > 0 ? aencs[0]->frame_size : 1024);
		auto pump_audio = [&](int64_t until, bool all) {
			while (!aencs.empty() && s_next < s_end) {
				int64_t want = std::min(s_next + size, s_end);
				if (!all) {
					if (want > until)
						return;
					for (Track *t : tracks)
						if (!t->ended && (t->next == AV_NOPTS_VALUE || t->next < want))
							return;
				}
				int count = (int)(want - s_next);
				for (size_t a = 0; a < aencs.size(); a++) {
					av_frame_unref(audio);
					audio->nb_samples = size;
					audio->format = AV_SAMPLE_FMT_FLTP;
					audio->sample_rate = RATE;
					av_channel_layout_default(&audio->ch_layout, 2);
					check(av_frame_get_buffer(audio, 0), "Couldn't make an audio frame");
					for (int ch = 0; ch < 2; ch++) {
						float *dst = reinterpret_cast<float *>(audio->data[ch]);
						for (int i = 0; i < size; i++) {
							float sum = 0;
							if (i < count) {
								if (o.separate)
									sum = tracks[a]->take(s_next, size, ch, i);
								else
									for (Track *t : tracks)
										sum += t->take(s_next, size, ch, i);
							}
							dst[i] = std::max(-1.0f, std::min(1.0f, sum));
						}
					}
					audio->pts = s_next - s_first;
					write(aencs[a], audio, aouts[a]);
				}
				s_next = want;
				for (Track *t : tracks)
					t->drop_before(s_next);
			}
		};

		Progress progress{to - o.from};
		double next_keep = -1e9;
		int64_t last_pts = INT64_MIN;
		auto take_frame = [&](AVFrame *f) {
			int64_t pts = f->best_effort_timestamp;
			if (pts == AV_NOPTS_VALUE)
				return;
			double t = pts * av_q2d(tb) - video_start;
			if (t < o.from - 0.0005 || t >= to)
				return;
			// Fewer frames a second: one at most every 1/max_fps (with a little room for timestamps' jitter)
			if (fewer) {
				if (t < next_keep - 0.1 / o.max_fps)
					return;
				next_keep = std::max(next_keep, t) + 1 / o.max_fps;
			}
			sws = sws_getCachedContext(sws, f->width, f->height, (AVPixelFormat)f->format, out_w, out_h, venc->pix_fmt, SWS_BICUBIC, nullptr,
						   nullptr, nullptr);
			if (!sws)
				throw Failure{"Couldn't set up the picture conversion"};
			av_frame_unref(scaled);
			scaled->format = venc->pix_fmt;
			scaled->width = out_w;
			scaled->height = out_h;
			check(av_frame_get_buffer(scaled, 0), "Couldn't make a frame");
			sws_scale(sws, f->data, f->linesize, 0, f->height, scaled->data, scaled->linesize);
			int64_t out_pts = llround((t - o.from) / av_q2d(tb));
			if (out_pts <= last_pts)
				out_pts = last_pts + 1;
			last_pts = out_pts;
			scaled->pts = out_pts;
			write(venc, scaled, vout);
			progress.at(t - o.from);
			pump_audio((int64_t)llround((t + video_start) * RATE), false);
		};
		auto drain_video = [&]() {
			while (avcodec_receive_frame(dec, frame) >= 0) {
				take_frame(frame);
				av_frame_unref(frame);
			}
		};
		auto drain_audio = [&](Track *t) {
			while (avcodec_receive_frame(t->dec, frame) >= 0) {
				t->push(frame, in.ctx->streams[t->stream]->time_base);
				av_frame_unref(frame);
			}
		};

		while (av_read_frame(in.ctx, pkt) >= 0) {
			AVStream *src = in.ctx->streams[pkt->stream_index];
			int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
			bool past = ts != AV_NOPTS_VALUE && ts * av_q2d(src->time_base) - video_start > to + 2;
			if (pkt->stream_index == in.video) {
				if (past) {
					av_packet_unref(pkt);
					break;
				}
				// A broken packet is skipped rather than failing the whole copy
				if (avcodec_send_packet(dec, pkt) >= 0)
					drain_video();
			} else if (!past) {
				for (Track *t : tracks)
					if (t->stream == pkt->stream_index && avcodec_send_packet(t->dec, pkt) >= 0)
						drain_audio(t);
			}
			av_packet_unref(pkt);
		}
		avcodec_send_packet(dec, nullptr);
		drain_video();
		write(venc, nullptr, vout);
		for (Track *t : tracks) {
			avcodec_send_packet(t->dec, nullptr);
			drain_audio(t);
			t->ended = true;
		}
		// The audio ends with the last frame kept
		if (last_pts != INT64_MIN)
			s_end = std::min(s_end, (int64_t)llround((o.from + last_pts * av_q2d(tb) + 1 / av_q2d(fps) + video_start) * RATE));
		pump_audio(s_end, true);
		for (size_t a = 0; a < aencs.size(); a++)
			write(aencs[a], nullptr, aouts[a]);
		if (last_pts == INT64_MIN)
			throw Failure{"There's no video in that part"};
		finish(out);
	} catch (...) {
		finish(out);
		DeleteFileW(wide(out_path).c_str());
		for (Track *t : tracks)
			delete t;
		for (AVCodecContext *&a : aencs)
			avcodec_free_context(&a);
		avcodec_free_context(&venc);
		avcodec_free_context(&dec);
		sws_freeContext(sws);
		av_frame_free(&frame);
		av_frame_free(&scaled);
		av_frame_free(&audio);
		av_packet_free(&pkt);
		av_packet_free(&got);
		throw;
	}
	for (Track *t : tracks)
		delete t;
	for (AVCodecContext *&a : aencs)
		avcodec_free_context(&a);
	avcodec_free_context(&venc);
	avcodec_free_context(&dec);
	sws_freeContext(sws);
	av_frame_free(&frame);
	av_frame_free(&scaled);
	av_frame_free(&audio);
	av_packet_free(&pkt);
	av_packet_free(&got);
}

// ---------- thumbnail: one frame as a JPEG ----------

static void run_thumbnail(const std::string &in_path, const std::string &out_path, double at, int width)
{
	Input in(in_path);
	if (in.video < 0)
		throw Failure{"The file has no video"};
	AVStream *vs = in.ctx->streams[in.video];
	for (unsigned i = 0; i < in.ctx->nb_streams; i++)
		if ((int)i != in.video)
			in.ctx->streams[i]->discard = AVDISCARD_ALL;

	// FFmpeg's own AV1 decoder only works with hardware help; libaom decodes AV1 in software anywhere
	const AVCodec *codec = vs->codecpar->codec_id == AV_CODEC_ID_AV1 ? avcodec_find_decoder_by_name("libaom-av1") : nullptr;
	if (!codec)
		codec = avcodec_find_decoder(vs->codecpar->codec_id);
	if (!codec)
		throw Failure{"No decoder for this video"};
	AVCodecContext *dec = avcodec_alloc_context3(codec);
	AVCodecContext *enc = nullptr;
	AVFrame *frame = av_frame_alloc(), *picked = av_frame_alloc(), *scaled = av_frame_alloc();
	AVPacket *pkt = av_packet_alloc();
	SwsContext *sws = nullptr;
	FILE *file = nullptr;
	try {
		check(avcodec_parameters_to_context(dec, vs->codecpar), "Couldn't read the video");
		dec->pkt_timebase = vs->time_base;
		check(avcodec_open2(dec, codec, nullptr), "Couldn't open the video decoder");

		int64_t start = vs->start_time == AV_NOPTS_VALUE ? 0 : vs->start_time;
		int64_t target = start + (int64_t)(at / av_q2d(vs->time_base));
		av_seek_frame(in.ctx, in.video, target, AVSEEK_FLAG_BACKWARD);
		bool have = false, done = false;
		auto take = [&]() {
			while (!done && avcodec_receive_frame(dec, frame) >= 0) {
				// Keep the latest frame up to the target; stop at the first one past it
				av_frame_unref(picked);
				av_frame_move_ref(picked, frame);
				have = true;
				if (picked->best_effort_timestamp != AV_NOPTS_VALUE && picked->best_effort_timestamp >= target)
					done = true;
			}
		};
		while (!done && av_read_frame(in.ctx, pkt) >= 0) {
			if (pkt->stream_index == in.video && avcodec_send_packet(dec, pkt) >= 0)
				take();
			av_packet_unref(pkt);
		}
		if (!done) {
			avcodec_send_packet(dec, nullptr);
			take();
		}
		if (!have)
			throw Failure{"No frame could be decoded"};

		int height = (int)(width * (double)picked->height / picked->width / 2 + 0.5) * 2;
		sws = sws_getContext(picked->width, picked->height, (AVPixelFormat)picked->format, width, height, AV_PIX_FMT_YUVJ420P, SWS_BICUBIC,
				     nullptr, nullptr, nullptr);
		if (!sws)
			throw Failure{"Couldn't scale the frame"};
		scaled->format = AV_PIX_FMT_YUVJ420P;
		scaled->width = width;
		scaled->height = height;
		check(av_frame_get_buffer(scaled, 0), "Couldn't make the thumbnail");
		sws_scale(sws, picked->data, picked->linesize, 0, picked->height, scaled->data, scaled->linesize);

		const AVCodec *jpeg = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
		if (!jpeg)
			throw Failure{"No JPEG encoder"};
		enc = avcodec_alloc_context3(jpeg);
		enc->width = width;
		enc->height = height;
		enc->pix_fmt = AV_PIX_FMT_YUVJ420P;
		enc->time_base = AVRational{1, 25};
		enc->flags |= AV_CODEC_FLAG_QSCALE;
		enc->global_quality = FF_QP2LAMBDA * 3;
		check(avcodec_open2(enc, jpeg, nullptr), "Couldn't start the JPEG encoder");
		scaled->pts = 0;
		scaled->quality = enc->global_quality;
		check(avcodec_send_frame(enc, scaled), "Couldn't encode the thumbnail");
		avcodec_send_frame(enc, nullptr);
		check(avcodec_receive_packet(enc, pkt), "Couldn't encode the thumbnail");
		file = _wfopen(wide(out_path).c_str(), L"wb");
		if (!file || fwrite(pkt->data, 1, pkt->size, file) != (size_t)pkt->size)
			throw Failure{"Couldn't write the thumbnail"};
	} catch (...) {
		if (file)
			fclose(file);
		sws_freeContext(sws);
		avcodec_free_context(&dec);
		avcodec_free_context(&enc);
		av_frame_free(&frame);
		av_frame_free(&picked);
		av_frame_free(&scaled);
		av_packet_free(&pkt);
		throw;
	}
	fclose(file);
	sws_freeContext(sws);
	avcodec_free_context(&dec);
	avcodec_free_context(&enc);
	av_frame_free(&frame);
	av_frame_free(&picked);
	av_frame_free(&scaled);
	av_packet_free(&pkt);
}

// The pixels of one area of the frame at each of the given times, as RGB (3 bytes a pixel, rows top to
// bottom), one after another in <out> in the order asked: for reading the game's HUD after a game
// (ability letters). Each time is found like a thumbnail's: the latest frame at or before it.
// The times are read in order in one pass forward. Seeking to each one decoded from the keyframe
// before it every time (up to 2 s of video), even when the last time was a second earlier in the
// same stretch; now it only seeks when a keyframe lies between the last frame and the next time,
// so no frame is decoded twice. The frames picked are the same ones.
//
// Decoded on the graphics card where it can be (D3D11VA: Windows' video decoding on any card, NVIDIA's,
// AMD's or Intel's). On the processor, every frame up to each time is decoded on all its cores: about 30-50 s
// of processor time a game at 1080p, 33 Mbit/s (measured 2026-11-06 on the owner's games). Only the frames
// actually used are copied back from the card, and turned into the processor decoder's own layout before
// they're made RGB, so the pixels come out the same either way (H.264, HEVC and AV1 decoding is exact).
// Anything about the card that fails (no such decoder, a driver error mid-way) reads it all again on the
// processor, as before.

/** Something about decoding on the graphics card failed: read again on the processor */
struct GpuFailure {
	std::string message;
};

/** The graphics card's frames, if the decoder offers them (the get_format callback) */
static AVPixelFormat pick_d3d11(AVCodecContext *, const AVPixelFormat *offered)
{
	for (const AVPixelFormat *f = offered; *f != AV_PIX_FMT_NONE; f++)
		if (*f == AV_PIX_FMT_D3D11)
			return *f;
	return AV_PIX_FMT_NONE; // fails the decoding: read on the processor instead
}

/** A part of the picture to cut out of each frame */
struct Area {
	int x, y, w, h;
};

/**
 * The rows of a frame the areas are in, from `top` to `bottom`, started on a row where the colour planes start
 * a row too (even rows in 4:2:0), so only those rows are made RGB and come out exactly as from the whole frame.
 * `shift`: how much smaller each plane's rows are (the colour planes of 4:2:0 have half as many).
 */
struct Band {
	int top = 0, bottom = 0;
	int shift[4] = {};
};

static Band band_of(const AVFrame *frame, const std::vector<Area> &cuts)
{
	Band band;
	band.bottom = frame->height;
	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
	// A layout whose rows can't be told apart this way: the whole frame
	if (!desc || (desc->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM)))
		return band;
	const int step = 1 << desc->log2_chroma_h;
	int top = frame->height, bottom = 0;
	for (const Area &c : cuts) {
		top = std::min(top, c.y);
		bottom = std::max(bottom, c.y + c.h);
	}
	band.top = top - top % step;
	band.bottom = std::min(frame->height, (bottom + step - 1) / step * step);
	band.shift[1] = band.shift[2] = desc->log2_chroma_h;
	return band;
}

/**
 * A frame copied back from the card as the processor's decoder would have given it: NV12 (one plane of
 * U and V side by side) as YUV420P, P010 (10-bit at the top of 16) as YUV420P10. Other layouts as they are.
 * Only rows `top` to `bottom` (`top` even) are filled in: the rest are never read.
 */
static void like_processor(const AVFrame *nv, AVFrame *out, int top, int bottom)
{
	bool ten = nv->format == AV_PIX_FMT_P010LE;
	const int format = ten ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
	// The same frame's buffer for every frame of the same size
	if (!out->data[0] || out->format != format || out->width != nv->width || out->height != nv->height) {
		av_frame_unref(out);
		out->format = format;
		out->width = nv->width;
		out->height = nv->height;
		check(av_frame_get_buffer(out, 0), "Couldn't convert the frame");
	}
	const int cw = (nv->width + 1) / 2, ch = std::min((nv->height + 1) / 2, (bottom + 1) / 2);
	for (int r = top; r < bottom; r++) {
		if (ten) {
			const uint16_t *from = (const uint16_t *)(nv->data[0] + (size_t)r * nv->linesize[0]);
			uint16_t *to = (uint16_t *)(out->data[0] + (size_t)r * out->linesize[0]);
			for (int c = 0; c < nv->width; c++)
				to[c] = from[c] >> 6;
		} else {
			memcpy(out->data[0] + (size_t)r * out->linesize[0], nv->data[0] + (size_t)r * nv->linesize[0], nv->width);
		}
	}
	for (int r = top / 2; r < ch; r++) {
		if (ten) {
			const uint16_t *uv = (const uint16_t *)(nv->data[1] + (size_t)r * nv->linesize[1]);
			uint16_t *u = (uint16_t *)(out->data[1] + (size_t)r * out->linesize[1]);
			uint16_t *v = (uint16_t *)(out->data[2] + (size_t)r * out->linesize[2]);
			for (int c = 0; c < cw; c++) {
				u[c] = uv[2 * c] >> 6;
				v[c] = uv[2 * c + 1] >> 6;
			}
		} else {
			const uint8_t *uv = nv->data[1] + (size_t)r * nv->linesize[1];
			uint8_t *u = out->data[1] + (size_t)r * out->linesize[1];
			uint8_t *v = out->data[2] + (size_t)r * out->linesize[2];
			for (int c = 0; c < cw; c++) {
				u[c] = uv[2 * c];
				v[c] = uv[2 * c + 1];
			}
		}
	}
}

/**
 * The areas at each time, in the order asked (`gpu`: decoded on the graphics card, or a GpuFailure). Several
 * areas come from the same decoded frames: decoding is nearly all the work, so reading your abilities and your
 * teammates' frames together costs about what one of them did. They're written one area after another (all of
 * the first area's times, then the second's), so one area comes out exactly as it did before.
 */
static std::vector<uint8_t> read_areas(const std::string &in_path, const std::vector<Area> &cuts, const std::vector<double> &times, bool keyframes,
				       bool gpu)
{
	Input in(in_path);
	if (in.video < 0)
		throw Failure{"The file has no video"};
	AVStream *vs = in.ctx->streams[in.video];
	for (unsigned i = 0; i < in.ctx->nb_streams; i++)
		if ((int)i != in.video)
			in.ctx->streams[i]->discard = AVDISCARD_ALL;

	// AV1 on the processor through libaom; on the card through FFmpeg's own AV1 decoder, which only decodes
	// with the card's help
	const bool av1 = vs->codecpar->codec_id == AV_CODEC_ID_AV1;
	const AVCodec *codec = av1 ? avcodec_find_decoder_by_name(gpu ? "av1" : "libaom-av1") : nullptr;
	if (!codec)
		codec = avcodec_find_decoder(vs->codecpar->codec_id);
	if (!codec)
		throw Failure{"No decoder for this video"};
	if (gpu) {
		bool offered = false;
		for (int i = 0;; i++) {
			const AVCodecHWConfig *c = avcodec_get_hw_config(codec, i);
			if (!c)
				break;
			if ((c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && c->device_type == AV_HWDEVICE_TYPE_D3D11VA)
				offered = true;
		}
		if (!offered)
			throw GpuFailure{std::string("the ") + codec->name + " decoder can't use the graphics card"};
	}
	AVCodecContext *dec = avcodec_alloc_context3(codec);
	AVFrame *frame = av_frame_alloc(), *picked = av_frame_alloc(), *ahead = av_frame_alloc(), *rgb = av_frame_alloc();
	AVFrame *copied = av_frame_alloc(), *planar = av_frame_alloc();
	AVPacket *pkt = av_packet_alloc();
	AVBufferRef *device = nullptr;
	SwsContext *sws = nullptr;
	auto cleanup = [&]() {
		sws_freeContext(sws);
		avcodec_free_context(&dec);
		av_buffer_unref(&device);
		av_frame_free(&frame);
		av_frame_free(&picked);
		av_frame_free(&ahead);
		av_frame_free(&rgb);
		av_frame_free(&copied);
		av_frame_free(&planar);
		av_packet_free(&pkt);
	};
	std::vector<uint8_t> areas;
	try {
		check(avcodec_parameters_to_context(dec, vs->codecpar), "Couldn't read the video");
		dec->pkt_timebase = vs->time_base;
		if (gpu) {
			int r = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
			if (r < 0)
				throw GpuFailure{"the graphics card's video decoding couldn't be started: " + ff_error(r)};
			dec->hw_device_ctx = av_buffer_ref(device);
			dec->get_format = pick_d3d11;
			// `picked` and `ahead` hold on to a frame each besides the ones the decoder keeps
			dec->extra_hw_frames = 4;
			dec->thread_count = 1;
		} else {
			dec->thread_count = 0; // as many as the CPU has
		}
		int opened = avcodec_open2(dec, codec, nullptr);
		if (opened < 0 && gpu)
			throw GpuFailure{"the decoder couldn't be opened for the graphics card: " + ff_error(opened)};
		check(opened, "Couldn't open the video decoder");
		// Where each area's block starts in the output
		std::vector<size_t> base(cuts.size());
		size_t total = 0;
		for (size_t a = 0; a < cuts.size(); a++) {
			const Area &c = cuts[a];
			if (c.x < 0 || c.y < 0 || c.w <= 0 || c.h <= 0 || c.x + c.w > dec->width || c.y + c.h > dec->height)
				throw Failure{"The area is outside the picture"};
			base[a] = total;
			total += times.size() * (size_t)c.w * 3 * c.h;
		}

		// The times in order (the HUD reader asks fight by fight, which can overlap); the areas are
		// written out in the order asked
		std::vector<size_t> order(times.size());
		for (size_t n = 0; n < order.size(); n++)
			order[n] = n;
		std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return times[a] < times[b]; });
		areas.resize(total);

		// `picked`: the newest frame decoded at or before the time being read; `ahead`: the first one
		// decoded after it, kept for the next time
		bool have = false, have_ahead = false, drained = false;
		int64_t target = 0;
		// Frames nothing else refers to (most B-frames) aren't decoded when a later frame at or before
		// the target has been sent already: they can't be the one picked. `latest`: the newest such time
		// sent; `later`: times sent that were past the target then
		int64_t latest = AV_NOPTS_VALUE;
		std::vector<int64_t> later;
		auto next = [&](AVFrame *into) {
			for (;;) {
				int r = avcodec_receive_frame(dec, into);
				if (r >= 0)
					return true;
				// On the processor a damaged stretch (the end of a recording cut short) just ends it here; on the
				// card an error may be the card's, so it's read again on the processor
				if (gpu && r != AVERROR(EAGAIN) && r != AVERROR_EOF)
					throw GpuFailure{"decoding on the graphics card failed: " + ff_error(r)};
				if (r != AVERROR(EAGAIN) || drained)
					return false;
				if (av_read_frame(in.ctx, pkt) < 0) {
					// The end: what the decoder still holds
					avcodec_send_packet(dec, nullptr);
					drained = true;
					continue;
				}
				if (pkt->stream_index == in.video) {
					bool skippable = pkt->pts != AV_NOPTS_VALUE && latest != AV_NOPTS_VALUE && pkt->pts < latest;
					dec->skip_frame = skippable ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
					int sent = avcodec_send_packet(dec, pkt);
					if (gpu && sent < 0 && sent != AVERROR(EAGAIN))
						throw GpuFailure{"decoding on the graphics card failed: " + ff_error(sent)};
					if (sent >= 0 && pkt->pts != AV_NOPTS_VALUE) {
						if (pkt->pts <= target)
							latest = latest == AV_NOPTS_VALUE ? pkt->pts : std::max(latest, pkt->pts);
						else
							later.push_back(pkt->pts);
					}
				}
				av_packet_unref(pkt);
			}
		};

		int64_t start = vs->start_time == AV_NOPTS_VALUE ? 0 : vs->start_time;
		for (size_t done_count = 0; done_count < order.size(); done_count++) {
			size_t n = order[done_count];
			target = start + (int64_t)(times[n] / av_q2d(vs->time_base));

			if (keyframes) {
				// Just the keyframe at or before the time: one picture decoded on its own, nothing before or after
				// it read (the recordings have one every 2 s, so reading a whole game this way decodes about a 120th
				// of its frames)
				avcodec_flush_buffers(dec);
				av_frame_unref(picked);
				have = false;
				if (av_seek_frame(in.ctx, in.video, target, AVSEEK_FLAG_BACKWARD) >= 0) {
					while (av_read_frame(in.ctx, pkt) >= 0) {
						if (pkt->stream_index != in.video || !(pkt->flags & AV_PKT_FLAG_KEY)) {
							av_packet_unref(pkt);
							continue;
						}
						dec->skip_frame = AVDISCARD_DEFAULT;
						int sent = avcodec_send_packet(dec, pkt);
						av_packet_unref(pkt);
						if (gpu && sent < 0)
							throw GpuFailure{"decoding on the graphics card failed: " + ff_error(sent)};
						// Out at once, not after the frames that would follow it
						avcodec_send_packet(dec, nullptr);
						int got = avcodec_receive_frame(dec, picked);
						if (gpu && got < 0 && got != AVERROR_EOF)
							throw GpuFailure{"decoding on the graphics card failed: " + ff_error(got)};
						have = got >= 0;
						break;
					}
				}
				if (have && picked->best_effort_timestamp != AV_NOPTS_VALUE)
					say("keyframe " + std::to_string(n) + " " + std::to_string((picked->best_effort_timestamp - start) * av_q2d(vs->time_base)));
			} else {

				// Jump only if a keyframe lies after the frame we're at, on the way to the target: decoding
				// from there is shorter. Without a keyframe list, jump for gaps over 2 s
				bool seek = !have || picked->best_effort_timestamp == AV_NOPTS_VALUE;
				if (!seek) {
					int64_t at = picked->best_effort_timestamp;
					const AVIndexEntry *key = avformat_index_get_entry_from_timestamp(vs, target, AVSEEK_FLAG_BACKWARD);
					seek = key ? key->timestamp > at : (target - at) * av_q2d(vs->time_base) > 2;
				}
				if (seek) {
					avcodec_flush_buffers(dec);
					av_seek_frame(in.ctx, in.video, target, AVSEEK_FLAG_BACKWARD);
					av_frame_unref(picked);
					av_frame_unref(ahead);
					have = have_ahead = drained = false;
					latest = AV_NOPTS_VALUE;
					later.clear();
				}
				// Times sent earlier that this target has reached
				for (size_t i = 0; i < later.size();) {
					if (later[i] <= target) {
						latest = latest == AV_NOPTS_VALUE ? later[i] : std::max(latest, later[i]);
						later.erase(later.begin() + i);
					} else {
						i++;
					}
				}

				// Forward to the target: the first frame after a jump is taken whatever its time; after
				// that, frames up to the target, stopping at one past it
				bool done = have && picked->best_effort_timestamp != AV_NOPTS_VALUE && picked->best_effort_timestamp >= target;
				while (!done) {
					if (have_ahead) {
						av_frame_unref(frame);
						av_frame_move_ref(frame, ahead);
						have_ahead = false;
					} else if (!next(frame)) {
						break; // past the end: keep the last frame decoded
					}
					if (have && frame->best_effort_timestamp != AV_NOPTS_VALUE && frame->best_effort_timestamp > target) {
						av_frame_move_ref(ahead, frame);
						have_ahead = true;
						break;
					}
					av_frame_unref(picked);
					av_frame_move_ref(picked, frame);
					have = true;
					done = picked->best_effort_timestamp != AV_NOPTS_VALUE && picked->best_effort_timestamp >= target;
				}
			}
			if (!have) {
				if (gpu)
					throw GpuFailure{"no frame came from the graphics card"};
				throw Failure{"No frame could be decoded"};
			}

			// A frame on the card: copied back, in the processor decoder's layout
			const AVFrame *source = picked;
			if (picked->format == AV_PIX_FMT_D3D11) {
				av_frame_unref(copied);
				int r = av_hwframe_transfer_data(copied, picked, 0);
				if (r < 0)
					throw GpuFailure{"a frame couldn't be copied from the graphics card: " + ff_error(r)};
				if (copied->format == AV_PIX_FMT_NV12 || copied->format == AV_PIX_FMT_P010LE) {
					const Band band = band_of(copied, cuts);
					like_processor(copied, planar, band.top, band.bottom);
					source = planar;
				} else {
					source = copied;
				}
			}

			// Only the rows the areas are in to RGB (the HUD's strips are a tenth of the picture), then the areas
			// out of them
			const Band band = band_of(source, cuts);
			const uint8_t *from[4] = {};
			for (int p = 0; p < 4 && source->data[p]; p++)
				from[p] = source->data[p] + (size_t)(band.top >> band.shift[p]) * source->linesize[p];
			const int rows = band.bottom - band.top;
			sws = sws_getCachedContext(sws, source->width, rows, (AVPixelFormat)source->format, source->width, rows, AV_PIX_FMT_RGB24, SWS_POINT,
						   nullptr, nullptr, nullptr);
			if (!sws)
				throw Failure{"Couldn't convert the frame"};
			if (!rgb->data[0] || rgb->width != source->width || rgb->height != rows) {
				av_frame_unref(rgb);
				rgb->format = AV_PIX_FMT_RGB24;
				rgb->width = source->width;
				rgb->height = rows;
				check(av_frame_get_buffer(rgb, 0), "Couldn't convert the frame");
			}
			sws_scale(sws, from, source->linesize, 0, rows, rgb->data, rgb->linesize);
			for (size_t a = 0; a < cuts.size(); a++) {
				const Area &c = cuts[a];
				const size_t row = (size_t)c.w * 3;
				for (int r = 0; r < c.h; r++)
					memcpy(&areas[base[a] + (n * c.h + r) * row], rgb->data[0] + (size_t)(c.y - band.top + r) * rgb->linesize[0] + (size_t)c.x * 3,
					       row);
			}
			say("progress " + std::to_string((double)(done_count + 1) / times.size()));
		}
	} catch (...) {
		cleanup();
		throw;
	}
	cleanup();
	return areas;
}

static void run_pixels(const std::string &in_path, const std::string &out_path, const std::vector<Area> &cuts, const std::vector<double> &times,
		       bool keyframes)
{
	FILE *file = _wfopen(wide(out_path).c_str(), L"wb");
	if (!file)
		throw Failure{"Couldn't write the pixels"};
	std::vector<uint8_t> areas;
	try {
		const char *cpu = getenv("LR_MEDIA_CPU");
		bool gpu = !(cpu && *cpu && strcmp(cpu, "0") != 0);
		if (gpu) {
			try {
				areas = read_areas(in_path, cuts, times, keyframes, true);
				say("decoder graphics card");
			} catch (const GpuFailure &f) {
				say("decoder processor: " + f.message);
				gpu = false;
			}
		} else {
			say("decoder processor: LR_MEDIA_CPU is set");
		}
		if (!gpu)
			areas = read_areas(in_path, cuts, times, keyframes, false);
		if (!areas.empty() && fwrite(areas.data(), 1, areas.size(), file) != areas.size())
			throw Failure{"Couldn't write the pixels"};
	} catch (...) {
		fclose(file);
		throw;
	}
	fclose(file);
}

int wmain()
{
	av_log_set_level(AV_LOG_ERROR);
	int argc = 0;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	std::vector<std::string> args;
	for (int i = 1; i < argc; i++)
		args.push_back(utf8(argv[i]));
	LocalFree(argv);
	try {
		if (args.size() == 4 && args[0] == "tracks") {
			run_tracks(args[1], args[2], args[3]);
		} else if (args.size() == 5 && args[0] == "thumbnail") {
			run_thumbnail(args[1], args[2], atof(args[3].c_str()), std::max(16, atoi(args[4].c_str())));
		} else if ((args.size() == 5 || (args.size() == 6 && args[5] == "keyframes")) && args[0] == "pixels") {
			auto numbers = [](const std::string &list) {
				std::vector<double> out;
				size_t from = 0;
				while (from <= list.size()) {
					size_t comma = list.find(',', from);
					std::string part = list.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
					if (!part.empty())
						out.push_back(atof(part.c_str()));
					if (comma == std::string::npos)
						break;
					from = comma + 1;
				}
				return out;
			};
			// One area "x,y,w,h", or several "x,y,w,h;x,y,w,h"
			std::vector<Area> cuts;
			size_t from = 0;
			while (from <= args[3].size()) {
				size_t semi = args[3].find(';', from);
				std::string part = args[3].substr(from, semi == std::string::npos ? std::string::npos : semi - from);
				if (!part.empty()) {
					std::vector<double> area = numbers(part);
					if (area.size() != 4)
						throw Failure{"Each area must be x,y,w,h"};
					cuts.push_back({(int)area[0], (int)area[1], (int)area[2], (int)area[3]});
				}
				if (semi == std::string::npos)
					break;
				from = semi + 1;
			}
			if (cuts.empty())
				throw Failure{"The area must be x,y,w,h"};
			run_pixels(args[1], args[2], cuts, numbers(args[4]), args.size() == 6);
		} else if (args.size() == 4 && args[0] == "export") {
			std::vector<int> chosen;
			size_t from = 0;
			while (from <= args[3].size()) {
				size_t comma = args[3].find(',', from);
				std::string part = args[3].substr(from, comma == std::string::npos ? std::string::npos : comma - from);
				if (!part.empty())
					chosen.push_back(atoi(part.c_str()));
				if (comma == std::string::npos)
					break;
				from = comma + 1;
			}
			run_export(args[1], args[2], chosen);
		} else if (args.size() >= 4 && args[0] == "encode") {
			EncodeOptions o;
			for (size_t i = 3; i < args.size(); i++) {
				size_t eq = args[i].find('=');
				std::string key = args[i].substr(0, eq), value = eq == std::string::npos ? "" : args[i].substr(eq + 1);
				if (key == "from")
					o.from = std::max(0.0, atof(value.c_str()));
				else if (key == "to")
					o.to = atof(value.c_str());
				else if (key == "height")
					o.height = atoi(value.c_str());
				else if (key == "fps")
					o.max_fps = atof(value.c_str());
				else if (key == "kbps")
					o.video_kbps = atoi(value.c_str());
				else if (key == "akbps")
					o.audio_kbps = std::max(32, atoi(value.c_str()));
				else if (key == "separate")
					o.separate = value == "1";
				else if (key == "fragmented")
					o.fragmented = value == "1";
				else if (key == "tracks") {
					size_t from = 0;
					while (from < value.size()) {
						size_t comma = value.find(',', from);
						std::string part = value.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
						if (!part.empty())
							o.tracks.push_back(atoi(part.c_str()));
						if (comma == std::string::npos)
							break;
						from = comma + 1;
					}
				} else
					throw Failure{"Unknown encode setting: " + args[i]};
			}
			if (o.video_kbps < 50)
				throw Failure{"encode needs kbps= (the video's bitrate)"};
			run_encode(args[1], args[2], o);
		} else {
			say("error usage: tracks <in> <folder> <prefix> | export <in> <out> <n,n,...> | thumbnail <in> <out.jpg> <seconds> <width> | pixels <in> <out.rgb> <x,y,w,h> <seconds,...>");
			return 1;
		}
	} catch (const Failure &f) {
		say("error " + f.message);
		return 1;
	}
	say("done");
	return 0;
}
