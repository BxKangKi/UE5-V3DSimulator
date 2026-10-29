// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file SettingControlWidget.h
 * Reusable Blueprint-authored settings row widgets selected by setting value type.
 */

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/SettingsFieldTypes.h"
#include "SettingControlWidget.generated.h"

class UButton;
class UComboBoxString;
class USlider;
class UTextBlock;
class USettingsMenuWidget;
class USizeBox;
class UScaleBox;
class UPanelWidget;
class UWidget;

/**
 * Base contract for a generated setting row WBP.
 *
 * Native row layouts are available in Blueprint children, including designer previews.
 * Class defaults control appearance; bUseNativeLayout=false enables custom WBP layouts.
 */
UCLASS(Abstract, Blueprintable, BlueprintType)
class V3DSIMULATOR_API USettingControlWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    USettingControlWidget(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    virtual void SynchronizeProperties() override;
    virtual void NativePreConstruct() override;
    virtual void ReleaseSlateResources(bool bReleaseChildren) override;

    UFUNCTION(BlueprintCallable, Category="Settings Control")
    virtual void ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField);

    UFUNCTION(BlueprintCallable, Category="Settings Control")
    virtual void RefreshSettingWidget();

    UFUNCTION(BlueprintPure, Category="Settings Control")
    ESettingsField GetSettingField() const { return SettingField; }

    UFUNCTION(BlueprintPure, Category="Settings Control")
    FText GetSettingLabel() const;

    UFUNCTION(BlueprintPure, Category="Settings Control")
    FText GetSettingValueText() const;

    /** Applies the parent menu palette while retaining each control's original text proportions. */
    UFUNCTION(BlueprintCallable, Category="Settings Control|Style")
    void ApplyControlTheme(UObject* InFontObject, int32 InOutlineSize, FLinearColor InTextColor,
        FLinearColor InPanelColor, FLinearColor InAccentColor, float InMinHeight);

protected:
    /** Native rows are visible in derived WBP designers; disable to use a custom WBP tree. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Layout")
    bool bUseNativeLayout = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Layout", meta=(ClampMin="32", ClampMax="256"))
    float RowMinHeight = 80.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Layout", meta=(ClampMin="1", ClampMax="16384"))
    float RowMinWidth = 240.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style", meta=(AllowedClasses="/Script/Engine.Font"))
    TObjectPtr<UObject> FontObject;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style", meta=(ClampMin="0", ClampMax="8"))
    int32 FontOutlineSize = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style", meta=(ClampMin="8", ClampMax="96"))
    int32 FontSize = 24;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style", meta=(ClampMin="8", ClampMax="96"))
    int32 ValueFontSize = 24;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style")
    FLinearColor TextColor = FLinearColor::White;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style")
    FLinearColor AccentColor = FLinearColor(.8f, .8f, .8f, 1.f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style")
    FLinearColor PanelColor = FLinearColor(.318547f, .391573f, .462077f, 1.f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Preview")
    FText PreviewLabel = NSLOCTEXT("V3DUI", "SettingPreview", "Setting");

    UFUNCTION() UWidget* GenerateNativeOption(FString Option);
    UPROPERTY(Transient) TArray<TWeakObjectPtr<UTextBlock>> NativeOptionLabels;

    virtual void BindNativeRowWidgets();
    void BeginNativeRow();
    bool NeedsNativeRow() const;
    virtual void ApplyNativeRowStyle();

    UPROPERTY(Transient) TObjectPtr<UScaleBox> NativeScaleRoot;
    UPROPERTY(Transient) TObjectPtr<USizeBox> NativeRoot;
    UPROPERTY(Transient) TObjectPtr<UPanelWidget> NativeRow;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> NativeLabel;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> NativeValue;
    UPROPERTY(Transient) TObjectPtr<UWidget> NativeControl;

    UPROPERTY(BlueprintReadOnly, Transient, Category="Settings Control")
    TObjectPtr<USettingsMenuWidget> OwnerMenu;

    UPROPERTY(BlueprintReadOnly, Transient, Category="Settings Control")
    ESettingsField SettingField = ESettingsField::BloomIntensity;
};

/** Boolean setting row. The WBP usually contains a styled UButton or custom toggle surface. */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API UBooleanSettingWidget : public USettingControlWidget
{
    GENERATED_BODY()

public:
    UBooleanSettingWidget(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual void ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField) override;
    virtual void RefreshSettingWidget() override;

    UFUNCTION(BlueprintCallable, Category="Settings Control|Boolean")
    void SetToggleButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Settings Control|Boolean")
    void SetLabelTextBlock(UTextBlock* InTextBlock);

    UFUNCTION(BlueprintCallable, Category="Settings Control|Boolean")
    void SetValueTextBlock(UTextBlock* InTextBlock);

    /** Allows a fully custom WBP interaction to toggle without assigning a native UButton. */
    UFUNCTION(BlueprintCallable, Category="Settings Control|Boolean")
    void ToggleValue();

    UFUNCTION(BlueprintPure, Category="Settings Control|Boolean")
    bool GetBooleanValue() const;

    UFUNCTION(BlueprintImplementableEvent, Category="Settings Control|Boolean", meta=(DisplayName="On Boolean Setting Updated"))
    void BP_OnBooleanSettingUpdated(const FText& Label, bool bValue, const FText& ValueText);

protected:
    virtual void BindNativeRowWidgets() override;

private:
    UFUNCTION()
    void HandleToggleClicked();

    TWeakObjectPtr<UButton> AssignedToggleButton;
    TWeakObjectPtr<UTextBlock> AssignedLabelText;
    TWeakObjectPtr<UTextBlock> AssignedValueText;
};

/** Numeric ranged setting row. The WBP normally contains a styled Slider/progress-bar-like control. */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API UFloatSettingWidget : public USettingControlWidget
{
    GENERATED_BODY()

public:
    UFloatSettingWidget(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style")
    FLinearColor SliderBarColor = FLinearColor(.33f, .33f, .33f, 1.f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style")
    FLinearColor SliderHoveredBarColor = FLinearColor(.5f, .5f, .5f, 1.f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Style")
    FLinearColor SliderHandleColor = FLinearColor::White;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Layout", meta=(ClampMin="1", ClampMax="128"))
    float SliderBarThickness = 24.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Layout")
    FVector2D SliderThumbSize = FVector2D(16.f, 32.f);

    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual void ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField) override;
    virtual void RefreshSettingWidget() override;

    UFUNCTION(BlueprintCallable, Category="Settings Control|Float")
    void SetSlider(USlider* InSlider);

    UFUNCTION(BlueprintCallable, Category="Settings Control|Float")
    void SetLabelTextBlock(UTextBlock* InTextBlock);

    UFUNCTION(BlueprintCallable, Category="Settings Control|Float")
    void SetValueTextBlock(UTextBlock* InTextBlock);

    /** Allows a custom Blueprint bar/control to push a numeric value directly. */
    UFUNCTION(BlueprintCallable, Category="Settings Control|Float")
    void SetNumericValue(float Value);

    UFUNCTION(BlueprintPure, Category="Settings Control|Float")
    float GetNumericValue() const;

    UFUNCTION(BlueprintPure, Category="Settings Control|Float")
    bool GetNumericRange(float& OutMin, float& OutMax, float& OutStep) const;

    UFUNCTION(BlueprintImplementableEvent, Category="Settings Control|Float", meta=(DisplayName="On Float Setting Updated"))
    void BP_OnFloatSettingUpdated(const FText& Label, float Value, float MinValue, float MaxValue, float Step, const FText& ValueText);

protected:
    virtual void BindNativeRowWidgets() override;
    virtual void ApplyNativeRowStyle() override;

private:
    UFUNCTION()
    void HandleSliderValueChanged(float Value);

    TWeakObjectPtr<USlider> AssignedSlider;
    TWeakObjectPtr<UTextBlock> AssignedLabelText;
    TWeakObjectPtr<UTextBlock> AssignedValueText;
    bool bInternalRefresh = false;
};

/** Discrete option setting row. Int-backed qualities and enum-like fields use this WBP. */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API UEnumSettingWidget : public USettingControlWidget
{
    GENERATED_BODY()

public:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual void ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField) override;
    virtual void RefreshSettingWidget() override;

    UFUNCTION(BlueprintCallable, Category="Settings Control|Enum")
    void SetDropdown(UComboBoxString* InDropdown);

    UFUNCTION(BlueprintCallable, Category="Settings Control|Enum")
    void SetLabelTextBlock(UTextBlock* InTextBlock);

    UFUNCTION(BlueprintCallable, Category="Settings Control|Enum")
    void SetValueTextBlock(UTextBlock* InTextBlock);

    /** Allows a custom dropdown WBP to submit an option string without assigning ComboBoxString. */
    UFUNCTION(BlueprintCallable, Category="Settings Control|Enum")
    void SelectOption(const FString& SelectedOption);

    UFUNCTION(BlueprintPure, Category="Settings Control|Enum")
    TArray<FText> GetOptions() const;

    UFUNCTION(BlueprintImplementableEvent, Category="Settings Control|Enum", meta=(DisplayName="On Enum Setting Updated"))
    void BP_OnEnumSettingUpdated(const FText& Label, const TArray<FText>& Options, const FText& SelectedValue);

protected:
    virtual void BindNativeRowWidgets() override;

private:
    UFUNCTION()
    void HandleSelectionChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

    void RebuildDropdownOptions();

    TWeakObjectPtr<UComboBoxString> AssignedDropdown;
    TWeakObjectPtr<UTextBlock> AssignedLabelText;
    TWeakObjectPtr<UTextBlock> AssignedValueText;
    bool bInternalRefresh = false;
};
