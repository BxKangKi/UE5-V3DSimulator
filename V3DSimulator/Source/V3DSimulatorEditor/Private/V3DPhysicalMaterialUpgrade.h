#pragma once
#include "CoreTypes.h"

class UMaterialExpressionGetMaterialAttributes;
namespace V3DMaterialDefaults
{
    void UpgradePhysicalMaterials();
    void UpgradeCelestialMaterials();
    void UpgradeWaterReflectionMaterials();
    int32 EnsureWaterOpacityOutput(UMaterialExpressionGetMaterialAttributes* Read, bool& Changed);
}
