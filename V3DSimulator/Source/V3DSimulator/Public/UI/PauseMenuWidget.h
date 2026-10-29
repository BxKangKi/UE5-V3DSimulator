// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file PauseMenuWidget.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "UI/ResponsiveHUDWidget.h"
#include "PauseMenuWidget.generated.h"

class UButton;
class UTextBlock;

/**
 * Blueprint-editable pause menu base widget.
 *
 * Supplies its visual tree in C++, including the Blueprint designer preview. Class Defaults expose
 * resolution, sizes and colours. Explicit setters remain available for custom Blueprint layouts.
 */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API UPauseMenuWidget : public UResponsiveHUDWidget
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Appearance")
    FLinearColor PauseAccentColor = FLinearColor(.1f, .3f, .7f, .3f);

    UPauseMenuWidget(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    UFUNCTION(BlueprintCallable, Category="Pause|Widgets")
    void SetTitleText(UTextBlock* InTitleText);

    UFUNCTION(BlueprintCallable, Category="Pause|Widgets")
    void SetContinueButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Pause|Widgets")
    void SetSettingsButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Pause|Widgets")
    void SetExitButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Pause")
    void ContinueFromUI();

    UFUNCTION(BlueprintCallable, Category="Pause")
    void OpenSettingsFromUI();

    UFUNCTION(BlueprintCallable, Category="Pause")
    void ExitFromUI();

    /** Re-enables Exit after a rejected/failed level-travel request restores this cached widget. */
    void ResetExitRequestState();

protected:
    virtual void BuildNativeLayout() override;
    virtual void ApplyNativeStyle() override;
    virtual void BindNativeWidgets() override;

private:
    void BindButtonEvents();
    void UnbindButtonEvents();

    TWeakObjectPtr<UTextBlock> AssignedTitleText;
    TWeakObjectPtr<UButton> AssignedContinueButton;
    TWeakObjectPtr<UButton> AssignedSettingsButton;
    TWeakObjectPtr<UButton> AssignedExitButton;

    UPROPERTY(Transient) TObjectPtr<UBorder> NativePanel;
    UPROPERTY(Transient) TObjectPtr<UBorder> NativePauseAccent;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> NativeTitle;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeContinue;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeSettings;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeExit;

    /** Prevents one click or duplicate Blueprint bindings from issuing more than one level travel. */
    bool bExitRequestInProgress = false;
};
