// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "MadKeyBindings.h"
#include "MadSettings.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadKeyBindingTest,
	"MadFall.Input.KeyBindings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadKeyBindingTest::RunTest(const FString& Parameters)
{
	const FName Jump(TEXT("jump"));
	const FName Interact(TEXT("interact"));
	const FName Forward(TEXT("move_forward"));

	FMadKeyBindings Keys;
	TestTrue(TEXT("defaults"), Keys.Get(Jump) == EKeys::SpaceBar);
	TestTrue(TEXT("defaults"), Keys.Get(Interact) == EKeys::E);
	TestFalse(TEXT("unknown action has no key"), Keys.Get(FName(TEXT("teleport"))).IsValid());
	TestEqual(TEXT("defaults store nothing"), Keys.ToOverrides().Num(), 0);

	// Every default is a legal binding, and no two actions share one.
	TSet<FKey> Seen;
	for (const FMadRebindableAction& Action : FMadKeyBindings::GetActions())
	{
		TestFalse(FString::Printf(TEXT("%s default is not reserved"), *Action.Id.ToString()), FMadKeyBindings::IsReserved(Action.Default));
		TestFalse(FString::Printf(TEXT("%s default is unique"), *Action.Id.ToString()), Seen.Contains(Action.Default));
		Seen.Add(Action.Default);
	}

	TestTrue(TEXT("rebind jump"), Keys.Set(Jump, EKeys::J) == EMadBindResult::Ok);
	TestTrue(TEXT("jump on J"), Keys.Get(Jump) == EKeys::J);
	TestEqual(TEXT("one override"), Keys.ToOverrides().FindRef(Jump), FString(TEXT("J")));

	// Taking a key another action uses swaps them.
	TestTrue(TEXT("interact onto J"), Keys.Set(Interact, EKeys::J) == EMadBindResult::Ok);
	TestTrue(TEXT("interact on J"), Keys.Get(Interact) == EKeys::J);
	TestTrue(TEXT("jump took interact's old key"), Keys.Get(Jump) == EKeys::E);

	// Rebinding back to the default removes the override.
	TestTrue(TEXT("interact back to E"), Keys.Set(Interact, EKeys::E) == EMadBindResult::Ok);
	TestTrue(TEXT("jump took J back"), Keys.Get(Jump) == EKeys::J);
	TestFalse(TEXT("interact has no override"), Keys.ToOverrides().Contains(Interact));

	TestTrue(TEXT("escape is reserved"), Keys.Set(Jump, EKeys::Escape) == EMadBindResult::ReservedKey);
	TestTrue(TEXT("hotbar keys are reserved"), Keys.Set(Jump, EKeys::Three) == EMadBindResult::ReservedKey);
	TestTrue(TEXT("mouse buttons are reserved"), Keys.Set(Jump, EKeys::LeftMouseButton) == EMadBindResult::ReservedKey);
	TestTrue(TEXT("gamepad keys are not keyboard keys"), Keys.Set(Jump, EKeys::Gamepad_FaceButton_Bottom) == EMadBindResult::InvalidKey);
	TestTrue(TEXT("nonsense key"), Keys.Set(Jump, FKey(FName(TEXT("NotAKey")))) == EMadBindResult::InvalidKey);
	TestTrue(TEXT("unknown action"), Keys.Set(FName(TEXT("teleport")), EKeys::K) == EMadBindResult::UnknownAction);
	TestTrue(TEXT("refusals change nothing"), Keys.Get(Jump) == EKeys::J);

	// A hand-edited settings file: bad entries dropped, the rest applied in action order.
	TMap<FName, FString> Edited;
	Edited.Add(Forward, TEXT("Up"));
	Edited.Add(Jump, TEXT("Escape"));
	Edited.Add(FName(TEXT("teleport")), TEXT("T"));
	Edited.Add(Interact, TEXT("Up"));
	FMadKeyBindings Loaded;
	Loaded.FromOverrides(Edited);
	TestTrue(TEXT("interact, later in order, took Up"), Loaded.Get(Interact) == EKeys::Up);
	TestTrue(TEXT("and forward got interact's key"), Loaded.Get(Forward) == EKeys::E);
	TestTrue(TEXT("reserved key ignored"), Loaded.Get(Jump) == EKeys::SpaceBar);
	TestFalse(TEXT("unknown action dropped"), Loaded.ToOverrides().Contains(FName(TEXT("teleport"))));

	// Through the settings file.
	const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("KeyBindingSettings.json"));
	FMadSettings Settings;
	Settings.KeyBindings = Keys.ToOverrides();
	FString Error;
	TestTrue(TEXT("settings save"), MadFall::Settings::Save(Settings, Path, Error));
	const FMadSettings Reloaded = MadFall::Settings::Load(Path);
	TestTrue(TEXT("bindings survive the file"), Reloaded.KeyBindings.OrderIndependentCompareEqual(Keys.ToOverrides()));
	IFileManager::Get().Delete(*Path);

	// The active bindings notify the survivor, and only on a real change.
	const FMadKeyBindings Before = MadFall::Input::GetActive();
	int32 Notified = 0;
	const FDelegateHandle Handle = MadFall::Input::OnBindingsChanged().AddLambda([&Notified]() { ++Notified; });
	MadFall::Input::SetActive(Keys);
	MadFall::Input::SetActive(Keys);
	TestEqual(TEXT("one notification for one change"), Notified, Before == Keys ? 0 : 1);
	TestEqual(TEXT("labels follow the active bindings"), MadFall::Input::GetKeyLabel(Jump), EKeys::J.GetDisplayName(false).ToString());
	MadFall::Input::SetActive(Before);
	MadFall::Input::OnBindingsChanged().Remove(Handle);
	return true;
}

#endif
