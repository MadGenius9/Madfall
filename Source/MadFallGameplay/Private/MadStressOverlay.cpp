// Copyright MadFall. All Rights Reserved.

#include "MadStressOverlay.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "MadFallCoordinates.h"
#include "MadFrameBudget.h"
#include "MadGameplayDefinitions.h"
#include "MadPlayerCharacter.h"
#include "MadStructuralSubsystem.h"
#include "MadSurvivorComponents.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"

namespace
{
	TAutoConsoleVariable<bool> CVarStressOverlay(
		TEXT("mad.si.Overlay"), true,
		TEXT("Shade structural stress around the aimed block while holding a block to place."));

	const TCHAR* CubeMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");
	const TCHAR* OverlayMaterialPath = TEXT("/Game/Materials/M_MadStressOverlay.M_MadStressOverlay");
}

FLinearColor MadFall::StressOverlay::RampColour(float Stress)
{
	// Kept in step with the ramp in Scripts/make_stress_material.py.
	const FLinearColor Blue(0.05f, 0.25f, 1.0f);
	const FLinearColor Green(0.1f, 0.9f, 0.15f);
	const FLinearColor Yellow(1.0f, 0.85f, 0.05f);
	const FLinearColor Red(1.0f, 0.05f, 0.02f);
	const float S = FMath::Max(0.0f, Stress);
	if (S < 0.5f) { return FMath::Lerp(Blue, Green, S / 0.5f); }
	if (S < 0.8f) { return FMath::Lerp(Green, Yellow, (S - 0.5f) / 0.3f); }
	return FMath::Lerp(Yellow, Red, FMath::Min(1.0f, (S - 0.8f) / 0.2f));
}

bool UMadStressOverlaySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && FApp::CanEverRender() && Super::ShouldCreateSubsystem(Outer);
}

TStatId UMadStressOverlaySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadStressOverlaySubsystem, STATGROUP_Tickables);
}

int32 UMadStressOverlaySubsystem::NumShaded() const
{
	return Boxes != nullptr && bVisible ? Boxes->GetInstanceCount() : 0;
}

void UMadStressOverlaySubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	MAD_FRAME_SCOPE(Structural);

	const UMadStructuralSubsystem* Structural = GetWorld()->GetSubsystem<UMadStructuralSubsystem>();
	const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	const AMadPlayerCharacter* Player = Controller ? Cast<AMadPlayerCharacter>(Controller->GetPawn()) : nullptr;

	int32 Version = 0;
	double LastQuery = 0.0;
	const TArray<FMadStressSample>* Samples = Structural ? &Structural->GetStressField(Version, LastQuery) : nullptr;

	// Shown while building: a block item in hand, and the HUD still asking about
	// a structural block (it stops the moment the aim leaves one).
	bool bWant = CVarStressOverlay.GetValueOnGameThread() && Samples != nullptr && Player != nullptr && !Player->IsInventoryOpen()
		&& FPlatformTime::Seconds() - LastQuery < 0.25 && Samples->Num() > 0;
	if (bWant)
	{
		const FMadItemStack* Held = Player->GetInventory() ? Player->GetInventory()->GetSelectedStack() : nullptr;
		const FMadItemDefinition* Item = Held && !Held->IsEmpty() ? MadFall::GetGameplayDefinitions().FindItem(Held->Item) : nullptr;
		bWant = Item != nullptr && !Item->PlacesBlock.IsNone() && Item->Kind == EMadItemKind::Block;
	}

	if (!bWant)
	{
		if (bVisible && Boxes != nullptr)
		{
			Boxes->SetVisibility(false);
		}
		bVisible = false;
		return;
	}

	UInstancedStaticMeshComponent* Component = GetOrCreateComponent();
	if (Component == nullptr)
	{
		return;
	}
	if (Version != AppliedVersion)
	{
		AppliedVersion = Version;
		Component->ClearInstances();
		TArray<FTransform> Transforms;
		Transforms.Reserve(Samples->Num());
		for (const FMadStressSample& Sample : *Samples)
		{
			// A shade larger than the block, so its faces sit just in front of the block's.
			Transforms.Emplace(FQuat::Identity, (FVector(Sample.Position) + FVector(0.5)) * MadFall::VoxelSizeUU, FVector(1.02));
		}
		Component->AddInstances(Transforms, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ true);
		for (int32 Index = 0; Index < Samples->Num(); ++Index)
		{
			const FMadStressSample& Sample = (*Samples)[Index];
			Component->SetCustomDataValue(Index, 0, Sample.Stress, /*bMarkRenderStateDirty*/ false);
			Component->SetCustomDataValue(Index, 1, Sample.bFailing ? 1.0f : 0.0f, /*bMarkRenderStateDirty*/ Index + 1 == Samples->Num());
		}
	}
	if (!bVisible)
	{
		Component->SetVisibility(true);
		bVisible = true;
	}
}

UInstancedStaticMeshComponent* UMadStressOverlaySubsystem::GetOrCreateComponent()
{
	if (Boxes != nullptr)
	{
		return Boxes;
	}
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubeMeshPath);
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, OverlayMaterialPath);
	if (Cube == nullptr || Material == nullptr)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.Name = MakeUniqueObjectName(GetWorld()->PersistentLevel, AActor::StaticClass(), TEXT("MadFallStressOverlay"));
	Params.ObjectFlags |= RF_Transient;
	OverlayActor = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	if (OverlayActor == nullptr)
	{
		return nullptr;
	}
	OverlayActor->SetActorEnableCollision(false);
	USceneComponent* Root = NewObject<USceneComponent>(OverlayActor, TEXT("Root"));
	Root->RegisterComponent();
	OverlayActor->SetRootComponent(Root);

	Boxes = NewObject<UInstancedStaticMeshComponent>(OverlayActor, TEXT("StressBoxes"), RF_Transient);
	Boxes->SetStaticMesh(Cube);
	Boxes->SetMaterial(0, Material);
	Boxes->NumCustomDataFloats = 2;
	Boxes->SetMobility(EComponentMobility::Movable);
	Boxes->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Boxes->SetCanEverAffectNavigation(false);
	Boxes->SetCastShadow(false);
	Boxes->bAffectDistanceFieldLighting = false;
	Boxes->SetupAttachment(Root);
	Boxes->RegisterComponent();
	return Boxes;
}
