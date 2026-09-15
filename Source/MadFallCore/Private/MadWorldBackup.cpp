// Copyright MadFall. All Rights Reserved.

#include "MadWorldBackup.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	const TCHAR* BackupsFolder = TEXT("backups");
	const TCHAR* ManifestFile = TEXT("manifest.txt");

	/**
	 * Each file's path, size and time stamp as they were when backed up. "Nothing
	 * changed" compares these, not file times against the backup's own time,
	 * which would assume the file system's clock and this one agree.
	 */
	FString BuildManifest(const FString& Root, const TArray<FString>& Relative)
	{
		FString Out;
		for (const FString& File : Relative)
		{
			const FString Path = FPaths::Combine(Root, File);
			// Time stamps can be whole seconds, and a save rewritten within one keeps
			// its size (day 1 -> day 2). The JSON files are kilobytes, so they are
			// compared by content; region files are large and grow as they change.
			uint32 Crc = 0;
			if (File.EndsWith(TEXT(".json")))
			{
				TArray<uint8> Bytes;
				FFileHelper::LoadFileToArray(Bytes, *Path);
				Crc = FCrc::MemCrc32(Bytes.GetData(), Bytes.Num());
			}
			Out += FString::Printf(TEXT("%s\t%lld\t%lld\t%u\n"), *File, IFileManager::Get().FileSize(*Path), IFileManager::Get().GetTimeStamp(*Path).GetTicks(), Crc);
		}
		return Out;
	}

	/** The files a backup holds, relative to the world folder. Temporary and quarantined files are not state. */
	void GatherWorldFiles(const FString& WorldDirectory, TArray<FString>& OutRelative)
	{
		OutRelative.Reset();
		const FString Root = FPaths::ConvertRelativePathToFull(WorldDirectory);
		for (const TCHAR* File : { TEXT("world.json"), TEXT("gameplay.json") })
		{
			if (IFileManager::Get().FileExists(*FPaths::Combine(Root, File)))
			{
				OutRelative.Add(File);
			}
		}
		const FString Regions = FPaths::Combine(Root, TEXT("regions"));
		IFileManager::Get().IterateDirectoryRecursively(*Regions, [&](const TCHAR* Path, bool bIsDirectory)
		{
			FString Full(Path);
			if (!bIsDirectory && !Full.EndsWith(TEXT(".tmp")) && !Full.EndsWith(TEXT(".corrupt")) && !Full.EndsWith(TEXT(".compact")))
			{
				FPaths::MakePathRelativeTo(Full, *(Root + TEXT("/")));
				OutRelative.Add(Full);
			}
			return true;
		});
		OutRelative.Sort();
	}

	/** "2026-09-14_08-30-05-123", UTC to the millisecond, so names sort by time. */
	FString MakeName(const FDateTime& Time)
	{
		return FString::Printf(TEXT("%04d-%02d-%02d_%02d-%02d-%02d-%03d"), Time.GetYear(), Time.GetMonth(), Time.GetDay(),
			Time.GetHour(), Time.GetMinute(), Time.GetSecond(), Time.GetMillisecond());
	}

	bool ParseName(const FString& Name, FDateTime& Out)
	{
		TArray<FString> Parts;
		Name.Replace(TEXT("_"), TEXT("-")).ParseIntoArray(Parts, TEXT("-"));
		if (Parts.Num() != 7)
		{
			return false;
		}
		int32 Values[7];
		for (int32 Index = 0; Index < 7; ++Index)
		{
			if (!Parts[Index].IsNumeric())
			{
				return false;
			}
			Values[Index] = FCString::Atoi(*Parts[Index]);
		}
		if (!FDateTime::Validate(Values[0], Values[1], Values[2], Values[3], Values[4], Values[5], Values[6]))
		{
			return false;
		}
		Out = FDateTime(Values[0], Values[1], Values[2], Values[3], Values[4], Values[5], Values[6]);
		return true;
	}

	bool CopyFiles(const FString& From, const FString& To, const TArray<FString>& Relative, FString& OutError)
	{
		for (const FString& File : Relative)
		{
			const FString Destination = FPaths::Combine(To, File);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), /*Tree*/ true);
			if (IFileManager::Get().Copy(*Destination, *FPaths::Combine(From, File), /*Replace*/ true) != COPY_OK)
			{
				OutError = FString::Printf(TEXT("could not copy %s to %s"), *File, *To);
				return false;
			}
		}
		return true;
	}

	bool BackUpKeeping(const FString& WorldDirectory, const FString& Keep, FString& OutCreated, FString& OutError)
	{
		OutCreated.Reset();
		const FString Root = FPaths::ConvertRelativePathToFull(WorldDirectory);
		TArray<FString> Files;
		GatherWorldFiles(Root, Files);
		if (Files.Num() == 0 || (Files.Num() == 1 && Files[0] == TEXT("world.json")))
		{
			return true;   // a world being created (New World writes world.json first): nothing to protect yet
		}

		const FString Manifest = BuildManifest(Root, Files);
		TArray<FMadWorldBackup> Existing;
		MadFall::WorldBackup::List(Root, Existing);
		FString NewestManifest;
		if (Existing.Num() > 0 && FFileHelper::LoadFileToString(NewestManifest, *FPaths::Combine(Existing[0].Directory, ManifestFile))
			&& NewestManifest == Manifest)
		{
			return true;
		}

		FString Name = MakeName(FDateTime::UtcNow());
		FString Target = FPaths::Combine(Root, BackupsFolder, Name);
		for (int32 Suffix = 1; IFileManager::Get().DirectoryExists(*Target); ++Suffix)
		{
			// Two backups in one millisecond (tests): bump the millisecond field.
			FDateTime Time;
			ParseName(Name, Time);
			Name = MakeName(Time + FTimespan::FromMilliseconds(Suffix));
			Target = FPaths::Combine(Root, BackupsFolder, Name);
		}
		// The manifest is written last: a backup without one was interrupted, and
		// never matches, so the next open simply makes a complete one.
		if (!CopyFiles(Root, Target, Files, OutError) || !FFileHelper::SaveStringToFile(Manifest, *FPaths::Combine(Target, ManifestFile)))
		{
			IFileManager::Get().DeleteDirectory(*Target, false, true);
			if (OutError.IsEmpty())
			{
				OutError = FString::Printf(TEXT("could not write %s"), ManifestFile);
			}
			return false;
		}
		OutCreated = Name;

		MadFall::WorldBackup::List(Root, Existing);
		int32 Kept = 0;
		for (const FMadWorldBackup& Backup : Existing)
		{
			if (Backup.Name == Keep || ++Kept <= MadFall::WorldBackup::MaxBackups)
			{
				continue;
			}
			IFileManager::Get().DeleteDirectory(*Backup.Directory, false, true);
		}
		return true;
	}
}

void MadFall::WorldBackup::List(const FString& WorldDirectory, TArray<FMadWorldBackup>& Out)
{
	Out.Reset();
	const FString Folder = FPaths::Combine(FPaths::ConvertRelativePathToFull(WorldDirectory), BackupsFolder);
	TArray<FString> Names;
	IFileManager::Get().FindFiles(Names, *FPaths::Combine(Folder, TEXT("*")), /*Files*/ false, /*Directories*/ true);
	for (const FString& Name : Names)
	{
		FMadWorldBackup& Backup = Out.AddDefaulted_GetRef();
		if (!ParseName(Name, Backup.Time))
		{
			Out.Pop();   // not ours
			continue;
		}
		Backup.Name = Name;
		Backup.Directory = FPaths::Combine(Folder, Name);
		IFileManager::Get().IterateDirectoryStatRecursively(*Backup.Directory, [&Backup](const TCHAR*, const FFileStatData& Stat)
		{
			Backup.Bytes += Stat.bIsDirectory ? 0 : Stat.FileSize;
			return true;
		});
	}
	Out.Sort([](const FMadWorldBackup& A, const FMadWorldBackup& B) { return A.Time > B.Time; });
}

bool MadFall::WorldBackup::BackUp(const FString& WorldDirectory, FString& OutCreated, FString& OutError)
{
	return BackUpKeeping(WorldDirectory, FString(), OutCreated, OutError);
}

bool MadFall::WorldBackup::Restore(const FString& WorldDirectory, const FString& BackupName, FString& OutError)
{
	const FString Root = FPaths::ConvertRelativePathToFull(WorldDirectory);
	TArray<FMadWorldBackup> Backups;
	List(Root, Backups);
	const FMadWorldBackup* Backup = Backups.FindByPredicate([&BackupName](const FMadWorldBackup& B) { return B.Name == BackupName; });
	if (Backup == nullptr)
	{
		OutError = FString::Printf(TEXT("no backup named %s"), *BackupName);
		return false;
	}
	const FString Source = Backup->Directory;
	TArray<FString> Files;
	GatherWorldFiles(Source, Files);
	if (Files.Num() == 0)
	{
		OutError = FString::Printf(TEXT("backup %s is empty"), *BackupName);
		return false;
	}

	// First, so the restore itself can be undone. The backup being restored is
	// kept even if it is now the oldest.
	FString Created;
	if (!BackUpKeeping(Root, BackupName, Created, OutError))
	{
		return false;
	}

	IFileManager::Get().DeleteDirectory(*FPaths::Combine(Root, TEXT("regions")), false, true);
	for (const TCHAR* File : { TEXT("world.json"), TEXT("gameplay.json"), TEXT("gameplay.json.bak") })
	{
		IFileManager::Get().Delete(*FPaths::Combine(Root, File), false, true, true);
	}
	return CopyFiles(Source, Root, Files, OutError);
}
