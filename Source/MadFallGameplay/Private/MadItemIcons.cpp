// Copyright MadFall. All Rights Reserved.

#include "MadItemIcons.h"

#include "Engine/Texture2D.h"
#include "MadBlockRegistry.h"
#include "MadGameplayDefinitions.h"
#include "MadSurfaceRegistry.h"
#include "MadVoxelWorldSubsystem.h"
#include "Misc/App.h"

namespace
{
	using MadFall::Icons::EShape;
	constexpr int32 IconSize = MadFall::Icons::Size;

	const FColor Clear(0, 0, 0, 0);
	const FColor Wood(139, 94, 52);
	const FColor DarkWood(92, 60, 32);
	const FColor Iron(158, 163, 168);
	const FColor Stone(128, 124, 116);
	const FColor Leather(128, 84, 48);
	const FColor Outline(24, 19, 15);

	FColor Scale(const FColor& C, float S)
	{
		return FColor(static_cast<uint8>(FMath::Clamp(C.R * S, 0.0f, 255.0f)), static_cast<uint8>(FMath::Clamp(C.G * S, 0.0f, 255.0f)),
			static_cast<uint8>(FMath::Clamp(C.B * S, 0.0f, 255.0f)), C.A);
	}

	/** A stable 0..1 hash per pixel, so textures do not shimmer between paints. */
	float Noise(int32 X, int32 Y, uint32 Seed)
	{
		uint32 H = HashCombine(HashCombine(GetTypeHash(X), GetTypeHash(Y)), Seed);
		H ^= H >> 13;
		H *= 0x5bd1e995;
		H ^= H >> 15;
		return (H & 0xFFFF) / 65535.0f;
	}

	/** A 32x32 sRGB pixel painter. Coordinates are pixels, y down; everything clips. */
	struct FPainter
	{
		TArray<FColor>& Pixels;

		void Set(int32 X, int32 Y, const FColor& C)
		{
			if (X >= 0 && Y >= 0 && X < IconSize && Y < IconSize)
			{
				Pixels[Y * IconSize + X] = C;
			}
		}

		void Rect(int32 X0, int32 Y0, int32 X1, int32 Y1, const FColor& C)
		{
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					Set(X, Y, C);
				}
			}
		}

		void Ellipse(float CX, float CY, float RX, float RY, const FColor& C)
		{
			for (int32 Y = 0; Y < IconSize; ++Y)
			{
				for (int32 X = 0; X < IconSize; ++X)
				{
					const float DX = (X + 0.5f - CX) / RX;
					const float DY = (Y + 0.5f - CY) / RY;
					if (DX * DX + DY * DY <= 1.0f)
					{
						Set(X, Y, C);
					}
				}
			}
		}

		void Line(float X0, float Y0, float X1, float Y1, float Thickness, const FColor& C)
		{
			const FVector2f A(X0, Y0);
			const FVector2f B(X1, Y1);
			const FVector2f AB = B - A;
			const float LengthSquared = FMath::Max(AB.SizeSquared(), 1.0e-4f);
			for (int32 Y = 0; Y < IconSize; ++Y)
			{
				for (int32 X = 0; X < IconSize; ++X)
				{
					const FVector2f P(X + 0.5f, Y + 0.5f);
					const float T = FMath::Clamp(FVector2f::DotProduct(P - A, AB) / LengthSquared, 0.0f, 1.0f);
					if (FVector2f::Distance(P, A + AB * T) <= Thickness * 0.5f + 0.25f)
					{
						Set(X, Y, C);
					}
				}
			}
		}

		/** A convex polygon, vertices in either winding. */
		void Poly(std::initializer_list<FVector2f> Points, const FColor& C)
		{
			const TArray<FVector2f> V(Points);
			for (int32 Y = 0; Y < IconSize; ++Y)
			{
				for (int32 X = 0; X < IconSize; ++X)
				{
					if (Inside(V, FVector2f(X + 0.5f, Y + 0.5f)))
					{
						Set(X, Y, C);
					}
				}
			}
		}

		static bool Inside(const TArray<FVector2f>& V, const FVector2f& P)
		{
			bool bPositive = false;
			bool bNegative = false;
			for (int32 I = 0; I < V.Num(); ++I)
			{
				const FVector2f& A = V[I];
				const FVector2f& B = V[(I + 1) % V.Num()];
				const float Cross = (B.X - A.X) * (P.Y - A.Y) - (B.Y - A.Y) * (P.X - A.X);
				bPositive |= Cross > 0.0f;
				bNegative |= Cross < 0.0f;
			}
			return !(bPositive && bNegative);
		}

		/** Varies each opaque pixel's brightness a little: pixel art, not flat fills. */
		void Grain(uint32 Seed, float Amount)
		{
			for (int32 Y = 0; Y < IconSize; ++Y)
			{
				for (int32 X = 0; X < IconSize; ++X)
				{
					FColor& C = Pixels[Y * IconSize + X];
					if (C.A > 0)
					{
						C = Scale(C, 1.0f - Amount * 0.5f + Amount * Noise(X, Y, Seed));
					}
				}
			}
		}

		/** A dark one-pixel outline around everything opaque, the pixel-art silhouette. */
		void Outline()
		{
			const TArray<FColor> Source = Pixels;
			for (int32 Y = 0; Y < IconSize; ++Y)
			{
				for (int32 X = 0; X < IconSize; ++X)
				{
					if (Source[Y * IconSize + X].A > 0)
					{
						continue;
					}
					auto Opaque = [&Source](int32 NX, int32 NY)
					{
						return NX >= 0 && NY >= 0 && NX < IconSize && NY < IconSize && Source[NY * IconSize + NX].A > 0;
					};
					if (Opaque(X - 1, Y) || Opaque(X + 1, Y) || Opaque(X, Y - 1) || Opaque(X, Y + 1))
					{
						Pixels[Y * IconSize + X] = ::Outline;
					}
				}
			}
		}
	};

	bool IdHas(const FMadItemDefinition& Item, const TCHAR* Part)
	{
		return Item.Id.ToString().Contains(Part);
	}

	/** An isometric cube in a surface's colour, with a hint of its pattern. */
	void PaintBlock(FPainter& P, const FMadItemDefinition& Item)
	{
		FLinearColor Surface(0.35f, 0.35f, 0.35f);
		int32 Pattern = 0;
		const FMadBlockDefView View = UMadVoxelWorldSubsystem::GetBlockRegistry().GetBlockViewById(Item.PlacesBlock);
		if (const FMadSurfaceDefinition* Found = MadFall::GetSurfaces().Find(View.MaterialClass))
		{
			Surface = Found->Color;
			Pattern = Found->Pattern;
		}
		const FColor Base = Surface.ToFColor(/*bSRGB*/ true);
		const uint32 Seed = GetTypeHash(Item.Id);

		const TArray<FVector2f> Top = { { 16.0f, 3.0f }, { 29.0f, 9.5f }, { 16.0f, 16.0f }, { 3.0f, 9.5f } };
		const TArray<FVector2f> Left = { { 3.0f, 9.5f }, { 16.0f, 16.0f }, { 16.0f, 29.0f }, { 3.0f, 22.5f } };
		const TArray<FVector2f> Right = { { 16.0f, 16.0f }, { 29.0f, 9.5f }, { 29.0f, 22.5f }, { 16.0f, 29.0f } };

		for (int32 Y = 0; Y < IconSize; ++Y)
		{
			for (int32 X = 0; X < IconSize; ++X)
			{
				const FVector2f Point(X + 0.5f, Y + 0.5f);
				const bool bTop = FPainter::Inside(Top, Point);
				const bool bLeft = !bTop && FPainter::Inside(Left, Point);
				const bool bRight = !bTop && !bLeft && FPainter::Inside(Right, Point);
				if (!bTop && !bLeft && !bRight)
				{
					continue;
				}
				// Distance down a side face from its top edge, for stripes and fringes.
				const float Down = bLeft ? Point.Y - (9.5f + (Point.X - 3.0f) * 0.5f) : Point.Y - (16.0f - (Point.X - 16.0f) * 0.5f);
				FColor C = Base;
				float Shade = bTop ? 1.0f : (bLeft ? 0.78f : 0.6f);
				const float N = Noise(X, Y, Seed);
				Shade *= 0.88f + 0.24f * N;
				if (Pattern == 3 && !bTop && Down > 2.5f)   // grass: dirt under a green fringe
				{
					C = FLinearColor(Surface.R * 1.55f, Surface.G * 0.78f, Surface.B * 0.62f).ToFColor(true);
				}
				else if ((Pattern == 5 || Pattern == 6) && !bTop && FMath::Fmod(Down, 4.0f) < 1.0f)   // planks, bark
				{
					Shade *= 0.7f;
				}
				else if (Pattern == 9 && !bTop && (FMath::Fmod(Down, 4.0f) < 1.0f || FMath::Fmod(Point.X + (FMath::Fmod(Down, 8.0f) < 4.0f ? 0.0f : 3.0f), 6.0f) < 1.0f))
				{
					C = FColor(170, 165, 155);   // brick mortar
				}
				else if (Pattern == 7 && N < 0.18f)   // leaves
				{
					Shade *= 0.55f;
				}
				else if ((Pattern == 1 || Pattern == 11 || Pattern == 15) && N > 0.9f)   // stone, ore, gravel
				{
					Shade *= Pattern == 11 ? 1.5f : 0.7f;
				}
				P.Set(X, Y, Scale(C, Shade));
			}
		}
	}

	FColor ToolHead(const FMadItemDefinition& Item)
	{
		return IdHas(Item, TEXT("stone")) ? Stone : Iron;
	}

	void Handle(FPainter& P)
	{
		P.Line(6.0f, 27.0f, 21.0f, 12.0f, 2.2f, Wood);
		P.Line(6.0f, 27.0f, 9.0f, 24.0f, 2.2f, DarkWood);
	}

	void PaintShape(FPainter& P, EShape Shape, const FMadItemDefinition& Item)
	{
		const uint32 Seed = GetTypeHash(Item.Id);
		switch (Shape)
		{
		case EShape::Block:
			PaintBlock(P, Item);
			return;
		case EShape::Pickaxe:
			Handle(P);
			P.Line(11.0f, 6.0f, 20.0f, 10.0f, 2.5f, ToolHead(Item));
			P.Line(20.0f, 10.0f, 26.0f, 18.0f, 2.5f, ToolHead(Item));
			P.Line(9.0f, 6.0f, 12.0f, 6.0f, 1.5f, Scale(ToolHead(Item), 1.2f));
			break;
		case EShape::Axe:
			Handle(P);
			P.Poly({ { 18.0f, 8.0f }, { 25.0f, 3.0f }, { 29.0f, 11.0f }, { 23.0f, 15.0f } }, ToolHead(Item));
			P.Line(25.0f, 3.5f, 29.0f, 11.0f, 1.2f, Scale(ToolHead(Item), 1.25f));
			break;
		case EShape::Shovel:
			Handle(P);
			P.Ellipse(24.5f, 8.5f, 4.5f, 5.5f, ToolHead(Item));
			break;
		case EShape::Hoe:
			Handle(P);
			P.Poly({ { 19.0f, 10.0f }, { 22.0f, 5.0f }, { 28.0f, 7.0f }, { 25.0f, 11.0f } }, ToolHead(Item));
			break;
		case EShape::Club:
			P.Line(6.0f, 27.0f, 15.0f, 18.0f, 2.2f, DarkWood);
			P.Line(14.0f, 19.0f, 25.0f, 8.0f, 5.5f, Wood);
			P.Set(19, 11, Iron);
			P.Set(22, 12, Iron);
			P.Set(21, 8, Iron);
			break;
		case EShape::Spear:
			P.Line(4.0f, 29.0f, 23.0f, 10.0f, 1.8f, Wood);
			P.Poly({ { 21.0f, 9.0f }, { 29.0f, 3.0f }, { 24.0f, 12.0f } }, Iron);
			break;
		case EShape::Bow:
			for (int32 Step = 0; Step <= 24; ++Step)
			{
				const float T = Step / 24.0f;
				P.Line(10.0f + 12.0f * FMath::Sin(T * UE_PI), 3.0f + 26.0f * T, 10.0f + 12.0f * FMath::Sin((T + 0.04f) * UE_PI), 3.0f + 26.0f * (T + 0.04f), 2.2f, Wood);
			}
			P.Line(10.5f, 3.0f, 10.5f, 29.0f, 0.8f, FColor(220, 214, 196));
			break;
		case EShape::Arrow:
			P.Line(6.0f, 26.0f, 24.0f, 8.0f, 1.5f, FColor(190, 150, 100));
			P.Poly({ { 22.0f, 6.0f }, { 29.0f, 3.0f }, { 26.0f, 10.0f } }, Iron);
			P.Rect(4, 23, 7, 26, FColor(190, 50, 40));
			break;
		case EShape::Can:
			P.Rect(10, 7, 22, 26, FColor(176, 181, 186));
			P.Rect(10, 12, 22, 20, FColor(172, 46, 36));
			P.Rect(11, 7, 21, 8, FColor(210, 214, 218));
			break;
		case EShape::Meat:
		{
			const bool bCooked = IdHas(Item, TEXT("cooked"));
			P.Line(22.0f, 10.0f, 27.0f, 5.0f, 2.5f, FColor(230, 225, 210));
			P.Ellipse(27.0f, 4.5f, 2.0f, 2.0f, FColor(230, 225, 210));
			P.Ellipse(14.0f, 18.0f, 10.5f, 8.0f, bCooked ? FColor(128, 72, 36) : FColor(172, 48, 44));
			P.Ellipse(13.0f, 17.0f, 6.0f, 4.0f, bCooked ? FColor(164, 102, 52) : FColor(212, 104, 98));
			break;
		}
		case EShape::Berries:
			P.Ellipse(15.0f, 9.0f, 4.0f, 2.5f, FColor(70, 130, 50));
			for (const FVector2f& Berry : { FVector2f(11.0f, 17.0f), FVector2f(18.0f, 15.0f), FVector2f(15.0f, 22.0f), FVector2f(21.0f, 21.0f), FVector2f(9.0f, 23.0f) })
			{
				P.Ellipse(Berry.X, Berry.Y, 3.4f, 3.4f, FColor(168, 30, 50));
				P.Set(static_cast<int32>(Berry.X) - 1, static_cast<int32>(Berry.Y) - 1, FColor(230, 120, 140));
			}
			break;
		case EShape::Potato:
			P.Ellipse(16.0f, 17.0f, 11.0f, 7.5f, IdHas(Item, TEXT("baked")) ? FColor(140, 92, 48) : FColor(176, 136, 84));
			P.Set(11, 15, DarkWood);
			P.Set(18, 19, DarkWood);
			P.Set(21, 14, DarkWood);
			break;
		case EShape::Corn:
		{
			const bool bGrilled = IdHas(Item, TEXT("grilled"));
			P.Ellipse(16.0f, 15.0f, 5.5f, 11.0f, bGrilled ? FColor(196, 146, 54) : FColor(232, 196, 64));
			P.Poly({ { 10.0f, 18.0f }, { 16.0f, 29.0f }, { 12.0f, 29.0f }, { 8.0f, 22.0f } }, FColor(90, 140, 60));
			P.Poly({ { 22.0f, 18.0f }, { 16.0f, 29.0f }, { 20.0f, 29.0f }, { 24.0f, 22.0f } }, FColor(80, 128, 52));
			break;
		}
		case EShape::Food:
			P.Ellipse(16.0f, 17.0f, 10.0f, 8.0f, FLinearColor::MakeFromHSV8(static_cast<uint8>(Seed), 170, 200).ToFColor(true));
			break;
		case EShape::Bottle:
		{
			FColor Liquid(70, 134, 204);
			if (IdHas(Item, TEXT("murky"))) { Liquid = FColor(112, 116, 62); }
			if (IdHas(Item, TEXT("empty"))) { Liquid = FColor(196, 214, 222); }
			P.Rect(10, 12, 21, 27, Liquid);
			P.Rect(11, 13, 12, 25, Scale(Liquid, 1.3f));
			P.Rect(13, 6, 18, 11, FColor(196, 214, 222));
			P.Rect(13, 3, 18, 5, FColor(90, 90, 96));
			break;
		}
		case EShape::Medical:
			if (IdHas(Item, TEXT("antibiotic")))
			{
				P.Rect(10, 10, 22, 27, FColor(222, 132, 40));
				P.Rect(9, 5, 23, 9, FColor(236, 236, 230));
				P.Rect(12, 15, 20, 21, FColor(236, 236, 230));
			}
			else
			{
				P.Rect(6, 9, 26, 24, FColor(232, 230, 222));
				P.Rect(14, 11, 18, 22, FColor(196, 36, 36));
				P.Rect(9, 15, 23, 18, FColor(196, 36, 36));
			}
			break;
		case EShape::Seeds:
			P.Ellipse(16.0f, 19.0f, 10.0f, 9.0f, FColor(176, 146, 96));
			P.Rect(13, 8, 19, 11, FColor(150, 120, 76));
			P.Ellipse(13.0f, 19.0f, 1.8f, 2.5f, FColor(236, 206, 110));
			P.Ellipse(19.0f, 20.0f, 1.8f, 2.5f, FColor(236, 206, 110));
			P.Ellipse(16.0f, 24.0f, 1.8f, 2.5f, FColor(236, 206, 110));
			break;
		case EShape::Hat:
		{
			const bool bHelmet = IdHas(Item, TEXT("helmet"));
			const FColor C = bHelmet ? Iron : (IdHas(Item, TEXT("straw")) ? FColor(214, 186, 106) : FColor(150, 116, 78));
			if (bHelmet)
			{
				P.Ellipse(16.0f, 19.0f, 11.0f, 11.0f, C);
				P.Rect(0, 19, 31, 31, Clear);
				P.Rect(4, 19, 28, 21, Scale(C, 0.8f));
			}
			else
			{
				P.Ellipse(16.0f, 21.0f, 13.0f, 4.0f, Scale(C, 0.85f));
				P.Rect(10, 10, 22, 20, C);
			}
			break;
		}
		case EShape::Shirt:
		{
			const FColor C = IdHas(Item, TEXT("scrap")) ? Iron : (IdHas(Item, TEXT("fur")) ? FColor(152, 114, 74) : Leather);
			P.Rect(9, 8, 23, 27, C);
			P.Poly({ { 9.0f, 8.0f }, { 3.0f, 14.0f }, { 6.0f, 17.0f }, { 9.0f, 14.0f } }, C);
			P.Poly({ { 23.0f, 8.0f }, { 29.0f, 14.0f }, { 26.0f, 17.0f }, { 23.0f, 14.0f } }, C);
			P.Rect(14, 8, 18, 10, Clear);
			break;
		}
		case EShape::Trousers:
			P.Rect(9, 5, 23, 11, Leather);
			P.Rect(9, 11, 15, 28, Leather);
			P.Rect(17, 11, 23, 28, Leather);
			P.Rect(9, 5, 23, 6, DarkWood);
			break;
		case EShape::Boots:
			P.Rect(6, 7, 12, 22, Leather);
			P.Rect(6, 20, 15, 25, Leather);
			P.Rect(18, 9, 24, 24, Scale(Leather, 0.85f));
			P.Rect(18, 22, 27, 27, Scale(Leather, 0.85f));
			break;
		case EShape::Coin:
			P.Ellipse(16.0f, 16.0f, 11.0f, 11.0f, FColor(206, 160, 44));
			P.Ellipse(16.0f, 16.0f, 7.5f, 7.5f, FColor(238, 196, 76));
			P.Rect(13, 10, 14, 13, FColor(255, 240, 180));
			break;
		case EShape::Gear:
			for (int32 Tooth = 0; Tooth < 8; ++Tooth)
			{
				const float A = Tooth * UE_TWO_PI / 8.0f;
				P.Ellipse(16.0f + 11.0f * FMath::Cos(A), 16.0f + 11.0f * FMath::Sin(A), 2.8f, 2.8f, FColor(120, 150, 180));
			}
			P.Ellipse(16.0f, 16.0f, 10.0f, 10.0f, FColor(120, 150, 180));
			P.Ellipse(16.0f, 16.0f, 3.5f, 3.5f, Clear);
			break;
		case EShape::Plank:
			for (int32 Row = 0; Row < 3; ++Row)
			{
				const int32 Y = 7 + Row * 7;
				P.Rect(4 + Row, Y, 27 - Row, Y + 4, Scale(Wood, 1.0f - Row * 0.08f));
				P.Rect(4 + Row, Y + 4, 27 - Row, Y + 4, DarkWood);
			}
			break;
		case EShape::Rock:
			if (IdHas(Item, TEXT("cement")))
			{
				P.Rect(7, 8, 25, 27, FColor(186, 180, 166));
				P.Rect(7, 8, 25, 10, FColor(150, 144, 130));
			}
			else
			{
				P.Ellipse(16.0f, 19.0f, 12.0f, 8.5f, Stone);
				P.Ellipse(13.0f, 16.0f, 6.0f, 3.5f, Scale(Stone, 1.2f));
			}
			break;
		case EShape::Scrap:
			P.Poly({ { 5.0f, 20.0f }, { 11.0f, 7.0f }, { 23.0f, 9.0f }, { 28.0f, 19.0f }, { 19.0f, 27.0f }, { 8.0f, 26.0f } }, Iron);
			P.Rect(11, 12, 12, 13, FColor(96, 70, 52));
			P.Rect(20, 19, 21, 20, FColor(96, 70, 52));
			break;
		case EShape::Cloth:
			if (IdHas(Item, TEXT("fiber")))
			{
				for (int32 Strand = 0; Strand < 5; ++Strand)
				{
					P.Line(8.0f + Strand * 3.0f, 27.0f, 12.0f + Strand * 2.0f, 5.0f, 1.2f, FColor(110, 150, 70));
				}
				P.Rect(8, 15, 24, 17, FColor(170, 140, 80));
			}
			else if (IdHas(Item, TEXT("hide")))
			{
				P.Poly({ { 6.0f, 9.0f }, { 26.0f, 6.0f }, { 28.0f, 22.0f }, { 16.0f, 28.0f }, { 4.0f, 22.0f } }, FColor(150, 108, 70));
			}
			else
			{
				P.Rect(5, 10, 26, 24, FColor(206, 196, 172));
				P.Rect(5, 16, 26, 16, FColor(170, 160, 138));
			}
			break;
		case EShape::Lump:
		default:
			P.Ellipse(16.0f, 18.0f, 10.0f, 8.0f, FLinearColor::MakeFromHSV8(static_cast<uint8>(Seed), 90, 170).ToFColor(true));
			break;
		}
		P.Grain(Seed, 0.16f);
	}
}

EShape MadFall::Icons::ChooseShape(const FMadItemDefinition& Item)
{
	auto Tag = [&Item](const TCHAR* Name) { return Item.HasTag(FName(Name)); };

	if (Tag(TEXT("item.seed"))) { return EShape::Seeds; }
	if (Item.Kind == EMadItemKind::Block) { return EShape::Block; }
	if (Tag(TEXT("tool.pickaxe"))) { return EShape::Pickaxe; }
	if (Tag(TEXT("tool.axe"))) { return EShape::Axe; }
	if (Tag(TEXT("tool.shovel"))) { return EShape::Shovel; }
	if (Tag(TEXT("tool.hoe"))) { return EShape::Hoe; }
	if (Item.bHasTool && Item.Tool.IsRanged()) { return EShape::Bow; }
	if (Tag(TEXT("item.ammo"))) { return EShape::Arrow; }
	if (Item.bHasTool || Item.Kind == EMadItemKind::Tool || Item.Kind == EMadItemKind::Weapon)
	{
		const float* Pierce = Item.Tool.Damage.Find(FName(TEXT("madfall:pierce")));
		const float* Blunt = Item.Tool.Damage.Find(FName(TEXT("madfall:blunt")));
		return Pierce != nullptr && (Blunt == nullptr || *Pierce > *Blunt) ? EShape::Spear : EShape::Club;
	}
	if (Tag(TEXT("item.currency"))) { return EShape::Coin; }
	if (Item.Kind == EMadItemKind::Mod || Item.bHasMod) { return EShape::Gear; }
	if (Tag(TEXT("item.drink")) || !Item.FillsInto.IsNone() || IdHas(Item, TEXT("bottle")) || IdHas(Item, TEXT("water"))) { return EShape::Bottle; }
	if (Tag(TEXT("item.medical"))) { return EShape::Medical; }
	if (Tag(TEXT("item.food")) || Tag(TEXT("item.meat")) || Item.bHasConsumable)
	{
		if (IdHas(Item, TEXT("can"))) { return EShape::Can; }
		if (IdHas(Item, TEXT("meat"))) { return EShape::Meat; }
		if (IdHas(Item, TEXT("berr"))) { return EShape::Berries; }
		if (IdHas(Item, TEXT("potato"))) { return EShape::Potato; }
		if (IdHas(Item, TEXT("corn"))) { return EShape::Corn; }
		return EShape::Food;
	}
	if (Item.bHasWear)
	{
		switch (MadFall::Wear::GetSlotIndex(Item.Wear.Slot))
		{
		case 0: return EShape::Hat;
		case 1: return EShape::Shirt;
		case 2: return EShape::Trousers;
		case 3: return EShape::Boots;
		default: break;
		}
	}
	if (Tag(TEXT("item.wood"))) { return EShape::Plank; }
	if (Tag(TEXT("item.stone")) || IdHas(Item, TEXT("rock")) || IdHas(Item, TEXT("cement"))) { return EShape::Rock; }
	if (Tag(TEXT("item.metal"))) { return EShape::Scrap; }
	if (IdHas(Item, TEXT("cloth")) || IdHas(Item, TEXT("fiber")) || IdHas(Item, TEXT("hide"))) { return EShape::Cloth; }
	if (!Item.PlacesBlock.IsNone()) { return EShape::Block; }
	return EShape::Lump;
}

void MadFall::Icons::Paint(const FMadItemDefinition& Item, TArray<FColor>& OutPixels)
{
	OutPixels.Init(Clear, Size * Size);
	FPainter Painter{ OutPixels };
	PaintShape(Painter, ChooseShape(Item), Item);
	Painter.Outline();
}

UTexture2D* MadFall::IconCache::Get(FName ItemId)
{
	if (!FApp::CanEverRender() || ItemId.IsNone())
	{
		return nullptr;
	}
	// Rooted, for the session: a few dozen 32x32 textures, painted once each.
	static TMap<FName, UTexture2D*> Cache;
	if (UTexture2D** Found = Cache.Find(ItemId))
	{
		return *Found;
	}

	UTexture2D* Texture = nullptr;
	const FMadItemDefinition* Item = MadFall::GetGameplayDefinitions().FindItem(ItemId);
	if (Item != nullptr && Item->Icon.IsValid())
	{
		Texture = Cast<UTexture2D>(Item->Icon.TryLoad());
	}
	if (Texture == nullptr && Item != nullptr)
	{
		TArray<FColor> Pixels;
		MadFall::Icons::Paint(*Item, Pixels);
		Texture = UTexture2D::CreateTransient(MadFall::Icons::Size, MadFall::Icons::Size, PF_B8G8R8A8);
		if (Texture != nullptr)
		{
			Texture->Filter = TF_Nearest;   // pixel art stays crisp when the slot scales it up
			Texture->SRGB = true;
			uint8* Mip = static_cast<uint8*>(Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE));
			// FColor is BGRA in memory, which is PF_B8G8R8A8's layout.
			FMemory::Memcpy(Mip, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
			Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
			Texture->UpdateResource();
		}
	}
	if (Texture != nullptr)
	{
		Texture->AddToRoot();
	}
	Cache.Add(ItemId, Texture);
	return Texture;
}
