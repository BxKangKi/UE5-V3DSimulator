// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file MenuButtonWidget.h
 * Reusable Blueprint-facing button widget base used by generated menu entries.
 */

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MenuButtonWidget.generated.h"

class UButton;
class UTextBlock;
class USizeBox;
class UScaleBox;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMenuButtonClicked, const FString&, ButtonKey);

/**
 * Code-defined menu button, visible in a derived WBP's designer without manual UMG construction.
 * Appearance and minimum size are editable in class defaults. ConfigureButton assigns the stable
 * action key and displayed label. Disable bUseNativeLayout to assign custom WBP controls instead.
 */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API UMenuButtonWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    UMenuButtonWidget(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void SynchronizeProperties() override;
    virtual void NativePreConstruct() override;
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Layout")
    bool bUseNativeLayout = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Layout")
    FVector2D ButtonMinSize = FVector2D(200.f, 64.f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Layout")
    FMargin ContentPadding = FMargin(0.f, 8.f, 0.f, 8.f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style", meta=(AllowedClasses="/Script/Engine.Font"))
    TObjectPtr<UObject> FontObject;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style", meta=(ClampMin="0", ClampMax="8"))
    int32 FontOutlineSize = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style", meta=(ClampMin="8", ClampMax="96"))
    int32 FontSize = 32;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style")
    FLinearColor TextColor = FLinearColor::White;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style")
    FLinearColor BackgroundColor = FLinearColor(.318547f, .391573f, .462077f, 1.f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style")
    FLinearColor AccentColor = FLinearColor(.8f, .8f, .8f, 1.f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Preview")
    FText PreviewLabel = NSLOCTEXT("V3DUI", "ButtonPreview", "Button");

    UFUNCTION(BlueprintCallable, Category="Menu Button|Style")
    void ApplyButtonTheme(FLinearColor InTextColor, FLinearColor InBackgroundColor,
        FLinearColor InAccentColor, int32 InFontSize, float InMinHeight);

    UPROPERTY(BlueprintAssignable, Category="Menu Button")
    FMenuButtonClicked OnButtonClicked;

    /** Assigns the actual clickable UButton contained by this WBP. */
    UFUNCTION(BlueprintCallable, Category="Menu Button|Widgets")
    void SetButton(UButton* InButton);

    /** Optional label. When assigned, ConfigureButton() writes DisplayLabel into it automatically. */
    UFUNCTION(BlueprintCallable, Category="Menu Button|Widgets")
    void SetLabelTextBlock(UTextBlock* InTextBlock);

    /** Supplies the stable action key and the text intended for this WBP instance. */
    UFUNCTION(BlueprintCallable, Category="Menu Button")
    void ConfigureButton(const FString& InButtonKey, const FString& InDisplayLabel);

    /** Allows a custom Blueprint interaction to emit the same click event without a UButton. */
    UFUNCTION(BlueprintCallable, Category="Menu Button")
    void TriggerClick();

    UFUNCTION(BlueprintPure, Category="Menu Button")
    FString GetButtonKey() const { return ButtonKey; }

    UFUNCTION(BlueprintPure, Category="Menu Button")
    FString GetDisplayLabel() const { return DisplayLabel; }

    /** Called after ConfigureButton so the WBP can update custom text/icons/layout. */
    UFUNCTION(BlueprintImplementableEvent, Category="Menu Button", meta=(DisplayName="On Button Configured"))
    void BP_OnButtonConfigured(const FString& InButtonKey, const FString& InDisplayLabel);

private:
    void BindNativeControls();
    void ApplyNativeStyle();
    UPROPERTY(Transient) TObjectPtr<UScaleBox> NativeScaleRoot;
    UPROPERTY(Transient) TObjectPtr<USizeBox> NativeRoot;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeButton;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> NativeLabel;

    UFUNCTION()
    void HandleNativeButtonClicked();

    TWeakObjectPtr<UButton> AssignedButton;
    TWeakObjectPtr<UTextBlock> AssignedLabel;

    FString ButtonKey;
    FString DisplayLabel;
};
