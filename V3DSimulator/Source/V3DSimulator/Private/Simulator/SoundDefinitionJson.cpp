// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "Simulator/SoundDefinitionJson.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "System/SafeFileIO.h"
#include "System/JsonMetadata.h"
#include "System/JsonMetadataNormalizer.h"

namespace
{
    constexpr int32 MaxSoundDefinitions = 100000;

    bool IsSafeAssetName(const FString& Value)
    {
        return !Value.IsEmpty() && FPaths::GetCleanFilename(Value) == Value
            && Value != TEXT(".") && Value != TEXT("..")
            && !Value.Contains(TEXT("/")) && !Value.Contains(TEXT("\\"));
    }


}

bool SoundDefinitionJson::EnsureDefinitions(
    const FString& AssetRootDirectory,
    TArray<FString>* OutUpdatedJsonFiles,
    TArray<FString>* OutDiscoveredWavFiles)
{
    if (OutUpdatedJsonFiles) OutUpdatedJsonFiles->Reset();
    if (OutDiscoveredWavFiles) OutDiscoveredWavFiles->Reset();
    if (AssetRootDirectory.IsEmpty() || !IFileManager::Get().DirectoryExists(*AssetRootDirectory)) return false;

    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *AssetRootDirectory, TEXT("*.*"), true, false, false);
    Files.RemoveAllSwap([](const FString& Path)
    {
        return !FPaths::GetExtension(Path).Equals(TEXT("wav"), ESearchCase::IgnoreCase);
    }, EAllowShrinking::No);

    TSet<FString> Seen;
    TArray<FString> Wavs;
    Wavs.Reserve(Files.Num());
    for (const FString& Candidate : Files)
    {
        FString Path = FSafeFileIO::NormalizeFilePath(Candidate);
        if (!Path.IsEmpty() && IFileManager::Get().FileExists(*Path) && !Seen.Contains(Path))
        {
            Seen.Add(Path);
            Wavs.Add(MoveTemp(Path));
        }
    }
    Wavs.Sort();
    if (Wavs.Num() > MaxSoundDefinitions) return false;
    if (OutDiscoveredWavFiles) *OutDiscoveredWavFiles = Wavs;

    bool bOk = true;
    for (const FString& WavPath : Wavs)
    {
        const FString JsonPath = FPaths::ChangeExtension(WavPath, TEXT("json"));
        bool bChanged = false;
        FString Error;
        if (V3DSimulatorJsonMetadataNormalizer::EnsureAssetJson(
                JsonPath, FPaths::GetBaseFilename(WavPath), EAssetDefinitionType::Sound,
                EModelDefinitionType::Invalid, bChanged, Error))
        {
            if (bChanged && OutUpdatedJsonFiles) OutUpdatedJsonFiles->Add(JsonPath);
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("Sound JSON normalization failed: %s (%s)"), *JsonPath, *Error);
            bOk = false;
        }
    }
    return bOk;
}

bool SoundDefinitionJson::LoadDefinition(
    const FString& JsonPath,
    const FString& ExpectedWavPath,
    FSoundDefinition& OutDefinition,
    FString& OutError,
    FString* OutCanonicalJson)
{
    OutDefinition = FSoundDefinition();
    OutError.Reset();
    if (OutCanonicalJson) OutCanonicalJson->Reset();

    if (!FPaths::GetExtension(ExpectedWavPath).Equals(TEXT("wav"), ESearchCase::IgnoreCase)
        || !FPaths::GetBaseFilename(JsonPath).Equals(FPaths::GetBaseFilename(ExpectedWavPath), ESearchCase::CaseSensitive)
        || FPaths::GetPath(JsonPath) != FPaths::GetPath(ExpectedWavPath))
    {
        OutError = TEXT("Sound asset requires same-directory, same-basename .json + .wav files.");
        return false;
    }

    FSafeJsonLimits Limits;
    Limits.MaxFileBytes = 4ll * 1024ll * 1024ll;
    Limits.MaxDepth = 16;
    Limits.MaxValues = 4096;
    Limits.MaxContainerEntries = 1024;
    Limits.MaxStringCharacters = 32768;
    Limits.bAllowBackupRecovery = false;
    const FSafeJsonLoadResult Loaded = FSafeFileIO::LoadJsonBlocking(JsonPath, Limits);
    if (!Loaded.IsSuccess() || !Loaded.JsonObject.IsValid())
    {
        OutError = Loaded.Error.IsEmpty() ? TEXT("Sound JSON is invalid.") : Loaded.Error;
        return false;
    }

    const TSharedPtr<FJsonObject>& Root = Loaded.JsonObject;
    FString AssetType;
    EAssetDefinitionType ParsedAssetType = EAssetDefinitionType::Model;
    if (!Root->TryGetStringField(V3DSimulatorJsonMetadata::AssetType, AssetType)
        || !V3DSimulatorAssetTypes::TryParse(AssetType, ParsedAssetType)
        || ParsedAssetType != EAssetDefinitionType::Sound)
    {
        OutError = TEXT("Sound JSON requires AssetType=Sound.");
        return false;
    }
    FString UUIDText;
    if (!Root->TryGetStringField(V3DSimulatorJsonMetadata::UUID, UUIDText)
        || !FGuid::Parse(UUIDText, OutDefinition.UUID) || !OutDefinition.UUID.IsValid())
    {
        OutError = TEXT("Sound JSON requires a valid UUID.");
        return false;
    }
    if (!Root->TryGetStringField(V3DSimulatorJsonMetadata::Name, OutDefinition.Name)
        || !Root->TryGetStringField(V3DSimulatorJsonMetadata::DisplayName, OutDefinition.DisplayName))
    {
        OutError = TEXT("Sound JSON requires Name and DisplayName.");
        return false;
    }
    FString VersionText;
    if (!Root->TryGetStringField(V3DSimulatorJsonMetadata::Version, VersionText)
        || VersionText.TrimStartAndEnd().IsEmpty())
    {
        OutError = TEXT("Sound JSON requires Version.");
        return false;
    }

    OutDefinition.Name.TrimStartAndEndInline();
    OutDefinition.DisplayName.TrimStartAndEndInline();
    if (!IsSafeAssetName(OutDefinition.Name) || OutDefinition.DisplayName.IsEmpty())
    {
        OutError = TEXT("Sound Name/DisplayName is invalid.");
        return false;
    }
    OutDefinition.AssetType = EAssetDefinitionType::Sound;
    OutDefinition.WavPath = FSafeFileIO::NormalizeFilePath(ExpectedWavPath);
    OutDefinition.JsonPath = FSafeFileIO::NormalizeFilePath(JsonPath);
    if (OutDefinition.WavPath.IsEmpty() || OutDefinition.JsonPath.IsEmpty())
    {
        OutError = TEXT("Sound asset paths are invalid.");
        return false;
    }

    if (OutCanonicalJson)
    {
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(OutCanonicalJson);
        if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
        {
            OutCanonicalJson->Reset();
            OutError = TEXT("Validated Sound JSON could not be serialized.");
            return false;
        }
    }
    return true;
}
