-- Switches the Dev CE feature on for this game (game capability, read when the game starts; INSERT OR IGNORE so several mods can add it).
INSERT OR IGNORE INTO Types (Type, Kind) VALUES ('CAPABILITY_DEVCE_UPGRADE_RULES', 'KIND_CAPABILITY');
INSERT OR IGNORE INTO GameCapabilities (GameCapability) VALUES ('CAPABILITY_DEVCE_UPGRADE_RULES');

INSERT INTO DevCE_UnitAltUpgrades (Unit,UpgradeUnit) VALUES
('UNIT_WARRIOR', 'UNIT_ARCHER');


-- Test: every major civilization's units can upgrade outside their own territory (Dev CE effect, player-wide modifier)
INSERT INTO Types(Type, Kind) VALUES
('EFFECT_DEVCE_UNIT_UPGRADE_OUTSIDE_TERRITORY', 'KIND_EFFECT');

INSERT INTO Modifiers (ModifierId, ModifierType) VALUES
	('DEVCE_TEST_UPGRADE_OUTSIDE_TERRITORY', 'MODIFIER_DEVCE_PLAYER_UNITS_UPGRADE_OUTSIDE_TERRITORY');
INSERT INTO TraitModifiers (TraitType, ModifierId) VALUES
	('TRAIT_LEADER_MAJOR_CIV', 'DEVCE_TEST_UPGRADE_OUTSIDE_TERRITORY');
