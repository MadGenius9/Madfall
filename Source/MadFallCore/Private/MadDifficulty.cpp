// Copyright MadFall. All Rights Reserved.

#include "MadDifficulty.h"

#include "Engine/World.h"
#include "MadGameplayDefinitions.h"
#include "MadVoxelWorldSubsystem.h"

const TArray<FName>& MadFall::Difficulty::GetLevels()
{
	static const TArray<FName> Levels = { FName(TEXT("easy")), Normal, FName(TEXT("hard")) };
	return Levels;
}

bool MadFall::Difficulty::IsValid(FName Level)
{
	return GetLevels().Contains(Level);
}

float MadFall::Difficulty::GetScale(FName Level, FName Key)
{
	const FName TuningId(*FString::Printf(TEXT("madfall:difficulty_%s"), *Level.ToString()));
	const FMadTuningDefinition* Tuning = MadFall::GetGameplayDefinitions().FindTuning(TuningId);
	const float* Value = Tuning ? Tuning->Values.Find(Key) : nullptr;
	return Value ? FMath::Max(0.0f, *Value) : 1.0f;
}

float MadFall::Difficulty::GetWorldScale(const UWorld* World, FName Key)
{
	const UMadVoxelWorldSubsystem* VoxelWorld = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
	return GetScale(VoxelWorld ? VoxelWorld->GetDifficulty() : Normal, Key);
}
