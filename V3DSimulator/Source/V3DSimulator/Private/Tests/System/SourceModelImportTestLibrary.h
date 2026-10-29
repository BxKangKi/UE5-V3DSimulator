// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "Kismet/BlueprintFunctionLibrary.h"
#include "glTFRuntimeParser.h"
#include "SourceModelImportTestLibrary.generated.h"

class UglTFRuntimeAsset;

// UHT must see native mock declarations in every target. These are not BlueprintCallable and
// have no content assets; automation invokes them only to verify the optional importer's ABI.
UCLASS(Transient, NotBlueprintable)
class UV3DSourceImportCompatibleTestLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION()
    static bool DecodeOBJRuntimeLOD(UglTFRuntimeAsset* Asset,
        const FglTFRuntimeMaterialsConfig& StaticMaterials,
        const FglTFRuntimeMaterialsConfig& SkeletalMaterials,
        FglTFRuntimeMeshLOD& RuntimeLOD, bool& bIsSkeletal);
};

UCLASS(Transient, NotBlueprintable)
class UV3DSourceImportAmbiguousTestLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION()
    static bool FirstOBJRuntimeLOD(UglTFRuntimeAsset* Asset, FglTFRuntimeMeshLOD& RuntimeLOD);
    UFUNCTION()
    static bool SecondOBJRuntimeLOD(UglTFRuntimeAsset* Asset, FglTFRuntimeMeshLOD& RuntimeLOD);
};

UCLASS(Transient, NotBlueprintable)
class UV3DSourceImportRequiredArgTestLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION()
    static bool LoadOBJAsRuntimeLOD(UglTFRuntimeAsset* Asset, int32 RequiredMeshIndex, FglTFRuntimeMeshLOD& RuntimeLOD);
};
