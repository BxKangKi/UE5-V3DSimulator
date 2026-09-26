// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/MenuButtonWidget.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Blueprint/WidgetTree.h"

TSharedRef<SWidget> UMenuButtonWidget::RebuildWidget()
{
    if (GetClass() == StaticClass() && IsValid(WidgetTree) && !WidgetTree->RootWidget)
    {
        UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("Button"));
        WidgetTree->RootWidget = Button;
        UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Label"));
        Button->AddChild(Label);
    }
    return Super::RebuildWidget();
}

void UMenuButtonWidget::NativeConstruct()
{
    Super::NativeConstruct();
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
    OnButtonClicked.Clear();
    Super::NativeDestruct();
}

void UMenuButtonWidget::SetButton(UButton* InButton)
{
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
    AssignedLabel = InTextBlock;
    if (IsValid(InTextBlock))
    {
        InTextBlock->SetText(FText::FromString(DisplayLabel));
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

    BP_OnButtonConfigured(ButtonKey, DisplayLabel);
}

void UMenuButtonWidget::TriggerClick()
{
    if (!ButtonKey.IsEmpty())
    {
        OnButtonClicked.Broadcast(ButtonKey);
    }
}

void UMenuButtonWidget::HandleNativeButtonClicked()
{
    TriggerClick();
}
