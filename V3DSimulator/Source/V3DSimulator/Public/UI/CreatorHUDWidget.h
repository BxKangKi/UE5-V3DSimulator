// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file CreatorHUDWidget.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "UI/ResponsiveHUDWidget.h"
#include "System/GameManagerSubSystem.h"
#include "CreatorHUDWidget.generated.h"

class UBorder;
class UScrollBox;
class UTextBlock;
class UUniformGridPanel;
class UGameUpdateSubSystem;
class UButton;
class UMenuButtonWidget;
class USizeBox;

/**
 * Complete native creator HUD, with a scaled reference canvas and a bounded item browser.
 * Blueprint children immediately preview the native layout; explicit setters remain available.
 */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API UCreatorHUDWidget : public UResponsiveHUDWidget
{
    GENERATED_BODY()

public:
    UCreatorHUDWidget();

    /** Optional diagnostics; the gameplay view shows only the compact toolbar by default. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Creator HUD|Layout")
    bool bShowStatusDetails = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Creator HUD|Layout")
    FVector2D StatusPanelSize = FVector2D(460, 320);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Creator HUD|Layout")
    FVector2D ActionsPanelSize = FVector2D(264, 280);

    /** Legacy serialized size. The native toolbar now sizes itself to its seven square slots. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Creator HUD|Layout")
    FVector2D ToolbarSize = FVector2D(432, 72);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Creator HUD|Layout")
    FVector2D StatusPanelOffset = FVector2D(24, 24);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Creator HUD|Layout")
    FVector2D ActionsPanelOffset = FVector2D(-24, 24);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Creator HUD|Layout")
    FVector2D ToolbarOffset = FVector2D(0, -16);

    /** Width and height of each square slot in reference-canvas pixels. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Creator HUD|Layout", meta=(ClampMin="32", ClampMax="96"))
    float ToolbarSlotHeight = 54.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Creator HUD|Layout", meta=(ClampMin="1", ClampMax="8"))
    int32 ItemColumns = 4;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Creator HUD|Layout", meta=(ClampMin="8", ClampMax="128"))
    int32 ItemsPerPage = 32;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Creator HUD|Performance", meta=(ClampMin="0.05", ClampMax="1.0"))
    float PlacementTextRefreshInterval = 0.1f;

    /** Explicitly assigns the toolbar grid owned by the WBP. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Widgets")
    void SetToolbarGrid(UUniformGridPanel* InToolbarGrid);

    /** Explicitly assigns the item-list scroll box owned by the WBP. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Widgets")
    void SetItemListScrollBox(UScrollBox* InItemListScrollBox);

    /** Explicitly assigns the item-list panel owned by the WBP. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Widgets")
    void SetItemListPanel(UBorder* InItemListPanel);

    /** Explicitly assigns the status text block owned by the WBP. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Widgets")
    void SetStatusText(UTextBlock* InStatusText);

    /** Explicitly assigns the placement-info text block owned by the WBP. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Widgets")
    void SetPlacementText(UTextBlock* InPlacementText);

    /** Explicitly assigns the message text block owned by the WBP. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Widgets")
    void SetMessageText(UTextBlock* InMessageText);

    /** Re-finds the GameManager used by this HUD and refreshes all displayed values. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD")
    void RefreshManagerReference();

    /** Updates the native toolbar while preserving custom setter-based layouts. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD")
    void RefreshToolbar();

    /** Updates the current browser page; hidden pages do not create widgets. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD")
    void RefreshItemList();

    /** Immediately refreshes status, coordinate, and snap text. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD")
    void RefreshStatus();

    /** Allows external Blueprints to select a specific toolbar slot manually. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Toolbar")
    void SelectToolbarSlotFromUI(int32 SlotIndex);

    /** Allows external Blueprints to select an item from the full item list manually. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Inventory")
    void SelectAvailableItemFromUI(int32 AvailableItemIndex);

    /** Called by UI buttons to open or close the full item-list window. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Inventory")
    void ToggleItemListFromUI();

    /** Called by UI buttons to save the current scene. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Save")
    void SaveSceneFromUI();

    /** Starts the DAT-only world bake and can be mapped directly to a world-list UI button. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Bake")
    void BuildActiveProjectArchiveFromUI();

    /** Called by UI buttons to toggle grid snapping. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Placement")
    void ToggleSnapFromUI();

protected:
    virtual void BuildNativeLayout() override;
    virtual void BindNativeWidgets() override;
    virtual void ApplyNativeStyle() override;

    /** Binds manager events after the widget is constructed. */
    virtual void NativeConstruct() override;

    /** Unbinds manager events when the widget is removed. */
    virtual void NativeDestruct() override;

private:
    UFUNCTION() void HandleNativeAction(const FString& Key);
    UFUNCTION() void PreviousItemPage();
    UFUNCTION() void NextItemPage();
    void BuildToolbarButtons();
    void ClearNativeInventory();
    float GetToolbarSlotExtent() const;
    void ApplyStatusVisibility();
    void UpdatePlacementRegistration();

    UPROPERTY(Transient) TObjectPtr<UUniformGridPanel> NativeToolbar;
    UPROPERTY(Transient) TObjectPtr<UBorder> NativeStatusPanel;
    UPROPERTY(Transient) TObjectPtr<UBorder> NativeToolbarPanel;
    UPROPERTY(Transient) TObjectPtr<UUniformGridPanel> NativeItemGrid;
    UPROPERTY(Transient) TObjectPtr<UScrollBox> NativeItemsScroll;
    UPROPERTY(Transient) TObjectPtr<UBorder> NativeItemsPanel;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> NativeStatus;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> NativePlacement;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> NativeMessage;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> NativePageText;
    UPROPERTY(Transient) TObjectPtr<UButton> NativePreviousPage;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeNextPage;
    UPROPERTY(Transient) TArray<TObjectPtr<UMenuButtonWidget>> NativeToolbarButtons;
    UPROPERTY(Transient) TArray<TObjectPtr<UMenuButtonWidget>> NativeInventoryButtons;
    UPROPERTY(Transient) TArray<TObjectPtr<USizeBox>> NativeToolbarSizes;
    TArray<FString> VisibleInventorySignature;
    int32 CurrentItemPage = 0;
    float PlacementRefreshElapsed = 0.f;
    bool bHUDConstructed = false;

    int32 GameUpdateTickHandle = INDEX_NONE;
    void UpdateFromGameUpdate(float DeltaSeconds);

    /** Bridges GameManager state changes to HUD refreshes. */
    UFUNCTION()
    void HandleStateChanged();

    /** Bridges GameManager message changes to HUD refreshes. */
    UFUNCTION()
    void HandleMessageChanged(const FString& Message);

    /** Bridges GameManager toolbar changes to HUD refreshes. */
    UFUNCTION()
    void HandleToolbarChanged();

    /** Bridges GameManager item-list window changes to HUD refreshes. */
    UFUNCTION()
    void HandleItemListWindowChanged(bool bOpen);

    /** Returns the status panel text. */
    FText GetStatusText() const;

    /** Returns the coordinate, snap, and validity panel text. */
    FText GetPlacementInfoText() const;

    /** Returns the last gameplay message text. */
    FText GetMessageText() const;

    /** Finds and caches the manager pointer when it has not been resolved yet. */
    UGameManagerSubSystem* GetGameManager() const;

    /** Binds manager events without duplicate bindings. */
    void BindManagerEvents();

    /** Safely removes manager event bindings. */
    void UnbindManagerEvents();

    TWeakObjectPtr<UUniformGridPanel> AssignedToolbarGrid;
    TWeakObjectPtr<UScrollBox> AssignedItemListScrollBox;
    TWeakObjectPtr<UBorder> AssignedItemListPanel;
    TWeakObjectPtr<UTextBlock> AssignedStatusText;
    TWeakObjectPtr<UTextBlock> AssignedPlacementText;
    TWeakObjectPtr<UTextBlock> AssignedMessageText;

    /** GameManager currently connected to this HUD. */
    UPROPERTY(Transient)
    mutable TObjectPtr<UGameManagerSubSystem> CachedGameManager;

    /** Tracks whether events are already bound so duplicate bindings are avoided. */
    bool bGameplayEventsBound = false;
};
