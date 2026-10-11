// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "System/SimulationCommandConfig.h"
#include "SimulationCommandObject.generated.h"

/** Construct Object from Class -> InitializeFromJson -> ExecuteObject (authority).
 * JSON is one {Name, AllowChat, Cooldown, Actions} object, not the whole commands.json file. */
UCLASS(BlueprintType, EditInlineNew)
class V3DSIMULATOR_API USimulationCommandObject final : public UObject
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="Simulation|Commands")
    bool InitializeFromJson(const FString& Json, FString& Error);
    UFUNCTION(BlueprintPure, Category="Simulation|Commands")
    FString ToJson() const;
    UFUNCTION(BlueprintPure, Category="Simulation|Commands")
    FName GetCommandName() const { return Definition.Name; }
    const FV3DCommandDefinition& GetDefinition() const { return Definition; }
    bool IsInitialized() const { return !Definition.Name.IsNone() && !Definition.Actions.IsEmpty(); }
private:
    friend class USimulationScriptSubsystem;
    FV3DCommandDefinition Definition;
    bool bInstalled = false;
};
