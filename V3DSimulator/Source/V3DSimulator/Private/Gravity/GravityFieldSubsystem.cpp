// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "Gravity/GravityFieldSubsystem.h"
#include "Gravity/GravityFieldComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

bool UGravityFieldSubsystem::DoesSupportWorldType(EWorldType::Type Type) const
{ return Type == EWorldType::Game || Type == EWorldType::PIE; }
void UGravityFieldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    PreActorTickHandle = FWorldDelegates::OnWorldPreActorTick.AddUObject(this, &UGravityFieldSubsystem::BeforeActorTick);
}
void UGravityFieldSubsystem::Deinitialize()
{
    FWorldDelegates::OnWorldPreActorTick.Remove(PreActorTickHandle);
    Fields.Empty();
    PreviousAccelerations.Empty();
    Super::Deinitialize();
}
void UGravityFieldSubsystem::RegisterField(UGravityFieldComponent* Field)
{
    if (IsValid(Field) && Field->GetWorld() == GetWorld()) Fields.AddUnique(Field);
}
void UGravityFieldSubsystem::UnregisterField(UGravityFieldComponent* Field) { Fields.Remove(Field); }

bool UGravityFieldSubsystem::Sample(const FVector& Position, const AActor* IgnoreActor, FVector& Acceleration) const
{
    Acceleration = FVector::ZeroVector;
    if (Position.ContainsNaN()) return false;
    bool Found = false;
    int32 BestPriority = MIN_int32;
    double BestStrength = -1.0, BestDistance = 0.0;
    FVector BestCenter = FVector::ZeroVector;
    for (const auto& Weak : Fields)
    {
        const UGravityFieldComponent* Field = Weak.Get();
        if (!IsValid(Field) || !IsValid(Field->GetOwner()) || Field->GetOwner() == IgnoreActor
            || Field->GetOwner()->IsActorBeingDestroyed()) continue;
        const FGravityFieldSettings& Settings = Field->GetSettingsRef();
        if (Found && Settings.Priority < BestPriority) continue;
        const FVector Center = Field->GetFieldCenter();
        FVector Candidate;
        double Strength, Distance;
        if (!V3DGravityMath::Evaluate(Settings, Center, Position, Candidate, Strength, Distance)) continue;
        // Geometric tie-breaks are independent of actor names, registration order and network roles.
        const bool CenterLess = Center.X != BestCenter.X ? Center.X < BestCenter.X
            : Center.Y != BestCenter.Y ? Center.Y < BestCenter.Y : Center.Z < BestCenter.Z;
        if (!Found || Settings.Priority > BestPriority || Strength > BestStrength
            || (Strength == BestStrength && (Distance < BestDistance || (Distance == BestDistance && CenterLess))))
        {
            Found = true;
            BestPriority = Settings.Priority;
            BestStrength = Strength;
            BestDistance = Distance;
            BestCenter = Center;
            Acceleration = Candidate;
        }
    }
    return Found;
}
FVector UGravityFieldSubsystem::GetGravityAtLocation(FVector Position, AActor* IgnoreActor) const
{
    FVector Acceleration;
    if (Sample(Position, IgnoreActor, Acceleration)) return Acceleration;
    return FVector(0.0, 0.0, GetWorld() ? GetWorld()->GetGravityZ() : -980.0);
}
void UGravityFieldSubsystem::BeforeActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
    if (World != GetWorld() || TickType == LEVELTICK_ViewportsOnly || TickType == LEVELTICK_PauseTick
        || World->IsPaused() || !FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.0f) return;
    Fields.RemoveAllSwap([](const auto& Field) { return !Field.IsValid(); });
    if (Fields.IsEmpty() && PreviousAccelerations.IsEmpty()) return;

    // Physics broad phase finds movable bodies of any actor class, including editor-placed props.
    // Queries are restricted to active field spheres; no per-frame world actor scan is necessary.
    TSet<TWeakObjectPtr<UPrimitiveComponent>> Candidates;
    TArray<FOverlapResult> Overlaps;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(V3DGravityFields), false);
    Params.MobilityType = EQueryMobilityType::Dynamic;
    for (const auto& Weak : Fields)
    {
        const auto* Field = Weak.Get();
        if (!IsValid(Field) || !Field->GetSettingsRef().bEnabled) continue;
        const FVector Center = Field->GetFieldCenter();
        if (Center.ContainsNaN()) continue;
        Overlaps.Reset();
        World->OverlapMultiByObjectType(Overlaps, Center, FQuat::Identity,
            FCollisionObjectQueryParams(FCollisionObjectQueryParams::AllObjects),
            FCollisionShape::MakeSphere(static_cast<float>(Field->GetSettingsRef().RadiusCm)), Params);
        for (const auto& Hit : Overlaps) if (auto* Component = Hit.GetComponent()) Candidates.Add(Component);
    }
    // Also revisit last frame's receivers: sleeping bodies must wake when a source exits/is removed.
    for (const auto& Pair : PreviousAccelerations) Candidates.Add(Pair.Key);
    TMap<TWeakObjectPtr<UPrimitiveComponent>, TMap<FName, FVector>> CurrentAccelerations;
    const FVector WorldAcceleration(0.0, 0.0, World->GetGravityZ());
    for (const auto& Weak : Candidates)
    {
        UPrimitiveComponent* Component = Weak.Get();
        if (!IsValid(Component) || !Component->IsRegistered() || !IsValid(Component->GetOwner())
            || Component->GetOwner()->IsActorBeingDestroyed()) continue;
        const TMap<FName, FVector>* Previous = PreviousAccelerations.Find(Weak);
        const auto Apply = [&](FBodyInstance* Body, FName Bone)
        {
            if (!Body || !Body->IsInstanceSimulatingPhysics() || !Body->bEnableGravity) return;
            FVector Acceleration;
            const bool InField = Sample(Body->GetCOMPosition(), Component->GetOwner(), Acceleration);
            const FVector* Last = Previous ? Previous->Find(Bone) : nullptr;
            if (!InField)
            {
                if (Last) Body->WakeInstance();
                return;
            }
            CurrentAccelerations.FindOrAdd(Weak).Add(Bone, Acceleration);
            const bool Changed = !Last || !Last->Equals(Acceleration, 0.01);
            if (Changed) Body->WakeInstance();
            if (Body->IsInstanceAwake()) Body->AddForce(Acceleration - WorldAcceleration, true, true);
        };
        if (USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Component))
        {
            for (FBodyInstance* Body : Skeletal->Bodies)
                if (Body && Body->BodySetup.IsValid()) Apply(Body, Body->BodySetup->BoneName);
        }
        else Apply(Component->GetBodyInstance(), NAME_None);
    }
    PreviousAccelerations = MoveTemp(CurrentAccelerations);
}
