// Copyright MadFall. All Rights Reserved.

#include "MadSurfaceRegistry.h"

#include "Dom/JsonObject.h"
#include "MadDefinitionPatches.h"
#include "MadDefinitionSources.h"
#include "MadFallCore.h"
#include "MadJsonReader.h"
#include "Misc/ScopeLock.h"

int32 MadFall::Surfaces::FindPattern(const FString& Name)
{
	for (int32 Index = 0; Index < NumPatterns; ++Index)
	{
		if (Name.Equals(PatternNames[Index], ESearchCase::IgnoreCase))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

void FMadSurfaceRegistry::BeginLoad()
{
	Staged.Reset();
	StagedOrder.Reset();
	Surfaces.Reset();
	Index.Reset();
}

bool FMadSurfaceRegistry::AddJson(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	const FMadJsonReader R{ SourcePath, OutErrors };

	FString Schema;
	if (!R.ReadString(Object, TEXT("schema"), TEXT("/schema"), Schema) || Schema != MadFall::SurfaceSchemaV1)
	{
		R.AddError(TEXT("/schema"), FString::Printf(TEXT("expected \"%s\""), MadFall::SurfaceSchemaV1));
		return false;
	}

	FName Id;
	FString Reason;
	if (!R.ReadName(Object, TEXT("id"), TEXT("/id"), Id) || !MadFall::BlockDefinitionJson::IsValidBlockId(Id, Reason))
	{
		R.AddError(TEXT("/id"), Reason.IsEmpty() ? FString(TEXT("missing required field")) : Reason);
		return false;
	}
	if (!ModId.IsNone() && MadFall::BlockDefinitionJson::GetNamespace(Id) != ModId)
	{
		R.AddError(TEXT("/id"), FString::Printf(
			TEXT("namespace '%s' does not match the owning mod id '%s'. To re-skin another mod's surface, ship a patch of kind \"surface\"."),
			*MadFall::BlockDefinitionJson::GetNamespace(Id).ToString(), *ModId.ToString()));
		return false;
	}

	// A later file of the same mod redefines the whole surface.
	if (!Staged.Contains(Id))
	{
		StagedOrder.Add(Id);
	}
	Staged.Add(Id, FStaged{ Object, SourcePath });
	return true;
}

int32 FMadSurfaceRegistry::AddFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	int32 Accepted = 0;
	MadFall::Definitions::ForEachJsonObject(Directory, OutErrors,
		[&](const FString& File, const TSharedRef<FJsonObject>& Object) { Accepted += AddJson(Object, File, ModId, OutErrors) ? 1 : 0; });
	return Accepted;
}

void FMadSurfaceRegistry::FinishLoad(const FMadPatchSet* Patches, TArray<FMadDefinitionError>& OutErrors)
{
	static const FName Kind(TEXT("surface"));
	Surfaces.Reset();
	Index.Reset();

	TSet<FName> Known;
	for (const FName& Id : StagedOrder)
	{
		const FStaged& Entry = Staged[Id];
		Known.Add(Id);
		if (Patches != nullptr)
		{
			Patches->ApplyTo(Kind, Id, Entry.Object.ToSharedRef(), OutErrors);
		}

		const TSharedRef<FJsonObject> Object = Entry.Object.ToSharedRef();
		const FMadJsonReader R{ Entry.SourcePath, OutErrors };

		FMadSurfaceDefinition Surface;
		Surface.Id = Id;
		Surface.SourcePath = Entry.SourcePath;
		R.ReadSoftPath(Object, TEXT("material"), TEXT("/material"), Surface.Material);

		TArray<TSharedPtr<FJsonValue>> Color;
		if (R.ReadArray(Object, TEXT("color"), TEXT("/color"), Color))
		{
			const bool bNumbers = (Color.Num() == 3 || Color.Num() == 4) && !Color.ContainsByPredicate(
				[](const TSharedPtr<FJsonValue>& V) { return !V.IsValid() || V->Type != EJson::Number; });
			if (bNumbers)
			{
				Surface.Color = FLinearColor(
					MadFall::Surfaces::SRGBToLinear(static_cast<float>(Color[0]->AsNumber())),
					MadFall::Surfaces::SRGBToLinear(static_cast<float>(Color[1]->AsNumber())),
					MadFall::Surfaces::SRGBToLinear(static_cast<float>(Color[2]->AsNumber())), 1.0f);
				Surface.bHasColor = true;
			}
			else
			{
				R.AddError(TEXT("/color"), TEXT("expected [r, g, b] with each 0..1"));
			}
		}

		FString Impact;
		if (R.ReadString(Object, TEXT("impact"), TEXT("/impact"), Impact))
		{
			static const TSet<FString> Kinds = { TEXT("stone"), TEXT("wood"), TEXT("dirt"), TEXT("metal"), TEXT("foliage") };
			if (Kinds.Contains(Impact.ToLower()))
			{
				Surface.Impact = FName(*Impact.ToLower());
			}
			else
			{
				R.AddError(TEXT("/impact"), FString::Printf(TEXT("'%s' is not one of: stone, wood, dirt, metal, foliage"), *Impact));
			}
		}

		FString PatternName;
		if (R.ReadString(Object, TEXT("pattern"), TEXT("/pattern"), PatternName))
		{
			const int32 Pattern = MadFall::Surfaces::FindPattern(PatternName);
			if (Pattern == INDEX_NONE)
			{
				R.AddError(TEXT("/pattern"), FString::Printf(TEXT("'%s' is not a known pattern (plain, stone, dirt, grass, sand, planks, bark, leaves, concrete, brick, metal, ore, farmland, cloth, water, gravel)"), *PatternName));
			}
			else
			{
				Surface.Pattern = Pattern;
			}
		}

		TSharedPtr<FJsonObject> Cover;
		if (R.ReadObject(Object, TEXT("cover"), TEXT("/cover"), Cover))
		{
			const TSharedRef<FJsonObject> C = Cover.ToSharedRef();
			R.ReadFloat(C, TEXT("density"), TEXT("/cover/density"), Surface.CoverDensity);
			R.ReadFloat(C, TEXT("flowers"), TEXT("/cover/flowers"), Surface.CoverFlowers);
			R.ReadRange(C, TEXT("height"), TEXT("/cover/height"), Surface.CoverHeightMin, Surface.CoverHeightMax);
			Surface.CoverColor = Surface.Color;
			TArray<TSharedPtr<FJsonValue>> CoverColour;
			if (R.ReadArray(C, TEXT("color"), TEXT("/cover/color"), CoverColour))
			{
				if (CoverColour.Num() == 3 && !CoverColour.ContainsByPredicate([](const TSharedPtr<FJsonValue>& V) { return !V.IsValid() || V->Type != EJson::Number; }))
				{
					Surface.CoverColor = FLinearColor(
						MadFall::Surfaces::SRGBToLinear(static_cast<float>(CoverColour[0]->AsNumber())),
						MadFall::Surfaces::SRGBToLinear(static_cast<float>(CoverColour[1]->AsNumber())),
						MadFall::Surfaces::SRGBToLinear(static_cast<float>(CoverColour[2]->AsNumber())), 1.0f);
				}
				else
				{
					R.AddError(TEXT("/cover/color"), TEXT("expected [r, g, b] with each 0..1"));
				}
			}
			if (Surface.CoverDensity < 0.0f || Surface.CoverDensity > 1.0f || Surface.CoverFlowers < 0.0f || Surface.CoverFlowers > 1.0f)
			{
				R.AddError(TEXT("/cover"), TEXT("density and flowers are chances per column, 0..1"));
				Surface.CoverDensity = FMath::Clamp(Surface.CoverDensity, 0.0f, 1.0f);
				Surface.CoverFlowers = FMath::Clamp(Surface.CoverFlowers, 0.0f, 1.0f);
			}
			Surface.CoverHeightMin = FMath::Clamp(Surface.CoverHeightMin, 0.05f, 2.0f);
			Surface.CoverHeightMax = FMath::Clamp(Surface.CoverHeightMax, Surface.CoverHeightMin, 2.0f);
			R.ReportUnknownFields(C, { TEXT("density"), TEXT("flowers"), TEXT("height"), TEXT("color") });
		}

		R.ReportUnknownFields(Object, { TEXT("schema"), TEXT("id"), TEXT("material"), TEXT("color"), TEXT("impact"), TEXT("pattern"), TEXT("cover") });

		Index.Add(Id, Surfaces.Num());
		Surfaces.Add(MoveTemp(Surface));
	}

	if (Patches != nullptr)
	{
		Patches->ReportUnmatched(Kind, Known, OutErrors);
	}
	Staged.Reset();
	StagedOrder.Reset();
}

const FMadSurfaceDefinition* FMadSurfaceRegistry::Find(FName MaterialClass) const
{
	const int32* Found = Index.Find(MaterialClass);
	return Found ? &Surfaces[*Found] : nullptr;
}

FColor FMadSurfaceRegistry::GetVertexColor(FName MaterialClass) const
{
	if (const FMadSurfaceDefinition* Surface = Find(MaterialClass); Surface && Surface->bHasColor)
	{
		// Linear bytes: the VertexColor material node reads vertex colour as
		// linear (byte / 255, no decode). Color is already linear (converted
		// from the authored sRGB on load).
		FColor Out = Surface->Color.ToFColor(/*bSRGB*/ false);
		Out.A = MadFall::Surfaces::PatternToAlpha(Surface->Pattern);
		return Out;
	}
	if (MaterialClass.IsNone())
	{
		return FColor(82, 82, 82);   // sRGB 0.6 grey, as linear bytes
	}

	// No surface: a stable placeholder per class, sRGB 0.38..0.88 per channel so
	// nothing comes out near-black and unreadable, stored linear like the rest.
	const uint32 Hash = GetTypeHash(MaterialClass);
	auto Channel = [](uint32 Bits) { return MadFall::Surfaces::SRGBToLinear((96.0f + static_cast<float>(Bits & 0x7F)) / 255.0f); };
	return FLinearColor(Channel(Hash), Channel(Hash >> 8), Channel(Hash >> 16), 1.0f).ToFColor(/*bSRGB*/ false);
}

namespace
{
	TUniquePtr<FMadSurfaceRegistry> GSurfaces;
	FCriticalSection GSurfacesLock;
}

namespace MadFall
{
	const FMadSurfaceRegistry& GetSurfaces()
	{
		// Mesher workers can be the first caller, so the one-time load is locked;
		// after it the registry is immutable and reads need nothing.
		if (!GSurfaces.IsValid())
		{
			FScopeLock Lock(&GSurfacesLock);
			if (!GSurfaces.IsValid())
			{
				TUniquePtr<FMadSurfaceRegistry> Registry = MakeUnique<FMadSurfaceRegistry>();
				TArray<FMadDefinitionError> Errors;
				Registry->BeginLoad();
				Definitions::ForEachSource(TEXT("surfaces"), [&](const FString& Directory, FName ModId)
				{
					Registry->AddFromDirectory(Directory, ModId, Errors);
				});
				Registry->FinishLoad(&GetPatchSet(), Errors);

				for (const FMadDefinitionError& Error : Errors)
				{
					UE_LOG(LogMadFallRegistry, Warning, TEXT("%s"), *Error.ToString());
				}
				UE_LOG(LogMadFallRegistry, Log, TEXT("Surfaces: %d material class(es)."), Registry->GetAll().Num());
				GSurfaces = MoveTemp(Registry);
			}
		}
		return *GSurfaces;
	}
}
