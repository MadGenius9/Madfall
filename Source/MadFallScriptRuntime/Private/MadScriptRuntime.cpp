// Copyright MadFall. All Rights Reserved.

#include "MadScriptRuntime.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// C++ linkage: Lua is compiled as C++ (MadLua.cpp).
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/**
 * One mod's Lua state and everything the runtime tracks about it.
 *
 * Declared here, not in the header, so no Lua type escapes this file.
 */
struct FMadScriptSandbox
{
	FMadScriptRuntime* Runtime = nullptr;
	FName Id;
	lua_State* L = nullptr;

	int64 Memory = 0;

	bool bInCall = false;
	bool bOverBudget = false;
	double CallStart = 0.0;
	int32 EditsThisCall = 0;
	int32 SpawnsThisCall = 0;

	int32 ScriptsLoaded = 0;
	int32 Errors = 0;
	bool bDisabled = false;
	FString LastError;
	double WorstCallMs = 0.0;

	/** Event name -> registry references to handler functions, in registration order. */
	TMap<FName, TArray<int32>> Handlers;

	/** Command name -> registry reference. */
	TMap<FString, int32> Commands;
	TArray<IConsoleObject*> ConsoleObjects;

	TMap<FString, FMadScriptValue> Store;

	~FMadScriptSandbox()
	{
		for (IConsoleObject* Object : ConsoleObjects)
		{
			IConsoleManager::Get().UnregisterConsoleObject(Object, /*bKeepState*/ false);
		}
		if (L != nullptr)
		{
			// lua_close frees through LuaAlloc, which reads this object: still alive here.
			lua_close(L);
		}
	}
};

namespace
{
	// --- plumbing -------------------------------------------------------------------------

	FMadScriptSandbox* SandboxOf(lua_State* L)
	{
		// Coroutines copy the main thread's extra space, so this works from any of a mod's threads.
		return *static_cast<FMadScriptSandbox**>(lua_getextraspace(L));
	}

	/** The memory cap: an allocation that would pass it fails, and Lua raises "not enough memory". */
	void* LuaAlloc(void* UserData, void* Ptr, size_t OldSize, size_t NewSize)
	{
		FMadScriptSandbox* Sandbox = static_cast<FMadScriptSandbox*>(UserData);
		// For a new block Lua passes the object's type in OldSize, not a size.
		const int64 Old = Ptr != nullptr ? static_cast<int64>(OldSize) : 0;
		if (NewSize == 0)
		{
			if (Ptr != nullptr)
			{
				FMemory::Free(Ptr);
				Sandbox->Memory -= Old;
			}
			return nullptr;
		}
		if (Sandbox->Memory - Old + static_cast<int64>(NewSize) > FMadScriptRuntime::MemoryLimitBytes)
		{
			return nullptr;
		}
		void* Block = FMemory::Realloc(Ptr, NewSize);
		if (Block != nullptr)
		{
			Sandbox->Memory += static_cast<int64>(NewSize) - Old;
		}
		return Block;
	}

	constexpr int HookInstructions = 1000;

	/**
	 * The time cap: every 1000 instructions, stop a call that has run too long.
	 *
	 * A script can catch that error with pcall and carry on, so once a call is
	 * over budget the hook moves to every instruction and raises again each time
	 * until the call has unwound: the first instruction outside the pcall fails
	 * too. ProtectedCall restores the normal count afterwards.
	 */
	void LuaHook(lua_State* L, lua_Debug*)
	{
		FMadScriptSandbox* Sandbox = SandboxOf(L);
		if (!Sandbox->bInCall)
		{
			return;
		}
		if (!Sandbox->bOverBudget && (FPlatformTime::Seconds() - Sandbox->CallStart) * 1000.0 > FMadScriptRuntime::CallBudgetMs)
		{
			Sandbox->bOverBudget = true;
		}
		// Hooks belong to each coroutine, so every thread that runs is adjusted as it runs.
		const int WantedCount = Sandbox->bOverBudget ? 1 : HookInstructions;
		if (lua_gethookcount(L) != WantedCount)
		{
			lua_sethook(L, LuaHook, LUA_MASKCOUNT, WantedCount);
		}
		if (Sandbox->bOverBudget)
		{
			luaL_error(L, "took longer than %d ms and was stopped", static_cast<int>(FMadScriptRuntime::CallBudgetMs));
		}
	}

	int LuaTraceback(lua_State* L)
	{
		const char* Message = lua_tostring(L, 1);
		luaL_traceback(L, L, Message ? Message : "(error object is not a string)", 1);
		return 1;
	}

	/**
	 * Calls the function below NumArgs arguments on the stack, protected, timed
	 * and with a traceback. Reports failure to the host and counts it.
	 */
	bool ProtectedCall(FMadScriptSandbox& Sandbox, int NumArgs, const TCHAR* What)
	{
		lua_State* L = Sandbox.L;
		const int Function = lua_gettop(L) - NumArgs;
		lua_pushcfunction(L, LuaTraceback);
		lua_insert(L, Function);

		Sandbox.bInCall = true;
		Sandbox.bOverBudget = false;
		Sandbox.CallStart = FPlatformTime::Seconds();
		Sandbox.EditsThisCall = 0;
		Sandbox.SpawnsThisCall = 0;
		const int Status = lua_pcall(L, NumArgs, 0, Function);
		Sandbox.bInCall = false;
		if (Sandbox.bOverBudget)
		{
			Sandbox.bOverBudget = false;
			lua_sethook(L, LuaHook, LUA_MASKCOUNT, HookInstructions);
		}
		Sandbox.WorstCallMs = FMath::Max(Sandbox.WorstCallMs, (FPlatformTime::Seconds() - Sandbox.CallStart) * 1000.0);

		if (Status != LUA_OK)
		{
			const char* Message = lua_tostring(L, -1);
			Sandbox.LastError = FString::Printf(TEXT("%s: %s"), What, Message ? UTF8_TO_TCHAR(Message) : TEXT("error"));
			lua_pop(L, 1);
			++Sandbox.Errors;
			Sandbox.Runtime->GetHost().Warn(Sandbox.Id, Sandbox.LastError);
			if (Sandbox.Errors >= FMadScriptRuntime::MaxErrors && !Sandbox.bDisabled)
			{
				Sandbox.bDisabled = true;
				Sandbox.Runtime->GetHost().Warn(Sandbox.Id, FString::Printf(TEXT("disabled after %d errors; fix the script and restart"), Sandbox.Errors));
			}
			// A failure may have been a memory error: give the collector a chance.
			lua_gc(L, LUA_GCCOLLECT);
		}
		lua_remove(L, Function);
		return Status == LUA_OK;
	}

	void PushValue(lua_State* L, const FMadScriptValue& Value)
	{
		switch (Value.Type)
		{
		case FMadScriptValue::EType::Bool:   lua_pushboolean(L, Value.Bool ? 1 : 0); break;
		case FMadScriptValue::EType::Number:
			// Whole numbers as Lua integers, so "day " .. 3 reads "day 3", not "day 3.0".
			if (FMath::IsFinite(Value.Number) && FMath::Abs(Value.Number) < 9.0e15 && FMath::Frac(Value.Number) == 0.0)
			{
				lua_pushinteger(L, static_cast<lua_Integer>(Value.Number));
			}
			else
			{
				lua_pushnumber(L, Value.Number);
			}
			break;
		case FMadScriptValue::EType::String: lua_pushstring(L, TCHAR_TO_UTF8(*Value.String)); break;
		default:                             lua_pushnil(L); break;
		}
	}

	// --- the madfall table ----------------------------------------------------------------
	//
	// Lua errors unwind with longjmp, which skips C++ destructors. Every binding
	// reads and checks its arguments first (luaL_check* may raise), and raises
	// errors only when no FString or container of its own is alive.

	int L_Log(lua_State* L)
	{
		const int Count = lua_gettop(L);
		for (int Index = 1; Index <= Count; ++Index)
		{
			luaL_tolstring(L, Index, nullptr);
			if (Index < Count)
			{
				lua_pushliteral(L, " ");
			}
		}
		lua_concat(L, FMath::Max(0, Count * 2 - 1));
		const char* Text = lua_tostring(L, -1);
		FMadScriptSandbox* Sandbox = SandboxOf(L);
		Sandbox->Runtime->GetHost().Log(Sandbox->Id, UTF8_TO_TCHAR(Text ? Text : ""));
		return 0;
	}

	int L_Message(lua_State* L)
	{
		const char* Text = luaL_checkstring(L, 1);
		FMadScriptSandbox* Sandbox = SandboxOf(L);
		Sandbox->Runtime->GetHost().ShowMessage(Sandbox->Id, UTF8_TO_TCHAR(Text));
		return 0;
	}

	int L_On(lua_State* L)
	{
		const char* EventText = luaL_checkstring(L, 1);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		const FName Event(EventText);
		if (!FMadScriptRuntime::GetEventNames().Contains(Event))
		{
			return luaL_error(L, "madfall.on: unknown event '%s'", EventText);
		}
		lua_pushvalue(L, 2);
		const int32 Ref = luaL_ref(L, LUA_REGISTRYINDEX);
		SandboxOf(L)->Handlers.FindOrAdd(Event).Add(Ref);
		return 0;
	}

	int L_Command(lua_State* L)
	{
		const char* NameText = luaL_checkstring(L, 1);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		const size_t Length = strlen(NameText);
		bool bValid = Length > 0 && Length <= 32;
		for (size_t Index = 0; Index < Length && bValid; ++Index)
		{
			const char C = NameText[Index];
			bValid = (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '_';
		}
		if (!bValid)
		{
			return luaL_error(L, "madfall.command: '%s' must be 1-32 characters of a-z, 0-9 and _", NameText);
		}
		lua_pushvalue(L, 2);
		const int32 Ref = luaL_ref(L, LUA_REGISTRYINDEX);

		FMadScriptSandbox* Sandbox = SandboxOf(L);
		const FString Name = UTF8_TO_TCHAR(NameText);
		if (int32* Existing = Sandbox->Commands.Find(Name))
		{
			luaL_unref(L, LUA_REGISTRYINDEX, *Existing);
			*Existing = Ref;
			return 0;
		}
		Sandbox->Commands.Add(Name, Ref);
		if (Sandbox->Runtime->ShouldRegisterConsoleCommands())
		{
			const FString Console = FString::Printf(TEXT("mod.%s.%s"), *Sandbox->Id.ToString(), *Name);
			FMadScriptRuntime* Runtime = Sandbox->Runtime;
			const FName ModId = Sandbox->Id;
			if (IConsoleObject* Object = IConsoleManager::Get().RegisterConsoleCommand(*Console,
				*FString::Printf(TEXT("A command from the %s script mod."), *ModId.ToString()),
				FConsoleCommandWithArgsDelegate::CreateLambda([Runtime, ModId, Name](const TArray<FString>& Args)
				{
					FString Error;
					if (!Runtime->RunCommand(ModId, Name, Args, Error) && !Error.IsEmpty())
					{
						Runtime->GetHost().Warn(ModId, Error);
					}
				}), ECVF_Default))
			{
				Sandbox->ConsoleObjects.Add(Object);
			}
		}
		return 0;
	}

	int L_GetBlock(lua_State* L)
	{
		const FIntVector Voxel(static_cast<int32>(luaL_checkinteger(L, 1)), static_cast<int32>(luaL_checkinteger(L, 2)), static_cast<int32>(luaL_checkinteger(L, 3)));
		FName Block;
		if (!SandboxOf(L)->Runtime->GetHost().GetBlock(Voxel, Block))
		{
			lua_pushnil(L);
			return 1;
		}
		lua_pushstring(L, TCHAR_TO_UTF8(*Block.ToString()));
		return 1;
	}

	int L_SetBlock(lua_State* L)
	{
		const FIntVector Voxel(static_cast<int32>(luaL_checkinteger(L, 1)), static_cast<int32>(luaL_checkinteger(L, 2)), static_cast<int32>(luaL_checkinteger(L, 3)));
		const char* BlockText = luaL_checkstring(L, 4);
		FMadScriptSandbox* Sandbox = SandboxOf(L);
		if (++Sandbox->EditsThisCall > FMadScriptRuntime::MaxEditsPerCall)
		{
			return luaL_error(L, "madfall.set_block: more than %d block edits in one call", FMadScriptRuntime::MaxEditsPerCall);
		}
		const bool bSet = Sandbox->Runtime->GetHost().SetBlock(Sandbox->Id, Voxel, FName(BlockText));
		lua_pushboolean(L, bSet ? 1 : 0);
		return 1;
	}

	int L_Player(lua_State* L)
	{
		FMadScriptPlayer Player;
		if (!SandboxOf(L)->Runtime->GetHost().GetPlayer(Player))
		{
			lua_pushnil(L);
			return 1;
		}
		lua_createtable(L, 0, 10);
		lua_pushinteger(L, Player.Voxel.X); lua_setfield(L, -2, "x");
		lua_pushinteger(L, Player.Voxel.Y); lua_setfield(L, -2, "y");
		lua_pushinteger(L, Player.Voxel.Z); lua_setfield(L, -2, "z");
		lua_pushnumber(L, Player.Health); lua_setfield(L, -2, "health");
		lua_pushnumber(L, Player.MaxHealth); lua_setfield(L, -2, "max_health");
		lua_pushnumber(L, Player.Food); lua_setfield(L, -2, "food");
		lua_pushnumber(L, Player.Water); lua_setfield(L, -2, "water");
		lua_pushnumber(L, Player.CoreTemperature); lua_setfield(L, -2, "core_temperature");
		lua_pushinteger(L, Player.Level); lua_setfield(L, -2, "level");
		lua_pushboolean(L, Player.bAlive ? 1 : 0); lua_setfield(L, -2, "alive");
		return 1;
	}

	int L_Give(lua_State* L)
	{
		const char* ItemText = luaL_checkstring(L, 1);
		const lua_Integer Count = luaL_optinteger(L, 2, 1);
		luaL_argcheck(L, Count >= 1 && Count <= 999, 2, "count must be 1-999");
		FMadScriptSandbox* Sandbox = SandboxOf(L);
		const int32 Given = Sandbox->Runtime->GetHost().GiveItem(Sandbox->Id, FName(ItemText), static_cast<int32>(Count));
		lua_pushinteger(L, Given);
		return 1;
	}

	int L_SpawnZombie(lua_State* L)
	{
		const char* ZombieText = luaL_checkstring(L, 1);
		const FIntVector Near(static_cast<int32>(luaL_checkinteger(L, 2)), static_cast<int32>(luaL_checkinteger(L, 3)), static_cast<int32>(luaL_checkinteger(L, 4)));
		FMadScriptSandbox* Sandbox = SandboxOf(L);
		if (++Sandbox->SpawnsThisCall > FMadScriptRuntime::MaxSpawnsPerCall)
		{
			return luaL_error(L, "madfall.spawn_zombie: more than %d spawns in one call", FMadScriptRuntime::MaxSpawnsPerCall);
		}
		lua_pushboolean(L, Sandbox->Runtime->GetHost().SpawnZombie(Sandbox->Id, FName(ZombieText), Near) ? 1 : 0);
		return 1;
	}

	int L_Time(lua_State* L)
	{
		int32 Day = 1;
		float Hour = 0.0f;
		SandboxOf(L)->Runtime->GetHost().GetTime(Day, Hour);
		lua_pushinteger(L, Day);
		lua_pushnumber(L, Hour);
		return 2;
	}

	int L_StoreGet(lua_State* L)
	{
		const char* KeyText = luaL_checkstring(L, 1);
		const FMadScriptValue* Value = SandboxOf(L)->Store.Find(UTF8_TO_TCHAR(KeyText));
		if (Value == nullptr)
		{
			lua_pushnil(L);
			return 1;
		}
		PushValue(L, *Value);
		return 1;
	}

	int L_StoreSet(lua_State* L)
	{
		size_t KeyLength = 0;
		const char* KeyText = luaL_checklstring(L, 1, &KeyLength);
		luaL_argcheck(L, KeyLength >= 1 && KeyLength <= 64, 1, "keys are 1-64 bytes");
		const int Type = lua_type(L, 2);
		size_t StringLength = 0;
		if (Type == LUA_TSTRING)
		{
			lua_tolstring(L, 2, &StringLength);
			luaL_argcheck(L, StringLength <= static_cast<size_t>(FMadScriptRuntime::MaxStoreStringLength), 2, "strings are at most 1024 bytes");
		}
		else if (Type != LUA_TNIL && Type != LUA_TBOOLEAN && Type != LUA_TNUMBER)
		{
			return luaL_argerror(L, 2, "values are nil, booleans, numbers or strings");
		}

		bool bFull = false;
		{
			FMadScriptSandbox* Sandbox = SandboxOf(L);
			const FString Key = UTF8_TO_TCHAR(KeyText);
			if (Type == LUA_TNIL)
			{
				Sandbox->Store.Remove(Key);
			}
			else if (!Sandbox->Store.Contains(Key) && Sandbox->Store.Num() >= FMadScriptRuntime::MaxStoreKeys)
			{
				bFull = true;
			}
			else
			{
				FMadScriptValue Value;
				if (Type == LUA_TBOOLEAN) { Value = FMadScriptValue::MakeBool(lua_toboolean(L, 2) != 0); }
				else if (Type == LUA_TNUMBER) { Value = FMadScriptValue::MakeNumber(lua_tonumber(L, 2)); }
				else { Value = FMadScriptValue::MakeString(UTF8_TO_TCHAR(lua_tostring(L, 2))); }
				Sandbox->Store.Add(Key, MoveTemp(Value));
			}
		}
		if (bFull)
		{
			return luaL_error(L, "madfall.store_set: at most %d keys per mod", FMadScriptRuntime::MaxStoreKeys);
		}
		return 0;
	}

	const luaL_Reg MadFallFunctions[] = {
		{ "log", L_Log },
		{ "message", L_Message },
		{ "on", L_On },
		{ "command", L_Command },
		{ "get_block", L_GetBlock },
		{ "set_block", L_SetBlock },
		{ "player", L_Player },
		{ "give", L_Give },
		{ "spawn_zombie", L_SpawnZombie },
		{ "time", L_Time },
		{ "store_get", L_StoreGet },
		{ "store_set", L_StoreSet },
		{ nullptr, nullptr }
	};

	/** Opens the safe libraries and the madfall table. Run protected: it allocates. */
	int OpenSandbox(lua_State* L)
	{
		luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
		lua_pop(L, 1);
		luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
		lua_pop(L, 1);
		luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
		lua_pop(L, 1);
		luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
		lua_pop(L, 1);
		luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
		lua_pop(L, 1);
		luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
		lua_pop(L, 1);

		// No filesystem and no precompiled bytecode, which can break the VM's guarantees.
		for (const char* Name : { "dofile", "loadfile", "load" })
		{
			lua_pushnil(L);
			lua_setglobal(L, Name);
		}
		lua_getglobal(L, "string");
		lua_pushnil(L);
		lua_setfield(L, -2, "dump");
		lua_pop(L, 1);

		lua_pushcfunction(L, L_Log);
		lua_setglobal(L, "print");

		luaL_newlib(L, MadFallFunctions);
		lua_pushstring(L, TCHAR_TO_UTF8(*SandboxOf(L)->Id.ToString()));
		lua_setfield(L, -2, "mod_id");
		lua_pushinteger(L, FMadScriptRuntime::ApiVersion);
		lua_setfield(L, -2, "api_version");
		lua_setglobal(L, "madfall");
		return 0;
	}

	struct FDispatchContext
	{
		const FMadScriptEvent* Event = nullptr;
		const TArray<FString>* Args = nullptr;
		int32 Ref = LUA_NOREF;
	};

	/** Builds the event table and calls one handler. Runs inside ProtectedCall: it allocates. */
	int CallHandler(lua_State* L)
	{
		const FDispatchContext* Context = static_cast<FDispatchContext*>(lua_touserdata(L, 1));
		lua_settop(L, 0);
		lua_rawgeti(L, LUA_REGISTRYINDEX, Context->Ref);
		if (Context->Event != nullptr)
		{
			const FMadScriptEvent& Event = *Context->Event;
			lua_createtable(L, 0, Event.Fields.Num() + 1);
			lua_pushstring(L, TCHAR_TO_UTF8(*Event.Name.ToString()));
			lua_setfield(L, -2, "name");
			for (const TPair<FString, FMadScriptValue>& Field : Event.Fields)
			{
				PushValue(L, Field.Value);
				lua_setfield(L, -2, TCHAR_TO_UTF8(*Field.Key));
			}
		}
		else
		{
			const TArray<FString>& Args = *Context->Args;
			lua_createtable(L, Args.Num(), 0);
			for (int32 Index = 0; Index < Args.Num(); ++Index)
			{
				lua_pushstring(L, TCHAR_TO_UTF8(*Args[Index]));
				lua_rawseti(L, -2, Index + 1);
			}
		}
		lua_call(L, 1, 0);
		return 0;
	}
}

// --- runtime ------------------------------------------------------------------------------

const TArray<FName>& FMadScriptRuntime::GetEventNames()
{
	static const TArray<FName> Names = {
		FName(TEXT("world_loaded")),
		FName(TEXT("second")),
		FName(TEXT("dawn")),
		FName(TEXT("dusk")),
		FName(TEXT("horde_night")),
		FName(TEXT("player_spawned")),
		FName(TEXT("player_died")),
		FName(TEXT("zombie_killed")),
		FName(TEXT("animal_killed")),
		FName(TEXT("block_placed")),
		FName(TEXT("block_broken")),
		FName(TEXT("item_crafted")),
		FName(TEXT("item_worn")),
		FName(TEXT("bed_set")),
		FName(TEXT("quest_completed")),
	};
	return Names;
}

FMadScriptRuntime::FMadScriptRuntime(IMadScriptHost& InHost, bool bRegisterConsoleCommands)
	: Host(InHost)
	, bConsoleCommands(bRegisterConsoleCommands)
{
}

FMadScriptRuntime::~FMadScriptRuntime()
{
	Sandboxes.Reset();
}

FMadScriptSandbox* FMadScriptRuntime::Find(FName ModId) const
{
	for (const TUniquePtr<FMadScriptSandbox>& Sandbox : Sandboxes)
	{
		if (Sandbox->Id == ModId)
		{
			return Sandbox.Get();
		}
	}
	return nullptr;
}

FMadScriptSandbox* FMadScriptRuntime::FindOrCreate(FName ModId)
{
	if (FMadScriptSandbox* Existing = Find(ModId))
	{
		return Existing;
	}

	TUniquePtr<FMadScriptSandbox> Sandbox = MakeUnique<FMadScriptSandbox>();
	Sandbox->Runtime = this;
	Sandbox->Id = ModId;
	Sandbox->L = lua_newstate(LuaAlloc, Sandbox.Get());
	if (Sandbox->L == nullptr)
	{
		Host.Warn(ModId, TEXT("could not create a Lua state"));
		return nullptr;
	}
	*static_cast<FMadScriptSandbox**>(lua_getextraspace(Sandbox->L)) = Sandbox.Get();
	lua_sethook(Sandbox->L, LuaHook, LUA_MASKCOUNT, HookInstructions);

	lua_pushcfunction(Sandbox->L, OpenSandbox);
	if (!ProtectedCall(*Sandbox, 0, TEXT("opening the sandbox")))
	{
		return nullptr;
	}

	if (TMap<FString, FMadScriptValue>* Saved = PendingStore.Find(ModId))
	{
		Sandbox->Store = MoveTemp(*Saved);
		PendingStore.Remove(ModId);
	}
	return Sandboxes.Add_GetRef(MoveTemp(Sandbox)).Get();
}

bool FMadScriptRuntime::LoadSource(FName ModId, const FString& ChunkName, const FString& Source, FString& OutError)
{
	FMadScriptSandbox* Sandbox = FindOrCreate(ModId);
	if (Sandbox == nullptr)
	{
		OutError = TEXT("no sandbox");
		return false;
	}
	if (Sandbox->bDisabled)
	{
		OutError = TEXT("the mod's scripts are disabled");
		return false;
	}

	const FTCHARToUTF8 Utf8(*Source);
	const FTCHARToUTF8 Name(*ChunkName);
	lua_State* L = Sandbox->L;
	if (!lua_checkstack(L, 4))
	{
		OutError = TEXT("out of stack");
		return false;
	}
	// Text only: a binary chunk is refused before it runs.
	const int Status = luaL_loadbufferx(L, Utf8.Get(), Utf8.Length(), Name.Get(), "t");
	if (Status != LUA_OK)
	{
		const char* Message = lua_tostring(L, -1);
		OutError = Message ? UTF8_TO_TCHAR(Message) : TEXT("could not load");
		lua_pop(L, 1);
		++Sandbox->Errors;
		Sandbox->LastError = OutError;
		Host.Warn(ModId, OutError);
		return false;
	}
	const int32 ErrorsBefore = Sandbox->Errors;
	if (!ProtectedCall(*Sandbox, 0, *ChunkName))
	{
		OutError = Sandbox->Errors > ErrorsBefore ? Sandbox->LastError : TEXT("error");
		return false;
	}
	++Sandbox->ScriptsLoaded;
	return true;
}

void FMadScriptRuntime::LoadMods(const TArray<FMadScriptMod>& Mods)
{
	for (const FMadScriptMod& Mod : Mods)
	{
		const FString Root = FPaths::ConvertRelativePathToFull(Mod.Directory);
		for (const FString& Script : Mod.Scripts)
		{
			const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(Root, Script));
			// A script listed as ../../something reads nothing outside its own mod.
			if (Script.Contains(TEXT("..")) || !FPaths::IsUnderDirectory(Path, Root))
			{
				Host.Warn(Mod.Id, FString::Printf(TEXT("script '%s' is outside the mod folder; skipped"), *Script));
				continue;
			}
			FString Source;
			if (!FFileHelper::LoadFileToString(Source, *Path))
			{
				Host.Warn(Mod.Id, FString::Printf(TEXT("script '%s' could not be read"), *Script));
				continue;
			}
			FString Error;
			if (LoadSource(Mod.Id, FString::Printf(TEXT("@%s/%s"), *Mod.Id.ToString(), *Script), Source, Error))
			{
				Host.Log(Mod.Id, FString::Printf(TEXT("loaded %s"), *Script));
			}
		}
	}
}

bool FMadScriptRuntime::HasHandlers(FName EventName) const
{
	for (const TUniquePtr<FMadScriptSandbox>& Sandbox : Sandboxes)
	{
		if (!Sandbox->bDisabled && Sandbox->Handlers.Contains(EventName))
		{
			return true;
		}
	}
	return false;
}

void FMadScriptRuntime::Dispatch(const FMadScriptEvent& Event)
{
	for (int32 Index = 0; Index < Sandboxes.Num(); ++Index)
	{
		FMadScriptSandbox& Sandbox = *Sandboxes[Index];
		const TArray<int32>* Found = Sandbox.bDisabled ? nullptr : Sandbox.Handlers.Find(Event.Name);
		if (Found == nullptr)
		{
			continue;
		}
		// A copy: a handler may register more handlers while it runs.
		const TArray<int32> Refs = *Found;
		for (const int32 Ref : Refs)
		{
			if (Sandbox.bDisabled || !lua_checkstack(Sandbox.L, 4))
			{
				break;
			}
			FDispatchContext Context;
			Context.Event = &Event;
			Context.Ref = Ref;
			lua_pushcfunction(Sandbox.L, CallHandler);
			lua_pushlightuserdata(Sandbox.L, &Context);
			ProtectedCall(Sandbox, 1, *FString::Printf(TEXT("%s handler"), *Event.Name.ToString()));
		}
	}
}

bool FMadScriptRuntime::RunCommand(FName ModId, const FString& Name, const TArray<FString>& Args, FString& OutError)
{
	FMadScriptSandbox* Sandbox = Find(ModId);
	const int32* Ref = Sandbox ? Sandbox->Commands.Find(Name) : nullptr;
	if (Ref == nullptr)
	{
		OutError = FString::Printf(TEXT("%s has no command '%s'"), *ModId.ToString(), *Name);
		return false;
	}
	if (Sandbox->bDisabled || !lua_checkstack(Sandbox->L, 4))
	{
		OutError = TEXT("the mod's scripts are disabled");
		return false;
	}
	FDispatchContext Context;
	Context.Args = &Args;
	Context.Ref = *Ref;
	const int32 ErrorsBefore = Sandbox->Errors;
	lua_pushcfunction(Sandbox->L, CallHandler);
	lua_pushlightuserdata(Sandbox->L, &Context);
	if (!ProtectedCall(*Sandbox, 1, *FString::Printf(TEXT("command %s"), *Name)))
	{
		// Already reported through the host; the caller need not repeat it.
		OutError.Reset();
		return Sandbox->Errors == ErrorsBefore;
	}
	return true;
}

void FMadScriptRuntime::GetStatus(TArray<FMadScriptModStatus>& Out) const
{
	Out.Reset();
	for (const TUniquePtr<FMadScriptSandbox>& Sandbox : Sandboxes)
	{
		FMadScriptModStatus& Status = Out.AddDefaulted_GetRef();
		Status.Id = Sandbox->Id;
		Status.ScriptsLoaded = Sandbox->ScriptsLoaded;
		for (const TPair<FName, TArray<int32>>& Pair : Sandbox->Handlers)
		{
			Status.Handlers += Pair.Value.Num();
		}
		Status.Commands = Sandbox->Commands.Num();
		Status.Errors = Sandbox->Errors;
		Status.bDisabled = Sandbox->bDisabled;
		Status.LastError = Sandbox->LastError;
		Status.WorstCallMs = Sandbox->WorstCallMs;
		Status.MemoryBytes = Sandbox->Memory;
	}
}

void FMadScriptRuntime::ExportStore(TMap<FName, TMap<FString, FMadScriptValue>>& Out) const
{
	Out = PendingStore;
	for (const TUniquePtr<FMadScriptSandbox>& Sandbox : Sandboxes)
	{
		if (Sandbox->Store.Num() > 0)
		{
			Out.Add(Sandbox->Id, Sandbox->Store);
		}
	}
}

void FMadScriptRuntime::ImportStore(const TMap<FName, TMap<FString, FMadScriptValue>>& In)
{
	PendingStore.Reset();
	for (const TPair<FName, TMap<FString, FMadScriptValue>>& Pair : In)
	{
		if (FMadScriptSandbox* Sandbox = Find(Pair.Key))
		{
			Sandbox->Store = Pair.Value;
		}
		else
		{
			// A removed mod's data is kept, like an unknown item in a slot.
			PendingStore.Add(Pair.Key, Pair.Value);
		}
	}
}
