// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/ProjectRepository.h"

#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "System/JsonMetadata.h"
#include "System/ProjectWorkspace.h"
#include "System/SafeFileIO.h"

namespace
{
    bool SaveAndInvalidate(
        const FString& ProjectName,
        const TSharedRef<FJsonObject>& Json,
        FString& OutError)
    {
        const FString ConfigPath = V3DSimulatorProjectWorkspace::ConfigPath(ProjectName);
        const FSafeFileWriteResult Result = FSafeFileIO::SaveJsonBlocking(Json, ConfigPath);
        if (!Result.IsSuccess())
        {
            OutError = Result.Error.IsEmpty() ? TEXT("Project config save failed.") : Result.Error;
            return false;
        }
        V3DSimulatorProjectWorkspace::InvalidateBuiltArtifacts(ProjectName);
        return true;
    }
}

bool V3DSimulatorProjectRepository::ListProjects(
    TArray<FV3DSimulatorProjectSummary>& OutProjects,
    FString& OutError)
{
    OutProjects.Reset();
    OutError.Reset();

    const FString Root = V3DSimulatorProjectWorkspace::ProjectsRoot();
    IFileManager& Files = IFileManager::Get();
    if (!V3DSimulatorProjectWorkspace::EnsureWorkspaceRoots())
    {
        OutError = TEXT("Project workspace directories could not be created.");
        return false;
    }

    TArray<FString> Folders;
    Files.FindFiles(Folders, *(FPaths::Combine(Root, TEXT("*"))), false, true);
    Folders.Sort([](const FString& A, const FString& B)
    {
        return A.Compare(B, ESearchCase::IgnoreCase) < 0;
    });

    for (const FString& Folder : Folders)
    {
        FString SafeName;
        if (!V3DSimulatorProjectWorkspace::NormalizeProjectName(Folder, SafeName) || SafeName != Folder) continue;
        const FString ConfigPath = V3DSimulatorProjectWorkspace::ConfigPath(SafeName);
        if (!Files.FileExists(*ConfigPath)) continue;
        if (!V3DSimulatorProjectWorkspace::EnsureProjectDirectories(SafeName))
        {
            UE_LOG(LogTemp, Warning, TEXT("Project '%s' ignored because its resources directory could not be created."), *SafeName);
            continue;
        }

        FV3DSimulatorProjectConfig Config;
        FString Error;
        if (!V3DSimulatorProjectConfig::Load(ConfigPath, SafeName, Config, nullptr, Error))
        {
            UE_LOG(LogTemp, Warning, TEXT("Project '%s' ignored: %s"), *SafeName, *Error);
            continue;
        }
        FV3DSimulatorProjectSummary& Summary = OutProjects.AddDefaulted_GetRef();
        Summary.FolderName = SafeName;
        Summary.DisplayName = Config.GetDisplayName(SafeName);
        Summary.UUID = Config.UUID;
        Summary.ProjectType = Config.ProjectType;
        Summary.bAllowProjectAssets = Config.bAllowProjectAssets;
    }
    return true;
}

bool V3DSimulatorProjectRepository::CreateProject(
    const FString& ProjectName,
    const EV3DSimulatorProjectType ProjectType,
    FString& OutError)
{
    OutError.Reset();
    FString SafeName;
    if (!V3DSimulatorProjectWorkspace::NormalizeProjectName(ProjectName, SafeName)
        || SafeName != ProjectName.TrimStartAndEnd())
    {
        OutError = TEXT("Project name must be a safe single folder name.");
        return false;
    }

    IFileManager& Files = IFileManager::Get();
    if (!V3DSimulatorProjectWorkspace::EnsureWorkspaceRoots())
    {
        OutError = TEXT("Project workspace directories could not be created.");
        return false;
    }
    const FString Root = V3DSimulatorProjectWorkspace::ProjectRoot(SafeName);
    if (Files.DirectoryExists(*Root))
    {
        OutError = TEXT("A project with that name already exists.");
        return false;
    }
    if (!V3DSimulatorProjectWorkspace::EnsureProjectDirectories(SafeName))
    {
        OutError = TEXT("Project directory or resources directory could not be created.");
        return false;
    }

    const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(V3DSimulatorJsonMetadata::UUID,
        FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
    Json->SetStringField(V3DSimulatorJsonMetadata::Name, SafeName);
    Json->SetStringField(V3DSimulatorJsonMetadata::Version, V3DSimulatorJsonMetadata::SchemaVersion);
    Json->SetStringField(V3DSimulatorJsonMetadata::ProjectType, V3DSimulatorProjectTypes::ToString(ProjectType));
    if (ProjectType == EV3DSimulatorProjectType::World)
    {
        Json->SetStringField(V3DSimulatorJsonMetadata::WorldName, SafeName);
        Json->SetBoolField(V3DSimulatorJsonMetadata::AllowProjectAssets, false);
    }

    const FSafeFileWriteResult Save = FSafeFileIO::SaveJsonBlocking(
        Json, V3DSimulatorProjectWorkspace::ConfigPath(SafeName));
    if (!Save.IsSuccess())
    {
        Files.DeleteDirectory(*Root, false, true);
        OutError = Save.Error.IsEmpty() ? TEXT("Project config creation failed.") : Save.Error;
        return false;
    }
    return true;
}

bool V3DSimulatorProjectRepository::LoadProject(
    const FString& ProjectName,
    FV3DSimulatorProjectConfig& OutConfig,
    TSharedPtr<FJsonObject>* OutJson,
    FString& OutError)
{
    FString SafeName;
    if (!V3DSimulatorProjectWorkspace::NormalizeProjectName(ProjectName, SafeName))
    {
        OutError = TEXT("Invalid project name.");
        return false;
    }
    const FString Root = V3DSimulatorProjectWorkspace::ProjectRoot(SafeName);
    if (Root.IsEmpty() || !IFileManager::Get().DirectoryExists(*Root))
    {
        OutError = TEXT("Project directory does not exist.");
        return false;
    }
    if (!V3DSimulatorProjectWorkspace::EnsureProjectDirectories(SafeName))
    {
        OutError = TEXT("Project resources directory could not be created.");
        return false;
    }
    return V3DSimulatorProjectConfig::Load(
        V3DSimulatorProjectWorkspace::ConfigPath(SafeName), SafeName, OutConfig, OutJson, OutError);
}

bool V3DSimulatorProjectRepository::SetProjectType(
    const FString& ProjectName,
    const EV3DSimulatorProjectType ProjectType,
    FString& OutError)
{
    FV3DSimulatorProjectConfig Existing;
    TSharedPtr<FJsonObject> Json;
    if (!LoadProject(ProjectName, Existing, &Json, OutError) || !Json.IsValid()) return false;

    Json->SetStringField(V3DSimulatorJsonMetadata::ProjectType, V3DSimulatorProjectTypes::ToString(ProjectType));
    if (ProjectType == EV3DSimulatorProjectType::World)
    {
        Json->SetStringField(V3DSimulatorJsonMetadata::WorldName,
            Existing.WorldName.IsEmpty() ? Existing.Name : Existing.WorldName);
        if (!Json->HasField(V3DSimulatorJsonMetadata::AllowProjectAssets))
            Json->SetBoolField(V3DSimulatorJsonMetadata::AllowProjectAssets, false);
    }
    else
    {
        Json->RemoveField(V3DSimulatorJsonMetadata::WorldName);
        Json->RemoveField(V3DSimulatorJsonMetadata::AllowProjectAssets);
    }

    bool bChanged = false;
    FString NormalizeError;
    FString SafeName;
    V3DSimulatorProjectWorkspace::NormalizeProjectName(ProjectName, SafeName);
    if (!V3DSimulatorProjectConfig::Normalize(Json.ToSharedRef(), SafeName, bChanged, NormalizeError))
    {
        OutError = NormalizeError;
        return false;
    }
    return SaveAndInvalidate(SafeName, Json.ToSharedRef(), OutError);
}

bool V3DSimulatorProjectRepository::SetWorldProjectAssetsAllowed(
    const FString& ProjectName,
    const bool bAllowed,
    FString& OutError)
{
    FV3DSimulatorProjectConfig Existing;
    TSharedPtr<FJsonObject> Json;
    if (!LoadProject(ProjectName, Existing, &Json, OutError) || !Json.IsValid()) return false;
    if (Existing.ProjectType != EV3DSimulatorProjectType::World)
    {
        OutError = TEXT("Only World projects can reference other project assets.");
        return false;
    }
    Json->SetBoolField(V3DSimulatorJsonMetadata::AllowProjectAssets, bAllowed);

    FString SafeName;
    V3DSimulatorProjectWorkspace::NormalizeProjectName(ProjectName, SafeName);
    return SaveAndInvalidate(SafeName, Json.ToSharedRef(), OutError);
}
