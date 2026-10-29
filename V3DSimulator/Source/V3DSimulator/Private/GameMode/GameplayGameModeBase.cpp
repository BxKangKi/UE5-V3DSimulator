// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "GameMode/GameplayGameModeBase.h"

#include "System/GameManagerSubSystem.h"
#include "System/GameUpdateSubSystem.h"
#include "System/V3DSimulatorGameInstance.h"
#include "Character/CharacterController.h"
#include "Character/PlayerCharacterController.h"

AV3DSimulatorGameplayGameModeBase::AV3DSimulatorGameplayGameModeBase()
{
    PrimaryActorTick.bCanEverTick = false;

    // Native defaults remove the need for a Blueprint GameMode just to wire the standard simulator
    // pawn/controller. Blueprint subclasses can still override these classes if a project needs to.
    PlayerControllerClass = APlayerCharacterController::StaticClass();
    DefaultPawnClass = ACharacterController::StaticClass();
}

void AV3DSimulatorGameplayGameModeBase::BeginPlay()
{
    // Prepare the class-backed central registry before Blueprint ReceiveBeginPlay or any runtime
    // subsystem asks for assets. The GameInstance keeps the resulting instance private.
    if (UV3DSimulatorGameInstance* SimulatorGameInstance = Cast<UV3DSimulatorGameInstance>(GetGameInstance()))
    {
        SimulatorGameInstance->EnsureAssetRegistry();
    }

    Super::BeginPlay();

    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        Manager->StartGameplaySession(this);
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        TWeakObjectPtr<AV3DSimulatorGameplayGameModeBase> WeakThis(this);
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [WeakThis](const float DeltaSeconds)
            {
                if (AV3DSimulatorGameplayGameModeBase* StrongThis = WeakThis.Get())
                {
                    StrongThis->UpdateFromGameUpdate(DeltaSeconds);
                }
            },
            5);
    }
}

void AV3DSimulatorGameplayGameModeBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;

    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        Manager->StopGameplaySession(EndPlayReason, this);
    }

    Super::EndPlay(EndPlayReason);
}

void AV3DSimulatorGameplayGameModeBase::UpdateFromGameUpdate(const float DeltaSeconds)
{
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        if (Manager->IsActiveGameMode(this))
        {
            Manager->UpdateGameManager(DeltaSeconds);
        }
    }
}
