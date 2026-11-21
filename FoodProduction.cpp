// SPDX-License-Identifier: AGPL-3.0-only
#include "FoodProduction.h"
#include "Runtime.h"
#include "DevBridge.h"
#include <cstring>

namespace FoodProduction {
	typedef void* (__fastcall* CalculateYieldFn)(void* city, void* out, int yield, uint32_t kind, bool track);
	typedef void(__fastcall* QueueTurnFn)(void* queue);
	typedef void(__fastcall* DoCityGrowthFn)(void* growth);
	typedef void(__fastcall* AddGrowthFn)(void* growth, const int* amount);
	typedef int* (__fastcall* GrowthModifierFn)(void* growth, int* out);
	typedef uint32_t(__fastcall* CurrentKindFn)(void* queue);
	typedef void(__fastcall* AddStepFn)(void* tracked, const int* amount, const char* locKey);
	typedef void* (__fastcall* EconomicGetFn)();
	typedef int(__fastcall* FoodConsumptionFn)(void* rules, int population);
	typedef uint32_t(__fastcall* MakeHashFn)(const char*);
	typedef const uint8_t* (__fastcall* VariantGetFn)(void* map, uint32_t key);
	typedef void(__fastcall* VectorTidyFn)(void* vector);

	static CalculateYieldFn base_CalculateYield;
	static QueueTurnFn base_QueueTurn;
	static DoCityGrowthFn base_DoCityGrowth;
	static AddGrowthFn base_AddGrowth;
	static GrowthModifierFn GrowthModifier;
	static CurrentKindFn CurrentKind;
	static AddStepFn AddStep;
	static EconomicGetFn EconomicGet;
	static FoodConsumptionFn FoodConsumption;
	static VectorTidyFn VectorTidy;
	static const int* gYieldFood = nullptr;
	static const int* gYieldProduction = nullptr;
	static uint32_t gKey = 0;
	static bool gOk = false;

	constexpr uintptr_t VECTOR_TIDY_OFFSET = 0x73980;   // vector<tuple<TrackedValue, String, uint, int>>::_Tidy (GameAttribute step lists +0x18 / +0x48)

	struct Check { const char* name; uintptr_t rva; uint8_t bytes[16]; size_t n; };
	static const Check kChecks[] = {
		{ "City::Instance::CalculateYield", CALCULATE_YIELD_OFFSET, { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x70, 0x20, 0x55, 0x57, 0x41, 0x54, 0x41 }, 16 },
		// movzx r12d, byte [rbp+0x60] (5th argument: track); ...; mov [rdx+0x14], r12b (out +0x14 = track)
		{ "CalculateYield: track flag (5th argument -> out +0x14)", CALCULATE_YIELD_OFFSET + 0x1e, { 0x44, 0x0f, 0xb6, 0x65, 0x60 }, 5 },
		{ "CalculateYield: out +0x14", CALCULATE_YIELD_OFFSET + 0x39, { 0x44, 0x88, 0x62, 0x14 }, 4 },
		{ "City::Instance::GetYield: CalculateYield(city, out, yield, current kind, track)", 0x133359, { 0xe8, 0x82, 0x9d, 0xfc, 0xff, 0x44, 0x8b, 0xc8, 0x40, 0x88, 0x7c, 0x24, 0x20, 0x44, 0x8b, 0xc5 }, 16 },
		{ "AddProductionSpecificYield: YIELD_PRODUCTION", 0x12f6e7, { 0x44, 0x3b, 0x05, 0x5e, 0xff, 0xa5, 0x00 }, 7 },
		{ "City::BuildQueue::DoTurnActivate", BUILD_QUEUE_TURN_OFFSET, { 0x8b, 0x81, 0xf8, 0x01, 0x00, 0x00, 0x01, 0x81, 0xf4, 0x01, 0x00, 0x00, 0xc7, 0x81, 0xf8, 0x01 }, 16 },
		{ "City::BuildQueue::DoBuilds: queue +0x1e8 = city", 0xfc348, { 0x48, 0x8b, 0x99, 0xe8, 0x01, 0x00, 0x00 }, 7 },
		{ "City::BuildQueue::GetCurrentProductionKind", CURRENT_KIND_OFFSET, { 0x44, 0x8b, 0x81, 0xe0, 0x01, 0x00, 0x00, 0x83, 0xc8, 0xff, 0x45, 0x85, 0xc0, 0x74, 0x64, 0x48 }, 16 },
		{ "GetCurrentProductionKind: unit", 0xfd12a, { 0xb8, 0x18, 0x1e, 0xd1, 0x15, 0xc3 }, 6 },
		{ "City::Growth::DoCityGrowth", DO_CITY_GROWTH_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56 }, 16 },
		// mov rax,[growth]; mov rdi,rcx; mov ebx,[rax+0x268] (growth +0 = city, population +0x268)
		{ "DoCityGrowth: city population", DO_CITY_GROWTH_OFFSET + 0x1e, { 0x48, 0x8b, 0x01, 0x48, 0x8b, 0xf9, 0x8b, 0x98, 0x68, 0x02, 0x00, 0x00 }, 12 },
		{ "DoCityGrowth: YIELD_FOOD", DO_CITY_GROWTH_OFFSET + 0x39, { 0x44, 0x8b, 0x05, 0xa8, 0xc1, 0xa6, 0x00 }, 7 },
		{ "DoCityGrowth: surplus * GetOverallGrowthModifier -> AddGrowth", DO_CITY_GROWTH_OFFSET + 0x60, { 0xe8, 0xfb, 0x12, 0x00, 0x00, 0x48, 0x63, 0xf6, 0x48, 0x63, 0x08, 0x48, 0x0f, 0xaf, 0xf1, 0x48 }, 16 },
		{ "City::Growth::AddGrowth", ADD_GROWTH_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b }, 16 },
		{ "City::Growth::GetOverallGrowthModifier", GROWTH_MODIFIER_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48 }, 16 },
		{ "TrackedValue::AddStep(FixedPoint, const char*)", ADD_STEP_OFFSET, { 0x48, 0x83, 0xec, 0x68, 0x8b, 0x02, 0x0f, 0x57, 0xc0, 0x4c, 0x89, 0x44, 0x24, 0x20, 0x48, 0x8d }, 16 },
		{ "Rules::Economic::Get", ECONOMIC_GET_OFFSET, { 0x48, 0x8b, 0x05, 0x31, 0xd3, 0x81, 0x00, 0xc3 }, 8 },
		{ "Rules::Economic::GetFoodConsumption", FOOD_CONSUMPTION_OFFSET, { 0x48, 0x8b, 0x05, 0x09, 0x9a, 0x81, 0x00, 0x8b, 0x80, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xaf, 0xc2 }, 16 },
		{ "vector<step>::_Tidy", VECTOR_TIDY_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x56, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x19, 0x48, 0x8b, 0xf1 }, 16 },
		{ "Utilities::MakeHash", MAKE_HASH_OFFSET, { 0x48, 0x8b, 0xd1, 0x41, 0x83, 0xc8, 0xff, 0x48 }, 8 },
		{ "Lua City:GetProperty: properties +0x1a0", 0x6c929e, { 0x4c, 0x8d, 0x80, 0xa0, 0x01, 0x00, 0x00 }, 7 },
	};

	// ---- helpers ---------------------------------------------------------------------------------------------------------
	static int CityProperty(void* city, uint32_t key) {
		void* map = (char*)city + CITY_PROPERTIES;
		const uint8_t* v = ((VariantGetFn)(*(void***)map)[0x60 / 8])(map, key);
		if (v == nullptr) return 0;
		const uint16_t type = *(const uint16_t*)v;
		const uint32_t flags = *(const uint32_t*)(v + 0x14);
		const bool isNumber = ((flags >> 29) & 1) == 0 && type <= 0x15 && ((0x30002eu >> type) & 1) != 0;
		return isNumber ? *(const int*)(v + 8) : 0;
	}

	static bool Enabled(void* city) {
		return city != nullptr && CityProperty(city, gKey) >= 1;
	}

	// TrackedValue: value clamped by its optional min / max (as GameAttribute::Value reads it)
	static int Clamped(const char* tv) {
		int v = *(const int*)(tv + TV_VALUE);
		if (*(const bool*)(tv + TV_HAS_MAX) && *(const int*)(tv + TV_MAX) < v) v = *(const int*)(tv + TV_MAX);
		if (*(const bool*)(tv + TV_HAS_MIN) && *(const int*)(tv + TV_MIN) > v) v = *(const int*)(tv + TV_MIN);
		return v;
	}

	// GameAttribute::Value with base b and modifier m (FixedPoint<8>, m in percent * 256): b + b * m / 25600 (truncated)
	static int ValueOf(long long b, long long m) {
		return (int)(b + (b * m) / 25600);
	}

	static int AttributeValue(const char* attr) {
		return ValueOf(Clamped(attr), Clamped(attr + ATTR_MODIFIER));
	}

	// What the citizens eat (FixedPoint<8>)
	static int Consumption(void* city) {
		return FoodConsumption(EconomicGet(), *(const int*)((char*)city + CITY_POPULATION)) * 0x100;
	}

	// The city's food yield as the game computes it (no conversion), FixedPoint<8>
	static int RawFood(void* city) {
		alignas(16) char attr[0x60];
		memset(attr, 0, sizeof(attr));
		base_CalculateYield(city, attr, *gYieldFood, 0xffffffffu, false);
		const int food = AttributeValue(attr);
		VectorTidy(attr + 0x18);
		VectorTidy(attr + ATTR_MODIFIER + 0x18);
		return food;
	}

	// Excess food (food minus what the citizens eat), FixedPoint<8>; 0 when there is none
	static int Excess(void* city) {
		const int surplus = RawFood(city) - Consumption(city);
		return surplus > 0 ? surplus : 0;
	}

	// ---- yields ----------------------------------------------------------------------------------------------------------
	static thread_local int tDepth = 0;

	static void* __fastcall Hook_CalculateYield(void* city, void* out, int yield, uint32_t kind, bool track) {
		void* r = base_CalculateYield(city, out, yield, kind, track);
		if (kind != KIND_UNIT || tDepth > 0 || (yield != *gYieldFood && yield != *gYieldProduction) || !Enabled(city)) return r;
		++tDepth;
		char* attr = (char*)out;
		if (yield == *gYieldProduction) {
			// excess food joins the base, so the production modifiers (city %, % toward units) apply to it
			const int excess = Excess(city);
			if (excess > 0) AddStep(attr, &excess, LOC_PRODUCTION_FROM_FOOD);
		}
		else {
			// food down to exactly what the citizens eat: the smallest base whose value is >= that
			const int target = Consumption(city);
			const int base = Clamped(attr);
			const int mod = Clamped(attr + ATTR_MODIFIER);
			if (AttributeValue(attr) > target && 25600 + mod > 0) {
				long long b = ((long long)target * 25600) / (25600 + mod);
				for (int i = 0; i < 64 && ValueOf(b, mod) < target; ++i) ++b;
				for (int i = 0; i < 64 && ValueOf(b - 1, mod) >= target; ++i) --b;
				const int delta = (int)(b - base);
				if (delta < 0) AddStep(attr, &delta, LOC_FOOD_TO_PRODUCTION);
			}
		}
		--tDepth;
		return r;
	}

	// ---- the turn: build step, then growth -------------------------------------------------------------------------------
	// The city whose food went into a unit in this turn's build step (the build step and the growth step run back to back for
	// one city in City::TurnManager::DoTurnActivate).
	static thread_local void* tConvertedCity = nullptr;
	static thread_local void* tGrowthCity = nullptr;

	static void __fastcall Hook_QueueTurn(void* queue) {
		void* city = *(void**)((char*)queue + QUEUE_CITY);
		tConvertedCity = (Enabled(city) && CurrentKind(queue) == KIND_UNIT && Excess(city) > 0) ? city : nullptr;
		base_QueueTurn(queue);
	}

	static void __fastcall Hook_DoCityGrowth(void* growth) {
		void* city = *(void**)((char*)growth + GROWTH_CITY);
		void* previous = tGrowthCity;
		tGrowthCity = Enabled(city) ? city : nullptr;
		base_DoCityGrowth(growth);
		tGrowthCity = previous;
		if (tConvertedCity == city) tConvertedCity = nullptr;
	}

	// Growth of an enabled city comes from its unconverted food, unless this turn's food went into a unit (then no growth from
	// it). Independent of the current item (the unit may have finished in the build step) and of the city yield cache.
	static void __fastcall Hook_AddGrowth(void* growth, const int* amount) {
		void* city = *(void**)((char*)growth + GROWTH_CITY);
		if (tGrowthCity == nullptr || tGrowthCity != city || amount == nullptr) {
			base_AddGrowth(growth, amount);
			return;
		}
		tGrowthCity = nullptr;   // only DoCityGrowth's own AddGrowth
		const int surplus = RawFood(city) - Consumption(city);
		int growthAmount = surplus;
		if (surplus > 0) {
			if (tConvertedCity == city) growthAmount = 0;
			else {
				int modifier = 0x100;
				GrowthModifier(growth, &modifier);
				growthAmount = (int)(((long long)surplus * modifier) >> 8);
			}
		}
		base_AddGrowth(growth, &growthAmount);
	}

	// ---- install ---------------------------------------------------------------------------------------------------------
	static bool Verify(uintptr_t b) {
		for (const Check& c : kChecks) {
			if (memcmp((void*)(b + c.rva), c.bytes, c.n) != 0) {
				DevBridge::Log("[FoodProduction] %s has unexpected bytes at 0x%llx: food to production is not available", c.name, (unsigned long long)c.rva);
				return false;
			}
		}
		return true;
	}

	template <typename T>
	static bool Hook(uintptr_t b, uintptr_t rva, void* detour, T* original) {
		LPVOID target = (LPVOID)(b + rva);
		if (MH_CreateHook(target, detour, (LPVOID*)original) != MH_OK) return false;
		return MH_EnableHook(target) == MH_OK;
	}

	void Create() {
		if (gOk) return;   // hooks stay installed when Dev CE starts again in the same process
		const uintptr_t b = Runtime::GameCoreAddress;
		if (!Verify(b)) return;
		GrowthModifier = (GrowthModifierFn)(b + GROWTH_MODIFIER_OFFSET);
		CurrentKind = (CurrentKindFn)(b + CURRENT_KIND_OFFSET);
		AddStep = (AddStepFn)(b + ADD_STEP_OFFSET);
		EconomicGet = (EconomicGetFn)(b + ECONOMIC_GET_OFFSET);
		FoodConsumption = (FoodConsumptionFn)(b + FOOD_CONSUMPTION_OFFSET);
		VectorTidy = (VectorTidyFn)(b + VECTOR_TIDY_OFFSET);
		gYieldFood = (const int*)(b + YIELD_FOOD_GLOBAL);
		gYieldProduction = (const int*)(b + YIELD_PRODUCTION_GLOBAL);
		gKey = ((MakeHashFn)(b + MAKE_HASH_OFFSET))(PROPERTY);
		const bool ok = Hook(b, CALCULATE_YIELD_OFFSET, (void*)&Hook_CalculateYield, &base_CalculateYield)
			&& Hook(b, BUILD_QUEUE_TURN_OFFSET, (void*)&Hook_QueueTurn, &base_QueueTurn)
			&& Hook(b, DO_CITY_GROWTH_OFFSET, (void*)&Hook_DoCityGrowth, &base_DoCityGrowth)
			&& Hook(b, ADD_GROWTH_OFFSET, (void*)&Hook_AddGrowth, &base_AddGrowth);
		if (!ok) {
			DevBridge::Log("[FoodProduction] a hook could not be installed: food to production is not available");
			return;
		}
		gOk = true;
		DevBridge::Log("[FoodProduction] hooks installed: city property %s >= 1 turns excess food into unit production", PROPERTY);
	}
}
