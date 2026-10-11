// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/SimulationScriptSubsystem.h"
#include "System/SimulationCommandObject.h"
#include "System/GameManagerSubSystem.h"
#include "System/WorldObjectStreamingSubsystem.h"
#include "Model/WorldSceneStreamingSubsystem.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"

namespace
{
    UPrimitiveComponent* PhysicsRoot(AActor* Actor)
    {
        auto* Root = IsValid(Actor) ? Cast<UPrimitiveComponent>(Actor->GetRootComponent()) : nullptr;
        return Root && Root->IsSimulatingPhysics() ? Root : nullptr;
    }
    UCharacterMovementComponent* CharacterMovement(AActor* Actor)
    {
        auto* Character = Cast<ACharacter>(Actor);
        return Character ? Character->GetCharacterMovement() : nullptr;
    }
    FVector LinearVelocity(AActor* Actor)
    {
        if (auto* Body = PhysicsRoot(Actor)) return Body->GetPhysicsLinearVelocity();
        return Actor->GetVelocity();
    }
    bool ValidTransform(const FTransform& Transform)
    {
        if (Transform.ContainsNaN()) return false;
        const auto Location = Transform.GetLocation(); const auto Scale = Transform.GetScale3D();
        for (int32 Index = 0; Index < 3; ++Index)
            if (FMath::Abs(Location[Index]) > 1.e9 || Scale[Index] < 0.001 || Scale[Index] > 1000) return false;
        return true;
    }
    FTransform ApplyTransform(const FTransform& Original, const FV3DCommandAction& Action)
    {
        FTransform Result = Original;
        if (Action.bLocation) Result.SetLocation(Action.bRelative ? Original.GetLocation() + Action.Location : Action.Location);
        if (Action.bRotation)
        {
            const FQuat Rotation = FRotator(Action.Rotation.X, Action.Rotation.Y, Action.Rotation.Z).Quaternion();
            Result.SetRotation(Action.bRelative ? (Rotation * Original.GetRotation()).GetNormalized() : Rotation);
        }
        if (Action.bScale) Result.SetScale3D(Action.bRelative ? Original.GetScale3D() * Action.Scale : Action.Scale);
        return Result;
    }
}

void USimulationScriptSubsystem::Configure(FV3DSimulationCommandConfig Config)
{
    check(IsInGameThread());
    auto RegisteredTargets = MoveTemp(Targets);
    Reset();
    Targets = MoveTemp(RegisteredTargets);
    for (auto& Definition : Config.Commands)
    {
        auto* Object = NewObject<USimulationCommandObject>(this);
        Object->Definition = MoveTemp(Definition); Object->bInstalled = true;
        CommandObjects.Add(Object->Definition.Name, Object);
    }
    Triggers = MoveTemp(Config.Triggers);
    TriggerStates.SetNum(Triggers.Num());
    LastTriggerErrors.SetNum(Triggers.Num());
    bool bAnyEnabled = false;
    for (const auto& Trigger : Triggers) bAnyEnabled |= Trigger.bEnabled;
    if (bAnyEnabled && GetWorld()->GetNetMode() != NM_Client)
        GetWorld()->GetTimerManager().SetTimer(TriggerTimer, this, &USimulationScriptSubsystem::PollTriggers, 0.1f, true);
}

void USimulationScriptSubsystem::Reset()
{
    ++Generation;
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(TriggerTimer);
        if (bChangedTimeDilation && World->GetNetMode() != NM_Client)
            UGameplayStatics::SetGlobalTimeDilation(World, OriginalTimeDilation);
    }
    bChangedTimeDilation = false; StartedAt = -1; TriggerCursor = 0;
    CommandObjects.Empty(); Targets.Empty(); NextCommandAt.Empty();
    Triggers.Empty(); TriggerStates.Empty(); LastTriggerErrors.Empty();
}
void USimulationScriptSubsystem::Deinitialize()
{
    Reset(); Super::Deinitialize();
}
bool USimulationScriptSubsystem::CanRequest(APlayerController* Controller)
{
    if (!IsValid(Controller) || !Controller->HasAuthority() || Controller->GetNetMode() == NM_Client) return false;
    if (Controller->IsLocalController()) return true;
    const auto* Manager = UGameManagerSubSystem::GetSubSystem(Controller);
    return Manager && Manager->AreCheatsEnabledForCurrentLevel();
}
bool USimulationScriptSubsystem::IsReady() const
{
    auto* Manager = UGameManagerSubSystem::GetSubSystem(this);
    return GetWorld() && GetWorld()->GetNetMode() != NM_Client && Manager
        && Manager->GetWorldData() && Manager->GetLoadingStatus() >= 1.0f;
}
USimulationCommandObject* USimulationScriptSubsystem::GetCommandObject(FName Name) const
{
    const auto* Object = CommandObjects.Find(Name);
    return Object ? Object->Get() : nullptr;
}
TArray<FName> USimulationScriptSubsystem::GetCommandNames() const
{
    TArray<FName> Names; CommandObjects.GetKeys(Names);
    Names.Sort([](FName A, FName B) { return A.LexicalLess(B); }); return Names;
}
bool USimulationScriptSubsystem::RegisterCommandTarget(FName Name, AActor* Actor)
{
    if (Name.IsNone() || !IsValid(Actor) || Actor->GetWorld() != GetWorld() || !Actor->HasAuthority()
        || GetWorld()->GetNetMode() == NM_Client) return false;
    const FString Text = Name.ToString();
    if (Text.Len() > 64) return false;
    for (int32 Index = 0; Index < Text.Len(); ++Index)
    {
        const TCHAR C = Text[Index];
        if (!((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_' || C == '-')) return false;
    }
    if (const auto* Existing = Targets.Find(Name); Existing && Existing->IsValid() && Existing->Get() != Actor) return false;
    // Remove expired aliases so repeated spawn/despawn cannot grow this registry indefinitely.
    for (auto It = Targets.CreateIterator(); It; ++It) if (!It.Value().IsValid()) It.RemoveCurrent();
    if (Targets.Num() >= 1024 && !Targets.Contains(Name)) return false;
    Targets.Add(Name, Actor); return true;
}
void USimulationScriptSubsystem::UnregisterCommandTarget(FName Name, AActor* Actor)
{
    if (const auto* Existing = Targets.Find(Name); Existing && Existing->Get() == Actor) Targets.Remove(Name);
}
AActor* USimulationScriptSubsystem::ResolveTarget(const FString& Target, APlayerController* Controller)
{
    if (Target == TEXT("self")) return IsValid(Controller) && Controller->GetWorld() == GetWorld() ? Controller->GetPawn() : nullptr;
    if (Target.StartsWith(TEXT("object:")))
    {
        const auto* Found = Targets.Find(FName(*Target.Mid(7))); return Found ? Found->Get() : nullptr;
    }
    if (Target.StartsWith(TEXT("entity:")))
    {
        FGuid UUID;
        if (FGuid::Parse(Target.Mid(7), UUID))
        {
            if (auto* Chunks = GetWorld()->GetSubsystem<UWorldObjectStreamingSubsystem>())
            {
                return Chunks->FindLoadedEntity(UUID);
            }
        }
    }
    return nullptr;
}

bool USimulationScriptSubsystem::ExecuteNamed(APlayerController* Controller, FName Name, FString& Message, bool bFromChat)
{
    if (!CanRequest(Controller) || Controller->GetWorld() != GetWorld()) { Message = TEXT("Command rejected: authority/cheats permission required."); return false; }
    auto* Command = GetCommandObject(Name);
    if (!Command) { Message = TEXT("Unknown command. Use /sim list."); return false; }
    if (bFromChat && !Command->Definition.bAllowChat) { Message = TEXT("This command does not allow chat/console execution."); return false; }
    return ExecuteDefinition(Controller, Command->Definition, Message);
}
bool USimulationScriptSubsystem::ExecuteObject(APlayerController* Controller, USimulationCommandObject* Command, FString& Message)
{
    if (!CanRequest(Controller) || Controller->GetWorld() != GetWorld() || !IsValid(Command) || !Command->IsInitialized())
    { Message = TEXT("Command rejected: invalid command object or authority/cheats permission."); return false; }
    return ExecuteDefinition(Controller, Command->Definition, Message);
}
bool USimulationScriptSubsystem::ExecuteDefinition(APlayerController* Controller, const FV3DCommandDefinition& Source, FString& Message, bool* bOutApplied)
{
    Message.Reset();
    if (bOutApplied) *bOutApplied = false;
    if (!IsReady() || bExecuting) { Message = TEXT("World is not ready or another command is executing."); return false; }
    const double Now = FPlatformTime::Seconds();
    if (const auto* Next = NextCommandAt.Find(Source.Name); Next && Now < *Next) { Message = TEXT("Command is on cooldown."); return false; }
    // A Blueprint callback can unload/reconfigure the world during actor movement.
    const FV3DCommandDefinition Command = Source;
    const uint64 ExecutionGeneration = Generation;
    TGuardValue<bool> Guard(bExecuting, true);
    TArray<TWeakObjectPtr<AActor>> Actors;
    TArray<FTransform> Transforms;
    TMap<AActor*, FTransform> Predicted;
    auto* Chunks = GetWorld()->GetSubsystem<UWorldObjectStreamingSubsystem>();
    auto* Scenes = UWorldSceneStreamingSubsystem::Get(GetWorld());
    // Validate all targets/capabilities before applying the first action.
    for (const auto& Action : Command.Actions)
    {
        if (Action.Type == EV3DCommandAction::TimeScale) { Actors.Add(nullptr); Transforms.Add(FTransform::Identity); continue; }
        AActor* Actor = ResolveTarget(Action.Target, Controller);
        if (!IsValid(Actor) || Actor->GetWorld() != GetWorld() || !Actor->HasAuthority() || !Actor->GetRootComponent())
        { Message = TEXT("Target is not loaded/registered: ") + Action.Target; return false; }
        Actors.Add(Actor);
        FTransform Transform = Predicted.Contains(Actor) ? Predicted[Actor] : Actor->GetActorTransform();
        if (Action.Type == EV3DCommandAction::Transform)
        {
            if (Actor->GetRootComponent()->Mobility != EComponentMobility::Movable)
            { Message = TEXT("Transform requires a Movable root: ") + Action.Target; return false; }
            Transform = ApplyTransform(Transform, Action);
            if (!ValidTransform(Transform)) { Message = TEXT("Resulting transform exceeds the coordinate/scale limits."); return false; }
            if (Action.bLocation && ((Chunks && Chunks->IsRunning() && !Chunks->IsLocationLoaded(Transform.GetLocation()))
                || (Scenes && Scenes->IsActiveForWorld(GetWorld()) && !Scenes->IsLocationReady(Transform.GetLocation()))))
            { Message = TEXT("Transform destination is not streamed in. Preload it first; player long-distance travel can use tp."); return false; }
            Predicted.Add(Actor, Transform);
        }
        else if (!PhysicsRoot(Actor) && (!CharacterMovement(Actor) || Action.bAngular))
        { Message = TEXT("Velocity/speed requires a simulated physics root or CharacterMovement; Angular requires physics."); return false; }
        Transforms.Add(Transform);
    }
    // Bound cooldown storage for dynamically constructed command objects.
    for (auto It = NextCommandAt.CreateIterator(); It; ++It) if (Now >= It.Value()) It.RemoveCurrent();
    if (NextCommandAt.Num() >= 1024 && !NextCommandAt.Contains(Command.Name))
    { Message = TEXT("Too many active command cooldowns."); return false; }
    NextCommandAt.Add(Command.Name, Now + Command.Cooldown);
    int32 Applied = 0;
    for (int32 Index = 0; Index < Command.Actions.Num(); ++Index)
    {
        if (Generation != ExecutionGeneration || !IsReady()) { Message = TEXT("Command interrupted by world change."); return false; }
        const auto& Action = Command.Actions[Index];
        if (Action.Type == EV3DCommandAction::TimeScale)
        {
            if (!bChangedTimeDilation) OriginalTimeDilation = UGameplayStatics::GetGlobalTimeDilation(this);
            bChangedTimeDilation = true;
            UGameplayStatics::SetGlobalTimeDilation(this, static_cast<float>(Action.Value));
        }
        else
        {
            AActor* Actor = Actors[Index].Get();
            if (!IsValid(Actor)) { Message = FString::Printf(TEXT("Target was destroyed after %d actions."), Applied); return false; }
            if (Action.Type == EV3DCommandAction::Transform)
            {
                if (!Actor->SetActorTransform(Transforms[Index], false, nullptr, ETeleportType::TeleportPhysics))
                { Message = FString::Printf(TEXT("Transform rejected after %d actions."), Applied); return false; }
            }
            else
            {
                FVector Velocity = Action.Linear;
                if (Action.Type == EV3DCommandAction::Speed)
                {
                    const auto Current = LinearVelocity(Actor);
                    Velocity = (Current.IsNearlyZero() ? Actor->GetActorForwardVector() : Current.GetSafeNormal()) * Action.Value;
                }
                if (auto* Body = PhysicsRoot(Actor))
                {
                    Body->SetPhysicsLinearVelocity(Velocity);
                    if (Action.bAngular) Body->SetPhysicsAngularVelocityInDegrees(Action.Angular);
                    Body->WakeAllRigidBodies();
                }
                else if (auto* Movement = CharacterMovement(Actor); Movement && !Action.bAngular) Movement->Velocity = Velocity;
                else { Message = TEXT("Target movement capability changed during execution."); return false; }
            }
            if (Generation != ExecutionGeneration) { Message = TEXT("Command interrupted by world change."); return false; }
            if (IsValid(Actor))
            {
                Actor->ForceNetUpdate();
                if (Chunks) Chunks->MarkObjectChanged(Actor);
                if (auto* Manager = UGameManagerSubSystem::GetSubSystem(this)) Manager->SetPlayerLocation(Actor->GetActorLocation(), Actor);
            }
        }
        ++Applied;
        if (bOutApplied) *bOutApplied = true;
    }
    Message = FString::Printf(TEXT("%s: applied %d action(s)."), *Command.Name.ToString(), Applied);
    OnCommandResult.Broadcast(Command.Name, Message);
    return true;
}

bool USimulationScriptSubsystem::Evaluate(const FV3DCommandCondition& Condition, APlayerController* Controller, double Elapsed, bool& Result)
{
    Result = false;
    double Value = Elapsed;
    if (Condition.Metric != EV3DCommandMetric::Elapsed)
    {
        AActor* Actor = ResolveTarget(Condition.Target, Controller);
        if (!IsValid(Actor)) return false;
        switch (Condition.Metric)
        {
        case EV3DCommandMetric::Speed: Value = LinearVelocity(Actor).Size(); break;
        case EV3DCommandMetric::Distance: Value = FVector::Distance(Actor->GetActorLocation(), Condition.Point); break;
        case EV3DCommandMetric::PositionZ: Value = Actor->GetActorLocation().Z; break;
        default: break;
        }
    }
    Result = Condition.Matches(Value); return true;
}
void USimulationScriptSubsystem::PollTriggers()
{
    if (!IsReady() || Triggers.IsEmpty() || bExecuting) return;
    const double Now = FPlatformTime::Seconds();
    if (StartedAt < 0) StartedAt = GetWorld()->GetTimeSeconds();
    const double Elapsed = GetWorld()->GetTimeSeconds() - StartedAt;
    const uint64 PollGeneration = Generation;
    APlayerController* Controller = GetWorld()->GetFirstPlayerController();
    int32 Executions = 0;
    for (int32 Checked = 0, Count = Triggers.Num(); Checked < Count; ++Checked)
    {
        const int32 Index = TriggerCursor;
        TriggerCursor = (TriggerCursor + 1) % Count;
        const auto& Trigger = Triggers[Index];
        if (!Trigger.bEnabled || (Trigger.bOnce && TriggerStates[Index].bFired)) continue;
        bool bMatched = !Trigger.bMatchAny, bKnown = true;
        for (const auto& Condition : Trigger.Conditions)
        {
            bool Result = false;
            if (!Evaluate(Condition, Controller, Elapsed, Result)) { bKnown = false; continue; }
            if (Trigger.bMatchAny) bMatched |= Result; else bMatched &= Result;
        }
        // Streaming-out is unknown, not false: it cannot rearm a latched trigger.
        if (!bKnown || !TriggerStates[Index].ShouldRun(bMatched, Now, Trigger.bOnce)) continue;
        auto* Object = GetCommandObject(Trigger.Command);
        FString Message;
        const double Cooldown = Trigger.Cooldown;
        bool bApplied = false;
        const bool bSuccess = Object && ExecuteDefinition(Controller, Object->GetDefinition(), Message, &bApplied);
        if (Generation != PollGeneration) return;
        if (bSuccess || bApplied) TriggerStates[Index].DidRun(Now, Cooldown);
        else TriggerStates[Index].NextAllowed = Now + Cooldown; // Retry unavailable targets without per-tick error spam.
        if (!bSuccess && LastTriggerErrors[Index] != Message)
            UE_LOG(LogTemp, Warning, TEXT("Simulation trigger '%s': %s"), *Triggers[Index].Name.ToString(), *Message);
        LastTriggerErrors[Index] = bSuccess ? FString() : Message;
        if (++Executions >= 8) break;
    }
}
