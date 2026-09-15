// Copyright MadFall. All Rights Reserved.

#include "MadSettings.h"

#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "MadFallGameplay.h"
#include "MadKeyBindings.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Scalability.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	TAutoConsoleVariable<float> CVarLookSensitivity(
		TEXT("mad.input.LookSensitivity"), 1.0f,
		TEXT("Mouse look speed multiplier."));

	TAutoConsoleVariable<bool> CVarInvertY(
		TEXT("mad.input.InvertY"), false,
		TEXT("Inverts vertical mouse look."));

	TAutoConsoleVariable<float> CVarFieldOfView(
		TEXT("mad.view.FieldOfView"), 90.0f,
		TEXT("Horizontal field of view, degrees."));

	const TCHAR* SettingsSchema = TEXT("madfall.settings/1");

	void SetCVar(const TCHAR* Name, const FString& Value)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			// SetByConsole: a player's saved choice outranks project defaults, and
			// a -ExecCmds= line later in startup still outranks it.
			Variable->Set(*Value, ECVF_SetByConsole);
		}
	}
}

void FMadSettings::Sanitize()
{
	LookSensitivity = FMath::Clamp(LookSensitivity, 0.1f, 5.0f);
	FieldOfView = FMath::Clamp(FieldOfView, 60.0f, 120.0f);
	ViewDistance = FMath::Clamp(ViewDistance, 3, 16);
	Quality = FMath::Clamp(Quality, 0, 3);
	Volume = FMath::Clamp(Volume, 0.0f, 1.0f);
	Language = Language.TrimStartAndEnd().Left(16);

	// Round trip through the bindings so unknown actions, bad key names and
	// clashes a hand-edited file might contain are resolved the same way here
	// as in the game.
	FMadKeyBindings Bindings;
	Bindings.FromOverrides(KeyBindings);
	KeyBindings = Bindings.ToOverrides();
}

FString MadFall::Settings::GetPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Config"), TEXT("MadFallSettings.json"));
}

FMadSettings MadFall::Settings::Load(const FString& Path)
{
	FMadSettings Out;
	FString Text;
	TSharedPtr<FJsonObject> Json;
	if (FFileHelper::LoadFileToString(Text, *Path) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json)
		&& Json.IsValid() && Json->GetStringField(TEXT("schema")) == SettingsSchema)
	{
		double Number = 0.0;
		if (Json->TryGetNumberField(TEXT("look_sensitivity"), Number)) { Out.LookSensitivity = static_cast<float>(Number); }
		Json->TryGetBoolField(TEXT("invert_y"), Out.bInvertY);
		if (Json->TryGetNumberField(TEXT("field_of_view"), Number)) { Out.FieldOfView = static_cast<float>(Number); }
		if (Json->TryGetNumberField(TEXT("view_distance"), Number)) { Out.ViewDistance = static_cast<int32>(Number); }
		if (Json->TryGetNumberField(TEXT("quality"), Number)) { Out.Quality = static_cast<int32>(Number); }
		if (Json->TryGetNumberField(TEXT("volume"), Number)) { Out.Volume = static_cast<float>(Number); }
		Json->TryGetStringField(TEXT("language"), Out.Language);
		const TSharedPtr<FJsonObject>* Keys = nullptr;
		if (Json->TryGetObjectField(TEXT("keys"), Keys))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Keys)->Values)
			{
				FString KeyName;
				if (Pair.Value.IsValid() && Pair.Value->TryGetString(KeyName))
				{
					Out.KeyBindings.Add(FName(*Pair.Key), KeyName);
				}
			}
		}
	}
	Out.Sanitize();
	return Out;
}

bool MadFall::Settings::Save(const FMadSettings& In, const FString& Path, FString& OutError)
{
	FMadSettings Settings = In;
	Settings.Sanitize();

	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("schema"), SettingsSchema);
	Json->SetNumberField(TEXT("look_sensitivity"), Settings.LookSensitivity);
	Json->SetBoolField(TEXT("invert_y"), Settings.bInvertY);
	Json->SetNumberField(TEXT("field_of_view"), Settings.FieldOfView);
	Json->SetNumberField(TEXT("view_distance"), Settings.ViewDistance);
	Json->SetNumberField(TEXT("quality"), Settings.Quality);
	Json->SetNumberField(TEXT("volume"), Settings.Volume);
	Json->SetStringField(TEXT("language"), Settings.Language);

	const TSharedRef<FJsonObject> Keys = MakeShared<FJsonObject>();
	TArray<FName> Actions;
	Settings.KeyBindings.GetKeys(Actions);
	Actions.Sort(FNameLexicalLess());
	for (const FName& Action : Actions)
	{
		Keys->SetStringField(Action.ToString(), Settings.KeyBindings[Action]);
	}
	Json->SetObjectField(TEXT("keys"), Keys);

	FString Text;
	FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text));
	if (!FFileHelper::SaveStringToFile(Text, *Path))
	{
		OutError = FString::Printf(TEXT("could not write %s"), *Path);
		return false;
	}
	return true;
}

void MadFall::Settings::Apply(const FMadSettings& In)
{
	FMadSettings Settings = In;
	Settings.Sanitize();
	SetCVar(TEXT("mad.input.LookSensitivity"), LexToString(Settings.LookSensitivity));
	SetCVar(TEXT("mad.input.InvertY"), Settings.bInvertY ? TEXT("1") : TEXT("0"));
	SetCVar(TEXT("mad.view.FieldOfView"), LexToString(Settings.FieldOfView));
	SetCVar(TEXT("mad.stream.Radius"), LexToString(Settings.ViewDistance));
	Scalability::FQualityLevels Levels = Scalability::GetQualityLevels();
	Levels.SetFromSingleQualityLevel(Settings.Quality);
	Scalability::SetQualityLevels(Levels, /*bForce*/ true);
	SetCVar(TEXT("mad.models.LightShadows"), Settings.Quality >= 2 ? TEXT("1") : TEXT("0"));
	SetCVar(TEXT("mad.audio.Volume"), LexToString(Settings.Volume));
	SetCVar(TEXT("mad.Language"), Settings.Language);

	FMadKeyBindings Bindings;
	Bindings.FromOverrides(Settings.KeyBindings);
	MadFall::Input::SetActive(Bindings);
}

FMadSettings MadFall::Settings::FromConsoleVariables()
{
	FMadSettings Out;
	Out.LookSensitivity = CVarLookSensitivity.GetValueOnGameThread();
	Out.bInvertY = CVarInvertY.GetValueOnGameThread();
	Out.FieldOfView = CVarFieldOfView.GetValueOnGameThread();
	if (const IConsoleVariable* Radius = IConsoleManager::Get().FindConsoleVariable(TEXT("mad.stream.Radius")))
	{
		Out.ViewDistance = Radius->GetInt();
	}
	// A level set group by group (from the console) reads as the lowest of them.
	Out.Quality = Scalability::GetQualityLevels().GetMinQualityLevel();
	if (const IConsoleVariable* Volume = IConsoleManager::Get().FindConsoleVariable(TEXT("mad.audio.Volume")))
	{
		Out.Volume = Volume->GetFloat();
	}
	if (const IConsoleVariable* Language = IConsoleManager::Get().FindConsoleVariable(TEXT("mad.Language")))
	{
		Out.Language = Language->GetString();
	}
	Out.KeyBindings = MadFall::Input::GetActive().ToOverrides();
	Out.Sanitize();
	return Out;
}

void MadFall::Settings::EnsureApplied()
{
	static bool bApplied = false;
	if (bApplied)
	{
		return;
	}
	bApplied = true;

	// Automated runs keep project defaults: a developer's saved view distance
	// must not change what CI measures. That includes graphics quality, which
	// the engine also persists by itself (the `scalability` console command
	// saves to GameUserSettings.ini): a probe once left shadows off at 50%
	// resolution, and every rendered screenshot and frame-time check after it
	// ran at Low without saying so. Epic is forced here, not saved.
	if (FParse::Param(FCommandLine::Get(), TEXT("unattended")) || FParse::Param(FCommandLine::Get(), TEXT("MadDefaultSettings")))
	{
		Scalability::FQualityLevels Levels = Scalability::GetQualityLevels();
		Levels.SetFromSingleQualityLevel(3);
		Scalability::SetQualityLevels(Levels, /*bForce*/ true);
		return;
	}
	Apply(Load(GetPath()));
	UE_LOG(LogMadFallGameplay, Log, TEXT("Settings applied from %s."), *GetPath());
}
