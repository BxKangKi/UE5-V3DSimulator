// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file V3DSimulatorEditorServices.h
 * Runtime-facing optional bridge to services implemented by the V3DSimulatorEditor module.
 *
 * The runtime module deliberately owns only this dependency-free interface. Packaged/game builds
 * simply have no registered implementation, so requests become cheap no-ops without linking UnrealEd.
 */

#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"
#include "Features/IModularFeatures.h"

class UPhysicsAsset;

class V3DSIMULATOR_API IV3DSimulatorEditorServices : public IModularFeature
{
public:
    virtual ~IV3DSimulatorEditorServices() = default;

    static FName GetModularFeatureName()
    {
        static const FName FeatureName(TEXT("V3DSimulatorEditorServices"));
        return FeatureName;
    }

    /** Rebuilds editor-side PhysicsAsset derived/cooked state after runtime asset mutation. */
    virtual void RefreshPhysicsAsset(UPhysicsAsset* PhysicsAsset) = 0;
};

/**
 * Small runtime helper that resolves the optional editor implementation through ModularFeatures.
 * No editor module header, UnrealEd type, or editor-only symbol is referenced by the game module.
 */
class V3DSIMULATOR_API FV3DSimulatorEditorServices
{
public:
    static IV3DSimulatorEditorServices* Get()
    {
        IModularFeatures& Features = IModularFeatures::Get();
        const FName FeatureName = IV3DSimulatorEditorServices::GetModularFeatureName();
        if (!Features.IsModularFeatureAvailable(FeatureName))
        {
            return nullptr;
        }
        return &Features.GetModularFeature<IV3DSimulatorEditorServices>(FeatureName);
    }

    static void RefreshPhysicsAsset(UPhysicsAsset* PhysicsAsset)
    {
        if (IV3DSimulatorEditorServices* Services = Get())
        {
            Services->RefreshPhysicsAsset(PhysicsAsset);
        }
    }
};
