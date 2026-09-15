// Copyright MadFall. All Rights Reserved.

using UnrealBuildTool;

/// <summary>
/// MadFallEditor - editor-only tooling: POI/prefab authoring volumes, the block
/// definition linter, region-file inspector.
///
/// Nothing depends on this module, and it is excluded from MadFallServer.Target.cs.
/// </summary>
public class MadFallEditor : ModuleRules
{
	public MadFallEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bWarningsAsErrors = true;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"MadFallCore",
			"MadFallMesher",
			"MadFallGameplay"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"EditorSubsystem",
			"EditorStyle",
			"PropertyEditor",
			"ToolMenus",
			"Projects"
		});
	}
}
