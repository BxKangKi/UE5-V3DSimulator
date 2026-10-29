// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "Gravity/GravityFieldComponent.h"
#include "Gravity/GravityFieldSubsystem.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Net/UnrealNetwork.h"

UGravityFieldComponent::UGravityFieldComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    SetIsReplicatedByDefault(true);
}
void UGravityFieldComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UGravityFieldComponent, Settings);
    DOREPLIFETIME(UGravityFieldComponent, bOverrideModelSettings);
}
void UGravityFieldComponent::BeginPlay() { Super::BeginPlay(); RefreshRegistration(); }
void UGravityFieldComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    if (UWorld* World = GetWorld())
        if (auto* Gravity = World->GetSubsystem<UGravityFieldSubsystem>()) Gravity->UnregisterField(this);
    if (bForcedAlwaysRelevant && IsValid(GetOwner()) && GetOwner()->HasAuthority())
    {
        GetOwner()->bAlwaysRelevant = false;
        bForcedAlwaysRelevant = false;
    }
    Super::EndPlay(Reason);
}
void UGravityFieldComponent::OnRep_Settings() { RefreshRegistration(); }
bool UGravityFieldComponent::SetSettings(const FGravityFieldSettings& NewSettings)
{
    if (!IsValid(GetOwner()) || !GetOwner()->HasAuthority() || !NewSettings.IsValid()) return false;
    bOverrideModelSettings = true;
    Settings = NewSettings;
    RefreshRegistration();
    GetOwner()->FlushNetDormancy();
    GetOwner()->ForceNetUpdate();
    return true;
}
void UGravityFieldComponent::ApplyModelSettings(const FGravityFieldSettings& NewSettings)
{
    if (bOverrideModelSettings || !NewSettings.IsValid()) return;
    Settings = NewSettings;
    RefreshRegistration();
}
FVector UGravityFieldComponent::GetFieldCenter() const
{
    return IsValid(GetOwner()) ? GetOwner()->GetActorTransform().TransformPosition(Settings.LocalCenter) : FVector::ZeroVector;
}
void UGravityFieldComponent::RefreshRegistration()
{
    if (!HasBegunPlay() || !GetWorld() || !IsValid(GetOwner())) return;
    const bool Enabled = Settings.bEnabled && Settings.IsValid();
    // Autonomous characters need the same moving sources as the server even beyond net cull distance.
    if (GetOwner()->HasAuthority())
    {
        if (Enabled && !GetOwner()->bAlwaysRelevant)
        {
            GetOwner()->bAlwaysRelevant = true;
            bForcedAlwaysRelevant = true;
        }
        else if (!Enabled && bForcedAlwaysRelevant)
        {
            GetOwner()->bAlwaysRelevant = false;
            bForcedAlwaysRelevant = false;
        }
    }
    if (auto* Gravity = GetWorld()->GetSubsystem<UGravityFieldSubsystem>())
    {
        if (Settings.bEnabled && Settings.IsValid()) Gravity->RegisterField(this);
        else Gravity->UnregisterField(this);
    }
}
