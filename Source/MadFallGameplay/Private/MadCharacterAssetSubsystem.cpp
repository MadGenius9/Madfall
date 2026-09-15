// Copyright MadFall. All Rights Reserved.

#include "MadCharacterAssetSubsystem.h"

#include "Engine/AssetManager.h"
#include "Engine/World.h"
#include "MadCharacterAnimInstance.h"
#include "MadFallGameplay.h"
#include "MadGameplayDefinitions.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"

bool UMadCharacterAssetSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && FApp::CanEverRender() && Super::ShouldCreateSubsystem(Outer);
}

void UMadCharacterAssetSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	TArray<FSoftObjectPath> Paths;
	UMadCharacterAnimInstance::GetMannequinPaths(Paths);
	for (const FMadAnimalDefinition& Animal : MadFall::GetGameplayDefinitions().GetAnimals())
	{
		const FMadAnimalModel& Model = Animal.Model;
		for (const FSoftObjectPath* Path : { &Model.Mesh, &Model.Idle, &Model.Walk, &Model.Run, &Model.Attack, &Model.Hit, &Model.Death, &Model.Graze })
		{
			Paths.Add(*Path);
		}
	}
	Paths.RemoveAll([](const FSoftObjectPath& Path)
	{
		return Path.IsNull() || !FPackageName::DoesPackageExist(Path.GetLongPackageName());
	});

	Requested = Paths.Num();
	if (Paths.Num() == 0)
	{
		bSettled = true;
		return;
	}
	TWeakObjectPtr<UMadCharacterAssetSubsystem> WeakThis(this);
	Handle = UAssetManager::GetStreamableManager().RequestAsyncLoad(Paths, [WeakThis]()
	{
		if (UMadCharacterAssetSubsystem* This = WeakThis.Get())
		{
			This->bSettled = true;
			UE_LOG(LogMadFallGameplay, Log, TEXT("Loaded %d character asset(s)."), This->Requested);
		}
	});
	if (Handle.IsValid() && Handle->HasLoadCompleted())
	{
		bSettled = true;
	}
}

void UMadCharacterAssetSubsystem::Deinitialize()
{
	// Releasing the handle lets the assets go once no rig uses them; an unfinished
	// load is cancelled rather than finished for a world that is gone.
	if (Handle.IsValid() && !Handle->HasLoadCompleted())
	{
		Handle->CancelHandle();
	}
	Handle.Reset();
	Super::Deinitialize();
}
