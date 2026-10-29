// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ResponsiveHUDWidget.generated.h"

class UBorder;
class UButton;
class UCanvasPanel;
class UOverlay;
class UPanelWidget;
class UScaleBox;
class USizeBox;
class UTextBlock;

/** Native HUD with a viewport-filling background and uniformly scaled reference-space controls. */
UCLASS(Abstract, Blueprintable, BlueprintType)
class V3DSIMULATOR_API UResponsiveHUDWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    UResponsiveHUDWidget(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    /** Enabled by default, including for existing Blueprint children. Disable to use a custom WBP tree. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Layout")
    bool bUseNativeLayout = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Layout", meta=(ClampMin="320", ClampMax="16384"))
    FVector2D DesignResolution = FVector2D(1920.0, 1080.0);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Layout", meta=(ClampMin="100", ClampMax="16384"))
    FVector2D PanelSize = FVector2D(1920.0, 1080.0);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Layout", meta=(ClampMin="0", ClampMax="200"))
    float PanelPadding = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Layout", meta=(ClampMin="20", ClampMax="200"))
    float ButtonHeight = 64.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Appearance", meta=(ClampMin="8", ClampMax="96"))
    int32 BodyFontSize = 24;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Appearance", meta=(ClampMin="8", ClampMax="144"))
    int32 TitleFontSize = 100;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Appearance")
    TObjectPtr<UObject> FontObject;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Appearance", meta=(ClampMin="0", ClampMax="8"))
    int32 FontOutlineSize = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Appearance")
    FLinearColor BackgroundColor = FLinearColor(.830770f, .830770f, .830770f, 1.f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Appearance")
    FLinearColor PanelColor = FLinearColor::Transparent;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Appearance")
    FLinearColor ButtonColor = FLinearColor(.318547f, .391573f, .462077f, 1.f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Appearance")
    FLinearColor HoveredButtonTint = FLinearColor(.8f, .8f, .8f, 1.f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Appearance")
    FLinearColor AccentColor = FLinearColor(.318547f, .391573f, .462077f, 1.f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Appearance")
    FLinearColor TextColor = FLinearColor::White;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Appearance")
    FLinearColor MutedTextColor = FLinearColor(.9f, .9f, .9f, 1.f);

    UFUNCTION(BlueprintPure, Category="HUD|Layout")
    bool IsUsingNativeLayout() const;

    UFUNCTION(BlueprintPure, Category="HUD|Layout")
    FVector2D GetSafeDesignResolution() const;

    /** Reapply defaults changed by Blueprint at runtime. No world queries or layout allocations. */
    UFUNCTION(BlueprintCallable, Category="HUD|Appearance")
    void RefreshAppearance();

    virtual void SynchronizeProperties() override;

#if WITH_EDITOR
    /** Editor-only template generation: no game world, viewport, or Slate allocation is needed. */
    void PrepareNativeDesignerLayout();
#endif

protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativePreConstruct() override;
    virtual void NativeConstruct() override;

    /** Construct child controls here. Called once per new widget tree, before Slate construction. */
    virtual void BuildNativeLayout() {}
    /** Restore native assignments after legacy Blueprint PreConstruct/Construct setter calls. */
    virtual void BindNativeWidgets() {}
    /** Cosmetic only: may execute in the Blueprint designer without a world or player. */
    virtual void ApplyNativeStyle();

    /** Reject references to obsolete WBP controls after replacing a legacy designer tree. */
    bool AcceptWidgetAssignment(const UWidget* Widget) const;

    UBorder* AddPanel(FVector2D Size, FVector2D Position,
        FVector2D Anchors = FVector2D(0.5, 0.5), FVector2D Alignment = FVector2D(0.5, 0.5));
    UTextBlock* AddText(UPanelWidget* Parent, const FText& Text, bool bTitle = false);
    UButton* AddButton(UPanelWidget* Parent, const FText& Text);

    UPROPERTY(Transient)
    TObjectPtr<UCanvasPanel> NativeLayout;

private:
    void EnsureNativeLayout();

    UPROPERTY(Transient) TObjectPtr<UOverlay> NativeViewportRoot;
    UPROPERTY(Transient) TObjectPtr<UScaleBox> NativeScaleRoot;
    UPROPERTY(Transient) TObjectPtr<USizeBox> NativeReferenceSize;
    UPROPERTY(Transient) TObjectPtr<UBorder> NativeBackground;
    UPROPERTY(Transient) TArray<TObjectPtr<UBorder>> StyledPanels;
    UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> StyledTexts;
    UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> StyledTitles;
    UPROPERTY(Transient) TArray<TObjectPtr<UButton>> StyledButtons;
    UPROPERTY(Transient) TArray<TObjectPtr<USizeBox>> StyledButtonSizes;
    UPROPERTY(Transient) TArray<TObjectPtr<UBorder>> PositionedPanels;
};
