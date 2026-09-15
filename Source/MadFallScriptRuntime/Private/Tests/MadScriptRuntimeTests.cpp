// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "HAL/PlatformTime.h"
#include "MadScriptRuntime.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Records what scripts ask for; a tiny world of one column of blocks. */
	class FFakeScriptHost : public IMadScriptHost
	{
	public:
		TArray<FString> Logs;
		TArray<FString> Warnings;
		TArray<FString> Messages;
		TMap<FIntVector, FName> Blocks;
		TArray<TPair<FName, int32>> Given;
		int32 Spawns = 0;
		bool bHasPlayer = true;

		virtual void Log(FName, const FString& Message) override { Logs.Add(Message); }
		virtual void Warn(FName, const FString& Message) override { Warnings.Add(Message); }
		virtual void ShowMessage(FName, const FString& Message) override { Messages.Add(Message); }
		virtual bool GetBlock(const FIntVector& Voxel, FName& OutBlock) override
		{
			const FName* Found = Blocks.Find(Voxel);
			OutBlock = Found ? *Found : FName(TEXT("madfall:air"));
			return true;
		}
		virtual bool SetBlock(FName, const FIntVector& Voxel, FName Block) override { Blocks.Add(Voxel, Block); return true; }
		virtual bool GetPlayer(FMadScriptPlayer& Out) override
		{
			Out.Voxel = FIntVector(10, 20, 30);
			Out.Health = 80.0f;
			Out.MaxHealth = 100.0f;
			Out.Level = 3;
			return bHasPlayer;
		}
		virtual int32 GiveItem(FName, FName Item, int32 Count) override { Given.Add({ Item, Count }); return Count; }
		virtual bool SpawnZombie(FName, FName, const FIntVector&) override { ++Spawns; return true; }
		virtual void GetTime(int32& OutDay, float& OutHour) override { OutDay = 3; OutHour = 21.5f; }

		bool Logged(const FString& Text) const
		{
			return Logs.ContainsByPredicate([&Text](const FString& Line) { return Line.Contains(Text); });
		}
		bool Warned(const FString& Text) const
		{
			return Warnings.ContainsByPredicate([&Text](const FString& Line) { return Line.Contains(Text); });
		}
	};

	const FName ModA(TEXT("mod_a"));
	const FName ModB(TEXT("mod_b"));

	bool Load(FMadScriptRuntime& Runtime, FName Mod, const TCHAR* Source)
	{
		FString Error;
		return Runtime.LoadSource(Mod, TEXT("=test"), Source, Error);
	}

	const FMadScriptModStatus* StatusOf(const FMadScriptRuntime& Runtime, FName Mod, TArray<FMadScriptModStatus>& Storage)
	{
		Runtime.GetStatus(Storage);
		return Storage.FindByPredicate([Mod](const FMadScriptModStatus& Status) { return Status.Id == Mod; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadScriptSandboxTest,
	"MadFall.Scripts.Sandbox",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadScriptSandboxTest::RunTest(const FString& Parameters)
{
	FFakeScriptHost Host;
	FMadScriptRuntime Runtime(Host, /*bRegisterConsoleCommands*/ false);

	TestTrue(TEXT("a plain script loads"), Load(Runtime, ModA, TEXT("secret = 42")));
	TestTrue(TEXT("unsafe libraries are absent"), Load(Runtime, ModA, TEXT(
		"assert(io == nil and os == nil and package == nil and require == nil and debug == nil)\n"
		"assert(load == nil and loadfile == nil and dofile == nil and string.dump == nil)\n"
		"assert(string and table and math and utf8 and coroutine)\n"
		"madfall.log('checked')")));
	TestTrue(TEXT("print goes to the host log"), Load(Runtime, ModA, TEXT("print('hello', 7, true)")) && Host.Logged(TEXT("hello 7 true")));

	// Mods cannot see each other's globals.
	TestTrue(TEXT("mod b loads"), Load(Runtime, ModB, TEXT("assert(secret == nil); madfall.log('isolated ' .. madfall.mod_id)")));
	TestTrue(TEXT("mod b saw no globals from mod a"), Host.Logged(TEXT("isolated mod_b")));

	// Precompiled chunks can break the VM's safety; only text is accepted.
	FString Error;
	TestFalse(TEXT("a binary chunk is refused"), Runtime.LoadSource(ModA, TEXT("=binary"), TEXT("\x1bLua\x54"), Error));
	TestTrue(TEXT("with a reason"), Error.Contains(TEXT("binary")));

	TestFalse(TEXT("a syntax error fails the load"), Load(Runtime, ModA, TEXT("this is not lua")));
	TestFalse(TEXT("a runtime error fails the load"), Load(Runtime, ModA, TEXT("error('boom')")));
	TestTrue(TEXT("and is reported"), Host.Warned(TEXT("boom")));
	TestFalse(TEXT("an unknown event is an error"), Load(Runtime, ModA, TEXT("madfall.on('no_such_event', function() end)")));

	// Time: an endless loop is stopped, even one that tries to catch the stop.
	TestTrue(TEXT("looping handlers register"), Load(Runtime, ModA, TEXT(
		"madfall.on('second', function() while true do end end)\n"
		"madfall.on('dawn', function() while true do pcall(function() while true do end end) end end)\n"
		"madfall.on('dusk', function() local co = coroutine.wrap(function() while true do end end); while true do pcall(co) end end)\n"
		"madfall.on('world_loaded', function() madfall.log('still alive') end)")));
	for (const TCHAR* Event : { TEXT("second"), TEXT("dawn"), TEXT("dusk") })
	{
		const double Start = FPlatformTime::Seconds();
		Runtime.Dispatch(FMadScriptEvent(FName(Event)));
		const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
		TestTrue(FString::Printf(TEXT("%s: the endless handler was stopped (%.2f ms)"), Event, Ms), Ms < FMadScriptRuntime::CallBudgetMs + 20.0);
	}
	TestTrue(TEXT("the stop is reported"), Host.Warned(TEXT("was stopped")));
	Runtime.Dispatch(FMadScriptEvent(TEXT("world_loaded")));
	TestTrue(TEXT("the mod still runs afterwards"), Host.Logged(TEXT("still alive")));

	// Memory: a script that keeps allocating hits the cap, not the game.
	TestTrue(TEXT("hungry handler registers"), Load(Runtime, ModB, TEXT(
		"madfall.on('second', function() local s = string.rep('x', 64 * 1024 * 1024) end)")));
	Runtime.Dispatch(FMadScriptEvent(TEXT("second")));
	TestTrue(TEXT("out of memory is reported"), Host.Warned(TEXT("not enough memory")));
	TArray<FMadScriptModStatus> Statuses;
	const FMadScriptModStatus* B = StatusOf(Runtime, ModB, Statuses);
	TestTrue(TEXT("memory is back under the cap after collection"), B != nullptr && B->MemoryBytes < FMadScriptRuntime::MemoryLimitBytes / 2);

	// A mod that keeps failing is disabled.
	FFakeScriptHost Host2;
	FMadScriptRuntime Failing(Host2, false);
	Load(Failing, ModA, TEXT("madfall.on('second', function() error('again') end)"));
	for (int32 Index = 0; Index < FMadScriptRuntime::MaxErrors + 5; ++Index)
	{
		Failing.Dispatch(FMadScriptEvent(TEXT("second")));
	}
	const FMadScriptModStatus* A = StatusOf(Failing, ModA, Statuses);
	TestTrue(TEXT("disabled after MaxErrors"), A != nullptr && A->bDisabled);
	TestEqual(TEXT("and stops being called"), A ? A->Errors : -1, FMadScriptRuntime::MaxErrors);
	TestFalse(TEXT("a disabled mod has no live handlers"), Failing.HasHandlers(TEXT("second")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadScriptApiTest,
	"MadFall.Scripts.Api",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadScriptApiTest::RunTest(const FString& Parameters)
{
	FFakeScriptHost Host;
	FMadScriptRuntime Runtime(Host, false);

	TestTrue(TEXT("api script loads"), Load(Runtime, ModA, TEXT(
		"madfall.on('block_placed', function(e)\n"
		"  madfall.log(string.format('%s %s at %d %d %d, day ', e.name, e.block, e.x, e.y, e.z) .. e.count)\n"
		"end)\n"
		"madfall.on('second', function()\n"
		"  local p = madfall.player()\n"
		"  madfall.set_block(p.x, p.y, p.z + 1, 'madfall:stone')\n"
		"  madfall.log('above is ' .. madfall.get_block(p.x, p.y, p.z + 1) .. ', level ' .. p.level)\n"
		"  local day, hour = madfall.time()\n"
		"  madfall.log('time ' .. day .. ' ' .. hour)\n"
		"  madfall.message('hi')\n"
		"  madfall.give('madfall:arrow', 5)\n"
		"end)\n"
		"madfall.on('dawn', function() for i = 1, 300 do madfall.set_block(i, 0, 0, 'madfall:stone') end end)\n"
		"madfall.on('dusk', function() for i = 1, 6 do madfall.spawn_zombie('madfall:zombie_civilian', 0, 0, 0) end end)\n"
		"madfall.on('horde_night', function() madfall.give('madfall:arrow', 0) end)\n"
		"madfall.command('echo', function(args) madfall.log('echo ' .. table.concat(args, ',')) end)\n")));

	TestTrue(TEXT("has block_placed handlers"), Runtime.HasHandlers(TEXT("block_placed")));
	TestFalse(TEXT("no handlers for bed_set"), Runtime.HasHandlers(TEXT("bed_set")));

	FMadScriptEvent Placed(TEXT("block_placed"));
	Placed.Add(TEXT("block"), FString(TEXT("madfall:wood_frame"))).Add(TEXT("x"), 1.0).Add(TEXT("y"), -2.0).Add(TEXT("z"), 3.0).Add(TEXT("count"), 1.0);
	Runtime.Dispatch(Placed);
	TestTrue(TEXT("event fields arrive, whole numbers as integers"), Host.Logged(TEXT("block_placed madfall:wood_frame at 1 -2 3, day 1")));

	Runtime.Dispatch(FMadScriptEvent(TEXT("second")));
	TestEqual(TEXT("set_block reached the host"), Host.Blocks.FindRef(FIntVector(10, 20, 31)), FName(TEXT("madfall:stone")));
	TestTrue(TEXT("get_block and player read back"), Host.Logged(TEXT("above is madfall:stone, level 3")));
	TestTrue(TEXT("time"), Host.Logged(TEXT("time 3 21.5")));
	TestEqual(TEXT("message"), Host.Messages.Num(), 1);
	TestTrue(TEXT("give"), Host.Given.Num() == 1 && Host.Given[0].Value == 5);

	Host.Blocks.Reset();
	Runtime.Dispatch(FMadScriptEvent(TEXT("dawn")));
	TestEqual(TEXT("block edits per call are capped"), Host.Blocks.Num(), FMadScriptRuntime::MaxEditsPerCall);
	TestTrue(TEXT("the cap is reported"), Host.Warned(TEXT("block edits in one call")));

	Runtime.Dispatch(FMadScriptEvent(TEXT("dusk")));
	TestEqual(TEXT("spawns per call are capped"), Host.Spawns, FMadScriptRuntime::MaxSpawnsPerCall);

	Runtime.Dispatch(FMadScriptEvent(TEXT("horde_night")));
	TestTrue(TEXT("bad arguments are script errors"), Host.Warned(TEXT("count must be 1-999")));

	FString Error;
	TestTrue(TEXT("command runs"), Runtime.RunCommand(ModA, TEXT("echo"), { TEXT("a"), TEXT("b") }, Error));
	TestTrue(TEXT("with its arguments"), Host.Logged(TEXT("echo a,b")));
	TestFalse(TEXT("unknown command"), Runtime.RunCommand(ModA, TEXT("nope"), {}, Error));
	TestFalse(TEXT("bad command names are refused"), Load(Runtime, ModA, TEXT("madfall.command('Bad Name', function() end)")));

	// Without a survivor, player() is nil rather than an error.
	Host.bHasPlayer = false;
	TestTrue(TEXT("player() is nil with no survivor"), Load(Runtime, ModA, TEXT("assert(madfall.player() == nil)")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadScriptStoreTest,
	"MadFall.Scripts.Store",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadScriptStoreTest::RunTest(const FString& Parameters)
{
	FFakeScriptHost Host;
	TMap<FName, TMap<FString, FMadScriptValue>> Saved;
	{
		FMadScriptRuntime Runtime(Host, false);
		TestTrue(TEXT("store script loads"), Load(Runtime, ModA, TEXT(
			"madfall.store_set('n', 3)\n"
			"madfall.store_set('pi', 3.25)\n"
			"madfall.store_set('flag', true)\n"
			"madfall.store_set('name', 'watch')\n"
			"madfall.store_set('gone', 1)\n"
			"madfall.store_set('gone', nil)\n"
			"assert(madfall.store_get('n') == 3 and math.type(madfall.store_get('n')) == 'integer')\n"
			"assert(madfall.store_get('gone') == nil)\n"
			"assert(not pcall(madfall.store_set, 'table', {}))\n"
			"assert(not pcall(madfall.store_set, 'long', string.rep('x', 2000)))\n")));
		TestTrue(TEXT("key limit"), Load(Runtime, ModB, TEXT(
			"for i = 1, 256 do madfall.store_set('k' .. i, i) end\n"
			"assert(not pcall(madfall.store_set, 'one_more', 1))\n"
			"madfall.store_set('k1', 'overwrite is fine')")));
		Runtime.ExportStore(Saved);
	}
	TestEqual(TEXT("both mods exported"), Saved.Num(), 2);
	const TMap<FString, FMadScriptValue>& A = Saved.FindRef(ModA);
	TestEqual(TEXT("four values"), A.Num(), 4);
	TestTrue(TEXT("number"), A.Contains(TEXT("pi")) && A[TEXT("pi")] == FMadScriptValue::MakeNumber(3.25));
	TestTrue(TEXT("bool"), A.Contains(TEXT("flag")) && A[TEXT("flag")] == FMadScriptValue::MakeBool(true));
	TestTrue(TEXT("string"), A.Contains(TEXT("name")) && A[TEXT("name")] == FMadScriptValue::MakeString(TEXT("watch")));

	// A save is imported before the mods' scripts load; values wait for their mod.
	Saved.Add(TEXT("removed_mod"), { { TEXT("kept"), FMadScriptValue::MakeNumber(1.0) } });
	FMadScriptRuntime Reloaded(Host, false);
	Reloaded.ImportStore(Saved);
	TestTrue(TEXT("values are there at a script's top level"), Load(Reloaded, ModA, TEXT(
		"assert(madfall.store_get('n') == 3 and madfall.store_get('flag') == true and madfall.store_get('name') == 'watch')")));
	TMap<FName, TMap<FString, FMadScriptValue>> Again;
	Reloaded.ExportStore(Again);
	TestTrue(TEXT("a removed mod's values survive a save"), Again.Contains(TEXT("removed_mod")));
	TestTrue(TEXT("a mod not loaded this session keeps its values too"), Again.Contains(ModB) && Again[ModB].Num() == 256);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadScriptExampleModTest,
	"MadFall.Scripts.ExampleMod",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadScriptExampleModTest::RunTest(const FString& Parameters)
{
	const FName Id(TEXT("example_scripted"));
	FFakeScriptHost Host;
	FMadScriptRuntime Runtime(Host, false);
	Runtime.LoadMods({ { Id, FPaths::Combine(FPaths::ProjectDir(), TEXT("Mods"), Id.ToString()), { TEXT("scripts/main.lua") } } });

	TArray<FMadScriptModStatus> Statuses;
	const FMadScriptModStatus* Status = StatusOf(Runtime, Id, Statuses);
	if (!TestTrue(TEXT("the example mod loaded"), Status != nullptr && Status->ScriptsLoaded == 1 && Status->Errors == 0))
	{
		for (const FString& Warning : Host.Warnings) { AddError(Warning); }
		return false;
	}

	FString Error;
	TestTrue(TEXT("selftest runs"), Runtime.RunCommand(Id, TEXT("selftest"), {}, Error));
	TestTrue(TEXT("and passes"), Host.Logged(TEXT("selftest passed")));

	TestTrue(TEXT("beacon runs"), Runtime.RunCommand(Id, TEXT("beacon"), { TEXT("4") }, Error));
	TestEqual(TEXT("beacon places four frames and a torch"), Host.Blocks.Num(), 5);
	TestEqual(TEXT("torch on top"), Host.Blocks.FindRef(FIntVector(12, 20, 34)), FName(TEXT("madfall:torch")));

	FMadScriptEvent Kill(TEXT("zombie_killed"));
	Kill.Add(TEXT("zombie"), FString(TEXT("madfall:zombie_civilian"))).Add(TEXT("count"), 1.0);
	for (int32 Index = 0; Index < 25; ++Index)
	{
		Runtime.Dispatch(Kill);
	}
	TestTrue(TEXT("bounty paid at 25 kills"), Host.Given.Num() == 1 && Host.Given[0].Key == FName(TEXT("madfall:arrow")) && Host.Given[0].Value == 10);

	Runtime.Dispatch(FMadScriptEvent(TEXT("horde_night")));
	FMadScriptEvent Dawn(TEXT("dawn"));
	Dawn.Add(TEXT("day"), 8.0);
	Runtime.Dispatch(Dawn);
	TestEqual(TEXT("supplies after the horde night"), Host.Given.Num(), 3);
	TestTrue(TEXT("dawn logs whole numbers"), Host.Logged(TEXT("dawn of day 8 (1 nights watched)")));

	TMap<FName, TMap<FString, FMadScriptValue>> Store;
	Runtime.ExportStore(Store);
	const TMap<FString, FMadScriptValue>& Values = Store.FindRef(Id);
	TestTrue(TEXT("kills stored"), Values.Contains(TEXT("kills")) && Values[TEXT("kills")] == FMadScriptValue::MakeNumber(25.0));
	TestFalse(TEXT("the pending flag was cleared"), Values.Contains(TEXT("horde_pending")));

	TestEqual(TEXT("no script errors"), Host.Warnings.Num(), 0);
	return true;
}

#endif
