// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE. Standalone audio playback for mods: play wav / mp3 / flac / ogg files shipped inside a mod, without a Wwise bank.
#pragma once
#include "HavokScript.h"

namespace AudioPlayer {
	// Registers the global table Audio in a Lua state:
	//   Audio.Play(path [, volume=1 [, loop=false [, pan=0 [, bus="sfx"]]]]) -> id (0 on failure, see Audio.GetLastError())
	//   Audio.Stop(id)  Audio.StopAll()  Audio.Pause(id)  Audio.Resume(id)
	//   Audio.SetVolume(id, v)  Audio.SetMasterVolume(v)  Audio.GetMasterVolume()
	//   Audio.FollowGameVolume(bool)   bus = sfx | music | speech | ambience | none; the game's sliders (SoundOpts.txt) scale each bus
	//   Audio.IsPlaying(id) -> bool   Audio.AddSearchPath(dir)   Audio.GetLastError() -> string
	void RegisterGlobals(hks::lua_State* L);
}
