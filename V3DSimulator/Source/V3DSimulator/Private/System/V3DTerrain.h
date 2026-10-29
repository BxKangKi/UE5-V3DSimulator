// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "System/WorldBakedData.h"
class UMaterialInterface;
class UObject;
class UglTFRuntimeAsset;

/** glTF authoring convention, independent of Unreal's transient material-object names. */
namespace V3DTerrain
{
    inline constexpr const TCHAR* DefaultMaterialPath = TEXT("/Game/Resources/Materials/V3D/M_V3DTerrain.M_V3DTerrain");
    bool IsTerrainName(const FString& Name);
    bool SourceHasTerrain(UglTFRuntimeAsset* Source);
    bool IsPaletteMaterial(const FGWorldBakedMaterial& Material);
    /** Checks the four array parameters before accepting a configured palette parent. Game-thread only. */
    bool IsCompatibleMaterial(UMaterialInterface* Material);
    /** Resolves the terrain base material path from the central AssetRegistry (with GConfig/default fallback). */
    FString ResolveMaterialPath(const UObject* WorldContextObject = nullptr);
    /** Loads the registry, configured or shipped terrain base material on the game thread. */
    UMaterialInterface* ResolveMaterial(const UObject* WorldContextObject = nullptr,
        const FString& ArchiveMaterialPath = FString());
    /**
     * Called after source capture and before archive metadata. .terrain explicitly selects
     * opaque, single-sided, default-lit surfaces with linear RGBA layer weights. Clearcoat,
     * other extra lighting lobes and valid alpha/doubleSided flags are ignored after validation.
     * Valid source samplers are normalized to repeat/trilinear arrays with rebuilt mip chains.
     * Unknown shaders and legacy diffuse/glossiness keep the original materials for the whole
     * model and emit a warning. Invalid source factors/references still fail.
     * Required images decode directly from source texture indices into a capped CPU cache;
     * captured material defaults/overrides and platform GPU normal encodings are not sources.
     * A warm texture-cache return is resolved through its source image, rather than treated
     * as an image decode failure or copied back from a platform-encoded GPU texture.
     * Large pure pixel conversions run in parallel; UObject/material resolution stays on the caller.
     */
    bool Bake(UglTFRuntimeAsset* Source, FGWorldBakedModel& Model, FString& Error, const UObject* WorldContextObject = nullptr);
}
