// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "MadWorldBackup.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadWorldBackupTest,
	"MadFall.Session.Backups",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadWorldBackupTest::RunTest(const FString& Parameters)
{
	const FString World = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("BackupTestWorld")));
	IFileManager::Get().DeleteDirectory(*World, false, true);

	auto Write = [&World](const TCHAR* File, const FString& Text)
	{
		return FFileHelper::SaveStringToFile(Text, *FPaths::Combine(World, File));
	};
	auto Read = [&World](const TCHAR* File)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *FPaths::Combine(World, File));
		return Text;
	};
	// File times on some file systems are coarse; a short sleep keeps "written
	// after the backup" true however the clock is rounded.
	auto Later = []() { FPlatformProcess::Sleep(0.02f); };

	FString Created;
	FString Error;
	TArray<FMadWorldBackup> Backups;

	TestTrue(TEXT("an empty folder backs up nothing"), MadFall::WorldBackup::BackUp(World, Created, Error) && Created.IsEmpty());

	Write(TEXT("world.json"), TEXT("{\"seed\":1}"));
	Write(TEXT("gameplay.json"), TEXT("day 1"));
	Write(TEXT("regions/r.0.0.mfr"), TEXT("region zero"));
	Write(TEXT("regions/r.0.0.mfr.tmp"), TEXT("half written"));
	Later();

	TestTrue(TEXT("first backup"), MadFall::WorldBackup::BackUp(World, Created, Error) && !Created.IsEmpty());
	const FString First = Created;
	MadFall::WorldBackup::List(World, Backups);
	TestEqual(TEXT("one backup listed"), Backups.Num(), 1);
	if (Backups.Num() == 1)
	{
		TestTrue(TEXT("with its size"), Backups[0].Bytes > 0);
		TestTrue(TEXT("regions copied"), IFileManager::Get().FileExists(*FPaths::Combine(Backups[0].Directory, TEXT("regions/r.0.0.mfr"))));
		TestFalse(TEXT("temporary files are not state"), IFileManager::Get().FileExists(*FPaths::Combine(Backups[0].Directory, TEXT("regions/r.0.0.mfr.tmp"))));
	}

	TestTrue(TEXT("unchanged world: no copy"), MadFall::WorldBackup::BackUp(World, Created, Error) && Created.IsEmpty());

	// Four more sessions, each changing the save.
	for (int32 Day = 2; Day <= 5; ++Day)
	{
		Later();
		Write(TEXT("gameplay.json"), FString::Printf(TEXT("day %d"), Day));
		Later();
		TestTrue(FString::Printf(TEXT("backup after day %d"), Day), MadFall::WorldBackup::BackUp(World, Created, Error) && !Created.IsEmpty());
	}
	MadFall::WorldBackup::List(World, Backups);
	TestEqual(TEXT("pruned to the newest three"), Backups.Num(), MadFall::WorldBackup::MaxBackups);
	TestFalse(TEXT("the first is gone"), Backups.ContainsByPredicate([&First](const FMadWorldBackup& B) { return B.Name == First; }));
	TestTrue(TEXT("newest first"), Backups.Num() == 3 && Backups[0].Time > Backups[1].Time && Backups[1].Time > Backups[2].Time);

	// The oldest kept backup holds day 3 (taken after day 3 was written).
	const FString Oldest = Backups.Num() > 0 ? Backups.Last().Name : FString();
	Later();
	Write(TEXT("gameplay.json"), TEXT("day 6, and the base burned down"));
	Write(TEXT("regions/r.1.0.mfr"), TEXT("a region explored later"));
	TestTrue(TEXT("restore the oldest"), MadFall::WorldBackup::Restore(World, Oldest, Error));
	TestEqual(TEXT("the save is back"), Read(TEXT("gameplay.json")), FString(TEXT("day 3")));
	TestFalse(TEXT("files the backup did not have are gone"), IFileManager::Get().FileExists(*FPaths::Combine(World, TEXT("regions/r.1.0.mfr"))));
	TestEqual(TEXT("regions are back"), Read(TEXT("regions/r.0.0.mfr")), FString(TEXT("region zero")));

	MadFall::WorldBackup::List(World, Backups);
	TestTrue(TEXT("the restored backup is kept"), Backups.ContainsByPredicate([&Oldest](const FMadWorldBackup& B) { return B.Name == Oldest; }));
	TestTrue(TEXT("and the state before the restore was backed up"), Backups.Num() > 0
		&& FFileHelper::LoadFileToString(Error, *FPaths::Combine(Backups[0].Directory, TEXT("gameplay.json"))) && Error.Contains(TEXT("burned down")));

	TestFalse(TEXT("an unknown backup"), MadFall::WorldBackup::Restore(World, TEXT("1999-01-01_00-00-00-000"), Error));

	IFileManager::Get().DeleteDirectory(*World, false, true);
	return true;
}

#endif
