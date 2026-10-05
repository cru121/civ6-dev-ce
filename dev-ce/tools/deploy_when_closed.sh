#!/bin/bash
# Waits until Civ VI has exited, then copies the freshly built Dev CE test mod files into the user's Mods folder.
M="/c/Users/janza/OneDrive/Documents/My Games/Sid Meier's Civilization VI/Mods/DevCE_Test"
SRC=/c/stuff/claude/DLL/dev-ce/mod/DevCE_Test
while tasklist | grep -qi CivilizationVI.exe; do sleep 3; done
sleep 2
cp "$SRC/Binaries/Win64/GameCore_XP2_CE_FinalRelease.dll" "$M/Binaries/Win64/" && cp "$SRC/Scripts/"*.lua "$M/Scripts/" && cp "$SRC/DevCE_Test.modinfo" "$M/" && rm -f "$M/Binaries/Win64/DevBridge.log"
cmp "$SRC/Binaries/Win64/GameCore_XP2_CE_FinalRelease.dll" "$M/Binaries/Win64/GameCore_XP2_CE_FinalRelease.dll" && echo "DEPLOYED identical $(date)"
