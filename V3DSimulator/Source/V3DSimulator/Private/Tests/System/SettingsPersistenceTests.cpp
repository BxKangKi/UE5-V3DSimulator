// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "System/EntityArchive.h"
#include "System/SafeFileIO.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DJsonNoBackupTest,
    "V3DSimulator.Settings.NoJsonBackups",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DJsonNoBackupTest::RunTest(const FString&)
{
    const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), FGuid::NewGuid().ToString());
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
    const FString Path = FPaths::Combine(Root, TEXT("settings.json"));
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    for (int32 Revision = 0; Revision < 3; ++Revision)
    {
        Json->SetNumberField(TEXT("MaxRenderDistanceMeters"), 1024 << Revision);
        TestTrue(TEXT("Save JSON"), FSafeFileIO::SaveJsonBlocking(Json, Path).IsSuccess());
        TestFalse(TEXT("No persistent backup"), IFileManager::Get().FileExists(*(Path + TEXT(".bak"))));
    }
    const FSafeJsonLoadResult Loaded = FSafeFileIO::LoadJsonBlocking(Path);
    if (TestTrue(TEXT("Latest primary can be read"), Loaded.IsSuccess()))
        TestEqual(TEXT("Last revision is retained"), Loaded.JsonObject->GetNumberField(TEXT("MaxRenderDistanceMeters")), 4096.0);
    Json->SetNumberField(TEXT("MaxRenderDistanceMeters"), 1024);
    TestTrue(TEXT("Create-if-missing accepts existing file"), FSafeFileIO::CreateJsonIfMissingBlocking(Json, Path).IsSuccess());
    const FSafeJsonLoadResult Unchanged = FSafeFileIO::LoadJsonBlocking(Path);
    if (TestTrue(TEXT("Author document still reads"), Unchanged.IsSuccess()))
        TestEqual(TEXT("Template creation does not replace author JSON"), Unchanged.JsonObject->GetNumberField(TEXT("MaxRenderDistanceMeters")), 4096.0);
    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *FPaths::Combine(Root, TEXT("*")), true, false);
    TestEqual(TEXT("Successful saves leave only the primary file"), Files.Num(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DDataFolderMigrationTest,
    "V3DSimulator.Streaming.DataFolderMigration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DDataFolderMigrationTest::RunTest(const FString&)
{
    const FString Root = FSafeFileIO::NormalizeFilePath(FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("Automation"), FGuid::NewGuid().ToString()));
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
    const FString WorldRoot = FPaths::Combine(Root, TEXT("Worlds"), TEXT("TestWorld"));
    const FString NewPath = FPaths::Combine(Root, TEXT("Data"), TEXT("TestWorld.dat"));
    const FString LegacyPath = FPaths::Combine(Root, TEXT("Worlds"), TEXT("Data"), TEXT("TestWorld.dat"));
    TestEqual(TEXT("Data is a sibling of Worlds"), FEntityArchiveStore::MakeArchivePath(WorldRoot), NewPath);
    FString Error;
    auto Store = FEntityArchiveStore::Open(WorldRoot, true, Error);
    if (!TestTrue(TEXT("Create archive fixture"), Store.IsValid())) return false;
    FWorldRuntimeState State;
    State.WorldTime = 123.0f;
    TestTrue(TEXT("Commit fixture state"), Store->SaveRuntimeState(State).IsSuccess());
    const uint64 Generation = Store->GetGeneration();
    Store.Reset();
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(LegacyPath), true);
    if (!TestTrue(TEXT("Seed legacy layout"), FPlatformFileManager::Get().GetPlatformFile().MoveFile(*LegacyPath, *NewPath))) return false;
    Store = FEntityArchiveStore::Open(WorldRoot, false, Error);
    if (!TestTrue(TEXT("Migrate existing data even with creation disabled"), Store.IsValid())) return false;
    TestEqual(TEXT("Migration preserves commits"), Store->GetGeneration(), Generation);
    bool bMissing = true;
    FWorldRuntimeState Loaded;
    TestTrue(TEXT("Read migrated state"), Store->LoadRuntimeState(Loaded, bMissing, Error));
    TestFalse(TEXT("State was not replaced by an empty world"), bMissing);
    TestEqual(TEXT("State value preserved"), Loaded.WorldTime, 123.0f);
    TestFalse(TEXT("Legacy file moved, not duplicated"), IFileManager::Get().FileExists(*LegacyPath));
    Store.Reset();
    const TSharedRef<FJsonObject> Sentinel = MakeShared<FJsonObject>();
    Sentinel->SetStringField(TEXT("Sentinel"), TEXT("do not overwrite"));
    TestTrue(TEXT("Create conflict fixture"), FSafeFileIO::SaveJsonBlocking(Sentinel, LegacyPath).IsSuccess());
    Store = FEntityArchiveStore::Open(WorldRoot, false, Error);
    if (TestTrue(TEXT("New path wins when both exist"), Store.IsValid()))
        TestEqual(TEXT("New data remains intact"), Store->GetGeneration(), Generation);
    TestTrue(TEXT("Conflicting legacy file is preserved"), IFileManager::Get().FileExists(*LegacyPath));
    Store.Reset();
    return true;
}
#endif
