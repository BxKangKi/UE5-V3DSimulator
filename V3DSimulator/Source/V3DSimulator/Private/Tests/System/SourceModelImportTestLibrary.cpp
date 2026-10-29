// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "SourceModelImportTestLibrary.h"
#include "glTFRuntimeAsset.h"

bool UV3DSourceImportCompatibleTestLibrary::DecodeOBJRuntimeLOD(UglTFRuntimeAsset* Asset,
    const FglTFRuntimeMaterialsConfig& StaticMaterials,
    const FglTFRuntimeMaterialsConfig& SkeletalMaterials,
    FglTFRuntimeMeshLOD& RuntimeLOD, bool& bIsSkeletal)
{
    bIsSkeletal = false;
    if (!IsValid(Asset) || StaticMaterials.SpecularFactor != 0.25f || SkeletalMaterials.SpecularFactor != 0.25f) return false;
    FglTFRuntimePrimitive& Primitive = RuntimeLOD.Primitives.AddDefaulted_GetRef();
    Primitive.Positions.Add(FVector(123.0, 456.0, 789.0));
    return true;
}
bool UV3DSourceImportAmbiguousTestLibrary::FirstOBJRuntimeLOD(UglTFRuntimeAsset*, FglTFRuntimeMeshLOD&) { return true; }
bool UV3DSourceImportAmbiguousTestLibrary::SecondOBJRuntimeLOD(UglTFRuntimeAsset*, FglTFRuntimeMeshLOD&) { return true; }
bool UV3DSourceImportRequiredArgTestLibrary::LoadOBJAsRuntimeLOD(UglTFRuntimeAsset*, int32, FglTFRuntimeMeshLOD&) { return true; }
