# miniaudio

- Source: https://github.com/mackron/miniaudio, tag `0.11.21` (2023-11-15)
- File: `miniaudio.h`, unmodified, sha256 `6b2029714f8634c4d7c70cc042f45074e0565766113fc064f20cd27c986be9c9`
- License: public domain (Unlicense) or MIT-0, the user's choice (`LICENSE`)
- Used for: the desktop output layer (`c/audio_out.c`): ALSA / PulseAudio / JACK on Linux, CoreAudio on macOS,
  WASAPI on Windows, each loaded at run time; and its null backend, which plays at real-time pace with no
  device, for the tests.  Its decoders, encoders, node graph, resource manager and engine are compiled out.
