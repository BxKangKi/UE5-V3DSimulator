// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "V3DMaterialAssetReferences.generated.h"
class UMaterialInterface;

/** Resolved, GC-owned copies of AssetRegistry parents, reserved materials and cooked fallback parents. */
USTRUCT(BlueprintType)
struct V3DRUNTIME_API FV3DMaterialAssetReferences
{
    GENERATED_BODY()
    UPROPERTY(Transient, BlueprintReadOnly, Category="Materials")
    TObjectPtr<UMaterialInterface> MI_Opaque = nullptr;
    UPROPERTY(Transient, BlueprintReadOnly, Category="Materials")
    TObjectPtr<UMaterialInterface> MI_Masked = nullptr;
    UPROPERTY(Transient, BlueprintReadOnly, Category="Materials")
    TObjectPtr<UMaterialInterface> MI_Translucent = nullptr;
    UPROPERTY(Transient, BlueprintReadOnly, Category="Materials")
    TObjectPtr<UMaterialInterface> MI_Toon = nullptr;

    UPROPERTY(Transient) TObjectPtr<UMaterialInterface> MI_OpaqueTwoSided = nullptr;
    UPROPERTY(Transient) TObjectPtr<UMaterialInterface> MI_TranslucentTwoSided = nullptr;
    UPROPERTY(Transient) TObjectPtr<UMaterialInterface> MI_ToonTwoSided = nullptr;
    UPROPERTY(Transient) TMap<FString, TObjectPtr<UMaterialInterface>> ReservedMaterials;
    UPROPERTY(Transient) TObjectPtr<UMaterialInterface> Terrain = nullptr;
    UPROPERTY(Transient) TObjectPtr<UMaterialInterface> Foliage = nullptr;
    UPROPERTY(Transient) TObjectPtr<UMaterialInterface> Impostor = nullptr;
    uint8 MaterialMode = 0;

    /** Opaque means BLEND_Masked. 0/1 opaque, 2 translucent, 3 toon; +4 two-sided. */
    static constexpr int32 SlotCount = 8;
    static FString NormalizeMaterialName(const FString& Name);
    static const TCHAR* FallbackPath(bool bTwoSided);
    static UMaterialInterface* LoadFallback(bool bTwoSided, bool bAllowSynchronousLoading = true);
    static bool IsSupportedParent(const UMaterialInterface* Material);
    UMaterialInterface* Get(int32 Slot) const;
    static const TCHAR* SlotName(int32 Slot);
    static bool MatchesSlot(const UMaterialInterface* Material, int32 Slot);
    bool IsEmpty() const;
    void Reset() { *this = FV3DMaterialAssetReferences(); }
};
