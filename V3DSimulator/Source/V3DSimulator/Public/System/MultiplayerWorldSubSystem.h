// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file MultiplayerWorldSubSystem.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "MultiplayerWorldSubSystem.generated.h"

class UWorld;
class UNetDriver;
class ULoadingHUDWidget;

UENUM(BlueprintType)
enum class EMultiplayerWorldMode : uint8
{
    SinglePlayer UMETA(DisplayName="Single Player"),
    Host UMETA(DisplayName="Host"),
    Client UMETA(DisplayName="Client")
};

/**
 * Stores the current play mode and centralizes travel to directly assigned world assets.
 * The subsystem lives for the GameInstance lifetime, so the selected world folder and server address
 * survive the MainWorld -> gameplay-world travel.
 */
UCLASS(BlueprintType)
class V3DSIMULATOR_API UMultiplayerWorldSubSystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    static UMultiplayerWorldSubSystem* Get(const UObject* WorldContextObject);
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintPure, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    static UMultiplayerWorldSubSystem* Find(const UObject* WorldContextObject) { return Get(WorldContextObject); }

    /** Stores only a normalized direct-child world key; empty input clears the current selection. */
    UFUNCTION(BlueprintCallable, Category="Multiplayer")
    void SetSelectedWorldFolderName(const FString& InWorldFolderName);

    UFUNCTION(BlueprintPure, Category="Multiplayer")
    FString GetSelectedWorldFolderName() const { return SelectedWorldFolderName; }

    /** Pending local map descriptor used only to validate that travel opened the requested map. */
    TSoftObjectPtr<UWorld> GetRequestedWorldAsset() const { return RequestedWorldAsset; }
    FString GetRequestedWorldFolder() const { return RequestedWorldFolder; }
    void ClearRequestedWorld();

    UFUNCTION(BlueprintCallable, Category="Multiplayer")
    void SetServerAddress(const FString& InServerAddress) { ServerAddress = InServerAddress; }

    UFUNCTION(BlueprintPure, Category="Multiplayer")
    FString GetServerAddress() const { return ServerAddress; }

    UFUNCTION(BlueprintCallable, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    bool StartSinglePlayerWorld(const UObject* WorldContextObject, const FString& WorldFolderName, TSoftObjectPtr<UWorld> SinglePlayerWorld);

    UFUNCTION(BlueprintCallable, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    bool HostMultiplayerWorld(const UObject* WorldContextObject, const FString& WorldFolderName, TSoftObjectPtr<UWorld> HostWorld, int32 Port = 7777);

    UFUNCTION(BlueprintCallable, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    bool OpenClientConnectionWorld(const UObject* WorldContextObject, TSoftObjectPtr<UWorld> ClientWorld);

    UFUNCTION(BlueprintCallable, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    bool JoinMultiplayerWorld(const UObject* WorldContextObject, const FString& InServerAddress, const FString& WorldFolderName);

    /** True while connecting or after a failed attempt; Back remains available in both states. */
    UFUNCTION(BlueprintPure, Category="Multiplayer")
    bool CanCancelConnection() const { return bConnectionPending || bConnectionFailed; }

    UFUNCTION(BlueprintPure, Category="Multiplayer")
    bool IsConnectionPending() const { return bConnectionPending; }

    UFUNCTION(BlueprintPure, Category="Multiplayer")
    FText GetConnectionStatusText() const;

    /** Cancels queued travel and the pending network handshake before restoring the menu. */
    UFUNCTION(BlueprintCallable, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    bool CancelClientConnection(const UObject* WorldContextObject);

    UFUNCTION(BlueprintPure, Category="Multiplayer")
    EMultiplayerWorldMode GetWorldMode() const { return WorldMode; }

    UFUNCTION(BlueprintPure, Category="Multiplayer")
    bool IsMultiplayer() const { return WorldMode != EMultiplayerWorldMode::SinglePlayer; }

    UFUNCTION(BlueprintPure, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    bool ShouldRunServerAuthority(const UObject* WorldContextObject) const;

    UFUNCTION(BlueprintPure, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    static bool ShouldUseClientRenderOnlyStreaming(const UObject* WorldContextObject);

private:
    void ShowConnectionWidget();
    void RemoveConnectionWidget();
    void StopConnectionTicker();
    void CancelPendingConnectionTravel(UWorld* World);
    void FailConnection(const FText& Message);
    bool TickConnection(float DeltaSeconds);
    void HandlePreLoadMap(const FWorldContext& Context, const FString& MapName);
    void HandlePostLoadMap(UWorld* LoadedWorld);
    void HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& Error);
    void HandleTravelFailure(UWorld* World, ETravelFailure::Type FailureType, const FString& Error);

    UPROPERTY(Transient)
    TObjectPtr<ULoadingHUDWidget> ConnectionWidget;

    bool bConnectionMapLoading = false;
    bool bConnectionPending = false;
    bool bConnectionFailed = false;
    double ConnectionStartSeconds = 0.0;
    FText ConnectionFailureText;
    FDelegateHandle NetworkFailureHandle;
    FDelegateHandle TravelFailureHandle;
    FDelegateHandle PreLoadMapHandle;
    FDelegateHandle PostLoadMapHandle;
    FTSTicker::FDelegateHandle ConnectionTickerHandle;

    EMultiplayerWorldMode WorldMode = EMultiplayerWorldMode::SinglePlayer;
    FString SelectedWorldFolderName;
    FString ServerAddress = TEXT("127.0.0.1:7777");

    // Map-only launch descriptor. GameMode is deliberately not stored here:
    // the destination map's World Settings owns GameMode selection.
    UPROPERTY(Transient)
    TSoftObjectPtr<UWorld> RequestedWorldAsset;

    UPROPERTY(Transient)
    FString RequestedWorldFolder;

    bool OpenWorldByReference(
        const UObject* WorldContextObject,
        TSoftObjectPtr<UWorld> WorldAsset,
        const FString& Options) const;
};
