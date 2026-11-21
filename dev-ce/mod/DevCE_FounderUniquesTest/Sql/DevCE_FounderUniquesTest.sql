-- Dev CE founder uniques: test data. Every major civ's cities get DEVCE_BUILD_FOUNDER_UNIQUES = 1 (only cities founded by
-- someone else are affected). A real mod would give it to one civilization through its TraitModifiers, or add requirements.

INSERT INTO Types (Type, Kind) VALUES ('MODIFIER_DEVCE_FOUNDER_TEST_PLAYER_CITIES_ADJUST_PROPERTY', 'KIND_MODIFIER');
INSERT INTO DynamicModifiers (ModifierType, CollectionType, EffectType) VALUES
	('MODIFIER_DEVCE_FOUNDER_TEST_PLAYER_CITIES_ADJUST_PROPERTY', 'COLLECTION_PLAYER_CITIES', 'EFFECT_ADJUST_CITY_PROPERTY');

INSERT INTO Modifiers (ModifierId, ModifierType) VALUES
	('DEVCE_TEST_BUILD_FOUNDER_UNIQUES', 'MODIFIER_DEVCE_FOUNDER_TEST_PLAYER_CITIES_ADJUST_PROPERTY');
INSERT INTO ModifierArguments (ModifierId, Name, Value) VALUES
	('DEVCE_TEST_BUILD_FOUNDER_UNIQUES', 'Key', 'DEVCE_BUILD_FOUNDER_UNIQUES'),
	('DEVCE_TEST_BUILD_FOUNDER_UNIQUES', 'Amount', 1);

INSERT INTO TraitModifiers (TraitType, ModifierId) VALUES
	('TRAIT_LEADER_MAJOR_CIV', 'DEVCE_TEST_BUILD_FOUNDER_UNIQUES');
