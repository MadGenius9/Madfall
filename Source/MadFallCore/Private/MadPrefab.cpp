// Copyright MadFall. All Rights Reserved.

#include "MadPrefab.h"

#include "Dom/JsonObject.h"
#include "MadFallCore.h"
#include "MadJsonReader.h"
#include "Misc/StringBuilder.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FIntVector FMadPrefab::GetEntrance() const
{
	for (const FMadPoiMarker& Marker : Markers)
	{
		if (Marker.Type == FName(TEXT("entrance")))
		{
			return Marker.Position;
		}
	}

	return FIntVector(Size.X / 2, Size.Y / 2, 0);
}

int32 FMadPrefab::CountSolidVoxels() const
{
	int32 Count = 0;
	for (uint16 Index : Voxels)
	{
		const FMadPrefabPaletteEntry& Entry = Palette[Index];
		if (!Entry.bVoid && !Entry.IsAir() && Entry.Density >= 128)
		{
			++Count;
		}
	}
	return Count;
}

namespace MadFall::PrefabJson
{
	namespace
	{
		using FReader = FMadJsonReader;

		bool ReadIntVector(const FReader& R, const TSharedRef<FJsonObject>& Obj, const TCHAR* Field,
			const FString& Pointer, FIntVector& Out)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			if (!R.ReadArray(Obj, Field, Pointer, Items))
			{
				return false;
			}

			if (Items.Num() != 3)
			{
				R.AddError(Pointer, FString::Printf(TEXT("expected [x, y, z], got %d element(s)"), Items.Num()));
				return false;
			}

			int32 Values[3];
			for (int32 Index = 0; Index < 3; ++Index)
			{
				if (!Items[Index].IsValid() || Items[Index]->Type != EJson::Number)
				{
					R.AddError(FString::Printf(TEXT("%s/%d"), *Pointer, Index),
						FString::Printf(TEXT("expected a number, got %s"), *FMadJsonReader::DescribeType(Items[Index])));
					return false;
				}
				Values[Index] = static_cast<int32>(FMath::RoundToDouble(Items[Index]->AsNumber()));
			}

			Out = FIntVector(Values[0], Values[1], Values[2]);
			return true;
		}

		void ParsePlacement(const FReader& R, const TSharedRef<FJsonObject>& Obj, FMadPrefabPlacement& Out)
		{
			R.ReadFloat(Obj, TEXT("rarity"), TEXT("/placement/rarity"), Out.Rarity);
			R.ReadInt(Obj, TEXT("embed_depth"), TEXT("/placement/embed_depth"), Out.EmbedDepth);
			R.ReadInt(Obj, TEXT("max_slope"), TEXT("/placement/max_slope"), Out.MaxSlope);
			R.ReadName(Obj, TEXT("foundation"), TEXT("/placement/foundation"), Out.FoundationBlock);
			R.ReadInt(Obj, TEXT("max_foundation_depth"), TEXT("/placement/max_foundation_depth"), Out.MaxFoundationDepth);
			R.ReadNameArray(Obj, TEXT("biomes"), TEXT("/placement/biomes"), Out.Biomes);
			R.ReadBool(Obj, TEXT("underwater"), TEXT("/placement/underwater"), Out.bUnderwater);
			R.ReadBool(Obj, TEXT("near_spawn"), TEXT("/placement/near_spawn"), Out.bNearSpawn);

			FString Conform;
			if (R.ReadString(Obj, TEXT("conform"), TEXT("/placement/conform"), Conform))
			{
				if (Conform.Equals(TEXT("none"), ESearchCase::IgnoreCase)) { Out.Conform = EMadPrefabConform::None; }
				else if (Conform.Equals(TEXT("base"), ESearchCase::IgnoreCase)) { Out.Conform = EMadPrefabConform::Base; }
				else
				{
					R.AddError(TEXT("/placement/conform"),
						FString::Printf(TEXT("'%s' is not one of: none, base"), *Conform));
				}
			}

			Out.Rarity = FMath::Max(Out.Rarity, 0.0f);
			Out.EmbedDepth = FMath::Clamp(Out.EmbedDepth, 0, 16);
			Out.MaxSlope = FMath::Max(Out.MaxSlope, 0);
			Out.MaxFoundationDepth = FMath::Clamp(Out.MaxFoundationDepth, 0, 64);
		}

		FString Escape(const FString& Text)
		{
			return Text.ReplaceCharWithEscapedChar();
		}
	}

	bool ParseText(
		const FString& JsonText,
		const FString& SourcePath,
		FName ModId,
		FMadPrefab& OutPrefab,
		TArray<FMadDefinitionError>& OutErrors)
	{
		const FReader R{ SourcePath, OutErrors };

		const TSharedRef<TJsonReader<TCHAR>> JsonReader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		TSharedPtr<FJsonObject> RootPtr;
		if (!FJsonSerializer::Deserialize(JsonReader, RootPtr) || !RootPtr.IsValid())
		{
			R.AddError(FString(), FString::Printf(TEXT("not valid JSON: %s"), *JsonReader->GetErrorMessage()));
			return false;
		}

		const TSharedRef<FJsonObject> Root = RootPtr.ToSharedRef();

		// --- schema and identity ---
		FString Schema;
		if (!R.ReadString(Root, TEXT("schema"), TEXT("/schema"), Schema) || Schema != MadFall::PrefabSchemaV1)
		{
			R.AddError(TEXT("/schema"),
				FString::Printf(TEXT("expected \"%s\", got \"%s\""), MadFall::PrefabSchemaV1, *Schema));
			return false;
		}

		if (!R.ReadName(Root, TEXT("id"), TEXT("/id"), OutPrefab.Id))
		{
			R.AddError(TEXT("/id"), TEXT("missing required field"));
			return false;
		}

		FString Reason;
		if (!MadFall::BlockDefinitionJson::IsValidBlockId(OutPrefab.Id, Reason))
		{
			R.AddError(TEXT("/id"), Reason);
			return false;
		}

		if (!ModId.IsNone() && MadFall::BlockDefinitionJson::GetNamespace(OutPrefab.Id) != ModId)
		{
			R.AddError(TEXT("/id"), FString::Printf(TEXT("namespace does not match the owning mod id '%s'"),
				*ModId.ToString()));
			return false;
		}

		R.ReadString(Root, TEXT("display_name"), TEXT("/display_name"), OutPrefab.DisplayName);
		R.ReadNameArray(Root, TEXT("tags"), TEXT("/tags"), OutPrefab.Tags);

		if (R.ReadInt(Root, TEXT("tier"), TEXT("/tier"), OutPrefab.Tier) && (OutPrefab.Tier < 1 || OutPrefab.Tier > 5))
		{
			R.AddError(TEXT("/tier"), FString::Printf(TEXT("must be 1-5; got %d, clamped"), OutPrefab.Tier));
			OutPrefab.Tier = FMath::Clamp(OutPrefab.Tier, 1, 5);
		}

		// --- size ---
		if (!ReadIntVector(R, Root, TEXT("size"), TEXT("/size"), OutPrefab.Size))
		{
			R.AddError(TEXT("/size"), TEXT("missing required field"));
			return false;
		}

		const FIntVector& Size = OutPrefab.Size;
		if (Size.X < 1 || Size.Y < 1 || Size.Z < 1
			|| Size.X > MadFall::PrefabMaxFootprint || Size.Y > MadFall::PrefabMaxFootprint
			|| Size.Z > MadFall::PrefabMaxHeight)
		{
			// Rejected rather than clamped: a clamped size would reinterpret every
			// voxel run against the wrong row length and scramble the building.
			R.AddError(TEXT("/size"), FString::Printf(
				TEXT("[%d, %d, %d] is outside 1..%d x 1..%d x 1..%d. Larger POIs do not fit in one placement cell."),
				Size.X, Size.Y, Size.Z,
				MadFall::PrefabMaxFootprint, MadFall::PrefabMaxFootprint, MadFall::PrefabMaxHeight));
			return false;
		}

		// --- palette ---
		TArray<TSharedPtr<FJsonValue>> PaletteItems;
		if (!R.ReadArray(Root, TEXT("palette"), TEXT("/palette"), PaletteItems) || PaletteItems.Num() == 0)
		{
			R.AddError(TEXT("/palette"), TEXT("missing or empty; a prefab needs at least one palette entry"));
			return false;
		}

		if (PaletteItems.Num() > 65535)
		{
			R.AddError(TEXT("/palette"), TEXT("more than 65535 entries"));
			return false;
		}

		for (int32 Index = 0; Index < PaletteItems.Num(); ++Index)
		{
			const FString Pointer = FString::Printf(TEXT("/palette/%d"), Index);

			if (!PaletteItems[Index].IsValid() || PaletteItems[Index]->Type != EJson::Object)
			{
				R.AddError(Pointer, TEXT("expected an object"));
				return false;
			}

			const TSharedRef<FJsonObject> EntryObj = PaletteItems[Index]->AsObject().ToSharedRef();
			FMadPrefabPaletteEntry Entry;

			FString BlockText;
			if (!R.ReadString(EntryObj, TEXT("block"), Pointer + TEXT("/block"), BlockText))
			{
				R.AddError(Pointer + TEXT("/block"), TEXT("missing required field"));
				return false;
			}

			if (BlockText == MadFall::PrefabVoidToken)
			{
				Entry.bVoid = true;
			}
			else
			{
				Entry.Block = FName(*BlockText);

				int32 Orientation = 0;
				int32 Variant = 0;
				int32 Density = 255;
				R.ReadInt(EntryObj, TEXT("orientation"), Pointer + TEXT("/orientation"), Orientation);
				R.ReadInt(EntryObj, TEXT("variant"), Pointer + TEXT("/variant"), Variant);
				R.ReadInt(EntryObj, TEXT("density"), Pointer + TEXT("/density"), Density);
				R.ReadBool(EntryObj, TEXT("cubic"), Pointer + TEXT("/cubic"), Entry.bCubic);

				if (Orientation < 0 || Orientation > 23)
				{
					R.AddError(Pointer + TEXT("/orientation"), FString::Printf(TEXT("must be 0-23; got %d"), Orientation));
					Orientation = 0;
				}
				if (Variant < 0 || Variant > 7)
				{
					R.AddError(Pointer + TEXT("/variant"), FString::Printf(TEXT("must be 0-7; got %d"), Variant));
					Variant = 0;
				}

				Entry.Orientation = static_cast<uint8>(Orientation);
				Entry.Variant = static_cast<uint8>(Variant);

				if (Entry.IsAir())
				{
					Entry.Density = 0;
					Entry.bCubic = false;
				}
				else
				{
					Entry.Density = static_cast<uint8>(FMath::Clamp(Density, 0, 255));
				}
			}

			OutPrefab.Palette.Add(Entry);
		}

		// --- voxels (run-length encoded) ---
		TArray<TSharedPtr<FJsonValue>> Runs;
		if (!R.ReadArray(Root, TEXT("voxels"), TEXT("/voxels"), Runs))
		{
			R.AddError(TEXT("/voxels"), TEXT("missing required field"));
			return false;
		}

		if (Runs.Num() % 2 != 0)
		{
			R.AddError(TEXT("/voxels"), TEXT("must be a flat [count, palette_index, ...] list with an even number of entries"));
			return false;
		}

		const int32 Expected = Size.X * Size.Y * Size.Z;
		OutPrefab.Voxels.Reserve(Expected);

		for (int32 Pair = 0; Pair < Runs.Num(); Pair += 2)
		{
			if (!Runs[Pair].IsValid() || Runs[Pair]->Type != EJson::Number
				|| !Runs[Pair + 1].IsValid() || Runs[Pair + 1]->Type != EJson::Number)
			{
				R.AddError(FString::Printf(TEXT("/voxels/%d"), Pair), TEXT("run entries must be numbers"));
				return false;
			}

			const int32 Count = static_cast<int32>(Runs[Pair]->AsNumber());
			const int32 PaletteIndex = static_cast<int32>(Runs[Pair + 1]->AsNumber());

			if (Count < 1 || OutPrefab.Voxels.Num() + Count > Expected)
			{
				R.AddError(FString::Printf(TEXT("/voxels/%d"), Pair),
					FString::Printf(TEXT("run of %d would exceed the %d voxels a [%d, %d, %d] prefab holds"),
						Count, Expected, Size.X, Size.Y, Size.Z));
				return false;
			}

			if (!OutPrefab.Palette.IsValidIndex(PaletteIndex))
			{
				R.AddError(FString::Printf(TEXT("/voxels/%d"), Pair + 1),
					FString::Printf(TEXT("palette index %d is out of range (palette has %d entries)"),
						PaletteIndex, OutPrefab.Palette.Num()));
				return false;
			}

			for (int32 Step = 0; Step < Count; ++Step)
			{
				OutPrefab.Voxels.Add(static_cast<uint16>(PaletteIndex));
			}
		}

		if (OutPrefab.Voxels.Num() != Expected)
		{
			// A short run list is refused, not padded: padding would silently fill
			// the top of a building with whatever palette entry 0 happened to be.
			R.AddError(TEXT("/voxels"), FString::Printf(TEXT("covers %d voxels; a [%d, %d, %d] prefab needs exactly %d"),
				OutPrefab.Voxels.Num(), Size.X, Size.Y, Size.Z, Expected));
			return false;
		}

		// --- markers ---
		TArray<TSharedPtr<FJsonValue>> MarkerItems;
		if (R.ReadArray(Root, TEXT("markers"), TEXT("/markers"), MarkerItems))
		{
			for (int32 Index = 0; Index < MarkerItems.Num(); ++Index)
			{
				const FString Pointer = FString::Printf(TEXT("/markers/%d"), Index);

				if (!MarkerItems[Index].IsValid() || MarkerItems[Index]->Type != EJson::Object)
				{
					R.AddError(Pointer, TEXT("expected an object"));
					continue;
				}

				const TSharedRef<FJsonObject> MarkerObj = MarkerItems[Index]->AsObject().ToSharedRef();
				FMadPoiMarker Marker;

				if (!R.ReadName(MarkerObj, TEXT("type"), Pointer + TEXT("/type"), Marker.Type))
				{
					R.AddError(Pointer + TEXT("/type"), TEXT("missing required field"));
					continue;
				}

				if (!ReadIntVector(R, MarkerObj, TEXT("position"), Pointer + TEXT("/position"), Marker.Position))
				{
					R.AddError(Pointer + TEXT("/position"), TEXT("missing required field"));
					continue;
				}

				if (!OutPrefab.Contains(Marker.Position))
				{
					// A marker outside the prefab would spawn loot in the terrain
					// next to the building, in a different chunk, with no way for
					// the player to tell why the crate is inside a hill.
					R.AddError(Pointer + TEXT("/position"), FString::Printf(
						TEXT("[%d, %d, %d] is outside the prefab's [%d, %d, %d] bounds; marker dropped"),
						Marker.Position.X, Marker.Position.Y, Marker.Position.Z, Size.X, Size.Y, Size.Z));
					continue;
				}

				R.ReadName(MarkerObj, TEXT("loot_table"), Pointer + TEXT("/loot_table"), Marker.LootTable);
				R.ReadName(MarkerObj, TEXT("spawn_group"), Pointer + TEXT("/spawn_group"), Marker.SpawnGroup);
				R.ReadInt(MarkerObj, TEXT("count"), Pointer + TEXT("/count"), Marker.Count);
				R.ReadNameArray(MarkerObj, TEXT("tags"), Pointer + TEXT("/tags"), Marker.Tags);
				R.ReadName(MarkerObj, TEXT("trader"), Pointer + TEXT("/trader"), Marker.Trader);
				Marker.Count = FMath::Max(Marker.Count, 0);
				if (Marker.Type == FName(TEXT("trader")) && Marker.Trader.IsNone())
				{
					R.AddError(Pointer + TEXT("/trader"), TEXT("a trader marker needs a trader id"));
				}

				if (Marker.Type == FName(TEXT("loot")) && Marker.LootTable.IsNone())
				{
					R.AddError(Pointer + TEXT("/loot_table"), TEXT("a loot marker with no loot_table will spawn an empty container"));
				}
				if (Marker.Type == FName(TEXT("spawn")) && Marker.SpawnGroup.IsNone())
				{
					R.AddError(Pointer + TEXT("/spawn_group"), TEXT("a spawn marker with no spawn_group will spawn nothing"));
				}

				OutPrefab.Markers.Add(Marker);
			}
		}

		// --- placement ---
		TSharedPtr<FJsonObject> PlacementObj;
		if (R.ReadObject(Root, TEXT("placement"), TEXT("/placement"), PlacementObj))
		{
			ParsePlacement(R, PlacementObj.ToSharedRef(), OutPrefab.Placement);
		}

		static const TSet<FString> KnownKeys = {
			TEXT("schema"), TEXT("id"), TEXT("display_name"), TEXT("tags"), TEXT("tier"), TEXT("size"),
			TEXT("palette"), TEXT("voxels"), TEXT("markers"), TEXT("placement"), TEXT("mod_data")
		};
		R.ReportUnknownFields(Root, KnownKeys);

		OutPrefab.SourceModId = ModId;
		OutPrefab.SourcePath = SourcePath;

		return true;
	}

	FString WriteText(const FMadPrefab& Prefab)
	{
		TStringBuilder<8192> Out;

		Out.Append(TEXT("{\n"));
		Out.Appendf(TEXT("\t\"schema\": \"%s\",\n"), MadFall::PrefabSchemaV1);
		Out.Appendf(TEXT("\t\"id\": \"%s\",\n"), *Prefab.Id.ToString());
		Out.Appendf(TEXT("\t\"display_name\": \"%s\",\n"), *Escape(Prefab.DisplayName));
		Out.Appendf(TEXT("\t\"tier\": %d,\n"), Prefab.Tier);

		Out.Append(TEXT("\t\"tags\": ["));
		for (int32 Index = 0; Index < Prefab.Tags.Num(); ++Index)
		{
			Out.Appendf(TEXT("%s\"%s\""), Index ? TEXT(", ") : TEXT(""), *Prefab.Tags[Index].ToString());
		}
		Out.Append(TEXT("],\n"));

		Out.Appendf(TEXT("\t\"size\": [%d, %d, %d],\n"), Prefab.Size.X, Prefab.Size.Y, Prefab.Size.Z);

		// --- placement ---
		const FMadPrefabPlacement& P = Prefab.Placement;
		Out.Append(TEXT("\t\"placement\": {\n"));
		Out.Appendf(TEXT("\t\t\"rarity\": %.3f,\n"), P.Rarity);
		Out.Appendf(TEXT("\t\t\"conform\": \"%s\",\n"), P.Conform == EMadPrefabConform::Base ? TEXT("base") : TEXT("none"));
		Out.Appendf(TEXT("\t\t\"embed_depth\": %d,\n"), P.EmbedDepth);
		Out.Appendf(TEXT("\t\t\"max_slope\": %d,\n"), P.MaxSlope);
		if (!P.FoundationBlock.IsNone())
		{
			Out.Appendf(TEXT("\t\t\"foundation\": \"%s\",\n"), *P.FoundationBlock.ToString());
		}
		Out.Appendf(TEXT("\t\t\"max_foundation_depth\": %d,\n"), P.MaxFoundationDepth);
		Out.Append(TEXT("\t\t\"biomes\": ["));
		for (int32 Index = 0; Index < P.Biomes.Num(); ++Index)
		{
			Out.Appendf(TEXT("%s\"%s\""), Index ? TEXT(", ") : TEXT(""), *P.Biomes[Index].ToString());
		}
		Out.Append(TEXT("],\n"));
		Out.Appendf(TEXT("\t\t\"underwater\": %s\n"), P.bUnderwater ? TEXT("true") : TEXT("false"));
		Out.Append(TEXT("\t},\n"));

		// --- palette ---
		Out.Append(TEXT("\t\"palette\": [\n"));
		for (int32 Index = 0; Index < Prefab.Palette.Num(); ++Index)
		{
			const FMadPrefabPaletteEntry& Entry = Prefab.Palette[Index];
			const TCHAR* Separator = (Index + 1 < Prefab.Palette.Num()) ? TEXT(",") : TEXT("");

			if (Entry.bVoid)
			{
				Out.Appendf(TEXT("\t\t{ \"block\": \"%s\" }%s\n"), MadFall::PrefabVoidToken, Separator);
			}
			else if (Entry.IsAir())
			{
				Out.Appendf(TEXT("\t\t{ \"block\": \"madfall:air\" }%s\n"), Separator);
			}
			else
			{
				Out.Appendf(TEXT("\t\t{ \"block\": \"%s\", \"orientation\": %d, \"variant\": %d, \"density\": %d, \"cubic\": %s }%s\n"),
					*Entry.Block.ToString(), Entry.Orientation, Entry.Variant, Entry.Density,
					Entry.bCubic ? TEXT("true") : TEXT("false"), Separator);
			}
		}
		Out.Append(TEXT("\t],\n"));

		// --- voxels: one line of runs, wrapped every 24 pairs ---
		Out.Append(TEXT("\t\"voxels\": [\n\t\t"));
		int32 PairsOnLine = 0;
		bool bFirst = true;

		for (int32 Index = 0; Index < Prefab.Voxels.Num();)
		{
			const uint16 Value = Prefab.Voxels[Index];
			int32 Run = 1;
			while (Index + Run < Prefab.Voxels.Num() && Prefab.Voxels[Index + Run] == Value)
			{
				++Run;
			}

			if (!bFirst)
			{
				Out.Append(TEXT(", "));
				if (PairsOnLine >= 24)
				{
					Out.Append(TEXT("\n\t\t"));
					PairsOnLine = 0;
				}
			}

			Out.Appendf(TEXT("%d, %d"), Run, Value);
			++PairsOnLine;
			bFirst = false;
			Index += Run;
		}
		Out.Append(TEXT("\n\t],\n"));

		// --- markers ---
		Out.Append(TEXT("\t\"markers\": [\n"));
		for (int32 Index = 0; Index < Prefab.Markers.Num(); ++Index)
		{
			const FMadPoiMarker& M = Prefab.Markers[Index];
			Out.Appendf(TEXT("\t\t{ \"type\": \"%s\", \"position\": [%d, %d, %d]"),
				*M.Type.ToString(), M.Position.X, M.Position.Y, M.Position.Z);
			if (!M.LootTable.IsNone())  { Out.Appendf(TEXT(", \"loot_table\": \"%s\""), *M.LootTable.ToString()); }
			if (!M.SpawnGroup.IsNone()) { Out.Appendf(TEXT(", \"spawn_group\": \"%s\", \"count\": %d"), *M.SpawnGroup.ToString(), M.Count); }
			Out.Appendf(TEXT(" }%s\n"), (Index + 1 < Prefab.Markers.Num()) ? TEXT(",") : TEXT(""));
		}
		Out.Append(TEXT("\t]\n"));

		Out.Append(TEXT("}\n"));
		return Out.ToString();
	}
}
