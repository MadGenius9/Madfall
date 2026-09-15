// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MadProjectile.generated.h"

class UStaticMeshComponent;

namespace MadFall::Projectile
{
	/** Earth gravity, cm/s^2. A projectile falls with a fraction of it. */
	inline constexpr float GravityCm = 980.0f;

	/**
	 * One explicit-Euler step of a ballistic flight: velocity first, then
	 * position, so the arc is a parabola within a centimetre at 60 Hz. Pure, for tests.
	 */
	MADFALLGAMEPLAY_API void Step(FVector& InOutLocation, FVector& InOutVelocity, float GravityScale, float DeltaSeconds);
}

/**
 * An arrow in flight.
 *
 * Moved by hand rather than by UProjectileMovementComponent: each step traces
 * the segment it covers against pawns (physics) and against voxels (the voxel
 * raycast - terrain collision is cooked asynchronously and can lag a fresh
 * edit, the voxel data never does). Whichever is nearer wins. A creature hit
 * takes the shooter's damage through IMadDamageable; a voxel hit stops the
 * arrow, and it may be recovered as a pickup where it struck.
 *
 * Lives at most MaxLifetime seconds, and never flies into unloaded chunks.
 */
UCLASS()
class MADFALLGAMEPLAY_API AMadProjectile : public AActor
{
	GENERATED_BODY()

public:
	AMadProjectile();

	virtual void Tick(float DeltaSeconds) override;

	/**
	 * Launches it. Damage and type come from the weapon; Ammo is the item id
	 * dropped back if RecoverChance succeeds after hitting the world.
	 */
	void Launch(AActor* InShooter, const FVector& Velocity, float InGravityScale, float InDamage, FName InDamageType,
		FName InAmmo, float InRecoverChance);

	/** Shaft colour; call after Launch. */
	void SetColour(const FLinearColor& Colour);

	static constexpr float MaxLifetime = 8.0f;

	/** Lifetime counters for CI. */
	static int32 TotalFired;
	static int32 TotalCreatureHits;
	static int32 TotalWorldHits;

private:
	UPROPERTY(VisibleAnywhere, Category = "MadFall")
	TObjectPtr<UStaticMeshComponent> Shaft;

	TWeakObjectPtr<AActor> Shooter;
	FVector Velocity = FVector::ZeroVector;
	float GravityScale = 0.5f;
	float Damage = 0.0f;
	FName DamageType;
	FName Ammo;
	float RecoverChance = 0.0f;
	float Age = 0.0f;
	bool bLaunched = false;
};
