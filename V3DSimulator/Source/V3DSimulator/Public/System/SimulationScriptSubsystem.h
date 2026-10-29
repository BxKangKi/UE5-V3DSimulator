// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "System/SimulationCommandConfig.h"
#include "TimerManager.h"
#include "SimulationScriptSubsystem.generated.h"

class USimulationCommandObject;
class APlayerController;
class AActor;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSimulationCommandResult, FName, Command, const FString&, Message);

/** World-scoped JSON programs. Definitions are read once; only bounded condition checks run at 10 Hz. */
UCLASS()
class V3DSIMULATOR_API USimulationScriptSubsystem final : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual void Deinitialize() override;
    void Configure(FV3DSimulationCommandConfig Config);
    void Reset();
    static bool CanRequest(APlayerController* Controller);
    bool ExecuteNamed(APlayerController* Controller, FName Name, FString& Message, bool bFromChat = true);

    UFUNCTION(BlueprintCallable, Category="Simulation|Commands")
    bool ExecuteObject(APlayerController* Controller, USimulationCommandObject* Command, FString& Message);
    UFUNCTION(BlueprintPure, Category="Simulation|Commands")
    USimulationCommandObject* GetCommandObject(FName Name) const;
    UFUNCTION(BlueprintPure, Category="Simulation|Commands")
    TArray<FName> GetCommandNames() const;
    /** Explicit alias for an actor, e.g. object:car. Call on authority when the actor becomes available. */
    UFUNCTION(BlueprintCallable, Category="Simulation|Commands")
    bool RegisterCommandTarget(FName Name, AActor* Actor);
    UFUNCTION(BlueprintCallable, Category="Simulation|Commands")
    void UnregisterCommandTarget(FName Name, AActor* Actor);
    UPROPERTY(BlueprintAssignable, Category="Simulation|Commands")
    FSimulationCommandResult OnCommandResult;
private:
    UPROPERTY(Transient) TMap<FName, TObjectPtr<USimulationCommandObject>> CommandObjects;
    TMap<FName, TWeakObjectPtr<AActor>> Targets;
    TMap<FName, double> NextCommandAt;
    TArray<FV3DCommandTrigger> Triggers;
    TArray<FV3DCommandTriggerState> TriggerStates;
    TArray<FString> LastTriggerErrors;
    FTimerHandle TriggerTimer;
    double StartedAt = -1;
    float OriginalTimeDilation = 1;
    bool bChangedTimeDilation = false;
    uint64 Generation = 0;
    int32 TriggerCursor = 0;
    bool bExecuting = false;
    bool IsReady() const;
    AActor* ResolveTarget(const FString& Target, APlayerController* Controller);
    bool ExecuteDefinition(APlayerController* Controller, const FV3DCommandDefinition& Command, FString& Message, bool* bOutApplied = nullptr);
    bool Evaluate(const FV3DCommandCondition& Condition, APlayerController* Controller, double Elapsed, bool& Result);
    void PollTriggers();
};
