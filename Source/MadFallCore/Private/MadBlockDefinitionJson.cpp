// Copyright MadFall. All Rights Reserved.

#include "MadBlockDefinitionJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MadFallCore.h"
#include "MadJsonReader.h"
#include "MadSurfaceRegistry.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace MadFall::BlockDefinitionJson
{
	namespace
	{
		// Blocks, biomes and every later definition format share one reader so
		// that the same mistake produces the same message everywhere.
		using FReader = FMadJsonReader;

		bool ParseShapeKind(const FString& Text, EMadBlockShapeKind& Out)
		{
			if (Text.Equals(TEXT("cubic"), ESearchCase::IgnoreCase))      { Out = EMadBlockShapeKind::Cubic; return true; }
			if (Text.Equals(TEXT("isosurface"), ESearchCase::IgnoreCase)) { Out = EMadBlockShapeKind::Isosurface; return true; }
			if (Text.Equals(TEXT("model"), ESearchCase::IgnoreCase))      { Out = EMadBlockShapeKind::Model; return true; }
			return false;
		}

		bool ParseRotationMode(const FString& Text, EMadBlockRotationMode& Out)
		{
			if (Text.Equals(TEXT("none"), ESearchCase::IgnoreCase))      { Out = EMadBlockRotationMode::None; return true; }
			if (Text.Equals(TEXT("axis"), ESearchCase::IgnoreCase))      { Out = EMadBlockRotationMode::Axis; return true; }
			if (Text.Equals(TEXT("facing_4"), ESearchCase::IgnoreCase))  { Out = EMadBlockRotationMode::Facing4; return true; }
			if (Text.Equals(TEXT("full_24"), ESearchCase::IgnoreCase))   { Out = EMadBlockRotationMode::Full24; return true; }
			return false;
		}

		bool ParseCollision(const FString& Text, EMadBlockCollisionKind& Out)
		{
			if (Text.Equals(TEXT("none"), ESearchCase::IgnoreCase)) { Out = EMadBlockCollisionKind::None; return true; }
			if (Text.Equals(TEXT("box"), ESearchCase::IgnoreCase))  { Out = EMadBlockCollisionKind::Box; return true; }
			if (Text.Equals(TEXT("mesh"), ESearchCase::IgnoreCase)) { Out = EMadBlockCollisionKind::Mesh; return true; }
			return false;
		}

		void ParseShape(const FReader& R, const TSharedRef<FJsonObject>& Shape, FMadBlockDefinitionData& Data)
		{
			FString Text;
			if (R.ReadString(Shape, TEXT("kind"), TEXT("/shape/kind"), Text))
			{
				if (!ParseShapeKind(Text, Data.ShapeKind))
				{
					R.AddError(TEXT("/shape/kind"),
						FString::Printf(TEXT("'%s' is not one of: cubic, isosurface, model"), *Text));
				}
			}

			if (R.ReadString(Shape, TEXT("rotation_mode"), TEXT("/shape/rotation_mode"), Text))
			{
				if (!ParseRotationMode(Text, Data.RotationMode))
				{
					R.AddError(TEXT("/shape/rotation_mode"),
						FString::Printf(TEXT("'%s' is not one of: none, axis, facing_4, full_24"), *Text));
				}
			}

			if (R.ReadString(Shape, TEXT("collision"), TEXT("/shape/collision"), Text))
			{
				if (!ParseCollision(Text, Data.Collision))
				{
					R.AddError(TEXT("/shape/collision"),
						FString::Printf(TEXT("'%s' is not one of: none, box, mesh"), *Text));
				}
			}

			R.ReadBool(Shape, TEXT("occludes_neighbors"), TEXT("/shape/occludes_neighbors"), Data.bOccludesNeighbors);

			if (R.ReadNameArray(Shape, TEXT("variants"), TEXT("/shape/variants"), Data.Variants))
			{
				// Variants index FMadVoxel::Rotation bits 5-7. A ninth variant
				// has nowhere to live, and truncating silently would make two
				// different shapes share one encoding.
				if (Data.Variants.Num() > 8)
				{
					R.AddError(TEXT("/shape/variants"),
						FString::Printf(TEXT("at most 8 variants are addressable (Rotation bits 5-7); got %d"), Data.Variants.Num()));
					Data.Variants.SetNum(8);
				}
			}
		}

		void ParseMaterial(const FReader& R, const TSharedRef<FJsonObject>& Material, FMadBlockDefinitionData& Data)
		{
			R.ReadName(Material, TEXT("class"), TEXT("/material/class"), Data.MaterialClass);
			R.ReadFloat(Material, TEXT("mass_kg"), TEXT("/material/mass_kg"), Data.MassKg);
			R.ReadFloat(Material, TEXT("hardness"), TEXT("/material/hardness"), Data.Hardness);

			TSharedPtr<FJsonObject> Resistances;
			if (R.ReadObject(Material, TEXT("resistances"), TEXT("/material/resistances"), Resistances))
			{
				for (const auto& Pair : Resistances->Values)
				{
					const FString Pointer = FString::Printf(TEXT("/material/resistances/%s"), *Pair.Key);
					if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Number)
					{
						R.AddError(Pointer,
							FString::Printf(TEXT("expected a number, got %s"), *FMadJsonReader::DescribeType(Pair.Value)));
						continue;
					}
					Data.Resistances.Add(FName(*Pair.Key), static_cast<float>(Pair.Value->AsNumber()));
				}
			}

			TSharedPtr<FJsonObject> Harvest;
			if (R.ReadObject(Material, TEXT("harvest"), TEXT("/material/harvest"), Harvest))
			{
				const TSharedRef<FJsonObject> HarvestRef = Harvest.ToSharedRef();
				R.ReadNameArray(HarvestRef, TEXT("tool_tags"), TEXT("/material/harvest/tool_tags"), Data.HarvestToolTags);
				R.ReadInt(HarvestRef, TEXT("tier"), TEXT("/material/harvest/tier"), Data.HarvestTier);
			}
		}

		void ParseStructure(const FReader& R, const TSharedRef<FJsonObject>& Structure, FMadBlockDefinitionData& Data)
		{
			R.ReadFloat(Structure, TEXT("support_strength"), TEXT("/structure/support_strength"), Data.SupportStrength);
			R.ReadInt(Structure, TEXT("max_horizontal_span"), TEXT("/structure/max_horizontal_span"), Data.MaxHorizontalSpan);
			R.ReadBool(Structure, TEXT("is_anchor"), TEXT("/structure/is_anchor"), Data.bIsAnchor);
			R.ReadName(Structure, TEXT("debris_on_collapse"), TEXT("/structure/debris_on_collapse"), Data.DebrisOnCollapse);
		}

		void ParseDamageStages(const FReader& R, const TSharedRef<FJsonObject>& Root, FMadBlockDefinitionData& Data)
		{
			const TSharedPtr<FJsonValue> Value = Root->TryGetField(TEXT("damage_states"));
			if (!Value.IsValid() || Value->Type == EJson::Null)
			{
				return;
			}

			if (Value->Type != EJson::Array)
			{
				R.TypeError(TEXT("/damage_states"), Value, TEXT("an array"));
				return;
			}

			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			TArray<FMadBlockDamageStage> Stages;

			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				const FString Pointer = FString::Printf(TEXT("/damage_states/%d"), Index);

				if (!Items[Index].IsValid() || Items[Index]->Type != EJson::Object)
				{
					R.AddError(Pointer,
						FString::Printf(TEXT("expected an object, got %s"), *FMadJsonReader::DescribeType(Items[Index])));
					continue;
				}

				const TSharedRef<FJsonObject> StageObj = Items[Index]->AsObject().ToSharedRef();
				FMadBlockDamageStage Stage;

				R.ReadInt(StageObj, TEXT("at"), Pointer + TEXT("/at"), Stage.At);
				R.ReadSoftPath(StageObj, TEXT("mesh"), Pointer + TEXT("/mesh"), Stage.Mesh);
				R.ReadFloat(StageObj, TEXT("support_multiplier"), Pointer + TEXT("/support_multiplier"), Stage.SupportMultiplier);
				R.ReadName(StageObj, TEXT("decal_set"), Pointer + TEXT("/decal_set"), Stage.DecalSet);
				R.ReadName(StageObj, TEXT("sound"), Pointer + TEXT("/sound"), Stage.Sound);
				R.ReadName(StageObj, TEXT("downgrade_to"), Pointer + TEXT("/downgrade_to"), Stage.DowngradeTo);

				if (Stage.At < 0 || Stage.At > 255)
				{
					R.AddError(Pointer + TEXT("/at"),
						FString::Printf(TEXT("must be 0-255 (it is compared against a uint8 damage byte); got %d"), Stage.At));
					Stage.At = FMath::Clamp(Stage.At, 0, 255);
				}

				Stages.Add(Stage);
			}

			// The damage lookup does a linear walk expecting ascending
			// thresholds. Sorting here rather than trusting the author means a
			// mod with out-of-order stages renders correctly instead of
			// selecting a stage at random.
			Stages.Sort([](const FMadBlockDamageStage& A, const FMadBlockDamageStage& B) { return A.At < B.At; });

			if (Stages.Num() > 0 && Stages[0].At != 0)
			{
				R.AddError(TEXT("/damage_states/0/at"),
					FString::Printf(TEXT("the first damage state must be at 0 (the intact block); got %d"), Stages[0].At));
			}

			Data.DamageStages = MoveTemp(Stages);
		}

		void ParseRender(const FReader& R, const TSharedRef<FJsonObject>& Render, FMadBlockDefinitionData& Data)
		{
			R.ReadSoftPath(Render, TEXT("mesh"), TEXT("/render/mesh"), Data.Mesh);
			R.ReadSoftPath(Render, TEXT("material"), TEXT("/render/material"), Data.Material);
			R.ReadBool(Render, TEXT("nanite"), TEXT("/render/nanite"), Data.bNanite);
			R.ReadBool(Render, TEXT("cast_shadow"), TEXT("/render/cast_shadow"), Data.bCastShadow);
			R.ReadVector(Render, TEXT("offset"), TEXT("/render/offset"), Data.MeshOffset);
			R.ReadVector(Render, TEXT("scale"), TEXT("/render/scale"), Data.MeshScale);

			TSharedPtr<FJsonObject> Light;
			if (R.ReadObject(Render, TEXT("light"), TEXT("/render/light"), Light))
			{
				const TSharedRef<FJsonObject> L = Light.ToSharedRef();
				FVector Color;
				if (R.ReadVector(L, TEXT("color"), TEXT("/render/light/color"), Color))
				{
					// sRGB like surface colours, so a picked colour means the same thing everywhere.
					Data.LightColor = FLinearColor(
						MadFall::Surfaces::SRGBToLinear(static_cast<float>(Color.X)),
						MadFall::Surfaces::SRGBToLinear(static_cast<float>(Color.Y)),
						MadFall::Surfaces::SRGBToLinear(static_cast<float>(Color.Z)), 1.0f);
				}
				R.ReadFloat(L, TEXT("lumens"), TEXT("/render/light/lumens"), Data.LightLumens);
				R.ReadFloat(L, TEXT("radius"), TEXT("/render/light/radius"), Data.LightRadius);
				R.ReadVector(L, TEXT("offset"), TEXT("/render/light/offset"), Data.LightOffset);
				R.ReadFloat(L, TEXT("flame"), TEXT("/render/light/flame"), Data.LightFlame);
				Data.LightFlame = FMath::Clamp(Data.LightFlame, 0.0f, 8.0f);
				Data.LightLumens = FMath::Max(0.0f, Data.LightLumens);
				Data.LightRadius = FMath::Clamp(Data.LightRadius, 1.0f, 32.0f);
				R.ReportUnknownFields(L, { TEXT("color"), TEXT("lumens"), TEXT("radius"), TEXT("offset"), TEXT("flame") });
			}
		}

		void ParseFlags(const FReader& R, const TSharedRef<FJsonObject>& Flags, FMadBlockDefinitionData& Data)
		{
			R.ReadBool(Flags, TEXT("transparent"), TEXT("/flags/transparent"), Data.bTransparent);
			R.ReadBool(Flags, TEXT("liquid"), TEXT("/flags/liquid"), Data.bLiquid);
			R.ReadBool(Flags, TEXT("climbable"), TEXT("/flags/climbable"), Data.bClimbable);
			R.ReadBool(Flags, TEXT("flammable"), TEXT("/flags/flammable"), Data.bFlammable);
			R.ReadBool(Flags, TEXT("conductive"), TEXT("/flags/conductive"), Data.bConductive);
		}

		void ParseDrops(const FReader& R, const TSharedRef<FJsonObject>& Drops, FMadBlockDefinitionData& Data)
		{
			R.ReadName(Drops, TEXT("table"), TEXT("/drops/table"), Data.DropTable);
			R.ReadName(Drops, TEXT("on_collapse"), TEXT("/drops/on_collapse"), Data.DropTableOnCollapse);
			R.ReadNameArray(Drops, TEXT("requires_tool_tags"), TEXT("/drops/requires_tool_tags"), Data.RequiresToolTags);
		}

		void ParseSounds(const FReader& R, const TSharedRef<FJsonObject>& Sounds, FMadBlockDefinitionData& Data)
		{
			for (const auto& Pair : Sounds->Values)
			{
				const FString Pointer = FString::Printf(TEXT("/sounds/%s"), *Pair.Key);
				if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
				{
					R.AddError(Pointer,
						FString::Printf(TEXT("expected a string, got %s"), *FMadJsonReader::DescribeType(Pair.Value)));
					continue;
				}

				const FString Text = Pair.Value->AsString();

				// "madfall:material_default" means inherit from the material
				// class rather than reference an asset. Recorded as an empty
				// path; the material class supplies it at play time.
				if (!Text.StartsWith(TEXT("/")))
				{
					continue;
				}

				Data.Sounds.Add(FName(*Pair.Key), FSoftObjectPath(Text));
			}
		}
	}

	FName GetNamespace(FName Id)
	{
		const FString Text = Id.ToString();
		int32 Colon = INDEX_NONE;
		if (!Text.FindChar(TEXT(':'), Colon) || Colon <= 0)
		{
			return NAME_None;
		}
		return FName(*Text.Left(Colon));
	}

	bool IsValidBlockId(FName Id, FString& OutReason)
	{
		if (Id.IsNone())
		{
			OutReason = TEXT("id is empty");
			return false;
		}

		const FString Text = Id.ToString();

		int32 Colon = INDEX_NONE;
		if (!Text.FindChar(TEXT(':'), Colon))
		{
			OutReason = FString::Printf(TEXT("'%s' has no namespace - ids look like 'mymod:stone'"), *Text);
			return false;
		}

		if (Colon == 0 || Colon == Text.Len() - 1)
		{
			OutReason = FString::Printf(TEXT("'%s' has an empty namespace or name"), *Text);
			return false;
		}

		if (Text.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromEnd) != Colon)
		{
			OutReason = FString::Printf(TEXT("'%s' contains more than one ':'"), *Text);
			return false;
		}

		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const TCHAR Char = Text[Index];
			const bool bAllowed = (Char >= TEXT('a') && Char <= TEXT('z'))
				|| (Char >= TEXT('0') && Char <= TEXT('9'))
				|| Char == TEXT('_')
				|| Char == TEXT(':');

			if (!bAllowed)
			{
				OutReason = FString::Printf(
					TEXT("'%s' contains '%c' - ids are limited to lowercase a-z, 0-9 and _ so that they resolve identically on case-sensitive and case-insensitive filesystems"),
					*Text, Char);
				return false;
			}
		}

		return true;
	}

	bool ParseObject(
		const TSharedRef<FJsonObject>& Object,
		const FString& SourcePath,
		FName ModId,
		FMadBlockDefinitionData& OutData,
		TArray<FMadDefinitionError>& OutErrors)
	{
		const FReader R{ SourcePath, OutErrors };

		// --- schema gate ---
		FString Schema;
		if (!R.ReadString(Object, TEXT("schema"), TEXT("/schema"), Schema))
		{
			R.AddError(TEXT("/schema"),
				FString::Printf(TEXT("missing required field; expected \"%s\""), MadFall::BlockSchemaV1));
			return false;
		}

		if (Schema != MadFall::BlockSchemaV1)
		{
			// Refusing an unknown schema rather than best-effort parsing it is
			// the point: a definition written against a future schema would
			// half-load and produce a block with silently wrong physics.
			R.AddError(TEXT("/schema"),
				FString::Printf(TEXT("unsupported schema \"%s\"; this build understands \"%s\""),
					*Schema, MadFall::BlockSchemaV1));
			return false;
		}

		// --- identity ---
		if (!R.ReadName(Object, TEXT("id"), TEXT("/id"), OutData.Id))
		{
			R.AddError(TEXT("/id"), TEXT("missing required field"));
			return false;
		}

		FString Reason;
		if (!IsValidBlockId(OutData.Id, Reason))
		{
			R.AddError(TEXT("/id"), Reason);
			return false;
		}

		// A mod may not define into someone else's namespace on first
		// definition. Editing another mod's block is a patch file, not a
		// redefinition - otherwise load order silently decides who wins.
		const FName Namespace = GetNamespace(OutData.Id);
		if (!ModId.IsNone() && Namespace != ModId)
		{
			R.AddError(TEXT("/id"),
				FString::Printf(TEXT("namespace '%s' does not match the owning mod id '%s'. To change another mod's block, ship a patch under definitions/patches/ instead."),
					*Namespace.ToString(), *ModId.ToString()));
			return false;
		}

		R.ReadName(Object, TEXT("extends"), TEXT("/extends"), OutData.Extends);
		R.ReadString(Object, TEXT("display_name"), TEXT("/display_name"), OutData.DisplayName);
		R.ReadNameArray(Object, TEXT("tags"), TEXT("/tags"), OutData.Tags);

		// --- sections ---
		TSharedPtr<FJsonObject> Section;

		if (R.ReadObject(Object, TEXT("shape"), TEXT("/shape"), Section))
		{
			ParseShape(R, Section.ToSharedRef(), OutData);
		}

		if (R.ReadObject(Object, TEXT("material"), TEXT("/material"), Section))
		{
			ParseMaterial(R, Section.ToSharedRef(), OutData);
		}

		if (R.ReadObject(Object, TEXT("structure"), TEXT("/structure"), Section))
		{
			ParseStructure(R, Section.ToSharedRef(), OutData);
		}

		ParseDamageStages(R, Object, OutData);

		if (R.ReadObject(Object, TEXT("render"), TEXT("/render"), Section))
		{
			ParseRender(R, Section.ToSharedRef(), OutData);
		}

		if (R.ReadObject(Object, TEXT("sounds"), TEXT("/sounds"), Section))
		{
			ParseSounds(R, Section.ToSharedRef(), OutData);
		}

		if (R.ReadObject(Object, TEXT("drops"), TEXT("/drops"), Section))
		{
			ParseDrops(R, Section.ToSharedRef(), OutData);
		}

		if (R.ReadObject(Object, TEXT("flags"), TEXT("/flags"), Section))
		{
			ParseFlags(R, Section.ToSharedRef(), OutData);
		}

		if (R.ReadObject(Object, TEXT("interact"), TEXT("/interact"), Section))
		{
			const TSharedRef<FJsonObject> Interact = Section.ToSharedRef();
			R.ReadName(Interact, TEXT("toggle_to"), TEXT("/interact/toggle_to"), OutData.ToggleTo);
			R.ReadBool(Interact, TEXT("spawn_point"), TEXT("/interact/spawn_point"), OutData.bSpawnPoint);
			R.ReportUnknownFields(Interact, { TEXT("toggle_to"), TEXT("spawn_point") });
		}

		if (R.ReadObject(Object, TEXT("trap"), TEXT("/trap"), Section))
		{
			const TSharedRef<FJsonObject> Trap = Section.ToSharedRef();
			R.ReadFloat(Trap, TEXT("damage"), TEXT("/trap/damage"), OutData.TrapDamage);
			R.ReadFloat(Trap, TEXT("seconds"), TEXT("/trap/seconds"), OutData.TrapSeconds);
			R.ReadFloat(Trap, TEXT("wear"), TEXT("/trap/wear"), OutData.TrapWear);
			R.ReadFloat(Trap, TEXT("slow"), TEXT("/trap/slow"), OutData.TrapSlow);
			R.ReportUnknownFields(Trap, { TEXT("damage"), TEXT("seconds"), TEXT("wear"), TEXT("slow") });
			if (OutData.TrapSlow <= 0.0f || OutData.TrapSlow > 1.0f || OutData.TrapSeconds < 0.1f || OutData.TrapDamage < 0.0f)
			{
				R.AddError(TEXT("/trap"), TEXT("slow must be above 0 and at most 1, seconds at least 0.1, damage not negative"));
				OutData.TrapSlow = FMath::Clamp(OutData.TrapSlow, 0.05f, 1.0f);
				OutData.TrapSeconds = FMath::Max(0.1f, OutData.TrapSeconds);
				OutData.TrapDamage = FMath::Max(0.0f, OutData.TrapDamage);
			}
			if (OutData.Collision != EMadBlockCollisionKind::None)
			{
				R.AddError(TEXT("/trap"), TEXT("a trap needs \"collision\": \"none\" - creatures have to walk into it"));
			}
		}

		if (R.ReadObject(Object, TEXT("grow"), TEXT("/grow"), Section))
		{
			const TSharedRef<FJsonObject> Grow = Section.ToSharedRef();
			R.ReadName(Grow, TEXT("into"), TEXT("/grow/into"), OutData.GrowInto);
			R.ReadFloat(Grow, TEXT("hours"), TEXT("/grow/hours"), OutData.GrowHours);
			if (OutData.GrowInto.IsNone() || OutData.GrowHours <= 0.0f)
			{
				R.AddError(TEXT("/grow"), TEXT("needs \"into\" and \"hours\" greater than 0"));
				OutData.GrowInto = NAME_None;
				OutData.GrowHours = 0.0f;
			}
			R.ReportUnknownFields(Grow, { TEXT("into"), TEXT("hours") });
		}

		if (R.ReadObject(Object, TEXT("placement"), TEXT("/placement"), Section))
		{
			const TSharedRef<FJsonObject> Placement = Section.ToSharedRef();
			R.ReadName(Placement, TEXT("on_tag"), TEXT("/placement/on_tag"), OutData.PlaceOnTag);
			// requires_support and snap are authored in shipped content ahead of use.
			R.ReportUnknownFields(Placement, { TEXT("on_tag"), TEXT("requires_support"), TEXT("snap") });
		}

		// A misspelled key would otherwise be silently ignored, and the modder
		// would spend an evening wondering why "mass_kilograms" did nothing.
		// Sections that later phases will consume are listed here on purpose so
		// that authoring ahead of the implementation is not punished.
		static const TSet<FString> KnownKeys = {
			TEXT("schema"), TEXT("id"), TEXT("extends"), TEXT("display_name"), TEXT("tags"),
			TEXT("shape"), TEXT("material"), TEXT("structure"), TEXT("damage_states"),
			TEXT("render"), TEXT("sounds"), TEXT("drops"), TEXT("flags"), TEXT("interact"),
			TEXT("grow"), TEXT("placement"), TEXT("trap"),
			// Not consumed by the game; free for mods' own tooling.
			TEXT("mod_data")
		};

		R.ReportUnknownFields(Object, KnownKeys);

		OutData.SourceModId = ModId;
		OutData.SourcePath = SourcePath;

		return true;
	}

	bool ParseText(
		const FString& JsonText,
		const FString& SourcePath,
		FName ModId,
		TArray<FMadBlockDefinitionData>& OutDefinitions,
		TArray<FMadDefinitionError>& OutErrors)
	{
		const FReader R{ SourcePath, OutErrors };
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);

		TSharedPtr<FJsonValue> Root;
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			R.AddError(FString(),
				FString::Printf(TEXT("not valid JSON: %s"), *Reader->GetErrorMessage()));
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> Objects;
		if (Root->Type == EJson::Array)
		{
			Objects = Root->AsArray();
		}
		else if (Root->Type == EJson::Object)
		{
			Objects.Add(Root);
		}
		else
		{
			R.AddError(FString(),
				FString::Printf(TEXT("expected an object or an array of objects, got %s"), *FMadJsonReader::DescribeType(Root)));
			return false;
		}

		bool bAnyLoaded = false;

		for (int32 Index = 0; Index < Objects.Num(); ++Index)
		{
			if (!Objects[Index].IsValid() || Objects[Index]->Type != EJson::Object)
			{
				R.AddError(FString::Printf(TEXT("/%d"), Index),
					FString::Printf(TEXT("expected an object, got %s"), *FMadJsonReader::DescribeType(Objects[Index])));
				continue;
			}

			FMadBlockDefinitionData Data;
			if (ParseObject(Objects[Index]->AsObject().ToSharedRef(), SourcePath, ModId, Data, OutErrors))
			{
				OutDefinitions.Add(MoveTemp(Data));
				bAnyLoaded = true;
			}
		}

		return bAnyLoaded;
	}

	bool ParseFile(
		const FString& FilePath,
		FName ModId,
		TArray<FMadBlockDefinitionData>& OutDefinitions,
		TArray<FMadDefinitionError>& OutErrors)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			const FReader R{ FilePath, OutErrors };
			R.AddError(FString(), TEXT("could not be read from disk"));
			return false;
		}

		return ParseText(JsonText, FilePath, ModId, OutDefinitions, OutErrors);
	}
}
