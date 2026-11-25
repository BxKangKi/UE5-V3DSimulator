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
#if PLATFORM_ANDROID || PLATFORM_IOS
    // Sandboxed mobile applications cannot write to a desktop Documents/UserDir path.
    return FSafeFileIO::NormalizeFilePath(FPaths::Combine(FPaths::ProjectSavedDir(), ProductDirectory));
#else
    return FSafeFileIO::NormalizeFilePath(FPaths::Combine(FPlatformProcess::UserDir(), ProductDirectory));
#endif
}

FString V3DSimulatorPaths::ProjectsRoot()
{
    return FSafeFileIO::NormalizeFilePath(FPaths::Combine(UserRoot(), TEXT("Projects")));
}

FString V3DSimulatorPaths::ResourcesRoot()
{
    return FSafeFileIO::NormalizeFilePath(FPaths::Combine(UserRoot(), TEXT("V3Ds"), TEXT("Resources")));
}

FString V3DSimulatorPaths::WorldsRoot()
{
    return FSafeFileIO::NormalizeFilePath(FPaths::Combine(UserRoot(), TEXT("V3Ds"), TEXT("Worlds")));
}

FString V3DSimulatorPaths::DataRoot()
{
    return FSafeFileIO::NormalizeFilePath(FPaths::Combine(UserRoot(), TEXT("Data")));
}

FString V3DSimulatorPaths::LogsRoot()
{
    return FSafeFileIO::NormalizeFilePath(FPaths::Combine(UserRoot(), TEXT("Logs")));
}

FString V3DSimulatorPaths::SettingsPath()
{
    return FSafeFileIO::NormalizeFilePath(FPaths::Combine(UserRoot(), TEXT("settings.json")));
}
