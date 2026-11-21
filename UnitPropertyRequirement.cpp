// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#include "UnitPropertyRequirement.h"
#include "Runtime.h"
#include "DevBridge.h"
#include "HavokScript.h"
#include <cstring>
#include <vector>
#include <algorithm>

namespace UnitPropertyRequirement {
	// ---- engine functions -------------------------------------------------------------------------------------------------
	struct FactoryVector { void** begin; void** end; void** capacity; };   // std::vector<IRequirementFactory*> (unique_ptr = one pointer)
	typedef FactoryVector* (__fastcall* GetTypesFn)();
	typedef void(__fastcall* VectorGrowFn)(FactoryVector* v, size_t n);
	typedef void* (__fastcall* OperatorNewFn)(size_t size);
	typedef void* (__fastcall* ThisFn)(void* self);
	typedef void(__fastcall* EnableSharedFn)(void* object, void* weak, void* control);
	typedef bool(__fastcall* InitializeFn)(void* self, void* definition);
	typedef void(__fastcall* SetStateFn)(void* self, int state, bool notify);
	typedef uint32_t(__fastcall* MakeHashFn)(const char* s);
	typedef void* (__fastcall* SubjectQueryFn)(void* subject, const void* typeId, bool flag);
	typedef void* (__fastcall* VariantGetFn)(void* map, uint32_t key);
	typedef bool(__fastcall* EffectUnitFn)(void* effect, void* unit);
	typedef int(__cdecl* LuaFn)(hks::lua_State* L);
	typedef void* (__fastcall* LuaGetUnitFn)(hks::lua_State* L, int index, bool required);
	typedef void(__fastcall* ContextUpdateFn)(void* context, bool flag);

	static GetTypesFn GetTypes;
	static VectorGrowFn VectorGrow;
	static OperatorNewFn OperatorNew;
	static ThisFn SimpleRequirementCtor, HandleCtor, ControlDestroy;
	static EnableSharedFn EnableShared;
	static InitializeFn Initialize;
	static SetStateFn SetState;
	static MakeHashFn MakeHash;
	static LuaGetUnitFn LuaGetUnit;
	static EffectUnitFn base_Apply, base_Remove;
	static LuaFn base_LuaSetProperty;
	static ContextUpdateFn base_ContextUpdate;
	static const void* UnitTypeId;
	static bool gOk = false;

	struct Check { const char* name; uintptr_t rva; uint8_t bytes[12]; size_t n; };
	static const Check kChecks[] = {
		{ "Registry<IRequirementFactory>::GetTypes", REQUIREMENT_REGISTRY_GET_TYPES_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0x65, 0x48, 0x8b, 0x04 }, 8 },
		{ "factory vector growth", FACTORY_VECTOR_GROW_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0x4c, 0x8b, 0x51, 0x10 }, 8 },
		{ "operator new", OPERATOR_NEW_OFFSET, { 0x48, 0x83, 0xc1, 0x0f, 0xba, 0x10, 0x00, 0x00 }, 8 },
		{ "SimpleRequirement::SimpleRequirement", SIMPLE_REQUIREMENT_CTOR_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74 }, 8 },
		{ "Message::Handle::Handle", MESSAGE_HANDLE_CTOR_OFFSET, { 0x33, 0xc0, 0x48, 0x89, 0x01, 0x48, 0x89, 0x41 }, 8 },
		{ "std::_Do_enable", ENABLE_SHARED_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74 }, 8 },
		{ "SimpleRequirement::Initialize", SIMPLE_REQUIREMENT_INITIALIZE_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74 }, 8 },
		{ "SimpleRequirement::SetState", SIMPLE_REQUIREMENT_SET_STATE_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c }, 8 },
		{ "Utilities::MakeHash", MAKE_HASH_OFFSET, { 0x48, 0x8b, 0xd1, 0x41, 0x83, 0xc8, 0xff, 0x48 }, 8 },
		{ "RequirementFactory<PlotPropertyMatches>::Create", PLOT_FACTORY_CREATE_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c }, 8 },
		{ "PlotPropertyMatches::Test", PLOT_REQUIREMENT_TEST_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x0f, 0x10 }, 8 },
		{ "_Ref_count_obj_alloc<PlotPropertyMatches>::_Destroy", PLOT_REQUIREMENT_CONTROL_DESTROY_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74 }, 8 },
		{ "AdjustUnitProperty::Apply", ADJUST_UNIT_PROPERTY_APPLY_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b }, 12 },
		{ "AdjustUnitProperty::Remove", ADJUST_UNIT_PROPERTY_REMOVE_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b }, 12 },
		{ "Lua::IUnit::lSetProperty", LUA_UNIT_SET_PROPERTY_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74 }, 8 },
		{ "ScopedInstance<IUnit>::GetInstance", LUA_GET_UNIT_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c }, 8 },
		{ "Exports::Context::Update", CONTEXT_UPDATE_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0x84, 0xd2, 0x75, 0x05 }, 8 },
	};
	// The vtables we copy: their slots must still point where this build has them.
	struct SlotCheck { uintptr_t vtable; int slot; uintptr_t target; };
	static const SlotCheck kSlotChecks[] = {
		{ PLOT_FACTORY_VTABLE, 0, 0x877a30 }, { PLOT_FACTORY_VTABLE, 1, 0x880090 }, { PLOT_FACTORY_VTABLE, 3, PLOT_FACTORY_CREATE_OFFSET }, { PLOT_FACTORY_VTABLE, 5, 0x882ab0 },
		{ PLOT_REQUIREMENT_VTABLE, 0, 0x7e8870 }, { PLOT_REQUIREMENT_VTABLE, 21, 0x8f4a30 }, { PLOT_REQUIREMENT_VTABLE, 22, PLOT_REQUIREMENT_TEST_OFFSET },
		{ PLOT_REQUIREMENT_CONTROL_VTABLE, 0, PLOT_REQUIREMENT_CONTROL_DESTROY_OFFSET },
	};

	constexpr int FACTORY_SLOTS = 6;
	constexpr int OBJECT_SLOTS = 23;
	constexpr int OBJECT_SLOT_TEST = 22;
	constexpr int CONTROL_SLOTS = 3;
	static void* gFactoryVtable[FACTORY_SLOTS];
	static void* gObjectVtable[OBJECT_SLOTS];
	static void* gControlVtable[CONTROL_SLOTS];
	static uint32_t gTypeHash = 0;

	// ---- live requirement objects (game thread only) -----------------------------------------------------------------------
	static std::vector<char*> gLive;          // our requirement objects that exist
	static std::vector<void*> gDirtyUnits;    // units whose properties a modifier changed: re-test at the next Context::Update
	static bool gInRefresh = false;

	static bool IsLive(char* req) { return std::find(gLive.begin(), gLive.end(), req) != gLive.end(); }
	static void Forget(char* req) {
		auto it = std::find(gLive.begin(), gLive.end(), req);
		if (it != gLive.end()) { *it = gLive.back(); gLive.pop_back(); }
	}

	// ---- the requirement ---------------------------------------------------------------------------------------------------
	// The requirement's unit, as UnitHasAbility::Test finds it: subject vtable +0x10 with the unit type id, owner = result - 8, owner vtable +8.
	// *subjectOk = false when the subject has no unit side at all (the game's "state 3").
	static char* ResolveUnit(char* req, bool* subjectOk) {
		*subjectOk = false;
		void* subject = *(void**)(req + REQ_SUBJECT);
		if (subject == nullptr) return nullptr;
		char* q = (char*)((SubjectQueryFn)(*(void***)subject)[0x10 / 8])(subject, UnitTypeId, true);
		if (q == nullptr || q - 8 == nullptr) return nullptr;
		*subjectOk = true;
		void* owner = q - 8;
		return (char*)((ThisFn)(*(void***)owner)[1])(owner);
	}

	// PlotPropertyMatches::Test with the unit's property map. States as the game's: 0 = met, 1 = not met, 3 = no subject.
	static int TestImpl(char* req) {
		bool subjectOk = false;
		char* unit = ResolveUnit(req, &subjectOk);
		if (!subjectOk) return 3;
		if (unit == nullptr) return 1;
		void* map = unit + UNIT_PROPERTIES;
		const uint8_t* v = (const uint8_t*)((VariantGetFn)(*(void***)map)[0x60 / 8])(map, *(uint32_t*)(req + REQ_PROPERTY_KEY));
		if (v == nullptr) return 1;   // no such property: not met (as for plots)
		const int minimum = *(int*)(req + REQ_PROPERTY_MINIMUM);
		const uint16_t type = *(const uint16_t*)v;
		const uint32_t flags = *(const uint32_t*)(v + 0x14);
		const bool isNumber = ((flags >> 29) & 1) == 0 && type <= 0x15 && ((0x30002eu >> type) & 1) != 0;
		const int value = isNumber ? *(const int*)(v + 8) : 0;
		return value < minimum ? 1 : 0;
	}
	static int __fastcall ObjectTest(char* self) {
		__try {
			return TestImpl(self);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 1;
		}
	}

	// Control block _Destroy (runs the object's destructor in place): forget the object first.
	static void __fastcall ControlDestroyHook(char* control) {
		Forget(control + 0x10);
		ControlDestroy(control);
	}

	// ---- the factory -------------------------------------------------------------------------------------------------------
	static const char* __fastcall FactoryGetName(void* /*self*/) { return TYPE_NAME; }

	// The definition's type must be ours (as every RequirementFactory<T>::ValidateDefinition checks). 0 = valid, 4 = wrong type.
	static int __fastcall FactoryValidate(void* /*self*/, void** definition) {
		void* def = definition != nullptr ? *definition : nullptr;
		if (def == nullptr) return 4;
		const uint32_t type = ((uint32_t(__fastcall*)(void*))(*(void***)def)[0x10 / 8])(def);
		return type == gTypeHash ? 0 : 4;
	}

	static void Release(char* control) {
		if (InterlockedDecrement((volatile long*)(control + 8)) == 0) {
			((ThisFn)(*(void***)control)[0])(control);
			if (InterlockedDecrement((volatile long*)(control + 0xc)) == 0) ((ThisFn)(*(void***)control)[1])(control);
		}
	}

	// RequirementFactory<PlotPropertyMatches>::Create (0x8813f0) step by step, with our object and control vtables.
	// out = std::shared_ptr<IRequirement> { object, control }.
	static void** __fastcall FactoryCreate(void* /*self*/, void** out, void* definition) {
		out[0] = nullptr;
		out[1] = nullptr;
		char* control = (char*)OperatorNew(REQUIREMENT_OBJECT_SIZE);
		if (control == nullptr) return out;
		*(void**)control = gControlVtable;
		*(int*)(control + 8) = 1;     // shared count
		*(int*)(control + 0xc) = 1;   // weak count
		char* object = control + 0x10;
		memset(object, 0, REQUIREMENT_OBJECT_SIZE - 0x10);
		SimpleRequirementCtor(object);
		*(void**)object = gObjectVtable;
		HandleCtor(object + 0x98);
		HandleCtor(object + 0xa8);
		EnableShared(object, object + 8, control);
		gLive.push_back(object);
		if (Initialize(object, definition)) {   // reads PropertyName / PropertyMinimum (the plot requirement's OnInitialize)
			out[0] = object;
			out[1] = control;
		} else {
			Release(control);                   // _Destroy forgets the object
		}
		return out;
	}

	// ---- re-testing ----------------------------------------------------------------------------------------------------------
	static void RetestOne(char* req) {
		__try {
			if (*(uint8_t*)(req + REQ_ACTIVE) == 0) return;
			SetState(req, TestImpl(req), true);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			DevBridge::Log("[UnitPropertyRequirement] FAULT 0x%08lx re-testing a requirement", GetExceptionCode());
		}
	}
	static char* UnitOf(char* req) {
		__try {
			bool subjectOk = false;
			return ResolveUnit(req, &subjectOk);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}

	void RefreshUnit(void* unit) {
		if (!gOk || unit == nullptr) return;
		if (gInRefresh) {   // a state change changed a property again: do it at the next update
			if (std::find(gDirtyUnits.begin(), gDirtyUnits.end(), unit) == gDirtyUnits.end()) gDirtyUnits.push_back(unit);
			return;
		}
		gInRefresh = true;
		const std::vector<char*> snapshot = gLive;   // SetState can create or delete requirements
		for (char* req : snapshot) {
			if (!IsLive(req)) continue;
			if (UnitOf(req) == unit) RetestOne(req);
		}
		gInRefresh = false;
	}

	static void MarkDirty(void* unit) {
		if (unit != nullptr && !gLive.empty() && std::find(gDirtyUnits.begin(), gDirtyUnits.end(), unit) == gDirtyUnits.end()) gDirtyUnits.push_back(unit);
	}

	// EFFECT_ADJUST_UNIT_PROPERTY: re-test later (we are inside the modifier system here).
	static bool __fastcall Hook_Apply(void* effect, void* unit) {
		const bool r = base_Apply(effect, unit);
		MarkDirty(unit);
		return r;
	}
	static bool __fastcall Hook_Remove(void* effect, void* unit) {
		const bool r = base_Remove(effect, unit);
		MarkDirty(unit);
		return r;
	}

	static void* LuaUnitAt1(hks::lua_State* L) {
		__try {
			return LuaGetUnit(L, 1, true);   // as lSetProperty itself asks (it already checked argument 1)
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}
	// Lua Unit:SetProperty: re-test now, as a plot property change does through its signal.
	static int __cdecl Hook_LuaSetProperty(hks::lua_State* L) {
		const int r = base_LuaSetProperty(L);
		if (!gLive.empty()) RefreshUnit(LuaUnitAt1(L));
		return r;
	}

	static void __fastcall Hook_ContextUpdate(void* context, bool flag) {
		base_ContextUpdate(context, flag);
		if (gDirtyUnits.empty() || gInRefresh) return;
		std::vector<void*> units;
		units.swap(gDirtyUnits);
		for (void* unit : units) RefreshUnit(unit);   // compares pointers only: a unit destroyed meanwhile matches nothing
	}

	// ---- setup -------------------------------------------------------------------------------------------------------------
	static bool Verify(uintptr_t b) {
		for (const Check& c : kChecks) {
			if (memcmp((void*)(b + c.rva), c.bytes, c.n) != 0) {
				DevBridge::Log("[UnitPropertyRequirement] %s has unexpected bytes at 0x%llx: %s is not available", c.name, (unsigned long long)c.rva, TYPE_NAME);
				return false;
			}
		}
		for (const SlotCheck& s : kSlotChecks) {
			const uintptr_t target = ((uintptr_t*)(b + s.vtable))[s.slot] - b;
			if (target != s.target) {
				DevBridge::Log("[UnitPropertyRequirement] vtable 0x%llx slot %d points to 0x%llx, expected 0x%llx: %s is not available",
					(unsigned long long)s.vtable, s.slot, (unsigned long long)target, (unsigned long long)s.target, TYPE_NAME);
				return false;
			}
		}
		return true;
	}

	static bool AlreadyRegistered(FactoryVector* v) {
		for (void** it = v->begin; it != v->end; ++it) if (*it != nullptr && *(void**)*it == (void*)gFactoryVtable) return true;
		return false;
	}

	void Create() {
		const uintptr_t b = Runtime::GameCoreAddress;
		if (!Verify(b)) return;
		GetTypes = (GetTypesFn)(b + REQUIREMENT_REGISTRY_GET_TYPES_OFFSET);
		VectorGrow = (VectorGrowFn)(b + FACTORY_VECTOR_GROW_OFFSET);
		OperatorNew = (OperatorNewFn)(b + OPERATOR_NEW_OFFSET);
		SimpleRequirementCtor = (ThisFn)(b + SIMPLE_REQUIREMENT_CTOR_OFFSET);
		HandleCtor = (ThisFn)(b + MESSAGE_HANDLE_CTOR_OFFSET);
		ControlDestroy = (ThisFn)(b + PLOT_REQUIREMENT_CONTROL_DESTROY_OFFSET);
		EnableShared = (EnableSharedFn)(b + ENABLE_SHARED_OFFSET);
		Initialize = (InitializeFn)(b + SIMPLE_REQUIREMENT_INITIALIZE_OFFSET);
		SetState = (SetStateFn)(b + SIMPLE_REQUIREMENT_SET_STATE_OFFSET);
		MakeHash = (MakeHashFn)(b + MAKE_HASH_OFFSET);
		LuaGetUnit = (LuaGetUnitFn)(b + LUA_GET_UNIT_OFFSET);
		UnitTypeId = (const void*)(b + UNIT_SUBJECT_TYPE_ID);
		gTypeHash = MakeHash(TYPE_NAME);

		// Factory: the plot factory's slots, with our name, validation and Create.
		memcpy(gFactoryVtable, (void*)(b + PLOT_FACTORY_VTABLE), sizeof(gFactoryVtable));
		gFactoryVtable[1] = (void*)&FactoryGetName;
		gFactoryVtable[3] = (void*)&FactoryCreate;
		gFactoryVtable[5] = (void*)&FactoryValidate;
		// Object: the plot requirement's slots (initialisation, state, events), with our Test.
		memcpy(gObjectVtable, (void*)(b + PLOT_REQUIREMENT_VTABLE), sizeof(gObjectVtable));
		gObjectVtable[OBJECT_SLOT_TEST] = (void*)&ObjectTest;
		// Control block: forget the object when it is destroyed.
		memcpy(gControlVtable, (void*)(b + PLOT_REQUIREMENT_CONTROL_VTABLE), sizeof(gControlVtable));
		gControlVtable[0] = (void*)&ControlDestroyHook;

		// Register as Registry<IRequirementFactory>::Register<T> does: a heap factory object (the game deletes it through slot 0).
		FactoryVector* types = GetTypes();
		if (!AlreadyRegistered(types)) {
			void** factory = (void**)OperatorNew(8);
			if (factory == nullptr) { DevBridge::Log("[UnitPropertyRequirement] out of memory"); return; }
			*factory = gFactoryVtable;
			if (types->end == types->capacity) VectorGrow(types, 1);
			*types->end = factory;
			types->end++;
		}

		Runtime::CreateHook((LPVOID)(b + ADJUST_UNIT_PROPERTY_APPLY_OFFSET), (LPVOID)&Hook_Apply, &base_Apply);
		Runtime::CreateHook((LPVOID)(b + ADJUST_UNIT_PROPERTY_REMOVE_OFFSET), (LPVOID)&Hook_Remove, &base_Remove);
		Runtime::CreateHook((LPVOID)(b + LUA_UNIT_SET_PROPERTY_OFFSET), (LPVOID)&Hook_LuaSetProperty, &base_LuaSetProperty);
		Runtime::CreateHook((LPVOID)(b + CONTEXT_UPDATE_OFFSET), (LPVOID)&Hook_ContextUpdate, &base_ContextUpdate);
		gOk = true;
		DevBridge::Log("[UnitPropertyRequirement] %s registered (%d requirement types), hooks installed", TYPE_NAME, (int)(types->end - types->begin));
	}
}
