// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadBlockDefinitionJson.h"

class FJsonObject;

namespace MadFall
{
	inline const TCHAR* StringsSchemaV1 = TEXT("madfall.strings/1");
}

/**
 * Player-facing text, by key and language.
 *
 * Definitions never hold display text directly when it should be translatable:
 * they hold "@key" (display_name "@items.wood_plank") and the table turns the
 * key into text for the current language. Files live in
 * `definitions/strings/<anything>.json`:
 *
 *   { "schema": "madfall.strings/1", "language": "en",
 *     "strings": { "items.wood_plank": "Wood Plank" } }
 *
 * WHY NOT UNREAL'S FText / .locres
 *   String tables and .locres files are cooked assets produced by the
 *   localization dashboard. A data mod has no editor and no cook step, and a
 *   translation pack is exactly the mod the community writes first. JSON loaded
 *   through the same sources and load order as every other definition keeps a
 *   translation a zip of text files. The tradeoff is no plural/gender rules or
 *   ICU formatting; Phase 7 can resolve through FText::FromStringTable once
 *   there is UI text that needs them.
 *
 * LOOKUP
 *   current language -> English -> a readable fallback made from the key's last
 *   segment ("@items.wood_plank" -> "Wood Plank"). A missing translation shows
 *   something sensible rather than a raw key, and the shipped-content test is
 *   what keeps English complete.
 *
 * Later sources override earlier ones key by key, in mod load order, so a mod
 * can retranslate or rename first-party text without replacing whole files.
 */
class MADFALLCORE_API FMadStringTable
{
public:
	void Reset();

	/** Stages one strings object. Returns false (with errors) if it is not a valid strings file. */
	bool AddJson(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId, TArray<FMadDefinitionError>& OutErrors);

	/** Adds every strings file under Directory. Returns the number of files accepted. */
	int32 AddFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors);

	/** Two-letter language code, lower case ("en", "de", "pt"). */
	void SetLanguage(const FString& InLanguage);
	const FString& GetLanguage() const { return Language; }

	/** "@key" to text; anything not starting with '@' is returned unchanged. */
	FString Resolve(const FString& TextOrKey) const;

	/** Text for a key (no '@') in one language, or null. */
	const FString* Find(const FString& Key, const FString& InLanguage) const;

	/** Languages with at least one string, sorted. */
	TArray<FString> GetLanguages() const;

	int32 NumStrings(const FString& InLanguage) const;

	/** Every key a language defines, sorted. */
	TArray<FString> GetKeys(const FString& InLanguage) const;

	/** "items.wood_plank" -> "Wood Plank". */
	static FString MakeReadable(const FString& Key);

	static bool IsValidKey(const FString& Key);

private:
	/** Language -> key -> text. */
	TMap<FString, TMap<FString, FString>> Strings;
	FString Language = TEXT("en");
};

namespace MadFall
{
	/**
	 * The process-wide table, loaded on first use from every source in load
	 * order. Its language follows the mad.Language console variable, or the
	 * operating system's language when that is empty.
	 */
	MADFALLCORE_API FMadStringTable& GetStrings();

	/** Shorthand for GetStrings().Resolve(TextOrKey). */
	MADFALLCORE_API FString Localize(const FString& TextOrKey);
}
