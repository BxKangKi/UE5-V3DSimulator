// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "System/WorldSurfaceConfig.h"
#include "System/SimulationCommandConfig.h"

/** Three independent immutable archive members; authoring config.json contains no script payload. */
struct V3DRUNTIME_API FV3DProjectScripts
{
    static constexpr int64 MaxScriptBytes = 2ll * 1024ll * 1024ll;
    FString MaterialsJson = TEXT("{}");
    FString SoundsJson = TEXT("{\"PhysicalMaterials\":{},\"MaterialPhysicalMappings\":[],\"SoundLists\":{},\"Events\":{},\"FootstepMappings\":{}}");
    FString CommandsJson = TEXT("{\"Version\":1,\"Commands\":[],\"Triggers\":[]}");

    static const TCHAR* MemberName(int32 Index);
    FString& Text(int32 Index);
    const FString& Text(int32 Index) const;
    /** Read JSON only. Missing files use empty defaults. Parse validates the combined schema. */
    static bool Load(const FString& ProjectRoot,
        FV3DProjectScripts& Out, FString& Error);
    static bool CreateDefaults(const FString& ProjectRoot, FString& Error);
    bool Parse(FV3DWorldSurfaceConfig& Surfaces, FV3DSimulationCommandConfig& Commands, FString& Error) const;
};
