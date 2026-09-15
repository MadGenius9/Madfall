// Copyright MadFall. All Rights Reserved.

#include "MadGameplaySave.h"

#include "MadFallCoordinates.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	using FJsonArray = TArray<TSharedPtr<FJsonValue>>;

	TSharedPtr<FJsonValue> VectorToJson(const FVector& V)
	{
		return MakeShared<FJsonValueArray>(FJsonArray{
			MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) });
	}

	TSharedPtr<FJsonValue> IntVectorToJson(const FIntVector& V)
	{
		return MakeShared<FJsonValueArray>(FJsonArray{
			MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) });
	}

	bool JsonToDoubles(const TSharedPtr<FJsonValue>& Value, int32 Count, double* Out)
	{
		if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != Count)
		{
			return false;
		}
		const FJsonArray& Items = Value->AsArray();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (!Items[Index].IsValid() || Items[Index]->Type != EJson::Number)
			{
				return false;
			}
			Out[Index] = Items[Index]->AsNumber();
		}
		return true;
	}

	TSharedPtr<FJsonValue> StackToJson(const FMadItemStack& Stack)
	{
		if (Stack.IsEmpty())
		{
			return MakeShared<FJsonValueNull>();
		}
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("item"), Stack.Item.ToString());
		Object->SetNumberField(TEXT("count"), Stack.Count);
		if (Stack.Durability >= 0)
		{
			Object->SetNumberField(TEXT("durability"), Stack.Durability);
		}
		if (Stack.Mods.Num() > 0)
		{
			FJsonArray Mods;
			for (const FName& Mod : Stack.Mods)
			{
				Mods.Add(MakeShared<FJsonValueString>(Mod.ToString()));
			}
			Object->SetArrayField(TEXT("mods"), Mods);
		}
		return MakeShared<FJsonValueObject>(Object);
	}

	/** Empty slots are null so slot positions survive the round trip. */
	FMadItemStack JsonToStack(const TSharedPtr<FJsonValue>& Value, const FString& Where, TArray<FString>& Warnings)
	{
		FMadItemStack Stack;
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return Stack;
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Value->TryGetObject(Object) || Object == nullptr)
		{
			Warnings.Add(FString::Printf(TEXT("%s is not an item stack; slot emptied"), *Where));
			return Stack;
		}

		FString Item;
		double Count = 0.0;
		if (!(*Object)->TryGetStringField(TEXT("item"), Item) || !(*Object)->TryGetNumberField(TEXT("count"), Count) || Count < 1.0)
		{
			Warnings.Add(FString::Printf(TEXT("%s is missing item or count; slot emptied"), *Where));
			return Stack;
		}

		Stack.Item = FName(*Item);
		Stack.Count = static_cast<int32>(Count);
		double Durability = -1.0;
		if ((*Object)->TryGetNumberField(TEXT("durability"), Durability))
		{
			Stack.Durability = static_cast<int32>(Durability);
		}
		const FJsonArray* Mods = nullptr;
		if ((*Object)->TryGetArrayField(TEXT("mods"), Mods))
		{
			for (const TSharedPtr<FJsonValue>& Mod : *Mods)
			{
				if (Mod.IsValid() && Mod->Type == EJson::String)
				{
					Stack.Mods.Add(FName(*Mod->AsString()));
				}
			}
		}
		return Stack;
	}

	FJsonArray StacksToJson(const TArray<FMadItemStack>& Stacks)
	{
		FJsonArray Out;
		for (const FMadItemStack& Stack : Stacks)
		{
			Out.Add(StackToJson(Stack));
		}
		return Out;
	}

	TArray<FMadItemStack> JsonToStacks(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Where, TArray<FString>& Warnings)
	{
		TArray<FMadItemStack> Out;
		const FJsonArray* Items = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Items))
		{
			for (int32 Index = 0; Index < Items->Num(); ++Index)
			{
				Out.Add(JsonToStack((*Items)[Index], FString::Printf(TEXT("%s[%d]"), *Where, Index), Warnings));
			}
		}
		return Out;
	}
}

namespace MadFall::GameplaySave
{
	bool IsRestorableLocation(const FVector& Location)
	{
		if (Location.ContainsNaN())
		{
			return false;
		}
		const double VoxelZ = FMath::FloorToDouble(Location.Z / MadFall::VoxelSizeUU);
		// Headroom above the ceiling: a survivor can stand, or be mid-jump, on the top block.
		return VoxelZ >= MadFall::WorldMinZ && VoxelZ <= MadFall::WorldMaxZ + 4;
	}

	FString ToJson(const FMadGameplaySave& Save)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), GameplaySaveSchemaV1);
		// A string: JSON numbers are doubles and would round a 64-bit seed.
		Root->SetStringField(TEXT("world_seed"), LexToString(Save.WorldSeed));

		TSharedRef<FJsonObject> Clock = MakeShared<FJsonObject>();
		Clock->SetNumberField(TEXT("day"), Save.Day);
		Clock->SetNumberField(TEXT("time_of_day"), Save.TimeOfDay);
		Root->SetObjectField(TEXT("clock"), Clock);

		if (Save.bHasPlayer)
		{
			const FMadPlayerSaveData& P = Save.Player;
			TSharedRef<FJsonObject> Player = MakeShared<FJsonObject>();
			Player->SetField(TEXT("location"), VectorToJson(P.Location));
			Player->SetField(TEXT("view"), VectorToJson(FVector(P.ViewRotation.Pitch, P.ViewRotation.Yaw, P.ViewRotation.Roll)));
			Player->SetArrayField(TEXT("spawn_column"), FJsonArray{ MakeShared<FJsonValueNumber>(P.SpawnColumn.X), MakeShared<FJsonValueNumber>(P.SpawnColumn.Y) });
			if (P.bHasBed)
			{
				Player->SetArrayField(TEXT("bed"), FJsonArray{ MakeShared<FJsonValueNumber>(P.BedVoxel.X),
					MakeShared<FJsonValueNumber>(P.BedVoxel.Y), MakeShared<FJsonValueNumber>(P.BedVoxel.Z) });
			}
			Player->SetNumberField(TEXT("level"), P.Level);
			Player->SetNumberField(TEXT("experience"), P.Experience);

			TSharedRef<FJsonObject> Perks = MakeShared<FJsonObject>();
			TArray<FName> PerkIds;
			P.PerkRanks.GenerateKeyArray(PerkIds);
			PerkIds.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
			for (const FName& Id : PerkIds)
			{
				Perks->SetNumberField(Id.ToString(), P.PerkRanks[Id]);
			}
			Player->SetObjectField(TEXT("perks"), Perks);

			TSharedRef<FJsonObject> Vitals = MakeShared<FJsonObject>();
			Vitals->SetNumberField(TEXT("health"), P.Stats.Health);
			Vitals->SetNumberField(TEXT("max_health"), P.Stats.MaxHealth);
			Vitals->SetNumberField(TEXT("stamina"), P.Stats.Stamina);
			Vitals->SetNumberField(TEXT("max_stamina"), P.Stats.MaxStamina);
			Vitals->SetNumberField(TEXT("food"), P.Stats.Food);
			Vitals->SetNumberField(TEXT("water"), P.Stats.Water);
			Vitals->SetNumberField(TEXT("core_temperature"), P.Stats.CoreTemperature);
			Vitals->SetNumberField(TEXT("infection"), P.Stats.Infection);
			Player->SetObjectField(TEXT("vitals"), Vitals);

			Player->SetArrayField(TEXT("inventory"), StacksToJson(P.Inventory));
			Player->SetArrayField(TEXT("worn"), StacksToJson(P.Worn));

			TSharedRef<FJsonObject> Quests = MakeShared<FJsonObject>();
			FJsonArray Completed;
			for (const FName& Id : P.CompletedQuests)
			{
				Completed.Add(MakeShared<FJsonValueString>(Id.ToString()));
			}
			Quests->SetArrayField(TEXT("completed"), Completed);
			FJsonArray ActiveQuests;
			for (const FMadQuestProgress& Progress : P.ActiveQuests)
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("id"), Progress.Quest.ToString());
				FJsonArray Counts;
				for (int32 Count : Progress.Counts)
				{
					Counts.Add(MakeShared<FJsonValueNumber>(Count));
				}
				Entry->SetArrayField(TEXT("counts"), Counts);
				ActiveQuests.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Quests->SetArrayField(TEXT("active"), ActiveQuests);
			Player->SetObjectField(TEXT("quests"), Quests);
			Player->SetNumberField(TEXT("selected_slot"), P.SelectedSlot);
			Root->SetObjectField(TEXT("player"), Player);
		}

		FJsonArray Containers;
		for (const FMadContainerSaveData& C : Save.Containers)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetField(TEXT("position"), IntVectorToJson(C.Position));
			if (!C.LootTable.IsNone())
			{
				Object->SetStringField(TEXT("loot_table"), C.LootTable.ToString());
			}
			Object->SetNumberField(TEXT("tier"), C.Tier);
			Object->SetBoolField(TEXT("rolled"), C.bRolled);
			Object->SetArrayField(TEXT("contents"), StacksToJson(C.Contents));
			Containers.Add(MakeShared<FJsonValueObject>(Object));
		}
		Root->SetArrayField(TEXT("containers"), Containers);

		FJsonArray TraderArray;
		for (const FMadTraderSaveData& T : Save.Traders)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetField(TEXT("position"), IntVectorToJson(T.Position));
			Object->SetStringField(TEXT("trader"), T.Trader.ToString());
			Object->SetNumberField(TEXT("restock_day"), T.RestockDay);
			Object->SetArrayField(TEXT("stock"), StacksToJson(T.Stock));
			TraderArray.Add(MakeShared<FJsonValueObject>(Object));
		}
		Root->SetArrayField(TEXT("traders"), TraderArray);

		// Sorted so two saves of the same state are byte-identical and diff cleanly.
		TArray<TPair<FIntVector, int32>> Sleepers;
		for (const TPair<FIntVector, int32>& Pair : Save.SleeperDays)
		{
			Sleepers.Add(Pair);
		}
		Sleepers.Sort([](const TPair<FIntVector, int32>& A, const TPair<FIntVector, int32>& B)
		{
			if (A.Key.X != B.Key.X) { return A.Key.X < B.Key.X; }
			if (A.Key.Y != B.Key.Y) { return A.Key.Y < B.Key.Y; }
			return A.Key.Z < B.Key.Z;
		});
		FJsonArray SleeperArray;
		for (const TPair<FIntVector, int32>& Pair : Sleepers)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetField(TEXT("position"), IntVectorToJson(Pair.Key));
			Object->SetNumberField(TEXT("day"), Pair.Value);
			SleeperArray.Add(MakeShared<FJsonValueObject>(Object));
		}
		Root->SetArrayField(TEXT("sleepers"), SleeperArray);

		FJsonArray PickupArray;
		for (const FMadPickupSaveData& Pickup : Save.Pickups)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetField(TEXT("location"), VectorToJson(Pickup.Location));
			Object->SetNumberField(TEXT("lifetime"), Pickup.Lifetime);
			Object->SetBoolField(TEXT("backpack"), Pickup.bIsBackpack);
			Object->SetArrayField(TEXT("stacks"), StacksToJson(Pickup.Stacks));
			PickupArray.Add(MakeShared<FJsonValueObject>(Object));
		}
		Root->SetArrayField(TEXT("pickups"), PickupArray);

		FJsonArray PlantArray;
		for (const FMadPlantSaveData& Plant : Save.Plants)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetField(TEXT("position"), IntVectorToJson(Plant.Position));
			Object->SetStringField(TEXT("block"), Plant.Block.ToString());
			Object->SetNumberField(TEXT("stage_start_hour"), Plant.StageStartHour);
			PlantArray.Add(MakeShared<FJsonValueObject>(Object));
		}
		Root->SetArrayField(TEXT("plants"), PlantArray);

		// Flat [x, y, x, y, ...]: thousands of cells, and an object each would triple the file.
		FJsonArray ExploredArray;
		ExploredArray.Reserve(Save.Explored.Num() * 2);
		for (const FIntPoint& Cell : Save.Explored)
		{
			ExploredArray.Add(MakeShared<FJsonValueNumber>(Cell.X));
			ExploredArray.Add(MakeShared<FJsonValueNumber>(Cell.Y));
		}
		Root->SetArrayField(TEXT("explored"), ExploredArray);

		// { "mod_id": { "key": value } }, keys sorted so identical stores write identical files.
		TSharedRef<FJsonObject> StoreObject = MakeShared<FJsonObject>();
		TArray<FName> StoreMods;
		Save.ModStore.GetKeys(StoreMods);
		StoreMods.Sort(FNameLexicalLess());
		for (const FName& Mod : StoreMods)
		{
			const TMap<FString, FMadScriptValue>& Values = Save.ModStore[Mod];
			TArray<FString> Keys;
			Values.GetKeys(Keys);
			Keys.Sort();
			TSharedRef<FJsonObject> ModObject = MakeShared<FJsonObject>();
			for (const FString& Key : Keys)
			{
				const FMadScriptValue& Value = Values[Key];
				switch (Value.Type)
				{
				case FMadScriptValue::EType::Bool:   ModObject->SetBoolField(Key, Value.Bool); break;
				case FMadScriptValue::EType::Number: ModObject->SetNumberField(Key, Value.Number); break;
				case FMadScriptValue::EType::String: ModObject->SetStringField(Key, Value.String); break;
				default: break;
				}
			}
			StoreObject->SetObjectField(Mod.ToString(), ModObject);
		}
		Root->SetObjectField(TEXT("mod_store"), StoreObject);

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return Out;
	}

	bool FromJson(const FString& Text, FMadGameplaySave& OutSave, TArray<FString>& OutWarnings)
	{
		OutSave = FMadGameplaySave();

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutWarnings.Add(FString::Printf(TEXT("not valid JSON: %s"), *Reader->GetErrorMessage()));
			return false;
		}

		FString Schema;
		if (!Root->TryGetStringField(TEXT("schema"), Schema) || Schema != GameplaySaveSchemaV1)
		{
			OutWarnings.Add(FString::Printf(TEXT("unsupported save schema '%s'; expected '%s'"), *Schema, GameplaySaveSchemaV1));
			return false;
		}

		FString Seed;
		if (Root->TryGetStringField(TEXT("world_seed"), Seed))
		{
			LexFromString(OutSave.WorldSeed, *Seed);
		}

		const TSharedPtr<FJsonObject>* Clock = nullptr;
		if (Root->TryGetObjectField(TEXT("clock"), Clock))
		{
			double Day = 1.0, Time = 8.0;
			(*Clock)->TryGetNumberField(TEXT("day"), Day);
			(*Clock)->TryGetNumberField(TEXT("time_of_day"), Time);
			OutSave.Day = FMath::Max(1, static_cast<int32>(Day));
			OutSave.TimeOfDay = FMath::Clamp(static_cast<float>(Time), 0.0f, 24.0f);
		}

		const TSharedPtr<FJsonObject>* PlayerObject = nullptr;
		if (Root->TryGetObjectField(TEXT("player"), PlayerObject))
		{
			const TSharedPtr<FJsonObject>& Player = *PlayerObject;
			FMadPlayerSaveData& P = OutSave.Player;
			OutSave.bHasPlayer = true;

			double V[3];
			if (JsonToDoubles(Player->TryGetField(TEXT("location")), 3, V))
			{
				P.Location = FVector(V[0], V[1], V[2]);
				if (!MadFall::GameplaySave::IsRestorableLocation(P.Location))
				{
					OutWarnings.Add(FString::Printf(TEXT("player location %s is outside the world; the player will start at the spawn column"), *P.Location.ToString()));
					P.bHasLocation = false;
				}
			}
			else
			{
				OutWarnings.Add(TEXT("player location is missing; the player will start at the spawn column"));
				OutSave.bHasPlayer = false;
			}
			if (JsonToDoubles(Player->TryGetField(TEXT("view")), 3, V))
			{
				P.ViewRotation = FRotator(V[0], V[1], V[2]);
			}
			double Column[2];
			if (JsonToDoubles(Player->TryGetField(TEXT("spawn_column")), 2, Column))
			{
				P.SpawnColumn = FIntPoint(static_cast<int32>(Column[0]), static_cast<int32>(Column[1]));
			}
			double Bed[3];
			if (JsonToDoubles(Player->TryGetField(TEXT("bed")), 3, Bed))
			{
				P.bHasBed = true;
				P.BedVoxel = FIntVector(static_cast<int32>(Bed[0]), static_cast<int32>(Bed[1]), static_cast<int32>(Bed[2]));
			}

			double Number = 0.0;
			if (Player->TryGetNumberField(TEXT("level"), Number)) { P.Level = FMath::Max(1, static_cast<int32>(Number)); }
			if (Player->TryGetNumberField(TEXT("experience"), Number)) { P.Experience = FMath::Max(0, static_cast<int32>(Number)); }
			if (Player->TryGetNumberField(TEXT("selected_slot"), Number)) { P.SelectedSlot = static_cast<int32>(Number); }

			const TSharedPtr<FJsonObject>* Perks = nullptr;
			if (Player->TryGetObjectField(TEXT("perks"), Perks))
			{
				for (const auto& Pair : (*Perks)->Values)
				{
					if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Number)
					{
						P.PerkRanks.Add(FName(*Pair.Key), FMath::Max(0, static_cast<int32>(Pair.Value->AsNumber())));
					}
				}
			}

			const TSharedPtr<FJsonObject>* Vitals = nullptr;
			if (Player->TryGetObjectField(TEXT("vitals"), Vitals))
			{
				auto Read = [&Vitals](const TCHAR* Field, float& Out)
				{
					double Value = 0.0;
					if ((*Vitals)->TryGetNumberField(Field, Value)) { Out = static_cast<float>(Value); }
				};
				Read(TEXT("health"), P.Stats.Health);
				Read(TEXT("max_health"), P.Stats.MaxHealth);
				Read(TEXT("stamina"), P.Stats.Stamina);
				Read(TEXT("max_stamina"), P.Stats.MaxStamina);
				Read(TEXT("food"), P.Stats.Food);
				Read(TEXT("water"), P.Stats.Water);
				Read(TEXT("core_temperature"), P.Stats.CoreTemperature);
				Read(TEXT("infection"), P.Stats.Infection);
			}

			P.Inventory = JsonToStacks(Player, TEXT("inventory"), TEXT("player.inventory"), OutWarnings);
			P.Worn = JsonToStacks(Player, TEXT("worn"), TEXT("player.worn"), OutWarnings);

			const TSharedPtr<FJsonObject>* Quests = nullptr;
			if (Player->TryGetObjectField(TEXT("quests"), Quests))
			{
				const FJsonArray* Completed = nullptr;
				if ((*Quests)->TryGetArrayField(TEXT("completed"), Completed))
				{
					for (const TSharedPtr<FJsonValue>& Value : *Completed)
					{
						FString Id;
						if (Value->TryGetString(Id))
						{
							P.CompletedQuests.Add(FName(*Id));
						}
					}
				}
				const FJsonArray* ActiveQuests = nullptr;
				if ((*Quests)->TryGetArrayField(TEXT("active"), ActiveQuests))
				{
					for (const TSharedPtr<FJsonValue>& Value : *ActiveQuests)
					{
						const TSharedPtr<FJsonObject>* Entry = nullptr;
						FString Id;
						if (!Value->TryGetObject(Entry) || !(*Entry)->TryGetStringField(TEXT("id"), Id))
						{
							OutWarnings.Add(TEXT("player.quests.active has an entry without an id; skipped"));
							continue;
						}
						FMadQuestProgress& Progress = P.ActiveQuests.AddDefaulted_GetRef();
						Progress.Quest = FName(*Id);
						const FJsonArray* Counts = nullptr;
						if ((*Entry)->TryGetArrayField(TEXT("counts"), Counts))
						{
							for (const TSharedPtr<FJsonValue>& Count : *Counts)
							{
								Progress.Counts.Add(static_cast<int32>(Count->AsNumber()));
							}
						}
					}
				}
			}
		}

		const FJsonArray* Containers = nullptr;
		if (Root->TryGetArrayField(TEXT("containers"), Containers))
		{
			for (int32 Index = 0; Index < Containers->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				double Position[3];
				if (!(*Containers)[Index]->TryGetObject(Object) || !JsonToDoubles((*Object)->TryGetField(TEXT("position")), 3, Position))
				{
					OutWarnings.Add(FString::Printf(TEXT("containers[%d] has no position; skipped"), Index));
					continue;
				}

				FMadContainerSaveData& C = OutSave.Containers.AddDefaulted_GetRef();
				C.Position = FIntVector(static_cast<int32>(Position[0]), static_cast<int32>(Position[1]), static_cast<int32>(Position[2]));
				FString Table;
				if ((*Object)->TryGetStringField(TEXT("loot_table"), Table)) { C.LootTable = FName(*Table); }
				double Tier = 1.0;
				(*Object)->TryGetNumberField(TEXT("tier"), Tier);
				C.Tier = static_cast<int32>(Tier);
				(*Object)->TryGetBoolField(TEXT("rolled"), C.bRolled);
				C.Contents = JsonToStacks(*Object, TEXT("contents"), FString::Printf(TEXT("containers[%d].contents"), Index), OutWarnings);
			}
		}

		const FJsonArray* Sleepers = nullptr;
		if (Root->TryGetArrayField(TEXT("sleepers"), Sleepers))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Sleepers)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				double Position[3];
				double Day = 0.0;
				if (Value->TryGetObject(Object) && JsonToDoubles((*Object)->TryGetField(TEXT("position")), 3, Position)
					&& (*Object)->TryGetNumberField(TEXT("day"), Day))
				{
					OutSave.SleeperDays.Add(FIntVector(static_cast<int32>(Position[0]), static_cast<int32>(Position[1]), static_cast<int32>(Position[2])),
						static_cast<int32>(Day));
				}
			}
		}

		const FJsonArray* Pickups = nullptr;
		if (Root->TryGetArrayField(TEXT("pickups"), Pickups))
		{
			for (int32 Index = 0; Index < Pickups->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				double Location[3];
				if (!(*Pickups)[Index]->TryGetObject(Object) || !JsonToDoubles((*Object)->TryGetField(TEXT("location")), 3, Location))
				{
					OutWarnings.Add(FString::Printf(TEXT("pickups[%d] has no location; skipped"), Index));
					continue;
				}
				FMadPickupSaveData& Pickup = OutSave.Pickups.AddDefaulted_GetRef();
				Pickup.Location = FVector(Location[0], Location[1], Location[2]);
				double Lifetime = Pickup.Lifetime;
				(*Object)->TryGetNumberField(TEXT("lifetime"), Lifetime);
				Pickup.Lifetime = static_cast<float>(Lifetime);
				(*Object)->TryGetBoolField(TEXT("backpack"), Pickup.bIsBackpack);
				Pickup.Stacks = JsonToStacks(*Object, TEXT("stacks"), FString::Printf(TEXT("pickups[%d].stacks"), Index), OutWarnings);
			}
		}

		const FJsonArray* TraderArray = nullptr;
		if (Root->TryGetArrayField(TEXT("traders"), TraderArray))
		{
			for (int32 Index = 0; Index < TraderArray->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				double Position[3];
				FString TraderId;
				if (!(*TraderArray)[Index]->TryGetObject(Object) || !JsonToDoubles((*Object)->TryGetField(TEXT("position")), 3, Position)
					|| !(*Object)->TryGetStringField(TEXT("trader"), TraderId))
				{
					OutWarnings.Add(FString::Printf(TEXT("traders[%d] needs position and trader; skipped"), Index));
					continue;
				}
				FMadTraderSaveData& T = OutSave.Traders.AddDefaulted_GetRef();
				T.Position = FIntVector(static_cast<int32>(Position[0]), static_cast<int32>(Position[1]), static_cast<int32>(Position[2]));
				T.Trader = FName(*TraderId);
				double RestockDay = 0.0;
				(*Object)->TryGetNumberField(TEXT("restock_day"), RestockDay);
				T.RestockDay = static_cast<int32>(RestockDay);
				T.Stock = JsonToStacks(*Object, TEXT("stock"), FString::Printf(TEXT("traders[%d].stock"), Index), OutWarnings);
			}
		}

		const FJsonArray* Plants = nullptr;
		if (Root->TryGetArrayField(TEXT("plants"), Plants))
		{
			for (int32 Index = 0; Index < Plants->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				double Position[3];
				FString Block;
				double StageStart = 0.0;
				if (!(*Plants)[Index]->TryGetObject(Object) || !JsonToDoubles((*Object)->TryGetField(TEXT("position")), 3, Position)
					|| !(*Object)->TryGetStringField(TEXT("block"), Block) || !(*Object)->TryGetNumberField(TEXT("stage_start_hour"), StageStart))
				{
					OutWarnings.Add(FString::Printf(TEXT("plants[%d] needs position, block and stage_start_hour; skipped"), Index));
					continue;
				}
				FMadPlantSaveData& Plant = OutSave.Plants.AddDefaulted_GetRef();
				Plant.Position = FIntVector(static_cast<int32>(Position[0]), static_cast<int32>(Position[1]), static_cast<int32>(Position[2]));
				Plant.Block = FName(*Block);
				Plant.StageStartHour = StageStart;
			}
		}

		const FJsonArray* ExploredArray = nullptr;
		if (Root->TryGetArrayField(TEXT("explored"), ExploredArray))
		{
			if (ExploredArray->Num() % 2 != 0)
			{
				OutWarnings.Add(TEXT("explored has an odd number of values; the last is ignored"));
			}
			OutSave.Explored.Reserve(ExploredArray->Num() / 2);
			for (int32 Index = 0; Index + 1 < ExploredArray->Num(); Index += 2)
			{
				OutSave.Explored.Add(FIntPoint(static_cast<int32>((*ExploredArray)[Index]->AsNumber()), static_cast<int32>((*ExploredArray)[Index + 1]->AsNumber())));
			}
		}

		const TSharedPtr<FJsonObject>* StoreObject = nullptr;
		if (Root->TryGetObjectField(TEXT("mod_store"), StoreObject))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& ModPair : (*StoreObject)->Values)
			{
				const TSharedPtr<FJsonObject>* ModObject = nullptr;
				if (!ModPair.Value.IsValid() || !ModPair.Value->TryGetObject(ModObject))
				{
					OutWarnings.Add(FString::Printf(TEXT("mod_store.%s is not an object; skipped"), *ModPair.Key));
					continue;
				}
				TMap<FString, FMadScriptValue>& Values = OutSave.ModStore.Add(FName(*ModPair.Key));
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ModObject)->Values)
				{
					switch (Pair.Value.IsValid() ? Pair.Value->Type : EJson::None)
					{
					case EJson::Boolean: Values.Add(Pair.Key, FMadScriptValue::MakeBool(Pair.Value->AsBool())); break;
					case EJson::Number:  Values.Add(Pair.Key, FMadScriptValue::MakeNumber(Pair.Value->AsNumber())); break;
					case EJson::String:  Values.Add(Pair.Key, FMadScriptValue::MakeString(Pair.Value->AsString())); break;
					default:
						OutWarnings.Add(FString::Printf(TEXT("mod_store.%s.%s must be a boolean, number or string; skipped"), *ModPair.Key, *Pair.Key));
						break;
					}
				}
			}
		}

		return true;
	}

	bool WriteFile(const FString& Path, const FMadGameplaySave& Save, FString& OutError)
	{
		IFileManager& Files = IFileManager::Get();
		Files.MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);

		const FString Temp = Path + TEXT(".tmp");
		const FString Backup = Path + TEXT(".bak");

		if (!FFileHelper::SaveStringToFile(ToJson(Save), *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("could not write %s"), *Temp);
			return false;
		}

		if (Files.FileExists(*Path) && !Files.Move(*Backup, *Path, /*Replace*/ true))
		{
			OutError = FString::Printf(TEXT("could not rotate %s to %s"), *Path, *Backup);
			return false;
		}

		if (!Files.Move(*Path, *Temp, /*Replace*/ true))
		{
			OutError = FString::Printf(TEXT("could not move %s into place"), *Temp);
			return false;
		}
		return true;
	}

	bool ReadFile(const FString& Path, FMadGameplaySave& OutSave, TArray<FString>& OutWarnings, bool& bOutUsedBackup)
	{
		bOutUsedBackup = false;

		FString Text;
		if (FFileHelper::LoadFileToString(Text, *Path) && FromJson(Text, OutSave, OutWarnings))
		{
			return true;
		}

		const FString Backup = Path + TEXT(".bak");
		if (FFileHelper::LoadFileToString(Text, *Backup) && FromJson(Text, OutSave, OutWarnings))
		{
			bOutUsedBackup = true;
			OutWarnings.Add(FString::Printf(TEXT("%s was unreadable; loaded the previous save from %s"), *Path, *Backup));
			return true;
		}
		return false;
	}
}
