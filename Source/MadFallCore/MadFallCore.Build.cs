// Copyright MadFall. All Rights Reserved.

using UnrealBuildTool;

/// <summary>
/// MadFallCore - voxel volume, chunk storage and palette, region serialization,
/// the block registry, and mod discovery / definition merging.
///
/// Implements the interfaces declared by MadFallModAPI.
/// </summary>
public class MadFallCore : ModuleRules
{
	public MadFallCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bWarningsAsErrors = true;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"MadFallModAPI"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"NetCore",
			"AssetRegistry",
			"DeveloperSettings",
			"Json",
			"JsonUtilities"
		});
	}
}
