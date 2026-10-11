// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

enum class EV3DCommandAction : uint8 { Transform, Velocity, Speed, TimeScale };
enum class EV3DCommandMetric : uint8 { Elapsed, Speed, Distance, PositionZ };
enum class EV3DCommandComparison : uint8 { Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual };

struct FV3DCommandAction
{
    EV3DCommandAction Type = EV3DCommandAction::Transform;
    FString Target = TEXT("self");
    FVector Location = FVector::ZeroVector;
    FVector Rotation = FVector::ZeroVector; // Pitch, Yaw, Roll in degrees.
    FVector Scale = FVector::OneVector;
    FVector Linear = FVector::ZeroVector;
    FVector Angular = FVector::ZeroVector; // Degrees/second.
    double Value = 0;
    bool bLocation = false, bRotation = false, bScale = false, bAngular = false;
    bool bRelative = false;
};

struct FV3DCommandCondition
{
    EV3DCommandMetric Metric = EV3DCommandMetric::Elapsed;
    EV3DCommandComparison Comparison = EV3DCommandComparison::GreaterEqual;
    FString Target = TEXT("self");
    FVector Point = FVector::ZeroVector;
    double Value = 0;
    double Tolerance = 0.001;
    bool Matches(double Actual) const;
};

struct FV3DCommandDefinition
{
    FName Name;
    bool bAllowChat = true;
    double Cooldown = 0.25;
    TArray<FV3DCommandAction> Actions;
};

struct FV3DCommandTrigger
{
    FName Name, Command;
    bool bEnabled = false, bOnce = true, bMatchAny = false;
    double Cooldown = 1;
    TArray<FV3DCommandCondition> Conditions;
};

/** Edge-trigger state: failed execution is retried; a successful sustained condition never loops. */
struct FV3DCommandTriggerState
{
    bool bLatched = false, bFired = false;
    double NextAllowed = 0;
    bool ShouldRun(bool bCondition, double Now, bool bOnce);
    void DidRun(double Now, double Cooldown);
};

/** Bounded, worker-safe JSON DSL. No console, shell, arbitrary UFunction or recursive evaluation. */
struct V3DSIMULATOR_API FV3DSimulationCommandConfig
{
    TArray<FV3DCommandDefinition> Commands;
    TArray<FV3DCommandTrigger> Triggers;
    static bool Parse(const TSharedPtr<FJsonObject>& Json, FV3DSimulationCommandConfig& Out, FString& Error);
    static bool ParseCommand(const TSharedPtr<FJsonObject>& Json, FV3DCommandDefinition& Out, FString& Error);
};
