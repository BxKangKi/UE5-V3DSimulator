// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Gravity/GravityFieldTypes.h"
#include "GravityFieldComponent.generated.h"

/** Attach to any actor to make it a spherical gravity source. Does not enable physics on its owner. */
UCLASS(ClassGroup=(Physics), meta=(BlueprintSpawnableComponent))
class V3DSIMULATOR_API UGravityFieldComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UGravityFieldComponent();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    UFUNCTION(BlueprintPure, Category="Gravity")
    FGravityFieldSettings GetSettings() const { return Settings; }
    /** Runtime mutation is server-authoritative; invalid settings are rejected without partial changes. */
    UFUNCTION(BlueprintCallable, Category="Gravity")
    bool SetSettings(const FGravityFieldSettings& NewSettings);
    void ApplyModelSettings(const FGravityFieldSettings& NewSettings);
    FVector GetFieldCenter() const;
    const FGravityFieldSettings& GetSettingsRef() const { return Settings; }
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, ReplicatedUsing=OnRep_Settings, Category="Gravity")
    FGravityFieldSettings Settings;
    /** Preserve the actor's settings when a model is loaded asynchronously. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Replicated, Category="Gravity")
    bool bOverrideModelSettings = false;
private:
    UFUNCTION()
    void OnRep_Settings();
    void RefreshRegistration();
    bool bForcedAlwaysRelevant = false;
};
