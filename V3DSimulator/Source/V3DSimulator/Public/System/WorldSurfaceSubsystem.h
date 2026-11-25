// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "System/RuntimeSoundSubsystem.h"
#include "System/WorldSurfaceConfig.h"
#include "System/V3DSoundPlayback.h"
#include "WorldSurfaceSubsystem.generated.h"

class UPhysicalMaterial;
class UStaticMeshComponent;
class UMeshComponent;
class UPrimitiveComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UAudioComponent;
class USoundAttenuation;

/** Owns world-specific transient physical materials and bounded short-effect PCM caches. */
UCLASS()
class V3DSIMULATOR_API UWorldSurfaceSubsystem final : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    static UWorldSurfaceSubsystem* Get(const UObject* Context);
    virtual void Deinitialize() override;
    bool Configure(const TSharedPtr<FJsonObject>& Json, FString& Error);
    bool Configure(FV3DWorldSurfaceConfig Parsed, FString& Error);
    void Reset();
    void ApplyToStaticMesh(UStaticMeshComponent* Component);
    void ApplyToMesh(UMeshComponent* Component);
    /** Applies a world/registry default to a simple collider without replacing authored overrides. */
    void ApplyToPrimitive(UPrimitiveComponent* Component);
    /** Registers ordered terrain palette layer material names (0..3) for a baked model asset. */
    void RegisterTerrainLayers(const UObject* BakedAsset, const TArray<FString>& LayerNames);
    UPhysicalMaterial* GetSimplePhysicalMaterial(UPrimitiveComponent* Component) const;
    /** True means a JSON mapping handled the step, including muted/not-yet-preloaded sounds. */
    bool PlayFootstep(const FHitResult& Hit, UObject* Emitter = nullptr);
private:
    friend class FV3DRegistryPhysicalMaterialTest;
    FV3DWorldSurfaceConfig Config;
    UPROPERTY(Transient) TObjectPtr<UPhysicalMaterial> DefaultPhysicalMaterial;
    FV3DFootstepLoops FootstepLoops;
    TMap<TWeakObjectPtr<UPrimitiveComponent>, TWeakObjectPtr<UPhysicalMaterial>> UniformComponents;
    TMap<TWeakObjectPtr<const UObject>, TArray<FString>> TerrainLayersByAsset;
    TMap<TWeakObjectPtr<UMeshComponent>, TWeakObjectPtr<const UObject>> TerrainAssetByComponent;
    UPROPERTY(Transient) TMap<FName, TObjectPtr<UPhysicalMaterial>> PhysicalMaterials;
    UPROPERTY(Transient) TArray<TObjectPtr<UAudioComponent>> ActiveSounds;
    UPROPERTY(Transient) TObjectPtr<USoundAttenuation> Attenuation;
    TMap<TWeakObjectPtr<UMaterialInterface>, TMap<FName, TWeakObjectPtr<UMaterialInstanceDynamic>>> MaterialCache;
    TMap<FString, FV3DPcmPtr> PcmCache;
    TArray<FString> PendingSounds;
    TMap<FName, int32> LastSoundIndex;
    uint64 Generation = 0;
    int32 InFlight = 0;
    int64 CachedBytes = 0;
    int32 MaterialApplications = 0;
    void PumpSoundLoads();
    FName FindSurface(const UPhysicalMaterial* Material) const;
    FName ResolveTerrainLayerSurface(const FString& LayerName) const;
    const TArray<FString>* FindTerrainLayersForHit(const FHitResult& Hit, UMeshComponent* MeshComp, UMaterialInterface* HitMaterial) const;
    bool PlaySurfaceFootstep(FName Surface, const FVector& Location, UObject* Emitter, float VolumeScale = 1.0f, bool bAllowLoopTracking = true);
    bool TryPlayTerrainFootstep(const FHitResult& Hit, UObject* Emitter, UMeshComponent* MeshComp, UMaterialInterface* HitMaterial);
};
