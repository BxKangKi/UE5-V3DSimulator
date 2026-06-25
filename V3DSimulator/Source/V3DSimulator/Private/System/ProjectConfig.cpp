// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/ProjectConfig.h"

#include "Misc/Guid.h"
#include "System/JsonMetadata.h"
#include "System/SafeFileIO.h"

// File-specific scope prevents collisions when Unreal combines .cpp files for Unity builds.
namespace V3DProjectConfigPrivate
{
namespace
{
    bool SetStringIfDifferent(const TSharedRef<FJsonObject>& Json, const TCHAR* Key, const FString& Value)
    {
        FString Existing;
        if (Json->TryGetStringField(Key, Existing) && Existing == Value) return false;
        Json->SetStringField(Key, Value);
        return true;
    }

    bool SetBoolIfDifferent(const TSharedRef<FJsonObject>& Json, const TCHAR* Key, const bool Value)
    {
        bool Existing = false;
        if (Json->TryGetBoolField(Key, Existing) && Existing == Value) return false;
        Json->SetBoolField(Key, Value);
        return true;
    }

    bool RemoveIfPresent(const TSharedRef<FJsonObject>& Json, const TCHAR* Key)
    {
        if (!Json->HasField(Key)) return false;
        Json->RemoveField(Key);
        return true;
    }
}
} // namespace V3DProjectConfigPrivate

FString FV3DSimulatorProjectConfig::GetDisplayName(const FString& FolderFallback) const
{
    if (!Name.IsEmpty()) return Name;
    if (ProjectType == EV3DSimulatorProjectType::World && !WorldName.IsEmpty()) return WorldName;
    return FolderFallback;
}

bool V3DSimulatorProjectConfig::Normalize(
    const TSharedRef<FJsonObject>& Json,
    const FString& FolderFallback,
    bool& bOutChanged,
    FString& OutError)
{
    bOutChanged = false;
    OutError.Reset();

    const FString SafeFallback = FolderFallback.TrimStartAndEnd();
    if (SafeFallback.IsEmpty())
    {
        OutError = TEXT("Project folder name is empty.");
        return false;
    }

    FString TypeText;
    EV3DSimulatorProjectType Type = EV3DSimulatorProjectType::World;
    if (!Json->TryGetStringField(V3DSimulatorJsonMetadata::ProjectType, TypeText) || TypeText.TrimStartAndEnd().IsEmpty())
    {
        TypeText = TEXT("World");
        bOutChanged |= V3DProjectConfigPrivate::SetStringIfDifferent(Json, V3DSimulatorJsonMetadata::ProjectType, TypeText);
    }
    if (!V3DSimulatorProjectTypes::TryParse(TypeText, Type))
    {
        OutError = FString::Printf(TEXT("Unsupported ProjectType '%s'."), *TypeText);
        return false;
    }
    bOutChanged |= V3DProjectConfigPrivate::SetStringIfDifferent(Json, V3DSimulatorJsonMetadata::ProjectType, V3DSimulatorProjectTypes::ToString(Type));

    FString UUIDText;
    FGuid UUID;
    if (!Json->TryGetStringField(V3DSimulatorJsonMetadata::UUID, UUIDText)
        || !FGuid::Parse(UUIDText, UUID) || !UUID.IsValid())
    {
        bOutChanged |= V3DProjectConfigPrivate::SetStringIfDifferent(
            Json, V3DSimulatorJsonMetadata::UUID,
            FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
    }

    FString Name;
    Json->TryGetStringField(V3DSimulatorJsonMetadata::Name, Name);
    Name.TrimStartAndEndInline();
    if (Name.IsEmpty()) Name = SafeFallback;
    bOutChanged |= V3DProjectConfigPrivate::SetStringIfDifferent(Json, V3DSimulatorJsonMetadata::Name, Name);
    bOutChanged |= V3DProjectConfigPrivate::SetStringIfDifferent(Json, V3DSimulatorJsonMetadata::Version, V3DSimulatorJsonMetadata::SchemaVersion);

    // config.json is a Project document, never an Asset document.
    bOutChanged |= V3DProjectConfigPrivate::RemoveIfPresent(Json, V3DSimulatorJsonMetadata::AssetType);
    bOutChanged |= V3DProjectConfigPrivate::RemoveIfPresent(Json, V3DSimulatorJsonMetadata::ModelType);

    bool bAllow = false;
    if (Type == EV3DSimulatorProjectType::World)
    {
        if (!Json->TryGetBoolField(V3DSimulatorJsonMetadata::AllowProjectAssets, bAllow))
        {
            bOutChanged |= V3DProjectConfigPrivate::SetBoolIfDifferent(Json, V3DSimulatorJsonMetadata::AllowProjectAssets, false);
        }
        FString WorldName;
        Json->TryGetStringField(V3DSimulatorJsonMetadata::WorldName, WorldName);
        WorldName.TrimStartAndEndInline();
        if (WorldName.IsEmpty()) bOutChanged |= V3DProjectConfigPrivate::SetStringIfDifferent(Json, V3DSimulatorJsonMetadata::WorldName, Name);
    }
    else
    {
        bOutChanged |= V3DProjectConfigPrivate::RemoveIfPresent(Json, V3DSimulatorJsonMetadata::AllowProjectAssets);
        bOutChanged |= V3DProjectConfigPrivate::RemoveIfPresent(Json, V3DSimulatorJsonMetadata::WorldName);
    }
    return true;
}

bool V3DSimulatorProjectConfig::Parse(
    const TSharedPtr<FJsonObject>& Json,
    const FString& FolderFallback,
    FV3DSimulatorProjectConfig& OutConfig,
    FString& OutError)
{
    OutConfig = FV3DSimulatorProjectConfig();
    OutError.Reset();
    if (!Json.IsValid()) { OutError = TEXT("Project config.json is invalid."); return false; }

    FString TypeText;
    FString UUIDText;
    if (!Json->TryGetStringField(V3DSimulatorJsonMetadata::ProjectType, TypeText)
        || !V3DSimulatorProjectTypes::TryParse(TypeText, OutConfig.ProjectType))
    {
        OutError = TEXT("Project config requires a valid ProjectType.");
        return false;
    }
    if (!Json->TryGetStringField(V3DSimulatorJsonMetadata::UUID, UUIDText)
        || !FGuid::Parse(UUIDText, OutConfig.UUID) || !OutConfig.UUID.IsValid())
    {
        OutError = TEXT("Project config requires a valid UUID.");
        return false;
    }
    if (!Json->TryGetStringField(V3DSimulatorJsonMetadata::Name, OutConfig.Name))
    {
        OutError = TEXT("Project config requires Name.");
        return false;
    }
    OutConfig.Name.TrimStartAndEndInline();
    if (OutConfig.Name.IsEmpty()) { OutError = TEXT("Project Name is empty."); return false; }
    Json->TryGetStringField(V3DSimulatorJsonMetadata::Version, OutConfig.Version);
    if (OutConfig.Version.IsEmpty()) { OutError = TEXT("Project config requires Version."); return false; }

    if (OutConfig.ProjectType == EV3DSimulatorProjectType::World)
    {
        Json->TryGetStringField(V3DSimulatorJsonMetadata::WorldName, OutConfig.WorldName);
        OutConfig.WorldName.TrimStartAndEndInline();
        if (OutConfig.WorldName.IsEmpty()) OutConfig.WorldName = OutConfig.Name.IsEmpty() ? FolderFallback : OutConfig.Name;
        Json->TryGetBoolField(V3DSimulatorJsonMetadata::AllowProjectAssets, OutConfig.bAllowProjectAssets);
    }
    return true;
}

bool V3DSimulatorProjectConfig::Load(
    const FString& ConfigPath,
    const FString& FolderFallback,
    FV3DSimulatorProjectConfig& OutConfig,
    TSharedPtr<FJsonObject>* OutJson,
    FString& OutError)
{
    FSafeJsonLimits Limits;
    Limits.MaxFileBytes = 64ll * 1024ll * 1024ll;
    Limits.MaxDepth = 32;
    Limits.MaxValues = 131072;
    Limits.MaxContainerEntries = 65536;
    Limits.MaxStringCharacters = 32768;
    Limits.bAllowBackupRecovery = false;

    const FSafeJsonLoadResult Loaded = FSafeFileIO::LoadJsonBlocking(ConfigPath, Limits);
    if (!Loaded.IsSuccess() || !Loaded.JsonObject.IsValid())
    {
        OutError = Loaded.Error.IsEmpty() ? TEXT("Project config.json could not be read.") : Loaded.Error;
        if (OutJson) OutJson->Reset();
        return false;
    }

    bool bChanged = false;
    if (!Normalize(Loaded.JsonObject.ToSharedRef(), FolderFallback, bChanged, OutError))
    {
        if (OutJson) *OutJson = Loaded.JsonObject;
        return false;
    }
    if (bChanged)
    {
        const FSafeFileWriteResult Save = FSafeFileIO::SaveJsonBlocking(Loaded.JsonObject.ToSharedRef(), ConfigPath);
        if (!Save.IsSuccess())
        {
            OutError = FString::Printf(TEXT("Project config upgrade failed: %s"), *Save.Error);
            return false;
        }
    }
    if (!Parse(Loaded.JsonObject, FolderFallback, OutConfig, OutError)) return false;
    if (OutJson) *OutJson = Loaded.JsonObject;
    return true;
}
