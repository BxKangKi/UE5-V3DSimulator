// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Model/V3DMaterialAssetReferences.h"
#include "MaterialDefaultAsset.generated.h"
class UMaterialInterface;

/** Current single-sided alpha policies; numeric values are part of the wire contract. */
UENUM(BlueprintType)
enum class EV3DMaterialMode : uint8
{
    Auto = 0,
    Masked = 1,
    Translucent = 3,
    Opaque = 5
};

/** Async mesh requests retain this object while their request-local config holds raw pointers. */
UCLASS(Transient)
class V3DRUNTIME_API UMaterialDefaultRuntimeCache : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(Transient) FV3DMaterialAssetReferences References;
};

/** Assign existing, cooked single-sided parents. No material asset is generated or modified. */
USTRUCT(BlueprintType)
struct V3DRUNTIME_API FV3DMaterialSoftAssetReferences
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Materials", meta=(DisplayName="MI_Opaque", ToolTip="Existing single-sided Opaque parent. Shared by static and dynamic models."))
    TSoftObjectPtr<UMaterialInterface> MI_Opaque;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Materials", meta=(DisplayName="MI_Masked", ToolTip="Existing single-sided Masked parent. Shared by static and dynamic models."))
    TSoftObjectPtr<UMaterialInterface> MI_Masked;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Materials", meta=(DisplayName="MI_Translucent", ToolTip="Existing single-sided Translucent parent. Shared by static and dynamic models."))
    TSoftObjectPtr<UMaterialInterface> MI_Translucent;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Materials", meta=(DisplayName="MI_Toon", ToolTip="Existing single-sided Toon parent. Keeps its authored BlendMode for every Toon model."))
    TSoftObjectPtr<UMaterialInterface> MI_Toon;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Materials", meta=(ToolTip="Default shader: Auto follows GLB alpha mode; forced modes choose that parent globally. Toon always uses MI_Toon. Two Sided stays disabled."))
    EV3DMaterialMode MaterialMode = EV3DMaterialMode::Auto;

    const TSoftObjectPtr<UMaterialInterface>& Get(int32 Slot) const;
    void GetPreloadPaths(TArray<FSoftObjectPath>& Paths) const;
    bool Resolve(FV3DMaterialAssetReferences& OutReferences, TArray<FString>& OutFailures,
        bool bAllowSynchronousLoading = true) const;
    int32 NumConfiguredReferences() const;
};
