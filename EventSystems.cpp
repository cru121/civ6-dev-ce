#include "EventSystems.h"
#include "GameProcessor.h"
#include <mutex>
#include <unordered_set>

namespace EventSystems {
	// Names of the processors a script registered. The handlers themselves live in the game's GameEvents table (not here), so no lua_State
	// pointer or registry reference is stored and nothing in this file runs Lua outside the game's own dispatcher.
	static std::mutex namesMutex;
	static std::unordered_set<std::string> registeredNames;

	int lRegisterProcessor(hks::lua_State* L) {
		size_t length;
		const char* name = hks::checklstring(L, 1, &length);

		// stack: [name, fn]  ->  GameEvents[name].Add(fn)
		hks::getfield(L, hks::LUA_GLOBAL, "GameEvents");   // 3
		hks::getfield(L, 3, name);                         // 4: the event object (created on first use by the game)
		hks::getfield(L, 4, "Add");                        // 5
		hks::pushvalue(L, 2);                              // 6: fn
		if (hks::pcall(L, 1, 0, 0) != 0) {
			size_t messageLength;
			const char* message = hks::checklstring(L, -1, &messageLength);
			hks::error(L, "RegisterProcessor('%s') failed: %s", name, message);
			return 0;
		}

		std::lock_guard<std::mutex> lock(namesMutex);
		registeredNames.insert(name);
		return 0;
	}

	bool DoesProcessorExist(const std::string& name) {
		std::lock_guard<std::mutex> lock(namesMutex);
		return registeredNames.find(name) != registeredNames.end();
	}

	bool CallCustomProcessor(const std::string& name, Data::LuaVariantMap& variantMap) {
		return GameProcessor::Call(name, variantMap);
	}

	int lProcessorTest(hks::lua_State* L) {
		size_t length;
		std::string name = hks::checklstring(L, 1, &length);
		std::string key = hks::checklstring(L, 2, &length);
		int value = hks::checkinteger(L, 3);

		Data::LuaVariantMap variantMap;
		variantMap.emplace(key, Data::LuaVariant(value));
		bool handled = GameProcessor::Call(name, variantMap, false);   // we are inside a Lua handler here: the game released the lock for us

		hks::pushboolean(L, handled);
		hks::pushinteger(L, std::get<int>(variantMap.at(key)));
		return 2;
	}
}
