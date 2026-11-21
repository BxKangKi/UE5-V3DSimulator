// Light-space depth for underwater scattering. Never read back to the CPU.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UnderwaterShaftOcclusionComponent.generated.h"

class AWaterActor;
class UDirectionalLightComponent;
class UMaterialInstanceDynamic;
class USceneCaptureComponent2D;
class UTexture2D;
class UTextureRenderTarget2D;

UCLASS()
class V3DSIMULATOR_API UUnderwaterShaftOcclusionComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UUnderwaterShaftOcclusionComponent();
    void Configure(UMaterialInstanceDynamic* Material, UDirectionalLightComponent* Sun,
        bool bOceanEnabled, float WaterHeight);
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;
protected:
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void OnUnregister() override;
private:
    bool EnsureResources();
    void ReleaseResources();
    void ResetCachedParameters();
    void SetReady(bool bReady);
    UTexture2D* GetFallbackTexture();

    UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> TargetMaterial;
    UPROPERTY(Transient) TObjectPtr<UDirectionalLightComponent> SunLight;
    UPROPERTY(Transient) TArray<TObjectPtr<USceneCaptureComponent2D>> Captures;
    UPROPERTY(Transient) TArray<TObjectPtr<UTextureRenderTarget2D>> DepthTargets;
    UPROPERTY(Transient) TObjectPtr<UTexture2D> FallbackTexture;
    TArray<AWaterActor*> CachedWaterActors;

    FLinearColor LastWrittenOrigins[2] = { FLinearColor::Transparent, FLinearColor::Transparent };
    FVector LastWrittenRight = FVector::ZeroVector;
    FVector LastWrittenForward = FVector::ZeroVector;
    FVector LastCapturedFarOrigin = FVector::ZeroVector;
    FQuat LastCapturedFarRotation = FQuat::Identity;

    float WaterLevel = 0.0f;
    float InactiveElapsedSeconds = 0.0f;
    uint32 CaptureFrameCounter = 0;
    bool bMaterialBindingsAvailable = false;
    bool bTexturesBound = false;
    bool bLastReadyState = false;
    bool bHasCachedLightBasis = false;
    bool bHasCachedOrigins[2] = { false, false };
    bool bHasCapturedFarOnce = false;
};
