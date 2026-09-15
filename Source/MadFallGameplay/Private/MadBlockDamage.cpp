// Copyright MadFall. All Rights Reserved.

#include "MadBlockDamage.h"

#include "MadBlockRegistry.h"

namespace MadFall::BlockDamage
{
	float GetResistance(const FMadBlockDefinitionData& Definition, FName DamageType)
	{
		if (const float* Found = Definition.Resistances.Find(DamageType))
		{
			return FMath::Max(0.0f, *Found);
		}
		return 1.0f;
	}

	int32 GetStageIndex(const FMadBlockDefinitionData& Definition, uint8 Damage)
	{
		int32 Result = 0;
		for (int32 Index = 0; Index < Definition.DamageStages.Num(); ++Index)
		{
			if (Definition.DamageStages[Index].At <= Damage)
			{
				Result = Index;
			}
		}
		return Result;
	}

	FMadBlockDamageResult Compute(const FMadVoxel& Voxel, float Amount, FName DamageType, const FMadBlockRegistry& Registry)
	{
		FMadBlockDamageResult Result;
		Result.NewVoxel = Voxel;

		if (Voxel.IsAir() || Amount <= 0.0f)
		{
			return Result;
		}

		// Each downgrade carries overflow into the next block. Bounded so a
		// content cycle (A downgrades to B downgrades to A) cannot hang a hit.
		constexpr int32 MaxDowngrades = 8;

		FMadVoxel Current = Voxel;
		float Remaining = Amount;
		const uint16 OriginalId = Voxel.BlockTypeID;
		int32 OriginalStage = INDEX_NONE;

		for (int32 Pass = 0; Pass <= MaxDowngrades; ++Pass)
		{
			const FMadBlockDefinitionData* Def = Registry.FindDefinition(Current.BlockTypeID);
			if (Def == nullptr)
			{
				// Unresolved blocks are placeholders for content that is not
				// installed. Destroying them would make removing a mod lossy.
				break;
			}

			if (OriginalStage == INDEX_NONE)
			{
				OriginalStage = GetStageIndex(*Def, Voxel.Damage);
			}

			const float Resistance = GetResistance(*Def, DamageType);
			const float Hardness = FMath::Max(0.0f, Def->Hardness);
			const float Effective = Remaining * Resistance;
			if (Effective <= 0.0f)
			{
				break;
			}

			const float LostBefore = Hardness * (static_cast<float>(Current.Damage) / 255.0f);
			const float LostAfter = LostBefore + Effective;

			// Byte the block would reach, before considering downgrades.
			const int32 NewByte = Hardness <= 0.0f
				? 255
				: FMath::Min(255, FMath::CeilToInt(LostAfter / Hardness * 255.0f - 1.0e-3f));

			// The first stage at or below NewByte with a downgrade, crossed by this hit.
			const FMadBlockDamageStage* Downgrade = nullptr;
			for (const FMadBlockDamageStage& Stage : Def->DamageStages)
			{
				if (!Stage.DowngradeTo.IsNone() && Stage.At > Current.Damage && Stage.At <= NewByte)
				{
					Downgrade = &Stage;
					break;
				}
			}

			if (Downgrade != nullptr)
			{
				const uint16 NewId = Registry.ResolveRuntimeId(Downgrade->DowngradeTo);
				if (NewId != MadFall::BlockTypeUnresolved && NewId != MadFall::BlockTypeAir)
				{
					const float LostAtStage = Hardness * (static_cast<float>(Downgrade->At) / 255.0f);
					const float Overflow = FMath::Max(0.0f, LostAfter - LostAtStage);

					Result.EffectiveDamage += (Effective - Overflow);
					Result.bDowngraded = true;

					Current.BlockTypeID = NewId;
					Current.Damage = 0;

					// Overflow was already scaled by this block's resistance;
					// undo that so the next block applies its own.
					Remaining = Resistance > 0.0f ? Overflow / Resistance : 0.0f;
					if (Remaining <= 0.0f)
					{
						break;
					}
					continue;
				}
			}

			if (LostAfter >= Hardness)
			{
				Result.EffectiveDamage += FMath::Max(0.0f, Hardness - LostBefore);
				Result.bDestroyed = true;

				Current = FMadVoxel();
				Current.BlockTypeID = MadFall::BlockTypeAir;
				Current.Density = 0;
				Current.Damage = 0;
				Current.Rotation = 0;
				Current.Flags = 0;
				break;
			}

			Result.EffectiveDamage += Effective;
			Current.Damage = static_cast<uint8>(FMath::Clamp(NewByte, 0, 254));
			break;
		}

		Result.NewVoxel = Current;

		if (Result.bDestroyed || Result.bDowngraded)
		{
			Result.bStageChanged = true;
		}
		else if (const FMadBlockDefinitionData* Def = Registry.FindDefinition(Current.BlockTypeID))
		{
			Result.bStageChanged = Current.BlockTypeID == OriginalId
				&& GetStageIndex(*Def, Current.Damage) != OriginalStage;
		}

		return Result;
	}
}
