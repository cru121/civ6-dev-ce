-- Dev CE city radius: test data.
--
-- The feature is on for a game whose database has the capability CAPABILITY_DEVCE_CITY_RADIUS (any mod may add it; use
-- INSERT OR IGNORE so several can). It must be there when the game starts: a game started without it keeps radius 3.
-- A city's radius is 3 + its property DEVCE_CITY_EXTRA_RINGS (capped at 5). Properties add up across modifiers.

INSERT OR IGNORE INTO Types (Type, Kind) VALUES ('CAPABILITY_DEVCE_CITY_RADIUS', 'KIND_CAPABILITY');
INSERT OR IGNORE INTO GameCapabilities (GameCapability) VALUES ('CAPABILITY_DEVCE_CITY_RADIUS');

INSERT INTO Types (Type, Kind) VALUES ('MODIFIER_DEVCE_RADIUS_TEST_PLAYER_CITIES_ADJUST_PROPERTY', 'KIND_MODIFIER');
INSERT INTO DynamicModifiers (ModifierType, CollectionType, EffectType) VALUES
	('MODIFIER_DEVCE_RADIUS_TEST_PLAYER_CITIES_ADJUST_PROPERTY', 'COLLECTION_PLAYER_CITIES', 'EFFECT_ADJUST_CITY_PROPERTY');

INSERT INTO Modifiers (ModifierId, ModifierType) VALUES
	('DEVCE_TEST_RADIUS_TRAIT', 'MODIFIER_DEVCE_RADIUS_TEST_PLAYER_CITIES_ADJUST_PROPERTY'),
	('DEVCE_TEST_RADIUS_WONDER', 'MODIFIER_DEVCE_RADIUS_TEST_PLAYER_CITIES_ADJUST_PROPERTY');
INSERT INTO ModifierArguments (ModifierId, Name, Value) VALUES
	('DEVCE_TEST_RADIUS_TRAIT', 'Key', 'DEVCE_CITY_EXTRA_RINGS'),
	('DEVCE_TEST_RADIUS_TRAIT', 'Amount', 1),
	('DEVCE_TEST_RADIUS_WONDER', 'Key', 'DEVCE_CITY_EXTRA_RINGS'),
	('DEVCE_TEST_RADIUS_WONDER', 'Amount', 1);

-- every major civ: radius 4
INSERT INTO TraitModifiers (TraitType, ModifierId) VALUES ('TRAIT_LEADER_MAJOR_CIV', 'DEVCE_TEST_RADIUS_TRAIT');
-- the Pyramids: +1 for all the owner's cities (5)
INSERT INTO BuildingModifiers (BuildingType, ModifierId) VALUES ('BUILDING_PYRAMIDS', 'DEVCE_TEST_RADIUS_WONDER');
