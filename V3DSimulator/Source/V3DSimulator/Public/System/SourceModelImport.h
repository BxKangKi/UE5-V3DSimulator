// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "glTFRuntimeParser.h"

class UglTFRuntimeAsset;
class USkeleton;
struct FGWorldBakedSkin;
class FStructOnScope;

/** Format dispatch is intentionally independent of UObjects so recursive discovery can run in
 * parallel. Optional native decoders are resolved only after returning to the game thread. */
namespace V3DSourceModelImport
{
    enum class EFormat : uint8 { Unsupported, Glb, Gltf, Obj, Fbx, Stl };
    V3DSIMULATOR_API EFormat GetFormat(const FString& Path);
    V3DSIMULATOR_API bool IsModelSource(const FString& Path);
    V3DSIMULATOR_API const TCHAR* RequiredPlugin(EFormat Format);
    V3DSIMULATOR_API bool ValidateSourceFile(const FString& Path, FString& OutError);
    V3DSIMULATOR_API bool CheckAvailability(const FString& Path, FString& OutError);
    V3DSIMULATOR_API bool UsesBlob(EFormat Format);
    /** Linear-time validation before any character, LOD or metadata hierarchy traversal. */
    V3DSIMULATOR_API bool ValidateHierarchy(const TArray<FglTFRuntimeNode>& Nodes, FString& OutError);

#if WITH_DEV_AUTOMATION_TESTS
    /** Exercises the same reflected ABI checks without requiring an optional plugin install. */
    V3DSIMULATOR_API bool DecodeWithLibraryForTesting(UClass* Library, UglTFRuntimeAsset* Asset,
        const FglTFRuntimeMaterialsConfig& Materials, FglTFRuntimeMeshLOD& OutLOD, FString& OutError);
#endif

    /** Build-only view of native glTF metadata or an extension's blob metadata. This never
     * changes the original glTF parser JSON (terrain and extension materials still need it).
     * All methods except static format/preflight helpers must run under the native gate on GT.
     * The owner must retain Asset via UPROPERTY until the session has been released. */
    class V3DSIMULATOR_API FSession
    {
    public:
        explicit FSession(EFormat InFormat) : Format(InFormat) {}
        ~FSession();
        bool Initialize(UglTFRuntimeAsset* Asset, const FString& SourcePath, FString& OutError);
        bool DecodeMesh(UglTFRuntimeAsset* Asset, int32 MeshIndex,
            const FglTFRuntimeMaterialsConfig& Materials, FglTFRuntimeMeshLOD& OutLOD,
            bool& bOutSkeletal, FString& OutError);
        bool CaptureSkin(FglTFRuntimeMeshLOD& LOD, int32 MeshIndex,
            const TMap<FString, FString>& BoneAliases, USkeleton* TargetSkeleton,
            FGWorldBakedSkin& OutSkin, FString& OutError);

        EFormat Format;
        TArray<FglTFRuntimeNode> Nodes;
        TArray<FString> MeshNames;
    private:
        TWeakObjectPtr<UClass> LibraryClass;
        TArray<TSharedPtr<FStructOnScope>> MeshNodes;
    };
}
