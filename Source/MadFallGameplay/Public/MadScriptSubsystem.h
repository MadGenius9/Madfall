// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadQuests.h"
#include "MadScriptHost.h"
#include "MadScriptRuntime.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadScriptSubsystem.generated.h"

class AMadPlayerCharacter;
class UMadHordeSubsystem;
class UMadVoxelWorldSubsystem;
class UMadWorldClockSubsystem;

/**
 * Runs Tier-3 script mods in a game world: the game's side of the sandbox.
 *
 * WHEN SCRIPTS LOAD
 *   After the gameplay save has been applied, one mod per frame. Waiting for
 *   the save means a script's top level can already read its store; one mod a
 *   frame keeps compiling a large script set from stalling a single frame.
 *
 * EVENTS ARE QUEUED, NOT CALLED WHERE THEY HAPPEN
 *   A block placed inside the player's tick does not run Lua inside the
 *   player's tick. Events go into a queue that this subsystem drains in its own
 *   tick, inside the frame budget (EMadFrameSystem::Scripts). Two reasons: a
 *   script that edits the world never re-enters gameplay code half way through
 *   its own update, and script time is charged and capped in one place. The
 *   cost is that a handler runs up to a frame after its event, which no script
 *   can observe in a way that matters - the event carries what it needs.
 *   Events nobody listens for are never queued at all.
 *
 *   `mad.scripts`                    each script mod: handlers, errors, memory, worst call
 *   `mad.scripts.event <name> [k=v]` queues an event by hand (testing)
 *   `mod.<mod id>.<command> ...`     commands scripts register
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadScriptSubsystem : public UTickableWorldSubsystem, public IMadScriptHost
{
	GENERATED_BODY()

public:
	UMadScriptSubsystem();
	virtual ~UMadScriptSubsystem() override;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	//~ Begin IMadScriptHost
	virtual void Log(FName ModId, const FString& Message) override;
	virtual void Warn(FName ModId, const FString& Message) override;
	virtual void ShowMessage(FName ModId, const FString& Message) override;
	virtual bool GetBlock(const FIntVector& Voxel, FName& OutBlock) override;
	virtual bool SetBlock(FName ModId, const FIntVector& Voxel, FName Block) override;
	virtual bool GetPlayer(FMadScriptPlayer& Out) override;
	virtual int32 GiveItem(FName ModId, FName Item, int32 Count) override;
	virtual bool SpawnZombie(FName ModId, FName Zombie, const FIntVector& Near) override;
	virtual void GetTime(int32& OutDay, float& OutHour) override;
	//~ End IMadScriptHost

	/** Queues an event for script handlers. Dropped at once if no handler listens. */
	void QueueEvent(FMadScriptEvent&& Event);

	/** Something the survivor did (the quest notifications), as a script event. */
	void NotifyPlayerAction(EMadQuestObjectiveType Type, FName Id, int32 Amount, const TOptional<FIntVector>& Where);

	/** Save support. Import may run before scripts load; the store waits for its mod. */
	void ExportStore(TMap<FName, TMap<FString, FMadScriptValue>>& Out) const;
	void ImportStore(const TMap<FName, TMap<FString, FMadScriptValue>>& In);

	/** Loads every remaining mod now instead of one per frame (tests). */
	void LoadAllNow();

	/** Dispatches every queued event now, ignoring the frame budget (tests). */
	void DrainAllNow();

	bool HasLoadedAll() const { return NextModToLoad >= ModsToLoad.Num(); }
	int32 NumQueued() const { return Queue.Num(); }
	FMadScriptRuntime* GetRuntime() const { return Runtime.Get(); }

	FString DescribeStatus() const;

	/** Events waiting past this are dropped, oldest first: a script falling behind must not grow without bound. */
	static constexpr int32 MaxQueuedEvents = 512;

	/** Script time wanted per frame, milliseconds, before the shared frame budget cuts it. */
	static constexpr double ScriptShareMs = 0.75;

private:
	AMadPlayerCharacter* FindPlayer() const;
	void PollState(float DeltaTime);
	void LoadNextMod();

	UPROPERTY(Transient)
	TObjectPtr<UMadVoxelWorldSubsystem> VoxelWorld;

	UPROPERTY(Transient)
	TObjectPtr<UMadWorldClockSubsystem> Clock;

	UPROPERTY(Transient)
	TObjectPtr<UMadHordeSubsystem> Horde;

	TUniquePtr<FMadScriptRuntime> Runtime;
	TArray<FMadScriptEvent> Queue;

	TArray<FMadScriptMod> ModsToLoad;
	int32 NextModToLoad = 0;
	bool bModsGathered = false;
	bool bWorldLoadedSent = false;

	FDelegateHandle DawnHandle;
	FDelegateHandle DuskHandle;

	float SecondTimer = 0.0f;
	bool bPlayerWasActive = false;
	bool bPlayerWasDead = false;
	bool bHordeWasActive = false;

	int64 EventsDispatched = 0;
	int64 EventsDropped = 0;
	int64 BlocksSet = 0;
	int64 ItemsGiven = 0;
	int64 ZombiesSpawned = 0;
};
