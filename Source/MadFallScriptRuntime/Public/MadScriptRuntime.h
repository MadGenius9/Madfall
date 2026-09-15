// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadScriptHost.h"

class IConsoleObject;
struct FMadScriptSandbox;

/** A mod's scripts to load. */
struct FMadScriptMod
{
	FName Id;
	FString Directory;

	/** Relative to Directory, in load order. */
	TArray<FString> Scripts;
};

/** What `mad.scripts` shows for one mod. */
struct FMadScriptModStatus
{
	FName Id;
	int32 ScriptsLoaded = 0;
	int32 Handlers = 0;
	int32 Commands = 0;
	int32 Errors = 0;
	bool bDisabled = false;
	FString LastError;
	double WorstCallMs = 0.0;
	int64 MemoryBytes = 0;
};

/**
 * Tier-3 script mods: Lua 5.4, one sandbox per mod.
 *
 * THE SANDBOX
 *   - Each mod gets its own lua_State, so mods cannot see or break each other's
 *     globals.
 *   - Only base, string, table, math, utf8 and coroutine are opened. io, os,
 *     package and debug are not even compiled into the game. From base,
 *     dofile, loadfile and load are removed (no filesystem, no bytecode), and
 *     print goes to the log. string.dump is removed too.
 *   - Memory: an allocator caps each mod (MemoryLimitBytes); an allocation past
 *     it fails and Lua raises "not enough memory" inside the script.
 *   - Time: a count hook checks the clock every 1000 instructions, and a call
 *     that runs past CallBudgetMs is stopped with an error. A mod whose
 *     handlers keep failing is disabled after MaxErrors.
 *   - World writes per call are capped (256 block edits, 4 zombie spawns), so
 *     one event cannot flatten a region or start a horde.
 *
 * THE API (the `madfall` table)
 *   madfall.on(event, fn), madfall.command(name, fn), madfall.log(...),
 *   madfall.message(text), madfall.get_block(x, y, z), madfall.set_block(x, y, z, id),
 *   madfall.player(), madfall.give(item, count), madfall.spawn_zombie(id, x, y, z),
 *   madfall.time(), madfall.store_get(key), madfall.store_set(key, value),
 *   madfall.mod_id, madfall.api_version.
 *
 * Handlers run on the game thread when the game dispatches an event. Lua errors
 * unwind with longjmp, so the bindings read every argument before touching C++
 * objects that own memory.
 */
class MADFALLSCRIPTRUNTIME_API FMadScriptRuntime
{
public:
	/** The scripting API's own version, readable as madfall.api_version. */
	static constexpr int32 ApiVersion = 1;

	static constexpr int64 MemoryLimitBytes = 16 * 1024 * 1024;
	/** One handler or command call. The game frame's whole budget is 2 ms; the game also caps script time per frame. */
	static constexpr double CallBudgetMs = 1.0;
	static constexpr int32 MaxErrors = 20;
	static constexpr int32 MaxEditsPerCall = 256;
	static constexpr int32 MaxSpawnsPerCall = 4;
	static constexpr int32 MaxStoreKeys = 256;
	static constexpr int32 MaxStoreStringLength = 1024;

	/** Event names a script may listen for. */
	static const TArray<FName>& GetEventNames();

	/**
	 * RegisterConsoleCommands: whether madfall.command also creates a console
	 * command `mod.<mod id>.<name>`. Tests turn it off.
	 */
	explicit FMadScriptRuntime(IMadScriptHost& InHost, bool bRegisterConsoleCommands = true);
	~FMadScriptRuntime();

	FMadScriptRuntime(const FMadScriptRuntime&) = delete;
	FMadScriptRuntime& operator=(const FMadScriptRuntime&) = delete;

	/** Loads every mod's scripts. A failing script is reported and its mod keeps what loaded before it. */
	void LoadMods(const TArray<FMadScriptMod>& Mods);

	/** Loads one chunk of source into a mod's sandbox, creating the sandbox if needed. */
	bool LoadSource(FName ModId, const FString& ChunkName, const FString& Source, FString& OutError);

	/** Calls every handler for Event.Name, each mod in load order. */
	void Dispatch(const FMadScriptEvent& Event);

	bool HasHandlers(FName EventName) const;

	/** Runs a command a mod registered. False if there is none, or it errored (OutError says why). */
	bool RunCommand(FName ModId, const FString& Name, const TArray<FString>& Args, FString& OutError);

	void GetStatus(TArray<FMadScriptModStatus>& Out) const;

	/** The per-world key/value store, per mod. */
	void ExportStore(TMap<FName, TMap<FString, FMadScriptValue>>& Out) const;
	void ImportStore(const TMap<FName, TMap<FString, FMadScriptValue>>& In);

	IMadScriptHost& GetHost() const { return Host; }
	bool ShouldRegisterConsoleCommands() const { return bConsoleCommands; }

private:
	FMadScriptSandbox* FindOrCreate(FName ModId);
	FMadScriptSandbox* Find(FName ModId) const;

	IMadScriptHost& Host;
	bool bConsoleCommands = true;
	TArray<TUniquePtr<FMadScriptSandbox>> Sandboxes;

	/** Store values imported before a mod's sandbox exists (a save loaded, then the mod). */
	TMap<FName, TMap<FString, FMadScriptValue>> PendingStore;
};
