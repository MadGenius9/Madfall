// Copyright MadFall. All Rights Reserved.

#include "MadConsoleScript.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "MadFallGameplay.h"
#include "Misc/DefaultValueHelper.h"

namespace MadFall::ConsoleScript
{
	namespace
	{
		TArray<FString> GQueue;
		float GWaitRemaining = 0.0f;
	}

	void Enqueue(const FString& Commands)
	{
		TArray<FString> Parts;
		Commands.ParseIntoArray(Parts, TEXT(";"), /*bCullEmpty*/ true);
		for (FString& Part : Parts)
		{
			Part.TrimStartAndEndInline();
			if (!Part.IsEmpty())
			{
				GQueue.Add(Part);
			}
		}
	}

	bool IsEmpty()
	{
		return GQueue.Num() == 0;
	}

	void Tick(UWorld* World, float DeltaSeconds)
	{
		if (GWaitRemaining > 0.0f)
		{
			GWaitRemaining -= DeltaSeconds;
			return;
		}
		if (GQueue.Num() == 0 || World == nullptr)
		{
			return;
		}

		const FString Command = GQueue[0];
		GQueue.RemoveAt(0);

		if (Command.StartsWith(TEXT("wait ")))
		{
			FDefaultValueHelper::ParseFloat(Command.Mid(5), GWaitRemaining);
			return;
		}

		UE_LOG(LogMadFallGameplay, Display, TEXT("[script] %s"), *Command);
		if (APlayerController* Controller = World->GetFirstPlayerController())
		{
			// Through the player controller, so commands like `quit` and `shot`
			// that live on the player's exec chain work as well as console variables.
			Controller->ConsoleCommand(Command, /*bWriteToLog*/ false);
		}
		else if (GEngine != nullptr)
		{
			GEngine->Exec(World, *Command);
		}
	}
}

static FAutoConsoleCommandWithWorldAndArgs GMadOnSpawnCommand(
	TEXT("mad.onspawn"),
	TEXT("mad.onspawn <cmd>; wait <s>; <cmd>... - runs commands once the player has spawned."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld*)
	{
		MadFall::ConsoleScript::Enqueue(FString::Join(Args, TEXT(" ")));
	}));
