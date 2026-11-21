-- Switches the Dev CE feature on for this game (game capability, read when the game starts; INSERT OR IGNORE so several mods can add it).
INSERT OR IGNORE INTO Types (Type, Kind) VALUES ('CAPABILITY_DEVCE_UPGRADE_FALLBACK', 'KIND_CAPABILITY');
INSERT OR IGNORE INTO GameCapabilities (GameCapability) VALUES ('CAPABILITY_DEVCE_UPGRADE_FALLBACK');
