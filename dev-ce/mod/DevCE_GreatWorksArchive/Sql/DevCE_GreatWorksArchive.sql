-- Dev CE Great Works Archive: turns the feature on for this game.
--
-- The native part (relics kept instead of lost, archived works tradable, ownership events) acts only in a game whose database has
-- the capability CAPABILITY_DEVCE_GW_ARCHIVE; with Dev CE alone it passes straight through to the game's own code. The capability
-- must be there when the game starts. INSERT OR IGNORE: any mod may add it.
INSERT OR IGNORE INTO Types (Type, Kind) VALUES ('CAPABILITY_DEVCE_GW_ARCHIVE', 'KIND_CAPABILITY');
INSERT OR IGNORE INTO GameCapabilities (GameCapability) VALUES ('CAPABILITY_DEVCE_GW_ARCHIVE');
