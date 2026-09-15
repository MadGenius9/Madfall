// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Asset paths the procedural rigs and the view model share. One definition:
 * each file used to keep its own copy in an anonymous namespace, and a unity
 * build that put two of those files in one translation unit failed to compile.
 */
namespace MadFall::BasicShapes
{
	inline constexpr const TCHAR* CubeMesh = TEXT("/Engine/BasicShapes/Cube.Cube");
	inline constexpr const TCHAR* CylinderMesh = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
	inline constexpr const TCHAR* TintMaterial = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
	inline constexpr const TCHAR* CharacterMaterial = TEXT("/Game/Materials/M_MadCharacter.M_MadCharacter");
}
