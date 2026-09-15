// Copyright MadFall. All Rights Reserved.

#include "MadMeshBuffers.h"

int32 FMadMeshSection::AddVertex(const FVector3f& Position, const FVector3f& Normal,
	const FVector2f& UV, const FColor& Color, uint8 InOcclusion)
{
	const int32 Index = Positions.Add(Position);
	Normals.Add(Normal);
	UVs.Add(UV);
	Colors.Add(Color);
	Occlusion.Add(InOcclusion);

	// A tangent perpendicular to the normal, chosen from whichever world axis
	// is least aligned with it. Generated geometry has no authored UV frame, so
	// any consistent perpendicular is as good as another - what matters is that
	// it is stable, because a tangent that flips between rebuilds makes normal
	// mapping shimmer.
	const FVector3f Reference = (FMath::Abs(Normal.Z) < 0.9f)
		? FVector3f(0.0f, 0.0f, 1.0f)
		: FVector3f(1.0f, 0.0f, 0.0f);

	FVector3f Tangent = FVector3f::CrossProduct(Reference, Normal);
	if (!Tangent.Normalize())
	{
		Tangent = FVector3f(1.0f, 0.0f, 0.0f);
	}
	Tangents.Add(Tangent);

	return Index;
}

int64 FMadMeshSection::GetAllocatedSize() const
{
	return Positions.GetAllocatedSize()
		+ Normals.GetAllocatedSize()
		+ Tangents.GetAllocatedSize()
		+ UVs.GetAllocatedSize()
		+ Colors.GetAllocatedSize()
		+ Occlusion.GetAllocatedSize()
		+ Indices.GetAllocatedSize();
}

bool FMadChunkMesh::IsEmpty() const
{
	for (const FMadMeshSection& Section : Sections)
	{
		if (!Section.IsEmpty())
		{
			return false;
		}
	}
	return true;
}

int32 FMadChunkMesh::TotalVertices() const
{
	int32 Total = 0;
	for (const FMadMeshSection& Section : Sections)
	{
		Total += Section.NumVertices();
	}
	return Total;
}

int32 FMadChunkMesh::TotalTriangles() const
{
	int32 Total = 0;
	for (const FMadMeshSection& Section : Sections)
	{
		Total += Section.NumTriangles();
	}
	return Total;
}

int64 FMadChunkMesh::GetAllocatedSize() const
{
	int64 Total = sizeof(FMadChunkMesh) + Sections.GetAllocatedSize();
	for (const FMadMeshSection& Section : Sections)
	{
		Total += Section.GetAllocatedSize();
	}
	return Total;
}

FMadMeshSection& FMadChunkMesh::FindOrAddSection(FName MaterialClass)
{
	for (FMadMeshSection& Section : Sections)
	{
		if (Section.MaterialClass == MaterialClass)
		{
			return Section;
		}
	}

	FMadMeshSection& Section = Sections.AddDefaulted_GetRef();
	Section.MaterialClass = MaterialClass;
	return Section;
}
