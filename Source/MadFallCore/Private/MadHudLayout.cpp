// Copyright MadFall. All Rights Reserved.

#include "MadHudLayout.h"

namespace
{
	FBox2D At(float X, float Y, float W, float H)
	{
		return FBox2D(FVector2D(X, Y), FVector2D(X + W, Y + H));
	}

	/** Overlap, treating boxes that merely touch as clear of each other. */
	bool Overlaps(const FBox2D& A, const FBox2D& B)
	{
		constexpr float Slack = 0.5f;
		return A.Min.X < B.Max.X - Slack && B.Min.X < A.Max.X - Slack
			&& A.Min.Y < B.Max.Y - Slack && B.Min.Y < A.Max.Y - Slack;
	}
}

float MadFall::Hud::GetScale(float ViewportWidth, float ViewportHeight, float UserScale)
{
	// The smaller ratio: a window that is wide but short must shrink the HUD as
	// much as a short one, or the hotbar eats the view.
	const float Fit = FMath::Min(ViewportWidth / FMadHudLayout::DesignWidth, ViewportHeight / FMadHudLayout::DesignHeight);
	// The lower bound on the multiplier is below the 0.5 a player can choose,
	// because Build() steps it down from their choice to find a size that fits.
	return FMath::Clamp(Fit, MinScale, MaxScale) * FMath::Clamp(UserScale, 0.3f, 2.0f);
}

FMadHudLayout MadFall::Hud::Build(float ViewportWidth, float ViewportHeight, float UserScale, int32 HotbarSlots, int32 JournalLines)
{
	// The player's multiplier is a wish, not a promise: a small window at x2
	// has no room for the panels at that size whatever order they are placed
	// in. Rather than let two of them share pixels, back the scale off until
	// the layout is valid - the same rule a window has when it cannot fit its
	// contents. Each step is 5%, so the largest that does fit is what is used.
	FString Problem;
	for (float Wish = FMath::Clamp(UserScale, 0.5f, 2.0f); Wish > 0.3f; Wish *= 0.95f)
	{
		const FMadHudLayout Candidate = BuildAt(ViewportWidth, ViewportHeight, Wish, HotbarSlots, JournalLines);
		if (Validate(Candidate, Problem))
		{
			return Candidate;
		}
	}
	return BuildAt(ViewportWidth, ViewportHeight, 0.3f, HotbarSlots, JournalLines);
}

FMadHudLayout MadFall::Hud::BuildAt(float ViewportWidth, float ViewportHeight, float UserScale, int32 HotbarSlots, int32 JournalLines)
{
	FMadHudLayout Layout;
	Layout.Width = FMath::Max(ViewportWidth, 1.0f);
	Layout.Height = FMath::Max(ViewportHeight, 1.0f);
	Layout.Scale = GetScale(Layout.Width, Layout.Height, UserScale);

	const float W = Layout.Width;
	const float H = Layout.Height;
	const float S = Layout.Scale;
	const float Margin = 14.0f * S;

	// --- the hotbar, the one thing that must always be reachable ---------------
	const float SlotSize = 64.0f * S;
	const float SlotGap = 4.0f * S;
	const float HotbarW = FMath::Min(HotbarSlots * (SlotSize + SlotGap) - SlotGap, W - 2.0f * Margin);
	const float HotbarH = SlotSize;
	Layout.Hotbar = At(W * 0.5f - HotbarW * 0.5f, H - HotbarH - 20.0f * S, HotbarW, HotbarH);

	// --- vitals, bottom left, lifted over the hotbar when they would meet ------
	const float VitalsW = 236.0f * S;
	const float VitalsH = 118.0f * S;
	float VitalsY = H - VitalsH - 20.0f * S;
	Layout.Vitals = At(Margin, VitalsY, VitalsW, VitalsH);
	if (Overlaps(Layout.Vitals, Layout.Hotbar))
	{
		// Above the hotbar rather than beside it: at the width where these meet
		// there is no room beside it, and a column of bars that jumps sideways
		// as the window is dragged reads as a glitch.
		VitalsY = Layout.Hotbar.Min.Y - 8.0f * S - VitalsH;
		Layout.Vitals = At(Margin, FMath::Max(VitalsY, Margin), VitalsW, VitalsH);
	}

	// --- top row: clock left, compass centred, journal right -------------------
	const float ClockW = 300.0f * S;
	const float ClockH = 56.0f * S;
	Layout.Clock = At(Margin, 14.0f * S, ClockW, ClockH);

	// The compass takes what the clock leaves on either side of centre, so it
	// shrinks instead of sliding under it; when there is nothing worth having it
	// drops to its own row below. (The clock is mirrored on the right because
	// the compass is centred: the space it may use is the narrower side twice.)
	const float CompassH = 44.0f * S;
	const float Free = W - 2.0f * (ClockW + 2.0f * Margin);
	const float CompassW = FMath::Min(520.0f * S, FMath::Max(Free, 0.0f));
	const float CompassY = CompassW >= 160.0f * S ? 14.0f * S : Layout.Clock.Max.Y + 6.0f * S;
	const float Width = CompassW >= 160.0f * S ? CompassW : FMath::Min(520.0f * S, W - 2.0f * Margin);
	Layout.Compass = At(W * 0.5f - Width * 0.5f, CompassY, Width, CompassH);

	// --- the crafting queue, bottom right --------------------------------------
	const float QueueW = FMath::Min(260.0f * S, W - 2.0f * Margin);
	const float QueueH = 96.0f * S;
	Layout.Queue = At(W - QueueW - Margin, Layout.Hotbar.Min.Y - 12.0f * S - QueueH, QueueW, QueueH);

	// --- the journal, down the right between the top row and the queue ---------
	// It is the one panel whose height is content, so it is the one that gives:
	// a short screen shows fewer quest lines rather than writing over the queue.
	const float JournalW = FMath::Min(326.0f * S, W - 2.0f * Margin);
	const float JournalTop = FMath::Max(Layout.Compass.Max.Y, Layout.Clock.Max.Y) + 6.0f * S;
	const float JournalRoom = FMath::Max(Layout.Queue.Min.Y - 6.0f * S - JournalTop, 28.0f * S);
	Layout.JournalLines = FMath::Clamp(FMath::FloorToInt32((JournalRoom - 28.0f * S) / (18.0f * S)), 0, FMath::Max(0, JournalLines));
	const float JournalH = FMath::Min(Layout.JournalLines * 18.0f * S + 28.0f * S, JournalRoom);
	Layout.Journal = At(W - JournalW - Margin, JournalTop, JournalW, JournalH);

	// --- messages, in the gap the vitals and the queue leave above the hotbar --
	const float MessagesH = 60.0f * S;
	const float MessagesY = Layout.Hotbar.Min.Y - 26.0f * S - MessagesH;
	auto SpansRow = [&](const FBox2D& Box, float Y) { return Box.Min.Y < Y + MessagesH && Y < Box.Max.Y; };
	float Left = SpansRow(Layout.Vitals, MessagesY) ? Layout.Vitals.Max.X + 8.0f * S : Margin;
	float Right = SpansRow(Layout.Queue, MessagesY) ? Layout.Queue.Min.X - 8.0f * S : W - Margin;
	if (Right - Left < 200.0f * S)
	{
		// Nothing worth reading fits between them: sit above both instead. A
		// pickup line squeezed to three words is worse than one a row higher.
		Left = Margin;
		Right = W - Margin;
		Layout.Messages = At(Left, FMath::Max(Margin, FMath::Min(Layout.Vitals.Min.Y, Layout.Queue.Min.Y) - 8.0f * S - MessagesH),
			Right - Left, MessagesH);
	}
	else
	{
		const float MessagesW = FMath::Min(560.0f * S, Right - Left);
		Layout.Messages = At((Left + Right) * 0.5f - MessagesW * 0.5f, MessagesY, MessagesW, MessagesH);
	}

	// --- the map fills what is left between the compass and the hotbar ---------
	const float MapTop = Layout.Compass.Max.Y + 24.0f * S;
	const float MapBottom = FMath::Min(Layout.Hotbar.Min.Y, Layout.Vitals.Min.Y) - 12.0f * S;
	const float MapSize = FMath::Max(FMath::Min(W * 0.95f, MapBottom - MapTop), 64.0f * S);
	Layout.Map = At(W * 0.5f - MapSize * 0.5f, MapTop, MapSize, MapSize);
	return Layout;
}

bool MadFall::Hud::Validate(const FMadHudLayout& Layout, FString& OutProblem)
{
	struct FNamed
	{
		const TCHAR* Name;
		const FBox2D& Box;
	};
	const FNamed Boxes[] = {
		{ TEXT("clock"), Layout.Clock },
		{ TEXT("compass"), Layout.Compass },
		{ TEXT("journal"), Layout.Journal },
		{ TEXT("vitals"), Layout.Vitals },
		{ TEXT("hotbar"), Layout.Hotbar },
		{ TEXT("messages"), Layout.Messages },
		{ TEXT("queue"), Layout.Queue },
	};

	for (const FNamed& Named : Boxes)
	{
		if (Named.Box.Min.X < -0.5 || Named.Box.Min.Y < -0.5
			|| Named.Box.Max.X > Layout.Width + 0.5 || Named.Box.Max.Y > Layout.Height + 0.5)
		{
			OutProblem = FString::Printf(TEXT("%s is off a %.0fx%.0f screen: (%.0f,%.0f)-(%.0f,%.0f)"), Named.Name,
				Layout.Width, Layout.Height, Named.Box.Min.X, Named.Box.Min.Y, Named.Box.Max.X, Named.Box.Max.Y);
			return false;
		}
	}

	for (int32 A = 0; A < UE_ARRAY_COUNT(Boxes); ++A)
	{
		for (int32 B = A + 1; B < UE_ARRAY_COUNT(Boxes); ++B)
		{
			if (Overlaps(Boxes[A].Box, Boxes[B].Box))
			{
				OutProblem = FString::Printf(TEXT("%s overlaps %s at %.0fx%.0f"), Boxes[A].Name, Boxes[B].Name, Layout.Width, Layout.Height);
				return false;
			}
		}
	}

	OutProblem.Reset();
	return true;
}

FMadInventoryLayout MadFall::Hud::BuildInventory(float ViewportWidth, float ViewportHeight, float UserScale,
	int32 Rows, float ExtraDesignHeight, float SideDesignWidth)
{
	FMadInventoryLayout Layout;
	const float W = FMath::Max(ViewportWidth, 1.0f);
	const float H = FMath::Max(ViewportHeight, 1.0f);
	float S = GetScale(W, H, UserScale);

	// The screen is one panel, so it may have to shrink past the HUD's scale to
	// fit its rows: a bag, a container and a trader's shelves stack up, and a
	// panel taller than the window would hide the slots at its bottom.
	const float DesignHeight = ExtraDesignHeight + Rows * 60.0f;
	const float Fits = (H - 32.0f) / FMath::Max(DesignHeight, 1.0f);
	// A floor of 16 px a slot: below that the icons are unreadable and the
	// slots are hard to hit, so a window too small for every row loses the
	// bottom of the panel instead of the whole screen becoming unusable.
	constexpr float SmallestSlot = 16.0f / 56.0f;
	S = FMath::Max(FMath::Min(S, Fits), SmallestSlot);

	Layout.Scale = S;
	Layout.SlotSize = 56.0f * S;
	Layout.Gap = 4.0f * S;

	const float GridW = Layout.Columns * (Layout.SlotSize + Layout.Gap) - Layout.Gap;
	const float PanelW = GridW + 32.0f * S;
	const float PanelH = FMath::Min(DesignHeight * S, H - 16.0f * S);
	const float SideW = SideDesignWidth * S;
	const float SideGap = 12.0f * S;

	Layout.bSideBySide = W >= PanelW + SideGap + SideW + 32.0f * S;
	const float PanelX = Layout.bSideBySide ? W * 0.5f - (PanelW + SideGap + SideW) * 0.5f : W * 0.5f - PanelW * 0.5f;
	const float PanelY = FMath::Max(8.0f * S, H * 0.5f - PanelH * 0.5f - 20.0f * S);
	Layout.Panel = At(PanelX, PanelY, PanelW, PanelH);

	const float SideX = Layout.bSideBySide ? PanelX + PanelW + SideGap : FMath::Max(0.0f, W - SideW - 16.0f * S);
	Layout.Side = At(SideX, PanelY, FMath::Min(SideW, W - SideX), PanelH);
	return Layout;
}
