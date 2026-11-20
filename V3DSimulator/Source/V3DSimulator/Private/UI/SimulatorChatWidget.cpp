// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "UI/SimulatorChatWidget.h"
#include "UI/WidgetStyleSafety.h"
#include "UI/HUDStyle.h"
#include "Character/PlayerCharacterController.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Border.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/TextBlock.h"
#include "InputCoreTypes.h"
#include "Components/ScrollBox.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/IVirtualKeyboardEntry.h"

USimulatorChatWidget::USimulatorChatWidget()
{
    BackgroundColor = FLinearColor::Transparent;
    PanelSize = FVector2D(740, 300);
    PanelPadding = 16.f;
    PanelColor = FLinearColor(.082754f, .082754f, .082754f, .94f);
    BodyFontSize = 18;
    TitleFontSize = 24;
    FontOutlineSize = 1;
    SetIsFocusable(true);
}

void USimulatorChatWidget::BuildNativeLayout()
{
    UBorder* Border = AddPanel(PanelSize, FVector2D(24, -24), FVector2D(0, 1), FVector2D(0, 1));
    UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
    Border->SetContent(Box);
    AddText(Box, NSLOCTEXT("SimulatorChat", "Title", "CHAT / COMMANDS"));
    OutputScroll = WidgetTree->ConstructWidget<UScrollBox>();
    Box->AddChildToVerticalBox(OutputScroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    Output = AddText(OutputScroll, History.IsEmpty()
        ? FText::FromString(TEXT("Press T to chat  |  /sim list  |  Esc: close"))
        : FText::FromString(FString::Join(History, TEXT("\n"))));

    UHorizontalBox* InputRow = WidgetTree->ConstructWidget<UHorizontalBox>();
    Box->AddChildToVerticalBox(InputRow);

    Input = WidgetTree->ConstructWidget<UEditableTextBox>();
    Input->SetHintText(FText::FromString(TEXT("Type message or /command...")));
    Input->SetClearKeyboardFocusOnCommit(false);
    Input->VirtualKeyboardTrigger = EVirtualKeyboardTrigger::OnFocusByPointer;
    Input->VirtualKeyboardDismissAction = EVirtualKeyboardDismissAction::TextCommitOnDismiss;
    if (UHorizontalBoxSlot* InputSlot = InputRow->AddChildToHorizontalBox(Input))
    {
        InputSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        InputSlot->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
    }

    SendButton = AddButton(InputRow, NSLOCTEXT("SimulatorChat", "Send", "Send"));
    CloseButton = AddButton(InputRow, NSLOCTEXT("SimulatorChat", "Close", "Close"));
}

void USimulatorChatWidget::ApplyNativeStyle()
{
    Super::ApplyNativeStyle();
    if (Input)
    {
        FEditableTextBoxStyle Style = Input->GetWidgetStyle();
        V3DHUDStyle::ApplyFont(Style.TextStyle.Font, FontObject, BodyFontSize, FontOutlineSize);
        Style.SetForegroundColor(FSlateColor(TextColor));
        V3DWidgetStyle::SetOwnedStyle(*Input, Style);
    }
}
void USimulatorChatWidget::NativeConstruct()
{
    Super::NativeConstruct();
    SetIsFocusable(true);
    if (Input) Input->OnTextCommitted.AddUniqueDynamic(this, &USimulatorChatWidget::Commit);
    if (SendButton) SendButton->OnClicked.AddUniqueDynamic(this, &USimulatorChatWidget::HandleSendClicked);
    if (CloseButton) CloseButton->OnClicked.AddUniqueDynamic(this, &USimulatorChatWidget::HandleCloseClicked);
}
void USimulatorChatWidget::NativeDestruct()
{
    if (Input) Input->OnTextCommitted.RemoveDynamic(this, &USimulatorChatWidget::Commit);
    if (SendButton) SendButton->OnClicked.RemoveDynamic(this, &USimulatorChatWidget::HandleSendClicked);
    if (CloseButton) CloseButton->OnClicked.RemoveDynamic(this, &USimulatorChatWidget::HandleCloseClicked);
    Super::NativeDestruct();
}
void USimulatorChatWidget::FocusInput()
{
    if (!IsValid(Input)) return;
    Input->SetKeyboardFocus();
#if PLATFORM_ANDROID || PLATFORM_IOS
    // On mobile without a physical keyboard, request the OS virtual touch keyboard when focusing chat.
    if (FSlateApplication::IsInitialized())
    {
        FSlateApplication::Get().SetKeyboardFocus(Input->TakeWidget(), EFocusCause::Mouse);
    }
#endif
}
void USimulatorChatWidget::AppendMessage(const FString& Message)
{
    History.Add(Message.Left(512));
    while (History.Num() > FMath::Clamp(MaxHistoryLines, 1, 64)) History.RemoveAt(0);
    if (Output) Output->SetText(FText::FromString(FString::Join(History, TEXT("\n"))));
    if (OutputScroll) OutputScroll->ScrollToEnd();
}
void USimulatorChatWidget::SubmitCurrentInput()
{
    if (!IsValid(Input)) return;
    const FString Line = Input->GetText().ToString().Left(512).TrimStartAndEnd();
    Input->SetText(FText::GetEmpty());
    if (Line.IsEmpty()) return;
    AppendMessage(TEXT("> ") + Line);
    if (auto* Controller = Cast<APlayerCharacterController>(GetOwningPlayer()))
    {
        if (Line.StartsWith(TEXT("/")))
        {
            if (!Controller->SubmitChatCommand(Line))
            {
                AppendMessage(TEXT("Unknown command. Try /sim list or /sim run <name>."));
            }
        }
        else
        {
            Controller->SubmitChatCommand(Line);
        }
    }
    FocusInput();
}

void USimulatorChatWidget::Commit(const FText& Text, ETextCommit::Type Method)
{
    if (Method != ETextCommit::OnEnter || !IsValid(Input)) return;
    SubmitCurrentInput();
}

void USimulatorChatWidget::HandleSendClicked()
{
    SubmitCurrentInput();
}

void USimulatorChatWidget::HandleCloseClicked()
{
    if (auto* Controller = Cast<APlayerCharacterController>(GetOwningPlayer()))
    {
        Controller->CloseSimulationChat();
    }
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
