// Copyright MadFall. All Rights Reserved.

#include "MadDefinitionPatches.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MadDefinitionSources.h"
#include "MadFallCore.h"

namespace
{
	/** "/a/b~1c/0" -> ["a", "b/c", "0"]. */
	bool SplitPointer(const FString& Pointer, TArray<FString>& OutTokens, FString& OutError)
	{
		OutTokens.Reset();
		if (Pointer.IsEmpty() || Pointer[0] != TEXT('/'))
		{
			OutError = FString::Printf(TEXT("path '%s' must be a JSON pointer starting with '/'"), *Pointer);
			return false;
		}
		Pointer.Mid(1).ParseIntoArray(OutTokens, TEXT("/"), /*bCullEmpty*/ false);
		for (FString& Token : OutTokens)
		{
			Token.ReplaceInline(TEXT("~1"), TEXT("/"));
			Token.ReplaceInline(TEXT("~0"), TEXT("~"));
		}
		return true;
	}

	bool ParseIndex(const FString& Token, int32 Size, bool bAllowEnd, int32& OutIndex)
	{
		if (bAllowEnd && Token == TEXT("-"))
		{
			OutIndex = Size;
			return true;
		}
		if (Token.IsEmpty() || !Token.IsNumeric())
		{
			return false;
		}
		OutIndex = FCString::Atoi(*Token);
		return OutIndex >= 0 && (OutIndex < Size || (bAllowEnd && OutIndex == Size));
	}

	/** A copy, so one parsed patch value can be applied to many definitions without aliasing. */
	TSharedPtr<FJsonValue> DeepCopy(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return Value;
		}
		switch (Value->Type)
		{
		case EJson::Array:
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				Items.Add(DeepCopy(Item));
			}
			return MakeShared<FJsonValueArray>(Items);
		}
		case EJson::Object:
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			for (const auto& Pair : Value->AsObject()->Values)
			{
				Object->SetField(Pair.Key, DeepCopy(Pair.Value));
			}
			return MakeShared<FJsonValueObject>(Object);
		}
		default:
			return Value;
		}
	}
}

namespace MadFall::JsonPatch
{
	bool Apply(const TSharedRef<FJsonObject>& Root, const FMadPatchOp& Op, FString& OutError)
	{
		TArray<FString> Tokens;
		if (!SplitPointer(Op.Path, Tokens, OutError))
		{
			return false;
		}
		if (Tokens.Num() == 0 || (Tokens.Num() == 1 && Tokens[0].IsEmpty()))
		{
			OutError = TEXT("cannot patch the whole definition; name a field");
			return false;
		}

		// Walk to the parent of the last token. Set and Append create missing
		// objects on the way; Remove requires the path to exist.
		const bool bCreate = Op.Type != FMadPatchOp::EType::Remove;
		TSharedPtr<FJsonValue> Parent = MakeShared<FJsonValueObject>(Root);

		for (int32 Depth = 0; Depth < Tokens.Num() - 1; ++Depth)
		{
			const FString& Token = Tokens[Depth];
			TSharedPtr<FJsonValue> Next;

			if (Parent->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Object = Parent->AsObject();
				Next = Object->TryGetField(Token);
				if (!Next.IsValid() || Next->Type == EJson::Null)
				{
					if (!bCreate)
					{
						OutError = FString::Printf(TEXT("'%s' does not exist"), *Op.Path);
						return false;
					}
					Next = MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
					Object->SetField(Token, Next);
				}
			}
			else if (Parent->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& Items = Parent->AsArray();
				int32 Index = 0;
				if (!ParseIndex(Token, Items.Num(), false, Index))
				{
					OutError = FString::Printf(TEXT("'%s': '%s' is not a valid index into an array of %d"), *Op.Path, *Token, Items.Num());
					return false;
				}
				Next = Items[Index];
			}
			else
			{
				OutError = FString::Printf(TEXT("'%s': '%s' is not an object or array"), *Op.Path, *Token);
				return false;
			}
			Parent = Next;
		}

		const FString& Last = Tokens.Last();

		if (Parent->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Object = Parent->AsObject();
			switch (Op.Type)
			{
			case FMadPatchOp::EType::Set:
				Object->SetField(Last, DeepCopy(Op.Value));
				return true;

			case FMadPatchOp::EType::Append:
			{
				TSharedPtr<FJsonValue> Existing = Object->TryGetField(Last);
				TArray<TSharedPtr<FJsonValue>> Items;
				if (Existing.IsValid() && Existing->Type != EJson::Null)
				{
					if (Existing->Type != EJson::Array)
					{
						OutError = FString::Printf(TEXT("'%s' is not an array; use set instead of append"), *Op.Path);
						return false;
					}
					Items = Existing->AsArray();
				}
				Items.Add(DeepCopy(Op.Value));
				Object->SetArrayField(Last, Items);
				return true;
			}

			case FMadPatchOp::EType::Remove:
				if (!Object->HasField(Last))
				{
					OutError = FString::Printf(TEXT("'%s' does not exist"), *Op.Path);
					return false;
				}
				Object->RemoveField(Last);
				return true;
			}
		}
		else if (Parent->Type == EJson::Array)
		{
			// FJsonValueArray exposes its array only by const reference, so arrays
			// are edited by rebuilding them - they are small definition arrays.
			TArray<TSharedPtr<FJsonValue>> Items = Parent->AsArray();
			int32 Index = 0;
			const bool bAllowEnd = Op.Type != FMadPatchOp::EType::Remove;
			if (!ParseIndex(Last, Items.Num(), bAllowEnd, Index))
			{
				OutError = FString::Printf(TEXT("'%s': '%s' is not a valid index into an array of %d"), *Op.Path, *Last, Items.Num());
				return false;
			}

			switch (Op.Type)
			{
			case FMadPatchOp::EType::Set:
				if (Index == Items.Num()) { Items.Add(DeepCopy(Op.Value)); }
				else { Items[Index] = DeepCopy(Op.Value); }
				break;
			case FMadPatchOp::EType::Append:
				Items.Insert(DeepCopy(Op.Value), Index);
				break;
			case FMadPatchOp::EType::Remove:
				Items.RemoveAt(Index);
				break;
			}

			// Write the rebuilt array back into its owner by re-walking one level up.
			FMadPatchOp Replace;
			Replace.Type = FMadPatchOp::EType::Set;
			Replace.Path = TEXT("/") + FString::Join(TArrayView<const FString>(Tokens.GetData(), Tokens.Num() - 1), TEXT("/"));
			Replace.Value = MakeShared<FJsonValueArray>(Items);
			return Apply(Root, Replace, OutError);
		}

		OutError = FString::Printf(TEXT("'%s': the parent is not an object or array"), *Op.Path);
		return false;
	}

	bool Parse(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
		FMadDefinitionPatch& Out, TArray<FMadDefinitionError>& OutErrors)
	{
		auto Error = [&](const FString& Pointer, const FString& Message)
		{
			OutErrors.Add(FMadDefinitionError{ SourcePath, Pointer, Message });
		};

		FString Schema;
		if (!Object->TryGetStringField(TEXT("schema"), Schema) || Schema != MadFall::PatchSchemaV1)
		{
			Error(TEXT("/schema"), FString::Printf(TEXT("expected \"%s\""), MadFall::PatchSchemaV1));
			return false;
		}

		static const TSet<FString> Kinds = { TEXT("block"), TEXT("biome"), TEXT("item"), TEXT("recipe"), TEXT("loot"), TEXT("zombie"), TEXT("animal"), TEXT("quest"), TEXT("tuning"), TEXT("perk"), TEXT("trader"), TEXT("surface") };
		FString Kind;
		if (!Object->TryGetStringField(TEXT("kind"), Kind) || !Kinds.Contains(Kind))
		{
			Error(TEXT("/kind"), TEXT("must be one of block, biome, item, recipe, loot, zombie, animal, quest, tuning, perk, trader, surface"));
			return false;
		}

		FString Target;
		if (!Object->TryGetStringField(TEXT("target"), Target) || Target.IsEmpty())
		{
			Error(TEXT("/target"), TEXT("missing the id of the definition to patch"));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
		if (!Object->TryGetArrayField(TEXT("ops"), Ops) || Ops->Num() == 0)
		{
			Error(TEXT("/ops"), TEXT("a patch needs a non-empty \"ops\" array"));
			return false;
		}

		Out = FMadDefinitionPatch();
		Out.Kind = FName(*Kind);
		Out.Target = FName(*Target);
		Out.ModId = ModId;
		Out.SourcePath = SourcePath;

		for (int32 Index = 0; Index < Ops->Num(); ++Index)
		{
			const FString Pointer = FString::Printf(TEXT("/ops/%d"), Index);
			const TSharedPtr<FJsonObject>* OpObject = nullptr;
			FString OpName;
			FString Path;
			if (!(*Ops)[Index]->TryGetObject(OpObject) || !(*OpObject)->TryGetStringField(TEXT("op"), OpName)
				|| !(*OpObject)->TryGetStringField(TEXT("path"), Path))
			{
				Error(Pointer, TEXT("each op needs \"op\" and \"path\""));
				return false;
			}

			FMadPatchOp Op;
			Op.Path = Path;
			if (OpName == TEXT("set"))         { Op.Type = FMadPatchOp::EType::Set; }
			else if (OpName == TEXT("append")) { Op.Type = FMadPatchOp::EType::Append; }
			else if (OpName == TEXT("remove")) { Op.Type = FMadPatchOp::EType::Remove; }
			else
			{
				Error(Pointer + TEXT("/op"), FString::Printf(TEXT("unknown op '%s'; expected set, append or remove"), *OpName));
				return false;
			}

			if (Op.Type != FMadPatchOp::EType::Remove)
			{
				Op.Value = (*OpObject)->TryGetField(TEXT("value"));
				if (!Op.Value.IsValid())
				{
					Error(Pointer + TEXT("/value"), FString::Printf(TEXT("'%s' needs a \"value\""), *OpName));
					return false;
				}
			}
			Out.Ops.Add(MoveTemp(Op));
		}
		return true;
	}
}

// ===========================================================================
// Patch set
// ===========================================================================

void FMadPatchSet::Add(FMadDefinitionPatch&& Patch)
{
	Patches.Add(MoveTemp(Patch));
}

void FMadPatchSet::LoadFromSources(TArray<FMadDefinitionError>& OutErrors)
{
	Patches.Reset();
	MadFall::Definitions::ForEachSource(TEXT("patches"), [&](const FString& Directory, FName ModId)
	{
		MadFall::Definitions::ForEachJsonObject(Directory, OutErrors, [&](const FString& File, const TSharedRef<FJsonObject>& Object)
		{
			FMadDefinitionPatch Patch;
			if (MadFall::JsonPatch::Parse(Object, File, ModId, Patch, OutErrors))
			{
				Patches.Add(MoveTemp(Patch));
			}
		});
	});
}

int32 FMadPatchSet::ApplyTo(FName Kind, FName Target, const TSharedRef<FJsonObject>& Object, TArray<FMadDefinitionError>& OutErrors) const
{
	int32 Applied = 0;
	TMap<FString, const FMadDefinitionPatch*> Writers;

	for (const FMadDefinitionPatch& Patch : Patches)
	{
		if (Patch.Kind != Kind || Patch.Target != Target)
		{
			continue;
		}

		for (int32 Index = 0; Index < Patch.Ops.Num(); ++Index)
		{
			const FMadPatchOp& Op = Patch.Ops[Index];

			if (Op.Type != FMadPatchOp::EType::Append)
			{
				if (const FMadDefinitionPatch* const* Previous = Writers.Find(Op.Path); Previous && (*Previous)->ModId != Patch.ModId)
				{
					OutErrors.Add(FMadDefinitionError{ Patch.SourcePath, FString::Printf(TEXT("/ops/%d"), Index), FString::Printf(
						TEXT("warning: %s %s is also written by mod '%s' (%s); '%s' loads later and wins"),
						*Target.ToString(), *Op.Path, *(*Previous)->ModId.ToString(), *(*Previous)->SourcePath, *Patch.ModId.ToString()) });
				}
				Writers.Add(Op.Path, &Patch);
			}

			FString Error;
			if (MadFall::JsonPatch::Apply(Object, Op, Error))
			{
				++Applied;
			}
			else
			{
				OutErrors.Add(FMadDefinitionError{ Patch.SourcePath, FString::Printf(TEXT("/ops/%d"), Index),
					FString::Printf(TEXT("patch on %s not applied: %s"), *Target.ToString(), *Error) });
			}
		}
	}
	return Applied;
}

void FMadPatchSet::ReportUnmatched(FName Kind, const TSet<FName>& KnownIds, TArray<FMadDefinitionError>& OutErrors) const
{
	for (const FMadDefinitionPatch& Patch : Patches)
	{
		if (Patch.Kind == Kind && !KnownIds.Contains(Patch.Target))
		{
			OutErrors.Add(FMadDefinitionError{ Patch.SourcePath, TEXT("/target"), FString::Printf(
				TEXT("no %s named '%s' is defined; the patch does nothing (is the mod that defines it installed?)"),
				*Kind.ToString(), *Patch.Target.ToString()) });
		}
	}
}

namespace
{
	TUniquePtr<FMadPatchSet> GPatchSet;
}

const FMadPatchSet& MadFall::GetPatchSet()
{
	if (!GPatchSet.IsValid())
	{
		GPatchSet = MakeUnique<FMadPatchSet>();
		TArray<FMadDefinitionError> Errors;
		GPatchSet->LoadFromSources(Errors);
		UE_LOG(LogMadFallRegistry, Log, TEXT("Definition patches: %d loaded, %d message(s)."), GPatchSet->Num(), Errors.Num());
		for (const FMadDefinitionError& Error : Errors)
		{
			UE_LOG(LogMadFallRegistry, Warning, TEXT("%s"), *Error.ToString());
		}
	}
	return *GPatchSet;
}
