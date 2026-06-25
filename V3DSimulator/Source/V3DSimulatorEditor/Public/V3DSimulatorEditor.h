// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file V3DSimulatorEditor.h
 * Primary editor module for V3DSimulator.
 *
 * UnrealEd-only behavior and editor automation tests live in this module. The
 * runtime module communicates through IV3DSimulatorEditorServices and never links UnrealEd.
 */

#pragma once

#include "CoreMinimal.h"
#include "Editor/V3DSimulatorEditorServices.h"
#include "Modules/ModuleManager.h"

class UPhysicsAsset;
struct FWorldContext;

class V3DSIMULATOREDITOR_API FV3DSimulatorEditorModule final
    : public IModuleInterface
    , public IV3DSimulatorEditorServices
{
public:
    static FV3DSimulatorEditorModule& Get();
    static bool IsAvailable();

    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    // IV3DSimulatorEditorServices
    virtual void RefreshPhysicsAsset(UPhysicsAsset* PhysicsAsset) override;

private:
    /** Clears editor undo references only for loads belonging to a PIE world context. */
    void HandlePreLoadMap(const FWorldContext& WorldContext, const FString& MapName);

    bool bEditorServicesRegistered = false;
    FDelegateHandle PreLoadMapHandle;
};
