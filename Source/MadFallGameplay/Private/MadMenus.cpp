// Copyright MadFall. All Rights Reserved.

#include "MadMenus.h"

#include "Camera/CameraActor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "MadAudioSubsystem.h"
#include "MadConsoleScript.h"
#include "MadDifficulty.h"
#include "MadFallGameplay.h"
#include "MadFrameBudget.h"
#include "MadGameplaySaveSubsystem.h"
#include "MadLocalization.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWorldClockSubsystem.h"
#include "MadWorldGenerator.h"
#include "Misc/DefaultValueHelper.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWeakWidget.h"
#include "Widgets/Text/STextBlock.h"

#include "MadKeyBindings.h"
#include "MadWorldBackup.h"

#define LOCTEXT_NAMESPACE "MadFallMenus"

namespace
{
	FText L(const TCHAR* KeyOrText)
	{
		return FText::FromString(MadFall::Localize(KeyOrText));
	}

	const FLinearColor Accent(0.85f, 0.25f, 0.15f);
	const FLinearColor Panel(0.02f, 0.02f, 0.025f, 0.78f);

	/**
	 * The menus' look, built once. The engine's default Slate buttons (grey
	 * bevels, centred labels) read as an editor dialog in front of a game world;
	 * these are dark rounded slabs with a thin outline that light up in the
	 * accent colour under the mouse. No assets: Slate draws rounded boxes itself.
	 */
	struct FMadMenuStyle
	{
		FSlateRoundedBoxBrush PanelBrush = FSlateRoundedBoxBrush(FLinearColor(0.015f, 0.015f, 0.02f, 0.82f), 10.0f, FLinearColor(1.0f, 1.0f, 1.0f, 0.08f), 1.0f);
		FSlateRoundedBoxBrush RuleBrush = FSlateRoundedBoxBrush(Accent, 1.5f);
		FButtonStyle Button;

		FMadMenuStyle()
		{
			const FSlateRoundedBoxBrush Normal(FLinearColor(0.06f, 0.06f, 0.07f, 0.9f), 6.0f, FLinearColor(1.0f, 1.0f, 1.0f, 0.1f), 1.0f);
			const FSlateRoundedBoxBrush Hovered(FLinearColor(Accent.R * 0.45f, Accent.G * 0.45f, Accent.B * 0.45f, 0.95f), 6.0f, Accent, 1.0f);
			const FSlateRoundedBoxBrush Pressed(FLinearColor(Accent.R * 0.7f, Accent.G * 0.7f, Accent.B * 0.7f, 1.0f), 6.0f, Accent, 1.0f);
			const FSlateRoundedBoxBrush Disabled(FLinearColor(0.04f, 0.04f, 0.045f, 0.6f), 6.0f, FLinearColor(1.0f, 1.0f, 1.0f, 0.04f), 1.0f);
			Button = FButtonStyle()
				.SetNormal(Normal)
				.SetHovered(Hovered)
				.SetPressed(Pressed)
				.SetDisabled(Disabled)
				.SetNormalPadding(FMargin(16.0f, 4.0f, 16.0f, 4.0f))
				.SetPressedPadding(FMargin(17.0f, 5.0f, 15.0f, 3.0f));
		}

		static const FMadMenuStyle& Get()
		{
			static const FMadMenuStyle Style;
			return Style;
		}
	};
}

// ===========================================================================
// Widget
// ===========================================================================

class SMadMenuWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMadMenuWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& Args, UMadMenuSubsystem* InOwner)
	{
		Owner = InOwner;

		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
			.BorderBackgroundColor(this, &SMadMenuWidget::GetBackdrop)
			.Padding(FMargin(64.0f, 48.0f))
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(560.0f)
				[
					SNew(SWidgetSwitcher)
					.WidgetIndex(this, &SMadMenuWidget::GetPageIndex)
					+ SWidgetSwitcher::Slot() [ SNullWidget::NullWidget ]
					+ SWidgetSwitcher::Slot() [ BuildTitle() ]
					+ SWidgetSwitcher::Slot() [ BuildNewWorld() ]
					+ SWidgetSwitcher::Slot() [ BuildLoadWorld() ]
					+ SWidgetSwitcher::Slot() [ BuildSettings() ]
					+ SWidgetSwitcher::Slot() [ BuildPause() ]
				+ SWidgetSwitcher::Slot() [ BuildHelp() ]
				+ SWidgetSwitcher::Slot() [ BuildControls() ]
				]
			]
		];
	}

	virtual bool SupportsKeyboardFocus() const override { return true; }

	/**
	 * While a Controls row waits for a key, the key is taken here, before the
	 * focused button sees it: a button would treat Space or Enter as a click
	 * and start waiting all over again.
	 */
	virtual FReply OnPreviewKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override
	{
		if (Capturing.IsNone())
		{
			return FReply::Unhandled();
		}
		const FName Action = Capturing;
		Capturing = NAME_None;
		if (Event.GetKey() == EKeys::Escape)
		{
			Error = FText::GetEmpty();
			return FReply::Handled();
		}
		const EMadBindResult Result = PendingKeys.Set(Action, Event.GetKey());
		Error = Result == EMadBindResult::Ok ? FText::GetEmpty()
			: FText::FromString(FString::Printf(TEXT("%s: %s"), *Event.GetKey().GetDisplayName().ToString(), MadFall::Input::ToString(Result)));
		return FReply::Handled();
	}

	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override
	{
		UMadMenuSubsystem* Menus = Owner.Get();
		if (Menus == nullptr || Event.GetKey() != EKeys::Escape)
		{
			return FReply::Unhandled();
		}
		switch (Menus->GetPage())
		{
		case EMadMenuPage::Pause:     Menus->Resume(); break;
		case EMadMenuPage::Settings:  Menus->ShowPage(Menus->GetSettingsReturnPage()); break;
		case EMadMenuPage::Controls:  Menus->ShowPage(EMadMenuPage::Settings); break;
		case EMadMenuPage::NewWorld:
		case EMadMenuPage::Help:
		case EMadMenuPage::LoadWorld: Menus->ShowPage(EMadMenuPage::Title); break;
		default: break;
		}
		return FReply::Handled();
	}

	/** Called when a page is shown, so lists and fields reflect the current state. */
	void Refresh(EMadMenuPage Page)
	{
		Error = FText::GetEmpty();
		if (Page == EMadMenuPage::LoadWorld || Page == EMadMenuPage::Title)
		{
			RebuildWorldList();
		}
		if (Page == EMadMenuPage::Settings && Owner.IsValid())
		{
			// Back from Controls, the unsaved edits on this page survive; only the
			// keys, which Controls saves itself, are refreshed.
			if (bKeepPendingSettings)
			{
				Pending.KeyBindings = Owner->GetSettings().KeyBindings;
			}
			else
			{
				Pending = Owner->GetSettings();
			}
		}
		if (Page != EMadMenuPage::Controls)
		{
			bKeepPendingSettings = false;
		}
		if (Page == EMadMenuPage::Controls && Owner.IsValid())
		{
			PendingKeys.FromOverrides(Owner->GetSettings().KeyBindings);
			Capturing = NAME_None;
		}
	}

private:
	TWeakObjectPtr<UMadMenuSubsystem> Owner;
	FText Error;
	FMadSettings Pending;
	FMadKeyBindings PendingKeys;

	/** The action whose row is waiting for a key press. */
	FName Capturing;

	/** Set while Controls is open over Settings, so returning keeps Settings' unsaved edits. */
	bool bKeepPendingSettings = false;
	TSharedPtr<SEditableTextBox> NameBox;
	TSharedPtr<SEditableTextBox> SeedBox;
	FName NewDifficulty = MadFall::Difficulty::Normal;
	TSharedPtr<SVerticalBox> WorldList;
	FString ConfirmDelete;
	FString ConfirmRestore;

	FSlateColor GetBackdrop() const
	{
		// The title shows its world behind a dark gradient-free veil; the pause
		// menu dims the game more, since the game behind it is frozen.
		const bool bTitle = Owner.IsValid() && Owner->IsTitle();
		return FLinearColor(0.0f, 0.0f, 0.0f, bTitle ? 0.25f : 0.55f);
	}

	int32 GetPageIndex() const
	{
		return Owner.IsValid() ? static_cast<int32>(Owner->GetPage()) : 0;
	}

	static TSharedRef<SWidget> Heading(const FText& Text, int32 Size = 40)
	{
		// A heading with a short accent rule under it, the menus' one flourish.
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).Text(Text).Font(FCoreStyle::GetDefaultFontStyle("Bold", Size)).ColorAndOpacity(FLinearColor::White)
				.ShadowOffset(FVector2D(2.0f, 2.0f)).ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.7f))
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(72.0f).HeightOverride(3.0f)
				[
					SNew(SBorder).BorderImage(&FMadMenuStyle::Get().RuleBrush)
				]
			];
	}

	TSharedRef<SWidget> Button(TAttribute<FText> Text, TFunction<void()> OnClick, TAttribute<bool> Enabled = true)
	{
		return SNew(SBox)
			.HeightOverride(48.0f)
			.Padding(FMargin(0.0f, 4.0f))
			[
				SNew(SButton)
				.ButtonStyle(&FMadMenuStyle::Get().Button)
				.IsEnabled(Enabled)
				.VAlign(VAlign_Center)
				.OnClicked_Lambda([this, OnClick]()
				{
					if (UMadMenuSubsystem* Menus = Owner.Get())
					{
						if (UMadAudioSubsystem* Audio = Menus->GetWorld() ? Menus->GetWorld()->GetSubsystem<UMadAudioSubsystem>() : nullptr)
						{
							Audio->Play2D(EMadSound::UIClick, 0.5f);
						}
					}
					OnClick();
					return FReply::Handled();
				})
				[
					SNew(STextBlock).Text(Text).Font(FCoreStyle::GetDefaultFontStyle("Regular", 18))
					.ColorAndOpacity(FLinearColor(0.92f, 0.9f, 0.86f))
					.ShadowOffset(FVector2D(1.0f, 1.0f))
					.ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.6f))
				]
			];
	}

	TSharedRef<SWidget> ErrorLine()
	{
		return SNew(STextBlock)
			.Text_Lambda([this]() { return Error; })
			.ColorAndOpacity(FLinearColor(1.0f, 0.45f, 0.35f))
			.AutoWrapText(true);
	}

	TSharedRef<SWidget> Panelled(TSharedRef<SWidget> Content)
	{
		return SNew(SBorder)
			.BorderImage(&FMadMenuStyle::Get().PanelBrush)
			.Padding(FMargin(28.0f, 24.0f))
			[
				Content
			];
	}

	TSharedRef<SWidget> BuildTitle()
	{
		return Panelled(SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight() [ SNew(STextBlock).Text(FText::FromString(TEXT("MADFALL"))).Font(FCoreStyle::GetDefaultFontStyle("Bold", 72)).ColorAndOpacity(Accent)
				.ShadowOffset(FVector2D(3.0f, 3.0f)).ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f)) ]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0, 0, 0, 10) [ SNew(SBox).WidthOverride(260.0f).HeightOverride(3.0f) [ SNew(SBorder).BorderImage(&FMadMenuStyle::Get().RuleBrush) ] ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 24) [ SNew(STextBlock).Text(L(TEXT("@menu.tagline"))).Font(FCoreStyle::GetDefaultFontStyle("Italic", 16)).ColorAndOpacity(FLinearColor(0.82f, 0.8f, 0.76f))
				.ShadowOffset(FVector2D(1.0f, 1.0f)).ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.7f)) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(
				TAttribute<FText>::CreateLambda([this]() { return ContinueLabel(); }),
				[this]() { ContinueLatest(); },
				TAttribute<bool>::CreateLambda([this]() { return Owner.IsValid() && Owner->GetWorlds().Num() > 0; })) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(L(TEXT("@menu.new_world")), [this]() { Owner->ShowPage(EMadMenuPage::NewWorld); }) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(L(TEXT("@menu.load_world")), [this]() { Owner->ShowPage(EMadMenuPage::LoadWorld); }) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(L(TEXT("@menu.settings")), [this]() { Owner->ShowPage(EMadMenuPage::Settings); }) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(L(TEXT("@menu.how_to_play")), [this]() { Owner->ShowPage(EMadMenuPage::Help); }) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(L(TEXT("@menu.quit")), [this]() { Owner->QuitGame(); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0) [ ErrorLine() ]);
	}

	FText ContinueLabel() const
	{
		UMadMenuSubsystem* Menus = Owner.Get();
		if (Menus == nullptr || Menus->GetWorlds().Num() == 0)
		{
			return L(TEXT("@menu.continue"));
		}
		const FMadWorldInfo& Latest = Menus->GetWorlds()[0];
		return FText::FromString(FString::Printf(TEXT("%s: %s"), *MadFall::Localize(TEXT("@menu.continue")), *Latest.DisplayName));
	}

	void ContinueLatest()
	{
		UMadMenuSubsystem* Menus = Owner.Get();
		if (Menus == nullptr || Menus->GetWorlds().Num() == 0)
		{
			return;
		}
		FString Reason;
		if (!Menus->LoadWorld(Menus->GetWorlds()[0].Name, Reason))
		{
			Error = FText::FromString(Reason);
		}
	}

	TSharedRef<SWidget> BuildNewWorld()
	{
		return Panelled(SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16) [ Heading(L(TEXT("@menu.new_world"))) ]
			+ SVerticalBox::Slot().AutoHeight() [ SNew(STextBlock).Text(L(TEXT("@menu.world_name"))) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 12) [ SAssignNew(NameBox, SEditableTextBox).Text(FText::FromString(TEXT("New World"))).Font(FCoreStyle::GetDefaultFontStyle("Regular", 16)) ]
			+ SVerticalBox::Slot().AutoHeight() [ SNew(STextBlock).Text(L(TEXT("@menu.seed"))) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 12) [ SAssignNew(SeedBox, SEditableTextBox).HintText(L(TEXT("@menu.seed_hint"))).Font(FCoreStyle::GetDefaultFontStyle("Regular", 16)) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(
				TAttribute<FText>::CreateLambda([this]()
				{
					return FText::FromString(FString::Printf(TEXT("%s: %s"), *MadFall::Localize(TEXT("@menu.difficulty")),
						*MadFall::Localize(FString::Printf(TEXT("@menu.difficulty_%s"), *NewDifficulty.ToString()))));
				}),
				[this]()
				{
					const TArray<FName>& Levels = MadFall::Difficulty::GetLevels();
					NewDifficulty = Levels[(Levels.IndexOfByKey(NewDifficulty) + 1) % Levels.Num()];
				}) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16) [ SNew(STextBlock)
				.Text_Lambda([this]() { return L(*FString::Printf(TEXT("@menu.difficulty_%s_description"), *NewDifficulty.ToString())); })
				.ColorAndOpacity(FLinearColor(0.75f, 0.75f, 0.75f)).AutoWrapText(true) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(L(TEXT("@menu.create")), [this]()
				{
					FString Reason;
					if (!Owner->CreateWorld(NameBox->GetText().ToString(), SeedBox->GetText().ToString(), NewDifficulty, Reason))
					{
						Error = FText::FromString(Reason);
					}
				}) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(L(TEXT("@menu.back")), [this]() { Owner->ShowPage(EMadMenuPage::Title); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0) [ ErrorLine() ]);
	}

	TSharedRef<SWidget> BuildLoadWorld()
	{
		return Panelled(SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16) [ Heading(L(TEXT("@menu.load_world"))) ]
			+ SVerticalBox::Slot().MaxHeight(420.0f) [ SNew(SScrollBox) + SScrollBox::Slot() [ SAssignNew(WorldList, SVerticalBox) ] ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 0) [ Button(L(TEXT("@menu.back")), [this]() { Owner->ShowPage(EMadMenuPage::Title); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0) [ ErrorLine() ]);
	}

	void RebuildWorldList()
	{
		if (!WorldList.IsValid() || !Owner.IsValid())
		{
			return;
		}
		WorldList->ClearChildren();
		const TArray<FMadWorldInfo>& Worlds = Owner->GetWorlds();
		if (Worlds.Num() == 0)
		{
			WorldList->AddSlot().AutoHeight()[ SNew(STextBlock).Text(L(TEXT("@menu.no_worlds"))) ];
		}
		for (const FMadWorldInfo& World : Worlds)
		{
			const FString Name = World.Name;
			TArray<FMadWorldBackup> Backups;
			MadFall::WorldBackup::List(World.Directory, Backups);
			const int32 BackupCount = Backups.Num();
			const FString Details = FString::Printf(TEXT("%s %d  -  %s  -  %s  -  %s %lld  -  %d %s"),
				*MadFall::Localize(TEXT("@menu.day")), World.Day, *MadFall::Localize(FString::Printf(TEXT("@menu.difficulty_%s"), *World.Difficulty.ToString())),
				*World.LastPlayed.ToString(TEXT("%Y-%m-%d %H:%M")), *MadFall::Localize(TEXT("@menu.seed")), World.Seed,
				BackupCount, *MadFall::Localize(TEXT("@menu.backups")));
			// Name and details above, buttons in an even row below: side by side, the
			// details were cut off at 720p, where the menu is scaled down.
			WorldList->AddSlot().AutoHeight().Padding(0, 6)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight() [ SNew(STextBlock).Text(FText::FromString(World.DisplayName)).Font(FCoreStyle::GetDefaultFontStyle("Bold", 16)) ]
				+ SVerticalBox::Slot().AutoHeight() [ SNew(STextBlock).Text(FText::FromString(Details)).ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f)).AutoWrapText(true) ]
				+ SVerticalBox::Slot().AutoHeight()
				[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 6, 0) [ Button(L(TEXT("@menu.play")), [this, Name]()
					{
						FString Reason;
						if (!Owner->LoadWorld(Name, Reason)) { Error = FText::FromString(Reason); }
					}) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 6, 0) [ Button(
					TAttribute<FText>::CreateLambda([this, Name]() { return ConfirmRestore == Name ? L(TEXT("@menu.confirm_restore")) : L(TEXT("@menu.restore")); }),
					[this, Name]()
					{
						// Two clicks, like Delete: a restore replaces the world's files.
						if (ConfirmRestore != Name)
						{
							ConfirmRestore = Name;
							ConfirmDelete.Reset();
							RebuildWorldList();
							return;
						}
						ConfirmRestore.Reset();
						FString Reason;
						Error = Owner->RestoreWorldBackup(Name, FString(), Reason) ? L(TEXT("@menu.restored")) : FText::FromString(Reason);
						RebuildWorldList();
					},
					BackupCount > 0) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f) [ Button(
					TAttribute<FText>::CreateLambda([this, Name]() { return ConfirmDelete == Name ? L(TEXT("@menu.confirm_delete")) : L(TEXT("@menu.delete")); }),
					[this, Name]()
					{
						// Two clicks: the first arms the button, the second deletes.
						if (ConfirmDelete != Name)
						{
							ConfirmDelete = Name;
							RebuildWorldList();
							return;
						}
						ConfirmDelete.Reset();
						FString Reason;
						if (!Owner->DeleteWorld(Name, Reason)) { Error = FText::FromString(Reason); }
						RebuildWorldList();
					}) ]
				]
			];
		}
	}

	TSharedRef<SWidget> SettingRow(const FText& Label, TSharedRef<SWidget> Control)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(0.5f).VAlign(VAlign_Center) [ SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Regular", 16)) ]
			+ SHorizontalBox::Slot().FillWidth(0.5f).Padding(0, 4) [ Control ];
	}

	TSharedRef<SWidget> BuildSettings()
	{
		return Panelled(SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16) [ Heading(L(TEXT("@menu.settings"))) ]
			+ SVerticalBox::Slot().AutoHeight() [ SettingRow(L(TEXT("@menu.look_sensitivity")),
				SNew(SSpinBox<float>).MinValue(0.1f).MaxValue(5.0f).Delta(0.05f)
				.Value_Lambda([this]() { return Pending.LookSensitivity; })
				.OnValueChanged_Lambda([this](float V) { Pending.LookSensitivity = V; })) ]
			+ SVerticalBox::Slot().AutoHeight() [ SettingRow(L(TEXT("@menu.invert_y")),
				SNew(SCheckBox)
				.IsChecked_Lambda([this]() { return Pending.bInvertY ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { Pending.bInvertY = S == ECheckBoxState::Checked; })) ]
			+ SVerticalBox::Slot().AutoHeight() [ SettingRow(L(TEXT("@menu.field_of_view")),
				SNew(SSpinBox<float>).MinValue(60.0f).MaxValue(120.0f).Delta(1.0f)
				.Value_Lambda([this]() { return Pending.FieldOfView; })
				.OnValueChanged_Lambda([this](float V) { Pending.FieldOfView = V; })) ]
			+ SVerticalBox::Slot().AutoHeight() [ SettingRow(L(TEXT("@menu.view_distance")),
				SNew(SSpinBox<int32>).MinValue(3).MaxValue(16).Delta(1)
				.Value_Lambda([this]() { return Pending.ViewDistance; })
				.OnValueChanged_Lambda([this](int32 V) { Pending.ViewDistance = V; })) ]
			+ SVerticalBox::Slot().AutoHeight() [ SettingRow(L(TEXT("@menu.quality")),
				SNew(SButton)
				.OnClicked_Lambda([this]() { Pending.Quality = (Pending.Quality + 1) % 4; return FReply::Handled(); })
				[ SNew(STextBlock).Text_Lambda([this]() { return L(*FString::Printf(TEXT("@menu.quality_%d"), FMath::Clamp(Pending.Quality, 0, 3))); }) ]) ]
			+ SVerticalBox::Slot().AutoHeight() [ SettingRow(L(TEXT("@menu.volume")),
				SNew(SSpinBox<float>).MinValue(0.0f).MaxValue(1.0f).Delta(0.05f)
				.Value_Lambda([this]() { return Pending.Volume; })
				.OnValueChanged_Lambda([this](float V) { Pending.Volume = V; })) ]
			+ SVerticalBox::Slot().AutoHeight() [ SettingRow(L(TEXT("@menu.music_volume")),
				SNew(SSpinBox<float>).MinValue(0.0f).MaxValue(1.0f).Delta(0.05f)
				.Value_Lambda([this]() { return Pending.MusicVolume; })
				.OnValueChanged_Lambda([this](float V) { Pending.MusicVolume = V; })) ]
			+ SVerticalBox::Slot().AutoHeight() [ SettingRow(L(TEXT("@menu.language")),
				SNew(SButton)
				.OnClicked_Lambda([this]() { CycleLanguage(); return FReply::Handled(); })
				[ SNew(STextBlock).Text_Lambda([this]() { return FText::FromString(Pending.Language.IsEmpty() ? MadFall::Localize(TEXT("@menu.language_system")) : Pending.Language); }) ]) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0) [ Button(L(TEXT("@menu.controls")), [this]()
				{
					bKeepPendingSettings = true;
					Owner->ShowPage(EMadMenuPage::Controls);
				}) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 16, 0, 0) [ Button(L(TEXT("@menu.apply")), [this]()
				{
					Owner->ApplyAndSaveSettings(Pending);
					Owner->ShowPage(Owner->GetSettingsReturnPage());
				}) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(L(TEXT("@menu.back")), [this]() { Owner->ShowPage(Owner->GetSettingsReturnPage()); }) ]);
	}

	void CycleLanguage()
	{
		TArray<FString> Options = MadFall::GetStrings().GetLanguages();
		Options.Sort();
		Options.Insert(FString(), 0);   // "follow the system"
		const int32 Current = Options.IndexOfByKey(Pending.Language);
		Pending.Language = Options[(Current + 1) % Options.Num()];
	}

	TSharedRef<SWidget> BuildHelp()
	{
		// Every line is a string, so a translation covers the help too.
		TSharedRef<SVerticalBox> Lines = SNew(SVerticalBox);
		// Lines run help_1, help_2, ... until one is missing in English.
		for (int32 Index = 1; Index < 100; ++Index)
		{
			const FString Key = FString::Printf(TEXT("menu.help_%d"), Index);
			if (MadFall::GetStrings().Find(Key, TEXT("en")) == nullptr)
			{
				break;
			}
			const FString Text = MadFall::Localize(TEXT("@") + Key);
			const bool bHeading = Text.StartsWith(TEXT("#"));
			Lines->AddSlot().AutoHeight().Padding(0, bHeading ? 10 : 2)
			[
				SNew(STextBlock).Text(FText::FromString(bHeading ? Text.Mid(1).TrimStart() : Text)).AutoWrapText(true)
				.Font(FCoreStyle::GetDefaultFontStyle(bHeading ? "Bold" : "Regular", bHeading ? 18 : 14))
				.ColorAndOpacity(bHeading ? Accent : FLinearColor(0.9f, 0.9f, 0.9f))
			];
		}
		return Panelled(SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12) [ Heading(L(TEXT("@menu.how_to_play"))) ]
			+ SVerticalBox::Slot().MaxHeight(480.0f) [ SNew(SBox).WidthOverride(620.0f) [ SNew(SScrollBox) + SScrollBox::Slot() [ Lines ] ] ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 0) [ Button(L(TEXT("@menu.back")), [this]() { Owner->ShowPage(EMadMenuPage::Title); }) ]);
	}

	TSharedRef<SWidget> BuildControls()
	{
		TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
		for (const FMadRebindableAction& Action : FMadKeyBindings::GetActions())
		{
			const FName Id = Action.Id;
			Rows->AddSlot().AutoHeight()
			[
				SettingRow(L(*Action.Label),
					SNew(SButton)
					.HAlign(HAlign_Center)
					.OnClicked_Lambda([this, Id]() { Capturing = Id; Error = FText::GetEmpty(); return FReply::Handled(); })
					[
						SNew(STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
						.Text_Lambda([this, Id]()
						{
							return Capturing == Id ? L(TEXT("@menu.press_a_key")) : PendingKeys.Get(Id).GetDisplayName(false);
						})
					])
			];
		}
		return Panelled(SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12) [ Heading(L(TEXT("@menu.controls"))) ]
			+ SVerticalBox::Slot().MaxHeight(440.0f) [ SNew(SScrollBox) + SScrollBox::Slot() [ Rows ] ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0) [ SNew(STextBlock).Text(L(TEXT("@menu.controls_reserved"))).ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f)).AutoWrapText(true) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0) [ Button(L(TEXT("@menu.reset_defaults")), [this]() { PendingKeys.ResetToDefaults(); Capturing = NAME_None; }) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(L(TEXT("@menu.apply")), [this]()
				{
					FMadSettings Settings = Owner->GetSettings();
					Settings.KeyBindings = PendingKeys.ToOverrides();
					Owner->ApplyAndSaveSettings(Settings);
					Owner->ShowPage(EMadMenuPage::Settings);
				}) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(L(TEXT("@menu.back")), [this]() { Owner->ShowPage(EMadMenuPage::Settings); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0) [ ErrorLine() ]);
	}

	TSharedRef<SWidget> BuildPause()
	{
		return Panelled(SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16) [ Heading(L(TEXT("@menu.paused"))) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(L(TEXT("@menu.resume")), [this]() { Owner->Resume(); }) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(L(TEXT("@menu.settings")), [this]() { Owner->ShowPage(EMadMenuPage::Settings); }) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(L(TEXT("@menu.save_and_quit")), [this]() { Owner->SaveAndQuitToTitle(); }) ]
			+ SVerticalBox::Slot().AutoHeight() [ Button(L(TEXT("@menu.quit_game")), [this]() { Owner->QuitGame(); }) ]);
	}
};

// ===========================================================================
// Subsystem
// ===========================================================================

bool UMadMenuSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

TStatId UMadMenuSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadMenuSubsystem, STATGROUP_Tickables);
}

void UMadMenuSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	MadFall::Settings::EnsureApplied();

	const UMadVoxelWorldSubsystem* VoxelWorld = InWorld.GetSubsystem<UMadVoxelWorldSubsystem>();
	bTitle = MadFall::Session::IsTitleScreen() && VoxelWorld != nullptr && VoxelWorld->IsReadOnly();
	if (bTitle)
	{
		ShowPage(EMadMenuPage::Title);
	}
}

void UMadMenuSubsystem::Deinitialize()
{
	if (WidgetContainer.IsValid() && GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(WidgetContainer.ToSharedRef());
	}
	WidgetContainer.Reset();
	Widget.Reset();
	Super::Deinitialize();
}

void UMadMenuSubsystem::SetupTitleScene()
{
	UWorld* World = GetWorld();
	APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	const UMadVoxelWorldSubsystem* VoxelWorld = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
	const FMadWorldGenerator* Generator = VoxelWorld ? VoxelWorld->GetWorldGenerator() : nullptr;
	if (Controller == nullptr || Generator == nullptr)
	{
		return;   // try again next frame; the controller spawns after begin play
	}
	bTitleSceneReady = true;

	// Somewhere wooded and near the origin, so the backdrop has something in it.
	FIntPoint Column(0, 0);
	const int32 Forest = UMadVoxelWorldSubsystem::GetBiomeRegistry().FindIndex(FName(TEXT("madfall:forest")));
	if (Forest != INDEX_NONE)
	{
		Generator->FindBiomeNear(Forest, FIntPoint::ZeroValue, 3000, Column);
	}
	const float Surface = Generator->GetSurfaceHeight(static_cast<float>(Column.X), static_cast<float>(Column.Y));
	OrbitCentre = FVector((Column.X + 0.5) * MadFall::VoxelSizeUU, (Column.Y + 0.5) * MadFall::VoxelSizeUU, (Surface + 6.0) * MadFall::VoxelSizeUU);

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	TitleCamera = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform(OrbitCentre), Params);
	if (TitleCamera != nullptr)
	{
		Controller->SetViewTarget(TitleCamera);
	}

	// Late afternoon, held still: the light is the best the sky model does.
	if (UMadWorldClockSubsystem* Clock = World->GetSubsystem<UMadWorldClockSubsystem>())
	{
		Clock->SetTime(16.5f, 1);
		Clock->SetPaused(true);
	}
}

void UMadMenuSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	MAD_FRAME_SCOPE(Other);

	// With no survivor ticking (the title screen) or the game paused, the menu
	// runs the console script, so scripted runs (CI, screenshots) can drive the
	// menus the way a player clicks them.
	if (bTitle || GetWorld()->IsPaused())
	{
		MadFall::ConsoleScript::Tick(GetWorld(), DeltaTime);
	}

	if (bTitle)
	{
		if (!bTitleSceneReady)
		{
			SetupTitleScene();
		}
		if (TitleCamera != nullptr)
		{
			// One slow lap every four minutes, 24 voxels out and 10 up.
			OrbitAngle = FMath::Fmod(OrbitAngle + DeltaTime * (UE_TWO_PI / 240.0f), UE_TWO_PI);
			const FVector Offset(FMath::Cos(OrbitAngle) * 2400.0, FMath::Sin(OrbitAngle) * 2400.0, 1000.0);
			TitleCamera->SetActorLocationAndRotation(OrbitCentre + Offset, (-Offset).Rotation());
		}
	}

	// A viewport can appear after begin play (and never does headless).
	if (Page != EMadMenuPage::None && !WidgetContainer.IsValid() && GEngine && GEngine->GameViewport)
	{
		ShowPage(Page);
	}
}

void UMadMenuSubsystem::ShowPage(EMadMenuPage NewPage)
{
	Page = NewPage;
	if (NewPage == EMadMenuPage::LoadWorld || NewPage == EMadMenuPage::Title)
	{
		bWorldsDirty = true;
	}

	if (GEngine && GEngine->GameViewport && !Widget.IsValid())
	{
		Widget = SNew(SMadMenuWidget, this);
		WidgetContainer = SNew(SWeakWidget).PossiblyNullContent(Widget);
		GEngine->GameViewport->AddViewportWidgetContent(WidgetContainer.ToSharedRef(), 100);
	}
	if (Widget.IsValid())
	{
		Widget->Refresh(NewPage);
		Widget->SetVisibility(NewPage == EMadMenuPage::None ? EVisibility::Collapsed : EVisibility::Visible);
	}
	UpdateInputMode();
}

void UMadMenuSubsystem::UpdateInputMode()
{
	APlayerController* Controller = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (Controller == nullptr)
	{
		return;
	}
	if (Page != EMadMenuPage::None)
	{
		FInputModeUIOnly Mode;
		if (Widget.IsValid())
		{
			Mode.SetWidgetToFocus(Widget);
		}
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Controller->SetInputMode(Mode);
		Controller->SetShowMouseCursor(true);
	}
	else
	{
		Controller->SetInputMode(FInputModeGameOnly());
		Controller->SetShowMouseCursor(false);
	}
}

const TArray<FMadWorldInfo>& UMadMenuSubsystem::GetWorlds()
{
	if (bWorldsDirty)
	{
		bWorldsDirty = false;
		MadFall::Session::ListWorlds(Worlds, MadFall::Session::GetWorldsRoot());
	}
	return Worlds;
}

bool UMadMenuSubsystem::CreateWorld(const FString& DisplayName, const FString& SeedText, FName Difficulty, FString& OutError)
{
	if (!MadFall::Difficulty::IsValid(Difficulty))
	{
		OutError = FString::Printf(TEXT("unknown difficulty '%s'"), *Difficulty.ToString());
		return false;
	}
	const FString Name = MadFall::Session::MakeWorldName(DisplayName);
	if (!MadFall::Session::IsValidWorldName(Name))
	{
		OutError = MadFall::Localize(TEXT("@menu.error_name"));
		return false;
	}
	const FString Directory = FPaths::Combine(MadFall::Session::GetWorldsRoot(), Name);
	if (IFileManager::Get().DirectoryExists(*Directory))
	{
		OutError = FString::Printf(TEXT("%s (%s)"), *MadFall::Localize(TEXT("@menu.error_exists")), *Name);
		return false;
	}

	// world.json is written here with the display name; the voxel world keeps it.
	FMadWorldInfo Info;
	Info.Name = Name;
	Info.DisplayName = DisplayName.TrimStartAndEnd();
	Info.Seed = MadFall::Session::ParseSeed(SeedText);
	Info.Difficulty = Difficulty;
	Info.Created = FDateTime::UtcNow();
	Info.LastPlayed = Info.Created;
	Info.Directory = Directory;
	if (!MadFall::Session::WriteWorldInfo(Info, OutError))
	{
		return false;
	}

	UE_LOG(LogMadFallGameplay, Display, TEXT("Creating world %s (%s), seed %lld, %s."), *Name, *Info.DisplayName, Info.Seed, *Difficulty.ToString());
	MadFall::Session::SelectWorld(Name, Info.Seed);
	TravelToCurrentMap();
	return true;
}

bool UMadMenuSubsystem::LoadWorld(const FString& Name, FString& OutError)
{
	FMadWorldInfo Info;
	if (!MadFall::Session::FindWorld(Name, MadFall::Session::GetWorldsRoot(), Info))
	{
		OutError = FString::Printf(TEXT("%s: %s"), *MadFall::Localize(TEXT("@menu.error_missing")), *Name);
		return false;
	}
	UE_LOG(LogMadFallGameplay, Display, TEXT("Loading world %s (%s), seed %lld."), *Name, *Info.DisplayName, Info.Seed);
	if (!bTitle)
	{
		SaveEverything();
	}
	MadFall::Session::SelectWorld(Name, TOptional<int64>());
	TravelToCurrentMap();
	return true;
}

bool UMadMenuSubsystem::RestoreWorldBackup(const FString& Name, const FString& Backup, FString& OutError)
{
	FMadWorldInfo Info;
	if (!MadFall::Session::FindWorld(Name, MadFall::Session::GetWorldsRoot(), Info))
	{
		OutError = FString::Printf(TEXT("%s: %s"), *MadFall::Localize(TEXT("@menu.error_missing")), *Name);
		return false;
	}
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld() ? GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
	if (VoxelWorld != nullptr && FPaths::IsSamePath(VoxelWorld->GetWorldDirectory(), Info.Directory))
	{
		// Its region files are open and its state is in memory, to be saved over the restore.
		OutError = TEXT("cannot restore the world being played; quit to the title first");
		return false;
	}
	FString Target = Backup;
	if (Target.IsEmpty())
	{
		TArray<FMadWorldBackup> Backups;
		MadFall::WorldBackup::List(Info.Directory, Backups);
		if (Backups.Num() == 0)
		{
			OutError = FString::Printf(TEXT("%s has no backups"), *Name);
			return false;
		}
		Target = Backups[0].Name;
	}
	bWorldsDirty = true;
	const bool bOk = MadFall::WorldBackup::Restore(Info.Directory, Target, OutError);
	UE_LOG(LogMadFallGameplay, Display, TEXT("Restore world %s from backup %s: %s"), *Name, *Target, bOk ? TEXT("done") : *OutError);
	return bOk;
}

bool UMadMenuSubsystem::DeleteWorld(const FString& Name, FString& OutError)
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld() ? GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
	if (VoxelWorld != nullptr && FPaths::IsSamePath(VoxelWorld->GetWorldDirectory(), FPaths::Combine(MadFall::Session::GetWorldsRoot(), Name)))
	{
		OutError = TEXT("cannot delete the world being played");
		return false;
	}
	bWorldsDirty = true;
	const bool bOk = MadFall::Session::DeleteWorld(Name, MadFall::Session::GetWorldsRoot(), OutError);
	UE_LOG(LogMadFallGameplay, Display, TEXT("Delete world %s: %s"), *Name, bOk ? TEXT("done") : *OutError);
	return bOk;
}

void UMadMenuSubsystem::OpenPauseMenu()
{
	if (bTitle)
	{
		return;
	}
	UGameplayStatics::SetGamePaused(GetWorld(), true);
	ShowPage(EMadMenuPage::Pause);
}

void UMadMenuSubsystem::Resume()
{
	if (bTitle)
	{
		ShowPage(EMadMenuPage::Title);
		return;
	}
	ShowPage(EMadMenuPage::None);
	UGameplayStatics::SetGamePaused(GetWorld(), false);
}

void UMadMenuSubsystem::SaveEverything()
{
	UWorld* World = GetWorld();
	if (UMadVoxelWorldSubsystem* VoxelWorld = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr)
	{
		FString Error;
		if (!VoxelWorld->SaveAll(Error))
		{
			UE_LOG(LogMadFallGameplay, Error, TEXT("World save failed: %s"), *Error);
		}
	}
	if (UMadGameplaySaveSubsystem* Saves = World ? World->GetSubsystem<UMadGameplaySaveSubsystem>() : nullptr)
	{
		Saves->SaveNow();
	}
}

void UMadMenuSubsystem::SaveAndQuitToTitle()
{
	if (bTitle)
	{
		return;
	}
	SaveEverything();
	UGameplayStatics::SetGamePaused(GetWorld(), false);
	MadFall::Session::ReturnToTitle();
	UE_LOG(LogMadFallGameplay, Display, TEXT("Saved; returning to the title screen."));
	TravelToCurrentMap();
}

void UMadMenuSubsystem::QuitGame()
{
	if (!bTitle)
	{
		SaveEverything();
	}
	UE_LOG(LogMadFallGameplay, Display, TEXT("Quitting."));
	UKismetSystemLibrary::QuitGame(GetWorld(), GetWorld()->GetFirstPlayerController(), EQuitPreference::Quit, /*bIgnorePlatformRestrictions*/ false);
}

void UMadMenuSubsystem::TravelToCurrentMap()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	ShowPage(EMadMenuPage::None);
	// Reopening the same map rebuilds every world subsystem, and the voxel world
	// reads the session's selection in its Initialize.
	const FString Map = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
	UGameplayStatics::OpenLevel(World, FName(*Map));
}

void UMadMenuSubsystem::ApplyAndSaveSettings(const FMadSettings& Settings)
{
	MadFall::Settings::Apply(Settings);
	FString Error;
	if (!MadFall::Settings::Save(Settings, MadFall::Settings::GetPath(), Error))
	{
		UE_LOG(LogMadFallGameplay, Error, TEXT("Settings: %s"), *Error);
	}
}

// ===========================================================================
// Console
// ===========================================================================

namespace
{
	UMadMenuSubsystem* GetMenus(UWorld* World)
	{
		return World ? World->GetSubsystem<UMadMenuSubsystem>() : nullptr;
	}

	FAutoConsoleCommandWithWorldAndArgs GMadMenuNewWorld(
		TEXT("mad.menu.newworld"), TEXT("mad.menu.newworld <name> [seed] [easy|normal|hard] - what the New World screen's Create button does."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			FString Error;
			UMadMenuSubsystem* Menus = GetMenus(World);
			const FName Difficulty = Args.Num() > 2 ? FName(*Args[2].ToLower()) : MadFall::Difficulty::Normal;
			if (Menus == nullptr || Args.Num() < 1 || !Menus->CreateWorld(Args[0], Args.Num() > 1 ? Args[1] : FString(), Difficulty, Error))
			{
				UE_LOG(LogMadFallGameplay, Warning, TEXT("New world failed: %s"), *Error);
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GMadMenuShow(
		TEXT("mad.menu.show"), TEXT("mad.menu.show <title|newworld|load|settings|help> - opens a menu page."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			UMadMenuSubsystem* Menus = GetMenus(World);
			static const TMap<FString, EMadMenuPage> Pages = {
				{ TEXT("title"), EMadMenuPage::Title }, { TEXT("newworld"), EMadMenuPage::NewWorld }, { TEXT("load"), EMadMenuPage::LoadWorld },
				{ TEXT("settings"), EMadMenuPage::Settings }, { TEXT("help"), EMadMenuPage::Help }, { TEXT("controls"), EMadMenuPage::Controls } };
			const EMadMenuPage* Page = Args.Num() > 0 ? Pages.Find(Args[0].ToLower()) : nullptr;
			if (Menus == nullptr || Page == nullptr)
			{
				UE_LOG(LogMadFallGameplay, Warning, TEXT("usage: mad.menu.show <title|newworld|load|settings|help|controls>"));
				return;
			}
			Menus->ShowPage(*Page);
		}));

	FAutoConsoleCommandWithWorldAndArgs GMadMenuLoad(
		TEXT("mad.menu.load"), TEXT("mad.menu.load <name> - what a world's Play button does."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			FString Error;
			UMadMenuSubsystem* Menus = GetMenus(World);
			if (Menus == nullptr || Args.Num() < 1 || !Menus->LoadWorld(Args[0], Error))
			{
				UE_LOG(LogMadFallGameplay, Warning, TEXT("Load world failed: %s"), *Error);
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GMadWorldBackups(
		TEXT("mad.world.backups"), TEXT("mad.world.backups <name> - lists a world's backups, newest first."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			FMadWorldInfo Info;
			if (Args.Num() < 1 || !MadFall::Session::FindWorld(Args[0], MadFall::Session::GetWorldsRoot(), Info))
			{
				UE_LOG(LogMadFallGameplay, Warning, TEXT("usage: mad.world.backups <world name>"));
				return;
			}
			TArray<FMadWorldBackup> Backups;
			MadFall::WorldBackup::List(Info.Directory, Backups);
			FString Out = FString::Printf(TEXT("%s: %d backup(s)"), *Args[0], Backups.Num());
			for (const FMadWorldBackup& Backup : Backups)
			{
				Out += FString::Printf(TEXT("\n  %s  %.1f MB"), *Backup.Name, Backup.Bytes / (1024.0 * 1024.0));
			}
			UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Out);
		}));

	FAutoConsoleCommandWithWorldAndArgs GMadWorldRestore(
		TEXT("mad.world.restore"), TEXT("mad.world.restore <name> [backup] - what a world's Restore button does (the newest backup by default)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			FString Error;
			UMadMenuSubsystem* Menus = GetMenus(World);
			if (Menus == nullptr || Args.Num() < 1 || !Menus->RestoreWorldBackup(Args[0], Args.Num() > 1 ? Args[1] : FString(), Error))
			{
				UE_LOG(LogMadFallGameplay, Warning, TEXT("Restore failed: %s"), *Error);
			}
		}));

	FAutoConsoleCommandWithWorld GMadMenuWorlds(
		TEXT("mad.menu.worlds"), TEXT("Lists saved worlds, most recently played first."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			TArray<FMadWorldInfo> Worlds;
			MadFall::Session::ListWorlds(Worlds, MadFall::Session::GetWorldsRoot());
			UE_LOG(LogMadFallGameplay, Display, TEXT("%d world(s)%s:"), Worlds.Num(), MadFall::Session::IsTitleScreen() ? TEXT(" (at the title screen)") : TEXT(""));
			for (const FMadWorldInfo& Info : Worlds)
			{
				UE_LOG(LogMadFallGameplay, Display, TEXT("  %s (%s): seed %lld, day %d, %s, last played %s"),
					*Info.Name, *Info.DisplayName, Info.Seed, Info.Day, *Info.Difficulty.ToString(), *Info.LastPlayed.ToString());
			}
		}));

	FAutoConsoleCommandWithWorld GMadMenuTitle(
		TEXT("mad.menu.title"), TEXT("Saves and returns to the title screen."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (UMadMenuSubsystem* Menus = GetMenus(World)) { Menus->SaveAndQuitToTitle(); }
		}));

	FAutoConsoleCommandWithWorld GMadMenuPause(
		TEXT("mad.menu.pause"), TEXT("Opens the pause menu."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (UMadMenuSubsystem* Menus = GetMenus(World)) { Menus->OpenPauseMenu(); }
		}));

	FAutoConsoleCommandWithWorld GMadMenuResume(
		TEXT("mad.menu.resume"), TEXT("Closes the pause menu."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (UMadMenuSubsystem* Menus = GetMenus(World)) { Menus->Resume(); }
		}));

	FAutoConsoleCommandWithWorld GMadMenuStatus(
		TEXT("mad.menu.status"), TEXT("Title or game, the open page and the world being played."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			const UMadMenuSubsystem* Menus = GetMenus(World);
			const UMadVoxelWorldSubsystem* VoxelWorld = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
			UE_LOG(LogMadFallGameplay, Display, TEXT("Menu: %s, page %d, paused %s, world %s (seed %lld)"),
				Menus && Menus->IsTitle() ? TEXT("title") : TEXT("game"), Menus ? static_cast<int32>(Menus->GetPage()) : -1,
				World && World->IsPaused() ? TEXT("yes") : TEXT("no"),
				VoxelWorld ? *FPaths::GetCleanFilename(VoxelWorld->GetWorldDirectory()) : TEXT("none"), VoxelWorld ? VoxelWorld->GetSeed() : 0);
		}));

	FAutoConsoleCommand GMadInputBind(
		TEXT("mad.input.bind"), TEXT("mad.input.bind <action> <key> - rebinds a key for this session (mad.settings.save keeps it). Keys are Unreal key names: J, SpaceBar, LeftControl."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogMadFallGameplay, Warning, TEXT("usage: mad.input.bind <action> <key>"));
				return;
			}
			FMadKeyBindings Bindings = MadFall::Input::GetActive();
			const EMadBindResult Result = Bindings.Set(FName(*Args[0]), FKey(FName(*Args[1])));
			MadFall::Input::SetActive(Bindings);
			UE_LOG(LogMadFallGameplay, Display, TEXT("Bind %s %s: %s."), *Args[0], *Args[1], MadFall::Input::ToString(Result));
		}));

	FAutoConsoleCommand GMadInputReset(
		TEXT("mad.input.reset"), TEXT("Puts every key back on its default for this session."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			MadFall::Input::SetActive(FMadKeyBindings());
			UE_LOG(LogMadFallGameplay, Display, TEXT("Keys reset to defaults."));
		}));

	FAutoConsoleCommand GMadInputBindings(
		TEXT("mad.input.bindings"), TEXT("Lists every rebindable action and its key."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			FString Out = TEXT("Key bindings:");
			for (const FMadRebindableAction& Action : FMadKeyBindings::GetActions())
			{
				const FKey Key = MadFall::Input::GetActive().Get(Action.Id);
				Out += FString::Printf(TEXT("\n  %-14s %s%s"), *Action.Id.ToString(), *Key.GetFName().ToString(), Key == Action.Default ? TEXT("") : TEXT("  (changed)"));
			}
			UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Out);
		}));

	FAutoConsoleCommandWithWorld GMadSettingsSave(
		TEXT("mad.settings.save"), TEXT("Writes the current settings console variables to MadFallSettings.json."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld*)
		{
			FString Error;
			const bool bOk = MadFall::Settings::Save(MadFall::Settings::FromConsoleVariables(), MadFall::Settings::GetPath(), Error);
			UE_LOG(LogMadFallGameplay, Display, TEXT("Settings %s %s"), bOk ? TEXT("saved to") : TEXT("not saved:"), bOk ? *MadFall::Settings::GetPath() : *Error);
		}));
}

#undef LOCTEXT_NAMESPACE
