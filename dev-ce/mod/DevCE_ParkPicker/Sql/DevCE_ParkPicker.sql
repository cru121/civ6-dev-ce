-- The picker's default policy: world wonders and the Preserve district may be part of a park (an allowed district skips the appeal rule).
-- NOTE: the rows apply to the game's tile rule as a whole, so the vanilla Naturalist action and the AI see them too. Delete a row in your own
-- SQL (after this mod) to take a district out again.
INSERT OR IGNORE INTO NationalPark_ValidDistricts (ParkType, DistrictType) VALUES
	('NATIONAL_PARK_DEFAULT', 'DISTRICT_WONDER'),
	('NATIONAL_PARK_DEFAULT', 'DISTRICT_PRESERVE');
