// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file SimulatorCommandSubsystem.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/SimulatorCommandSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/PlatformTime.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/Parse.h"
#include "System/GameManagerSubSystem.h"
#include "System/WorldObjectStreamingSubsystem.h"
#include "Model/WorldSceneStreamingSubsystem.h"
#include "TimerManager.h"
#include "Weather/WeatherSubsystem.h"
#include "World/WorldData.h"

namespace
{
    bool IsCommandName(const FString& Value, const TCHAR* Expected)
    {
        return Value.Equals(Expected, ESearchCase::IgnoreCase);
    }
}

void USimulatorCommandSubsystem::Deinitialize()
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(PendingTeleportTimer);
    }
    ClearTeleportStreamingFocus();
    PendingTeleport = FPendingTeleportRequest();
    Super::Deinitialize();
}

bool USimulatorCommandSubsystem::ExecuteCommand(
    APlayerController* RequestingController,
    const FString& CommandLine,
    FString& OutMessage)
{
    OutMessage.Reset();

    if (!IsValid(RequestingController) || RequestingController->GetGameInstance() != GetGameInstance())
    {
        OutMessage = TEXT("Command rejected: invalid player controller.");
        return false;
    }

    TArray<FString> Tokens;
    Tokenize(CommandLine, Tokens);
    if (Tokens.IsEmpty())
    {
        return false;
    }

    const FString Command = Tokens[0];
    Tokens.RemoveAt(0, 1, EAllowShrinking::No);

    if (IsCommandName(Command, TEXT("weather")))
    {
        ExecuteWeather(RequestingController, Tokens, OutMessage);
        return true;
    }
    if (IsCommandName(Command, TEXT("time")))
    {
        ExecuteTime(RequestingController, Tokens, OutMessage);
        return true;
    }
    if (IsCommandName(Command, TEXT("tp")))
    {
        ExecuteTeleport(RequestingController, Tokens, OutMessage);
        return true;
    }

    return false;
}

bool USimulatorCommandSubsystem::IsSimulatorCommand(const TCHAR* CommandLine)
{
    if (!CommandLine)
    {
        return false;
    }

    const TCHAR* Cursor = CommandLine;
    const FString Name = FParse::Token(Cursor, false);
    return IsCommandName(Name, TEXT("weather")) ||
        IsCommandName(Name, TEXT("time")) ||
        IsCommandName(Name, TEXT("tp"));
}

bool USimulatorCommandSubsystem::ExecuteWeather(
    APlayerController* Controller,
    const TArray<FString>& Args,
    FString& OutMessage)
{
    if (Args.Num() < 1 || Args.Num() > 3)
    {
        OutMessage = TEXT("Usage: weather <clear|rain|snow> [timer <seconds>|timer=<seconds>|seconds]");
        return false;
    }

    const FString Preset = Args[0].ToLower();
    if (Preset != TEXT("clear") && Preset != TEXT("rain") && Preset != TEXT("snow"))
    {
        OutMessage = TEXT("Weather must be clear, rain, or snow.");
        return false;
    }

    // Keep the short positional form for convenience, while also accepting the explicit
    // `timer` form requested by level/chat command UX. Both reach the same subsystem API.
    double Duration = -1.0;
    if (Args.Num() >= 2)
    {
        FString DurationToken;
        if (Args[1].Equals(TEXT("timer"), ESearchCase::IgnoreCase))
        {
            if (Args.Num() != 3)
            {
                OutMessage = TEXT("Usage: weather <clear|rain|snow> timer <seconds>");
                return false;
            }
            DurationToken = Args[2];
        }
        else if (Args[1].StartsWith(TEXT("timer="), ESearchCase::IgnoreCase))
        {
            if (Args.Num() != 2)
            {
                OutMessage = TEXT("Use either timer=<seconds> or timer <seconds>, not both.");
                return false;
            }
            DurationToken = Args[1].RightChop(6);
        }
        else
        {
            if (Args.Num() != 2)
            {
                OutMessage = TEXT("Usage: weather <clear|rain|snow> [timer <seconds>|timer=<seconds>|seconds]");
                return false;
            }
            DurationToken = Args[1];
        }

        if (!ParseFiniteDouble(DurationToken, Duration) || Duration < 0.0)
        {
            OutMessage = TEXT("Weather duration must be a finite number >= 0 seconds.");
            return false;
        }
    }

    UGameInstance* GameInstance = Controller->GetGameInstance();
    UWeatherSubsystem* Weather = IsValid(GameInstance)
        ? GameInstance->GetSubsystem<UWeatherSubsystem>()
        : nullptr;
    if (!IsValid(Weather))
    {
        OutMessage = TEXT("Weather subsystem is unavailable.");
        return false;
    }

    // clear is still an enabled simulated state: the effect actor is unloaded immediately and the
    // optional duration controls when auto-cycle chooses the next state.
    if (!Weather->ApplyWeather(Preset, 1.0f, true, static_cast<float>(Duration)))
    {
        OutMessage = TEXT("Weather command failed.");
        return false;
    }

    OutMessage = Duration >= 0.0
        ? FString::Printf(TEXT("Weather set to %s for %.2f seconds."), *Preset, Duration)
        : FString::Printf(TEXT("Weather set to %s."), *Preset);
    return true;
}

bool USimulatorCommandSubsystem::ExecuteTime(
    APlayerController* Controller,
    const TArray<FString>& Args,
    FString& OutMessage)
{
    if (Args.Num() != 2)
    {
        OutMessage = TEXT("Usage: time <dt|sec|day> <value>");
        return false;
    }

    double Value = 0.0;
    if (!ParseFiniteDouble(Args[1], Value))
    {
        OutMessage = TEXT("Time value must be a finite number.");
        return false;
    }

    UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(Controller);
    if (!IsValid(GameManager) || !IsValid(GameManager->GetWorldData()))
    {
        OutMessage = TEXT("World time is unavailable.");
        return false;
    }

    const FString Mode = Args[0].ToLower();
    bool bSuccess = false;
    if (Mode == TEXT("dt"))
    {
        bSuccess = GameManager->AddWorldTimeSeconds(Value);
    }
    else if (Mode == TEXT("sec"))
    {
        bSuccess = GameManager->SetWorldTimeSeconds(Value);
    }
    else if (Mode == TEXT("day"))
    {
        bSuccess = GameManager->SetWorldDay(Value);
    }
    else
    {
        OutMessage = TEXT("Time mode must be dt, sec, or day.");
        return false;
    }

    if (!bSuccess)
    {
        OutMessage = TEXT("Time command was rejected by the active world.");
        return false;
    }

    OutMessage = FString::Printf(TEXT("Time %s applied: %.3f"), *Mode, Value);
    return true;
}

bool USimulatorCommandSubsystem::ExecuteTeleport(
    APlayerController* Controller,
    const TArray<FString>& Args,
    FString& OutMessage)
{
    if (Args.Num() < 3)
    {
        OutMessage = TEXT("Usage: tp <x> <y> <z> [playerName]");
        return false;
    }

    if (PendingTeleport.bActive)
    {
        OutMessage = TEXT("Teleport rejected: another destination is still being preloaded.");
        return false;
    }

    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;
    if (!ParseFiniteDouble(Args[0], X) || !ParseFiniteDouble(Args[1], Y) || !ParseFiniteDouble(Args[2], Z))
    {
        OutMessage = TEXT("Teleport coordinates must be finite numbers.");
        return false;
    }

    constexpr double MaxCoordinateMagnitude = 1.0e9;
    if (FMath::Abs(X) > MaxCoordinateMagnitude || FMath::Abs(Y) > MaxCoordinateMagnitude || FMath::Abs(Z) > MaxCoordinateMagnitude)
    {
        OutMessage = TEXT("Teleport coordinate exceeds the simulator safety limit.");
        return false;
    }

    FString PlayerName;
    if (Args.Num() >= 4)
    {
        PlayerName = Args[3];
        for (int32 Index = 4; Index < Args.Num(); ++Index)
        {
            PlayerName += TEXT(" ");
            PlayerName += Args[Index];
        }
    }

    APlayerController* TargetController = ResolveTargetPlayer(Controller, PlayerName);
    APawn* TargetPawn = IsValid(TargetController) ? TargetController->GetPawn() : nullptr;
    if (!IsValid(TargetPawn))
    {
        OutMessage = TEXT("Target player/pawn was not found.");
        return false;
    }

    UWorld* World = TargetPawn->GetWorld();
    if (!World || World != Controller->GetWorld())
    {
        OutMessage = TEXT("Teleport failed because the target world is unavailable.");
        return false;
    }

    const FVector Destination(X, Y, Z);
    const FString TargetName = IsValid(TargetController->PlayerState)
        ? TargetController->PlayerState->GetPlayerName()
        : TargetController->GetName();

    // Add the destination as a second streaming observer before moving the Pawn. The current player
    // area remains resident until both the 512 m object chunks and every intersecting static model
    // (including its internal fine chunks) report completion.
    PendingTeleport.RequestingController = Controller;
    PendingTeleport.TargetController = TargetController;
    PendingTeleport.Destination = Destination;
    PendingTeleport.TargetName = TargetName;
    PendingTeleport.StartedAtSeconds = FPlatformTime::Seconds();
    PendingTeleport.bActive = true;

    if (PrepareTeleportDestination(World, Destination))
    {
        const bool bMoved = TargetPawn->TeleportTo(
            Destination, TargetPawn->GetActorRotation(), false, true);
        if (bMoved)
        {
            if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(TargetPawn))
            {
                Manager->SetPlayerLocation(TargetPawn->GetActorLocation(), TargetPawn);
            }
            ClearTeleportStreamingFocus();
            ResetPendingTeleport();
            OutMessage = FString::Printf(
                TEXT("Teleported %s to %.3f %.3f %.3f"), *TargetName, X, Y, Z);
            return true;
        }

        ClearTeleportStreamingFocus();
        ResetPendingTeleport();
        OutMessage = TEXT("Teleport failed because the destination was rejected.");
        return false;
    }

    SchedulePendingTeleportCheck();
    OutMessage = FString::Printf(
        TEXT("Preloading destination for %s at %.3f %.3f %.3f; teleport will occur when the area is fully ready."),
        *TargetName, X, Y, Z);
    return true;
}

bool USimulatorCommandSubsystem::PrepareTeleportDestination(
    UWorld* World,
    const FVector& Destination) const
{
    if (!World
        || !FMath::IsFinite(Destination.X)
        || !FMath::IsFinite(Destination.Y)
        || !FMath::IsFinite(Destination.Z))
    {
        return false;
    }

    bool bObjectAreaReady = true;
    if (UWorldObjectStreamingSubsystem* Chunks = World->GetSubsystem<UWorldObjectStreamingSubsystem>())
    {
        if (Chunks->IsRunning())
        {
            Chunks->SetPriorityStreamingFocus(Destination);
            bObjectAreaReady = Chunks->IsAreaLoaded(Destination);
        }
    }

    bool bSceneAreaReady = true;
    if (UWorldSceneStreamingSubsystem* Scenes = UWorldSceneStreamingSubsystem::Get(World))
    {
        if (Scenes->IsActiveForWorld(World))
        {
            Scenes->SetPriorityStreamingFocus(Destination);
            bSceneAreaReady = Scenes->IsLocationReady(Destination);
        }
    }

    return bObjectAreaReady && bSceneAreaReady;
}

void USimulatorCommandSubsystem::SchedulePendingTeleportCheck()
{
    if (!PendingTeleport.bActive) return;
    APlayerController* TargetController = PendingTeleport.TargetController.Get();
    UWorld* World = IsValid(TargetController) ? TargetController->GetWorld() : GetWorld();
    if (!World)
    {
        ClearTeleportStreamingFocus();
        ResetPendingTeleport();
        return;
    }

    World->GetTimerManager().SetTimer(
        PendingTeleportTimer,
        this,
        &USimulatorCommandSubsystem::ProcessPendingTeleport,
        TeleportPollIntervalSeconds,
        false);
}

void USimulatorCommandSubsystem::ProcessPendingTeleport()
{
    if (!PendingTeleport.bActive) return;

    APlayerController* TargetController = PendingTeleport.TargetController.Get();
    APawn* TargetPawn = IsValid(TargetController) ? TargetController->GetPawn() : nullptr;
    UWorld* World = IsValid(TargetPawn) ? TargetPawn->GetWorld() : nullptr;
    if (!World || !IsValid(TargetPawn))
    {
        SendTeleportStatus(TEXT("Teleport cancelled because the target player/pawn is no longer available."));
        ClearTeleportStreamingFocus();
        ResetPendingTeleport();
        return;
    }

    if (FPlatformTime::Seconds() - PendingTeleport.StartedAtSeconds > TeleportLoadTimeoutSeconds)
    {
        SendTeleportStatus(FString::Printf(
            TEXT("Teleport to %.3f %.3f %.3f timed out while waiting for streaming."),
            PendingTeleport.Destination.X, PendingTeleport.Destination.Y, PendingTeleport.Destination.Z));
        ClearTeleportStreamingFocus();
        ResetPendingTeleport();
        return;
    }

    if (!PrepareTeleportDestination(World, PendingTeleport.Destination))
    {
        SchedulePendingTeleportCheck();
        return;
    }

    const FVector Destination = PendingTeleport.Destination;
    const FString TargetName = PendingTeleport.TargetName;
    const bool bMoved = TargetPawn->TeleportTo(
        Destination, TargetPawn->GetActorRotation(), false, true);
    if (bMoved)
    {
        if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(TargetPawn))
        {
            Manager->SetPlayerLocation(TargetPawn->GetActorLocation(), TargetPawn);
        }
        SendTeleportStatus(FString::Printf(
            TEXT("Teleported %s to %.3f %.3f %.3f after destination streaming completed."),
            *TargetName, Destination.X, Destination.Y, Destination.Z));
    }
    else
    {
        SendTeleportStatus(TEXT("Teleport failed because the fully streamed destination was rejected."));
    }

    ClearTeleportStreamingFocus();
    ResetPendingTeleport();
}

void USimulatorCommandSubsystem::ClearTeleportStreamingFocus()
{
    UWorld* World = nullptr;
    if (APlayerController* TargetController = PendingTeleport.TargetController.Get())
    {
        World = TargetController->GetWorld();
    }
    if (!World) World = GetWorld();
    if (!World) return;

    if (UWorldObjectStreamingSubsystem* Chunks = World->GetSubsystem<UWorldObjectStreamingSubsystem>())
    {
        Chunks->ClearPriorityStreamingFocus();
    }
    if (UWorldSceneStreamingSubsystem* Scenes = UWorldSceneStreamingSubsystem::Get(World))
    {
        Scenes->ClearPriorityStreamingFocus();
    }
}

void USimulatorCommandSubsystem::ResetPendingTeleport()
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(PendingTeleportTimer);
    }
    PendingTeleport = FPendingTeleportRequest();
}

void USimulatorCommandSubsystem::SendTeleportStatus(const FString& Message) const
{
    if (APlayerController* RequestingController = PendingTeleport.RequestingController.Get())
    {
        RequestingController->ClientMessage(Message);
    }
}


void USimulatorCommandSubsystem::Tokenize(const FString& Input, TArray<FString>& OutTokens)
{
    OutTokens.Reset();

    const TCHAR* Cursor = *Input;
    while (Cursor && *Cursor)
    {
        FString Token = FParse::Token(Cursor, false);
        if (Token.IsEmpty())
        {
            break;
        }
        OutTokens.Add(MoveTemp(Token));
    }
}

bool USimulatorCommandSubsystem::ParseFiniteDouble(const FString& Token, double& OutValue)
{
    OutValue = 0.0;
    return FDefaultValueHelper::ParseDouble(Token, OutValue) && FMath::IsFinite(OutValue);
}

APlayerController* USimulatorCommandSubsystem::ResolveTargetPlayer(
    APlayerController* RequestingController,
    const FString& PlayerName)
{
    if (!IsValid(RequestingController) || PlayerName.IsEmpty())
    {
        return RequestingController;
    }

    UWorld* World = RequestingController->GetWorld();
    if (!World)
    {
        return nullptr;
    }

    for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
    {
        APlayerController* Candidate = It->Get();
        if (!IsValid(Candidate))
        {
            continue;
        }

        const FString CandidateName = IsValid(Candidate->PlayerState)
            ? Candidate->PlayerState->GetPlayerName()
            : Candidate->GetName();
        if (CandidateName.Equals(PlayerName, ESearchCase::IgnoreCase))
        {
            return Candidate;
        }
    }

    return nullptr;
}
