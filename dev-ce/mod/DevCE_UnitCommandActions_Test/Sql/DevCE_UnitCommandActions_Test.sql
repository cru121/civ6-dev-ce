-- Test command: Warriors can use Crassus' action (+60 gold, claim this unowned tile next to your territory) every turn.
INSERT INTO Types (Type, Kind) VALUES ('UNITCOMMAND_DEVCE_TEST_CRASSUS', 'KIND_UNITCOMMAND');
INSERT INTO UnitCommands (CommandType, Description, Help, Icon, VisibleInUI, HoldCycling, CategoryInUI)
	VALUES ('UNITCOMMAND_DEVCE_TEST_CRASSUS', 'LOC_GREAT_PERSON_INDIVIDUAL_MARCUS_LICINIUS_CRASSUS_NAME', 'LOC_GREAT_PERSON_INDIVIDUAL_MARCUS_LICINIUS_CRASSUS_NAME',
	        'ICON_UNITCOMMAND_ACTIVATE_GREAT_PERSON', 1, 1, 'SPECIFIC');
INSERT INTO DevCE_UnitCommandActions (CommandType, GreatPersonIndividualType) VALUES ('UNITCOMMAND_DEVCE_TEST_CRASSUS', 'GREAT_PERSON_INDIVIDUAL_MARCUS_LICINIUS_CRASSUS');
INSERT INTO DevCE_UnitCommandGrants (CommandType, UnitType) VALUES ('UNITCOMMAND_DEVCE_TEST_CRASSUS', 'UNIT_WARRIOR');

-- Test 2: units with the Battlecry promotion can use Colaeus' action (+100 faith, a free copy of the resource under the unit) every turn.
INSERT INTO Types (Type, Kind) VALUES ('UNITCOMMAND_DEVCE_TEST_COLAEUS', 'KIND_UNITCOMMAND');
INSERT INTO UnitCommands (CommandType, Description, Help, Icon, VisibleInUI, HoldCycling, CategoryInUI)
	VALUES ('UNITCOMMAND_DEVCE_TEST_COLAEUS', 'LOC_GREAT_PERSON_INDIVIDUAL_COLAEUS_NAME', 'LOC_GREAT_PERSON_INDIVIDUAL_COLAEUS_NAME',
	        'ICON_UNITCOMMAND_ACTIVATE_GREAT_PERSON', 1, 1, 'SPECIFIC');
INSERT INTO DevCE_UnitCommandActions (CommandType, GreatPersonIndividualType) VALUES ('UNITCOMMAND_DEVCE_TEST_COLAEUS', 'GREAT_PERSON_INDIVIDUAL_COLAEUS');
INSERT INTO DevCE_UnitCommandGrants (CommandType, UnitPromotionType) VALUES ('UNITCOMMAND_DEVCE_TEST_COLAEUS', 'PROMOTION_BATTLECRY');
