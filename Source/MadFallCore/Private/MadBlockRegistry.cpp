// Copyright MadFall. All Rights Reserved.

#include "MadBlockRegistry.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "MadDefinitionPatches.h"
#include "MadFallCore.h"
#include "MadFallStats.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeRWLock.h"
#include "Misc/StringBuilder.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DECLARE_CYCLE_STAT(TEXT("Registry FinishLoad"), STAT_MadRegistryFinishLoad, STATGROUP_MadFallModLoad);

namespace
{
	const FName NameAir(TEXT("madfall:air"));
}

FMadBlockRegistry::FMadBlockRegistry()
{
	BeginLoad();
}

FMadBlockRegistry::~FMadBlockRegistry() = default;

// ===========================================================================
// Load
// ===========================================================================

void FMadBlockRegistry::BeginLoad()
{
	Entries.Reset();
	IdToRuntimeId.Reset();
	RuntimeIdToEntryIndex.Reset();
	Pending.Reset();
	PendingByIndex.Reset();

	{
		FRWScopeLock Lock(UnresolvedLock, SLT_Write);
		UnresolvedIdToRuntimeId.Reset();
		UnresolvedEntries.Reset();
		NextUnresolvedId = MadFall::BlockTypeUnresolved - 1;
	}

	bLoaded = false;
	InstallReservedIds();
}

void FMadBlockRegistry::InstallReservedIds()
{
	// Air must be runtime id 0 unconditionally: FMadChunkStorage's default
	// construction, FMemory::Memset on a fresh chunk and FMadVoxel::Air() all
	// assume a zeroed block id means air. Letting load order decide would make
	// "empty" mean something different depending on which mods are installed.
	FMadBlockDefinitionData AirData;
	AirData.Id = NameAir;
	AirData.DisplayName = TEXT("@blocks.air");
	AirData.ShapeKind = EMadBlockShapeKind::Isosurface;
	AirData.MassKg = 0.0f;
	AirData.Hardness = 0.0f;
	AirData.SupportStrength = 0.0f;
	AirData.bTransparent = true;
	AirData.bOccludesNeighbors = false;
	AirData.Collision = EMadBlockCollisionKind::None;
	AirData.SourceModId = FName(TEXT("madfall"));
	AirData.SourcePath = TEXT("<built-in>");

	FMadBlockEntry AirEntry;
	AirEntry.RuntimeId = MadFall::BlockTypeAir;
	AirEntry.Definition = AirData;
	AirEntry.View = AirData.MakeView(MadFall::BlockTypeAir);

	RuntimeIdToEntryIndex.Add(MadFall::BlockTypeAir, Entries.Num());
	IdToRuntimeId.Add(NameAir, MadFall::BlockTypeAir);
	Entries.Add(MoveTemp(AirEntry));
}

int32 FMadBlockRegistry::AddFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors)
{
	if (!IFileManager::Get().DirectoryExists(*Directory))
	{
		return 0;
	}

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.json"), true, false);

	// Deterministic order. Without this, two machines with the same mods could
	// number blocks differently purely because of filesystem enumeration order,
	// which would make a bug reproduce on one and not the other.
	Files.Sort();

	int32 Staged = 0;

	for (const FString& File : Files)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *File))
		{
			OutErrors.Add(FMadDefinitionError{ File, FString(), TEXT("could not be read from disk") });
			continue;
		}

		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		TSharedPtr<FJsonValue> Root;
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutErrors.Add(FMadDefinitionError{ File, FString(),
				FString::Printf(TEXT("not valid JSON: %s"), *Reader->GetErrorMessage()) });
			continue;
		}

		TArray<TSharedPtr<FJsonValue>> Objects;
		if (Root->Type == EJson::Array) { Objects = Root->AsArray(); }
		else if (Root->Type == EJson::Object) { Objects.Add(Root); }
		else
		{
			OutErrors.Add(FMadDefinitionError{ File, FString(),
				TEXT("expected an object or an array of objects") });
			continue;
		}

		for (const TSharedPtr<FJsonValue>& Value : Objects)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				OutErrors.Add(FMadDefinitionError{ File, FString(), TEXT("array entries must be objects") });
				continue;
			}

			const TSharedRef<FJsonObject> Object = Value->AsObject().ToSharedRef();

			// Parse once now purely to validate and extract identity. The real
			// parse happens in FinishLoad, after the parent chain is known.
			FMadBlockDefinitionData Probe;
			TArray<FMadDefinitionError> ProbeErrors;
			if (!MadFall::BlockDefinitionJson::ParseObject(Object, File, ModId, Probe, ProbeErrors))
			{
				OutErrors.Append(ProbeErrors);
				continue;
			}

			FMadPendingDefinition PendingDef;
			PendingDef.Id = Probe.Id;
			PendingDef.Extends = Probe.Extends;
			PendingDef.ModId = ModId;
			PendingDef.SourcePath = File;
			PendingDef.Json = Object;

			if (const int32* Existing = PendingByIndex.Find(Probe.Id))
			{
				// Two definitions of the same id: later load order wins, and we
				// say so loudly, naming both files. Silence here is how a mod
				// conflict turns into a three-day bug report.
				UE_LOG(LogMadFallRegistry, Warning,
					TEXT("Block '%s' is defined more than once. '%s' (mod '%s') overrides '%s' (mod '%s'). ")
					TEXT("To change another mod's block, ship a patch instead of redefining it."),
					*Probe.Id.ToString(),
					*File, *ModId.ToString(),
					*Pending[*Existing].SourcePath, *Pending[*Existing].ModId.ToString());

				Pending[*Existing] = MoveTemp(PendingDef);
			}
			else
			{
				PendingByIndex.Add(Probe.Id, Pending.Num());
				Pending.Add(MoveTemp(PendingDef));
			}

			++Staged;
		}
	}

	return Staged;
}

bool FMadBlockRegistry::AddFromAsset(const FMadBlockDefinitionData& Data, TArray<FMadDefinitionError>& OutErrors)
{
	FString Reason;
	if (!MadFall::BlockDefinitionJson::IsValidBlockId(Data.Id, Reason))
	{
		OutErrors.Add(FMadDefinitionError{ Data.SourcePath, TEXT("/id"), Reason });
		return false;
	}

	if (!Data.Extends.IsNone())
	{
		// Inheritance works by re-applying the child's explicitly-present JSON
		// fields over the resolved parent. A data asset has no notion of
		// "explicitly present" - every field always has a value - so there is
		// no way to tell an intentional 0.0 from an unset one. Composing in the
		// editor is the answer there, and saying so beats guessing.
		OutErrors.Add(FMadDefinitionError{ Data.SourcePath, TEXT("/extends"),
			TEXT("'extends' is only supported for JSON definitions; a data asset has no way to distinguish an unset field from a defaulted one. Copy the parent asset and edit it instead.") });
		return false;
	}

	FMadPendingDefinition PendingDef;
	PendingDef.Id = Data.Id;
	PendingDef.ModId = Data.SourceModId;
	PendingDef.SourcePath = Data.SourcePath;
	PendingDef.AssetData = Data;

	if (const int32* Existing = PendingByIndex.Find(Data.Id))
	{
		UE_LOG(LogMadFallRegistry, Warning,
			TEXT("Block '%s' is defined more than once. Data asset '%s' overrides '%s'."),
			*Data.Id.ToString(), *Data.SourcePath, *Pending[*Existing].SourcePath);
		Pending[*Existing] = MoveTemp(PendingDef);
	}
	else
	{
		PendingByIndex.Add(Data.Id, Pending.Num());
		Pending.Add(MoveTemp(PendingDef));
	}

	return true;
}

int32 FMadBlockRegistry::AddFromAssetRegistry(TArray<FMadDefinitionError>& OutErrors)
{
	const FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	FARFilter Filter;
	Filter.ClassPaths.Add(UMadBlockDefinition::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> Assets;
	AssetRegistryModule.Get().GetAssets(Filter, Assets);

	// Same reason as the directory scan: deterministic numbering across runs.
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetObjectPathString() < B.GetObjectPathString();
	});

	int32 Staged = 0;

	for (const FAssetData& Asset : Assets)
	{
		const UMadBlockDefinition* Definition = Cast<UMadBlockDefinition>(Asset.GetAsset());
		if (Definition == nullptr)
		{
			OutErrors.Add(FMadDefinitionError{ Asset.GetObjectPathString(), FString(),
				TEXT("could not be loaded as a UMadBlockDefinition") });
			continue;
		}

		FMadBlockDefinitionData Data = Definition->Data;
		Data.SourcePath = Asset.GetObjectPathString();
		if (Data.SourceModId.IsNone())
		{
			Data.SourceModId = MadFall::BlockDefinitionJson::GetNamespace(Data.Id);
		}

		if (AddFromAsset(Data, OutErrors))
		{
			++Staged;
		}
	}

	return Staged;
}

bool FMadBlockRegistry::ResolveDefinition(FName Id, TSet<FName>& Visiting,
	TMap<FName, FMadBlockDefinitionData>& Resolved, TArray<FMadDefinitionError>& OutErrors)
{
	if (Resolved.Contains(Id))
	{
		return true;
	}

	const int32* PendingIndex = PendingByIndex.Find(Id);
	if (PendingIndex == nullptr)
	{
		return false;
	}

	const FMadPendingDefinition& Def = Pending[*PendingIndex];

	if (Visiting.Contains(Id))
	{
		// A cycle would otherwise recurse until the stack gives out, and the
		// crash would name none of the mods involved.
		OutErrors.Add(FMadDefinitionError{ Def.SourcePath, TEXT("/extends"),
			FString::Printf(TEXT("'%s' is part of an inheritance cycle"), *Id.ToString()) });
		return false;
	}

	Visiting.Add(Id);

	FMadBlockDefinitionData Data;

	if (!Def.Extends.IsNone())
	{
		if (!ResolveDefinition(Def.Extends, Visiting, Resolved, OutErrors))
		{
			OutErrors.Add(FMadDefinitionError{ Def.SourcePath, TEXT("/extends"),
				FString::Printf(TEXT("parent definition '%s' does not exist or failed to load"), *Def.Extends.ToString()) });
			Visiting.Remove(Id);
			return false;
		}

		// Start from the fully resolved parent, then let the child's own fields
		// land on top. This works because every JSON reader leaves the output
		// untouched for an absent field - inheritance is that contract applied
		// twice, not a separate merge engine.
		Data = Resolved[Def.Extends];
	}

	if (Def.AssetData.IsSet())
	{
		Data = Def.AssetData.GetValue();
	}
	else if (Def.Json.IsValid())
	{
		TArray<FMadDefinitionError> ParseErrors;
		if (!MadFall::BlockDefinitionJson::ParseObject(Def.Json.ToSharedRef(), Def.SourcePath, Def.ModId, Data, ParseErrors))
		{
			OutErrors.Append(ParseErrors);
			Visiting.Remove(Id);
			return false;
		}
		OutErrors.Append(ParseErrors);
	}

	Visiting.Remove(Id);
	Resolved.Add(Id, MoveTemp(Data));
	return true;
}

uint16 FMadBlockRegistry::AssignRuntimeId(const FMadBlockDefinitionData& Data)
{
	if (const uint16* Existing = IdToRuntimeId.Find(Data.Id))
	{
		// Reserved id (air). Keep the number, take the authored definition.
		const int32 EntryIndex = RuntimeIdToEntryIndex[*Existing];
		Entries[EntryIndex].Definition = Data;
		Entries[EntryIndex].View = Data.MakeView(*Existing);
		return *Existing;
	}

	// Sequential from 1 upward. Unresolved placeholders grow downward from
	// 65534, so the two allocators cannot meet until 65k block types exist -
	// at which point the uint16 is the problem, not the scheme.
	const int32 NextId = Entries.Num();
	checkf(NextId < MadFall::BlockTypeUnresolved - 1,
		TEXT("Ran out of runtime block ids (%d). FMadVoxel::BlockTypeID is a uint16."), NextId);

	const uint16 RuntimeId = static_cast<uint16>(NextId);

	FMadBlockEntry Entry;
	Entry.RuntimeId = RuntimeId;
	Entry.Definition = Data;
	Entry.View = Data.MakeView(RuntimeId);

	RuntimeIdToEntryIndex.Add(RuntimeId, Entries.Num());
	IdToRuntimeId.Add(Data.Id, RuntimeId);
	Entries.Add(MoveTemp(Entry));

	return RuntimeId;
}

void FMadBlockRegistry::FinishLoad(TArray<FMadDefinitionError>& OutErrors)
{
	SCOPE_CYCLE_COUNTER(STAT_MadRegistryFinishLoad);

	TMap<FName, FMadBlockDefinitionData> Resolved;
	TSet<FName> Visiting;

	// Resolve in staged order so that inheritance chains are followed, then
	// assign ids in sorted id order so numbering does not depend on which file
	// happened to mention a block first.
	for (const FMadPendingDefinition& Def : Pending)
	{
		ResolveDefinition(Def.Id, Visiting, Resolved, OutErrors);
	}

	TArray<FName> SortedIds;
	Resolved.GenerateKeyArray(SortedIds);
	SortedIds.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

	for (const FName& Id : SortedIds)
	{
		AssignRuntimeId(Resolved[Id]);
	}

	Pending.Reset();
	PendingByIndex.Reset();
	bLoaded = true;

	UE_LOG(LogMadFallRegistry, Log, TEXT("Block registry loaded: %d definitions."), Entries.Num());

	for (const FMadDefinitionError& Error : OutErrors)
	{
		UE_LOG(LogMadFallRegistry, Warning, TEXT("%s"), *Error.ToString());
	}
}

// ===========================================================================
// Queries
// ===========================================================================

uint16 FMadBlockRegistry::ResolveRuntimeId(FName BlockId) const
{
	if (BlockId.IsNone())
	{
		return MadFall::BlockTypeAir;
	}

	if (const uint16* Found = IdToRuntimeId.Find(BlockId))
	{
		return *Found;
	}

	{
		FRWScopeLock Lock(UnresolvedLock, SLT_ReadOnly);
		if (const uint16* Found = UnresolvedIdToRuntimeId.Find(BlockId))
		{
			return *Found;
		}
	}

	return MadFall::BlockTypeUnresolved;
}

FName FMadBlockRegistry::GetStringId(uint16 RuntimeId) const
{
	if (const int32* EntryIndex = RuntimeIdToEntryIndex.Find(RuntimeId))
	{
		return Entries[*EntryIndex].Definition.Id;
	}

	{
		FRWScopeLock Lock(UnresolvedLock, SLT_ReadOnly);
		if (const FMadBlockEntry* Entry = UnresolvedEntries.Find(RuntimeId))
		{
			// The original id, not a placeholder name. This return value is
			// what the serializer writes, and it is what makes uninstalling
			// and reinstalling a mod lossless.
			return Entry->Definition.Id;
		}
	}

	return NAME_None;
}

FMadBlockDefView FMadBlockRegistry::GetBlockView(uint16 RuntimeId) const
{
	if (const int32* EntryIndex = RuntimeIdToEntryIndex.Find(RuntimeId))
	{
		return Entries[*EntryIndex].View;
	}

	{
		FRWScopeLock Lock(UnresolvedLock, SLT_ReadOnly);
		if (const FMadBlockEntry* Entry = UnresolvedEntries.Find(RuntimeId))
		{
			return Entry->View;
		}
	}

	return FMadBlockDefView();
}

FMadBlockDefView FMadBlockRegistry::GetBlockViewById(FName BlockId) const
{
	return GetBlockView(ResolveRuntimeId(BlockId));
}

bool FMadBlockRegistry::IsRegistered(FName BlockId) const
{
	return IdToRuntimeId.Contains(BlockId);
}

int32 FMadBlockRegistry::Num() const
{
	return Entries.Num();
}

void FMadBlockRegistry::GetAllBlockIds(TArray<FName>& OutIds) const
{
	OutIds.Reset(Entries.Num());
	for (const FMadBlockEntry& Entry : Entries)
	{
		OutIds.Add(Entry.Definition.Id);
	}
}

const FMadBlockDefinitionData* FMadBlockRegistry::FindDefinition(uint16 RuntimeId) const
{
	if (const int32* EntryIndex = RuntimeIdToEntryIndex.Find(RuntimeId))
	{
		return &Entries[*EntryIndex].Definition;
	}
	return nullptr;
}

// ===========================================================================
// Unresolved blocks
// ===========================================================================

uint16 FMadBlockRegistry::GetOrCreateUnresolvedId(FName BlockId)
{
	if (BlockId.IsNone())
	{
		return MadFall::BlockTypeAir;
	}

	if (const uint16* Registered = IdToRuntimeId.Find(BlockId))
	{
		return *Registered;
	}

	{
		FRWScopeLock ReadLock(UnresolvedLock, SLT_ReadOnly);
		if (const uint16* Found = UnresolvedIdToRuntimeId.Find(BlockId))
		{
			return *Found;
		}
	}

	FRWScopeLock WriteLock(UnresolvedLock, SLT_Write);

	// Re-check: another worker thread loading an adjacent chunk may have
	// allocated this id between our read and our write.
	if (const uint16* Found = UnresolvedIdToRuntimeId.Find(BlockId))
	{
		return *Found;
	}

	if (NextUnresolvedId <= static_cast<uint16>(Entries.Num()))
	{
		UE_LOG(LogMadFallRegistry, Error,
			TEXT("Ran out of placeholder ids for unresolved blocks; '%s' will render as generic unknown and WILL NOT round-trip."),
			*BlockId.ToString());
		return MadFall::BlockTypeUnresolved;
	}

	const uint16 RuntimeId = NextUnresolvedId--;

	FMadBlockDefinitionData Data;
	Data.Id = BlockId;                      // the ORIGINAL id, preserved verbatim
	Data.DisplayName = TEXT("@blocks.unresolved");
	Data.ShapeKind = EMadBlockShapeKind::Cubic;
	Data.MassKg = 0.0f;
	Data.Hardness = 0.0f;
	// Infinite support: an unresolved block must not cause the structure it is
	// part of to collapse just because a mod is temporarily uninstalled.
	Data.SupportStrength = TNumericLimits<float>::Max();
	Data.bIsAnchor = true;
	Data.Collision = EMadBlockCollisionKind::Box;
	Data.SourceModId = MadFall::BlockDefinitionJson::GetNamespace(BlockId);
	Data.SourcePath = TEXT("<unresolved>");

	FMadBlockEntry Entry;
	Entry.RuntimeId = RuntimeId;
	Entry.bUnresolved = true;
	Entry.Definition = Data;
	Entry.View = Data.MakeView(RuntimeId);
	Entry.View.bUnresolved = true;

	UnresolvedIdToRuntimeId.Add(BlockId, RuntimeId);
	UnresolvedEntries.Add(RuntimeId, MoveTemp(Entry));

	UE_LOG(LogMadFallRegistry, Warning,
		TEXT("Block '%s' is not provided by any installed definition. It will be preserved as an inert placeholder and written back unchanged on save."),
		*BlockId.ToString());

	return RuntimeId;
}

int32 FMadBlockRegistry::NumUnresolved() const
{
	FRWScopeLock Lock(UnresolvedLock, SLT_ReadOnly);
	return UnresolvedEntries.Num();
}

bool FMadBlockRegistry::IsUnresolvedId(uint16 RuntimeId) const
{
	FRWScopeLock Lock(UnresolvedLock, SLT_ReadOnly);
	return UnresolvedEntries.Contains(RuntimeId);
}

FString FMadBlockRegistry::DescribeContents() const
{
	TStringBuilder<4096> Builder;
	Builder.Appendf(TEXT("Block registry: %d definitions, %d unresolved placeholders\n"),
		Entries.Num(), NumUnresolved());

	for (const FMadBlockEntry& Entry : Entries)
	{
		Builder.Appendf(TEXT("  [%5d] %-32s mod=%-16s mass=%.0fkg hardness=%.0f support=%.0f\n"),
			Entry.RuntimeId,
			*Entry.Definition.Id.ToString(),
			*Entry.Definition.SourceModId.ToString(),
			Entry.Definition.MassKg,
			Entry.Definition.Hardness,
			Entry.Definition.SupportStrength);
	}

	{
		FRWScopeLock Lock(UnresolvedLock, SLT_ReadOnly);
		for (const TPair<uint16, FMadBlockEntry>& Pair : UnresolvedEntries)
		{
			Builder.Appendf(TEXT("  [%5d] %-32s UNRESOLVED (preserved on save)\n"),
				Pair.Key, *Pair.Value.Definition.Id.ToString());
		}
	}

	return Builder.ToString();
}

void FMadBlockRegistry::ApplyPatches(const FMadPatchSet& Patches, TArray<FMadDefinitionError>& OutErrors)
{
	static const FName Kind(TEXT("block"));
	TSet<FName> Known;
	for (FMadPendingDefinition& Def : Pending)
	{
		Known.Add(Def.Id);
		// Data-asset definitions have no JSON to patch; a patch aimed at one is
		// reported by the unmatched check below only if no JSON definition shares its id.
		if (Def.Json.IsValid())
		{
			Patches.ApplyTo(Kind, Def.Id, Def.Json.ToSharedRef(), OutErrors);
		}
	}
	Patches.ReportUnmatched(Kind, Known, OutErrors);
}
