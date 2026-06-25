// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/SimulatorFileServices.h"

#include "System/SafeFileIO.h"
#include "System/SimulatorPaths.h"
#include "Misc/Paths.h"

namespace
{
    constexpr int64 MaxSafeJsonBytes = 64ll * 1024ll * 1024ll;
}

void FSimulatorFileServices::WriteLogAsync(const FString& Category, const FString& Message)
{
    const FString SafeCategory = Category.IsEmpty() ? TEXT("General") : Category;
    const FString Path = FPaths::Combine(
        V3DSimulatorPaths::LogsRoot(),
        FString::Printf(TEXT("log_%s.txt"), *FDateTime::Now().ToString(TEXT("%Y%m%d"))));
    const FString Line = FString::Printf(
        TEXT("[%s][%s] %s%s"), *FDateTime::Now().ToString(), *SafeCategory, *Message, LINE_TERMINATOR);
    FSafeFileIO::AppendTextAsync(Line, Path, [SafeCategory](FSafeFileWriteResult Result)
    {
        if (!Result.IsSuccess() && Result.Status != ESafeFileIOStatus::ShuttingDown)
        {
            UE_LOG(LogTemp, Error, TEXT("Simulator log write failed. Category=%s Error=%s"),
                *SafeCategory, *Result.Error);
        }
    });
}

TSharedPtr<FJsonObject> FSimulatorFileServices::LoadJson(const FString& Path)
{
    FSafeJsonLimits Limits;
    Limits.MaxFileBytes = MaxSafeJsonBytes;
    const FSafeJsonLoadResult Result = FSafeFileIO::LoadJsonBlocking(Path, Limits);
    if (Result.IsSuccess()) return Result.JsonObject;
    if (Result.Status != ESafeFileIOStatus::Missing)
    {
        UE_LOG(LogTemp, Error, TEXT("JSON load failed. Path=%s Error=%s"), *Path, *Result.Error);
    }
    return nullptr;
}

void FSimulatorFileServices::SaveJsonAsync(TSharedRef<FJsonObject> Json, const FString& Path)
{
    FSafeFileIO::SaveJsonAsync(Json, Path, [Path](FSafeFileWriteResult Result)
    {
        if (!Result.IsSuccess() && Result.Status != ESafeFileIOStatus::ShuttingDown
            && Result.Status != ESafeFileIOStatus::Superseded)
        {
            UE_LOG(LogTemp, Error, TEXT("JSON save failed. Path=%s Error=%s"), *Path, *Result.Error);
        }
    }, MaxSafeJsonBytes);
}
