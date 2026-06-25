// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/V3DSimulatorGameInstance.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/ProjectWorkspace.h"
#include "Engine/World.h"

void UV3DSimulatorGameInstance::Init()
{
    Super::Init();
    if (!V3DSimulatorProjectWorkspace::EnsureWorkspaceRoots())
    {
        UE_LOG(LogTemp, Error,
            TEXT("V3DSimulator failed to create the Projects, Resources, or Worlds user-data directory."));
    }
    EnsureAssetRegistry();
}

bool UV3DSimulatorGameInstance::EnsureAssetRegistry()
{
    if (IsValid(RuntimeAssetRegistry))
    {
        return true;
    }

    RuntimeAssetRegistry = nullptr;
    if (AssetRegistryClass.IsNull())
    {
        UE_LOG(LogTemp, Error,
            TEXT("V3DSimulatorGameInstance has no AssetRegistryClass. Assign a Blueprint subclass of UV3DSimulatorAssetRegistry in the GameInstance defaults."));
        return false;
    }

    UClass* RegistryClass = AssetRegistryClass.LoadSynchronous();
    if (!IsValid(RegistryClass)
        || !RegistryClass->IsChildOf(UV3DSimulatorAssetRegistry::StaticClass())
        || RegistryClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
    {
        UE_LOG(LogTemp, Error,
            TEXT("V3DSimulatorGameInstance AssetRegistryClass is invalid or cannot be instantiated. Class=%s"),
            *GetNameSafe(RegistryClass));
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
    if (!IsValid(WorldContextObject))
    {
        return nullptr;
    }
    const UWorld* World = WorldContextObject->GetWorld();
    UGameInstance* GameInstance = World ? World->GetGameInstance() : Cast<UGameInstance>(const_cast<UObject*>(WorldContextObject));
    if (UV3DSimulatorGameInstance* SimulatorGameInstance = Cast<UV3DSimulatorGameInstance>(GameInstance))
    {
        // Init normally creates this before any world starts. The fallback makes direct/editor world
        // startup robust if a custom lifecycle calls into the registry unusually early.
        SimulatorGameInstance->EnsureAssetRegistry();
        return SimulatorGameInstance->GetAssetRegistry();
    }
    return nullptr;
}
