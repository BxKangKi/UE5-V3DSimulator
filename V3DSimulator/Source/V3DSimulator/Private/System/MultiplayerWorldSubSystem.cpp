// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file MultiplayerWorldSubSystem.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/MultiplayerWorldSubSystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "System/GameManagerSubSystem.h"

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

    // A client does not select the authoritative GameMode. The server's active map/travel URL does.
    WorldMode = EMultiplayerWorldMode::Client;
    ClearRequestedWorld();
    SelectedWorldFolderName = NormalizedWorldFolderName;
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(WorldContextObject))
    {
        Manager->SetCurrentWorldName(NormalizedWorldFolderName);
    }

    ServerAddress = InServerAddress.IsEmpty() ? ServerAddress : InServerAddress;
    if (ServerAddress.IsEmpty())
    {
        ServerAddress = TEXT("127.0.0.1:7777");
    }

    UWorld* World = WorldContextObject->GetWorld();
    APlayerController* PlayerController = World ? UGameplayStatics::GetPlayerController(WorldContextObject, 0) : nullptr;
    if (!PlayerController)
    {
        return false;
    }

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
