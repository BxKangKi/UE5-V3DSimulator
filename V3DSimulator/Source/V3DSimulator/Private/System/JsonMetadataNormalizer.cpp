// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/JsonMetadataNormalizer.h"

#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "System/JsonMetadata.h"
#include "System/SafeFileIO.h"

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
        FString Value;
        FGuid Id;
        const bool ValidMetadata = Json->TryGetStringField(V3DSimulatorJsonMetadata::UUID, Value)
            && FGuid::Parse(Value, Id) && Id.IsValid()
            && Json->TryGetStringField(V3DSimulatorJsonMetadata::Name, Value) && !Value.TrimStartAndEnd().IsEmpty()
            && Json->TryGetStringField(V3DSimulatorJsonMetadata::DisplayName, Value) && !Value.TrimStartAndEnd().IsEmpty()
            && Json->TryGetStringField(V3DSimulatorJsonMetadata::Version, Value)
            && Value == V3DSimulatorJsonMetadata::SchemaVersion
            && Json->TryGetStringField(V3DSimulatorJsonMetadata::AssetType, Value)
            && Value == V3DSimulatorAssetTypes::ToString(ExpectedAssetType)
            && !Json->HasField(V3DSimulatorJsonMetadata::ProjectType);
        if (!ValidMetadata)
        { OutError = TEXT("Existing asset JSON must use the canonical metadata schema; no migration is performed"); return false; }
        if (ExpectedAssetType == EAssetDefinitionType::Model)
        {
            EModelDefinitionType Type;
            if (!Json->TryGetStringField(V3DSimulatorJsonMetadata::ModelType, Value)
                || !V3DSimulatorModelTypes::TryParse(Value, Type))
            { OutError = TEXT("Existing model JSON requires canonical ModelType"); return false; }
        }
        else if (Json->HasField(V3DSimulatorJsonMetadata::ModelType))
        { OutError = TEXT("Sound JSON cannot contain ModelType"); return false; }
        return true; // Validation-only for author-owned documents.

    }
    else
    {
        Json = MakeShared<FJsonObject>();
        bOutChanged = true;
    }

    Json->SetStringField(V3DSimulatorJsonMetadata::UUID, FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
    Json->SetStringField(V3DSimulatorJsonMetadata::Name, BaseName);
    Json->SetStringField(V3DSimulatorJsonMetadata::DisplayName, BaseName);
    Json->SetStringField(V3DSimulatorJsonMetadata::Version, V3DSimulatorJsonMetadata::SchemaVersion);
    Json->SetStringField(V3DSimulatorJsonMetadata::AssetType, V3DSimulatorAssetTypes::ToString(ExpectedAssetType));
    if (ExpectedAssetType == EAssetDefinitionType::Model)
        Json->SetStringField(V3DSimulatorJsonMetadata::ModelType, V3DSimulatorModelTypes::ToString(DefaultModelType));
    const FSafeFileWriteResult Saved = FSafeFileIO::SaveJsonBlocking(Json.ToSharedRef(), JsonPath);
    if (!Saved.IsSuccess()) { OutError = Saved.Error; return false; }
    return true;
}
