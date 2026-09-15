// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The Tier-3 scripting surface, on the game's side of the sandbox.
 *
 * WHY IT LIVES HERE: the approved design is that a script can reach exactly
 * what MadFallModAPI's public headers describe and nothing else. The Lua
 * runtime (MadFallScriptRuntime) depends only on this module, so it cannot see
 * the voxel world, actors or save files even by accident; the game implements
 * IMadScriptHost and hands it in. Everything crossing the boundary is plain
 * data - names, voxel coordinates, numbers, strings - never a pointer into the
 * engine.
 *
 * Everything below is header-only, so none of it carries MADFALLMODAPI_API:
 * exporting a type no ModAPI translation unit instantiates leaves its implicit
 * members undefined for importers, which fails at link time.
 */

/** A value passed between scripts and the game: nil, boolean, number or string. */
struct FMadScriptValue
{
	enum class EType : uint8
	{
		Nil,
		Bool,
		Number,
		String
	};

	EType Type = EType::Nil;
	bool Bool = false;
	double Number = 0.0;
	FString String;

	static FMadScriptValue MakeBool(bool bValue) { FMadScriptValue V; V.Type = EType::Bool; V.Bool = bValue; return V; }
	static FMadScriptValue MakeNumber(double Value) { FMadScriptValue V; V.Type = EType::Number; V.Number = Value; return V; }
	static FMadScriptValue MakeString(const FString& Value) { FMadScriptValue V; V.Type = EType::String; V.String = Value; return V; }

	bool IsNil() const { return Type == EType::Nil; }

	bool operator==(const FMadScriptValue& Other) const
	{
		return Type == Other.Type && Bool == Other.Bool && Number == Other.Number && String == Other.String;
	}
};

/** Something that happened, delivered to every script handler registered for its name. */
struct FMadScriptEvent
{
	FName Name;
	TArray<TPair<FString, FMadScriptValue>> Fields;

	FMadScriptEvent() = default;
	explicit FMadScriptEvent(FName InName) : Name(InName) {}

	FMadScriptEvent& Add(const FString& Key, const FString& Value) { Fields.Emplace(Key, FMadScriptValue::MakeString(Value)); return *this; }
	FMadScriptEvent& Add(const FString& Key, double Value) { Fields.Emplace(Key, FMadScriptValue::MakeNumber(Value)); return *this; }
	FMadScriptEvent& Add(const FString& Key, bool bValue) { Fields.Emplace(Key, FMadScriptValue::MakeBool(bValue)); return *this; }
};

/** The survivor, as a script sees them. */
struct FMadScriptPlayer
{
	/** Voxel coordinates of the feet. */
	FIntVector Voxel = FIntVector::ZeroValue;
	float Health = 0.0f;
	float MaxHealth = 0.0f;
	float Food = 0.0f;
	float Water = 0.0f;
	float CoreTemperature = 37.0f;
	int32 Level = 1;
	bool bAlive = true;
};

/**
 * What the game lets scripts do. Implemented by the game; every call is made
 * on the game thread, from inside a script handler.
 *
 * Methods that change the world take the calling mod's id, so the game can log
 * and attribute what a mod did.
 */
class IMadScriptHost
{
public:
	virtual ~IMadScriptHost() = default;

	/** A script's log line (madfall.log, print). */
	virtual void Log(FName ModId, const FString& Message) = 0;

	/** A script error or warning the runtime reports. */
	virtual void Warn(FName ModId, const FString& Message) = 0;

	/** A line on the survivor's HUD. */
	virtual void ShowMessage(FName ModId, const FString& Message) = 0;

	/** The block id at a voxel. False when the chunk is not loaded. */
	virtual bool GetBlock(const FIntVector& Voxel, FName& OutBlock) = 0;

	/** Writes a block (madfall:air to clear). False for an unknown block or an unloaded chunk. */
	virtual bool SetBlock(FName ModId, const FIntVector& Voxel, FName Block) = 0;

	/** False while there is no survivor in the world (spawning, dead, title screen). */
	virtual bool GetPlayer(FMadScriptPlayer& Out) = 0;

	/** Gives the survivor items, dropping what does not fit. Returns how many were given; 0 for an unknown item. */
	virtual int32 GiveItem(FName ModId, FName Item, int32 Count) = 0;

	/** Spawns a zombie variant standing near a voxel. */
	virtual bool SpawnZombie(FName ModId, FName Zombie, const FIntVector& Near) = 0;

	/** Day (1-based) and hour (0-24). */
	virtual void GetTime(int32& OutDay, float& OutHour) = 0;
};
