// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/V3DSimulatorGameInstance.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/ProjectWorkspace.h"
#include "Engine/World.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/World.h"
#include "System/SimulatorFileServices.h"

void UV3DSimulatorGameInstance::Init()
{
    FSimulatorFileServices::WriteStartupLog(TEXT("GameInstance Init entered"));
    Super::Init();
    if (!V3DSimulatorProjectWorkspace::EnsureWorkspaceRoots())
    {
        UE_LOG(LogTemp, Error,
            TEXT("V3DSimulator failed to create the Projects, Resources, or Worlds user-data directory."));
    }
    const bool bRegistryReady = EnsureAssetRegistry();
    FSimulatorFileServices::WriteStartupLog(bRegistryReady ? TEXT("AssetRegistry ready") : TEXT("ERROR: AssetRegistry initialization failed"));
    if (bRegistryReady && IsValid(RuntimeAssetRegistry))
    {
        FSimulatorFileServices::WriteStartupLog(FString::Printf(
            TEXT("Registry world refs: MainWorld=%s GameplayWorld=%s HostWorld=%s ClientWorld=%s"),
            *RuntimeAssetRegistry->MainWorld.ToSoftObjectPath().ToString(),
            *RuntimeAssetRegistry->GameplayWorld.ToSoftObjectPath().ToString(),
            *RuntimeAssetRegistry->HostWorld.ToSoftObjectPath().ToString(),
            *RuntimeAssetRegistry->ClientWorld.ToSoftObjectPath().ToString()));
        if (RuntimeAssetRegistry->MainWorld.IsNull())
        {
            UE_LOG(LogTemp, Error, TEXT("AssetRegistry.MainWorld is not assigned."));
        }
    }
}

bool UV3DSimulatorGameInstance::EnsureAssetRegistry()
{
    // Loading a Blueprint class and allocating a UObject are game-thread operations. During GC/map
    // teardown, fail closed instead of trying to recreate project assets from a stale callback.
    if (!IsInGameThread() || IsGarbageCollecting())
    {
        return IsValid(RuntimeAssetRegistry);
    }

    if (IsValid(RuntimeAssetRegistry))
    {
        return true;
    }

    RuntimeAssetRegistry = nullptr;
    if (AssetRegistryClass.IsNull())
    {
        AssetRegistryClass = TSoftClassPtr<UV3DSimulatorAssetRegistry>(FSoftObjectPath(
            TEXT("/Game/Blueprints/BP_AssetRegistry.BP_AssetRegistry_C")));
    }

    UClass* RegistryClass = AssetRegistryClass.LoadSynchronous();
    if (!IsValid(RegistryClass)
        || !RegistryClass->IsChildOf(UV3DSimulatorAssetRegistry::StaticClass())
        || RegistryClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
    {
        UE_LOG(LogTemp, Error,
            TEXT("V3DSimulatorGameInstance AssetRegistryClass is invalid or cannot be instantiated. Class=%s"),
            *GetNameSafe(RegistryClass));
        FSimulatorFileServices::WriteStartupLog(TEXT("ERROR: Cannot load AssetRegistry class: ") + AssetRegistryClass.ToSoftObjectPath().ToString());
        return false;
    }

    RuntimeAssetRegistry = NewObject<UV3DSimulatorAssetRegistry>(
        this, RegistryClass, NAME_None, RF_Transient);
    if (!IsValid(RuntimeAssetRegistry))
    {
        UE_LOG(LogTemp, Error,
            TEXT("V3DSimulatorGameInstance failed to create the private runtime asset registry from class %s."),
            *GetNameSafe(RegistryClass));
        return false;
    }

    RuntimeAssetRegistry->EnsureMenuDefaults();

    UE_LOG(LogTemp, Display,
        TEXT("V3DSimulator asset registry initialized from class %s."),
        *GetNameSafe(RegistryClass));
    return true;
}

UV3DSimulatorAssetRegistry* UV3DSimulatorGameInstance::GetAssetRegistry() const
{
    return IsValid(RuntimeAssetRegistry) ? RuntimeAssetRegistry.Get() : nullptr;
}

UV3DSimulatorAssetRegistry* UV3DSimulatorGameInstance::GetAssetRegistryFromContext(const UObject* WorldContextObject)
{
    if (!IsValid(WorldContextObject) || !IsInGameThread() || IsGarbageCollecting())
    {
        return nullptr;
    }

    UGameInstance* GameInstance = Cast<UGameInstance>(const_cast<UObject*>(WorldContextObject));
    if (!GameInstance)
    {
        const UWorld* World = WorldContextObject->GetWorld();
        GameInstance = IsValid(World) ? World->GetGameInstance() : nullptr;
    }

    if (UV3DSimulatorGameInstance* SimulatorGameInstance = Cast<UV3DSimulatorGameInstance>(GameInstance))
    {
        if (UV3DSimulatorAssetRegistry* Existing = SimulatorGameInstance->GetAssetRegistry())
        {
            return Existing;
        }

        if (SimulatorGameInstance->EnsureAssetRegistry())
        {
            return SimulatorGameInstance->GetAssetRegistry();
        }
    }
    return nullptr;
}
