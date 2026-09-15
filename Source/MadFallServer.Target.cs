// Copyright MadFall. All Rights Reserved.

using UnrealBuildTool;

/// <summary>
/// Dedicated server target (Windows + Linux).
///
/// Exists from day one on purpose. The most common way a UE project discovers
/// it cannot ship a dedicated server is by adding editor-only or client-only
/// includes for months and finding out at the end. Building this target in CI
/// on every commit makes that a compile error the same day it is introduced.
/// </summary>
public class MadFallServerTarget : TargetRules
{
	public MadFallServerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Server;

		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		CppStandard = CppStandardVersion.Cpp20;

		// Server runs headless; MadFallEditor is deliberately absent.
		ExtraModuleNames.AddRange(new string[]
		{
			"MadFallModAPI",
			"MadFallCore",
			"MadFallMesher",
			"MadFallGameplay"
		});
	}
}
