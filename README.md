# League Recorder: recorder, media tool and capture plugin (GPL)

[League Recorder](https://reallifemoon.lol) by RealLifeMoon records League of Legends games. Three parts
inside it are built on GPL-licensed code, and their source is here, under the GPL-2.0 licence:

- **`native/recorder`** builds `league-recorder-obs.exe`, the recorder. It uses OBS Studio's core library
  (libobs) and plugins to capture the game and audio and encode the video. It has none of OBS's own
  interface; the app drives it over a local named pipe (the protocol is described at the top of
  `recorder.cpp`).
- **`native/media`** builds `league-recorder-media.exe`, which reads and writes the recordings' audio
  tracks and frames with the FFmpeg libraries that OBS ships.
- **`native/win-capture`** is a patch to OBS Studio 30.1.2's capture plugin (`win-capture.dll`), which
  `scripts/build-native.mjs` builds from OBS's source in place of the bundled one. `win-capture.patch`
  changes three files, each marked "League Recorder:":
  - `deps/ipc-util/ipc-util/pipe-windows.c`: when the thread that reads the game hook's messages is told
    to stop, it cancels its pending read and waits for it, instead of leaving. A message arriving then was
    copied onto the thread's old stack and could crash the recorder.
  - `plugins/win-capture/game-capture.c`: a capture try clears the hook's stop event when it opens it.
    A stop signal left from the try before (the hook only reads that event) ended each try at once, over
    and over, while the game hadn't drawn its first frame.
  - `plugins/win-capture/plugin-main.c`: the compatibility list is only downloaded from obsproject.com
    with `ENABLE_COMPAT_UPDATES` defined, which this build doesn't do.

  The plugin has jansson 2.14 built in (MIT licence), as OBS's own build does; `native/win-capture/jansson`
  holds the two config headers jansson's CMake build would write.

The rest of League Recorder is a separate program that talks to these over that pipe and from the
command line; it isn't part of this repository.

## The OBS and FFmpeg parts

League Recorder bundles **OBS Studio 30.1.2, unchanged except `win-capture.dll`** (above): the official
Windows release's libobs, its Direct3D 11 renderer, the FFmpeg libraries and MP4 writer, and the game
capture hook and helpers, audio capture and encoder plugins (NVENC/AMF, Quick Sync, x264). Their source is
OBS Studio's own: [obsproject/obs-studio at 30.1.2](https://github.com/obsproject/obs-studio/tree/30.1.2),
and FFmpeg's (6.1.1 headers, as OBS's release uses): [ffmpeg.org](https://ffmpeg.org/download.html).
jansson: [akheron/jansson 2.14](https://github.com/akheron/jansson/releases/tag/v2.14).

## Building

Windows, Node.js 22 or newer, Git, and Visual Studio's C++ build tools.

```
npm run setup    # downloads OBS 30.1.2 and its source, the FFmpeg headers and jansson 2.14 (each checked against a pinned SHA-256)
npm run build    # compiles both programs into resources/obs/bin/64bit, and win-capture.dll into resources/obs/obs-plugins/64bit
```

`node scripts/build-native.mjs win-capture` builds only the named parts (`recorder`, `media`, `win-capture`).
The plugin is built with OBS's own compiler settings and `/Brepro`, so building it again gives the same file.

## Licence

GPL-2.0 (see `LICENSE`), like OBS Studio. If you received League Recorder and want this source in
another form, open an issue here.
