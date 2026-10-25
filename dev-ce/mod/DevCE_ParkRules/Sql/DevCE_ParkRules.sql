-- Dev CE National Park rules. Defaults reproduce the vanilla rule: exactly 4 tiles, appeal 2 or more, mountains and natural wonders always fit,
-- no improvement, no district.
CREATE TABLE IF NOT EXISTS NationalParkTypes (
	ParkType            TEXT    NOT NULL,
	MinTiles            INTEGER NOT NULL DEFAULT 4,      -- used by the custom picker / DesignateCustom callers (1..32)
	MaxTiles            INTEGER NOT NULL DEFAULT 4,
	MinAppeal           INTEGER NOT NULL DEFAULT 2,      -- a tile needs appeal in [MinAppeal, MaxAppeal] unless an exemption below applies
	MaxAppeal           INTEGER NOT NULL DEFAULT 1000000,
	AllowImprovements   BOOLEAN NOT NULL DEFAULT 0,      -- 1 = tiles with an improvement may be part of a park
	AllowMountains      BOOLEAN NOT NULL DEFAULT 1,      -- 1 = mountain tiles fit whatever their appeal
	AllowNaturalWonders BOOLEAN NOT NULL DEFAULT 1,      -- 1 = natural wonder tiles fit whatever their appeal
	PRIMARY KEY (ParkType)
);

-- A tile with one of these districts may be part of a park; an allowed district is an explicit permission, so the appeal rule is skipped for it.
CREATE TABLE IF NOT EXISTS NationalPark_ValidDistricts (
	ParkType     TEXT NOT NULL,
	DistrictType TEXT NOT NULL,
	PRIMARY KEY (ParkType, DistrictType),
	FOREIGN KEY (ParkType) REFERENCES NationalParkTypes (ParkType) ON DELETE CASCADE ON UPDATE CASCADE,
	FOREIGN KEY (DistrictType) REFERENCES Districts (DistrictType) ON DELETE CASCADE ON UPDATE CASCADE
);

INSERT OR IGNORE INTO NationalParkTypes (ParkType) VALUES ('NATIONAL_PARK_DEFAULT');
