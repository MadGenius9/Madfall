// Copyright MadFall. All Rights Reserved.

#include "MadHudLayout.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadHudLayoutTests
{
	/** Window sizes a player might actually have, from a small window to 4K. */
	const FIntPoint Sizes[] = {
		{ 800, 450 }, { 888, 500 }, { 1024, 576 }, { 1280, 720 }, { 1366, 768 },
		{ 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3440, 1440 }, { 3840, 2160 },
		{ 1080, 1920 },  // a screen turned on its side
		{ 640, 360 }, { 400, 300 },  // smaller than we support, and still legible
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMadHudLayoutScaleTest, "MadFall.UI.LayoutScale",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadHudLayoutScaleTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::Hud;

	TestEqual(TEXT("the design size is scale 1"), GetScale(1600.0f, 900.0f), 1.0f);
	TestEqual(TEXT("twice the design size is clamped to the maximum"), GetScale(3200.0f, 1800.0f), MaxScale);
	TestEqual(TEXT("a tiny window is clamped to the minimum"), GetScale(320.0f, 200.0f), MinScale);
	TestEqual(TEXT("a wide but short window scales by its height"), GetScale(3440.0f, 1440.0f), 1440.0f / 900.0f);
	TestEqual(TEXT("a tall but narrow window scales by its width"), GetScale(1080.0f, 1920.0f), 1080.0f / 1600.0f);
	TestEqual(TEXT("the player's multiplier multiplies"), GetScale(1600.0f, 900.0f, 1.5f), 1.5f);
	TestEqual(TEXT("and is clamped too"), GetScale(1600.0f, 900.0f, 9.0f), 2.0f);

	// Bigger window, never smaller HUD.
	float Last = 0.0f;
	for (int32 Height = 300; Height <= 2160; Height += 60)
	{
		const float Scale = GetScale(Height * 16.0f / 9.0f, static_cast<float>(Height));
		TestTrue(TEXT("the scale never shrinks as the window grows"), Scale >= Last - KINDA_SMALL_NUMBER);
		Last = Scale;
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMadHudLayoutTest, "MadFall.UI.Layout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadHudLayoutTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::Hud;

	for (const FIntPoint& Size : MadHudLayoutTests::Sizes)
	{
		for (const float UserScale : { 0.5f, 1.0f, 2.0f })
		{
			// The journal grows with the quests; three lines and a full six.
			for (const int32 Lines : { 1, 6, 12 })
			{
				const FMadHudLayout Layout = Build(static_cast<float>(Size.X), static_cast<float>(Size.Y), UserScale, 9, Lines);
				FString Problem;
				if (!TestTrue(FString::Printf(TEXT("%dx%d at x%.1f, %d journal lines: %s"), Size.X, Size.Y, UserScale, Lines, *Problem),
					Validate(Layout, Problem)))
				{
					AddError(Problem);
				}
			}
		}
	}

	// The hotbar is the anchor: centred, on screen, and wide enough to click.
	for (const FIntPoint& Size : MadHudLayoutTests::Sizes)
	{
		const FMadHudLayout Layout = Build(static_cast<float>(Size.X), static_cast<float>(Size.Y));
		const FVector2D Centre = Layout.Hotbar.GetCenter();
		TestTrue(TEXT("the hotbar is centred"), FMath::IsNearlyEqual(Centre.X, Layout.Width * 0.5, 1.0));
		TestTrue(TEXT("the hotbar's slots are big enough to hit"), Layout.Hotbar.GetSize().X / 9.0 >= 24.0);
		TestTrue(TEXT("the hotbar is above the bottom edge"), Layout.Hotbar.Max.Y < Layout.Height);
		TestTrue(TEXT("the map is clear of the hotbar"), Layout.Map.Max.Y <= Layout.Hotbar.Min.Y + 0.5);
		TestTrue(TEXT("and of the compass"), Layout.Map.Min.Y >= Layout.Compass.Max.Y - 0.5);
	}

	// A window with room honours the player's multiplier exactly; one without
	// it backs the multiplier off rather than stacking panels on each other.
	TestEqual(TEXT("1920x1080 at x1.5 is exactly that"), Build(1920.0f, 1080.0f, 1.5f).Scale, GetScale(1920.0f, 1080.0f, 1.5f));
	const FMadHudLayout Cramped = Build(640.0f, 360.0f, 2.0f);
	TestTrue(TEXT("a small window at x2 backs off"), Cramped.Scale < GetScale(640.0f, 360.0f, 2.0f) - KINDA_SMALL_NUMBER);
	FString CrampedProblem;
	TestTrue(TEXT("and lands somewhere valid"), Validate(Cramped, CrampedProblem));

	// The backing off is real work, not a no-op: at that size the asked-for
	// scale really does overlap, which is what Validate is there to catch.
	FString Asked;
	TestFalse(TEXT("640x360 at x2 does not fit"), Validate(BuildAt(640.0f, 360.0f, 2.0f), Asked));

	// A 1600x900 screen keeps the sizes the HUD was drawn with, so the change of
	// layout is not a change of look at the size it was authored for.
	const FMadHudLayout Reference = Build(1600.0f, 900.0f);
	TestEqual(TEXT("design scale"), Reference.Scale, 1.0f);
	TestEqual(TEXT("design hotbar height"), Reference.Hotbar.GetSize().Y, 64.0);
	TestEqual(TEXT("design vitals width"), Reference.Vitals.GetSize().X, 236.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMadInventoryLayoutTest, "MadFall.UI.InventoryLayout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadInventoryLayoutTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::Hud;

	constexpr float SideWidth = 380.0f;
	for (const FIntPoint& Size : MadHudLayoutTests::Sizes)
	{
		// A bag alone, then a bag with a container, then a trader's shelves too.
		for (const int32 Rows : { 5, 9, 14 })
		{
			const FMadInventoryLayout Layout = BuildInventory(static_cast<float>(Size.X), static_cast<float>(Size.Y), 1.0f,
				Rows, 200.0f, SideWidth);
			const FString Where = FString::Printf(TEXT("%dx%d, %d rows"), Size.X, Size.Y, Rows);
			TestTrue(*(Where + TEXT(": the panel fits the height")), Layout.Panel.Max.Y <= Size.Y + 0.5f);
			TestTrue(*(Where + TEXT(": the panel starts on screen")), Layout.Panel.Min.X >= -0.5f && Layout.Panel.Min.Y >= -0.5f);
			TestTrue(*(Where + TEXT(": the panel fits the width")), Layout.Panel.Max.X <= Size.X + 0.5f);
			TestTrue(*(Where + TEXT(": the side column fits the width")), Layout.Side.Max.X <= Size.X + 0.5f);
			TestTrue(*(Where + TEXT(": slots stay clickable")), Layout.SlotSize >= 15.9f);
			if (Layout.bSideBySide)
			{
				TestTrue(*(Where + TEXT(": side by side means no overlap")), Layout.Side.Min.X >= Layout.Panel.Max.X - 0.5f);
			}
		}
	}

	// Wide screens put the crafting column beside the bag. Because the screen
	// scales down with the window, that is now almost always possible: only a
	// window narrower than the smallest scale's two panels overlays them.
	TestTrue(TEXT("1920x1080 fits both"), BuildInventory(1920.0f, 1080.0f, 1.0f, 9, 200.0f, SideWidth).bSideBySide);
	TestTrue(TEXT("800x450 fits both once it scales"), BuildInventory(800.0f, 450.0f, 1.0f, 9, 200.0f, SideWidth).bSideBySide);
	TestFalse(TEXT("a letterbox 200x1000 window cannot"), BuildInventory(200.0f, 1000.0f, 1.0f, 9, 200.0f, SideWidth).bSideBySide);

	// Rows the window cannot take at HUD scale shrink the screen, they do not
	// run off the bottom: a trader's shelves under a full bag.
	const FMadInventoryLayout Tall = BuildInventory(1280.0f, 720.0f, 1.0f, 14, 260.0f, SideWidth);
	TestTrue(TEXT("a tall screen shrinks to fit"), Tall.Scale < GetScale(1280.0f, 720.0f));
	TestTrue(TEXT("and still fits"), Tall.Panel.Max.Y <= 720.5f);
	return true;
}

#endif
