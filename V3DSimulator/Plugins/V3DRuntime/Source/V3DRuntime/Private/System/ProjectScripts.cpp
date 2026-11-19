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

bool FV3DProjectScripts::Load(const FString& ProjectRoot, FV3DProjectScripts& Out, FString& Error)
{
    Error.Reset(); FV3DProjectScripts Parsed;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        const FString Path = FPaths::Combine(ProjectRoot, MemberName(Index));
        const auto Loaded = FSafeFileIO::LoadJsonBlocking(Path, ScriptLimits());
        if (Loaded.Status == ESafeFileIOStatus::Missing) continue;
        if (!Loaded.IsSuccess() || !Loaded.JsonObject.IsValid())
        { Error = Path + TEXT(": ") + Loaded.Error; return false; }
        if (!SerializeScript(Loaded.JsonObject.ToSharedRef(), Parsed.Text(Index)))
        { Error = Path + TEXT(": JSON serialization failed"); return false; }
    }
    // The caller parses once into its native surface/command configuration before publishing.
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
    if (!Materials.JsonObject->Values.IsEmpty())
    { Error = TEXT("scripts/materials.json must be empty. Surface and sound settings belong in scripts/sounds.json."); return false; }
    for (const auto& Field : Sounds.JsonObject->Values)
    {
        if (Field.Key != TEXT("FootstepMappings") && Field.Key != TEXT("SoundLists") && Field.Key != TEXT("Events")
            && Field.Key != TEXT("PhysicalMaterials") && Field.Key != TEXT("DefaultPhysicalMaterial") && Field.Key != TEXT("MaterialPhysicalMappings")) { Error = FString::Printf(TEXT("Unknown scripts/sounds.json field: %s"), *Field.Key); return false; }
    }
    if (!FV3DWorldSurfaceConfig::Parse(Sounds.JsonObject, Surfaces, Error)) return false;
    return FV3DSimulationCommandConfig::Parse(CommandJson.JsonObject, Commands, Error);
}

