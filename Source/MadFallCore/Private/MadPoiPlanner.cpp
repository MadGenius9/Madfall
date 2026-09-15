// Copyright MadFall. All Rights Reserved.

#include "MadPoiPlanner.h"

#include "MadBiomeRegistry.h"
#include "MadBlockRegistry.h"
#include "MadFallCore.h"
#include "MadFallStats.h"
#include "MadNoise.h"
#include "MadOrientation.h"
#include "MadPrefabRegistry.h"
#include "MadWorldGenerator.h"
#include "Algo/Sort.h"
#include "Misc/ScopeRWLock.h"
#include "Misc/StringBuilder.h"

DECLARE_CYCLE_STAT(TEXT("WorldGen POI Plan"), STAT_MadPoiPlan, STATGROUP_MadFallWorldGen);
DECLARE_CYCLE_STAT(TEXT("WorldGen POI Stamp"), STAT_MadPoiStamp, STATGROUP_MadFallWorldGen);
DECLARE_CYCLE_STAT(TEXT("WorldGen Road Stamp"), STAT_MadRoadStamp, STATGROUP_MadFallWorldGen);

namespace
{
	constexpr uint16 VoidRuntimeId = MAX_uint16;
	using MadFall::ChunkSize;

	/** Headroom cleared above a road, so a road through a hillside is a cutting rather than a tunnel mouth. */
	constexpr int32 RoadClearance = 4;

	/** Deepest embankment filled under a road crossing a dip. */
	constexpr int32 RoadMaxFill = 6;

	FORCEINLINE bool ChunkContains(const FMadChunkCoord& Coord, int32 WorldX, int32 WorldY, int32 WorldZ)
	{
		return MadFall::FloorDiv(WorldX, ChunkSize) == Coord.X
			&& MadFall::FloorDiv(WorldY, ChunkSize) == Coord.Y
			&& MadFall::FloorDiv(WorldZ, ChunkSize) == Coord.Z;
	}
}

float MadFall::Roads::BankHeight(float RoadHeight, float TerrainHeight, float Beyond)
{
	if (Beyond <= 0.0f)
	{
		return RoadHeight;
	}
	const float Width = FMath::Clamp(FMath::Abs(TerrainHeight - RoadHeight) * BankRun, 1.0f, MaxBank);
	return FMath::Lerp(RoadHeight, TerrainHeight, FMath::SmoothStep(0.0f, Width, Beyond));
}

void FMadPoiInstance::GetWorldBounds(int32 FoundationDepth, FIntVector& OutMin, FIntVector& OutMax) const
{
	OutMin = FIntVector(Origin.X, Origin.Y, Origin.Z - FoundationDepth);
	OutMax = FIntVector(Origin.X + RotatedSize.X - 1, Origin.Y + RotatedSize.Y - 1, Origin.Z + RotatedSize.Z - 1);
}

// ===========================================================================
// Setup
// ===========================================================================

void FMadPoiPlanner::Initialize(const FMadWorldGenSettings& InSettings, const FMadPrefabRegistry* InPrefabs,
	const FMadBiomeRegistry& InBiomes, FMadBlockRegistry& InBlocks)
{
	Settings = &InSettings;
	Prefabs = InPrefabs;
	Biomes = &InBiomes;

	PoiSeed = MadFall::Noise::HashInt(InSettings.Seed ^ 0x9015EED0u);
	RoadSeed = MadFall::Noise::HashInt(InSettings.Seed ^ 0x40AD5EEDu);

	auto Resolve = [&InBlocks](FName Id, FName Fallback) -> uint16
	{
		uint16 RuntimeId = InBlocks.ResolveRuntimeId(Id);
		if (RuntimeId == MadFall::BlockTypeUnresolved)
		{
			RuntimeId = InBlocks.ResolveRuntimeId(Fallback);
		}
		return RuntimeId;
	};

	FoundationFallback = Resolve(FName(TEXT("madfall:concrete_frame")), FName(TEXT("madfall:stone")));
	RoadBlock = Resolve(FName(TEXT("madfall:gravel_path")), FName(TEXT("madfall:dirt")));
	RoadBedBlock = Resolve(FName(TEXT("madfall:dirt")), FName(TEXT("madfall:stone")));

	PaletteRuntimeIds.Reset();
	FoundationIds.Reset();

	if (Prefabs == nullptr)
	{
		return;
	}

	// Resolve every palette entry once. Per voxel would be a hash lookup for
	// every block of every building in every chunk.
	PaletteRuntimeIds.SetNum(Prefabs->Num());
	FoundationIds.SetNum(Prefabs->Num());

	for (int32 PrefabIndex = 0; PrefabIndex < Prefabs->Num(); ++PrefabIndex)
	{
		const FMadPrefab& Prefab = Prefabs->Get(PrefabIndex);
		TArray<uint16>& Ids = PaletteRuntimeIds[PrefabIndex];

		FoundationIds[PrefabIndex] = Prefab.Placement.FoundationBlock.IsNone()
			? FoundationFallback
			: Resolve(Prefab.Placement.FoundationBlock, FName(TEXT("madfall:concrete_frame")));
		Ids.Reserve(Prefab.Palette.Num());

		for (const FMadPrefabPaletteEntry& Entry : Prefab.Palette)
		{
			if (Entry.bVoid)
			{
				Ids.Add(VoidRuntimeId);
				continue;
			}

			const uint16 RuntimeId = InBlocks.ResolveRuntimeId(Entry.Block);
			if (RuntimeId == MadFall::BlockTypeUnresolved)
			{
				// A prefab naming a block nothing provides is a definition bug.
				// Treating it as void leaves a visible gap in the building, which
				// is easier to trace than a building full of grey placeholders.
				UE_LOG(LogMadFallRegistry, Warning,
					TEXT("Prefab '%s' uses block '%s', which no installed definition provides. Those voxels are left empty."),
					*Prefab.Id.ToString(), *Entry.Block.ToString());
				Ids.Add(VoidRuntimeId);
				continue;
			}

			Ids.Add(RuntimeId);
		}
	}

	FRWScopeLock Lock(CacheLock, SLT_Write);
	PlanCache.Reset();
}

bool FMadPoiPlanner::HasPrefabs() const
{
	return Prefabs != nullptr && Prefabs->Num() > 0;
}

FIntPoint FMadPoiPlanner::ChunkToCell(const FMadChunkCoord& Coord) const
{
	return FIntPoint(
		MadFall::FloorDiv(Coord.X, Settings->PoiCellChunks),
		MadFall::FloorDiv(Coord.Y, Settings->PoiCellChunks));
}

int32 FMadPoiPlanner::GetCellSizeVoxels() const
{
	return Settings->PoiCellChunks * ChunkSize;
}

int32 FMadPoiPlanner::GetCellTier(int32 CellX, int32 CellY) const
{
	const float CellSize = static_cast<float>(GetCellSizeVoxels());
	const float CentreX = (static_cast<float>(CellX) + 0.5f) * CellSize;
	const float CentreY = (static_cast<float>(CellY) + 0.5f) * CellSize;
	const float Distance = FMath::Sqrt(CentreX * CentreX + CentreY * CentreY);

	// Distance-based difficulty: a new player near spawn meets tier 1 ruins, and
	// the tier 5 loot is a journey away. That gives the tier tag a gameplay
	// meaning beyond decoration.
	return FMath::Clamp(1 + static_cast<int32>(Distance / FMath::Max(Settings->TierDistanceStep, 1)), 1, 5);
}

const FMadPrefab* FMadPoiPlanner::GetPrefab(const FMadPoiInstance& Poi) const
{
	return (Prefabs != nullptr && Poi.PrefabIndex >= 0 && Poi.PrefabIndex < Prefabs->Num())
		? &Prefabs->Get(Poi.PrefabIndex) : nullptr;
}

int32 FMadPoiPlanner::NumCachedCells() const
{
	FRWScopeLock Lock(CacheLock, SLT_ReadOnly);
	return PlanCache.Num();
}

// ===========================================================================
// Planning
// ===========================================================================

bool FMadPoiPlanner::PlanCell(const FMadWorldGenerator& Generator, int32 CellX, int32 CellY, FMadPoiInstance& Out) const
{
	const FIntPoint Key(CellX, CellY);

	{
		FRWScopeLock Lock(CacheLock, SLT_ReadOnly);
		if (const FMadPoiInstance* Cached = PlanCache.Find(Key))
		{
			Out = *Cached;
			return Out.IsValid();
		}
	}

	FMadPoiInstance Planned;
	PlanCellUncached(Generator, CellX, CellY, Planned);

	{
		// Two threads may plan the same cell at once. Both compute the identical
		// answer - planning is a pure function of the seed - so whichever lands
		// second simply overwrites with the same value.
		FRWScopeLock Lock(CacheLock, SLT_Write);
		PlanCache.Add(Key, Planned);
	}

	Out = Planned;
	return Out.IsValid();
}

bool FMadPoiPlanner::PlanCellUncached(const FMadWorldGenerator& Generator, int32 CellX, int32 CellY,
	FMadPoiInstance& Out) const
{
	SCOPE_CYCLE_COUNTER(STAT_MadPoiPlan);

	Out = FMadPoiInstance();
	Out.Cell = FIntPoint(CellX, CellY);

	if (!HasPrefabs())
	{
		return false;
	}

	// A cell reserved for a near_spawn prefab holds that prefab and nothing else.
	ResolveNearSpawn(Generator);
	{
		FRWScopeLock Lock(NearSpawnLock, SLT_ReadOnly);
		if (const int32* Reserved = NearSpawnCells.Find(FIntPoint(CellX, CellY)))
		{
			MadFall::Noise::FChunkRandom Reserve(PoiSeed, CellX, CellY, 0, 0x7EAD + *Reserved);
			return PlacePrefabInCell(Generator, *Reserved, CellX, CellY, Reserve, Out);
		}
	}

	MadFall::Noise::FChunkRandom Random(PoiSeed, CellX, CellY, 0);

	if (!Random.Chance(Settings->PoiDensity))
	{
		return false;
	}

	const int32 CellSize = GetCellSizeVoxels();
	const int32 CellMinX = CellX * CellSize;
	const int32 CellMinY = CellY * CellSize;
	const int32 CellTier = GetCellTier(CellX, CellY);

	const FName CellBiomeId = [&]()
	{
		const int32 Index = Generator.GetDominantBiome(
			static_cast<float>(CellMinX + CellSize / 2), static_cast<float>(CellMinY + CellSize / 2));
		return (Index != INDEX_NONE) ? Biomes->Get(Index).Id : NAME_None;
	}();

	// --- choose a prefab ---
	// Eligible: rarity above zero, tier no higher than the cell's, and either no
	// biome restriction or a restriction that includes this cell's biome.
	// Weighted toward the cell's own tier, so distant cells lean dangerous
	// without ever making the easy prefabs vanish.
	TArray<TPair<int32, float>, TInlineAllocator<16>> Candidates;
	float TotalWeight = 0.0f;

	for (int32 Index = 0; Index < Prefabs->Num(); ++Index)
	{
		const FMadPrefab& Prefab = Prefabs->Get(Index);

		if (Prefab.Placement.Rarity <= 0.0f || Prefab.Tier > CellTier || Prefab.Placement.bNearSpawn)
		{
			continue;
		}

		if (Prefab.Placement.Biomes.Num() > 0 && !Prefab.Placement.Biomes.Contains(CellBiomeId))
		{
			continue;
		}

		const float Weight = Prefab.Placement.Rarity * static_cast<float>(1 + Prefab.Tier);
		Candidates.Emplace(Index, Weight);
		TotalWeight += Weight;
	}

	if (Candidates.Num() == 0 || TotalWeight <= 0.0f)
	{
		return false;
	}

	float Roll = Random.NextFloat() * TotalWeight;
	int32 PrefabIndex = Candidates.Last().Key;
	for (const TPair<int32, float>& Candidate : Candidates)
	{
		Roll -= Candidate.Value;
		if (Roll <= 0.0f)
		{
			PrefabIndex = Candidate.Key;
			break;
		}
	}

	return PlacePrefabInCell(Generator, PrefabIndex, CellX, CellY, Random, Out);
}

void FMadPoiPlanner::ResolveNearSpawn(const FMadWorldGenerator& Generator) const
{
	{
		FRWScopeLock Lock(NearSpawnLock, SLT_ReadOnly);
		if (bNearSpawnResolved)
		{
			return;
		}
	}
	FRWScopeLock Lock(NearSpawnLock, SLT_Write);
	if (bNearSpawnResolved || !HasPrefabs())
	{
		bNearSpawnResolved = true;
		return;
	}

	// The spawn column is the corner shared by cells (-1..0, -1..0). Those four
	// are skipped - a building dropped on the spawn point would put a new player
	// on its roof - and the rings beyond are searched nearest first, cells of a
	// ring in an order hashed from the seed so the direction varies by world.
	TArray<TPair<int32, FIntPoint>> Order;
	for (int32 Y = -1 - NearSpawnRings; Y <= NearSpawnRings; ++Y)
	{
		for (int32 X = -1 - NearSpawnRings; X <= NearSpawnRings; ++X)
		{
			const int32 Ring = FMath::Max(FMath::Max(X, -1 - X), FMath::Max(Y, -1 - Y));
			if (Ring >= 1)
			{
				const int32 Hash = static_cast<int32>(MadFall::Noise::HashInt(PoiSeed ^ MadFall::Noise::HashInt(static_cast<uint32>(X * 73856093) ^ static_cast<uint32>(Y * 19349663))) & 0xFFFFFF);
				Order.Add({ Ring * 0x1000000 + Hash, FIntPoint(X, Y) });
			}
		}
	}
	Order.Sort([](const TPair<int32, FIntPoint>& A, const TPair<int32, FIntPoint>& B) { return A.Key < B.Key; });

	for (int32 Index = 0; Index < Prefabs->Num(); ++Index)
	{
		if (!Prefabs->Get(Index).Placement.bNearSpawn)
		{
			continue;
		}
		bool bPlaced = false;
		for (const TPair<int32, FIntPoint>& Candidate : Order)
		{
			const FIntPoint& Cell = Candidate.Value;
			if (NearSpawnCells.Contains(Cell))
			{
				continue;
			}
			MadFall::Noise::FChunkRandom Reserve(PoiSeed, Cell.X, Cell.Y, 0, 0x7EAD + Index);
			FMadPoiInstance Trial;
			if (PlacePrefabInCell(Generator, Index, Cell.X, Cell.Y, Reserve, Trial))
			{
				NearSpawnCells.Add(Cell, Index);
				bPlaced = true;
				break;
			}
		}
		if (!bPlaced)
		{
			UE_LOG(LogMadFallVoxel, Warning, TEXT("No cell within %d rings of the spawn has a site for near_spawn prefab %s."),
				NearSpawnRings, *Prefabs->Get(Index).Id.ToString());
		}
	}
	bNearSpawnResolved = true;
}

bool FMadPoiPlanner::GetNearSpawnCell(const FMadWorldGenerator& Generator, FName PrefabId, FIntPoint& OutCell) const
{
	ResolveNearSpawn(Generator);
	FRWScopeLock Lock(NearSpawnLock, SLT_ReadOnly);
	for (const TPair<FIntPoint, int32>& Pair : NearSpawnCells)
	{
		if (Prefabs->Get(Pair.Value).Id == PrefabId)
		{
			OutCell = Pair.Key;
			return true;
		}
	}
	return false;
}

bool FMadPoiPlanner::PlacePrefabInCell(const FMadWorldGenerator& Generator, int32 PrefabIndex, int32 CellX, int32 CellY,
	MadFall::Noise::FChunkRandom& Random, FMadPoiInstance& Out) const
{
	Out = FMadPoiInstance();
	Out.Cell = FIntPoint(CellX, CellY);

	const int32 CellSize = GetCellSizeVoxels();
	const int32 CellMinX = CellX * CellSize;
	const int32 CellMinY = CellY * CellSize;
	const int32 Margin = Settings->PoiCellMargin;
	const int32 CellTier = GetCellTier(CellX, CellY);
	const FName CellBiomeId = [&]()
	{
		const int32 Index = Generator.GetDominantBiome(
			static_cast<float>(CellMinX + CellSize / 2), static_cast<float>(CellMinY + CellSize / 2));
		return (Index != INDEX_NONE) ? Biomes->Get(Index).Id : NAME_None;
	}();

	const FMadPrefab& Prefab = Prefabs->Get(PrefabIndex);
	const int32 Yaw = Random.NextRange(0, 3);
	const FIntVector RotatedSize = MadFall::Orientation::RotateSize(Prefab.Size, Yaw);

	const int32 RangeX = CellSize - 2 * Margin - RotatedSize.X;
	const int32 RangeY = CellSize - 2 * Margin - RotatedSize.Y;
	if (RangeX < 0 || RangeY < 0)
	{
		// The prefab is bigger than a cell with margins. The loader bounds prefab
		// size, so this only happens if the cell size was tuned down below it.
		return false;
	}

	// --- find a site flat enough ---
	// A few attempts rather than one: a cell that is mostly hillside still
	// usually has a shelf somewhere, and giving up on the first steep sample
	// would make POIs vanish from exactly the scenic terrain that deserves them.
	constexpr int32 SiteAttempts = 5;

	for (int32 Attempt = 0; Attempt < SiteAttempts; ++Attempt)
	{
		const int32 OriginX = CellMinX + Margin + Random.NextRange(0, RangeX);
		const int32 OriginY = CellMinY + Margin + Random.NextRange(0, RangeY);

		// 3x3 sample grid across the footprint: corners, edge midpoints, centre.
		float Heights[9];
		int32 SampleIndex = 0;
		for (int32 SY = 0; SY < 3; ++SY)
		{
			for (int32 SX = 0; SX < 3; ++SX)
			{
				const float X = static_cast<float>(OriginX + (RotatedSize.X - 1) * SX / 2);
				const float Y = static_cast<float>(OriginY + (RotatedSize.Y - 1) * SY / 2);
				Heights[SampleIndex++] = Generator.GetSurfaceHeight(X, Y);
			}
		}

		float MinHeight = Heights[0];
		float MaxHeight = Heights[0];
		for (float Height : Heights)
		{
			MinHeight = FMath::Min(MinHeight, Height);
			MaxHeight = FMath::Max(MaxHeight, Height);
		}

		if (MaxHeight - MinHeight > static_cast<float>(Prefab.Placement.MaxSlope))
		{
			continue;
		}

		// Median rather than mean: one sample on a spike or in a dip should not
		// lift or sink the whole building.
		Algo::Sort(Heights);
		const float Median = Heights[4];

		int32 BaseZ = FMath::FloorToInt(Median) + 1;
		if (Prefab.Placement.Conform == EMadPrefabConform::Base)
		{
			BaseZ -= Prefab.Placement.EmbedDepth;
		}

		if (!Prefab.Placement.bUnderwater && BaseZ <= Settings->SeaLevel)
		{
			continue;
		}

		if (BaseZ - Prefab.Placement.MaxFoundationDepth <= Settings->BedrockTop
			|| BaseZ + RotatedSize.Z > MadFall::WorldMaxZ)
		{
			continue;
		}

		Out.PrefabIndex = PrefabIndex;
		Out.Origin = FIntVector(OriginX, OriginY, BaseZ);
		Out.Yaw = Yaw;
		Out.RotatedSize = RotatedSize;
		Out.CellTier = CellTier;
		Out.BiomeId = CellBiomeId;
		return true;
	}

	return false;
}

void FMadPoiPlanner::GetPoisOverlappingChunk(const FMadWorldGenerator& Generator, const FMadChunkCoord& Coord,
	TArray<FMadPoiInstance>& OutPois) const
{
	OutPois.Reset();

	if (!HasPrefabs())
	{
		return;
	}

	// Cells align with chunk boundaries and a POI never leaves its cell, so only
	// the chunk's own cell can possibly reach it.
	const FIntPoint Cell = ChunkToCell(Coord);

	FMadPoiInstance Poi;
	if (!PlanCell(Generator, Cell.X, Cell.Y, Poi))
	{
		return;
	}

	const FMadPrefab* Prefab = GetPrefab(Poi);
	FIntVector Min, Max;
	Poi.GetWorldBounds(Prefab->Placement.MaxFoundationDepth, Min, Max);

	const int32 ChunkMinX = Coord.X * ChunkSize;
	const int32 ChunkMinY = Coord.Y * ChunkSize;
	const int32 ChunkMinZ = Coord.Z * ChunkSize;

	const bool bOverlaps =
		Max.X >= ChunkMinX && Min.X < ChunkMinX + ChunkSize &&
		Max.Y >= ChunkMinY && Min.Y < ChunkMinY + ChunkSize &&
		Max.Z >= ChunkMinZ && Min.Z < ChunkMinZ + ChunkSize;

	if (bOverlaps)
	{
		OutPois.Add(Poi);
	}
}

// ===========================================================================
// Stamping
// ===========================================================================

bool FMadPoiPlanner::WorldToLocal(const FMadPoiInstance& Poi, const FIntVector& World, FIntVector& OutLocal) const
{
	const FMadPrefab* Prefab = GetPrefab(Poi);

	const int32 RotatedX = World.X - Poi.Origin.X;
	const int32 RotatedY = World.Y - Poi.Origin.Y;
	const int32 LocalZ = World.Z - Poi.Origin.Z;

	if (RotatedX < 0 || RotatedY < 0 || LocalZ < 0
		|| RotatedX >= Poi.RotatedSize.X || RotatedY >= Poi.RotatedSize.Y || LocalZ >= Poi.RotatedSize.Z)
	{
		return false;
	}

	int32 LocalX, LocalY;
	MadFall::Orientation::UnrotateFootprint(RotatedX, RotatedY, Prefab->Size.X, Prefab->Size.Y, Poi.Yaw, LocalX, LocalY);

	OutLocal = FIntVector(LocalX, LocalY, LocalZ);
	return true;
}

void FMadPoiPlanner::StampPoi(const FMadWorldGenerator& Generator, const FMadPoiInstance& Poi,
	const FMadChunkCoord& Coord, FMadChunkStorage& Storage) const
{
	SCOPE_CYCLE_COUNTER(STAT_MadPoiStamp);

	const FMadPrefab* Prefab = GetPrefab(Poi);
	if (Prefab == nullptr)
	{
		return;
	}

	const TArray<uint16>& RuntimeIds = PaletteRuntimeIds[Poi.PrefabIndex];

	const int32 ChunkMinX = Coord.X * ChunkSize;
	const int32 ChunkMinY = Coord.Y * ChunkSize;
	const int32 ChunkMinZ = Coord.Z * ChunkSize;

	FIntVector BoundsMin, BoundsMax;
	Poi.GetWorldBounds(Prefab->Placement.MaxFoundationDepth, BoundsMin, BoundsMax);

	// Walk only the intersection of the POI's box with this chunk, inverse-mapping
	// each world voxel into the prefab. Walking the prefab forwards instead would
	// touch every voxel of a 96^3 building once for each of the chunks it spans.
	const int32 X0 = FMath::Max(BoundsMin.X, ChunkMinX);
	const int32 X1 = FMath::Min(BoundsMax.X, ChunkMinX + ChunkSize - 1);
	const int32 Y0 = FMath::Max(BoundsMin.Y, ChunkMinY);
	const int32 Y1 = FMath::Min(BoundsMax.Y, ChunkMinY + ChunkSize - 1);
	const int32 Z0 = FMath::Max(Poi.Origin.Z, ChunkMinZ);
	const int32 Z1 = FMath::Min(BoundsMax.Z, ChunkMinZ + ChunkSize - 1);

	// --- the prefab itself ---
	for (int32 WorldZ = Z0; WorldZ <= Z1; ++WorldZ)
	{
		for (int32 WorldY = Y0; WorldY <= Y1; ++WorldY)
		{
			for (int32 WorldX = X0; WorldX <= X1; ++WorldX)
			{
				FIntVector Local;
				if (!WorldToLocal(Poi, FIntVector(WorldX, WorldY, WorldZ), Local))
				{
					continue;
				}

				const int32 PaletteIndex = Prefab->Voxels[Prefab->VoxelIndex(Local.X, Local.Y, Local.Z)];
				const uint16 RuntimeId = RuntimeIds[PaletteIndex];

				if (RuntimeId == VoidRuntimeId)
				{
					continue;
				}

				const FMadPrefabPaletteEntry& Entry = Prefab->Palette[PaletteIndex];

				FMadVoxel Voxel;
				Voxel.BlockTypeID = RuntimeId;
				Voxel.Density = Entry.Density;
				Voxel.Damage = 0;
				Voxel.Rotation = 0;

				// The block turns with the building. Without this a staircase
				// built facing north stays facing north inside a building that
				// now faces east.
				Voxel.SetOrientation(MadFall::Orientation::ApplyYaw(Entry.Orientation, Poi.Yaw));
				Voxel.SetShapeVariant(Entry.Variant);
				Voxel.Flags = 0;
				Voxel.SetFlag(EMadVoxelFlags::Cubic, Entry.bCubic && !Entry.IsAir());

				Storage.SetVoxel(
					MadFall::VoxelIndex(WorldX - ChunkMinX, WorldY - ChunkMinY, WorldZ - ChunkMinZ), Voxel);
			}
		}
	}

	// --- foundation ---
	if (Prefab->Placement.Conform != EMadPrefabConform::Base || Prefab->Placement.MaxFoundationDepth <= 0)
	{
		return;
	}

	const int32 FoundationTopZ = Poi.Origin.Z - 1;
	const int32 FoundationBottomZ = Poi.Origin.Z - Prefab->Placement.MaxFoundationDepth;

	const int32 FZ0 = FMath::Max(FoundationBottomZ, ChunkMinZ);
	const int32 FZ1 = FMath::Min(FoundationTopZ, ChunkMinZ + ChunkSize - 1);
	if (FZ0 > FZ1)
	{
		return;
	}

	const uint16 FoundationId = FoundationIds[Poi.PrefabIndex];

	for (int32 WorldY = Y0; WorldY <= Y1; ++WorldY)
	{
		for (int32 WorldX = X0; WorldX <= X1; ++WorldX)
		{
			FIntVector Local;
			if (!WorldToLocal(Poi, FIntVector(WorldX, WorldY, Poi.Origin.Z), Local))
			{
				continue;
			}

			// Only under columns the building actually stands on. A void corner
			// of the bottom layer gets no plinth.
			const int32 PaletteIndex = Prefab->Voxels[Prefab->VoxelIndex(Local.X, Local.Y, 0)];
			const FMadPrefabPaletteEntry& Bottom = Prefab->Palette[PaletteIndex];
			if (Bottom.bVoid || Bottom.IsAir() || Bottom.Density < 128)
			{
				continue;
			}

			// Ground under this column, from the pure terrain function, so every
			// chunk the foundation spans agrees on where it stops.
			const int32 GroundTop = Generator.FindTerrainTopBelow(
				WorldX, WorldY, FoundationTopZ, Prefab->Placement.MaxFoundationDepth);

			// The pure terrain function knows nothing about caves or road cuts, so
			// it can report solid ground directly under the base where the chunk
			// actually has a hole: warp can lift the local surface far enough that
			// the cave margin no longer protects the voxel under the building, and
			// a road's clearance can cut beneath a doorway. A stamp test over 11
			// POIs found 4 floating columns from exactly this. So the range always
			// includes the voxel directly below the base, and within the range only
			// voxels that are genuinely empty IN THIS CHUNK are filled. The range
			// comes from the pure function and the emptiness test is local to each
			// voxel's own chunk, so independently generated chunks still agree.
			const int32 FillFrom = (GroundTop == INDEX_NONE)
				? FoundationBottomZ
				: FMath::Min(GroundTop + 1, FoundationTopZ);

			for (int32 WorldZ = FMath::Max(FZ0, FillFrom); WorldZ <= FZ1; ++WorldZ)
			{
				const int32 Index = MadFall::VoxelIndex(WorldX - ChunkMinX, WorldY - ChunkMinY, WorldZ - ChunkMinZ);
				if (Storage.GetVoxel(Index).IsSolid())
				{
					continue;
				}

				FMadVoxel Voxel;
				Voxel.BlockTypeID = FoundationId;
				Voxel.Density = 255;
				Voxel.Damage = 0;
				Voxel.Rotation = 0;
				Voxel.Flags = 0;
				Voxel.SetFlag(EMadVoxelFlags::Cubic, true);

				Storage.SetVoxel(Index, Voxel);
			}
		}
	}
}

// ===========================================================================
// Markers
// ===========================================================================

FIntVector FMadPoiPlanner::GetWorldEntrance(const FMadPoiInstance& Poi) const
{
	const FMadPrefab* Prefab = GetPrefab(Poi);
	if (Prefab == nullptr)
	{
		return Poi.Origin;
	}

	const FIntVector Local = Prefab->GetEntrance();

	int32 RotatedX, RotatedY;
	MadFall::Orientation::RotateFootprint(Local.X, Local.Y, Prefab->Size.X, Prefab->Size.Y, Poi.Yaw, RotatedX, RotatedY);

	return FIntVector(Poi.Origin.X + RotatedX, Poi.Origin.Y + RotatedY, Poi.Origin.Z + Local.Z);
}

void FMadPoiPlanner::GetWorldMarkers(const FMadPoiInstance& Poi, TArray<FMadPoiWorldMarker>& OutMarkers) const
{
	OutMarkers.Reset();

	const FMadPrefab* Prefab = GetPrefab(Poi);
	if (Prefab == nullptr)
	{
		return;
	}

	for (const FMadPoiMarker& Marker : Prefab->Markers)
	{
		int32 RotatedX, RotatedY;
		MadFall::Orientation::RotateFootprint(
			Marker.Position.X, Marker.Position.Y, Prefab->Size.X, Prefab->Size.Y, Poi.Yaw, RotatedX, RotatedY);

		FMadPoiWorldMarker& World = OutMarkers.AddDefaulted_GetRef();
		World.Type = Marker.Type;
		World.WorldPosition = FIntVector(Poi.Origin.X + RotatedX, Poi.Origin.Y + RotatedY, Poi.Origin.Z + Marker.Position.Z);
		World.LootTable = Marker.LootTable;
		World.SpawnGroup = Marker.SpawnGroup;
		World.Count = Marker.Count;
		World.PrefabId = Prefab->Id;
		// The CELL's tier, not the prefab's: a tier-1 cabin found far from spawn
		// should still hold far-from-spawn loot.
		World.Tier = Poi.CellTier;
		World.Tags = Marker.Tags;
		World.Trader = Marker.Trader;
	}
}

// ===========================================================================
// Roads
// ===========================================================================

void FMadPoiPlanner::GetRoadsNearChunk(const FMadWorldGenerator& Generator, const FMadChunkCoord& Coord,
	TArray<FMadRoadSegment>& OutRoads) const
{
	OutRoads.Reset();

	if (!HasPrefabs() || !Settings->bRoads)
	{
		return;
	}

	const FIntPoint Cell = ChunkToCell(Coord);

	const int32 ChunkMinX = Coord.X * ChunkSize;
	const int32 ChunkMinY = Coord.Y * ChunkSize;
	const int32 ChunkMinZ = Coord.Z * ChunkSize;

	// Each cell owns the road to its +X and +Y neighbour, so every connection is
	// owned exactly once. A road owned by a neighbouring cell can still cross
	// this chunk, so the 3x3 neighbourhood of owners is checked.
	for (int32 OwnerY = Cell.Y - 1; OwnerY <= Cell.Y + 1; ++OwnerY)
	{
		for (int32 OwnerX = Cell.X - 1; OwnerX <= Cell.X + 1; ++OwnerX)
		{
			FMadPoiInstance From;
			if (!PlanCell(Generator, OwnerX, OwnerY, From))
			{
				continue;
			}

			const FIntPoint Neighbours[2] = { FIntPoint(OwnerX + 1, OwnerY), FIntPoint(OwnerX, OwnerY + 1) };

			for (const FIntPoint& NeighbourCell : Neighbours)
			{
				FMadPoiInstance To;
				if (!PlanCell(Generator, NeighbourCell.X, NeighbourCell.Y, To))
				{
					continue;
				}

				FMadRoadSegment Road;
				Road.Start = GetWorldEntrance(From);
				Road.End = GetWorldEntrance(To);
				Road.Seed = MadFall::Noise::Hash3(OwnerX, OwnerY, NeighbourCell.X == OwnerX ? 1 : 0, RoadSeed);

				const float Length = FVector2f(
					static_cast<float>(Road.End.X - Road.Start.X),
					static_cast<float>(Road.End.Y - Road.Start.Y)).Size();

				if (Length > static_cast<float>(Settings->RoadMaxLength) || Length < 8.0f)
				{
					continue;
				}

				// Bounding box test with generous vertical slack: the road follows
				// terrain, which can rise well above either endpoint mid-route.
				const int32 Pad = FMath::CeilToInt(Settings->RoadMeander + MadFall::Roads::MaxBank) + Settings->RoadWidth + 2;
				const int32 MinX = FMath::Min(Road.Start.X, Road.End.X) - Pad;
				const int32 MaxX = FMath::Max(Road.Start.X, Road.End.X) + Pad;
				const int32 MinY = FMath::Min(Road.Start.Y, Road.End.Y) - Pad;
				const int32 MaxY = FMath::Max(Road.Start.Y, Road.End.Y) + Pad;

				if (MaxX < ChunkMinX || MinX >= ChunkMinX + ChunkSize
					|| MaxY < ChunkMinY || MinY >= ChunkMinY + ChunkSize)
				{
					continue;
				}

				const int32 LowZ = FMath::Min(Road.Start.Z, Road.End.Z) - 64;
				const int32 HighZ = FMath::Max(Road.Start.Z, Road.End.Z) + 64;
				if (HighZ < ChunkMinZ || LowZ >= ChunkMinZ + ChunkSize)
				{
					continue;
				}

				// Skip roads that would have to cross open water; bridges are a
				// feature, not something to fake with a gravel causeway.
				bool bCrossesWater = false;
				for (int32 Sample = 1; Sample < 8; ++Sample)
				{
					const float T = static_cast<float>(Sample) / 8.0f;
					const float X = FMath::Lerp(static_cast<float>(Road.Start.X), static_cast<float>(Road.End.X), T);
					const float Y = FMath::Lerp(static_cast<float>(Road.Start.Y), static_cast<float>(Road.End.Y), T);
					if (Generator.GetSurfaceHeight(X, Y) < static_cast<float>(Settings->SeaLevel) + 0.5f)
					{
						bCrossesWater = true;
						break;
					}
				}

				if (!bCrossesWater)
				{
					OutRoads.Add(Road);
				}
			}
		}
	}
}

float FMadPoiPlanner::RoadHeightAt(const FMadWorldGenerator& Generator, const FMadRoadSegment& Road,
	const FVector2f& Point, float T) const
{
	// Terrain height smoothed along the road's own direction, so the road rides
	// over small bumps instead of reproducing every one of them as a pothole.
	const FVector2f Direction = FVector2f(
		static_cast<float>(Road.End.X - Road.Start.X),
		static_cast<float>(Road.End.Y - Road.Start.Y)).GetSafeNormal();

	float Sum = 0.0f;
	for (int32 Offset = -2; Offset <= 2; ++Offset)
	{
		const FVector2f Sample = Point + Direction * static_cast<float>(Offset * 4);
		Sum += Generator.GetSurfaceHeight(Sample.X, Sample.Y);
	}
	const float Smoothed = Sum / 5.0f;

	// Blend into each endpoint's floor over the last 16 voxels, so the road meets
	// the doorway at the building's own level rather than a step below it.
	const float Length = FVector2f(
		static_cast<float>(Road.End.X - Road.Start.X),
		static_cast<float>(Road.End.Y - Road.Start.Y)).Size();
	const float Distance = T * Length;
	const float FromStart = FMath::SmoothStep(0.0f, 16.0f, Distance);
	const float FromEnd = FMath::SmoothStep(0.0f, 16.0f, Length - Distance);

	float Height = FMath::Lerp(static_cast<float>(Road.Start.Z) - 0.5f, Smoothed, FromStart);
	Height = FMath::Lerp(static_cast<float>(Road.End.Z) - 0.5f, Height, FromEnd);

	// Continuous, not rounded: an integer road height steps every time the
	// terrain crosses a voxel boundary, and each step is a visible notch.
	return Height;
}

bool FMadPoiPlanner::IsNearPoiOrRoad(const FMadWorldGenerator& Generator, int32 WorldX, int32 WorldY, int32 WorldZ, int32 Reach) const
{
	if (!HasPrefabs())
	{
		return false;
	}

	// The chunks under the corners of the reach box cover every cell and every
	// road lookup that can come within Reach of the column.
	TArray<FMadChunkCoord, TInlineAllocator<4>> Corners;
	for (int32 DY : { -Reach, Reach })
	{
		for (int32 DX : { -Reach, Reach })
		{
			Corners.AddUnique(MadFall::WorldToChunk(WorldX + DX, WorldY + DY, WorldZ));
		}
	}

	TArray<FIntPoint, TInlineAllocator<4>> Cells;
	for (const FMadChunkCoord& Corner : Corners)
	{
		Cells.AddUnique(ChunkToCell(Corner));
	}
	for (const FIntPoint& Cell : Cells)
	{
		FMadPoiInstance Poi;
		if (!PlanCell(Generator, Cell.X, Cell.Y, Poi))
		{
			continue;
		}
		const FMadPrefab* Prefab = GetPrefab(Poi);
		FIntVector Min, Max;
		Poi.GetWorldBounds(Prefab ? Prefab->Placement.MaxFoundationDepth : 0, Min, Max);
		if (WorldX + Reach >= Min.X && WorldX - Reach <= Max.X && WorldY + Reach >= Min.Y && WorldY - Reach <= Max.Y)
		{
			return true;
		}
	}

	if (!Settings->bRoads)
	{
		return false;
	}

	// The bank is part of the road: a tree rooted on it would float over a cutting.
	const float Pad = static_cast<float>(Settings->RoadWidth) * 0.5f + Settings->RoadMeander + MadFall::Roads::MaxBank + 2.0f + static_cast<float>(Reach);
	const FVector2f Point(static_cast<float>(WorldX) + 0.5f, static_cast<float>(WorldY) + 0.5f);
	TArray<FMadRoadSegment> Roads;
	for (const FMadChunkCoord& Corner : Corners)
	{
		GetRoadsNearChunk(Generator, Corner, Roads);
		for (const FMadRoadSegment& Road : Roads)
		{
			const FVector2f Start(static_cast<float>(Road.Start.X), static_cast<float>(Road.Start.Y));
			const FVector2f End(static_cast<float>(Road.End.X), static_cast<float>(Road.End.Y));
			const FVector2f Delta = End - Start;
			const float T = FMath::Clamp(FVector2f::DotProduct(Point - Start, Delta) / FMath::Max(Delta.SizeSquared(), 1.0f), 0.0f, 1.0f);
			if (FVector2f::Distance(Point, Start + Delta * T) <= Pad)
			{
				return true;
			}
		}
	}
	return false;
}

void FMadPoiPlanner::StampRoad(const FMadWorldGenerator& Generator, const FMadRoadSegment& Road,
	const FMadChunkCoord& Coord, FMadChunkStorage& Storage, const float* SurfaceHeights) const
{
	SCOPE_CYCLE_COUNTER(STAT_MadRoadStamp);

	const int32 ChunkMinX = Coord.X * ChunkSize;
	const int32 ChunkMinY = Coord.Y * ChunkSize;

	const FVector2f Start(static_cast<float>(Road.Start.X), static_cast<float>(Road.Start.Y));
	const FVector2f End(static_cast<float>(Road.End.X), static_cast<float>(Road.End.Y));
	const FVector2f Delta = End - Start;
	const float Length = Delta.Size();
	const FVector2f Direction = Delta / Length;
	const FVector2f Perpendicular(-Direction.Y, Direction.X);

	const float HalfWidth = static_cast<float>(Settings->RoadWidth) * 0.5f;
	const float Reach = HalfWidth + Settings->RoadMeander + 2.0f;

	const float MeanderPhase = MadFall::Noise::HashToUnitFloat(Road.Seed) * 6.2831853f;

	// Walk the centreline in half-voxel steps, so a diagonal road leaves no gaps.
	const int32 Steps = FMath::CeilToInt(Length * 2.0f);

	// Sine-envelope wander: zero at both ends so the road still hits the
	// doorways, largest in the middle where a straight line looks surveyed.
	auto CentreAt = [&](float T)
	{
		const float Wander = FMath::Sin(T * PI) * Settings->RoadMeander
			* FMath::Sin(T * 6.2831853f * 1.5f + MeanderPhase);
		return Start + Delta * T + Perpendicular * Wander;
	};

	StampRoadBanks(Generator, Road, Coord, Storage, Steps, CentreAt, SurfaceHeights);

	for (int32 Step = 0; Step <= Steps; ++Step)
	{
		const float T = static_cast<float>(Step) / static_cast<float>(Steps);
		const FVector2f Centre = CentreAt(T);

		// Cheap reject before any height sampling: most steps of a long road are
		// nowhere near this particular chunk.
		if (Centre.X + Reach < static_cast<float>(ChunkMinX) || Centre.X - Reach >= static_cast<float>(ChunkMinX + ChunkSize)
			|| Centre.Y + Reach < static_cast<float>(ChunkMinY) || Centre.Y - Reach >= static_cast<float>(ChunkMinY + ChunkSize))
		{
			continue;
		}

		const float RoadHeight = RoadHeightAt(Generator, Road, Centre, T);
		const int32 RoadTopZ = FMath::FloorToInt(RoadHeight);

		for (float Across = -HalfWidth; Across <= HalfWidth; Across += 0.5f)
		{
			const FVector2f Point = Centre + Perpendicular * Across;
			const int32 WorldX = FMath::FloorToInt(Point.X);
			const int32 WorldY = FMath::FloorToInt(Point.Y);

			if (WorldX < ChunkMinX || WorldX >= ChunkMinX + ChunkSize
				|| WorldY < ChunkMinY || WorldY >= ChunkMinY + ChunkSize)
			{
				continue;
			}

			// Road surface, clearance above, embankment below. Non-cubic, and
			// written with the terrain's own continuous density band, so the
			// isosurface mesher produces a smooth graded path. An earlier version
			// wrote a hard 255-over-0 step at an integer height, and rendered as a
			// notched trench with a stair every time the terrain crossed a voxel.
			for (int32 WorldZ = RoadTopZ - RoadMaxFill; WorldZ <= RoadTopZ + RoadClearance; ++WorldZ)
			{
				if (!ChunkContains(Coord, WorldX, WorldY, WorldZ) || !MadFall::IsValidWorldZ(WorldZ))
				{
					continue;
				}

				const int32 Index = MadFall::VoxelIndex(
					WorldX - ChunkMinX, WorldY - ChunkMinY, WorldZ - Coord.Z * ChunkSize);

				const FMadVoxel Existing = Storage.GetVoxel(Index);

				// Never carve or fill through something that is already
				// construction.
				if (Existing.HasFlag(EMadVoxelFlags::Cubic))
				{
					continue;
				}

				const float DistanceBelow = RoadHeight - static_cast<float>(WorldZ);
				const uint8 Density = MadFall::DistanceToDensity(DistanceBelow);

				FMadVoxel Voxel;
				Voxel.Damage = 0;
				Voxel.Rotation = 0;
				Voxel.Flags = 0;
				Voxel.Density = Density;

				if (Density < 128)
				{
					// Above the road. Carve, but keep the soft edge: the partial
					// density just above the surface is what lets the mesher place
					// the surface between voxels instead of on a voxel boundary.
					Voxel.BlockTypeID = MadFall::BlockTypeAir;
				}
				else if (DistanceBelow < 1.5f)
				{
					// The wearing course: the top solid layer is gravel.
					Voxel.BlockTypeID = RoadBlock;
				}
				else
				{
					// Embankment: fill only where the terrain is empty, so a road
					// crossing solid ground does not replace the rock under it.
					if (Existing.Density >= 128)
					{
						continue;
					}
					Voxel.BlockTypeID = RoadBedBlock;
				}

				Storage.SetVoxel(Index, Voxel);
			}
		}
	}
}

void FMadPoiPlanner::StampRoadBanks(const FMadWorldGenerator& Generator, const FMadRoadSegment& Road,
	const FMadChunkCoord& Coord, FMadChunkStorage& Storage, int32 Steps, TFunctionRef<FVector2f(float)> CentreAt,
	const float* SurfaceHeights) const
{
	// Per column, not per centreline step. Walking the centreline, a step's bank
	// on the inside of a bend lands on another step's road; a column knows its
	// one nearest point on the road, so each is shaped once, the same way from
	// whichever chunk asks.
	const int32 ChunkMinX = Coord.X * ChunkSize;
	const int32 ChunkMinY = Coord.Y * ChunkSize;
	const int32 ChunkMinZ = Coord.Z * ChunkSize;
	const float HalfWidth = static_cast<float>(Settings->RoadWidth) * 0.5f;
	const float BandReach = HalfWidth + MadFall::Roads::MaxBank + 1.0f;

	struct FSample
	{
		FVector2f Centre;
		float T = 0.0f;
		float Height = 0.0f;
		bool bHeight = false;
	};
	TArray<FSample, TInlineAllocator<256>> Samples;
	for (int32 Step = 0; Step <= Steps; ++Step)
	{
		const float T = static_cast<float>(Step) / static_cast<float>(Steps);
		const FVector2f Centre = CentreAt(T);
		if (Centre.X + BandReach >= static_cast<float>(ChunkMinX) && Centre.X - BandReach < static_cast<float>(ChunkMinX + ChunkSize)
			&& Centre.Y + BandReach >= static_cast<float>(ChunkMinY) && Centre.Y - BandReach < static_cast<float>(ChunkMinY + ChunkSize))
		{
			Samples.Add(FSample{ Centre, T });
		}
	}
	if (Samples.Num() == 0)
	{
		return;
	}

	// A road's search reaches 64 voxels up and down from its ends, so most
	// chunks asked are sky or rock well away from it. Measured: without this the
	// banks did a height lookup and a warp search per column in all of them, and
	// the generation workers were so busy the far terrain built 4 of 195 tiles in
	// the frame-budget run instead of all of them. A road stays near the ground
	// beside it (its height is the terrain smoothed over 8 voxels, easing to
	// entrances at ground level), so a chunk more than warp slack plus a generous
	// margin from every column's surface cannot be touched.
	const int32 Slack = FMath::CeilToInt(Settings->WarpStrength) + 2;
	auto SurfaceAt = [&](int32 LocalX, int32 LocalY)
	{
		return SurfaceHeights != nullptr
			? SurfaceHeights[LocalX + ChunkSize * LocalY]
			: Generator.GetSurfaceHeight(static_cast<float>(ChunkMinX + LocalX), static_cast<float>(ChunkMinY + LocalY));
	};
	if (SurfaceHeights != nullptr)
	{
		float MinSurface = MAX_flt;
		float MaxSurface = -MAX_flt;
		for (int32 Column = 0; Column < ChunkSize * ChunkSize; ++Column)
		{
			MinSurface = FMath::Min(MinSurface, SurfaceHeights[Column]);
			MaxSurface = FMath::Max(MaxSurface, SurfaceHeights[Column]);
		}
		const float Margin = static_cast<float>(Slack) + 24.0f;
		if (static_cast<float>(ChunkMinZ) > MaxSurface + Margin || static_cast<float>(ChunkMinZ + ChunkSize) < MinSurface - Margin)
		{
			return;
		}
	}

	for (int32 LocalY = 0; LocalY < ChunkSize; ++LocalY)
	{
		for (int32 LocalX = 0; LocalX < ChunkSize; ++LocalX)
		{
			const FVector2f Point(static_cast<float>(ChunkMinX + LocalX) + 0.5f, static_cast<float>(ChunkMinY + LocalY) + 0.5f);
			int32 Nearest = 0;
			float NearestSq = MAX_flt;
			for (int32 Index = 0; Index < Samples.Num(); ++Index)
			{
				const float DistSq = FVector2f::DistSquared(Point, Samples[Index].Centre);
				if (DistSq < NearestSq)
				{
					NearestSq = DistSq;
					Nearest = Index;
				}
			}
			// Past HalfWidth is bank. The body, which runs after, still writes some
			// columns just past it; there the bank's target is the road's height anyway.
			const float Beyond = FMath::Sqrt(NearestSq) - HalfWidth;
			if (Beyond >= MadFall::Roads::MaxBank)
			{
				continue;
			}

			FSample& Sample = Samples[Nearest];
			if (!Sample.bHeight)
			{
				Sample.Height = RoadHeightAt(Generator, Road, Sample.Centre, Sample.T);
				Sample.bHeight = true;
			}
			// The ground as generated: the blended surface moved by the 3D warp, up
			// to WarpStrength voxels. The unwarped height alone is why a road's
			// clearance left walls - the real ground stood several voxels above it.
			// Found from the noise, not from the chunk's voxels, so the chunks above
			// and below a column agree on it.
			const float Surface = SurfaceAt(LocalX, LocalY);
			// Everything below is written between the road and the ground; skip the
			// warp search when that span misses this chunk.
			const float SpanLow = FMath::Min(Sample.Height, Surface - static_cast<float>(Slack)) - 3.0f;
			const float SpanHigh = FMath::Max(Sample.Height + static_cast<float>(RoadClearance) + 2.0f, Surface + static_cast<float>(Slack)) + 3.0f;
			if (SpanHigh < static_cast<float>(ChunkMinZ) || SpanLow > static_cast<float>(ChunkMinZ + ChunkSize))
			{
				continue;
			}
			auto DistanceAt = [&](int32 Z)
			{
				return Surface + Generator.GetWarp(Point.X - 0.5f, Point.Y - 0.5f, static_cast<float>(Z)) - static_cast<float>(Z);
			};
			float Terrain = Surface;
			float Above = DistanceAt(FMath::FloorToInt(Surface) + Slack + 1);
			for (int32 Z = FMath::FloorToInt(Surface) + Slack; Z >= FMath::FloorToInt(Surface) - Slack; --Z)
			{
				const float Here = DistanceAt(Z);
				if (Here >= 0.0f && Above < 0.0f)
				{
					Terrain = static_cast<float>(Z) + Here / FMath::Max(Here - Above, 1e-3f);
					break;
				}
				Above = Here;
			}
			const float Target = MadFall::Roads::BankHeight(Sample.Height, Terrain, Beyond);

			// On the road itself only what the body leaves alone - earth above its
			// clearance, which would otherwise roof the road over in a deep cutting.
			const bool bOnRoad = Beyond <= 0.0f;
			const float CarveFrom = bOnRoad ? Target + static_cast<float>(RoadClearance) + 1.0f : Target;
			const bool bCut = Terrain > CarveFrom + 0.25f;
			const bool bFill = !bOnRoad && Terrain < Target - 0.25f;
			if (!bCut && !bFill)
			{
				continue;
			}

			const int32 LowZ = FMath::Max(ChunkMinZ, FMath::FloorToInt(FMath::Min(Target, Terrain)) - 2);
			const int32 HighZ = FMath::Min(ChunkMinZ + ChunkSize - 1, FMath::CeilToInt(FMath::Max(CarveFrom, Terrain)) + 2);
			if (LowZ > HighZ)
			{
				continue;
			}

			// Leave junctions to the road already there, and never touch construction.
			uint16 Fill = RoadBedBlock;
			bool bSkip = false;
			bool bFoundSurface = false;
			for (int32 WorldZ = HighZ; WorldZ >= LowZ; --WorldZ)
			{
				const FMadVoxel Existing = Storage.GetVoxel(MadFall::VoxelIndex(LocalX, LocalY, WorldZ - ChunkMinZ));
				if (Existing.HasFlag(EMadVoxelFlags::Cubic) || Existing.BlockTypeID == RoadBlock)
				{
					bSkip = true;
					break;
				}
				if (!bFoundSurface && Existing.Density >= 128 && Existing.BlockTypeID != MadFall::BlockTypeAir)
				{
					// An embankment is built of what the ground here is made of, so a
					// grassy slope stays grassy up to the road's edge.
					Fill = Existing.BlockTypeID;
					bFoundSurface = true;
				}
			}
			if (bSkip)
			{
				continue;
			}

			for (int32 WorldZ = LowZ; WorldZ <= HighZ; ++WorldZ)
			{
				const int32 Index = MadFall::VoxelIndex(LocalX, LocalY, WorldZ - ChunkMinZ);
				const FMadVoxel Existing = Storage.GetVoxel(Index);
				const uint8 Density = MadFall::DistanceToDensity((bOnRoad ? CarveFrom : Target) - static_cast<float>(WorldZ));
				const bool bSolidNow = Existing.Density >= 128 && Existing.BlockTypeID != MadFall::BlockTypeAir;

				FMadVoxel Voxel = Existing;
				if (Density < 128)
				{
					// Above the new ground. Carve earth in a cutting; over a fill, only
					// soften the air so the surface lands between voxels.
					if (bSolidNow && bCut)
					{
						Voxel.BlockTypeID = MadFall::BlockTypeAir;
						Voxel.Density = Density;
					}
					else if (!bSolidNow && bFill)
					{
						Voxel.Density = FMath::Max(Existing.Density, Density);
					}
					else
					{
						continue;
					}
				}
				else
				{
					if (bOnRoad)
					{
						continue;   // at and below the clearance: the body's
					}
					if (bSolidNow)
					{
						// A cutting's new face: the same rock or earth, with its surface
						// density moved down to the bank.
						if (!bCut)
						{
							continue;
						}
						Voxel.Density = FMath::Min(Existing.Density == 0 ? Density : Existing.Density, Density);
					}
					else if (bFill)
					{
						Voxel.BlockTypeID = Fill;
						Voxel.Density = Density;
						Voxel.Damage = 0;
						Voxel.Rotation = 0;
						Voxel.Flags = 0;
					}
					else
					{
						continue;
					}
				}
				Storage.SetVoxel(Index, Voxel);
			}
		}
	}
}

// ===========================================================================
// Diagnostics
// ===========================================================================

FString FMadPoiPlanner::DescribeCell(const FMadWorldGenerator& Generator, int32 CellX, int32 CellY) const
{
	TStringBuilder<1024> Builder;
	Builder.Appendf(TEXT("POI cell (%d, %d)  size %d voxels  tier %d\n"),
		CellX, CellY, GetCellSizeVoxels(), GetCellTier(CellX, CellY));

	FMadPoiInstance Poi;
	if (!PlanCell(Generator, CellX, CellY, Poi))
	{
		Builder.Append(TEXT("  no POI\n"));
		return Builder.ToString();
	}

	const FMadPrefab* Prefab = GetPrefab(Poi);
	const FIntVector Entrance = GetWorldEntrance(Poi);

	Builder.Appendf(TEXT("  prefab:   %s (tier %d)\n"), *Prefab->Id.ToString(), Prefab->Tier);
	Builder.Appendf(TEXT("  biome:    %s\n"), *Poi.BiomeId.ToString());
	Builder.Appendf(TEXT("  origin:   (%d, %d, %d)  yaw %d  size %dx%dx%d\n"),
		Poi.Origin.X, Poi.Origin.Y, Poi.Origin.Z, Poi.Yaw * 90,
		Poi.RotatedSize.X, Poi.RotatedSize.Y, Poi.RotatedSize.Z);
	Builder.Appendf(TEXT("  entrance: (%d, %d, %d)\n"), Entrance.X, Entrance.Y, Entrance.Z);

	TArray<FMadPoiWorldMarker> Markers;
	GetWorldMarkers(Poi, Markers);
	for (const FMadPoiWorldMarker& Marker : Markers)
	{
		Builder.Appendf(TEXT("  marker %-9s at (%d, %d, %d)"),
			*Marker.Type.ToString(), Marker.WorldPosition.X, Marker.WorldPosition.Y, Marker.WorldPosition.Z);
		if (!Marker.LootTable.IsNone())  { Builder.Appendf(TEXT("  loot=%s tier %d"), *Marker.LootTable.ToString(), Marker.Tier); }
		if (!Marker.SpawnGroup.IsNone()) { Builder.Appendf(TEXT("  spawn=%s x%d"), *Marker.SpawnGroup.ToString(), Marker.Count); }
		Builder.Append(TEXT("\n"));
	}

	return Builder.ToString();
}
