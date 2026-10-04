// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file MainGameMode.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "GameMode/MainGameMode.h"

#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "HAL/PlatformTime.h"
#include "UObject/UObjectGlobals.h"
#include "ShaderPipelineCache.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "System/SimulatorFileServices.h"
#include "System/GameManagerSubSystem.h"
#include "System/MacroLibrary.h"
#include "System/SimulatorPaths.h"
#include "System/MultiplayerWorldSubSystem.h"
#include "System/ProjectConfig.h"
#include "TimerManager.h"
#include "UI/StartWorldWidget.h"
#include "UI/WorldSelectionWidget.h"
#include "UI/SettingsMenuWidget.h"
#include "UI/ProjectSelectionWidget.h"
#include "Blueprint/UserWidget.h"
#include "System/V3DSimulatorGameInstance.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/SimulatorFileServices.h"
#include "System/WorldArchive.h"
#include "System/SafeFileIO.h"
#include "World/WorldData.h"

namespace
{
    FString NormalizeStartWorldString(FString Value)
    {
        Value.TrimStartAndEndInline();
        return Value;
    }

    bool ValidateLaunchWorld(const TSoftObjectPtr<UWorld>& WorldAsset, const TCHAR* Label)
    {
        if (WorldAsset.IsNull())
        {
            UE_LOG(LogTemp, Error, TEXT("[WorldTravel] %s world is not assigned in AssetRegistry."), Label);
            return false;
        }

        const FSoftObjectPath Path = WorldAsset.ToSoftObjectPath();
        if (!Path.IsValid() || Path.GetLongPackageName().IsEmpty())
        {
            UE_LOG(LogTemp, Error, TEXT("[WorldTravel] %s world reference is invalid: %s"),
                Label, *Path.ToString());
            return false;
        }
        return true;
    }


    bool IsUsableWidgetClass(UClass* WidgetClass, UClass* RequiredBaseClass)
    {
        return IsValid(WidgetClass)
            && IsValid(RequiredBaseClass)
            && WidgetClass->IsChildOf(RequiredBaseClass)
            && !WidgetClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
    }

    bool SameWorldAsset(const TSoftObjectPtr<UWorld>& A, const TSoftObjectPtr<UWorld>& B)
    {
        if (A.IsNull() || B.IsNull())
        {
            return false;
        }
        const FString APackage = A.ToSoftObjectPath().GetLongPackageName();
        const FString BPackage = B.ToSoftObjectPath().GetLongPackageName();
        return !APackage.IsEmpty() && APackage.Equals(BPackage, ESearchCase::IgnoreCase);
    }

    FString CanonicalMenuMapPackage(FString Package)
    {
        Package = FPackageName::ObjectPathToPackageName(Package);
        int32 Slash = INDEX_NONE;
        Package.FindLastChar(TEXT('/'), Slash);
        const FString Folder = Package.Left(Slash + 1);
        FString Leaf = Package.Mid(Slash + 1);
        if (Leaf.StartsWith(TEXT("UEDPIE_")))
        {
            const int32 Separator = Leaf.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 7);
            if (Separator > 7 && Leaf.Mid(7, Separator - 7).IsNumeric())
            {
                Leaf = Leaf.Mid(Separator + 1);
            }
        }
        return Folder + Leaf;
    }

    bool CurrentWorldMatches(const UWorld* World, const TSoftObjectPtr<UWorld>& Expected)
    {
        if (!IsValid(World) || Expected.IsNull()) return false;
        const FString ActualPackage = CanonicalMenuMapPackage(World->GetOutermost()->GetName());
        const FString ExpectedPackage = CanonicalMenuMapPackage(
            Expected.ToSoftObjectPath().GetLongPackageName());
        return !ActualPackage.IsEmpty() && !ExpectedPackage.IsEmpty()
            && ActualPackage.Equals(ExpectedPackage, ESearchCase::IgnoreCase);
    }

}

AMainGameMode::AMainGameMode()
{
    PrimaryActorTick.bCanEverTick = false;

    PendingServerAddress = DefaultServerAddress;

    // Registry widget classes are auto-created in BeginPlay. Set*Widget remains an optional
    // compatibility/override path for projects that explicitly construct a custom instance.
}

void AMainGameMode::SetStartMenuWidget(UStartWorldWidget* InWidget)
{
    StartMenuWidget = InWidget;
    if (IsValid(InWidget))
    {
        InWidget->SetMainGameMode(this);
    }
}

void AMainGameMode::SetWorldSelectionWidget(UWorldSelectionWidget* InWidget)
{
    WorldSelectionWidget = InWidget;
    if (IsValid(InWidget))
    {
        InWidget->SetMainGameMode(this);
        InWidget->SetWorldSelectionData(FolderNameMap);
    }
}

void AMainGameMode::SetMultiplayerMenuWidget(UStartWorldWidget* InWidget)
{
    MultiplayerMenuWidget = InWidget;
    if (IsValid(InWidget))
    {
        InWidget->SetMainGameMode(this);
        InWidget->SetWorldSelectionData(FolderNameMap);
    }
}

void AMainGameMode::SetSettingsWidget(USettingsMenuWidget* InWidget)
{
    if (IsValid(SettingsWidget))
    {
        SettingsWidget->OnCloseRequested.RemoveDynamic(this, &AMainGameMode::ReturnFromSettings);
    }
    SettingsWidget = InWidget;
    if (IsValid(SettingsWidget))
    {
        SettingsWidget->OnCloseRequested.RemoveDynamic(this, &AMainGameMode::ReturnFromSettings);
        SettingsWidget->OnCloseRequested.AddDynamic(this, &AMainGameMode::ReturnFromSettings);
        SettingsWidget->SetVisibility(ESlateVisibility::Collapsed);
    }
}

bool AMainGameMode::EnsureSettingsMenuWidget()
{
    UWorld* World = GetWorld();
    APlayerController* PlayerController = IsValid(World) ? UGameplayStatics::GetPlayerController(this, 0) : nullptr;
    if (!IsValid(World) || !IsValid(PlayerController) || !PlayerController->IsLocalController()
        || !PlayerController->GetLocalPlayer() || !World->GetGameViewport())
        return false;
    if (!IsValid(SettingsWidget))
    {
        UV3DSimulatorAssetRegistry* Registry = UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this);
        if (!IsValid(Registry)) return false;
        Registry->EnsureMenuDefaults();
        UClass* WidgetClass = Registry->SettingsMenuWidgetClass.LoadSynchronous();
        if (!IsUsableWidgetClass(WidgetClass, USettingsMenuWidget::StaticClass()))
        {
            UE_LOG(LogTemp, Error, TEXT("Cannot load settings menu class: %s"),
                *Registry->SettingsMenuWidgetClass.ToSoftObjectPath().ToString());
            return false;
        }
        SetSettingsWidget(CreateWidget<USettingsMenuWidget>(PlayerController, WidgetClass));
    }
    return IsValid(SettingsWidget)
        && (SettingsWidget->IsInViewport() || SettingsWidget->AddToPlayerScreen(20));
}

void AMainGameMode::ShowSettingsMenu()
{
    if (bGameplayWorldTravelPending || !EnsureSettingsMenuWidget())
    {
        UE_LOG(LogTemp, Warning, TEXT("Settings menu is not ready; keeping the current menu visible."));
        return;
    }
    HideAllMenuWidgets();
    SettingsWidget->InitializeSettingsFromSavedData();
    SettingsWidget->SetIsEnabled(true);
    SettingsWidget->SetVisibility(ESlateVisibility::Visible);
    ApplyMenuInputMode(SettingsWidget.Get());
}

void AMainGameMode::ReturnFromSettings()
{
    if (IsValid(SettingsWidget))
    {
        SettingsWidget->SetVisibility(ESlateVisibility::Collapsed);
    }
    ShowStartMenu();
}

void AMainGameMode::BeginPlay()
{
    FSimulatorFileServices::WriteStartupLog(TEXT("MainGameMode BeginPlay entered"));

    if (const UWorld* World = GetWorld())
    {
        const AWorldSettings* WorldSettings = World->GetWorldSettings();
        UClass* ConfiguredGameMode = IsValid(WorldSettings) ? WorldSettings->DefaultGameMode.Get() : nullptr;
        if (!IsValid(ConfiguredGameMode))
        {
            UE_LOG(LogTemp, Error, TEXT("[WorldSettings] MainWorld has no explicit GameMode Override. Assign AMainGameMode (or its Blueprint subclass) in this map's World Settings."));
        }
        else if (!ConfiguredGameMode->IsChildOf(AMainGameMode::StaticClass()))
        {
            UE_LOG(LogTemp, Error, TEXT("[WorldSettings] MainWorld GameMode Override is %s, not an AMainGameMode subclass."), *ConfiguredGameMode->GetPathName());
        }
    }
    // The registry is class-backed and privately instantiated by the GameInstance. Prepare it before
    // Blueprint ReceiveBeginPlay so even legacy menu Blueprint code can resolve central assets.
    if (UV3DSimulatorGameInstance* SimulatorGameInstance = Cast<UV3DSimulatorGameInstance>(GetGameInstance()))
    {
        SimulatorGameInstance->EnsureAssetRegistry();
    }

    if (const UV3DSimulatorAssetRegistry* Registry = UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this))
    {
        FSimulatorFileServices::WriteStartupLog(FString::Printf(
            TEXT("MainGameMode map binding: ActiveMap=%s ActiveGM=%s RegistryMainWorld=%s (GameMode comes from World Settings)"),
            GetWorld() ? *GetWorld()->GetOutermost()->GetName() : TEXT("<none>"),
            *GetClass()->GetPathName(),
            *Registry->MainWorld.ToSoftObjectPath().ToString()));

        if (Registry->MainWorld.IsNull())
        {
            UE_LOG(LogTemp, Error, TEXT("MainGameMode is running but AssetRegistry.MainWorld is empty."));
        }
        else if (GetWorld() && !CurrentWorldMatches(GetWorld(), Registry->MainWorld))
        {
            UE_LOG(LogTemp, Error,
                TEXT("MainGameMode is running on a map that is not AssetRegistry.MainWorld. Active=%s RegistryMainWorld=%s"),
                *GetWorld()->GetOutermost()->GetName(),
                *Registry->MainWorld.ToSoftObjectPath().ToString());
        }
    }

    // Arm before Super::BeginPlay so a legacy Blueprint ReceiveBeginPlay cannot consume the
    // carried-over Exit click before the native menu is rebuilt later in this same BeginPlay call.
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        bWorldSelectionReturnInputGuardActive = GameManager->ShouldOpenWorldSelectionMenuOnNextMainWorld();
    }

    Super::BeginPlay();

    // Rebuild the level list before any UI asks for it.
    BuildLevelFolderNameMap();

    // Blueprint ReceiveBeginPlay has completed when Super::BeginPlay() returns. Keep any instances
    // explicitly registered by Blueprint, then fill only the missing slots from AssetRegistryClass.
    // BeginPlay can precede local-player/viewport readiness in PIE and packaged startup.
    if (GetNetMode() != NM_DedicatedServer)
    {
        StartScreenInitializationStartedAt = FPlatformTime::Seconds();
        TryInitializeStartScreen();
    }
}

void AMainGameMode::TryInitializeStartScreen()
{
    if (bStartupEnding || bStartScreenInitialized || GetNetMode() == NM_DedicatedServer)
    {
        GetWorldTimerManager().ClearTimer(StartScreenInitializationHandle);
        ClearStartupStatus();
        return;
    }
    ++StartScreenInitializationAttempts;
    const double Now = FPlatformTime::Seconds();
    const double Elapsed = FMath::Max(0.0, Now - StartScreenInitializationStartedAt);
    ShowStartupStatus(Elapsed < 30.0
        ? NSLOCTEXT("V3DStartup", "LoadingMenu", "Starting V3DSimulator... Loading the main menu.")
        : NSLOCTEXT("V3DStartup", "DelayedMenu", "The main menu is taking longer than expected. See startup.log for loading details."));
    if (InitializeRegistryDrivenUI())
    {
        if (bStartupEnding) return; // Widget Blueprint callbacks can initiate map travel.
        FSimulatorFileServices::WriteStartupLog(FString::Printf(
            TEXT("Start menu created and attached after %.2fs; PSO requests remaining=%u"),
            FPlatformTime::Seconds() - StartScreenInitializationStartedAt,
            FShaderPipelineCache::NumPrecompilesRemaining()));
        bStartScreenInitialized = true;
        GetWorldTimerManager().ClearTimer(StartScreenInitializationHandle);
        ClearStartupStatus();
        InitializeStartScreenAfterBlueprintBeginPlay();
        return;
    }
    if (bStartupEnding) return;
    if (Now >= NextStartupLogTime)
    {
        NextStartupLogTime = Now + 10.0;
        LogStartupReadiness(Elapsed);
    }
    // A repeating timer can execute many overdue callbacks after a driver/IO stall and consume the
    // old 100-attempt budget immediately. Re-arm once, and keep recovering when the viewport arrives.
    // PSO work is diagnostic only: never hide the menu waiting for a global shader queue to reach zero.
    GetWorldTimerManager().SetTimer(StartScreenInitializationHandle, this,
        &AMainGameMode::TryInitializeStartScreen, Elapsed < 5.0 ? 0.25f : 1.0f, false);
}

void AMainGameMode::ShowStartupStatus(const FText& Message)
{
    UWorld* World = GetWorld();
    UGameViewportClient* Viewport = IsValid(World) ? World->GetGameViewport() : nullptr;
    if (!IsValid(Viewport)) return;
    if (StartupStatusViewport.Get() != Viewport) ClearStartupStatus();
    if (!StartupStatusWidget.IsValid())
    {
        StartupStatusWidget = SNew(SBorder)
            .Visibility(EVisibility::HitTestInvisible)
            .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
            .BorderBackgroundColor(FLinearColor(0.035f, 0.045f, 0.065f, 1.0f))
            .HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(32.0f)
            [
                SAssignNew(StartupStatusText, STextBlock)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 20))
                .ColorAndOpacity(FLinearColor::White)
                .Justification(ETextJustify::Center).AutoWrapText(true).Text(Message)
            ];
        StartupStatusViewport = Viewport;
        Viewport->AddViewportWidgetContent(StartupStatusWidget.ToSharedRef(), 10000);
    }
    else if (StartupStatusText.IsValid())
    {
        StartupStatusText->SetText(Message);
    }
}

void AMainGameMode::ClearStartupStatus()
{
    if (StartupStatusWidget.IsValid())
    {
        if (UGameViewportClient* Viewport = StartupStatusViewport.Get())
            Viewport->RemoveViewportWidgetContent(StartupStatusWidget.ToSharedRef());
    }
    StartupStatusText.Reset();
    StartupStatusWidget.Reset();
    StartupStatusViewport.Reset();
}

void AMainGameMode::LogStartupReadiness(double ElapsedSeconds) const
{
    UWorld* World = GetWorld();
    APlayerController* Player = IsValid(World) ? UGameplayStatics::GetPlayerController(this, 0) : nullptr;
    const UV3DSimulatorGameInstance* Instance = Cast<UV3DSimulatorGameInstance>(GetGameInstance());
    const UV3DSimulatorAssetRegistry* Registry = IsValid(Instance) ? Instance->GetAssetRegistry() : nullptr;
    FSimulatorFileServices::WriteStartupLog(FString::Printf(
        TEXT("UI startup pending: elapsed=%.2fs player=%d localPlayer=%d gameViewport=%d registry=%s startClass=%s classLoaded=%d PSORemaining=%u asyncLoading=%d failure=%s"),
        ElapsedSeconds, IsValid(Player), IsValid(Player) && Player->GetLocalPlayer() != nullptr,
        IsValid(World) && World->GetGameViewport() != nullptr, *GetNameSafe(Registry),
        Registry ? *Registry->StartMenuWidgetClass.ToSoftObjectPath().ToString() : TEXT("<none>"),
        Registry && Registry->StartMenuWidgetClass.IsValid(),
        FShaderPipelineCache::NumPrecompilesRemaining(), IsAsyncLoading(), *LastStartScreenFailure));
}

bool AMainGameMode::InitializeRegistryDrivenUI()
{
    LastStartScreenFailure.Reset();
    UWorld* World = GetWorld();
    APlayerController* PlayerController = IsValid(World) ? UGameplayStatics::GetPlayerController(this, 0) : nullptr;
    if (!IsValid(World) || !IsValid(PlayerController) || !PlayerController->IsLocalController()
        || !PlayerController->GetLocalPlayer() || !World->GetGameViewport())
    {
        LastStartScreenFailure = TEXT("Local player or game viewport is not ready");
        return false;
    }

    UV3DSimulatorAssetRegistry* Registry = UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this);
    if (!IsValid(Registry))
    {
        LastStartScreenFailure = TEXT("GameInstance could not initialize AssetRegistry");
        UE_LOG(LogTemp, Error, TEXT("MainGameMode cannot initialize registry-driven UI because the central AssetRegistry is unavailable."));
        return false;
    }

    const auto AddTopLevelWidget = [](UUserWidget* Widget, const int32 ZOrder)
    {
        if (IsValid(Widget) && !Widget->IsInViewport())
        {
            Widget->AddToPlayerScreen(ZOrder);
        }
    };

    if (!IsValid(StartMenuWidget))
    {
        UClass* WidgetClass = Registry->StartMenuWidgetClass.LoadSynchronous();
        if (!IsUsableWidgetClass(WidgetClass, UStartWorldWidget::StaticClass()))
        {
            LastStartScreenFailure = FString::Printf(
                TEXT("StartMenu class load/validation failed: path=%s resolved=%s expectedBase=%s flags=0x%08x"),
                *Registry->StartMenuWidgetClass.ToSoftObjectPath().ToString(),
                *GetPathNameSafe(WidgetClass), *UStartWorldWidget::StaticClass()->GetPathName(),
                IsValid(WidgetClass) ? static_cast<uint32>(WidgetClass->GetClassFlags()) : 0u);
            return false;
        }
        UStartWorldWidget* Widget = CreateWidget<UStartWorldWidget>(PlayerController, WidgetClass);
        if (!IsValid(Widget))
        {
            LastStartScreenFailure = FString::Printf(
                TEXT("CreateWidget failed: class=%s player=%s world=%s"),
                *WidgetClass->GetPathName(), *GetPathNameSafe(PlayerController), *GetPathNameSafe(World));
            return false;
        }
        if (bStartupEnding) return false;
        SetStartMenuWidget(Widget);
        AddTopLevelWidget(Widget, 10);
        if (bStartupEnding) return false;
    }

    if (!IsValid(WorldSelectionWidget) && !Registry->WorldSelectionWidgetClass.IsNull())
    {
        if (UClass* WidgetClass = Registry->WorldSelectionWidgetClass.LoadSynchronous();
            IsUsableWidgetClass(WidgetClass, UWorldSelectionWidget::StaticClass()))
        {
            UWorldSelectionWidget* Widget = CreateWidget<UWorldSelectionWidget>(PlayerController, WidgetClass);
            if (bStartupEnding) return false;
            SetWorldSelectionWidget(Widget);
            AddTopLevelWidget(Widget, 11);
            if (bStartupEnding) return false;
        }
    }

    if (!IsValid(MultiplayerMenuWidget) && !Registry->MultiplayerMenuWidgetClass.IsNull())
    {
        if (UClass* WidgetClass = Registry->MultiplayerMenuWidgetClass.LoadSynchronous();
            IsUsableWidgetClass(WidgetClass, UStartWorldWidget::StaticClass()))
        {
            UStartWorldWidget* Widget = CreateWidget<UStartWorldWidget>(PlayerController, WidgetClass);
            if (bStartupEnding) return false;
            SetMultiplayerMenuWidget(Widget);
            AddTopLevelWidget(Widget, 12);
            if (bStartupEnding) return false;
        }
    }

    EnsureSettingsMenuWidget();
    if (bStartupEnding) return false;

    // Use the same creation/attachment path here and when the Projects button is clicked.
    if (IsValid(StartMenuWidget))
        StartMenuWidget->EnsureProjectSelectionWidget();
    if (bStartupEnding) return false;

    AddTopLevelWidget(StartMenuWidget.Get(), 10);
    AddTopLevelWidget(WorldSelectionWidget.Get(), 11);
    AddTopLevelWidget(MultiplayerMenuWidget.Get(), 12);
    AddTopLevelWidget(SettingsWidget.Get(), 20);
    if (IsValid(StartMenuWidget) && IsValid(StartMenuWidget->GetProjectSelectionWidget()))
        AddTopLevelWidget(StartMenuWidget->GetProjectSelectionWidget(), 30);
    if (StartScreenInitializationAttempts == 1)
    {
        FSimulatorFileServices::WriteStartupLog(FString::Printf(TEXT("Start menu class=%s; instance=%s; widgetInViewport=%d"),
            *Registry->StartMenuWidgetClass.ToSoftObjectPath().ToString(), *GetNameSafe(StartMenuWidget.Get()),
            IsValid(StartMenuWidget) && StartMenuWidget->IsInViewport()));
    }
    HideAllMenuWidgets();
    const bool bMenuAttached = IsValid(StartMenuWidget) && StartMenuWidget->IsInViewport();
    if (!bMenuAttached)
        LastStartScreenFailure = TEXT("StartMenu was created but could not attach to the local player screen");
    return bMenuAttached;
}

void AMainGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    bStartupEnding = true;
    ClearStartupStatus();
    if (IsValid(SettingsWidget))
    {
        SettingsWidget->OnCloseRequested.RemoveDynamic(this, &AMainGameMode::ReturnFromSettings);
    }

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(StartScreenInitializationHandle);
        World->GetTimerManager().ClearTimer(GameplayTravelWatchdogHandle);
        World->GetTimerManager().ClearTimer(WorldSelectionReturnInputGuardHandle);
    }
    HideAllMenuWidgets();

    if (IsValid(StartMenuWidget) && IsValid(StartMenuWidget->GetProjectSelectionWidget())) StartMenuWidget->GetProjectSelectionWidget()->RemoveFromParent();
    if (IsValid(StartMenuWidget)) StartMenuWidget->RemoveFromParent();
    if (IsValid(WorldSelectionWidget)) WorldSelectionWidget->RemoveFromParent();
    if (IsValid(MultiplayerMenuWidget)) MultiplayerMenuWidget->RemoveFromParent();
    if (IsValid(SettingsWidget)) SettingsWidget->RemoveFromParent();
    StartMenuWidget = nullptr;
    WorldSelectionWidget = nullptr;
    MultiplayerMenuWidget = nullptr;
    SettingsWidget = nullptr;

    // Do not force garbage collection from EndPlay. During level travel this can block asset loading
    // and make the editor/game appear stuck around the loading-progress phase. Editor-only world-travel
    // cleanup is owned by the engine/runtime teardown path; no project editor module participates.
    Super::EndPlay(EndPlayReason);
}

void AMainGameMode::InitializeStartScreenAfterBlueprintBeginPlay()
{
    bool bOpenWorldSelection = false;

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        // Runtime world memory is released only after the destination menu level has finished loading.
        if (GameManager->HasPendingMainWorldRuntimePurge())
        {
            GameManager->ReleaseMainWorldRuntimeMemory(true);
        }

        // Keep the request alive until the return-input guard is released. This lets lower-level
        // world-travel code reject any stale callback that tries to reopen gameplay during arrival.
        bOpenWorldSelection = GameManager->ShouldOpenWorldSelectionMenuOnNextMainWorld();
    }

    if (bOpenWorldSelection)
    {
        // Arm before widget construction so legacy WBP Construct callbacks cannot immediately
        // reopen the previously selected gameplay world.
        bWorldSelectionReturnInputGuardActive = true;
        ShowWorldSelectionMenu();
    }
    else
    {
        CancelWorldSelectionReturnInputGuard();
        ShowStartMenu();
    }
}

void AMainGameMode::StartGame()
{
    ShowWorldSelectionMenu();
}

void AMainGameMode::ReturnToMainMenuFromWorldSelection()
{
    if (IsWorldSelectionReturnBlocked())
    {
        UE_LOG(LogTemp, Display,
            TEXT("[WorldSelection] Ignored a carried-over Back callback while the return input guard is active."));
        return;
    }

    if (bGameplayWorldTravelPending)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[WorldSelection] Ignored a return-to-main-menu callback because travel to world '%s' is already pending."),
            *PendingGameplayWorldFolderName);
        return;
    }

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->ClearWorldSelectionMenuRequest();
        GameManager->SetGamePaused(false);
    }

    ShowStartMenu();
}

void AMainGameMode::ShowStartMenu()
{
    if (IsWorldSelectionReturnBlocked() || bGameplayWorldTravelPending)
    {
        return;
    }
    CancelWorldSelectionReturnInputGuard();
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->ClearWorldSelectionMenuRequest();
    }
    HideAllMenuWidgets();
    if (IsValid(StartMenuWidget))
    {
        StartMenuWidget->SetVisibility(ESlateVisibility::Visible);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("MainGameMode has no registered StartMenu widget."));
    }
    ApplyMenuInputMode(StartMenuWidget.Get());
}

void AMainGameMode::ShowWorldSelectionMenu()
{
    if (bGameplayWorldTravelPending)
    {
        return;
    }
    BuildLevelFolderNameMap();
    if (!IsValid(WorldSelectionWidget) || !WorldSelectionWidget->IsInViewport()) return;
    HideAllMenuWidgets();
    if (IsValid(WorldSelectionWidget))
    {
        WorldSelectionWidget->SetWorldSelectionData(FolderNameMap);
        WorldSelectionWidget->SetVisibility(ESlateVisibility::Visible);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("MainGameMode has no registered WorldSelection widget."));
    }
    ApplyMenuInputMode(WorldSelectionWidget.Get());
    if (bWorldSelectionReturnInputGuardActive)
    {
        StartWorldSelectionReturnInputGuard();
    }
}

void AMainGameMode::ShowMultiplayerMenu()
{
    if (IsWorldSelectionReturnBlocked())
    {
        return;
    }
    CancelWorldSelectionReturnInputGuard();
    BuildLevelFolderNameMap();
    if (!IsValid(MultiplayerMenuWidget) || !MultiplayerMenuWidget->IsInViewport()) return;
    HideAllMenuWidgets();
    if (IsValid(MultiplayerMenuWidget))
    {
        MultiplayerMenuWidget->SetWorldSelectionData(FolderNameMap);
        MultiplayerMenuWidget->SetVisibility(ESlateVisibility::Visible);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("MainGameMode has no registered Multiplayer widget."));
    }
    ApplyMenuInputMode(MultiplayerMenuWidget.Get());
}

void AMainGameMode::RefreshWorldFolderNameMap()
{
    BuildLevelFolderNameMap();

    if (IsValid(WorldSelectionWidget))
    {
        WorldSelectionWidget->SetWorldSelectionData(FolderNameMap);
    }

    if (IsValid(MultiplayerMenuWidget))
    {
        MultiplayerMenuWidget->SetWorldSelectionData(FolderNameMap);
    }
}

bool AMainGameMode::TryResolveWorldFolderKey(const FString& WorldFolderKey, FString& OutFolderName) const
{
    const FString ExactKey = WorldFolderKey.TrimStartAndEnd();
    FString SafeKey;
    if (!UGameManagerSubSystem::TryNormalizeWorldFolderName(ExactKey, SafeKey, false)
        || SafeKey != ExactKey
        || !FolderNameMap.Contains(ExactKey))
    {
        OutFolderName.Reset();
        return false;
    }

    OutFolderName = ExactKey;
    return true;
}

const FV3DWorldLaunchProfile* AMainGameMode::FindWorldLaunchProfile(const FString& WorldFolderName) const
{
    const FString NormalizedFolderName = NormalizeStartWorldString(WorldFolderName);
    if (NormalizedFolderName.IsEmpty())
    {
        return nullptr;
    }

    const UV3DSimulatorAssetRegistry* Registry =
        UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this);
    if (!IsValid(Registry))
    {
        return nullptr;
    }

    const FV3DWorldLaunchProfile* Match = nullptr;
    for (const FV3DWorldLaunchProfile& Profile : Registry->WorldLaunchProfiles)
    {
        const FString ProfileFolderName = NormalizeStartWorldString(Profile.WorldFolderName);
        if (!ProfileFolderName.Equals(NormalizedFolderName, ESearchCase::IgnoreCase))
        {
            continue;
        }

        if (Match)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[WorldTravel] Duplicate launch profile for folder '%s'. The first profile is used."),
                *NormalizedFolderName);
            continue;
        }
        Match = &Profile;
    }

    return Match;
}

void AMainGameMode::ResolveWorldLaunch(
    const FString& WorldFolderName,
    bool bForHost,
    TSoftObjectPtr<UWorld>& OutWorld,
    FString& OutResolutionSource) const
{
    const UV3DSimulatorAssetRegistry* Registry = UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this);
    OutWorld = Registry ? (bForHost ? Registry->HostWorld : Registry->GameplayWorld) : TSoftObjectPtr<UWorld>();
    OutResolutionSource = bForHost ? FString(TEXT("Common HostWorld (GameMode from World Settings)")) : FString(TEXT("Common GameplayWorld (GameMode from World Settings)"));
    const FV3DWorldLaunchProfile* Profile = FindWorldLaunchProfile(WorldFolderName);
    if (!Profile) return;
    TSoftObjectPtr<UWorld> ProfileWorld = bForHost ? Profile->HostWorld : Profile->SinglePlayerWorld;
    if (!ProfileWorld.IsNull())
    {
        OutWorld = ProfileWorld;
        OutResolutionSource = TEXT("Profile world (GameMode from World Settings)");
    }
}

void AMainGameMode::OpenSinglePlayerWorldByFolderName(const FString& WorldFolderName)
{
    if (IsWorldSelectionReturnBlocked())
    {
        UE_LOG(LogTemp, Display,
            TEXT("[WorldSelection] Ignored carried-over activation for '%s' while the return input guard is active."),
            *WorldFolderName);
        return;
    }

    if (bGameplayWorldTravelPending)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[WorldSelection] Ignored duplicate world click '%s'; travel to '%s' is already pending."),
            *WorldFolderName,
            *PendingGameplayWorldFolderName);
        return;
    }

    FString ResolvedFolderName;
    if (!TryResolveWorldFolderKey(WorldFolderName, ResolvedFolderName))
    {
        UE_LOG(LogTemp, Warning, TEXT("MainGameMode cannot open single-player world. Unknown world folder key: %s"), *WorldFolderName);
        return;
    }

    TSoftObjectPtr<UWorld> SelectedGameplayWorld;
    FString LaunchResolutionSource;
    ResolveWorldLaunch(ResolvedFolderName, false, SelectedGameplayWorld, LaunchResolutionSource);

    if (SelectedGameplayWorld.IsNull())
    {
        UE_LOG(LogTemp, Error,
            TEXT("[WorldSelection] Cannot open world '%s': neither the folder profile nor GameplayWorld has a map assigned."),
            *ResolvedFolderName);
        return;
    }

    if (!ValidateLaunchWorld(SelectedGameplayWorld, TEXT("single-player gameplay")))
    {
        UE_LOG(LogTemp, Error, TEXT("[WorldSelection] Refused to load '%s' because its world reference is invalid."), *ResolvedFolderName);
        return;
    }

    if (const UV3DSimulatorAssetRegistry* Registry = UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this);
        IsValid(Registry) && SameWorldAsset(SelectedGameplayWorld, Registry->MainWorld))
    {
        UE_LOG(LogTemp, Error,
            TEXT("[WorldTravel] Refused world '%s': GameplayWorld resolves to AssetRegistry.MainWorld (%s). "
                 "MainWorld is UI-only and must not be reused as the gameplay map."),
            *ResolvedFolderName,
            *Registry->MainWorld.ToSoftObjectPath().ToString());
        return;
    }

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->SetCurrentWorldName(ResolvedFolderName);
        GameManager->ClearWorldSelectionMenuRequest();
        GameManager->SetGamePaused(false);
    }

    bGameplayWorldTravelPending = true;
    PendingGameplayWorldFolderName = ResolvedFolderName;

    UE_LOG(LogTemp, Display,
        TEXT("[WorldTravel] Single-player selection. Folder=%s World=%s GameMode=<map World Settings> Source=%s"),
        *ResolvedFolderName,
        *SelectedGameplayWorld.ToSoftObjectPath().ToString(),
        *LaunchResolutionSource);

    PrepareMenuForWorldTravel();

    // Arm this before world travel. Successful travel destroys this actor and clears the timer; a failed
    // travel leaves the menu world alive and restores the world list instead of showing a blank screen.
    const float WatchdogDelay = FMath::Max(1.0f, GameplayTravelFailureTimeoutSeconds);
    GetWorldTimerManager().SetTimer(
        GameplayTravelWatchdogHandle,
        this,
        &AMainGameMode::HandleGameplayTravelWatchdogExpired,
        WatchdogDelay,
        false);

    bool bTravelRequested = false;
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        bTravelRequested = Multiplayer->StartSinglePlayerWorld(this, ResolvedFolderName, SelectedGameplayWorld);
    }
    else
    {
        UE_LOG(LogTemp, Error,
            TEXT("[WorldSelection] Cannot travel because MultiplayerWorldSubSystem is unavailable; "
                 "refusing the old direct OpenLevel fallback so GameMode selection cannot diverge."));
    }

    if (!bTravelRequested)
    {
        UE_LOG(LogTemp, Error,
            TEXT("[WorldSelection] Failed to request travel to the assigned gameplay world. Folder=%s"),
            *ResolvedFolderName);
        GetWorldTimerManager().ClearTimer(GameplayTravelWatchdogHandle);
        bGameplayWorldTravelPending = false;
        PendingGameplayWorldFolderName.Reset();
        if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
        {
            Multiplayer->ClearRequestedWorld();
        }
        ShowWorldSelectionMenu();
        return;
    }
}

void AMainGameMode::HostMultiplayerWorldByFolderName(const FString& WorldFolderName)
{
    if (IsWorldSelectionReturnBlocked())
    {
        return;
    }

    FString ResolvedFolderName;
    if (!TryResolveWorldFolderKey(WorldFolderName, ResolvedFolderName))
    {
        UE_LOG(LogTemp, Warning, TEXT("MainGameMode cannot host multiplayer world. Unknown world folder key: %s"), *WorldFolderName);
        return;
    }

    TSoftObjectPtr<UWorld> SelectedHostWorld;
    FString LaunchResolutionSource;
    ResolveWorldLaunch(ResolvedFolderName, true, SelectedHostWorld, LaunchResolutionSource);

    if (SelectedHostWorld.IsNull())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("MainGameMode cannot host world '%s': neither the folder profile nor HostWorld has a map assigned."),
            *ResolvedFolderName);
        return;
    }

    if (!ValidateLaunchWorld(SelectedHostWorld, TEXT("host gameplay")))
    {
        UE_LOG(LogTemp, Error, TEXT("[WorldTravel] Refused to host '%s' because its world reference is invalid."), *ResolvedFolderName);
        return;
    }

    if (const UV3DSimulatorAssetRegistry* Registry = UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this);
        IsValid(Registry) && SameWorldAsset(SelectedHostWorld, Registry->MainWorld))
    {
        UE_LOG(LogTemp, Error,
            TEXT("[WorldTravel] Refused host world '%s': HostWorld resolves to AssetRegistry.MainWorld (%s). "
                 "MainWorld is UI-only and must not be reused as the host gameplay map."),
            *ResolvedFolderName,
            *Registry->MainWorld.ToSoftObjectPath().ToString());
        return;
    }

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->SetCurrentWorldName(ResolvedFolderName);
        GameManager->ClearWorldSelectionMenuRequest();
        GameManager->SetGamePaused(false);
    }

    UE_LOG(LogTemp, Display,
        TEXT("[WorldTravel] Host selection. Folder=%s World=%s GameMode=<map World Settings> Source=%s"),
        *ResolvedFolderName,
        *SelectedHostWorld.ToSoftObjectPath().ToString(),
        *LaunchResolutionSource);

    PrepareMenuForWorldTravel();
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        if (!Multiplayer->HostMultiplayerWorld(this, ResolvedFolderName, SelectedHostWorld))
        {
            UE_LOG(LogTemp, Error, TEXT("[WorldTravel] Host travel request was rejected for '%s'."), *ResolvedFolderName);
            ShowMultiplayerMenu();
        }
    }
    else
    {
        UE_LOG(LogTemp, Error,
            TEXT("[WorldTravel] Cannot host because MultiplayerWorldSubSystem is unavailable; "
                 "direct OpenLevel fallback is disabled to keep GameMode selection deterministic."));
        ShowMultiplayerMenu();
    }
}

void AMainGameMode::OpenClientConnectionWorld(const FString& InServerAddress)
{
    if (IsWorldSelectionReturnBlocked())
    {
        return;
    }

    const UV3DSimulatorAssetRegistry* Registry =
        UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this);
    const TSoftObjectPtr<UWorld> ClientWorld = Registry
        ? Registry->ClientWorld : TSoftObjectPtr<UWorld>();
    if (ClientWorld.IsNull())
    {
        UE_LOG(LogTemp, Warning, TEXT("MainGameMode cannot open the client connection world because AssetRegistry.ClientWorld is not assigned."));
        return;
    }

    SetPendingServerAddress(InServerAddress);
    PrepareMenuForWorldTravel();
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        Multiplayer->SetServerAddress(PendingServerAddress);
        Multiplayer->OpenClientConnectionWorld(this, ClientWorld);
    }
}

void AMainGameMode::JoinMultiplayerServer(const FString& InServerAddress, const FString& OptionalWorldFolderName)
{
    if (IsWorldSelectionReturnBlocked())
    {
        return;
    }

    SetPendingServerAddress(InServerAddress);
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->ClearWorldSelectionMenuRequest();
        GameManager->SetGamePaused(false);
    }

    PrepareMenuForWorldTravel();
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        Multiplayer->JoinMultiplayerWorld(this, PendingServerAddress, OptionalWorldFolderName);
    }
}

void AMainGameMode::SetPendingServerAddress(const FString& InServerAddress)
{
    PendingServerAddress = InServerAddress;
    PendingServerAddress.TrimStartAndEndInline();
    if (PendingServerAddress.IsEmpty())
    {
        PendingServerAddress = DefaultServerAddress.IsEmpty() ? FString(TEXT("127.0.0.1:7777")) : DefaultServerAddress;
    }

    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        Multiplayer->SetServerAddress(PendingServerAddress);
    }
}

void AMainGameMode::HandleGameplayTravelWatchdogExpired()
{
    if (!bGameplayWorldTravelPending || IsActorBeingDestroyed())
    {
        return;
    }

    const FString FailedWorldFolder = PendingGameplayWorldFolderName;
    bGameplayWorldTravelPending = false;
    PendingGameplayWorldFolderName.Reset();
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        Multiplayer->ClearRequestedWorld();
    }

    UE_LOG(LogTemp, Error,
        TEXT("[WorldSelection] Travel to the assigned gameplay world did not complete. Restoring the world list. "
             "Confirm that the world reference is assigned and included in the packaged build. World=%s"),
        *FailedWorldFolder);

    ShowWorldSelectionMenu();
}

void AMainGameMode::StartWorldSelectionReturnInputGuard()
{
    bWorldSelectionReturnInputGuardActive = true;
    if (IsValid(WorldSelectionWidget))
    {
        WorldSelectionWidget->SetIsEnabled(false);
    }

    const float Delay = FMath::Clamp(WorldSelectionReturnInputGuardSeconds, 0.05f, 2.0f);
    GetWorldTimerManager().ClearTimer(WorldSelectionReturnInputGuardHandle);
    GetWorldTimerManager().SetTimer(
        WorldSelectionReturnInputGuardHandle,
        this,
        &AMainGameMode::TryReleaseWorldSelectionReturnInputGuard,
        Delay,
        false);

    UE_LOG(LogTemp, Display, TEXT("[WorldSelection] Return input guard armed for %.2f seconds."), Delay);
}

bool AMainGameMode::IsWorldSelectionActivationInputHeld() const
{
    APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
    if (!PlayerController)
    {
        return false;
    }

    return PlayerController->IsInputKeyDown(EKeys::LeftMouseButton)
        || PlayerController->IsInputKeyDown(EKeys::RightMouseButton)
        || PlayerController->IsInputKeyDown(EKeys::Enter)
        || PlayerController->IsInputKeyDown(EKeys::SpaceBar)
        || PlayerController->IsInputKeyDown(EKeys::Gamepad_FaceButton_Bottom);
}

bool AMainGameMode::IsWorldSelectionReturnBlocked() const
{
    if (bWorldSelectionReturnInputGuardActive)
    {
        return true;
    }

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        return GameManager->ShouldOpenWorldSelectionMenuOnNextMainWorld();
    }

    return false;
}

void AMainGameMode::TryReleaseWorldSelectionReturnInputGuard()
{
    if (!bWorldSelectionReturnInputGuardActive || IsActorBeingDestroyed())
    {
        return;
    }

    if (IsWorldSelectionActivationInputHeld())
    {
        // Do not expose the world buttons until the exact input that initiated Exit has been released.
        GetWorldTimerManager().SetTimer(
            WorldSelectionReturnInputGuardHandle,
            this,
            &AMainGameMode::TryReleaseWorldSelectionReturnInputGuard,
            0.05f,
            false);
        return;
    }

    bWorldSelectionReturnInputGuardActive = false;
    GetWorldTimerManager().ClearTimer(WorldSelectionReturnInputGuardHandle);
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        // The arrival transaction is complete only now. World travel may be initiated again by a
        // deliberate button click after this point.
        GameManager->ClearWorldSelectionMenuRequest();
    }
    if (IsValid(WorldSelectionWidget))
    {
        WorldSelectionWidget->SetIsEnabled(true);
    }

    UE_LOG(LogTemp, Display, TEXT("[WorldSelection] Return input guard released; world selection is ready."));
}

void AMainGameMode::CancelWorldSelectionReturnInputGuard()
{
    bWorldSelectionReturnInputGuardActive = false;
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(WorldSelectionReturnInputGuardHandle);
    }
    if (IsValid(WorldSelectionWidget))
    {
        WorldSelectionWidget->SetIsEnabled(true);
    }
}

void AMainGameMode::PrepareMenuForWorldTravel()
{
    CancelWorldSelectionReturnInputGuard();
    HideAllMenuWidgets();
}

void AMainGameMode::HideAllMenuWidgets()
{
    if (IsValid(StartMenuWidget)) StartMenuWidget->SetVisibility(ESlateVisibility::Collapsed);
    if (IsValid(WorldSelectionWidget)) WorldSelectionWidget->SetVisibility(ESlateVisibility::Collapsed);
    if (IsValid(MultiplayerMenuWidget)) MultiplayerMenuWidget->SetVisibility(ESlateVisibility::Collapsed);
    if (IsValid(SettingsWidget)) SettingsWidget->SetVisibility(ESlateVisibility::Collapsed);
}

void AMainGameMode::ApplyMenuInputMode(UUserWidget* FocusWidget) const
{
    if (!bApplyMenuInputMode)
    {
        return;
    }

    APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
    if (!PlayerController)
    {
        return;
    }

    PlayerController->bShowMouseCursor = true;

    FInputModeUIOnly InputMode;
    InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
    // Supplying a non-focusable SObjectWidget makes PlayerController emit an error every time a
    // menu opens. Mouse-only menu roots do not need explicit keyboard focus.
    if (IsValid(FocusWidget) && FocusWidget->IsFocusable())
    {
        InputMode.SetWidgetToFocus(FocusWidget->TakeWidget());
    }
    PlayerController->SetInputMode(InputMode);
}

void AMainGameMode::BuildLevelFolderNameMap()
{
    FolderNameMap.Empty();
    TArray<FString> WorldFiles;
    IFileManager::Get().FindFiles(WorldFiles,
        *FPaths::Combine(V3DSimulatorPaths::WorldsRoot(), TEXT("*.v3d")), true, false);
    WorldFiles.Sort([](const FString& A, const FString& B)
    {
        return A.Compare(B, ESearchCase::IgnoreCase) < 0;
    });

    for (const FString& FileName : WorldFiles)
    {
        const FString WorldKey = FPaths::GetBaseFilename(FileName);
        FString SafeKey;
        if (!UGameManagerSubSystem::TryNormalizeWorldFolderName(WorldKey, SafeKey, false)
            || SafeKey != WorldKey) continue;

        FString Error;
        const FString ArchivePath = FPaths::Combine(V3DSimulatorPaths::WorldsRoot(), FileName);
        const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Reader =
            FGWorldArchiveReader::Open(ArchivePath, Error);
        FString ConfigText;
        if (!Reader.IsValid() || !Reader->ReadWorldConfig(ConfigText, Error))
        {
            UE_LOG(LogTemp, Error,
                TEXT("[WorldSelection] Ignoring invalid .v3d '%s': %s"), *ArchivePath, *Error);
            continue;
        }
        const FSafeJsonLoadResult Config = FSafeFileIO::ParseJsonText(
            ConfigText, ArchivePath + TEXT("#config.json"));
        FV3DSimulatorProjectConfig ProjectConfig;
        FString ProjectConfigError;
        if (!Config.IsSuccess() || !Config.JsonObject.IsValid()
            || !V3DSimulatorProjectConfig::Parse(
                Config.JsonObject, SafeKey, ProjectConfig, ProjectConfigError)
            || ProjectConfig.ProjectType != EV3DSimulatorProjectType::World)
        {
            UE_LOG(LogTemp, Error,
                TEXT("[WorldSelection] Ignoring .v3d without a canonical World project config: %s (%s)"),
                *ArchivePath, *ProjectConfigError);
            continue;
        }
        FString DisplayName = ProjectConfig.WorldName.IsEmpty()
            ? ProjectConfig.Name : ProjectConfig.WorldName;
        DisplayName.TrimStartAndEndInline();
        if (DisplayName.IsEmpty()) continue;
        FolderNameMap.Add(SafeKey, MoveTemp(DisplayName));
    }
}

