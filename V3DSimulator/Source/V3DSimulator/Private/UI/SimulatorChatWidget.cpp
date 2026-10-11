// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "UI/SimulatorChatWidget.h"
#include "Character/PlayerCharacterController.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Border.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/TextBlock.h"
#include "InputCoreTypes.h"

TSharedRef<SWidget> USimulatorChatWidget::RebuildWidget()
{
    if (WidgetTree && !WidgetTree->RootWidget)
    {
        auto* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>();
        WidgetTree->RootWidget = Canvas;
        auto* Border = WidgetTree->ConstructWidget<UBorder>();
        Border->SetBrushColor(FLinearColor(0.025f, 0.035f, 0.05f, 0.94f));
        Border->SetPadding(FMargin(12));
        auto* ChatPanelSlot = Canvas->AddChildToCanvas(Border);
        ChatPanelSlot->SetAnchors(FAnchors(0.0f, 1.0f)); ChatPanelSlot->SetAlignment(FVector2D(0, 1));
        ChatPanelSlot->SetPosition(FVector2D(20, -30)); ChatPanelSlot->SetSize(FVector2D(700, 260));
        auto* Box = WidgetTree->ConstructWidget<UVerticalBox>(); Border->SetContent(Box);
        Output = WidgetTree->ConstructWidget<UTextBlock>();
        Output->SetAutoWrapText(true);
        Output->SetText(FText::FromString(TEXT("/sim list  |  /sim run <name>  |  Esc: close")));
        Box->AddChildToVerticalBox(Output)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        Input = WidgetTree->ConstructWidget<UEditableTextBox>();
        Input->SetHintText(FText::FromString(TEXT("/sim run stop")));
        Input->SetClearKeyboardFocusOnCommit(false);
        Box->AddChildToVerticalBox(Input);
    }
    return Super::RebuildWidget();
}
void USimulatorChatWidget::NativeConstruct()
{
    Super::NativeConstruct();
    SetIsFocusable(true);
    if (Input) Input->OnTextCommitted.AddUniqueDynamic(this, &USimulatorChatWidget::Commit);
}
void USimulatorChatWidget::FocusInput()
{
    if (Input) Input->SetKeyboardFocus();
}
void USimulatorChatWidget::AppendMessage(const FString& Message)
{
    History.Add(Message.Left(512));
    while (History.Num() > 8) History.RemoveAt(0);
    if (Output) Output->SetText(FText::FromString(FString::Join(History, TEXT("\n"))));
}
void USimulatorChatWidget::Commit(const FText& Text, ETextCommit::Type Method)
{
    if (Method != ETextCommit::OnEnter || !Input) return;
    const FString Line = Text.ToString().TrimStartAndEnd();
    Input->SetText(FText::GetEmpty());
    if (Line.IsEmpty()) return;
    AppendMessage(TEXT("> ") + Line);
    auto* Controller = Cast<APlayerCharacterController>(GetOwningPlayer());
    if (!Controller || !Controller->SubmitChatCommand(Line)) AppendMessage(TEXT("Use /sim list or /sim run <name>."));
    FocusInput();
}
FReply USimulatorChatWidget::NativeOnPreviewKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
{
    if (Event.GetKey() == EKeys::Escape)
    {
        if (auto* Controller = Cast<APlayerCharacterController>(GetOwningPlayer())) Controller->CloseSimulationChat();
        return FReply::Handled();
    }
    return Super::NativeOnPreviewKeyDown(Geometry, Event);
}
