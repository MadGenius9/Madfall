// Copyright MadFall. All Rights Reserved.

#include "MadGameMode.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "MadBiomeRegistry.h"
#include "MadBlockRegistry.h"
#include "MadCompass.h"
#include "Components/CapsuleComponent.h"
#include "MadDebris.h"
#include "MadFallGameplay.h"
#include "MadFarming.h"
#include "MadGameplayDefinitions.h"
#include "MadHarvest.h"
#include "MadKeyBindings.h"
#include "MadProgression.h"
#include "MadTrading.h"
#include "MadInventory.h"
#include "MadItemIcons.h"
#include "MadLocalization.h"
#include "MadPlayerCharacter.h"
#include "MadQuests.h"
#include "MadSession.h"
#include "MadStructuralSubsystem.h"
#include "MadSurvivorComponents.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWeather.h"
#include "MadWorldMap.h"
#include "MadWorldClockSubsystem.h"
#include "MadWorldGenerator.h"
#include "Misc/DefaultValueHelper.h"

AMadGameMode::AMadGameMode()
{
	DefaultPawnClass = AMadPlayerCharacter::StaticClass();
	HUDClass = AMadHUD::StaticClass();
}

UClass* AMadGameMode::GetDefaultPawnClassForController_Implementation(AController* InController)
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld() ? GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
	if (VoxelWorld != nullptr && VoxelWorld->IsReadOnly() && MadFall::Session::IsTitleScreen())
	{
		return nullptr;
	}
	return Super::GetDefaultPawnClassForController_Implementation(InController);
}

// ===========================================================================
// HUD
// ===========================================================================

void AMadHUD::DrawBar(float X, float Y, float Width, float Fraction, const FLinearColor& Colour, const FString& Label)
{
	// A framed bar with a lit top edge and its label inside, shadowed: flat
	// rectangles with a label beside them read as a debug overlay. A vital below
	// a quarter pulses, so a starving survivor notices without reading numbers.
	constexpr float Height = 18.0f;
	const float Clamped = FMath::Clamp(Fraction, 0.0f, 1.0f);
	const float Pulse = Clamped < 0.25f ? 0.65f + 0.35f * FMath::Sin(GetWorld()->GetRealTimeSeconds() * 6.0f) : 1.0f;
	DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.7f), X - 1.0f, Y - 1.0f, Width + 2.0f, Height + 2.0f);
	DrawRect(FLinearColor(Colour.R * 0.18f, Colour.G * 0.18f, Colour.B * 0.18f, 0.85f), X + 1.0f, Y + 1.0f, Width - 2.0f, Height - 2.0f);
	const float FillW = (Width - 2.0f) * Clamped;
	DrawRect(Colour * Pulse, X + 1.0f, Y + 1.0f, FillW, Height - 2.0f);
	DrawRect(FLinearColor(1.0f, 1.0f, 1.0f, 0.18f * Pulse), X + 1.0f, Y + 1.0f, FillW, 4.0f);
	UFont* Small = GEngine->GetSmallFont();
	DrawText(Label, FLinearColor(0.0f, 0.0f, 0.0f, 0.8f), X + 7.0f, Y + 1.0f, Small);
	DrawText(Label, FLinearColor::White, X + 6.0f, Y, Small);
}

void AMadHUD::DrawHUD()
{
	Super::DrawHUD();

	const AMadPlayerCharacter* Player = Cast<AMadPlayerCharacter>(GetOwningPawn());
	if (Player == nullptr || Canvas == nullptr)
	{
		return;
	}

	const float W = Canvas->ClipX;
	const float H = Canvas->ClipY;
	UFont* Font = GEngine->GetMediumFont();
	UFont* Small = GEngine->GetSmallFont();

	if (Player->IsWaitingForWorld())
	{
		DrawRect(FLinearColor(0.02f, 0.02f, 0.03f, 1.0f), 0.0f, 0.0f, W, H);
		DrawText(TEXT("Loading the world..."), FLinearColor::White, W * 0.5f - 90.0f, H * 0.5f, Font);
		return;
	}

	// --- crosshair ------------------------------------------------------------
	DrawRect(FLinearColor::White, W * 0.5f - 1.0f, H * 0.5f - 8.0f, 2.0f, 16.0f);
	DrawRect(FLinearColor::White, W * 0.5f - 8.0f, H * 0.5f - 1.0f, 16.0f, 2.0f);

	// --- vitals ---------------------------------------------------------------
	const FMadSurvivalStats S = Player->GetSurvival()->GetStats();
	float Y = H - 150.0f;
	DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.3f), 14.0f, Y - 10.0f, 236.0f, 118.0f);
	DrawBar(24.0f, Y, 216.0f, S.Health / S.MaxHealth, FLinearColor(0.75f, 0.1f, 0.1f), S.MaxHealth != 100.0f
		? FString::Printf(TEXT("Health %.0f / %.0f"), S.Health, S.MaxHealth) : FString::Printf(TEXT("Health %.0f"), S.Health));
	Y += 22.0f;
	DrawBar(24.0f, Y, 216.0f, S.Stamina / S.MaxStamina, FLinearColor(0.85f, 0.7f, 0.15f), S.MaxStamina != 100.0f
		? FString::Printf(TEXT("Stamina %.0f / %.0f"), S.Stamina, S.MaxStamina) : FString::Printf(TEXT("Stamina %.0f"), S.Stamina));
	Y += 22.0f;
	DrawBar(24.0f, Y, 216.0f, S.Food / 100.0f, FLinearColor(0.6f, 0.36f, 0.12f), FString::Printf(TEXT("Food %.0f"), S.Food));
	Y += 22.0f;
	DrawBar(24.0f, Y, 216.0f, S.Water / 100.0f, FLinearColor(0.15f, 0.42f, 0.85f), FString::Printf(TEXT("Water %.0f"), S.Water));
	Y += 24.0f;

	const FLinearColor TempColour = S.CoreTemperature < 35.5f ? FLinearColor(0.5f, 0.7f, 1.0f)
		: (S.CoreTemperature > 38.5f ? FLinearColor(1.0f, 0.5f, 0.2f) : FLinearColor::White);
	const UMadSurvivalComponent* Survival = Player->GetSurvival();
	FString Worn;
	if (Survival->GetArmor() > 0.0f)
	{
		Worn += FString::Printf(TEXT("   Armor %.0f%%"), Survival->GetArmor() * 100.0f);
	}
	if (Survival->GetColdInsulation() > 0.0f)
	{
		Worn += FString::Printf(TEXT("   Warmth +%.0f"), Survival->GetColdInsulation());
	}
	DrawText(FString::Printf(TEXT("Core %.1f C   Air %.0f C%s%s"), S.CoreTemperature, Survival->GetAmbientTemperature(),
		S.Infection > 0.0f ? *FString::Printf(TEXT("   Infection %.0f%%"), S.Infection) : TEXT(""), *Worn), TempColour, 24.0f, Y, Small);

	// --- clock ----------------------------------------------------------------
	if (const UMadWorldClockSubsystem* Clock = GetWorld()->GetSubsystem<UMadWorldClockSubsystem>())
	{
		const float Time = Clock->GetTimeOfDay();
		const int32 Hours = FMath::FloorToInt32(Time);
		const int32 Minutes = FMath::FloorToInt32((Time - Hours) * 60.0f);
		const FString Horde = Clock->IsHordeNight() ? FString(TEXT("  HORDE NIGHT"))
			: FString::Printf(TEXT("  horde in %d day(s)"), Clock->DaysUntilHorde());
		const UMadWeatherSubsystem* Weather = GetWorld()->GetSubsystem<UMadWeatherSubsystem>();
		const FString Sky = Weather != nullptr && Weather->GetState().Kind != EMadWeather::Clear
			? FString::Printf(TEXT("  %s"), MadFall::Weather::GetName(Weather->GetState().Kind)) : FString();
		DrawText(FString::Printf(TEXT("Day %d  %02d:%02d%s%s"), Clock->GetDay(), Hours, Minutes, *Sky, *Horde),
			Clock->IsHordeNight() ? FLinearColor(1.0f, 0.25f, 0.2f) : FLinearColor::White, 24.0f, 20.0f, Font);
	}
	DrawText(FString::Printf(TEXT("Level %d  (%d/%d xp)  game stage %d%s"), Player->GetLevel(), Player->GetExperience(),
		Player->GetExperienceForNextLevel(), Player->GetGameStage(),
		Player->GetUnspentPerkPoints() > 0 ? *FString::Printf(TEXT("  [%d perk point(s)]"), Player->GetUnspentPerkPoints()) : TEXT("")), FLinearColor(0.8f, 0.8f, 0.8f), 24.0f, 48.0f, Small);

	// --- compass --------------------------------------------------------------
	{
		constexpr float HalfSpan = 60.0f;
		constexpr float StripW = 520.0f;
		constexpr float StripH = 22.0f;
		const float StripX = W * 0.5f - StripW * 0.5f;
		constexpr float StripY = 14.0f;
		const float Yaw = FRotator::ClampAxis(Player->GetControlRotation().Yaw);

		DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.4f), StripX, StripY, StripW, StripH);
		for (int32 Degrees = 0; Degrees < 360; Degrees += 15)
		{
			const TOptional<float> At = MadFall::Compass::ProjectBearing(Yaw, static_cast<float>(Degrees), HalfSpan);
			if (!At.IsSet())
			{
				continue;
			}
			const float X = W * 0.5f + At.GetValue() * StripW * 0.5f;
			if (Degrees % 45 == 0)
			{
				const TCHAR* Name = MadFall::Compass::GetHeadingName(static_cast<float>(Degrees));
				DrawText(Name, Degrees == 0 ? FLinearColor(1.0f, 0.4f, 0.3f) : FLinearColor::White, X - 4.0f * FCString::Strlen(Name), StripY + 4.0f, Small);
			}
			else
			{
				DrawRect(FLinearColor(0.8f, 0.8f, 0.8f, 0.6f), X - 0.5f, StripY + 7.0f, 1.0f, 8.0f);
			}
		}
		DrawRect(FLinearColor(1.0f, 0.9f, 0.4f), W * 0.5f - 1.0f, StripY + StripH, 2.0f, 5.0f);

		TArray<FMadCompassMarker> Markers;
		MadFall::Compass::GatherMarkers(*Player, Markers);
		for (const FMadCompassMarker& Marker : Markers)
		{
			const float Bearing = MadFall::Compass::BearingTo(Player->GetActorLocation(), Marker.Location);
			const TOptional<float> At = MadFall::Compass::ProjectBearing(Yaw, Bearing, HalfSpan);
			if (!At.IsSet())
			{
				continue;
			}
			const float X = W * 0.5f + At.GetValue() * StripW * 0.5f;
			const int32 Metres = FMath::RoundToInt32(FVector::Dist2D(Player->GetActorLocation(), Marker.Location) / 100.0);
			DrawRect(Marker.Colour, X - 3.0f, StripY + StripH - 6.0f, 6.0f, 6.0f);
			DrawText(FString::Printf(TEXT("%s %dm"), *Marker.Label, Metres), Marker.Colour, X - 20.0f, StripY + StripH + 6.0f, Small);
		}
	}

	// --- world map ----------------------------------------------------------------
	if (const UMadWorldMapSubsystem* Map = GetWorld()->GetSubsystem<UMadWorldMapSubsystem>(); Map && Map->IsOpen())
	{
		// Clear of the compass above and the hotbar below.
		const float MapSize = FMath::Min(W, H - 190.0f) * 0.95f;
		const float MapX = W * 0.5f - MapSize * 0.5f;
		const float MapY = 80.0f;
		DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.75f), MapX - 12.0f, MapY - 36.0f, MapSize + 24.0f, MapSize + 60.0f);
		const FMadMapImage& Image = Map->GetImage();
		if (Map->GetTexture() != nullptr && Image.Size > 0)
		{
			DrawTexture(Map->GetTexture(), MapX, MapY, MapSize, MapSize, 0.0f, 0.0f, 1.0f, 1.0f, FLinearColor::White, BLEND_Opaque);

			auto ToScreen = [&](const FVector& WorldCm)
			{
				const FVector2D Pixel = MadFall::WorldMap::ToPixel(Image, FVector2D(WorldCm.X, WorldCm.Y) / MadFall::VoxelSizeUU);
				return FVector2D(MapX, MapY) + Pixel / Image.Size * MapSize;
			};
			auto OnMap = [&](const FVector2D& P) { return P.X >= MapX && P.X <= MapX + MapSize && P.Y >= MapY && P.Y <= MapY + MapSize; };

			TArray<FMadCompassMarker> Markers;
			MadFall::Compass::GatherMarkers(*Player, Markers);
			for (const FMadCompassMarker& Marker : Markers)
			{
				const FVector2D P = ToScreen(Marker.Location);
				const FIntPoint Cell = MadFall::WorldMap::CellOf(Marker.Location.X / MadFall::VoxelSizeUU, Marker.Location.Y / MadFall::VoxelSizeUU);
				if (OnMap(P) && Map->IsExplored(Cell))
				{
					DrawRect(Marker.Colour, P.X - 4.0f, P.Y - 4.0f, 8.0f, 8.0f);
					DrawText(Marker.Label, Marker.Colour, P.X + 7.0f, P.Y - 8.0f, Small);
				}
			}

			// The survivor: an arrow pointing the way they face (north up, so yaw 0 points up).
			const FVector2D Me = ToScreen(Player->GetActorLocation());
			const float Yaw = FMath::DegreesToRadians(Player->GetControlRotation().Yaw);
			const FVector2D Forward(FMath::Sin(Yaw), -FMath::Cos(Yaw));
			const FVector2D Side(-Forward.Y, Forward.X);
			const FVector2D Tip = Me + Forward * 12.0f;
			const FVector2D Left = Me - Forward * 7.0f + Side * 7.0f;
			const FVector2D Right = Me - Forward * 7.0f - Side * 7.0f;
			for (const TPair<FVector2D, FVector2D>& Edge : { TPair<FVector2D, FVector2D>(Tip, Left), TPair<FVector2D, FVector2D>(Left, Right), TPair<FVector2D, FVector2D>(Right, Tip) })
			{
				DrawLine(Edge.Key.X, Edge.Key.Y, Edge.Value.X, Edge.Value.Y, FLinearColor(1.0f, 0.2f, 0.15f), 3.0f);
			}
			const int32 Metres = FMath::RoundToInt32(Image.Size * Image.VoxelsPerPixel * MadFall::VoxelSizeUU / 100.0f);
			DrawText(FString::Printf(TEXT("MAP   %d m across   N up   wheel: zoom   %s or Esc: close"), Metres, *MadFall::Input::GetKeyLabel(TEXT("map"))),
				FLinearColor(0.9f, 0.8f, 0.3f), MapX, MapY - 26.0f, Small);
		}
		else
		{
			DrawText(TEXT("Drawing the map..."), FLinearColor::White, W * 0.5f - 60.0f, H * 0.5f, Small);
		}
	}

	// --- journal ----------------------------------------------------------------
	// The first two active quests, top right, with each objective's progress.
	const UMadWorldMapSubsystem* OpenMap = GetWorld()->GetSubsystem<UMadWorldMapSubsystem>();
	if (!Player->IsInventoryOpen() && !(OpenMap && OpenMap->IsOpen()))
	{
		const FMadGameplayDefinitions& Journal = MadFall::GetGameplayDefinitions();
		TArray<TPair<const FMadQuestDefinition*, const FMadQuestProgress*>> Active;
		Player->GetQuestLog().GetJournal(Journal, Active);
		float QuestY = 70.0f;
		const float QuestX = W - 330.0f;
		for (int32 Index = 0; Index < FMath::Min(2, Active.Num()); ++Index)
		{
			const FMadQuestDefinition& Quest = *Active[Index].Key;
			const FMadQuestProgress& Progress = *Active[Index].Value;
			const float BoxH = 26.0f + 18.0f * Quest.Objectives.Num();
			DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.35f), QuestX - 8.0f, QuestY - 4.0f, 318.0f, BoxH);
			DrawText(MadFall::Localize(Quest.DisplayName), FLinearColor(0.95f, 0.8f, 0.35f), QuestX, QuestY, Small);
			QuestY += 20.0f;
			for (int32 Objective = 0; Objective < Quest.Objectives.Num(); ++Objective)
			{
				const FMadQuestObjective& Goal = Quest.Objectives[Objective];
				const int32 Have = Progress.Counts.IsValidIndex(Objective) ? Progress.Counts[Objective] : 0;
				const bool bDone = Have >= Goal.Count;
				const FString Count = Goal.Type == EMadQuestObjectiveType::ReachDay || Goal.Count == 1
					? FString() : FString::Printf(TEXT("  %d/%d"), Have, Goal.Count);
				DrawText(FString::Printf(TEXT("%s %s%s"), bDone ? TEXT("+") : TEXT("-"),
					*MadFall::Quests::DescribeObjective(Goal, Journal), *Count),
					bDone ? FLinearColor(0.5f, 0.9f, 0.4f) : FLinearColor(0.9f, 0.9f, 0.9f), QuestX + 6.0f, QuestY, Small);
				QuestY += 18.0f;
			}
			QuestY += 12.0f;
		}
		if (Active.Num() > 2)
		{
			DrawText(FString::Printf(TEXT("(+%d more - mad.quests)"), Active.Num() - 2), FLinearColor(0.6f, 0.6f, 0.6f), QuestX, QuestY, Small);
		}
	}

	// --- target ---------------------------------------------------------------
	if (const AMadTrader* Aimed = Player->GetAimedTrader())
	{
		const FMadTraderDefinition* Trader = MadFall::GetGameplayDefinitions().FindTrader(Aimed->GetTraderId());
		DrawText(FString::Printf(TEXT("%s  -  %s to trade"), Trader ? *MadFall::Localize(Trader->DisplayName) : *Aimed->GetTraderId().ToString(),
			*MadFall::Input::GetKeyLabel(TEXT("interact"))), FLinearColor(0.95f, 0.85f, 0.4f), W * 0.5f + 16.0f, H * 0.5f + 12.0f, Small);
	}
	else if (Player->HasTarget())
	{
		if (const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>())
		{
			const FIntVector& V = Player->GetTarget().Voxel;
			const FMadVoxel Voxel = VoxelWorld->GetVoxel(V.X, V.Y, V.Z);
			const FName BlockId = UMadVoxelWorldSubsystem::GetBlockRegistry().GetStringId(Voxel.BlockTypeID);
			FString Label = FMadGameplayDefinitions::GetBlockName(BlockId);
			if (Voxel.Damage > 0)
			{
				Label += FString::Printf(TEXT("  %d%% damaged"), FMath::RoundToInt32(Voxel.Damage / 2.55f));
			}
			DrawText(Label, FLinearColor::White, W * 0.5f + 16.0f, H * 0.5f + 12.0f, Small);

			// Crops show how they are coming along instead of a structural load
			// nobody farming cares about.
			const FMadBlockDefinitionData* TargetDef = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(Voxel.BlockTypeID);
			if (TargetDef != nullptr && TargetDef->Tags.Contains(FName(TEXT("block.crop"))))
			{
				FString GrowthText = TEXT("ripe - harvest it");
				if (!TargetDef->GrowInto.IsNone())
				{
					float Progress = 0.0f;
					const UMadFarmingSubsystem* Farming = GetWorld()->GetSubsystem<UMadFarmingSubsystem>();
					GrowthText = Farming != nullptr && Farming->GetStageProgress(V, Progress)
						? FString::Printf(TEXT("growing %d%%"), FMath::RoundToInt32(Progress * 100.0f))
						: FString(TEXT("not growing"));
				}
				DrawText(GrowthText, FLinearColor(0.5f, 0.9f, 0.4f), W * 0.5f + 16.0f, H * 0.5f + 28.0f, Small);
			}
			// Structural load: how close this block is to its span or weight
			// limit, so a builder can see which support is about to go.
			else if (UMadStructuralSubsystem* Structural = GetWorld()->GetSubsystem<UMadStructuralSubsystem>())
			{
				FMadStructuralNodeReport Report;
				const EMadStressQuery Query = Structural->QueryStress(V, Report);
				FString StressText;
				FLinearColor StressColour = FLinearColor(0.7f, 0.7f, 0.7f);
				if (Query == EMadStressQuery::Ready)
				{
					const float Stress = FMath::Max(0.0f, Report.Stress);
					StressText = Report.Failure != EMadStructuralFailure::None ? FString(TEXT("FAILING"))
						: FString::Printf(TEXT("load %d%%"), FMath::RoundToInt32(Stress * 100.0f));
					StressColour = FLinearColor::LerpUsingHSV(FLinearColor(0.3f, 0.9f, 0.3f), FLinearColor(1.0f, 0.2f, 0.1f), FMath::Clamp(Stress, 0.0f, 1.0f));
				}
				else if (Query == EMadStressQuery::Pending)
				{
					StressText = TEXT("load ...");
				}
				else if (Query == EMadStressQuery::TooLarge)
				{
					StressText = TEXT("load: structure too large to inspect");
				}
				if (!StressText.IsEmpty())
				{
					DrawText(StressText, StressColour, W * 0.5f + 16.0f, H * 0.5f + 28.0f, Small);
				}
			}
		}
	}

	// --- messages -------------------------------------------------------------
	// With the inventory screen up, the screen draws them under its panels.
	if (!Player->IsInventoryOpen())
	{
		float MessageY = H * 0.22f;
		for (const AMadPlayerCharacter::FMessage& Message : Player->GetMessages())
		{
			DrawText(Message.Text, FLinearColor(1.0f, 0.95f, 0.8f), W * 0.5f - 200.0f, MessageY, Font);
			MessageY += 26.0f;
		}
	}

	// --- hotbar ---------------------------------------------------------------
	const UMadInventoryComponent* Inventory = Player->GetInventory();
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	constexpr float SlotSize = 64.0f;
	const float HotbarX = W * 0.5f - (UMadInventoryComponent::HotbarSlots * (SlotSize + 4.0f)) * 0.5f;
	const float HotbarY = H - SlotSize - 20.0f;

	for (int32 Slot = 0; Slot < UMadInventoryComponent::HotbarSlots; ++Slot)
	{
		const float X = HotbarX + Slot * (SlotSize + 4.0f);
		const bool bSelected = Slot == Inventory->GetSelectedSlot();
		DrawRect(bSelected ? FLinearColor(0.9f, 0.8f, 0.3f, 0.8f) : FLinearColor(0.0f, 0.0f, 0.0f, 0.5f), X, HotbarY, SlotSize, SlotSize);
		DrawRect(FLinearColor(0.08f, 0.08f, 0.08f, 0.85f), X + 3.0f, HotbarY + 3.0f, SlotSize - 6.0f, SlotSize - 6.0f);
		DrawText(FString::FromInt(Slot + 1), FLinearColor(0.6f, 0.6f, 0.6f), X + 5.0f, HotbarY + 3.0f, Small);

		const FMadItemStack& Stack = Inventory->GetInventory().GetSlot(Slot);
		if (Stack.IsEmpty())
		{
			continue;
		}

		DrawItemIcon(Stack.Item, X + 8.0f, HotbarY + 8.0f, SlotSize - 16.0f);
		if (bSelected)
		{
			// The selected item's name over the hotbar: the icons say what, this says which.
			const FString Name = Definitions.GetItemName(Stack.Item);
			float NameW = 0.0f;
			float NameH = 0.0f;
			GetTextSize(Name, NameW, NameH, Small);
			DrawText(Name, FLinearColor(1.0f, 1.0f, 1.0f, 0.9f), W * 0.5f - NameW * 0.5f, HotbarY - 22.0f, Small);
		}
		if (Stack.Count > 1)
		{
			DrawCount(Stack.Count, X + SlotSize - 5.0f, HotbarY + SlotSize - 20.0f);
		}
		if (Stack.Durability >= 0)
		{
			if (const FMadItemDefinition* Item = Definitions.FindItem(Stack.Item); Item && Item->Tool.Durability > 0)
			{
				const float Fraction = static_cast<float>(Stack.Durability) / Item->Tool.Durability;
				DrawRect(FLinearColor::LerpUsingHSV(FLinearColor::Red, FLinearColor::Green, Fraction),
					X + 5.0f, HotbarY + SlotSize - 8.0f, (SlotSize - 10.0f) * Fraction, 3.0f);
			}
		}
	}

	// --- crafting queue -------------------------------------------------------
	float QueueY = 20.0f;
	for (const AMadPlayerCharacter::FCraftJob& Job : Player->GetCraftQueue())
	{
		const FMadRecipeDefinition* Recipe = Definitions.FindRecipe(Job.Recipe);
		const float Progress = Job.Total > 0.0f ? 1.0f - Job.Remaining / Job.Total : 1.0f;
		DrawBar(W - 330.0f, QueueY, 120.0f, Progress, FLinearColor(0.9f, 0.8f, 0.3f),
			Recipe ? FString::Printf(TEXT("%d x %s"), Recipe->Output.Count * Job.Times, *Definitions.GetItemName(Recipe->Output.Item)) : Job.Recipe.ToString());
		QueueY += 22.0f;
	}

	if (Player->IsInventoryOpen())
	{
		DrawInventoryScreen(*Player);
	}
}

// ===========================================================================
// Inventory screen
// ===========================================================================

namespace
{
	const TCHAR* BackpackBoxPrefix = TEXT("inv.b.");
	const TCHAR* ContainerBoxPrefix = TEXT("inv.c.");
	const TCHAR* WornBoxPrefix = TEXT("inv.w.");
	const FName TakeAllBox(TEXT("inv.takeall"));
	const FName SortBackpackBox(TEXT("inv.sort.b"));
	const FName SortContainerBox(TEXT("inv.sort.c"));
	const TCHAR* PerkBoxPrefix = TEXT("perk.");
	const TCHAR* TradeBoxPrefix = TEXT("trade.b.");
}

bool AMadHUD::DrawItemIcon(FName ItemId, float X, float Y, float Size, float Opacity)
{
	UTexture2D* Icon = MadFall::IconCache::Get(ItemId);
	if (Icon == nullptr || Canvas == nullptr)
	{
		return false;
	}
	FCanvasTileItem Tile(FVector2D(X, Y), Icon->GetResource(), FVector2D(Size, Size), FLinearColor(1.0f, 1.0f, 1.0f, Opacity));
	Tile.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Tile);
	return true;
}

void AMadHUD::DrawCount(int32 Count, float RightX, float Y)
{
	// Right-aligned with a shadow, so a count reads over any icon.
	UFont* Small = GEngine->GetSmallFont();
	const FString Text = FString::FromInt(Count);
	float TextW = 0.0f;
	float TextH = 0.0f;
	GetTextSize(Text, TextW, TextH, Small);
	DrawText(Text, FLinearColor(0.0f, 0.0f, 0.0f, 0.85f), RightX - TextW + 1.0f, Y + 1.0f, Small);
	DrawText(Text, FLinearColor::White, RightX - TextW, Y, Small);
}

void AMadHUD::DrawSlot(const FMadItemStack& Stack, float X, float Y, float Size, bool bHighlighted, FName HitBox)
{
	UFont* Small = GEngine->GetSmallFont();
	DrawRect(bHighlighted ? FLinearColor(0.9f, 0.8f, 0.3f, 0.9f) : FLinearColor(0.0f, 0.0f, 0.0f, 0.6f), X, Y, Size, Size);
	DrawRect(FLinearColor(0.1f, 0.1f, 0.1f, 0.9f), X + 3.0f, Y + 3.0f, Size - 6.0f, Size - 6.0f);
	AddHitBox(FVector2D(X, Y), FVector2D(Size, Size), HitBox, /*bConsumesInput*/ true);

	if (Stack.IsEmpty())
	{
		return;
	}

	// Hover is tested here rather than through hit-box cursor events, which
	// arrive a frame late and not at all while the cursor is still.
	if (Mouse.X >= X && Mouse.X < X + Size && Mouse.Y >= Y && Mouse.Y < Y + Size)
	{
		HoveredStack = Stack;
		HoveredBox = HitBox;
	}

	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const bool bKnown = Definitions.FindItem(Stack.Item) != nullptr;
	if (!bKnown || !DrawItemIcon(Stack.Item, X + 6.0f, Y + 6.0f, Size - 12.0f))
	{
		// An item a removed mod left behind, or no renderer: the name, red when unknown.
		DrawText(Definitions.GetItemName(Stack.Item).Left(8), bKnown ? FLinearColor::White : FLinearColor(1.0f, 0.4f, 0.4f), X + 5.0f, Y + 6.0f, Small);
	}
	if (Stack.Count > 1)
	{
		DrawCount(Stack.Count, X + Size - 5.0f, Y + Size - 20.0f);
	}
	if (Stack.Durability >= 0)
	{
		if (const FMadItemDefinition* Item = Definitions.FindItem(Stack.Item); Item && Item->Tool.Durability > 0)
		{
			const float Fraction = FMath::Clamp(static_cast<float>(Stack.Durability) / Item->Tool.Durability, 0.0f, 1.0f);
			DrawRect(FLinearColor::LerpUsingHSV(FLinearColor::Red, FLinearColor::Green, Fraction), X + 5.0f, Y + Size - 7.0f, (Size - 10.0f) * Fraction, 3.0f);
		}
	}
	if (Stack.Mods.Num() > 0)
	{
		DrawText(FString::Printf(TEXT("+%d"), Stack.Mods.Num()), FLinearColor(0.5f, 0.8f, 1.0f), X + 5.0f, Y + Size - 22.0f, Small);
	}
}

void AMadHUD::DrawInventoryScreen(const AMadPlayerCharacter& Player)
{
	const float W = Canvas->ClipX;
	const float H = Canvas->ClipY;
	UFont* Small = GEngine->GetSmallFont();

	constexpr float SlotSize = 56.0f;
	constexpr float Gap = 4.0f;
	constexpr int32 Columns = 9;
	const float GridW = Columns * (SlotSize + Gap) - Gap;
	const float PanelW = GridW + 32.0f;

	// The skills column sits beside the inventory when the screen is wide
	// enough for both, and over its right edge when it is not.
	constexpr float SkillsGap = 12.0f;
	const bool bSideBySide = W >= PanelW + SkillsGap + SkillsPanelWidth + 32.0f;
	const float PanelX = bSideBySide ? W * 0.5f - (PanelW + SkillsGap + SkillsPanelWidth) * 0.5f : W * 0.5f - PanelW * 0.5f;
	const float SkillsX = bSideBySide ? PanelX + PanelW + SkillsGap : W - SkillsPanelWidth - 16.0f;

	HoveredStack.Reset();
	Mouse = FVector2D(-1.0, -1.0);
	if (const APlayerController* Controller = GetOwningPlayerController())
	{
		float MouseX = 0.0f;
		float MouseY = 0.0f;
		if (Controller->GetMousePosition(MouseX, MouseY))
		{
			Mouse = FVector2D(MouseX, MouseY);
		}
	}

	FMadTraderState* TradeState = nullptr;
	const FMadTraderDefinition* TradeDef = nullptr;
	const bool bTrading = Player.GetOpenTrade(TradeState, TradeDef);
	const FMadInventory* Crate = bTrading ? &TradeState->Stock : Player.GetOpenContainerInventory();
	const int32 CrateRows = Crate ? FMath::DivideAndRoundUp(Crate->NumSlots(), Columns) : 0;
	const int32 BackpackRows = FMath::DivideAndRoundUp(UMadInventoryComponent::NumSlots - UMadInventoryComponent::HotbarSlots, Columns);
	const float CrateH = Crate ? 36.0f + CrateRows * (SlotSize + Gap) + 12.0f + (bTrading ? 20.0f : 0.0f) : 0.0f;
	const float WornH = 24.0f + SlotSize + 12.0f;
	// The last 24 are the footer line (the install hint, or what is picked up).
	const float PanelH = 40.0f + CrateH + WornH + (BackpackRows + 1) * (SlotSize + Gap) + 24.0f + 40.0f + 24.0f;
	const float PanelY = FMath::Max(16.0f, H * 0.5f - PanelH * 0.5f - 40.0f);

	DrawRect(FLinearColor(0.02f, 0.02f, 0.03f, 0.85f), PanelX, PanelY, PanelW, PanelH);
	DrawText(TEXT("INVENTORY   click: pick up / put down   right-click: half   shift-click: quick move or wear"),
		FLinearColor(0.9f, 0.8f, 0.3f), PanelX + 16.0f, PanelY + 10.0f, Small);

	const AMadPlayerCharacter::FInventoryCursor& Cursor = Player.GetInventoryCursor();
	float Y = PanelY + 36.0f;

	if (bTrading)
	{
		const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
		const int32 Coins = Player.GetInventory()->GetInventory().CountItem(TradeDef->Currency);
		DrawText(FString::Printf(TEXT("%s   -   you have %d %s"), *MadFall::Localize(TradeDef->DisplayName), Coins, *Definitions.GetItemName(TradeDef->Currency)),
			FLinearColor(0.95f, 0.85f, 0.4f), PanelX + 16.0f, Y, Small);
		Y += 20.0f;
		DrawText(TEXT("click: buy one   shift-click: buy the stack   shift-click your own items: sell"), FLinearColor(0.7f, 0.65f, 0.35f), PanelX + 16.0f, Y, Small);
		Y += 24.0f;
		for (int32 Slot = 0; Slot < Crate->NumSlots(); ++Slot)
		{
			const float X = PanelX + 16.0f + (Slot % Columns) * (SlotSize + Gap);
			const float SlotY = Y + (Slot / Columns) * (SlotSize + Gap);
			const FMadItemStack& Shelf = Crate->GetSlot(Slot);
			DrawSlot(Shelf, X, SlotY, SlotSize, false, FName(*FString::Printf(TEXT("%s%d"), TradeBoxPrefix, Slot)));
			if (const int32 Price = MadFall::Trade::GetUnitPrice(Shelf, *TradeDef, Definitions); Price > 0)
			{
				DrawText(FString::Printf(TEXT("$%d"), Price), Coins >= Price ? FLinearColor(0.95f, 0.85f, 0.4f) : FLinearColor(1.0f, 0.4f, 0.35f),
					X + 5.0f, SlotY + SlotSize - 22.0f, Small);
			}
		}
		Y += CrateRows * (SlotSize + Gap) + 12.0f;
	}
	else if (Crate != nullptr)
	{
		DrawText(TEXT("Container"), FLinearColor::White, PanelX + 16.0f, Y, Small);
		const float ButtonX = PanelX + PanelW - 16.0f - 90.0f;
		DrawRect(FLinearColor(0.3f, 0.3f, 0.15f, 0.9f), ButtonX, Y - 4.0f, 90.0f, 22.0f);
		DrawText(TEXT("Take all"), FLinearColor::White, ButtonX + 16.0f, Y - 1.0f, Small);
		AddHitBox(FVector2D(ButtonX, Y - 4.0f), FVector2D(90.0f, 22.0f), TakeAllBox, true);
		const float SortX = ButtonX - 8.0f - 60.0f;
		DrawRect(FLinearColor(0.2f, 0.22f, 0.26f, 0.9f), SortX, Y - 4.0f, 60.0f, 22.0f);
		DrawText(TEXT("Sort"), FLinearColor::White, SortX + 16.0f, Y - 1.0f, Small);
		AddHitBox(FVector2D(SortX, Y - 4.0f), FVector2D(60.0f, 22.0f), SortContainerBox, true);
		Y += 24.0f;

		for (int32 Slot = 0; Slot < Crate->NumSlots(); ++Slot)
		{
			const float X = PanelX + 16.0f + (Slot % Columns) * (SlotSize + Gap);
			const float SlotY = Y + (Slot / Columns) * (SlotSize + Gap);
			const bool bHeld = Cursor.bActive && Cursor.Side == EMadInventorySide::Container && Cursor.Slot == Slot;
			DrawSlot(Crate->GetSlot(Slot), X, SlotY, SlotSize, bHeld, FName(*FString::Printf(TEXT("%s%d"), ContainerBoxPrefix, Slot)));
		}
		Y += CrateRows * (SlotSize + Gap) + 12.0f;
	}

	// Worn: four labelled slots. Shift-click clothing in the backpack to put it on.
	const FMadInventory& Worn = Player.GetInventory()->GetWorn();
	const UMadSurvivalComponent* Survival = Player.GetSurvival();
	DrawText(FString::Printf(TEXT("Worn   armor %.0f%%   warmth +%.0f C   cooling +%.0f C"), Survival->GetArmor() * 100.0f,
		Survival->GetColdInsulation(), Survival->GetHeatInsulation()), FLinearColor::White, PanelX + 16.0f, Y, Small);
	Y += 24.0f;
	for (int32 Slot = 0; Slot < Worn.NumSlots(); ++Slot)
	{
		const float X = PanelX + 16.0f + Slot * (SlotSize + Gap) * 2.0f;
		const bool bHeld = Cursor.bActive && Cursor.Side == EMadInventorySide::Worn && Cursor.Slot == Slot;
		DrawSlot(Worn.GetSlot(Slot), X, Y, SlotSize, bHeld, FName(*FString::Printf(TEXT("%s%d"), WornBoxPrefix, Slot)));
		if (Worn.GetSlot(Slot).IsEmpty())
		{
			DrawText(MadFall::Wear::GetSlotName(Slot).ToString(), FLinearColor(0.45f, 0.45f, 0.45f), X + 8.0f, Y + SlotSize * 0.5f - 6.0f, Small);
		}
	}
	Y += SlotSize + 12.0f;

	const FMadInventory& Backpack = Player.GetInventory()->GetInventory();
	DrawText(TEXT("Backpack"), FLinearColor::White, PanelX + 16.0f, Y, Small);
	{
		const float SortX = PanelX + PanelW - 16.0f - 60.0f;
		DrawRect(FLinearColor(0.2f, 0.22f, 0.26f, 0.9f), SortX, Y - 4.0f, 60.0f, 22.0f);
		DrawText(TEXT("Sort"), FLinearColor::White, SortX + 16.0f, Y - 1.0f, Small);
		AddHitBox(FVector2D(SortX, Y - 4.0f), FVector2D(60.0f, 22.0f), SortBackpackBox, true);
	}
	Y += 24.0f;

	auto DrawBackpackSlot = [&](int32 Slot, float X, float SlotY)
	{
		const bool bHeld = Cursor.bActive && Cursor.Side == EMadInventorySide::Backpack && Cursor.Slot == Slot;
		DrawSlot(Backpack.GetSlot(Slot), X, SlotY, SlotSize, bHeld, FName(*FString::Printf(TEXT("%s%d"), BackpackBoxPrefix, Slot)));
	};

	for (int32 Slot = UMadInventoryComponent::HotbarSlots; Slot < Backpack.NumSlots(); ++Slot)
	{
		const int32 Index = Slot - UMadInventoryComponent::HotbarSlots;
		DrawBackpackSlot(Slot, PanelX + 16.0f + (Index % Columns) * (SlotSize + Gap), Y + (Index / Columns) * (SlotSize + Gap));
	}
	Y += BackpackRows * (SlotSize + Gap) + 12.0f;

	DrawText(TEXT("Hotbar"), FLinearColor(0.7f, 0.7f, 0.7f), PanelX + 16.0f, Y, Small);
	Y += 20.0f;
	for (int32 Slot = 0; Slot < UMadInventoryComponent::HotbarSlots; ++Slot)
	{
		DrawBackpackSlot(Slot, PanelX + 16.0f + Slot * (SlotSize + Gap), Y);
	}
	Y += SlotSize + 12.0f;

	// What is picked up, so a half-stack pickup is visibly different; otherwise
	// the one instruction the header has no room for.
	if (!Cursor.bActive)
	{
		DrawText(FString::Printf(TEXT("mod onto a tool: install   ctrl-click a tool: remove its mod   wheel: scroll   %s/%s: close"),
			*MadFall::Input::GetKeyLabel(TEXT("inventory")), *MadFall::Input::GetKeyLabel(TEXT("interact"))),
			FLinearColor(0.7f, 0.65f, 0.35f), PanelX + 16.0f, Y, Small);
	}
	else
	{
		const FMadInventory* Source = Cursor.Side == EMadInventorySide::Backpack ? &Backpack
			: (Cursor.Side == EMadInventorySide::Worn ? &Worn : Crate);
		if (Source != nullptr && !Source->GetSlot(Cursor.Slot).IsEmpty())
		{
			const FMadItemStack& Held = Source->GetSlot(Cursor.Slot);
			DrawText(FString::Printf(TEXT("Holding %d of %d x %s - click a slot to put it down   wheel: more / fewer"),
				Player.GetHeldCount(), Held.Count, *MadFall::GetGameplayDefinitions().GetItemName(Held.Item)),
				FLinearColor(0.9f, 0.8f, 0.3f), PanelX + 16.0f, Y, Small);
		}
	}

	// The right-hand column may run taller than the inventory, down to just above the hotbar.
	{
		const float ColumnH = FMath::Max(PanelH, H - PanelY - 100.0f);
		constexpr float TabH = 26.0f;
		const EMadInventoryTab Tab = Player.GetInventoryTab();
		for (int32 Index = 0; Index < 2; ++Index)
		{
			const bool bActive = static_cast<int32>(Tab) == Index;
			const float TabX = SkillsX + Index * (SkillsPanelWidth * 0.5f);
			DrawRect(bActive ? FLinearColor(0.9f, 0.8f, 0.3f, 0.9f) : FLinearColor(0.1f, 0.1f, 0.12f, 0.9f), TabX, PanelY, SkillsPanelWidth * 0.5f - 2.0f, TabH);
			DrawText(Index == 0 ? FString::Printf(TEXT("CRAFTING (%s)"), *MadFall::Input::GetKeyLabel(TEXT("craft"))) : FString(TEXT("SKILLS")),
				bActive ? FLinearColor(0.05f, 0.05f, 0.05f) : FLinearColor(0.8f, 0.8f, 0.8f), TabX + 12.0f, PanelY + 6.0f, Small);
			AddHitBox(FVector2D(TabX, PanelY), FVector2D(SkillsPanelWidth * 0.5f - 2.0f, TabH), FName(Index == 0 ? TEXT("tab.crafting") : TEXT("tab.skills")), true);
		}
		if (Tab == EMadInventoryTab::Crafting)
		{
			DrawCraftingPanel(Player, SkillsX, PanelY + TabH + 2.0f, ColumnH - TabH - 2.0f);
		}
		else
		{
			DrawSkillsPanel(Player, SkillsX, PanelY + TabH + 2.0f, ColumnH - TabH - 2.0f);
		}
	}

	// The newest messages, under the panels and clear of the hotbar.
	{
		UFont* Font = GEngine->GetMediumFont();
		const TArray<AMadPlayerCharacter::FMessage>& Messages = Player.GetMessages();
		// Right of the vitals bars and their labels, which fill the bottom-left corner.
		const float MessageX = FMath::Max(PanelX + 16.0f, 330.0f);
		float MessageY = PanelY + PanelH + 8.0f;
		for (int32 Index = FMath::Max(0, Messages.Num() - 2); Index < Messages.Num(); ++Index)
		{
			DrawText(Messages[Index].Text, FLinearColor(1.0f, 0.95f, 0.8f), MessageX, MessageY, Font);
			MessageY += 24.0f;
		}
	}

	if (HoveredStack.IsSet())
	{
		DrawTooltip(HoveredStack.GetValue());
	}
	DrawHeldStack(Player);
}

void AMadHUD::DrawHeldStack(const AMadPlayerCharacter& Player)
{
	// The stack itself stays in its slot until put down (so nothing is lost if
	// the screen closes), which left nothing under the mouse to show what a click
	// would drop. A ghost of it follows the cursor instead.
	const int32 Count = Player.GetHeldCount();
	if (Count <= 0 || Mouse.X < 0.0)
	{
		return;
	}
	const AMadPlayerCharacter::FInventoryCursor& Cursor = Player.GetInventoryCursor();
	const FMadInventory* Source = Cursor.Side == EMadInventorySide::Backpack ? &Player.GetInventory()->GetInventory()
		: Cursor.Side == EMadInventorySide::Worn ? &Player.GetInventory()->GetWorn()
		: Player.GetOpenContainerInventory();
	if (Source == nullptr || Cursor.Slot < 0 || Cursor.Slot >= Source->NumSlots())
	{
		return;
	}
	constexpr float Size = 44.0f;
	const float X = static_cast<float>(Mouse.X) + 10.0f;
	const float Y = static_cast<float>(Mouse.Y) + 10.0f;
	DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.35f), X - 2.0f, Y - 2.0f, Size + 4.0f, Size + 4.0f);
	if (!DrawItemIcon(Source->GetSlot(Cursor.Slot).Item, X, Y, Size, 0.9f))
	{
		DrawText(MadFall::GetGameplayDefinitions().GetItemName(Source->GetSlot(Cursor.Slot).Item).Left(6), FLinearColor::White, X + 2.0f, Y + 14.0f,
			GEngine->GetSmallFont());
	}
	if (Count > 1)
	{
		DrawCount(Count, X + Size - 2.0f, Y + Size - 16.0f);
	}
}

void AMadHUD::DrawSkillsPanel(const AMadPlayerCharacter& Player, float X, float Y, float Height)
{
	UFont* Small = GEngine->GetSmallFont();
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();

	DrawRect(FLinearColor(0.02f, 0.02f, 0.03f, 0.9f), X, Y, SkillsPanelWidth, Height);
	const int32 Points = Player.GetUnspentPerkPoints();
	DrawText(FString::Printf(TEXT("SKILLS   level %d   %d point(s) to spend"), Player.GetLevel(), Points),
		Points > 0 ? FLinearColor(0.95f, 0.8f, 0.3f) : FLinearColor(0.75f, 0.75f, 0.75f), X + 14.0f, Y + 10.0f, Small);

	// A point comes with every level; say how far the next one is, since that
	// is the question a player with none to spend is asking.
	DrawText(FString::Printf(TEXT("next level at %d / %d xp"), Player.GetExperience(), Player.GetExperienceForNextLevel()),
		FLinearColor(0.55f, 0.55f, 0.55f), X + 14.0f, Y + 28.0f, Small);

	constexpr int32 WrapChars = 58;
	float RowY = Y + 54.0f;
	const float Bottom = Y + Height - 24.0f;
	const TArray<FMadPerkDefinition>& Perks = Definitions.GetPerks();
	// Tree order: perks grouped by attribute, each below what it needs.
	const TArray<int32> Order = MadFall::Perks::GetTreeOrder(Definitions);
	const int32 First = FMath::Clamp(Player.GetColumnScroll(), 0, FMath::Max(0, Perks.Num() - 1));
	int32 Shown = 0;
	FName PreviousGroup = NAME_None;
	for (int32 OrderIndex = First; OrderIndex < Order.Num(); ++OrderIndex)
	{
		const FMadPerkDefinition& Perk = Perks[Order[OrderIndex]];
		const FName Group = Perk.Tags.Num() > 0 ? Perk.Tags[0] : NAME_None;
		const int32 Depth = FMath::Min(MadFall::Perks::GetTreeDepth(Definitions, Perk.Id), 3);
		const float Indent = 14.0f * Depth;
		const int32 Owned = Player.GetPerkRanks().FindRef(Perk.Id);
		TArray<FString> Lines;
		MadFall::Localize(Perk.Description).ParseIntoArray(Lines, TEXT(" "));
		TArray<FString> Wrapped;
		for (const FString& Word : Lines)
		{
			if (Wrapped.Num() == 0 || Wrapped.Last().Len() + 1 + Word.Len() > WrapChars)
			{
				Wrapped.Add(Word);
			}
			else
			{
				Wrapped.Last() += TEXT(" ") + Word;
			}
		}
		Wrapped.SetNum(FMath::Min(Wrapped.Num(), 2));

		const bool bNewGroup = Shown == 0 || Group != PreviousGroup;
		const float HeaderH = bNewGroup ? 18.0f : 0.0f;
		const float RowH = 20.0f + 16.0f * Wrapped.Num() + 16.0f + 8.0f;
		if (RowY + HeaderH + RowH > Bottom)
		{
			break;
		}
		++Shown;

		// Attribute heading, from the tag: "perk.strength" -> "STRENGTH".
		if (bNewGroup)
		{
			FString Heading = Group.IsNone() ? FString(TEXT("OTHER")) : Group.ToString();
			Heading.Split(TEXT("."), nullptr, &Heading, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			DrawText(Heading.ToUpper(), FLinearColor(0.95f, 0.8f, 0.3f, 0.8f), X + 14.0f, RowY, Small);
			RowY += HeaderH;
			PreviousGroup = Group;
		}

		DrawRect(FLinearColor(1.0f, 1.0f, 1.0f, 0.04f), X + 6.0f + Indent, RowY - 4.0f, SkillsPanelWidth - 12.0f - Indent, RowH - 4.0f);
		if (Depth > 0)
		{
			// A branch mark: this perk hangs off something above it.
			DrawRect(FLinearColor(0.95f, 0.8f, 0.3f, 0.5f), X + Indent, RowY + 2.0f, 2.0f, 12.0f);
		}
		DrawText(FString::Printf(TEXT("%s   %d / %d"), *Definitions.GetPerkName(Perk.Id), Owned, Perk.Ranks.Num()),
			Owned > 0 ? FLinearColor(0.6f, 0.9f, 0.5f) : FLinearColor::White, X + 14.0f + Indent, RowY, Small);

		// Rank pips.
		for (int32 Rank = 0; Rank < Perk.Ranks.Num(); ++Rank)
		{
			DrawRect(Rank < Owned ? FLinearColor(0.6f, 0.9f, 0.5f) : FLinearColor(0.3f, 0.3f, 0.3f),
				X + 200.0f + Indent + Rank * 12.0f, RowY + 4.0f, 8.0f, 8.0f);
		}

		const EMadPerkResult Can = MadFall::Perks::CanBuy(Player.GetPerkRanks(), Perk.Id, Player.GetLevel(), Definitions);
		constexpr float ButtonW = 64.0f;
		const float ButtonX = X + SkillsPanelWidth - 14.0f - ButtonW;
		if (Can == EMadPerkResult::Ok)
		{
			DrawRect(FLinearColor(0.25f, 0.5f, 0.2f, 0.95f), ButtonX, RowY - 2.0f, ButtonW, 20.0f);
			DrawText(TEXT("Take"), FLinearColor::White, ButtonX + 18.0f, RowY, Small);
			AddHitBox(FVector2D(ButtonX, RowY - 2.0f), FVector2D(ButtonW, 20.0f), FName(*FString::Printf(TEXT("perk.%s"), *Perk.Id.ToString())), true);
		}
		else
		{
			FName MissingPerk;
			int32 MissingRank = 0;
			const bool bMissing = Can == EMadPerkResult::MissingPrerequisite
				&& MadFall::Perks::FindMissingPrerequisite(Player.GetPerkRanks(), Perk.Ranks[Owned], MissingPerk, MissingRank);
			// The prerequisite is long ("needs Athlete 2"), so it is right-aligned
			// to end where the button would, instead of starting at the button.
			const FString Why = Can == EMadPerkResult::MaxRank ? FString(TEXT("maxed"))
				: Can == EMadPerkResult::LevelTooLow ? FString::Printf(TEXT("level %d"), Perk.Ranks[Owned].RequiredLevel)
				: bMissing ? FString::Printf(TEXT("needs %s %d"), *Definitions.GetPerkName(MissingPerk), MissingRank)
				: Can == EMadPerkResult::NoPoints ? FString(TEXT("no points"))
				: FString(MadFall::Perks::ToString(Can));
			float WhyW = 0.0f;
			float WhyH = 0.0f;
			GetTextSize(Why, WhyW, WhyH, Small);
			DrawText(Why, bMissing ? FLinearColor(0.85f, 0.6f, 0.35f) : FLinearColor(0.5f, 0.5f, 0.5f),
				FMath::Min(ButtonX + 4.0f, ButtonX + ButtonW - WhyW), RowY, Small);
		}

		float LineY = RowY + 20.0f;
		for (const FString& Line : Wrapped)
		{
			DrawText(Line, FLinearColor(0.7f, 0.7f, 0.7f), X + 14.0f + Indent, LineY, Small);
			LineY += 16.0f;
		}
		const FString Effects = Owned < Perk.Ranks.Num()
			? FString::Printf(TEXT("next: %s"), *MadFall::Perks::DescribeRank(Perk.Ranks[Owned]))
			: FString::Printf(TEXT("now: %s"), *MadFall::Perks::DescribeRank(Perk.Ranks.Last()));
		DrawText(Effects, FLinearColor(0.5f, 0.75f, 0.95f), X + 14.0f + Indent, LineY, Small);

		RowY += RowH;
	}

	const int32 Below = Perks.Num() - First - Shown;
	if (First > 0 || Below > 0)
	{
		DrawText(FString::Printf(TEXT("%d above, %d below - mouse wheel to scroll"), First, Below),
			FLinearColor(0.55f, 0.55f, 0.55f), X + 14.0f, Y + Height - 20.0f, Small);
	}
}

void AMadHUD::DrawCraftingPanel(const AMadPlayerCharacter& Player, float X, float Y, float Height)
{
	UFont* Small = GEngine->GetSmallFont();
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	DrawRect(FLinearColor(0.02f, 0.02f, 0.03f, 0.9f), X, Y, SkillsPanelWidth, Height);

	// Category tabs.
	const int32 Categories = static_cast<int32>(EMadRecipeCategory::Num);
	const float CatW = (SkillsPanelWidth - 20.0f) / Categories;
	for (int32 Index = 0; Index < Categories; ++Index)
	{
		const EMadRecipeCategory Category = static_cast<EMadRecipeCategory>(Index);
		const bool bActive = Player.GetCraftCategory() == Category;
		const float CatX = X + 10.0f + Index * CatW;
		DrawRect(bActive ? FLinearColor(0.35f, 0.3f, 0.12f, 0.95f) : FLinearColor(0.12f, 0.12f, 0.14f, 0.95f), CatX, Y + 8.0f, CatW - 3.0f, 20.0f);
		DrawText(MadFall::Crafting::GetCategoryName(Category), bActive ? FLinearColor(0.95f, 0.85f, 0.4f) : FLinearColor(0.75f, 0.75f, 0.75f), CatX + 6.0f, Y + 10.0f, Small);
		AddHitBox(FVector2D(CatX, Y + 8.0f), FVector2D(CatW - 3.0f, 20.0f), FName(*FString::Printf(TEXT("craft.cat.%d"), Index)), true);
	}

	// Craftable-only toggle.
	const float ToggleY = Y + 34.0f;
	DrawRect(FLinearColor(0.6f, 0.6f, 0.6f), X + 12.0f, ToggleY + 2.0f, 12.0f, 12.0f);
	DrawRect(Player.IsCraftableOnly() ? FLinearColor(0.5f, 0.85f, 0.4f) : FLinearColor(0.05f, 0.05f, 0.05f), X + 14.0f, ToggleY + 4.0f, 8.0f, 8.0f);
	DrawText(TEXT("only what I can craft now"), FLinearColor(0.8f, 0.8f, 0.8f), X + 30.0f, ToggleY, Small);
	AddHitBox(FVector2D(X + 10.0f, ToggleY), FVector2D(200.0f, 16.0f), FName(TEXT("craft.only")), true);

	TArray<FMadRecipeRow> Rows;
	Player.GetRecipeRows(Rows);
	const FName Selected = Player.GetSelectedRecipe();

	// The detail pane has a fixed height at the bottom; the list takes the rest.
	constexpr float DetailH = 190.0f;
	constexpr float RowH = 20.0f;
	const float ListY = ToggleY + 24.0f;
	const float ListBottom = Y + Height - DetailH - 8.0f;
	const int32 Visible = FMath::Max(1, FMath::FloorToInt32((ListBottom - ListY) / RowH));
	const int32 First = FMath::Clamp(Player.GetColumnScroll(), 0, FMath::Max(0, Rows.Num() - 1));
	for (int32 Index = First; Index < Rows.Num() && Index < First + Visible; ++Index)
	{
		const FMadRecipeRow& Row = Rows[Index];
		const float RowY = ListY + (Index - First) * RowH;
		const bool bSelected = Row.Recipe->Id == Selected;
		if (bSelected)
		{
			DrawRect(FLinearColor(0.9f, 0.8f, 0.3f, 0.22f), X + 6.0f, RowY - 1.0f, SkillsPanelWidth - 12.0f, RowH);
		}
		const FString Label = FString::Printf(TEXT("%s%s"), *Definitions.GetItemName(Row.Recipe->Output.Item),
			Row.Recipe->Output.Count > 1 ? *FString::Printf(TEXT("  x%d"), Row.Recipe->Output.Count) : TEXT(""));
		const bool bIcon = DrawItemIcon(Row.Recipe->Output.Item, X + 12.0f, RowY, RowH - 2.0f, Row.CanCraftNow() ? 1.0f : 0.45f);
		DrawText(Label, Row.CanCraftNow() ? FLinearColor::White : FLinearColor(0.5f, 0.5f, 0.5f), X + (bIcon ? 36.0f : 14.0f), RowY, Small);
		if (Row.CanCraftNow())
		{
			DrawText(FString::Printf(TEXT("%d"), Row.Craftable), FLinearColor(0.5f, 0.85f, 0.4f), X + SkillsPanelWidth - 40.0f, RowY, Small);
		}
		AddHitBox(FVector2D(X + 6.0f, RowY - 1.0f), FVector2D(SkillsPanelWidth - 12.0f, RowH), FName(*FString::Printf(TEXT("craft.row.%s"), *Row.Recipe->Id.ToString())), true);
	}
	if (Rows.Num() == 0)
	{
		DrawText(TEXT("Nothing here yet."), FLinearColor(0.55f, 0.55f, 0.55f), X + 14.0f, ListY, Small);
	}
	else if (First > 0 || First + Visible < Rows.Num())
	{
		DrawText(FString::Printf(TEXT("%d above, %d below - mouse wheel to scroll"), First, FMath::Max(0, Rows.Num() - First - Visible)),
			FLinearColor(0.55f, 0.55f, 0.55f), X + 14.0f, ListBottom - 4.0f, Small);
	}

	// Detail pane.
	const float DetailY = Y + Height - DetailH;
	DrawRect(FLinearColor(1.0f, 1.0f, 1.0f, 0.05f), X + 6.0f, DetailY, SkillsPanelWidth - 12.0f, DetailH - 6.0f);
	const FMadRecipeRow* Row = Rows.FindByPredicate([Selected](const FMadRecipeRow& R) { return R.Recipe->Id == Selected; });
	if (Row == nullptr)
	{
		return;
	}
	const FMadRecipeDefinition& Recipe = *Row->Recipe;
	float LineY = DetailY + 8.0f;
	DrawText(FString::Printf(TEXT("%d x %s   (%.0f s)"), Recipe.Output.Count, *Definitions.GetItemName(Recipe.Output.Item), Recipe.CraftSeconds),
		FLinearColor(0.95f, 0.85f, 0.4f), X + 14.0f, LineY, Small);
	LineY += 20.0f;
	const FMadInventory& Backpack = Player.GetInventory()->GetInventory();
	for (const FMadItemAmount& Ingredient : Recipe.Ingredients)
	{
		const int32 Have = Backpack.CountItem(Ingredient.Item);
		DrawText(FString::Printf(TEXT("%d / %d  %s"), Have, Ingredient.Count, *Definitions.GetItemName(Ingredient.Item)),
			Have >= Ingredient.Count ? FLinearColor(0.6f, 0.9f, 0.5f) : FLinearColor(1.0f, 0.45f, 0.35f), X + 20.0f, LineY, Small);
		LineY += 16.0f;
	}
	if (!Row->bStation)
	{
		DrawText(FString::Printf(TEXT("needs a %s nearby"), *FMadGameplayDefinitions::GetBlockName(Recipe.Station)), FLinearColor(1.0f, 0.45f, 0.35f), X + 20.0f, LineY, Small);
		LineY += 16.0f;
	}
	if (!Row->bLevel)
	{
		DrawText(FString::Printf(TEXT("needs level %d"), Recipe.RequiredLevel), FLinearColor(1.0f, 0.45f, 0.35f), X + 20.0f, LineY, Small);
	}

	// Craft buttons along the bottom.
	const float ButtonY = DetailY + DetailH - 34.0f;
	const float ButtonW = (SkillsPanelWidth - 36.0f) / 3.0f;
	const TCHAR* Labels[] = { TEXT("Craft 1"), TEXT("Craft 5"), TEXT("Craft all") };
	const TCHAR* Boxes[] = { TEXT("craft.make.1"), TEXT("craft.make.5"), TEXT("craft.make.max") };
	const int32 Needed[] = { 1, 5, 1 };
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const bool bEnabled = Row->CanCraftNow() && Row->Craftable >= Needed[Index];
		const float ButtonX = X + 12.0f + Index * (ButtonW + 6.0f);
		DrawRect(bEnabled ? FLinearColor(0.25f, 0.5f, 0.2f, 0.95f) : FLinearColor(0.2f, 0.2f, 0.2f, 0.9f), ButtonX, ButtonY, ButtonW, 24.0f);
		DrawText(Labels[Index], bEnabled ? FLinearColor::White : FLinearColor(0.5f, 0.5f, 0.5f), ButtonX + 14.0f, ButtonY + 5.0f, Small);
		if (bEnabled)
		{
			AddHitBox(FVector2D(ButtonX, ButtonY), FVector2D(ButtonW, 24.0f), FName(Boxes[Index]), true);
		}
	}
}

void AMadHUD::DrawTooltip(const FMadItemStack& Stack)
{
	UFont* Small = GEngine->GetSmallFont();
	TArray<FString> Lines;
	MadFall::Items::DescribeStack(Stack, MadFall::GetGameplayDefinitions(), Lines);
	if (Lines.Num() == 0)
	{
		return;
	}

	// While trading, what it costs or what it fetches.
	if (const AMadPlayerCharacter* Player = Cast<AMadPlayerCharacter>(GetOwningPawn()))
	{
		FMadTraderState* State = nullptr;
		const FMadTraderDefinition* Trader = nullptr;
		if (Player->GetOpenTrade(State, Trader))
		{
			const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
			const FString Currency = Definitions.GetItemName(Trader->Currency);
			if (HoveredBox.ToString().StartsWith(TradeBoxPrefix))
			{
				Lines.Add(FString::Printf(TEXT("Costs %d %s each"), MadFall::Trade::GetUnitPrice(Stack, *Trader, Definitions), *Currency));
			}
			else if (HoveredBox.ToString().StartsWith(BackpackBoxPrefix))
			{
				const int32 Offer = MadFall::Trade::GetUnitOffer(Stack, *Trader, Definitions);
				Lines.Add(Offer > 0 ? FString::Printf(TEXT("Sells for %d %s each"), Offer, *Currency) : FString(TEXT("The trader will not buy this")));
			}
		}
	}

	float Width = 0.0f;
	for (const FString& Line : Lines)
	{
		float LineW = 0.0f;
		float LineH = 0.0f;
		GetTextSize(Line, LineW, LineH, Small);
		Width = FMath::Max(Width, LineW);
	}
	Width += 20.0f;
	const float Height = 12.0f + Lines.Num() * 17.0f;

	// Below and right of the cursor, kept on screen.
	const float X = FMath::Clamp(static_cast<float>(Mouse.X) + 18.0f, 4.0f, Canvas->ClipX - Width - 4.0f);
	const float Y = FMath::Clamp(static_cast<float>(Mouse.Y) + 18.0f, 4.0f, Canvas->ClipY - Height - 4.0f);
	DrawRect(FLinearColor(0.9f, 0.8f, 0.3f, 0.9f), X - 1.0f, Y - 1.0f, Width + 2.0f, Height + 2.0f);
	DrawRect(FLinearColor(0.04f, 0.04f, 0.05f, 0.97f), X, Y, Width, Height);
	for (int32 Index = 0; Index < Lines.Num(); ++Index)
	{
		DrawText(Lines[Index], Index == 0 ? FLinearColor(0.95f, 0.85f, 0.4f) : FLinearColor(0.85f, 0.85f, 0.85f),
			X + 10.0f, Y + 6.0f + Index * 17.0f, Small);
	}
}

void AMadHUD::NotifyHitBoxClick(FName BoxName)
{
	Super::NotifyHitBoxClick(BoxName);

	const APlayerController* Controller = GetOwningPlayerController();
	if (Controller == nullptr)
	{
		return;
	}
	// Input state is already updated for this click when the hit box is dispatched.
	const bool bQuick = Controller->IsInputKeyDown(EKeys::LeftShift) || Controller->IsInputKeyDown(EKeys::RightShift);
	const bool bHalf = Controller->IsInputKeyDown(EKeys::RightMouseButton);
	const bool bCtrl = Controller->IsInputKeyDown(EKeys::LeftControl) || Controller->IsInputKeyDown(EKeys::RightControl);
	ClickBox(BoxName, bQuick, bHalf, bCtrl);
}

void AMadHUD::ClickBox(FName BoxName, bool bQuick, bool bHalf, bool bCtrl)
{
	AMadPlayerCharacter* Player = Cast<AMadPlayerCharacter>(GetOwningPawn());
	if (Player == nullptr || !Player->IsInventoryOpen())
	{
		return;
	}

	if (BoxName == TakeAllBox)
	{
		Player->TakeAllFromContainer();
		return;
	}
	if (BoxName == SortBackpackBox || BoxName == SortContainerBox)
	{
		Player->SortInventory(BoxName == SortBackpackBox ? EMadInventorySide::Backpack : EMadInventorySide::Container);
		return;
	}

	const FString Name = BoxName.ToString();
	if (Name == TEXT("tab.crafting") || Name == TEXT("tab.skills"))
	{
		Player->SetInventoryTab(Name == TEXT("tab.crafting") ? EMadInventoryTab::Crafting : EMadInventoryTab::Skills);
		return;
	}
	if (Name.StartsWith(TEXT("craft.cat.")))
	{
		const int32 Index = FCString::Atoi(*Name.Mid(10));
		if (Index >= 0 && Index < static_cast<int32>(EMadRecipeCategory::Num))
		{
			Player->SetCraftCategory(static_cast<EMadRecipeCategory>(Index));
		}
		return;
	}
	if (Name == TEXT("craft.only"))
	{
		Player->SetCraftableOnly(!Player->IsCraftableOnly());
		return;
	}
	if (Name.StartsWith(TEXT("craft.row.")))
	{
		Player->SelectRecipe(FName(*Name.Mid(10)));
		return;
	}
	if (Name.StartsWith(TEXT("craft.make.")))
	{
		const FName Recipe = Player->GetSelectedRecipe();
		TArray<FMadRecipeRow> Rows;
		Player->GetRecipeRows(Rows);
		const FMadRecipeRow* Row = Rows.FindByPredicate([Recipe](const FMadRecipeRow& R) { return R.Recipe->Id == Recipe; });
		const FString Amount = Name.Mid(11);
		const int32 Times = Amount == TEXT("max") ? (Row ? Row->Craftable : 0) : FCString::Atoi(*Amount);
		if (Row != nullptr && Times > 0)
		{
			Player->CraftRecipe(Recipe, Times);
		}
		return;
	}
	if (Name.StartsWith(TradeBoxPrefix))
	{
		Player->BuyFromTrader(FCString::Atoi(*Name.Mid(FCString::Strlen(TradeBoxPrefix))), bQuick ? 0 : 1);
		return;
	}
	if (Player->IsTrading() && bQuick && Name.StartsWith(BackpackBoxPrefix))
	{
		// While trading, shift-click on your own items sells them.
		Player->SellToTrader(FCString::Atoi(*Name.Mid(FCString::Strlen(BackpackBoxPrefix))), 0);
		return;
	}
	if (Name.StartsWith(PerkBoxPrefix))
	{
		const FName Perk(*Name.Mid(FCString::Strlen(PerkBoxPrefix)));
		UE_LOG(LogMadFallGameplay, Display, TEXT("Perk %s: %s"), *Perk.ToString(), MadFall::Perks::ToString(Player->BuyPerk(Perk)));
		return;
	}

	EMadInventorySide Side = EMadInventorySide::Container;
	if (Name.StartsWith(BackpackBoxPrefix))
	{
		Side = EMadInventorySide::Backpack;
	}
	else if (Name.StartsWith(WornBoxPrefix))
	{
		Side = EMadInventorySide::Worn;
	}
	else if (!Name.StartsWith(ContainerBoxPrefix))
	{
		return;
	}

	int32 Slot = INDEX_NONE;
	if (!FDefaultValueHelper::ParseInt(Name.Mid(FCString::Strlen(BackpackBoxPrefix)), Slot))
	{
		return;
	}

	if (bCtrl && Side == EMadInventorySide::Backpack && !Player->GetInventoryCursor().bActive)
	{
		Player->RemoveModFromSlot(Slot);
		return;
	}
	Player->ClickInventorySlot(Side, Slot, bQuick, bHalf);
}

// ===========================================================================
// Console: drive the player from the command line
// ===========================================================================
//
// These call the same verbs the input bindings call. CI's gameplay acceptance
// test uses them to play the loop - give, aim, mine, craft, place, loot -
// without a human or a mouse.

namespace
{
	AMadPlayerCharacter* GetPlayer(UWorld* World)
	{
		const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
		AMadPlayerCharacter* Player = Controller ? Cast<AMadPlayerCharacter>(Controller->GetPawn()) : nullptr;
		if (Player == nullptr)
		{
			UE_LOG(LogMadFallGameplay, Error, TEXT("No MadFall player in this world (run with -game on a map using MadGameMode)."));
		}
		return Player;
	}

	bool ParseVoxel(const TArray<FString>& Args, int32 First, FIntVector& Out)
	{
		return Args.Num() >= First + 3
			&& FDefaultValueHelper::ParseInt(Args[First], Out.X)
			&& FDefaultValueHelper::ParseInt(Args[First + 1], Out.Y)
			&& FDefaultValueHelper::ParseInt(Args[First + 2], Out.Z);
	}

	FAutoConsoleCommandWithWorld CmdPlayerStatus(
		TEXT("mad.player.status"), TEXT("Vitals, position, target and inventory."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (AMadPlayerCharacter* P = GetPlayer(World)) { UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *P->DescribeStatus()); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerGive(
		TEXT("mad.player.give"), TEXT("mad.player.give <item> [count]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			if (P == nullptr || Args.Num() < 1) { return; }
			int32 Count = 1;
			if (Args.Num() > 1) { FDefaultValueHelper::ParseInt(Args[1], Count); }
			const int32 Left = P->GetInventory()->AddItem(FName(*Args[0]), Count);
			UE_LOG(LogMadFallGameplay, Display, TEXT("Gave %d x %s (%d did not fit)."), Count - Left, *Args[0], Left);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerSelect(
		TEXT("mad.player.select"), TEXT("mad.player.select <hotbar slot 0-8>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			int32 Slot = 0;
			if (P && Args.Num() > 0 && FDefaultValueHelper::ParseInt(Args[0], Slot)) { P->GetInventory()->SelectSlot(Slot); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerHold(
		TEXT("mad.player.hold"), TEXT("mad.player.hold <item> - move the first stack of an item into the last hotbar slot and select it."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			if (P == nullptr || Args.Num() < 1) { return; }

			UMadInventoryComponent* Inventory = P->GetInventory();
			FMadInventory& Items = Inventory->GetInventory();
			const FName Item(*Args[0]);
			const int32 Target = UMadInventoryComponent::HotbarSlots - 1;

			for (int32 Slot = 0; Slot < Items.NumSlots(); ++Slot)
			{
				if (!Items.GetSlot(Slot).IsEmpty() && Items.GetSlot(Slot).Item == Item)
				{
					const FMadItemStack Moving = Items.GetSlot(Slot);
					Items.SetSlot(Slot, Items.GetSlot(Target));
					Items.SetSlot(Target, Moving);
					Inventory->SelectSlot(Target);
					UE_LOG(LogMadFallGameplay, Display, TEXT("Holding %s x%d."), *Item.ToString(), Moving.Count);
					return;
				}
			}
			UE_LOG(LogMadFallGameplay, Display, TEXT("No %s in the inventory."), *Item.ToString());
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerShelter(
		TEXT("mad.player.shelter"), TEXT("mad.player.shelter [block=madfall:wood_frame] - a 3-high wall ring two voxels around the player."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			UMadVoxelWorldSubsystem* VoxelWorld = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
			if (P == nullptr || VoxelWorld == nullptr) { return; }

			const FName BlockId(Args.Num() > 0 ? *Args[0] : TEXT("madfall:wood_frame"));
			const uint16 RuntimeId = UMadVoxelWorldSubsystem::GetBlockRegistry().ResolveRuntimeId(BlockId);
			if (RuntimeId == MadFall::BlockTypeAir || RuntimeId == MadFall::BlockTypeUnresolved)
			{
				UE_LOG(LogMadFallGameplay, Error, TEXT("'%s' is not a registered block."), *BlockId.ToString());
				return;
			}

			FMadVoxel Voxel;
			Voxel.BlockTypeID = RuntimeId;
			Voxel.Density = 255;
			Voxel.Damage = 0;
			Voxel.Rotation = 0;
			Voxel.Flags = static_cast<uint8>(EMadVoxelFlags::Cubic);

			const FIntVector Feet = P->GetFeetVoxel();
			int32 Placed = 0;
			for (int32 DY = -2; DY <= 2; ++DY)
			{
				for (int32 DX = -2; DX <= 2; ++DX)
				{
					if (FMath::Max(FMath::Abs(DX), FMath::Abs(DY)) != 2) { continue; }
					// Foundation under the wall too, so it stands on uneven ground.
					for (int32 DZ = -1; DZ <= 2; ++DZ)
					{
						Placed += VoxelWorld->SetVoxel(Feet.X + DX, Feet.Y + DY, Feet.Z + DZ, Voxel) ? 1 : 0;
					}
				}
			}
			UE_LOG(LogMadFallGameplay, Display, TEXT("Shelter of %s around %s: %d blocks."), *BlockId.ToString(), *Feet.ToString(), Placed);
		}));

	FAutoConsoleCommandWithWorld CmdPlayerStress(
		TEXT("mad.player.stress"), TEXT("The HUD's structural load readout for the targeted block (runs over a few frames; repeat to see it)."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			UMadStructuralSubsystem* Structural = World ? World->GetSubsystem<UMadStructuralSubsystem>() : nullptr;
			if (P == nullptr || Structural == nullptr || !P->HasTarget())
			{
				UE_LOG(LogMadFallGameplay, Display, TEXT("Stress: no target."));
				return;
			}
			FMadStructuralNodeReport Report;
			switch (Structural->QueryStress(P->GetTarget().Voxel, Report))
			{
			case EMadStressQuery::Ready:
				UE_LOG(LogMadFallGameplay, Display, TEXT("Stress at %s: %.0f%% (carrying %.0f / %.0f kg)."),
					*P->GetTarget().Voxel.ToString(), Report.Stress * 100.0f, Report.CarriedKg, Report.CapacityKg);
				break;
			case EMadStressQuery::Pending:   UE_LOG(LogMadFallGameplay, Display, TEXT("Stress at %s: pending."), *P->GetTarget().Voxel.ToString()); break;
			case EMadStressQuery::TooLarge:  UE_LOG(LogMadFallGameplay, Display, TEXT("Stress: structure too large.")); break;
			default:                         UE_LOG(LogMadFallGameplay, Display, TEXT("Stress: not a structural block.")); break;
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerPillar(
		TEXT("mad.player.pillar"),
		TEXT("mad.player.pillar [height=3] [block=madfall:wood_frame] - lifts the player onto a pillar of blocks built under their feet."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			UMadVoxelWorldSubsystem* VoxelWorld = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
			if (P == nullptr || VoxelWorld == nullptr) { return; }

			int32 Height = 3;
			if (Args.Num() > 0) { FDefaultValueHelper::ParseInt(Args[0], Height); }
			Height = FMath::Clamp(Height, 1, 32);
			const FName BlockId(Args.Num() > 1 ? *Args[1] : TEXT("madfall:wood_frame"));
			const uint16 RuntimeId = UMadVoxelWorldSubsystem::GetBlockRegistry().ResolveRuntimeId(BlockId);
			if (RuntimeId == MadFall::BlockTypeAir || RuntimeId == MadFall::BlockTypeUnresolved)
			{
				UE_LOG(LogMadFallGameplay, Error, TEXT("'%s' is not a registered block."), *BlockId.ToString());
				return;
			}

			FMadVoxel Voxel;
			Voxel.BlockTypeID = RuntimeId;
			Voxel.Density = 255;
			Voxel.Damage = 0;
			Voxel.Rotation = 0;
			Voxel.Flags = static_cast<uint8>(EMadVoxelFlags::Cubic);

			// Up first, then build underneath, so the capsule is never inside a block.
			const FIntVector Feet = P->GetFeetVoxel();
			P->TeleportToVoxel(Feet + FIntVector(0, 0, Height));
			int32 Placed = 0;
			for (int32 DZ = 0; DZ < Height; ++DZ)
			{
				Placed += VoxelWorld->SetVoxel(Feet.X, Feet.Y, Feet.Z + DZ, Voxel) ? 1 : 0;
			}
			UE_LOG(LogMadFallGameplay, Display, TEXT("Pillar of %d x %s under the player at %s."), Placed, *BlockId.ToString(), *Feet.ToString());
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerOverhead(
		TEXT("mad.player.overhead"),
		TEXT("mad.player.overhead [block=madfall:wood_frame] [height=4] [radius=1] - an unsupported slab above the player's feet; it collapses onto them."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			UMadVoxelWorldSubsystem* VoxelWorld = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
			if (P == nullptr || VoxelWorld == nullptr) { return; }

			const FName BlockId(Args.Num() > 0 ? *Args[0] : TEXT("madfall:wood_frame"));
			int32 Height = 4;
			int32 Radius = 1;
			if (Args.Num() > 1) { FDefaultValueHelper::ParseInt(Args[1], Height); }
			if (Args.Num() > 2) { FDefaultValueHelper::ParseInt(Args[2], Radius); }
			Height = FMath::Clamp(Height, 2, 64);
			Radius = FMath::Clamp(Radius, 0, 4);

			const uint16 RuntimeId = UMadVoxelWorldSubsystem::GetBlockRegistry().ResolveRuntimeId(BlockId);
			if (RuntimeId == MadFall::BlockTypeAir || RuntimeId == MadFall::BlockTypeUnresolved)
			{
				UE_LOG(LogMadFallGameplay, Error, TEXT("'%s' is not a registered block."), *BlockId.ToString());
				return;
			}

			FMadVoxel Voxel;
			Voxel.BlockTypeID = RuntimeId;
			Voxel.Density = 255;
			Voxel.Damage = 0;
			Voxel.Rotation = 0;
			Voxel.Flags = static_cast<uint8>(EMadVoxelFlags::Cubic);

			// Written in one frame, so the structural job sees the whole slab and
			// it falls as one cluster.
			const FIntVector Feet = P->GetFeetVoxel();
			int32 Placed = 0;
			for (int32 DY = -Radius; DY <= Radius; ++DY)
			{
				for (int32 DX = -Radius; DX <= Radius; ++DX)
				{
					Placed += VoxelWorld->SetVoxel(Feet.X + DX, Feet.Y + DY, Feet.Z + Height, Voxel) ? 1 : 0;
				}
			}
			UE_LOG(LogMadFallGameplay, Display, TEXT("Overhead slab of %s at %d above %s: %d blocks."), *BlockId.ToString(), Height, *Feet.ToString(), Placed);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerTeleport(
		TEXT("mad.player.tp"), TEXT("mad.player.tp <x> <y> <z> - voxel the player's feet stand in."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			FIntVector V;
			if (P && ParseVoxel(Args, 0, V)) { P->TeleportToVoxel(V); }
		}));

	/**
	 * Scenes built around an anchor rather than at fixed world coordinates.
	 *
	 * WHY: the CI scenes used to write blocks at absolute voxels a few steps from
	 * spawn ("mad.voxel.set 2 -6 22 madfall:ladder"), which assumed the ground
	 * there stood at a particular height. Retuning the lowland biomes moved it by
	 * a voxel and six checks failed - a placed block ended up where a scripted
	 * walk went. A scene now anchors itself where the survivor spawned, flattens
	 * a pad, and places everything relative to that anchor, so it reads the same
	 * whatever the generator does. The anchor is deliberately not the survivor's
	 * current position: scenes move them about while building.
	 */
	FIntVector GSceneAnchor = FIntVector::ZeroValue;
	bool bGSceneAnchored = false;

	FIntVector SceneAnchor(AMadPlayerCharacter* Player)
	{
		if (!bGSceneAnchored && Player != nullptr)
		{
			GSceneAnchor = Player->GetFeetVoxel();
			bGSceneAnchored = true;
		}
		return GSceneAnchor;
	}

	FAutoConsoleCommandWithWorld CmdSceneAnchor(
		TEXT("mad.scene.anchor"),
		TEXT("Anchors scripted scenes at the voxel the survivor stands in; mad.scene.set and mad.scene.tp are relative to it."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			if (P == nullptr)
			{
				return;
			}
			bGSceneAnchored = false;
			const FIntVector Anchor = SceneAnchor(P);
			UE_LOG(LogMadFallGameplay, Display, TEXT("Scene anchored at X=%d Y=%d Z=%d."), Anchor.X, Anchor.Y, Anchor.Z);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdSceneSet(
		TEXT("mad.scene.set"),
		TEXT("mad.scene.set <dx> <dy> <dz> <blockId> [density] [orientation] [variant] - mad.voxel.set, offset from the scene anchor."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			FIntVector Delta;
			if (P == nullptr || !ParseVoxel(Args, 0, Delta) || Args.Num() < 4)
			{
				UE_LOG(LogMadFallGameplay, Error, TEXT("Usage: mad.scene.set <dx> <dy> <dz> <blockId> [density] [orientation] [variant]"));
				return;
			}
			const FIntVector At = SceneAnchor(P) + Delta;
			FString Command = FString::Printf(TEXT("mad.voxel.set %d %d %d"), At.X, At.Y, At.Z);
			for (int32 Index = 3; Index < Args.Num(); ++Index)
			{
				Command += TEXT(" ") + Args[Index];
			}
			GEngine->Exec(World, *Command);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdSceneAim(
		TEXT("mad.scene.aim"),
		TEXT("mad.scene.aim <dx> <dy> <dz> - looks at a voxel offset from the scene anchor."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			FIntVector Delta;
			if (P != nullptr && ParseVoxel(Args, 0, Delta))
			{
				const FIntVector At = SceneAnchor(P) + Delta;
				GEngine->Exec(World, *FString::Printf(TEXT("mad.player.aim %d %d %d"), At.X, At.Y, At.Z));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdSceneBox(
		TEXT("mad.scene.box"),
		TEXT("mad.scene.box <blockId> <dx0> <dy0> <dz0> <dx1> <dy1> <dz1> [hollow] - mad.voxel.box, offset from the scene anchor."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			FIntVector From, To;
			if (P == nullptr || Args.Num() < 7 || !ParseVoxel(Args, 1, From) || !ParseVoxel(Args, 4, To))
			{
				UE_LOG(LogMadFallGameplay, Error, TEXT("Usage: mad.scene.box <blockId> <dx0> <dy0> <dz0> <dx1> <dy1> <dz1> [hollow]"));
				return;
			}
			const FIntVector Anchor = SceneAnchor(P);
			From += Anchor;
			To += Anchor;
			FString Command = FString::Printf(TEXT("mad.voxel.box %s %d %d %d %d %d %d"), *Args[0], From.X, From.Y, From.Z, To.X, To.Y, To.Z);
			for (int32 Index = 7; Index < Args.Num(); ++Index)
			{
				Command += TEXT(" ") + Args[Index];
			}
			GEngine->Exec(World, *Command);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdSceneTeleport(
		TEXT("mad.scene.tp"),
		TEXT("mad.scene.tp <dx> <dy> <dz> - teleports that many voxels from the scene anchor."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			FIntVector Delta;
			if (P != nullptr && ParseVoxel(Args, 0, Delta))
			{
				P->TeleportToVoxel(SceneAnchor(P) + Delta);
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdScenePad(
		TEXT("mad.scene.pad"),
		TEXT("mad.scene.pad [radius=10] [headroom=8] - flattens a square of ground around the scene anchor: stone at the anchor's floor, air above."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			if (P == nullptr)
			{
				return;
			}
			int32 Radius = 10;
			int32 Headroom = 8;
			if (Args.Num() > 0) { FDefaultValueHelper::ParseInt(Args[0], Radius); }
			if (Args.Num() > 1) { FDefaultValueHelper::ParseInt(Args[1], Headroom); }
			Radius = FMath::Clamp(Radius, 1, 32);
			Headroom = FMath::Clamp(Headroom, 1, 32);

			// The floor goes one voxel below the anchor, so the survivor stands on
			// the pad rather than in it, and the air above clears whatever hillside
			// or tree was there.
			const FIntVector Anchor = SceneAnchor(P);
			GEngine->Exec(World, *FString::Printf(TEXT("mad.voxel.box madfall:air %d %d %d %d %d %d"),
				Anchor.X - Radius, Anchor.Y - Radius, Anchor.Z, Anchor.X + Radius, Anchor.Y + Radius, Anchor.Z + Headroom - 1));
			GEngine->Exec(World, *FString::Printf(TEXT("mad.voxel.box madfall:stone %d %d %d %d %d %d"),
				Anchor.X - Radius, Anchor.Y - Radius, Anchor.Z - 1, Anchor.X + Radius, Anchor.Y + Radius, Anchor.Z - 1));
			UE_LOG(LogMadFallGameplay, Display, TEXT("Scene pad of %d x %d voxels with its floor at Z=%d, %d clear above."),
				Radius * 2 + 1, Radius * 2 + 1, Anchor.Z - 1, Headroom);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerWalk(
		TEXT("mad.player.walk"), TEXT("mad.player.walk <seconds> [dx dy] - walks for a while along a world direction (default +X)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			float Seconds = 0.0f;
			if (!P || Args.Num() < 1 || !FDefaultValueHelper::ParseFloat(Args[0], Seconds)) { return; }
			float DX = 1.0f, DY = 0.0f;
			if (Args.Num() > 2) { FDefaultValueHelper::ParseFloat(Args[1], DX); FDefaultValueHelper::ParseFloat(Args[2], DY); }
			P->WalkFor(Seconds, FVector(DX, DY, 0.0));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerTeleportBiome(
		TEXT("mad.player.tpbiome"), TEXT("mad.player.tpbiome <biomeId> [maxRadius=6000] - stand on the surface well inside the nearest such biome."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			UMadVoxelWorldSubsystem* VoxelWorld = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
			const FMadWorldGenerator* Generator = VoxelWorld ? VoxelWorld->GetWorldGenerator() : nullptr;
			if (!P || !Generator || Args.Num() < 1) { return; }

			const int32 BiomeIndex = UMadVoxelWorldSubsystem::GetBiomeRegistry().FindIndex(FName(*Args[0]));
			int32 MaxRadius = 6000;
			if (Args.Num() > 1) { FDefaultValueHelper::ParseInt(Args[1], MaxRadius); }
			const FIntVector Feet = P->GetFeetVoxel();
			FIntPoint Column;
			if (BiomeIndex == INDEX_NONE || !Generator->FindBiomeNear(BiomeIndex, FIntPoint(Feet.X, Feet.Y), MaxRadius, Column))
			{
				UE_LOG(LogMadFallGameplay, Warning, TEXT("No %s within %d voxels."), *Args[0], MaxRadius);
				return;
			}
			const int32 Surface = FMath::FloorToInt(Generator->GetSurfaceHeight(static_cast<float>(Column.X), static_cast<float>(Column.Y)));
			const int32 Top = Generator->FindTerrainTopBelow(Column.X, Column.Y, Surface + 12, 32);
			const FIntVector Destination(Column.X, Column.Y, (Top == INDEX_NONE ? Surface : Top) + 1);
			P->TravelToVoxel(Destination);
			UE_LOG(LogMadFallGameplay, Display, TEXT("Teleported to %s in %s."), *Destination.ToString(), *Args[0]);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerAim(
		TEXT("mad.player.aim"), TEXT("mad.player.aim <x> <y> <z> - look at a voxel."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			FIntVector V;
			if (P && ParseVoxel(Args, 0, V))
			{
				P->AimAtVoxel(V);
				UE_LOG(LogMadFallGameplay, Display, TEXT("Aiming at %s: %s"), *V.ToString(),
					P->HasTarget() ? *FString::Printf(TEXT("target %s face %s"), *P->GetTarget().Voxel.ToString(), *P->GetTarget().Normal.ToString())
					               : TEXT("no target in reach"));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerAimRelative(
		TEXT("mad.player.aimrel"), TEXT("mad.player.aimrel <dx> <dy> <dz> - look at a voxel relative to the player's feet."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			FIntVector Offset;
			if (P && ParseVoxel(Args, 0, Offset))
			{
				const FIntVector V = P->GetFeetVoxel() + Offset;
				P->AimAtVoxel(V);
				UE_LOG(LogMadFallGameplay, Display, TEXT("Aiming at %s: %s"), *V.ToString(),
					P->HasTarget() ? *FString::Printf(TEXT("target %s face %s"), *P->GetTarget().Voxel.ToString(), *P->GetTarget().Normal.ToString())
					               : TEXT("no target in reach"));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerUse(
		TEXT("mad.player.use"), TEXT("mad.player.use [swings=1] - swing the held item at the target, ignoring cooldown."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			if (P == nullptr) { return; }
			int32 Swings = 1;
			if (Args.Num() > 0) { FDefaultValueHelper::ParseInt(Args[0], Swings); }
			int32 Landed = 0;
			for (int32 Index = 0; Index < Swings; ++Index)
			{
				// Stamina is refilled between console swings: this tests mining, not endurance.
				FMadSurvivalStats Stats = P->GetSurvival()->GetStats();
				Stats.Stamina = Stats.MaxStamina;
				P->GetSurvival()->SetStats(Stats);
				Landed += P->UsePrimary(/*bIgnoreCooldown*/ true) ? 1 : 0;
			}
			UE_LOG(LogMadFallGameplay, Display, TEXT("%d of %d swing(s) landed."), Landed, Swings);
		}));

	FAutoConsoleCommandWithWorld CmdPlayerPlace(
		TEXT("mad.player.place"), TEXT("Place the held block against the target face, or consume the held item."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (AMadPlayerCharacter* P = GetPlayer(World))
			{
				UE_LOG(LogMadFallGameplay, Display, TEXT("Secondary use %s."), P->UseSecondary() ? TEXT("succeeded") : TEXT("did nothing"));
			}
		}));

	FAutoConsoleCommandWithWorld CmdPlayerInteract(
		TEXT("mad.player.interact"), TEXT("Open the targeted container."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (AMadPlayerCharacter* P = GetPlayer(World))
			{
				UE_LOG(LogMadFallGameplay, Display, TEXT("Interact %s."), P->Interact() ? TEXT("succeeded") : TEXT("found nothing to use"));
			}
		}));

	bool ParseSide(const FString& Text, EMadInventorySide& Out)
	{
		if (Text == TEXT("b") || Text == TEXT("backpack")) { Out = EMadInventorySide::Backpack; return true; }
		if (Text == TEXT("c") || Text == TEXT("container")) { Out = EMadInventorySide::Container; return true; }
		UE_LOG(LogMadFallGameplay, Error, TEXT("'%s' is not a side: use b (backpack) or c (container)."), *Text);
		return false;
	}

	FString DescribeSlots(const FMadInventory& Slots)
	{
		FString Out;
		for (int32 Slot = 0; Slot < Slots.NumSlots(); ++Slot)
		{
			const FMadItemStack& Stack = Slots.GetSlot(Slot);
			if (!Stack.IsEmpty())
			{
				Out += FString::Printf(TEXT("  [%2d] %s x%d\n"), Slot, *Stack.Item.ToString(), Stack.Count);
			}
		}
		return Out.IsEmpty() ? FString(TEXT("  (empty)\n")) : Out;
	}

	FAutoConsoleCommandWithWorld CmdPlayerContainer(
		TEXT("mad.player.container"), TEXT("Lists the open container's slots."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			const AMadPlayerCharacter* P = GetPlayer(World);
			const FMadInventory* Crate = P ? P->GetOpenContainerInventory() : nullptr;
			UE_LOG(LogMadFallGameplay, Display, TEXT("Container:\n%s"), Crate ? *DescribeSlots(*Crate) : TEXT("  (none open)\n"));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerInventoryMove(
		TEXT("mad.player.invmove"), TEXT("mad.player.invmove <b|c> <slot> <b|c> <slot> [count] - drag a stack in the inventory screen."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			EMadInventorySide From, To;
			int32 FromSlot = 0, ToSlot = 0, Count = 0;
			if (P == nullptr || Args.Num() < 4 || !ParseSide(Args[0], From) || !ParseSide(Args[2], To)
				|| !FDefaultValueHelper::ParseInt(Args[1], FromSlot) || !FDefaultValueHelper::ParseInt(Args[3], ToSlot))
			{
				return;
			}
			if (Args.Num() > 4) { FDefaultValueHelper::ParseInt(Args[4], Count); }
			UE_LOG(LogMadFallGameplay, Display, TEXT("Inventory move: %s."), MadFall::InventoryOps::ToString(P->MoveItem(From, FromSlot, To, ToSlot, Count)));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdHudClick(
		TEXT("mad.hud.click"), TEXT("mad.hud.click <box> [shift] [right] [ctrl] - clicks an inventory-screen hit box: inv.b.<slot>, inv.c.<slot>, inv.w.<slot>, inv.takeall, inv.sort.b, inv.sort.c, perk.<id>."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
			AMadHUD* Hud = Controller ? Cast<AMadHUD>(Controller->GetHUD()) : nullptr;
			if (Hud == nullptr || Args.Num() < 1)
			{
				UE_LOG(LogMadFallGameplay, Warning, TEXT("usage: mad.hud.click <box> [shift] [right] (needs the survival HUD)"));
				return;
			}
			Hud->ClickBox(FName(*Args[0]), Args.Contains(TEXT("shift")), Args.Contains(TEXT("right")), Args.Contains(TEXT("ctrl")));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerBuried(
		TEXT("mad.player.buried"), TEXT("mad.player.buried - how many solid voxels are inside the survivor's capsule."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			const UMadVoxelWorldSubsystem* VoxelWorld = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
			if (P == nullptr || VoxelWorld == nullptr)
			{
				return;
			}
			TArray<FIntVector> Voxels;
			MadFall::Debris::GetPawnVoxels(P->GetActorLocation(), P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),
				P->GetCapsuleComponent()->GetScaledCapsuleRadius(), Voxels);
			int32 Solid = 0;
			FString Detail;
			const int32 FeetZ = P->GetFeetVoxel().Z;
			for (const FIntVector& Voxel : Voxels)
			{
				// Smooth ground fills part of the voxel the feet stand in; buried is above that.
				if (Voxel.Z <= FeetZ)
				{
					continue;
				}
				const FMadVoxel Contents = VoxelWorld->GetVoxel(Voxel.X, Voxel.Y, Voxel.Z);
				if (Contents.IsSolid())
				{
					++Solid;
					Detail += FString::Printf(TEXT(" %s=%s"), *Voxel.ToString(),
						*UMadVoxelWorldSubsystem::GetBlockRegistry().GetStringId(Contents.BlockTypeID).ToString());
				}
			}
			UE_LOG(LogMadFallGameplay, Display, TEXT("Buried: %s (%d solid of %d voxel(s) in the survivor's space)%s."), Solid > 0 ? TEXT("yes") : TEXT("no"), Solid, Voxels.Num(), *Detail);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerSlots(
		TEXT("mad.player.slots"), TEXT("mad.player.slots - every non-empty backpack slot as index=item xcount, in slot order."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			if (P == nullptr)
			{
				return;
			}
			const FMadInventory& Backpack = P->GetInventory()->GetInventory();
			FString Out;
			for (int32 Slot = 0; Slot < Backpack.NumSlots(); ++Slot)
			{
				const FMadItemStack& Stack = Backpack.GetSlot(Slot);
				if (!Stack.IsEmpty())
				{
					Out += FString::Printf(TEXT(" %d=%s x%d%s"), Slot, *Stack.Item.ToString(), Stack.Count,
						Stack.Mods.Num() > 0 ? *FString::Printf(TEXT(" (%d mod)"), Stack.Mods.Num()) : TEXT(""));
				}
			}
			UE_LOG(LogMadFallGameplay, Display, TEXT("Slots:%s"), *Out);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerInvHeld(
		TEXT("mad.player.invheld"), TEXT("mad.player.invheld <delta> - takes more or fewer of the held stack, as the mouse wheel does, and prints the held count."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			if (P == nullptr)
			{
				return;
			}
			int32 Delta = 0;
			if (Args.Num() > 0) { FDefaultValueHelper::ParseInt(Args[0], Delta); }
			P->AdjustHeldCount(Delta);
			UE_LOG(LogMadFallGameplay, Display, TEXT("Holding %d."), P->GetHeldCount());
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerRecipes(
		TEXT("mad.player.recipes"), TEXT("mad.player.recipes - the crafting column's list as it is now: tab, filter and rows in order."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			if (P == nullptr)
			{
				return;
			}
			TArray<FMadRecipeRow> Rows;
			P->GetRecipeRows(Rows);
			FString Out = FString::Printf(TEXT("Recipes: %s%s, %d row(s), selected %s"), MadFall::Crafting::GetCategoryName(P->GetCraftCategory()),
				P->IsCraftableOnly() ? TEXT(", craftable only") : TEXT(""), Rows.Num(), *P->GetSelectedRecipe().ToString());
			for (const FMadRecipeRow& Row : Rows)
			{
				Out += FString::Printf(TEXT("\n  %s %s x%d"), Row.CanCraftNow() ? TEXT("+") : TEXT("-"), *Row.Recipe->Id.ToString(), Row.Craftable);
			}
			UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Out);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerTooltip(
		TEXT("mad.player.tooltip"), TEXT("mad.player.tooltip <b|c|w> <slot> - prints the tooltip of a stack."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			EMadInventorySide Side;
			int32 Slot = 0;
			if (P == nullptr || Args.Num() < 2 || !ParseSide(Args[0], Side) || !FDefaultValueHelper::ParseInt(Args[1], Slot))
			{
				return;
			}
			const FMadInventory* Items = Side == EMadInventorySide::Backpack ? &P->GetInventory()->GetInventory()
				: Side == EMadInventorySide::Worn ? &P->GetInventory()->GetWorn() : P->GetOpenContainerInventory();
			if (Items == nullptr || Slot < 0 || Slot >= Items->NumSlots())
			{
				return;
			}
			TArray<FString> Lines;
			MadFall::Items::DescribeStack(Items->GetSlot(Slot), MadFall::GetGameplayDefinitions(), Lines);
			UE_LOG(LogMadFallGameplay, Display, TEXT("Tooltip: %s"), *FString::Join(Lines, TEXT(" | ")));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerStore(
		TEXT("mad.player.store"), TEXT("mad.player.store <item> - quick-moves the first backpack stack of an item into the open container."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			if (P == nullptr || Args.Num() < 1) { return; }
			if (!P->HasOpenContainer())
			{
				UE_LOG(LogMadFallGameplay, Error, TEXT("No container is open (mad.player.interact first)."));
				return;
			}
			const FName Item(*Args[0]);
			const FMadInventory& Backpack = P->GetInventory()->GetInventory();
			for (int32 Slot = 0; Slot < Backpack.NumSlots(); ++Slot)
			{
				if (Backpack.GetSlot(Slot).Item == Item)
				{
					const int32 Moved = P->QuickMoveItem(EMadInventorySide::Backpack, Slot);
					UE_LOG(LogMadFallGameplay, Display, TEXT("Stored %d x %s in the container."), Moved, *Item.ToString());
					return;
				}
			}
			UE_LOG(LogMadFallGameplay, Display, TEXT("No %s in the backpack."), *Item.ToString());
		}));

	FAutoConsoleCommandWithWorld CmdPlayerTakeAll(
		TEXT("mad.player.takeall"), TEXT("Takes everything that fits from the open container."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (AMadPlayerCharacter* P = GetPlayer(World))
			{
				UE_LOG(LogMadFallGameplay, Display, TEXT("Took %d item(s) from the container."), P->TakeAllFromContainer());
			}
		}));

	FAutoConsoleCommandWithWorld CmdQuests(
		TEXT("mad.quests"), TEXT("Lists active quests with their progress, and how many are complete."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			const AMadPlayerCharacter* P = GetPlayer(World);
			if (P == nullptr)
			{
				return;
			}
			const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
			TArray<TPair<const FMadQuestDefinition*, const FMadQuestProgress*>> Active;
			P->GetQuestLog().GetJournal(Definitions, Active);
			UE_LOG(LogMadFallGameplay, Display, TEXT("Quests: %d active, %d complete."), Active.Num(), P->GetQuestLog().NumCompleted());
			for (const TPair<const FMadQuestDefinition*, const FMadQuestProgress*>& Entry : Active)
			{
				UE_LOG(LogMadFallGameplay, Display, TEXT("  %s (%s)"), *Entry.Key->Id.ToString(), *MadFall::Localize(Entry.Key->DisplayName));
				for (int32 Index = 0; Index < Entry.Key->Objectives.Num(); ++Index)
				{
					UE_LOG(LogMadFallGameplay, Display, TEXT("    %s: %d/%d"), *MadFall::Quests::DescribeObjective(Entry.Key->Objectives[Index], Definitions),
						Entry.Value->Counts.IsValidIndex(Index) ? Entry.Value->Counts[Index] : 0, Entry.Key->Objectives[Index].Count);
				}
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdQuestComplete(
		TEXT("mad.quests.complete"), TEXT("mad.quests.complete <quest id>: completes a quest and pays its rewards."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			if (P != nullptr && Args.Num() > 0 && !P->CompleteQuest(FName(*Args[0])))
			{
				UE_LOG(LogMadFallGameplay, Warning, TEXT("No incomplete quest '%s'."), *Args[0]);
			}
		}));

	FAutoConsoleCommandWithWorld CmdPlayerOpenInventory(
		TEXT("mad.player.openinventory"), TEXT("Opens the inventory screen (backpack and worn clothing)."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (AMadPlayerCharacter* P = GetPlayer(World))
			{
				P->OpenInventory();
			}
		}));

	FAutoConsoleCommandWithWorld CmdPlayerCloseInventory(
		TEXT("mad.player.closeinventory"), TEXT("Closes the inventory screen."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (AMadPlayerCharacter* P = GetPlayer(World))
			{
				P->CloseInventory();
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerCraft(
		TEXT("mad.player.craft"), TEXT("mad.player.craft <recipe> [times]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			if (P == nullptr || Args.Num() < 1) { return; }
			int32 Times = 1;
			if (Args.Num() > 1) { FDefaultValueHelper::ParseInt(Args[1], Times); }
			const EMadCraftResult Result = P->CraftRecipe(FName(*Args[0]), Times);
			UE_LOG(LogMadFallGameplay, Display, TEXT("Craft %s x%d: %s"), *Args[0], Times, MadFall::Crafting::ToString(Result));
		}));

	FAutoConsoleCommandWithWorld CmdPlayerRepair(
		TEXT("mad.player.repair"), TEXT("Repair the held tool with materials from the backpack."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (AMadPlayerCharacter* P = GetPlayer(World))
			{
				UE_LOG(LogMadFallGameplay, Display, TEXT("Repair: %s"), MadFall::Items::ToString(P->RepairSelected()));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerInstallMod(
		TEXT("mad.player.installmod"), TEXT("mad.player.installmod <mod item> - install a mod into the held tool."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			if (P && Args.Num() > 0)
			{
				UE_LOG(LogMadFallGameplay, Display, TEXT("Install %s: %s"), *Args[0], MadFall::Items::ToString(P->InstallModOnSelected(FName(*Args[0]))));
			}
		}));

	FAutoConsoleCommandWithWorld CmdPlayerDrop(
		TEXT("mad.player.drop"), TEXT("Drop the held stack on the ground."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (AMadPlayerCharacter* P = GetPlayer(World))
			{
				UE_LOG(LogMadFallGameplay, Display, TEXT("Drop %s."), P->DropSelected() ? TEXT("succeeded") : TEXT("did nothing"));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerPerk(
		TEXT("mad.player.perk"), TEXT("mad.player.perk <perk id> - spend a level-up point on the next rank."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			if (P && Args.Num() > 0)
			{
				UE_LOG(LogMadFallGameplay, Display, TEXT("Perk %s: %s"), *Args[0], MadFall::Perks::ToString(P->BuyPerk(FName(*Args[0]))));
			}
		}));

	FAutoConsoleCommandWithWorld CmdPerks(
		TEXT("mad.perks"), TEXT("Lists perks, ranks owned and points available."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			const AMadPlayerCharacter* P = GetPlayer(World);
			FString Out = FString::Printf(TEXT("Level %d, %d perk point(s) unspent\n"), P ? P->GetLevel() : 0, P ? P->GetUnspentPerkPoints() : 0);
			for (const FMadPerkDefinition& Perk : MadFall::GetGameplayDefinitions().GetPerks())
			{
				const int32 Owned = P ? P->GetPerkRanks().FindRef(Perk.Id) : 0;
				FString Next = Owned < Perk.Ranks.Num() ? FString::Printf(TEXT("next at level %d"), Perk.Ranks[Owned].RequiredLevel) : FString(TEXT("maxed"));
				Out += FString::Printf(TEXT("  %-28s %d/%d  %s  %s\n"), *Perk.Id.ToString(), Owned, Perk.Ranks.Num(), *Next, *MadFall::Localize(Perk.Description));
			}
			UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Out);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerExperience(
		TEXT("mad.player.xp"), TEXT("mad.player.xp <amount> - grant experience."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			int32 Amount = 0;
			if (P && Args.Num() > 0 && FDefaultValueHelper::ParseInt(Args[0], Amount))
			{
				P->AddExperience(Amount);
				UE_LOG(LogMadFallGameplay, Display, TEXT("Level %d, %d/%d xp, %d perk point(s)."), P->GetLevel(), P->GetExperience(),
					P->GetExperienceForNextLevel(), P->GetUnspentPerkPoints());
			}
		}));

	FAutoConsoleCommandWithWorld CmdPlayerCancelCraft(
		TEXT("mad.player.cancelcraft"), TEXT("Cancel queued crafts and refund their ingredients."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (AMadPlayerCharacter* P = GetPlayer(World)) { P->CancelCrafting(); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPlayerDamage(
		TEXT("mad.player.damage"), TEXT("mad.player.damage <amount>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			AMadPlayerCharacter* P = GetPlayer(World);
			float Amount = 0.0f;
			if (P && Args.Num() > 0 && FDefaultValueHelper::ParseFloat(Args[0], Amount)) { P->GetSurvival()->ApplyDamage(Amount); }
		}));

	FAutoConsoleCommandWithWorld CmdItems(
		TEXT("mad.items"), TEXT("Lists items, recipes and loot tables."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld*)
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *MadFall::GetGameplayDefinitions().DescribeContents());
		}));
}
