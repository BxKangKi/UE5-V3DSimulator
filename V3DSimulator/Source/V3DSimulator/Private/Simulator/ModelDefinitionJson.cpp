/**
 * @file ModelDefinitionJson.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Simulator/ModelDefinitionJson.h"

#include "Character/CharacterBoneSchema.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "System/SafeFileIO.h"
#include "System/JsonMetadata.h"
#include "System/JsonMetadataNormalizer.h"
#include "System/ProjectBuildPolicy.h"


bool ModelDefinitionJson::EnsureDefinitions(
    const FString& ModelRootDirectory,
    const EV3DSimulatorProjectType ProjectType,
    TArray<FString>* OutUpdatedJsonFiles,
    TArray<FString>* OutDiscoveredGlbFiles)
{
    if (OutUpdatedJsonFiles != nullptr)
    {
        OutUpdatedJsonFiles->Reset();
    }
    if (OutDiscoveredGlbFiles != nullptr)
    {
        OutDiscoveredGlbFiles->Reset();
    }

    if (ModelRootDirectory.IsEmpty()
        || !IFileManager::Get().DirectoryExists(*ModelRootDirectory))
    {
        return false;
    }

    TArray<FString> GlbFiles;
    IFileManager::Get().FindFilesRecursive(
        GlbFiles,
        *ModelRootDirectory,
        // *.* is used instead of an extension wildcard so .glb, .GLB and mixed-case variants are
        // returned consistently by platform file implementations. The extension is filtered below.
        TEXT("*.*"),
        true,
        false,
        false);

    // Wildcard extension matching is case-sensitive on some packaged platforms. Enumerate the
    // authoring root once, then apply the extension contract explicitly and consistently.
    GlbFiles.RemoveAllSwap(
        [](const FString& Path)
        {
            return !FPaths::GetExtension(Path).Equals(TEXT("glb"), ESearchCase::IgnoreCase);
        },
        EAllowShrinking::No);

    // Normalize and de-duplicate before JSON generation. This exact ordered list is handed back to
    // ModelDatabaseSubsystem, so discovery and build cannot accidentally operate on different
    // recursive scans while an authoring tool is saving the project authoring tree.
    TSet<FString> SeenPaths;
    TArray<FString> NormalizedGlbFiles;
    NormalizedGlbFiles.Reserve(GlbFiles.Num());
    for (const FString& CandidatePath : GlbFiles)
    {
        FString Path = FSafeFileIO::NormalizeFilePath(CandidatePath);
        if (!Path.IsEmpty() && IFileManager::Get().FileExists(*Path)
            && !SeenPaths.Contains(Path))
        {
            SeenPaths.Add(Path);
            NormalizedGlbFiles.Add(MoveTemp(Path));
        }
    }
    GlbFiles = MoveTemp(NormalizedGlbFiles);
    constexpr int32 MaxGeneratedDefinitions = 100000; // Must not exceed the .v3d model cap.
    if (GlbFiles.Num() > MaxGeneratedDefinitions)
    {
        UE_LOG(LogTemp, Error,
            TEXT("Model definition generation rejected more than %d GLB files under %s"),
            MaxGeneratedDefinitions, *ModelRootDirectory);
        return false;
    }

    GlbFiles.Sort();
    if (OutDiscoveredGlbFiles != nullptr)
    {
        *OutDiscoveredGlbFiles = GlbFiles;
    }
    bool bAllWritesSucceeded = true;
    const EModelDefinitionType DefaultType = V3DSimulatorProjectBuildPolicy::DefaultModelType(ProjectType);
    for (const FString& GlbPath : GlbFiles)
    {
        const FString JsonPath = FPaths::ChangeExtension(GlbPath, TEXT("json"));
        bool bChanged = false;
        FString Error;
        if (!V3DSimulatorJsonMetadataNormalizer::EnsureAssetJson(
                JsonPath, FPaths::GetBaseFilename(GlbPath), EAssetDefinitionType::Model,
                DefaultType, bChanged, Error))
        {
            UE_LOG(LogTemp, Error, TEXT("Model JSON normalization failed: %s (%s)"), *JsonPath, *Error);
            bAllWritesSucceeded = false;
            continue;
        }
        if (bChanged && OutUpdatedJsonFiles) OutUpdatedJsonFiles->Add(JsonPath);
    }

    return bAllWritesSucceeded;
}

bool ModelDefinitionJson::LoadDefinition(
    const FString& JsonPath,
    const FString& ExpectedGlbPath,
    FModelDefinition& OutDefinition,
    FString& OutError,
    FString* OutCanonicalJson)
{
    OutDefinition = FModelDefinition();
    OutError.Reset();
    if (OutCanonicalJson) OutCanonicalJson->Reset();

    FSafeJsonLimits Limits;
    Limits.MaxFileBytes = 16ll * 1024ll * 1024ll;
    Limits.MaxDepth = 32;
    Limits.MaxValues = 131072;
    Limits.MaxContainerEntries = 65536;
    Limits.MaxStringCharacters = 32768;
    Limits.bAllowBackupRecovery = false;
    const FSafeJsonLoadResult Loaded = FSafeFileIO::LoadJsonBlocking(JsonPath, Limits);
    if (!Loaded.IsSuccess() || !Loaded.JsonObject.IsValid())
    {
        OutError = Loaded.Error.IsEmpty() ? TEXT("JSON document is invalid") : Loaded.Error;
        return false;
    }

    const TSharedPtr<FJsonObject>& Root = Loaded.JsonObject;
    FString UUIDText;
    FString AssetTypeText;
    FString TypeText;
    if (!Root->TryGetStringField(V3DSimulatorJsonMetadata::AssetType, AssetTypeText))
    {
        OutError = TEXT("required AssetType field is missing");
        return false;
    }
    AssetTypeText.TrimStartAndEndInline();
    EAssetDefinitionType ParsedAssetType = EAssetDefinitionType::Sound;
    if (!V3DSimulatorAssetTypes::TryParse(AssetTypeText, ParsedAssetType)
        || ParsedAssetType != EAssetDefinitionType::Model)
    {
        OutError = FString::Printf(TEXT("AssetType '%s' cannot be paired with a GLB; expected Model"), *AssetTypeText);
        return false;
    }
    OutDefinition.AssetType = EAssetDefinitionType::Model;
    if (!Root->TryGetStringField(V3DSimulatorJsonMetadata::UUID, UUIDText) || !FGuid::Parse(UUIDText, OutDefinition.UUID))
    {
        OutError = TEXT("required UUID is missing or is not a valid UUID");
        return false;
    }
    if (!Root->TryGetStringField(V3DSimulatorJsonMetadata::Name, OutDefinition.Name) || OutDefinition.Name.TrimStartAndEnd().IsEmpty()
        || !Root->TryGetStringField(V3DSimulatorJsonMetadata::DisplayName, OutDefinition.DisplayName) || OutDefinition.DisplayName.TrimStartAndEnd().IsEmpty())
    {
        OutError = TEXT("required Name or DisplayName field is missing/empty");
        return false;
    }
    OutDefinition.Name.TrimStartAndEndInline();
    OutDefinition.DisplayName.TrimStartAndEndInline();
    if (FPaths::GetCleanFilename(OutDefinition.Name) != OutDefinition.Name
        || OutDefinition.Name == TEXT(".") || OutDefinition.Name == TEXT("..")
        || OutDefinition.Name.Contains(TEXT("/")) || OutDefinition.Name.Contains(TEXT("\\")))
    {
        OutError = TEXT("Name must be a single safe identifier, without path separators");
        return false;
    }

    FString VersionText;
    if (!Root->TryGetStringField(V3DSimulatorJsonMetadata::Version, VersionText)
        || VersionText.TrimStartAndEnd().IsEmpty())
    {
        OutError = TEXT("required Version field is missing/empty");
        return false;
    }

    if (!Root->TryGetStringField(V3DSimulatorJsonMetadata::ModelType, TypeText))
    {
        OutError = TEXT("required ModelType field is missing");
        return false;
    }
    TypeText.TrimStartAndEndInline();
    if (!V3DSimulatorModelTypes::TryParse(TypeText, OutDefinition.ModelType)
        || OutDefinition.ModelType == EModelDefinitionType::Invalid)
    {
        OutError = FString::Printf(
            TEXT("unsupported or case-mismatched ModelType '%s'; expected Static, Dynamic, or Character"),
            *TypeText);
        return false;
    }

    if (OutDefinition.ModelType == EModelDefinitionType::Dynamic)
    {
        FString EntitySubtype;
        FString ItemSubtype;
        const bool bHasEntityType = Root->TryGetStringField(TEXT("EntityType"), EntitySubtype);
        const bool bHasItemType = Root->TryGetStringField(TEXT("ItemType"), ItemSubtype);
        if (bHasEntityType && bHasItemType)
        {
            OutError = TEXT("Dynamic cannot specify both EntityType and ItemType");
            return false;
        }
        if (bHasEntityType)
        {
            if (EntitySubtype == TEXT("Vehicle")) OutDefinition.EntityType = EModelEntityType::Vehicle;
            else if (EntitySubtype == TEXT("Prop")) OutDefinition.EntityType = EModelEntityType::Prop;
            else if (EntitySubtype == TEXT("Animal")) OutDefinition.EntityType = EModelEntityType::Animal;
            else
            {
                OutError = FString::Printf(TEXT("unsupported EntityType '%s'"), *EntitySubtype);
                return false;
            }
        }
        if (bHasItemType)
        {
            if (ItemSubtype == TEXT("Weapon")) OutDefinition.ItemType = EModelItemType::Weapon;
            else if (ItemSubtype == TEXT("Tool")) OutDefinition.ItemType = EModelItemType::Tool;
            else if (ItemSubtype == TEXT("Misc")) OutDefinition.ItemType = EModelItemType::Misc;
            else
            {
                OutError = FString::Printf(TEXT("unsupported ItemType '%s'"), *ItemSubtype);
                return false;
            }
        }
    }
    else if (Root->HasField(TEXT("EntityType")) || Root->HasField(TEXT("ItemType")))
    {
        OutError = TEXT("EntityType and ItemType are only valid when ModelType is Dynamic");
        return false;
    }

    if (OutDefinition.ModelType == EModelDefinitionType::Character)
    {
        const TSharedPtr<FJsonObject>* BonesObject = nullptr;
        if (!Root->TryGetObjectField(TEXT("Bones"), BonesObject)
            || BonesObject == nullptr
            || !BonesObject->IsValid())
        {
            OutError = TEXT("Character requires a Bones object.");
            return false;
        }

        // The disk schema is canonical-key -> source-bone-name. Canonical keys are fixed and
        // source values are the only author-controlled part. glTFRuntime expects the inverse
        // source -> canonical alias map, so conversion happens only after strict validation.
        if (!CharacterBoneSchema::BuildSourceToCanonicalMap(
                *BonesObject, OutDefinition.Bones, OutError))
        {
            return false;
        }
    }

    OutDefinition.GlbPath = FSafeFileIO::NormalizeFilePath(ExpectedGlbPath);
    OutDefinition.JsonPath = FSafeFileIO::NormalizeFilePath(JsonPath);

    if (OutCanonicalJson)
    {
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
                OutCanonicalJson);
        if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
        {
            OutCanonicalJson->Reset();
            OutError = TEXT("validated model definition could not be serialized for the archive");
            OutDefinition = FModelDefinition();
            return false;
        }
    }
    return true;
}
