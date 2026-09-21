// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadSession.h"
#include "MadSettings.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadMenus.generated.h"

class ACameraActor;
class SMadMenuWidget;
class SWidget;

enum class EMadMenuPage : uint8
{
	None,
	Title,
	NewWorld,
	LoadWorld,
	Settings,
	Pause,
	Help,
	Controls
};

/**
 * The title screen, the world list, the pause menu and settings.
 *
 * Every action a button takes is a public function here, and every one has a
 * console command, so CI drives exactly the code a click does (`mad.menu.*`).
 * The widget (Slate, built in C++ with no assets, like the HUD) is only drawn
 * when there is a game viewport; headless runs get the same logic without it.
 *
 * The title screen is a real world: a read-only backdrop (Session::TitleWorldName)
 * streamed around an orbiting camera, with no survivor spawned.
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadMenuSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual TStatId GetStatId() const override;

	bool IsTitle() const { return bTitle; }
	EMadMenuPage GetPage() const { return Page; }
	void ShowPage(EMadMenuPage NewPage);

	// --- title actions -----------------------------------------------------

	/** Creates and enters a new world. False (with a reason) for a bad name or an existing world. */
	bool CreateWorld(const FString& DisplayName, const FString& SeedText, FName Difficulty, FString& OutError, bool bCreative = false);
	bool LoadWorld(const FString& Name, FString& OutError);
	bool DeleteWorld(const FString& Name, FString& OutError);

	/** Puts a world back as a backup had it (the newest when Backup is empty). Not the world being played. */
	bool RestoreWorldBackup(const FString& Name, const FString& Backup, FString& OutError);
	const TArray<FMadWorldInfo>& GetWorlds();

	// --- in-game actions ---------------------------------------------------

	void OpenPauseMenu();
	void Resume();

	/** Saves everything and returns to the title screen. */
	void SaveAndQuitToTitle();

	/** Saves (outside the title) and exits the game. */
	void QuitGame();

	// --- settings ----------------------------------------------------------

	FMadSettings GetSettings() const { return MadFall::Settings::FromConsoleVariables(); }
	void ApplyAndSaveSettings(const FMadSettings& Settings);

	/** Where Settings' Back button goes. */
	EMadMenuPage GetSettingsReturnPage() const { return bTitle ? EMadMenuPage::Title : EMadMenuPage::Pause; }

private:
	void SetupTitleScene();
	void SaveEverything();
	void TravelToCurrentMap();
	void UpdateInputMode();

	bool bTitle = false;
	bool bTitleSceneReady = false;
	EMadMenuPage Page = EMadMenuPage::None;
	TSharedPtr<SMadMenuWidget> Widget;
	TSharedPtr<SWidget> WidgetContainer;
	TArray<FMadWorldInfo> Worlds;
	bool bWorldsDirty = true;
	float OrbitAngle = 0.0f;
	FVector OrbitCentre = FVector::ZeroVector;

	UPROPERTY(Transient)
	TObjectPtr<ACameraActor> TitleCamera;
};
