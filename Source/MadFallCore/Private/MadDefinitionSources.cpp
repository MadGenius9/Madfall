// Copyright MadFall. All Rights Reserved.

#include "MadDefinitionSources.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "MadModManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace MadFall::Definitions
{
	void ForEachSource(const TCHAR* KindFolder, TFunctionRef<void(const FString& Directory, FName ModId)> Visit)
	{
		FMadModManager& Mods = MadFall::GetModManager();
		if (!Mods.HasDiscovered())
		{
			// Normally done at PostConfigInit; tools that link Core without the
			// module startup (commandlets) still get the same order.
			Mods.Discover(FPaths::Combine(FPaths::ProjectDir(), TEXT("Mods")),
				FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Config"), TEXT("MadFallMods.json")));
		}

		// Mod ids, not folder names, are the namespaces; and the resolved load
		// order, not directory enumeration, decides who overrides whom.
		Mods.ForEachContentRoot(FPaths::ProjectDir(), [&Visit, KindFolder](const FString& Root, FName ModId)
		{
			const bool bFirstParty = ModId == FName(MadFall::CoreModId);
			Visit(FPaths::Combine(Root, bFirstParty ? TEXT("Definitions") : TEXT("definitions"), KindFolder), ModId);
		});
	}

	int32 ForEachJsonObject(const FString& Directory, TArray<FMadDefinitionError>& OutErrors,
		TFunctionRef<void(const FString& File, const TSharedRef<FJsonObject>& Object)> Visit)
	{
		if (!IFileManager::Get().DirectoryExists(*Directory))
		{
			return 0;
		}

		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.json"), true, false);

		// Sorted so override order between two files of the same mod does not
		// depend on what the filesystem happens to enumerate first.
		Files.Sort();

		int32 Visited = 0;
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

			for (int32 Index = 0; Index < Objects.Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = Objects[Index];
				if (!Value.IsValid() || Value->Type != EJson::Object)
				{
					OutErrors.Add(FMadDefinitionError{ File, FString::Printf(TEXT("/%d"), Index), TEXT("array entries must be objects") });
					continue;
				}

				Visit(File, Value->AsObject().ToSharedRef());
				++Visited;
			}
		}

		return Visited;
	}

	bool IsValidPathId(FName Id, FString& OutReason)
	{
		const FString Text = Id.ToString();

		if (Text.StartsWith(TEXT("/")) || Text.EndsWith(TEXT("/")) || Text.Contains(TEXT("//")) || Text.Contains(TEXT(":/")))
		{
			OutReason = FString::Printf(TEXT("'%s' has an empty path segment"), *Text);
			return false;
		}

		// Everything else is the block rule with '/' allowed as a separator.
		FString Flattened = Text.Replace(TEXT("/"), TEXT("_"));
		if (!MadFall::BlockDefinitionJson::IsValidBlockId(FName(*Flattened), OutReason))
		{
			OutReason = OutReason.Replace(*Flattened, *Text);
			return false;
		}
		return true;
	}
}
