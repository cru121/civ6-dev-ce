// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#include "GameProcessor.h"
#include "Runtime.h"
#include "DevBridge.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <variant>
#include <vector>

namespace GameProcessor {
	// Offsets for installed build 15038592 (GameCore_XP2_FinalRelease.dll). Names are from the leaked-symbol build, mapped to this build.
	// Globals.
	constexpr uintptr_t SCRIPT_SYSTEM_OFFSET = 0xb8aa88;          // GameCore::Context::Globals::ms_pkScriptSystem (IScriptSystem1*)
	constexpr uintptr_t ENGINE_UTILITY_OFFSET = 0xb8aa80;         // GameCore::Context::Globals::ms_pkEngineUtility (IEngineUtility1*), vtable +0x30 = HasGameCoreLock
	constexpr uintptr_t DEFAULT_TYPED_MANAGER_OFFSET = 0xb90770;  // Data::ms_DefaultTypedManager
	// Functions.
	constexpr uintptr_t CALL_PROCESSOR_OFFSET = 0x24530;          // GameCore::Lua::Utility::CallProcessor(IScriptSystem1*, const char*, IScriptSystemArgs1*, TypedVariantMap&)
	constexpr uintptr_t ARGS_HANDLE_CTOR_OFFSET = 0x5eda60;       // GameCore::Lua::Args::Handle::Handle()
	constexpr uintptr_t ARGS_HANDLE_DTOR_OFFSET = 0x5eda90;       // GameCore::Lua::Args::Handle::~Handle()
	constexpr uintptr_t VARIANT_MAP_CTOR_OFFSET = 0x99e450;       // Data::TypedVariantMap::TypedVariantMap(TypedVariantManager*)
	constexpr uintptr_t VARIANT_MAP_DTOR_OFFSET = 0x99e5d0;       // Data::TypedVariantMap::~TypedVariantMap()
	constexpr uintptr_t VARIANT_MAP_SET_OFFSET = 0x99fc60;        // Data::TypedVariantMap::SetVariant(uint key, TypedVariant&& value)
	constexpr uintptr_t VARIANT_MAP_FIND_OFFSET = 0x99f570;       // Data::TypedVariantMap::FindVariant(uint key) -> TypedVariant* or null
	constexpr uintptr_t REGISTER_KEY_OFFSET = 0x9a3f10;           // Data::VariantManager::RegisterKey(const char* name) -> uint key (CRC of the name)

	namespace Types {
		typedef bool(__cdecl* CallProcessor)(void* scriptSystem, const char* name, void* args, void* variantMap);
		typedef void* (__fastcall* ArgsHandleCtor)(void* handle);
		typedef void(__fastcall* ArgsHandleDtor)(void* handle);
		typedef void* (__fastcall* VariantMapCtor)(void* map, void* manager);
		typedef void(__fastcall* VariantMapDtor)(void* map);
		typedef void* (__fastcall* VariantMapSet)(void* map, unsigned int key, void* variant);
		typedef void* (__fastcall* VariantMapFind)(void* map, unsigned int key);
		typedef unsigned int(__fastcall* RegisterKey)(void* manager, const char* name);
		typedef char(__fastcall* HasGameCoreLock)(void* engineUtility);
	}

	// Data::TypedVariant is 24 bytes: u16 value type, u16 modifiers, 4 bytes padding, then the 16-byte value (leaf values at +8). Value types from the Linux DWARF
	// (Data::VariantValueType): 1 Bool, 2 Int32, 3 UInt32, 4 Float, 20 Int64, 21 UInt64.
	struct TypedVariant {
		uint16_t type;
		uint16_t modifiers;
		uint32_t pad;
		union {
			int32_t i32;
			uint32_t u32;
			float f32;
			int64_t i64;
			uint8_t raw[16];
		} value;
	};
	static_assert(sizeof(TypedVariant) == 24, "TypedVariant layout");
	constexpr uint16_t TYPE_BOOL = 1, TYPE_INT32 = 2, TYPE_UINT32 = 3, TYPE_FLOAT = 4, TYPE_INT64 = 20, TYPE_UINT64 = 21;

	static Types::CallProcessor CallProcessor;
	static Types::ArgsHandleCtor ArgsHandleCtor;
	static Types::ArgsHandleDtor ArgsHandleDtor;
	static Types::VariantMapCtor VariantMapCtor;
	static Types::VariantMapDtor VariantMapDtor;
	static Types::VariantMapSet VariantMapSet;
	static Types::VariantMapFind VariantMapFind;
	static Types::RegisterKey RegisterKey;
	static bool created = false;

	void Create() {
		using namespace Runtime;
		CallProcessor = GetGameCoreGlobalAt<Types::CallProcessor>(CALL_PROCESSOR_OFFSET);
		ArgsHandleCtor = GetGameCoreGlobalAt<Types::ArgsHandleCtor>(ARGS_HANDLE_CTOR_OFFSET);
		ArgsHandleDtor = GetGameCoreGlobalAt<Types::ArgsHandleDtor>(ARGS_HANDLE_DTOR_OFFSET);
		VariantMapCtor = GetGameCoreGlobalAt<Types::VariantMapCtor>(VARIANT_MAP_CTOR_OFFSET);
		VariantMapDtor = GetGameCoreGlobalAt<Types::VariantMapDtor>(VARIANT_MAP_DTOR_OFFSET);
		VariantMapSet = GetGameCoreGlobalAt<Types::VariantMapSet>(VARIANT_MAP_SET_OFFSET);
		VariantMapFind = GetGameCoreGlobalAt<Types::VariantMapFind>(VARIANT_MAP_FIND_OFFSET);
		RegisterKey = GetGameCoreGlobalAt<Types::RegisterKey>(REGISTER_KEY_OFFSET);
		created = true;
	}

	bool HasGameCoreLock() {
		void* engineUtility = *Runtime::GetGameCoreGlobalAt<void**>(ENGINE_UTILITY_OFFSET);
		if (engineUtility == nullptr) return false;
		void** vtable = *reinterpret_cast<void***>(engineUtility);
		return reinterpret_cast<Types::HasGameCoreLock>(vtable[0x30 / sizeof(void*)])(engineUtility) != 0;
	}

	// Diagnostics go to DevBridge.log (first 60 lines per game process, to keep the file small).
	static void Log(const char* format, ...) {
		static int lines = 0;
		if (lines >= 60) return;
		lines++;
		char buf[512];
		va_list args;
		va_start(args, format);
		vsnprintf(buf, sizeof(buf), format, args);
		va_end(args);
		DevBridge::Log("[GameProcessor] %s", buf);
	}

	bool Call(const std::string& name, Data::LuaVariantMap& variantMap, bool requireGameCoreLock) {
		if (!created) return false;

		void* scriptSystem = *Runtime::GetGameCoreGlobalAt<void**>(SCRIPT_SYSTEM_OFFSET);
		if (scriptSystem == nullptr) return false;

		bool hasLock = HasGameCoreLock();
		if (requireGameCoreLock && !hasLock) {
			Log("refused '%s': thread %lu does not hold the GameCore lock", name.c_str(), GetCurrentThreadId());
			return false;
		}

		void* defaultManager = Runtime::GetGameCoreGlobalAt<void*>(DEFAULT_TYPED_MANAGER_OFFSET);

		alignas(16) unsigned char map[0x100];   // Data::TypedVariantMap is 0x60 bytes in the game's own stack frames; keep generous room
		std::memset(map, 0, sizeof(map));
		VariantMapCtor(map, defaultManager);
		void* manager = *reinterpret_cast<void**>(map + 0x20);   // the manager the map ended up with (VariantContainer::m_pManager)
		if (manager == nullptr) manager = defaultManager;

		std::vector<std::pair<Data::LuaVariant*, unsigned int>> sent;
		for (auto& pair : variantMap) {
			if (!std::holds_alternative<int>(pair.second)) continue;
			unsigned int key = RegisterKey(manager, pair.first.c_str());
			TypedVariant variant;
			std::memset(&variant, 0, sizeof(variant));
			variant.type = TYPE_INT32;
			variant.value.i32 = std::get<int>(pair.second);
			VariantMapSet(map, key, &variant);
			sent.emplace_back(&pair.second, key);
		}

		alignas(16) unsigned char handle[16];   // Lua::Args::Handle holds one IScriptSystemArgs1*
		std::memset(handle, 0, sizeof(handle));
		ArgsHandleCtor(handle);

		bool handled = CallProcessor(scriptSystem, name.c_str(), *reinterpret_cast<void**>(handle), map);

		if (handled) {
			for (auto& entry : sent) {
				const TypedVariant* result = static_cast<const TypedVariant*>(VariantMapFind(map, entry.second));
				if (result == nullptr || (result->value.raw[15] & 0x20) /* bit 29 of the dword at struct +0x14: the game skips such variants */) continue;
				switch (result->type) {
				case TYPE_BOOL:   *entry.first = Data::LuaVariant(int(result->value.raw[0] != 0)); break;
				case TYPE_INT32:  *entry.first = Data::LuaVariant(int(result->value.i32)); break;
				case TYPE_UINT32: *entry.first = Data::LuaVariant(int(result->value.u32)); break;
				case TYPE_FLOAT:  *entry.first = Data::LuaVariant(int(std::lround(result->value.f32))); break;
				case TYPE_INT64:
				case TYPE_UINT64: *entry.first = Data::LuaVariant(int(result->value.i64)); break;
				default: break;   // handler wrote something we do not understand: keep the value we sent
				}
			}
		}

		Log("'%s' thread %lu lock=%d sent=%d handled=%d", name.c_str(), GetCurrentThreadId(), hasLock ? 1 : 0, int(sent.size()), handled ? 1 : 0);

		ArgsHandleDtor(handle);
		VariantMapDtor(map);
		return handled;
	}
}
