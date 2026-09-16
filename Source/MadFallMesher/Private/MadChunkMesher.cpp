// Copyright MadFall. All Rights Reserved.

#include "MadChunkMesher.h"

#include "IMadBlockRegistry.h"
#include "MadChunkSampleGrid.h"
#include "MadFallMesher.h"
#include "MadFallStats.h"
#include "MadSurfaceRegistry.h"

DECLARE_CYCLE_STAT(TEXT("Mesh Isosurface"), STAT_MadMeshIsosurface, STATGROUP_MadFallMesher);
DECLARE_CYCLE_STAT(TEXT("Mesh Cubic"), STAT_MadMeshCubic, STATGROUP_MadFallMesher);

namespace MadFall::ChunkMesher
{
	namespace
	{
		constexpr int32 ChunkSize = MadFall::ChunkSize;
		constexpr float VoxelSize = MadFall::VoxelSizeUU;

		/** The density value the surface passes through. */
		constexpr float IsoLevel = 128.0f;

		/**
		 * Unreal renders a triangle as front-facing when its vertices appear
		 * CLOCKWISE from the front, because the engine is left-handed. Every
		 * quad below is built counter-clockwise by the right-hand rule, so the
		 * whole mesher flips in exactly one place rather than each caller
		 * getting it wrong independently.
		 */
		constexpr bool bFlipWindingForUnreal = true;

		void AddOrientedQuad(FMadMeshSection& Section, int32 V0, int32 V1, int32 V2, int32 V3, bool bReverse)
		{
			const bool bFinalReverse = bReverse != bFlipWindingForUnreal;
			if (bFinalReverse)
			{
				Section.AddQuad(V3, V2, V1, V0);
			}
			else
			{
				Section.AddQuad(V0, V1, V2, V3);
			}
		}

		/** The surface definition's colour for a class, or a stable placeholder (see FMadSurfaceRegistry). */
		FColor ColorForMaterialClass(FName MaterialClass)
		{
			// Called per vertex; a chunk is long runs of one class, so a one-entry
			// cache turns a map probe per vertex into a compare.
			thread_local FName CachedClass;
			thread_local FColor CachedColor;
			thread_local bool bCached = false;
			if (!bCached || CachedClass != MaterialClass)
			{
				CachedClass = MaterialClass;
				CachedColor = MadFall::GetSurfaces().GetVertexColor(MaterialClass);
				bCached = true;
			}
			return CachedColor;
		}

		/**
		 * Caches block id -> material class for the life of one meshing job.
		 *
		 * The registry lookup is a virtual call plus a hash probe. A chunk can
		 * ask for it a hundred thousand times; a chunk's palette is rarely more
		 * than a dozen entries.
		 */
		class FMaterialResolver
		{
		public:
			explicit FMaterialResolver(const IMadBlockRegistry& InRegistry) : Registry(InRegistry) {}

			FName GetMaterialClass(uint16 BlockId)
			{
				if (const FName* Found = Cache.Find(BlockId))
				{
					return *Found;
				}

				const FMadBlockDefView View = Registry.GetBlockView(BlockId);
				FName MaterialClass = View.MaterialClass;

				if (MaterialClass.IsNone())
				{
					// A block with no material class still has to draw. Falling
					// back to its own id keeps distinct blocks visually distinct
					// instead of collapsing them all into one grey section.
					MaterialClass = View.Id.IsNone() ? FName(TEXT("madfall:unknown")) : View.Id;
				}

				Cache.Add(BlockId, MaterialClass);
				return MaterialClass;
			}

			/**
			 * Model blocks are drawn as instanced meshes by UMadModelInstanceSubsystem:
			 * the chunk mesh gives them no faces, and they do not hide the faces of
			 * the blocks around them (a barrel does not fill its voxel).
			 */
			bool IsModel(uint16 BlockId)
			{
				if (BlockId == MadFall::BlockTypeAir)
				{
					return false;
				}
				// Flat, lazily filled table: this is asked per corner in the hottest
				// loop of the isosurface pass, where a map probe showed up.
				if (ModelFlags.Num() == 0)
				{
					ModelFlags.SetNumZeroed(MAX_uint16 + 1);
				}
				uint8& Flag = ModelFlags[BlockId];
				if (Flag == 0)
				{
					Flag = Registry.GetBlockView(BlockId).ShapeKind == EMadBlockShapeKind::Model ? 2 : 1;
				}
				return Flag == 2;
			}

		private:
			const IMadBlockRegistry& Registry;
			TMap<uint16, FName> Cache;
			/** 0 unknown, 1 not a model, 2 model. */
			TArray<uint8> ModelFlags;
		};

		/** Per-cell vertex data produced by the first Surface Nets pass. */
		struct FCellVertex
		{
			FVector3f Position = FVector3f::ZeroVector;
			FVector3f Normal = FVector3f::UpVector;
			FName MaterialClass;
			/** The worst damage of the solid corners around it, for the material's cracks. */
			uint8 Damage = 0;
			bool bValid = false;
		};

		// The 12 edges of a cell, as pairs of corner indices in the 0..7
		// corner numbering (bit 0 = X, bit 1 = Y, bit 2 = Z).
		constexpr int32 EdgeCorners[12][2] =
		{
			{0,1}, {2,3}, {4,5}, {6,7},   // along X
			{0,2}, {1,3}, {4,6}, {5,7},   // along Y
			{0,4}, {1,5}, {2,6}, {3,7}    // along Z
		};

		// ===================================================================
		// Surface Nets
		// ===================================================================

		/**
		 * Surface Nets over a lattice Points steps across horizontally, each step
		 * Stride voxels, and one voxel per step vertically: the full sample grid
		 * (32 steps of 1) or a distant chunk's coarse one (FMadLodSampleGrid).
		 *
		 * Cells run from -1 to Points - 1 so that a chunk can build the quads
		 * touching its own minimum face using its neighbour's margin samples.
		 * Without the -1 row every chunk would have a one-step gap along three of
		 * its faces.
		 */
		template <typename GridType>
		void BuildIsosurface(
			const GridType& Grid,
			int32 Points,
			int32 Stride,
			FMaterialResolver& Materials,
			FMadChunkMesh& OutMesh)
		{
			SCOPE_CYCLE_COUNTER(STAT_MadMeshIsosurface);

			const int32 CellMin = -1;
			const int32 CellMax = Points - 1;
			const int32 CellSpan = CellMax - CellMin + 1;
			const int32 CellMaxZ = ChunkSize - 1;
			const int32 CellSpanZ = CellMaxZ - CellMin + 1;
			auto CellIndex = [CellMin, CellSpan](int32 X, int32 Y, int32 Z)
			{
				return (X - CellMin) + CellSpan * ((Y - CellMin) + CellSpan * (Z - CellMin));
			};
			// A cell is Stride x Stride x 1 voxels.
			const FVector3f CellScale(static_cast<float>(Stride), static_cast<float>(Stride), 1.0f);

			TArray<FCellVertex> Cells;
			Cells.SetNum(CellSpan * CellSpan * CellSpanZ);

			// --- pass 1: one vertex per cell the surface passes through ---
			const bool bAnyDamage = Grid.HasDamage();
			for (int32 CellZ = CellMin; CellZ <= CellMaxZ; ++CellZ)
			{
				for (int32 CellY = CellMin; CellY <= CellMax; ++CellY)
				{
					for (int32 CellX = CellMin; CellX <= CellMax; ++CellX)
					{
						float Densities[8];
						bool bInside[8];
						uint16 BlockIds[8];
						int32 InsideCount = 0;
						uint8 CellDamage = 0;

						for (int32 Corner = 0; Corner < 8; ++Corner)
						{
							const int32 X = CellX + (Corner & 1);
							const int32 Y = CellY + ((Corner >> 1) & 1);
							const int32 Z = CellZ + ((Corner >> 2) & 1);

							// A cubic voxel is construction, not terrain. Treating
							// it as empty here is what stops a placed concrete
							// block from also bulging the isosurface around it.
							const bool bIsCubic = Grid.IsCubic(X, Y, Z) || Materials.IsModel(Grid.GetBlockId(X, Y, Z));

							Densities[Corner] = bIsCubic ? 0.0f : static_cast<float>(Grid.GetDensity(X, Y, Z));
							bInside[Corner] = Densities[Corner] >= IsoLevel;
							BlockIds[Corner] = Grid.GetBlockId(X, Y, Z);

							InsideCount += bInside[Corner] ? 1 : 0;
							if (bAnyDamage && bInside[Corner])
							{
								// The worst of the ground this vertex sits on: a
								// vertex is shared by up to eight voxels, and a
								// half-mined one should show its cracks rather
								// than have them averaged away by whole neighbours.
								CellDamage = FMath::Max(CellDamage, Grid.GetDamage(X, Y, Z));
							}
						}

						if (InsideCount == 0 || InsideCount == 8)
						{
							continue;
						}

						// Vertex position: the average of every edge crossing.
						FVector3f Sum = FVector3f::ZeroVector;
						int32 CrossingCount = 0;

						for (int32 Edge = 0; Edge < 12; ++Edge)
						{
							const int32 A = EdgeCorners[Edge][0];
							const int32 B = EdgeCorners[Edge][1];

							if (bInside[A] == bInside[B])
							{
								continue;
							}

							const float Delta = Densities[B] - Densities[A];
							const float T = (FMath::Abs(Delta) > KINDA_SMALL_NUMBER)
								? FMath::Clamp((IsoLevel - Densities[A]) / Delta, 0.0f, 1.0f)
								: 0.5f;

							const FVector3f PointA(
								static_cast<float>(A & 1),
								static_cast<float>((A >> 1) & 1),
								static_cast<float>((A >> 2) & 1));
							const FVector3f PointB(
								static_cast<float>(B & 1),
								static_cast<float>((B >> 1) & 1),
								static_cast<float>((B >> 2) & 1));

							Sum += PointA + (PointB - PointA) * T;
							++CrossingCount;
						}

						if (CrossingCount == 0)
						{
							continue;
						}

						const FVector3f Offset = Sum / static_cast<float>(CrossingCount);

						/**
						 * The normal comes from the density gradient across the
						 * cell's own 8 corners, NOT from the triangles around
						 * the vertex.
						 *
						 * That matters at chunk boundaries: a gradient computed
						 * from 8 samples is identical whichever chunk computes
						 * it, so two chunks agree on the normal at a shared
						 * seam. Averaging face normals instead would give each
						 * chunk a slightly different answer, and the seam would
						 * show up as a visible crease in every lighting setup.
						 */
						const float GradX =
							(Densities[1] + Densities[3] + Densities[5] + Densities[7])
							- (Densities[0] + Densities[2] + Densities[4] + Densities[6]);
						const float GradY =
							(Densities[2] + Densities[3] + Densities[6] + Densities[7])
							- (Densities[0] + Densities[1] + Densities[4] + Densities[5]);
						const float GradZ =
							(Densities[4] + Densities[5] + Densities[6] + Densities[7])
							- (Densities[0] + Densities[1] + Densities[2] + Densities[3]);

						// Per voxel, not per lattice step: a coarse cell is wider than
						// it is tall, and an unscaled gradient would tilt every slope's
						// normal towards the horizontal.
						FVector3f Normal(-GradX / CellScale.X, -GradY / CellScale.Y, -GradZ);
						if (!Normal.Normalize())
						{
							Normal = FVector3f::UpVector;
						}

						// Material: the densest solid corner wins. Using the
						// first solid corner instead would make the surface
						// flicker between materials as density changed.
						uint16 DominantBlock = MadFall::BlockTypeAir;
						float BestDensity = -1.0f;
						for (int32 Corner = 0; Corner < 8; ++Corner)
						{
							if (bInside[Corner] && Densities[Corner] > BestDensity)
							{
								BestDensity = Densities[Corner];
								DominantBlock = BlockIds[Corner];
							}
						}

						FCellVertex& Cell = Cells[CellIndex(CellX, CellY, CellZ)];
						// +0.5 because a voxel's density sample sits at the CENTRE
						// of its cube, while the cubic mesher treats the same
						// voxel as spanning [p, p+1]. Without this the smooth and
						// cubic geometry would be half a voxel out of register.
						// The half voxel is added after scaling by the stride:
						// a coarse lattice point is still one voxel's sample.
						Cell.Position = ((FVector3f(
							static_cast<float>(CellX),
							static_cast<float>(CellY),
							static_cast<float>(CellZ)) + Offset) * CellScale + FVector3f(0.5f)) * VoxelSize;

						// WHY THE EDGE ROWS ARE PULLED OUT: a chunk's surface ends at
						// its boundary cells' vertices, which sit anywhere inside
						// those cells. Two chunks at one level share those cells
						// and meet exactly; a full-detail chunk next to a coarse one
						// does not, and the ground showed pinholes along the seam.
						// Clamping a coarse chunk's outer rows half a voxel past its
						// boundary makes it overlap whatever finer surface it meets
						// (a full chunk's own surface ends within half a voxel of
						// the boundary). Only X and Y: a column of chunks is always
						// one level, so vertical neighbours match. The cost is up to
						// a coarse step of sideways shift at a distant seam, where
						// two surfaces overlapping read better than a hole.
						if (Stride > 1)
						{
							const int32 CellXY[2] = { CellX, CellY };
							for (int32 Axis = 0; Axis < 2; ++Axis)
							{
								if (CellXY[Axis] == CellMin)
								{
									Cell.Position[Axis] = FMath::Min(Cell.Position[Axis], -0.5f * VoxelSize);
								}
								else if (CellXY[Axis] == CellMax)
								{
									Cell.Position[Axis] = FMath::Max(Cell.Position[Axis], (ChunkSize + 0.5f) * VoxelSize);
								}
							}
						}
						Cell.Normal = Normal;
						Cell.MaterialClass = Materials.GetMaterialClass(DominantBlock);
						Cell.Damage = CellDamage;
						Cell.bValid = true;
					}
				}
			}

			// --- pass 2: one quad per sign-changing edge ---
			// Per-section vertex reuse: a cell vertex is emitted once per section
			// that references it, which is almost always exactly one.
			TMap<FName, TMap<int32, int32>> SectionVertexCache;

			auto EmitVertex = [&](FMadMeshSection& Section, TMap<int32, int32>& Cache, int32 Cell) -> int32
			{
				if (const int32* Found = Cache.Find(Cell))
				{
					return *Found;
				}

				const FCellVertex& Vertex = Cells[Cell];

				// Planar UVs in metres. The real material projects triplanar from
				// world position, so these are a fallback, not the plan.
				const FVector2f UV(Vertex.Position.X / VoxelSize, Vertex.Position.Y / VoxelSize);

				// Colour from the cell's own surface, so it still blends across a
				// boundary between two surfaces. The pattern index (alpha) from the
				// section's: it is an index, not a quantity, and interpolated across
				// a triangle from one surface's to another's it passes through every
				// index in between - a thin band of water, planks or metal pattern,
				// rendered as bright zig-zag lines, along every material boundary.
				FColor Color = ColorForMaterialClass(Vertex.MaterialClass);
				Color.A = ColorForMaterialClass(Section.MaterialClass).A;
				const int32 Index = Section.AddVertex(Vertex.Position, Vertex.Normal, UV, Color, /*Occlusion*/ 0, Vertex.Damage);

				Cache.Add(Cell, Index);
				return Index;
			};

			// Right-handed axis triples: the quad's winding depends on b x c = a.
			constexpr int32 AxisB[3] = { 1, 2, 0 };
			constexpr int32 AxisC[3] = { 2, 0, 1 };

			for (int32 Z = 0; Z < ChunkSize; ++Z)
			{
				for (int32 Y = 0; Y < Points; ++Y)
				{
					for (int32 X = 0; X < Points; ++X)
					{
						const int32 Position[3] = { X, Y, Z };

						const bool bInsideHere = !Grid.IsCubic(X, Y, Z) && Grid.IsSolid(X, Y, Z) && !Materials.IsModel(Grid.GetBlockId(X, Y, Z));

						for (int32 Axis = 0; Axis < 3; ++Axis)
						{
							int32 Next[3] = { X, Y, Z };
							Next[Axis] += 1;

							const bool bInsideNext =
								!Grid.IsCubic(Next[0], Next[1], Next[2]) && Grid.IsSolid(Next[0], Next[1], Next[2])
								&& !Materials.IsModel(Grid.GetBlockId(Next[0], Next[1], Next[2]));

							if (bInsideHere == bInsideNext)
							{
								continue;
							}

							const int32 B = AxisB[Axis];
							const int32 C = AxisC[Axis];

							// The four cells sharing this edge, walked
							// counter-clockwise in the (b, c) plane so the
							// right-hand rule gives a normal along +Axis.
							int32 Quad[4];
							bool bAllValid = true;
							FName SectionMaterial;

							static const int32 Offsets[4][2] = { {-1,-1}, {0,-1}, {0,0}, {-1,0} };

							for (int32 Corner = 0; Corner < 4; ++Corner)
							{
								int32 Cell[3] = { Position[0], Position[1], Position[2] };
								Cell[B] += Offsets[Corner][0];
								Cell[C] += Offsets[Corner][1];

								const int32 Index = CellIndex(Cell[0], Cell[1], Cell[2]);
								if (!Cells[Index].bValid)
								{
									bAllValid = false;
									break;
								}

								Quad[Corner] = Index;
								if (SectionMaterial.IsNone())
								{
									SectionMaterial = Cells[Index].MaterialClass;
								}
							}

							if (!bAllValid)
							{
								continue;
							}

							FMadMeshSection& Section = OutMesh.FindOrAddSection(SectionMaterial);
							TMap<int32, int32>& Cache = SectionVertexCache.FindOrAdd(SectionMaterial);

							const int32 V0 = EmitVertex(Section, Cache, Quad[0]);
							const int32 V1 = EmitVertex(Section, Cache, Quad[1]);
							const int32 V2 = EmitVertex(Section, Cache, Quad[2]);
							const int32 V3 = EmitVertex(Section, Cache, Quad[3]);

							// Outward normal points away from the solid side.
							AddOrientedQuad(Section, V0, V1, V2, V3, /*bReverse*/ bInsideNext);
						}
					}
				}
			}
		}

		// ===================================================================
		// Greedy cubic meshing
		// ===================================================================

		/**
		 * Corner ambient occlusion, the block-game way: each corner of a face is
		 * darkened by the blocks touching it in the layer in front of the face,
		 * the two beside the corner and the one diagonal to it. Two sides make
		 * a full corner whatever the diagonal holds.
		 *
		 * WHY baked per vertex and not left to Lumen: chunk meshes are
		 * procedural, so they have no mesh distance fields and Lumen's software
		 * tracing cannot see them beyond the screen. Without this, a block on a
		 * floor, an inside corner or a tunnel mouth has no contact shadow at
		 * all, which is what makes voxel scenes read as flat. It costs three
		 * sample lookups per corner of a visible face, on the worker thread, and
		 * some merging: faces merge only with faces of identical occlusion, and a
		 * face whose corners differ does not merge at all, because stretching
		 * one face's gradient across a merged rectangle would put the shadow in
		 * the wrong place.
		 *
		 * Packed 2 bits per corner, corners in quad order: (0,0), (1,0), (1,1), (0,1) in (B, C).
		 */
		uint8 ComputeFaceOcclusion(const FMadChunkSampleGrid& Grid, FMaterialResolver& Materials,
			const int32 Voxel[3], int32 Axis, int32 Step, int32 B, int32 C)
		{
			auto Occludes = [&Grid, &Materials](const int32 Sample[3])
			{
				return Grid.IsSolid(Sample[0], Sample[1], Sample[2]) && !Materials.IsModel(Grid.GetBlockId(Sample[0], Sample[1], Sample[2]));
			};

			static constexpr int32 CornerB[4] = { -1, 1, 1, -1 };
			static constexpr int32 CornerC[4] = { -1, -1, 1, 1 };

			uint8 Packed = 0;
			for (int32 Corner = 0; Corner < 4; ++Corner)
			{
				int32 Side1[3] = { Voxel[0], Voxel[1], Voxel[2] };
				Side1[Axis] += Step;
				int32 Side2[3] = { Side1[0], Side1[1], Side1[2] };
				Side1[B] += CornerB[Corner];
				Side2[C] += CornerC[Corner];
				int32 Diagonal[3] = { Side1[0], Side1[1], Side1[2] };
				Diagonal[C] += CornerC[Corner];

				const bool bSide1 = Occludes(Side1);
				const bool bSide2 = Occludes(Side2);
				const int32 Level = (bSide1 && bSide2) ? 3 : (int32(bSide1) + int32(bSide2) + int32(Occludes(Diagonal)));
				Packed |= static_cast<uint8>(Level << (Corner * 2));
			}
			return Packed;
		}

		FORCEINLINE uint8 CornerOcclusion(uint8 Packed, int32 Corner)
		{
			return (Packed >> (Corner * 2)) & 3;
		}

		/** A cubic face's vertex value: its corner occlusion, flagged as a placed block's face for the bevel. */
		FORCEINLINE uint8 CubicVertex(uint8 Packed, int32 Corner)
		{
			return CornerOcclusion(Packed, Corner) | FMadMeshSection::CubicFaceFlag;
		}

		FORCEINLINE bool IsUniformOcclusion(uint8 Packed)
		{
			return Packed == 0 || Packed == 0x55 || Packed == 0xAA || Packed == 0xFF;
		}

		void BuildCubic(
			const FMadChunkSampleGrid& Grid,
			FMaterialResolver& Materials,
			FMadChunkMesh& OutMesh)
		{
			SCOPE_CYCLE_COUNTER(STAT_MadMeshCubic);

			constexpr int32 AxisB[3] = { 1, 2, 0 };
			constexpr int32 AxisC[3] = { 2, 0, 1 };

			// Reused across all 192 slices so the mesher allocates once. Each entry is
			// the face's block id in the low 16 bits and its packed corner
			// occlusion above: faces merge only when both match.
			TArray<uint32> Mask;
			Mask.SetNumZeroed(ChunkSize * ChunkSize);

			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const int32 B = AxisB[Axis];
				const int32 C = AxisC[Axis];

				for (int32 Direction = 0; Direction < 2; ++Direction)
				{
					const int32 Step = (Direction == 0) ? 1 : -1;

					for (int32 Slice = 0; Slice < ChunkSize; ++Slice)
					{
						// --- build the visibility mask for this slice ---
						bool bAnyVisible = false;

						for (int32 CoordC = 0; CoordC < ChunkSize; ++CoordC)
						{
							for (int32 CoordB = 0; CoordB < ChunkSize; ++CoordB)
							{
								int32 Voxel[3];
								Voxel[Axis] = Slice;
								Voxel[B] = CoordB;
								Voxel[C] = CoordC;

								uint32 Value = 0;

								const bool bIsBlock = Grid.IsCubic(Voxel[0], Voxel[1], Voxel[2])
									&& Grid.IsSolid(Voxel[0], Voxel[1], Voxel[2])
									&& !Materials.IsModel(Grid.GetBlockId(Voxel[0], Voxel[1], Voxel[2]));

								if (bIsBlock)
								{
									int32 Neighbour[3] = { Voxel[0], Voxel[1], Voxel[2] };
									Neighbour[Axis] += Step;

									// The neighbour may be a margin sample, which
									// is exactly why the grid has one. Without it
									// every chunk would draw its own boundary
									// faces and the interior of a wall would be
									// full of hidden geometry.
									const bool bNeighbourSolid =
										Grid.IsSolid(Neighbour[0], Neighbour[1], Neighbour[2])
										&& !Materials.IsModel(Grid.GetBlockId(Neighbour[0], Neighbour[1], Neighbour[2]));

									if (!bNeighbourSolid)
									{
										// Damage joins the key, so a cracked block never
										// merges into a whole neighbour's rectangle and
										// wear its cracks stretched across both. It is
										// quantised to 16 steps first: merging is what
										// keeps a wall one quad, and a wall taking horde
										// hits would otherwise split into a quad a block
										// for differences no eye can see.
										const uint8 Damage = Grid.GetDamage(Voxel[0], Voxel[1], Voxel[2]);
										Value = Grid.GetBlockId(Voxel[0], Voxel[1], Voxel[2])
											| (static_cast<uint32>(ComputeFaceOcclusion(Grid, Materials, Voxel, Axis, Step, B, C)) << 16)
											| (static_cast<uint32>(Damage & 0xF0) << 20);
										bAnyVisible = true;
									}
								}

								Mask[CoordB + ChunkSize * CoordC] = Value;
							}
						}

						if (!bAnyVisible)
						{
							continue;
						}

						// --- greedy rectangle merge ---
						for (int32 StartC = 0; StartC < ChunkSize; ++StartC)
						{
							for (int32 StartB = 0; StartB < ChunkSize;)
							{
								const uint32 FaceKey = Mask[StartB + ChunkSize * StartC];
								if (FaceKey == 0)
								{
									++StartB;
									continue;
								}
								const uint16 BlockId = static_cast<uint16>(FaceKey & 0xFFFF);
								const uint8 Occlusion = static_cast<uint8>(FaceKey >> 16);
								const uint8 Damage = static_cast<uint8>((FaceKey >> 20) & 0xF0);
								const bool bMergeable = IsUniformOcclusion(Occlusion);

								// Grow along B while the block id and occlusion match.
								int32 Width = 1;
								while (bMergeable && StartB + Width < ChunkSize
									&& Mask[StartB + Width + ChunkSize * StartC] == FaceKey)
								{
									++Width;
								}

								// Then grow along C, but only by whole rows.
								int32 Height = 1;
								bool bCanGrow = bMergeable;
								while (StartC + Height < ChunkSize && bCanGrow)
								{
									for (int32 Offset = 0; Offset < Width; ++Offset)
									{
										if (Mask[StartB + Offset + ChunkSize * (StartC + Height)] != FaceKey)
										{
											bCanGrow = false;
											break;
										}
									}
									if (bCanGrow)
									{
										++Height;
									}
								}

								// --- emit ---
								const float Plane = static_cast<float>((Step > 0) ? Slice + 1 : Slice);

								FVector3f Base(0.0f);
								Base[Axis] = Plane;
								Base[B] = static_cast<float>(StartB);
								Base[C] = static_cast<float>(StartC);

								FVector3f DeltaB(0.0f);
								DeltaB[B] = static_cast<float>(Width);

								FVector3f DeltaC(0.0f);
								DeltaC[C] = static_cast<float>(Height);

								FVector3f Normal(0.0f);
								Normal[Axis] = static_cast<float>(Step);

								const FName MaterialClass = Materials.GetMaterialClass(BlockId);
								const FColor Color = ColorForMaterialClass(MaterialClass);

								FMadMeshSection& Section = OutMesh.FindOrAddSection(MaterialClass);
								Section.Reserve(Section.NumVertices() + 4, Section.Indices.Num() + 6);

								// UVs in voxel units so a merged quad tiles its
								// material once per block rather than stretching
								// one texture across the whole merged rectangle.
								const int32 V0 = Section.AddVertex(Base * VoxelSize, Normal,
									FVector2f(0.0f, 0.0f), Color, CubicVertex(Occlusion, 0), Damage);
								const int32 V1 = Section.AddVertex((Base + DeltaB) * VoxelSize, Normal,
									FVector2f(static_cast<float>(Width), 0.0f), Color, CubicVertex(Occlusion, 1), Damage);
								const int32 V2 = Section.AddVertex((Base + DeltaB + DeltaC) * VoxelSize, Normal,
									FVector2f(static_cast<float>(Width), static_cast<float>(Height)), Color, CubicVertex(Occlusion, 2), Damage);
								const int32 V3 = Section.AddVertex((Base + DeltaC) * VoxelSize, Normal,
									FVector2f(0.0f, static_cast<float>(Height)), Color, CubicVertex(Occlusion, 3), Damage);

								// Walking B then C is counter-clockwise with a
								// +Axis normal, so a -Axis face reverses. The quad
								// splits along whichever diagonal joins the corners
								// more alike, or occlusion interpolates anisotropically
								// and a shadow shows the triangle seam.
								if (CornerOcclusion(Occlusion, 0) + CornerOcclusion(Occlusion, 2) > CornerOcclusion(Occlusion, 1) + CornerOcclusion(Occlusion, 3))
								{
									AddOrientedQuad(Section, V1, V2, V3, V0, /*bReverse*/ Step < 0);
								}
								else
								{
									AddOrientedQuad(Section, V0, V1, V2, V3, /*bReverse*/ Step < 0);
								}

								// Clear the consumed rectangle.
								for (int32 OffsetC = 0; OffsetC < Height; ++OffsetC)
								{
									for (int32 OffsetB = 0; OffsetB < Width; ++OffsetB)
									{
										Mask[StartB + OffsetB + ChunkSize * (StartC + OffsetC)] = 0;
									}
								}

								StartB += Width;
							}
						}
					}
				}
			}
		}
	}

	int32 ChooseLod(int32 Distance, int32 CurrentLod, int32 Lod1Distance, int32 Lod2Distance)
	{
		auto LevelAt = [Lod1Distance, Lod2Distance](int32 D) { return D > Lod2Distance ? 2 : (D > Lod1Distance ? 1 : 0); };
		const int32 Wanted = LevelAt(Distance);
		if (CurrentLod < 0 || Wanted <= CurrentLod)
		{
			return Wanted;
		}
		return FMath::Max(CurrentLod, LevelAt(Distance - 1));
	}

	void BuildChunkMesh(
		const FMadChunkSampleGrid& Grid,
		const IMadBlockRegistry& Registry,
		const FMeshSettings& Settings,
		FMadChunkMesh& OutMesh)
	{
		BuildChunkMesh(Grid, nullptr, Registry, Settings, OutMesh);
	}

	void BuildChunkMesh(
		const FMadChunkSampleGrid& Grid,
		const FMadLodSampleGrid* LodGrid,
		const IMadBlockRegistry& Registry,
		const FMeshSettings& Settings,
		FMadChunkMesh& OutMesh)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MadFall::BuildChunkMesh);

		const double Start = FPlatformTime::Seconds();

		OutMesh.Coord = Grid.Coord;
		// The coarse lattice is used only when it matches the level asked for;
		// anything else meshes at full resolution rather than guessing.
		const bool bCoarse = LodGrid != nullptr && Settings.LodLevel > 0 && LodGrid->Stride == (1 << Settings.LodLevel);
		OutMesh.LodLevel = bCoarse ? Settings.LodLevel : 0;
		OutMesh.Sections.Reset();

		// A chunk that is entirely air or entirely solid interior has no surface
		// anywhere. Checking 39304 bytes is far cheaper than running two meshers
		// over it, and most of a loaded world is one or the other.
		if (Grid.IsEmpty() || Grid.IsFullySolid())
		{
			OutMesh.BuildMilliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
			return;
		}

		FMaterialResolver Materials(Registry);

		if (Settings.bIsosurface)
		{
			const double IsoStart = FPlatformTime::Seconds();
			if (bCoarse)
			{
				BuildIsosurface(*LodGrid, LodGrid->Points, LodGrid->Stride, Materials, OutMesh);
			}
			else
			{
				BuildIsosurface(Grid, MadFall::ChunkSize, 1, Materials, OutMesh);
			}
			OutMesh.IsosurfaceMilliseconds = (FPlatformTime::Seconds() - IsoStart) * 1000.0;
		}

		if (Settings.bCubic)
		{
			const double CubicStart = FPlatformTime::Seconds();
			BuildCubic(Grid, Materials, OutMesh);
			OutMesh.CubicMilliseconds = (FPlatformTime::Seconds() - CubicStart) * 1000.0;
		}

		// Drop sections that ended up with no geometry, so the component does
		// not create empty draw calls for them.
		OutMesh.Sections.RemoveAll([](const FMadMeshSection& Section) { return Section.IsEmpty(); });

		OutMesh.BuildMilliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
	}
}
