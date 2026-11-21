// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>

// New requirement type REQUIREMENT_UNIT_PROPERTY_MATCHES: the unit version of the game's REQUIREMENT_PLOT_PROPERTY_MATCHES.
// Can use Arguments PropertyName, PropertyMinimum, met when subject Units property is above or at least PropertyMinimum. Missing read property as 0.

// A requirement factory of our own is appended to GameEffects::Registry<IRequirementFactory> before the game builds its
// RequirementLibrary (per game). It builds the plot requirement's object (same layout and initialisation: the key is
// Utilities::MakeHash(PropertyName) at +0xb8, the minimum at +0xbc) with a copy of its vtable whose Test reads the unit instead.
// Plot properties re-test their requirements through a game signal; unit properties have none, so Dev CE re-tests the live
// requirements of a unit after Lua Unit:SetProperty, and after the EFFECT_ADJUST_UNIT_PROPERTY effect (deferred to the next
// Context::Update, outside the modifier code).
namespace UnitPropertyRequirement {
	// Addresses in GameCore_XP2_FinalRelease.dll, Steam build 15038592 (same DLL in 15296837).
	constexpr uintptr_t REQUIREMENT_REGISTRY_GET_TYPES_OFFSET = 0x626740;   // Registry<IRequirementFactory>::GetTypes() -> std::vector<IRequirementFactory*>&
	constexpr uintptr_t FACTORY_VECTOR_GROW_OFFSET = 0x308f50;              // the vector growth Register<T> calls when full (vector*, 1)
	constexpr uintptr_t OPERATOR_NEW_OFFSET = 0x990900;                     // the game's operator new(size)
	constexpr uintptr_t SIMPLE_REQUIREMENT_CTOR_OFFSET = 0x92ea60;          // Requirements::SimpleRequirement::SimpleRequirement
	constexpr uintptr_t MESSAGE_HANDLE_CTOR_OFFSET = 0x602980;              // Message::Handle::Handle (empty)
	constexpr uintptr_t ENABLE_SHARED_OFFSET = 0x920340;                    // std::_Do_enable (enable_shared_from_this) (object, &object->weak, control)
	constexpr uintptr_t SIMPLE_REQUIREMENT_INITIALIZE_OFFSET = 0x8ef260;    // SimpleRequirement::Initialize(definition) -> bool (calls OnInitialize)
	constexpr uintptr_t SIMPLE_REQUIREMENT_SET_STATE_OFFSET = 0x9755e0;     // SimpleRequirement::SetState(state, notify)
	constexpr uintptr_t MAKE_HASH_OFFSET = 0x606270;                        // Utilities::MakeHash(const char*)
	constexpr uintptr_t PLOT_FACTORY_VTABLE = 0xa8bba0;                     // RequirementFactory<PlotPropertyMatches> vtable (6 slots)
	constexpr uintptr_t PLOT_FACTORY_CREATE_OFFSET = 0x8813f0;              //   its Create (replicated by ours; checked only)
	constexpr uintptr_t PLOT_REQUIREMENT_VTABLE = 0xa6bdd0;                 // PlotPropertyMatches vtable (23 slots; 22 = Test)
	constexpr uintptr_t PLOT_REQUIREMENT_TEST_OFFSET = 0x8f5060;            //   PlotPropertyMatches::Test (the model for ours)
	constexpr uintptr_t PLOT_REQUIREMENT_CONTROL_VTABLE = 0xa6dc58;         // _Ref_count_obj_alloc<PlotPropertyMatches> vtable (3 slots; 0 = _Destroy)
	constexpr uintptr_t PLOT_REQUIREMENT_CONTROL_DESTROY_OFFSET = 0x8efec0;
	constexpr uintptr_t UNIT_SUBJECT_TYPE_ID = 0xa9a578;                    // 16-byte id a unit requirement passes to subject vtable +0x10 (UnitHasAbility::Test)
	constexpr uintptr_t ADJUST_UNIT_PROPERTY_APPLY_OFFSET = 0x8d9d00;       // Effects::AdjustUnitProperty::Apply(effect, Unit::Instance*) (hooked)
	constexpr uintptr_t ADJUST_UNIT_PROPERTY_REMOVE_OFFSET = 0x8da950;      // Effects::AdjustUnitProperty::Remove(effect, Unit::Instance*) (hooked)
	constexpr uintptr_t LUA_UNIT_SET_PROPERTY_OFFSET = 0x6ff610;            // Lua::IUnit::lSetProperty(L) (hooked)
	constexpr uintptr_t LUA_GET_UNIT_OFFSET = 0x24da0;                      // Lua::ScopedInstance<IUnit, Unit::Instance>::GetInstance(L, index, bool)
	constexpr uintptr_t CONTEXT_UPDATE_OFFSET = 0x591900;                   // Exports::Context::Update(context, bool) (hooked: deferred re-tests)

	// Layout (same build)
	constexpr uintptr_t REQUIREMENT_OBJECT_SIZE = 0xd0;    // control block (0x10) + PlotPropertyMatches (0xc0)
	constexpr uintptr_t REQ_SUBJECT = 0x40;                // the requirement's subject (game effects object)
	constexpr uintptr_t REQ_ACTIVE = 0x1c;                 // bool: activated (only active requirements are re-tested)
	constexpr uintptr_t REQ_PROPERTY_KEY = 0xb8;           // MakeHash(PropertyName)
	constexpr uintptr_t REQ_PROPERTY_MINIMUM = 0xbc;
	constexpr uintptr_t UNIT_PROPERTIES = 0x1f0;           // Unit::Instance: Data::VariantMap (vtable +0x60 = get(key) -> Variant*)

	constexpr const char* TYPE_NAME = "REQUIREMENT_UNIT_PROPERTY_MATCHES";

	// Verifies the addresses, registers the requirement type and installs the hooks (call from InitHooks, before the first game).
	void Create();
	// Re-tests the active unit-property requirements of this unit (Unit::Instance*). For C++ code that wrote a unit property.
	void RefreshUnit(void* unit);
}
