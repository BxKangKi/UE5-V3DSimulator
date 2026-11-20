// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Model/V3DMaterialAssetReferences.h"
#include "MaterialDefaultAsset.generated.h"
class UMaterialInterface;

/** Alpha and sidedness policies (Opaque uses a Masked shader); numeric values are part of the wire contract. */
UENUM(BlueprintType)
enum class EV3DMaterialMode : uint8
{
    Auto = 0,
    Masked = 1 UMETA(Hidden), // Read old archives/settings as Opaque. No separate UI mode.
    Translucent = 3,
    Opaque = 5,
    OpaqueTwoSided = 6 UMETA(DisplayName="Opaque Two Sided"),
    TranslucentTwoSided = 7 UMETA(DisplayName="Translucent Two Sided")
};

/** Async mesh requests retain this object while their request-local config holds raw pointers. */
UCLASS(Transient)
class V3DRUNTIME_API UMaterialDefaultRuntimeCache : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(Transient) FV3DMaterialAssetReferences References;
};

/** Assign existing, cooked parents; Opaque slots require BLEND_Masked. No material asset is generated or modified. */
USTRUCT(BlueprintType)
struct V3DRUNTIME_API FV3DMaterialSoftAssetReferences
{
    GENERATED_BODY()
    FV3DMaterialSoftAssetReferences();

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Materials", meta=(DisplayName="MI_Opaque", ToolTip="Single-sided Masked parent, presented as Opaque for dithered culling. Shared by static and dynamic models."))
    TSoftObjectPtr<UMaterialInterface> MI_Opaque;
    UPROPERTY() // Serialized compatibility only; migrated into MI_Opaque at resolution.
    TSoftObjectPtr<UMaterialInterface> MI_Masked;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Materials", meta=(DisplayName="MI_Translucent", ToolTip="Existing single-sided Translucent parent. Shared by static and dynamic models."))
    TSoftObjectPtr<UMaterialInterface> MI_Translucent;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Materials", meta=(DisplayName="MI_Toon", ToolTip="Single-sided Toon parent with Masked or Translucent blending."))
    TSoftObjectPtr<UMaterialInterface> MI_Toon;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Materials", meta=(ToolTip="Two-sided Masked parent; Opaque and Masked glTF input share this slot."))
    TSoftObjectPtr<UMaterialInterface> MI_OpaqueTwoSided;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Materials")
    TSoftObjectPtr<UMaterialInterface> MI_TranslucentTwoSided;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Materials")
    TSoftObjectPtr<UMaterialInterface> MI_ToonTwoSided;

    /** Reserved material-name to material-asset pairs (e.g., glass, tinted_glass, foliage). Case-insensitive. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Reserved Materials", meta=(ToolTip="Exact reserved material-name overrides (e.g. glass, tinted_glass, foliage). Suffix .foliage also resolves to foliage."))
    TMap<FString, TSoftObjectPtr<UMaterialInterface>> ReservedMaterials;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Materials", meta=(ToolTip="Auto maps both glTF Opaque and Masked to Opaque (Masked shader). Reserved materials take precedence."))
    EV3DMaterialMode MaterialMode = EV3DMaterialMode::Auto;

    const TSoftObjectPtr<UMaterialInterface>& Get(int32 Slot) const;
    void GetPreloadPaths(TArray<FSoftObjectPath>& Paths) const;
    bool Resolve(FV3DMaterialAssetReferences& OutReferences, TArray<FString>& OutFailures,
        bool bAllowSynchronousLoading = true) const;
    int32 NumConfiguredReferences() const;
};
