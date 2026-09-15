// Copyright MadFall. All Rights Reserved.

using UnrealBuildTool;

/// <summary>
/// MadFallGameplay - survival stats, GAS abilities, structural integrity solver,
/// block damage, crafting, loot, progression, zombie AI and the horde director.
///
/// Depends on MadFallMesher because structural collapse spawns falling debris
/// actors whose meshes are generated from a voxel sub-volume. Routing that
/// through an interface would be purity for its own sake - the two modules
/// ship and version together.
/// </summary>
public class MadFallGameplay : ModuleRules
{
	public MadFallGameplay(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bWarningsAsErrors = true;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			"MadFallCore",
			"MadFallMesher",
			"MadFallModAPI",
			"MadFallScriptRuntime"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AIModule",
			"NavigationSystem",
			"EnhancedInput",
			"InputCore",
			"Json",
			"NetCore",
			"Slate",
			"SlateCore"
		});
	}
}
