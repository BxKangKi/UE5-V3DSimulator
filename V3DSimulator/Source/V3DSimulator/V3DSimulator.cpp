// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file V3DSimulator.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "V3DSimulator.h"

#include "MoviePlayer.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"
#include "System/SimulatorFileServices.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameModeBase.h"
#include "Framework/Application/SlateApplication.h"
#include "System/SafeFileIO.h"
#include "System/V3DRuntimeSafety.h"
#include "System/V3DStreamingBudget.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    /**
     * Commandlets and dedicated servers do not create a game viewport. Avoid touching MoviePlayer
     * in those processes, and respect platforms/build modes where the engine disables it.
     */
    bool CanUseV3DSimulatorLoadingScreen()
    {
        return !IsRunningCommandlet() &&
            !IsRunningDedicatedServer() &&
            IsMoviePlayerEnabled();
    }

}

void FV3DSimulatorModule::StartupModule()
{
    FDefaultGameModuleImpl::StartupModule();
    FSimulatorFileServices::WriteStartupLog(TEXT("=== V3DSimulator module started ==="));
    PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddRaw(
        this, &FV3DSimulatorModule::HandlePostLoadMap);

    if (!CanUseV3DSimulatorLoadingScreen())
    {
        return;
    }

    // Arm only for an actual map load. The global OnPrepareLoadingScreen hook
    // also runs for startup movies; continuously re-arming it can obscure the
    // game viewport after loading has completed on packaged launch paths.
    PreLoadMapHandle = FCoreUObjectDelegates::PreLoadMap.AddRaw(
        this, &FV3DSimulatorModule::HandlePreLoadMap);
}

void FV3DSimulatorModule::PrepareLoadingScreen()
{
    if (!CanUseV3DSimulatorLoadingScreen())
    {
        return;
    }

    // StartupModule can be entered before Slate on unusual launch paths. Building an SWidget before
    // FSlateApplication exists is unsafe, so let the registered engine callbacks retry later.
    if (!FSlateApplication::IsInitialized())
    {
        return;
    }

    IGameMoviePlayer* MoviePlayer = GetMoviePlayer();
    if (!MoviePlayer || MoviePlayer->IsMovieCurrentlyPlaying())
    {
        return;
    }

    FLoadingScreenAttributes LoadingScreen;
    LoadingScreen.bAllowEngineTick = false;
    LoadingScreen.bAllowInEarlyStartup = false;
    LoadingScreen.bAutoCompleteWhenLoadingCompletes = true;
    LoadingScreen.bMoviesAreSkippable = false;
    LoadingScreen.bWaitForManualStop = false;
    LoadingScreen.MinimumLoadingScreenDisplayTime = 0.0f;

    // No UMG/material package dependency: this message can render while cooked assets are loading.
    LoadingScreen.WidgetLoadingScreen = SNew(SBorder)
        .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
        .BorderBackgroundColor(FLinearColor(0.035f, 0.045f, 0.065f, 1.0f))
        .HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(32.0f)
        [
            SNew(STextBlock)
            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 20))
            .ColorAndOpacity(FLinearColor::White)
            .Text(NSLOCTEXT("V3DStartup", "LoadingMap", "V3DSimulator - Loading..."))
        ];
    MoviePlayer->SetupLoadingScreen(LoadingScreen);
    bOwnsLoadingScreen = true;
}

void FV3DSimulatorModule::HandlePreLoadMap(const FString& MapName)
{
    FSimulatorFileServices::WriteStartupLog(TEXT("Loading map: ") + MapName);
    // MapName is intentionally not dereferenced or resolved here: package lookup can create UObjects,
    // while this callback only needs to arm the already self-contained Slate loading screen.
    UE_LOG(LogTemp, VeryVerbose, TEXT("Preparing blocking loading screen for map: %s"), *MapName);
    PrepareLoadingScreen();
}

void FV3DSimulatorModule::HandlePostLoadMap(UWorld* World)
{
    // Finish only our map-loading screen. Never wait here: the engine must
    // regain the game thread so local-player/UMG initialization can continue.
    if (bOwnsLoadingScreen)
    {
        bOwnsLoadingScreen = false;
        if (IGameMoviePlayer* MoviePlayer = GetMoviePlayer()) MoviePlayer->StopMovie();
    }
    FSimulatorFileServices::WriteStartupLog(FString::Printf(
        TEXT("Map loaded: %s; GameInstance=%s; GameMode=%s"),
        *GetPathNameSafe(World),
        *GetNameSafe(World && World->GetGameInstance() ? World->GetGameInstance()->GetClass() : nullptr),
        *GetNameSafe(World && World->GetAuthGameMode() ? World->GetAuthGameMode()->GetClass() : nullptr)));
}

void FV3DSimulatorModule::ShutdownModule()
{
    FSimulatorFileServices::WriteStartupLog(TEXT("V3DSimulator module shutting down"));
    FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
    PostLoadMapHandle.Reset();
    // Remove all engine callbacks first. This guarantees that no late map-load notification can call
    // into the game module after its shutdown sequence has begun.
    if (PreLoadMapHandle.IsValid())
    {
        FCoreUObjectDelegates::PreLoadMap.Remove(PreLoadMapHandle);
        PreLoadMapHandle.Reset();
    }

    if (bOwnsLoadingScreen)
    {
        bOwnsLoadingScreen = false;
        if (IGameMoviePlayer* MoviePlayer = GetMoviePlayer()) MoviePlayer->StopMovie();
    }

    // Stop accepting new native mesh work first, then reject queued requests. An active plugin job
    // is allowed to reach its terminal callback because interrupting it could free parser memory
    // while a glTFRuntime worker still references it.
    FV3DStreamingBudget::Shutdown();
    FV3DRuntimeSafety::BeginShutdown();

    // Stop new disk transactions and wait for already accepted atomic saves to finish. This greatly
    // reduces the chance of leaving only an incomplete temporary file during a normal application exit.
    FSafeFileIO::BeginShutdown();

    // A timeout followed by DLL unload is more dangerous than a slow shutdown: a native parser or
    // queued lambda could still return into unloaded module code. Drain both lifetime trackers fully.
    FV3DRuntimeSafety::FlushPendingOperations(-1.0);
    FSafeFileIO::FlushPendingOperations(-1.0);

    FDefaultGameModuleImpl::ShutdownModule();
}

IMPLEMENT_PRIMARY_GAME_MODULE(FV3DSimulatorModule, V3DSimulator, "V3DSimulator");
