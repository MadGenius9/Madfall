// Copyright MadFall. All Rights Reserved.

#include "MadBiomeRegistry.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "MadDefinitionPatches.h"
#include "MadFallCore.h"
#include "Misc/FileHelper.h"
#include "Misc/StringBuilder.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FMadBiomeRegistry::FMadBiomeRegistry()
{
	BeginLoad();
}

void FMadBiomeRegistry::BeginLoad()
{
	Biomes.Reset();
	IdToIndex.Reset();
	Pending.Reset();
	PendingByIndex.Reset();
	bLoaded = false;
}

int32 FMadBiomeRegistry::AddFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	if (!IFileManager::Get().DirectoryExists(*Directory))
	{
		return 0;
	}

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.json"), true, false);

	// Same reason as the block registry: without a sort, two machines with the
	// same mods could order biomes differently and pick different winners for
	// the same climate sample.
	Files.Sort();

	int32 Staged = 0;

	for (const FString& File : Files)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *File))
		{
			OutErrors.Add(FMadDefinitionError{ File, FString(), TEXT("could not be read from disk") });
			continue;
		}

		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		TSharedPtr<FJsonValue> Root;
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutErrors.Add(FMadDefinitionError{ File, FString(),
				FString::Printf(TEXT("not valid JSON: %s"), *Reader->GetErrorMessage()) });
			continue;
		}

		TArray<TSharedPtr<FJsonValue>> Objects;
		if (Root->Type == EJson::Array) { Objects = Root->AsArray(); }
		else if (Root->Type == EJson::Object) { Objects.Add(Root); }
		else
		{
			OutErrors.Add(FMadDefinitionError{ File, FString(), TEXT("expected an object or an array of objects") });
			continue;
		}

		for (const TSharedPtr<FJsonValue>& Value : Objects)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				OutErrors.Add(FMadDefinitionError{ File, FString(), TEXT("array entries must be objects") });
				continue;
			}

			const TSharedRef<FJsonObject> Object = Value->AsObject().ToSharedRef();

			FMadBiomeDefinitionData Probe;
			TArray<FMadDefinitionError> ProbeErrors;
			if (!MadFall::BiomeDefinitionJson::ParseObject(Object, File, ModId, Probe, ProbeErrors))
			{
				OutErrors.Append(ProbeErrors);
				continue;
			}

			FPending PendingDef;
			PendingDef.Id = Probe.Id;
			PendingDef.Extends = Probe.Extends;
			PendingDef.ModId = ModId;
			PendingDef.SourcePath = File;
			PendingDef.Json = Object;

			if (const int32* Existing = PendingByIndex.Find(Probe.Id))
			{
				UE_LOG(LogMadFallRegistry, Warning,
					TEXT("Biome '%s' is defined more than once. '%s' (mod '%s') overrides '%s' (mod '%s')."),
					*Probe.Id.ToString(), *File, *ModId.ToString(),
					*Pending[*Existing].SourcePath, *Pending[*Existing].ModId.ToString());
				Pending[*Existing] = MoveTemp(PendingDef);
			}
			else
			{
				PendingByIndex.Add(Probe.Id, Pending.Num());
				Pending.Add(MoveTemp(PendingDef));
			}

			++Staged;
		}
	}

	return Staged;
}

bool FMadBiomeRegistry::ResolveDefinition(FName Id, TSet<FName>& Visiting,
	TMap<FName, FMadBiomeDefinitionData>& Resolved, TArray<FMadDefinitionError>& OutErrors)
{
	if (Resolved.Contains(Id))
	{
		return true;
	}

	const int32* PendingIndex = PendingByIndex.Find(Id);
	if (PendingIndex == nullptr)
	{
		return false;
	}

	const FPending& Def = Pending[*PendingIndex];

	if (Visiting.Contains(Id))
	{
		OutErrors.Add(FMadDefinitionError{ Def.SourcePath, TEXT("/extends"),
			FString::Printf(TEXT("'%s' is part of an inheritance cycle"), *Id.ToString()) });
		return false;
	}

	Visiting.Add(Id);

	FMadBiomeDefinitionData Data;

	if (!Def.Extends.IsNone())
	{
		if (!ResolveDefinition(Def.Extends, Visiting, Resolved, OutErrors))
		{
			OutErrors.Add(FMadDefinitionError{ Def.SourcePath, TEXT("/extends"),
				FString::Printf(TEXT("parent biome '%s' does not exist or failed to load"), *Def.Extends.ToString()) });
			Visiting.Remove(Id);
			return false;
		}

		Data = Resolved[Def.Extends];
	}

	if (Def.Json.IsValid())
	{
		TArray<FMadDefinitionError> ParseErrors;
		if (!MadFall::BiomeDefinitionJson::ParseObject(Def.Json.ToSharedRef(), Def.SourcePath, Def.ModId, Data, ParseErrors))
		{
			OutErrors.Append(ParseErrors);
			Visiting.Remove(Id);
			return false;
		}
		OutErrors.Append(ParseErrors);
	}

	Visiting.Remove(Id);
	Resolved.Add(Id, MoveTemp(Data));
	return true;
}

void FMadBiomeRegistry::FinishLoad(TArray<FMadDefinitionError>& OutErrors)
{
	TMap<FName, FMadBiomeDefinitionData> Resolved;
	TSet<FName> Visiting;

	for (const FPending& Def : Pending)
	{
		ResolveDefinition(Def.Id, Visiting, Resolved, OutErrors);
	}

	TArray<FName> SortedIds;
	Resolved.GenerateKeyArray(SortedIds);
	SortedIds.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

	for (const FName& Id : SortedIds)
	{
		// A biome that only exists to be inherited from - a "base_" template -
		// has no climate ranges worth matching. Keeping it in the list is
		// harmless: its weight can be set to 0 to keep it out of selection.
		IdToIndex.Add(Id, Biomes.Num());
		Biomes.Add(Resolved[Id]);
	}

	Pending.Reset();
	PendingByIndex.Reset();
	bLoaded = true;

	UE_LOG(LogMadFallRegistry, Log, TEXT("Biome registry loaded: %d biomes."), Biomes.Num());

	for (const FMadDefinitionError& Error : OutErrors)
	{
		UE_LOG(LogMadFallRegistry, Warning, TEXT("%s"), *Error.ToString());
	}
}

int32 FMadBiomeRegistry::FindIndex(FName BiomeId) const
{
	const int32* Found = IdToIndex.Find(BiomeId);
	return Found ? *Found : INDEX_NONE;
}

void FMadBiomeRegistry::SampleBiomes(float Temperature, float Moisture, float Continentalness,
	TArray<FMadBiomeSample>& OutSamples, int32 MaxResults) const
{
	OutSamples.Reset();

	if (Biomes.Num() == 0)
	{
		return;
	}

	TArray<FMadBiomeSample, TInlineAllocator<16>> Scored;
	Scored.Reserve(Biomes.Num());

	for (int32 Index = 0; Index < Biomes.Num(); ++Index)
	{
		const float Score = Biomes[Index].ScoreClimate(Temperature, Moisture, Continentalness);
		if (Score > 0.0f)
		{
			Scored.Add(FMadBiomeSample{ Index, Score });
		}
	}

	if (Scored.Num() == 0)
	{
		// Nothing claims this climate. Falling back to the single best partial
		// match keeps the world generating: a hole where no biome applied would
		// be a far worse failure than slightly wrong terrain.
		int32 BestIndex = 0;
		float BestScore = -1.0f;

		for (int32 Index = 0; Index < Biomes.Num(); ++Index)
		{
			// Score again with a very wide falloff so something always wins.
			const float Score = Biomes[Index].Temperature.Score(Temperature, 10.0f)
				* Biomes[Index].Moisture.Score(Moisture, 10.0f)
				* Biomes[Index].Continentalness.Score(Continentalness, 10.0f)
				* FMath::Max(Biomes[Index].Weight, KINDA_SMALL_NUMBER);

			if (Score > BestScore)
			{
				BestScore = Score;
				BestIndex = Index;
			}
		}

		OutSamples.Add(FMadBiomeSample{ BestIndex, 1.0f });
		return;
	}

	Scored.Sort([](const FMadBiomeSample& A, const FMadBiomeSample& B) { return A.Weight > B.Weight; });

	const int32 Count = FMath::Min(MaxResults, Scored.Num());

	// Sharpen before normalising.
	//
	// Linear normalisation makes a column that scores 1.0 / 0.9 / 0.8 blend at
	// 37/33/30, which is barely a preference at all. Every column ends up an
	// average of three biomes, and a biome's distinctive terrain - a highlands
	// biome's 40-voxel base height, say - never actually appears anywhere. A
	// survey caught this: mountains topped out at 38 when their base height
	// alone was 40. Cubing turns 1.0 / 0.9 / 0.8 into 51/37/26, which still
	// blends smoothly across a border but lets a biome be itself in its middle.
	float Total = 0.0f;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float Sharpened = Scored[Index].Weight * Scored[Index].Weight * Scored[Index].Weight;
		Scored[Index].Weight = Sharpened;
		Total += Sharpened;
	}

	if (Total <= KINDA_SMALL_NUMBER)
	{
		OutSamples.Add(FMadBiomeSample{ Scored[0].BiomeIndex, 1.0f });
		return;
	}

	for (int32 Index = 0; Index < Count; ++Index)
	{
		OutSamples.Add(FMadBiomeSample{ Scored[Index].BiomeIndex, Scored[Index].Weight / Total });
	}
}

int32 FMadBiomeRegistry::PickDominant(float Temperature, float Moisture, float Continentalness) const
{
	TArray<FMadBiomeSample> Samples;
	SampleBiomes(Temperature, Moisture, Continentalness, Samples, 1);
	return Samples.Num() > 0 ? Samples[0].BiomeIndex : INDEX_NONE;
}

FString FMadBiomeRegistry::DescribeContents() const
{
	TStringBuilder<2048> Builder;
	Builder.Appendf(TEXT("Biome registry: %d biomes\n"), Biomes.Num());

	for (int32 Index = 0; Index < Biomes.Num(); ++Index)
	{
		const FMadBiomeDefinitionData& Biome = Biomes[Index];
		Builder.Appendf(TEXT("  [%2d] %-28s T[%.2f..%.2f] M[%.2f..%.2f] C[%.2f..%.2f] h=%.0f+/-%.0f ridge=%.2f w=%.2f\n"),
			Index, *Biome.Id.ToString(),
			Biome.Temperature.Min, Biome.Temperature.Max,
			Biome.Moisture.Min, Biome.Moisture.Max,
			Biome.Continentalness.Min, Biome.Continentalness.Max,
			Biome.BaseHeight, Biome.HeightVariation, Biome.Ridging, Biome.Weight);
	}

	return Builder.ToString();
}

void FMadBiomeRegistry::ApplyPatches(const FMadPatchSet& Patches, TArray<FMadDefinitionError>& OutErrors)
{
	static const FName Kind(TEXT("biome"));
	TSet<FName> Known;
	for (FPending& Def : Pending)
	{
		Known.Add(Def.Id);
		if (Def.Json.IsValid())
		{
			Patches.ApplyTo(Kind, Def.Id, Def.Json.ToSharedRef(), OutErrors);
		}
	}
	Patches.ReportUnmatched(Kind, Known, OutErrors);
}
