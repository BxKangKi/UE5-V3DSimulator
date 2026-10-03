// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WeaponProjectileActor.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Weapon/WeaponProjectileActor.h"

#include "Components/SphereComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "System/GameUpdateSubSystem.h"

namespace WeaponProjectileTuning
{
    constexpr float MinProjectileLifeSeconds = 0.1f;
}

AWeaponProjectileActor::AWeaponProjectileActor()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = true;
    SetReplicateMovement(true);

    Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
    SetRootComponent(Collision);
    Collision->InitSphereRadius(4.0f);
    Collision->SetCollisionProfileName(TEXT("BlockAllDynamic"));
    Collision->SetNotifyRigidBodyCollision(true);
}

void AWeaponProjectileActor::BeginPlay()
{
    Super::BeginPlay();
    if (!HasAuthority())
    {
        SetActorEnableCollision(false);
        return; // Replicated projectiles follow the server transform.
    }

    if (Collision)
    {
        Collision->OnComponentHit.AddUniqueDynamic(this, &AWeaponProjectileActor::OnProjectileHit);
        if (IsValid(GetOwner())) Collision->IgnoreActorWhenMoving(GetOwner(), true);
        if (IsValid(GetInstigator())) Collision->IgnoreActorWhenMoving(GetInstigator(), true);
    }

    SetLifeSpan(FMath::Max(WeaponProjectileTuning::MinProjectileLifeSeconds, LifeSeconds));
    RegisterGameUpdate();
}

void AWeaponProjectileActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    bHitProcessed = true;
    if (IsValid(Collision)) Collision->OnComponentHit.RemoveDynamic(this, &AWeaponProjectileActor::OnProjectileHit);
    UnregisterGameUpdate();
    Super::EndPlay(EndPlayReason);
}

void AWeaponProjectileActor::InitProjectile(AController* InInstigatorController, float InDamage, float InImpulseStrength, float InLifeSeconds, const FVector& InLaunchVelocity)
{
    if (bHitProcessed || IsActorBeingDestroyed()) return;
    CachedInstigatorController = IsValid(InInstigatorController) ? InInstigatorController : nullptr;
    Damage = FMath::IsFinite(InDamage) ? FMath::Max(0.0f, InDamage) : 0.0f;
    ImpulseStrength = FMath::IsFinite(InImpulseStrength) ? FMath::Max(0.0f, InImpulseStrength) : 0.0f;
    LifeSeconds = FMath::IsFinite(InLifeSeconds)
        ? FMath::Max(WeaponProjectileTuning::MinProjectileLifeSeconds, InLifeSeconds) : 5.0f;
    Velocity = InLaunchVelocity.ContainsNaN() ? FVector::ZeroVector : InLaunchVelocity;
    SetInstigator(IsValid(InInstigatorController) ? InInstigatorController->GetPawn() : nullptr);
    if (IsValid(Collision))
    {
        if (IsValid(GetOwner())) Collision->IgnoreActorWhenMoving(GetOwner(), true);
        if (IsValid(GetInstigator())) Collision->IgnoreActorWhenMoving(GetInstigator(), true);
    }
    // SpawnActor runs BeginPlay before this initializer on the normal weapon path.
    if (HasAuthority() && HasActorBegunPlay()) SetLifeSpan(LifeSeconds);

    if (!Velocity.IsNearlyZero())
    {
        SetActorRotation(Velocity.Rotation());
    }
}

void AWeaponProjectileActor::RegisterGameUpdate()
{
    if (!HasAuthority() || GameUpdateTickHandle != INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [WeakThis = TWeakObjectPtr<AWeaponProjectileActor>(this)](const float DeltaSeconds)
            {
                if (AWeaponProjectileActor* StrongThis = WeakThis.Get())
                {
                    StrongThis->UpdateProjectile(DeltaSeconds);
                }
            },
            25);
    }
}

void AWeaponProjectileActor::UnregisterGameUpdate()
{
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;
}

void AWeaponProjectileActor::UpdateProjectile(float DeltaSeconds)
{
    if (!HasAuthority() || bHitProcessed || IsActorBeingDestroyed()
        || Velocity.ContainsNaN() || Velocity.IsNearlyZero()
        || !FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.0f)
    {
        return;
    }

    const FVector NewLocation = GetActorLocation() + Velocity * DeltaSeconds;
    FHitResult Hit;
    if (NewLocation.ContainsNaN()) { Destroy(); return; }
    SetActorLocation(NewLocation, true, &Hit, ETeleportType::None);
    // Swept movement may already have delivered OnComponentHit and destroyed this actor.
    if (bHitProcessed || IsActorBeingDestroyed()) return;
    SetActorRotation(Velocity.Rotation());

    if (Hit.bBlockingHit)
    {
        OnProjectileHit(Collision.Get(), Hit.GetActor(), Hit.GetComponent(), FVector::ZeroVector, Hit);
    }
}

void AWeaponProjectileActor::OnProjectileHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit)
{
    if (!HasAuthority() || bHitProcessed || IsActorBeingDestroyed()) return;
    if (OtherActor && (OtherActor == this || OtherActor == GetOwner() || OtherActor == GetInstigator())) return;
    // Latch before damage: a receiver can re-enter hit handling or destroy the projectile.
    bHitProcessed = true;
    if (!IsValid(OtherActor)) { Destroy(); return; }

    const FVector ShotDirection = Velocity.IsNearlyZero() ? GetActorForwardVector() : Velocity.GetSafeNormal();
    UGameplayStatics::ApplyPointDamage(OtherActor, Damage, ShotDirection, Hit, CachedInstigatorController.Get(), this, nullptr);

    if (IsValid(OtherComp) && OtherComp->IsSimulatingPhysics())
    {
        OtherComp->AddImpulseAtLocation(ShotDirection * ImpulseStrength, Hit.ImpactPoint, Hit.BoneName);
    }

    Destroy();
}
