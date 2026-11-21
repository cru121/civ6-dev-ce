-- Switches the Dev CE feature on for this game (game capability, read when the game starts; INSERT OR IGNORE so several mods can add it).
INSERT OR IGNORE INTO Types (Type, Kind) VALUES ('CAPABILITY_DEVCE_DISTRICT_CITIZENS', 'KIND_CAPABILITY');
INSERT OR IGNORE INTO GameCapabilities (GameCapability) VALUES ('CAPABILITY_DEVCE_DISTRICT_CITIZENS');

-- Test: a policy, God King, makes Campus takes 1 more citizen, and each citizen there gives +1 Science and +1 Gold.
-- (A Campus has no CitizenSlots of its own: before a Library it normally takes no citizens.)
--
-- Dev CE reads two reserved district properties, set with the game's EFFECT_ADJUST_DISTRICT_PROPERTY (Key, Amount):
--   DEVCE_CITIZEN_<YieldType>  (e.g. DEVCE_CITIZEN_YIELD_SCIENCE): each citizen working the district gives Amount more
--   DEVCE_CITIZEN_SLOTS                                                     : the district tile takes Amount more citizens
-- The game has no modifier type for that effect, so a mod using it adds its own (as below).

INSERT INTO Types(Type, Kind) VALUES
('MODIFIER_DEVCE_TEST_PLAYER_DISTRICTS_ADJUST_PROPERTY', 'KIND_MODIFIER');
INSERT INTO DynamicModifiers(ModifierType, CollectionType, EffectType) VALUES
('MODIFIER_DEVCE_TEST_PLAYER_DISTRICTS_ADJUST_PROPERTY', 'COLLECTION_PLAYER_DISTRICTS', 'EFFECT_ADJUST_DISTRICT_PROPERTY');

INSERT INTO RequirementSets(RequirementSetId, RequirementSetType) VALUES
('DEVCE_TEST_DISTRICT_IS_CAMPUS', 'REQUIREMENTSET_TEST_ALL');
INSERT INTO RequirementSetRequirements(RequirementSetId, RequirementId) VALUES
('DEVCE_TEST_DISTRICT_IS_CAMPUS', 'REQUIRES_DISTRICT_IS_CAMPUS');

INSERT INTO Modifiers(ModifierId, ModifierType, SubjectRequirementSetId) VALUES
('DEVCE_TEST_CAMPUS_CITIZEN_SLOT', 'MODIFIER_DEVCE_TEST_PLAYER_DISTRICTS_ADJUST_PROPERTY', 'DEVCE_TEST_DISTRICT_IS_CAMPUS'),
('DEVCE_TEST_CAMPUS_CITIZEN_SCIENCE', 'MODIFIER_DEVCE_TEST_PLAYER_DISTRICTS_ADJUST_PROPERTY', 'DEVCE_TEST_DISTRICT_IS_CAMPUS'),
('DEVCE_TEST_CAMPUS_CITIZEN_GOLD', 'MODIFIER_DEVCE_TEST_PLAYER_DISTRICTS_ADJUST_PROPERTY', 'DEVCE_TEST_DISTRICT_IS_CAMPUS');

INSERT INTO ModifierArguments(ModifierId, Name, Value) VALUES
('DEVCE_TEST_CAMPUS_CITIZEN_SLOT', 'Key', 'DEVCE_CITIZEN_SLOTS'),
('DEVCE_TEST_CAMPUS_CITIZEN_SLOT', 'Amount', '1'),
('DEVCE_TEST_CAMPUS_CITIZEN_SCIENCE', 'Key', 'DEVCE_CITIZEN_YIELD_SCIENCE'),
('DEVCE_TEST_CAMPUS_CITIZEN_SCIENCE', 'Amount', '1'),
('DEVCE_TEST_CAMPUS_CITIZEN_GOLD', 'Key', 'DEVCE_CITIZEN_YIELD_GOLD'),
('DEVCE_TEST_CAMPUS_CITIZEN_GOLD', 'Amount', '1');


INSERT INTO PolicyModifiers(PolicyType, ModifierId) VALUES
('POLICY_GOD_KING', 'DEVCE_TEST_CAMPUS_CITIZEN_SLOT'),
('POLICY_GOD_KING', 'DEVCE_TEST_CAMPUS_CITIZEN_SCIENCE'),
('POLICY_GOD_KING', 'DEVCE_TEST_CAMPUS_CITIZEN_GOLD');


-- Test convenience: the same modifiers also for every major civ from the start (no God King needed), and a cheap Campus
-- that needs no tech, so it can be built on turn 1.
INSERT INTO TraitModifiers(TraitType, ModifierId) VALUES
('TRAIT_LEADER_MAJOR_CIV', 'DEVCE_TEST_CAMPUS_CITIZEN_SLOT'),
('TRAIT_LEADER_MAJOR_CIV', 'DEVCE_TEST_CAMPUS_CITIZEN_SCIENCE'),
('TRAIT_LEADER_MAJOR_CIV', 'DEVCE_TEST_CAMPUS_CITIZEN_GOLD');
UPDATE Districts SET Cost = 1, PrereqTech = NULL WHERE DistrictType = 'DISTRICT_CAMPUS';
