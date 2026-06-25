// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/JsonMetadataNormalizer.h"

#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "System/JsonMetadata.h"
#include "System/SafeFileIO.h"

// File-specific scope prevents collisions when Unreal combines .cpp files for Unity builds.
namespace V3DJsonMetadataNormalizerPrivate
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
}
} // namespace V3DJsonMetadataNormalizerPrivate

bool V3DSimulatorJsonMetadataNormalizer::EnsureAssetJson(
    const FString& JsonPath,
    const FString& BaseName,
    const EAssetDefinitionType ExpectedAssetType,
    const EModelDefinitionType DefaultModelType,
    bool& bOutChanged,
    FString& OutError)
{
    bOutChanged = false;
    OutError.Reset();

    TSharedPtr<FJsonObject> Json;
    if (IFileManager::Get().FileExists(*JsonPath))
    {
        const FSafeJsonLoadResult Loaded = FSafeFileIO::LoadJsonBlocking(JsonPath);
        if (!Loaded.IsSuccess() || !Loaded.JsonObject.IsValid())
        {
            OutError = Loaded.Error.IsEmpty() ? TEXT("Existing asset JSON is invalid.") : Loaded.Error;
            return false;
        }
        Json = Loaded.JsonObject;
    }
    else
    {
        Json = MakeShared<FJsonObject>();
        bOutChanged = true;
    }

    FString ExistingAssetType;
    if (Json->TryGetStringField(V3DSimulatorJsonMetadata::AssetType, ExistingAssetType))
    {
        ExistingAssetType.TrimStartAndEndInline();
        const FString Expected = V3DSimulatorAssetTypes::ToString(ExpectedAssetType);
        if (!ExistingAssetType.IsEmpty() && !ExistingAssetType.Equals(Expected, ESearchCase::IgnoreCase))
        {
            OutError = FString::Printf(
                TEXT("AssetType '%s' conflicts with this source file; expected %s."),
                *ExistingAssetType, *Expected);
            return false;
        }
    }

    FString UUIDText;
    FGuid UUID;
    if (!Json->TryGetStringField(V3DSimulatorJsonMetadata::UUID, UUIDText)
        || !FGuid::Parse(UUIDText, UUID) || !UUID.IsValid())
    {
        bOutChanged |= V3DJsonMetadataNormalizerPrivate::SetStringIfDifferent(Json.ToSharedRef(), V3DSimulatorJsonMetadata::UUID,
            FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
    }

    FString Name;
    Json->TryGetStringField(V3DSimulatorJsonMetadata::Name, Name);
    Name.TrimStartAndEndInline();
    if (Name.IsEmpty()) bOutChanged |= V3DJsonMetadataNormalizerPrivate::SetStringIfDifferent(Json.ToSharedRef(), V3DSimulatorJsonMetadata::Name, BaseName);

    FString DisplayName;
    Json->TryGetStringField(V3DSimulatorJsonMetadata::DisplayName, DisplayName);
    DisplayName.TrimStartAndEndInline();
    if (DisplayName.IsEmpty()) bOutChanged |= V3DJsonMetadataNormalizerPrivate::SetStringIfDifferent(Json.ToSharedRef(), V3DSimulatorJsonMetadata::DisplayName, BaseName);

    bOutChanged |= V3DJsonMetadataNormalizerPrivate::SetStringIfDifferent(Json.ToSharedRef(), V3DSimulatorJsonMetadata::Version, V3DSimulatorJsonMetadata::SchemaVersion);
    bOutChanged |= V3DJsonMetadataNormalizerPrivate::SetStringIfDifferent(Json.ToSharedRef(), V3DSimulatorJsonMetadata::AssetType, V3DSimulatorAssetTypes::ToString(ExpectedAssetType));

    if (Json->HasField(V3DSimulatorJsonMetadata::ProjectType))
    {
        Json->RemoveField(V3DSimulatorJsonMetadata::ProjectType);
        bOutChanged = true;
    }

    if (ExpectedAssetType == EAssetDefinitionType::Model)
    {
        FString ModelType;
        Json->TryGetStringField(V3DSimulatorJsonMetadata::ModelType, ModelType);
        ModelType.TrimStartAndEndInline();
        if (ModelType.IsEmpty())
        {
            bOutChanged |= V3DJsonMetadataNormalizerPrivate::SetStringIfDifferent(Json.ToSharedRef(), V3DSimulatorJsonMetadata::ModelType,
                V3DSimulatorModelTypes::ToString(DefaultModelType));
        }
    }
    else if (Json->HasField(V3DSimulatorJsonMetadata::ModelType))
    {
        Json->RemoveField(V3DSimulatorJsonMetadata::ModelType);
        bOutChanged = true;
    }

    if (bOutChanged)
    {
        const FSafeFileWriteResult Saved = FSafeFileIO::SaveJsonBlocking(Json.ToSharedRef(), JsonPath);
        if (!Saved.IsSuccess())
        {
            OutError = Saved.Error.IsEmpty() ? TEXT("Asset JSON upgrade could not be saved.") : Saved.Error;
            return false;
        }
    }
    return true;
}
