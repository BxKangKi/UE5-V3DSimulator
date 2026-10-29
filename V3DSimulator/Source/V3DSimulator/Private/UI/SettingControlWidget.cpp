// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/SettingControlWidget.h"
#include "UI/NativeWidgetTreeNames.h"
#include "UI/HUDLegacyStyle.h"
#include "UI/WidgetStyleSafety.h"
#include "UI/SettingsMenuWidget.h"

#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/ComboBoxString.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Blueprint/WidgetTree.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/ScaleBox.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Widget.h"
#include "Brushes/SlateColorBrush.h"

namespace
{
    void ApplySettingRowComboTheme(UComboBoxString* Dropdown, FLinearColor Panel, FLinearColor Accent, FLinearColor Text)
    {
        FComboBoxStyle Style = Dropdown->GetWidgetStyle();
        Style.ComboButtonStyle.ButtonStyle.Normal = FSlateColorBrush(Panel);
        Style.ComboButtonStyle.ButtonStyle.Hovered = FSlateColorBrush(Accent);
        Style.ComboButtonStyle.ButtonStyle.Pressed = FSlateColorBrush(
            FLinearColor(Panel.R * .5f, Panel.G * .5f, Panel.B * .5f, Panel.A));
        Style.ComboButtonStyle.DownArrowImage.ImageSize = FVector2D(32.f, 32.f);
        Style.ComboButtonStyle.DownArrowImage.TintColor = FSlateColor(Text);
        Style.ComboButtonStyle.MenuBorderPadding = FMargin(5.f);
        Style.MenuRowPadding = FMargin(4.f);
        V3DWidgetStyle::SetOwnedStyle(*Dropdown, Style);
        FTableRowStyle Item = Dropdown->GetItemStyle();
        Item.TextColor = FSlateColor(Text);
        Item.SelectedTextColor = FSlateColor(Text);
        V3DWidgetStyle::SetOwnedItemStyle(*Dropdown, Item);
    }
}

USettingControlWidget::USettingControlWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    FontObject = V3DHUDLegacyStyle::FindFontForConstructor();
}

UBooleanSettingWidget::UBooleanSettingWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    FontSize = 32;
    ValueFontSize = 16;
}

UFloatSettingWidget::UFloatSettingWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    ValueFontSize = 12;
}

void USettingControlWidget::ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField)
{
    OwnerMenu = InOwner;
    SettingField = InField;
    RefreshSettingWidget();
}

void USettingControlWidget::RefreshSettingWidget()
{
}

FText USettingControlWidget::GetSettingLabel() const
{
    return IsValid(OwnerMenu) ? OwnerMenu->GetSettingLabelText(SettingField) : FText::GetEmpty();
}

FText USettingControlWidget::GetSettingValueText() const
{
    return IsValid(OwnerMenu) ? OwnerMenu->GetPendingSettingValueText(SettingField) : FText::GetEmpty();
}

TSharedRef<SWidget> UBooleanSettingWidget::RebuildWidget()
{
    const bool bBuildNativeTree = NeedsNativeRow();
    if (bBuildNativeTree)
    {
        BeginNativeRow();
        UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
        NativeRow = Row;
        NativeRoot->SetContent(Row);
        UHorizontalBoxSlot* LabelSlot = Row->AddChildToHorizontalBox(NativeLabel);
        LabelSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        LabelSlot->SetVerticalAlignment(VAlign_Center);
        UButton* Control = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(),
            TEXT("Button"));
        Row->AddChildToHorizontalBox(Control)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        UTextBlock* Value = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
            TEXT("Value"));
        Control->SetContent(Value);
        UButtonSlot* ButtonSlot = CastChecked<UButtonSlot>(Value->Slot);
        ButtonSlot->SetPadding(FMargin(0.f, 8.f, 0.f, 8.f));
        ButtonSlot->SetHorizontalAlignment(HAlign_Fill);
        ButtonSlot->SetVerticalAlignment(VAlign_Center);
        Value->SetJustification(ETextJustify::Center);
        NativeControl = Control;
        NativeValue = Value;
    }
    BindNativeRowWidgets();
    ApplyNativeRowStyle();
    RefreshSettingWidget();
    if (bBuildNativeTree)
    {
        V3DNativeWidgetTree::StabilizeNames(WidgetTree);
        V3DNativeWidgetTree::BindBlueprintWidgets(this);
    }
    return Super::RebuildWidget();
}

void UBooleanSettingWidget::NativeConstruct()
{
    Super::NativeConstruct();
    BindNativeRowWidgets();
    if (!AssignedToggleButton.IsValid()) SetToggleButton(Cast<UButton>(GetWidgetFromName(TEXT("Button"))));
    if (!AssignedLabelText.IsValid()) SetLabelTextBlock(Cast<UTextBlock>(GetWidgetFromName(TEXT("Label"))));
    if (!AssignedValueText.IsValid()) SetValueTextBlock(Cast<UTextBlock>(GetWidgetFromName(TEXT("Value"))));
    RefreshSettingWidget();
}

void UBooleanSettingWidget::NativeDestruct()
{
    if (UButton* Button = AssignedToggleButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UBooleanSettingWidget::HandleToggleClicked);
    }
    AssignedToggleButton.Reset();
    AssignedLabelText.Reset();
    AssignedValueText.Reset();
    Super::NativeDestruct();
}

void UBooleanSettingWidget::ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField)
{
    Super::ConfigureSetting(InOwner, InField);
    if (UButton* Button = AssignedToggleButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UBooleanSettingWidget::HandleToggleClicked);
        Button->OnClicked.AddDynamic(this, &UBooleanSettingWidget::HandleToggleClicked);
    }
    RefreshSettingWidget();
}

void UBooleanSettingWidget::RefreshSettingWidget()
{
    if (!IsValid(OwnerMenu))
    {
        if (UTextBlock* Text = AssignedLabelText.Get()) Text->SetText(PreviewLabel);
        if (UTextBlock* Text = AssignedValueText.Get()) Text->SetText(NSLOCTEXT("V3DUI", "TogglePreview", "On"));
        return;
    }

    const FText Label = OwnerMenu->GetSettingLabelText(SettingField);
    const FText ValueText = OwnerMenu->GetPendingSettingValueText(SettingField);
    const bool bValue = OwnerMenu->GetPendingBooleanSettingValue(SettingField);

    if (UTextBlock* Text = AssignedLabelText.Get()) Text->SetText(Label);
    if (UTextBlock* Text = AssignedValueText.Get()) Text->SetText(ValueText);
    if (!IsDesignTime()) BP_OnBooleanSettingUpdated(Label, bValue, ValueText);
}

void UBooleanSettingWidget::SetToggleButton(UButton* InButton)
{
    if (bUseNativeLayout && IsValid(NativeRoot) && InButton && InButton != NativeControl.Get()) return;
    if (UButton* Existing = AssignedToggleButton.Get())
    {
        Existing->OnClicked.RemoveDynamic(this, &UBooleanSettingWidget::HandleToggleClicked);
    }
    AssignedToggleButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UBooleanSettingWidget::HandleToggleClicked);
        InButton->OnClicked.AddDynamic(this, &UBooleanSettingWidget::HandleToggleClicked);
    }
}

void UBooleanSettingWidget::SetLabelTextBlock(UTextBlock* InTextBlock)
{
    if (bUseNativeLayout && IsValid(NativeRoot) && InTextBlock && InTextBlock != NativeLabel.Get()) return;
    AssignedLabelText = InTextBlock;
    RefreshSettingWidget();
}

void UBooleanSettingWidget::SetValueTextBlock(UTextBlock* InTextBlock)
{
    if (bUseNativeLayout && IsValid(NativeRoot) && InTextBlock && InTextBlock != NativeValue.Get()) return;
    AssignedValueText = InTextBlock;
    RefreshSettingWidget();
}

void UBooleanSettingWidget::ToggleValue()
{
    if (IsValid(OwnerMenu))
    {
        OwnerMenu->ToggleSettingFromUI(SettingField);
    }
}

bool UBooleanSettingWidget::GetBooleanValue() const
{
    return IsValid(OwnerMenu) && OwnerMenu->GetPendingBooleanSettingValue(SettingField);
}

void UBooleanSettingWidget::HandleToggleClicked()
{
    ToggleValue();
}

TSharedRef<SWidget> UFloatSettingWidget::RebuildWidget()
{
    const bool bBuildNativeTree = NeedsNativeRow();
    if (bBuildNativeTree)
    {
        BeginNativeRow();
        UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
        NativeRow = Row;
        NativeRoot->SetContent(Row);
        UHorizontalBoxSlot* LabelSlot = Row->AddChildToHorizontalBox(NativeLabel);
        LabelSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        LabelSlot->SetVerticalAlignment(VAlign_Center);
        NativeLabel->SetJustification(ETextJustify::Center);
        UVerticalBox* ValueColumn = WidgetTree->ConstructWidget<UVerticalBox>();
        Row->AddChildToHorizontalBox(ValueColumn)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        USlider* Control = WidgetTree->ConstructWidget<USlider>(USlider::StaticClass(),
            TEXT("Slider"));
        Control->SetMinValue(0.f);
        Control->SetMaxValue(1.f);
        Control->SetValue(.5f);
        ValueColumn->AddChildToVerticalBox(Control)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        NativeControl = Control;
        NativeValue = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
            TEXT("Value"));
        UVerticalBoxSlot* ValueSlot = ValueColumn->AddChildToVerticalBox(NativeValue);
        ValueSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        ValueSlot->SetVerticalAlignment(VAlign_Center);
        NativeValue->SetJustification(ETextJustify::Center);
    }
    BindNativeRowWidgets();
    ApplyNativeRowStyle();
    RefreshSettingWidget();
    if (bBuildNativeTree)
    {
        V3DNativeWidgetTree::StabilizeNames(WidgetTree);
        V3DNativeWidgetTree::BindBlueprintWidgets(this);
    }
    return Super::RebuildWidget();
}

void UFloatSettingWidget::NativeConstruct()
{
    Super::NativeConstruct();
    BindNativeRowWidgets();
    if (!AssignedSlider.IsValid()) SetSlider(Cast<USlider>(GetWidgetFromName(TEXT("Slider"))));
    if (!AssignedLabelText.IsValid()) SetLabelTextBlock(Cast<UTextBlock>(GetWidgetFromName(TEXT("Label"))));
    if (!AssignedValueText.IsValid()) SetValueTextBlock(Cast<UTextBlock>(GetWidgetFromName(TEXT("Value"))));
    RefreshSettingWidget();
}

void UFloatSettingWidget::NativeDestruct()
{
    if (USlider* Slider = AssignedSlider.Get())
    {
        Slider->OnValueChanged.RemoveDynamic(this, &UFloatSettingWidget::HandleSliderValueChanged);
    }
    AssignedSlider.Reset();
    AssignedLabelText.Reset();
    AssignedValueText.Reset();
    Super::NativeDestruct();
}

void UFloatSettingWidget::ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField)
{
    Super::ConfigureSetting(InOwner, InField);
    if (USlider* Slider = AssignedSlider.Get())
    {
        Slider->OnValueChanged.RemoveDynamic(this, &UFloatSettingWidget::HandleSliderValueChanged);
        Slider->OnValueChanged.AddDynamic(this, &UFloatSettingWidget::HandleSliderValueChanged);
    }
    RefreshSettingWidget();
}

void UFloatSettingWidget::RefreshSettingWidget()
{
    if (!IsValid(OwnerMenu))
    {
        if (UTextBlock* Text = AssignedLabelText.Get()) Text->SetText(PreviewLabel);
        if (UTextBlock* Text = AssignedValueText.Get()) Text->SetText(NSLOCTEXT("V3DUI", "FloatPreview", "0.50"));
        return;
    }

    float MinValue = 0.0f;
    float MaxValue = 1.0f;
    float Step = 0.01f;
    OwnerMenu->GetSettingSliderRange(SettingField, MinValue, MaxValue, Step);
    const float Value = OwnerMenu->GetPendingNumericSettingValue(SettingField);
    const FText Label = OwnerMenu->GetSettingLabelText(SettingField);
    const FText ValueText = OwnerMenu->GetPendingSettingValueText(SettingField);

    bInternalRefresh = true;
    if (USlider* Slider = AssignedSlider.Get())
    {
        Slider->SetMinValue(MinValue);
        Slider->SetMaxValue(MaxValue);
        Slider->SetStepSize(Step);
        Slider->SetValue(Value);
    }
    bInternalRefresh = false;

    if (UTextBlock* Text = AssignedLabelText.Get()) Text->SetText(Label);
    if (UTextBlock* Text = AssignedValueText.Get()) Text->SetText(ValueText);
    if (!IsDesignTime()) BP_OnFloatSettingUpdated(Label, Value, MinValue, MaxValue, Step, ValueText);
}

void UFloatSettingWidget::SetSlider(USlider* InSlider)
{
    if (bUseNativeLayout && IsValid(NativeRoot) && InSlider && InSlider != NativeControl.Get()) return;
    if (USlider* Existing = AssignedSlider.Get())
    {
        Existing->OnValueChanged.RemoveDynamic(this, &UFloatSettingWidget::HandleSliderValueChanged);
    }
    AssignedSlider = InSlider;
    if (IsValid(InSlider))
    {
        InSlider->OnValueChanged.RemoveDynamic(this, &UFloatSettingWidget::HandleSliderValueChanged);
        InSlider->OnValueChanged.AddDynamic(this, &UFloatSettingWidget::HandleSliderValueChanged);
    }
    RefreshSettingWidget();
}

void UFloatSettingWidget::SetLabelTextBlock(UTextBlock* InTextBlock)
{
    if (bUseNativeLayout && IsValid(NativeRoot) && InTextBlock && InTextBlock != NativeLabel.Get()) return;
    AssignedLabelText = InTextBlock;
    RefreshSettingWidget();
}

void UFloatSettingWidget::SetValueTextBlock(UTextBlock* InTextBlock)
{
    if (bUseNativeLayout && IsValid(NativeRoot) && InTextBlock && InTextBlock != NativeValue.Get()) return;
    AssignedValueText = InTextBlock;
    RefreshSettingWidget();
}

void UFloatSettingWidget::SetNumericValue(float Value)
{
    if (IsValid(OwnerMenu))
    {
        OwnerMenu->SetSettingFromSliderValue(SettingField, Value);
    }
}

float UFloatSettingWidget::GetNumericValue() const
{
    return IsValid(OwnerMenu) ? OwnerMenu->GetPendingNumericSettingValue(SettingField) : 0.0f;
}

bool UFloatSettingWidget::GetNumericRange(float& OutMin, float& OutMax, float& OutStep) const
{
    return IsValid(OwnerMenu) && OwnerMenu->GetSettingSliderRange(SettingField, OutMin, OutMax, OutStep);
}

void UFloatSettingWidget::HandleSliderValueChanged(float Value)
{
    if (!bInternalRefresh)
    {
        SetNumericValue(Value);
    }
}

TSharedRef<SWidget> UEnumSettingWidget::RebuildWidget()
{
    const bool bBuildNativeTree = NeedsNativeRow();
    if (bBuildNativeTree)
    {
        BeginNativeRow();
        UVerticalBox* Root = WidgetTree->ConstructWidget<UVerticalBox>();
        NativeRow = Root;
        NativeRoot->SetContent(Root);
        NativeLabel->SetJustification(ETextJustify::Center);
        NativeLabel->SetMinDesiredWidth(64.f);
        UVerticalBoxSlot* LabelSlot = Root->AddChildToVerticalBox(NativeLabel);
        LabelSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        LabelSlot->SetPadding(FMargin(8.f));
        UComboBoxString* Control = WidgetTree->ConstructWidget<UComboBoxString>(UComboBoxString::StaticClass(),
            TEXT("Dropdown"));
        Control->OnGenerateWidgetEvent.BindDynamic(this, &UEnumSettingWidget::GenerateNativeOption);
        Control->AddOption(TEXT("Low"));
        Control->AddOption(TEXT("Medium"));
        Control->AddOption(TEXT("High"));
        Control->AddOption(TEXT("Epic"));
        Control->SetSelectedOption(TEXT("High"));
        UVerticalBoxSlot* DropdownSlot = Root->AddChildToVerticalBox(Control);
        DropdownSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        DropdownSlot->SetPadding(FMargin(8.f));
        NativeControl = Control;
        NativeValue = nullptr;
    }
    BindNativeRowWidgets();
    ApplyNativeRowStyle();
    RefreshSettingWidget();
    if (bBuildNativeTree)
    {
        V3DNativeWidgetTree::StabilizeNames(WidgetTree);
        V3DNativeWidgetTree::BindBlueprintWidgets(this);
    }
    return Super::RebuildWidget();
}

void UEnumSettingWidget::NativeConstruct()
{
    Super::NativeConstruct();
    BindNativeRowWidgets();
    if (!AssignedDropdown.IsValid()) SetDropdown(Cast<UComboBoxString>(GetWidgetFromName(TEXT("Dropdown"))));
    if (!AssignedLabelText.IsValid()) SetLabelTextBlock(Cast<UTextBlock>(GetWidgetFromName(TEXT("Label"))));
    RefreshSettingWidget();
}

void UEnumSettingWidget::NativeDestruct()
{
    if (UComboBoxString* Dropdown = AssignedDropdown.Get())
    {
        Dropdown->OnSelectionChanged.RemoveDynamic(this, &UEnumSettingWidget::HandleSelectionChanged);
    }
    AssignedDropdown.Reset();
    AssignedLabelText.Reset();
    AssignedValueText.Reset();
    Super::NativeDestruct();
}

void UEnumSettingWidget::ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField)
{
    Super::ConfigureSetting(InOwner, InField);
    if (UComboBoxString* Dropdown = AssignedDropdown.Get())
    {
        Dropdown->OnSelectionChanged.RemoveDynamic(this, &UEnumSettingWidget::HandleSelectionChanged);
        Dropdown->OnSelectionChanged.AddDynamic(this, &UEnumSettingWidget::HandleSelectionChanged);
    }
    RebuildDropdownOptions();
    RefreshSettingWidget();
}

void UEnumSettingWidget::RefreshSettingWidget()
{
    if (!IsValid(OwnerMenu))
    {
        if (UTextBlock* Text = AssignedLabelText.Get()) Text->SetText(PreviewLabel);
        return;
    }

    const FText Label = OwnerMenu->GetSettingLabelText(SettingField);
    const FText ValueText = OwnerMenu->GetPendingSettingValueText(SettingField);
    const TArray<FText> Options = OwnerMenu->GetSettingOptionTexts(SettingField);

    bInternalRefresh = true;
    if (UComboBoxString* Dropdown = AssignedDropdown.Get())
    {
        const FString Selected = ValueText.ToString();
        if (Dropdown->FindOptionIndex(Selected) == INDEX_NONE)
        {
            RebuildDropdownOptions();
            bInternalRefresh = true;
        }
        Dropdown->SetSelectedOption(Selected);
    }
    bInternalRefresh = false;

    if (UTextBlock* Text = AssignedLabelText.Get()) Text->SetText(Label);
    if (UTextBlock* Text = AssignedValueText.Get()) Text->SetText(ValueText);
    if (!IsDesignTime()) BP_OnEnumSettingUpdated(Label, Options, ValueText);
}

void UEnumSettingWidget::SetDropdown(UComboBoxString* InDropdown)
{
    if (bUseNativeLayout && IsValid(NativeRoot) && InDropdown && InDropdown != NativeControl.Get()) return;
    if (UComboBoxString* Existing = AssignedDropdown.Get())
    {
        Existing->OnSelectionChanged.RemoveDynamic(this, &UEnumSettingWidget::HandleSelectionChanged);
    }
    AssignedDropdown = InDropdown;
    if (IsValid(InDropdown))
    {
        InDropdown->OnSelectionChanged.RemoveDynamic(this, &UEnumSettingWidget::HandleSelectionChanged);
        InDropdown->OnSelectionChanged.AddDynamic(this, &UEnumSettingWidget::HandleSelectionChanged);
    }
    RebuildDropdownOptions();
    RefreshSettingWidget();
}

void UEnumSettingWidget::SetLabelTextBlock(UTextBlock* InTextBlock)
{
    if (bUseNativeLayout && IsValid(NativeRoot) && InTextBlock && InTextBlock != NativeLabel.Get()) return;
    AssignedLabelText = InTextBlock;
    RefreshSettingWidget();
}

void UEnumSettingWidget::SetValueTextBlock(UTextBlock* InTextBlock)
{
    if (bUseNativeLayout && IsValid(NativeRoot) && InTextBlock && InTextBlock != NativeValue.Get()) return;
    AssignedValueText = InTextBlock;
    RefreshSettingWidget();
}

void UEnumSettingWidget::SelectOption(const FString& SelectedOption)
{
    if (IsValid(OwnerMenu))
    {
        OwnerMenu->SetSettingFromDropdownSelection(SettingField, SelectedOption);
    }
}

TArray<FText> UEnumSettingWidget::GetOptions() const
{
    return IsValid(OwnerMenu) ? OwnerMenu->GetSettingOptionTexts(SettingField) : TArray<FText>();
}

void UEnumSettingWidget::HandleSelectionChanged(FString SelectedItem, ESelectInfo::Type /*SelectionType*/)
{
    if (!bInternalRefresh)
    {
        SelectOption(SelectedItem);
    }
}

void UEnumSettingWidget::RebuildDropdownOptions()
{
    UComboBoxString* Dropdown = AssignedDropdown.Get();
    if (!IsValid(Dropdown) || !IsValid(OwnerMenu)) return;

    bInternalRefresh = true;
    Dropdown->ClearOptions();
    for (const FText& Option : OwnerMenu->GetSettingOptionTexts(SettingField))
    {
        Dropdown->AddOption(Option.ToString());
    }
    bInternalRefresh = false;
}


bool USettingControlWidget::NeedsNativeRow() const
{
    return bUseNativeLayout && IsValid(WidgetTree)
        && (!IsValid(NativeScaleRoot) || WidgetTree->RootWidget != NativeScaleRoot);
}

void USettingControlWidget::BeginNativeRow()
{
    WidgetTree = NewObject<UWidgetTree>(this, NAME_None, RF_Transient);
    NativeScaleRoot = WidgetTree->ConstructWidget<UScaleBox>();
    // The enclosing HUD scales for resolution; this root must not distort its fonts.
    NativeScaleRoot->SetStretch(EStretch::None);
    WidgetTree->RootWidget = NativeScaleRoot;
    NativeRoot = WidgetTree->ConstructWidget<USizeBox>();
    NativeRoot->SetMinDesiredHeight(FMath::Clamp(FMath::IsFinite(RowMinHeight) ? RowMinHeight : 80.f, 32.f, 256.f));
    NativeRoot->SetMinDesiredWidth(FMath::Clamp(FMath::IsFinite(RowMinWidth) ? RowMinWidth : 240.f, 1.f, 16384.f));
    UScaleBoxSlot* ScaleSlot = CastChecked<UScaleBoxSlot>(NativeScaleRoot->AddChild(NativeRoot));
    ScaleSlot->SetHorizontalAlignment(HAlign_Fill);
    ScaleSlot->SetVerticalAlignment(VAlign_Fill);
    NativeLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
        TEXT("Label"));
    NativeLabel->SetText(PreviewLabel);
    NativeValue = nullptr;
}

void USettingControlWidget::BindNativeRowWidgets()
{
}

void USettingControlWidget::SynchronizeProperties()
{
    Super::SynchronizeProperties();
    BindNativeRowWidgets();
    ApplyNativeRowStyle();
    RefreshSettingWidget();
}

void USettingControlWidget::NativePreConstruct()
{
    Super::NativePreConstruct();
    BindNativeRowWidgets();
    ApplyNativeRowStyle();
    RefreshSettingWidget();
}

void USettingControlWidget::ApplyNativeRowStyle()
{
    if (!bUseNativeLayout) return;
    if (IsValid(NativeRoot))
    {
        NativeRoot->SetMinDesiredHeight(FMath::Clamp(FMath::IsFinite(RowMinHeight) ? RowMinHeight : 80.f, 32.f, 256.f));
        NativeRoot->SetMinDesiredWidth(FMath::Clamp(FMath::IsFinite(RowMinWidth) ? RowMinWidth : 240.f, 1.f, 16384.f));
    }
    for (UTextBlock* Text : {NativeLabel.Get(), NativeValue.Get()})
    {
        if (!IsValid(Text)) continue;
        FSlateFontInfo Font = Text->GetFont();
        V3DHUDLegacyStyle::ApplyFont(Font, FontObject,
            FMath::Clamp(Text == NativeLabel.Get() ? FontSize : ValueFontSize, 8, 96), FontOutlineSize);
        Text->SetFont(Font);
        Text->SetColorAndOpacity(FSlateColor(TextColor));
    }
    NativeOptionLabels.RemoveAll([](const TWeakObjectPtr<UTextBlock>& Item) { return !Item.IsValid(); });
    for (const TWeakObjectPtr<UTextBlock>& WeakText : NativeOptionLabels)
    {
        UTextBlock* Text = WeakText.Get();
        if (!IsValid(Text)) continue;
        FSlateFontInfo Font = Text->GetFont();
        V3DHUDLegacyStyle::ApplyFont(Font, FontObject, FMath::Clamp(FontSize, 8, 96), FontOutlineSize);
        Text->SetFont(Font);
        Text->SetColorAndOpacity(FSlateColor(TextColor));
    }
    if (UButton* Button = Cast<UButton>(NativeControl))
    {
        V3DHUDLegacyStyle::ApplyButton(*Button, PanelColor);
        FButtonStyle Style = Button->GetStyle();
        Style.Hovered.TintColor = FSlateColor(AccentColor);
        V3DWidgetStyle::SetOwnedStyle(*Button, Style);
    }
    if (UComboBoxString* Dropdown = Cast<UComboBoxString>(NativeControl))
        ApplySettingRowComboTheme(Dropdown, PanelColor, AccentColor, TextColor);
}

void UFloatSettingWidget::ApplyNativeRowStyle()
{
    Super::ApplyNativeRowStyle();
    USlider* Slider = bUseNativeLayout ? Cast<USlider>(NativeControl) : nullptr;
    if (!IsValid(Slider)) return;
    FSliderStyle Style = Slider->GetWidgetStyle();
    Style.NormalBarImage.TintColor = FSlateColor(SliderBarColor);
    Style.HoveredBarImage.TintColor = FSlateColor(SliderHoveredBarColor);
    const FVector2D ThumbSize(
        FMath::Clamp(FMath::IsFinite(SliderThumbSize.X) ? SliderThumbSize.X : 16.0, 1.0, 512.0),
        FMath::Clamp(FMath::IsFinite(SliderThumbSize.Y) ? SliderThumbSize.Y : 32.0, 1.0, 512.0));
    Style.NormalThumbImage.ImageSize = ThumbSize;
    Style.HoveredThumbImage.ImageSize = ThumbSize;
    Style.DisabledThumbImage.ImageSize = ThumbSize;
    Style.BarThickness = FMath::Clamp(FMath::IsFinite(SliderBarThickness) ? SliderBarThickness : 24.f, 1.f, 128.f);
    V3DWidgetStyle::SetOwnedStyle(*Slider, Style);
    Slider->SetSliderBarColor(FLinearColor::White);
    Slider->SetSliderHandleColor(SliderHandleColor);
}

void USettingControlWidget::ApplyControlTheme(UObject* InFontObject, int32 InOutlineSize, FLinearColor InTextColor,
    FLinearColor InPanelColor, FLinearColor InAccentColor, float InMinHeight)
{
    FontObject = InFontObject;
    FontOutlineSize = FMath::Clamp(InOutlineSize, 0, 8);
    TextColor = InTextColor;
    PanelColor = InPanelColor;
    AccentColor = InAccentColor;
    RowMinHeight = FMath::Clamp(FMath::IsFinite(InMinHeight) ? InMinHeight : 80.f, 32.f, 256.f);
    ApplyNativeRowStyle();
}

void UBooleanSettingWidget::BindNativeRowWidgets()
{
    if (!bUseNativeLayout || !IsValid(NativeRoot)) return;
    if (AssignedToggleButton.Get() != NativeControl.Get()) SetToggleButton(Cast<UButton>(NativeControl));
    AssignedLabelText = NativeLabel.Get();
    AssignedValueText = NativeValue.Get();
}

void UFloatSettingWidget::BindNativeRowWidgets()
{
    if (!bUseNativeLayout || !IsValid(NativeRoot)) return;
    if (AssignedSlider.Get() != NativeControl.Get()) SetSlider(Cast<USlider>(NativeControl));
    AssignedLabelText = NativeLabel.Get();
    AssignedValueText = NativeValue.Get();
}

void UEnumSettingWidget::BindNativeRowWidgets()
{
    if (!bUseNativeLayout || !IsValid(NativeRoot)) return;
    if (AssignedDropdown.Get() != NativeControl.Get()) SetDropdown(Cast<UComboBoxString>(NativeControl));
    AssignedLabelText = NativeLabel.Get();
    AssignedValueText = NativeValue.Get();
}


UWidget* USettingControlWidget::GenerateNativeOption(FString Option)
{
    UTextBlock* Text = NewObject<UTextBlock>(this);
    Text->SetText(FText::FromString(Option));
    FSlateFontInfo Font = Text->GetFont();
    V3DHUDLegacyStyle::ApplyFont(Font, FontObject, FMath::Clamp(FontSize, 8, 96), FontOutlineSize);
    Text->SetFont(Font);
    Text->SetColorAndOpacity(FSlateColor(TextColor));
    NativeOptionLabels.RemoveAll([](const TWeakObjectPtr<UTextBlock>& Item) { return !Item.IsValid(); });
    NativeOptionLabels.Add(Text);
    return Text;
}

void USettingControlWidget::ReleaseSlateResources(bool bReleaseChildren)
{
    Super::ReleaseSlateResources(bReleaseChildren);
    NativeOptionLabels.Reset();
}
