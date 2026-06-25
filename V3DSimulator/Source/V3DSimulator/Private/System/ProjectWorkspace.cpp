// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/ProjectWorkspace.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "System/SimulatorPaths.h"

bool V3DSimulatorProjectWorkspace::NormalizeProjectName(const FString& Input, FString& OutSafeName)
{
    OutSafeName = Input.TrimStartAndEnd();
    return !OutSafeName.IsEmpty()
        && OutSafeName != TEXT(".") && OutSafeName != TEXT("..")
        && FPaths::GetCleanFilename(OutSafeName) == OutSafeName
        && !OutSafeName.Contains(TEXT("/")) && !OutSafeName.Contains(TEXT("\\"))
        && !OutSafeName.Contains(TEXT(":"));
}

FString V3DSimulatorProjectWorkspace::ProjectsRoot()
{
    return V3DSimulatorPaths::ProjectsRoot();
}

FString V3DSimulatorProjectWorkspace::ProjectRoot(const FString& ProjectName)
{
    FString SafeName;
    return NormalizeProjectName(ProjectName, SafeName)
        ? FPaths::Combine(ProjectsRoot(), SafeName) : FString();
}

FString V3DSimulatorProjectWorkspace::ProjectResourcesRoot(const FString& ProjectName)
{
    const FString Root = ProjectRoot(ProjectName);
    return Root.IsEmpty() ? FString() : FPaths::Combine(Root, TEXT("resources"));
}

FString V3DSimulatorProjectWorkspace::ConfigPath(const FString& ProjectName)
{
    const FString Root = ProjectRoot(ProjectName);
    return Root.IsEmpty() ? FString() : FPaths::Combine(Root, TEXT("config.json"));
}

FString V3DSimulatorProjectWorkspace::AssetArchivePath(const FString& ProjectName)
{
    FString SafeName;
    return NormalizeProjectName(ProjectName, SafeName)
        ? FPaths::Combine(V3DSimulatorPaths::ResourcesRoot(), SafeName + TEXT(".v3d"))
        : FString();
}

bool V3DSimulatorProjectWorkspace::EnsureWorkspaceRoots()
{
    IFileManager& Files = IFileManager::Get();
    const FString Projects = ProjectsRoot();
    const FString Resources = V3DSimulatorPaths::ResourcesRoot();
    const FString Worlds = V3DSimulatorPaths::WorldsRoot();
    return !Projects.IsEmpty() && !Resources.IsEmpty() && !Worlds.IsEmpty()
        && Files.MakeDirectory(*Projects, true)
        && Files.MakeDirectory(*Resources, true)
        && Files.MakeDirectory(*Worlds, true);
}

bool V3DSimulatorProjectWorkspace::EnsureProjectDirectories(const FString& ProjectName)
{
    if (!EnsureWorkspaceRoots()) return false;
    const FString Root = ProjectRoot(ProjectName);
    const FString Resources = ProjectResourcesRoot(ProjectName);
    if (Root.IsEmpty() || Resources.IsEmpty()) return false;
    IFileManager& Files = IFileManager::Get();
    return Files.MakeDirectory(*Root, true) && Files.MakeDirectory(*Resources, true);
}

FString V3DSimulatorProjectWorkspace::WorldArchivePath(const FString& ProjectName)
{
    FString SafeName;
    return NormalizeProjectName(ProjectName, SafeName)
        ? FPaths::Combine(V3DSimulatorPaths::WorldsRoot(), SafeName + TEXT(".v3d")) : FString();
}

void V3DSimulatorProjectWorkspace::InvalidateBuiltArtifacts(const FString& ProjectName)
{
    IFileManager& Files = IFileManager::Get();
    const FString Asset = AssetArchivePath(ProjectName);
    const FString World = WorldArchivePath(ProjectName);
    if (!Asset.IsEmpty()) Files.Delete(*Asset, false, true, true);
    if (!World.IsEmpty()) Files.Delete(*World, false, true, true);
}
