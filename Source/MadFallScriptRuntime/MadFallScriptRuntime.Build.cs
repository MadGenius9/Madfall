// Copyright MadFall. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

/**
 * Tier-3 script mods: Lua 5.4, sandboxed per mod.
 *
 * Depends on MadFallModAPI and Core only. The scripting surface is exactly the
 * IMadScriptHost interface declared there; this module cannot see the engine,
 * the voxel world or the filesystem beyond reading the scripts it is handed.
 *
 * Lua's sources (Source/ThirdParty/Lua) are compiled into this module as C++
 * (Private/MadLua.cpp explains why) rather than linked from a prebuilt library:
 * one platform-independent build, no binary in the repository, and the unsafe
 * standard libraries (io, os, package, debug) simply are not in the source set.
 */
public class MadFallScriptRuntime : ModuleRules
{
	public MadFallScriptRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		// MadLua.cpp compiles Lua, which has functions named check and verify -
		// Unreal macros. No shared PCH and no unity build, so it never sees them.
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;

		// Lua is third-party C; its warnings are not ours to fix. MadFall's own
		// files in this module are kept warning-clean by review and CI builds of
		// the modules that include them.
		bWarningsAsErrors = false;
		CppCompileWarningSettings.ShadowVariableWarningLevel = WarningLevel.Off;
		CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Off;
		CppCompileWarningSettings.UnreachableCodeWarningLevel = WarningLevel.Off;

		// Lua built as C++ raises its errors with throw/catch (ldo.c,
		// LUAI_THROW). Enabled for this module only: the throw never leaves Lua
		// and the bindings, because the game side of IMadScriptHost never calls
		// back into a script.
		bEnableExceptions = true;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"MadFallModAPI"
		});

		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "ThirdParty", "Lua"));
	}
}
