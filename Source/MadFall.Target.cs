// Copyright MadFall. All Rights Reserved.

using UnrealBuildTool;

/// <summary>
/// Standalone game client target (and listen-server host).
/// </summary>
public class MadFallTarget : TargetRules
{
	public MadFallTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;

		// Pinned, not Latest: an engine upgrade should be a deliberate commit
		// that flips these, not a silent behaviour change on someone's machine.
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		CppStandard = CppStandardVersion.Cpp20;

		ExtraModuleNames.AddRange(new string[]
		{
			"MadFallModAPI",
			"MadFallCore",
			"MadFallMesher",
			"MadFallGameplay"
		});
	}
}
