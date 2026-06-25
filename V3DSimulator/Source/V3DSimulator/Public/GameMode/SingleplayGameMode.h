// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "GameMode/GameplayGameModeBase.h"
#include "SingleplayGameMode.generated.h"

/** Standalone gameplay GameMode. Runtime world data is read only from Worlds/<name>.v3d. */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API ASingleplayGameMode : public AV3DSimulatorGameplayGameModeBase
{
    GENERATED_BODY()
public:
    ASingleplayGameMode();

protected:
    virtual void BeginPlay() override;
};
