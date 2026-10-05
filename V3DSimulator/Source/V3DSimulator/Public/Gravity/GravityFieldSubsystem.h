// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "GravityFieldSubsystem.generated.h"
class UGravityFieldComponent;
class UPrimitiveComponent;

/** One world-local field registry; no owning references to actors/components or persistent body pointers. */
UCLASS()
class V3DSIMULATOR_API UGravityFieldSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
    void RegisterField(UGravityFieldComponent* Field);
    void UnregisterField(UGravityFieldComponent* Field);
    /** True includes a zero-strength field. IgnoreActor prevents a source from attracting itself. */
    bool Sample(const FVector& Position, const AActor* IgnoreActor, FVector& Acceleration) const;
    UFUNCTION(BlueprintPure, Category="Gravity")
    FVector GetGravityAtLocation(FVector Position, AActor* IgnoreActor) const;
private:
    void BeforeActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
    TArray<TWeakObjectPtr<UGravityFieldComponent>> Fields;
    TMap<TWeakObjectPtr<UPrimitiveComponent>, TMap<FName, FVector>> PreviousAccelerations;
    FDelegateHandle PreActorTickHandle;
};
