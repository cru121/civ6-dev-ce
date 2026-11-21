-- Dev CE unit command actions: a custom unit command whose effect is a great person action.
--
-- 1. Add the command as usual: Types (KIND_UNITCOMMAND) + UnitCommands (VisibleInUI = 1, Icon, Description, Help).
-- 2. Add a "template" great person individual for its effect: GreatPersonIndividuals (requirements: the ActionRequires... columns,
--    text: ActionNameTextOverride / ActionEffectTextOverride) + GreatPersonIndividualActionModifiers (what it does).
--    Give the template a great person class that never appears in the timeline (GreatPersonClasses.AvailableInTimeline = 0),
--    so nobody can recruit it.
-- 3. Bind them here and grant the command to unit types.
CREATE TABLE IF NOT EXISTS DevCE_UnitCommandActions (
	CommandType               TEXT NOT NULL,   -- a UnitCommands row that has no game handler (a new command)
	GreatPersonIndividualType TEXT NOT NULL,   -- the template: its requirements and action modifiers are used
	AISearchRadius            INTEGER NOT NULL DEFAULT 3,   -- AI: a unit moves up to this many plots to a revealed plot where it can use the command (0 = never moves for it)
	PRIMARY KEY (CommandType),
	FOREIGN KEY (CommandType) REFERENCES UnitCommands (CommandType) ON DELETE CASCADE ON UPDATE CASCADE,
	FOREIGN KEY (GreatPersonIndividualType) REFERENCES GreatPersonIndividuals (GreatPersonIndividualType) ON DELETE CASCADE ON UPDATE CASCADE
);

-- Who may use a command. One row per grant; set exactly one of the three columns.
-- UnitType: every unit of that type. UnitPromotionType / UnitAbilityType: any unit that has the promotion / ability (any unit type;
-- an ability counts while the unit has it, e.g. granted by a modifier, and stops counting when it is removed).
CREATE TABLE IF NOT EXISTS DevCE_UnitCommandGrants (
	CommandType       TEXT NOT NULL,
	UnitType          TEXT,
	UnitAbilityType   TEXT,
	UnitPromotionType TEXT,
	FOREIGN KEY (CommandType) REFERENCES DevCE_UnitCommandActions (CommandType) ON DELETE CASCADE ON UPDATE CASCADE,
	FOREIGN KEY (UnitType) REFERENCES Units (UnitType) ON DELETE CASCADE ON UPDATE CASCADE,
	FOREIGN KEY (UnitPromotionType) REFERENCES UnitPromotions (UnitPromotionType) ON DELETE CASCADE ON UPDATE CASCADE,
	FOREIGN KEY (UnitAbilityType) REFERENCES UnitAbilities (UnitAbilityType) ON DELETE CASCADE ON UPDATE CASCADE
);

-- Example (not active):
-- INSERT INTO Types (Type, Kind) VALUES ('UNITCOMMAND_MY_BLESSING', 'KIND_UNITCOMMAND');
-- INSERT INTO UnitCommands (CommandType, Description, Help, Icon, VisibleInUI, HoldCycling, CategoryInUI)
--   VALUES ('UNITCOMMAND_MY_BLESSING', 'LOC_UNITCOMMAND_MY_BLESSING_DESCRIPTION', 'LOC_UNITCOMMAND_MY_BLESSING_HELP', 'ICON_UNITCOMMAND_ACTIVATE_GREAT_PERSON', 1, 1, 'SPECIFIC');
-- INSERT INTO DevCE_UnitCommandActions (CommandType, GreatPersonIndividualType) VALUES ('UNITCOMMAND_MY_BLESSING', 'MY_TEMPLATE_INDIVIDUAL_BLESSING');
-- INSERT INTO DevCE_UnitCommandGrants (CommandType, UnitType) VALUES ('UNITCOMMAND_MY_BLESSING', 'UNIT_WARRIOR');
