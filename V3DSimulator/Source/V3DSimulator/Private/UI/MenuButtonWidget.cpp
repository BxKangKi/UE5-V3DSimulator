// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/MenuButtonWidget.h"
#include "UI/NativeWidgetTreeNames.h"
#include "UI/HUDLegacyStyle.h"
#include "UI/WidgetStyleSafety.h"

#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/ScaleBox.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/TextBlock.h"
#include "Blueprint/WidgetTree.h"
#include "Components/SizeBox.h"

UMenuButtonWidget::UMenuButtonWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    FontObject = V3DHUDLegacyStyle::FindFontForConstructor();
}

TSharedRef<SWidget> UMenuButtonWidget::RebuildWidget()
{
    const bool bBuildNativeTree = bUseNativeLayout && IsValid(WidgetTree)
        && (!IsValid(NativeScaleRoot) || WidgetTree->RootWidget != NativeScaleRoot);
    if (bBuildNativeTree)
    {
        WidgetTree = NewObject<UWidgetTree>(this, NAME_None, RF_Transient);
        NativeScaleRoot = WidgetTree->ConstructWidget<UScaleBox>();
        // The enclosing HUD scales for resolution; this root must not distort its fonts.
        NativeScaleRoot->SetStretch(EStretch::None);
        WidgetTree->RootWidget = NativeScaleRoot;
        NativeRoot = WidgetTree->ConstructWidget<USizeBox>();
        UScaleBoxSlot* ScaleSlot = CastChecked<UScaleBoxSlot>(NativeScaleRoot->AddChild(NativeRoot));
        ScaleSlot->SetHorizontalAlignment(HAlign_Fill);
        ScaleSlot->SetVerticalAlignment(VAlign_Fill);
        NativeButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(),
            TEXT("Button"));
        NativeRoot->SetContent(NativeButton);
        NativeLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
            TEXT("Label"));
        NativeLabel->SetJustification(ETextJustify::Center);
        NativeButton->SetContent(NativeLabel);
    }
    BindNativeControls();
    ApplyNativeStyle();
    if (bBuildNativeTree)
    {
        V3DNativeWidgetTree::StabilizeNames(WidgetTree);
        V3DNativeWidgetTree::BindBlueprintWidgets(this);
    }
    return Super::RebuildWidget();
}

void UMenuButtonWidget::SynchronizeProperties()
{
    Super::SynchronizeProperties();
    BindNativeControls();
    ApplyNativeStyle();
}

void UMenuButtonWidget::NativePreConstruct()
{
    Super::NativePreConstruct();
    BindNativeControls();
    ApplyNativeStyle();
}

void UMenuButtonWidget::NativeConstruct()
{
    Super::NativeConstruct();
    BindNativeControls();
    ApplyNativeStyle();
    // Blueprint Construct may assign custom controls. Only discover an unambiguous fallback.
    if (!IsValid(WidgetTree)) return;
    TArray<UWidget*> Widgets;
    WidgetTree->GetAllWidgets(Widgets);
    UButton* OnlyButton = nullptr;
    UTextBlock* OnlyLabel = nullptr;
    int32 ButtonCount = 0;
    int32 LabelCount = 0;
    for (UWidget* Widget : Widgets)
    {
        if (UButton* Button = Cast<UButton>(Widget)) { OnlyButton = Button; ++ButtonCount; }
        if (UTextBlock* Label = Cast<UTextBlock>(Widget)) { OnlyLabel = Label; ++LabelCount; }
    }
    if (!AssignedButton.IsValid() && ButtonCount == 1) SetButton(OnlyButton);
    if (!AssignedLabel.IsValid() && LabelCount == 1) SetLabelTextBlock(OnlyLabel);
}

void UMenuButtonWidget::NativeDestruct()
{
    if (UButton* Button = AssignedButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMenuButtonWidget::HandleNativeButtonClicked);
    }

    AssignedButton.Reset();
    AssignedLabel.Reset();
    Super::NativeDestruct();
}

void UMenuButtonWidget::SetButton(UButton* InButton)
{
    if (bUseNativeLayout && IsValid(NativeRoot) && InButton && InButton != NativeButton) return;
    if (AssignedButton.Get() == InButton) return;
    if (UButton* ExistingButton = AssignedButton.Get())
    {
        ExistingButton->OnClicked.RemoveDynamic(this, &UMenuButtonWidget::HandleNativeButtonClicked);
    }

    AssignedButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UMenuButtonWidget::HandleNativeButtonClicked);
        InButton->OnClicked.AddDynamic(this, &UMenuButtonWidget::HandleNativeButtonClicked);
    }
}

void UMenuButtonWidget::SetLabelTextBlock(UTextBlock* InTextBlock)
{
    if (bUseNativeLayout && IsValid(NativeRoot) && InTextBlock && InTextBlock != NativeLabel) return;
    AssignedLabel = InTextBlock;
    if (IsValid(InTextBlock))
    {
        InTextBlock->SetText(DisplayLabel.IsEmpty() ? PreviewLabel : FText::FromString(DisplayLabel));
    }
}

void UMenuButtonWidget::ConfigureButton(const FString& InButtonKey, const FString& InDisplayLabel)
{
    ButtonKey = InButtonKey.TrimStartAndEnd();
    DisplayLabel = InDisplayLabel.TrimStartAndEnd();
    if (DisplayLabel.IsEmpty())
    {
        DisplayLabel = ButtonKey;
    }

    if (UTextBlock* Label = AssignedLabel.Get())
    {
        Label->SetText(FText::FromString(DisplayLabel));
    }

    if (!IsDesignTime()) BP_OnButtonConfigured(ButtonKey, DisplayLabel);
    BindNativeControls();
    ApplyNativeStyle();
}

void UMenuButtonWidget::TriggerClick()
{
    if (!IsDesignTime() && GetIsEnabled() && !ButtonKey.IsEmpty())
    {
        OnButtonClicked.Broadcast(ButtonKey);
    }
}

void UMenuButtonWidget::HandleNativeButtonClicked()
{
    TriggerClick();
}


void UMenuButtonWidget::BindNativeControls()
{
    if (!bUseNativeLayout || !IsValid(NativeRoot)) return;
    SetButton(NativeButton);
    SetLabelTextBlock(NativeLabel);
}

void UMenuButtonWidget::ApplyNativeStyle()
{
    if (!bUseNativeLayout || !IsValid(NativeRoot)) return;
    NativeRoot->SetMinDesiredWidth(FMath::Clamp(FMath::IsFinite(ButtonMinSize.X) ? ButtonMinSize.X : 200.0, 1.0, 16384.0));
    NativeRoot->SetMinDesiredHeight(FMath::Clamp(FMath::IsFinite(ButtonMinSize.Y) ? ButtonMinSize.Y : 64.0, 1.0, 16384.0));
    if (IsValid(NativeButton))
    {
        V3DHUDLegacyStyle::ApplyButton(*NativeButton, BackgroundColor);
        FButtonStyle Style = NativeButton->GetStyle();
        Style.Hovered.TintColor = FSlateColor(AccentColor);
        V3DWidgetStyle::SetOwnedStyle(*NativeButton, Style);
        if (UButtonSlot* ButtonSlot = Cast<UButtonSlot>(NativeLabel ? NativeLabel->Slot : nullptr))
        {
            ButtonSlot->SetPadding(ContentPadding);
            ButtonSlot->SetHorizontalAlignment(HAlign_Fill);
            ButtonSlot->SetVerticalAlignment(VAlign_Center);
        }
    }
    if (IsValid(NativeLabel))
    {
        FSlateFontInfo Font = NativeLabel->GetFont();
        V3DHUDLegacyStyle::ApplyFont(Font, FontObject, FMath::Clamp(FontSize, 8, 96), FontOutlineSize);
        NativeLabel->SetFont(Font);
        NativeLabel->SetColorAndOpacity(FSlateColor(TextColor));
        NativeLabel->SetText(DisplayLabel.IsEmpty() ? PreviewLabel : FText::FromString(DisplayLabel));
    }
}

void UMenuButtonWidget::ApplyButtonTheme(FLinearColor InTextColor, FLinearColor InBackgroundColor,
    FLinearColor InAccentColor, int32 InFontSize, float InMinHeight)
{
    TextColor = InTextColor;
    BackgroundColor = InBackgroundColor;
    AccentColor = InAccentColor;
    FontSize = FMath::Clamp(InFontSize, 8, 96);
    ButtonMinSize.Y = FMath::Clamp(FMath::IsFinite(InMinHeight) ? InMinHeight : 64.f, 1.f, 16384.f);
    ApplyNativeStyle();
}
