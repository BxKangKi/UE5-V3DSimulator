// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"

class UWidgetTree;
class UUserWidget;

namespace V3DNativeWidgetTree
{
    /** Stable names link the Blueprint asset's Hierarchy nodes to regenerated preview controls. */
    V3DSIMULATOR_API void StabilizeNames(UWidgetTree* Tree);

    /** Reconnect generated Blueprint widget variables/events once, after creating a fresh tree. */
    V3DSIMULATOR_API void BindBlueprintWidgets(UUserWidget* Owner);
}
