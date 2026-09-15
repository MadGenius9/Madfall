// Copyright MadFall. All Rights Reserved.

#include "MadGameplayDefinitions.h"
#include "MadSurfaceRegistry.h"

#include "Dom/JsonObject.h"
#include "MadBlockRegistry.h"
#include "MadDefinitionPatches.h"
#include "MadDefinitionSources.h"
#include "MadFallCore.h"
#include "MadJsonReader.h"
#include "MadLocalization.h"
#include "MadVoxelWorldSubsystem.h"
#include "Misc/StringBuilder.h"

namespace
{
	using FReader = FMadJsonReader;

	const FName TagIndestructible(TEXT("block.indestructible"));
	const FName TagItemBlock(TEXT("item.block"));

	/** schema + id + namespace, shared by all three kinds. */
	bool ReadHeader(const FReader& R, const TSharedRef<FJsonObject>& Object, const TCHAR* Schema, FName ModId,
		bool bPathId, FName& OutId)
	{
		FString SchemaText;
		if (!R.ReadString(Object, TEXT("schema"), TEXT("/schema"), SchemaText))
		{
			R.AddError(TEXT("/schema"), FString::Printf(TEXT("missing required field; expected \"%s\""), Schema));
			return false;
		}
		if (SchemaText != Schema)
		{
			R.AddError(TEXT("/schema"), FString::Printf(TEXT("unsupported schema \"%s\"; this build understands \"%s\""), *SchemaText, Schema));
			return false;
		}

		if (!R.ReadName(Object, TEXT("id"), TEXT("/id"), OutId))
		{
			R.AddError(TEXT("/id"), TEXT("missing required field"));
			return false;
		}

		FString Reason;
		const bool bValid = bPathId
			? MadFall::Definitions::IsValidPathId(OutId, Reason)
			: MadFall::BlockDefinitionJson::IsValidBlockId(OutId, Reason);
		if (!bValid)
		{
			R.AddError(TEXT("/id"), Reason);
			return false;
		}

		const FName Namespace = MadFall::BlockDefinitionJson::GetNamespace(OutId);
		if (!ModId.IsNone() && Namespace != ModId)
		{
			R.AddError(TEXT("/id"), FString::Printf(
				TEXT("namespace '%s' does not match the owning mod id '%s'. To change another mod's definitions, ship a patch instead."),
				*Namespace.ToString(), *ModId.ToString()));
			return false;
		}
		return true;
	}

	/** { "item": "...", "count": n }. Count defaults to 1. */
	bool ReadAmountObject(const FReader& R, const TSharedPtr<FJsonValue>& Value, const FString& Pointer, FMadItemAmount& Out)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			R.AddError(Pointer, FString::Printf(TEXT("expected an object like {\"item\": \"mod:id\", \"count\": 1}, got %s"),
				*FReader::DescribeType(Value)));
			return false;
		}

		const TSharedRef<FJsonObject> Obj = Value->AsObject().ToSharedRef();
		FMadItemAmount Amount;
		if (!R.ReadName(Obj, TEXT("item"), Pointer + TEXT("/item"), Amount.Item))
		{
			R.AddError(Pointer + TEXT("/item"), TEXT("missing required field"));
			return false;
		}
		R.ReadInt(Obj, TEXT("count"), Pointer + TEXT("/count"), Amount.Count);
		if (Amount.Count < 1)
		{
			R.AddError(Pointer + TEXT("/count"), FString::Printf(TEXT("must be at least 1, got %d"), Amount.Count));
			return false;
		}
		R.ReportUnknownFields(Obj, { TEXT("item"), TEXT("count") });
		Out = Amount;
		return true;
	}

	bool ReadAmountArray(const FReader& R, const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Pointer,
		TArray<FMadItemAmount>& Out)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		if (!R.ReadArray(Obj, Field, Pointer, Values))
		{
			return false;
		}

		TArray<FMadItemAmount> Parsed;
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			FMadItemAmount Amount;
			if (ReadAmountObject(R, Values[Index], FString::Printf(TEXT("%s/%d"), *Pointer, Index), Amount))
			{
				Parsed.Add(Amount);
			}
		}
		Out = MoveTemp(Parsed);
		return true;
	}

	/** Maps MERGE on inheritance: only keys present are written. */
	bool ReadFloatMap(const FReader& R, const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Pointer,
		TMap<FName, float>& InOut)
	{
		TSharedPtr<FJsonObject> Map;
		if (!R.ReadObject(Obj, Field, Pointer, Map))
		{
			return false;
		}
		for (const auto& Pair : Map->Values)
		{
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Number)
			{
				R.AddError(FString::Printf(TEXT("%s/%s"), *Pointer, *Pair.Key),
					FString::Printf(TEXT("expected a number, got %s"), *FReader::DescribeType(Pair.Value)));
				continue;
			}
			InOut.Add(FName(*Pair.Key), static_cast<float>(Pair.Value->AsNumber()));
		}
		return true;
	}

	/** A number, or [min, max]. */
	bool ReadIntRange(const FReader& R, const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Pointer,
		int32& OutMin, int32& OutMax)
	{
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Field);
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return false;
		}
		if (Value->Type == EJson::Number)
		{
			OutMin = OutMax = static_cast<int32>(FMath::RoundToDouble(Value->AsNumber()));
			return true;
		}

		float Min = 0.0f, Max = 0.0f;
		if (!R.ReadRange(Obj, Field, Pointer, Min, Max))
		{
			return false;
		}
		OutMin = FMath::RoundToInt(Min);
		OutMax = FMath::RoundToInt(Max);
		return true;
	}

	bool ParseKind(const FString& Text, EMadItemKind& Out)
	{
		static const TMap<FString, EMadItemKind> Kinds =
		{
			{ TEXT("resource"), EMadItemKind::Resource }, { TEXT("block"), EMadItemKind::Block },
			{ TEXT("tool"), EMadItemKind::Tool }, { TEXT("weapon"), EMadItemKind::Weapon },
			{ TEXT("consumable"), EMadItemKind::Consumable }, { TEXT("mod"), EMadItemKind::Mod }
		};
		if (const EMadItemKind* Found = Kinds.Find(Text))
		{
			Out = *Found;
			return true;
		}
		return false;
	}

	const TCHAR* KindName(EMadItemKind Kind)
	{
		switch (Kind)
		{
		case EMadItemKind::Block:      return TEXT("block");
		case EMadItemKind::Tool:       return TEXT("tool");
		case EMadItemKind::Weapon:     return TEXT("weapon");
		case EMadItemKind::Consumable: return TEXT("consumable");
		case EMadItemKind::Mod:        return TEXT("mod");
		default:                       return TEXT("resource");
		}
	}
}

// ===========================================================================
// Parsing
// ===========================================================================

namespace MadFall::GameplayDefinitionsJson
{
	bool ParseItem(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
		FMadItemDefinition& Data, TArray<FMadDefinitionError>& OutErrors)
	{
		const FReader R{ SourcePath, OutErrors };

		if (!ReadHeader(R, Object, MadFall::ItemSchemaV1, ModId, /*bPathId*/ false, Data.Id))
		{
			return false;
		}

		R.ReadName(Object, TEXT("extends"), TEXT("/extends"), Data.Extends);
		R.ReadString(Object, TEXT("display_name"), TEXT("/display_name"), Data.DisplayName);
		R.ReadNameArray(Object, TEXT("tags"), TEXT("/tags"), Data.Tags);
		if (R.ReadInt(Object, TEXT("max_stack"), TEXT("/max_stack"), Data.MaxStack))
		{
			Data.bMaxStackExplicit = true;
		}
		R.ReadFloat(Object, TEXT("weight_kg"), TEXT("/weight_kg"), Data.WeightKg);
		R.ReadSoftPath(Object, TEXT("icon"), TEXT("/icon"), Data.Icon);
		R.ReadName(Object, TEXT("places_block"), TEXT("/places_block"), Data.PlacesBlock);
		if (R.ReadInt(Object, TEXT("value"), TEXT("/value"), Data.Value) && Data.Value < 0)
		{
			R.AddError(TEXT("/value"), TEXT("must not be negative"));
			Data.Value = 0;
		}

		FString KindText;
		if (R.ReadString(Object, TEXT("kind"), TEXT("/kind"), KindText) && !ParseKind(KindText, Data.Kind))
		{
			R.AddError(TEXT("/kind"), FString::Printf(
				TEXT("unknown kind '%s'; expected resource, block, tool, weapon, consumable or mod"), *KindText));
		}

		TSharedPtr<FJsonObject> Section;
		if (R.ReadObject(Object, TEXT("tool"), TEXT("/tool"), Section))
		{
			const TSharedRef<FJsonObject> T = Section.ToSharedRef();
			FMadToolStats& Tool = Data.Tool;
			Data.bHasTool = true;

			ReadFloatMap(R, T, TEXT("damage"), TEXT("/tool/damage"), Tool.Damage);
			R.ReadNameArray(T, TEXT("harvest_tags"), TEXT("/tool/harvest_tags"), Tool.HarvestTags);
			R.ReadInt(T, TEXT("tier"), TEXT("/tool/tier"), Tool.Tier);
			R.ReadInt(T, TEXT("durability"), TEXT("/tool/durability"), Tool.Durability);
			R.ReadFloat(T, TEXT("use_seconds"), TEXT("/tool/use_seconds"), Tool.UseSeconds);
			R.ReadFloat(T, TEXT("stamina_cost"), TEXT("/tool/stamina_cost"), Tool.StaminaCost);
			R.ReadFloat(T, TEXT("range"), TEXT("/tool/range"), Tool.Range);
			R.ReadInt(T, TEXT("mod_slots"), TEXT("/tool/mod_slots"), Tool.ModSlots);
			ReadAmountArray(R, T, TEXT("repair_with"), TEXT("/tool/repair_with"), Tool.RepairWith);
			R.ReadFloat(T, TEXT("repair_fraction"), TEXT("/tool/repair_fraction"), Tool.RepairFraction);

			TSharedPtr<FJsonObject> RangedSection;
			if (R.ReadObject(T, TEXT("ranged"), TEXT("/tool/ranged"), RangedSection))
			{
				const TSharedRef<FJsonObject> Ranged = RangedSection.ToSharedRef();
				R.ReadName(Ranged, TEXT("ammo"), TEXT("/tool/ranged/ammo"), Tool.Ammo);
				R.ReadFloat(Ranged, TEXT("speed"), TEXT("/tool/ranged/speed"), Tool.ProjectileSpeed);
				R.ReadFloat(Ranged, TEXT("gravity"), TEXT("/tool/ranged/gravity"), Tool.ProjectileGravity);
				R.ReadFloat(Ranged, TEXT("recover_chance"), TEXT("/tool/ranged/recover_chance"), Tool.RecoverChance);
				R.ReportUnknownFields(Ranged, { TEXT("ammo"), TEXT("speed"), TEXT("gravity"), TEXT("recover_chance") });
				if (Tool.Ammo.IsNone() || Tool.ProjectileSpeed <= 0.0f)
				{
					R.AddError(TEXT("/tool/ranged"), TEXT("needs \"ammo\" and a \"speed\" above 0"));
					Tool.Ammo = NAME_None;
				}
			}
			R.ReportUnknownFields(T, { TEXT("damage"), TEXT("harvest_tags"), TEXT("tier"), TEXT("durability"),
				TEXT("use_seconds"), TEXT("stamina_cost"), TEXT("range"), TEXT("mod_slots"), TEXT("repair_with"),
				TEXT("repair_fraction"), TEXT("ranged") });
		}

		if (R.ReadObject(Object, TEXT("consumable"), TEXT("/consumable"), Section))
		{
			const TSharedRef<FJsonObject> C = Section.ToSharedRef();
			Data.bHasConsumable = true;
			R.ReadFloat(C, TEXT("use_seconds"), TEXT("/consumable/use_seconds"), Data.Consumable.UseSeconds);
			ReadFloatMap(R, C, TEXT("effects"), TEXT("/consumable/effects"), Data.Consumable.Effects);
			R.ReadName(C, TEXT("returns"), TEXT("/consumable/returns"), Data.Consumable.Returns);
			R.ReportUnknownFields(C, { TEXT("use_seconds"), TEXT("effects"), TEXT("returns") });
		}

		if (R.ReadObject(Object, TEXT("wear"), TEXT("/wear"), Section))
		{
			const TSharedRef<FJsonObject> Wear = Section.ToSharedRef();
			Data.bHasWear = true;
			R.ReadName(Wear, TEXT("slot"), TEXT("/wear/slot"), Data.Wear.Slot);
			R.ReadFloat(Wear, TEXT("cold"), TEXT("/wear/cold"), Data.Wear.Cold);
			R.ReadFloat(Wear, TEXT("heat"), TEXT("/wear/heat"), Data.Wear.Heat);
			R.ReadFloat(Wear, TEXT("armor"), TEXT("/wear/armor"), Data.Wear.Armor);
			R.ReportUnknownFields(Wear, { TEXT("slot"), TEXT("cold"), TEXT("heat"), TEXT("armor") });
			if (MadFall::Wear::GetSlotIndex(Data.Wear.Slot) == INDEX_NONE)
			{
				R.AddError(TEXT("/wear/slot"), FString::Printf(TEXT("'%s' is not one of head, body, legs, feet"), *Data.Wear.Slot.ToString()));
				Data.bHasWear = false;
			}
			if (Data.Wear.Armor < 0.0f || Data.Wear.Armor > MadFall::Wear::MaxArmor)
			{
				R.AddError(TEXT("/wear/armor"), TEXT("must be from 0 to 0.8"));
				Data.Wear.Armor = FMath::Clamp(Data.Wear.Armor, 0.0f, MadFall::Wear::MaxArmor);
			}
		}

		R.ReadName(Object, TEXT("fill"), TEXT("/fill"), Data.FillsInto);
		R.ReadName(Object, TEXT("till"), TEXT("/till"), Data.TillsInto);

		if (R.ReadObject(Object, TEXT("mod"), TEXT("/mod"), Section))
		{
			const TSharedRef<FJsonObject> M = Section.ToSharedRef();
			Data.bHasMod = true;
			R.ReadNameArray(M, TEXT("applies_to_tags"), TEXT("/mod/applies_to_tags"), Data.Mod.AppliesToTags);
			ReadFloatMap(R, M, TEXT("multipliers"), TEXT("/mod/multipliers"), Data.Mod.Multipliers);
			R.ReportUnknownFields(M, { TEXT("applies_to_tags"), TEXT("multipliers") });
		}

		R.ReportUnknownFields(Object, { TEXT("schema"), TEXT("id"), TEXT("extends"), TEXT("display_name"), TEXT("tags"),
			TEXT("kind"), TEXT("max_stack"), TEXT("weight_kg"), TEXT("icon"), TEXT("places_block"), TEXT("tool"),
			TEXT("consumable"), TEXT("mod"), TEXT("fill"), TEXT("till"), TEXT("wear"), TEXT("value"), TEXT("mod_data") });

		Data.SourceModId = ModId;
		Data.SourcePath = SourcePath;
		return true;
	}

	bool ParseRecipe(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
		FMadRecipeDefinition& Data, TArray<FMadDefinitionError>& OutErrors)
	{
		const FReader R{ SourcePath, OutErrors };

		if (!ReadHeader(R, Object, MadFall::RecipeSchemaV1, ModId, /*bPathId*/ true, Data.Id))
		{
			return false;
		}

		const TSharedPtr<FJsonValue> Output = Object->TryGetField(TEXT("output"));
		if (!Output.IsValid())
		{
			R.AddError(TEXT("/output"), TEXT("missing required field"));
			return false;
		}
		if (!ReadAmountObject(R, Output, TEXT("/output"), Data.Output))
		{
			return false;
		}

		if (!ReadAmountArray(R, Object, TEXT("ingredients"), TEXT("/ingredients"), Data.Ingredients) || Data.Ingredients.Num() == 0)
		{
			R.AddError(TEXT("/ingredients"), TEXT("a recipe needs at least one ingredient"));
			return false;
		}

		R.ReadName(Object, TEXT("station"), TEXT("/station"), Data.Station);
		R.ReadFloat(Object, TEXT("craft_seconds"), TEXT("/craft_seconds"), Data.CraftSeconds);
		R.ReadInt(Object, TEXT("required_level"), TEXT("/required_level"), Data.RequiredLevel);
		R.ReadNameArray(Object, TEXT("tags"), TEXT("/tags"), Data.Tags);

		R.ReportUnknownFields(Object, { TEXT("schema"), TEXT("id"), TEXT("output"), TEXT("ingredients"), TEXT("station"),
			TEXT("craft_seconds"), TEXT("required_level"), TEXT("tags"), TEXT("mod_data") });

		Data.CraftSeconds = FMath::Max(0.0f, Data.CraftSeconds);
		Data.SourceModId = ModId;
		Data.SourcePath = SourcePath;
		return true;
	}

	bool ParseLootTable(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
		FMadLootTableDefinition& Data, TArray<FMadDefinitionError>& OutErrors)
	{
		const FReader R{ SourcePath, OutErrors };

		if (!ReadHeader(R, Object, MadFall::LootSchemaV1, ModId, /*bPathId*/ true, Data.Id))
		{
			return false;
		}

		ReadIntRange(R, Object, TEXT("rolls"), TEXT("/rolls"), Data.RollsMin, Data.RollsMax);
		R.ReadFloat(Object, TEXT("rolls_per_tier"), TEXT("/rolls_per_tier"), Data.RollsPerTier);
		R.ReadFloat(Object, TEXT("empty_weight"), TEXT("/empty_weight"), Data.EmptyWeight);

		TArray<TSharedPtr<FJsonValue>> Entries;
		if (!R.ReadArray(Object, TEXT("entries"), TEXT("/entries"), Entries) || Entries.Num() == 0)
		{
			R.AddError(TEXT("/entries"), TEXT("a loot table needs at least one entry"));
			return false;
		}

		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			const FString Pointer = FString::Printf(TEXT("/entries/%d"), Index);
			if (!Entries[Index].IsValid() || Entries[Index]->Type != EJson::Object)
			{
				R.AddError(Pointer, FString::Printf(TEXT("expected an object, got %s"), *FReader::DescribeType(Entries[Index])));
				continue;
			}

			const TSharedRef<FJsonObject> E = Entries[Index]->AsObject().ToSharedRef();
			FMadLootEntry Entry;
			R.ReadName(E, TEXT("item"), Pointer + TEXT("/item"), Entry.Item);
			R.ReadName(E, TEXT("table"), Pointer + TEXT("/table"), Entry.Table);

			if (Entry.Item.IsNone() == Entry.Table.IsNone())
			{
				R.AddError(Pointer, TEXT("an entry needs exactly one of \"item\" or \"table\""));
				continue;
			}

			R.ReadFloat(E, TEXT("weight"), Pointer + TEXT("/weight"), Entry.Weight);
			ReadIntRange(R, E, TEXT("count"), Pointer + TEXT("/count"), Entry.CountMin, Entry.CountMax);
			ReadIntRange(R, E, TEXT("tier"), Pointer + TEXT("/tier"), Entry.MinTier, Entry.MaxTier);
			R.ReadInt(E, TEXT("min_game_stage"), Pointer + TEXT("/min_game_stage"), Entry.MinGameStage);
			R.ReadRange(E, TEXT("durability"), Pointer + TEXT("/durability"), Entry.DurabilityMin, Entry.DurabilityMax);
			R.ReadBool(E, TEXT("always"), Pointer + TEXT("/always"), Entry.bAlways);

			if (Entry.Weight < 0.0f)
			{
				R.AddError(Pointer + TEXT("/weight"), TEXT("must not be negative"));
				continue;
			}
			if (Entry.CountMin < 0 || Entry.CountMax < Entry.CountMin)
			{
				R.AddError(Pointer + TEXT("/count"), TEXT("count must be >= 0 with min <= max"));
				continue;
			}

			R.ReportUnknownFields(E, { TEXT("item"), TEXT("table"), TEXT("weight"), TEXT("count"), TEXT("tier"),
				TEXT("min_game_stage"), TEXT("durability"), TEXT("always") });
			Data.Entries.Add(Entry);
		}

		R.ReportUnknownFields(Object, { TEXT("schema"), TEXT("id"), TEXT("rolls"), TEXT("rolls_per_tier"),
			TEXT("empty_weight"), TEXT("entries"), TEXT("mod_data") });

		Data.RollsMin = FMath::Max(0, Data.RollsMin);
		Data.RollsMax = FMath::Max(Data.RollsMin, Data.RollsMax);
		Data.SourceModId = ModId;
		Data.SourcePath = SourcePath;
		return true;
	}

	bool ParseZombie(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
		FMadZombieDefinition& Data, TArray<FMadDefinitionError>& OutErrors)
	{
		const FReader R{ SourcePath, OutErrors };

		if (!ReadHeader(R, Object, MadFall::ZombieSchemaV1, ModId, /*bPathId*/ false, Data.Id))
		{
			return false;
		}

		R.ReadString(Object, TEXT("display_name"), TEXT("/display_name"), Data.DisplayName);
		R.ReadNameArray(Object, TEXT("tags"), TEXT("/tags"), Data.Tags);
		R.ReadNameArray(Object, TEXT("spawn_groups"), TEXT("/spawn_groups"), Data.SpawnGroups);
		R.ReadFloat(Object, TEXT("spawn_weight"), TEXT("/spawn_weight"), Data.SpawnWeight);
		R.ReadInt(Object, TEXT("min_game_stage"), TEXT("/min_game_stage"), Data.MinGameStage);

		TSharedPtr<FJsonObject> Section;
		if (R.ReadObject(Object, TEXT("stats"), TEXT("/stats"), Section))
		{
			const TSharedRef<FJsonObject> S = Section.ToSharedRef();
			R.ReadFloat(S, TEXT("health"), TEXT("/stats/health"), Data.Health);
			R.ReadFloat(S, TEXT("walk_speed"), TEXT("/stats/walk_speed"), Data.WalkSpeed);
			R.ReadFloat(S, TEXT("run_speed"), TEXT("/stats/run_speed"), Data.RunSpeed);
			R.ReadFloat(S, TEXT("attack_damage"), TEXT("/stats/attack_damage"), Data.AttackDamage);
			R.ReadFloat(S, TEXT("block_damage"), TEXT("/stats/block_damage"), Data.BlockDamage);
			R.ReadFloat(S, TEXT("attack_seconds"), TEXT("/stats/attack_seconds"), Data.AttackSeconds);
			R.ReadName(S, TEXT("damage_type"), TEXT("/stats/damage_type"), Data.DamageType);
			R.ReadFloat(S, TEXT("infection_per_hit"), TEXT("/stats/infection_per_hit"), Data.InfectionPerHit);
			R.ReportUnknownFields(S, { TEXT("health"), TEXT("walk_speed"), TEXT("run_speed"), TEXT("attack_damage"),
				TEXT("block_damage"), TEXT("attack_seconds"), TEXT("damage_type"), TEXT("infection_per_hit") });
		}

		if (R.ReadObject(Object, TEXT("abilities"), TEXT("/abilities"), Section))
		{
			const TSharedRef<FJsonObject> A = Section.ToSharedRef();
			TSharedPtr<FJsonObject> Ability;
			if (R.ReadObject(A, TEXT("ranged"), TEXT("/abilities/ranged"), Ability))
			{
				const TSharedRef<FJsonObject> Ranged = Ability.ToSharedRef();
				R.ReadFloat(Ranged, TEXT("damage"), TEXT("/abilities/ranged/damage"), Data.RangedDamage);
				R.ReadFloat(Ranged, TEXT("range"), TEXT("/abilities/ranged/range"), Data.RangedRange);
				R.ReadFloat(Ranged, TEXT("seconds"), TEXT("/abilities/ranged/seconds"), Data.RangedSeconds);
				R.ReadFloat(Ranged, TEXT("speed"), TEXT("/abilities/ranged/speed"), Data.RangedSpeed);
				R.ReportUnknownFields(Ranged, { TEXT("damage"), TEXT("range"), TEXT("seconds"), TEXT("speed") });
				if (Data.RangedSeconds < 0.2f || Data.RangedSpeed <= 0.0f)
				{
					R.AddError(TEXT("/abilities/ranged"), TEXT("seconds must be at least 0.2 and speed above 0"));
					Data.RangedSeconds = FMath::Max(0.2f, Data.RangedSeconds);
					Data.RangedSpeed = FMath::Max(1.0f, Data.RangedSpeed);
				}
			}
			if (R.ReadObject(A, TEXT("scream"), TEXT("/abilities/scream"), Ability))
			{
				const TSharedRef<FJsonObject> Scream = Ability.ToSharedRef();
				R.ReadFloat(Scream, TEXT("radius"), TEXT("/abilities/scream/radius"), Data.ScreamRadius);
				R.ReadFloat(Scream, TEXT("seconds"), TEXT("/abilities/scream/seconds"), Data.ScreamSeconds);
				R.ReadInt(Scream, TEXT("summons"), TEXT("/abilities/scream/summons"), Data.ScreamSummons);
				R.ReportUnknownFields(Scream, { TEXT("radius"), TEXT("seconds"), TEXT("summons") });
				Data.ScreamSeconds = FMath::Max(5.0f, Data.ScreamSeconds);
				Data.ScreamSummons = FMath::Clamp(Data.ScreamSummons, 0, 8);
			}
			R.ReadBool(A, TEXT("climbs_walls"), TEXT("/abilities/climbs_walls"), Data.bClimbsWalls);
			R.ReportUnknownFields(A, { TEXT("ranged"), TEXT("scream"), TEXT("climbs_walls") });
		}

		if (R.ReadObject(Object, TEXT("resistances"), TEXT("/resistances"), Section))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Section->Values)
			{
				const FString Pointer = FString::Printf(TEXT("/resistances/%s"), *Pair.Key);
				if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Number || Pair.Value->AsNumber() < 0.0)
				{
					R.AddError(Pointer, TEXT("expected a damage multiplier, a number of 0 or more"));
					continue;
				}
				Data.Resistances.Add(FName(*Pair.Key), static_cast<float>(Pair.Value->AsNumber()));
			}
		}

		if (R.ReadObject(Object, TEXT("senses"), TEXT("/senses"), Section))
		{
			const TSharedRef<FJsonObject> S = Section.ToSharedRef();
			R.ReadFloat(S, TEXT("sight"), TEXT("/senses/sight"), Data.SightRange);
			R.ReadFloat(S, TEXT("hearing"), TEXT("/senses/hearing"), Data.HearingRange);
			R.ReportUnknownFields(S, { TEXT("sight"), TEXT("hearing") });
		}

		if (R.ReadObject(Object, TEXT("rewards"), TEXT("/rewards"), Section))
		{
			const TSharedRef<FJsonObject> S = Section.ToSharedRef();
			R.ReadInt(S, TEXT("experience"), TEXT("/rewards/experience"), Data.Experience);
			R.ReadName(S, TEXT("loot_table"), TEXT("/rewards/loot_table"), Data.LootTable);
			R.ReportUnknownFields(S, { TEXT("experience"), TEXT("loot_table") });
		}

		if (R.ReadObject(Object, TEXT("appearance"), TEXT("/appearance"), Section))
		{
			const TSharedRef<FJsonObject> S = Section.ToSharedRef();
			R.ReadFloat(S, TEXT("scale"), TEXT("/appearance/scale"), Data.Scale);
			TArray<TSharedPtr<FJsonValue>> Tint;
			if (R.ReadArray(S, TEXT("tint"), TEXT("/appearance/tint"), Tint))
			{
				if (Tint.Num() == 3 && Tint[0]->Type == EJson::Number && Tint[1]->Type == EJson::Number && Tint[2]->Type == EJson::Number)
				{
					// sRGB, like every other authored colour.
					Data.Tint = FLinearColor(
						MadFall::Surfaces::SRGBToLinear(static_cast<float>(Tint[0]->AsNumber())),
						MadFall::Surfaces::SRGBToLinear(static_cast<float>(Tint[1]->AsNumber())),
						MadFall::Surfaces::SRGBToLinear(static_cast<float>(Tint[2]->AsNumber())));
				}
				else
				{
					R.AddError(TEXT("/appearance/tint"), TEXT("expected [r, g, b] with numbers from 0 to 1"));
				}
			}
			R.ReportUnknownFields(S, { TEXT("scale"), TEXT("tint") });
		}

		R.ReportUnknownFields(Object, { TEXT("schema"), TEXT("id"), TEXT("display_name"), TEXT("tags"), TEXT("spawn_groups"),
			TEXT("spawn_weight"), TEXT("min_game_stage"), TEXT("stats"), TEXT("senses"), TEXT("rewards"), TEXT("appearance"), TEXT("abilities"),
			TEXT("resistances"), TEXT("mod_data") });

		Data.Health = FMath::Max(1.0f, Data.Health);
		Data.AttackSeconds = FMath::Max(0.1f, Data.AttackSeconds);
		Data.Scale = FMath::Clamp(Data.Scale, 0.25f, 4.0f);
		Data.SourceModId = ModId;
		Data.SourcePath = SourcePath;
		return true;
	}

	bool ReadColour(const FReader& R, const TSharedRef<FJsonObject>& Object, const TCHAR* Field, const FString& Pointer, FLinearColor& Out)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		if (!R.ReadArray(Object, Field, Pointer, Values))
		{
			return false;
		}
		if (Values.Num() != 3 || Values[0]->Type != EJson::Number || Values[1]->Type != EJson::Number || Values[2]->Type != EJson::Number)
		{
			R.AddError(Pointer, TEXT("expected [r, g, b] with numbers from 0 to 1"));
			return false;
		}
		// sRGB, like every other authored colour.
		Out = FLinearColor(
			MadFall::Surfaces::SRGBToLinear(static_cast<float>(Values[0]->AsNumber())),
			MadFall::Surfaces::SRGBToLinear(static_cast<float>(Values[1]->AsNumber())),
			MadFall::Surfaces::SRGBToLinear(static_cast<float>(Values[2]->AsNumber())));
		return true;
	}

	bool ParseQuest(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
		FMadQuestDefinition& Data, TArray<FMadDefinitionError>& OutErrors)
	{
		const FReader R{ SourcePath, OutErrors };
		if (!ReadHeader(R, Object, MadFall::QuestSchemaV1, ModId, /*bPathId*/ true, Data.Id))
		{
			return false;
		}

		R.ReadString(Object, TEXT("display_name"), TEXT("/display_name"), Data.DisplayName);
		R.ReadString(Object, TEXT("description"), TEXT("/description"), Data.Description);
		R.ReadNameArray(Object, TEXT("requires"), TEXT("/requires"), Data.Requires);
		R.ReadInt(Object, TEXT("order"), TEXT("/order"), Data.Order);

		TArray<TSharedPtr<FJsonValue>> Objectives;
		if (!R.ReadArray(Object, TEXT("objectives"), TEXT("/objectives"), Objectives) || Objectives.Num() == 0)
		{
			R.AddError(TEXT("/objectives"), TEXT("a quest needs at least one objective"));
			return false;
		}

		static const TMap<FString, EMadQuestObjectiveType> Types = {
			{ TEXT("craft"), EMadQuestObjectiveType::Craft },
			{ TEXT("have"), EMadQuestObjectiveType::Have },
			{ TEXT("place"), EMadQuestObjectiveType::Place },
			{ TEXT("break"), EMadQuestObjectiveType::Break },
			{ TEXT("kill_zombie"), EMadQuestObjectiveType::KillZombie },
			{ TEXT("kill_animal"), EMadQuestObjectiveType::KillAnimal },
			{ TEXT("wear"), EMadQuestObjectiveType::Wear },
			{ TEXT("set_spawn"), EMadQuestObjectiveType::SetSpawn },
			{ TEXT("reach_day"), EMadQuestObjectiveType::ReachDay },
			{ TEXT("trade"), EMadQuestObjectiveType::Trade },
		};

		for (int32 Index = 0; Index < Objectives.Num(); ++Index)
		{
			const FString Pointer = FString::Printf(TEXT("/objectives/%d"), Index);
			if (!Objectives[Index].IsValid() || Objectives[Index]->Type != EJson::Object)
			{
				R.AddError(Pointer, TEXT("expected an object"));
				continue;
			}
			const TSharedRef<FJsonObject> O = Objectives[Index]->AsObject().ToSharedRef();
			FMadQuestObjective Objective;
			FString TypeText;
			R.ReadString(O, TEXT("type"), Pointer + TEXT("/type"), TypeText);
			const EMadQuestObjectiveType* Type = Types.Find(TypeText);
			if (Type == nullptr)
			{
				R.AddError(Pointer + TEXT("/type"), FString::Printf(TEXT("'%s' is not one of craft, have, place, break, kill_zombie, kill_animal, wear, set_spawn, reach_day, trade"), *TypeText));
				continue;
			}
			Objective.Type = *Type;
			R.ReadName(O, TEXT("target"), Pointer + TEXT("/target"), Objective.Target);
			R.ReadName(O, TEXT("tag"), Pointer + TEXT("/tag"), Objective.Tag);
			R.ReadInt(O, TEXT("count"), Pointer + TEXT("/count"), Objective.Count);
			R.ReadString(O, TEXT("text"), Pointer + TEXT("/text"), Objective.Text);
			R.ReportUnknownFields(O, { TEXT("type"), TEXT("target"), TEXT("tag"), TEXT("count"), TEXT("text") });
			if (Objective.Count < 1)
			{
				R.AddError(Pointer + TEXT("/count"), TEXT("must be at least 1"));
				Objective.Count = 1;
			}
			if ((Objective.Type == EMadQuestObjectiveType::Craft || Objective.Type == EMadQuestObjectiveType::Have)
				&& Objective.Target.IsNone() && Objective.Tag.IsNone())
			{
				R.AddError(Pointer, TEXT("craft and have objectives need a target or a tag"));
				continue;
			}
			Data.Objectives.Add(Objective);
		}

		TSharedPtr<FJsonObject> Rewards;
		if (R.ReadObject(Object, TEXT("rewards"), TEXT("/rewards"), Rewards))
		{
			const TSharedRef<FJsonObject> Rw = Rewards.ToSharedRef();
			R.ReadInt(Rw, TEXT("experience"), TEXT("/rewards/experience"), Data.RewardExperience);
			ReadAmountArray(R, Rw, TEXT("items"), TEXT("/rewards/items"), Data.RewardItems);
			R.ReportUnknownFields(Rw, { TEXT("experience"), TEXT("items") });
		}

		R.ReportUnknownFields(Object, { TEXT("schema"), TEXT("id"), TEXT("display_name"), TEXT("description"), TEXT("requires"),
			TEXT("order"), TEXT("objectives"), TEXT("rewards"), TEXT("mod_data") });

		Data.SourceModId = ModId;
		Data.SourcePath = SourcePath;
		return Data.Objectives.Num() > 0;
	}

	bool ParseTrader(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
		FMadTraderDefinition& Data, TArray<FMadDefinitionError>& OutErrors)
	{
		const FReader R{ SourcePath, OutErrors };
		if (!ReadHeader(R, Object, MadFall::TraderSchemaV1, ModId, /*bPathId*/ false, Data.Id))
		{
			return false;
		}

		R.ReadString(Object, TEXT("display_name"), TEXT("/display_name"), Data.DisplayName);
		R.ReadString(Object, TEXT("greeting"), TEXT("/greeting"), Data.Greeting);
		R.ReadName(Object, TEXT("currency"), TEXT("/currency"), Data.Currency);
		R.ReadFloat(Object, TEXT("buy_factor"), TEXT("/buy_factor"), Data.BuyFactor);
		R.ReadNameArray(Object, TEXT("buys_tags"), TEXT("/buys_tags"), Data.BuysTags);
		R.ReadInt(Object, TEXT("restock_days"), TEXT("/restock_days"), Data.RestockDays);
		if (Data.BuyFactor < 0.0f || Data.BuyFactor > 1.0f)
		{
			// Above 1 a player could buy and sell back at a profit, forever.
			R.AddError(TEXT("/buy_factor"), TEXT("must be between 0 and 1"));
			Data.BuyFactor = FMath::Clamp(Data.BuyFactor, 0.0f, 1.0f);
		}
		Data.RestockDays = FMath::Max(1, Data.RestockDays);

		TArray<TSharedPtr<FJsonValue>> Lines;
		if (R.ReadArray(Object, TEXT("stock"), TEXT("/stock"), Lines))
		{
			for (int32 Index = 0; Index < Lines.Num(); ++Index)
			{
				const FString Pointer = FString::Printf(TEXT("/stock/%d"), Index);
				if (!Lines[Index].IsValid() || Lines[Index]->Type != EJson::Object)
				{
					R.AddError(Pointer, TEXT("expected an object"));
					continue;
				}
				const TSharedRef<FJsonObject> L = Lines[Index]->AsObject().ToSharedRef();
				FMadTraderStockEntry Entry;
				if (!R.ReadName(L, TEXT("item"), Pointer + TEXT("/item"), Entry.Item))
				{
					R.AddError(Pointer + TEXT("/item"), TEXT("missing required field"));
					continue;
				}
				ReadIntRange(R, L, TEXT("count"), Pointer + TEXT("/count"), Entry.CountMin, Entry.CountMax);
				R.ReadInt(L, TEXT("price"), Pointer + TEXT("/price"), Entry.Price);
				R.ReadFloat(L, TEXT("chance"), Pointer + TEXT("/chance"), Entry.Chance);
				R.ReportUnknownFields(L, { TEXT("item"), TEXT("count"), TEXT("price"), TEXT("chance") });
				Entry.CountMin = FMath::Max(1, Entry.CountMin);
				Entry.CountMax = FMath::Max(Entry.CountMin, Entry.CountMax);
				Entry.Price = FMath::Max(0, Entry.Price);
				Entry.Chance = FMath::Clamp(Entry.Chance, 0.0f, 1.0f);
				Data.Stock.Add(Entry);
			}
		}

		R.ReportUnknownFields(Object, { TEXT("schema"), TEXT("id"), TEXT("display_name"), TEXT("greeting"), TEXT("currency"),
			TEXT("stock"), TEXT("buy_factor"), TEXT("buys_tags"), TEXT("restock_days"), TEXT("mod_data") });

		Data.SourceModId = ModId;
		Data.SourcePath = SourcePath;
		return true;
	}

	bool ParseAnimal(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
		FMadAnimalDefinition& Data, TArray<FMadDefinitionError>& OutErrors)
	{
		const FReader R{ SourcePath, OutErrors };

		if (!ReadHeader(R, Object, MadFall::AnimalSchemaV1, ModId, /*bPathId*/ false, Data.Id))
		{
			return false;
		}

		R.ReadString(Object, TEXT("display_name"), TEXT("/display_name"), Data.DisplayName);
		R.ReadNameArray(Object, TEXT("tags"), TEXT("/tags"), Data.Tags);

		FString Behaviour;
		if (R.ReadString(Object, TEXT("behaviour"), TEXT("/behaviour"), Behaviour))
		{
			if (Behaviour == TEXT("skittish"))        { Data.Behaviour = EMadAnimalBehaviour::Skittish; }
			else if (Behaviour == TEXT("defensive"))  { Data.Behaviour = EMadAnimalBehaviour::Defensive; }
			else if (Behaviour == TEXT("aggressive")) { Data.Behaviour = EMadAnimalBehaviour::Aggressive; }
			else
			{
				R.AddError(TEXT("/behaviour"), FString::Printf(TEXT("'%s' is not one of skittish, defensive, aggressive"), *Behaviour));
			}
		}

		TSharedPtr<FJsonObject> Section;
		if (R.ReadObject(Object, TEXT("spawn"), TEXT("/spawn"), Section))
		{
			const TSharedRef<FJsonObject> S = Section.ToSharedRef();
			R.ReadNameArray(S, TEXT("biomes"), TEXT("/spawn/biomes"), Data.Biomes);
			R.ReadFloat(S, TEXT("weight"), TEXT("/spawn/weight"), Data.SpawnWeight);
			ReadIntRange(R, S, TEXT("herd"), TEXT("/spawn/herd"), Data.HerdMin, Data.HerdMax);
			FString Active;
			if (R.ReadString(S, TEXT("active"), TEXT("/spawn/active"), Active))
			{
				if (Active == TEXT("always"))     { Data.Activity = EMadAnimalActivity::Always; }
				else if (Active == TEXT("day"))   { Data.Activity = EMadAnimalActivity::Day; }
				else if (Active == TEXT("night")) { Data.Activity = EMadAnimalActivity::Night; }
				else
				{
					R.AddError(TEXT("/spawn/active"), FString::Printf(TEXT("'%s' is not one of always, day, night"), *Active));
				}
			}
			R.ReportUnknownFields(S, { TEXT("biomes"), TEXT("weight"), TEXT("herd"), TEXT("active") });
		}

		if (R.ReadObject(Object, TEXT("stats"), TEXT("/stats"), Section))
		{
			const TSharedRef<FJsonObject> S = Section.ToSharedRef();
			R.ReadFloat(S, TEXT("health"), TEXT("/stats/health"), Data.Health);
			R.ReadFloat(S, TEXT("walk_speed"), TEXT("/stats/walk_speed"), Data.WalkSpeed);
			R.ReadFloat(S, TEXT("run_speed"), TEXT("/stats/run_speed"), Data.RunSpeed);
			R.ReadFloat(S, TEXT("attack_damage"), TEXT("/stats/attack_damage"), Data.AttackDamage);
			R.ReadFloat(S, TEXT("attack_seconds"), TEXT("/stats/attack_seconds"), Data.AttackSeconds);
			R.ReadName(S, TEXT("damage_type"), TEXT("/stats/damage_type"), Data.DamageType);
			R.ReportUnknownFields(S, { TEXT("health"), TEXT("walk_speed"), TEXT("run_speed"), TEXT("attack_damage"),
				TEXT("attack_seconds"), TEXT("damage_type") });
		}

		if (R.ReadObject(Object, TEXT("senses"), TEXT("/senses"), Section))
		{
			const TSharedRef<FJsonObject> S = Section.ToSharedRef();
			R.ReadFloat(S, TEXT("sight"), TEXT("/senses/sight"), Data.SightRange);
			R.ReadFloat(S, TEXT("hearing"), TEXT("/senses/hearing"), Data.HearingRange);
			R.ReadFloat(S, TEXT("flee"), TEXT("/senses/flee"), Data.FleeRange);
			R.ReportUnknownFields(S, { TEXT("sight"), TEXT("hearing"), TEXT("flee") });
		}

		if (R.ReadObject(Object, TEXT("rewards"), TEXT("/rewards"), Section))
		{
			const TSharedRef<FJsonObject> S = Section.ToSharedRef();
			R.ReadInt(S, TEXT("experience"), TEXT("/rewards/experience"), Data.Experience);
			R.ReadName(S, TEXT("loot_table"), TEXT("/rewards/loot_table"), Data.LootTable);
			R.ReportUnknownFields(S, { TEXT("experience"), TEXT("loot_table") });
		}

		if (R.ReadObject(Object, TEXT("appearance"), TEXT("/appearance"), Section))
		{
			const TSharedRef<FJsonObject> S = Section.ToSharedRef();
			TArray<TSharedPtr<FJsonValue>> Body;
			if (R.ReadArray(S, TEXT("body"), TEXT("/appearance/body"), Body))
			{
				if (Body.Num() == 3 && Body[0]->Type == EJson::Number && Body[1]->Type == EJson::Number && Body[2]->Type == EJson::Number
					&& Body[0]->AsNumber() > 0.0 && Body[1]->AsNumber() > 0.0 && Body[2]->AsNumber() > 0.0)
				{
					Data.BodySize = FVector(Body[0]->AsNumber(), Body[1]->AsNumber(), Body[2]->AsNumber());
				}
				else
				{
					R.AddError(TEXT("/appearance/body"), TEXT("expected [length, width, height] in centimetres, all above 0"));
				}
			}
			R.ReadFloat(S, TEXT("legs"), TEXT("/appearance/legs"), Data.LegLength);
			R.ReadFloat(S, TEXT("neck"), TEXT("/appearance/neck"), Data.NeckLength);
			R.ReadFloat(S, TEXT("scale"), TEXT("/appearance/scale"), Data.Scale);
			ReadColour(R, S, TEXT("tint"), TEXT("/appearance/tint"), Data.Tint);
			R.ReportUnknownFields(S, { TEXT("body"), TEXT("legs"), TEXT("neck"), TEXT("scale"), TEXT("tint") });
		}

		R.ReportUnknownFields(Object, { TEXT("schema"), TEXT("id"), TEXT("display_name"), TEXT("tags"), TEXT("behaviour"),
			TEXT("spawn"), TEXT("stats"), TEXT("senses"), TEXT("rewards"), TEXT("appearance"), TEXT("mod_data") });

		if (Data.Health <= 0.0f)
		{
			R.AddError(TEXT("/stats/health"), TEXT("must be above 0"));
			Data.Health = 1.0f;
		}
		if (Data.HerdMin < 1 || Data.HerdMax < Data.HerdMin)
		{
			R.AddError(TEXT("/spawn/herd"), TEXT("herd must be at least 1 with min <= max"));
			Data.HerdMin = FMath::Max(1, Data.HerdMin);
			Data.HerdMax = FMath::Max(Data.HerdMin, Data.HerdMax);
		}
		Data.LegLength = FMath::Max(4.0f, Data.LegLength);
		Data.NeckLength = FMath::Max(0.0f, Data.NeckLength);
		Data.Scale = FMath::Clamp(Data.Scale, 0.2f, 4.0f);

		Data.SourceModId = ModId;
		Data.SourcePath = SourcePath;
		return true;
	}
}

// ===========================================================================
// Registry
// ===========================================================================

void FMadGameplayDefinitions::BeginLoad()
{
	PendingItems.Reset();
	PendingItemIndex.Reset();
	PendingRecipes.Reset();
	PendingLoot.Reset();
	PendingZombies.Reset();
	PendingAnimals.Reset();
	PendingQuests.Reset();
	RecipeSources.Reset();
	LootSources.Reset();
	ZombieSources.Reset();
	AnimalSources.Reset();
	QuestSources.Reset();
	PendingTuning.Reset();
	TuningSources.Reset();
	PendingPerks.Reset();
	PerkSources.Reset();
	PendingTraders.Reset();
	TraderSources.Reset();
	Tunings.Reset();
	Perks.Reset();
	Traders.Reset();
	TuningIndex.Reset();
	PerkIndex.Reset();
	TraderIndex.Reset();
	Items.Reset();
	Recipes.Reset();
	LootTables.Reset();
	Zombies.Reset();
	Animals.Reset();
	Quests.Reset();
	ItemIndex.Reset();
	RecipeIndex.Reset();
	LootIndex.Reset();
	ZombieIndex.Reset();
	AnimalIndex.Reset();
	QuestIndex.Reset();
	BlockToItem.Reset();
}

bool FMadGameplayDefinitions::AddZombieJson(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
	TArray<FMadDefinitionError>& OutErrors)
{
	FMadZombieDefinition Zombie;
	if (!MadFall::GameplayDefinitionsJson::ParseZombie(Object, SourcePath, ModId, Zombie, OutErrors))
	{
		return false;
	}
	if (const FMadZombieDefinition* Existing = PendingZombies.Find(Zombie.Id))
	{
		UE_LOG(LogMadFallRegistry, Warning, TEXT("Zombie '%s' is defined more than once; '%s' overrides '%s'."),
			*Zombie.Id.ToString(), *SourcePath, *Existing->SourcePath);
	}
	ZombieSources.Add(Zombie.Id, FPendingItem{ Zombie.Id, NAME_None, ModId, SourcePath, Object });
	PendingZombies.Add(Zombie.Id, MoveTemp(Zombie));
	return true;
}

int32 FMadGameplayDefinitions::AddZombiesFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	int32 Staged = 0;
	MadFall::Definitions::ForEachJsonObject(Directory, OutErrors,
		[&](const FString& File, const TSharedRef<FJsonObject>& Object) { Staged += AddZombieJson(Object, File, ModId, OutErrors) ? 1 : 0; });
	return Staged;
}

int32 MadFall::Wear::GetSlotIndex(FName Slot)
{
	static const FName Names[NumSlots] = { FName(TEXT("head")), FName(TEXT("body")), FName(TEXT("legs")), FName(TEXT("feet")) };
	for (int32 Index = 0; Index < NumSlots; ++Index)
	{
		if (Slot == Names[Index])
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

FName MadFall::Wear::GetSlotName(int32 Index)
{
	static const FName Names[NumSlots] = { FName(TEXT("head")), FName(TEXT("body")), FName(TEXT("legs")), FName(TEXT("feet")) };
	return Index >= 0 && Index < NumSlots ? Names[Index] : NAME_None;
}

bool FMadGameplayDefinitions::AddAnimalJson(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
	TArray<FMadDefinitionError>& OutErrors)
{
	FMadAnimalDefinition Animal;
	if (!MadFall::GameplayDefinitionsJson::ParseAnimal(Object, SourcePath, ModId, Animal, OutErrors))
	{
		return false;
	}
	if (const FMadAnimalDefinition* Existing = PendingAnimals.Find(Animal.Id))
	{
		UE_LOG(LogMadFallRegistry, Warning, TEXT("Animal '%s' is defined more than once; '%s' overrides '%s'."),
			*Animal.Id.ToString(), *SourcePath, *Existing->SourcePath);
	}
	AnimalSources.Add(Animal.Id, FPendingItem{ Animal.Id, NAME_None, ModId, SourcePath, Object });
	PendingAnimals.Add(Animal.Id, MoveTemp(Animal));
	return true;
}

int32 FMadGameplayDefinitions::AddAnimalsFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	int32 Staged = 0;
	MadFall::Definitions::ForEachJsonObject(Directory, OutErrors,
		[&](const FString& File, const TSharedRef<FJsonObject>& Object) { Staged += AddAnimalJson(Object, File, ModId, OutErrors) ? 1 : 0; });
	return Staged;
}

bool FMadGameplayDefinitions::AddQuestJson(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
	TArray<FMadDefinitionError>& OutErrors)
{
	FMadQuestDefinition Quest;
	if (!MadFall::GameplayDefinitionsJson::ParseQuest(Object, SourcePath, ModId, Quest, OutErrors))
	{
		return false;
	}
	if (const FMadQuestDefinition* Existing = PendingQuests.Find(Quest.Id))
	{
		UE_LOG(LogMadFallRegistry, Warning, TEXT("Quest '%s' is defined more than once; '%s' overrides '%s'."),
			*Quest.Id.ToString(), *SourcePath, *Existing->SourcePath);
	}
	QuestSources.Add(Quest.Id, FPendingItem{ Quest.Id, NAME_None, ModId, SourcePath, Object });
	PendingQuests.Add(Quest.Id, MoveTemp(Quest));
	return true;
}

bool FMadGameplayDefinitions::AddTraderJson(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
	TArray<FMadDefinitionError>& OutErrors)
{
	FMadTraderDefinition Trader;
	if (!MadFall::GameplayDefinitionsJson::ParseTrader(Object, SourcePath, ModId, Trader, OutErrors))
	{
		return false;
	}
	if (const FMadTraderDefinition* Existing = PendingTraders.Find(Trader.Id))
	{
		UE_LOG(LogMadFallRegistry, Warning, TEXT("Trader '%s' is defined more than once; '%s' overrides '%s'."),
			*Trader.Id.ToString(), *SourcePath, *Existing->SourcePath);
	}
	TraderSources.Add(Trader.Id, FPendingItem{ Trader.Id, NAME_None, ModId, SourcePath, Object });
	PendingTraders.Add(Trader.Id, MoveTemp(Trader));
	return true;
}

int32 FMadGameplayDefinitions::AddTradersFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	int32 Staged = 0;
	MadFall::Definitions::ForEachJsonObject(Directory, OutErrors,
		[&](const FString& File, const TSharedRef<FJsonObject>& Object) { Staged += AddTraderJson(Object, File, ModId, OutErrors) ? 1 : 0; });
	return Staged;
}

const FMadTraderDefinition* FMadGameplayDefinitions::FindTrader(FName Id) const
{
	const int32* Index = TraderIndex.Find(Id);
	return Index ? &Traders[*Index] : nullptr;
}

int32 FMadGameplayDefinitions::GetTraderPrice(const FMadTraderDefinition& Trader, FName ItemId) const
{
	const FMadItemDefinition* Item = FindItem(ItemId);
	if (Item == nullptr || ItemId == Trader.Currency)
	{
		return 0;
	}
	for (const FMadTraderStockEntry& Entry : Trader.Stock)
	{
		if (Entry.Item == ItemId && Entry.Price > 0)
		{
			return Entry.Price;
		}
	}
	return Item->Value;
}

int32 FMadGameplayDefinitions::GetTraderOffer(const FMadTraderDefinition& Trader, FName ItemId) const
{
	const FMadItemDefinition* Item = FindItem(ItemId);
	if (Item == nullptr || ItemId == Trader.Currency || Item->Value <= 0)
	{
		return 0;
	}
	if (Trader.BuysTags.Num() > 0 && !Trader.BuysTags.ContainsByPredicate([Item](FName Tag) { return Item->Tags.Contains(Tag); }))
	{
		return 0;
	}
	// Never 0 for something with a value: a trader who takes a stack for nothing is a bug report.
	return FMath::Max(1, FMath::FloorToInt32(Item->Value * Trader.BuyFactor));
}

int32 FMadGameplayDefinitions::AddQuestsFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	int32 Staged = 0;
	MadFall::Definitions::ForEachJsonObject(Directory, OutErrors,
		[&](const FString& File, const TSharedRef<FJsonObject>& Object) { Staged += AddQuestJson(Object, File, ModId, OutErrors) ? 1 : 0; });
	return Staged;
}

const FMadQuestDefinition* FMadGameplayDefinitions::FindQuest(FName Id) const
{
	const int32* Index = QuestIndex.Find(Id);
	return Index ? &Quests[*Index] : nullptr;
}

const FMadAnimalDefinition* FMadGameplayDefinitions::FindAnimal(FName Id) const
{
	const int32* Index = AnimalIndex.Find(Id);
	return Index ? &Animals[*Index] : nullptr;
}

const FMadZombieDefinition* FMadGameplayDefinitions::FindZombie(FName Id) const
{
	const int32* Index = ZombieIndex.Find(Id);
	return Index ? &Zombies[*Index] : nullptr;
}

void FMadGameplayDefinitions::GetZombiesInGroup(FName Group, int32 GameStage, TArray<const FMadZombieDefinition*>& OutZombies) const
{
	OutZombies.Reset();
	for (const FMadZombieDefinition& Zombie : Zombies)
	{
		if (Zombie.SpawnGroups.Contains(Group) && GameStage >= Zombie.MinGameStage && Zombie.SpawnWeight > 0.0f)
		{
			OutZombies.Add(&Zombie);
		}
	}
}

bool FMadGameplayDefinitions::AddItemJson(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
	TArray<FMadDefinitionError>& OutErrors)
{
	FMadItemDefinition Probe;
	if (!MadFall::GameplayDefinitionsJson::ParseItem(Object, SourcePath, ModId, Probe, OutErrors))
	{
		return false;
	}

	FPendingItem Pending{ Probe.Id, Probe.Extends, ModId, SourcePath, Object };
	if (const int32* Existing = PendingItemIndex.Find(Probe.Id))
	{
		UE_LOG(LogMadFallRegistry, Warning, TEXT("Item '%s' is defined more than once; '%s' overrides '%s'."),
			*Probe.Id.ToString(), *SourcePath, *PendingItems[*Existing].SourcePath);
		PendingItems[*Existing] = MoveTemp(Pending);
	}
	else
	{
		PendingItemIndex.Add(Probe.Id, PendingItems.Num());
		PendingItems.Add(MoveTemp(Pending));
	}
	return true;
}

bool FMadGameplayDefinitions::AddRecipeJson(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
	TArray<FMadDefinitionError>& OutErrors)
{
	FMadRecipeDefinition Recipe;
	if (!MadFall::GameplayDefinitionsJson::ParseRecipe(Object, SourcePath, ModId, Recipe, OutErrors))
	{
		return false;
	}
	if (const FMadRecipeDefinition* Existing = PendingRecipes.Find(Recipe.Id))
	{
		UE_LOG(LogMadFallRegistry, Warning, TEXT("Recipe '%s' is defined more than once; '%s' overrides '%s'."),
			*Recipe.Id.ToString(), *SourcePath, *Existing->SourcePath);
	}
	RecipeSources.Add(Recipe.Id, FPendingItem{ Recipe.Id, NAME_None, ModId, SourcePath, Object });
	PendingRecipes.Add(Recipe.Id, MoveTemp(Recipe));
	return true;
}

bool FMadGameplayDefinitions::AddLootJson(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
	TArray<FMadDefinitionError>& OutErrors)
{
	FMadLootTableDefinition Table;
	if (!MadFall::GameplayDefinitionsJson::ParseLootTable(Object, SourcePath, ModId, Table, OutErrors))
	{
		return false;
	}
	if (const FMadLootTableDefinition* Existing = PendingLoot.Find(Table.Id))
	{
		UE_LOG(LogMadFallRegistry, Warning, TEXT("Loot table '%s' is defined more than once; '%s' overrides '%s'."),
			*Table.Id.ToString(), *SourcePath, *Existing->SourcePath);
	}
	LootSources.Add(Table.Id, FPendingItem{ Table.Id, NAME_None, ModId, SourcePath, Object });
	PendingLoot.Add(Table.Id, MoveTemp(Table));
	return true;
}

int32 FMadGameplayDefinitions::AddItemsFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	int32 Staged = 0;
	MadFall::Definitions::ForEachJsonObject(Directory, OutErrors,
		[&](const FString& File, const TSharedRef<FJsonObject>& Object) { Staged += AddItemJson(Object, File, ModId, OutErrors) ? 1 : 0; });
	return Staged;
}

int32 FMadGameplayDefinitions::AddRecipesFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	int32 Staged = 0;
	MadFall::Definitions::ForEachJsonObject(Directory, OutErrors,
		[&](const FString& File, const TSharedRef<FJsonObject>& Object) { Staged += AddRecipeJson(Object, File, ModId, OutErrors) ? 1 : 0; });
	return Staged;
}

int32 FMadGameplayDefinitions::AddLootFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	int32 Staged = 0;
	MadFall::Definitions::ForEachJsonObject(Directory, OutErrors,
		[&](const FString& File, const TSharedRef<FJsonObject>& Object) { Staged += AddLootJson(Object, File, ModId, OutErrors) ? 1 : 0; });
	return Staged;
}

bool FMadGameplayDefinitions::ResolveItem(FName Id, TSet<FName>& Visiting, TMap<FName, FMadItemDefinition>& Resolved,
	TArray<FMadDefinitionError>& OutErrors)
{
	if (Resolved.Contains(Id))
	{
		return true;
	}

	const int32* PendingIndex = PendingItemIndex.Find(Id);
	if (PendingIndex == nullptr)
	{
		return false;
	}
	const FPendingItem& Def = PendingItems[*PendingIndex];

	if (Visiting.Contains(Id))
	{
		OutErrors.Add(FMadDefinitionError{ Def.SourcePath, TEXT("/extends"),
			FString::Printf(TEXT("'%s' is part of an inheritance cycle"), *Id.ToString()) });
		return false;
	}

	Visiting.Add(Id);
	FMadItemDefinition Data;

	if (!Def.Extends.IsNone())
	{
		if (!ResolveItem(Def.Extends, Visiting, Resolved, OutErrors))
		{
			OutErrors.Add(FMadDefinitionError{ Def.SourcePath, TEXT("/extends"),
				FString::Printf(TEXT("parent item '%s' does not exist or failed to load"), *Def.Extends.ToString()) });
			Visiting.Remove(Id);
			return false;
		}
		Data = Resolved[Def.Extends];
		Data.Extends = NAME_None;
	}

	// Parsing again over the resolved parent is the whole of inheritance: see
	// the contract on FMadJsonReader. Errors were already reported at staging.
	TArray<FMadDefinitionError> Discard;
	MadFall::GameplayDefinitionsJson::ParseItem(Def.Json.ToSharedRef(), Def.SourcePath, Def.ModId, Data, Discard);

	Visiting.Remove(Id);
	Resolved.Add(Id, MoveTemp(Data));
	return true;
}

void FMadGameplayDefinitions::FinishLoad(const FMadBlockRegistry* Blocks, TArray<FMadDefinitionError>& OutErrors)
{
	// --- items ---------------------------------------------------------------
	TMap<FName, FMadItemDefinition> Resolved;
	{
		TSet<FName> Visiting;
		for (const FPendingItem& Def : PendingItems)
		{
			ResolveItem(Def.Id, Visiting, Resolved, OutErrors);
		}
	}

	if (Blocks != nullptr)
	{
		for (const FMadBlockEntry& Entry : Blocks->GetEntries())
		{
			const FMadBlockDefinitionData& Block = Entry.Definition;
			if (Entry.RuntimeId == MadFall::BlockTypeAir || Entry.bUnresolved || Block.bLiquid
				|| Block.Tags.Contains(TagIndestructible))
			{
				continue;
			}

			if (FMadItemDefinition* Explicit = Resolved.Find(Block.Id))
			{
				// An item file with a block's id customises that block's item.
				if (Explicit->PlacesBlock.IsNone())
				{
					Explicit->PlacesBlock = Block.Id;
				}
				continue;
			}

			FMadItemDefinition Item;
			Item.Id = Block.Id;
			Item.DisplayName = Block.DisplayName;
			Item.Tags = Block.Tags;
			Item.Tags.AddUnique(TagItemBlock);
			Item.Kind = EMadItemKind::Block;
			Item.MaxStack = 100;
			Item.WeightKg = 1.0f;
			Item.PlacesBlock = Block.Id;
			Item.bAutoGenerated = true;
			Item.SourceModId = Block.SourceModId;
			Item.SourcePath = Block.SourcePath;
			Resolved.Add(Item.Id, MoveTemp(Item));
		}
	}

	TArray<FName> ItemIds;
	Resolved.GenerateKeyArray(ItemIds);
	ItemIds.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

	for (const FName& Id : ItemIds)
	{
		FMadItemDefinition& Item = Resolved[Id];

		if (Item.Kind == EMadItemKind::Resource)
		{
			if (!Item.PlacesBlock.IsNone()) { Item.Kind = EMadItemKind::Block; }
			else if (Item.bHasTool)         { Item.Kind = EMadItemKind::Tool; }
			else if (Item.bHasConsumable)   { Item.Kind = EMadItemKind::Consumable; }
			else if (Item.bHasMod)          { Item.Kind = EMadItemKind::Mod; }
		}

		if (!Item.PlacesBlock.IsNone() && Blocks != nullptr && !Blocks->IsRegistered(Item.PlacesBlock))
		{
			OutErrors.Add(FMadDefinitionError{ Item.SourcePath, TEXT("/places_block"),
				FString::Printf(TEXT("block '%s' is not registered; the item will not be placeable"), *Item.PlacesBlock.ToString()) });
			Item.PlacesBlock = NAME_None;
		}

		const bool bEquipment = Item.Kind == EMadItemKind::Tool || Item.Kind == EMadItemKind::Weapon;
		if (bEquipment && Item.MaxStack != 1)
		{
			// Durability is per item, so a stack of tools would share one.
			if (Item.MaxStack > 1 && Item.bMaxStackExplicit)
			{
				OutErrors.Add(FMadDefinitionError{ Item.SourcePath, TEXT("/max_stack"),
					TEXT("tools and weapons carry their own durability and cannot stack; max_stack forced to 1") });
			}
			Item.MaxStack = 1;
		}
		Item.MaxStack = FMath::Max(1, Item.MaxStack);

		ItemIndex.Add(Id, Items.Num());
		if (!Item.PlacesBlock.IsNone() && !BlockToItem.Contains(Item.PlacesBlock))
		{
			BlockToItem.Add(Item.PlacesBlock, Items.Num());
		}
		Items.Add(MoveTemp(Item));
	}

	// Repair costs name items, so they are checked once every item exists.
	for (FMadItemDefinition& Item : Items)
	{
		Item.Tool.RepairWith.RemoveAll([&](const FMadItemAmount& Amount)
		{
			if (ItemIndex.Contains(Amount.Item)) { return false; }
			OutErrors.Add(FMadDefinitionError{ Item.SourcePath, TEXT("/tool/repair_with"),
				FString::Printf(TEXT("unknown item '%s'; entry removed"), *Amount.Item.ToString()) });
			return true;
		});
	}

	// --- recipes -------------------------------------------------------------
	TArray<FName> RecipeIds;
	PendingRecipes.GenerateKeyArray(RecipeIds);
	RecipeIds.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

	for (const FName& Id : RecipeIds)
	{
		const FMadRecipeDefinition& Recipe = PendingRecipes[Id];

		bool bValid = true;
		auto Check = [&](const FMadItemAmount& Amount, const TCHAR* Pointer)
		{
			if (!ItemIndex.Contains(Amount.Item))
			{
				OutErrors.Add(FMadDefinitionError{ Recipe.SourcePath, Pointer,
					FString::Printf(TEXT("unknown item '%s'; recipe '%s' is disabled"), *Amount.Item.ToString(), *Id.ToString()) });
				bValid = false;
			}
		};

		Check(Recipe.Output, TEXT("/output/item"));
		for (const FMadItemAmount& Ingredient : Recipe.Ingredients)
		{
			Check(Ingredient, TEXT("/ingredients"));
		}

		if (bValid)
		{
			RecipeIndex.Add(Id, Recipes.Num());
			Recipes.Add(Recipe);
		}
	}

	// --- loot ----------------------------------------------------------------
	TArray<FName> LootIds;
	PendingLoot.GenerateKeyArray(LootIds);
	LootIds.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

	for (const FName& Id : LootIds)
	{
		FMadLootTableDefinition Table = PendingLoot[Id];
		Table.Entries.RemoveAll([&](const FMadLootEntry& Entry)
		{
			const bool bKnown = Entry.Item.IsNone() ? PendingLoot.Contains(Entry.Table) : ItemIndex.Contains(Entry.Item);
			if (!bKnown)
			{
				OutErrors.Add(FMadDefinitionError{ Table.SourcePath, TEXT("/entries"),
					FString::Printf(TEXT("unknown %s '%s'; entry removed"),
						Entry.Item.IsNone() ? TEXT("loot table") : TEXT("item"),
						*(Entry.Item.IsNone() ? Entry.Table : Entry.Item).ToString()) });
			}
			return !bKnown;
		});

		LootIndex.Add(Id, LootTables.Num());
		LootTables.Add(MoveTemp(Table));
	}

	// Nested-table cycles: rolling would recurse forever. Break them at load,
	// naming the table, rather than relying on a depth cap at roll time.
	{
		enum class EMark : uint8 { None, Visiting, Done };
		TArray<EMark> Marks;
		Marks.Init(EMark::None, LootTables.Num());

		TFunction<void(int32)> Visit = [&](int32 Index)
		{
			Marks[Index] = EMark::Visiting;
			FMadLootTableDefinition& Table = LootTables[Index];
			for (int32 E = Table.Entries.Num() - 1; E >= 0; --E)
			{
				if (Table.Entries[E].Table.IsNone())
				{
					continue;
				}
				const int32 Child = LootIndex[Table.Entries[E].Table];
				if (Marks[Child] == EMark::Visiting)
				{
					OutErrors.Add(FMadDefinitionError{ Table.SourcePath, TEXT("/entries"),
						FString::Printf(TEXT("nesting '%s' creates a cycle; entry removed"), *Table.Entries[E].Table.ToString()) });
					Table.Entries.RemoveAt(E);
				}
				else if (Marks[Child] == EMark::None)
				{
					Visit(Child);
				}
			}
			Marks[Index] = EMark::Done;
		};

		for (int32 Index = 0; Index < LootTables.Num(); ++Index)
		{
			if (Marks[Index] == EMark::None)
			{
				Visit(Index);
			}
		}
	}

	// --- zombies ---------------------------------------------------------------
	TArray<FName> ZombieIds;
	PendingZombies.GenerateKeyArray(ZombieIds);
	ZombieIds.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

	for (const FName& Id : ZombieIds)
	{
		FMadZombieDefinition Zombie = PendingZombies[Id];
		if (!Zombie.LootTable.IsNone() && !LootIndex.Contains(Zombie.LootTable))
		{
			OutErrors.Add(FMadDefinitionError{ Zombie.SourcePath, TEXT("/rewards/loot_table"),
				FString::Printf(TEXT("unknown loot table '%s'; this zombie will drop nothing"), *Zombie.LootTable.ToString()) });
			Zombie.LootTable = NAME_None;
		}
		ZombieIndex.Add(Id, Zombies.Num());
		Zombies.Add(MoveTemp(Zombie));
	}

	// --- animals -----------------------------------------------------------------
	TArray<FName> AnimalIds;
	PendingAnimals.GenerateKeyArray(AnimalIds);
	AnimalIds.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

	for (const FName& Id : AnimalIds)
	{
		FMadAnimalDefinition Animal = PendingAnimals[Id];
		if (!Animal.LootTable.IsNone() && !LootIndex.Contains(Animal.LootTable))
		{
			OutErrors.Add(FMadDefinitionError{ Animal.SourcePath, TEXT("/rewards/loot_table"),
				FString::Printf(TEXT("unknown loot table '%s'; this animal will drop nothing"), *Animal.LootTable.ToString()) });
			Animal.LootTable = NAME_None;
		}
		AnimalIndex.Add(Id, Animals.Num());
		Animals.Add(MoveTemp(Animal));
	}

	// --- quests ------------------------------------------------------------------
	TArray<FName> QuestIds;
	PendingQuests.GenerateKeyArray(QuestIds);
	QuestIds.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	for (const FName& Id : QuestIds)
	{
		FMadQuestDefinition Quest = PendingQuests[Id];
		// A requirement nobody defines would lock the quest forever: say so, and drop it.
		Quest.Requires.RemoveAll([this, &Quest, &OutErrors](const FName& Required)
		{
			if (PendingQuests.Contains(Required))
			{
				return false;
			}
			OutErrors.Add(FMadDefinitionError{ Quest.SourcePath, TEXT("/requires"),
				FString::Printf(TEXT("unknown quest '%s'; the requirement is ignored"), *Required.ToString()) });
			return true;
		});
		for (int32 Index = 0; Index < Quest.Objectives.Num(); ++Index)
		{
			const FMadQuestObjective& Objective = Quest.Objectives[Index];
			const bool bItemTarget = Objective.Type == EMadQuestObjectiveType::Craft || Objective.Type == EMadQuestObjectiveType::Have
				|| Objective.Type == EMadQuestObjectiveType::Wear;
			if (bItemTarget && !Objective.Target.IsNone() && !ItemIndex.Contains(Objective.Target))
			{
				OutErrors.Add(FMadDefinitionError{ Quest.SourcePath, FString::Printf(TEXT("/objectives/%d/target"), Index),
					FString::Printf(TEXT("unknown item '%s'; the objective can never be met"), *Objective.Target.ToString()) });
			}
		}
		for (int32 Index = Quest.RewardItems.Num() - 1; Index >= 0; --Index)
		{
			if (!ItemIndex.Contains(Quest.RewardItems[Index].Item))
			{
				OutErrors.Add(FMadDefinitionError{ Quest.SourcePath, TEXT("/rewards/items"),
					FString::Printf(TEXT("unknown item '%s'; it is not given"), *Quest.RewardItems[Index].Item.ToString()) });
				Quest.RewardItems.RemoveAt(Index);
			}
		}
		QuestIndex.Add(Id, Quests.Num());
		Quests.Add(MoveTemp(Quest));
	}

	// --- tuning and perks --------------------------------------------------------
	auto SortedKeys = [](const auto& Map)
	{
		TArray<FName> Keys;
		Map.GenerateKeyArray(Keys);
		Keys.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
		return Keys;
	};

	for (const FName& Id : SortedKeys(PendingTuning))
	{
		TuningIndex.Add(Id, Tunings.Num());
		Tunings.Add(PendingTuning[Id]);
	}
	for (const FName& Id : SortedKeys(PendingPerks))
	{
		PerkIndex.Add(Id, Perks.Num());
		Perks.Add(PendingPerks[Id]);
	}
	// Prerequisites that cannot be met would lock a rank forever: an unknown
	// perk (its mod not installed) is dropped, a rank beyond what the perk has
	// is lowered to its top rank.
	for (FMadPerkDefinition& Perk : Perks)
	{
		for (int32 RankIndex = 0; RankIndex < Perk.Ranks.Num(); ++RankIndex)
		{
			for (auto It = Perk.Ranks[RankIndex].Requires.CreateIterator(); It; ++It)
			{
				const FString Pointer = FString::Printf(TEXT("/ranks/%d/requires/%s"), RankIndex, *It.Key().ToString());
				const int32* Other = PerkIndex.Find(It.Key());
				if (Other == nullptr)
				{
					OutErrors.Add(FMadDefinitionError{ Perk.SourcePath, Pointer,
						FString::Printf(TEXT("unknown perk '%s'; the requirement is ignored"), *It.Key().ToString()) });
					It.RemoveCurrent();
				}
				else if (It.Value() > Perks[*Other].Ranks.Num())
				{
					OutErrors.Add(FMadDefinitionError{ Perk.SourcePath, Pointer,
						FString::Printf(TEXT("'%s' has only %d rank(s); rank %d is required instead"),
							*It.Key().ToString(), Perks[*Other].Ranks.Num(), Perks[*Other].Ranks.Num()) });
					It.Value() = Perks[*Other].Ranks.Num();
				}
			}
		}
	}
	// Cycles (A needs B, B needs A) lock every perk in them. Break each at the
	// requirement that closes it, naming both perks, rather than at buy time.
	{
		enum class EMark : uint8 { None, Visiting, Done };
		TArray<EMark> Marks;
		Marks.Init(EMark::None, Perks.Num());

		TFunction<void(int32)> Visit = [&](int32 Index)
		{
			Marks[Index] = EMark::Visiting;
			FMadPerkDefinition& Perk = Perks[Index];
			for (int32 RankIndex = 0; RankIndex < Perk.Ranks.Num(); ++RankIndex)
			{
				TArray<FName> Required;
				Perk.Ranks[RankIndex].Requires.GenerateKeyArray(Required);
				Required.Sort(FNameLexicalLess());
				for (const FName& RequiredId : Required)
				{
					const int32 Child = PerkIndex[RequiredId];
					if (Marks[Child] == EMark::Visiting)
					{
						OutErrors.Add(FMadDefinitionError{ Perk.SourcePath,
							FString::Printf(TEXT("/ranks/%d/requires/%s"), RankIndex, *RequiredId.ToString()),
							FString::Printf(TEXT("requiring '%s' creates a cycle; the requirement is ignored"), *RequiredId.ToString()) });
						Perk.Ranks[RankIndex].Requires.Remove(RequiredId);
					}
					else if (Marks[Child] == EMark::None)
					{
						Visit(Child);
					}
				}
			}
			Marks[Index] = EMark::Done;
		};
		for (int32 Index = 0; Index < Perks.Num(); ++Index)
		{
			if (Marks[Index] == EMark::None)
			{
				Visit(Index);
			}
		}
	}
	for (const FName& Id : SortedKeys(PendingTraders))
	{
		FMadTraderDefinition Trader = PendingTraders[Id];
		if (!ItemIndex.Contains(Trader.Currency))
		{
			OutErrors.Add(FMadDefinitionError{ Trader.SourcePath, TEXT("/currency"),
				FString::Printf(TEXT("unknown currency item '%s'; the trader is not loaded"), *Trader.Currency.ToString()) });
			continue;
		}
		for (int32 Index = Trader.Stock.Num() - 1; Index >= 0; --Index)
		{
			const FMadTraderStockEntry& Entry = Trader.Stock[Index];
			const int32* ItemAt = ItemIndex.Find(Entry.Item);
			if (ItemAt == nullptr)
			{
				OutErrors.Add(FMadDefinitionError{ Trader.SourcePath, FString::Printf(TEXT("/stock/%d/item"), Index),
					FString::Printf(TEXT("unknown item '%s'; the line is dropped"), *Entry.Item.ToString()) });
				Trader.Stock.RemoveAt(Index);
			}
			else if (Entry.Price <= 0 && Items[*ItemAt].Value <= 0)
			{
				OutErrors.Add(FMadDefinitionError{ Trader.SourcePath, FString::Printf(TEXT("/stock/%d/price"), Index),
					FString::Printf(TEXT("'%s' has no value and the line names no price; the line is dropped"), *Entry.Item.ToString()) });
				Trader.Stock.RemoveAt(Index);
			}
		}
		TraderIndex.Add(Id, Traders.Num());
		Traders.Add(MoveTemp(Trader));
	}

	PendingItems.Reset();
	PendingItemIndex.Reset();
	PendingRecipes.Reset();
	PendingLoot.Reset();
	PendingZombies.Reset();
	PendingAnimals.Reset();
	PendingQuests.Reset();
	PendingTuning.Reset();
	TuningSources.Reset();
	PendingPerks.Reset();
	PerkSources.Reset();
	PendingTraders.Reset();
	TraderSources.Reset();
	RecipeSources.Reset();
	LootSources.Reset();
	ZombieSources.Reset();
	AnimalSources.Reset();
	QuestSources.Reset();
}

const FMadItemDefinition* FMadGameplayDefinitions::FindItem(FName Id) const
{
	const int32* Index = ItemIndex.Find(Id);
	return Index ? &Items[*Index] : nullptr;
}

const FMadRecipeDefinition* FMadGameplayDefinitions::FindRecipe(FName Id) const
{
	const int32* Index = RecipeIndex.Find(Id);
	return Index ? &Recipes[*Index] : nullptr;
}

const FMadLootTableDefinition* FMadGameplayDefinitions::FindLootTable(FName Id) const
{
	const int32* Index = LootIndex.Find(Id);
	return Index ? &LootTables[*Index] : nullptr;
}

const FMadItemDefinition* FMadGameplayDefinitions::FindItemForBlock(FName BlockId) const
{
	const int32* Index = BlockToItem.Find(BlockId);
	return Index ? &Items[*Index] : nullptr;
}

void FMadGameplayDefinitions::FindRecipesFor(FName ItemId, TArray<const FMadRecipeDefinition*>& OutRecipes) const
{
	OutRecipes.Reset();
	for (const FMadRecipeDefinition& Recipe : Recipes)
	{
		if (Recipe.Output.Item == ItemId)
		{
			OutRecipes.Add(&Recipe);
		}
	}
}

FString FMadGameplayDefinitions::DescribeContents() const
{
	TStringBuilder<4096> Out;
	int32 Generated = 0;
	for (const FMadItemDefinition& Item : Items)
	{
		Generated += Item.bAutoGenerated ? 1 : 0;
	}

	Out.Appendf(TEXT("%d items (%d generated from blocks), %d recipes, %d loot tables\n"),
		Items.Num(), Generated, Recipes.Num(), LootTables.Num());

	for (const FMadItemDefinition& Item : Items)
	{
		if (Item.bAutoGenerated)
		{
			continue;
		}
		Out.Appendf(TEXT("  item   %-32s %-10s stack %3d"), *Item.Id.ToString(), KindName(Item.Kind), Item.MaxStack);
		if (Item.bHasTool)
		{
			Out.Appendf(TEXT("  tier %d, durability %d"), Item.Tool.Tier, Item.Tool.Durability);
		}
		Out.Append(TEXT("\n"));
	}
	for (const FMadRecipeDefinition& Recipe : Recipes)
	{
		Out.Appendf(TEXT("  recipe %-32s -> %d x %s (%d ingredient(s)%s%s)\n"), *Recipe.Id.ToString(),
			Recipe.Output.Count, *Recipe.Output.Item.ToString(), Recipe.Ingredients.Num(),
			Recipe.Station.IsNone() ? TEXT("") : TEXT(", at "),
			Recipe.Station.IsNone() ? TEXT("") : *Recipe.Station.ToString());
	}
	for (const FMadLootTableDefinition& Table : LootTables)
	{
		Out.Appendf(TEXT("  loot   %-32s %d entries, rolls %d-%d\n"), *Table.Id.ToString(), Table.Entries.Num(), Table.RollsMin, Table.RollsMax);
	}
	for (const FMadZombieDefinition& Zombie : Zombies)
	{
		Out.Appendf(TEXT("  zombie %-32s %.0f hp, %.1f/%.1f m/s, %.0f dmg, %.0f block dmg, %d group(s), stage %d+\n"),
			*Zombie.Id.ToString(), Zombie.Health, Zombie.WalkSpeed, Zombie.RunSpeed, Zombie.AttackDamage, Zombie.BlockDamage,
			Zombie.SpawnGroups.Num(), Zombie.MinGameStage);
	}
	for (const FMadAnimalDefinition& Animal : Animals)
	{
		static const TCHAR* Behaviours[] = { TEXT("skittish"), TEXT("defensive"), TEXT("aggressive") };
		Out.Appendf(TEXT("  animal %-32s %.0f hp, %.1f/%.1f m/s, %s, %d biome(s)\n"), *Animal.Id.ToString(), Animal.Health,
			Animal.WalkSpeed, Animal.RunSpeed, Behaviours[static_cast<int32>(Animal.Behaviour)], Animal.Biomes.Num());
	}
	return FString(Out.ToString());
}

// ===========================================================================
// Global
// ===========================================================================

namespace
{
	TUniquePtr<FMadGameplayDefinitions> GGameplayDefinitions;
}

FMadGameplayDefinitions& MadFall::GetGameplayDefinitions()
{
	if (!GGameplayDefinitions.IsValid())
	{
		GGameplayDefinitions = MakeUnique<FMadGameplayDefinitions>();
		GGameplayDefinitions->BeginLoad();

		TArray<FMadDefinitionError> Errors;
		int32 Staged = 0;

		MadFall::Definitions::ForEachSource(TEXT("items"), [&](const FString& Dir, FName ModId)
		{
			Staged += GGameplayDefinitions->AddItemsFromDirectory(Dir, ModId, Errors);
		});
		MadFall::Definitions::ForEachSource(TEXT("recipes"), [&](const FString& Dir, FName ModId)
		{
			Staged += GGameplayDefinitions->AddRecipesFromDirectory(Dir, ModId, Errors);
		});
		MadFall::Definitions::ForEachSource(TEXT("loot"), [&](const FString& Dir, FName ModId)
		{
			Staged += GGameplayDefinitions->AddLootFromDirectory(Dir, ModId, Errors);
		});
		MadFall::Definitions::ForEachSource(TEXT("zombies"), [&](const FString& Dir, FName ModId)
		{
			Staged += GGameplayDefinitions->AddZombiesFromDirectory(Dir, ModId, Errors);
		});
		MadFall::Definitions::ForEachSource(TEXT("animals"), [&](const FString& Dir, FName ModId)
		{
			Staged += GGameplayDefinitions->AddAnimalsFromDirectory(Dir, ModId, Errors);
		});
		MadFall::Definitions::ForEachSource(TEXT("quests"), [&](const FString& Dir, FName ModId)
		{
			Staged += GGameplayDefinitions->AddQuestsFromDirectory(Dir, ModId, Errors);
		});
		MadFall::Definitions::ForEachSource(TEXT("tuning"), [&](const FString& Dir, FName ModId)
		{
			Staged += GGameplayDefinitions->AddTuningFromDirectory(Dir, ModId, Errors);
		});
		MadFall::Definitions::ForEachSource(TEXT("perks"), [&](const FString& Dir, FName ModId)
		{
			Staged += GGameplayDefinitions->AddPerksFromDirectory(Dir, ModId, Errors);
		});
		MadFall::Definitions::ForEachSource(TEXT("traders"), [&](const FString& Dir, FName ModId)
		{
			Staged += GGameplayDefinitions->AddTradersFromDirectory(Dir, ModId, Errors);
		});

		GGameplayDefinitions->ApplyPatches(MadFall::GetPatchSet(), Errors);
		GGameplayDefinitions->FinishLoad(&UMadVoxelWorldSubsystem::GetBlockRegistry(), Errors);

		UE_LOG(LogMadFallRegistry, Log, TEXT("Gameplay definitions: %d staged; %d items, %d recipes, %d loot tables; %d message(s)."),
			Staged, GGameplayDefinitions->GetItems().Num(), GGameplayDefinitions->GetRecipes().Num(),
			GGameplayDefinitions->GetLootTables().Num(), Errors.Num());

		for (const FMadDefinitionError& Error : Errors)
		{
			UE_LOG(LogMadFallRegistry, Warning, TEXT("%s"), *Error.ToString());
		}
	}
	return *GGameplayDefinitions;
}

// ===========================================================================
// Patches
// ===========================================================================

void FMadGameplayDefinitions::ApplyPatches(const FMadPatchSet& Patches, TArray<FMadDefinitionError>& OutErrors)
{
	// Items keep their JSON for inheritance anyway. A probe parse after patching
	// surfaces errors a patch introduced, which the later re-parse discards.
	{
		static const FName Kind(TEXT("item"));
		TSet<FName> Known;
		for (FPendingItem& Pending : PendingItems)
		{
			Known.Add(Pending.Id);
			if (Patches.ApplyTo(Kind, Pending.Id, Pending.Json.ToSharedRef(), OutErrors) > 0)
			{
				FMadItemDefinition Probe;
				MadFall::GameplayDefinitionsJson::ParseItem(Pending.Json.ToSharedRef(), Pending.SourcePath, Pending.ModId, Probe, OutErrors);
			}
		}
		Patches.ReportUnmatched(Kind, Known, OutErrors);
	}

	// Recipes, loot and zombies were parsed at staging; re-parse the patched JSON.
	auto PatchParsed = [&Patches, &OutErrors](const TCHAR* KindText, auto& Sources, auto& Parsed, auto Parse)
	{
		using DataType = std::remove_reference_t<decltype(*Parsed.Find(NAME_None))>;
		const FName Kind(KindText);
		TSet<FName> Known;
		for (TPair<FName, FPendingItem>& Pair : Sources)
		{
			Known.Add(Pair.Key);
			if (Patches.ApplyTo(Kind, Pair.Key, Pair.Value.Json.ToSharedRef(), OutErrors) == 0)
			{
				continue;
			}
			DataType Data;
			if (Parse(Pair.Value.Json.ToSharedRef(), Pair.Value.SourcePath, Pair.Value.ModId, Data, OutErrors))
			{
				Parsed.Add(Pair.Key, MoveTemp(Data));
			}
			else
			{
				// The patch broke the definition. Dropping it is loud (the parse
				// errors above) and safer than loading a half-valid recipe.
				Parsed.Remove(Pair.Key);
			}
		}
		Patches.ReportUnmatched(Kind, Known, OutErrors);
	};

	PatchParsed(TEXT("recipe"), RecipeSources, PendingRecipes, &MadFall::GameplayDefinitionsJson::ParseRecipe);
	PatchParsed(TEXT("loot"), LootSources, PendingLoot, &MadFall::GameplayDefinitionsJson::ParseLootTable);
	PatchParsed(TEXT("zombie"), ZombieSources, PendingZombies, &MadFall::GameplayDefinitionsJson::ParseZombie);
	PatchParsed(TEXT("animal"), AnimalSources, PendingAnimals, &MadFall::GameplayDefinitionsJson::ParseAnimal);
	PatchParsed(TEXT("quest"), QuestSources, PendingQuests, &MadFall::GameplayDefinitionsJson::ParseQuest);
	PatchParsed(TEXT("tuning"), TuningSources, PendingTuning, &MadFall::GameplayDefinitionsJson::ParseTuning);
	PatchParsed(TEXT("perk"), PerkSources, PendingPerks, &MadFall::GameplayDefinitionsJson::ParsePerk);
	PatchParsed(TEXT("trader"), TraderSources, PendingTraders, &MadFall::GameplayDefinitionsJson::ParseTrader);
}

// ===========================================================================
// Tuning and perks
// ===========================================================================

namespace MadFall::GameplayDefinitionsJson
{
	bool ParseTuning(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
		FMadTuningDefinition& Data, TArray<FMadDefinitionError>& OutErrors)
	{
		const FReader R{ SourcePath, OutErrors };
		if (!ReadHeader(R, Object, MadFall::TuningSchemaV1, ModId, /*bPathId*/ false, Data.Id))
		{
			return false;
		}
		ReadFloatMap(R, Object, TEXT("values"), TEXT("/values"), Data.Values);
		R.ReportUnknownFields(Object, { TEXT("schema"), TEXT("id"), TEXT("values"), TEXT("mod_data") });
		Data.SourceModId = ModId;
		Data.SourcePath = SourcePath;
		return true;
	}

	bool ParsePerk(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
		FMadPerkDefinition& Data, TArray<FMadDefinitionError>& OutErrors)
	{
		const FReader R{ SourcePath, OutErrors };
		if (!ReadHeader(R, Object, MadFall::PerkSchemaV1, ModId, /*bPathId*/ false, Data.Id))
		{
			return false;
		}

		R.ReadString(Object, TEXT("display_name"), TEXT("/display_name"), Data.DisplayName);
		R.ReadString(Object, TEXT("description"), TEXT("/description"), Data.Description);
		R.ReadNameArray(Object, TEXT("tags"), TEXT("/tags"), Data.Tags);

		TArray<TSharedPtr<FJsonValue>> Ranks;
		if (!R.ReadArray(Object, TEXT("ranks"), TEXT("/ranks"), Ranks) || Ranks.Num() == 0)
		{
			R.AddError(TEXT("/ranks"), TEXT("a perk needs at least one rank"));
			return false;
		}

		Data.Ranks.Reset();
		int32 PreviousLevel = 0;
		for (int32 Index = 0; Index < Ranks.Num(); ++Index)
		{
			const FString Pointer = FString::Printf(TEXT("/ranks/%d"), Index);
			if (!Ranks[Index].IsValid() || Ranks[Index]->Type != EJson::Object)
			{
				R.AddError(Pointer, TEXT("expected an object with \"level\" and \"modifiers\""));
				return false;
			}
			const TSharedRef<FJsonObject> RankObject = Ranks[Index]->AsObject().ToSharedRef();
			FMadPerkRank& Rank = Data.Ranks.AddDefaulted_GetRef();
			R.ReadInt(RankObject, TEXT("level"), Pointer + TEXT("/level"), Rank.RequiredLevel);
			ReadFloatMap(R, RankObject, TEXT("modifiers"), Pointer + TEXT("/modifiers"), Rank.Modifiers);
			TMap<FName, float> Requires;
			ReadFloatMap(R, RankObject, TEXT("requires"), Pointer + TEXT("/requires"), Requires);
			for (const TPair<FName, float>& Required : Requires)
			{
				const int32 RequiredRank = FMath::RoundToInt32(Required.Value);
				if (RequiredRank < 1 || !FMath::IsNearlyEqual(Required.Value, static_cast<float>(RequiredRank)))
				{
					R.AddError(Pointer + TEXT("/requires/") + Required.Key.ToString(), TEXT("expected a whole rank of 1 or more"));
					continue;
				}
				if (Required.Key == Data.Id)
				{
					// Earlier ranks of the same perk are already required by rank order.
					R.AddError(Pointer + TEXT("/requires/") + Required.Key.ToString(), TEXT("a perk cannot require itself"));
					continue;
				}
				Rank.Requires.Add(Required.Key, RequiredRank);
			}
			R.ReportUnknownFields(RankObject, { TEXT("level"), TEXT("modifiers"), TEXT("requires") });

			if (Rank.RequiredLevel < PreviousLevel)
			{
				R.AddError(Pointer + TEXT("/level"), TEXT("ranks must be listed in rising level order"));
			}
			PreviousLevel = Rank.RequiredLevel;
		}

		R.ReportUnknownFields(Object, { TEXT("schema"), TEXT("id"), TEXT("display_name"), TEXT("description"), TEXT("tags"),
			TEXT("ranks"), TEXT("mod_data") });
		Data.SourceModId = ModId;
		Data.SourcePath = SourcePath;
		return true;
	}
}

bool FMadGameplayDefinitions::AddTuningJson(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
	TArray<FMadDefinitionError>& OutErrors)
{
	FMadTuningDefinition Tuning;
	if (!MadFall::GameplayDefinitionsJson::ParseTuning(Object, SourcePath, ModId, Tuning, OutErrors))
	{
		return false;
	}
	TuningSources.Add(Tuning.Id, FPendingItem{ Tuning.Id, NAME_None, ModId, SourcePath, Object });
	PendingTuning.Add(Tuning.Id, MoveTemp(Tuning));
	return true;
}

bool FMadGameplayDefinitions::AddPerkJson(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
	TArray<FMadDefinitionError>& OutErrors)
{
	FMadPerkDefinition Perk;
	if (!MadFall::GameplayDefinitionsJson::ParsePerk(Object, SourcePath, ModId, Perk, OutErrors))
	{
		return false;
	}
	PerkSources.Add(Perk.Id, FPendingItem{ Perk.Id, NAME_None, ModId, SourcePath, Object });
	PendingPerks.Add(Perk.Id, MoveTemp(Perk));
	return true;
}

int32 FMadGameplayDefinitions::AddTuningFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	int32 Staged = 0;
	MadFall::Definitions::ForEachJsonObject(Directory, OutErrors,
		[&](const FString& File, const TSharedRef<FJsonObject>& Object) { Staged += AddTuningJson(Object, File, ModId, OutErrors) ? 1 : 0; });
	return Staged;
}

int32 FMadGameplayDefinitions::AddPerksFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	int32 Staged = 0;
	MadFall::Definitions::ForEachJsonObject(Directory, OutErrors,
		[&](const FString& File, const TSharedRef<FJsonObject>& Object) { Staged += AddPerkJson(Object, File, ModId, OutErrors) ? 1 : 0; });
	return Staged;
}

const FMadTuningDefinition* FMadGameplayDefinitions::FindTuning(FName Id) const
{
	const int32* Index = TuningIndex.Find(Id);
	return Index ? &Tunings[*Index] : nullptr;
}

const FMadPerkDefinition* FMadGameplayDefinitions::FindPerk(FName Id) const
{
	const int32* Index = PerkIndex.Find(Id);
	return Index ? &Perks[*Index] : nullptr;
}

// ===========================================================================
// Names
// ===========================================================================

FString FMadGameplayDefinitions::GetItemName(FName ItemId) const
{
	const FMadItemDefinition* Item = FindItem(ItemId);
	return (Item && !Item->DisplayName.IsEmpty()) ? MadFall::Localize(Item->DisplayName) : FMadStringTable::MakeReadable(ItemId.ToString());
}

FString FMadGameplayDefinitions::GetPerkName(FName PerkId) const
{
	const FMadPerkDefinition* Perk = FindPerk(PerkId);
	return (Perk && !Perk->DisplayName.IsEmpty()) ? MadFall::Localize(Perk->DisplayName) : FMadStringTable::MakeReadable(PerkId.ToString());
}

FString FMadGameplayDefinitions::GetBlockName(FName BlockId)
{
	const FMadBlockRegistry& Registry = UMadVoxelWorldSubsystem::GetBlockRegistry();
	const FMadBlockDefinitionData* Block = Registry.IsRegistered(BlockId) ? Registry.FindDefinition(Registry.ResolveRuntimeId(BlockId)) : nullptr;
	return (Block && !Block->DisplayName.IsEmpty()) ? MadFall::Localize(Block->DisplayName) : FMadStringTable::MakeReadable(BlockId.ToString());
}
