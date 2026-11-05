# miniaudio

- Source: https://github.com/mackron/miniaudio, tag `0.11.26` (2026-10-31)
- File: `miniaudio.h`, unmodified, sha256 `6b376c7fbb1352ba1e0e432ab66069b335ea9405e45cb6c30b97ef0f3c9c3e0e`
- License: public domain (Unlicense) or MIT-0, the user's choice (`LICENSE`)
- Used for: the desktop output layer (`c/audio_out.c`): ALSA / PulseAudio / JACK on Linux, CoreAudio on macOS,
  WASAPI on Windows, each loaded at run time; and its null backend, which plays at real-time pace with no
  device, for the tests.  Its decoders, encoders, node graph, resource manager and engine are compiled out.
