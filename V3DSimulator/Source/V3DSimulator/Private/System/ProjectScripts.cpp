// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/ProjectScripts.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "System/SafeFileIO.h"

namespace
{
    FSafeJsonLimits ScriptLimits()
    {
        FSafeJsonLimits Limits;
        Limits.MaxFileBytes = FV3DProjectScripts::MaxScriptBytes;
        Limits.MaxDepth = 16; Limits.MaxValues = 100000;
        Limits.bAllowBackupRecovery = false;
        return Limits;
    }
    bool SerializeScript(const TSharedRef<FJsonObject>& Json, FString& Text)
    {
        Text.Reset();
        const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
        return FJsonSerializer::Serialize(Json, Writer);
    }
}

const TCHAR* FV3DProjectScripts::MemberName(int32 Index)
{
    check(Index >= 0 && Index < 3);
    const TCHAR* Names[] = {TEXT("scripts/materials.json"), TEXT("scripts/sounds.json"), TEXT("scripts/commands.json")};
    return Names[Index];
}
FString& FV3DProjectScripts::Text(int32 Index)
{
    check(Index >= 0 && Index < 3);
    return Index == 0 ? MaterialsJson : (Index == 1 ? SoundsJson : CommandsJson);
}
const FString& FV3DProjectScripts::Text(int32 Index) const
{
    check(Index >= 0 && Index < 3);
    return Index == 0 ? MaterialsJson : (Index == 1 ? SoundsJson : CommandsJson);
}

void FV3DProjectScripts::FromLegacyConfig(const TSharedPtr<FJsonObject>& Json, FV3DProjectScripts& Out)
{
    Out = FV3DProjectScripts();
    if (!Json.IsValid()) return;
    auto Materials = MakeShared<FJsonObject>(); auto Sounds = MakeShared<FJsonObject>();
    for (const TCHAR* Key : {TEXT("PhysicalMaterials"), TEXT("DefaultPhysicalMaterial"), TEXT("MaterialPhysicalMappings")})
        if (const auto* Value = Json->Values.Find(Key)) Materials->SetField(Key, *Value);
    if (const auto* Value = Json->Values.Find(TEXT("FootstepMappings"))) Sounds->SetField(TEXT("FootstepMappings"), *Value);
    SerializeScript(Materials, Out.MaterialsJson); SerializeScript(Sounds, Out.SoundsJson);
}
void FV3DProjectScripts::RemoveLegacyFields(FJsonObject& Json)
{
    for (const TCHAR* Key : {TEXT("PhysicalMaterials"), TEXT("DefaultPhysicalMaterial"), TEXT("MaterialPhysicalMappings"), TEXT("FootstepMappings")}) Json.RemoveField(Key);
}

bool FV3DProjectScripts::Load(const FString& ProjectRoot, const TSharedPtr<FJsonObject>& LegacyConfig, FV3DProjectScripts& Out, FString& Error)
{
    Error.Reset(); FV3DProjectScripts Parsed;
    FromLegacyConfig(LegacyConfig, Parsed);
    for (int32 Index = 0; Index < 3; ++Index)
    {
        const FString Path = FPaths::Combine(ProjectRoot, MemberName(Index));
        const auto Loaded = FSafeFileIO::LoadJsonBlocking(Path, ScriptLimits());
        if (Loaded.Status == ESafeFileIOStatus::Missing) continue;
        if (!Loaded.IsSuccess() || !SerializeScript(Loaded.JsonObject.ToSharedRef(), Parsed.Text(Index)))
        { Error = Path + TEXT(": ") + Loaded.Error; return false; }
    }
    Out = MoveTemp(Parsed); return true;
}

bool FV3DProjectScripts::CreateDefaults(const FString& ProjectRoot, FString& Error)
{
    Error.Reset();
    if (!IFileManager::Get().MakeDirectory(*FPaths::Combine(ProjectRoot, TEXT("scripts")), true))
    { Error = TEXT("Could not create the project scripts directory."); return false; }
    const FV3DProjectScripts Defaults;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        const FString Path = FPaths::Combine(ProjectRoot, MemberName(Index));
        // Creation only: do not rewrite an author's script or produce a backup.
        if (IFileManager::Get().FileExists(*Path)) continue;
        const auto Json = FSafeFileIO::ParseJsonText(Defaults.Text(Index), Path, ScriptLimits());
        if (!Json.IsSuccess()) { Error = Json.Error; return false; }
        const auto Saved = FSafeFileIO::SaveJsonBlocking(Json.JsonObject.ToSharedRef(), Path);
        if (!Saved.IsSuccess()) { Error = Saved.Error; return false; }
    }
    return true;
}

bool FV3DProjectScripts::Parse(FV3DWorldSurfaceConfig& Surfaces, FV3DSimulationCommandConfig& Commands, FString& Error) const
{
    const auto Materials = FSafeFileIO::ParseJsonText(MaterialsJson, MemberName(0), ScriptLimits());
    const auto Sounds = FSafeFileIO::ParseJsonText(SoundsJson, MemberName(1), ScriptLimits());
    const auto CommandJson = FSafeFileIO::ParseJsonText(CommandsJson, MemberName(2), ScriptLimits());
    if (!Materials.IsSuccess()) { Error = Materials.Error; return false; }
    if (!Sounds.IsSuccess()) { Error = Sounds.Error; return false; }
    if (!CommandJson.IsSuccess()) { Error = CommandJson.Error; return false; }
    // Compose a native parsing view only; neither config.json nor either script is modified.
    const auto SurfaceJson = MakeShared<FJsonObject>();
    for (const auto& Field : Materials.JsonObject->Values)
    {
        if (Field.Key != TEXT("PhysicalMaterials") && Field.Key != TEXT("DefaultPhysicalMaterial") && Field.Key != TEXT("MaterialPhysicalMappings"))
        { Error = FString::Printf(TEXT("Unknown scripts/materials.json field: %s"), *Field.Key); return false; }
        SurfaceJson->SetField(Field.Key, Field.Value);
    }
    for (const auto& Field : Sounds.JsonObject->Values)
    {
        if (Field.Key != TEXT("FootstepMappings")) { Error = FString::Printf(TEXT("Unknown scripts/sounds.json field: %s"), *Field.Key); return false; }
        SurfaceJson->SetField(Field.Key, Field.Value);
    }
    if (!FV3DWorldSurfaceConfig::Parse(SurfaceJson, Surfaces, Error)) return false;
    return FV3DSimulationCommandConfig::Parse(CommandJson.JsonObject, Commands, Error);
}
