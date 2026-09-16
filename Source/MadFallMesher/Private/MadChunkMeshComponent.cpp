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

void UMadChunkMeshComponent::Prepare(FMadChunkMesh& Mesh)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MadFall::PrepareChunkMesh);

	TSharedPtr<FMadPreparedChunkMesh, ESPMode::ThreadSafe> Prepared = MakeShared<FMadPreparedChunkMesh, ESPMode::ThreadSafe>();
	Prepared->Sections.SetNum(Mesh.Sections.Num());
	for (int32 SectionIndex = 0; SectionIndex < Mesh.Sections.Num(); ++SectionIndex)
	{
		const FMadMeshSection& Source = Mesh.Sections[SectionIndex];
		FProcMeshSection& Ready = Prepared->Sections[SectionIndex];
		Ready.bEnableCollision = true;
		Ready.bSectionVisible = true;
		Ready.ProcVertexBuffer.SetNum(Source.NumVertices());
		for (int32 Index = 0; Index < Source.NumVertices(); ++Index)
		{
			FProcMeshVertex& Vertex = Ready.ProcVertexBuffer[Index];
			Vertex.Position = FVector(Source.Positions[Index]);
			Vertex.Normal = FVector(Source.Normals[Index]);
			Vertex.Tangent = FProcMeshTangent(FVector(Source.Tangents[Index]), false);
			Vertex.Color = Source.Colors[Index];
			Vertex.UV0 = FVector2D(Source.UVs[Index]);
			const uint8 Occlusion = Source.Occlusion[Index];
			Vertex.UV1 = FVector2D((Occlusion & 3) / 3.0, (Occlusion & FMadMeshSection::CubicFaceFlag) != 0 ? 1.0 : 0.0);
			// Damage in UV2.x, 0 whole to 1 destroyed, for the material's cracks.
			Vertex.UV2 = FVector2D(Source.Damage.IsValidIndex(Index) ? Source.Damage[Index] / 255.0 : 0.0, 0.0);
			Vertex.UV3 = FVector2D::ZeroVector;
			Ready.SectionLocalBox += Vertex.Position;
		}
		Ready.ProcIndexBuffer = Source.Indices;
	}
	Mesh.Prepared = Prepared;
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

	// Normally prepared on the meshing worker; a mesh applied straight from a
	// synchronous rebuild (tests, console) is converted here.
	TSharedPtr<FMadPreparedChunkMesh, ESPMode::ThreadSafe> Prepared = Mesh.Prepared;
	const double ConvertStart = FPlatformTime::Seconds();
	if (!Prepared.IsValid() || Prepared->Sections.Num() != Mesh.Sections.Num())
	{
		FMadChunkMesh Copy = Mesh;
		Prepare(Copy);
		Prepared = Copy.Prepared;
	}
	ConvertSeconds += FPlatformTime::Seconds() - ConvertStart;

	for (const FGroup& Group : Groups)
	{
		const double MergeStart = FPlatformTime::Seconds();
		FProcMeshSection Merged;
		const FProcMeshSection* Section = nullptr;
		if (Group.Sections.Num() == 1)
		{
			Section = &Prepared->Sections[static_cast<int32>(Group.Sections[0] - Mesh.Sections.GetData())];
		}
		else
		{
			// Sections sharing a material become one: vertices appended, indices offset.
			int32 VertexCount = 0;
			int32 IndexCount = 0;
			for (const FMadMeshSection* Source : Group.Sections)
			{
				const FProcMeshSection& Ready = Prepared->Sections[static_cast<int32>(Source - Mesh.Sections.GetData())];
				VertexCount += Ready.ProcVertexBuffer.Num();
				IndexCount += Ready.ProcIndexBuffer.Num();
			}
			Merged.ProcVertexBuffer.Reserve(VertexCount);
			Merged.ProcIndexBuffer.Reserve(IndexCount);
			for (const FMadMeshSection* Source : Group.Sections)
			{
				const FProcMeshSection& Ready = Prepared->Sections[static_cast<int32>(Source - Mesh.Sections.GetData())];
				const uint32 Base = static_cast<uint32>(Merged.ProcVertexBuffer.Num());
				Merged.ProcVertexBuffer.Append(Ready.ProcVertexBuffer);
				for (uint32 Index : Ready.ProcIndexBuffer)
				{
					Merged.ProcIndexBuffer.Add(Base + Index);
				}
				Merged.SectionLocalBox += Ready.SectionLocalBox;
			}
			Merged.bEnableCollision = true;
			Merged.bSectionVisible = true;
			Section = &Merged;
		}
		ConvertSeconds += FPlatformTime::Seconds() - MergeStart;

		// Material BEFORE geometry.
		//
		// Setting a section marks the render state dirty, and the scene proxy is
		// rebuilt from whatever GetMaterial() returns at that moment. Setting the
		// material afterwards left the proxy holding a null material, and Unreal
		// substitutes its grey checker default for null - which looks exactly
		// like a mesher bug and is not one. Assigning first means there is no
		// window in which the section exists without its material.
		const double CreateStart = FPlatformTime::Seconds();
		if (Group.Material != nullptr)
		{
			SetMaterial(SectionIndex, Group.Material);
		}
		// Corner occlusion rides in UV1 because vertex colour is full: RGB is the
		// surface colour and A its pattern or texture layer. A material without a
		// UV1 read (a mod's own surface material) simply ignores it.
		SetProcMeshSection(SectionIndex, *Section);
		CreateSeconds += FPlatformTime::Seconds() - CreateStart;

		for (const FMadMeshSection* Source : Group.Sections)
		{
			TotalTriangles += Source->NumTriangles();
		}
		TotalVertices += Section->ProcVertexBuffer.Num();
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
