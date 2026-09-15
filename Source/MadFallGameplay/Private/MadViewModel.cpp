// Copyright MadFall. All Rights Reserved.

#include "MadViewModel.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "MadBlockRegistry.h"
#include "MadGameplayDefinitions.h"
#include "MadSurfaceRegistry.h"
#include "MadVoxelWorldSubsystem.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
	const TCHAR* CubeMesh = TEXT("/Engine/BasicShapes/Cube.Cube");
	const TCHAR* CylinderMesh = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
	const TCHAR* TintMaterial = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
	const TCHAR* HeldBlockMaterial = TEXT("/Game/Materials/M_MadVoxelHeld.M_MadVoxelHeld");

	constexpr float SwingSeconds = 0.28f;
	constexpr float UseSeconds = 0.22f;

	/** Where the hand rests, in camera space: low and to the right. */
	const FVector RestLocation(46.0, 24.0, -27.0);
	const FRotator RestRotation(8.0, -18.0, -6.0);

	const FLinearColor Wood(0.09f, 0.05f, 0.025f);
	const FLinearColor Iron(0.12f, 0.12f, 0.13f);
	const FLinearColor Stone(0.18f, 0.18f, 0.17f);
	const FLinearColor Skin(0.45f, 0.28f, 0.2f);

	FLinearColor ColourFromId(FName Id)
	{
		const uint32 Hash = GetTypeHash(Id);
		// Muted: a saturated hashed colour read as a toy, not a material.
		const float Base = 0.08f + (Hash & 0xFF) / 1500.0f;
		return FLinearColor(Base + ((Hash >> 8) & 0x3F) / 2000.0f, Base + ((Hash >> 14) & 0x3F) / 2000.0f, Base + ((Hash >> 20) & 0x3F) / 2000.0f);
	}
}

EMadHeldShape MadFall::ViewModel::ChooseShape(const FMadItemDefinition* Item)
{
	if (Item == nullptr)
	{
		return EMadHeldShape::Empty;
	}
	if (!Item->PlacesBlock.IsNone())
	{
		return EMadHeldShape::Block;
	}
	if (Item->bHasTool && Item->Tool.IsRanged())    { return EMadHeldShape::Bow; }
	if (Item->HasTag(FName(TEXT("tool.pickaxe")))) { return EMadHeldShape::Pickaxe; }
	if (Item->HasTag(FName(TEXT("tool.axe"))))     { return EMadHeldShape::Axe; }
	if (Item->HasTag(FName(TEXT("tool.shovel"))))  { return EMadHeldShape::Shovel; }
	if (Item->HasTag(FName(TEXT("tool.hoe"))))     { return EMadHeldShape::Hoe; }
	if (Item->Kind == EMadItemKind::Tool || Item->Kind == EMadItemKind::Weapon) { return EMadHeldShape::Club; }
	if (Item->HasTag(FName(TEXT("item.drink"))))   { return EMadHeldShape::Drink; }
	if (Item->Kind == EMadItemKind::Consumable)     { return EMadHeldShape::Food; }
	return EMadHeldShape::Resource;
}

FTransform MadFall::ViewModel::ComputeOffset(float Swing, float Use, float BobPhase, float Moving)
{
	Swing = FMath::Clamp(Swing, 0.0f, 1.0f);
	Use = FMath::Clamp(Use, 0.0f, 1.0f);
	Moving = FMath::Clamp(Moving, 0.0f, 1.0f);

	// A swing chops forward and down, fast out and a little slower back.
	const float SwingCurve = Swing < 1.0f ? FMath::Sin(FMath::Pow(Swing, 0.7f) * UE_PI) : 0.0f;
	const float UseCurve = Use < 1.0f ? FMath::Sin(Use * UE_PI) : 0.0f;

	const FVector Location(
		8.0 * SwingCurve + 10.0 * UseCurve,
		-4.0 * SwingCurve + 1.2 * FMath::Sin(BobPhase) * Moving,
		-6.0 * SwingCurve - 4.0 * UseCurve + 1.0 * FMath::Sin(BobPhase * 2.0f) * Moving);
	const FRotator Rotation(-70.0f * SwingCurve - 10.0f * UseCurve, 0.0f, -12.0f * SwingCurve);
	return FTransform(Rotation, Location);
}

UMadViewModelComponent::UMadViewModelComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UMadViewModelComponent::SetHeldItem(FName ItemId)
{
	if (bBuiltOnce && ItemId == HeldItem)
	{
		return;
	}
	HeldItem = ItemId;
	Rebuild();
}

void UMadViewModelComponent::PlaySwing()
{
	SwingProgress = 0.0f;
}

void UMadViewModelComponent::PlayUse()
{
	UseProgress = 0.0f;
}

UStaticMeshComponent* UMadViewModelComponent::AddPart(const TCHAR* MeshPath, const FVector& Location, const FVector& SizeCm,
	const FLinearColor& Colour, const FRotator& Rotation)
{
	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(GetOwner(), NAME_None, RF_Transient);
	Part->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, MeshPath));
	UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(LoadObject<UMaterialInterface>(nullptr, TintMaterial), this);
	Material->SetVectorParameterValue(TEXT("Color"), Colour);
	Materials.Add(Material);
	Part->SetMaterial(0, Material);
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// A shadow from a tool floating in front of the camera lands in odd places.
	Part->SetCastShadow(false);
	Part->SetupAttachment(Hand);
	Part->SetRelativeLocationAndRotation(Location, Rotation);
	Part->SetRelativeScale3D(SizeCm / 100.0f);
	Part->RegisterComponent();
	Parts.Add(Part);
	return Part;
}

void UMadViewModelComponent::Rebuild()
{
	if (GetOwner() == nullptr || GetWorld() == nullptr || !GetWorld()->IsGameWorld())
	{
		return;
	}
	bBuiltOnce = true;

	for (UStaticMeshComponent* Part : Parts)
	{
		if (Part != nullptr)
		{
			Part->DestroyComponent();
		}
	}
	Parts.Reset();
	Materials.Reset();

	if (Hand == nullptr)
	{
		Hand = NewObject<USceneComponent>(GetOwner(), TEXT("ViewModelHand"), RF_Transient);
		Hand->SetupAttachment(this);
		Hand->RegisterComponent();
	}

	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const FMadItemDefinition* Item = HeldItem.IsNone() ? nullptr : Definitions.FindItem(HeldItem);
	Shape = MadFall::ViewModel::ChooseShape(Item);

	// Held blocks and materials wear the world's surface patterns. Without the
	// asset (a stripped build) the tinted part still reads as what it is.
	UMaterialInterface* const PatternMaterial = LoadObject<UMaterialInterface>(nullptr, HeldBlockMaterial);
	auto ApplyPattern = [this, PatternMaterial](UStaticMeshComponent* Part, const FLinearColor& Colour, int32 Pattern)
	{
		if (PatternMaterial != nullptr && Pattern > 0)
		{
			UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(PatternMaterial, this);
			Material->SetVectorParameterValue(TEXT("Color"), Colour);
			Material->SetScalarParameterValue(TEXT("PatternAlpha"), MadFall::Surfaces::PatternToAlpha(Pattern) / 255.0f);
			// In the hand: no rain on it, no snow on it.
			Material->SetScalarParameterValue(TEXT("Weather"), 0.0f);
			Materials.Add(Material);
			Part->SetMaterial(0, Material);
		}
	};

	// Stone tools have stone heads; anything else metal.
	const bool bStoneTool = HeldItem.ToString().Contains(TEXT("stone"));
	const FLinearColor Head = bStoneTool ? Stone : Iron;

	switch (Shape)
	{
	case EMadHeldShape::Block:
	{
		FLinearColor Colour = ColourFromId(Item->PlacesBlock);
		int32 Pattern = 0;
		const FMadBlockDefView View = UMadVoxelWorldSubsystem::GetBlockRegistry().GetBlockViewById(Item->PlacesBlock);
		if (const FMadSurfaceDefinition* Surface = MadFall::GetSurfaces().Find(View.MaterialClass))
		{
			Colour = Surface->Color;
			Pattern = Surface->Pattern;
		}
		// The block in hand wears the same pattern as the placed block.
		ApplyPattern(AddPart(CubeMesh, FVector(0.0, 0.0, 22.0), FVector(15.0), Colour, FRotator(0.0, 20.0, 0.0)), Colour, Pattern);
		break;
	}
	case EMadHeldShape::Pickaxe:
		AddPart(CylinderMesh, FVector(0.0, 0.0, 16.0), FVector(3.5, 3.5, 46.0), Wood);
		AddPart(CubeMesh, FVector(3.0, 0.0, 38.0), FVector(30.0, 4.0, 4.5), Head, FRotator(-8.0, 0.0, 0.0));
		break;
	case EMadHeldShape::Axe:
		AddPart(CylinderMesh, FVector(0.0, 0.0, 16.0), FVector(3.5, 3.5, 46.0), Wood);
		AddPart(CubeMesh, FVector(7.0, 0.0, 34.0), FVector(13.0, 3.0, 15.0), Head);
		break;
	case EMadHeldShape::Shovel:
		AddPart(CylinderMesh, FVector(0.0, 0.0, 18.0), FVector(3.0, 3.0, 52.0), Wood);
		AddPart(CubeMesh, FVector(0.0, 0.0, 49.0), FVector(2.0, 14.0, 17.0), Head);
		break;
	case EMadHeldShape::Hoe:
		AddPart(CylinderMesh, FVector(0.0, 0.0, 18.0), FVector(3.0, 3.0, 52.0), Wood);
		AddPart(CubeMesh, FVector(6.0, 0.0, 43.0), FVector(14.0, 10.0, 2.5), Head, FRotator(-15.0, 0.0, 0.0));
		break;
	case EMadHeldShape::Bow:
		// Two limbs angled back from the grip, and the string between their tips.
		AddPart(CubeMesh, FVector(0.0, 0.0, 20.0), FVector(3.0, 3.0, 12.0), Wood * 1.4f);
		AddPart(CubeMesh, FVector(-3.0, 0.0, 34.0), FVector(2.5, 2.5, 20.0), Wood * 1.4f, FRotator(-15.0, 0.0, 0.0));
		AddPart(CubeMesh, FVector(-3.0, 0.0, 6.0), FVector(2.5, 2.5, 20.0), Wood * 1.4f, FRotator(15.0, 0.0, 0.0));
		AddPart(CylinderMesh, FVector(-7.0, 0.0, 20.0), FVector(0.5, 0.5, 46.0), FLinearColor(0.7f, 0.68f, 0.6f));
		break;
	case EMadHeldShape::Club:
		AddPart(CylinderMesh, FVector(0.0, 0.0, 14.0), FVector(4.5, 4.5, 40.0), Wood);
		AddPart(CylinderMesh, FVector(0.0, 0.0, 34.0), FVector(8.0, 8.0, 16.0), Wood * 1.3f);
		break;
	case EMadHeldShape::Food:
		AddPart(CylinderMesh, FVector(0.0, 0.0, 20.0), FVector(7.5, 7.5, 10.0), FLinearColor(0.35f, 0.04f, 0.03f));
		break;
	case EMadHeldShape::Drink:
		AddPart(CylinderMesh, FVector(0.0, 0.0, 22.0), FVector(6.0, 6.0, 17.0), FLinearColor(0.05f, 0.18f, 0.35f));
		AddPart(CylinderMesh, FVector(0.0, 0.0, 32.0), FVector(2.5, 2.5, 4.0), FLinearColor(0.6f, 0.6f, 0.6f));
		break;
	case EMadHeldShape::Resource:
	{
		// What it is made of, from its tags; a hashed tint only when the tags say nothing.
		FLinearColor Colour = ColourFromId(HeldItem);
		const TCHAR* PatternName = TEXT("plain");
		if (Item->HasTag(FName(TEXT("item.wood"))))       { Colour = Wood * 2.5f; PatternName = TEXT("planks"); }
		else if (Item->HasTag(FName(TEXT("item.stone")))) { Colour = Stone * 1.6f; PatternName = TEXT("stone"); }
		else if (Item->HasTag(FName(TEXT("item.metal")))) { Colour = Iron * 1.5f; PatternName = TEXT("metal"); }
		ApplyPattern(AddPart(CubeMesh, FVector(0.0, 0.0, 20.0), FVector(9.0), Colour, FRotator(15.0, 30.0, 0.0)), Colour,
			FMath::Max(0, MadFall::Surfaces::FindPattern(PatternName)));
		break;
	}
	case EMadHeldShape::Empty:
	default:
		AddPart(CubeMesh, FVector(0.0, 0.0, 16.0), FVector(9.0, 8.0, 9.0), Skin);
		break;
	}
}

void UMadViewModelComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (Hand == nullptr)
	{
		return;
	}

	SwingProgress = FMath::Min(1.0f, SwingProgress + DeltaTime / SwingSeconds);
	UseProgress = FMath::Min(1.0f, UseProgress + DeltaTime / UseSeconds);

	const AActor* Owner = GetOwner();
	const float Speed = Owner ? static_cast<float>(Owner->GetVelocity().Size2D()) : 0.0f;
	BobPhase = FMath::Fmod(BobPhase + Speed * DeltaTime / 165.0f * UE_PI, UE_TWO_PI);

	const FTransform Offset = MadFall::ViewModel::ComputeOffset(SwingProgress, UseProgress, BobPhase, Speed / 450.0f);
	// Parts are authored life-size; drawn at 60% they read as held rather than
	// filling half the screen at a 90-degree field of view.
	const FTransform Rest(RestRotation, RestLocation, FVector(0.6));
	Hand->SetRelativeTransform(Offset * Rest);
}
