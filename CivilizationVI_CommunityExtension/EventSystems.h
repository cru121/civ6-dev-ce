#pragma once
#include "Data.h"
#include "HavokScript.h"
#include <string>

namespace EventSystems {
	// True when a Lua script registered a handler for this processor name (through RegisterProcessor in this session).
	extern bool DoesProcessorExist(const std::string& name);
	// Runs the processor through the game's own dispatcher (GameProcessor::Call). Only integer entries are transferred.
	extern bool CallCustomProcessor(const std::string& name, Data::LuaVariantMap& variantMap);
	// RegisterProcessor(name, fn): same as GameEvents[name].Add(fn), plus remembering the name so hooks can skip processors nobody registered.
	extern int lRegisterProcessor(hks::lua_State* L);
	// ProcessorTest(name, key, value) -> handled, value: test entry point, runs one {key = value} map through the native path.
	extern int lProcessorTest(hks::lua_State* L);
}
