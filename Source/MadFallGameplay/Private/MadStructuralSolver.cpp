// Copyright MadFall. All Rights Reserved.

#include "MadStructuralSolver.h"

#include "Algo/Sort.h"
#include "MadBlockRegistry.h"
#include "MadChunkStorage.h"
#include "MadFallCoordinates.h"
#include "MadFallStats.h"

DECLARE_CYCLE_STAT(TEXT("Structural Gather"), STAT_MadStructuralGather, STATGROUP_MadFallStructural);
DECLARE_CYCLE_STAT(TEXT("Structural Solve"), STAT_MadStructuralSolve, STATGROUP_MadFallStructural);
DECLARE_CYCLE_STAT(TEXT("Structural Load"), STAT_MadStructuralLoad, STATGROUP_MadFallStructural);

namespace
{
	/** +X, -X, +Y, -Y, +Z, -Z. Opposite of d is d ^ 1. */
	const FIntVector DirectionOffsets[6] =
	{
		FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
		FIntVector(0, 1, 0), FIntVector(0, -1, 0),
		FIntVector(0, 0, 1), FIntVector(0, 0, -1)
	};

	constexpr int32 DirUp = 4;
	constexpr int32 DirDown = 5;

	FORCEINLINE int32 Opposite(int32 Dir) { return Dir ^ 1; }

	/** Min-heap ordering: distance, then height, then node index for total determinism. */
	struct FHeapLess
	{
		template <typename T>
		bool operator()(const T& A, const T& B) const
		{
			if (A.Distance != B.Distance) { return A.Distance < B.Distance; }
			if (A.Z != B.Z) { return A.Z < B.Z; }
			return A.Node < B.Node;
		}
	};

	/** Saturating add for path distances. */
	FORCEINLINE int32 AddCost(int32 A, int32 B)
	{
		const int64 Sum = static_cast<int64>(A) + static_cast<int64>(B);
		return Sum >= MAX_int32 ? MAX_int32 : static_cast<int32>(Sum);
	}
}

// ===========================================================================
// Materials
// ===========================================================================

int32 FMadStructuralMaterial::GetStageIndex(uint8 Damage) const
{
	int32 Result = 0;
	for (int32 Index = 0; Index < Stages.Num(); ++Index)
	{
		if (Stages[Index].Key <= Damage)
		{
			Result = Index;
		}
		else
		{
			break;
		}
	}
	return Result;
}

float FMadStructuralMaterial::GetMultiplier(uint8 Damage) const
{
	if (Stages.Num() == 0)
	{
		return 1.0f;
	}

	// Stages are validated to start at 0, but a mod could still ship one that
	// does not; below the first threshold the block is intact.
	if (Damage < Stages[0].Key)
	{
		return 1.0f;
	}
	return Stages[GetStageIndex(Damage)].Value;
}

void FMadStructuralMaterials::Build(const FMadBlockRegistry& Registry)
{
	Materials.Reset();

	UnknownAnchor = FMadStructuralMaterial();
	UnknownAnchor.bKnown = false;
	UnknownAnchor.bIsAnchor = true;

	int32 MaxId = 0;
	for (const FMadBlockEntry& Entry : Registry.GetEntries())
	{
		MaxId = FMath::Max(MaxId, static_cast<int32>(Entry.RuntimeId));
	}
	Materials.SetNum(MaxId + 1);

	for (const FMadBlockEntry& Entry : Registry.GetEntries())
	{
		const FMadBlockDefinitionData& Def = Entry.Definition;

		FMadStructuralMaterial& Material = Materials[Entry.RuntimeId];
		Material.bKnown = !Entry.bUnresolved;
		Material.bIsAnchor = Def.bIsAnchor || Entry.bUnresolved;
		Material.MassKg = Def.MassKg;
		Material.SupportStrength = Def.SupportStrength;
		Material.MaxHorizontalSpan = Def.MaxHorizontalSpan;
		Material.bSkipLoadCheck = Def.Tags.Contains(FName(TEXT("block.natural")));

		for (const FMadBlockDamageStage& Stage : Def.DamageStages)
		{
			Material.Stages.Emplace(static_cast<uint8>(FMath::Clamp(Stage.At, 0, 255)),
				FMath::Clamp(Stage.SupportMultiplier, 0.0f, 1.0f));
		}
		Algo::SortBy(Material.Stages, [](const TPair<uint8, float>& Stage) { return Stage.Key; });
	}
}

void FMadStructuralMaterials::Set(uint16 RuntimeId, const FMadStructuralMaterial& Material)
{
	if (Materials.Num() <= RuntimeId)
	{
		Materials.SetNum(RuntimeId + 1);
	}
	Materials[RuntimeId] = Material;

	UnknownAnchor.bKnown = false;
	UnknownAnchor.bIsAnchor = true;
}

const FMadStructuralMaterial& FMadStructuralMaterials::Get(uint16 RuntimeId) const
{
	if (Materials.IsValidIndex(RuntimeId) && Materials[RuntimeId].bKnown)
	{
		return Materials[RuntimeId];
	}
	return UnknownAnchor;
}

// ===========================================================================
// Job
// ===========================================================================

FMadStructuralJob::FMadStructuralJob(TArray<FIntVector> InSeeds, const FMadStructuralSettings& InSettings)
	: Seeds(MoveTemp(InSeeds))
	, Settings(InSettings)
{
}

EMadStructuralRole FMadStructuralJob::Classify(const FMadVoxel& Voxel, const FMadStructuralMaterials& Materials)
{
	if (Voxel.IsAir() || !Voxel.IsSolid() || Voxel.HasFlag(EMadVoxelFlags::Liquid))
	{
		return EMadStructuralRole::Empty;
	}

	// Natural terrain: see the header on why it never participates.
	if (!Voxel.HasFlag(EMadVoxelFlags::Cubic))
	{
		return EMadStructuralRole::Anchor;
	}

	const FMadStructuralMaterial& Material = Materials.Get(Voxel.BlockTypeID);
	if (!Material.bKnown || Material.bIsAnchor)
	{
		return EMadStructuralRole::Anchor;
	}

	return EMadStructuralRole::Member;
}

EMadStructuralRole FMadStructuralJob::ClassifyAt(const IMadStructuralWorld& World,
	const FMadStructuralMaterials& Materials, const FIntVector& Position, FMadVoxel& OutVoxel) const
{
	if (!World.IsLoaded(Position))
	{
		OutVoxel = FMadVoxel();
		return EMadStructuralRole::Anchor;
	}

	OutVoxel = World.GetVoxel(Position);
	return Classify(OutVoxel, Materials);
}

bool FMadStructuralJob::Touches(const FIntVector& Position) const
{
	if (NodeIndex.Contains(Position))
	{
		return true;
	}

	for (const FIntVector& Offset : DirectionOffsets)
	{
		if (NodeIndex.Contains(Position + Offset))
		{
			return true;
		}
	}
	return false;
}

int32 FMadStructuralJob::EdgeCost(int32 Index, int32 Dir) const
{
	const FNode& Node = Nodes[Index];

	if (Dir == DirDown)
	{
		// Resting on something. Span is irrelevant; whether the thing below can
		// take the weight is the load pass's question, not this one.
		return 0;
	}

	if (Node.StepCost == MAX_int32)
	{
		return MAX_int32;
	}

	if (Dir == DirUp)
	{
		const double Hanging = static_cast<double>(Node.StepCost) * Settings.HangingCostMultiplier;
		return Hanging >= static_cast<double>(MAX_int32) ? MAX_int32 : FMath::Max(1, static_cast<int32>(Hanging));
	}

	return Node.StepCost;
}

bool FMadStructuralJob::Step(const IMadStructuralWorld& World, const FMadStructuralMaterials& Materials, int32 WorkUnits)
{
	int32 Budget = FMath::Max(1, WorkUnits);

	while (Budget > 0 && Phase != EPhase::Done)
	{
		switch (Phase)
		{
		case EPhase::Gather:
			if (StepGather(World, Materials, Budget))
			{
				Phase = bTruncated ? EPhase::Done : EPhase::Solve;
			}
			break;

		case EPhase::Solve:
			if (StepSolve(Budget))
			{
				LoadCursor = SettleOrder.Num() - 1;
				Phase = EPhase::Load;
			}
			break;

		case EPhase::Load:
			if (StepLoad(Budget))
			{
				Finish();
				Phase = EPhase::Done;
			}
			break;

		default:
			break;
		}
	}

	return Phase == EPhase::Done;
}

void FMadStructuralJob::RunToCompletion(const IMadStructuralWorld& World, const FMadStructuralMaterials& Materials)
{
	while (!Step(World, Materials, 1 << 20))
	{
	}
}

bool FMadStructuralJob::StepGather(const IMadStructuralWorld& World, const FMadStructuralMaterials& Materials, int32& Budget)
{
	SCOPE_CYCLE_COUNTER(STAT_MadStructuralGather);

	auto AddNode = [this, &Materials](const FIntVector& Position, const FMadVoxel& Voxel) -> int32
	{
		const FMadStructuralMaterial& Material = Materials.Get(Voxel.BlockTypeID);
		const float Multiplier = Material.GetMultiplier(Voxel.Damage);

		FNode Node;
		Node.Position = Position;
		Node.Voxel = Voxel;
		for (int32& Neighbour : Node.Neighbours)
		{
			Neighbour = NeighbourEmpty;
		}

		// Integer step cost. With an intact block and span <= 16 this divides
		// SupportScale exactly; see the header.
		const double EffectiveSpan = static_cast<double>(Material.MaxHorizontalSpan) * Multiplier;
		Node.StepCost = EffectiveSpan >= 1.0
			? FMath::Max(1, static_cast<int32>(FMath::FloorToDouble(MadFall::Structural::SupportScale / EffectiveSpan)))
			: MAX_int32;

		Node.MassKg = Material.MassKg;
		Node.CapacityKg = Material.SupportStrength * Multiplier;
		Node.bCanSupport = Multiplier > 0.0f;

		const int32 Index = Nodes.Add(MoveTemp(Node));
		NodeIndex.Add(Position, Index);
		return Index;
	};

	// Seeds first. A seed that is not a member (a removed block, say) still
	// matters through its neighbours, which is why callers pass the neighbours
	// in too rather than the solver guessing.
	while (SeedCursor < Seeds.Num() && Budget > 0)
	{
		const FIntVector& Seed = Seeds[SeedCursor++];
		--Budget;

		if (NodeIndex.Contains(Seed))
		{
			continue;
		}

		FMadVoxel Voxel;
		if (ClassifyAt(World, Materials, Seed, Voxel) == EMadStructuralRole::Member)
		{
			if (Nodes.Num() >= Settings.MaxComponentSize)
			{
				bTruncated = true;
				return true;
			}
			AddNode(Seed, Voxel);
		}
	}

	if (SeedCursor < Seeds.Num())
	{
		return false;
	}

	// Breadth-first over the connected members, recording every neighbour's role
	// so later phases never read the world again.
	while (GatherCursor < Nodes.Num() && Budget > 0)
	{
		const int32 Current = GatherCursor++;
		--Budget;

		for (int32 Dir = 0; Dir < 6; ++Dir)
		{
			const FIntVector NeighbourPos = Nodes[Current].Position + DirectionOffsets[Dir];

			if (const int32* Existing = NodeIndex.Find(NeighbourPos))
			{
				Nodes[Current].Neighbours[Dir] = *Existing;
				// And the reverse. Gather spans frames, so the existing node may
				// have been processed while this position's chunk was still
				// unloaded - and recorded it as an anchor. Without this the edge is
				// one-way, the solve never flows support back across it, and a
				// block beside a chunk that streamed in mid-job collapsed as
				// "unsupported" (found as single leaves dropping out of forests).
				Nodes[*Existing].Neighbours[Dir ^ 1] = Current;
				continue;
			}

			FMadVoxel Voxel;
			switch (ClassifyAt(World, Materials, NeighbourPos, Voxel))
			{
			case EMadStructuralRole::Member:
			{
				if (Nodes.Num() >= Settings.MaxComponentSize)
				{
					bTruncated = true;
					return true;
				}
				// AddNode may reallocate Nodes; index, never hold a reference across it.
				const int32 Added = AddNode(NeighbourPos, Voxel);
				Nodes[Current].Neighbours[Dir] = Added;
				break;
			}
			case EMadStructuralRole::Anchor:
				Nodes[Current].Neighbours[Dir] = NeighbourAnchor;
				break;
			default:
				Nodes[Current].Neighbours[Dir] = NeighbourEmpty;
				break;
			}
		}
	}

	return GatherCursor >= Nodes.Num();
}

bool FMadStructuralJob::StepSolve(int32& Budget)
{
	SCOPE_CYCLE_COUNTER(STAT_MadStructuralSolve);

	const int32 NumNodes = Nodes.Num();

	if (!bSolveInitialised)
	{
		bSolveInitialised = true;
		Settled.Init(false, NumNodes);
		SettleOrder.Reserve(NumNodes);
		Heap.Reserve(NumNodes);

		// Multi-source: every member touching an anchor starts at the cost of
		// receiving support from that anchor.
		for (int32 Index = 0; Index < NumNodes; ++Index)
		{
			int32 Best = MadFall::Structural::Unreachable;
			for (int32 Dir = 0; Dir < 6; ++Dir)
			{
				if (Nodes[Index].Neighbours[Dir] == NeighbourAnchor)
				{
					Best = FMath::Min(Best, EdgeCost(Index, Dir));
				}
			}

			if (Best <= MadFall::Structural::SupportScale)
			{
				Nodes[Index].Distance = Best;
				Heap.HeapPush(FHeapEntry{ Best, Nodes[Index].Position.Z, Index }, FHeapLess());
			}
		}

		Budget -= FMath::Max(1, NumNodes / 16);
		return false;
	}

	while (Heap.Num() > 0 && Budget > 0)
	{
		FHeapEntry Entry;
		Heap.HeapPop(Entry, FHeapLess(), EAllowShrinking::No);
		--Budget;

		// Lazy deletion: a node may be in the heap several times.
		if (Settled[Entry.Node] || Entry.Distance != Nodes[Entry.Node].Distance)
		{
			continue;
		}

		Settled[Entry.Node] = true;
		SettleOrder.Add(Entry.Node);

		if (!Nodes[Entry.Node].bCanSupport)
		{
			continue;
		}

		for (int32 Dir = 0; Dir < 6; ++Dir)
		{
			const int32 Neighbour = Nodes[Entry.Node].Neighbours[Dir];
			if (Neighbour < 0 || Settled[Neighbour])
			{
				continue;
			}

			// The neighbour receives support from this node, which lies in the
			// opposite direction from its point of view.
			const int32 Cost = EdgeCost(Neighbour, Opposite(Dir));
			const int32 Candidate = AddCost(Entry.Distance, Cost);

			if (Candidate <= MadFall::Structural::SupportScale && Candidate < Nodes[Neighbour].Distance)
			{
				Nodes[Neighbour].Distance = Candidate;
				Heap.HeapPush(FHeapEntry{ Candidate, Nodes[Neighbour].Position.Z, Neighbour }, FHeapLess());
			}
		}
	}

	return Heap.Num() == 0;
}

bool FMadStructuralJob::StepLoad(int32& Budget)
{
	SCOPE_CYCLE_COUNTER(STAT_MadStructuralLoad);

	// Reverse settle order visits every node after everything it supports.
	while (LoadCursor >= 0 && Budget > 0)
	{
		const int32 Index = SettleOrder[LoadCursor--];
		--Budget;

		FNode& Node = Nodes[Index];

		if (Settings.bCheckLoad && Node.Accumulated > Node.CapacityKg)
		{
			Node.Failure = EMadStructuralFailure::Overloaded;
		}
		else
		{
			// Everything this node supports was visited first, so its load is final.
			const float Stress = NodeStress(Node);
			if (!bHasMostStressed || Stress > MostStressed.Stress)
			{
				MostStressed = FMadStressSample{ Node.Position, Stress, false };
				bHasMostStressed = true;
			}
		}

		// Collect tight supporters: the neighbours this node's best path came
		// through. Equal splitting among them is what spreads a floor's weight
		// over every column it is equally close to, instead of dumping it all on
		// whichever one the heap happened to settle first.
		int32 TightNodes[6];
		int32 NumTightNodes = 0;
		int32 NumTightAnchors = 0;

		for (int32 Dir = 0; Dir < 6; ++Dir)
		{
			const int32 Neighbour = Node.Neighbours[Dir];
			const int32 Cost = EdgeCost(Index, Dir);
			if (Cost == MAX_int32)
			{
				continue;
			}

			if (Neighbour == NeighbourAnchor)
			{
				if (Cost == Node.Distance)
				{
					++NumTightAnchors;
				}
			}
			else if (Neighbour >= 0)
			{
				const FNode& Supporter = Nodes[Neighbour];
				if (Supporter.bCanSupport && Supporter.Distance != MadFall::Structural::Unreachable
					&& AddCost(Supporter.Distance, Cost) == Node.Distance)
				{
					TightNodes[NumTightNodes++] = Neighbour;
				}
			}
		}

		const int32 NumTight = NumTightNodes + NumTightAnchors;
		if (NumTight == 0)
		{
			continue;
		}

		const float Share = (Node.MassKg + Node.Accumulated) / static_cast<float>(NumTight);
		for (int32 T = 0; T < NumTightNodes; ++T)
		{
			Nodes[TightNodes[T]].Accumulated += Share;
		}
	}

	return LoadCursor < 0;
}

void FMadStructuralJob::Finish()
{
	Failures.Reset();

	for (FNode& Node : Nodes)
	{
		if (Node.Distance == MadFall::Structural::Unreachable)
		{
			Node.Failure = EMadStructuralFailure::Unsupported;
		}

		if (Node.Failure != EMadStructuralFailure::None)
		{
			FMadStructuralFailureRecord& Record = Failures.AddDefaulted_GetRef();
			Record.Position = Node.Position;
			Record.Voxel = Node.Voxel;
			Record.Reason = Node.Failure;
		}
	}

	// The heap and settle bookkeeping are not needed for reports.
	Heap.Empty();
	Settled.Empty();
}

bool FMadStructuralJob::GetNodeReport(const FIntVector& Position, FMadStructuralNodeReport& OutReport) const
{
	const int32* Index = NodeIndex.Find(Position);
	if (Index == nullptr || Phase != EPhase::Done)
	{
		return false;
	}

	const FNode& Node = Nodes[*Index];
	OutReport.SupportDistance = Node.Distance;
	OutReport.CarriedKg = Node.Accumulated;
	OutReport.CapacityKg = Node.CapacityKg;
	OutReport.Failure = Node.Failure;
	OutReport.Stress = NodeStress(Node);
	return true;
}

float FMadStructuralJob::NodeStress(const FNode& Node)
{
	const float SpanUse = Node.Distance == MadFall::Structural::Unreachable
		? 1.0f
		: static_cast<float>(Node.Distance) / static_cast<float>(MadFall::Structural::SupportScale);
	const float LoadUse = Node.CapacityKg > 0.0f
		? Node.Accumulated / Node.CapacityKg
		: (Node.Accumulated > 0.0f ? 1.0f : 0.0f);
	return FMath::Max(SpanUse, LoadUse);
}

void FMadStructuralJob::CollectStressNear(const FIntVector& Centre, int32 Radius, int32 MaxCount, TArray<FMadStressSample>& OutSamples) const
{
	OutSamples.Reset();
	if (Phase != EPhase::Done || MaxCount <= 0)
	{
		return;
	}
	TArray<TPair<int32, int32>> Near;   // squared distance, node index
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		const FIntVector Delta = Nodes[Index].Position - Centre;
		if (FMath::Abs(Delta.X) <= Radius && FMath::Abs(Delta.Y) <= Radius && FMath::Abs(Delta.Z) <= Radius)
		{
			Near.Emplace(Delta.X * Delta.X + Delta.Y * Delta.Y + Delta.Z * Delta.Z, Index);
		}
	}
	// Stable, so equally near members keep gather order and the result is deterministic.
	Near.StableSort([](const TPair<int32, int32>& A, const TPair<int32, int32>& B) { return A.Key < B.Key; });
	const int32 Count = FMath::Min(Near.Num(), MaxCount);
	OutSamples.Reserve(Count);
	for (int32 I = 0; I < Count; ++I)
	{
		const FNode& Node = Nodes[Near[I].Value];
		OutSamples.Add(FMadStressSample{ Node.Position, NodeStress(Node), Node.Failure != EMadStructuralFailure::None });
	}
}

namespace MadFall::Structural
{
	void FindMembersInChunk(const FMadChunkStorage& Storage, const FMadChunkCoord& Coord,
		const FMadStructuralMaterials& Materials, TArray<FIntVector>& OutMembers)
	{
		OutMembers.Reset();

		TArray<uint16, TInlineAllocator<16>> Candidates;
		for (const FMadBlockPaletteEntry& Entry : Storage.GetPalette())
		{
			// A uniform chunk keeps its one entry at RefCount 0; include it and let
			// the per-voxel classification decide.
			const FMadStructuralMaterial& Material = Materials.Get(Entry.RuntimeId);
			if (Material.bKnown && !Material.bIsAnchor && !Material.bSkipLoadCheck && Entry.RuntimeId != MadFall::BlockTypeAir)
			{
				Candidates.AddUnique(Entry.RuntimeId);
			}
		}
		if (Candidates.Num() == 0)
		{
			return;
		}

		// No flags array means every voxel has the default flags: if that is not
		// Cubic, nothing here is construction, whatever the palette says.
		const uint8 CubicBit = static_cast<uint8>(EMadVoxelFlags::Cubic);
		const uint8* Flags = Storage.GetFlagsArray();
		if (Flags == nullptr && (Storage.GetDefaultFlags() & CubicBit) == 0)
		{
			return;
		}

		for (int32 Index = 0; Index < MadFall::ChunkVoxelCount; ++Index)
		{
			if (((Flags ? Flags[Index] : Storage.GetDefaultFlags()) & CubicBit) == 0 || !Candidates.Contains(Storage.GetBlockId(Index)))
			{
				continue;
			}
			if (FMadStructuralJob::Classify(Storage.GetVoxel(Index), Materials) != EMadStructuralRole::Member)
			{
				continue;
			}

			int32 LocalX, LocalY, LocalZ;
			MadFall::VoxelCoords(Index, LocalX, LocalY, LocalZ);
			int32 WorldX, WorldY, WorldZ;
			MadFall::ChunkToWorld(Coord, LocalX, LocalY, LocalZ, WorldX, WorldY, WorldZ);
			OutMembers.Emplace(WorldX, WorldY, WorldZ);
		}
	}
}

FString FMadStructuralJob::DescribeNeighbours(const FIntVector& Position) const
{
	const int32* Index = NodeIndex.Find(Position);
	if (Index == nullptr)
	{
		return TEXT("not gathered");
	}
	static const TCHAR* DirNames[6] = { TEXT("+X"), TEXT("-X"), TEXT("+Y"), TEXT("-Y"), TEXT("up"), TEXT("down") };
	FString Out;
	const FNode& Node = Nodes[*Index];
	for (int32 Dir = 0; Dir < 6; ++Dir)
	{
		const int32 Neighbour = Node.Neighbours[Dir];
		FString Role;
		if (Neighbour == NeighbourAnchor)
		{
			Role = TEXT("anchor");
		}
		else if (Neighbour == NeighbourEmpty)
		{
			Role = TEXT("empty");
		}
		else
		{
			Role = FString::Printf(TEXT("member d=%d"), Nodes[Neighbour].Distance);
		}
		Out += FString::Printf(TEXT("%s%s %s"), Dir == 0 ? TEXT("") : TEXT(", "), DirNames[Dir], *Role);
	}
	return Out;
}
