// Copyright MadFall. All Rights Reserved.

#include "MadFallModAPI.h"

#include "HAL/IConsoleManager.h"
#include "MadFallApiVersion.h"
#include "MadModManager.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY(LogMadFall);
DEFINE_LOG_CATEGORY(LogMadFallMods);

#define LOCTEXT_NAMESPACE "FMadFallModAPIModule"

void FMadFallModAPIModule::StartupModule()
{
	UE_LOG(LogMadFall, Log, TEXT("MadFall ModAPI %s starting up (PostConfigInit)."),
		*MadFall::ModApi::GetVersionString());

	// This is the only point in startup that runs before the asset registry
	// scans, which is why Tier-2 .pak mounting happens from this callback rather
	// than from a UEngineSubsystem - see docs/ARCHITECTURE.md.
	FMadModManager& Mods = MadFall::GetModManager();
	Mods.Discover(
		FPaths::Combine(FPaths::ProjectDir(), TEXT("Mods")),
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Config"), TEXT("MadFallMods.json")));
	Mods.MountPaks();
}

void FMadFallModAPIModule::ShutdownModule()
{
	UE_LOG(LogMadFall, Log, TEXT("MadFall ModAPI shutting down."));
}

namespace MadFall::ModApi
{
	FString GetVersionString()
	{
		return FString::Printf(TEXT("%d.%d.%d"), VersionMajor, VersionMinor, VersionPatch);
	}

	bool IsCompatible(int32 RequiredMajor, int32 RequiredMinor)
	{
		// Standard semver consumer rule: same major, running minor at least the
		// required minor. Pre-1.0 (major == 0) every minor is allowed to break,
		// so require an exact minor match until the surface is declared stable.
		if (RequiredMajor != VersionMajor)
		{
			return false;
		}

		return (VersionMajor == 0) ? (RequiredMinor == VersionMinor) : (VersionMinor >= RequiredMinor);
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMadFallModAPIModule, MadFallModAPI)

static FAutoConsoleCommand GMadModsCommand(
	TEXT("mad.mods"),
	TEXT("Lists mods in load order, with every resolution issue."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		UE_LOG(LogMadFallMods, Display, TEXT("%s"), *MadFall::GetModManager().Describe());
	}));

static FAutoConsoleCommand GMadModEnableCommand(
	TEXT("mad.mods.enable"),
	TEXT("mad.mods.enable <mod id> <0|1> - enable or disable a mod from the next launch."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMadFallMods, Error, TEXT("Usage: mad.mods.enable <mod id> <0|1>"));
			return;
		}
		const bool bEnable = Args[1] != TEXT("0");
		if (MadFall::GetModManager().SetModEnabled(FName(*Args[0]), bEnable))
		{
			UE_LOG(LogMadFallMods, Display, TEXT("%s will be %s on the next launch."), *Args[0], bEnable ? TEXT("enabled") : TEXT("disabled"));
		}
	}));
