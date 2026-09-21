// Copyright MadFall. All Rights Reserved.

#include "MadSession.h"

#include "Algo/AllOf.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	const TCHAR* WorldInfoFile = TEXT("world.json");
	const TCHAR* WorldSchema = TEXT("madfall.world/1");

	enum class EStartState : uint8 { Undecided, Title, Game };
	EStartState GStart = EStartState::Undecided;

	FString GSelectedName;
	TOptional<int64> GSelectedSeed;
	FCriticalSection GSessionLock;
}

FString MadFall::Session::ChooseWorldsRoot(bool bPackaged, const FString& Override, const FString& ProjectSavedDir, const FString& UserDir)
{
	if (!Override.IsEmpty())
	{
		return FPaths::ConvertRelativePathToFull(Override);
	}
	if (bPackaged)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(UserDir, TEXT("MadFall"), TEXT("Worlds")));
	}
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(ProjectSavedDir, TEXT("MadFallWorlds")));
}

FString MadFall::Session::GetWorldsRoot()
{
	FString Override;
	FParse::Value(FCommandLine::Get(), TEXT("MadWorldsDir="), Override);
	// A cooked build is what a player runs; the editor, -game from the editor
	// binaries and automation all run uncooked.
	return ChooseWorldsRoot(FPlatformProperties::RequiresCookedData(), Override, FPaths::ProjectSavedDir(), FPlatformProcess::UserSettingsDir());
}

bool MadFall::Session::IsValidWorldName(const FString& Name)
{
	return !Name.IsEmpty() && Name.Len() <= 64 && Name[0] != TEXT('_')
		&& Algo::AllOf(Name, [](TCHAR C) { return FChar::IsAlnum(C) || C == TEXT('-') || C == TEXT('_'); });
}

FString MadFall::Session::MakeWorldName(const FString& DisplayName)
{
	FString Out;
	for (TCHAR C : DisplayName.TrimStartAndEnd())
	{
		if (C < 128 && (FChar::IsAlnum(C) || C == TEXT('-') || C == TEXT('_')))
		{
			Out.AppendChar(C);
		}
		else if (FChar::IsWhitespace(C) && !Out.IsEmpty() && Out[Out.Len() - 1] != TEXT('_'))
		{
			Out.AppendChar(TEXT('_'));
		}
	}
	while (Out.StartsWith(TEXT("_")))
	{
		Out.RightChopInline(1);
	}
	return Out.Left(64);
}

int64 MadFall::Session::ParseSeed(const FString& Text)
{
	const FString Trimmed = Text.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return static_cast<int64>(FMath::Rand32()) ^ (static_cast<int64>(FDateTime::UtcNow().GetTicks()) & 0x7FFFFFFF);
	}
	if (Trimmed.IsNumeric())
	{
		return FCString::Atoi64(*Trimmed);
	}
	// FNV-1a over the text: stable across runs and platforms, unlike GetTypeHash.
	uint64 Hash = 1469598103934665603ull;
	for (TCHAR C : Trimmed)
	{
		Hash = (Hash ^ static_cast<uint64>(C)) * 1099511628211ull;
	}
	return static_cast<int64>(Hash & 0x7FFFFFFF);
}

bool MadFall::Session::ReadWorldInfo(const FString& Directory, FMadWorldInfo& Out)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Directory, WorldInfoFile)))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Json;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid()
		|| Json->GetStringField(TEXT("schema")) != WorldSchema)
	{
		return false;
	}

	Out = FMadWorldInfo();
	Out.Directory = Directory;
	Out.Name = FPaths::GetCleanFilename(Directory);
	Out.DisplayName = Json->GetStringField(TEXT("name"));
	if (Out.DisplayName.IsEmpty())
	{
		Out.DisplayName = Out.Name;
	}
	// Seeds are written as strings: a JSON number is a double, which cannot hold every int64.
	FString SeedText;
	if (Json->TryGetStringField(TEXT("seed"), SeedText))
	{
		Out.Seed = FCString::Atoi64(*SeedText);
	}
	else
	{
		Out.Seed = static_cast<int64>(Json->GetNumberField(TEXT("seed")));
	}
	FDateTime::ParseIso8601(*Json->GetStringField(TEXT("created")), Out.Created);
	FDateTime::ParseIso8601(*Json->GetStringField(TEXT("last_played")), Out.LastPlayed);
	Out.Day = FMath::Max(1, static_cast<int32>(Json->GetNumberField(TEXT("day"))));
	FString Difficulty;
	if (Json->TryGetStringField(TEXT("difficulty"), Difficulty) && !Difficulty.IsEmpty())
	{
		Out.Difficulty = FName(*Difficulty.ToLower());
	}
	return true;
}

bool MadFall::Session::WriteWorldInfo(const FMadWorldInfo& Info, FString& OutError)
{
	if (Info.Directory.IsEmpty())
	{
		OutError = TEXT("world has no directory");
		return false;
	}

	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("schema"), WorldSchema);
	Json->SetStringField(TEXT("name"), Info.DisplayName.IsEmpty() ? Info.Name : Info.DisplayName);
	Json->SetStringField(TEXT("seed"), LexToString(Info.Seed));
	Json->SetStringField(TEXT("created"), Info.Created.ToIso8601());
	Json->SetStringField(TEXT("last_played"), Info.LastPlayed.ToIso8601());
	Json->SetNumberField(TEXT("day"), Info.Day);
	Json->SetStringField(TEXT("difficulty"), Info.Difficulty.ToString());

	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Json, Writer);

	IFileManager::Get().MakeDirectory(*Info.Directory, /*Tree*/ true);
	// Write-then-rename, like chunk saves: a crash mid-write must not lose the seed.
	const FString Final = FPaths::Combine(Info.Directory, WorldInfoFile);
	const FString Temp = Final + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(Text, *Temp) || !IFileManager::Get().Move(*Final, *Temp, /*Replace*/ true))
	{
		OutError = FString::Printf(TEXT("could not write %s"), *Final);
		return false;
	}
	return true;
}

bool MadFall::Session::FindWorld(const FString& Name, const FString& Root, FMadWorldInfo& Out)
{
	if (!IsValidWorldName(Name))
	{
		return false;
	}
	const FString Directory = FPaths::Combine(Root, Name);
	if (ReadWorldInfo(Directory, Out))
	{
		return true;
	}
	const FString Regions = FPaths::Combine(Directory, TEXT("regions"));
	if (!IFileManager::Get().DirectoryExists(*Regions))
	{
		return false;
	}
	Out = FMadWorldInfo();
	Out.Name = Name;
	Out.DisplayName = Name;
	Out.Directory = Directory;
	Out.LastPlayed = IFileManager::Get().GetTimeStamp(*Regions);
	Out.Created = Out.LastPlayed;
	return true;
}

void MadFall::Session::ListWorlds(TArray<FMadWorldInfo>& Out, const FString& Root)
{
	Out.Reset();
	TArray<FString> Folders;
	IFileManager::Get().FindFiles(Folders, *FPaths::Combine(Root, TEXT("*")), /*Files*/ false, /*Directories*/ true);
	for (const FString& Folder : Folders)
	{
		FMadWorldInfo Info;
		if (FindWorld(Folder, Root, Info))
		{
			Out.Add(MoveTemp(Info));
		}
	}
	Out.Sort([](const FMadWorldInfo& A, const FMadWorldInfo& B) { return A.LastPlayed > B.LastPlayed; });
}

bool MadFall::Session::DeleteWorld(const FString& Name, const FString& Root, FString& OutError)
{
	if (!IsValidWorldName(Name))
	{
		OutError = FString::Printf(TEXT("'%s' is not a world name"), *Name);
		return false;
	}
	const FString Directory = FPaths::Combine(Root, Name);
	FMadWorldInfo Info;
	if (!FindWorld(Name, Root, Info))
	{
		// Only folders the game made: never delete a directory just because its name is valid.
		OutError = FString::Printf(TEXT("%s is not a MadFall world"), *Directory);
		return false;
	}
	if (!IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists*/ true, /*Tree*/ true))
	{
		OutError = FString::Printf(TEXT("could not delete %s"), *Directory);
		return false;
	}
	return true;
}

void MadFall::Session::SelectWorld(const FString& Name, TOptional<int64> SeedIfNew)
{
	FScopeLock Lock(&GSessionLock);
	GSelectedName = Name;
	GSelectedSeed = SeedIfNew;
	GStart = EStartState::Game;
}

bool MadFall::Session::GetSelectedWorld(FString& OutName, TOptional<int64>& OutSeedIfNew)
{
	FScopeLock Lock(&GSessionLock);
	if (GSelectedName.IsEmpty())
	{
		return false;
	}
	OutName = GSelectedName;
	OutSeedIfNew = GSelectedSeed;
	return true;
}

bool MadFall::Session::IsTitleScreen()
{
	FScopeLock Lock(&GSessionLock);
	if (GStart == EStartState::Undecided)
	{
		const TCHAR* CommandLine = FCommandLine::Get();
		FString Unused;
		const bool bForceTitle = FParse::Param(CommandLine, TEXT("MadTitle"));
		const bool bStraightIn = FParse::Param(CommandLine, TEXT("unattended")) || FParse::Param(CommandLine, TEXT("MadNoTitle"))
			|| FParse::Value(CommandLine, TEXT("MadWorld="), Unused) || IsRunningCommandlet() || GIsEditor || !FApp::IsGame();
		GStart = (bForceTitle || !bStraightIn) ? EStartState::Title : EStartState::Game;
	}
	return GStart == EStartState::Title;
}

void MadFall::Session::ReturnToTitle()
{
	FScopeLock Lock(&GSessionLock);
	GStart = EStartState::Title;
}
