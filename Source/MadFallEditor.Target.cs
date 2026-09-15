// Copyright MadFall. All Rights Reserved.

using UnrealBuildTool;

/// <summary>
/// Editor target. Adds MadFallEditor (prefab/POI authoring, block-definition
/// linter) on top of the runtime module set.
/// </summary>
public class MadFallEditorTarget : TargetRules
{
	public MadFallEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;

		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		CppStandard = CppStandardVersion.Cpp20;

		ExtraModuleNames.AddRange(new string[]
		{
			"MadFallModAPI",
			"MadFallCore",
			"MadFallMesher",
			"MadFallGameplay",
			"MadFallEditor"
		});
	}
}
