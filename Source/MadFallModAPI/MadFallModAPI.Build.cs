// Copyright MadFall. All Rights Reserved.

using UnrealBuildTool;

/// <summary>
/// MadFallModAPI - the stable public surface mods compile and script against.
///
/// WHY THIS MODULE IS AT THE BOTTOM OF THE DEPENDENCY GRAPH:
///
/// 1. It physically cannot leak internal types. It does not reference
///    MadFallCore, so a refactor of FMadChunkStorage cannot reach a modder.
///    The stable surface is enforced by the build graph, not by discipline.
///
/// 2. It can load at PostConfigInit, which is the only phase early enough to
///    mount Tier-2 mod .pak files BEFORE the asset registry scans. A module
///    that depended on Engine could not load that early.
///
/// 3. In Phase 5 the Lua sandbox allow-list is "everything in this module's
///    Public folder, nothing else" - which is checkable by a script.
///
/// The cost, accepted deliberately: this module owns POD mirror types
/// (FMadVoxel, FMadBlockDefView, ...) rather than reusing Core's. Duplication
/// is the price of a surface that can hold still across versions.
///
/// DO NOT add "Engine" here. If you need Engine, the code belongs in
/// MadFallCore or higher.
/// </summary>
public class MadFallModAPI : ModuleRules
{
	public MadFallModAPI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bWarningsAsErrors = true;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"JsonUtilities",
			"PakFile",
			"Projects"
		});
	}
}
