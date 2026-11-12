// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file SettingsMenuWidget.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "UI/SettingsMenuWidget.h"
#include "UI/WidgetStyleSafety.h"
#include "UI/HUDLegacyStyle.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "UI/SettingControlWidget.h"
#include "System/SimulatorFileServices.h"
#include "Blueprint/WidgetTree.h"

#include "Components/ContentWidget.h"
#include "Components/ButtonSlot.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Setting/GameSettings.h"
#include "System/GameManagerSubSystem.h"

namespace
{
    void ApplySettingsMenuComboTheme(UComboBoxString* Dropdown, FLinearColor Panel, FLinearColor Accent, FLinearColor Text)
    {
        FComboBoxStyle Style = Dropdown->GetWidgetStyle();
        Style.ComboButtonStyle.ButtonStyle.Normal.TintColor = FSlateColor(Panel);
        Style.ComboButtonStyle.ButtonStyle.Hovered.TintColor = FSlateColor(Accent);
        Style.ComboButtonStyle.ButtonStyle.Pressed.TintColor = FSlateColor(Accent);
        Style.ComboButtonStyle.MenuBorderBrush.TintColor = FSlateColor(Panel);
        V3DWidgetStyle::SetOwnedStyle(*Dropdown, Style);
        FTableRowStyle Item = Dropdown->GetItemStyle();
        Item.EvenRowBackgroundBrush.TintColor = FSlateColor(Panel);
        Item.OddRowBackgroundBrush.TintColor = FSlateColor(Panel);
        Item.EvenRowBackgroundHoveredBrush.TintColor = FSlateColor(Accent);
        Item.OddRowBackgroundHoveredBrush.TintColor = FSlateColor(Accent);
        Item.ActiveBrush.TintColor = FSlateColor(Accent);
        Item.ActiveHoveredBrush.TintColor = FSlateColor(Accent);
        Item.InactiveBrush.TintColor = FSlateColor(Panel);
        Item.InactiveHoveredBrush.TintColor = FSlateColor(Accent);
        Item.TextColor = FSlateColor(Text);
        Item.SelectedTextColor = FSlateColor(Text);
        V3DWidgetStyle::SetOwnedItemStyle(*Dropdown, Item);
    }
}

namespace
{
    constexpr float BloomStep = 0.25f;
    constexpr float BloomIntensityMin = 0.0f;
    constexpr float BloomIntensityMax = 10.0f;
    constexpr float BloomThresholdMin = -1.0f;
    constexpr float BloomThresholdMax = 10.0f;
    constexpr float AmbientOcclusionStep = 0.25f;
    constexpr float AmbientOcclusionMin = 0.0f;
    constexpr float AmbientOcclusionMax = 5.0f;
    constexpr int32 ExposureAutoValue = -11;
    constexpr int32 ExposureManualMin = -10;
    constexpr int32 ExposureManualMax = 20;
    constexpr int32 QualityMin = 0;
    constexpr int32 QualityMax = 3;
    constexpr int32 ReflectionMethodMin = 0;
    constexpr int32 ReflectionMethodMax = 2;
    constexpr int32 TextureResolutionMin = 256;
    constexpr int32 TextureResolutionMax = 4096;
    constexpr float StreamingDistanceMin = 1.0f;
    constexpr float StreamingDistanceMax = 512.0f;
    constexpr float StreamingDistanceStep = 1.0f;
    constexpr float StreamingUnloadMin = 1.0f;
    constexpr float StreamingUnloadMax = 2.0f;
    constexpr float StreamingUnloadStep = 0.01f;
    constexpr float ObjectRadiusMin = 512.0f;
    constexpr float ObjectRadiusMax = 4096.0f;
    constexpr float ObjectRadiusStep = 64.0f;
    constexpr float SceneSpawnBudgetMin = 1.0f;
    constexpr float SceneSpawnBudgetMax = 32.0f;
    constexpr float SceneSpawnBudgetStep = 1.0f;
    constexpr float NodeBudgetMin = 1.0f;
    constexpr float NodeBudgetMax = 256.0f;
    constexpr float NodeBudgetStep = 1.0f;

    const TArray<ESettingsField>& GetDefaultSettingsFields()
    {
        static const TArray<ESettingsField> Fields = {
            ESettingsField::MouseSensitivity,
            ESettingsField::QualityProfile,
            ESettingsField::BloomIntensity,
            ESettingsField::BloomThreshold,
            ESettingsField::AmbientOcclusionIntensity,
            ESettingsField::Exposure,
            ESettingsField::RayTracing,
            ESettingsField::HeightFog,
            ESettingsField::Cloud,
            ESettingsField::CelShadingMode,
            ESettingsField::ShadowQuality,
            ESettingsField::TextureQuality,
            ESettingsField::MaxTextureResolution,
            ESettingsField::ViewDistanceQuality,
            ESettingsField::MaxRenderDistanceMeters,
            ESettingsField::AntiAliasingQuality,
            ESettingsField::PostProcessingQuality,
            ESettingsField::EffectsQuality,
            ESettingsField::FoliageQuality,
            ESettingsField::ShadingQuality,
            ESettingsField::GlobalIlluminationQuality,
            ESettingsField::ReflectionQuality,
            ESettingsField::DynamicGlobalIlluminationMethod,
            ESettingsField::ReflectionMethod
        };
        return Fields;
    }

    bool IsAutomaticStreamingField(const ESettingsField Field)
    {
        switch (Field)
        {
        case ESettingsField::StreamingDistanceMultiplier:
        case ESettingsField::StreamingUnloadDistanceMultiplier:
        case ESettingsField::ObjectStreamingRadiusMeters:
        case ESettingsField::StreamingSceneSpawnBudget:
        case ESettingsField::StreamingNodeBudgetPerFrame:
            return true;
        default:
            return false;
        }
    }

    int32 WrapIndex(int32 Value, int32 MinValue, int32 MaxValue)
    {
        const int32 Count = MaxValue - MinValue + 1;
        if (Count <= 0)
        {
            return MinValue;
        }

        int32 Offset = (Value - MinValue) % Count;
        if (Offset < 0)
        {
            Offset += Count;
        }
        return MinValue + Offset;
    }

    int32 NormalizedDirection(int32 Direction)
    {
        return Direction < 0 ? -1 : 1;
    }

    void CycleInt(int32& Value, int32 MinValue, int32 MaxValue, int32 Direction)
    {
        Value = WrapIndex(Value + NormalizedDirection(Direction), MinValue, MaxValue);
    }

    void CycleTextureResolution(int32& Value, int32 Direction)
    {
        static const int32 Options[] = {256, 512, 768, 1024, 1536, 2048, 4096};
        constexpr int32 OptionCount = sizeof(Options) / sizeof(Options[0]);

        int32 ClosestIndex = 0;
        int32 ClosestDistance = TNumericLimits<int32>::Max();
        for (int32 Index = 0; Index < OptionCount; ++Index)
        {
            const int32 Distance = FMath::Abs(Value - Options[Index]);
            if (Distance < ClosestDistance)
            {
                ClosestDistance = Distance;
                ClosestIndex = Index;
            }
        }

        Value = Options[WrapIndex(ClosestIndex + NormalizedDirection(Direction), 0, OptionCount - 1)];
    }

    void CycleFloat(float& Value, float MinValue, float MaxValue, float Step, int32 Direction)
    {
        const int32 Count = FMath::Max(1, FMath::RoundToInt((MaxValue - MinValue) / Step) + 1);
        int32 CurrentIndex = FMath::RoundToInt((FMath::Clamp(Value, MinValue, MaxValue) - MinValue) / Step);
        CurrentIndex = FMath::Clamp(CurrentIndex, 0, Count - 1);

        int32 NewIndex = (CurrentIndex + NormalizedDirection(Direction)) % Count;
        if (NewIndex < 0)
        {
            NewIndex += Count;
        }

        Value = MinValue + static_cast<float>(NewIndex) * Step;
        Value = FMath::Clamp(Value, MinValue, MaxValue);
    }

    FString NormalizeFieldText(FString Text)
    {
        Text.TrimStartAndEndInline();
        Text.ToLowerInline();
        Text.ReplaceInline(TEXT(" "), TEXT(""));
        Text.ReplaceInline(TEXT("_"), TEXT(""));
        Text.ReplaceInline(TEXT("-"), TEXT(""));
        return Text;
    }

    FString StripValuePart(const FString& Text)
    {
        FString Left;
        FString Right;
        if (Text.Split(TEXT(":"), &Left, &Right))
        {
            return Left;
        }
        return Text;
    }

    FName FieldName(ESettingsField Field)
    {
        switch (Field)
        {
        case ESettingsField::MouseSensitivity: return TEXT("MouseSensitivity");
        case ESettingsField::QualityProfile: return TEXT("QualityProfile");
        case ESettingsField::BloomIntensity: return TEXT("BloomIntensity");
        case ESettingsField::BloomThreshold: return TEXT("BloomThreshold");
        case ESettingsField::AmbientOcclusionIntensity: return TEXT("AmbientOcclusionIntensity");
        case ESettingsField::Exposure: return TEXT("Exposure");
        case ESettingsField::RayTracing: return TEXT("RayTracing");
        case ESettingsField::HeightFog: return TEXT("HeightFog");
        case ESettingsField::Cloud: return TEXT("Cloud");
        case ESettingsField::CelShadingMode: return TEXT("CelShadingMode");
        case ESettingsField::ShadowQuality: return TEXT("ShadowQuality");
        case ESettingsField::TextureQuality: return TEXT("TextureQuality");
        case ESettingsField::MaxTextureResolution: return TEXT("MaxTextureResolution");
        case ESettingsField::ViewDistanceQuality: return TEXT("ViewDistanceQuality");
        case ESettingsField::MaxRenderDistanceMeters: return TEXT("MaxRenderDistanceMeters");
        case ESettingsField::StreamingDistanceMultiplier: return TEXT("StreamingDistanceMultiplier");
        case ESettingsField::StreamingUnloadDistanceMultiplier: return TEXT("StreamingUnloadDistanceMultiplier");
        case ESettingsField::ObjectStreamingRadiusMeters: return TEXT("ObjectStreamingRadiusMeters");
        case ESettingsField::StreamingSceneSpawnBudget: return TEXT("StreamingSceneSpawnBudget");
        case ESettingsField::StreamingNodeBudgetPerFrame: return TEXT("StreamingNodeBudgetPerFrame");
        case ESettingsField::AntiAliasingQuality: return TEXT("AntiAliasingQuality");
        case ESettingsField::PostProcessingQuality: return TEXT("PostProcessingQuality");
        case ESettingsField::EffectsQuality: return TEXT("EffectsQuality");
        case ESettingsField::FoliageQuality: return TEXT("FoliageQuality");
        case ESettingsField::ShadingQuality: return TEXT("ShadingQuality");
        case ESettingsField::GlobalIlluminationQuality: return TEXT("GlobalIlluminationQuality");
        case ESettingsField::ReflectionQuality: return TEXT("ReflectionQuality");
        case ESettingsField::DynamicGlobalIlluminationMethod: return TEXT("DynamicGlobalIlluminationMethod");
        case ESettingsField::ReflectionMethod: return TEXT("ReflectionMethod");
        default: return NAME_None;
        }
    }
}

void USettingsControlBinding::BindSlider(
    USettingsMenuWidget* InOwner, ESettingsField InField, USlider* InSlider)
{
    Unbind();
    OwnerWidget = InOwner;
    Field = InField;
    Slider = InSlider;
    if (IsValid(Slider))
    {
        Slider->OnValueChanged.RemoveDynamic(this, &USettingsControlBinding::HandleSliderValueChanged);
        Slider->OnValueChanged.AddDynamic(this, &USettingsControlBinding::HandleSliderValueChanged);
    }
}

void USettingsControlBinding::BindDropdown(
    USettingsMenuWidget* InOwner, ESettingsField InField, UComboBoxString* InDropdown)
{
    Unbind();
    OwnerWidget = InOwner;
    Field = InField;
    Dropdown = InDropdown;
    if (IsValid(Dropdown))
    {
        Dropdown->OnSelectionChanged.RemoveDynamic(this, &USettingsControlBinding::HandleDropdownSelectionChanged);
        Dropdown->OnSelectionChanged.AddDynamic(this, &USettingsControlBinding::HandleDropdownSelectionChanged);
    }
}

void USettingsControlBinding::BindToggle(
    USettingsMenuWidget* InOwner, ESettingsField InField, UButton* InButton)
{
    Unbind();
    OwnerWidget = InOwner;
    Field = InField;
    ToggleButton = InButton;
    if (IsValid(ToggleButton))
    {
        ToggleButton->OnClicked.RemoveDynamic(this, &USettingsControlBinding::HandleToggleClicked);
        ToggleButton->OnClicked.AddDynamic(this, &USettingsControlBinding::HandleToggleClicked);
    }
}

void USettingsControlBinding::Unbind()
{
    if (IsValid(Slider))
    {
        Slider->OnValueChanged.RemoveDynamic(this, &USettingsControlBinding::HandleSliderValueChanged);
    }
    if (IsValid(Dropdown))
    {
        Dropdown->OnSelectionChanged.RemoveDynamic(this, &USettingsControlBinding::HandleDropdownSelectionChanged);
    }
    if (IsValid(ToggleButton))
    {
        ToggleButton->OnClicked.RemoveDynamic(this, &USettingsControlBinding::HandleToggleClicked);
    }
    Slider = nullptr;
    Dropdown = nullptr;
    ToggleButton = nullptr;
    OwnerWidget = nullptr;
}

void USettingsControlBinding::HandleSliderValueChanged(const float Value)
{
    if (IsValid(OwnerWidget))
    {
        OwnerWidget->SetSettingFromSliderValue(Field, Value);
    }
}

void USettingsControlBinding::HandleDropdownSelectionChanged(
    FString SelectedItem, ESelectInfo::Type SelectionType)
{
    if (SelectionType != ESelectInfo::Direct && IsValid(OwnerWidget))
    {
        OwnerWidget->SetSettingFromDropdownSelection(Field, SelectedItem);
    }
}

void USettingsControlBinding::HandleToggleClicked()
{
    if (IsValid(OwnerWidget))
    {
        OwnerWidget->ToggleSettingFromUI(Field);
    }
}

UObject* USettingsControlBinding::GetControlObject() const
{
    if (IsValid(Slider)) return Slider.Get();
    if (IsValid(Dropdown)) return Dropdown.Get();
    return ToggleButton.Get();
}

void USettingsCycleButton::SetupCycleButton(USettingsMenuWidget* InOwner, ESettingsField InField, int32 InDirection)
{
    OwnerWidget = InOwner;
    Field = InField;
    Direction = InDirection;
    if (IsAutomaticStreamingField(Field))
    {
        SetVisibility(ESlateVisibility::Collapsed);
        OnClicked.RemoveDynamic(this, &USettingsCycleButton::HandleClicked);
        return;
    }
    OnClicked.RemoveDynamic(this, &USettingsCycleButton::HandleClicked);
    OnClicked.AddDynamic(this, &USettingsCycleButton::HandleClicked);
    RefreshDisplayedText();
}

void USettingsCycleButton::RefreshDisplayedText()
{
    if (OwnerWidget)
    {
        OwnerWidget->SetButtonTextFromUI(this, OwnerWidget->GetSettingButtonText(Field));
    }
}

void USettingsCycleButton::HandleClicked()
{
    if (OwnerWidget)
    {
        OwnerWidget->CycleSettingValueFromUI(Field, Direction);
        RefreshDisplayedText();
    }
}

USettingsMenuWidget::USettingsMenuWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    BodyFontSize = 24;
    TitleFontSize = 100;
    ButtonHeight = 64.f;
    FontOutlineSize = 2;
}

void USettingsMenuWidget::NativeConstruct()
{
    Super::NativeConstruct();

    if (IsDesignTime()) return;
    // Explicit custom layouts may opt into the same native rows through a named or assigned host.
    if (!IsUsingNativeLayout())
    {
        if (!AssignedSettingsListBox.IsValid())
            AssignedSettingsListBox = Cast<UVerticalBox>(GetWidgetFromName(TEXT("SettingsList")));
        if (!AssignedBackButton.IsValid())
            SetBackButton(Cast<UButton>(GetWidgetFromName(TEXT("Back"))));
    }
    CollectAssignedWidgetReferences();
    BindButtonEvents();
    InitializeSettingsFromSavedData();
}

void USettingsMenuWidget::NativeDestruct()
{
    ClearGeneratedSettingWidgets();
    AssignedSettingsListBox.Reset();
    UnbindButtonEvents();
    BoundFieldButtons.Empty();
    AssignedTitleText.Reset();
    AssignedApplyButton.Reset();
    AssignedConfirmButton.Reset();
    AssignedBackButton.Reset();
    AssignedCancelButton.Reset();
    RegisteredValueTextWidgets.Empty();
    RegisteredSettingButtons.Empty();
    ValueTextBlocks.Empty();
    ValueFields.Empty();
    for (USettingsControlBinding* Binding : ControlBindings)
    {
        if (IsValid(Binding)) Binding->Unbind();
    }
    ControlBindings.Empty();
    Super::NativeDestruct();
}

void USettingsMenuWidget::SetTitleText(UTextBlock* InTitleText)
{
    if (!AcceptWidgetAssignment(InTitleText)) return;
    AssignedTitleText = InTitleText;
}

void USettingsMenuWidget::SetApplyButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = AssignedApplyButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ApplyAndSaveSettingsFromUI);
    }

    AssignedApplyButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ApplyAndSaveSettingsFromUI);
        InButton->OnClicked.AddDynamic(this, &USettingsMenuWidget::ApplyAndSaveSettingsFromUI);
    }
}

void USettingsMenuWidget::SetConfirmButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = AssignedConfirmButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ConfirmSettingsFromUI);
    }

    AssignedConfirmButton = InButton;
    if (IsValid(InButton) && InButton != GetApplyButton())
    {
        InButton->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ConfirmSettingsFromUI);
        InButton->OnClicked.AddDynamic(this, &USettingsMenuWidget::ConfirmSettingsFromUI);
    }
}

void USettingsMenuWidget::SetBackButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = AssignedBackButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
    }

    AssignedBackButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
        InButton->OnClicked.AddDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
    }
}

void USettingsMenuWidget::SetCancelButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = AssignedCancelButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
    }

    AssignedCancelButton = InButton;
    if (IsValid(InButton) && InButton != GetBackButton())
    {
        InButton->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
        InButton->OnClicked.AddDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
    }
}


void USettingsMenuWidget::SetSettingsListBox(UVerticalBox* InVerticalBox)
{
    if (IsUsingNativeLayout()) return;
    if (AssignedSettingsListBox.Get() == InVerticalBox)
    {
        RebuildGeneratedSettingWidgets();
        return;
    }

    ClearGeneratedSettingWidgets();
    AssignedSettingsListBox = InVerticalBox;
    RebuildGeneratedSettingWidgets();
}

void USettingsMenuWidget::ClearGeneratedSettingWidgets()
{
    // Remove only rows we created. A custom Blueprint may also place headings/spacers in the host.
    for (USettingControlWidget* Row : GeneratedSettingWidgets)
    {
        if (IsValid(Row))
        {
            // Detached rows retained by an external graph must not keep editing this menu.
            Row->ConfigureSetting(nullptr, Row->GetSettingField());
            Row->RemoveFromParent();
        }
    }
    GeneratedSettingWidgets.Empty();
}

USettingControlWidget* USettingsMenuWidget::CreateGeneratedSettingWidget(ESettingsField Field)
{
    UClass* WidgetClass = nullptr;
    // Selecting a native class cannot trigger package IO or run a component Blueprint graph.
    switch (GetSettingControlType(Field))
    {
    case ESettingsControlType::Toggle: WidgetClass = UBooleanSettingWidget::StaticClass(); break;
    case ESettingsControlType::Slider: WidgetClass = UFloatSettingWidget::StaticClass(); break;
    case ESettingsControlType::Dropdown: WidgetClass = UEnumSettingWidget::StaticClass(); break;
    default: return nullptr;
    }

    return Cast<USettingControlWidget>(
        UUserWidget::CreateWidgetInstance(*this, WidgetClass, NAME_None));
}

void USettingsMenuWidget::RebuildGeneratedSettingWidgets()
{
    if (IsUsingNativeLayout() || IsDesignTime()) return;
    if (!AssignedSettingsListBox.IsValid())
        AssignedSettingsListBox = Cast<UVerticalBox>(GetWidgetFromName(TEXT("SettingsList")));
    UVerticalBox* Host = AssignedSettingsListBox.Get();
    if (!IsValid(Host))
    {
        FSimulatorFileServices::WriteLogAsync(TEXT("UI"), FString::Printf(
            TEXT("Settings host missing. Widget=%s Root=%s"), *GetClass()->GetPathName(), *GetNameSafe(GetRootWidget())));
        return;
    }

    ClearGeneratedSettingWidgets();

    for (ESettingsField Field : GetDefaultSettingsFields())
    {
        USettingControlWidget* Row = CreateGeneratedSettingWidget(Field);
        if (!IsValid(Row))
        {
            const ESettingsControlType ControlType = GetSettingControlType(Field);
            UE_LOG(LogTemp, Verbose,
                TEXT("Cannot create native settings row for field %s (control type %d)."),
                *FieldName(Field).ToString(), static_cast<int32>(ControlType));
            continue;
        }

        // Supply metadata before the row's Slate tree and Construct events can run.
        Row->ApplyControlTheme(FontObject, FontOutlineSize, TextColor, ButtonColor, HoveredButtonTint, ButtonHeight);
        Row->ConfigureSetting(this, Field);
        Host->AddChildToVerticalBox(Row);
        GeneratedSettingWidgets.Add(Row);
    }
    FSimulatorFileServices::WriteLogAsync(TEXT("UI"), FString::Printf(
        TEXT("Settings rows=%d Host=%s"), GeneratedSettingWidgets.Num(), *GetNameSafe(Host)));
}

void USettingsMenuWidget::RefreshGeneratedSettingWidgets()
{
    for (USettingControlWidget* Widget : GeneratedSettingWidgets)
    {
        if (IsValid(Widget))
        {
            Widget->RefreshSettingWidget();
        }
    }
}

void USettingsMenuWidget::RegisterSettingValueText(ESettingsField Field, UTextBlock* InTextBlock)
{
    if (!AcceptWidgetAssignment(InTextBlock)) return;
    if (IsAutomaticStreamingField(Field))
    {
        if (IsValid(InTextBlock)) InTextBlock->SetVisibility(ESlateVisibility::Collapsed);
        RegisteredValueTextWidgets.Remove(Field);
        return;
    }
    if (IsValid(InTextBlock))
    {
        RegisteredValueTextWidgets.FindOrAdd(Field) = InTextBlock;
    }
    else
    {
        RegisteredValueTextWidgets.Remove(Field);
    }

    CollectAssignedWidgetReferences();
    RefreshSettingsValues();
}

void USettingsMenuWidget::RegisterSettingButton(ESettingsField Field, UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (IsAutomaticStreamingField(Field))
    {
        if (IsValid(InButton)) InButton->SetVisibility(ESlateVisibility::Collapsed);
        RegisteredSettingButtons.Remove(Field);
        return;
    }
    if (TWeakObjectPtr<UButton>* ExistingButtonPtr = RegisteredSettingButtons.Find(Field))
    {
        if (UButton* ExistingButton = ExistingButtonPtr->Get())
        {
            UnbindFieldButton(Field, ExistingButton);
            BoundFieldButtons.Remove(TWeakObjectPtr<UButton>(ExistingButton));
        }
    }

    if (IsValid(InButton))
    {
        RegisteredSettingButtons.FindOrAdd(Field) = InButton;
        BindFieldButton(Field, InButton);
    }
    else
    {
        RegisteredSettingButtons.Remove(Field);
    }

    RefreshSettingsValues();
}

void USettingsMenuWidget::RegisterSettingSlider(ESettingsField Field, USlider* InSlider)
{
    if (!AcceptWidgetAssignment(InSlider)) return;
    if (IsAutomaticStreamingField(Field))
    {
        if (IsValid(InSlider)) InSlider->SetVisibility(ESlateVisibility::Collapsed);
        RemoveControlBinding(Field);
        return;
    }
    RemoveControlBinding(Field);
    if (!IsValid(InSlider))
    {
        return;
    }
    if (GetSettingControlType(Field) != ESettingsControlType::Slider)
    {
        UE_LOG(LogTemp, Warning, TEXT("Settings field %s is not a slider field."), *FieldName(Field).ToString());
        return;
    }

    float MinValue = 0.0f;
    float MaxValue = 1.0f;
    float Step = 0.01f;
    if (!GetSettingSliderRange(Field, MinValue, MaxValue, Step))
    {
        return;
    }
    InSlider->SetMinValue(MinValue);
    InSlider->SetMaxValue(MaxValue);
    InSlider->SetStepSize(Step);

    USettingsControlBinding* Binding = NewObject<USettingsControlBinding>(this);
    if (IsValid(Binding))
    {
        Binding->BindSlider(this, Field, InSlider);
        ControlBindings.Add(Binding);
    }
    RefreshRegisteredControls();
}

void USettingsMenuWidget::RegisterSettingDropdown(ESettingsField Field, UComboBoxString* InDropdown)
{
    if (!AcceptWidgetAssignment(InDropdown)) return;
    if (IsAutomaticStreamingField(Field))
    {
        if (IsValid(InDropdown)) InDropdown->SetVisibility(ESlateVisibility::Collapsed);
        RemoveControlBinding(Field);
        return;
    }
    RemoveControlBinding(Field);
    if (!IsValid(InDropdown))
    {
        return;
    }
    if (GetSettingControlType(Field) != ESettingsControlType::Dropdown)
    {
        UE_LOG(LogTemp, Warning, TEXT("Settings field %s is not a dropdown field."), *FieldName(Field).ToString());
        return;
    }

    InDropdown->ClearOptions();
    for (const FText& Option : GetSettingOptionTexts(Field))
    {
        InDropdown->AddOption(Option.ToString());
    }

    USettingsControlBinding* Binding = NewObject<USettingsControlBinding>(this);
    if (IsValid(Binding))
    {
        Binding->BindDropdown(this, Field, InDropdown);
        ControlBindings.Add(Binding);
    }
    RefreshRegisteredControls();
}

void USettingsMenuWidget::RegisterSettingToggleButton(ESettingsField Field, UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (IsAutomaticStreamingField(Field))
    {
        if (IsValid(InButton)) InButton->SetVisibility(ESlateVisibility::Collapsed);
        RemoveControlBinding(Field);
        return;
    }
    RemoveControlBinding(Field);
    if (!IsValid(InButton))
    {
        return;
    }
    if (GetSettingControlType(Field) != ESettingsControlType::Toggle)
    {
        UE_LOG(LogTemp, Warning, TEXT("Settings field %s is not a toggle field."), *FieldName(Field).ToString());
        return;
    }

    USettingsControlBinding* Binding = NewObject<USettingsControlBinding>(this);
    if (IsValid(Binding))
    {
        Binding->BindToggle(this, Field, InButton);
        ControlBindings.Add(Binding);
    }
    RefreshRegisteredControls();
}

void USettingsMenuWidget::UnregisterSettingControl(ESettingsField Field)
{
    RemoveControlBinding(Field);
}

ESettingsControlType USettingsMenuWidget::GetSettingControlType(ESettingsField Field) const
{
    switch (Field)
    {
    case ESettingsField::RayTracing:
    case ESettingsField::HeightFog:
    case ESettingsField::Cloud:
    case ESettingsField::CelShadingMode:
        return ESettingsControlType::Toggle;

    case ESettingsField::MouseSensitivity:
    case ESettingsField::BloomIntensity:
    case ESettingsField::BloomThreshold:
    case ESettingsField::AmbientOcclusionIntensity:
    case ESettingsField::StreamingDistanceMultiplier:
    case ESettingsField::StreamingUnloadDistanceMultiplier:
    case ESettingsField::ObjectStreamingRadiusMeters:
    case ESettingsField::StreamingSceneSpawnBudget:
    case ESettingsField::StreamingNodeBudgetPerFrame:
        return ESettingsControlType::Slider;

    default:
        return ESettingsControlType::Dropdown;
    }
}

bool USettingsMenuWidget::GetSettingSliderRange(
    ESettingsField Field, float& OutMin, float& OutMax, float& OutStep) const
{
    switch (Field)
    {
    case ESettingsField::MouseSensitivity:
        OutMin = 0.05f; OutMax = 5.0f; OutStep = 0.05f; return true;
    case ESettingsField::BloomIntensity:
        OutMin = BloomIntensityMin; OutMax = BloomIntensityMax; OutStep = 0.01f; return true;
    case ESettingsField::BloomThreshold:
        OutMin = BloomThresholdMin; OutMax = BloomThresholdMax; OutStep = 0.01f; return true;
    case ESettingsField::AmbientOcclusionIntensity:
        OutMin = AmbientOcclusionMin; OutMax = AmbientOcclusionMax; OutStep = 0.01f; return true;
    case ESettingsField::StreamingDistanceMultiplier:
        OutMin = StreamingDistanceMin; OutMax = StreamingDistanceMax; OutStep = StreamingDistanceStep; return true;
    case ESettingsField::StreamingUnloadDistanceMultiplier:
        OutMin = StreamingUnloadMin; OutMax = StreamingUnloadMax; OutStep = StreamingUnloadStep; return true;
    case ESettingsField::ObjectStreamingRadiusMeters:
        OutMin = ObjectRadiusMin; OutMax = ObjectRadiusMax; OutStep = ObjectRadiusStep; return true;
    case ESettingsField::StreamingSceneSpawnBudget:
        OutMin = SceneSpawnBudgetMin; OutMax = SceneSpawnBudgetMax; OutStep = SceneSpawnBudgetStep; return true;
    case ESettingsField::StreamingNodeBudgetPerFrame:
        OutMin = NodeBudgetMin; OutMax = NodeBudgetMax; OutStep = NodeBudgetStep; return true;
    default:
        OutMin = 0.0f; OutMax = 1.0f; OutStep = 0.01f; return false;
    }
}


bool USettingsMenuWidget::GetPendingBooleanSettingValue(ESettingsField Field) const
{
    switch (Field)
    {
    case ESettingsField::RayTracing: return bPendingRayTracing;
    case ESettingsField::HeightFog: return bPendingHeightFog;
    case ESettingsField::Cloud: return bPendingCloud;
    case ESettingsField::CelShadingMode: return PendingCelShadingMode >= 0.5f;
    default: return false;
    }
}

float USettingsMenuWidget::GetPendingNumericSettingValue(ESettingsField Field) const
{
    return GetPendingNumericValue(Field);
}

void USettingsMenuWidget::SetSettingFromSliderValue(ESettingsField Field, const float Value)
{
    if (bRefreshingControls || !FMath::IsFinite(Value)) return;
    switch (Field)
    {
    case ESettingsField::MouseSensitivity:
        PendingMouseSensitivity = FMath::Clamp(Value, 0.05f, 5.0f); break;
    case ESettingsField::BloomIntensity:
        PendingBloomIntensity = FMath::Clamp(Value, BloomIntensityMin, BloomIntensityMax); break;
    case ESettingsField::BloomThreshold:
        PendingBloomThreshold = FMath::Clamp(Value, BloomThresholdMin, BloomThresholdMax); break;
    case ESettingsField::AmbientOcclusionIntensity:
        PendingAmbientOcclusionIntensity = FMath::Clamp(Value, AmbientOcclusionMin, AmbientOcclusionMax); break;
    case ESettingsField::StreamingDistanceMultiplier:
        PendingStreamingDistanceMultiplier = FMath::Clamp(Value, StreamingDistanceMin, StreamingDistanceMax); break;
    case ESettingsField::StreamingUnloadDistanceMultiplier:
        PendingStreamingUnloadDistanceMultiplier = FMath::Clamp(Value, StreamingUnloadMin, StreamingUnloadMax); break;
    case ESettingsField::ObjectStreamingRadiusMeters:
        PendingObjectStreamingRadiusMeters = FMath::Clamp(Value, ObjectRadiusMin, ObjectRadiusMax); break;
    case ESettingsField::StreamingSceneSpawnBudget:
        PendingStreamingSceneSpawnBudget = FMath::Clamp(FMath::RoundToInt(Value), 1, 32); break;
    case ESettingsField::StreamingNodeBudgetPerFrame:
        PendingStreamingNodeBudgetPerFrame = FMath::Clamp(FMath::RoundToInt(Value), 1, 256); break;
    default:
        return;
    }
    if (Field == ESettingsField::MouseSensitivity)
    {
        // Reading the multiplier during Input_Look makes a drag immediately visible without
        // reapplying all render settings and rebuilding streaming budgets for each mouse event.
        if (!IsDesignTime())
        {
            if (UGameSettings* Settings = GetEditableSettings())
            {
                Settings->MouseSensitivity = PendingMouseSensitivity;
                Settings->SaveSettingsData();
            }
        }
        RefreshSettingsValues();
    }
    else ApplyAndSaveSettingsFromUI();
}

void USettingsMenuWidget::SetSettingFromDropdownSelection(
    ESettingsField Field, const FString& SelectedOption)
{
    if (bRefreshingControls) return;
    const TArray<FText> Options = GetSettingOptionTexts(Field);
    int32 SelectedIndex = INDEX_NONE;
    for (int32 Index = 0; Index < Options.Num(); ++Index)
    {
        if (Options[Index].ToString().Equals(SelectedOption, ESearchCase::IgnoreCase))
        {
            SelectedIndex = Index;
            break;
        }
    }
    if (SelectedIndex == INDEX_NONE) return;

    switch (Field)
    {
    case ESettingsField::Exposure:
        PendingExposure = SelectedIndex == 0
            ? ExposureAutoValue
            : FMath::Clamp(ExposureManualMin + SelectedIndex - 1, ExposureManualMin, ExposureManualMax);
        break;
    case ESettingsField::MaxTextureResolution:
        {
            static const int32 Values[] = {256, 512, 768, 1024, 1536, 2048, 4096};
            if (SelectedIndex >= 0 && SelectedIndex < static_cast<int32>(UE_ARRAY_COUNT(Values)))
                PendingMaxTextureResolution = Values[SelectedIndex];
        }
        break;
    case ESettingsField::DynamicGlobalIlluminationMethod:
        PendingDynamicGlobalIlluminationMethod = FMath::Clamp(SelectedIndex, 0, 3); break;
    case ESettingsField::ReflectionMethod:
        PendingReflectionMethod = FMath::Clamp(SelectedIndex, 0, 2); break;
    case ESettingsField::ShadowQuality: PendingShadowQuality = FMath::Clamp(SelectedIndex, 0, 3); break;
    case ESettingsField::TextureQuality: PendingTextureQuality = FMath::Clamp(SelectedIndex, 0, 3); break;
    case ESettingsField::QualityProfile:
        if (SelectedIndex >= 4) return;
        SetPendingQualityProfile(SelectedIndex); break;
    case ESettingsField::ViewDistanceQuality: PendingViewDistanceQuality = FMath::Clamp(SelectedIndex, 0, 3); break;
    case ESettingsField::MaxRenderDistanceMeters: PendingMaxRenderDistanceMeters = (FMath::Clamp(SelectedIndex, 0, 31) + 1) * 1024; break;
    case ESettingsField::AntiAliasingQuality: PendingAntiAliasingQuality = FMath::Clamp(SelectedIndex, 0, 3); break;
    case ESettingsField::PostProcessingQuality: PendingPostProcessingQuality = FMath::Clamp(SelectedIndex, 0, 3); break;
    case ESettingsField::EffectsQuality: PendingEffectsQuality = FMath::Clamp(SelectedIndex, 0, 3); break;
    case ESettingsField::FoliageQuality: PendingFoliageQuality = FMath::Clamp(SelectedIndex, 0, 3); break;
    case ESettingsField::ShadingQuality: PendingShadingQuality = FMath::Clamp(SelectedIndex, 0, 3); break;
    case ESettingsField::GlobalIlluminationQuality: PendingGlobalIlluminationQuality = FMath::Clamp(SelectedIndex, 0, 3); break;
    case ESettingsField::ReflectionQuality: PendingReflectionQuality = FMath::Clamp(SelectedIndex, 0, 3); break;
    default: return;
    }
    ApplyAndSaveSettingsFromUI();
}

void USettingsMenuWidget::ToggleSettingFromUI(ESettingsField Field)
{
    if (bRefreshingControls) return;
    switch (Field)
    {
    case ESettingsField::RayTracing: bPendingRayTracing = !bPendingRayTracing; break;
    case ESettingsField::HeightFog: bPendingHeightFog = !bPendingHeightFog; break;
    case ESettingsField::Cloud: bPendingCloud = !bPendingCloud; break;
    case ESettingsField::CelShadingMode: PendingCelShadingMode = PendingCelShadingMode >= 0.5f ? 0.0f : 1.0f; break;
    default: return;
    }
    ApplyAndSaveSettingsFromUI();
}

UGameSettings* USettingsMenuWidget::GetEditableSettings() const
{
    if (IsDesignTime()) return nullptr;
    if (UGameManagerSubSystem* SubSystem = UGameManagerSubSystem::GetSubSystem(GetWorld()))
    {
        return SubSystem->GetGameSettings();
    }
    return nullptr;
}

void USettingsMenuWidget::InitializeSettingsFromSavedData()
{
    CopySettingsToPending(GetEditableSettings());
    if (GeneratedSettingWidgets.IsEmpty()) RebuildGeneratedSettingWidgets();
    RefreshSettingsValues();
}

void USettingsMenuWidget::CollectAssignedWidgetReferences()
{
    ValueTextBlocks.Reset();
    ValueFields.Reset();

    for (ESettingsField Field : GetDefaultSettingsFields())
    {
        if (TWeakObjectPtr<UTextBlock>* TextBlockPtr = RegisteredValueTextWidgets.Find(Field))
        {
            RegisterValueTextBlock(Field, TextBlockPtr->Get());
        }
    }
}

void USettingsMenuWidget::RegisterValueTextBlock(ESettingsField Field, UTextBlock* TextBlock)
{
    if (!IsValid(TextBlock))
    {
        return;
    }

    ValueTextBlocks.Add(TextBlock);
    ValueFields.Add(Field);
}

void USettingsMenuWidget::BindButtonEvents()
{
    if (UButton* Button = GetApplyButton())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ApplyAndSaveSettingsFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::ApplyAndSaveSettingsFromUI);
    }
    if (UButton* Button = GetConfirmButton())
    {
        if (Button != GetApplyButton())
        {
            Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ConfirmSettingsFromUI);
            Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::ConfirmSettingsFromUI);
        }
    }
    if (UButton* Button = GetBackButton())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
    }
    if (UButton* Button = GetCancelButton())
    {
        if (Button != GetBackButton())
        {
            Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
            Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
        }
    }

    BindAssignedSettingButtons();
}

void USettingsMenuWidget::UnbindButtonEvents()
{
    if (UButton* Button = GetApplyButton())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ApplyAndSaveSettingsFromUI);
    }
    if (UButton* Button = GetConfirmButton())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ConfirmSettingsFromUI);
    }
    if (UButton* Button = GetBackButton())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
    }
    if (UButton* Button = GetCancelButton())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
    }

    for (const TPair<TWeakObjectPtr<UButton>, ESettingsField>& Pair : BoundFieldButtons)
    {
        if (UButton* Button = Pair.Key.Get())
        {
            UnbindFieldButton(Pair.Value, Button);
        }
    }
}

void USettingsMenuWidget::BindAssignedSettingButtons()
{
    BoundFieldButtons.Empty();

    for (ESettingsField Field : GetDefaultSettingsFields())
    {
        if (TWeakObjectPtr<UButton>* ButtonPtr = RegisteredSettingButtons.Find(Field))
        {
            BindAssignedSettingButton(Field, ButtonPtr->Get());
        }
    }
}

void USettingsMenuWidget::BindAssignedSettingButton(ESettingsField Field, UButton* Button)
{
    BindFieldButton(Field, Button);
}

void USettingsMenuWidget::BindFieldButton(ESettingsField Field, UButton* Button)
{
    if (!IsValid(Button) || Button == GetApplyButton() || Button == GetConfirmButton() || Button == GetBackButton() || Button == GetCancelButton())
    {
        return;
    }
    if (IsAutomaticStreamingField(Field))
    {
        Button->SetVisibility(ESlateVisibility::Collapsed);
        return;
    }

    if (USettingsCycleButton* CycleButton = Cast<USettingsCycleButton>(Button))
    {
        CycleButton->SetupCycleButton(this, Field, CycleButton->Direction);
        BoundFieldButtons.Add(TWeakObjectPtr<UButton>(Button), Field);
        return;
    }

    switch (Field)
    {
    case ESettingsField::BloomIntensity:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleBloomIntensityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleBloomIntensityFromUI);
        break;
    case ESettingsField::BloomThreshold:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleBloomThresholdFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleBloomThresholdFromUI);
        break;
    case ESettingsField::AmbientOcclusionIntensity:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleAmbientOcclusionIntensityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleAmbientOcclusionIntensityFromUI);
        break;
    case ESettingsField::Exposure:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleExposureFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleExposureFromUI);
        break;
    case ESettingsField::RayTracing:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleRayTracingFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleRayTracingFromUI);
        break;
    case ESettingsField::HeightFog:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleHeightFogFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleHeightFogFromUI);
        break;
    case ESettingsField::Cloud:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleCloudFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleCloudFromUI);
        break;
    case ESettingsField::CelShadingMode:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleCelShadingModeFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleCelShadingModeFromUI);
        break;
    case ESettingsField::ShadowQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleShadowQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleShadowQualityFromUI);
        break;
    case ESettingsField::TextureQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleTextureQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleTextureQualityFromUI);
        break;
    case ESettingsField::MaxTextureResolution:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleMaxTextureResolutionFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleMaxTextureResolutionFromUI);
        break;
    case ESettingsField::QualityProfile:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleQualityProfileFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleQualityProfileFromUI);
        break;
    case ESettingsField::MaxRenderDistanceMeters:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleMaxRenderDistanceMetersFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleMaxRenderDistanceMetersFromUI);
        break;
    case ESettingsField::ViewDistanceQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleViewDistanceQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleViewDistanceQualityFromUI);
        break;
    case ESettingsField::StreamingDistanceMultiplier:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleStreamingDistanceMultiplierFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleStreamingDistanceMultiplierFromUI);
        break;
    case ESettingsField::StreamingUnloadDistanceMultiplier:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleStreamingUnloadDistanceMultiplierFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleStreamingUnloadDistanceMultiplierFromUI);
        break;
    case ESettingsField::ObjectStreamingRadiusMeters:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleObjectStreamingRadiusMetersFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleObjectStreamingRadiusMetersFromUI);
        break;
    case ESettingsField::StreamingSceneSpawnBudget:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleStreamingSceneSpawnBudgetFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleStreamingSceneSpawnBudgetFromUI);
        break;
    case ESettingsField::StreamingNodeBudgetPerFrame:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleStreamingNodeBudgetPerFrameFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleStreamingNodeBudgetPerFrameFromUI);
        break;
    case ESettingsField::AntiAliasingQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleAntiAliasingQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleAntiAliasingQualityFromUI);
        break;
    case ESettingsField::PostProcessingQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CyclePostProcessingQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CyclePostProcessingQualityFromUI);
        break;
    case ESettingsField::EffectsQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleEffectsQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleEffectsQualityFromUI);
        break;
    case ESettingsField::FoliageQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleFoliageQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleFoliageQualityFromUI);
        break;
    case ESettingsField::ShadingQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleShadingQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleShadingQualityFromUI);
        break;
    case ESettingsField::GlobalIlluminationQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleGlobalIlluminationQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleGlobalIlluminationQualityFromUI);
        break;
    case ESettingsField::ReflectionQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleReflectionQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleReflectionQualityFromUI);
        break;
    case ESettingsField::DynamicGlobalIlluminationMethod:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleDynamicGlobalIlluminationMethodFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleDynamicGlobalIlluminationMethodFromUI);
        break;
    case ESettingsField::ReflectionMethod:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleReflectionMethodFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleReflectionMethodFromUI);
        break;
    default: break;
    }

    BoundFieldButtons.Add(TWeakObjectPtr<UButton>(Button), Field);
}

void USettingsMenuWidget::UnbindFieldButton(ESettingsField Field, UButton* Button)
{
    if (!IsValid(Button))
    {
        return;
    }

    if (USettingsCycleButton* CycleButton = Cast<USettingsCycleButton>(Button))
    {
        CycleButton->OnClicked.RemoveDynamic(CycleButton, &USettingsCycleButton::HandleClicked);
        return;
    }

    switch (Field)
    {
    case ESettingsField::BloomIntensity: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleBloomIntensityFromUI); break;
    case ESettingsField::BloomThreshold: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleBloomThresholdFromUI); break;
    case ESettingsField::AmbientOcclusionIntensity: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleAmbientOcclusionIntensityFromUI); break;
    case ESettingsField::Exposure: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleExposureFromUI); break;
    case ESettingsField::RayTracing: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleRayTracingFromUI); break;
    case ESettingsField::HeightFog: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleHeightFogFromUI); break;
    case ESettingsField::Cloud: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleCloudFromUI); break;
    case ESettingsField::CelShadingMode: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleCelShadingModeFromUI); break;
    case ESettingsField::ShadowQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleShadowQualityFromUI); break;
    case ESettingsField::TextureQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleTextureQualityFromUI); break;
    case ESettingsField::MaxTextureResolution: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleMaxTextureResolutionFromUI); break;
    case ESettingsField::QualityProfile: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleQualityProfileFromUI); break;
    case ESettingsField::MaxRenderDistanceMeters: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleMaxRenderDistanceMetersFromUI); break;
    case ESettingsField::ViewDistanceQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleViewDistanceQualityFromUI); break;
    case ESettingsField::StreamingDistanceMultiplier: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleStreamingDistanceMultiplierFromUI); break;
    case ESettingsField::StreamingUnloadDistanceMultiplier: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleStreamingUnloadDistanceMultiplierFromUI); break;
    case ESettingsField::ObjectStreamingRadiusMeters: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleObjectStreamingRadiusMetersFromUI); break;
    case ESettingsField::StreamingSceneSpawnBudget: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleStreamingSceneSpawnBudgetFromUI); break;
    case ESettingsField::StreamingNodeBudgetPerFrame: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleStreamingNodeBudgetPerFrameFromUI); break;
    case ESettingsField::AntiAliasingQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleAntiAliasingQualityFromUI); break;
    case ESettingsField::PostProcessingQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CyclePostProcessingQualityFromUI); break;
    case ESettingsField::EffectsQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleEffectsQualityFromUI); break;
    case ESettingsField::FoliageQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleFoliageQualityFromUI); break;
    case ESettingsField::ShadingQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleShadingQualityFromUI); break;
    case ESettingsField::GlobalIlluminationQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleGlobalIlluminationQualityFromUI); break;
    case ESettingsField::ReflectionQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleReflectionQualityFromUI); break;
    case ESettingsField::DynamicGlobalIlluminationMethod: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleDynamicGlobalIlluminationMethodFromUI); break;
    case ESettingsField::ReflectionMethod: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleReflectionMethodFromUI); break;
    default: break;
    }
}

void USettingsMenuWidget::RemoveControlBinding(ESettingsField Field)
{
    for (int32 Index = ControlBindings.Num() - 1; Index >= 0; --Index)
    {
        USettingsControlBinding* Binding = ControlBindings[Index];
        if (!IsValid(Binding) || Binding->GetField() == Field)
        {
            if (IsValid(Binding)) Binding->Unbind();
            ControlBindings.RemoveAtSwap(Index);
        }
    }
}

float USettingsMenuWidget::GetPendingNumericValue(ESettingsField Field) const
{
    switch (Field)
    {
    case ESettingsField::MouseSensitivity: return PendingMouseSensitivity;
    case ESettingsField::BloomIntensity: return PendingBloomIntensity;
    case ESettingsField::BloomThreshold: return PendingBloomThreshold;
    case ESettingsField::AmbientOcclusionIntensity: return PendingAmbientOcclusionIntensity;
    case ESettingsField::StreamingDistanceMultiplier: return PendingStreamingDistanceMultiplier;
    case ESettingsField::StreamingUnloadDistanceMultiplier: return PendingStreamingUnloadDistanceMultiplier;
    case ESettingsField::ObjectStreamingRadiusMeters: return PendingObjectStreamingRadiusMeters;
    case ESettingsField::StreamingSceneSpawnBudget: return static_cast<float>(PendingStreamingSceneSpawnBudget);
    case ESettingsField::StreamingNodeBudgetPerFrame: return static_cast<float>(PendingStreamingNodeBudgetPerFrame);
    default: return 0.0f;
    }
}

void USettingsMenuWidget::RefreshRegisteredControls()
{
    TGuardValue<bool> RefreshGuard(bRefreshingControls, true);
    for (USettingsControlBinding* Binding : ControlBindings)
    {
        if (!IsValid(Binding)) continue;
        const ESettingsField Field = Binding->GetField();
        UObject* Control = Binding->GetControlObject();
        if (USlider* Slider = Cast<USlider>(Control))
        {
            const float Desired = GetPendingNumericValue(Field);
            if (!FMath::IsNearlyEqual(Slider->GetValue(), Desired, KINDA_SMALL_NUMBER))
            {
                Slider->SetValue(Desired);
            }
        }
        else if (UComboBoxString* Dropdown = Cast<UComboBoxString>(Control))
        {
            const FString Desired = GetPendingSettingValueText(Field).ToString();
            if (Dropdown->FindOptionIndex(Desired) != INDEX_NONE
                && Dropdown->GetSelectedOption() != Desired)
            {
                Dropdown->SetSelectedOption(Desired);
            }
        }
        else if (UButton* Button = Cast<UButton>(Control))
        {
            SetButtonTextFromUI(Button, IsUsingNativeLayout() ? GetPendingSettingValueText(Field) : GetSettingButtonText(Field));
        }
    }
}

void USettingsMenuWidget::CopySettingsToPending(const UGameSettings* Settings)
{
    if (!Settings)
    {
        return;
    }

    PendingMouseSensitivity = Settings->GetClampedMouseSensitivity();
    PendingBloomIntensity = FMath::Clamp(Settings->BloomIntensity, BloomIntensityMin, BloomIntensityMax);
    PendingBloomThreshold = FMath::Clamp(Settings->BloomThreshold, BloomThresholdMin, BloomThresholdMax);
    PendingAmbientOcclusionIntensity = FMath::Clamp(Settings->AmbientOcclusionIntensity, AmbientOcclusionMin, AmbientOcclusionMax);
    PendingExposure = Settings->Exposure <= -10.5f
        ? ExposureAutoValue
        : FMath::Clamp(FMath::RoundToInt(Settings->Exposure), ExposureManualMin, ExposureManualMax);
    bPendingRayTracing = Settings->bRayTracing;
    bPendingHeightFog = Settings->bHeightFog;
    bPendingCloud = Settings->bCloud;
    PendingCelShadingMode = Settings->CelShadingMode >= 0.5f ? 1.0f : 0.0f;
    PendingShadowQuality = FMath::Clamp(Settings->ShadowQuality, QualityMin, QualityMax);
    PendingTextureQuality = FMath::Clamp(Settings->TextureQuality, QualityMin, QualityMax);
    PendingMaxTextureResolution = FMath::Clamp(Settings->MaxTextureResolution, TextureResolutionMin, TextureResolutionMax);
    PendingViewDistanceQuality = FMath::Clamp(Settings->ViewDistanceQuality, QualityMin, QualityMax);
    PendingMaxRenderDistanceMeters = Settings->GetClampedMaxRenderDistanceMeters();
    PendingAntiAliasingQuality = FMath::Clamp(Settings->AntiAliasingQuality, QualityMin, QualityMax);
    PendingPostProcessingQuality = FMath::Clamp(Settings->PostProcessingQuality, QualityMin, QualityMax);
    PendingEffectsQuality = FMath::Clamp(Settings->EffectsQuality, QualityMin, QualityMax);
    PendingFoliageQuality = FMath::Clamp(Settings->FoliageQuality, QualityMin, QualityMax);
    PendingShadingQuality = FMath::Clamp(Settings->ShadingQuality, QualityMin, QualityMax);
    PendingGlobalIlluminationQuality = FMath::Clamp(Settings->GlobalIlluminationQuality, QualityMin, QualityMax);
    PendingReflectionQuality = FMath::Clamp(Settings->ReflectionQuality, QualityMin, QualityMax);
    PendingDynamicGlobalIlluminationMethod = FMath::Clamp(Settings->DynamicGlobalIlluminationMethod, QualityMin, QualityMax);
    PendingReflectionMethod = FMath::Clamp(Settings->ReflectionMethod, ReflectionMethodMin, ReflectionMethodMax);
}

void USettingsMenuWidget::ApplyPendingToSettings(UGameSettings* Settings) const
{
    if (!Settings)
    {
        return;
    }

    Settings->MouseSensitivity = PendingMouseSensitivity;
    Settings->BloomIntensity = PendingBloomIntensity;
    Settings->BloomThreshold = PendingBloomThreshold;
    Settings->AmbientOcclusionIntensity = PendingAmbientOcclusionIntensity;
    Settings->Exposure = static_cast<float>(PendingExposure);
    Settings->bRayTracing = bPendingRayTracing;
    Settings->bHeightFog = bPendingHeightFog;
    Settings->bCloud = bPendingCloud;
    Settings->CelShadingMode = PendingCelShadingMode >= 0.5f ? 1.0f : 0.0f;
    Settings->ShadowQuality = PendingShadowQuality;
    Settings->TextureQuality = PendingTextureQuality;
    Settings->MaxTextureResolution = PendingMaxTextureResolution;
    Settings->ViewDistanceQuality = PendingViewDistanceQuality;
    Settings->MaxRenderDistanceMeters = PendingMaxRenderDistanceMeters;
    Settings->AntiAliasingQuality = PendingAntiAliasingQuality;
    Settings->PostProcessingQuality = PendingPostProcessingQuality;
    Settings->EffectsQuality = PendingEffectsQuality;
    Settings->FoliageQuality = PendingFoliageQuality;
    Settings->ShadingQuality = PendingShadingQuality;
    Settings->GlobalIlluminationQuality = PendingGlobalIlluminationQuality;
    Settings->ReflectionQuality = PendingReflectionQuality;
    Settings->DynamicGlobalIlluminationMethod = PendingDynamicGlobalIlluminationMethod;
    Settings->ReflectionMethod = PendingReflectionMethod;
}

void USettingsMenuWidget::RefreshSettingsValues()
{
    TGuardValue<bool> RefreshGuard(bRefreshingControls, true);
    for (int32 Index = 0; Index < ValueTextBlocks.Num() && Index < ValueFields.Num(); ++Index)
    {
        if (UTextBlock* TextBlock = ValueTextBlocks[Index].Get())
        {
            TextBlock->SetText(GetPendingSettingValueText(ValueFields[Index]));
        }
    }

    for (const TPair<TWeakObjectPtr<UButton>, ESettingsField>& Pair : BoundFieldButtons)
    {
        if (UButton* Button = Pair.Key.Get())
        {
            SetButtonTextFromUI(Button, GetSettingButtonText(Pair.Value));
        }
    }
    RefreshRegisteredControls();
    RefreshGeneratedSettingWidgets();
}

void USettingsMenuWidget::CycleSettingValueFromUI(ESettingsField Field, int32 Direction)
{
    if (bRefreshingControls) return;
    if (IsAutomaticStreamingField(Field)) return;
    CyclePendingValue(Field, Direction);
    ApplyAndSaveSettingsFromUI();
}

void USettingsMenuWidget::CycleSettingByNameFromUI(FName FieldNameValue, int32 Direction)
{
    ESettingsField Field = ESettingsField::BloomIntensity;
    if (TryMatchFieldName(FieldNameValue.ToString(), Field))
    {
        CycleSettingValueFromUI(Field, Direction);
    }
}

void USettingsMenuWidget::CycleSettingByButtonTextFromUI(UButton* SourceButton, int32 Direction)
{
    if (!SourceButton)
    {
        return;
    }

    ESettingsField Field = ESettingsField::BloomIntensity;
    if (TryGetSettingFieldFromButtonText(GetButtonTextFromUI(SourceButton), Field))
    {
        CycleSettingValueFromUI(Field, Direction);
    }
}

void USettingsMenuWidget::ApplyAndSaveSettingsFromUI()
{
    if (bRefreshingControls || IsDesignTime()) return;
    if (UGameManagerSubSystem* SubSystem = UGameManagerSubSystem::GetSubSystem(GetWorld()))
    {
        ApplyPendingToSettings(SubSystem->GetGameSettings());
        SubSystem->UpdateSettings();
    }

    RefreshSettingsValues();
}

void USettingsMenuWidget::ConfirmSettingsFromUI()
{
    ApplyAndSaveSettingsFromUI();
}

void USettingsMenuWidget::DiscardPendingSettingsFromUI()
{
    InitializeSettingsFromSavedData();
}

void USettingsMenuWidget::CloseSettingsFromUI()
{
    // Back/Cancel closes without committing pending changes. The owner decides whether to return
    // to the start menu or pause menu; both surfaces use this same widget contract.
    DiscardPendingSettingsFromUI();
    OnCloseRequested.Broadcast();
}

TArray<ESettingsField> USettingsMenuWidget::GetSettingFieldList() const
{
    return GetDefaultSettingsFields();
}

FText USettingsMenuWidget::GetSettingLabelText(ESettingsField Field) const
{
    switch (Field)
    {
    case ESettingsField::MouseSensitivity: return FText::FromString(TEXT("Mouse Sensitivity"));
    case ESettingsField::BloomIntensity: return FText::FromString(TEXT("Bloom Intensity"));
    case ESettingsField::BloomThreshold: return FText::FromString(TEXT("Bloom Threshold"));
    case ESettingsField::AmbientOcclusionIntensity: return FText::FromString(TEXT("Ambient Occlusion"));
    case ESettingsField::Exposure: return FText::FromString(TEXT("Exposure"));
    case ESettingsField::RayTracing: return FText::FromString(TEXT("Ray Tracing"));
    case ESettingsField::HeightFog: return FText::FromString(TEXT("Height Fog"));
    case ESettingsField::Cloud: return FText::FromString(TEXT("Cloud"));
    case ESettingsField::CelShadingMode: return FText::FromString(TEXT("Cel Shading"));
    case ESettingsField::ShadowQuality: return FText::FromString(TEXT("Shadow Quality"));
    case ESettingsField::TextureQuality: return FText::FromString(TEXT("Texture Quality"));
    case ESettingsField::MaxTextureResolution: return FText::FromString(TEXT("Max Texture Resolution"));
    case ESettingsField::ViewDistanceQuality: return FText::FromString(TEXT("Render Distance Quality"));
    case ESettingsField::MaxRenderDistanceMeters: return FText::FromString(TEXT("Max Render Distance"));
    case ESettingsField::QualityProfile: return FText::FromString(TEXT("Quality Profile"));
    case ESettingsField::StreamingDistanceMultiplier: return FText::FromString(TEXT("Streaming Distance Multiplier"));
    case ESettingsField::StreamingUnloadDistanceMultiplier: return FText::FromString(TEXT("Streaming Unload Multiplier"));
    case ESettingsField::ObjectStreamingRadiusMeters: return FText::FromString(TEXT("Object Streaming Radius"));
    case ESettingsField::StreamingSceneSpawnBudget: return FText::FromString(TEXT("Scene Spawn Budget"));
    case ESettingsField::StreamingNodeBudgetPerFrame: return FText::FromString(TEXT("Node Budget Per Frame"));
    case ESettingsField::AntiAliasingQuality: return FText::FromString(TEXT("Anti Aliasing"));
    case ESettingsField::PostProcessingQuality: return FText::FromString(TEXT("Post Processing"));
    case ESettingsField::EffectsQuality: return FText::FromString(TEXT("Effects"));
    case ESettingsField::FoliageQuality: return FText::FromString(TEXT("Foliage"));
    case ESettingsField::ShadingQuality: return FText::FromString(TEXT("Shading"));
    case ESettingsField::GlobalIlluminationQuality: return FText::FromString(TEXT("GI Quality"));
    case ESettingsField::ReflectionQuality: return FText::FromString(TEXT("Reflection Quality"));
    case ESettingsField::DynamicGlobalIlluminationMethod: return FText::FromString(TEXT("GI Method"));
    case ESettingsField::ReflectionMethod: return FText::FromString(TEXT("Reflection Method"));
    default: break;
    }
    return FText::FromString(TEXT("Unknown"));
}

FText USettingsMenuWidget::GetPendingSettingValueText(ESettingsField Field) const
{
    return GetFieldValueTextFromPending(Field);
}

FText USettingsMenuWidget::GetSettingButtonText(ESettingsField Field) const
{
    return FText::FromString(FString::Printf(TEXT("%s: %s"), *GetSettingLabelText(Field).ToString(), *GetPendingSettingValueText(Field).ToString()));
}

TArray<FText> USettingsMenuWidget::GetSettingOptionTexts(ESettingsField Field) const
{
    TArray<FText> Options;

    switch (Field)
    {
    case ESettingsField::MouseSensitivity:
        for (int32 Index = 1; Index <= 100; ++Index)
            Options.Add(FText::FromString(FString::Printf(TEXT("%.2fx"), Index * 0.05f)));
        break;
    case ESettingsField::BloomIntensity:
        for (float Value = BloomIntensityMin; Value <= BloomIntensityMax + KINDA_SMALL_NUMBER; Value += BloomStep)
        {
            Options.Add(FText::FromString(FString::Printf(TEXT("%.2f"), Value)));
        }
        break;
    case ESettingsField::BloomThreshold:
        for (float Value = BloomThresholdMin; Value <= BloomThresholdMax + KINDA_SMALL_NUMBER; Value += BloomStep)
        {
            Options.Add(FText::FromString(FString::Printf(TEXT("%.2f"), Value)));
        }
        break;
    case ESettingsField::AmbientOcclusionIntensity:
        for (float Value = AmbientOcclusionMin; Value <= AmbientOcclusionMax + KINDA_SMALL_NUMBER; Value += AmbientOcclusionStep)
        {
            Options.Add(FText::FromString(FString::Printf(TEXT("%.2f"), Value)));
        }
        break;
    case ESettingsField::Exposure:
        Options.Add(FText::FromString(TEXT("Auto")));
        for (int32 Value = ExposureManualMin; Value <= ExposureManualMax; ++Value)
        {
            Options.Add(FText::FromString(FString::Printf(TEXT("%d EV"), Value)));
        }
        break;
    case ESettingsField::RayTracing:
    case ESettingsField::HeightFog:
    case ESettingsField::Cloud:
    case ESettingsField::CelShadingMode:
        Options.Add(GetBoolText(false));
        Options.Add(GetBoolText(true));
        break;
    case ESettingsField::DynamicGlobalIlluminationMethod:
        for (int32 Value = QualityMin; Value <= QualityMax; ++Value)
        {
            Options.Add(GetDynamicGlobalIlluminationMethodText(Value));
        }
        break;
    case ESettingsField::ReflectionMethod:
        for (int32 Value = ReflectionMethodMin; Value <= ReflectionMethodMax; ++Value)
        {
            Options.Add(GetReflectionMethodText(Value));
        }
        break;
    case ESettingsField::QualityProfile:
        for (int32 Quality = 0; Quality <= 3; ++Quality) Options.Add(GetQualityText(Quality));
        Options.Add(FText::FromString(TEXT("Custom")));
        break;
    case ESettingsField::MaxRenderDistanceMeters:
        for (int32 Value = 1024; Value <= 32768; Value += 1024)
            Options.Add(FText::FromString(FString::Printf(TEXT("%d m"), Value)));
        break;
    case ESettingsField::MaxTextureResolution:
        {
            static const int32 OptionsPx[] = {256, 512, 768, 1024, 1536, 2048, 4096};
            for (const int32 Value : OptionsPx)
            {
                Options.Add(FText::FromString(FString::Printf(TEXT("%d px"), Value)));
            }
        }
        break;
    default:
        for (int32 Value = QualityMin; Value <= QualityMax; ++Value)
        {
            Options.Add(GetQualityText(Value));
        }
        break;
    }

    return Options;
}

FText USettingsMenuWidget::GetButtonTextFromUI(UButton* SourceButton) const
{
    if (!SourceButton)
    {
        return FText::GetEmpty();
    }

    if (UTextBlock* TextBlock = Cast<UTextBlock>(SourceButton->GetContent()))
    {
        return TextBlock->GetText();
    }

    return FText::GetEmpty();
}

bool USettingsMenuWidget::SetButtonTextFromUI(UButton* SourceButton, const FText& NewText) const
{
    if (!SourceButton)
    {
        return false;
    }

    if (UTextBlock* TextBlock = Cast<UTextBlock>(SourceButton->GetContent()))
    {
        TextBlock->SetText(NewText);
        return true;
    }

    return false;
}

bool USettingsMenuWidget::TryGetSettingFieldFromButtonText(const FText& ButtonText, ESettingsField& OutField) const
{
    return TryMatchFieldName(StripValuePart(ButtonText.ToString()), OutField);
}

void USettingsMenuWidget::CyclePendingValue(ESettingsField Field, int32 Direction)
{
    switch (Field)
    {
    case ESettingsField::MouseSensitivity: CycleFloat(PendingMouseSensitivity, 0.05f, 5.0f, 0.05f, Direction); break;
    case ESettingsField::BloomIntensity: CycleFloat(PendingBloomIntensity, BloomIntensityMin, BloomIntensityMax, BloomStep, Direction); break;
    case ESettingsField::BloomThreshold: CycleFloat(PendingBloomThreshold, BloomThresholdMin, BloomThresholdMax, BloomStep, Direction); break;
    case ESettingsField::AmbientOcclusionIntensity: CycleFloat(PendingAmbientOcclusionIntensity, AmbientOcclusionMin, AmbientOcclusionMax, AmbientOcclusionStep, Direction); break;
    case ESettingsField::Exposure: CycleInt(PendingExposure, ExposureAutoValue, ExposureManualMax, Direction); break;
    case ESettingsField::RayTracing: bPendingRayTracing = !bPendingRayTracing; break;
    case ESettingsField::HeightFog: bPendingHeightFog = !bPendingHeightFog; break;
    case ESettingsField::Cloud: bPendingCloud = !bPendingCloud; break;
    case ESettingsField::CelShadingMode: PendingCelShadingMode = PendingCelShadingMode >= 0.5f ? 0.0f : 1.0f; break;
    case ESettingsField::ShadowQuality: CycleInt(PendingShadowQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::TextureQuality: CycleInt(PendingTextureQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::MaxTextureResolution: CycleTextureResolution(PendingMaxTextureResolution, Direction); break;
    case ESettingsField::QualityProfile:
        {
            int32 Quality = GetPendingQualityProfileIndex();
            if (Quality == INDEX_NONE) Quality = Direction < 0 ? 0 : -1;
            CycleInt(Quality, 0, 3, Direction);
            SetPendingQualityProfile(Quality);
        }
        break;
    case ESettingsField::ViewDistanceQuality: CycleInt(PendingViewDistanceQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::MaxRenderDistanceMeters:
        {
            int32 Step = PendingMaxRenderDistanceMeters / 1024;
            CycleInt(Step, 1, 32, Direction);
            PendingMaxRenderDistanceMeters = Step * 1024;
        }
        break;
    case ESettingsField::StreamingDistanceMultiplier: CycleFloat(PendingStreamingDistanceMultiplier, StreamingDistanceMin, StreamingDistanceMax, StreamingDistanceStep, Direction); break;
    case ESettingsField::StreamingUnloadDistanceMultiplier: CycleFloat(PendingStreamingUnloadDistanceMultiplier, StreamingUnloadMin, StreamingUnloadMax, StreamingUnloadStep, Direction); break;
    case ESettingsField::ObjectStreamingRadiusMeters: CycleFloat(PendingObjectStreamingRadiusMeters, ObjectRadiusMin, ObjectRadiusMax, ObjectRadiusStep, Direction); break;
    case ESettingsField::StreamingSceneSpawnBudget: CycleInt(PendingStreamingSceneSpawnBudget, 1, 32, Direction); break;
    case ESettingsField::StreamingNodeBudgetPerFrame: CycleInt(PendingStreamingNodeBudgetPerFrame, 1, 256, Direction); break;
    case ESettingsField::AntiAliasingQuality: CycleInt(PendingAntiAliasingQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::PostProcessingQuality: CycleInt(PendingPostProcessingQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::EffectsQuality: CycleInt(PendingEffectsQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::FoliageQuality: CycleInt(PendingFoliageQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::ShadingQuality: CycleInt(PendingShadingQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::GlobalIlluminationQuality: CycleInt(PendingGlobalIlluminationQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::ReflectionQuality: CycleInt(PendingReflectionQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::DynamicGlobalIlluminationMethod: CycleInt(PendingDynamicGlobalIlluminationMethod, QualityMin, QualityMax, Direction); break;
    case ESettingsField::ReflectionMethod: CycleInt(PendingReflectionMethod, ReflectionMethodMin, ReflectionMethodMax, Direction); break;
    default: break;
    }
}

bool USettingsMenuWidget::TryMatchFieldName(const FString& Input, ESettingsField& OutField) const
{
    const FString NormalizedInput = NormalizeFieldText(StripValuePart(Input));
    for (ESettingsField Field : GetDefaultSettingsFields())
    {
        const FString Label = NormalizeFieldText(GetSettingLabelText(Field).ToString());
        const FString Name = NormalizeFieldText(FieldName(Field).ToString());
        if (NormalizedInput == Label || NormalizedInput == Name)
        {
            OutField = Field;
            return true;
        }
    }
    return false;
}

FText USettingsMenuWidget::GetFieldValueTextFromPending(ESettingsField Field) const
{
    switch (Field)
    {
    case ESettingsField::MouseSensitivity: return FText::FromString(FString::Printf(TEXT("%.2fx"), PendingMouseSensitivity));
    case ESettingsField::BloomIntensity: return FText::FromString(FString::Printf(TEXT("%.2f"), PendingBloomIntensity));
    case ESettingsField::BloomThreshold: return FText::FromString(FString::Printf(TEXT("%.2f"), PendingBloomThreshold));
    case ESettingsField::AmbientOcclusionIntensity: return FText::FromString(FString::Printf(TEXT("%.2f"), PendingAmbientOcclusionIntensity));
    case ESettingsField::Exposure:
        return PendingExposure == ExposureAutoValue
            ? FText::FromString(TEXT("Auto"))
            : FText::FromString(FString::Printf(TEXT("%d EV"), PendingExposure));
    case ESettingsField::RayTracing: return GetBoolText(bPendingRayTracing);
    case ESettingsField::HeightFog: return GetBoolText(bPendingHeightFog);
    case ESettingsField::Cloud: return GetBoolText(bPendingCloud);
    case ESettingsField::CelShadingMode: return GetBoolText(PendingCelShadingMode >= 0.5f);
    case ESettingsField::ShadowQuality: return GetQualityText(PendingShadowQuality);
    case ESettingsField::TextureQuality: return GetQualityText(PendingTextureQuality);
    case ESettingsField::MaxTextureResolution: return FText::FromString(FString::Printf(TEXT("%d px"), PendingMaxTextureResolution));
    case ESettingsField::QualityProfile:
        {
            const int32 Profile = GetPendingQualityProfileIndex();
            return Profile == INDEX_NONE ? FText::FromString(TEXT("Custom")) : GetQualityText(Profile);
        }
    case ESettingsField::ViewDistanceQuality: return GetQualityText(PendingViewDistanceQuality);
    case ESettingsField::MaxRenderDistanceMeters: return FText::FromString(FString::Printf(TEXT("%d m"), PendingMaxRenderDistanceMeters));
    case ESettingsField::StreamingDistanceMultiplier: return FText::FromString(FString::Printf(TEXT("%.0fx"), PendingStreamingDistanceMultiplier));
    case ESettingsField::StreamingUnloadDistanceMultiplier: return FText::FromString(FString::Printf(TEXT("%.2fx"), PendingStreamingUnloadDistanceMultiplier));
    case ESettingsField::ObjectStreamingRadiusMeters: return FText::FromString(FString::Printf(TEXT("%.0f m"), PendingObjectStreamingRadiusMeters));
    case ESettingsField::StreamingSceneSpawnBudget: return FText::AsNumber(PendingStreamingSceneSpawnBudget);
    case ESettingsField::StreamingNodeBudgetPerFrame: return FText::AsNumber(PendingStreamingNodeBudgetPerFrame);
    case ESettingsField::AntiAliasingQuality: return GetQualityText(PendingAntiAliasingQuality);
    case ESettingsField::PostProcessingQuality: return GetQualityText(PendingPostProcessingQuality);
    case ESettingsField::EffectsQuality: return GetQualityText(PendingEffectsQuality);
    case ESettingsField::FoliageQuality: return GetQualityText(PendingFoliageQuality);
    case ESettingsField::ShadingQuality: return GetQualityText(PendingShadingQuality);
    case ESettingsField::GlobalIlluminationQuality: return GetQualityText(PendingGlobalIlluminationQuality);
    case ESettingsField::ReflectionQuality: return GetQualityText(PendingReflectionQuality);
    case ESettingsField::DynamicGlobalIlluminationMethod: return GetDynamicGlobalIlluminationMethodText(PendingDynamicGlobalIlluminationMethod);
    case ESettingsField::ReflectionMethod: return GetReflectionMethodText(PendingReflectionMethod);
    default: break;
    }
    return FText::FromString(TEXT("-"));
}

FText USettingsMenuWidget::GetQualityText(int32 Value) const
{
    switch (FMath::Clamp(Value, QualityMin, QualityMax))
    {
    case 0: return FText::FromString(TEXT("Low"));
    case 1: return FText::FromString(TEXT("Medium"));
    case 2: return FText::FromString(TEXT("High"));
    case 3: return FText::FromString(TEXT("Epic"));
    default: break;
    }
    return FText::AsNumber(Value);
}

FText USettingsMenuWidget::GetBoolText(bool bValue) const
{
    return FText::FromString(bValue ? TEXT("On") : TEXT("Off"));
}

FText USettingsMenuWidget::GetDynamicGlobalIlluminationMethodText(int32 Value) const
{
    switch (FMath::Clamp(Value, QualityMin, QualityMax))
    {
    case 0: return FText::FromString(TEXT("None"));
    case 1: return FText::FromString(TEXT("Lumen"));
    case 2: return FText::FromString(TEXT("Screen Space"));
    case 3: return FText::FromString(TEXT("Plugin"));
    default: break;
    }
    return FText::AsNumber(Value);
}

FText USettingsMenuWidget::GetReflectionMethodText(int32 Value) const
{
    switch (FMath::Clamp(Value, ReflectionMethodMin, ReflectionMethodMax))
    {
    case 0: return FText::FromString(TEXT("None"));
    case 1: return FText::FromString(TEXT("Lumen"));
    case 2: return FText::FromString(TEXT("Screen Space"));
    default: break;
    }
    return FText::AsNumber(Value);
}

void USettingsMenuWidget::CycleBloomIntensityFromUI() { CycleSettingValueFromUI(ESettingsField::BloomIntensity); }
void USettingsMenuWidget::CycleBloomThresholdFromUI() { CycleSettingValueFromUI(ESettingsField::BloomThreshold); }
void USettingsMenuWidget::CycleAmbientOcclusionIntensityFromUI() { CycleSettingValueFromUI(ESettingsField::AmbientOcclusionIntensity); }
void USettingsMenuWidget::CycleExposureFromUI() { CycleSettingValueFromUI(ESettingsField::Exposure); }
void USettingsMenuWidget::CycleRayTracingFromUI() { CycleSettingValueFromUI(ESettingsField::RayTracing); }
void USettingsMenuWidget::CycleHeightFogFromUI() { CycleSettingValueFromUI(ESettingsField::HeightFog); }
void USettingsMenuWidget::CycleCloudFromUI() { CycleSettingValueFromUI(ESettingsField::Cloud); }
void USettingsMenuWidget::CycleCelShadingModeFromUI() { CycleSettingValueFromUI(ESettingsField::CelShadingMode); }
void USettingsMenuWidget::CycleShadowQualityFromUI() { CycleSettingValueFromUI(ESettingsField::ShadowQuality); }
void USettingsMenuWidget::CycleTextureQualityFromUI() { CycleSettingValueFromUI(ESettingsField::TextureQuality); }
void USettingsMenuWidget::CycleMaxTextureResolutionFromUI() { CycleSettingValueFromUI(ESettingsField::MaxTextureResolution); }
int32 USettingsMenuWidget::GetPendingQualityProfileIndex() const
{
    const int32 Q = FMath::Clamp(PendingViewDistanceQuality, 0, 3);
    const bool bMatches = PendingViewDistanceQuality == Q && PendingShadowQuality == Q
        && PendingTextureQuality == Q && PendingAntiAliasingQuality == Q
        && PendingPostProcessingQuality == Q && PendingEffectsQuality == Q
        && PendingFoliageQuality == Q && PendingShadingQuality == Q
        && PendingGlobalIlluminationQuality == Q && PendingReflectionQuality == Q
        && PendingMaxTextureResolution == 256 * (Q + 1)
        && PendingMaxRenderDistanceMeters == (1024 << Q)
        && bPendingRayTracing == (Q >= 2) && bPendingCloud == (Q >= 1) && bPendingHeightFog
        && PendingDynamicGlobalIlluminationMethod == (Q >= 2 ? 1 : 0)
        && PendingReflectionMethod == (Q >= 2 ? 1 : (Q == 1 ? 2 : 0));
    return bMatches ? Q : INDEX_NONE;
}

void USettingsMenuWidget::SetPendingQualityProfile(const int32 Quality)
{
    UGameSettings* Snapshot = NewObject<UGameSettings>();
    ApplyPendingToSettings(Snapshot);
    Snapshot->ApplyQualityProfile(static_cast<EQualitySettings>(FMath::Clamp(Quality, 0, 3)));
    CopySettingsToPending(Snapshot);
}

void USettingsMenuWidget::CycleQualityProfileFromUI() { CycleSettingValueFromUI(ESettingsField::QualityProfile); }
void USettingsMenuWidget::CycleMaxRenderDistanceMetersFromUI() { CycleSettingValueFromUI(ESettingsField::MaxRenderDistanceMeters); }
void USettingsMenuWidget::CycleViewDistanceQualityFromUI() { CycleSettingValueFromUI(ESettingsField::ViewDistanceQuality); }
void USettingsMenuWidget::CycleStreamingDistanceMultiplierFromUI() { /* automatic from ViewDistanceQuality */ }
void USettingsMenuWidget::CycleStreamingUnloadDistanceMultiplierFromUI() { /* automatic from ViewDistanceQuality */ }
void USettingsMenuWidget::CycleObjectStreamingRadiusMetersFromUI() { /* automatic from ViewDistanceQuality */ }
void USettingsMenuWidget::CycleStreamingSceneSpawnBudgetFromUI() { /* automatic from ViewDistanceQuality */ }
void USettingsMenuWidget::CycleStreamingNodeBudgetPerFrameFromUI() { /* automatic from ViewDistanceQuality */ }
void USettingsMenuWidget::CycleAntiAliasingQualityFromUI() { CycleSettingValueFromUI(ESettingsField::AntiAliasingQuality); }
void USettingsMenuWidget::CyclePostProcessingQualityFromUI() { CycleSettingValueFromUI(ESettingsField::PostProcessingQuality); }
void USettingsMenuWidget::CycleEffectsQualityFromUI() { CycleSettingValueFromUI(ESettingsField::EffectsQuality); }
void USettingsMenuWidget::CycleFoliageQualityFromUI() { CycleSettingValueFromUI(ESettingsField::FoliageQuality); }
void USettingsMenuWidget::CycleShadingQualityFromUI() { CycleSettingValueFromUI(ESettingsField::ShadingQuality); }
void USettingsMenuWidget::CycleGlobalIlluminationQualityFromUI() { CycleSettingValueFromUI(ESettingsField::GlobalIlluminationQuality); }
void USettingsMenuWidget::CycleReflectionQualityFromUI() { CycleSettingValueFromUI(ESettingsField::ReflectionQuality); }
void USettingsMenuWidget::CycleDynamicGlobalIlluminationMethodFromUI() { CycleSettingValueFromUI(ESettingsField::DynamicGlobalIlluminationMethod); }
void USettingsMenuWidget::CycleReflectionMethodFromUI() { CycleSettingValueFromUI(ESettingsField::ReflectionMethod); }

void USettingsMenuWidget::BuildNativeLayout()
{
    NativeControls.Reset();
    NativeValueLabels.Reset();
    NativeFieldLabels.Reset();
    NativeControlFields.Reset();
    NativeApply = nullptr;

    UBorder* Panel = AddPanel(PanelSize, FVector2D::ZeroVector);
    UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>();
    Panel->SetContent(Content);
    UOverlay* Header = WidgetTree->ConstructWidget<UOverlay>();
    Content->AddChildToVerticalBox(Header);
    NativeTitle = AddText(Header, NSLOCTEXT("V3DUI", "Settings", "Settings"), true);
    if (UOverlaySlot* TitleSlot = Cast<UOverlaySlot>(NativeTitle->Slot))
    {
        TitleSlot->SetHorizontalAlignment(HAlign_Center);
        TitleSlot->SetVerticalAlignment(VAlign_Center);
        TitleSlot->SetPadding(FMargin(0.f, 8.f));
    }
    NativeBack = AddButton(Header, NSLOCTEXT("V3DUI", "Back", "Back"));
    if (UButtonSlot* BackContentSlot = Cast<UButtonSlot>(NativeBack->GetContent()->Slot)) BackContentSlot->SetPadding(FMargin(12.f));
    if (USizeBox* Size = Cast<USizeBox>(NativeBack->GetParent())) Size->SetMinDesiredWidth(128.f);
    if (UOverlaySlot* BackOverlaySlot = Cast<UOverlaySlot>(NativeBack->GetParent()->Slot))
    {
        BackOverlaySlot->SetHorizontalAlignment(HAlign_Left);
        BackOverlaySlot->SetVerticalAlignment(VAlign_Center);
        BackOverlaySlot->SetPadding(FMargin(32.f, 0.f));
    }

    UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>();
    Scroll->SetAnimateWheelScrolling(true);
    Content->AddChildToVerticalBox(Scroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    UVerticalBox* List = WidgetTree->ConstructWidget<UVerticalBox>();
    Scroll->AddChild(List);

    for (ESettingsField Field : GetDefaultSettingsFields())
    {
        const ESettingsControlType Type = GetSettingControlType(Field);
        UVerticalBox* Row = WidgetTree->ConstructWidget<UVerticalBox>();
        List->AddChildToVerticalBox(Row)->SetPadding(FMargin(8.f));
        UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>();
        Label->SetText(GetSettingLabelText(Field));
        Label->SetAutoWrapText(true);
        UVerticalBox* Controls = WidgetTree->ConstructWidget<UVerticalBox>();
        if (Type == ESettingsControlType::Dropdown)
        {
            Row->AddChildToVerticalBox(Label)->SetPadding(FMargin(8.f));
            Row->AddChildToVerticalBox(Controls)->SetPadding(FMargin(8.f));
        }
        else
        {
            UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>();
            Row->AddChildToVerticalBox(Line);
            UHorizontalBoxSlot* LabelSlot = Line->AddChildToHorizontalBox(Label);
            LabelSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
            LabelSlot->SetVerticalAlignment(VAlign_Center);
            LabelSlot->SetPadding(FMargin(8.f));
            UHorizontalBoxSlot* ControlSlot = Line->AddChildToHorizontalBox(Controls);
            ControlSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
            ControlSlot->SetVerticalAlignment(VAlign_Center);
            ControlSlot->SetPadding(FMargin(8.f));
        }
        UTextBlock* Value = WidgetTree->ConstructWidget<UTextBlock>();
        Value->SetText(GetPendingSettingValueText(Field));
        Value->SetJustification(ETextJustify::Center);
        UWidget* Control = nullptr;
        switch (Type)
        {
        case ESettingsControlType::Slider:
            {
                USlider* Slider = WidgetTree->ConstructWidget<USlider>();
                float Min = 0.f, Max = 1.f, Step = .01f;
                GetSettingSliderRange(Field, Min, Max, Step);
                Slider->SetMinValue(Min);
                Slider->SetMaxValue(Max);
                Slider->SetStepSize(Step);
                Slider->SetValue(GetPendingNumericSettingValue(Field));
                Controls->AddChildToVerticalBox(Slider);
                Control = Slider;
            }
            break;
        case ESettingsControlType::Dropdown:
            {
                UComboBoxString* Dropdown = WidgetTree->ConstructWidget<UComboBoxString>();
                Dropdown->OnGenerateWidgetEvent.BindDynamic(this, &USettingsMenuWidget::GenerateNativeOption);
                for (const FText& Option : GetSettingOptionTexts(Field)) Dropdown->AddOption(Option.ToString());
                Dropdown->SetSelectedOption(GetPendingSettingValueText(Field).ToString());
                Controls->AddChildToVerticalBox(Dropdown);
                Control = Dropdown;
            }
            break;
        case ESettingsControlType::Toggle:
            Control = AddButton(Controls, GetPendingSettingValueText(Field));
            break;
        }
        Controls->AddChildToVerticalBox(Value);
        if (Type != ESettingsControlType::Slider) Value->SetVisibility(ESlateVisibility::Collapsed);
        NativeControlFields.Add(Field);
        NativeControls.Add(Control);
        NativeValueLabels.Add(Value);
        NativeFieldLabels.Add(Label);
    }
}

void USettingsMenuWidget::BindNativeWidgets()
{
    if (!IsUsingNativeLayout()) return;
    AssignedSettingsListBox.Reset();
    SetTitleText(NativeTitle);
    SetApplyButton(NativeApply);
    SetBackButton(NativeBack);
    SetConfirmButton(nullptr);
    SetCancelButton(nullptr);

    // Native controls are restored after a legacy WBP Construct/PreConstruct graph.
    // Replace only adapters that were replaced by that graph; retain live adapters on refresh.
    for (int32 Index = 0; Index < NativeControls.Num(); ++Index)
    {
        const ESettingsField Field = NativeControlFields[Index];
        RegisteredValueTextWidgets.FindOrAdd(Field) = NativeValueLabels[Index].Get();
        UWidget* Control = NativeControls[Index];
        bool bAlreadyBound = false;
        for (USettingsControlBinding* Binding : ControlBindings)
            if (IsValid(Binding) && Binding->GetField() == Field && Binding->GetControlObject() == Control)
                bAlreadyBound = true;
        if (bAlreadyBound) continue;
        if (USlider* Slider = Cast<USlider>(Control)) RegisterSettingSlider(Field, Slider);
        else if (UComboBoxString* Dropdown = Cast<UComboBoxString>(Control)) RegisterSettingDropdown(Field, Dropdown);
        else if (UButton* Button = Cast<UButton>(Control)) RegisterSettingToggleButton(Field, Button);
    }
    CollectAssignedWidgetReferences();
    RefreshSettingsValues();
}

void USettingsMenuWidget::ApplyNativeStyle()
{
    Super::ApplyNativeStyle();
    // The original Settings header uses the engine's default font; generated rows use Pretendard.
    if (NativeTitle)
    {
        FSlateFontInfo Font = GetDefault<UTextBlock>()->GetFont();
        V3DHUDLegacyStyle::ApplyFont(Font, nullptr, TitleFontSize, FontOutlineSize);
        NativeTitle->SetFont(Font);
    }
    if (NativeBack)
    {
        if (UButtonSlot* BackContentSlot = Cast<UButtonSlot>(NativeBack->GetContent()->Slot)) BackContentSlot->SetPadding(FMargin(12.f));
        if (UTextBlock* Label = Cast<UTextBlock>(NativeBack->GetContent()))
        {
            FSlateFontInfo Font = GetDefault<UTextBlock>()->GetFont();
            V3DHUDLegacyStyle::ApplyFont(Font, nullptr, FMath::RoundToInt(BodyFontSize * 4.f / 3.f), FontOutlineSize);
            Label->SetFont(Font);
        }
    }
    for (int32 Index = 0; Index < NativeFieldLabels.Num(); ++Index)
    {
        UTextBlock* Label = NativeFieldLabels[Index];
        if (!IsValid(Label)) continue;
        const bool bToggle = GetSettingControlType(NativeControlFields[Index]) == ESettingsControlType::Toggle;
        FSlateFontInfo Font = Label->GetFont();
        V3DHUDLegacyStyle::ApplyFont(Font, FontObject,
            bToggle ? FMath::RoundToInt(BodyFontSize * 4.f / 3.f) : BodyFontSize, 1);
        Label->SetFont(Font);
        Label->SetColorAndOpacity(FSlateColor(TextColor));
    }
    NativeOptionLabels.RemoveAll([](const TWeakObjectPtr<UTextBlock>& Item) { return !Item.IsValid(); });
    for (const TWeakObjectPtr<UTextBlock>& WeakLabel : NativeOptionLabels)
    {
        UTextBlock* Label = WeakLabel.Get();
        if (!IsValid(Label)) continue;
        FSlateFontInfo Font = Label->GetFont();
        V3DHUDLegacyStyle::ApplyFont(Font, FontObject, BodyFontSize, 1);
        Label->SetFont(Font);
        Label->SetColorAndOpacity(FSlateColor(TextColor));
    }
    for (UTextBlock* Value : NativeValueLabels)
    {
        if (!IsValid(Value)) continue;
        FSlateFontInfo Font = Value->GetFont();
        V3DHUDLegacyStyle::ApplyFont(Font, FontObject, FMath::Max(8, BodyFontSize / 2), 1);
        Value->SetFont(Font);
        Value->SetColorAndOpacity(FSlateColor(TextColor));
    }
    for (UWidget* Control : NativeControls)
    {
        if (USlider* Slider = Cast<USlider>(Control))
        {
            FSliderStyle Style = Slider->GetWidgetStyle();
            Style.BarThickness = 24.f;
            Style.NormalThumbImage.ImageSize = FVector2D(16.f, 32.f);
            Style.HoveredThumbImage.ImageSize = FVector2D(16.f, 32.f);
            V3DWidgetStyle::SetOwnedStyle(*Slider, Style);
            Slider->SetSliderHandleColor(FLinearColor::White);
            Slider->SetSliderBarColor(FLinearColor(.33f, .33f, .33f, 1.f));
        }
        else if (UComboBoxString* Dropdown = Cast<UComboBoxString>(Control))
            ApplySettingsMenuComboTheme(Dropdown, ButtonColor, HoveredButtonTint, TextColor);
        else if (UButton* Button = Cast<UButton>(Control))
        {
            if (UTextBlock* Label = Cast<UTextBlock>(Button->GetContent()))
            {
                FSlateFontInfo Font = Label->GetFont();
                V3DHUDLegacyStyle::ApplyFont(Font, FontObject, FMath::RoundToInt(BodyFontSize * 2.f / 3.f), 1);
                Label->SetFont(Font);
            }
        }
    }
}


UWidget* USettingsMenuWidget::GenerateNativeOption(FString Option)
{
    UTextBlock* Text = NewObject<UTextBlock>(this);
    Text->SetText(FText::FromString(Option));
    FSlateFontInfo Font = Text->GetFont();
    V3DHUDLegacyStyle::ApplyFont(Font, FontObject, BodyFontSize, 1);
    Text->SetFont(Font);
    Text->SetColorAndOpacity(FSlateColor(TextColor));
    NativeOptionLabels.RemoveAll([](const TWeakObjectPtr<UTextBlock>& Item) { return !Item.IsValid(); });
    NativeOptionLabels.Add(Text);
    return Text;
}

void USettingsMenuWidget::ReleaseSlateResources(bool bReleaseChildren)
{
    Super::ReleaseSlateResources(bReleaseChildren);
    NativeOptionLabels.Reset();
}
