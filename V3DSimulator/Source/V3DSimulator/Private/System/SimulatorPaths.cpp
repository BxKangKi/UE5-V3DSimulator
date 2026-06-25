// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/SimulatorPaths.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "System/SafeFileIO.h"

namespace
{
    constexpr TCHAR ProductDirectory[] = TEXT("V3DSimulator");
}

FString V3DSimulatorPaths::UserRoot()
{
    return FSafeFileIO::NormalizeFilePath(FPaths::Combine(FPlatformProcess::UserDir(), ProductDirectory));
}

FString V3DSimulatorPaths::ProjectsRoot()
{
    return FSafeFileIO::NormalizeFilePath(FPaths::Combine(UserRoot(), TEXT("Projects")));
}

FString V3DSimulatorPaths::ResourcesRoot()
{
    return FSafeFileIO::NormalizeFilePath(FPaths::Combine(UserRoot(), TEXT("Resources")));
}

FString V3DSimulatorPaths::WorldsRoot()
{
    return FSafeFileIO::NormalizeFilePath(FPaths::Combine(UserRoot(), TEXT("Worlds")));
}

FString V3DSimulatorPaths::LogsRoot()
{
    return FSafeFileIO::NormalizeFilePath(FPaths::Combine(UserRoot(), TEXT("Logs")));
}

FString V3DSimulatorPaths::SettingsPath()
{
    return FSafeFileIO::NormalizeFilePath(FPaths::Combine(UserRoot(), TEXT("settings.json")));
}
