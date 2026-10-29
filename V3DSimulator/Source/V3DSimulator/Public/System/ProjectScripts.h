// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "System/WorldSurfaceConfig.h"
#include "System/SimulationCommandConfig.h"

/** Three independent immutable archive members; authoring config.json contains no script payload. */
struct V3DSIMULATOR_API FV3DProjectScripts
{
    static constexpr int64 MaxScriptBytes = 2ll * 1024ll * 1024ll;
    FString MaterialsJson = TEXT("{\"PhysicalMaterials\":{},\"MaterialPhysicalMappings\":[]}");
    FString SoundsJson = TEXT("{\"FootstepMappings\":{}}");
    FString CommandsJson = TEXT("{\"Version\":1,\"Commands\":[],\"Triggers\":[]}");

    static const TCHAR* MemberName(int32 Index);
    FString& Text(int32 Index);
    const FString& Text(int32 Index) const;
    /** Read only. Missing files use empty defaults (or legacy config surface fields). */
    static bool Load(const FString& ProjectRoot, const TSharedPtr<FJsonObject>& LegacyConfig,
        FV3DProjectScripts& Out, FString& Error);
    static bool CreateDefaults(const FString& ProjectRoot, FString& Error);
    static void FromLegacyConfig(const TSharedPtr<FJsonObject>& Json, FV3DProjectScripts& Out);
    static void RemoveLegacyFields(FJsonObject& Json);
    bool Parse(FV3DWorldSurfaceConfig& Surfaces, FV3DSimulationCommandConfig& Commands, FString& Error) const;
};
