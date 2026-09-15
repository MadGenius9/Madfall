// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MadBlockDefinitionJson.h"
#include "UObject/SoftObjectPath.h"

/**
 * Shared typed JSON reading for every MadFall definition format.
 *
 * Private to MadFallCore - modders see the errors this produces, never this
 * type. Extracted because blocks, biomes and (in later phases) items, recipes
 * and loot tables all need identical behaviour, and three copies of "report the
 * pointer and the expected type" would drift into three different error
 * vocabularies for the same mistake.
 *
 * THE CONTRACT, which the whole definition system leans on:
 *   - field absent        -> output untouched, nothing reported
 *   - field present, wrong type -> output untouched, error reported
 *   - field present, right type -> output written
 *
 * The first case is what makes `extends` inheritance work: resolve the parent,
 * then re-apply the child's JSON over it, and the fields the child never
 * mentioned survive. Inheritance is that contract applied twice, not a separate
 * merge engine.
 */
struct FMadJsonReader
{
	const FString& SourcePath;
	TArray<FMadDefinitionError>& Errors;

	void AddError(const FString& Pointer, const FString& Message) const
	{
		Errors.Add(FMadDefinitionError{ SourcePath, Pointer, Message });
	}

	static FString DescribeType(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("nothing");
		}

		switch (Value->Type)
		{
		case EJson::Null:    return TEXT("null");
		case EJson::String:  return TEXT("a string");
		case EJson::Number:  return TEXT("a number");
		case EJson::Boolean: return TEXT("a boolean");
		case EJson::Array:   return TEXT("an array");
		case EJson::Object:  return TEXT("an object");
		default:             return TEXT("an unknown type");
		}
	}

	void TypeError(const FString& Pointer, const TSharedPtr<FJsonValue>& Value, const TCHAR* Expected) const
	{
		AddError(Pointer, FString::Printf(TEXT("expected %s, got %s"), Expected, *DescribeType(Value)));
	}

	bool ReadString(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Pointer, FString& Out) const
	{
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Field);
		if (!Value.IsValid() || Value->Type == EJson::Null) { return false; }
		if (Value->Type != EJson::String) { TypeError(Pointer, Value, TEXT("a string")); return false; }
		Out = Value->AsString();
		return true;
	}

	bool ReadName(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Pointer, FName& Out) const
	{
		FString Text;
		if (!ReadString(Obj, Field, Pointer, Text)) { return false; }
		Out = FName(*Text);
		return true;
	}

	bool ReadFloat(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Pointer, float& Out) const
	{
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Field);
		if (!Value.IsValid() || Value->Type == EJson::Null) { return false; }
		if (Value->Type != EJson::Number) { TypeError(Pointer, Value, TEXT("a number")); return false; }
		Out = static_cast<float>(Value->AsNumber());
		return true;
	}

	bool ReadInt(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Pointer, int32& Out) const
	{
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Field);
		if (!Value.IsValid() || Value->Type == EJson::Null) { return false; }
		if (Value->Type != EJson::Number) { TypeError(Pointer, Value, TEXT("a number")); return false; }
		Out = static_cast<int32>(FMath::RoundToDouble(Value->AsNumber()));
		return true;
	}

	bool ReadBool(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Pointer, bool& Out) const
	{
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Field);
		if (!Value.IsValid() || Value->Type == EJson::Null) { return false; }
		if (Value->Type != EJson::Boolean) { TypeError(Pointer, Value, TEXT("a boolean")); return false; }
		Out = Value->AsBool();
		return true;
	}

	bool ReadObject(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Pointer,
		TSharedPtr<FJsonObject>& Out) const
	{
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Field);
		if (!Value.IsValid() || Value->Type == EJson::Null) { return false; }
		if (Value->Type != EJson::Object) { TypeError(Pointer, Value, TEXT("an object")); return false; }
		Out = Value->AsObject();
		return Out.IsValid();
	}

	bool ReadArray(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Pointer,
		TArray<TSharedPtr<FJsonValue>>& Out) const
	{
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Field);
		if (!Value.IsValid() || Value->Type == EJson::Null) { return false; }
		if (Value->Type != EJson::Array) { TypeError(Pointer, Value, TEXT("an array")); return false; }
		Out = Value->AsArray();
		return true;
	}

	bool ReadNameArray(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Pointer,
		TArray<FName>& Out) const
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		if (!ReadArray(Obj, Field, Pointer, Items)) { return false; }

		TArray<FName> Parsed;
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			if (!Items[Index].IsValid() || Items[Index]->Type != EJson::String)
			{
				AddError(FString::Printf(TEXT("%s/%d"), *Pointer, Index),
					FString::Printf(TEXT("expected a string, got %s"), *DescribeType(Items[Index])));
				continue;
			}
			Parsed.Add(FName(*Items[Index]->AsString()));
		}

		// Arrays REPLACE on inheritance; maps merge. Documented in
		// docs/MODDING.md, and this is where the array half is implemented.
		Out = MoveTemp(Parsed);
		return true;
	}

	/**
	 * A two-element `[min, max]` array, the shape every climate range uses.
	 *
	 * Reversed bounds are corrected rather than rejected: a biome that would
	 * otherwise match nothing at all is a silent disappearance, and a swapped
	 * pair is obviously a typo rather than an intent.
	 */
	bool ReadRange(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Pointer,
		float& OutMin, float& OutMax) const
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		if (!ReadArray(Obj, Field, Pointer, Items)) { return false; }

		if (Items.Num() != 2)
		{
			AddError(Pointer, FString::Printf(TEXT("expected exactly 2 numbers [min, max], got %d"), Items.Num()));
			return false;
		}

		for (int32 Index = 0; Index < 2; ++Index)
		{
			if (!Items[Index].IsValid() || Items[Index]->Type != EJson::Number)
			{
				AddError(FString::Printf(TEXT("%s/%d"), *Pointer, Index),
					FString::Printf(TEXT("expected a number, got %s"), *DescribeType(Items[Index])));
				return false;
			}
		}

		OutMin = static_cast<float>(Items[0]->AsNumber());
		OutMax = static_cast<float>(Items[1]->AsNumber());

		if (OutMin > OutMax)
		{
			AddError(Pointer, FString::Printf(
				TEXT("min (%.3f) is greater than max (%.3f); the bounds have been swapped"), OutMin, OutMax));
			Swap(OutMin, OutMax);
		}

		return true;
	}

	/** A three-number `[x, y, z]` array. */
	bool ReadVector(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Pointer, FVector& Out) const
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		if (!ReadArray(Obj, Field, Pointer, Items)) { return false; }
		if (Items.Num() != 3 || Items.ContainsByPredicate([](const TSharedPtr<FJsonValue>& V) { return !V.IsValid() || V->Type != EJson::Number; }))
		{
			AddError(Pointer, TEXT("expected [x, y, z] (three numbers)"));
			return false;
		}
		Out = FVector(Items[0]->AsNumber(), Items[1]->AsNumber(), Items[2]->AsNumber());
		return true;
	}

	bool ReadSoftPath(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Pointer,
		FSoftObjectPath& Out) const
	{
		FString Text;
		if (!ReadString(Obj, Field, Pointer, Text)) { return false; }

		// A "madfall:..." id here is a category mistake worth naming: asset
		// references are content paths, ids are registry keys.
		if (Text.Contains(TEXT(":")) && !Text.StartsWith(TEXT("/")))
		{
			AddError(Pointer, FString::Printf(
				TEXT("'%s' looks like a namespaced id, but this field expects a content path like /MadFall/..."), *Text));
			return false;
		}

		Out = FSoftObjectPath(Text);
		return true;
	}

	/**
	 * Reports fields the parser does not know, so a typo is a visible error
	 * rather than a field that silently does nothing.
	 */
	void ReportUnknownFields(const TSharedRef<FJsonObject>& Obj, const TSet<FString>& KnownKeys) const
	{
		for (const auto& Pair : Obj->Values)
		{
			if (!KnownKeys.Contains(FString(Pair.Key)))
			{
				AddError(FString::Printf(TEXT("/%s"), *Pair.Key),
					TEXT("unknown field; it will be ignored. Check the spelling against docs/MODDING.md."));
			}
		}
	}
};
