// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "Model/V3DMaterialAssetReferences.h"
#include "Format/v3d.h"
#include "glTFRuntimeParser.h"

namespace V3DMaterialOverrideUtils
{
    /** Sided parent and reserved-material refs plus request policy. DefaultFamily request values:
     * 0 static/default, 1 dynamic (MID FadeDistance=50), 2 Toon. Pointer identities and
     * request policy keys participate in material/mesh caches, even when parents are shared. */
    inline void ApplyOverrides(const FV3DMaterialAssetReferences& References, FglTFRuntimeMaterialsConfig& Config, int32 DefaultFamily = 0)
    {
        for (auto It = Config.MaterialsOverrideByNameMap.CreateIterator(); It; ++It)
            if (It.Key().StartsWith(TEXT("V3D.Slot.")) || It.Key().StartsWith(TEXT("V3D.Policy."))
                || It.Key().StartsWith(TEXT("V3D.DefaultFamily."))
                || It.Key().StartsWith(TEXT("V3D.Reserved."))
                || It.Key() == TEXT("V3D.TerrainPalette")
                || It.Key() == TEXT("V3D.Foliage")) It.RemoveCurrent();
        for (int32 Slot = 0; Slot < FV3DMaterialAssetReferences::SlotCount; ++Slot)
        {
            auto* Material = References.Get(Slot);
            // Null slots select the shared Opaque fallback, never abort a mesh request.
            Config.MaterialsOverrideByNameMap.Add(FString::Printf(TEXT("V3D.Slot.%d"), Slot),
                FV3DMaterialAssetReferences::MatchesSlot(Material, Slot) ? Material : nullptr);
        }
        for (const auto& Pair : References.ReservedMaterials)
            Config.MaterialsOverrideByNameMap.Add(TEXT("V3D.Reserved.") + Pair.Key, Pair.Value.Get());
        Config.MaterialsOverrideByNameMap.Add(FString::Printf(TEXT("V3D.Policy.%d"),
            v3d::normalize_material_mode(References.MaterialMode)), nullptr);
        Config.MaterialsOverrideByNameMap.Add(TEXT("V3D.TerrainPalette"), References.Terrain.Get());
        Config.MaterialsOverrideByNameMap.Add(TEXT("V3D.Foliage"), References.Foliage.Get());
        Config.MaterialsOverrideByNameMap.Add(FString::Printf(TEXT("V3D.DefaultFamily.%d"), FMath::Clamp(DefaultFamily, 0, 2)), nullptr);
        Config.bMaterialsOverrideMapInjectParams = true;
    }
}
