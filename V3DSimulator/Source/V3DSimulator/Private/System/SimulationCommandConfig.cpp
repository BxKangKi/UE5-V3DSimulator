// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/SimulationCommandConfig.h"
#include <initializer_list>

namespace V3DCommandConfigPrivate
{
    bool Fields(const FJsonObject& Json, std::initializer_list<const TCHAR*> Allowed, FString& Error)
    {
        for (const auto& Field : Json.Values)
        {
            bool bKnown = false;
            for (const TCHAR* Key : Allowed) if (Field.Key == Key) { bKnown = true; break; }
            if (!bKnown) { Error = FString::Printf(TEXT("Unknown command field: %s"), *Field.Key); return false; }
        }
        return true;
    }
    bool Number(const FJsonObject& Json, const TCHAR* Key, double& Out, double Min, double Max, bool bRequired = false)
    {
        if (!Json.HasField(Key)) return !bRequired;
        return Json.TryGetNumberField(Key, Out) && FMath::IsFinite(Out) && Out >= Min && Out <= Max;
    }
    bool Boolean(const FJsonObject& Json, const TCHAR* Key, bool& Out)
    {
        return !Json.HasField(Key) || Json.TryGetBoolField(Key, Out);
    }
    bool Identifier(const FString& Name)
    {
        if (Name.IsEmpty() || Name.Len() > 64 || FName(*Name).IsNone()) return false;
        for (int32 Index = 0; Index < Name.Len(); ++Index)
        {
            const TCHAR C = Name[Index];
            if (!((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_' || C == '-')) return false;
        }
        return true;
    }
    bool Target(const FJsonObject& Json, FString& Out)
    {
        if (!Json.HasField(TEXT("Target"))) return true;
        if (!Json.TryGetStringField(TEXT("Target"), Out)) return false;
        if (Out == TEXT("self")) return true;
        if (Out.StartsWith(TEXT("object:"))) return Identifier(Out.Mid(7));
        FGuid UUID;
        return Out.StartsWith(TEXT("entity:")) && FGuid::Parse(Out.Mid(7), UUID) && UUID.IsValid();
    }
    bool Vector(const FJsonObject& Json, const TCHAR* Key, FVector& Out, double Min, double Max)
    {
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        if (!Json.TryGetArrayField(Key, Items) || Items->Num() != 3) return false;
        for (int32 Index = 0; Index < 3; ++Index)
        {
            double Value = 0;
            if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetNumber(Value)
                || !FMath::IsFinite(Value) || Value < Min || Value > Max) return false;
            Out[Index] = Value;
        }
        return true;
    }
    bool Action(const TSharedPtr<FJsonObject>& Json, FV3DCommandAction& Out, FString& Error)
    {
        FString Type;
        if (!Json.IsValid() || !Json->TryGetStringField(TEXT("Type"), Type) || !Target(*Json, Out.Target)) return false;
        if (Type == TEXT("transform"))
        {
            if (!Fields(*Json, {TEXT("Type"), TEXT("Target"), TEXT("Location"), TEXT("Rotation"), TEXT("Scale"), TEXT("Relative")}, Error)
                || !Boolean(*Json, TEXT("Relative"), Out.bRelative)) return false;
            Out.bLocation = Json->HasField(TEXT("Location")); Out.bRotation = Json->HasField(TEXT("Rotation")); Out.bScale = Json->HasField(TEXT("Scale"));
            return (Out.bLocation || Out.bRotation || Out.bScale)
                && (!Out.bLocation || Vector(*Json, TEXT("Location"), Out.Location, -1.e9, 1.e9))
                && (!Out.bRotation || Vector(*Json, TEXT("Rotation"), Out.Rotation, -360000, 360000))
                && (!Out.bScale || Vector(*Json, TEXT("Scale"), Out.Scale, 0.001, 1000));
        }
        if (Type == TEXT("velocity"))
        {
            Out.Type = EV3DCommandAction::Velocity;
            Out.bAngular = Json->HasField(TEXT("Angular"));
            return Fields(*Json, {TEXT("Type"), TEXT("Target"), TEXT("Linear"), TEXT("Angular")}, Error)
                && Vector(*Json, TEXT("Linear"), Out.Linear, -1.e6, 1.e6)
                && (!Out.bAngular || Vector(*Json, TEXT("Angular"), Out.Angular, -360000, 360000));
        }
        if (Type == TEXT("speed"))
        {
            Out.Type = EV3DCommandAction::Speed;
            return Fields(*Json, {TEXT("Type"), TEXT("Target"), TEXT("Value")}, Error)
                && Number(*Json, TEXT("Value"), Out.Value, 0, 1.e6, true);
        }
        if (Type == TEXT("time_scale"))
        {
            Out.Type = EV3DCommandAction::TimeScale;
            return Fields(*Json, {TEXT("Type"), TEXT("Value")}, Error)
                && Number(*Json, TEXT("Value"), Out.Value, 0.01, 10, true);
        }
        return false;
    }
    bool Condition(const TSharedPtr<FJsonObject>& Json, FV3DCommandCondition& Out, FString& Error)
    {
        FString Type, Operator;
        if (!Json.IsValid() || !Fields(*Json, {TEXT("Type"), TEXT("Target"), TEXT("Operator"), TEXT("Value"), TEXT("Tolerance"), TEXT("Point")}, Error)
            || !Json->TryGetStringField(TEXT("Type"), Type) || !Json->TryGetStringField(TEXT("Operator"), Operator)
            || !Target(*Json, Out.Target) || !Number(*Json, TEXT("Value"), Out.Value, -1.e12, 1.e12, true)
            || !Number(*Json, TEXT("Tolerance"), Out.Tolerance, 0, 1.e6)) return false;
        if (Type == TEXT("elapsed")) Out.Metric = EV3DCommandMetric::Elapsed;
        else if (Type == TEXT("speed")) Out.Metric = EV3DCommandMetric::Speed;
        else if (Type == TEXT("distance")) Out.Metric = EV3DCommandMetric::Distance;
        else if (Type == TEXT("position_z")) Out.Metric = EV3DCommandMetric::PositionZ;
        else return false;
        if (Out.Metric == EV3DCommandMetric::Distance)
        {
            if (!Vector(*Json, TEXT("Point"), Out.Point, -1.e9, 1.e9)) return false;
        }
        else if (Json->HasField(TEXT("Point"))) return false;
        if (Out.Metric == EV3DCommandMetric::Elapsed && Json->HasField(TEXT("Target"))) return false;
        if (Out.Metric != EV3DCommandMetric::PositionZ && Out.Value < 0) return false;
        if (Operator == TEXT("<")) Out.Comparison = EV3DCommandComparison::Less;
        else if (Operator == TEXT("<=")) Out.Comparison = EV3DCommandComparison::LessEqual;
        else if (Operator == TEXT(">")) Out.Comparison = EV3DCommandComparison::Greater;
        else if (Operator == TEXT(">=")) Out.Comparison = EV3DCommandComparison::GreaterEqual;
        else if (Operator == TEXT("==")) Out.Comparison = EV3DCommandComparison::Equal;
        else if (Operator == TEXT("!=")) Out.Comparison = EV3DCommandComparison::NotEqual;
        else return false;
        return true;
    }
}

bool FV3DCommandCondition::Matches(double Actual) const
{
    if (!FMath::IsFinite(Actual)) return false;
    switch (Comparison)
    {
    case EV3DCommandComparison::Less: return Actual < Value;
    case EV3DCommandComparison::LessEqual: return Actual <= Value;
    case EV3DCommandComparison::Greater: return Actual > Value;
    case EV3DCommandComparison::GreaterEqual: return Actual >= Value;
    case EV3DCommandComparison::Equal: return FMath::Abs(Actual - Value) <= Tolerance;
    case EV3DCommandComparison::NotEqual: return FMath::Abs(Actual - Value) > Tolerance;
    }
    return false;
}

bool FV3DCommandTriggerState::ShouldRun(bool bCondition, double Now, bool bOnce)
{
    if (!bCondition) bLatched = false;
    return bCondition && !bLatched && !(bOnce && bFired) && Now >= NextAllowed;
}
void FV3DCommandTriggerState::DidRun(double Now, double Cooldown)
{
    bLatched = true; bFired = true; NextAllowed = Now + Cooldown;
}

bool FV3DSimulationCommandConfig::ParseCommand(const TSharedPtr<FJsonObject>& Json, FV3DCommandDefinition& Out, FString& Error)
{
    using namespace V3DCommandConfigPrivate;
    Out = FV3DCommandDefinition(); Error.Reset(); FV3DCommandDefinition Parsed;
    FString Name;
    const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
    if (!Json.IsValid() || !Fields(*Json, {TEXT("Name"), TEXT("AllowChat"), TEXT("Cooldown"), TEXT("Actions")}, Error)
        || !Json->TryGetStringField(TEXT("Name"), Name) || !Identifier(Name)
        || !Boolean(*Json, TEXT("AllowChat"), Parsed.bAllowChat)
        || !Number(*Json, TEXT("Cooldown"), Parsed.Cooldown, 0.1, 86400)
        || !Json->TryGetArrayField(TEXT("Actions"), Actions) || Actions->IsEmpty() || Actions->Num() > 64)
    { if (Error.IsEmpty()) Error = TEXT("Invalid command: Name, 1..64 Actions and Cooldown >= 0.1 required."); return false; }
    Parsed.Name = FName(*Name);
    for (const auto& Item : *Actions)
    {
        FV3DCommandAction Entry;
        if (!Item.IsValid() || Item->Type != EJson::Object || !Action(Item->AsObject(), Entry, Error))
        { if (Error.IsEmpty()) Error = TEXT("Invalid action in command: ") + Name; return false; }
        Parsed.Actions.Add(MoveTemp(Entry));
    }
    Out = MoveTemp(Parsed); return true;
}

bool FV3DSimulationCommandConfig::Parse(const TSharedPtr<FJsonObject>& Json, FV3DSimulationCommandConfig& Out, FString& Error)
{
    using namespace V3DCommandConfigPrivate;
    Out = FV3DSimulationCommandConfig(); Error.Reset(); FV3DSimulationCommandConfig Parsed;
    auto Fail = [&Error](const FString& Message) { if (Error.IsEmpty()) Error = Message; return false; };
    double Version = 1;
    if (!Json.IsValid() || !Fields(*Json, {TEXT("Version"), TEXT("Commands"), TEXT("Triggers")}, Error)
        || !Number(*Json, TEXT("Version"), Version, 1, 1, true)) return Fail(TEXT("commands.json requires numeric Version: 1"));
    const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Triggers = nullptr;
    if (!Json->TryGetArrayField(TEXT("Commands"), Commands) || Commands->Num() > 128
        || !Json->TryGetArrayField(TEXT("Triggers"), Triggers) || Triggers->Num() > 128) return Fail(TEXT("Commands and Triggers must be arrays with at most 128 entries each"));
    TSet<FName> Names;
    int32 TotalActions = 0;
    for (const auto& Item : *Commands)
    {
        FV3DCommandDefinition Entry;
        if (!Item.IsValid() || Item->Type != EJson::Object || !ParseCommand(Item->AsObject(), Entry, Error)
            || Names.Contains(Entry.Name)) return Fail(TEXT("Invalid/duplicate command"));
        TotalActions += Entry.Actions.Num();
        if (TotalActions > 1024) return Fail(TEXT("Total command actions exceed 1024"));
        Names.Add(Entry.Name); Parsed.Commands.Add(MoveTemp(Entry));
    }
    TSet<FName> TriggerNames;
    for (const auto& Item : *Triggers)
    {
        if (!Item.IsValid() || Item->Type != EJson::Object) return Fail(TEXT("Invalid trigger object"));
        const auto Trigger = Item->AsObject(); FV3DCommandTrigger Entry; FString Name, Command, Match;
        const TArray<TSharedPtr<FJsonValue>>* Conditions = nullptr;
        if (!Trigger.IsValid() || !Fields(*Trigger, {TEXT("Name"), TEXT("Command"), TEXT("Enabled"), TEXT("Once"), TEXT("Cooldown"), TEXT("Match"), TEXT("Conditions")}, Error)
            || !Trigger->TryGetStringField(TEXT("Name"), Name) || !Identifier(Name)
            || !Trigger->TryGetStringField(TEXT("Command"), Command) || !Names.Contains(FName(*Command))
            || TriggerNames.Contains(FName(*Name)) || !Boolean(*Trigger, TEXT("Enabled"), Entry.bEnabled)
            || !Boolean(*Trigger, TEXT("Once"), Entry.bOnce) || !Number(*Trigger, TEXT("Cooldown"), Entry.Cooldown, 0.1, 86400)
            || !Trigger->TryGetArrayField(TEXT("Conditions"), Conditions) || Conditions->IsEmpty() || Conditions->Num() > 8)
            return Fail(TEXT("Invalid trigger, duplicate name or unknown command: ") + Name);
        if (Trigger->HasField(TEXT("Match")))
        {
            if (!Trigger->TryGetStringField(TEXT("Match"), Match) || (Match != TEXT("All") && Match != TEXT("Any"))) return Fail(TEXT("Trigger Match must be All or Any"));
            Entry.bMatchAny = Match == TEXT("Any");
        }
        Entry.Name = FName(*Name); Entry.Command = FName(*Command);
        for (const auto& Value : *Conditions)
        {
            FV3DCommandCondition ConditionEntry;
            if (!Value.IsValid() || Value->Type != EJson::Object || !Condition(Value->AsObject(), ConditionEntry, Error)) return Fail(TEXT("Invalid condition in trigger: ") + Name);
            Entry.Conditions.Add(MoveTemp(ConditionEntry));
        }
        TriggerNames.Add(Entry.Name); Parsed.Triggers.Add(MoveTemp(Entry));
    }
    Out = MoveTemp(Parsed); return true;
}
