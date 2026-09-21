// Copyright MadFall. All Rights Reserved.

#include "MadWorldGenerator.h"

#include "MadBlockRegistry.h"
#include "MadFallCore.h"
#include "MadFallStats.h"
#include "MadWorldScatter.h"
#include "Misc/StringBuilder.h"

DECLARE_CYCLE_STAT(TEXT("WorldGen Chunk"), STAT_MadWorldGenChunk, STATGROUP_MadFallWorldGen);
DECLARE_CYCLE_STAT(TEXT("WorldGen Columns"), STAT_MadWorldGenColumns, STATGROUP_MadFallWorldGen);
DECLARE_CYCLE_STAT(TEXT("WorldGen Density"), STAT_MadWorldGenDensity, STATGROUP_MadFallWorldGen);
DECLARE_CYCLE_STAT(TEXT("WorldGen Ores"), STAT_MadWorldGenOres, STATGROUP_MadFallWorldGen);
DECLARE_CYCLE_STAT(TEXT("WorldGen Scatter"), STAT_MadWorldGenScatter, STATGROUP_MadFallWorldGen);

namespace
{
	using MadFall::ChunkSize;

	/**
	 * Maps noise output into [0, 1], expanding it by Contrast first.
	 *
	 * See FMadWorldGenSettings::ClimateContrast for why the expansion is not
	 * optional: without it the field never reaches the ends of its own range and
	 * biomes defined there are silently unreachable.
	 */
	FORCEINLINE float ToUnit(float NoiseValue, float Contrast)
	{
		return FMath::Clamp(NoiseValue * Contrast * 0.5f + 0.5f, 0.0f, 1.0f);
	}
}

uint64 FMadWorldGenSettings::GetGenerationVersion() const
{
	// Every field that changes the shape of the world feeds this. A chunk
	// generated before a tuning change and one generated after would not meet
	// cleanly, so the region loader has to be able to tell them apart.
	uint32 Hash = 0x4D414447u;   // 'MADG'
	auto Mix = [&Hash](uint32 Value) { Hash = MadFall::Noise::HashInt(Hash ^ Value); };

	// memcpy rather than reinterpret_cast: type-punning a float through a
	// uint32 pointer is undefined behaviour, and the optimiser is entitled to
	// assume it never happens.
	auto MixFloat = [&Mix](float Value) { uint32 Bits = 0; FMemory::Memcpy(&Bits, &Value, sizeof(Bits)); Mix(Bits); };

	// The seed changes the shape of the world more than any other field, so it
	// belongs here even though the region header also checks it separately. The
	// header check gives the better error message; this one is the backstop.
	Mix(Seed);
	Mix(static_cast<uint32>(SeaLevel));
	Mix(static_cast<uint32>(BedrockTop));
	MixFloat(ContinentFrequency);
	MixFloat(ClimateFrequency);
	MixFloat(ClimateContrast);
	MixFloat(WarpStrength);
	MixFloat(WarpFrequency);
	MixFloat(CaveThreshold);
	MixFloat(CaveFrequency);
	Mix(static_cast<uint32>(CaveSurfaceMargin));
	Mix(static_cast<uint32>(PoiCellChunks));
	MixFloat(PoiDensity);
	Mix(static_cast<uint32>(PoiCellMargin));
	Mix(static_cast<uint32>(TierDistanceStep));
	Mix(bRoads ? 1u : 0u);
	Mix(static_cast<uint32>(RoadWidth));
	Mix(static_cast<uint32>(RoadMaxLength));
	MixFloat(RoadMeander);

	// Generator algorithm revision. Bump by hand when the code changes shape,
	// because the parameters alone cannot detect that.
	// 2: POIs and roads.
	// 3: biome scatter (trees, boulders, plants).
	// 4: road banks (cuttings and embankments eased to the terrain).
	// 5: shipped biomes retuned for relief (taller, more ridged highlands; hillier
	//    forest and tundra). Biome data does not feed this hash, as with mods, so
	//    a retune of the shipped set bumps it by hand.
	// 6: highlands became mountains (base 74, variation 78, ridging 0.95), which
	//    streaming can now follow: chunk columns load the layers their ground
	//    reaches instead of a fixed window around the player.
	// 7: mountains settled at base 62, variation 54, ridging 0.92 (peaks around
	//    116) after measuring what the taller ones cost to mesh.
	// 8: iron outcrops on the upper slopes of the highlands (a second, richer
	//    band from z 61 up), so a mountain is worth climbing rather than only
	//    worth looking at.
	// 9: trees and bushes anchored - the ground under each trunk and bush, and
	//    round it, is made fully solid so the smooth surface meets the cube's
	//    underside instead of dipping up to half a voxel below it.
	Mix(9u);

	return (static_cast<uint64>(Hash) << 32) | MadFall::Noise::HashInt(Hash);
}

// ===========================================================================
// Construction
// ===========================================================================

FMadWorldGenerator::FMadWorldGenerator(const FMadWorldGenSettings& InSettings,
	const FMadBiomeRegistry& InBiomes, FMadBlockRegistry& InBlocks, const FMadPrefabRegistry* InPrefabs)
	: Settings(InSettings)
	, Biomes(InBiomes)
	, Blocks(InBlocks)
{
	// One derived seed per field. Using the raw seed everywhere would correlate
	// the fields - mountains would always sit where it is hot, because both
	// would be reading the same lattice.
	ContinentSeed   = MadFall::Noise::HashInt(Settings.Seed ^ 0xC0117E17u);
	TemperatureSeed = MadFall::Noise::HashInt(Settings.Seed ^ 0x7E3F0000u);
	MoistureSeed    = MadFall::Noise::HashInt(Settings.Seed ^ 0x3015700Eu);
	HeightSeed      = MadFall::Noise::HashInt(Settings.Seed ^ 0x4E1C4700u);
	WarpSeed        = MadFall::Noise::HashInt(Settings.Seed ^ 0x0A297E00u);
	CaveSeed        = MadFall::Noise::HashInt(Settings.Seed ^ 0xCA7E5000u);
	OreSeed         = MadFall::Noise::HashInt(Settings.Seed ^ 0x05E50000u);
	ScatterSeed     = MadFall::Noise::HashInt(Settings.Seed ^ 0x5CA77E20u);

	Resolved.Air = MadFall::BlockTypeAir;
	Resolved.Bedrock = ResolveBlock(FName(TEXT("madfall:bedrock")));
	Resolved.Water = ResolveBlock(FName(TEXT("madfall:water")));

	// Resolve every biome's blocks once. Doing it per voxel would be a hash
	// lookup 32768 times per chunk for information that never changes.
	BiomeBlocks.Reserve(Biomes.Num());
	for (int32 Index = 0; Index < Biomes.Num(); ++Index)
	{
		const FMadBiomeDefinitionData& Biome = Biomes.Get(Index);

		FBiomeBlocks Entry;
		Entry.Surface = ResolveBlock(Biome.SurfaceBlock);
		Entry.Subsurface = ResolveBlock(Biome.SubsurfaceBlock);
		Entry.Stone = ResolveBlock(Biome.StoneBlock);
		Entry.UnderwaterSurface = ResolveBlock(Biome.UnderwaterSurfaceBlock);

		Entry.Ores.Reserve(Biome.Ores.Num());
		for (const FMadOreDistribution& Ore : Biome.Ores)
		{
			Entry.Ores.Add(ResolveBlock(Ore.Block));
		}

		float BiomeChance = 0.0f;
		for (const FMadScatterFeature& Feature : Biome.Scatter)
		{
			Entry.ScatterBlocks.Add(ResolveBlock(Feature.Block));
			Entry.ScatterLeaves.Add(ResolveBlock(Feature.Leaves));
			const FMadBlockDefinitionData* LeavesDef = Blocks.FindDefinition(Entry.ScatterLeaves.Last());
			Entry.ScatterLeafSpans.Add(LeavesDef ? LeavesDef->MaxHorizontalSpan : 0);
			BiomeChance += Feature.Chance;
			MaxScatterReach = FMath::Max(MaxScatterReach, Feature.GetReach());
			MaxScatterHeight = FMath::Max(MaxScatterHeight,
				Feature.Kind == EMadScatterKind::Tree ? Feature.MaxHeight + 3 : FMath::CeilToInt(Feature.MaxRadius) + 1);
		}
		MaxScatterChance = FMath::Max(MaxScatterChance, FMath::Min(BiomeChance, 1.0f));

		BiomeBlocks.Add(MoveTemp(Entry));
	}

	PoiPlanner.Initialize(Settings, InPrefabs, Biomes, Blocks);
}

uint16 FMadWorldGenerator::ResolveBlock(FName BlockId) const
{
	if (BlockId.IsNone())
	{
		return MadFall::BlockTypeAir;
	}

	const uint16 RuntimeId = Blocks.ResolveRuntimeId(BlockId);

	if (RuntimeId == MadFall::BlockTypeUnresolved)
	{
		// A biome referencing a block no mod provides is a definition bug, not
		// a world-corruption risk: nothing has been saved yet. Say so once and
		// generate stone, which is always present, rather than air - a biome
		// made of holes is harder to diagnose than one made of the wrong rock.
		UE_LOG(LogMadFallRegistry, Warning,
			TEXT("World generation references block '%s', which no installed definition provides. Using stone."),
			*BlockId.ToString());

		return Blocks.ResolveRuntimeId(FName(TEXT("madfall:stone")));
	}

	return RuntimeId;
}

// ===========================================================================
// Fields
// ===========================================================================

float FMadWorldGenerator::GetContinentalness(float WorldX, float WorldY) const
{
	MadFall::Noise::FFractalSettings Fractal;
	Fractal.Octaves = 3;
	Fractal.Frequency = Settings.ContinentFrequency;
	Fractal.Gain = 0.5f;

	return ToUnit(MadFall::Noise::FBM2D(WorldX, WorldY, ContinentSeed, Fractal), Settings.ClimateContrast);
}

float FMadWorldGenerator::GetTemperature(float WorldX, float WorldY, float Height) const
{
	MadFall::Noise::FFractalSettings Fractal;
	Fractal.Octaves = 2;
	Fractal.Frequency = Settings.ClimateFrequency;

	const float Base = ToUnit(MadFall::Noise::FBM2D(WorldX, WorldY, TemperatureSeed, Fractal), Settings.ClimateContrast);

	// Altitude cools. Without this, snow-capped peaks would be a coincidence of
	// where the temperature noise happened to be low rather than a property of
	// being high up, and the same mountain could be tropical on one side.
	const float AltitudeAboveSea = FMath::Max(0.0f, Height - static_cast<float>(Settings.SeaLevel));
	// Measured: with a 96-voxel divisor and a 0.6 cap the lapse pulled the mean
	// temperature from 0.51 down to 0.40 and tundra took a quarter of the world.
	// Softer cooling keeps altitude meaningful without making the whole map cold.
	const float Lapse = FMath::Min(AltitudeAboveSea / 130.0f, 0.35f);

	return FMath::Clamp(Base - Lapse, 0.0f, 1.0f);
}

float FMadWorldGenerator::GetMoisture(float WorldX, float WorldY) const
{
	MadFall::Noise::FFractalSettings Fractal;
	Fractal.Octaves = 3;
	Fractal.Frequency = Settings.ClimateFrequency * 1.31f;   // deliberately not a harmonic of temperature

	return ToUnit(MadFall::Noise::FBM2D(WorldX, WorldY, MoistureSeed, Fractal), Settings.ClimateContrast);
}

void FMadWorldGenerator::ComputeColumnBiomes(float WorldX, float WorldY, TArray<FMadBiomeSample>& OutSamples) const
{
	const float Continentalness = GetContinentalness(WorldX, WorldY);
	const float Moisture = GetMoisture(WorldX, WorldY);

	// Temperature depends on height and height depends on the biome, which is a
	// circular dependency. Broken by sampling temperature at a first-guess
	// height from continentalness alone: a one-iteration fixed point. The
	// alternative - iterating to convergence - costs several noise evaluations
	// per column for a difference no player can see.
	const float GuessHeight = FMath::Lerp(
		static_cast<float>(Settings.SeaLevel) - 24.0f,
		static_cast<float>(Settings.SeaLevel) + 40.0f,
		Continentalness);

	const float Temperature = GetTemperature(WorldX, WorldY, GuessHeight);

	Biomes.SampleBiomes(Temperature, Moisture, Continentalness, OutSamples, 3);
}

float FMadWorldGenerator::GetSurfaceHeight(float WorldX, float WorldY) const
{
	TArray<FMadBiomeSample> Samples;
	return ComputeSurfaceHeight(WorldX, WorldY, Samples);
}

float FMadWorldGenerator::GetWarp(float WorldX, float WorldY, float WorldZ) const
{
	MadFall::Noise::FFractalSettings WarpFractal;
	WarpFractal.Octaves = 2;
	WarpFractal.Frequency = Settings.WarpFrequency;

	return MadFall::Noise::FBM3D(WorldX, WorldY, WorldZ, WarpSeed, WarpFractal) * Settings.WarpStrength;
}

bool FMadWorldGenerator::IsTerrainSolid(int32 WorldX, int32 WorldY, int32 WorldZ) const
{
	if (WorldZ <= Settings.BedrockTop)
	{
		return true;
	}

	const float Xf = static_cast<float>(WorldX);
	const float Yf = static_cast<float>(WorldY);
	const float Zf = static_cast<float>(WorldZ);

	// Exactly the expression GenerateChunk uses, through the same helpers, so a
	// foundation computed from this can never disagree with the terrain it sits on.
	const float Distance = (GetSurfaceHeight(Xf, Yf) + GetWarp(Xf, Yf, Zf)) - Zf;
	return MadFall::DistanceToDensity(Distance) >= 128;
}

int32 FMadWorldGenerator::FindTerrainTopBelow(int32 WorldX, int32 WorldY, int32 StartZ, int32 MaxDepth) const
{
	// The surface height is invariant in Z, so it is computed once for the whole
	// search; only the warp varies per step.
	const float Xf = static_cast<float>(WorldX);
	const float Yf = static_cast<float>(WorldY);
	const float Surface = GetSurfaceHeight(Xf, Yf);

	for (int32 Z = StartZ; Z >= StartZ - MaxDepth; --Z)
	{
		if (Z <= Settings.BedrockTop)
		{
			return Z;
		}

		const float Zf = static_cast<float>(Z);
		if (MadFall::DistanceToDensity((Surface + GetWarp(Xf, Yf, Zf)) - Zf) >= 128)
		{
			return Z;
		}
	}

	return INDEX_NONE;
}

float FMadWorldGenerator::ComputeSurfaceHeight(float WorldX, float WorldY, TArray<FMadBiomeSample>& Samples) const
{
	ComputeColumnBiomes(WorldX, WorldY, Samples);

	if (Samples.Num() == 0)
	{
		return static_cast<float>(Settings.SeaLevel);
	}

	float Height = 0.0f;

	for (const FMadBiomeSample& Sample : Samples)
	{
		const FMadBiomeDefinitionData& Biome = Biomes.Get(Sample.BiomeIndex);

		MadFall::Noise::FFractalSettings Fractal;
		Fractal.Octaves = 4;
		Fractal.Frequency = FMath::Max(Biome.Roughness, KINDA_SMALL_NUMBER);
		Fractal.Gain = 0.5f;

		const float Rolling = MadFall::Noise::FBM2D(WorldX, WorldY, HeightSeed, Fractal);

		// Ridged noise is in [0, 1] and biased high, so it is re-centred before
		// blending or every ridged biome would also be systematically taller.
		const float RidgeRaw = MadFall::Noise::Ridged2D(WorldX, WorldY, HeightSeed, Fractal);
		const float Ridge = RidgeRaw * 2.0f - 1.0f;

		const float Shape = FMath::Lerp(Rolling, Ridge, Biome.Ridging);

		// Blending the FINISHED height of each candidate biome, weighted, is
		// what makes a plains/mountain border a slope instead of a cliff.
		Height += (Biome.BaseHeight + Shape * Biome.HeightVariation) * Sample.Weight;
	}

	return Height;
}

int32 FMadWorldGenerator::GetDominantBiome(float WorldX, float WorldY) const
{
	TArray<FMadBiomeSample> Samples;
	ComputeColumnBiomes(WorldX, WorldY, Samples);
	return Samples.Num() > 0 ? Samples[0].BiomeIndex : INDEX_NONE;
}

// ===========================================================================
// Generation
// ===========================================================================

void FMadWorldGenerator::GenerateChunk(const FMadChunkCoord& Coord, FMadChunkStorage& OutStorage) const
{
	SCOPE_CYCLE_COUNTER(STAT_MadWorldGenChunk);
	TRACE_CPUPROFILER_EVENT_SCOPE(MadFall::GenerateChunk);

	OutStorage = FMadChunkStorage();

	if (Biomes.IsEmpty())
	{
		// No biomes registered means no world to describe. Empty air is the
		// honest result; inventing default terrain here would hide the fact
		// that the biome definitions failed to load.
		return;
	}

	const int32 BaseX = Coord.X * ChunkSize;
	const int32 BaseY = Coord.Y * ChunkSize;
	const int32 BaseZ = Coord.Z * ChunkSize;

	// --- pass 1: per-column surface height and biome ---
	// Done once per column rather than per voxel: 1024 columns instead of
	// 32768 voxels, and the height field does not vary with Z.
	float SurfaceHeights[ChunkSize * ChunkSize];
	int32 ColumnBiomes[ChunkSize * ChunkSize];

	{
		SCOPE_CYCLE_COUNTER(STAT_MadWorldGenColumns);

		TArray<FMadBiomeSample> Samples;

		for (int32 LocalY = 0; LocalY < ChunkSize; ++LocalY)
		{
			for (int32 LocalX = 0; LocalX < ChunkSize; ++LocalX)
			{
				const float WorldX = static_cast<float>(BaseX + LocalX);
				const float WorldY = static_cast<float>(BaseY + LocalY);

				// One implementation shared with GetSurfaceHeight. Two copies of
				// this formula would drift, and a POI foundation computed from one
				// would float above terrain generated by the other.
				const float Height = ComputeSurfaceHeight(WorldX, WorldY, Samples);

				const int32 ColumnIndex = LocalX + ChunkSize * LocalY;
				SurfaceHeights[ColumnIndex] = Height;
				ColumnBiomes[ColumnIndex] = Samples.Num() > 0 ? Samples[0].BiomeIndex : 0;
			}
		}
	}

	// --- pass 2: density, composition and caves ---
	{
		SCOPE_CYCLE_COUNTER(STAT_MadWorldGenDensity);

		MadFall::Noise::FFractalSettings CaveFractal;
		CaveFractal.Octaves = 2;
		CaveFractal.Frequency = Settings.CaveFrequency;
		CaveFractal.Gain = 0.55f;

		for (int32 LocalZ = 0; LocalZ < ChunkSize; ++LocalZ)
		{
			const int32 WorldZ = BaseZ + LocalZ;

			for (int32 LocalY = 0; LocalY < ChunkSize; ++LocalY)
			{
				for (int32 LocalX = 0; LocalX < ChunkSize; ++LocalX)
				{
					const int32 ColumnIndex = LocalX + ChunkSize * LocalY;
					const int32 VoxelIndex = MadFall::VoxelIndex(LocalX, LocalY, LocalZ);

					const float WorldX = static_cast<float>(BaseX + LocalX);
					const float WorldY = static_cast<float>(BaseY + LocalY);
					const float WorldZf = static_cast<float>(WorldZ);

					FMadVoxel Voxel = FMadVoxel::Air();

					// Bedrock: unconditional, so no cave or overhang can open a
					// hole out of the bottom of the world.
					if (WorldZ <= Settings.BedrockTop)
					{
						Voxel.BlockTypeID = Resolved.Bedrock;
						Voxel.Density = 255;
						Voxel.SetFlag(EMadVoxelFlags::Anchor, true);
						OutStorage.SetVoxel(VoxelIndex, Voxel);
						continue;
					}

					// 3D domain warp. The surface becomes a function of
					// (x, y, z) rather than (x, y), which is the only way a
					// heightfield can fold back over itself into an overhang.
					const float Warp = GetWarp(WorldX, WorldY, WorldZf);

					const float Surface = SurfaceHeights[ColumnIndex];
					const float DistanceBelowSurface = (Surface + Warp) - WorldZf;

					uint8 Density = MadFall::DistanceToDensity(DistanceBelowSurface);

					// Caves: ridged 3D noise creases into connected tunnels
					// where plain fBm would make disconnected bubbles.
					if (Density >= 128 && DistanceBelowSurface > Settings.CaveSurfaceMargin)
					{
						const float Cave = MadFall::Noise::Ridged3D(
							WorldX, WorldY * 1.0f, WorldZf * 1.6f, CaveSeed, CaveFractal);

						if (Cave > Settings.CaveThreshold)
						{
							// Fade the carve in over the threshold band so cave
							// walls are smooth surfaces rather than voxel steps.
							const float Excess = (Cave - Settings.CaveThreshold) / (1.0f - Settings.CaveThreshold);
							Density = static_cast<uint8>(FMath::Clamp(
								FMath::RoundToInt(255.0f * (1.0f - FMath::Min(Excess * 2.0f, 1.0f))), 0, 255));
						}
					}

					Voxel.Density = Density;

					if (Density >= 128)
					{
						const int32 BiomeIndex = ColumnBiomes[ColumnIndex];
						const FMadBiomeDefinitionData& Biome = Biomes.Get(BiomeIndex);
						const FBiomeBlocks& Palette = BiomeBlocks[BiomeIndex];

						const float Depth = Surface - WorldZf;

						if (Depth < 1.0f)
						{
							// Below sea level the surface layer is whatever the
							// biome puts underwater - sand on a beach rather
							// than grass growing on a lake bed.
							Voxel.BlockTypeID = (WorldZ <= Settings.SeaLevel)
								? Palette.UnderwaterSurface : Palette.Surface;
						}
						else if (Depth < static_cast<float>(Biome.SubsurfaceDepth))
						{
							Voxel.BlockTypeID = Palette.Subsurface;
						}
						else
						{
							Voxel.BlockTypeID = Palette.Stone;
						}

						// Deep stone anchors the structural solver, so mining a
						// hillside does not make the hill collapse.
						if (Depth > 8.0f)
						{
							Voxel.SetFlag(EMadVoxelFlags::Anchor, true);
						}
					}
					else if (WorldZ <= Settings.SeaLevel)
					{
						Voxel.BlockTypeID = Resolved.Water;
						Voxel.Density = 255;
						Voxel.SetFlag(EMadVoxelFlags::Liquid, true);
					}

					OutStorage.SetVoxel(VoxelIndex, Voxel);
				}
			}
		}
	}

	// --- pass 3: ores ---
	{
		SCOPE_CYCLE_COUNTER(STAT_MadWorldGenOres);

		// Seeded from the chunk coordinate alone, so a chunk's ore is the same
		// whether it is generated first or last, and regenerating it produces
		// the identical result.
		MadFall::Noise::FChunkRandom Random(OreSeed, Coord.X, Coord.Y, Coord.Z);

		// Every biome present in this chunk gets its ore attempts, not just the
		// dominant one - a chunk straddling two biomes should carry both.
		TSet<int32> PresentBiomes;
		for (int32 ColumnIndex = 0; ColumnIndex < ChunkSize * ChunkSize; ++ColumnIndex)
		{
			PresentBiomes.Add(ColumnBiomes[ColumnIndex]);
		}

		for (int32 BiomeIndex : PresentBiomes)
		{
			const FMadBiomeDefinitionData& Biome = Biomes.Get(BiomeIndex);
			const FBiomeBlocks& Palette = BiomeBlocks[BiomeIndex];

			for (int32 OreIndex = 0; OreIndex < Biome.Ores.Num(); ++OreIndex)
			{
				const FMadOreDistribution& Ore = Biome.Ores[OreIndex];
				const uint16 OreBlock = Palette.Ores[OreIndex];

				for (int32 Attempt = 0; Attempt < Ore.AttemptsPerChunk; ++Attempt)
				{
					if (!Random.Chance(Ore.Probability))
					{
						continue;
					}

					const int32 SeedX = Random.NextRange(0, ChunkSize - 1);
					const int32 SeedY = Random.NextRange(0, ChunkSize - 1);
					const int32 SeedZ = Random.NextRange(0, ChunkSize - 1);

					const int32 WorldZ = BaseZ + SeedZ;
					if (WorldZ < Ore.MinZ || WorldZ > Ore.MaxZ)
					{
						continue;
					}

					// A random walk rather than a sphere: veins that wander read
					// as geology, spheres read as placed objects.
					int32 X = SeedX;
					int32 Y = SeedY;
					int32 Z = SeedZ;

					for (int32 Step = 0; Step < Ore.ClusterSize; ++Step)
					{
						if (X < 0 || X >= ChunkSize || Y < 0 || Y >= ChunkSize || Z < 0 || Z >= ChunkSize)
						{
							break;
						}

						const int32 VoxelIndex = MadFall::VoxelIndex(X, Y, Z);
						FMadVoxel Voxel = OutStorage.GetVoxel(VoxelIndex);

						// Only replace the biome's stone. Ore floating in a cave
						// or poking through the grass looks like a bug.
						if (Voxel.BlockTypeID == Palette.Stone && Voxel.Density >= 128)
						{
							Voxel.BlockTypeID = OreBlock;
							OutStorage.SetVoxel(VoxelIndex, Voxel);
						}

						switch (Random.NextUInt() % 6)
						{
						case 0: ++X; break;
						case 1: --X; break;
						case 2: ++Y; break;
						case 3: --Y; break;
						case 4: ++Z; break;
						default: --Z; break;
						}
					}
				}
			}
		}
	}

	// --- pass 4: scatter ---
	// Before roads and POIs, which stay clear of it (IsNearPoiOrRoad) but would
	// otherwise be the ones overwritten.
	GenerateScatter(Coord, SurfaceHeights, ColumnBiomes, OutStorage);

	// --- pass 5: roads, then POIs ---
	// Roads first so a POI stamped on top of a road end overwrites it cleanly at
	// the entrance, rather than a road cutting a trench through the doorway.
	if (PoiPlanner.HasPrefabs())
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MadFall::GeneratePois);

		if (Settings.bRoads)
		{
			TArray<FMadRoadSegment> Roads;
			PoiPlanner.GetRoadsNearChunk(*this, Coord, Roads);
			for (const FMadRoadSegment& Road : Roads)
			{
				PoiPlanner.StampRoad(*this, Road, Coord, OutStorage, SurfaceHeights);
			}
		}

		TArray<FMadPoiInstance> Pois;
		PoiPlanner.GetPoisOverlappingChunk(*this, Coord, Pois);
		for (const FMadPoiInstance& Poi : Pois)
		{
			PoiPlanner.StampPoi(*this, Poi, Coord, OutStorage);
		}
	}

	// Generated terrain is not player-modified, so a chunk nobody has touched
	// can be skipped by the save path entirely and regenerated from the seed.
	OutStorage.Compact();
}

bool FMadWorldGenerator::FindBiomeNear(int32 BiomeIndex, const FIntPoint& Near, int32 MaxRadius, FIntPoint& OutColumn, int32 Step, int32 Margin) const
{
	Step = FMath::Max(Step, 1);
	const int32 Rings = FMath::Max(MaxRadius / Step, 0);
	for (int32 Ring = 0; Ring <= Rings; ++Ring)
	{
		const int32 Samples = FMath::Max(8 * Ring, 1);
		for (int32 Index = 0; Index < Samples; ++Index)
		{
			const float Angle = 2.0f * PI * static_cast<float>(Index) / static_cast<float>(Samples);
			const FIntPoint Column(Near.X + FMath::RoundToInt(FMath::Cos(Angle) * Ring * Step),
				Near.Y + FMath::RoundToInt(FMath::Sin(Angle) * Ring * Step));

			const FIntPoint Probes[5] = { {0, 0}, {Margin, 0}, {-Margin, 0}, {0, Margin}, {0, -Margin} };
			bool bInside = true;
			for (const FIntPoint& Probe : Probes)
			{
				if (GetDominantBiome(static_cast<float>(Column.X + Probe.X), static_cast<float>(Column.Y + Probe.Y)) != BiomeIndex)
				{
					bInside = false;
					break;
				}
			}
			if (bInside)
			{
				OutColumn = Column;
				return true;
			}
		}
	}
	return false;
}

void FMadWorldGenerator::GenerateScatter(const FMadChunkCoord& Coord, const float* SurfaceHeights, const int32* ColumnBiomes,
	FMadChunkStorage& Storage) const
{
	if (MaxScatterChance <= 0.0f)
	{
		return;
	}

	SCOPE_CYCLE_COUNTER(STAT_MadWorldGenScatter);

	const int32 BaseX = Coord.X * ChunkSize;
	const int32 BaseY = Coord.Y * ChunkSize;
	const int32 BaseZ = Coord.Z * ChunkSize;
	const int32 Reach = MaxScatterReach;

	// The generator runs one voxel of warp slack either side of the height
	// field; a root is looked for inside that band, so this is how far the top
	// solid voxel can sit from the blended surface height.
	const int32 WarpSlack = FMath::CeilToInt(Settings.WarpStrength) + 2;

	auto WriteVoxel = [&](const FIntVector& World, uint16 BlockId, bool bCubic, uint8 Density, bool bOnlyIntoAir)
	{
		const int32 LocalX = World.X - BaseX;
		const int32 LocalY = World.Y - BaseY;
		const int32 LocalZ = World.Z - BaseZ;
		if (LocalX < 0 || LocalX >= ChunkSize || LocalY < 0 || LocalY >= ChunkSize || LocalZ < 0 || LocalZ >= ChunkSize)
		{
			return;
		}
		const int32 Index = MadFall::VoxelIndex(LocalX, LocalY, LocalZ);
		FMadVoxel Voxel = Storage.GetVoxel(Index);
		if (Voxel.HasFlag(EMadVoxelFlags::Liquid))
		{
			return;
		}
		if (bOnlyIntoAir && Voxel.IsSolid())
		{
			return;
		}
		Voxel.BlockTypeID = BlockId;
		Voxel.Density = Density;
		Voxel.SetFlag(EMadVoxelFlags::Cubic, bCubic);
		Storage.SetVoxel(Index, Voxel);
	};

	// Cubes planted on smooth ground float. The surface crosses between a
	// ground voxel's centre and the cube's at the density threshold, so it
	// meets the cube's underside only when the ground voxel is full; the top
	// voxel of a slope is often barely over half, and the surface then passes
	// up to half a voxel under the trunk or bush. Filling the voxel under the
	// base and its eight neighbours flattens a footprint the cube stands on.
	// Neighbours that are air (the downhill side) stay air, so the ground still
	// falls away from the trunk rather than growing a pedestal.
	auto AnchorFootprint = [&](const FIntVector& Ground)
	{
		for (int32 DY = -1; DY <= 1; ++DY)
		{
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				const int32 LX = Ground.X + DX - BaseX, LY = Ground.Y + DY - BaseY, LZ = Ground.Z - BaseZ;
				if (LX < 0 || LX >= ChunkSize || LY < 0 || LY >= ChunkSize || LZ < 0 || LZ >= ChunkSize)
				{
					continue;
				}
				const int32 Index = MadFall::VoxelIndex(LX, LY, LZ);
				FMadVoxel Voxel = Storage.GetVoxel(Index);
				if (!Voxel.IsSolid() || Voxel.HasFlag(EMadVoxelFlags::Cubic) || Voxel.HasFlag(EMadVoxelFlags::Liquid))
				{
					continue;
				}
				Voxel.Density = 255;
				Storage.SetVoxel(Index, Voxel);
			}
		}
	};

	TArray<FMadBiomeSample> Samples;
	TArray<FIntVector> Trunk;
	TArray<FIntVector> Leaves;

	// Roots within Reach of the chunk, in one global order (Y then X): two
	// overlapping features resolve the same way in every chunk they touch,
	// because every chunk sees both roots and applies them in the same order.
	for (int32 WorldY = BaseY - Reach; WorldY < BaseY + ChunkSize + Reach; ++WorldY)
	{
		for (int32 WorldX = BaseX - Reach; WorldX < BaseX + ChunkSize + Reach; ++WorldX)
		{
			const float Roll = MadFall::Scatter::RollColumn(ScatterSeed, WorldX, WorldY);
			if (Roll >= MaxScatterChance)
			{
				continue;   // nearly every column stops here, before any noise
			}

			const int32 LocalX = WorldX - BaseX;
			const int32 LocalY = WorldY - BaseY;
			const bool bInside = LocalX >= 0 && LocalX < ChunkSize && LocalY >= 0 && LocalY < ChunkSize;

			float Surface = 0.0f;
			int32 BiomeIndex = 0;
			if (bInside)
			{
				Surface = SurfaceHeights[LocalX + ChunkSize * LocalY];
				BiomeIndex = ColumnBiomes[LocalX + ChunkSize * LocalY];
			}
			else
			{
				Surface = ComputeSurfaceHeight(static_cast<float>(WorldX), static_cast<float>(WorldY), Samples);
				BiomeIndex = Samples.Num() > 0 ? Samples[0].BiomeIndex : 0;
			}

			const FMadBiomeDefinitionData& Biome = Biomes.Get(BiomeIndex);
			int32 FeatureIndex = INDEX_NONE;
			float Cumulative = 0.0f;
			for (int32 Index = 0; Index < Biome.Scatter.Num(); ++Index)
			{
				Cumulative += Biome.Scatter[Index].Chance;
				if (Roll < Cumulative)
				{
					FeatureIndex = Index;
					break;
				}
			}
			if (FeatureIndex == INDEX_NONE)
			{
				continue;
			}

			// Vertical reject before the expensive root search.
			const int32 SurfaceZ = FMath::FloorToInt(Surface);
			if (SurfaceZ - WarpSlack - Reach >= BaseZ + ChunkSize || SurfaceZ + WarpSlack + MaxScatterHeight < BaseZ)
			{
				continue;
			}

			const int32 Top = FindTerrainTopBelow(WorldX, WorldY, SurfaceZ + WarpSlack, WarpSlack * 2);
			if (Top == INDEX_NONE || Top <= Settings.SeaLevel)
			{
				continue;
			}

			const FMadScatterFeature& Feature = Biome.Scatter[FeatureIndex];
			const int32 FeatureReach = Feature.GetReach();
			if (PoiPlanner.HasPrefabs() && PoiPlanner.IsNearPoiOrRoad(*this, WorldX, WorldY, Top, FeatureReach + 1))
			{
				continue;
			}

			const FBiomeBlocks& Palette = BiomeBlocks[BiomeIndex];
			const uint16 Block = Palette.ScatterBlocks[FeatureIndex];
			const uint16 LeavesBlock = Palette.ScatterLeaves[FeatureIndex];
			MadFall::Noise::FChunkRandom Random(ScatterSeed, WorldX, WorldY, 0, 0x7EE5u);
			const FIntVector Root(WorldX, WorldY, Top);

			switch (Feature.Kind)
			{
			case EMadScatterKind::Tree:
			{
				const int32 Height = Random.NextRange(Feature.MinHeight, Feature.MaxHeight);
				const float Radius = FMath::Lerp(Feature.MinRadius, Feature.MaxRadius, Random.NextFloat());
				MadFall::Scatter::BuildTree(Random.NextUInt(), Height, Radius, LeavesBlock != MadFall::BlockTypeAir, Trunk, Leaves,
					Palette.ScatterLeafSpans[FeatureIndex]);

				// Every column the trunk rises from stands on anchored ground.
				int32 LowestTrunk = MAX_int32;
				for (const FIntVector& Offset : Trunk)
				{
					LowestTrunk = FMath::Min(LowestTrunk, Offset.Z);
				}
				for (const FIntVector& Offset : Trunk)
				{
					if (Offset.Z == LowestTrunk)
					{
						// Where the trunk becomes visible, not where it was rooted:
						// a warped bump of hillside can fill the trunk's first voxel,
						// and the trunk then shows from the voxel above it. The
						// ground top comes from the terrain function, not this
						// chunk's storage, so neighbouring chunks agree on it.
						const FIntVector Column = Root + FIntVector(Offset.X, Offset.Y, 0);
						const int32 Ground = FindTerrainTopBelow(Column.X, Column.Y, Root.Z + Height, Height + WarpSlack * 2);
						AnchorFootprint(FIntVector(Column.X, Column.Y, Root.Z + Offset.Z - 1));
						if (Ground != INDEX_NONE && Ground > Root.Z + Offset.Z - 1)
						{
							AnchorFootprint(FIntVector(Column.X, Column.Y, Ground));
						}
					}
				}

				// Trunk over leaves, leaves only into air: two crowns that meet
				// merge, and a trunk is never interrupted by a neighbour's canopy.
				TMap<FIntPoint, int32> LowestWritten;
				for (const FIntVector& Offset : Trunk)
				{
					const FIntVector World = Root + Offset;
					const int32 LX = World.X - BaseX, LY = World.Y - BaseY, LZ = World.Z - BaseZ;
					if (LX >= 0 && LX < ChunkSize && LY >= 0 && LY < ChunkSize && LZ >= 0 && LZ < ChunkSize)
					{
						const FMadVoxel Existing = Storage.GetVoxel(MadFall::VoxelIndex(LX, LY, LZ));
						const bool bFree = !Existing.IsSolid() || Existing.BlockTypeID == LeavesBlock;
						if (bFree)
						{
							WriteVoxel(World, Block, true, 255, false);
							int32& Lowest = LowestWritten.FindOrAdd(FIntPoint(World.X, World.Y), World.Z);
							Lowest = FMath::Min(Lowest, World.Z);
						}
					}
				}
				// And under whatever the trunk actually ended up standing on in
				// this chunk: a neighbouring boulder can have put a soft edge of
				// stone where the trunk's first voxel was meant to go.
				for (const TPair<FIntPoint, int32>& Base : LowestWritten)
				{
					AnchorFootprint(FIntVector(Base.Key.X, Base.Key.Y, Base.Value - 1));
				}
				for (const FIntVector& Offset : Leaves)
				{
					WriteVoxel(Root + Offset, LeavesBlock, true, 255, true);
				}
				break;
			}
			case EMadScatterKind::Boulder:
			{
				const float Radius = FMath::Lerp(Feature.MinRadius, Feature.MaxRadius, Random.NextFloat());
				// Sunk a third into the ground so it sits rather than perches.
				const FVector3f Centre(static_cast<float>(WorldX) + 0.5f, static_cast<float>(WorldY) + 0.5f, static_cast<float>(Top) + 1.0f - Radius * 0.33f);
				const int32 R = FMath::CeilToInt(Radius) + 1;
				for (int32 DZ = -R; DZ <= R; ++DZ)
				{
					for (int32 DY = -R; DY <= R; ++DY)
					{
						for (int32 DX = -R; DX <= R; ++DX)
						{
							const FIntVector World(WorldX + DX, WorldY + DY, FMath::FloorToInt(Centre.Z) + DZ);
							const int32 LX = World.X - BaseX, LY = World.Y - BaseY, LZ = World.Z - BaseZ;
							if (LX < 0 || LX >= ChunkSize || LY < 0 || LY >= ChunkSize || LZ < 0 || LZ >= ChunkSize)
							{
								continue;
							}
							const FVector3f P(static_cast<float>(World.X) + 0.5f, static_cast<float>(World.Y) + 0.5f, static_cast<float>(World.Z) + 0.5f);
							const uint8 Density = MadFall::DistanceToDensity(Radius - FVector3f::Distance(P, Centre));
							const FMadVoxel Existing = Storage.GetVoxel(MadFall::VoxelIndex(LX, LY, LZ));
							// Terrain-style: smooth, never over construction or a
							// denser voxel, so it blends into the hillside it sits in.
							if (Density < 128 || Existing.HasFlag(EMadVoxelFlags::Cubic) || Density <= Existing.Density)
							{
								continue;
							}
							WriteVoxel(World, Block, false, Density, false);
						}
					}
				}
				break;
			}
			case EMadScatterKind::Plant:
			default:
				AnchorFootprint(Root);
				WriteVoxel(Root + FIntVector(0, 0, 1), Block, true, 255, true);
				break;
			}
		}
	}
}

FString FMadWorldGenerator::ProbeColumn(int32 WorldX, int32 WorldY) const
{
	const float Xf = static_cast<float>(WorldX);
	const float Yf = static_cast<float>(WorldY);

	const float Continentalness = GetContinentalness(Xf, Yf);
	const float Moisture = GetMoisture(Xf, Yf);
	const float Height = GetSurfaceHeight(Xf, Yf);
	const float Temperature = GetTemperature(Xf, Yf, Height);

	TArray<FMadBiomeSample> Samples;
	ComputeColumnBiomes(Xf, Yf, Samples);

	TStringBuilder<1024> Builder;
	Builder.Appendf(TEXT("World column (%d, %d)\n"), WorldX, WorldY);
	Builder.Appendf(TEXT("  seed:            %u\n"), Settings.Seed);
	Builder.Appendf(TEXT("  continentalness: %.3f\n"), Continentalness);
	Builder.Appendf(TEXT("  temperature:     %.3f\n"), Temperature);
	Builder.Appendf(TEXT("  moisture:        %.3f\n"), Moisture);
	Builder.Appendf(TEXT("  surface height:  %.2f  (sea level %d)\n"), Height, Settings.SeaLevel);
	Builder.Appendf(TEXT("  biome blend:\n"));

	for (const FMadBiomeSample& Sample : Samples)
	{
		Builder.Appendf(TEXT("    %-28s %.3f\n"),
			*Biomes.Get(Sample.BiomeIndex).Id.ToString(), Sample.Weight);
	}

	return Builder.ToString();
}
