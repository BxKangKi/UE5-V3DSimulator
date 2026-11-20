// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Format/v3d.h"
class UglTFRuntimeAsset;
class USkeleton;
class USkeletalMesh;
class UAnimSequence;
struct FGWorldBakedModel;

/** Source capture and packaged-runtime reconstruction. Caller holds the synchronous native gate. */
namespace V3DModelAnimation
{
    V3DRUNTIME_API bool Capture(UglTFRuntimeAsset* Source, const TMap<FString, FString>& BoneAliases,
        FGWorldBakedModel& Model, FString& Error);
    V3DRUNTIME_API UAnimSequence* Create(const v3d::animation& Clip, USkeletalMesh* Mesh, FString& Error);
}
