// Copyright MadFall. All Rights Reserved.

#include "MadModManager.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "MadFallModAPI.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FMadModManager GModManager;
}

FMadModManager& MadFall::GetModManager()
{
	return GModManager;
}

void FMadModManager::Discover(const FString& ModsRoot, const FString& DisabledListPath)
{
	bDiscovered = true;
	DisabledPath = DisabledListPath;
	Disabled.Reset();

	// --- the player's disabled list ------------------------------------------------
	FString DisabledText;
	if (FFileHelper::LoadFileToString(DisabledText, *DisabledListPath))
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(DisabledText);
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid() && Root->TryGetArrayField(TEXT("disabled"), Items))
		{
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				if (Item.IsValid() && Item->Type == EJson::String)
				{
					Disabled.Add(FName(*Item->AsString()));
				}
			}
		}
		else
		{
			UE_LOG(LogMadFallMods, Warning, TEXT("%s is not a valid mod settings file; every mod is treated as enabled."), *DisabledListPath);
		}
	}

	// --- manifests -------------------------------------------------------------
	TArray<FString> Folders;
	if (IFileManager::Get().DirectoryExists(*ModsRoot))
	{
		IFileManager::Get().IterateDirectory(*ModsRoot, [&Folders](const TCHAR* Path, bool bIsDirectory)
		{
			if (bIsDirectory)
			{
				Folders.Add(Path);
			}
			return true;
		});
	}
	Folders.Sort();

	TArray<FMadModManifest> Manifests;
	TArray<FMadModIssue> DiscoveryIssues;

	for (const FString& Folder : Folders)
	{
		const FString ManifestPath = FPaths::Combine(Folder, TEXT("mod.json"));
		const FName FolderName(*FPaths::GetCleanFilename(Folder));

		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *ManifestPath))
		{
			DiscoveryIssues.Add(FMadModIssue{ FolderName, false, TEXT("folder has no mod.json and is ignored") });
			continue;
		}

		FMadModManifest Manifest;
		TArray<FString> Errors;
		const bool bOk = FMadModManifest::ParseText(Text, FPaths::ConvertRelativePathToFull(Folder), Manifest, Errors);
		const FName ReportId = Manifest.Id.IsNone() ? FolderName : Manifest.Id;

		for (const FString& Error : Errors)
		{
			const bool bWarning = Error.StartsWith(TEXT("warning: "));
			DiscoveryIssues.Add(FMadModIssue{ ReportId, !bWarning, bWarning ? Error.Mid(9) : Error });
		}
		if (bOk)
		{
			Manifests.Add(MoveTemp(Manifest));
		}
	}

	Resolution = MadFall::ModResolver::Resolve(Manifests, Disabled);
	Resolution.Issues.Insert(DiscoveryIssues, 0);

	UE_LOG(LogMadFallMods, Display, TEXT("Mods: %d found, %d enabled, fingerprint %s."),
		Manifests.Num(), Resolution.LoadOrder.Num(), *Resolution.Fingerprint());
	for (const FMadModIssue& Issue : Resolution.Issues)
	{
		if (Issue.bFatal)
		{
			UE_LOG(LogMadFallMods, Error, TEXT("%s"), *Issue.ToString());
		}
		else
		{
			UE_LOG(LogMadFallMods, Warning, TEXT("%s"), *Issue.ToString());
		}
	}
}

void FMadModManager::ForEachContentRoot(const FString& FirstPartyRoot, TFunctionRef<void(const FString& Directory, FName ModId)> Visit) const
{
	Visit(FirstPartyRoot, FName(MadFall::CoreModId));
	for (const FMadModManifest& Mod : Resolution.LoadOrder)
	{
		Visit(Mod.Directory, Mod.Id);
	}
}

int32 FMadModManager::MountPaks()
{
	int32 Mounted = 0;
	for (const FMadModManifest& Mod : Resolution.LoadOrder)
	{
		for (const FString& Pak : Mod.Paks)
		{
			const FString Path = FPaths::Combine(Mod.Directory, Pak);
			if (MountedPaks.Contains(Path))
			{
				continue;
			}
			if (!IFileManager::Get().FileExists(*Path))
			{
				// A warning, not an error: the mod's data still loads, and a missing
				// pak is the normal state of a content mod before it is built - the
				// cook that builds it would otherwise fail on this very message.
				UE_LOG(LogMadFallMods, Warning, TEXT("%s: pak '%s' does not exist; its content is unavailable (build it with Scripts/PackageMod.ps1 -Mod %s)."),
					*Mod.Id.ToString(), *Path, *Mod.Id.ToString());
				continue;
			}

			// The pak platform file only exists when the game runs from paks
			// (packaged builds, or -pak). An uncooked editor session has nothing
			// to mount into; say so rather than failing silently.
			if (!FCoreDelegates::MountPak.IsBound())
			{
				UE_LOG(LogMadFallMods, Warning, TEXT("%s: pak '%s' not mounted - this session is not running from paks (packaged builds mount them)."),
					*Mod.Id.ToString(), *Pak);
				continue;
			}

			// Priority above the base game so a mod pak can shadow shipped assets,
			// and rising with load order so later mods win.
			const int32 Priority = 1000 + Mounted;
			if (FCoreDelegates::MountPak.Execute(Path, Priority) != nullptr)
			{
				MountedPaks.Add(Path);
				++Mounted;
				UE_LOG(LogMadFallMods, Display, TEXT("%s: mounted %s (priority %d)."), *Mod.Id.ToString(), *Pak, Priority);
			}
			else
			{
				UE_LOG(LogMadFallMods, Error, TEXT("%s: failed to mount %s."), *Mod.Id.ToString(), *Path);
			}
		}
	}
	return Mounted;
}

bool FMadModManager::SetModEnabled(FName ModId, bool bEnabled)
{
	if (bEnabled)
	{
		Disabled.Remove(ModId);
	}
	else
	{
		Disabled.Add(ModId);
	}

	TArray<FName> Sorted = Disabled.Array();
	Sorted.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FName& Id : Sorted)
	{
		Items.Add(MakeShared<FJsonValueString>(Id.ToString()));
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("disabled"), Items);

	FString Out;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Out));
	return FFileHelper::SaveStringToFile(Out, *DisabledPath);
}

FString FMadModManager::Describe() const
{
	FString Out = FString::Printf(TEXT("Load order (fingerprint %s):\n  0. madfall (first-party)\n"), *Resolution.Fingerprint());
	for (int32 Index = 0; Index < Resolution.LoadOrder.Num(); ++Index)
	{
		const FMadModManifest& Mod = Resolution.LoadOrder[Index];
		FString Tiers;
		if (IFileManager::Get().DirectoryExists(*FPaths::Combine(Mod.Directory, TEXT("definitions")))) { Tiers += TEXT("data "); }
		if (Mod.Paks.Num() > 0) { Tiers += TEXT("content "); }
		if (Mod.Scripts.Num() > 0) { Tiers += TEXT("script "); }
		Out += FString::Printf(TEXT("  %d. %s %s \"%s\" [%s]\n"), Index + 1, *Mod.Id.ToString(), *Mod.Version.ToString(), *Mod.Name, *Tiers.TrimEnd());
	}

	for (const TPair<FName, EMadModStatus>& Pair : Resolution.Status)
	{
		if (Pair.Value == EMadModStatus::DisabledByUser)
		{
			Out += FString::Printf(TEXT("  disabled: %s\n"), *Pair.Key.ToString());
		}
	}
	for (const FMadModIssue& Issue : Resolution.Issues)
	{
		Out += FString::Printf(TEXT("  %s\n"), *Issue.ToString());
	}
	return Out;
}
