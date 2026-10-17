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
//   league-recorder-media.exe pixels <in.mp4> <out.rgb> <x,y,w,h> <seconds,seconds,...>
//       One area of the frame at each time, as raw RGB one after another: for reading the game's HUD
//       (which abilities were ready) after a game.
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
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
// bottom), one after another in <out>: for reading the game's HUD after a game (ability letters).
// Each time is found like a thumbnail's: the latest frame at or before it.
static void run_pixels(const std::string &in_path, const std::string &out_path, int x, int y, int w, int h, const std::vector<double> &times)
{
	Input in(in_path);
	if (in.video < 0)
		throw Failure{"The file has no video"};
	AVStream *vs = in.ctx->streams[in.video];
	for (unsigned i = 0; i < in.ctx->nb_streams; i++)
		if ((int)i != in.video)
			in.ctx->streams[i]->discard = AVDISCARD_ALL;

	const AVCodec *codec = vs->codecpar->codec_id == AV_CODEC_ID_AV1 ? avcodec_find_decoder_by_name("libaom-av1") : nullptr;
	if (!codec)
		codec = avcodec_find_decoder(vs->codecpar->codec_id);
	if (!codec)
		throw Failure{"No decoder for this video"};
	AVCodecContext *dec = avcodec_alloc_context3(codec);
	AVFrame *frame = av_frame_alloc(), *picked = av_frame_alloc(), *rgb = av_frame_alloc();
	AVPacket *pkt = av_packet_alloc();
	SwsContext *sws = nullptr;
	FILE *file = nullptr;
	auto cleanup = [&]() {
		if (file)
			fclose(file);
		sws_freeContext(sws);
		avcodec_free_context(&dec);
		av_frame_free(&frame);
		av_frame_free(&picked);
		av_frame_free(&rgb);
		av_packet_free(&pkt);
	};
	try {
		check(avcodec_parameters_to_context(dec, vs->codecpar), "Couldn't read the video");
		dec->pkt_timebase = vs->time_base;
		dec->thread_count = 0; // as many as the CPU has: each seek decodes up to a keyframe interval
		check(avcodec_open2(dec, codec, nullptr), "Couldn't open the video decoder");
		if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > dec->width || y + h > dec->height)
			throw Failure{"The area is outside the picture"};
		file = _wfopen(wide(out_path).c_str(), L"wb");
		if (!file)
			throw Failure{"Couldn't write the pixels"};
		std::vector<uint8_t> row((size_t)w * 3);

		int64_t start = vs->start_time == AV_NOPTS_VALUE ? 0 : vs->start_time;
		for (size_t n = 0; n < times.size(); n++) {
			int64_t target = start + (int64_t)(times[n] / av_q2d(vs->time_base));
			avcodec_flush_buffers(dec);
			av_seek_frame(in.ctx, in.video, target, AVSEEK_FLAG_BACKWARD);
			bool have = false, done = false;
			auto take = [&]() {
				while (!done && avcodec_receive_frame(dec, frame) >= 0) {
					// The latest frame up to the target, as for thumbnails
					if (have && frame->best_effort_timestamp != AV_NOPTS_VALUE && frame->best_effort_timestamp > target) {
						done = true;
						av_frame_unref(frame);
						break;
					}
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

			// The whole frame to RGB, then just the area's rows
			sws = sws_getCachedContext(sws, picked->width, picked->height, (AVPixelFormat)picked->format, picked->width, picked->height,
						   AV_PIX_FMT_RGB24, SWS_POINT, nullptr, nullptr, nullptr);
			if (!sws)
				throw Failure{"Couldn't convert the frame"};
			if (!rgb->data[0] || rgb->width != picked->width || rgb->height != picked->height) {
				av_frame_unref(rgb);
				rgb->format = AV_PIX_FMT_RGB24;
				rgb->width = picked->width;
				rgb->height = picked->height;
				check(av_frame_get_buffer(rgb, 0), "Couldn't convert the frame");
			}
			sws_scale(sws, picked->data, picked->linesize, 0, picked->height, rgb->data, rgb->linesize);
			for (int r = 0; r < h; r++)
				if (fwrite(rgb->data[0] + (size_t)(y + r) * rgb->linesize[0] + (size_t)x * 3, 1, row.size(), file) != row.size())
					throw Failure{"Couldn't write the pixels"};
			say("progress " + std::to_string((double)(n + 1) / times.size()));
		}
	} catch (...) {
		cleanup();
		throw;
	}
	cleanup();
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
		} else if (args.size() == 5 && args[0] == "pixels") {
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
			std::vector<double> area = numbers(args[3]);
			if (area.size() != 4)
				throw Failure{"The area must be x,y,w,h"};
			run_pixels(args[1], args[2], (int)area[0], (int)area[1], (int)area[2], (int)area[3], numbers(args[4]));
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
