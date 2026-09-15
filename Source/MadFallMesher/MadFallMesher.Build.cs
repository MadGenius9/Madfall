// Copyright MadFall. All Rights Reserved.

using UnrealBuildTool;

/// <summary>
/// MadFallMesher - Dual Contouring / Surface Nets for terrain, greedy meshing
/// for cubic construction, LOD and seam stitching, async Chaos collision cooking.
///
/// Reads voxel data, never writes it. All jobs run on worker threads; the game
/// thread only swaps in finished results.
/// </summary>
public class MadFallMesher : ModuleRules
{
	public MadFallMesher(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bWarningsAsErrors = true;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"MadFallCore",
			"ProceduralMeshComponent"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore",
			"RHI",
			"Chaos",
			"GeometryCore",
			"MeshDescription",
			"StaticMeshDescription",
			// Tests parse block JSON directly.
			"Json"
		});
	}
}
