// Copyright MadFall. All Rights Reserved.

#include "MadLocalization.h"

#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "MadDefinitionSources.h"
#include "MadFallCore.h"

// ===========================================================================
// Table
// ===========================================================================

void FMadStringTable::Reset()
{
	Strings.Reset();
}

bool FMadStringTable::IsValidKey(const FString& Key)
{
	// Lower-case dotted segments: "items.wood_plank", "my_mod.hud.hint".
	if (Key.IsEmpty() || Key.Len() > 128 || Key.StartsWith(TEXT(".")) || Key.EndsWith(TEXT(".")) || Key.Contains(TEXT("..")))
	{
		return false;
	}
	for (TCHAR C : Key)
	{
		const bool bOk = (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('0') && C <= TEXT('9')) || C == TEXT('_') || C == TEXT('.');
		if (!bOk)
		{
			return false;
		}
	}
	return true;
}

bool FMadStringTable::AddJson(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	auto Error = [&](const TCHAR* Pointer, const FString& Message)
	{
		OutErrors.Add(FMadDefinitionError{ SourcePath, Pointer, Message });
	};

	FString Schema;
	if (!Object->TryGetStringField(TEXT("schema"), Schema) || Schema != MadFall::StringsSchemaV1)
	{
		Error(TEXT("/schema"), FString::Printf(TEXT("must be \"%s\""), MadFall::StringsSchemaV1));
		return false;
	}

	FString FileLanguage;
	if (!Object->TryGetStringField(TEXT("language"), FileLanguage) || FileLanguage.Len() != 2
		|| !FChar::IsLower(FileLanguage[0]) || !FChar::IsLower(FileLanguage[1]))
	{
		Error(TEXT("/language"), TEXT("must be a two-letter lower-case language code such as \"en\""));
		return false;
	}

	const TSharedPtr<FJsonObject>* Entries = nullptr;
	if (!Object->TryGetObjectField(TEXT("strings"), Entries) || Entries == nullptr)
	{
		Error(TEXT("/strings"), TEXT("must be an object of key: text"));
		return false;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
	{
		if (Pair.Key != TEXT("schema") && Pair.Key != TEXT("language") && Pair.Key != TEXT("strings"))
		{
			Error(*FString::Printf(TEXT("/%s"), *Pair.Key), TEXT("warning: unknown field"));
		}
	}

	TMap<FString, FString>& Table = Strings.FindOrAdd(FileLanguage);
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Entries)->Values)
	{
		const FString Pointer = FString::Printf(TEXT("/strings/%s"), *Pair.Key);
		if (!IsValidKey(Pair.Key))
		{
			Error(*Pointer, TEXT("keys are lower-case letters, digits, '_' and '.' separated segments"));
			continue;
		}
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
		{
			Error(*Pointer, TEXT("must be a string"));
			continue;
		}
		Table.Add(Pair.Key, Pair.Value->AsString());
	}
	return true;
}

int32 FMadStringTable::AddFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	int32 Accepted = 0;
	MadFall::Definitions::ForEachJsonObject(Directory, OutErrors,
		[&](const FString& File, const TSharedRef<FJsonObject>& Object) { Accepted += AddJson(Object, File, ModId, OutErrors) ? 1 : 0; });
	return Accepted;
}

void FMadStringTable::SetLanguage(const FString& InLanguage)
{
	Language = InLanguage.ToLower();
}

const FString* FMadStringTable::Find(const FString& Key, const FString& InLanguage) const
{
	const TMap<FString, FString>* Table = Strings.Find(InLanguage);
	return Table ? Table->Find(Key) : nullptr;
}

FString FMadStringTable::Resolve(const FString& TextOrKey) const
{
	if (!TextOrKey.StartsWith(TEXT("@")))
	{
		return TextOrKey;
	}

	const FString Key = TextOrKey.Mid(1);
	if (const FString* Text = Find(Key, Language))
	{
		return *Text;
	}
	if (const FString* English = Find(Key, TEXT("en")))
	{
		return *English;
	}
	return MakeReadable(Key);
}

TArray<FString> FMadStringTable::GetLanguages() const
{
	TArray<FString> Out;
	for (const TPair<FString, TMap<FString, FString>>& Pair : Strings)
	{
		if (Pair.Value.Num() > 0)
		{
			Out.Add(Pair.Key);
		}
	}
	Out.Sort();
	return Out;
}

int32 FMadStringTable::NumStrings(const FString& InLanguage) const
{
	const TMap<FString, FString>* Table = Strings.Find(InLanguage);
	return Table ? Table->Num() : 0;
}

TArray<FString> FMadStringTable::GetKeys(const FString& InLanguage) const
{
	TArray<FString> Out;
	if (const TMap<FString, FString>* Table = Strings.Find(InLanguage))
	{
		Table->GetKeys(Out);
	}
	Out.Sort();
	return Out;
}

FString FMadStringTable::MakeReadable(const FString& Key)
{
	FString Last = Key;
	int32 Dot = INDEX_NONE;
	if (Key.FindLastChar(TEXT('.'), Dot))
	{
		Last = Key.Mid(Dot + 1);
	}
	// Ids passed through without a key prefix ("madfall:wood_plank") read the same way.
	int32 Colon = INDEX_NONE;
	if (Last.FindLastChar(TEXT(':'), Colon))
	{
		Last = Last.Mid(Colon + 1);
	}

	FString Out;
	bool bWordStart = true;
	for (TCHAR C : Last)
	{
		if (C == TEXT('_') || C == TEXT('/'))
		{
			Out.AppendChar(TEXT(' '));
			bWordStart = true;
			continue;
		}
		Out.AppendChar(bWordStart ? FChar::ToUpper(C) : C);
		bWordStart = false;
	}
	return Out;
}

// ===========================================================================
// Global table
// ===========================================================================

namespace
{
	TUniquePtr<FMadStringTable> GStrings;

	FString ResolveLanguage(const FString& Requested)
	{
		if (!Requested.IsEmpty())
		{
			return Requested.ToLower();
		}
		const FString System = FInternationalization::Get().GetCurrentLanguage()->GetTwoLetterISOLanguageName();
		return System.IsEmpty() ? FString(TEXT("en")) : System.ToLower();
	}

	void OnLanguageChanged(IConsoleVariable* Variable);

	TAutoConsoleVariable<FString> CVarLanguage(
		TEXT("mad.Language"),
		TEXT(""),
		TEXT("Two-letter language for MadFall text (\"en\", \"de\"). Empty follows the operating system."),
		FConsoleVariableDelegate::CreateStatic(&OnLanguageChanged),
		ECVF_Default);

	void OnLanguageChanged(IConsoleVariable* Variable)
	{
		if (GStrings.IsValid())
		{
			GStrings->SetLanguage(ResolveLanguage(Variable->GetString()));
		}
	}

	FAutoConsoleCommand CmdStrings(
		TEXT("mad.strings"),
		TEXT("Lists loaded languages and the current one. mad.strings <@key> resolves one key."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const FMadStringTable& Table = MadFall::GetStrings();
			if (Args.Num() > 0)
			{
				UE_LOG(LogMadFallRegistry, Display, TEXT("%s -> \"%s\" (%s)"), *Args[0], *Table.Resolve(Args[0]), *Table.GetLanguage());
				return;
			}
			FString Languages;
			for (const FString& Language : Table.GetLanguages())
			{
				Languages += FString::Printf(TEXT(" %s(%d)"), *Language, Table.NumStrings(Language));
			}
			UE_LOG(LogMadFallRegistry, Display, TEXT("Strings: current language %s; loaded:%s"), *Table.GetLanguage(), *Languages);
		}));
}

namespace MadFall
{
	FMadStringTable& GetStrings()
	{
		if (!GStrings.IsValid())
		{
			GStrings = MakeUnique<FMadStringTable>();

			TArray<FMadDefinitionError> Errors;
			int32 Files = 0;
			Definitions::ForEachSource(TEXT("strings"), [&](const FString& Directory, FName ModId)
			{
				Files += GStrings->AddFromDirectory(Directory, ModId, Errors);
			});
			GStrings->SetLanguage(ResolveLanguage(CVarLanguage.GetValueOnAnyThread()));

			for (const FMadDefinitionError& Error : Errors)
			{
				UE_LOG(LogMadFallRegistry, Warning, TEXT("%s"), *Error.ToString());
			}
			UE_LOG(LogMadFallRegistry, Log, TEXT("Strings: %d file(s), %d English string(s), language %s."),
				Files, GStrings->NumStrings(TEXT("en")), *GStrings->GetLanguage());
		}
		return *GStrings;
	}

	FString Localize(const FString& TextOrKey)
	{
		return GetStrings().Resolve(TextOrKey);
	}
}
