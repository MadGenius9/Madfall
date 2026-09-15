// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadBiomeRegistry.h"
#include "MadBlockRegistry.h"
#include "MadGameplaySave.h"
#include "MadWorldGenerator.h"
#include "MadWorldMap.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadWorldMapTest,
	"MadFall.World.Map",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadWorldMapTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::WorldMap;

	// Revealing marks a disc of cells once.
	TSet<FIntPoint> Explored;
	const int32 First = Reveal(Explored, FVector2D(10.0, 10.0), 128);
	TestTrue(*FString::Printf(TEXT("a reveal marks a disc of cells (%d)"), First), First >= 40 && First <= 90);
	TestEqual(TEXT("revealing the same place again adds nothing"), Reveal(Explored, FVector2D(10.0, 10.0), 128), 0);
	TestTrue(TEXT("the survivor's own cell is explored"), Explored.Contains(CellOf(10.0, 10.0)));
	TestFalse(TEXT("far away is not"), Explored.Contains(CellOf(2000.0, 10.0)));
	TestEqual(TEXT("negative coordinates floor"), CellOf(-1.0, -33.0), FIntPoint(-1, -2));

	// Orientation: north (+X) is up, east (+Y) is right.
	FMadMapImage Frame;
	Frame.Size = 256;
	Frame.VoxelsPerPixel = 8;
	Frame.Centre = FIntPoint(1000, 2000);
	const FVector2D Centre = ToPixel(Frame, FVector2D(1000.0, 2000.0));
	TestTrue(TEXT("the centre is mid-image"), Centre.Equals(FVector2D(128.0, 128.0), 0.01));
	TestTrue(TEXT("north is up"), ToPixel(Frame, FVector2D(1080.0, 2000.0)).Y < Centre.Y - 9.0);
	TestTrue(TEXT("east is right"), ToPixel(Frame, FVector2D(1000.0, 2080.0)).X > Centre.X + 9.0);

	// A drawn image: explored land has colour, the unexplored stays dark, and it round-trips a save.
	FMadBlockRegistry Blocks;
	FMadBiomeRegistry Biomes;
	TArray<FMadDefinitionError> Errors;
	Blocks.BeginLoad();
	Blocks.AddFromDirectory(FPaths::Combine(FPaths::ProjectDir(), TEXT("Definitions"), TEXT("blocks")), FName(TEXT("madfall")), Errors);
	Blocks.FinishLoad(Errors);
	Biomes.BeginLoad();
	Biomes.AddFromDirectory(FPaths::Combine(FPaths::ProjectDir(), TEXT("Definitions"), TEXT("biomes")), FName(TEXT("madfall")), Errors);
	Biomes.FinishLoad(Errors);
	FMadWorldGenSettings Settings;
	Settings.Seed = 20260913u;
	const FMadWorldGenerator Generator(Settings, Biomes, Blocks);

	TArray<FColor> Colours;
	Colours.Init(FColor(40, 160, 40), Biomes.Num());
	TSet<FIntPoint> Seen;
	Reveal(Seen, FVector2D(0.0, 0.0), 300);
	FMadMapImage Image;
	BuildImage(Generator, Colours, FColor(20, 40, 200), Seen, FIntPoint(0, 0), 128, 8, Image);
	TestEqual(TEXT("image size"), Image.Pixels.Num(), 128 * 128);
	TestNotEqual(TEXT("the centre is drawn"), Image.Pixels[64 + 64 * 128], Unexplored);
	TestEqual(TEXT("a corner 700 voxels out is unexplored"), Image.Pixels[0], Unexplored);
	int32 Drawn = 0;
	for (const FColor& Pixel : Image.Pixels)
	{
		Drawn += Pixel != Unexplored ? 1 : 0;
	}
	TestTrue(*FString::Printf(TEXT("about the revealed disc is drawn (%d px)"), Drawn), Drawn > 3000 && Drawn < 7000);

	FMadGameplaySave Save;
	Save.Explored = Seen.Array();
	FMadGameplaySave Loaded;
	TArray<FString> Warnings;
	TestTrue(TEXT("save parses"), MadFall::GameplaySave::FromJson(MadFall::GameplaySave::ToJson(Save), Loaded, Warnings));
	TestTrue(TEXT("explored cells survive the save"), TSet<FIntPoint>(Loaded.Explored).Num() == Seen.Num() && Seen.Includes(TSet<FIntPoint>(Loaded.Explored)));
	return true;
}

#endif
