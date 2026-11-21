-- Dev CE tech and civic gates (read by the Dev CE GameCore when the database loads).
--
-- A row adds a condition to a tech or civic, on top of TechnologyPrereqs / CivicPrereqs: the player's property Property must be
-- >= 1. Several rows on one item must all hold. Set the property with MODIFIER_PLAYER_ADJUST_PROPERTY (Key = Property, Amount),
-- which takes the amount off again when the modifier goes (a policy unslotted):
--   policy-locked   PolicyModifiers: the policy adds 1 while slotted
--   cross-tree      CivicModifiers on the civic a tech needs (or TechnologyModifiers on the tech a civic needs)
--   one of several  leave out the prereq rows; each candidate tech adds 1 through TechnologyModifiers
-- Techs / civics whose TechnologyModifiers / CivicModifiers add to the property are the gate's openers: GetResearchPath /
-- GetCivicPath (the tree's "research up to here") go through the cheapest opener in the same tree, and the AI does too. With no
-- opener in that tree (only a civic, a policy...), clicking the item in the tree does nothing while the gate is shut.
-- If the gate of the item being researched shuts, its science / culture goes to overflow and the player chooses again; progress
-- already made stays on the item.
-- Description: optional LOC key, shown in the item's tooltip while the gate is shut (else LOC_DEVCE_GATE_SHUT).

CREATE TABLE IF NOT EXISTS DevCE_TechnologyGates (
	TechnologyType TEXT NOT NULL,
	Property TEXT NOT NULL,
	Description TEXT,
	PRIMARY KEY (TechnologyType, Property),
	FOREIGN KEY (TechnologyType) REFERENCES Technologies(TechnologyType) ON DELETE CASCADE ON UPDATE CASCADE
);

CREATE TABLE IF NOT EXISTS DevCE_CivicGates (
	CivicType TEXT NOT NULL,
	Property TEXT NOT NULL,
	Description TEXT,
	PRIMARY KEY (CivicType, Property),
	FOREIGN KEY (CivicType) REFERENCES Civics(CivicType) ON DELETE CASCADE ON UPDATE CASCADE
);
