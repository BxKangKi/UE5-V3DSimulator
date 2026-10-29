// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "V3DTerrainMaterialPreparation.h"

#include "Materials/Material.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogV3DTerrainMaterialPreparation, Log, All);

void V3DTerrainMaterialPreparation::PrepareShippedMaterial()
{
    static const TCHAR* MaterialPath =
        TEXT("/Game/Resources/Materials/V3D/M_V3DTerrain.M_V3DTerrain");
    UMaterial* Material = LoadObject<UMaterial>(nullptr, MaterialPath);
    if (!Material)
    {
        UE_LOG(LogV3DTerrainMaterialPreparation, Warning,
            TEXT("Cannot prepare the shipped terrain material '%s' for instanced rendering."), MaterialPath);
        return;
    }

    // Do not follow a redirect and rewrite a project's replacement material.
    if (Material->GetPathName() != MaterialPath)
    {
        UE_LOG(LogV3DTerrainMaterialPreparation, Warning,
            TEXT("Terrain material '%s' resolves to '%s'; leaving that replacement asset unchanged."),
            MaterialPath, *Material->GetPathName());
        return;
    }
    if (Material->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes)) return;

    UPackage* Package = Material->GetOutermost();
    Package->FullyLoad();

    // Terrain sections use HISM. A missing usage permutation renders the fallback material
    // after cooking, even when all texture arrays loaded correctly. Set this explicitly:
    // automatic usage detection can be disabled and is not a cook preparation mechanism.
    Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
    Material->PostEditChange();
    Material->MarkPackageDirty();

    const FString Filename = FPackageName::LongPackageNameToFilename(
        Package->GetName(), FPackageName::GetAssetPackageExtension());
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    SaveArgs.bSlowTask = false;
    if (!UPackage::SavePackage(Package, Material, *Filename, SaveArgs))
    {
        UE_LOG(LogV3DTerrainMaterialPreparation, Warning,
            TEXT("Enabled instanced terrain material usage in memory, but could not save '%s'. "
                 "Make the asset writable and save it before future cooks."), *Filename);
        return;
    }

    UE_LOG(LogV3DTerrainMaterialPreparation, Log,
        TEXT("Enabled and saved instanced terrain material usage in '%s'."), *Filename);
}
