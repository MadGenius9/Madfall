// Copyright MadFall. All Rights Reserved.

#include "MadBiomeDefinition.h"

#include "MadFallCore.h"
#include "MadFallVoxelTypes.h"
#include "MadJsonReader.h"

namespace MadFall::BiomeDefinitionJson
{
	namespace
	{
		using FReader = FMadJsonReader;

		void ParseClimate(const FReader& R, const TSharedRef<FJsonObject>& Climate, FMadBiomeDefinitionData& Data)
		{
			R.ReadRange(Climate, TEXT("temperature"), TEXT("/climate/temperature"),
				Data.Temperature.Min, Data.Temperature.Max);
			R.ReadRange(Climate, TEXT("moisture"), TEXT("/climate/moisture"),
				Data.Moisture.Min, Data.Moisture.Max);
			R.ReadRange(Climate, TEXT("continentalness"), TEXT("/climate/continentalness"),
				Data.Continentalness.Min, Data.Continentalness.Max);
			R.ReadFloat(Climate, TEXT("weight"), TEXT("/climate/weight"), Data.Weight);
		}

		void ParseTerrain(const FReader& R, const TSharedRef<FJsonObject>& Terrain, FMadBiomeDefinitionData& Data)
		{
			R.ReadFloat(Terrain, TEXT("base_height"), TEXT("/terrain/base_height"), Data.BaseHeight);
			R.ReadFloat(Terrain, TEXT("height_variation"), TEXT("/terrain/height_variation"), Data.HeightVariation);
			R.ReadFloat(Terrain, TEXT("roughness"), TEXT("/terrain/roughness"), Data.Roughness);
			R.ReadFloat(Terrain, TEXT("ridging"), TEXT("/terrain/ridging"), Data.Ridging);

			if (Data.Ridging < 0.0f || Data.Ridging > 1.0f)
			{
				R.AddError(TEXT("/terrain/ridging"),
					FString::Printf(TEXT("must be between 0 and 1; got %.3f, clamped"), Data.Ridging));
				Data.Ridging = FMath::Clamp(Data.Ridging, 0.0f, 1.0f);
			}

			// A base height outside the world would generate a chunk of solid
			// bedrock or open sky and look like the generator had crashed.
			if (Data.BaseHeight < MadFall::WorldMinZ || Data.BaseHeight > MadFall::WorldMaxZ)
			{
				R.AddError(TEXT("/terrain/base_height"),
					FString::Printf(TEXT("%.1f is outside the world's vertical range (%d..%d)"),
						Data.BaseHeight, MadFall::WorldMinZ, MadFall::WorldMaxZ));
				Data.BaseHeight = FMath::Clamp(Data.BaseHeight,
					static_cast<float>(MadFall::WorldMinZ), static_cast<float>(MadFall::WorldMaxZ));
			}
		}

		void ParseBlocks(const FReader& R, const TSharedRef<FJsonObject>& Blocks, FMadBiomeDefinitionData& Data)
		{
			R.ReadName(Blocks, TEXT("surface"), TEXT("/blocks/surface"), Data.SurfaceBlock);
			R.ReadName(Blocks, TEXT("subsurface"), TEXT("/blocks/subsurface"), Data.SubsurfaceBlock);
			R.ReadInt(Blocks, TEXT("subsurface_depth"), TEXT("/blocks/subsurface_depth"), Data.SubsurfaceDepth);
			R.ReadName(Blocks, TEXT("stone"), TEXT("/blocks/stone"), Data.StoneBlock);
			R.ReadName(Blocks, TEXT("underwater_surface"), TEXT("/blocks/underwater_surface"), Data.UnderwaterSurfaceBlock);
		}

		void ParseOres(const FReader& R, const TSharedRef<FJsonObject>& Root, FMadBiomeDefinitionData& Data)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			if (!R.ReadArray(Root, TEXT("ores"), TEXT("/ores"), Items))
			{
				return;
			}

			TArray<FMadOreDistribution> Ores;

			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				const FString Pointer = FString::Printf(TEXT("/ores/%d"), Index);

				if (!Items[Index].IsValid() || Items[Index]->Type != EJson::Object)
				{
					R.AddError(Pointer, FString::Printf(TEXT("expected an object, got %s"),
						*FMadJsonReader::DescribeType(Items[Index])));
					continue;
				}

				const TSharedRef<FJsonObject> OreObj = Items[Index]->AsObject().ToSharedRef();
				FMadOreDistribution Ore;

				if (!R.ReadName(OreObj, TEXT("block"), Pointer + TEXT("/block"), Ore.Block))
				{
					R.AddError(Pointer + TEXT("/block"), TEXT("missing required field"));
					continue;
				}

				R.ReadInt(OreObj, TEXT("attempts_per_chunk"), Pointer + TEXT("/attempts_per_chunk"), Ore.AttemptsPerChunk);
				R.ReadInt(OreObj, TEXT("cluster_size"), Pointer + TEXT("/cluster_size"), Ore.ClusterSize);
				R.ReadInt(OreObj, TEXT("min_z"), Pointer + TEXT("/min_z"), Ore.MinZ);
				R.ReadInt(OreObj, TEXT("max_z"), Pointer + TEXT("/max_z"), Ore.MaxZ);
				R.ReadFloat(OreObj, TEXT("probability"), Pointer + TEXT("/probability"), Ore.Probability);

				if (Ore.MinZ > Ore.MaxZ)
				{
					R.AddError(Pointer, FString::Printf(
						TEXT("min_z (%d) is above max_z (%d); the ore would never place. Bounds swapped."),
						Ore.MinZ, Ore.MaxZ));
					Swap(Ore.MinZ, Ore.MaxZ);
				}

				Ore.ClusterSize = FMath::Max(Ore.ClusterSize, 1);
				Ore.AttemptsPerChunk = FMath::Max(Ore.AttemptsPerChunk, 0);
				Ore.Probability = FMath::Clamp(Ore.Probability, 0.0f, 1.0f);

				Ores.Add(Ore);
			}

			Data.Ores = MoveTemp(Ores);
		}

		void ParseScatter(const FReader& R, const TSharedRef<FJsonObject>& Root, FMadBiomeDefinitionData& Data)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			if (!R.ReadArray(Root, TEXT("scatter"), TEXT("/scatter"), Items))
			{
				return;
			}

			TArray<FMadScatterFeature> Features;
			float TotalChance = 0.0f;

			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				const FString Pointer = FString::Printf(TEXT("/scatter/%d"), Index);

				if (!Items[Index].IsValid() || Items[Index]->Type != EJson::Object)
				{
					R.AddError(Pointer, FString::Printf(TEXT("expected an object, got %s"),
						*FMadJsonReader::DescribeType(Items[Index])));
					continue;
				}

				const TSharedRef<FJsonObject> Obj = Items[Index]->AsObject().ToSharedRef();
				FMadScatterFeature Feature;

				FString Kind;
				if (!R.ReadString(Obj, TEXT("feature"), Pointer + TEXT("/feature"), Kind))
				{
					R.AddError(Pointer + TEXT("/feature"), TEXT("missing required field; expected \"tree\", \"boulder\" or \"plant\""));
					continue;
				}
				if (Kind.Equals(TEXT("tree"), ESearchCase::IgnoreCase))         { Feature.Kind = EMadScatterKind::Tree; }
				else if (Kind.Equals(TEXT("boulder"), ESearchCase::IgnoreCase)) { Feature.Kind = EMadScatterKind::Boulder; }
				else if (Kind.Equals(TEXT("plant"), ESearchCase::IgnoreCase))   { Feature.Kind = EMadScatterKind::Plant; }
				else
				{
					R.AddError(Pointer + TEXT("/feature"), FString::Printf(TEXT("unknown feature \"%s\"; expected \"tree\", \"boulder\" or \"plant\""), *Kind));
					continue;
				}

				if (!R.ReadName(Obj, TEXT("block"), Pointer + TEXT("/block"), Feature.Block))
				{
					R.AddError(Pointer + TEXT("/block"), TEXT("missing required field"));
					continue;
				}
				R.ReadName(Obj, TEXT("leaves"), Pointer + TEXT("/leaves"), Feature.Leaves);
				R.ReadFloat(Obj, TEXT("chance"), Pointer + TEXT("/chance"), Feature.Chance);

				float MinHeight = static_cast<float>(Feature.MinHeight);
				float MaxHeight = static_cast<float>(Feature.MaxHeight);
				if (R.ReadRange(Obj, TEXT("height"), Pointer + TEXT("/height"), MinHeight, MaxHeight))
				{
					Feature.MinHeight = FMath::RoundToInt(MinHeight);
					Feature.MaxHeight = FMath::RoundToInt(MaxHeight);
				}
				R.ReadRange(Obj, TEXT("radius"), Pointer + TEXT("/radius"), Feature.MinRadius, Feature.MaxRadius);

				// Clamped rather than rejected, with the reason: a feature wider than
				// this reaches past the neighbour chunks the generator looks at, and
				// would be cut off at chunk borders.
				const float RadiusCap = 6.0f;
				if (Feature.MaxRadius > RadiusCap)
				{
					R.AddError(Pointer + TEXT("/radius"), FString::Printf(TEXT("radius above %.0f is not supported; clamped"), RadiusCap));
				}
				Feature.MinRadius = FMath::Clamp(Feature.MinRadius, 0.0f, RadiusCap);
				Feature.MaxRadius = FMath::Clamp(Feature.MaxRadius, Feature.MinRadius, RadiusCap);
				Feature.MinHeight = FMath::Clamp(Feature.MinHeight, 1, 24);
				Feature.MaxHeight = FMath::Clamp(Feature.MaxHeight, Feature.MinHeight, 24);
				Feature.Chance = FMath::Clamp(Feature.Chance, 0.0f, 1.0f);

				TotalChance += Feature.Chance;
				Features.Add(Feature);

				R.ReportUnknownFields(Obj, { TEXT("feature"), TEXT("block"), TEXT("leaves"), TEXT("chance"), TEXT("height"), TEXT("radius") });
			}

			if (TotalChance > 1.0f)
			{
				R.AddError(TEXT("/scatter"), FString::Printf(
					TEXT("chances add up to %.3f; above 1 the later features can never be picked"), TotalChance));
			}

			Data.Scatter = MoveTemp(Features);
		}
	}

	bool ParseObject(
		const TSharedRef<FJsonObject>& Object,
		const FString& SourcePath,
		FName ModId,
		FMadBiomeDefinitionData& OutData,
		TArray<FMadDefinitionError>& OutErrors)
	{
		const FReader R{ SourcePath, OutErrors };

		FString Schema;
		if (!R.ReadString(Object, TEXT("schema"), TEXT("/schema"), Schema))
		{
			R.AddError(TEXT("/schema"),
				FString::Printf(TEXT("missing required field; expected \"%s\""), MadFall::BiomeSchemaV1));
			return false;
		}

		if (Schema != MadFall::BiomeSchemaV1)
		{
			R.AddError(TEXT("/schema"),
				FString::Printf(TEXT("unsupported schema \"%s\"; this build understands \"%s\""),
					*Schema, MadFall::BiomeSchemaV1));
			return false;
		}

		if (!R.ReadName(Object, TEXT("id"), TEXT("/id"), OutData.Id))
		{
			R.AddError(TEXT("/id"), TEXT("missing required field"));
			return false;
		}

		// Biome ids follow exactly the block id rules - same namespacing, same
		// character set, same reasons.
		FString Reason;
		if (!MadFall::BlockDefinitionJson::IsValidBlockId(OutData.Id, Reason))
		{
			R.AddError(TEXT("/id"), Reason);
			return false;
		}

		const FName Namespace = MadFall::BlockDefinitionJson::GetNamespace(OutData.Id);
		if (!ModId.IsNone() && Namespace != ModId)
		{
			R.AddError(TEXT("/id"),
				FString::Printf(TEXT("namespace '%s' does not match the owning mod id '%s'. To change another mod's biome, ship a patch instead."),
					*Namespace.ToString(), *ModId.ToString()));
			return false;
		}

		R.ReadName(Object, TEXT("extends"), TEXT("/extends"), OutData.Extends);
		R.ReadString(Object, TEXT("display_name"), TEXT("/display_name"), OutData.DisplayName);
		R.ReadNameArray(Object, TEXT("tags"), TEXT("/tags"), OutData.Tags);

		TSharedPtr<FJsonObject> Section;

		if (R.ReadObject(Object, TEXT("climate"), TEXT("/climate"), Section))
		{
			ParseClimate(R, Section.ToSharedRef(), OutData);
		}

		if (R.ReadObject(Object, TEXT("terrain"), TEXT("/terrain"), Section))
		{
			ParseTerrain(R, Section.ToSharedRef(), OutData);
		}

		if (R.ReadObject(Object, TEXT("blocks"), TEXT("/blocks"), Section))
		{
			ParseBlocks(R, Section.ToSharedRef(), OutData);
		}

		ParseOres(R, Object, OutData);
		ParseScatter(R, Object, OutData);

		static const TSet<FString> KnownKeys = {
			TEXT("schema"), TEXT("id"), TEXT("extends"), TEXT("display_name"), TEXT("tags"),
			TEXT("climate"), TEXT("terrain"), TEXT("blocks"), TEXT("ores"), TEXT("scatter"),
			// Reserved for later phases so authoring ahead is not punished.
			TEXT("structures"), TEXT("mobs"), TEXT("ambience"), TEXT("mod_data")
		};
		R.ReportUnknownFields(Object, KnownKeys);

		OutData.SourceModId = ModId;
		OutData.SourcePath = SourcePath;

		return true;
	}
}
