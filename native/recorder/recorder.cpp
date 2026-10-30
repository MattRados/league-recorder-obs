// League Recorder's recorder: a small program built directly on OBS's core library (libobs), with
// none of OBS's own interface. It records the League game with OBS's Game Capture, the chosen apps'
// and mics' audio, and a hardware encoder, into a crash-safe (fragmented) MP4.
//
// League Recorder's core drives it over a Windows named pipe that rejects remote clients (no
// network port). Tab-separated lines, newline-terminated, UTF-8:
//   client -> recorder:  <id>\tsetup\t<json>                   (re)build scene, encoders, output; not while recording
//                        <id>\tstatus                          -> res\t<id>\tok\t<0|1 recording>\t<0|1 game hooked>\t<0|1 replay buffer>
//                        <id>\tstart\t<folder>\t<file name>    -> res\t<id>\tok   (then evt\tstarted)
//                        <id>\tstop                            -> res\t<id>\tok   (then evt\tstopped\t<file>)
//                        <id>\tbuffer-start\t<folder>\t<seconds>\t<max MB>  keep the last <seconds> in memory (then evt\tbuffer-started)
//                        <id>\tbuffer-stop                     (then evt\tbuffer-stopped)
//                        <id>\tclip                            save the buffer to <folder> (then evt\tclip\t<file>)
//                        <id>\tvolume\t<source id>\t<level>    set an audio source's volume now (1 = as it plays), even while recording
//                        <id>\tmeters\t<on|off>               report audio levels (evt\tlevels) ten times a second
//                        <id>\tquit                            close cleanly
//   recorder -> client:  evt\tstarted | evt\tstopped\t<file> (also if recording stops on its own)
//                        evt\tbuffer-started | evt\tbuffer-stopped | evt\tclip\t<file>
//                        evt\tlevels\t<source id>=<peak dB>\t...  (the loudest moment since the last report)
//                        evt\thooked | evt\tunhooked             (Game Capture got into League, or lost it)
//                        res\t<id>\terror\t<message>
//
// The replay buffer shares the recording's encoders: keeping the last seconds costs memory, not encoding.
// Besides OBS's sources, setups can use "lr_app_audio" ({ exe }): an app's sound found by program name (below).
// Audio inputs carry the core's source "id" (for volume and levels) and a "volume" (1 = as it plays).
//
// Command line: league-recorder-obs.exe --pipe <name> --config <folder for logs and plugin settings>
// It lives in OBS's bin\64bit folder so libobs finds its data and plugins at ../../data and ../../obs-plugins.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <psapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <share.h>
#include <tlhelp32.h>

#include <obs.h>
#include <util/base.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------- logging (added to the day file --log names, or <config>\recorder.log) ----------

static FILE *log_file = nullptr;
static std::mutex log_lock;
/** When libobs last said the screen couldn't be captured the usual way (DXGI duplication), in os_gettime_ns; 0 = it hasn't */
static std::atomic<uint64_t> duplicator_failed_at{0};
/** How many times Game Capture couldn't open its connection to League's hook since it was last in League: only
 *  one program can have it, so another one capturing League the same way has the game (watch) */
static std::atomic<int> hook_pipe_taken{0};
/** When the app asked to stop the recording, while it hasn't stopped yet (os_gettime_ns; 0 = not stopping) */
static std::atomic<uint64_t> stop_asked_at{0};

static void log_handler(int level, const char *format, va_list args, void *)
{
	char message[8192];
	vsnprintf(message, sizeof(message), format, args);
	// libobs keeps trying every few seconds and only says so in the log: noted for the screen capture fallback (watch)
	if (strncmp(message, "device_duplicator_create: Failed", 32) == 0 && !duplicator_failed_at)
		duplicator_failed_at = os_gettime_ns();
	if (strstr(message, "init_pipe: failed to start pipe"))
		hook_pipe_taken++;
	SYSTEMTIME t;
	GetLocalTime(&t);
	std::lock_guard<std::mutex> lock(log_lock);
	if (!log_file)
		return;
	const char *tag = level <= LOG_ERROR ? "error: " : level <= LOG_WARNING ? "warning: " : "";
	fprintf(log_file, "%02d:%02d:%02d.%03d: %s%s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, tag, message);
	fflush(log_file);
}

// ---------- tasks libobs wants run on the "UI" (main) thread ----------

static DWORD main_thread = 0;
static const UINT WM_RUN_TASK = WM_APP + 1;

struct UiTask {
	obs_task_t task;
	void *param;
	HANDLE done; // set when finished, if the caller waits
};

static void ui_task_handler(obs_task_t task, void *param, bool wait)
{
	if (GetCurrentThreadId() == main_thread) {
		task(param);
		return;
	}
	auto *t = new UiTask{task, param, wait ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr};
	HANDLE done = t->done;
	PostThreadMessageW(main_thread, WM_RUN_TASK, 0, reinterpret_cast<LPARAM>(t));
	if (done) {
		WaitForSingleObject(done, INFINITE);
		CloseHandle(done);
	}
}

// ---------- the pipe ----------

static std::mutex write_lock;
static HANDLE client = INVALID_HANDLE_VALUE;
static std::atomic<bool> exiting{false};

/** Send one line to the connected client, if any (from any thread). */
static void send_line(const std::string &line)
{
	std::lock_guard<std::mutex> lock(write_lock);
	if (client == INVALID_HANDLE_VALUE)
		return;
	std::string data = line + "\n";
	OVERLAPPED ov{};
	ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	DWORD written = 0;
	if (WriteFile(client, data.data(), (DWORD)data.size(), nullptr, &ov) || GetLastError() == ERROR_IO_PENDING)
		GetOverlappedResult(client, &ov, &written, TRUE);
	CloseHandle(ov.hEvent);
}

static std::vector<std::string> split_tabs(const std::string &s, size_t max_parts = SIZE_MAX)
{
	std::vector<std::string> parts;
	size_t start = 0;
	for (;;) {
		size_t tab = parts.size() + 1 >= max_parts ? std::string::npos : s.find('\t', start);
		parts.push_back(s.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
		if (tab == std::string::npos)
			return parts;
		start = tab + 1;
	}
}

// ---------- app audio by program name (source "lr_app_audio") ----------
//
// OBS's Application Audio Capture finds an app through a visible window, so it can't record apps
// that have none: Riot's voice chat (RiotClientServices.exe) never shows one, and Discord hides its
// window when minimized to the tray. This source finds the program by its exe name instead and
// records it, with all its child processes, through Windows' process loopback (Windows 10 2004+).
// It keeps looking while the app isn't running and picks it up again if it restarts.

/** Receives the audio client once Windows has activated it */
class ActivateHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
	LONG refs = 1;

public:
	HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	IAudioClient *client = nullptr;

	~ActivateHandler() { CloseHandle(done); }
	STDMETHODIMP QueryInterface(REFIID riid, void **out) override
	{
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
			*out = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
		else if (riid == __uuidof(IAgileObject))
			*out = static_cast<IAgileObject *>(this);
		else
			return (*out = nullptr), E_NOINTERFACE;
		AddRef();
		return S_OK;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }
	STDMETHODIMP_(ULONG) Release() override
	{
		ULONG n = InterlockedDecrement(&refs);
		if (n == 0)
			delete this;
		return n;
	}
	STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation *op) override
	{
		HRESULT hr = E_FAIL;
		IUnknown *unknown = nullptr;
		if (SUCCEEDED(op->GetActivateResult(&hr, &unknown)) && SUCCEEDED(hr) && unknown) {
			unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(&client));
			unknown->Release();
		}
		SetEvent(done);
		return S_OK;
	}
};

/** The topmost running process with this exe name (its children are included in the capture) */
static DWORD find_root_process(const std::wstring &exe)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return 0;
	std::vector<std::pair<DWORD, DWORD>> matches; // pid, parent
	PROCESSENTRY32W e{sizeof(e)};
	for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
		if (_wcsicmp(e.szExeFile, exe.c_str()) == 0)
			matches.push_back({e.th32ProcessID, e.th32ParentProcessID});
	CloseHandle(snap);
	for (auto &m : matches) {
		bool parent_matches = false;
		for (auto &other : matches)
			parent_matches = parent_matches || other.first == m.second;
		if (!parent_matches)
			return m.first;
	}
	return matches.empty() ? 0 : matches[0].first;
}

struct AppAudio {
	obs_source_t *source = nullptr;
	std::wstring exe;
	std::mutex lock; // guards exe
	HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	std::thread worker;

	/** Record one run of the app until it exits, the target changes, or we're stopping */
	void capture(DWORD pid, HANDLE process)
	{
		AUDIOCLIENT_ACTIVATION_PARAMS params = {};
		params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
		params.ProcessLoopbackParams.TargetProcessId = pid;
		params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
		PROPVARIANT activation = {};
		activation.vt = VT_BLOB;
		activation.blob.cbSize = sizeof(params);
		activation.blob.pBlobData = reinterpret_cast<BYTE *>(&params);

		auto *handler = new ActivateHandler();
		IActivateAudioInterfaceAsyncOperation *op = nullptr;
		HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activation, handler, &op);
		IAudioClient *client = nullptr;
		if (SUCCEEDED(hr) && WaitForSingleObject(handler->done, 5000) == WAIT_OBJECT_0) {
			client = handler->client;
			handler->client = nullptr;
		}
		if (op)
			op->Release();
		handler->Release();
		if (!client) {
			blog(LOG_WARNING, "[recorder] app audio: couldn't capture %ls (pid %lu): 0x%08lx", exe.c_str(), pid, hr);
			WaitForSingleObject(stop, 2000);
			return;
		}

		WAVEFORMATEX format = {};
		format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
		format.nChannels = 2;
		format.nSamplesPerSec = 48000;
		format.wBitsPerSample = 32;
		format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
		format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
		HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		IAudioCaptureClient *capture_client = nullptr;
		hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0,
					&format, nullptr);
		if (SUCCEEDED(hr))
			hr = client->SetEventHandle(ready);
		if (SUCCEEDED(hr))
			hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&capture_client));
		if (SUCCEEDED(hr))
			hr = client->Start();
		if (FAILED(hr)) {
			blog(LOG_WARNING, "[recorder] app audio: couldn't start capturing %ls: 0x%08lx", exe.c_str(), hr);
		} else {
			blog(LOG_INFO, "[recorder] app audio: capturing %ls (pid %lu)", exe.c_str(), pid);
			HANDLE waits[] = {stop, changed, process, ready};
			for (;;) {
				DWORD w = WaitForMultipleObjects(4, waits, FALSE, 1000);
				if (w == WAIT_OBJECT_0 || w == WAIT_OBJECT_0 + 1 || w == WAIT_OBJECT_0 + 2)
					break; // stopping, a different app was set, or the app exited
				UINT32 frames = 0;
				while (SUCCEEDED(capture_client->GetNextPacketSize(&frames)) && frames > 0) {
					BYTE *data = nullptr;
					DWORD flags = 0;
					if (FAILED(capture_client->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
						break;
					std::vector<float> silence;
					if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
						silence.assign(frames * 2, 0.0f);
						data = reinterpret_cast<BYTE *>(silence.data());
					}
					obs_source_audio audio = {};
					audio.data[0] = data;
					audio.frames = frames;
					audio.speakers = SPEAKERS_STEREO;
					audio.format = AUDIO_FORMAT_FLOAT;
					audio.samples_per_sec = 48000;
					audio.timestamp = os_gettime_ns() - util_mul_div64(frames, 1000000000ULL, 48000);
					obs_source_output_audio(source, &audio);
					capture_client->ReleaseBuffer(frames);
				}
			}
			client->Stop();
			blog(LOG_INFO, "[recorder] app audio: stopped capturing %ls", exe.c_str());
		}
		if (capture_client)
			capture_client->Release();
		client->Release();
		CloseHandle(ready);
	}

	void run()
	{
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
			std::wstring target;
			{
				std::lock_guard<std::mutex> l(lock);
				target = exe;
			}
			ResetEvent(changed);
			DWORD pid = target.empty() ? 0 : find_root_process(target);
			HANDLE process = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
			if (!process) {
				// Not running: look again in a couple of seconds
				HANDLE waits[] = {stop, changed};
				WaitForMultipleObjects(2, waits, FALSE, 2000);
				continue;
			}
			capture(pid, process);
			CloseHandle(process);
		}
		CoUninitialize();
	}

	void update(obs_data_t *settings)
	{
		const char *name = obs_data_get_string(settings, "exe");
		int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
		std::wstring w(n > 0 ? n - 1 : 0, L'\0');
		if (n > 1)
			MultiByteToWideChar(CP_UTF8, 0, name, -1, w.data(), n);
		{
			std::lock_guard<std::mutex> l(lock);
			if (w == exe)
				return;
			exe = w;
		}
		SetEvent(changed);
	}
};

static void register_app_audio_source()
{
	obs_source_info info = {};
	info.id = "lr_app_audio";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = [](void *) -> const char * { return "App audio by program name"; };
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		auto *a = new AppAudio();
		a->source = source;
		a->update(settings);
		a->worker = std::thread([a] { a->run(); });
		return a;
	};
	info.destroy = [](void *data) {
		auto *a = static_cast<AppAudio *>(data);
		SetEvent(a->stop);
		if (a->worker.joinable())
			a->worker.join();
		CloseHandle(a->stop);
		CloseHandle(a->changed);
		delete a;
	};
	info.update = [](void *data, obs_data_t *settings) { static_cast<AppAudio *>(data)->update(settings); };
	obs_register_source(&info);
}

// ---------- recording ----------

static std::mutex obs_lock; // one command at a time
static std::atomic<bool> hooked{false};
/** Running with administrator rights (set at start; see "who may connect") */
static bool running_elevated = false;
/** The install folder: three up from this exe (obs\bin\64bit) */
static std::wstring install_dir;
static std::string utf8(const std::wstring &w);

struct Recording {
	obs_scene_t *scene = nullptr;
	obs_source_t *game = nullptr; // the picture: Game Capture, or the monitor for a screen recording
	bool screen = false; // `game` is a monitor
	bool screen_fallback = false; // ...captured with Windows Graphics Capture, the usual way having failed (watch)
	std::vector<obs_source_t *> audio; // on output channels 1..n
	std::vector<std::string> audio_ids; // the core's id for each of them (volume, levels)
	obs_encoder_t *video_encoder = nullptr;
	std::vector<obs_encoder_t *> audio_encoders; // one per track
	obs_output_t *output = nullptr;
	obs_output_t *replay = nullptr; // the replay buffer, sharing the encoders above
	std::string path;
	std::string applied; // the setup JSON these were built from
	uint32_t base_w = 0, base_h = 0, out_w = 0, out_h = 0, fps = 0;
} rec;

// ---------- audio levels (for the meters in Settings) ----------

struct Meter {
	std::string id;
	obs_volmeter_t *volmeter = nullptr;
	std::atomic<float> peak{-100.0f}; // loudest since the last report, dB
};

static std::mutex meters_lock;
static std::vector<std::unique_ptr<Meter>> meters; // one per audio source, guarded by meters_lock
static std::atomic<bool> metering{false};

static void on_levels(void *param, const float[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS], const float[MAX_AUDIO_CHANNELS])
{
	auto *m = static_cast<Meter *>(param);
	float top = std::max(peak[0], peak[1]);
	float prev = m->peak.load();
	while (top > prev && !m->peak.compare_exchange_weak(prev, top)) {
	}
}

static void add_meter(const std::string &id, obs_source_t *source)
{
	auto m = std::make_unique<Meter>();
	m->id = id;
	m->volmeter = obs_volmeter_create(OBS_FADER_LOG);
	obs_volmeter_attach_source(m->volmeter, source);
	obs_volmeter_add_callback(m->volmeter, on_levels, m.get());
	std::lock_guard<std::mutex> lock(meters_lock);
	meters.push_back(std::move(m));
}

static void remove_meters()
{
	std::lock_guard<std::mutex> lock(meters_lock);
	for (auto &m : meters) {
		obs_volmeter_remove_callback(m->volmeter, on_levels, m.get());
		obs_volmeter_destroy(m->volmeter);
	}
	meters.clear();
}

/** While metering is on: every source's loudest moment, ten times a second */
static void report_levels()
{
	while (!exiting) {
		Sleep(100);
		if (!metering)
			continue;
		std::string line = "evt\tlevels";
		{
			std::lock_guard<std::mutex> lock(meters_lock);
			for (auto &m : meters) {
				float p = m->peak.exchange(-100.0f);
				char field[32];
				snprintf(field, sizeof(field), "=%.1f", p < -100.0f ? -100.0f : p);
				line += "\t" + m->id + field;
			}
		}
		send_line(line);
	}
}

// ---------- the picture check ----------
//
// Whether what's being recorded is black, said in the log (owner, 2026-10-25: a tester's games and screen
// recordings came out black, and the log couldn't say so for the screen). Every 2 s a 64x36 copy of the
// canvas is made on the graphics card and read back a frame later, so nothing waits on the card: a few
// thousand pixels every 2 s. The log gets a line when the picture turns black or comes back, and each
// recording's totals when it ends (and in the status line every minute, below).

static const uint32_t CHECK_W = 64, CHECK_H = 36;
static const uint64_t CHECK_EVERY_NS = 2000000000ULL;

struct PictureCheck {
	gs_texrender_t *scaled = nullptr;
	gs_stagesurf_t *stage = nullptr;
	bool pending = false; // a copy was asked for and not read yet
	uint64_t requested = 0;
	uint8_t last[CHECK_W * CHECK_H] = {}; // each pixel's brightness at the last check
	std::atomic<int> state{-1}; // -1 not checked yet, 0 black, 1 showing something
	// Since the recording started (counting while one runs)
	std::atomic<bool> counting{false};
	std::atomic<int> checks{0}, black{0}, unchanged{0};
};
static PictureCheck picture;

/** What a check found: the brightest pixel, the average, how many pixels are lit and how many changed (0-255 scale) */
static void note_picture(int brightest, int average, int lit, int changed)
{
	const int all = CHECK_W * CHECK_H;
	bool black = brightest <= 12;
	if (picture.counting) {
		picture.checks++;
		picture.black += black;
		picture.unchanged += changed == 0;
	}
	if (picture.state == (black ? 0 : 1))
		return;
	picture.state = black ? 0 : 1;
	if (black)
		blog(LOG_WARNING, "[recorder] picture: black (brightest pixel %d of 255)%s", brightest, picture.counting ? ", while recording" : "");
	else
		blog(LOG_INFO, "[recorder] picture: showing something (average brightness %d%%, %d%% of it lit)", average * 100 / 255, lit * 100 / all);
	send_line(std::string("evt\tpicture\t") + (black ? "black" : "ok"));
}

/** After the canvas is drawn (graphics thread): read the last copy, and every 2 s ask for a new one */
static void on_rendered(void *)
{
	PictureCheck &p = picture;
	uint64_t now = os_gettime_ns();
	if (p.pending) {
		// Give the card a moment to make the copy, so mapping it doesn't wait
		if (now - p.requested < 100000000ULL)
			return;
		p.pending = false;
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(p.stage, &data, &linesize)) {
			int brightest = 0, lit = 0, changed = 0;
			long long sum = 0;
			for (uint32_t y = 0; y < CHECK_H; y++) {
				for (uint32_t x = 0; x < CHECK_W; x++) {
					const uint8_t *px = data + y * linesize + x * 4;
					int v = std::max({px[0], px[1], px[2]});
					brightest = std::max(brightest, v);
					sum += v;
					lit += v > 24;
					uint8_t &was = p.last[y * CHECK_W + x];
					changed += abs(v - was) > 8;
					was = (uint8_t)v;
				}
			}
			gs_stagesurface_unmap(p.stage);
			note_picture(brightest, (int)(sum / (CHECK_W * CHECK_H)), lit, changed);
		}
	}
	if (now - p.requested < CHECK_EVERY_NS)
		return;
	p.requested = now;
	gs_texture_t *tex = obs_get_main_texture();
	if (!tex)
		return;
	if (!p.scaled)
		p.scaled = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!p.stage)
		p.stage = gs_stagesurface_create(CHECK_W, CHECK_H, GS_RGBA);
	if (!p.scaled || !p.stage)
		return;
	uint32_t w = gs_texture_get_width(tex), h = gs_texture_get_height(tex);
	gs_texrender_reset(p.scaled);
	if (!gs_texrender_begin(p.scaled, CHECK_W, CHECK_H))
		return;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_enable_blending(false);
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), tex);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(tex, 0, w, h);
	gs_blend_state_pop();
	gs_texrender_end(p.scaled);
	gs_stage_texture(p.stage, gs_texrender_get_texture(p.scaled));
	p.pending = true;
}

/** A recording's picture totals, e.g. "black in 3 of 120 checks, unchanged in 5" */
static std::string picture_totals()
{
	char text[128];
	snprintf(text, sizeof(text), "picture black in %d of %d checks, unchanged in %d", picture.black.load(), picture.checks.load(),
		 picture.unchanged.load());
	return text;
}

static void on_hooked(void *, calldata_t *)
{
	hooked = true;
	hook_pipe_taken = 0;
	send_line("evt\thooked");
}

static void on_unhooked(void *, calldata_t *)
{
	hooked = false;
	send_line("evt\tunhooked");
}

static void on_output_start(void *, calldata_t *)
{
	picture.checks = picture.black = picture.unchanged = 0;
	picture.counting = true;
	blog(LOG_INFO, "[recorder] recording started: %s", rec.path.c_str());
	send_line("evt\tstarted");
}

static void on_output_stop(void *, calldata_t *cd)
{
	long long code = calldata_int(cd, "code");
	picture.counting = false;
	stop_asked_at = 0;
	if (code != OBS_OUTPUT_SUCCESS)
		blog(LOG_WARNING, "[recorder] recording stopped with code %lld: %s", code,
		     rec.output ? (obs_output_get_last_error(rec.output) ? obs_output_get_last_error(rec.output) : "") : "");
	blog(LOG_INFO, "[recorder] recording stopped: %d frames written, %d dropped; %s", rec.output ? obs_output_get_total_frames(rec.output) : 0,
	     rec.output ? obs_output_get_frames_dropped(rec.output) : 0, picture_totals().c_str());
	send_line("evt\tstopped\t" + rec.path);
}

static void on_buffer_start(void *, calldata_t *)
{
	send_line("evt\tbuffer-started");
}

static void on_buffer_stop(void *, calldata_t *cd)
{
	long long code = calldata_int(cd, "code");
	if (code != OBS_OUTPUT_SUCCESS)
		blog(LOG_WARNING, "[recorder] replay buffer stopped with code %lld", code);
	send_line("evt\tbuffer-stopped");
}

/** The buffer was written to a file: report where (OBS names it; the core renames it) */
static void on_buffer_saved(void *, calldata_t *)
{
	std::string path;
	if (rec.replay) {
		calldata_t cd = {};
		proc_handler_call(obs_output_get_proc_handler(rec.replay), "get_last_replay", &cd);
		const char *p = calldata_string(&cd, "path");
		if (p)
			path = p;
		calldata_free(&cd);
	}
	send_line("evt\tclip\t" + path);
}

static bool recording_active()
{
	return rec.output && obs_output_active(rec.output);
}

static bool buffer_active()
{
	return rec.replay && obs_output_active(rec.replay);
}

/** Release the scene, sources, encoders and outputs (not while recording or buffering). */
static void teardown()
{
	if (rec.output) {
		obs_output_release(rec.output);
		rec.output = nullptr;
	}
	if (rec.replay) {
		obs_output_release(rec.replay);
		rec.replay = nullptr;
	}
	remove_meters();
	if (rec.video_encoder) {
		obs_encoder_release(rec.video_encoder);
		rec.video_encoder = nullptr;
	}
	for (auto *enc : rec.audio_encoders)
		obs_encoder_release(enc);
	rec.audio_encoders.clear();
	for (uint32_t ch = 0; ch < MAX_CHANNELS; ch++)
		obs_set_output_source(ch, nullptr);
	for (auto *src : rec.audio)
		obs_source_release(src);
	rec.audio.clear();
	rec.audio_ids.clear();
	if (rec.game) {
		obs_source_release(rec.game);
		rec.game = nullptr;
	}
	rec.screen = false;
	rec.screen_fallback = false;
	if (rec.scene) {
		obs_scene_release(rec.scene);
		rec.scene = nullptr;
	}
	hooked = false;
	hook_pipe_taken = 0;
	rec.applied.clear();
}

static bool reset_video(uint32_t base_w, uint32_t base_h, uint32_t out_w, uint32_t out_h, uint32_t fps)
{
	obs_video_info ovi = {};
	ovi.graphics_module = "libobs-d3d11.dll";
	ovi.fps_num = fps;
	ovi.fps_den = 1;
	ovi.base_width = base_w;
	ovi.base_height = base_h;
	ovi.output_width = out_w;
	ovi.output_height = out_h;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	int result = obs_reset_video(&ovi);
	if (result != OBS_VIDEO_SUCCESS) {
		blog(LOG_ERROR, "[recorder] obs_reset_video failed: %d", result);
		return false;
	}
	rec.base_w = base_w;
	rec.base_h = base_h;
	rec.out_w = out_w;
	rec.out_h = out_h;
	rec.fps = fps;
	return true;
}

/**
 * Build everything from the core's setup: canvas and output size, frame rate, the Game Capture
 * source and audio sources (with their track mixers), the video encoder and its settings, one AAC
 * encoder per audio track, and the MP4 output.
 */
static std::string apply_setup(const std::string &json)
{
	if (recording_active() || buffer_active())
		return "Can't change the setup while recording";
	if (json == rec.applied)
		return "";
	obs_data_t *setup = obs_data_create_from_json(json.c_str());
	if (!setup)
		return "Bad setup";
	teardown();

	obs_data_t *base = obs_data_get_obj(setup, "base");
	obs_data_t *out = obs_data_get_obj(setup, "out");
	uint32_t bw = (uint32_t)obs_data_get_int(base, "w"), bh = (uint32_t)obs_data_get_int(base, "h");
	uint32_t ow = (uint32_t)obs_data_get_int(out, "w"), oh = (uint32_t)obs_data_get_int(out, "h");
	uint32_t fps = (uint32_t)obs_data_get_int(setup, "fps");
	obs_data_release(base);
	obs_data_release(out);
	std::string error;
	if (bw != rec.base_w || bh != rec.base_h || ow != rec.out_w || oh != rec.out_h || fps != rec.fps) {
		if (!reset_video(bw, bh, ow, oh, fps)) {
			obs_data_release(setup);
			return "The video settings couldn't be applied";
		}
	}

	rec.scene = obs_scene_create_private("Game");
	obs_data_array_t *inputs = obs_data_get_array(setup, "inputs");
	size_t count = obs_data_array_count(inputs);
	uint32_t channel = 1; // 0 is the scene; audio sources take 1, 2, ...
	for (size_t i = 0; i < count; i++) {
		obs_data_t *input = obs_data_array_item(inputs, i);
		const char *kind = obs_data_get_string(input, "kind");
		const char *name = obs_data_get_string(input, "name");
		obs_data_t *settings = obs_data_get_obj(input, "settings");
		bool picture = strcmp(kind, "game_capture") == 0 || strcmp(kind, "monitor_capture") == 0;
		// What each source was made with, for the log. A microphone's settings name the device, so audio
		// sources only say what they are.
		if (picture)
			blog(LOG_INFO, "[recorder] picture source %s '%s': %s", kind, name, obs_data_get_json(settings));
		else
			blog(LOG_INFO, "[recorder] audio source %s '%s', tracks 0x%llx", kind, name, (long long)obs_data_get_int(input, "mixers"));
		obs_source_t *source = obs_source_create_private(kind, name, settings);
		obs_data_release(settings);
		if (!source) {
			blog(LOG_WARNING, "[recorder] couldn't create %s '%s'", kind, name);
		} else if (picture && !rec.game) {
			rec.game = source;
			rec.screen = strcmp(kind, "monitor_capture") == 0;
			duplicator_failed_at = 0;
			obs_sceneitem_t *item = obs_scene_add(rec.scene, source);
			// Scale it to fit the canvas, whatever size League's window (or the monitor) is
			vec2 bounds;
			vec2_set(&bounds, (float)bw, (float)bh);
			obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
			obs_sceneitem_set_bounds(item, &bounds);
			obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
			if (!rec.screen) {
				signal_handler_t *signals = obs_source_get_signal_handler(source);
				signal_handler_connect(signals, "hooked", on_hooked, nullptr);
				signal_handler_connect(signals, "unhooked", on_unhooked, nullptr);
			}
		} else if (picture) {
			obs_source_release(source);
		} else if (channel < MAX_CHANNELS) {
			obs_source_set_audio_mixers(source, (uint32_t)obs_data_get_int(input, "mixers"));
			obs_source_set_volume(source, obs_data_has_user_value(input, "volume") ? (float)obs_data_get_double(input, "volume") : 1.0f);
			obs_set_output_source(channel++, source);
			rec.audio.push_back(source);
			rec.audio_ids.push_back(obs_data_get_string(input, "id"));
			add_meter(rec.audio_ids.back(), source);
		} else {
			obs_source_release(source);
		}
		obs_data_release(input);
	}
	obs_data_array_release(inputs);
	obs_set_output_source(0, obs_scene_get_source(rec.scene));

	obs_data_t *encoder_settings = obs_data_get_obj(setup, "encoderSettings");
	blog(LOG_INFO, "[recorder] video encoder %s: %s", obs_data_get_string(setup, "encoder"), obs_data_get_json(encoder_settings));
	rec.video_encoder = obs_video_encoder_create(obs_data_get_string(setup, "encoder"), "video", encoder_settings, nullptr);
	obs_data_release(encoder_settings);
	if (!rec.video_encoder)
		error = std::string("The encoder couldn't be created: ") + obs_data_get_string(setup, "encoder");
	else
		obs_encoder_set_video(rec.video_encoder, obs_get_video());

	// Tracks: one AAC encoder per track bit (track 1 = the full mix)
	uint32_t tracks = (uint32_t)obs_data_get_int(setup, "recTracks");
	obs_data_t *aac = obs_data_create();
	obs_data_set_int(aac, "bitrate", 192);
	for (uint32_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		if (!(tracks & (1u << i)))
			continue;
		std::string name = "audio" + std::to_string(i + 1);
		obs_encoder_t *enc = obs_audio_encoder_create("ffmpeg_aac", name.c_str(), aac, i, nullptr);
		if (enc) {
			obs_encoder_set_audio(enc, obs_get_audio());
			rec.audio_encoders.push_back(enc);
		}
	}
	obs_data_release(aac);

	rec.output = obs_output_create("ffmpeg_muxer", "record", nullptr, nullptr);
	if (rec.output) {
		if (rec.video_encoder)
			obs_output_set_video_encoder(rec.output, rec.video_encoder);
		for (size_t i = 0; i < rec.audio_encoders.size(); i++)
			obs_output_set_audio_encoder(rec.output, rec.audio_encoders[i], i);
		signal_handler_t *signals = obs_output_get_signal_handler(rec.output);
		signal_handler_connect(signals, "start", on_output_start, nullptr);
		signal_handler_connect(signals, "stop", on_output_stop, nullptr);
	} else {
		error = "The recording output couldn't be created";
	}

	rec.replay = obs_output_create("replay_buffer", "replay", nullptr, nullptr);
	if (rec.replay) {
		if (rec.video_encoder)
			obs_output_set_video_encoder(rec.replay, rec.video_encoder);
		for (size_t i = 0; i < rec.audio_encoders.size(); i++)
			obs_output_set_audio_encoder(rec.replay, rec.audio_encoders[i], i);
		signal_handler_t *signals = obs_output_get_signal_handler(rec.replay);
		signal_handler_connect(signals, "start", on_buffer_start, nullptr);
		signal_handler_connect(signals, "stop", on_buffer_stop, nullptr);
		signal_handler_connect(signals, "saved", on_buffer_saved, nullptr);
	} else {
		blog(LOG_WARNING, "[recorder] the replay buffer couldn't be created; clips won't work");
	}
	obs_data_release(setup);
	if (error.empty())
		rec.applied = json;
	blog(LOG_INFO, "[recorder] setup applied: %ux%u -> %ux%u @ %u fps, %zu audio sources, %zu tracks", bw, bh, ow, oh, fps,
	     rec.audio.size(), rec.audio_encoders.size());
	return error;
}

static std::string start_recording(const std::string &folder, const std::string &name)
{
	if (!rec.output || rec.applied.empty())
		return "The recorder isn't set up yet";
	if (recording_active())
		return "Already recording";
	rec.path = folder + "/" + name + ".mp4";
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "path", rec.path.c_str());
	// Fragmented MP4, as OBS does it: playable even if the recording is cut off by a crash
	obs_data_set_string(settings, "muxer_settings", "movflags=frag_keyframe+empty_moov+delay_moov");
	obs_output_update(rec.output, settings);
	obs_data_release(settings);
	if (!obs_output_start(rec.output)) {
		const char *why = obs_output_get_last_error(rec.output);
		return std::string("Recording didn't start") + (why ? std::string(": ") + why : "");
	}
	return "";
}

static std::string start_buffer(const std::string &folder, int seconds, int max_mb)
{
	if (!rec.replay || rec.applied.empty())
		return "The recorder isn't set up yet";
	if (buffer_active())
		return "";
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "directory", folder.c_str());
	obs_data_set_string(settings, "format", "Clip %CCYY-%MM-%DD_%hh-%mm-%ss");
	obs_data_set_string(settings, "extension", "mp4");
	obs_data_set_bool(settings, "allow_spaces", true);
	obs_data_set_int(settings, "max_time_sec", seconds);
	obs_data_set_int(settings, "max_size_mb", max_mb);
	obs_output_update(rec.replay, settings);
	obs_data_release(settings);
	if (!obs_output_start(rec.replay)) {
		const char *why = obs_output_get_last_error(rec.replay);
		return std::string("The clip buffer didn't start") + (why ? std::string(": ") + why : "");
	}
	return "";
}

static std::string save_clip()
{
	if (!buffer_active())
		return "The clip buffer isn't running";
	calldata_t cd = {};
	proc_handler_call(obs_output_get_proc_handler(rec.replay), "save", &cd);
	calldata_free(&cd);
	return "";
}

// ---------- a stop or a close that doesn't finish ----------
//
// A tester's recorder (2026-10-29) never finished stopping a recording: OBS's muxer output waits, with no limit,
// for its file writer (obs-ffmpeg-mux.exe) to exit once it has the last frames, on the thread that hands the
// encoder's frames on. The writer never did, so that thread stopped for good: the recorder said "recording"
// from then on (the next two games: "Already recording"), and closing it later hung the same way. Started with
// administrator rights, the app couldn't end it either, so it lived on next to the next recorder and took
// League's picture in 4 of the next 5 games (the other's video black). Why the writer hung isn't known (what's
// logged when it's ended below should say more), so both waits get a limit here. The video is written in
// 2-second pieces as it goes (fragmented MP4), so ending the writer loses at most the last moments.
// A writer that's slow but still going (a drive waking up, a busy network folder) isn't stuck: past the first
// limit it's ended only once it has made no progress for a few seconds, neither writing to the file nor using
// the processor (the tester's used none at all), and at the latest well before the app gives up on the stop.

/** How long a stop may take before the file writer is looked at (it normally takes well under a second) */
static const uint64_t STOP_STUCK_NS = 15000000000ULL;
/** From then on, how often it's looked at: no progress since the last look and it's ended */
static const uint64_t STOP_LOOK_NS = 5000000000ULL;
/** ...and the latest it's ended, still working or not (the app waits 60 s for a stop: obs.ts stop) */
static const uint64_t STOP_GIVE_UP_NS = 45000000000ULL;
/** How long closing may take before the recorder ends itself */
static const DWORD CLOSE_STUCK_MS = 10000;
/** How long the recorder runs on with no app connected before it closes (watch) */
static const uint64_t ALONE_NS = 30000000000ULL;

/** League's game, which a recording with no app waits for (LR_RECORDER_LEAGUE_EXE names another, for testing) */
static const std::wstring &league_exe()
{
	static const std::wstring exe = [] {
		wchar_t name[MAX_PATH] = L"";
		return GetEnvironmentVariableW(L"LR_RECORDER_LEAGUE_EXE", name, MAX_PATH) ? std::wstring(name) : std::wstring(L"League of Legends.exe");
	}();
	return exe;
}

static std::wstring image_of(DWORD pid, ULONGLONG *created);

static double cpu_seconds(HANDLE process)
{
	FILETIME unused, kernel, user;
	if (!GetProcessTimes(process, &unused, &unused, &kernel, &user))
		return -1;
	auto ticks = [](FILETIME t) { return ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime; };
	return (double)(ticks(kernel) + ticks(user)) / 1e7;
}

/** The programs this recorder started that are still running (`exe` only, if given): by parent id, and started
 *  after this one, so never another program's (OBS Studio has its own obs-ffmpeg-mux.exe) */
static std::vector<DWORD> own_children(const wchar_t *exe)
{
	std::vector<DWORD> found;
	ULONGLONG self_created = 0;
	image_of(GetCurrentProcessId(), &self_created);
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return found;
	PROCESSENTRY32W e{sizeof(e)};
	for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
		if (e.th32ParentProcessID != GetCurrentProcessId() || (exe && _wcsicmp(e.szExeFile, exe) != 0))
			continue;
		ULONGLONG created = 0;
		image_of(e.th32ProcessID, &created);
		if (created >= self_created)
			found.push_back(e.th32ProcessID);
	}
	CloseHandle(snap);
	return found;
}

/** The file writers running when the stop was asked for, with their CPU time then (to tell busy from waiting) */
static std::vector<std::pair<DWORD, double>> writers_at_stop;
/** ...their CPU time and the video's size at the last look since (writers_progressed) */
static std::vector<std::pair<DWORD, double>> writers_looked;
static int64_t size_looked = -1;
static std::mutex writers_lock;

static void note_writers_at_stop()
{
	std::lock_guard<std::mutex> lock(writers_lock);
	writers_at_stop.clear();
	for (DWORD pid : own_children(L"obs-ffmpeg-mux.exe")) {
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		writers_at_stop.push_back({pid, process ? cpu_seconds(process) : -1});
		if (process)
			CloseHandle(process);
	}
	writers_looked = writers_at_stop;
	size_looked = os_get_file_size(rec.path.c_str());
}

/** Whether the file writers did anything since the last look: the video grew, or one used the processor */
static bool writers_progressed()
{
	std::lock_guard<std::mutex> lock(writers_lock);
	bool progressed = false;
	int64_t size = os_get_file_size(rec.path.c_str());
	if (size > size_looked)
		progressed = true;
	size_looked = size;
	for (auto &w : writers_looked) {
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, w.first);
		if (!process)
			continue;
		double cpu = cpu_seconds(process);
		CloseHandle(process);
		if (cpu >= 0 && w.second >= 0 && cpu - w.second >= 0.05)
			progressed = true;
		w.second = cpu;
	}
	return progressed;
}

/** End this recorder's file writers (only those running at the stop, if `at_stop`) and log what each was doing */
static void end_file_writers(const char *why, bool at_stop)
{
	std::vector<DWORD> pids = own_children(L"obs-ffmpeg-mux.exe");
	std::lock_guard<std::mutex> lock(writers_lock);
	for (DWORD pid : pids) {
		auto was = std::find_if(writers_at_stop.begin(), writers_at_stop.end(), [pid](auto &w) { return w.first == pid; });
		if (at_stop && was == writers_at_stop.end())
			continue;
		HANDLE process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!process) {
			blog(LOG_WARNING, "[recorder] %s: the file writer (obs-ffmpeg-mux.exe, pid %lu) couldn't be opened to end it (error %lu)", why, pid,
			     GetLastError());
			continue;
		}
		double cpu = cpu_seconds(process);
		char busy[128] = "";
		if (was != writers_at_stop.end() && was->second >= 0 && cpu >= 0)
			snprintf(busy, sizeof(busy), ", %.2f s of CPU since the stop (%s)", cpu - was->second,
				 cpu - was->second < 0.05 ? "it was waiting on something" : "it was still working");
		bool ended = TerminateProcess(process, 1);
		blog(LOG_WARNING, "[recorder] %s: %s the file writer (obs-ffmpeg-mux.exe, pid %lu%s)", why, ended ? "ended" : "couldn't end", pid, busy);
		CloseHandle(process);
	}
}

/** Closing (quit) hasn't finished in CLOSE_STUCK_MS: end this recorder, with what it started, rather than run on */
static void end_if_closing_hangs()
{
	Sleep(CLOSE_STUCK_MS);
	blog(LOG_WARNING, "[recorder] closing hasn't finished after %lu s: ending now", CLOSE_STUCK_MS / 1000);
	end_file_writers("closing hung", false);
	for (DWORD pid : own_children(nullptr)) {
		HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
		if (process) {
			TerminateProcess(process, 1);
			CloseHandle(process);
		}
	}
	{
		std::lock_guard<std::mutex> lock(log_lock);
		if (log_file)
			fflush(log_file);
	}
	// (Not ExitProcess: that runs every library's clean-up, where it may hang the same way)
	TerminateProcess(GetCurrentProcess(), 0);
}

/** Close: the main loop ends, stops what's still running and shuts libobs down (with the limit above) */
static void close_recorder()
{
	if (!exiting)
		std::thread(end_if_closing_hangs).detach();
	exiting = true;
	PostThreadMessageW(main_thread, WM_QUIT, 0, 0);
}

static void handle_line(const std::string &line)
{
	std::vector<std::string> head = split_tabs(line, 3);
	if (head.size() < 2)
		return;
	const std::string &id = head[0], &cmd = head[1];
	std::string error;
	std::string extra;
	{
		std::lock_guard<std::mutex> lock(obs_lock);
		if (cmd == "setup" && head.size() >= 3) {
			error = apply_setup(head[2]);
		} else if (cmd == "status") {
			// (The last: running with administrator rights, so a core started again after a crash knows: leagueAdmin.ts)
			extra = std::string("\t") + (recording_active() ? "1" : "0") + "\t" + (hooked ? "1" : "0") + "\t" + (buffer_active() ? "1" : "0") + "\t" +
				(running_elevated ? "1" : "0");
		} else if (cmd == "start" && head.size() >= 3) {
			std::vector<std::string> args = split_tabs(head[2]);
			error = args.size() >= 2 ? start_recording(args[0], args[1]) : "start needs a folder and a name";
		} else if (cmd == "stop") {
			if (recording_active()) {
				note_writers_at_stop();
				stop_asked_at = os_gettime_ns();
				obs_output_stop(rec.output);
			} else
				error = "Not recording";
		} else if (cmd == "buffer-start" && head.size() >= 3) {
			std::vector<std::string> args = split_tabs(head[2]);
			error = args.size() >= 3 ? start_buffer(args[0], atoi(args[1].c_str()), atoi(args[2].c_str()))
						 : "buffer-start needs a folder, seconds and a size";
		} else if (cmd == "buffer-stop") {
			if (buffer_active())
				obs_output_stop(rec.replay);
		} else if (cmd == "clip") {
			error = save_clip();
		} else if (cmd == "volume" && head.size() >= 3) {
			std::vector<std::string> args = split_tabs(head[2]);
			bool found = false;
			for (size_t i = 0; args.size() >= 2 && i < rec.audio_ids.size(); i++) {
				if (rec.audio_ids[i] == args[0]) {
					obs_source_set_volume(rec.audio[i], (float)atof(args[1].c_str()));
					found = true;
				}
			}
			if (!found)
				error = "No such audio source";
		} else if (cmd == "meters" && head.size() >= 3) {
			metering = head[2] == "on";
		} else if (cmd == "quit") {
			close_recorder();
		} else {
			error = "Unknown command " + cmd;
		}
	}
	send_line(error.empty() ? "res\t" + id + "\tok" + extra : "res\t" + id + "\terror\t" + error);
}

/**
 * Who may use the pipe: this Windows user (and the system and administrators), from a normal app too. Started
 * with administrator rights (when League runs with them, 2026-10-26), the pipe's default rights would let the
 * app only read it: they go to the administrators, and an elevated process's objects are out of a normal one's
 * reach for writing. Null (Windows' default) if the user can't be read.
 */
static SECURITY_ATTRIBUTES *pipe_security()
{
	static SECURITY_ATTRIBUTES attributes = {sizeof(attributes), nullptr, FALSE};
	if (attributes.lpSecurityDescriptor)
		return &attributes;
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return nullptr;
	DWORD size = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	std::vector<BYTE> user(size ? size : 1);
	wchar_t *sid = nullptr;
	if (size && GetTokenInformation(token, TokenUser, user.data(), size, &size))
		ConvertSidToStringSidW(((TOKEN_USER *)user.data())->User.Sid, &sid);
	CloseHandle(token);
	if (!sid)
		return nullptr;
	// Full access for the system, administrators and this user; the medium label lets a normal app write to it
	std::wstring sddl = std::wstring(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;") + sid + L")S:(ML;;NW;;;ME)";
	LocalFree(sid);
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
		blog(LOG_WARNING, "[recorder] the pipe's rights couldn't be set (error %lu): Windows' default", GetLastError());
		return nullptr;
	}
	attributes.lpSecurityDescriptor = descriptor;
	return &attributes;
}

// ---------- who may connect ----------
//
// With administrator rights the pipe is open to normal programs (above), so any program running as this user could
// otherwise drive a recorder that has those rights: start a recording into one of Windows' own folders, say. So
// then it takes commands only from League Recorder's core in this install (core\league-recorder-core.exe), started
// by League Recorder's window in this install (League Recorder.exe). That keeps out a program that merely finds the
// pipe; one set on getting in could still go further, since the install folder is the user's own (2026-10-27).

/** A process's program and when it started; "" if it can't be read */
static std::wstring image_of(DWORD pid, ULONGLONG *created)
{
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process)
		return L"";
	wchar_t path[MAX_PATH * 2];
	DWORD len = _countof(path);
	std::wstring image = QueryFullProcessImageNameW(process, 0, path, &len) ? std::wstring(path, len) : L"";
	FILETIME start, unused;
	if (created)
		*created = GetProcessTimes(process, &start, &unused, &unused, &unused) ? ((ULONGLONG)start.dwHighDateTime << 32) | start.dwLowDateTime : 0;
	CloseHandle(process);
	return image;
}

/** The process that started `pid` (0 if it isn't known) */
static DWORD parent_of(DWORD pid)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return 0;
	DWORD parent = 0;
	PROCESSENTRY32W e{sizeof(e)};
	for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
		if (e.th32ProcessID == pid) {
			parent = e.th32ParentProcessID;
			break;
		}
	CloseHandle(snap);
	return parent;
}

/** Whether the program connected to `pipe` is League Recorder's core, started by its window; `why` says if not */
static bool trusted_client(HANDLE pipe, std::string &why)
{
	ULONG pid = 0;
	if (!GetNamedPipeClientProcessId(pipe, &pid)) {
		why = "Windows didn't say which program it is (error " + std::to_string(GetLastError()) + ")";
		return false;
	}
	ULONGLONG started = 0, parent_started = 0;
	std::wstring image = image_of(pid, &started);
	if (_wcsicmp(image.c_str(), (install_dir + L"\\core\\league-recorder-core.exe").c_str()) != 0) {
		why = "pid " + std::to_string(pid) + " is " + (image.empty() ? std::string("a program that can't be read") : utf8(image)) + ", not League Recorder's core";
		return false;
	}
	DWORD parent = parent_of(pid);
	std::wstring parent_image = parent ? image_of(parent, &parent_started) : L"";
	// (A parent that started after its child is another program that got the old one's number)
	if (_wcsicmp(parent_image.c_str(), (install_dir + L"\\League Recorder.exe").c_str()) != 0 || parent_started > started) {
		why = "League Recorder's core (pid " + std::to_string(pid) + "), but not started by League Recorder's window (its parent: " +
		      (parent_image.empty() ? std::string("gone or unreadable") : utf8(parent_image)) + ")";
		return false;
	}
	return true;
}

static void serve(std::wstring name)
{
	// (LR_RECORDER_CHECK_CLIENTS=1 checks connections without administrator rights too, for testing)
	wchar_t forced[4] = L"";
	bool check = running_elevated || (GetEnvironmentVariableW(L"LR_RECORDER_CHECK_CLIENTS", forced, 4) && forced[0] == L'1');
	if (check)
		blog(LOG_INFO, "[recorder] taking commands only from League Recorder's core in %s", utf8(install_dir).c_str());
	while (!exiting) {
		HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
					       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
					       64 * 1024, 64 * 1024, 0, pipe_security());
		if (pipe == INVALID_HANDLE_VALUE) {
			blog(LOG_WARNING, "[recorder] CreateNamedPipe failed: %lu", GetLastError());
			Sleep(1000);
			continue;
		}
		OVERLAPPED ov{};
		ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		bool connected = ConnectNamedPipe(pipe, &ov) != FALSE;
		if (!connected) {
			DWORD err = GetLastError();
			if (err == ERROR_IO_PENDING) {
				DWORD ignored;
				connected = GetOverlappedResult(pipe, &ov, &ignored, TRUE) != FALSE;
			} else {
				connected = err == ERROR_PIPE_CONNECTED;
			}
		}
		std::string why;
		if (connected && check && !trusted_client(pipe, why)) {
			blog(LOG_WARNING, "[recorder] refused a connection: %s", why.c_str());
			connected = false;
		}
		if (connected) {
			{
				std::lock_guard<std::mutex> lock(write_lock);
				client = pipe;
			}
			std::string buffer;
			char chunk[16384];
			for (;;) {
				ResetEvent(ov.hEvent);
				DWORD n = 0;
				if (!ReadFile(pipe, chunk, sizeof(chunk), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING)
					break;
				if (!GetOverlappedResult(pipe, &ov, &n, TRUE) || n == 0)
					break;
				buffer.append(chunk, n);
				size_t nl;
				while ((nl = buffer.find('\n')) != std::string::npos) {
					std::string line = buffer.substr(0, nl);
					buffer.erase(0, nl + 1);
					if (!line.empty() && line.back() == '\r')
						line.pop_back();
					handle_line(line);
				}
			}
			std::lock_guard<std::mutex> lock(write_lock);
			client = INVALID_HANDLE_VALUE;
		}
		DisconnectNamedPipe(pipe);
		CloseHandle(ov.hEvent);
		CloseHandle(pipe);
	}
}

static std::string utf8(const std::wstring &w);

// ---------- rights, League's process, and a status line ----------
//
// More for the log (owner, 2026-10-25: "a log having too much information is almost always beneficial").
// A tester's Game Capture said "could not open process" all game long, which doesn't say why: Windows'
// reason and both programs' rights do. League running as administrator, with League Recorder not,
// looks like "access denied" with League's rights unreadable.

/** "elevated, integrity high (administrator)" for a process handle with at least limited query rights */
static std::string rights_of(HANDLE process)
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(process, TOKEN_QUERY, &token))
		return "its rights can't be read (error " + std::to_string(GetLastError()) + ")";
	TOKEN_ELEVATION elevation = {};
	DWORD size = 0;
	bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
	std::string level = "unknown";
	GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
	std::vector<BYTE> label(size ? size : 1);
	if (size && GetTokenInformation(token, TokenIntegrityLevel, label.data(), size, &size)) {
		PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(label.data())->Label.Sid;
		DWORD rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
		level = rid >= SECURITY_MANDATORY_SYSTEM_RID   ? "system"
			: rid >= SECURITY_MANDATORY_HIGH_RID   ? "high (administrator)"
			: rid >= SECURITY_MANDATORY_MEDIUM_RID ? "medium (normal)"
							       : "low";
	}
	CloseHandle(token);
	return std::string(elevated ? "elevated" : "not elevated") + ", integrity " + level;
}

/** What Windows says when this process asks for League's process the way Game Capture does; "" if League isn't running */
static std::string league_access()
{
	DWORD pid = find_root_process(L"League of Legends.exe");
	if (!pid)
		return "";
	std::string head = "League of Legends.exe (pid " + std::to_string(pid) + ")";
	HANDLE full = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
	if (full) {
		std::string rights = rights_of(full);
		CloseHandle(full);
		return head + " can be opened for Game Capture (" + rights + "), so it isn't hooked for another reason";
	}
	DWORD error = GetLastError();
	std::string text = head + " can't be opened for Game Capture: error " + std::to_string(error) + (error == ERROR_ACCESS_DENIED ? " (access denied)" : "");
	HANDLE limited = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!limited)
		return text + "; not even with limited rights (error " + std::to_string(GetLastError()) +
		       "): something is blocking access to League (security software, or League runs with more rights)";
	std::string rights = rights_of(limited);
	CloseHandle(limited);
	text += "; with limited rights it opens, " + rights;
	if (error == ERROR_ACCESS_DENIED)
		text += ". That's how League running with more rights than League Recorder looks (League or the Riot Client run as administrator)";
	return text;
}

/** Every second: League's process while Game Capture isn't in it (on a change, or every 5 minutes), and
 * while recording or keeping clips a status line every minute */
static void watch()
{
	std::string last_access;
	bool hook_taken_said = false;
	uint64_t writers_ended_at = 0, stop_looked_at = 0;
	bool slow_stop_said = false;
	uint64_t alone_since = 0;
	bool finishing_alone = false;
	uint64_t access_checked = 0, access_logged = 0, last_status = 0;
	FILETIME was_kernel = {}, was_user = {}, unused;
	uint64_t was_at = os_gettime_ns();
	GetProcessTimes(GetCurrentProcess(), &unused, &unused, &was_kernel, &was_user);
	auto ticks = [](FILETIME t) { return ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime; };
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	while (!exiting) {
		Sleep(1000);
		uint64_t now = os_gettime_ns();
		bool waiting = false;
		{
			std::lock_guard<std::mutex> lock(obs_lock);
			// (Closing: libobs is about to go, see the end of wWinMain)
			if (exiting)
				return;
			waiting = rec.game && !rec.screen && !hooked;
			// The screen, when Windows refuses the usual way of capturing it (DXGI duplication: "unsupported" on
			// some PCs, e.g. from inside some sandboxes, and the picture stays black) for 2 s: Windows Graphics
			// Capture instead, which worked where it didn't (2026-10-25). Not the first choice: on Windows 10 it
			// puts a yellow border around the screen while it records. Only for this setup (a new one tries the
			// usual way again).
			uint64_t failed = duplicator_failed_at;
			if (rec.screen && !rec.screen_fallback && failed && now - failed >= 2000000000ULL && picture.state == 0) {
				rec.screen_fallback = true;
				blog(LOG_WARNING, "[recorder] screen capture: Windows refused the usual way of capturing the screen (DXGI, the "
						  "\"Failed\" lines above), so it's captured with Windows Graphics Capture instead");
				obs_data_t *settings = obs_source_get_settings(rec.game);
				obs_data_set_int(settings, "method", 2);
				obs_source_update(rec.game, settings);
				obs_data_release(settings);
				send_line("evt\tscreen-fallback");
			}
			if ((recording_active() || buffer_active()) && now - last_status >= 60000000000ULL) {
				last_status = now;
				FILETIME kernel, user;
				GetProcessTimes(GetCurrentProcess(), &unused, &unused, &kernel, &user);
				double busy = (double)(ticks(kernel) + ticks(user) - ticks(was_kernel) - ticks(was_user)) * 100.0; // ns
				double cores = busy / (double)(now - was_at);
				was_kernel = kernel, was_user = user, was_at = now;
				PROCESS_MEMORY_COUNTERS memory = {sizeof(memory)};
				GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
				char line[1024];
				snprintf(line, sizeof(line),
					 "[recorder] status: %s%s, %s %ux%u, canvas %.1f fps, lagged frames %u, encoder skipped %u of %u, %s; "
					 "using %.2f of %lu cores, %llu MB",
					 recording_active() ? "recording" : "not recording", buffer_active() ? ", keeping clips" : "",
					 rec.screen ? "screen" : hooked ? "League hooked" : "League NOT hooked", rec.game ? obs_source_get_width(rec.game) : 0,
					 rec.game ? obs_source_get_height(rec.game) : 0, obs_get_active_fps(), obs_get_lagged_frames(),
					 video_output_get_skipped_frames(obs_get_video()), video_output_get_total_frames(obs_get_video()),
					 picture.counting ? picture_totals().c_str() : "no recording", cores, si.dwNumberOfProcessors,
					 (unsigned long long)(memory.WorkingSetSize >> 20));
				blog(LOG_INFO, "%s", line);
			}
		}
		// A stop that hasn't finished: its file writer is what it waits on (see "a stop or a close that doesn't finish").
		// Still writing or working, it's given more time; with no progress since the last look it's ended. With it
		// ended, OBS reports the stop but can still count the output as running (checked with a frozen writer:
		// "Already recording" next), so a few seconds later it's stopped outright.
		uint64_t asked = stop_asked_at;
		if (asked && now - asked >= STOP_STUCK_NS && now - stop_looked_at >= STOP_LOOK_NS) {
			stop_looked_at = now;
			bool progressed = writers_progressed();
			unsigned long long waited = (now - asked) / 1000000000ULL;
			if (progressed && now - asked < STOP_GIVE_UP_NS) {
				if (!slow_stop_said)
					blog(LOG_INFO, "[recorder] the recording is still being finished %llu s after the stop (its file writer is still working): waiting", waited);
				slow_stop_said = true;
			} else {
				stop_asked_at = 0;
				blog(LOG_WARNING, "[recorder] the recording hasn't stopped %llu s after it was asked to%s", waited,
				     progressed ? " (its file writer was still working, but that's the limit)" : " (no progress in the last 5 s)");
				end_file_writers("stopping hung", true);
				writers_ended_at = now;
			}
		}
		if (!asked)
			stop_looked_at = 0, slow_stop_said = false;
		if (writers_ended_at && now - writers_ended_at >= 3000000000ULL) {
			writers_ended_at = 0;
			std::lock_guard<std::mutex> lock(obs_lock);
			if (recording_active()) {
				blog(LOG_WARNING, "[recorder] still recording after its file writer was ended: stopping it outright");
				obs_output_force_stop(rec.output);
			}
		}
		// No app connected: the recorder carries on by itself so that an app started again (its core restarts in
		// under a second after a crash; its crash window's Restart) takes over the same recording. But with the app
		// gone for good (ended in Task Manager, crashed outright, quit while its crash window was up) nothing would
		// ever tell it to stop, and it would run, and record, until the PC restarts. So after ALONE_NS with no app:
		// not recording, it closes; recording a game, it finishes the video once League's game has closed (an app
		// started before then still takes it over), a screen recording at once. The app's next start finds the
		// saved video and adds it to the library (resume.ts).
		bool alone;
		{
			std::lock_guard<std::mutex> lock(write_lock);
			alone = client == INVALID_HANDLE_VALUE;
		}
		if (!alone)
			alone_since = 0, finishing_alone = false;
		else if (!alone_since)
			alone_since = now;
		if (alone && now - alone_since >= ALONE_NS) {
			bool recording, screen;
			{
				std::lock_guard<std::mutex> lock(obs_lock);
				recording = recording_active();
				screen = rec.screen;
			}
			if (!recording && !stop_asked_at) {
				blog(LOG_INFO, "[recorder] no app has been connected for %llu s%s: closing", (unsigned long long)((now - alone_since) / 1000000000ULL),
				     finishing_alone ? " and the recording is saved" : "");
				close_recorder();
				return;
			}
			if (recording && !finishing_alone && (screen || !find_root_process(league_exe()))) {
				finishing_alone = true;
				blog(LOG_WARNING, "[recorder] no app has been connected for %llu s and %s: finishing the recording, then closing",
				     (unsigned long long)((now - alone_since) / 1000000000ULL), screen ? "this is a screen recording" : "League's game has closed");
				std::lock_guard<std::mutex> lock(obs_lock);
				note_writers_at_stop();
				stop_asked_at = os_gettime_ns();
				obs_output_stop(rec.output);
			}
		}
		if (!waiting) {
			last_access.clear();
			hook_taken_said = false;
			continue;
		}
		// Another program has League's hook (it keeps refusing the connection): said once each game, so the app can
		// say why the video is black. In the tester's games it was this recorder's own earlier copy, still running.
		if (hook_pipe_taken >= 3 && !hook_taken_said) {
			hook_taken_said = true;
			blog(LOG_WARNING, "[recorder] Game Capture can't connect to League's hook: another program capturing League the same way has it");
			send_line("evt\thook-taken");
		}
		if (now - access_checked < 5000000000ULL)
			continue;
		access_checked = now;
		std::string access = league_access();
		// (League closed: the next game is told about again)
		if (access.empty()) {
			hook_pipe_taken = 0;
			hook_taken_said = false;
		}
		if (!access.empty() && (access != last_access || now - access_logged >= 300000000000ULL)) {
			blog(access.find("can't") != std::string::npos ? LOG_WARNING : LOG_INFO, "[recorder] %s", access.c_str());
			access_logged = now;
		}
		last_access = access;
	}
}

/** Who and where the recorder runs as, at its start */
static void log_environment()
{
	DWORD session = 0;
	ProcessIdToSessionId(GetCurrentProcessId(), &session);
	blog(LOG_INFO, "[recorder] running %s; Windows session %lu%s", rights_of(GetCurrentProcess()).c_str(), session,
	     GetSystemMetrics(SM_REMOTESESSION) ? " (a remote desktop session: screen capture may be black)" : "");
	// Which C++ runtime it loaded: the copy next to it (shipped from 0.5.31), or Windows' own
	for (const wchar_t *dll : {L"vcruntime140.dll", L"vcruntime140_1.dll", L"msvcp140.dll"}) {
		HMODULE module = GetModuleHandleW(dll);
		wchar_t path[MAX_PATH] = L"";
		if (module)
			GetModuleFileNameW(module, path, MAX_PATH);
		blog(LOG_INFO, "[recorder] %s: %s", utf8(dll).c_str(), module ? utf8(path).c_str() : "not loaded");
	}
}

// ---------- crashes ----------
//
// What a crash leaves in the log, so it says where it happened and not only that the log stopped: the
// exception, the module and offset it happened at, and the call stack as module+offset (there are no
// symbols here; offsets can be matched up with the build later). The app's window process copies these
// "[crash]" lines into the crash log, next to the app's own crashes (crash.rs). A fail-fast (0xC0000409)
// ends the process without running any of this: then the exit code, which the window's process reads,
// is all there is.

/** One "[crash]" line straight to the log: no lock another thread may be holding, no allocations */
static void crash_line(const char *format, ...)
{
	if (!log_file)
		return;
	char text[512];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	SYSTEMTIME t;
	GetLocalTime(&t);
	char line[600];
	int n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d: [crash] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text);
	if (n <= 0)
		return;
	_fwrite_nolock(line, 1, n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1, log_file);
	_fflush_nolock(log_file);
}

/** The module (DLL or exe) an address is in, and how far into it */
static void module_at(DWORD64 address, char *name, size_t size, DWORD64 *offset)
{
	HMODULE module = nullptr;
	char path[MAX_PATH];
	DWORD n = 0;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module) && module)
		n = GetModuleFileNameA(module, path, MAX_PATH);
	if (!n || n >= MAX_PATH) {
		snprintf(name, size, "?");
		*offset = address;
		return;
	}
	const char *file = path;
	for (DWORD i = 0; i < n; i++)
		if (path[i] == '\\' || path[i] == '/')
			file = path + i + 1;
	snprintf(name, size, "%s", file);
	*offset = address - (DWORD64)module;
}

/** One step up the call stack (x64 unwind data); false at its end, or when the stack can't be read */
static bool unwind(CONTEXT *ctx)
{
	__try {
		DWORD64 base = 0;
		PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx->Rip, &base, nullptr);
		if (fn) {
			void *handler = nullptr;
			DWORD64 frame = 0;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx->Rip, fn, ctx, &handler, &frame, nullptr);
		} else {
			// A function that calls nothing: the return address is on top of the stack
			ctx->Rip = *(DWORD64 *)ctx->Rsp;
			ctx->Rsp += 8;
		}
		return ctx->Rip != 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

static const char *exception_name(DWORD code)
{
	switch (code) {
	case EXCEPTION_ACCESS_VIOLATION:
		return "memory access violation";
	case EXCEPTION_STACK_OVERFLOW:
		return "stack overflow";
	case EXCEPTION_ILLEGAL_INSTRUCTION:
		return "illegal instruction";
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
		return "divide by zero";
	case EXCEPTION_IN_PAGE_ERROR:
		return "couldn't read a file it had mapped";
	case EXCEPTION_BREAKPOINT:
		return "breakpoint";
	case 0xC0000374:
		return "heap corruption";
	case 0xE06D7363:
		return "C++ exception nothing caught";
	default:
		return "unexpected exception";
	}
}

struct Crash {
	EXCEPTION_POINTERS *info;
	DWORD thread;
};

/** The "[crash]" lines for an exception on thread `crash->thread` */
static DWORD WINAPI report_crash(void *param)
{
	const Crash *crash = (const Crash *)param;
	EXCEPTION_POINTERS *info = crash->info;
	DWORD thread = crash->thread;
	const EXCEPTION_RECORD *e = info->ExceptionRecord;
	char module[MAX_PATH];
	DWORD64 offset = 0;
	module_at((DWORD64)e->ExceptionAddress, module, sizeof(module), &offset);
	crash_line("The recorder crashed: exception 0x%08lX (%s) at %s+0x%llx, thread %lu%s", e->ExceptionCode, exception_name(e->ExceptionCode),
		   module, offset, thread, thread == main_thread ? " (main)" : "");
	if (e->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && e->NumberParameters >= 2) {
		ULONG_PTR kind = e->ExceptionInformation[0];
		crash_line("%s address 0x%llx", kind == 0 ? "Reading" : kind == 1 ? "Writing" : "Running code at",
			   (unsigned long long)e->ExceptionInformation[1]);
	}
	CONTEXT ctx = *info->ContextRecord;
	for (int i = 0; i < 32; i++) {
		module_at(ctx.Rip, module, sizeof(module), &offset);
		crash_line("  #%d %s+0x%llx", i, module, offset);
		if (!unwind(&ctx))
			break;
	}
	return 0;
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *info)
{
	static std::atomic<bool> reported{false};
	if (reported.exchange(true))
		return EXCEPTION_CONTINUE_SEARCH;
	Crash crash{info, GetCurrentThreadId()};
	// A stack overflow leaves this thread almost no stack, too little for the report (a few KB): it's
	// made on a new thread with a stack of its own, while this one waits
	HANDLE helper = info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW
				? CreateThread(nullptr, 256 * 1024, report_crash, &crash, 0, nullptr)
				: nullptr;
	if (helper) {
		WaitForSingleObject(helper, 5000);
		CloseHandle(helper);
	} else {
		report_crash(&crash);
	}
	// Windows ends the process as it would have, with the exception as its exit code
	return EXCEPTION_CONTINUE_SEARCH;
}

/** libobs gave up on an error it can't carry on from (bcrash). Its own handler would print it and exit(0). */
static void on_library_crash(const char *format, va_list args, void *)
{
	char message[512];
	vsnprintf(message, sizeof(message), format, args);
	crash_line("The recorder stopped on a fatal error in its capture library: %s", message);
	TerminateProcess(GetCurrentProcess(), 3);
}

// ---------- startup ----------

static std::string utf8(const std::wstring &w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 1)
		WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
	return s;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	SetErrorMode(SEM_FAILCRITICALERRORS);
	// libobs finds its data and plugins relative to the working folder (../../data, ../../obs-plugins)
	wchar_t exe_path[MAX_PATH];
	DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
	if (len > 0 && len < MAX_PATH) {
		wchar_t *slash = wcsrchr(exe_path, L'\\');
		if (slash) {
			*slash = L'\0';
			SetCurrentDirectoryW(exe_path);
		}
	}
	// The install folder (from obs\bin\64bit) and this process's rights, for who may connect (serve)
	install_dir = exe_path;
	for (int up = 0; up < 3; up++)
		install_dir = install_dir.substr(0, install_dir.find_last_of(L'\\') == std::wstring::npos ? 0 : install_dir.find_last_of(L'\\'));
	HANDLE self_token = nullptr;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &self_token)) {
		TOKEN_ELEVATION elevation = {};
		DWORD size = 0;
		running_elevated = GetTokenInformation(self_token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
		CloseHandle(self_token);
	}
	main_thread = GetCurrentThreadId();
	MSG msg;
	PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // create this thread's message queue

	std::wstring pipe_name = L"league-recorder-obs";
	std::wstring config;
	std::wstring log_path;
	int argc = 0;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (int i = 1; i + 1 < argc; i++) {
		if (wcscmp(argv[i], L"--pipe") == 0)
			pipe_name = argv[++i];
		else if (wcscmp(argv[i], L"--config") == 0)
			config = argv[++i];
		else if (wcscmp(argv[i], L"--log") == 0)
			log_path = argv[++i];
	}
	LocalFree(argv);
	if (config.empty()) {
		wchar_t temp[MAX_PATH];
		GetTempPathW(MAX_PATH, temp);
		config = std::wstring(temp) + L"league-recorder-obs";
	}
	CreateDirectoryW(config.c_str(), nullptr);
	// Shared, so the log can be read while the recorder runs. The day file the app passes is added to
	// (the app keeps a week of them); without one, the old single file is started afresh.
	if (!log_path.empty())
		log_file = _wfsopen(log_path.c_str(), L"a", _SH_DENYNO);
	if (!log_file)
		log_file = _wfsopen((config + L"\\recorder.log").c_str(), L"w", _SH_DENYNO);
	base_set_log_handler(log_handler, nullptr);
	base_set_crash_handler(on_library_crash, nullptr);
	SetUnhandledExceptionFilter(on_crash);
	blog(LOG_INFO, "---- recorder started (pid %lu) ----", GetCurrentProcessId());

	std::string plugin_config = utf8(config) + "/plugin_config";
	if (!obs_startup("en-US", plugin_config.c_str(), nullptr)) {
		blog(LOG_ERROR, "[recorder] obs_startup failed");
		return 1;
	}
	obs_set_ui_task_handler(ui_task_handler);

	obs_audio_info oai = {};
	oai.samples_per_sec = 48000;
	oai.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio(&oai)) {
		blog(LOG_ERROR, "[recorder] obs_reset_audio failed");
		obs_shutdown();
		return 1;
	}
	// A placeholder canvas until the first setup arrives (plugins load against a running renderer)
	if (!reset_video(1920, 1080, 1920, 1080, 60)) {
		obs_shutdown();
		return 1;
	}
	obs_load_all_modules();
	obs_log_loaded_modules();
	obs_post_load_modules();
	register_app_audio_source();
	// Again, in case a plugin set its own while loading
	SetUnhandledExceptionFilter(on_crash);
	log_environment();
	obs_add_main_rendered_callback(on_rendered, nullptr);

	std::thread(serve, L"\\\\.\\pipe\\" + pipe_name).detach();
	std::thread(report_levels).detach();
	std::thread(watch).detach();
	blog(LOG_INFO, "[recorder] ready");

	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		if (msg.message == WM_RUN_TASK) {
			auto *t = reinterpret_cast<UiTask *>(msg.lParam);
			t->task(t->param);
			if (t->done)
				SetEvent(t->done);
			delete t;
		} else {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	exiting = true;
	{
		std::lock_guard<std::mutex> lock(obs_lock);
		if (recording_active())
			obs_output_force_stop(rec.output);
		if (buffer_active())
			obs_output_force_stop(rec.replay);
		teardown();
	}
	obs_remove_main_rendered_callback(on_rendered, nullptr);
	obs_enter_graphics();
	gs_texrender_destroy(picture.scaled);
	gs_stagesurface_destroy(picture.stage);
	obs_leave_graphics();
	obs_shutdown();
	blog(LOG_INFO, "[recorder] closed");
	if (log_file)
		fclose(log_file);
	return 0;
}
