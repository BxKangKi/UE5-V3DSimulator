// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NativeWidgetDesignerLibrary.generated.h"

class UWidgetBlueprint;

/** Editor-only bridge between a native HUD layout and the Widget Blueprint Hierarchy panel. */
UCLASS()
class V3DSIMULATOREDITOR_API UNativeWidgetDesignerLibrary final : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /**
     * Materialize the native layout as serialized designer templates. Class Defaults remain the
     * source of native appearance/layout settings. Custom bUseNativeLayout=false trees are retained.
     * The shipped generic WBP_Debug/WBP_Loading assets also receive their native parent on first use.
     * Returns true only when the asset tree changed; leaves saving to the normal editor workflow.
     */
    UFUNCTION(BlueprintCallable, Category="V3DSimulator|Editor|UI")
    static bool RefreshNativeWidgetHierarchy(UWidgetBlueprint* WidgetBlueprint, bool bForce = false);

    /** Refresh the native HUD assets currently open in editors, without scanning/loading assets. */
    UFUNCTION(BlueprintCallable, Category="V3DSimulator|Editor|UI")
    static int32 RefreshOpenNativeWidgetHierarchies(bool bForce = true);

    UFUNCTION(BlueprintPure, Category="V3DSimulator|Editor|UI")
    static bool UsesNativeWidgetLayout(const UWidgetBlueprint* WidgetBlueprint);
};
