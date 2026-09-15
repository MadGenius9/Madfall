// Copyright MadFall. All Rights Reserved.

#include "MadBlockDefinition.h"

FMadBlockDefView FMadBlockDefinitionData::MakeView(int32 RuntimeId) const
{
	FMadBlockDefView View;

	View.Id = Id;
	View.RuntimeId = RuntimeId;
	View.MaterialClass = MaterialClass;
	View.ShapeKind = ShapeKind;
	View.RotationMode = RotationMode;
	View.MassKg = MassKg;
	View.Hardness = Hardness;
	View.SupportStrength = SupportStrength;
	View.MaxHorizontalSpan = MaxHorizontalSpan;
	View.bIsAnchor = bIsAnchor;
	View.bTransparent = bTransparent;
	View.bLiquid = bLiquid;
	View.bClimbable = bClimbable;
	View.bFlammable = bFlammable;
	View.bOccludesNeighbors = bOccludesNeighbors;
	View.bUnresolved = false;

	// A definition with no explicit stages still has one: intact.
	View.NumDamageStages = FMath::Max(DamageStages.Num(), 1);

	return View;
}
