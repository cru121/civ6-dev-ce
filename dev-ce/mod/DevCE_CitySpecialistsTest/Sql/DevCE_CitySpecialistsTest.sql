-- Test: city-centre specialists on; five specialist types. Every major civilization's city gets 2 Scientist and 2 Engineer slots
-- and 1 slot of each other type.

UPDATE GlobalParameters SET Value = 1 WHERE Name = 'DEVCE_CITY_CENTER_SPECIALISTS';

INSERT INTO DevCE_Specialists (SpecialistType, Name, Color, SortIndex) VALUES
('SPECIALIST_SCIENTIST', 'Scientist', '170,215,255', 0),
('SPECIALIST_ENGINEER', 'Engineer', '255,140,40', 1),
('SPECIALIST_MERCHANT', 'Merchant', '255,215,60', 2),
('SPECIALIST_ARTIST', 'Artist', '160,80,255', 3),
('SPECIALIST_PRIEST', 'Priest', '255,255,255', 4);

INSERT INTO DevCE_Specialist_Yields (SpecialistType, YieldType, YieldChange) VALUES
('SPECIALIST_SCIENTIST', 'YIELD_SCIENCE', 2),
('SPECIALIST_ENGINEER', 'YIELD_PRODUCTION', 2),
('SPECIALIST_MERCHANT', 'YIELD_GOLD', 3),
('SPECIALIST_ARTIST', 'YIELD_CULTURE', 2),
('SPECIALIST_PRIEST', 'YIELD_FAITH', 2);

-- Slots: city property DEVCE_SPECIALIST_SLOTS_<SpecialistType>, set with the game's EFFECT_ADJUST_CITY_PROPERTY.
INSERT INTO Types (Type, Kind) VALUES
('MODIFIER_DEVCE_TEST_PLAYER_CITIES_ADJUST_PROPERTY', 'KIND_MODIFIER');
INSERT INTO DynamicModifiers (ModifierType, CollectionType, EffectType) VALUES
('MODIFIER_DEVCE_TEST_PLAYER_CITIES_ADJUST_PROPERTY', 'COLLECTION_PLAYER_CITIES', 'EFFECT_ADJUST_CITY_PROPERTY');

INSERT INTO Modifiers (ModifierId, ModifierType)
SELECT 'DEVCE_TEST_SLOT_' || SpecialistType, 'MODIFIER_DEVCE_TEST_PLAYER_CITIES_ADJUST_PROPERTY' FROM DevCE_Specialists;

INSERT INTO ModifierArguments (ModifierId, Name, Value)
SELECT 'DEVCE_TEST_SLOT_' || SpecialistType, 'Key', 'DEVCE_SPECIALIST_SLOTS_' || SpecialistType FROM DevCE_Specialists;
INSERT INTO ModifierArguments (ModifierId, Name, Value)
SELECT 'DEVCE_TEST_SLOT_' || SpecialistType, 'Amount',
	CASE WHEN SpecialistType IN ('SPECIALIST_SCIENTIST', 'SPECIALIST_ENGINEER') THEN '2' ELSE '1' END FROM DevCE_Specialists;

INSERT INTO TraitModifiers (TraitType, ModifierId)
SELECT 'TRAIT_LEADER_MAJOR_CIV', 'DEVCE_TEST_SLOT_' || SpecialistType FROM DevCE_Specialists;

-- Slots that can be taken away again: Urban Planning gives 2 more Scientist slots
INSERT INTO Modifiers (ModifierId, ModifierType) VALUES
('DEVCE_TEST_POLICY_SLOT_SCIENTIST', 'MODIFIER_DEVCE_TEST_PLAYER_CITIES_ADJUST_PROPERTY');
INSERT INTO ModifierArguments (ModifierId, Name, Value) VALUES
('DEVCE_TEST_POLICY_SLOT_SCIENTIST', 'Key', 'DEVCE_SPECIALIST_SLOTS_SPECIALIST_SCIENTIST'),
('DEVCE_TEST_POLICY_SLOT_SCIENTIST', 'Amount', '2');
INSERT INTO PolicyModifiers (PolicyType, ModifierId) VALUES
('POLICY_URBAN_PLANNING', 'DEVCE_TEST_POLICY_SLOT_SCIENTIST');

-- Per-type yield change example: Discipline gives +3 Science per Scientist
INSERT INTO Modifiers (ModifierId, ModifierType) VALUES
('DEVCE_TEST_POLICY_SCIENTIST_SCIENCE', 'MODIFIER_DEVCE_TEST_PLAYER_CITIES_ADJUST_PROPERTY');
INSERT INTO ModifierArguments (ModifierId, Name, Value) VALUES
('DEVCE_TEST_POLICY_SCIENTIST_SCIENCE', 'Key', 'DEVCE_SPECIALIST_YIELD_SPECIALIST_SCIENTIST_YIELD_SCIENCE'),
('DEVCE_TEST_POLICY_SCIENTIST_SCIENCE', 'Amount', '3');
INSERT INTO PolicyModifiers (PolicyType, ModifierId) VALUES
('POLICY_DISCIPLINE', 'DEVCE_TEST_POLICY_SCIENTIST_SCIENCE');

-- Great person points per specialist.
INSERT INTO DevCE_Specialist_GreatPersonPoints (SpecialistType, GreatPersonClassType, PointsPerTurn) VALUES
('SPECIALIST_SCIENTIST', 'GREAT_PERSON_CLASS_SCIENTIST', 1),
('SPECIALIST_ENGINEER', 'GREAT_PERSON_CLASS_ENGINEER', 1),
('SPECIALIST_MERCHANT', 'GREAT_PERSON_CLASS_MERCHANT', 1),
('SPECIALIST_ARTIST', 'GREAT_PERSON_CLASS_ARTIST', 2),
('SPECIALIST_PRIEST', 'GREAT_PERSON_CLASS_PROPHET', 2);
