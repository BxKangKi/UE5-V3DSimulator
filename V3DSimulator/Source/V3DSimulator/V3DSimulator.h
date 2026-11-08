// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file V3DSimulator.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * Primary game module.
 *
 * Besides draining tracked runtime work during shutdown, the module prepares a pure-Slate
 * MoviePlayer screen for blocking game map loads outside the editor. PIE retains normal UMG
 * loading UI. File checkpoints are registered in both paths so a stalled editor leaves evidence.
 */
class FV3DSimulatorModule final : public FDefaultGameModuleImpl
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    /** Prepares the lightweight screen for a real map-load request. */
    void PrepareLoadingScreen();

    /** Prepares the screen before a blocking map load starts. */
    void HandlePreLoadMap(const FString& MapName);
    void HandlePostLoadMap(class UWorld* World);
    FDelegateHandle PostLoadMapHandle;

    /** Delegate handles are removed before module shutdown to prevent callbacks into unloaded code. */
    bool bOwnsLoadingScreen = false;
    FDelegateHandle PreLoadMapHandle;
};
