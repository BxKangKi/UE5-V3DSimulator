// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/ProjectEditorDocument.h"
#include "System/ProjectWorkspace.h"
#include "System/ProjectRepository.h"
#include "System/ProjectConfig.h"
#include "System/ProjectScripts.h"
#include "System/SafeFileIO.h"
#include "System/SourceModelImport.h"
#include "System/JsonMetadata.h"
#include "Simulator/ModelDefinitionJson.h"
#include "Simulator/SoundDefinitionJson.h"
#include "Character/CharacterBoneSchema.h"
#include "World/WorldData.h"
#include "World/WorldEnvironmentConfig.h"
#include "World/WorldFogConfig.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ProjectEditorSchemaData.inl"

namespace
{
    // Cache parsing per calling thread; default TSharedPtr JSON nodes are not thread-safe.
    FString BuildEditorSchemaJson()
    {
        FString Combined;
        for (const TCHAR* Part : EditorSchemaJsonParts)
        {
            Combined += Part;
        }
        return Combined;
    }

    const TSharedPtr<FJsonObject>& EditorTemplates()
    {
        static thread_local const FSafeJsonLoadResult Templates =
            FSafeFileIO::ParseJsonText(BuildEditorSchemaJson(), TEXT("Editor schema"));
        return Templates.JsonObject;
    }

    TSharedPtr<FJsonValue> CloneValue(const TSharedPtr<FJsonValue>& Value)
    {
        return Value.IsValid() ? FJsonValue::Duplicate(Value) : MakeShared<FJsonValueNull>();
    }
}

TSharedPtr<FJsonObject> V3DProjectEditor::CloneObject(const TSharedPtr<FJsonObject>& Object)
{
    if (!Object) return nullptr;
    auto Result = MakeShared<FJsonObject>();
    for (const auto& Pair : Object->Values) Result->SetField(Pair.Key, CloneValue(Pair.Value));
    return Result;
}

FString V3DProjectEditor::Serialize(const TSharedPtr<FJsonObject>& Json)
{
    FString Text;
    if (Json.IsValid()) FJsonSerializer::Serialize(Json.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
    return Text;
}

bool V3DProjectEditor::Discover(const FString& Project, TArray<TSharedPtr<FV3DEditorDocument>>& Documents, FString& Error)
{
    FString Safe;
    if (!V3DSimulatorProjectWorkspace::NormalizeProjectName(Project, Safe) || Project != Safe)
    { Error = TEXT("Invalid project name."); return false; }
    FV3DSimulatorProjectConfig Config;
    if (!V3DSimulatorProjectRepository::LoadProject(Project, Config, nullptr, Error)) return false;
    const FString Root = V3DSimulatorProjectWorkspace::ProjectRoot(Project);
    const FString Resources = V3DSimulatorProjectWorkspace::ProjectResourcesRoot(Project);
    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *Resources, TEXT("*.*"), true, false);
    TMap<FString, FString> Sources;
    for (const auto& Path : Files)
        if (V3DSourceModelImport::IsModelSource(Path) || FPaths::GetExtension(Path).Equals(TEXT("wav"), ESearchCase::IgnoreCase))
        {
            const FString Key = FSafeFileIO::NormalizeFilePath(FPaths::ChangeExtension(Path, TEXT("json"))).ToLower();
            if (Sources.Contains(Key)) { Error = TEXT("Two resources share a JSON basename: ") + Path; return false; }
            Sources.Add(Key, Path);
        }
    if (Sources.Num() > 4096) { Error = TEXT("Editor limit: 4096 resource definitions per project."); return false; }
    // Missing metadata is generated through the same source discovery used by Build.
    // Existing invalid metadata remains editable and is not rewritten.
    ModelDefinitionJson::EnsureDefinitions(Resources, Config.ProjectType);
    SoundDefinitionJson::EnsureDefinitions(Resources);
    Files.Empty();
    IFileManager::Get().FindFilesRecursive(Files, *Resources, TEXT("*.json"), true, false);
    Files.Sort();
    Files.Insert(V3DSimulatorProjectWorkspace::ConfigPath(Project), 0);
    if (Config.ProjectType == EV3DSimulatorProjectType::World) Files.Add(FPaths::Combine(Root, FV3DWorldFogConfig::MemberName()));
    for (int32 I = 0; I < 3; ++I) Files.Add(FPaths::Combine(Root, FV3DProjectScripts::MemberName(I)));
    if (Files.Num() > 4101) { Error = TEXT("Too many editor documents."); return false; }
    int64 TotalBytes = 0;
    for (const auto& Path : Files)
    {
        const bool bEnvironment = Path == FPaths::Combine(Root, FV3DWorldFogConfig::MemberName());
        const int64 Bytes = IFileManager::Get().FileSize(*Path);
        if (Bytes > 16ll * 1024 * 1024 || (Bytes > 0 && (TotalBytes += Bytes) > 64ll * 1024 * 1024))
        { Error = TEXT("Editor JSON memory limit exceeded (16 MiB per file, 64 MiB total)."); return false; }
        auto Doc = MakeShared<FV3DEditorDocument>();
        Doc->Path = FSafeFileIO::NormalizeFilePath(Path);
        Doc->Label = Path; FPaths::MakePathRelativeTo(Doc->Label, *(Root / TEXT("")));
        if (const auto* Source = Sources.Find(Doc->Path.ToLower())) Doc->SourcePath = *Source;
        FSafeJsonLimits Limits; Limits.MaxFileBytes = 16ll * 1024 * 1024;
        const auto Loaded = FSafeFileIO::LoadJsonBlocking(Path, Limits);
        if (!FFileHelper::LoadFileToString(Doc->DiskText, *Path))
        {
            if (Loaded.Status == ESafeFileIOStatus::Missing && bEnvironment)
            {
                Doc->Json = FV3DWorldFogConfig().ToJson();
                Doc->bNewFile = Doc->bDirty = true; Documents.Add(Doc); continue;
            }
            int32 ScriptIndex = INDEX_NONE;
            for (int32 I = 0; I < 3; ++I) if (Doc->Path.EndsWith(FV3DProjectScripts::MemberName(I))) ScriptIndex = I;
            if (Loaded.Status != ESafeFileIOStatus::Missing || ScriptIndex == INDEX_NONE) continue;
            Doc->Json = FSafeFileIO::ParseJsonText(FV3DProjectScripts().Text(ScriptIndex), Doc->Path).JsonObject;
            Doc->bNewFile = Doc->bDirty = true; Documents.Add(Doc); continue;
        }
        Doc->Json = Loaded.JsonObject; // Invalid JSON is still exposed in the raw editor for repair.
        Documents.Add(Doc);
    }
    return !Documents.IsEmpty();
}

bool V3DProjectEditor::Validate(const FV3DEditorDocument& Doc, const FString& Project, FString& Error, const TArray<FV3DEditorDocument>* Batch)
{
    Error.Reset();
    if (!Doc.Json.IsValid()) { Error = TEXT("Invalid JSON object."); return false; }
    const FString ConfigPath = V3DSimulatorProjectWorkspace::ConfigPath(Project);
    if (Doc.Path == FSafeFileIO::NormalizeFilePath(V3DSimulatorProjectWorkspace::ProjectRoot(Project) / FV3DWorldFogConfig::MemberName()))
    {
        FV3DWorldFogConfig Fog;
        return FV3DWorldFogConfig::Parse(Doc.Json, Fog, Error);
    }
    if (Doc.Path == FSafeFileIO::NormalizeFilePath(ConfigPath))
    {
        FV3DSimulatorProjectConfig C; FV3DEnvironmentConfig E;
        return V3DSimulatorProjectConfig::Parse(Doc.Json, Project, C, Error)
            && FV3DEnvironmentConfig::Parse(Doc.Json, E, Error);
    }
    if (!Doc.SourcePath.IsEmpty())
    {
        if (V3DSourceModelImport::IsModelSource(Doc.SourcePath))
        {
            FModelDefinition Model;
            return ModelDefinitionJson::ValidateDefinition(Doc.Json, Doc.Path, Doc.SourcePath, Model, Error);
        }
        if (Doc.Json->HasField(TEXT("ModelType"))) { Error = TEXT("Sound cannot contain ModelType."); return false; }
        FSoundDefinition Sound;
        return SoundDefinitionJson::ValidateDefinition(Doc.Json, Doc.Path, Doc.SourcePath, Sound, Error);
    }
    for (int32 I = 0; I < 3; ++I)
        if (Doc.Path.EndsWith(FV3DProjectScripts::MemberName(I)))
        {
            FV3DProjectScripts Scripts; FV3DWorldSurfaceConfig Surfaces; FV3DSimulationCommandConfig Commands;
            for (int32 J = 0; J < 3; ++J)
            {
                const FString Path = FSafeFileIO::NormalizeFilePath(FPaths::Combine(V3DSimulatorProjectWorkspace::ProjectRoot(Project), FV3DProjectScripts::MemberName(J)));
                const FV3DEditorDocument* Override = Batch ? Batch->FindByPredicate([&Path](const auto& D) { return D.Path == Path; }) : nullptr;
                if (J == I) Scripts.Text(J) = Serialize(Doc.Json);
                else if (Override) Scripts.Text(J) = Serialize(Override->Json);
                else
                {
                    const auto Loaded = FSafeFileIO::LoadJsonBlocking(Path);
                    if (Loaded.Status == ESafeFileIOStatus::Missing) continue;
                    if (!Loaded.IsSuccess()) { Error = Loaded.Error; return false; }
                    Scripts.Text(J) = Serialize(Loaded.JsonObject);
                }
            }
            return Scripts.Parse(Surfaces, Commands, Error);
        }
    return true; // Author-defined extra JSON remains supported.
}

bool V3DProjectEditor::Save(FV3DEditorDocument& Doc, const FString& Project, FString& Error, const TArray<FV3DEditorDocument>* Batch)
{
    if (!Validate(Doc, Project, Error, Batch)) return false;
    const FString Root = FSafeFileIO::NormalizeFilePath(V3DSimulatorProjectWorkspace::ProjectRoot(Project));
    if (!Doc.Path.StartsWith(Root + TEXT("/")) || !Doc.Path.EndsWith(TEXT(".json")))
    { Error = TEXT("Save path is outside the selected project."); return false; }
    FString Current;
    const bool bExists = IFileManager::Get().FileExists(*Doc.Path);
    if ((Doc.bNewFile && bExists) || (!Doc.bNewFile && (!FFileHelper::LoadFileToString(Current, *Doc.Path) || Current != Doc.DiskText)))
    { Error = TEXT("File changed outside Editor. Reload before saving: ") + Doc.Label; return false; }
    if (Doc.bNewFile && !IFileManager::Get().MakeDirectory(*FPaths::GetPath(Doc.Path), true))
    { Error = TEXT("Could not create document directory."); return false; }
    const auto Written = FSafeFileIO::SaveJsonBlocking(Doc.Json.ToSharedRef(), Doc.Path);
    if (!Written.IsSuccess()) { Error = Written.Error; return false; }
    FFileHelper::LoadFileToString(Doc.DiskText, *Doc.Path);
    Doc.bDirty = Doc.bNewFile = false;
    // Keep the last successful build playable. Only explicit Build replaces the archive.
    return true;
}

TSharedPtr<FJsonObject> V3DProjectEditor::Suggestions(const FV3DEditorDocument& Doc)
{
    // Relative labels distinguish this script from resources/environments.json.
    if (Doc.Label == FV3DWorldFogConfig::MemberName()) return FV3DWorldFogConfig().ToJson();
    auto Result = MakeShared<FJsonObject>();
    const auto& Templates = EditorTemplates();
    if (!Templates.IsValid() || !Doc.Json.IsValid()) return Result;
    auto Merge = [&Result, &Templates](const TCHAR* Key)
    {
        const TSharedPtr<FJsonObject>* Part = nullptr;
        if (Templates->TryGetObjectField(Key, Part))
            for (const auto& Pair : (*Part)->Values) Result->SetField(FString(Pair.Key), CloneValue(Pair.Value));
    };
    if (Doc.Path.EndsWith(TEXT("scripts/materials.json"))) Merge(TEXT("MaterialsScript"));
    if (Doc.Path.EndsWith(TEXT("scripts/sounds.json"))) Merge(TEXT("SoundsScript"));
    if (Doc.Path.EndsWith(TEXT("scripts/commands.json"))) Merge(TEXT("CommandsScript"));
    FString Type, Subtype, Project;
    Doc.Json->TryGetStringField(TEXT("AssetType"), Type);
    Doc.Json->TryGetStringField(TEXT("ProjectType"), Project);
    if (!Project.IsEmpty())
    {
        Result->SetStringField(TEXT("ProjectType"), Project);
        Result->SetStringField(TEXT("Name"), TEXT("Project"));
        if (Project == TEXT("World"))
        {
            Merge(TEXT("World"));
            Result->SetStringField(TEXT("WorldName"), TEXT("World"));
            Result->SetObjectField(TEXT("Cloud"), FLevelCloudSettings().ToJson());
            Result->SetObjectField(TEXT("Weather"), FLevelWeatherSettings().ToJson());
            Result->SetObjectField(TEXT("Gameplay"), FLevelGameplaySettings().ToJson());
        }
    }
    else if (!Type.IsEmpty() || !Doc.SourcePath.IsEmpty())
    {
        Result->SetStringField(TEXT("UUID"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
        Result->SetStringField(TEXT("Name"), FPaths::GetBaseFilename(Doc.Path));
        Result->SetStringField(TEXT("DisplayName"), FPaths::GetBaseFilename(Doc.Path));
        Result->SetStringField(TEXT("Version"), V3DSimulatorJsonMetadata::SchemaVersion);
        Result->SetStringField(TEXT("AssetType"), Type.IsEmpty() ? TEXT("Model") : Type);
        if (Type != TEXT("Sound"))
        {
            Merge(TEXT("Model"));
            Doc.Json->TryGetStringField(TEXT("ModelType"), Subtype);
            Result->SetStringField(TEXT("ModelType"), Subtype.IsEmpty() ? TEXT("Static") : Subtype);
            if (Subtype == TEXT("Character"))
            {
                auto Bones = MakeShared<FJsonObject>();
                for (const auto& Key : CharacterBoneSchema::GetRequiredKeys()) Bones->SetStringField(Key.ToString(), Key.ToString());
                Result->SetObjectField(TEXT("Bones"), Bones);
                Merge(TEXT("CharacterInteraction"));
            }
            else
            {
                Merge(TEXT("Gravity"));
                if (Subtype.IsEmpty() || Subtype == TEXT("Static"))
                {
                    Merge(TEXT("StaticInteraction"));
                }
            }
            if (Subtype == TEXT("Dynamic"))
            {
                Merge(TEXT("Dynamic"));
                FString Entity, Item;
                Doc.Json->TryGetStringField(TEXT("EntityType"), Entity); Doc.Json->TryGetStringField(TEXT("ItemType"), Item);
                if (Item.IsEmpty()) Result->SetStringField(TEXT("EntityType"), Entity.IsEmpty() ? TEXT("Prop") : Entity);
                if (Entity.IsEmpty()) Result->SetStringField(TEXT("ItemType"), Item.IsEmpty() ? TEXT("Misc") : Item);
                if (Entity == TEXT("Vehicle")) Merge(TEXT("Vehicle"));
                if (!Item.IsEmpty()) Merge(TEXT("Item"));
                if (Item == TEXT("Weapon")) Merge(TEXT("Weapon"));
            }
        }
    }
    return Result;
}

TSharedPtr<FJsonObject> V3DProjectEditor::EntryTemplate(const FString& Container)
{
    if (Container == TEXT("WeatherProfiles") || Container == TEXT("BiomeProfiles"))
        return FV3DWorldFogConfig::ProfileTemplate();
    static const TMap<FString, FString> Keys = {
        {TEXT("SoundLists"), TEXT("SoundListEntry")}, {TEXT("Events"), TEXT("SoundEventEntry")},
        {TEXT("MeshData"), TEXT("MeshEntry")}, {TEXT("Biomes"), TEXT("BiomeEntry")},
        {TEXT("PhysicalMaterials"), TEXT("PhysicalMaterialEntry")}, {TEXT("FootstepMappings"), TEXT("FootstepEntry")},
        {TEXT("MaterialPhysicalMappings"), TEXT("MappingEntry")}, {TEXT("Commands"), TEXT("CommandEntry")},
        {TEXT("Triggers"), TEXT("TriggerEntry")}, {TEXT("Actions"), TEXT("ActionEntry")},
        {TEXT("Conditions"), TEXT("ConditionEntry")}, {TEXT("Colliders"), TEXT("ColliderEntry")}, {TEXT("Lights"), TEXT("LightEntry")}};
    const auto* Key = Keys.Find(Container);
    if (!Key) return MakeShared<FJsonObject>();
    const auto& Templates = EditorTemplates();
    const TSharedPtr<FJsonObject>* Entry = nullptr;
    if (Templates.IsValid() && Templates->TryGetObjectField(*Key, Entry)) return CloneObject(*Entry);
    return MakeShared<FJsonObject>();
}
