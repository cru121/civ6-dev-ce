-- Switches the Dev CE feature on for this game (game capability, read when the game starts; INSERT OR IGNORE so several mods can add it).
INSERT OR IGNORE INTO Types (Type, Kind) VALUES ('CAPABILITY_DEVCE_FOOD_PRODUCTION', 'KIND_CAPABILITY');
INSERT OR IGNORE INTO GameCapabilities (GameCapability) VALUES ('CAPABILITY_DEVCE_FOOD_PRODUCTION');

-- Dev CE food to production: test data. Every major civ's cities turn excess food into unit production.

INSERT INTO Types (Type, Kind) VALUES ('MODIFIER_DEVCE_FOOD_TEST_PLAYER_CITIES_ADJUST_PROPERTY', 'KIND_MODIFIER');
INSERT INTO DynamicModifiers (ModifierType, CollectionType, EffectType) VALUES
	('MODIFIER_DEVCE_FOOD_TEST_PLAYER_CITIES_ADJUST_PROPERTY', 'COLLECTION_PLAYER_CITIES', 'EFFECT_ADJUST_CITY_PROPERTY');

INSERT INTO Modifiers (ModifierId, ModifierType) VALUES
	('DEVCE_TEST_FOOD_TO_PRODUCTION', 'MODIFIER_DEVCE_FOOD_TEST_PLAYER_CITIES_ADJUST_PROPERTY');

INSERT INTO ModifierArguments (ModifierId, Name, Value) VALUES
	('DEVCE_TEST_FOOD_TO_PRODUCTION', 'Key', 'DEVCE_FOOD_TO_PRODUCTION'),
	('DEVCE_TEST_FOOD_TO_PRODUCTION', 'Amount', 1);

INSERT INTO PolicyModifiers (PolicyType, ModifierId) VALUES
	('POLICY_DISCIPLINE', 'DEVCE_TEST_FOOD_TO_PRODUCTION');

-- Test convenience: also on from the start for every major civ (the policy row above needs Discipline slotted).
INSERT INTO TraitModifiers (TraitType, ModifierId) VALUES ('TRAIT_LEADER_MAJOR_CIV', 'DEVCE_TEST_FOOD_TO_PRODUCTION');
