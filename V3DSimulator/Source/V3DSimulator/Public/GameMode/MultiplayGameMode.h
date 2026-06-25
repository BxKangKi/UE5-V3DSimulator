// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "GameMode/GameplayGameModeBase.h"
#include "MultiplayGameMode.generated.h"

/** Listen/dedicated-server gameplay GameMode. Clients bootstrap render-only streaming from replicated world state. */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API AMultiplayGameMode : public AV3DSimulatorGameplayGameModeBase
{
    GENERATED_BODY()
public:
    AMultiplayGameMode();

protected:
    virtual void BeginPlay() override;
};
