// Copyright MadFall. All Rights Reserved.

#include "MadModManifest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MadFallApiVersion.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ===========================================================================
// Versions
// ===========================================================================

bool FMadSemanticVersion::Parse(const FString& InText, FMadSemanticVersion& Out)
{
	FString Text = InText.TrimStartAndEnd();

	// "1.2.3-beta+build7": ordering ignores the suffixes.
	int32 Suffix = INDEX_NONE;
	if (Text.FindChar(TEXT('-'), Suffix) || Text.FindChar(TEXT('+'), Suffix))
	{
		Text.LeftInline(Suffix);
	}

	TArray<FString> Parts;
	Text.ParseIntoArray(Parts, TEXT("."), /*bCullEmpty*/ false);
	if (Parts.Num() < 1 || Parts.Num() > 3)
	{
		return false;
	}

	int32 Values[3] = { 0, 0, 0 };
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		if (Parts[Index].IsEmpty() || !Parts[Index].IsNumeric() || Parts[Index].Contains(TEXT(".")) || Parts[Index].Contains(TEXT("-")))
		{
			return false;
		}
		Values[Index] = FCString::Atoi(*Parts[Index]);
	}

	Out.Major = Values[0];
	Out.Minor = Values[1];
	Out.Patch = Values[2];
	return true;
}

bool FMadVersionConstraint::Parse(const FString& Text, FMadVersionConstraint& Out, FString& OutError)
{
	Out = FMadVersionConstraint();
	Out.Source = Text;

	TArray<FString> Parts;
	Text.ParseIntoArray(Parts, TEXT(","), /*bCullEmpty*/ true);
	if (Parts.Num() == 0)
	{
		OutError = TEXT("empty version constraint; use \"*\" for any version");
		return false;
	}

	for (FString Part : Parts)
	{
		Part.TrimStartAndEndInline();
		if (Part == TEXT("*"))
		{
			continue;
		}

		auto AddClause = [&Out](EOp Op, const FMadSemanticVersion& Version) { Out.Clauses.Add(FClause{ Op, Version }); };

		FMadSemanticVersion Version;
		const TCHAR* Ops[] = { TEXT(">="), TEXT("<="), TEXT(">"), TEXT("<"), TEXT("^"), TEXT("~"), TEXT("=") };
		FString Prefix;
		for (const TCHAR* Op : Ops)
		{
			if (Part.StartsWith(Op))
			{
				Prefix = Op;
				break;
			}
		}

		if (!FMadSemanticVersion::Parse(Part.Mid(Prefix.Len()), Version))
		{
			OutError = FString::Printf(TEXT("'%s' is not a version or a version requirement"), *Part);
			return false;
		}

		if (Prefix == TEXT(">="))      { AddClause(EOp::GreaterEqual, Version); }
		else if (Prefix == TEXT("<=")) { AddClause(EOp::LessEqual, Version); }
		else if (Prefix == TEXT(">"))  { AddClause(EOp::Greater, Version); }
		else if (Prefix == TEXT("<"))  { AddClause(EOp::Less, Version); }
		else if (Prefix == TEXT("^"))
		{
			// Caret: compatible within the leftmost non-zero component, as in npm and cargo.
			AddClause(EOp::GreaterEqual, Version);
			FMadSemanticVersion Upper;
			if (Version.Major > 0)      { Upper.Major = Version.Major + 1; }
			else if (Version.Minor > 0) { Upper.Minor = Version.Minor + 1; }
			else                        { Upper.Patch = Version.Patch + 1; }
			AddClause(EOp::Less, Upper);
		}
		else if (Prefix == TEXT("~"))
		{
			AddClause(EOp::GreaterEqual, Version);
			FMadSemanticVersion Upper = Version;
			Upper.Minor += 1;
			Upper.Patch = 0;
			AddClause(EOp::Less, Upper);
		}
		else
		{
			AddClause(EOp::Equal, Version);
		}
	}
	return true;
}

bool FMadVersionConstraint::IsSatisfiedBy(const FMadSemanticVersion& Version) const
{
	for (const FClause& Clause : Clauses)
	{
		const int32 C = Version.Compare(Clause.Version);
		bool bOk = false;
		switch (Clause.Op)
		{
		case EOp::Equal:        bOk = C == 0; break;
		case EOp::Greater:      bOk = C > 0; break;
		case EOp::GreaterEqual: bOk = C >= 0; break;
		case EOp::Less:         bOk = C < 0; break;
		case EOp::LessEqual:    bOk = C <= 0; break;
		}
		if (!bOk)
		{
			return false;
		}
	}
	return true;
}

// ===========================================================================
// Manifest
// ===========================================================================

bool FMadModManifest::IsValidModId(const FString& Id, FString& OutReason)
{
	if (Id.Len() < 2 || Id.Len() > 64)
	{
		OutReason = FString::Printf(TEXT("mod id '%s' must be 2-64 characters"), *Id);
		return false;
	}
	if (Id == MadFall::CoreModId)
	{
		OutReason = TEXT("'madfall' is reserved for first-party content");
		return false;
	}
	if (FChar::IsDigit(Id[0]))
	{
		OutReason = FString::Printf(TEXT("mod id '%s' must not start with a digit"), *Id);
		return false;
	}
	for (TCHAR Char : Id)
	{
		if (!((Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('0') && Char <= TEXT('9')) || Char == TEXT('_')))
		{
			OutReason = FString::Printf(TEXT("mod id '%s' contains '%c'; ids are lowercase a-z, 0-9 and _ because they become the namespace of every definition in the mod"), *Id, Char);
			return false;
		}
	}
	return true;
}

namespace
{
	bool ReadNameList(const TSharedPtr<FJsonObject>& Root, const TCHAR* Field, TArray<FName>& Out, TArray<FString>& Errors)
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Root->HasField(Field))
		{
			return true;
		}
		if (!Root->TryGetArrayField(Field, Items))
		{
			Errors.Add(FString::Printf(TEXT("'%s' must be an array of mod ids"), Field));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			if (!Item.IsValid() || Item->Type != EJson::String)
			{
				Errors.Add(FString::Printf(TEXT("'%s' entries must be strings"), Field));
				return false;
			}
			Out.Add(FName(*Item->AsString()));
		}
		return true;
	}

	bool ReadStringList(const TSharedPtr<FJsonObject>& Root, const TCHAR* Field, TArray<FString>& Out, TArray<FString>& Errors)
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Root->HasField(Field))
		{
			return true;
		}
		if (!Root->TryGetArrayField(Field, Items))
		{
			Errors.Add(FString::Printf(TEXT("'%s' must be an array of strings"), Field));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			if (!Item.IsValid() || Item->Type != EJson::String)
			{
				Errors.Add(FString::Printf(TEXT("'%s' entries must be strings"), Field));
				return false;
			}
			Out.Add(Item->AsString());
		}
		return true;
	}

	/** Paths inside a mod must stay inside the mod. */
	bool IsSafeRelativePath(const FString& Path)
	{
		return !Path.IsEmpty() && !Path.Contains(TEXT("..")) && !Path.StartsWith(TEXT("/")) && !Path.StartsWith(TEXT("\\"))
			&& !Path.Contains(TEXT(":"));
	}
}

bool FMadModManifest::ParseText(const FString& JsonText, const FString& InDirectory, FMadModManifest& Out, TArray<FString>& OutErrors)
{
	Out = FMadModManifest();
	Out.Directory = InDirectory;
	const int32 ErrorsBefore = OutErrors.Num();

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutErrors.Add(FString::Printf(TEXT("mod.json is not valid JSON: %s"), *Reader->GetErrorMessage()));
		return false;
	}

	FString Schema;
	if (!Root->TryGetStringField(TEXT("schema"), Schema) || Schema != MadFall::ModManifestSchemaV1)
	{
		OutErrors.Add(FString::Printf(TEXT("mod.json must declare \"schema\": \"%s\""), MadFall::ModManifestSchemaV1));
		return false;
	}

	FString Id;
	FString Reason;
	if (!Root->TryGetStringField(TEXT("id"), Id))
	{
		OutErrors.Add(TEXT("mod.json is missing \"id\""));
		return false;
	}
	if (!IsValidModId(Id, Reason))
	{
		OutErrors.Add(Reason);
		return false;
	}
	Out.Id = FName(*Id);

	FString VersionText;
	if (!Root->TryGetStringField(TEXT("version"), VersionText) || !FMadSemanticVersion::Parse(VersionText, Out.Version))
	{
		OutErrors.Add(TEXT("\"version\" must be a version like \"1.0.0\""));
	}

	Root->TryGetStringField(TEXT("name"), Out.Name);
	if (Out.Name.IsEmpty())
	{
		Out.Name = Id;
	}
	Root->TryGetStringField(TEXT("description"), Out.Description);
	ReadStringList(Root, TEXT("authors"), Out.Authors, OutErrors);

	FString ApiText;
	FMadSemanticVersion Api;
	if (!Root->TryGetStringField(TEXT("api_version"), ApiText) || !FMadSemanticVersion::Parse(ApiText, Api))
	{
		OutErrors.Add(TEXT("\"api_version\" must name the mod API version the mod targets, like \"0.1\""));
	}
	else
	{
		Out.ApiMajor = Api.Major;
		Out.ApiMinor = Api.Minor;
	}

	const TArray<TSharedPtr<FJsonValue>>* Dependencies = nullptr;
	if (Root->HasField(TEXT("dependencies")))
	{
		if (!Root->TryGetArrayField(TEXT("dependencies"), Dependencies))
		{
			OutErrors.Add(TEXT("\"dependencies\" must be an array"));
		}
		else
		{
			for (int32 Index = 0; Index < Dependencies->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				FString DepId;
				if (!(*Dependencies)[Index]->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("id"), DepId))
				{
					OutErrors.Add(FString::Printf(TEXT("dependencies[%d] must be an object with an \"id\""), Index));
					continue;
				}
				FMadModDependency& Dependency = Out.Dependencies.AddDefaulted_GetRef();
				Dependency.ModId = FName(*DepId);
				(*Object)->TryGetBoolField(TEXT("optional"), Dependency.bOptional);

				FString Constraint = TEXT("*");
				(*Object)->TryGetStringField(TEXT("version"), Constraint);
				FString ConstraintError;
				if (!FMadVersionConstraint::Parse(Constraint, Dependency.Version, ConstraintError))
				{
					OutErrors.Add(FString::Printf(TEXT("dependencies[%d]: %s"), Index, *ConstraintError));
				}
			}
		}
	}

	ReadNameList(Root, TEXT("load_after"), Out.LoadAfter, OutErrors);
	ReadNameList(Root, TEXT("load_before"), Out.LoadBefore, OutErrors);
	ReadNameList(Root, TEXT("incompatible"), Out.Incompatible, OutErrors);
	ReadStringList(Root, TEXT("paks"), Out.Paks, OutErrors);
	ReadStringList(Root, TEXT("scripts"), Out.Scripts, OutErrors);

	for (const FString& Path : Out.Paks)
	{
		if (!IsSafeRelativePath(Path))
		{
			OutErrors.Add(FString::Printf(TEXT("pak path '%s' must be relative and stay inside the mod folder"), *Path));
		}
	}
	for (const FString& Path : Out.Scripts)
	{
		if (!IsSafeRelativePath(Path))
		{
			OutErrors.Add(FString::Printf(TEXT("script path '%s' must be relative and stay inside the mod folder"), *Path));
		}
	}

	// Unknown fields are reported but do not stop the mod loading: a manifest
	// written for a newer game version should degrade, not vanish.
	const bool bLoadable = OutErrors.Num() == ErrorsBefore;

	static const TSet<FString> Known = {
		TEXT("schema"), TEXT("id"), TEXT("name"), TEXT("version"), TEXT("description"), TEXT("authors"), TEXT("api_version"),
		TEXT("dependencies"), TEXT("load_after"), TEXT("load_before"), TEXT("incompatible"), TEXT("paks"), TEXT("scripts"),
		TEXT("homepage"), TEXT("license")
	};
	for (const auto& Pair : Root->Values)
	{
		if (!Known.Contains(FString(Pair.Key)))
		{
			OutErrors.Add(FString::Printf(TEXT("warning: unknown field \"%s\" in mod.json is ignored"), *Pair.Key));
		}
	}

	return bLoadable;
}
