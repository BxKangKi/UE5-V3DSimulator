// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file SelectionWidgetBase.h
 * Shared generated-selection behavior used by Project and World selection menus.
 */

#pragma once

#include "CoreMinimal.h"
#include "UI/ResponsiveHUDWidget.h"
#include "SelectionWidgetBase.generated.h"

class UMenuButtonWidget;
class UPanelWidget;
class UWidget;

/**
 * Base class for menus that generate selectable entries at runtime.
 *
 * Entries use the native UMenuButtonWidget class. Screen layout
 * and shared appearance are supplied by UResponsiveHUDWidget and remain editable in Class Defaults.
 */
UCLASS(Abstract, Blueprintable, BlueprintType)
class V3DSIMULATOR_API USelectionWidgetBase : public UResponsiveHUDWidget
{
    GENERATED_BODY()

public:
    virtual void NativeDestruct() override;

    /** Internal click receiver used by generated UMenuButtonWidget instances. */
    UFUNCTION()
    void HandleSelectionButtonClicked(const FString& SelectionKey);

protected:
    virtual void ApplyNativeStyle() override;

    /** Assigns the panel that owns generated entries. Existing generated entries are removed. */
    void SetSelectionListPanel(UPanelWidget* InPanel);

    UPanelWidget* GetSelectionListPanel() const { return SelectionListPanel.Get(); }

    /** Creates, configures, binds, and adds one native entry to the assigned panel. */
    UWidget* AddGeneratedSelectionEntry(const FString& SelectionKey, const FString& DisplayName);

    /** Removes generated entries without touching Blueprint-authored children. */
    void ClearGeneratedSelectionEntries();

    /** Domain action implemented by derived selection widgets. */
    virtual void OnSelectionEntryActivated(const FString& SelectionKey);

private:
    TWeakObjectPtr<UPanelWidget> SelectionListPanel;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UMenuButtonWidget>> GeneratedSelectionEntries;
};
