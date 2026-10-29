// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "V3DSimulatorGameInstance.generated.h"

class UV3DSimulatorAssetRegistry;

/**
 * Project GameInstance. Assign only AssetRegistryClass in the GameInstance defaults.
 * A transient registry object is created internally from that class; no registry instance is exposed
 * as project configuration. Heavy entries inside the registry remain soft references.
 */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API UV3DSimulatorGameInstance : public UGameInstance
{
    GENERATED_BODY()

public:
    virtual void Init() override;

    /** Ensures the private runtime registry instance exists. Safe to call repeatedly from GameMode startup. */
    bool EnsureAssetRegistry();

    UFUNCTION(BlueprintPure, Category="V3DSimulator|Assets")
    UV3DSimulatorAssetRegistry* GetAssetRegistry() const;

    UFUNCTION(BlueprintPure, Category="V3DSimulator|Assets", meta=(WorldContext="WorldContextObject"))
    static UV3DSimulatorAssetRegistry* GetAssetRegistryFromContext(const UObject* WorldContextObject);

protected:
    /**
     * The only project-facing registry setting. Assign a Blueprint subclass of
     * UV3DSimulatorAssetRegistry in the GameInstance defaults.
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="V3DSimulator|Assets")
    TSoftClassPtr<UV3DSimulatorAssetRegistry> AssetRegistryClass;

private:
    /** Private runtime instance created from AssetRegistryClass. Never exposed as an editable project setting. */
    UPROPERTY(Transient)
    TObjectPtr<UV3DSimulatorAssetRegistry> RuntimeAssetRegistry;
};
