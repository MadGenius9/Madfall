// Copyright MadFall. All Rights Reserved.

#include "MadContainerSubsystem.h"

#include "MadBlockRegistry.h"
#include "MadFallGameplay.h"
#include "MadGameplayDefinitions.h"
#include "MadGameplaySave.h"
#include "MadPickupSubsystem.h"
#include "MadPoiPlanner.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWorldGenerator.h"
#include "Math/RandomStream.h"

namespace
{
	const FName ContainerTag(TEXT("block.container"));
	const FName LootMarkerType(TEXT("loot"));
}

void UMadContainerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	VoxelWorld = Collection.InitializeDependency<UMadVoxelWorldSubsystem>();
	if (VoxelWorld != nullptr)
	{
		VoxelChangedHandle = VoxelWorld->OnVoxelChanged().AddUObject(this, &UMadContainerSubsystem::HandleVoxelChanged);
	}
}

void UMadContainerSubsystem::Deinitialize()
{
	if (VoxelWorld != nullptr)
	{
		VoxelWorld->OnVoxelChanged().Remove(VoxelChangedHandle);
	}
	VoxelWorld = nullptr;
	Containers.Reset();
	Super::Deinitialize();
}

bool UMadContainerSubsystem::IsContainer(const FIntVector& Voxel) const
{
	if (VoxelWorld == nullptr)
	{
		return false;
	}
	const FMadVoxel Value = VoxelWorld->GetVoxel(Voxel.X, Voxel.Y, Voxel.Z);
	const FMadBlockDefinitionData* Def = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(Value.BlockTypeID);
	return Def != nullptr && Value.IsSolid() && Def->Tags.Contains(ContainerTag);
}

void UMadContainerSubsystem::HandleVoxelChanged(const FIntVector& Position, const FMadVoxel& Before, const FMadVoxel& After)
{
	// A container that stops being one spills its contents where it stood.
	if (Before.BlockTypeID == After.BlockTypeID)
	{
		return;
	}

	FMadContainer Removed;
	if (!Containers.RemoveAndCopyValue(Position, Removed))
	{
		return;
	}

	if (UMadPickupSubsystem* Pickups = GetWorld() ? GetWorld()->GetSubsystem<UMadPickupSubsystem>() : nullptr)
	{
		const FVector Centre = (FVector(Position) + FVector(0.5)) * MadFall::VoxelSizeUU;
		Pickups->Drop(Centre, Removed.Contents.GetSlots());
	}
	UE_LOG(LogMadFallGameplay, Log, TEXT("Container at %s destroyed; its contents spilled."), *Position.ToString());
}

bool UMadContainerSubsystem::FindLootMarker(const FIntVector& Voxel, FName& OutTable, int32& OutTier) const
{
	const FMadWorldGenerator* Generator = VoxelWorld ? VoxelWorld->GetWorldGenerator() : nullptr;
	if (Generator == nullptr)
	{
		return false;
	}

	const FMadPoiPlanner& Planner = Generator->GetPoiPlanner();
	TArray<FMadPoiInstance> Pois;
	Planner.GetPoisOverlappingChunk(*Generator, MadFall::WorldToChunk(Voxel.X, Voxel.Y, Voxel.Z), Pois);

	TArray<FMadPoiWorldMarker> Markers;
	for (const FMadPoiInstance& Poi : Pois)
	{
		Planner.GetWorldMarkers(Poi, Markers);
		for (const FMadPoiWorldMarker& Marker : Markers)
		{
			if (Marker.Type == LootMarkerType && Marker.WorldPosition == Voxel && !Marker.LootTable.IsNone())
			{
				OutTable = Marker.LootTable;
				OutTier = Marker.Tier;
				return true;
			}
		}
	}
	return false;
}

FMadContainer* UMadContainerSubsystem::Open(const FIntVector& Voxel, int32 GameStage)
{
	if (!IsContainer(Voxel))
	{
		return nullptr;
	}

	FMadContainer& Container = Containers.FindOrAdd(Voxel);
	if (Container.bRolled)
	{
		return &Container;
	}
	Container.bRolled = true;

	FName Table;
	int32 Tier = 1;
	if (!FindLootMarker(Voxel, Table, Tier))
	{
		return &Container;
	}

	Container.LootTable = Table;
	Container.Tier = Tier;

	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const FMadLootTableDefinition* LootTable = Definitions.FindLootTable(Table);
	if (LootTable == nullptr)
	{
		UE_LOG(LogMadFallGameplay, Warning, TEXT("Container at %s names loot table '%s', which is not registered."),
			*Voxel.ToString(), *Table.ToString());
		return &Container;
	}

	// Seeded by world and position: reproducible, and independent of the order
	// in which containers happen to be opened.
	const uint32 Seed = HashCombineFast(GetTypeHash(VoxelWorld->GetSeed()), GetTypeHash(Voxel));
	FRandomStream Random(static_cast<int32>(Seed));

	TArray<FMadItemStack> Rolled;
	MadFall::Loot::Roll(*LootTable, Definitions, FMadLootContext{ Tier, GameStage }, Random, Rolled);
	for (const FMadItemStack& Stack : Rolled)
	{
		Container.Contents.Add(Stack, Definitions);
	}

	UE_LOG(LogMadFallGameplay, Log, TEXT("Rolled %s (tier %d, game stage %d) at %s: %d stack(s)."),
		*Table.ToString(), Tier, GameStage, *Voxel.ToString(), Rolled.Num());
	return &Container;
}

int32 UMadContainerSubsystem::TakeAll(const FIntVector& Voxel, FMadInventory& Inventory, int32 GameStage)
{
	FMadContainer* Container = Open(Voxel, GameStage);
	if (Container == nullptr)
	{
		return 0;
	}

	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	int32 Moved = 0;
	for (int32 Slot = 0; Slot < Container->Contents.NumSlots(); ++Slot)
	{
		const FMadItemStack Stack = Container->Contents.GetSlot(Slot);
		if (Stack.IsEmpty())
		{
			continue;
		}
		const int32 Left = Inventory.Add(Stack, Definitions);
		Moved += Stack.Count - Left;

		FMadItemStack Remaining = Stack;
		Remaining.Count = Left;
		Container->Contents.SetSlot(Slot, Left > 0 ? Remaining : FMadItemStack());
	}
	return Moved;
}

void UMadContainerSubsystem::ExportState(TArray<FMadContainerSaveData>& Out) const
{
	Out.Reset();
	for (const TPair<FIntVector, FMadContainer>& Pair : Containers)
	{
		FMadContainerSaveData& Data = Out.AddDefaulted_GetRef();
		Data.Position = Pair.Key;
		Data.LootTable = Pair.Value.LootTable;
		Data.Tier = Pair.Value.Tier;
		Data.bRolled = Pair.Value.bRolled;
		Data.Contents = Pair.Value.Contents.GetSlots();
	}

	// Stable order keeps saves diffable.
	Out.Sort([](const FMadContainerSaveData& A, const FMadContainerSaveData& B)
	{
		if (A.Position.X != B.Position.X) { return A.Position.X < B.Position.X; }
		if (A.Position.Y != B.Position.Y) { return A.Position.Y < B.Position.Y; }
		return A.Position.Z < B.Position.Z;
	});
}

void UMadContainerSubsystem::ImportState(const TArray<FMadContainerSaveData>& In)
{
	Containers.Reset();
	for (const FMadContainerSaveData& Data : In)
	{
		FMadContainer& Container = Containers.Add(Data.Position);
		Container.LootTable = Data.LootTable;
		Container.Tier = Data.Tier;
		Container.bRolled = Data.bRolled;
		for (int32 Slot = 0; Slot < FMath::Min(Data.Contents.Num(), Container.Contents.NumSlots()); ++Slot)
		{
			Container.Contents.SetSlot(Slot, Data.Contents[Slot]);
		}
	}
}
