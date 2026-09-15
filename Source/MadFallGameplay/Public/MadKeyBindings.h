// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

/** Why a rebinding was refused. */
enum class EMadBindResult : uint8
{
	Ok,
	UnknownAction,
	InvalidKey,
	/** Escape, the mouse buttons and wheel, and 1-9: see FMadKeyBindings. */
	ReservedKey
};

/** A keyboard action a player may rebind. */
struct MADFALLGAMEPLAY_API FMadRebindableAction
{
	FName Id;
	FKey Default;

	/** String key for the controls page, "@menu.key_<id>". */
	FString Label;
};

/**
 * Which key each rebindable action uses.
 *
 * Only keyboard actions are rebindable. The mouse buttons and wheel, Escape and
 * the hotbar number keys are reserved: a player who rebinds Escape away can no
 * longer reach the menu that would undo it, and the mouse and number keys are
 * the ones every survival game shares, which the help page and tooltips name.
 *
 * A key already used by another action is swapped rather than refused: the
 * other action takes the key this one gave up, so no action is ever left
 * unbound and a player never has to clear a key before reusing it.
 *
 * Stored as overrides only (action id -> key name), so a default changed in a
 * later version reaches players who never rebound that action.
 */
class MADFALLGAMEPLAY_API FMadKeyBindings
{
public:
	static const TArray<FMadRebindableAction>& GetActions();
	static const FMadRebindableAction* FindAction(FName Id);
	static bool IsReserved(const FKey& Key);

	/** The key an action uses now; an invalid key for an unknown action. */
	FKey Get(FName Action) const;

	/** Binds Key to Action, swapping with whichever action had it. */
	EMadBindResult Set(FName Action, const FKey& Key);

	void ResetToDefaults() { Overrides.Reset(); }

	/** Action id -> key name, for every action not on its default. */
	TMap<FName, FString> ToOverrides() const;

	/**
	 * Replaces the bindings. Entries for unknown actions, invalid or reserved
	 * keys are dropped; two actions claiming one key keep the first in action
	 * order and the other returns to its default (or loses the clash if that is
	 * taken too).
	 */
	void FromOverrides(const TMap<FName, FString>& In);

	bool operator==(const FMadKeyBindings& Other) const { return ToOverrides().OrderIndependentCompareEqual(Other.ToOverrides()); }

private:
	TMap<FName, FKey> Overrides;
};

namespace MadFall::Input
{
	MADFALLGAMEPLAY_API const TCHAR* ToString(EMadBindResult Result);

	/** The bindings the game is using. Settings apply to this; the player maps keys from it. */
	MADFALLGAMEPLAY_API const FMadKeyBindings& GetActive();
	MADFALLGAMEPLAY_API void SetActive(const FMadKeyBindings& Bindings);

	/** Broadcast after SetActive, so a spawned survivor remaps its keys at once. */
	DECLARE_MULTICAST_DELEGATE(FOnBindingsChanged);
	MADFALLGAMEPLAY_API FOnBindingsChanged& OnBindingsChanged();

	/** A key name for on-screen hints: "E", "Tab", "Left Shift". */
	MADFALLGAMEPLAY_API FString GetKeyLabel(FName Action);
}
