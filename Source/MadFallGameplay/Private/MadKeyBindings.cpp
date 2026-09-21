// Copyright MadFall. All Rights Reserved.

#include "MadKeyBindings.h"

namespace
{
	FMadRebindableAction MakeAction(const TCHAR* Id, const FKey& Default)
	{
		return { FName(Id), Default, FString::Printf(TEXT("@menu.key_%s"), Id) };
	}
}

const TArray<FMadRebindableAction>& FMadKeyBindings::GetActions()
{
	// Order is the controls page's order, and the order saved overrides apply in.
	static const TArray<FMadRebindableAction> Actions = {
		MakeAction(TEXT("move_forward"), EKeys::W),
		MakeAction(TEXT("move_back"), EKeys::S),
		MakeAction(TEXT("move_left"), EKeys::A),
		MakeAction(TEXT("move_right"), EKeys::D),
		MakeAction(TEXT("jump"), EKeys::SpaceBar),
		MakeAction(TEXT("sprint"), EKeys::LeftShift),
		MakeAction(TEXT("interact"), EKeys::E),
		MakeAction(TEXT("inventory"), EKeys::I),
		MakeAction(TEXT("craft"), EKeys::Tab),
		MakeAction(TEXT("map"), EKeys::M),
		MakeAction(TEXT("repair"), EKeys::R),
		MakeAction(TEXT("drop"), EKeys::Q),
		// Creative flight only: sinks while held.
		MakeAction(TEXT("fly_down"), EKeys::LeftControl),
	};
	return Actions;
}

const FMadRebindableAction* FMadKeyBindings::FindAction(FName Id)
{
	return GetActions().FindByPredicate([Id](const FMadRebindableAction& Action) { return Action.Id == Id; });
}

bool FMadKeyBindings::IsReserved(const FKey& Key)
{
	static const TArray<FKey> Reserved = {
		EKeys::Escape, EKeys::P,
		EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine,
		EKeys::MouseWheelAxis, EKeys::MouseScrollUp, EKeys::MouseScrollDown, EKeys::Mouse2D, EKeys::MouseX, EKeys::MouseY
	};
	return Key.IsMouseButton() || Reserved.Contains(Key);
}

FKey FMadKeyBindings::Get(FName Action) const
{
	if (const FKey* Override = Overrides.Find(Action))
	{
		return *Override;
	}
	const FMadRebindableAction* Info = FindAction(Action);
	return Info != nullptr ? Info->Default : FKey();
}

EMadBindResult FMadKeyBindings::Set(FName Action, const FKey& Key)
{
	const FMadRebindableAction* Info = FindAction(Action);
	if (Info == nullptr)
	{
		return EMadBindResult::UnknownAction;
	}
	// Keyboard keys only: an axis or a gamepad button is not something the
	// controls page can show or the mapping code maps as a button.
	if (!Key.IsValid() || Key.IsGamepadKey() || Key.IsAxis1D() || Key.IsAxis2D() || Key.IsAxis3D() || Key.IsTouch())
	{
		return EMadBindResult::InvalidKey;
	}
	if (IsReserved(Key))
	{
		return EMadBindResult::ReservedKey;
	}

	auto Assign = [this](const FMadRebindableAction& Target, const FKey& NewKey)
	{
		if (NewKey == Target.Default)
		{
			Overrides.Remove(Target.Id);
		}
		else
		{
			Overrides.Add(Target.Id, NewKey);
		}
	};

	const FKey Previous = Get(Action);
	if (Previous == Key)
	{
		return EMadBindResult::Ok;
	}
	for (const FMadRebindableAction& Other : GetActions())
	{
		if (Other.Id != Action && Get(Other.Id) == Key)
		{
			Assign(Other, Previous);
		}
	}
	Assign(*Info, Key);
	return EMadBindResult::Ok;
}

TMap<FName, FString> FMadKeyBindings::ToOverrides() const
{
	TMap<FName, FString> Out;
	for (const TPair<FName, FKey>& Pair : Overrides)
	{
		Out.Add(Pair.Key, Pair.Value.GetFName().ToString());
	}
	return Out;
}

void FMadKeyBindings::FromOverrides(const TMap<FName, FString>& In)
{
	ResetToDefaults();
	for (const FMadRebindableAction& Action : GetActions())
	{
		if (const FString* KeyName = In.Find(Action.Id))
		{
			// Invalid, reserved and unknown entries are simply not applied.
			Set(Action.Id, FKey(FName(**KeyName)));
		}
	}
}

namespace MadFall::Input
{
	const TCHAR* ToString(EMadBindResult Result)
	{
		switch (Result)
		{
		case EMadBindResult::Ok:            return TEXT("ok");
		case EMadBindResult::UnknownAction: return TEXT("no such action");
		case EMadBindResult::InvalidKey:    return TEXT("not a keyboard key");
		case EMadBindResult::ReservedKey:   return TEXT("that key is reserved (Escape, P, 1-9 and the mouse)");
		default:                            return TEXT("?");
		}
	}

	namespace
	{
		FMadKeyBindings& Active()
		{
			static FMadKeyBindings Bindings;
			return Bindings;
		}
	}

	const FMadKeyBindings& GetActive()
	{
		return Active();
	}

	void SetActive(const FMadKeyBindings& Bindings)
	{
		if (Active() == Bindings)
		{
			return;
		}
		Active() = Bindings;
		OnBindingsChanged().Broadcast();
	}

	FOnBindingsChanged& OnBindingsChanged()
	{
		static FOnBindingsChanged Delegate;
		return Delegate;
	}

	FString GetKeyLabel(FName Action)
	{
		const FKey Key = GetActive().Get(Action);
		return Key.IsValid() ? Key.GetDisplayName(/*bLongDisplayName*/ false).ToString() : FString(TEXT("?"));
	}
}
