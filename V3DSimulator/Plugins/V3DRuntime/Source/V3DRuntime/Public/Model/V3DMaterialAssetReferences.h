// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "V3DMaterialAssetReferences.generated.h"
class UMaterialInterface;

/** Resolved, GC-owned copies of AssetRegistry's four existing single-sided parents. */
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

    UPROPERTY(Transient) TObjectPtr<UMaterialInterface> Terrain = nullptr;
    uint8 MaterialMode = 0;

    /** Slot: Opaque=0, Masked=1, Translucent=2, Toon=3. No separate dynamic parents. */
    UMaterialInterface* Get(int32 Slot) const;
    static const TCHAR* SlotName(int32 Slot);
    static bool MatchesSlot(const UMaterialInterface* Material, int32 Slot);
    bool IsEmpty() const;
    void Reset() { *this = FV3DMaterialAssetReferences(); }
};
