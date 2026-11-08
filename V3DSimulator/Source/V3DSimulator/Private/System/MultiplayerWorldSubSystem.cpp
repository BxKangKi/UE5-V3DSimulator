// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file MultiplayerWorldSubSystem.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/MultiplayerWorldSubSystem.h"

#include "Engine/GameInstance.h"
#include "Engine/Engine.h"
#include "Engine/PendingNetGame.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "GameMode/MainGameMode.h"
#include "HAL/PlatformTime.h"
#include "System/V3DSimulatorGameInstance.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "UI/LoadingHUDWidget.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "System/GameManagerSubSystem.h"
#include "System/SimulatorFileServices.h"

namespace
{
    bool IsReturningToWorldSelection(const UObject* WorldContextObject)
    {
        if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(WorldContextObject))
        {
            if (Manager->ShouldOpenWorldSelectionMenuOnNextMainWorld())
            {
                UE_LOG(LogTemp, Display,
                    TEXT("[WorldSelection] Ignored stale gameplay-world travel while returning to world selection."));
                return true;
            }
        }
        return false;
    }

    /** Adds one URL option without carrying options from the previous map. */
    void AppendTravelOption(FString& InOutOptions, const FString& Key, const FString& Value)
    {
        if (Key.IsEmpty() || Value.IsEmpty())
        {
            return;
        }

        if (!InOutOptions.IsEmpty())
        {
            InOutOptions += TEXT("?");
        }
        InOutOptions += Key;
        InOutOptions += TEXT("=");
        InOutOptions += Value;
    }

    /**
     * Normalizes an optional world key before persistent state or a travel URL can observe it.
     * Empty remains meaningful for client connections where the server supplies the world option.
     */
    bool NormalizeOptionalWorldFolder(
        const FString& Candidate,
        FString& OutNormalized)
    {
        if (Candidate.TrimStartAndEnd().IsEmpty())
        {
            OutNormalized.Reset();
            return true;
        }

        if (UGameManagerSubSystem::TryNormalizeWorldFolderName(
                Candidate, OutNormalized, false))
        {
            return true;
        }

        UE_LOG(LogTemp, Error,
            TEXT("Rejected invalid multiplayer world folder key: %s"),
            *Candidate.Left(256));
        return false;
    }
}

void UMultiplayerWorldSubSystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    if (GEngine)
    {
        NetworkFailureHandle = GEngine->OnNetworkFailure().AddUObject(this, &UMultiplayerWorldSubSystem::HandleNetworkFailure);
        TravelFailureHandle = GEngine->OnTravelFailure().AddUObject(this, &UMultiplayerWorldSubSystem::HandleTravelFailure);
    }
    PreLoadMapHandle = FCoreUObjectDelegates::PreLoadMapWithContext.AddUObject(this, &UMultiplayerWorldSubSystem::HandlePreLoadMap);
    PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UMultiplayerWorldSubSystem::HandlePostLoadMap);
}

void UMultiplayerWorldSubSystem::Deinitialize()
{
    StopConnectionTicker();
    bConnectionPending = false;
    bConnectionFailed = false;
    RemoveConnectionWidget();
    if (GEngine)
    {
        GEngine->OnNetworkFailure().Remove(NetworkFailureHandle);
        GEngine->OnTravelFailure().Remove(TravelFailureHandle);
    }
    FCoreUObjectDelegates::PreLoadMapWithContext.Remove(PreLoadMapHandle);
    FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
    NetworkFailureHandle.Reset();
    TravelFailureHandle.Reset();
    PreLoadMapHandle.Reset();
    PostLoadMapHandle.Reset();
    Super::Deinitialize();
}

void UMultiplayerWorldSubSystem::StopConnectionTicker()
{
    FTSTicker::GetCoreTicker().RemoveTicker(ConnectionTickerHandle);
    ConnectionTickerHandle.Reset();
}

void UMultiplayerWorldSubSystem::RemoveConnectionWidget()
{
    if (IsValid(ConnectionWidget)) ConnectionWidget->RemoveFromParent();
    ConnectionWidget = nullptr;
}

void UMultiplayerWorldSubSystem::ShowConnectionWidget()
{
    APlayerController* Controller = UGameplayStatics::GetPlayerController(this, 0);
    if (bConnectionMapLoading || !IsValid(Controller) || !Controller->IsLocalController()) return;
    if (!IsValid(ConnectionWidget) || ConnectionWidget->GetWorld() != GetWorld())
    {
        RemoveConnectionWidget();
        // Always available, including maps whose WBP loading screen still has legacy graph logic.
        ConnectionWidget = CreateWidget<ULoadingHUDWidget>(Controller, ULoadingHUDWidget::StaticClass());
    }
    if (!IsValid(ConnectionWidget)) return;
    if (!ConnectionWidget->IsInViewport()) ConnectionWidget->AddToPlayerScreen(200);
    ConnectionWidget->RefreshLoadingStatus();
    UWidgetBlueprintLibrary::SetInputMode_UIOnlyEx(Controller, ConnectionWidget, EMouseLockMode::DoNotLock, true);
    Controller->bShowMouseCursor = true;
    Controller->bEnableClickEvents = true;
    Controller->bEnableMouseOverEvents = true;
}

FText UMultiplayerWorldSubSystem::GetConnectionStatusText() const
{
    if (bConnectionFailed) return ConnectionFailureText;
    return FText::Format(NSLOCTEXT("V3DMultiplayer", "Connecting", "Connecting to {0}..."), FText::FromString(ServerAddress));
}

void UMultiplayerWorldSubSystem::CancelPendingConnectionTravel(UWorld* World)
{
    if (!GEngine || !IsValid(World)) return;
    if (FWorldContext* Context = GEngine->GetWorldContextFromWorld(World))
    {
        // ClientTravel is queued before the PendingNetGame exists. Clear both stages so clicking
        // Back on the same frame as Join cannot start a connection on the following engine tick.
        Context->TravelURL.Reset();
        // The FWorldContext overload is engine-internal; use the public world overload.
        GEngine->CancelPending(World, nullptr);
    }
}

void UMultiplayerWorldSubSystem::FailConnection(const FText& Message)
{
    if (!bConnectionPending) return;
    bConnectionPending = false;
    bConnectionFailed = true;
    bConnectionMapLoading = false;
    ConnectionFailureText = Message;
    CancelPendingConnectionTravel(GetWorld());
    ClearRequestedWorld();
    SelectedWorldFolderName.Reset();
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this)) Manager->SetCurrentWorldName(FString());
    ShowConnectionWidget();
}

bool UMultiplayerWorldSubSystem::TickConnection(float)
{
    if (!CanCancelConnection())
    {
        ConnectionTickerHandle.Reset();
        return false;
    }
    if (bConnectionPending && FPlatformTime::Seconds() - ConnectionStartSeconds >= 30.0)
    {
        FailConnection(NSLOCTEXT("V3DMultiplayer", "ConnectionTimeout", "Server did not respond. Select Back to return."));
    }
    if (bConnectionMapLoading) return true;
    // Recreate the overlay if Unreal opened its default failure map. Do not retain the old world.
    if (!IsValid(ConnectionWidget) || ConnectionWidget->GetWorld() != GetWorld() || !ConnectionWidget->IsInViewport())
        ShowConnectionWidget();
    if (IsValid(ConnectionWidget)) ConnectionWidget->RefreshLoadingStatus();
    return true;
}

void UMultiplayerWorldSubSystem::HandlePreLoadMap(const FWorldContext& Context, const FString&)
{
    if (Context.OwningGameInstance != GetGameInstance() || !CanCancelConnection()) return;
    bConnectionMapLoading = true;
    // Drop the owning player/widget graph before LoadMap performs old-world garbage collection.
    RemoveConnectionWidget();
}

void UMultiplayerWorldSubSystem::HandlePostLoadMap(UWorld* LoadedWorld)
{
    if (!IsValid(LoadedWorld) || LoadedWorld->GetGameInstance() != GetGameInstance()) return;
    bConnectionMapLoading = false;
    RemoveConnectionWidget();
    if (bConnectionPending && LoadedWorld->GetNetMode() == NM_Client)
    {
        bConnectionPending = false;
        bConnectionFailed = false;
        StopConnectionTicker();
    }
    else if (CanCancelConnection()) ShowConnectionWidget();
}

void UMultiplayerWorldSubSystem::HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type, const FString& Error)
{
    if (!bConnectionPending) return;
    const FWorldContext* Context = GEngine ? GEngine->GetWorldContextFromWorld(GetWorld()) : nullptr;
    const bool bOwnPendingDriver = Context && Context->PendingNetGame && NetDriver
        && Context->PendingNetGame->NetDriver == NetDriver;
    if ((!IsValid(World) || World->GetGameInstance() != GetGameInstance()) && !bOwnPendingDriver) return;
    UE_LOG(LogTemp, Warning, TEXT("[Multiplayer] Connection failed: %s"), *Error.Left(1024));
    FailConnection(NSLOCTEXT("V3DMultiplayer", "ConnectionFailed", "Could not connect to the server. Select Back to return."));
}

void UMultiplayerWorldSubSystem::HandleTravelFailure(UWorld* World, ETravelFailure::Type, const FString& Error)
{
    if (!bConnectionPending) return;
    if (IsValid(World))
    {
        if (World->GetGameInstance() != GetGameInstance()) return;
    }
    else
    {
        // LoadMap may report failure after tearing down the old world. With no world/driver in
        // this delegate, accept only an unambiguous game context; multi-PIE falls back to timeout.
        const FWorldContext* OnlyGameContext = nullptr;
        if (!GEngine) return;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (!Context.OwningGameInstance) continue;
            if (OnlyGameContext) return;
            OnlyGameContext = &Context;
        }
        if (!OnlyGameContext || OnlyGameContext->OwningGameInstance != GetGameInstance()) return;
    }
    bConnectionMapLoading = false;
    UE_LOG(LogTemp, Warning, TEXT("[Multiplayer] Connection travel failed: %s"), *Error.Left(1024));
    FailConnection(NSLOCTEXT("V3DMultiplayer", "ConnectionTravelFailed", "The server world could not be opened. Select Back to return."));
}

bool UMultiplayerWorldSubSystem::CancelClientConnection(const UObject* WorldContextObject)
{
    check(IsInGameThread());
    if (!CanCancelConnection() || !IsValid(WorldContextObject) || WorldContextObject->GetWorld() != GetWorld()) return false;
    UWorld* World = GetWorld();
    const UV3DSimulatorAssetRegistry* Registry = UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this);
    AMainGameMode* Menu = IsValid(World) ? Cast<AMainGameMode>(World->GetAuthGameMode()) : nullptr;
    if (!IsValid(Menu) && (!IsValid(Registry) || Registry->MainWorld.IsNull())) return false;

    // Invalidate state before cancellation: engine failure delegates can fire synchronously.
    bConnectionPending = false;
    bConnectionFailed = false;
    StopConnectionTicker();
    CancelPendingConnectionTravel(World);
    ClearRequestedWorld();
    SelectedWorldFolderName.Reset();
    WorldMode = EMultiplayerWorldMode::SinglePlayer;
    ConnectionFailureText = FText::GetEmpty();
    RemoveConnectionWidget();
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        Manager->SetCurrentWorldName(FString());
        Manager->SetGamePaused(false);
        Manager->SetWorldLoading(false);
        // Unregister through the public owner API, which also removes the widget and its pins.
        Manager->SetLoadingWidget(nullptr);
    }
    if (IsValid(Menu))
    {
        Menu->ShowMultiplayerMenu();
        return true;
    }
    return UGameManagerSubSystem::TryOpenWorldSelectionScreen(this, Registry->MainWorld);
}

UMultiplayerWorldSubSystem* UMultiplayerWorldSubSystem::Get(const UObject* WorldContextObject)
{
    if (!IsValid(WorldContextObject))
    {
        return nullptr;
    }

    if (const UGameInstance* GameInstance = Cast<UGameInstance>(WorldContextObject))
    {
        return const_cast<UGameInstance*>(GameInstance)->GetSubsystem<UMultiplayerWorldSubSystem>();
    }

    if (const UGameInstanceSubsystem* GameInstanceSubsystem = Cast<UGameInstanceSubsystem>(WorldContextObject))
    {
        if (UGameInstance* GameInstance = GameInstanceSubsystem->GetGameInstance())
        {
            return GameInstance->GetSubsystem<UMultiplayerWorldSubSystem>();
        }
    }

    UWorld* World = WorldContextObject->GetWorld();
    if (!World)
    {
        return nullptr;
    }

    UGameInstance* GameInstance = World->GetGameInstance();
    return GameInstance ? GameInstance->GetSubsystem<UMultiplayerWorldSubSystem>() : nullptr;
}

void UMultiplayerWorldSubSystem::ClearRequestedWorld()
{
    check(IsInGameThread());

    // The requested map descriptor is only a transient travel-validation token. Clear both fields
    // together so a failed/cancelled travel cannot leave a stale asset paired with a newer folder
    // (or vice versa) for GameManagerSubSystem to validate on the next map.
    RequestedWorldAsset.Reset();
    RequestedWorldFolder.Reset();
}

void UMultiplayerWorldSubSystem::SetSelectedWorldFolderName(
    const FString& InWorldFolderName)
{
    check(IsInGameThread());
    FString Normalized;
    if (!NormalizeOptionalWorldFolder(InWorldFolderName, Normalized))
    {
        // Clear instead of retaining a previous world: stale selection is more dangerous than an
        // explicit startup failure when this method receives untrusted replicated/Blueprint data.
        SelectedWorldFolderName.Reset();
        return;
    }
    SelectedWorldFolderName = MoveTemp(Normalized);
}


bool UMultiplayerWorldSubSystem::OpenWorldByReference(
    const UObject* WorldContextObject,
    TSoftObjectPtr<UWorld> WorldAsset,
    const FString& Options) const
{
    const FScopedSimulatorLoadCheckpoint Checkpoint(TEXT("World.TravelRequest: ") + WorldAsset.ToSoftObjectPath().ToString());
    if (!IsValid(WorldContextObject) || WorldAsset.IsNull())
    {
        return false;
    }

    if (IsReturningToWorldSelection(WorldContextObject))
    {
        return false;
    }

    const FString FinalOptions = Options;
    const FString WorldPath = WorldAsset.ToSoftObjectPath().ToString();

    UE_LOG(LogTemp, Display,
        TEXT("[WorldTravel] Open world. World=%s GameMode=<map World Settings> Options=%s"),
        WorldPath.IsEmpty() ? TEXT("<invalid>") : *WorldPath,
        FinalOptions.IsEmpty() ? TEXT("<none>") : *FinalOptions);

    // Absolute travel deliberately drops the previous map's URL options, especially stale ?game=
    // values. The destination map's World Settings is the only GameMode authority.
    UGameplayStatics::OpenLevelBySoftObjectPtr(WorldContextObject, WorldAsset, true, FinalOptions);
    return true;
}

bool UMultiplayerWorldSubSystem::StartSinglePlayerWorld(
    const UObject* WorldContextObject,
    const FString& WorldFolderName,
    TSoftObjectPtr<UWorld> SinglePlayerWorld)
{
    if (IsReturningToWorldSelection(WorldContextObject)) return false;
    FString NormalizedWorldFolderName;
    if (!NormalizeOptionalWorldFolder(WorldFolderName, NormalizedWorldFolderName)) return false;
    WorldMode = EMultiplayerWorldMode::SinglePlayer;
    SelectedWorldFolderName = NormalizedWorldFolderName;
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(WorldContextObject)) Manager->SetCurrentWorldName(NormalizedWorldFolderName);
    RequestedWorldAsset = SinglePlayerWorld;
    RequestedWorldFolder = NormalizedWorldFolderName;
    FString Options;
    if (!NormalizedWorldFolderName.IsEmpty()) AppendTravelOption(Options, TEXT("World"), NormalizedWorldFolderName);
    if (!OpenWorldByReference(WorldContextObject, SinglePlayerWorld, Options)) { ClearRequestedWorld(); return false; }
    return true;
}

bool UMultiplayerWorldSubSystem::HostMultiplayerWorld(
    const UObject* WorldContextObject,
    const FString& WorldFolderName,
    TSoftObjectPtr<UWorld> HostWorld,
    int32 Port)
{
    if (IsReturningToWorldSelection(WorldContextObject)) return false;
    FString NormalizedWorldFolderName;
    if (!NormalizeOptionalWorldFolder(WorldFolderName, NormalizedWorldFolderName)) return false;
    WorldMode = EMultiplayerWorldMode::Host;
    SelectedWorldFolderName = NormalizedWorldFolderName;
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(WorldContextObject)) Manager->SetCurrentWorldName(NormalizedWorldFolderName);
    RequestedWorldAsset = HostWorld;
    RequestedWorldFolder = NormalizedWorldFolderName;
    FString Options(TEXT("listen"));
    if (!NormalizedWorldFolderName.IsEmpty()) AppendTravelOption(Options, TEXT("World"), NormalizedWorldFolderName);
    if (Port > 0) AppendTravelOption(Options, TEXT("Port"), FString::FromInt(Port));
    if (!OpenWorldByReference(WorldContextObject, HostWorld, Options)) { ClearRequestedWorld(); return false; }
    return true;
}

bool UMultiplayerWorldSubSystem::OpenClientConnectionWorld(
    const UObject* WorldContextObject,
    TSoftObjectPtr<UWorld> ClientWorld)
{
    if (IsReturningToWorldSelection(WorldContextObject))
    {
        return false;
    }

    WorldMode = EMultiplayerWorldMode::Client;
    // The connection map is UI, not an external data world. Clear any prior selection so the
    // automatic destination bootstrap cannot mistake this intermediate map for gameplay.
    SelectedWorldFolderName.Reset();
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(WorldContextObject))
    {
        Manager->SetCurrentWorldName(FString());
    }
    return OpenWorldByReference(
        WorldContextObject,
        ClientWorld,
        FString());
}

bool UMultiplayerWorldSubSystem::JoinMultiplayerWorld(
    const UObject* WorldContextObject,
    const FString& InServerAddress,
    const FString& WorldFolderName)
{
    if (!IsValid(WorldContextObject) || IsReturningToWorldSelection(WorldContextObject))
    {
        return false;
    }

    FString NormalizedWorldFolderName;
    if (!NormalizeOptionalWorldFolder(WorldFolderName, NormalizedWorldFolderName))
    {
        return false;
    }

    // Reject duplicate WBP/native clicks while one attempt owns the pending net game.
    if (CanCancelConnection()) return false;
    UWorld* World = WorldContextObject->GetWorld();
    APlayerController* PlayerController = World ? UGameplayStatics::GetPlayerController(WorldContextObject, 0) : nullptr;
    if (!IsValid(PlayerController) || !PlayerController->IsLocalController()) return false;

    WorldMode = EMultiplayerWorldMode::Client;
    ClearRequestedWorld();
    SelectedWorldFolderName = NormalizedWorldFolderName;
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(WorldContextObject))
        Manager->SetCurrentWorldName(NormalizedWorldFolderName);

    ServerAddress = InServerAddress.TrimStartAndEnd().IsEmpty() ? ServerAddress : InServerAddress.TrimStartAndEnd();
    if (ServerAddress.IsEmpty()) ServerAddress = TEXT("127.0.0.1:7777");
    bConnectionMapLoading = false;
    bConnectionPending = true;
    bConnectionFailed = false;
    ConnectionFailureText = FText::GetEmpty();
    ConnectionStartSeconds = FPlatformTime::Seconds();
    ShowConnectionWidget();
    StopConnectionTicker();
    ConnectionTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UMultiplayerWorldSubSystem::TickConnection), 0.1f);

    FString TravelAddress = ServerAddress;
    if (!SelectedWorldFolderName.IsEmpty() && !TravelAddress.Contains(TEXT("?World=")))
    {
        // The optional hint is attached to direct ClientTravel for servers that route several
        // external data worlds through the same gameplay map.
        TravelAddress += FString::Printf(TEXT("?World=%s"), *SelectedWorldFolderName);
    }

    PlayerController->ClientTravel(TravelAddress, TRAVEL_Absolute);
    return true;
}

bool UMultiplayerWorldSubSystem::ShouldRunServerAuthority(const UObject* WorldContextObject) const
{
    const UWorld* World = IsValid(WorldContextObject) ? WorldContextObject->GetWorld() : nullptr;
    if (!World)
    {
        return WorldMode != EMultiplayerWorldMode::Client;
    }

    return World->GetNetMode() != NM_Client;
}

bool UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(const UObject* WorldContextObject)
{
    UMultiplayerWorldSubSystem* Subsystem = Get(WorldContextObject);
    const UWorld* World = IsValid(WorldContextObject) ? WorldContextObject->GetWorld() : nullptr;
    const bool bIsNetworkClient = World && World->GetNetMode() == NM_Client;
    return bIsNetworkClient || (Subsystem && Subsystem->WorldMode == EMultiplayerWorldMode::Client);
}
