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
    inline constexpr const TCHAR* DefaultMaterialPath = TEXT("/Game/V3D/Materials/M_V3DTerrain.M_V3DTerrain");
    bool IsTerrainName(const FString& Name);
    bool SourceHasTerrain(UglTFRuntimeAsset* Source);
    bool IsPaletteMaterial(const FGWorldBakedMaterial& Material);
    /** Resolves the terrain base material path from the central AssetRegistry (with GConfig/default fallback). */
    FString ResolveMaterialPath(const UObject* WorldContextObject = nullptr);
    /** Loads the terrain base material from the central AssetRegistry on the game thread. */
    UMaterialInterface* ResolveMaterial(const UObject* WorldContextObject = nullptr);
    /** Called after all source meshes are captured and before archive metadata is generated. */
    bool Bake(UglTFRuntimeAsset* Source, FGWorldBakedModel& Model, FString& Error, const UObject* WorldContextObject = nullptr);
}
