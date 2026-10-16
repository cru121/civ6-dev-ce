// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE. See AudioPlayer.h.
#include "AudioPlayer.h"
#include "DevBridge.h"
#include "miniaudio.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <thread>
#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>

// stb_vorbis (stb_vorbis.c, compiled as C): streaming decode from a file
struct stb_vorbis;
struct stb_vorbis_info { unsigned int sample_rate; int channels; unsigned int setup_memory_required; unsigned int setup_temp_memory_required; unsigned int temp_memory_required; int max_frame_size; };
extern "C" {
	stb_vorbis* stb_vorbis_open_filename(const char* filename, int* error, void* alloc);
	stb_vorbis_info stb_vorbis_get_info(stb_vorbis* f);
	unsigned int stb_vorbis_stream_length_in_samples(stb_vorbis* f);
	int stb_vorbis_get_samples_float_interleaved(stb_vorbis* f, int channels, float* buffer, int num_floats);
	int stb_vorbis_seek_start(stb_vorbis* f);
	int stb_vorbis_get_sample_offset(stb_vorbis* f);
	void stb_vorbis_close(stb_vorbis* f);
}

namespace AudioPlayer {
	namespace {
		// miniaudio data source that streams an ogg file through stb_vorbis (the mixing thread calls these)
		struct OggSource {
			ma_data_source_base base;
			stb_vorbis* vorbis = nullptr;
			ma_uint32 channels = 0, rate = 0;
			ma_uint64 length = 0;
		};

		ma_result OggRead(ma_data_source* ds, void* out, ma_uint64 frameCount, ma_uint64* framesRead) {
			OggSource* o = (OggSource*)ds;
			float* dst = (float*)out;
			ma_uint64 total = 0;
			while (total < frameCount) {
				const int want = (int)(std::min<ma_uint64>)(frameCount - total, 4096);
				const int got = stb_vorbis_get_samples_float_interleaved(o->vorbis, (int)o->channels, dst + total * o->channels, want * (int)o->channels);
				if (got <= 0) break;
				total += (ma_uint64)got;
			}
			*framesRead = total;
			return total == 0 ? MA_AT_END : MA_SUCCESS;
		}
		ma_result OggSeek(ma_data_source* ds, ma_uint64 frame) {
			OggSource* o = (OggSource*)ds;
			return (frame == 0 && stb_vorbis_seek_start(o->vorbis)) ? MA_SUCCESS : MA_NOT_IMPLEMENTED;   // only rewind (looping) is needed
		}
		ma_result OggFormat(ma_data_source* ds, ma_format* fmt, ma_uint32* ch, ma_uint32* rate, ma_channel* map, size_t mapCap) {
			OggSource* o = (OggSource*)ds;
			*fmt = ma_format_f32; *ch = o->channels; *rate = o->rate;
			if (map) ma_channel_map_init_standard(ma_standard_channel_map_default, map, mapCap, o->channels);
			return MA_SUCCESS;
		}
		ma_result OggCursor(ma_data_source* ds, ma_uint64* cursor) {
			*cursor = (ma_uint64)(std::max)(0, stb_vorbis_get_sample_offset(((OggSource*)ds)->vorbis));
			return MA_SUCCESS;
		}
		ma_result OggLength(ma_data_source* ds, ma_uint64* length) {
			*length = ((OggSource*)ds)->length;
			return MA_SUCCESS;
		}
		ma_data_source_vtable gOggVtable = { OggRead, OggSeek, OggFormat, OggCursor, OggLength, nullptr, 0 };

		struct Voice {
			ma_sound sound;
			OggSource* ogg = nullptr;   // streamed ogg, otherwise the sound reads the file itself
		};

		enum Bus { BUS_SFX, BUS_MUSIC, BUS_SPEECH, BUS_AMBIENCE, BUS_NONE, BUS_COUNT };
		const char* kBusNames[] = { "sfx", "music", "speech", "ambience", "none" };

		std::mutex gLock;
		ma_engine gEngine;
		ma_sound_group gGroups[BUS_COUNT];   // one per game volume slider; sounds are attached to one of them
		float gGameVol[BUS_COUNT] = { 1, 1, 1, 1, 1 };
		float gGameMaster = 1.0f;
		bool gMuteFocus = true;
		bool gFollowGame = true;
		std::atomic<bool> gFocused{ true };
		bool gThreadStarted = false;
		bool gEngineUp = false;
		bool gEngineFailed = false;
		std::map<int, Voice*> gVoices;
		int gNextId = 1;
		float gMaster = 1.0f;
		std::string gLastError;
		std::vector<std::string> gSearchPaths;

		void Fail(const std::string& msg) {
			gLastError = msg;
			DevBridge::Log("[Audio] %s", msg.c_str());
		}

		std::string DllDir() {
			HMODULE self = nullptr;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&DllDir, &self);
			char p[MAX_PATH] = { 0 };
			GetModuleFileNameA(self, p, MAX_PATH);
			char* slash = strrchr(p, '\\');
			if (slash) *slash = 0;
			return p;
		}

		bool Exists(const std::string& p) {
			DWORD a = GetFileAttributesA(p.c_str());
			return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
		}

		// absolute path as is; otherwise relative to added search paths, the DLL folder, and the folder above it (the mod root when the DLL sits in <mod>\Binaries)
		std::string Resolve(const std::string& path) {
			if (path.size() > 1 && (path[1] == ':' || path[0] == '\\' || path[0] == '/')) return Exists(path) ? path : "";
			std::vector<std::string> bases = gSearchPaths;
			// DLL at <Mods>\<mod>\Binaries\Win64: walk up through Win64, Binaries, the mod root and the Mods folder itself ("OtherMod/Sounds/a.wav" then resolves)
			std::string dir = DllDir();
			std::string modsRoot;
			for (int i = 0; i < 4; i++) {
				bases.push_back(dir);
				const size_t cut = dir.find_last_of('\\');
				if (cut == std::string::npos) break;
				dir = dir.substr(0, cut);
				if (i == 2) modsRoot = dir;   // the Mods folder is one above the mod root
			}
			std::string rel = path;
			std::replace(rel.begin(), rel.end(), '/', '\\');
			for (const auto& b : bases) {
				const std::string full = b + "\\" + rel;
				if (Exists(full)) return full;
			}
			// last resort: the same relative path inside any mod folder ("Audio/click.wav" shipped by whichever mod has it)
			if (!modsRoot.empty()) {
				WIN32_FIND_DATAA fd;
				HANDLE h = FindFirstFileA((modsRoot + "\\*").c_str(), &fd);
				if (h != INVALID_HANDLE_VALUE) {
					do {
						if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
							const std::string full = modsRoot + "\\" + fd.cFileName + "\\" + rel;
							if (Exists(full)) { FindClose(h); return full; }
						}
					} while (FindNextFileA(h, &fd));
					FindClose(h);
				}
			}
			return "";
		}

		// The game keeps its volume sliders (0-100) in %LOCALAPPDATA%\Firaxis Games\Sid Meier's Civilization VI\SoundOpts.txt ("Master Volume 100" ...).
		std::string SoundOptsPath() {
			char base[MAX_PATH] = { 0 };
			if (!GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH)) return "";
			return std::string(base) + "\\Firaxis Games\\Sid Meier's Civilization VI\\SoundOpts.txt";
		}

		void ReadSoundOpts() {   // caller holds gLock
			std::ifstream f(SoundOptsPath());
			if (!f) return;
			std::string line;
			while (std::getline(f, line)) {
				if (line.empty() || line[0] == ';' || line[0] == '[') continue;
				const size_t sp = line.find_last_of(' ');
				if (sp == std::string::npos) continue;
				const std::string key = line.substr(0, sp);
				const int val = atoi(line.c_str() + sp + 1);
				const float f01 = (val < 0 ? 0 : (val > 100 ? 100 : val)) / 100.0f;
				if (key == "Master Volume") gGameMaster = f01;
				else if (key == "SFX Volume") gGameVol[BUS_SFX] = f01;
				else if (key == "Music Volume") gGameVol[BUS_MUSIC] = f01;
				else if (key == "Speech Volume") gGameVol[BUS_SPEECH] = f01;
				else if (key == "Ambience Volume") gGameVol[BUS_AMBIENCE] = f01;
				else if (key == "Mute Focus") gMuteFocus = val != 0;
			}
		}

		void ApplyVolumes() {   // caller holds gLock, engine is up
			for (int b = 0; b < BUS_COUNT; b++) {
				const float v = (gFollowGame && b != BUS_NONE) ? gGameVol[b] : 1.0f;
				ma_sound_group_set_volume(&gGroups[b], v);
			}
			const float master = gFollowGame ? gGameMaster : 1.0f;
			const bool mute = gFollowGame && gMuteFocus && !gFocused.load();
			ma_engine_set_volume(&gEngine, mute ? 0.0f : gMaster * master);
		}

		bool GameHasFocus() {
			HWND fg = GetForegroundWindow();
			if (!fg) return false;
			DWORD pid = 0;
			GetWindowThreadProcessId(fg, &pid);
			return pid == GetCurrentProcessId();
		}

		// polls SoundOpts.txt (sliders can change at any time) and the window focus; sounds follow within ~0.3 s
		void PollThread() {
			FILETIME last = { 0, 0 };
			const std::string path = SoundOptsPath();
			for (;;) {
				std::this_thread::sleep_for(std::chrono::milliseconds(300));
				WIN32_FILE_ATTRIBUTE_DATA fa;
				const bool changed = GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &last) != 0;
				if (changed) last = fa.ftLastWriteTime;
				const bool focus = GameHasFocus();
				std::lock_guard<std::mutex> g(gLock);
				if (!gEngineUp) continue;
				gFocused = focus;
				if (changed) ReadSoundOpts();
				ApplyVolumes();
			}
		}

		bool EnsureEngine() {
			if (gEngineUp) return true;
			if (gEngineFailed) return false;
			ma_engine_config cfg = ma_engine_config_init();
			const ma_result r = ma_engine_init(&cfg, &gEngine);
			if (r != MA_SUCCESS) {
				gEngineFailed = true;
				Fail(std::string("audio engine init failed: ") + ma_result_description(r));
				return false;
			}
			for (int b = 0; b < BUS_COUNT; b++) ma_sound_group_init(&gEngine, 0, nullptr, &gGroups[b]);
			gEngineUp = true;
			ReadSoundOpts();
			gFocused = GameHasFocus();
			ApplyVolumes();
			if (!gThreadStarted) { gThreadStarted = true; std::thread(PollThread).detach(); }
			DevBridge::Log("[Audio] game volumes: master %.2f sfx %.2f music %.2f speech %.2f ambience %.2f, mute on focus loss %d", gGameMaster, gGameVol[BUS_SFX], gGameVol[BUS_MUSIC], gGameVol[BUS_SPEECH], gGameVol[BUS_AMBIENCE], gMuteFocus ? 1 : 0);
			DevBridge::Log("[Audio] engine up: %u Hz, %u channels", ma_engine_get_sample_rate(&gEngine), ma_engine_get_channels(&gEngine));
			return true;
		}

		void Destroy(Voice* v) {
			ma_sound_uninit(&v->sound);   // waits for the mixing thread, so the source is no longer in use afterwards
			if (v->ogg) { ma_data_source_uninit(&v->ogg->base); stb_vorbis_close(v->ogg->vorbis); delete v->ogg; }
			delete v;
		}

		void Reap() {   // drop finished, non-looping voices
			for (auto it = gVoices.begin(); it != gVoices.end();) {
				if (ma_sound_at_end(&it->second->sound)) { Destroy(it->second); it = gVoices.erase(it); }
				else ++it;
			}
		}

		bool HasExt(const std::string& p, const char* ext) {
			const size_t n = strlen(ext);
			return p.size() >= n && _stricmp(p.c_str() + p.size() - n, ext) == 0;
		}

		// length of any file without playing it (seconds), or -1
		double FileDuration(const std::string& full) {
			if (HasExt(full, ".ogg")) {
				int err = 0;
				stb_vorbis* vb = stb_vorbis_open_filename(full.c_str(), &err, nullptr);
				if (!vb) return -1;
				const double d = (double)stb_vorbis_stream_length_in_samples(vb) / (double)stb_vorbis_get_info(vb).sample_rate;
				stb_vorbis_close(vb);
				return d;
			}
			const int wlen = MultiByteToWideChar(CP_ACP, 0, full.c_str(), -1, nullptr, 0);
			std::vector<wchar_t> w(wlen);
			MultiByteToWideChar(CP_ACP, 0, full.c_str(), -1, w.data(), wlen);
			ma_decoder dec;
			ma_decoder_config dc = ma_decoder_config_init_default();
			if (ma_decoder_init_file_w(w.data(), &dc, &dec) != MA_SUCCESS) return -1;
			ma_uint64 frames = 0;
			double d = -1;
			if (ma_decoder_get_length_in_pcm_frames(&dec, &frames) == MA_SUCCESS && frames > 0 && dec.outputSampleRate) d = (double)frames / dec.outputSampleRate;
			ma_decoder_uninit(&dec);
			return d;
		}

		int Play(const std::string& path, float volume, bool loop, float pan, int bus) {
			if (!EnsureEngine()) return 0;
			Reap();
			const std::string full = Resolve(path);
			if (full.empty()) { Fail("file not found: " + path); return 0; }

			Voice* v = new Voice();
			ma_result r;
			if (HasExt(full, ".ogg")) {
				int err = 0;
				stb_vorbis* vb = stb_vorbis_open_filename(full.c_str(), &err, nullptr);
				if (!vb) { Fail("cannot open ogg (stb_vorbis error " + std::to_string(err) + "): " + full); delete v; return 0; }
				const stb_vorbis_info info = stb_vorbis_get_info(vb);
				v->ogg = new OggSource();
				v->ogg->vorbis = vb;
				v->ogg->channels = (ma_uint32)info.channels;
				v->ogg->rate = info.sample_rate;
				v->ogg->length = stb_vorbis_stream_length_in_samples(vb);
				ma_data_source_config dc = ma_data_source_config_init();
				dc.vtable = &gOggVtable;
				r = ma_data_source_init(&dc, &v->ogg->base);
				if (r == MA_SUCCESS) r = ma_sound_init_from_data_source(&gEngine, &v->ogg->base, 0, &gGroups[bus], &v->sound);
				if (r != MA_SUCCESS) { if (v->ogg) { stb_vorbis_close(vb); delete v->ogg; v->ogg = nullptr; } }
			} else {
				// big files stream from disk, small ones are decoded once
				WIN32_FILE_ATTRIBUTE_DATA fa;
				unsigned long long size = 0;
				if (GetFileAttributesExA(full.c_str(), GetFileExInfoStandard, &fa)) size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
				const ma_uint32 flags = size > (2ull << 20) ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
				const int wlen = MultiByteToWideChar(CP_ACP, 0, full.c_str(), -1, nullptr, 0);
				std::vector<wchar_t> w(wlen);
				MultiByteToWideChar(CP_ACP, 0, full.c_str(), -1, w.data(), wlen);
				r = ma_sound_init_from_file_w(&gEngine, w.data(), flags, &gGroups[bus], nullptr, &v->sound);
			}
			if (r != MA_SUCCESS) {
				Fail(std::string("cannot open ") + full + ": " + ma_result_description(r));
				delete v;
				return 0;
			}
			ma_sound_set_volume(&v->sound, volume);
			ma_sound_set_looping(&v->sound, loop ? MA_TRUE : MA_FALSE);
			ma_sound_set_pan(&v->sound, pan);
			ma_sound_start(&v->sound);
			gLastError.clear();
			const int id = gNextId++;
			gVoices[id] = v;
			return id;
		}

		Voice* Find(int id) {
			auto it = gVoices.find(id);
			return it == gVoices.end() ? nullptr : it->second;
		}

		// ---- Lua bindings. The lock also covers calls arriving from another Lua state / thread.
		int lPlay(hks::lua_State* L) {
			unsigned __int64 len = 0;
			const char* p = hks::checklstring(L, 1, &len);
			const float vol = hks::isnumber(L, 2) ? (float)hks::checknumber(L, 2) : 1.0f;
			const bool loop = hks::toboolean(L, 3) != 0;
			const float pan = hks::isnumber(L, 4) ? (float)hks::checknumber(L, 4) : 0.0f;
			int bus = BUS_SFX;
			unsigned __int64 blen = 0;
			if (hks::gettop(L) >= 5) {
				const char* b = hks::checklstring(L, 5, &blen);
				bus = -1;
				for (int i = 0; i < BUS_COUNT && b; i++) if (_stricmp(b, kBusNames[i]) == 0) bus = i;
				if (bus < 0) hks::error(L, "Audio.Play: unknown bus (use sfx, music, speech, ambience or none)");
			}
			std::lock_guard<std::mutex> g(gLock);
			hks::pushinteger(L, Play(p ? p : "", vol, loop, pan, bus));
			return 1;
		}
		int lStop(hks::lua_State* L) {
			const int id = hks::checkinteger(L, 1);
			std::lock_guard<std::mutex> g(gLock);
			auto it = gVoices.find(id);
			if (it != gVoices.end()) { Destroy(it->second); gVoices.erase(it); }
			return 0;
		}
		int lStopAll(hks::lua_State*) {
			std::lock_guard<std::mutex> g(gLock);
			for (auto& kv : gVoices) Destroy(kv.second);
			gVoices.clear();
			return 0;
		}
		int lPause(hks::lua_State* L) {
			const int id = hks::checkinteger(L, 1);
			std::lock_guard<std::mutex> g(gLock);
			if (Voice* v = Find(id)) ma_sound_stop(&v->sound);
			return 0;
		}
		int lResume(hks::lua_State* L) {
			const int id = hks::checkinteger(L, 1);
			std::lock_guard<std::mutex> g(gLock);
			if (Voice* v = Find(id)) ma_sound_start(&v->sound);
			return 0;
		}
		int lSetVolume(hks::lua_State* L) {
			const int id = hks::checkinteger(L, 1);
			const float vol = (float)hks::checknumber(L, 2);
			std::lock_guard<std::mutex> g(gLock);
			if (Voice* v = Find(id)) ma_sound_set_volume(&v->sound, vol);
			return 0;
		}
		int lSetMasterVolume(hks::lua_State* L) {
			const float vol = (float)hks::checknumber(L, 1);
			std::lock_guard<std::mutex> g(gLock);
			gMaster = vol;
			if (gEngineUp) ApplyVolumes();
			return 0;
		}
		// Audio.FollowGameVolume(bool): default true = the game's Master/SFX/Music/Speech/Ambience sliders and "mute on focus loss" apply to these sounds
		int lFollowGameVolume(hks::lua_State* L) {
			std::lock_guard<std::mutex> g(gLock);
			gFollowGame = hks::toboolean(L, 1) != 0;
			if (gEngineUp) ApplyVolumes();
			return 0;
		}
		int lGetMasterVolume(hks::lua_State* L) {
			std::lock_guard<std::mutex> g(gLock);
			hks::pushnumber(L, gMaster);
			return 1;
		}
		int lIsPlaying(hks::lua_State* L) {
			const int id = hks::checkinteger(L, 1);
			std::lock_guard<std::mutex> g(gLock);
			Voice* v = Find(id);
			hks::pushboolean(L, v && ma_sound_is_playing(&v->sound));
			return 1;
		}
		// Audio.GetDuration(path): length in seconds of any sound file (no need to play it); -1 + GetLastError on failure
		int lGetDuration(hks::lua_State* L) {
			unsigned __int64 len = 0;
			const char* p = hks::checklstring(L, 1, &len);
			std::lock_guard<std::mutex> g(gLock);
			const std::string full = Resolve(p ? p : "");
			if (full.empty()) { Fail(std::string("file not found: ") + (p ? p : "")); hks::pushnumber(L, -1); return 1; }
			const double d = FileDuration(full);
			if (d < 0) { Fail("cannot read length of " + full); hks::pushnumber(L, -1); return 1; }
			gLastError.clear();
			hks::pushnumber(L, d);
			return 1;
		}
		// Audio.GetLength(id) / Audio.GetPosition(id): seconds of a playing (or paused) sound; -1 if the id is not known
		int lGetLength(hks::lua_State* L) {
			const int id = hks::checkinteger(L, 1);
			std::lock_guard<std::mutex> g(gLock);
			float sec = 0;
			Voice* v = Find(id);
			if (!v || ma_sound_get_length_in_seconds(&v->sound, &sec) != MA_SUCCESS) { hks::pushnumber(L, -1); return 1; }
			hks::pushnumber(L, sec);
			return 1;
		}
		int lGetPosition(hks::lua_State* L) {
			const int id = hks::checkinteger(L, 1);
			std::lock_guard<std::mutex> g(gLock);
			Voice* v = Find(id);
			if (!v) { hks::pushnumber(L, -1); return 1; }
			float pos = 0; ma_sound_get_cursor_in_seconds(&v->sound, &pos);
			hks::pushnumber(L, pos);
			return 1;
		}
		int lAddSearchPath(hks::lua_State* L) {
			unsigned __int64 len = 0;
			const char* p = hks::checklstring(L, 1, &len);
			std::lock_guard<std::mutex> g(gLock);
			if (p && *p) gSearchPaths.push_back(p);
			return 0;
		}
		int lGetLastError(hks::lua_State* L) {
			std::lock_guard<std::mutex> g(gLock);
			hks::pushfstring(L, "%s", gLastError.c_str());
			return 1;
		}
	}

	void RegisterGlobals(hks::lua_State* L) {
		hks::createtable(L, 0, 16);
		PushLuaMethod(L, lPlay, "lAudioPlay", -2, "Play");
		PushLuaMethod(L, lStop, "lAudioStop", -2, "Stop");
		PushLuaMethod(L, lStopAll, "lAudioStopAll", -2, "StopAll");
		PushLuaMethod(L, lPause, "lAudioPause", -2, "Pause");
		PushLuaMethod(L, lResume, "lAudioResume", -2, "Resume");
		PushLuaMethod(L, lSetVolume, "lAudioSetVolume", -2, "SetVolume");
		PushLuaMethod(L, lSetMasterVolume, "lAudioSetMasterVolume", -2, "SetMasterVolume");
		PushLuaMethod(L, lFollowGameVolume, "lAudioFollowGameVolume", -2, "FollowGameVolume");
		PushLuaMethod(L, lGetMasterVolume, "lAudioGetMasterVolume", -2, "GetMasterVolume");
		PushLuaMethod(L, lIsPlaying, "lAudioIsPlaying", -2, "IsPlaying");
		PushLuaMethod(L, lGetDuration, "lAudioGetDuration", -2, "GetDuration");
		PushLuaMethod(L, lGetLength, "lAudioGetLength", -2, "GetLength");
		PushLuaMethod(L, lGetPosition, "lAudioGetPosition", -2, "GetPosition");
		PushLuaMethod(L, lAddSearchPath, "lAudioAddSearchPath", -2, "AddSearchPath");
		PushLuaMethod(L, lGetLastError, "lAudioGetLastError", -2, "GetLastError");
		hks::setfield(L, hks::LUA_GLOBAL, "Audio");
	}
}
