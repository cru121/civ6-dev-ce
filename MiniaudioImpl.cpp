// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE. Single translation unit holding the miniaudio implementation (miniaudio is public domain / MIT-0).
// WASAPI only, no encoding / waveform generation; WAV, MP3 and FLAC decoders are built in. OGG is decoded separately via stb_vorbis (see AudioPlayer.cpp).
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
