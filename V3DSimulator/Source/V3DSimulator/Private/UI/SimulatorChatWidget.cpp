// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "UI/SimulatorChatWidget.h"
#include "UI/WidgetStyleSafety.h"
#include "UI/HUDLegacyStyle.h"
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
    AddText(Box, NSLOCTEXT("SimulatorChat", "Title", "SIMULATION COMMANDS"));
    OutputScroll = WidgetTree->ConstructWidget<UScrollBox>();
    Box->AddChildToVerticalBox(OutputScroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    Output = AddText(OutputScroll, History.IsEmpty()
        ? FText::FromString(TEXT("/sim list  |  /sim run <name>  |  Esc: close"))
        : FText::FromString(FString::Join(History, TEXT("\n"))));
    Input = WidgetTree->ConstructWidget<UEditableTextBox>();
    Input->SetHintText(FText::FromString(TEXT("/sim run stop")));
    Input->SetClearKeyboardFocusOnCommit(false);
    Box->AddChildToVerticalBox(Input);
}

void USimulatorChatWidget::ApplyNativeStyle()
{
    Super::ApplyNativeStyle();
    if (Input)
    {
        FEditableTextBoxStyle Style = Input->GetWidgetStyle();
        V3DHUDLegacyStyle::ApplyFont(Style.TextStyle.Font, FontObject, BodyFontSize, FontOutlineSize);
        Style.SetForegroundColor(FSlateColor(TextColor));
        V3DWidgetStyle::SetOwnedStyle(*Input, Style);
    }
}
void USimulatorChatWidget::NativeConstruct()
{
    Super::NativeConstruct();
    SetIsFocusable(true);
    if (Input) Input->OnTextCommitted.AddUniqueDynamic(this, &USimulatorChatWidget::Commit);
}
void USimulatorChatWidget::NativeDestruct()
{
    if (Input) Input->OnTextCommitted.RemoveDynamic(this, &USimulatorChatWidget::Commit);
    Super::NativeDestruct();
}
void USimulatorChatWidget::FocusInput()
{
    if (Input) Input->SetKeyboardFocus();
}
void USimulatorChatWidget::AppendMessage(const FString& Message)
{
    History.Add(Message.Left(512));
    while (History.Num() > FMath::Clamp(MaxHistoryLines, 1, 64)) History.RemoveAt(0);
    if (Output) Output->SetText(FText::FromString(FString::Join(History, TEXT("\n"))));
    if (OutputScroll) OutputScroll->ScrollToEnd();
}
void USimulatorChatWidget::Commit(const FText& Text, ETextCommit::Type Method)
{
    if (Method != ETextCommit::OnEnter || !Input) return;
    const FString Line = Text.ToString().Left(512).TrimStartAndEnd();
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
