# League Recorder: recorder and media tool (GPL)

[League Recorder](https://reallifemoon.lol) by RealLifeMoon records League of Legends games. Two small
programs inside it are built on GPL-licensed code, and their source is here, under the GPL-2.0 licence:

- **`native/recorder`** builds `league-recorder-obs.exe`, the recorder. It uses OBS Studio's core library
  (libobs) and plugins to capture the game and audio and encode the video. It has none of OBS's own
  interface; the app drives it over a local named pipe (the protocol is described at the top of
  `recorder.cpp`).
- **`native/media`** builds `league-recorder-media.exe`, which reads and writes the recordings' audio
  tracks and frames with the FFmpeg libraries that OBS ships.

The rest of League Recorder is a separate program that talks to these two over that pipe and from the
command line; it isn't part of this repository.

## The OBS and FFmpeg parts

League Recorder bundles **OBS Studio 30.1.2, unchanged**: the official Windows release's libobs, its
Direct3D 11 renderer, the FFmpeg libraries and MP4 writer, and the game capture, audio capture and
encoder plugins (NVENC/AMF, Quick Sync, x264). Their source is OBS Studio's own:
[obsproject/obs-studio at 30.1.2](https://github.com/obsproject/obs-studio/tree/30.1.2), and FFmpeg's
(6.1.1 headers, as OBS's release uses): [ffmpeg.org](https://ffmpeg.org/download.html).

## Building

Windows, Node.js 22 or newer, and Visual Studio's C++ build tools.

```
npm run setup    # downloads OBS 30.1.2 (checked against a pinned SHA-256), the libobs headers and the FFmpeg headers
npm run build    # compiles both programs into resources/obs/bin/64bit
```

## Licence

GPL-2.0 (see `LICENSE`), like OBS Studio. If you received League Recorder and want this source in
another form, open an issue here.
