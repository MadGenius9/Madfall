// Copyright MadFall. All Rights Reserved.

#include "MadPrefabRegistry.h"

#include "HAL/FileManager.h"
#include "MadFallCore.h"
#include "Misc/FileHelper.h"
#include "Misc/StringBuilder.h"

void FMadPrefabRegistry::Reset()
{
	Prefabs.Reset();
	IdToIndex.Reset();
}

int32 FMadPrefabRegistry::AddFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	if (!IFileManager::Get().DirectoryExists(*Directory))
	{
		return 0;
	}

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.json"), true, false);
	Files.Sort();

	int32 Loaded = 0;

	for (const FString& File : Files)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File))
		{
			OutErrors.Add(FMadDefinitionError{ File, FString(), TEXT("could not be read from disk") });
			continue;
		}

		FMadPrefab Prefab;
		if (MadFall::PrefabJson::ParseText(Text, File, ModId, Prefab, OutErrors))
		{
			Add(MoveTemp(Prefab));
			++Loaded;
		}
	}

	return Loaded;
}

void FMadPrefabRegistry::Add(FMadPrefab&& Prefab)
{
	if (const int32* Existing = IdToIndex.Find(Prefab.Id))
	{
		UE_LOG(LogMadFallRegistry, Warning,
			TEXT("Prefab '%s' is defined more than once. '%s' overrides '%s'."),
			*Prefab.Id.ToString(), *Prefab.SourcePath, *Prefabs[*Existing].SourcePath);
		Prefabs[*Existing] = MoveTemp(Prefab);
		return;
	}

	IdToIndex.Add(Prefab.Id, Prefabs.Num());
	Prefabs.Add(MoveTemp(Prefab));
}

void FMadPrefabRegistry::Finalize()
{
	// Placement picks a prefab by index from a seeded roll. If the order depended
	// on how the filesystem enumerated the folder, the same seed would place a
	// bunker on one machine and a cabin on another.
	Prefabs.Sort([](const FMadPrefab& A, const FMadPrefab& B) { return A.Id.LexicalLess(B.Id); });

	IdToIndex.Reset();
	for (int32 Index = 0; Index < Prefabs.Num(); ++Index)
	{
		IdToIndex.Add(Prefabs[Index].Id, Index);
	}

	UE_LOG(LogMadFallRegistry, Log, TEXT("Prefab registry loaded: %d prefabs."), Prefabs.Num());
}

int32 FMadPrefabRegistry::FindIndex(FName Id) const
{
	const int32* Found = IdToIndex.Find(Id);
	return Found ? *Found : INDEX_NONE;
}

FString FMadPrefabRegistry::DescribeContents() const
{
	TStringBuilder<2048> Builder;
	Builder.Appendf(TEXT("Prefab registry: %d prefabs\n"), Prefabs.Num());

	for (int32 Index = 0; Index < Prefabs.Num(); ++Index)
	{
		const FMadPrefab& P = Prefabs[Index];
		Builder.Appendf(TEXT("  [%2d] %-28s tier %d  size %dx%dx%d  solid %6d  markers %2d  rarity %.2f  biomes %d\n"),
			Index, *P.Id.ToString(), P.Tier, P.Size.X, P.Size.Y, P.Size.Z,
			P.CountSolidVoxels(), P.Markers.Num(), P.Placement.Rarity, P.Placement.Biomes.Num());
	}

	return Builder.ToString();
}
