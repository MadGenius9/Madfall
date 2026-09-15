// Copyright MadFall. All Rights Reserved.

#include "MadChunkMeshComponent.h"

#include "MadFallMesher.h"
#include "MadFallStats.h"
#include "Materials/MaterialInterface.h"

DECLARE_CYCLE_STAT(TEXT("Mesh Apply (game thread)"), STAT_MadMeshApply, STATGROUP_MadFallMesher);

UMadChunkMeshComponent::UMadChunkMeshComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false;

	// Chaos collision cooking is the single most expensive part of publishing a
	// rebuilt chunk. Doing it synchronously would put tens of milliseconds on
	// the game thread every time a player mined a block.
	bUseAsyncCooking = true;

	bUseComplexAsSimpleCollision = true;
	SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);

	SetCastShadow(true);
	bCastDynamicShadow = true;

	// Generated geometry has no lightmap UVs and is rebuilt at runtime.
	bAffectDistanceFieldLighting = false;
	SetMobility(EComponentMobility::Movable);
}

void UMadChunkMeshComponent::ApplyChunkMesh(const FMadChunkMesh& Mesh, TFunctionRef<UMaterialInterface*(FName MaterialClass)> MaterialFor)
{
	SCOPE_CYCLE_COUNTER(STAT_MadMeshApply);
	TRACE_CPUPROFILER_EVENT_SCOPE(MadFall::ApplyChunkMesh);

	check(IsInGameThread());

	ChunkCoord = Mesh.Coord;

	const double ApplyStart = FPlatformTime::Seconds();
	double ConvertSeconds = 0.0;
	double CreateSeconds = 0.0;
	// ClearAllMeshSections rather than updating in place: section count and
	// material assignment both change when the palette does, and an update path
	// that has to handle "this section no longer exists" is more code than a
	// rebuild is worth at this size.
	ClearAllMeshSections();

	int32 SectionIndex = 0;
	int32 TotalVertices = 0;
	int32 TotalTriangles = 0;

	// One component section per MATERIAL, not per material class. The mesher
	// keys sections by class so a surface can swap in its own material, but
	// classes without one all draw with the same voxel material and differ only
	// in vertex colour. Each component section is a CreateMeshSection call, a
	// collision cook and a draw call: a forest chunk (grass, dirt, stone, bark,
	// leaves, ...) measured 6-9 sections and 1.6-1.8 ms of creation, over the
	// frame budget on its own. Merged, it is one or two.
	struct FGroup
	{
		UMaterialInterface* Material = nullptr;
		TArray<const FMadMeshSection*, TInlineAllocator<8>> Sections;
	};
	TArray<FGroup, TInlineAllocator<4>> Groups;
	for (const FMadMeshSection& Section : Mesh.Sections)
	{
		if (Section.IsEmpty())
		{
			continue;
		}
		UMaterialInterface* Material = MaterialFor(Section.MaterialClass);
		FGroup* Group = Groups.FindByPredicate([Material](const FGroup& G) { return G.Material == Material; });
		if (Group == nullptr)
		{
			Group = &Groups.AddDefaulted_GetRef();
			Group->Material = Material;
		}
		Group->Sections.Add(&Section);
	}

	for (const FGroup& Group : Groups)
	{
		// ProceduralMeshComponent wants doubles and its own tangent type, so
		// this is where the mesher's compact float types get widened. Keeping
		// the mesher on FVector3f halves the memory a queued rebuild holds.
		TArray<FVector> Positions;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FVector2D> OcclusionUVs;
		TArray<FColor> Colors;
		TArray<FProcMeshTangent> Tangents;
		TArray<int32> Triangles;

		const double ConvertStart = FPlatformTime::Seconds();
		int32 VertexCount = 0;
		int32 IndexCount = 0;
		for (const FMadMeshSection* Section : Group.Sections)
		{
			VertexCount += Section->NumVertices();
			IndexCount += Section->Indices.Num();
		}
		Positions.Reserve(VertexCount);
		Normals.Reserve(VertexCount);
		UVs.Reserve(VertexCount);
		OcclusionUVs.Reserve(VertexCount);
		Colors.Reserve(VertexCount);
		Tangents.Reserve(VertexCount);
		Triangles.Reserve(IndexCount);

		for (const FMadMeshSection* Section : Group.Sections)
		{
			const int32 Base = Positions.Num();
			for (int32 Index = 0; Index < Section->NumVertices(); ++Index)
			{
				Positions.Add(FVector(Section->Positions[Index]));
				Normals.Add(FVector(Section->Normals[Index]));
				UVs.Add(FVector2D(Section->UVs[Index]));
				const uint8 Occlusion = Section->Occlusion[Index];
				OcclusionUVs.Add(FVector2D((Occlusion & 3) / 3.0, (Occlusion & FMadMeshSection::CubicFaceFlag) != 0 ? 1.0 : 0.0));
				Tangents.Add(FProcMeshTangent(FVector(Section->Tangents[Index]), false));
			}
			Colors.Append(Section->Colors);
			for (uint32 Index : Section->Indices)
			{
				Triangles.Add(Base + static_cast<int32>(Index));
			}
			TotalTriangles += Section->NumTriangles();
		}

		// Material BEFORE geometry.
		//
		// CreateMeshSection marks the render state dirty, and the scene proxy is
		// rebuilt from whatever GetMaterial() returns at that moment. Setting the
		// material afterwards left the proxy holding a null material, and Unreal
		// substitutes its grey checker default for null - which looks exactly
		// like a mesher bug and is not one. Assigning first means there is no
		// window in which the section exists without its material.
		ConvertSeconds += FPlatformTime::Seconds() - ConvertStart;
		const double CreateStart = FPlatformTime::Seconds();
		if (Group.Material != nullptr)
		{
			SetMaterial(SectionIndex, Group.Material);
		}

		// Corner occlusion rides in UV1 because vertex colour is full: RGB is the
		// surface colour and A its pattern. A material without a UV1 read (a mod's
		// own surface material) simply ignores it.
		CreateMeshSection(SectionIndex, Positions, Triangles, Normals, UVs, OcclusionUVs, TArray<FVector2D>(), TArray<FVector2D>(),
			Colors, Tangents, /*bCreateCollision*/ true);
		CreateSeconds += FPlatformTime::Seconds() - CreateStart;

		TotalVertices += VertexCount;
		++SectionIndex;
	}

	const double TotalMs = (FPlatformTime::Seconds() - ApplyStart) * 1000.0;
	if (TotalMs > 2.0)
	{
		UE_LOG(LogMadFallMesher, Display, TEXT("Slow apply %s: %.2f ms total, %d sections, %d verts - convert %.2f ms, create sections %.2f ms."),
			*Mesh.Coord.ToString(), TotalMs, SectionIndex, TotalVertices, ConvertSeconds * 1000.0, CreateSeconds * 1000.0);
	}

	LastSectionCount = SectionIndex;
	LastVertexCount = TotalVertices;
	LastTriangleCount = TotalTriangles;

	UE_LOG(LogMadFallMesher, Verbose,
		TEXT("Chunk %s meshed: %d sections, %d verts, %d tris in %.2f ms (iso %.2f, cubic %.2f)."),
		*Mesh.Coord.ToString(), SectionIndex, TotalVertices, TotalTriangles,
		Mesh.BuildMilliseconds, Mesh.IsosurfaceMilliseconds, Mesh.CubicMilliseconds);

	// What the component ACTUALLY ended up with, which is not always what was
	// passed in: a material whose shaders are unavailable for the running
	// platform is silently swapped for the engine default at render time, and
	// that is indistinguishable from a mesher bug in a screenshot.
	if (SectionIndex > 0)
	{
		const UMaterialInterface* Applied = GetMaterial(0);
		UE_LOG(LogMadFallMesher, Log, TEXT("Chunk %s section 0 material: %s"),
			*Mesh.Coord.ToString(),
			Applied ? *Applied->GetPathName() : TEXT("(none - will render as the engine default)"));
	}
}
