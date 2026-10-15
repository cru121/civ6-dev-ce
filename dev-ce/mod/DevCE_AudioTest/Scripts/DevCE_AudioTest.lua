-- Dev CE Audio API smoke test (gameplay VM). Needs the Dev CE GameCore mod; Audio is a global table it adds.
-- Turn 1: beep.wav. Turn 2: stereo chord (wav, found by scanning mod folders). Turn 3: 75 s tune_ogg.ogg on the music bus.
-- Turn 4: stops it, plays 75 s tune_mp3.mp3 on the music bus. Turn 5: missing file (must only log an error).
-- Turn 6: starts the chord LOOPING (quiet). Turn 7: it must still be playing (a looped 1.2 s sound), then Pause. Turn 8: must be silent, then Resume.
-- Turn 9: must be playing again; StopAll; must be silent.
local function log(s) if DevCE_Log then DevCE_Log("[AudioTest] " .. s) end end

if Audio == nil then
	print("DevCE_AudioTest: Audio API missing (is the Dev CE GameCore mod enabled?)")
	return
end

local turns = 0
local musicId = 0
local function OnTurn(playerID)
	local p = Players[playerID]
	if p == nil or not p:IsHuman() then return end
	turns = turns + 1
	if turns == 1 then
		local id = Audio.Play("DevCE_AudioTest/Audio/beep.wav", 0.6)
		log("turn 1 beep id=" .. tostring(id) .. " err=" .. Audio.GetLastError())
	elseif turns == 2 then
		local id = Audio.Play("Audio/chord_stereo.wav", 0.5, false, -0.5)
		log("turn 2 chord id=" .. tostring(id) .. " playing=" .. tostring(Audio.IsPlaying(id)))
	elseif turns == 3 then
		musicId = Audio.Play("Audio/tune_ogg.ogg", 0.8, false, 0, "music")
		log("turn 3 ogg id=" .. tostring(musicId) .. " err=" .. Audio.GetLastError())
	elseif turns == 4 then
		log("turn 4 ogg still playing=" .. tostring(Audio.IsPlaying(musicId)))
		Audio.Stop(musicId)
		musicId = Audio.Play("Audio/tune_mp3.mp3", 0.8, false, 0, "music")
		log("turn 4 mp3 id=" .. tostring(musicId) .. " err=" .. Audio.GetLastError())
	elseif turns == 5 then
		log("turn 5 mp3 still playing=" .. tostring(Audio.IsPlaying(musicId)))
		log("turn 5 missing file -> id=" .. tostring(Audio.Play("nope.ogg")) .. " err=" .. Audio.GetLastError())
	elseif turns == 6 then
		Audio.StopAll()
		musicId = Audio.Play("Audio/chord_stereo.wav", 0.3, true)
		log("turn 6 loop started id=" .. tostring(musicId))
	elseif turns == 7 then
		log("turn 7 loop playing=" .. tostring(Audio.IsPlaying(musicId)) .. " (expect true) -> Pause")
		Audio.Pause(musicId)
	elseif turns == 8 then
		log("turn 8 paused playing=" .. tostring(Audio.IsPlaying(musicId)) .. " (expect false) -> Resume")
		Audio.Resume(musicId)
	elseif turns == 9 then
		log("turn 9 resumed playing=" .. tostring(Audio.IsPlaying(musicId)) .. " (expect true) -> StopAll")
		Audio.StopAll()
		log("turn 9 after StopAll playing=" .. tostring(Audio.IsPlaying(musicId)) .. " (expect false)")
	end
end

GameEvents.PlayerTurnStarted.Add(OnTurn)
log("registered")
