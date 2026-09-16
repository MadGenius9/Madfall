// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Player settings, persisted to Saved/Config/MadFallSettings.json.
 *
 * Every setting is applied by setting a console variable, so the menu, the
 * console and a -ExecCmds= line all change the same thing, and a system reads
 * one CVar rather than knowing about this struct.
 */
struct MADFALLGAMEPLAY_API FMadSettings
{
	/** mad.input.LookSensitivity */
	float LookSensitivity = 1.0f;

	/** mad.input.InvertY */
	bool bInvertY = false;

	/** mad.view.FieldOfView, degrees. */
	float FieldOfView = 90.0f;

	/** mad.stream.Radius, chunks. */
	int32 ViewDistance = 8;

	/**
	 * Graphics quality 0 (low) to 3 (epic): the engine's scalability groups all
	 * at that level, and below high no shadows from block lights
	 * (mad.models.LightShadows). One knob rather than a page of them: the
	 * engine's groups are tuned to go together, and a survivor choosing between
	 * "fast" and "pretty" should not need to know what Lumen is.
	 */
	int32 Quality = 3;

	/** mad.audio.Volume, 0..1. */
	float Volume = 0.8f;

	/** mad.audio.MusicVolume, 0..1; separate so a player can keep the effects and lose the music. */
	float MusicVolume = 0.6f;

	/**
	 * mad.ui.Scale, 0.5 to 2: how big the HUD is on top of the size the window
	 * already implies. A window with no room for the result gets the largest
	 * size that fits, so this is a preference, not a promise.
	 */
	float UiScale = 1.0f;

	/** mad.Language; empty follows the system. */
	FString Language;

	/** Rebound keys: action id -> key name, only those off their default (FMadKeyBindings). */
	TMap<FName, FString> KeyBindings;

	/** Clamps every field to its supported range. */
	void Sanitize();
};

namespace MadFall::Settings
{
	MADFALLGAMEPLAY_API FString GetPath();

	/** Reads the file (defaults for anything missing or unreadable), sanitised. */
	MADFALLGAMEPLAY_API FMadSettings Load(const FString& Path);
	MADFALLGAMEPLAY_API bool Save(const FMadSettings& Settings, const FString& Path, FString& OutError);

	/** Pushes the settings into their console variables. */
	MADFALLGAMEPLAY_API void Apply(const FMadSettings& Settings);

	/** What the console variables hold now. */
	MADFALLGAMEPLAY_API FMadSettings FromConsoleVariables();

	/** Loads and applies the saved settings once per process. */
	MADFALLGAMEPLAY_API void EnsureApplied();
}
