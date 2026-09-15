// Copyright MadFall. All Rights Reserved.

#include "MadSurfaceMaterials.h"

#include "Engine/Texture.h"
#include "MadSurfaceRegistry.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace MadFall::SurfaceMaterials
{
	bool IsTexturedWorldMaterial(const UMaterialInterface* Material)
	{
		const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
		return Base != nullptr && Base->GetPathName() == WorldMaterialPath;
	}

	UMaterialInstanceDynamic* MakeHeld(UObject* Outer, FName MaterialClass, float Weather)
	{
		const FMadSurfaceDefinition* Surface = MadFall::GetSurfaces().Find(MaterialClass);
		if (Surface == nullptr || Surface->Material.IsNull())
		{
			return nullptr;
		}
		const UMaterialInterface* World = Cast<UMaterialInterface>(Surface->Material.TryLoad());
		const FColor Bytes = MadFall::GetSurfaces().GetVertexColor(MaterialClass);
		const FLinearColor Colour(Bytes.R / 255.0f, Bytes.G / 255.0f, Bytes.B / 255.0f, 1.0f);

		// A layered surface: the held array material, told which layer.
		const UMaterial* Base = World ? World->GetMaterial() : nullptr;
		if (Surface->TextureLayer != INDEX_NONE && Base != nullptr && Base->GetPathName() == ArrayMaterialPath)
		{
			UMaterialInterface* HeldArray = LoadObject<UMaterialInterface>(nullptr, ArrayHeldMaterialPath);
			if (HeldArray == nullptr)
			{
				return nullptr;
			}
			UMaterialInstanceDynamic* Layered = UMaterialInstanceDynamic::Create(HeldArray, Outer);
			Layered->SetScalarParameterValue(TEXT("Layer"), static_cast<float>(Surface->TextureLayer));
			Layered->SetVectorParameterValue(TEXT("Color"), Colour);
			Layered->SetScalarParameterValue(TEXT("Weather"), Weather);
			return Layered;
		}

		if (!IsTexturedWorldMaterial(World))
		{
			return nullptr;
		}
		UMaterialInterface* Held = LoadObject<UMaterialInterface>(nullptr, HeldMaterialPath);
		if (Held == nullptr)
		{
			return nullptr;
		}

		UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Held, Outer);
		for (const TCHAR* Name : { TEXT("BaseColor"), TEXT("Normal"), TEXT("Roughness"), TEXT("SideBaseColor"), TEXT("SideNormal"), TEXT("SideRoughness") })
		{
			UTexture* Texture = nullptr;
			if (World->GetTextureParameterValue(FHashedMaterialParameterInfo(FName(Name)), Texture) && Texture != nullptr)
			{
				Material->SetTextureParameterValue(FName(Name), Texture);
			}
		}
		for (const TCHAR* Name : { TEXT("TileVoxels"), TEXT("Tint"), TEXT("UseSides"), TEXT("Metallic") })
		{
			float Value = 0.0f;
			if (World->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(Name)), Value))
			{
				Material->SetScalarParameterValue(FName(Name), Value);
			}
		}
		// The world material reads the surface colour from vertex colour, stored as
		// linear bytes; the held one takes the same linear value as a parameter.
		Material->SetVectorParameterValue(TEXT("Color"), Colour);
		Material->SetScalarParameterValue(TEXT("Weather"), Weather);
		return Material;
	}
}
