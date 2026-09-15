// Copyright MadFall. All Rights Reserved.

#include "MadFallEditor.h"

DEFINE_LOG_CATEGORY(LogMadFallEditor);

void FMadFallEditorModule::StartupModule()
{
	UE_LOG(LogMadFallEditor, Log, TEXT("MadFallEditor starting."));

	// PHASE 3 attaches here: POI/prefab authoring volumes and the prefab asset
	// type. PHASE 1 adds the block-definition linter commandlet.
}

void FMadFallEditorModule::ShutdownModule()
{
	UE_LOG(LogMadFallEditor, Log, TEXT("MadFallEditor shutting down."));
}

IMPLEMENT_MODULE(FMadFallEditorModule, MadFallEditor)
