// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file PauseMenuWidget.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "UI/PauseMenuWidget.h"
#include "UI/HUDLegacyStyle.h"
#include "Character/PlayerCharacterController.h"
#include "Components/Button.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBox.h"
#include "Components/TextBlock.h"
#include "Kismet/GameplayStatics.h"

UPauseMenuWidget::UPauseMenuWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    PanelSize = FVector2D(1920.0, 1080.0);
    PanelPadding = 0.0f;
    ButtonHeight = 100.0f;
    BodyFontSize = 64;
    TitleFontSize = 100;
    BackgroundColor = V3DHUDLegacyStyle::PauseBackground;
    PanelColor = FLinearColor::Transparent;
}

void UPauseMenuWidget::NativeConstruct()
{
    Super::NativeConstruct();
    ResetExitRequestState();
    BindButtonEvents();
}

void UPauseMenuWidget::NativeDestruct()
{
    bExitRequestInProgress = false;
    UnbindButtonEvents();
    AssignedTitleText.Reset();
    AssignedContinueButton.Reset();
    AssignedSettingsButton.Reset();
    AssignedExitButton.Reset();
    Super::NativeDestruct();
}

void UPauseMenuWidget::SetTitleText(UTextBlock* InTitleText)
{
    if (!AcceptWidgetAssignment(InTitleText)) return;
    AssignedTitleText = InTitleText;
}

void UPauseMenuWidget::SetContinueButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = AssignedContinueButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ContinueFromUI);
    }

    AssignedContinueButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ContinueFromUI);
        InButton->OnClicked.AddDynamic(this, &UPauseMenuWidget::ContinueFromUI);
    }
}

void UPauseMenuWidget::SetSettingsButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = AssignedSettingsButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::OpenSettingsFromUI);
    }

    AssignedSettingsButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::OpenSettingsFromUI);
        InButton->OnClicked.AddDynamic(this, &UPauseMenuWidget::OpenSettingsFromUI);
    }
}

void UPauseMenuWidget::SetExitButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = AssignedExitButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ExitFromUI);
    }

    AssignedExitButton = InButton;
    if (IsValid(InButton))
    {
        // Exit travel has one owner. WBP OnClicked navigation is removed so a carried click
        // cannot issue another OpenLevel after the destination menu has loaded.
        InButton->OnClicked.Clear();
        InButton->OnClicked.AddDynamic(this, &UPauseMenuWidget::ExitFromUI);
    }
}

void UPauseMenuWidget::BindButtonEvents()
{
    if (UButton* Button = AssignedContinueButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ContinueFromUI);
        Button->OnClicked.AddDynamic(this, &UPauseMenuWidget::ContinueFromUI);
    }
    if (UButton* Button = AssignedSettingsButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::OpenSettingsFromUI);
        Button->OnClicked.AddDynamic(this, &UPauseMenuWidget::OpenSettingsFromUI);
    }
    if (UButton* Button = AssignedExitButton.Get())
    {
        Button->OnClicked.Clear();
        Button->OnClicked.AddDynamic(this, &UPauseMenuWidget::ExitFromUI);
    }
}

void UPauseMenuWidget::UnbindButtonEvents()
{
    if (UButton* Button = AssignedContinueButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ContinueFromUI);
    }
    if (UButton* Button = AssignedSettingsButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::OpenSettingsFromUI);
    }
    if (UButton* Button = AssignedExitButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ExitFromUI);
    }
}

void UPauseMenuWidget::ResetExitRequestState()
{
    bExitRequestInProgress = false;
    SetIsEnabled(true);
    if (UButton* Button = AssignedExitButton.Get())
    {
        Button->SetIsEnabled(true);
    }
}

void UPauseMenuWidget::ContinueFromUI()
{
    if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(GetOwningPlayer()))
    {
        PlayerController->ClosePauseMenu(true);
    }
}

void UPauseMenuWidget::OpenSettingsFromUI()
{
    if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(GetOwningPlayer()))
    {
        PlayerController->ShowSettingsMenuFromPause();
    }
}

void UPauseMenuWidget::ExitFromUI()
{
    if (bExitRequestInProgress)
    {
        UE_LOG(LogTemp, Verbose, TEXT("[MenuTravel] Ignored a duplicate pause-menu Exit click."));
        return;
    }

    APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(GetOwningPlayer());
    if (!IsValid(PlayerController))
    {
        PlayerController = Cast<APlayerCharacterController>(UGameplayStatics::GetPlayerController(this, 0));
    }
    if (!IsValid(PlayerController))
    {
        UE_LOG(LogTemp, Error, TEXT("[MenuTravel] Pause Exit has no valid APlayerCharacterController owner."));
        return;
    }

    // Mark the click first so another serialized Blueprint listener on the same button cannot start
    // a second native request. The controller returns false without tearing down the UI when the
    // destination is not configured or the request is otherwise rejected.
    bExitRequestInProgress = true;
    if (!PlayerController->TryExitToWorldSelectionFromPauseMenu())
    {
        ResetExitRequestState();
        UE_LOG(LogTemp, Warning, TEXT("[MenuTravel] Pause Exit was rejected; the pause menu remains usable."));
        return;
    }

    SetIsEnabled(false);
    if (UButton* Button = AssignedExitButton.Get())
    {
        Button->SetIsEnabled(false);
    }
}


void UPauseMenuWidget::BuildNativeLayout()
{
    SetIsFocusable(true);
    NativePanel = AddPanel(PanelSize, FVector2D::ZeroVector);
    UOverlay* Layout = WidgetTree->ConstructWidget<UOverlay>();
    NativePanel->SetContent(Layout);

    // The original pause menu overlays only the left half of its translucent backdrop.
    NativePauseAccent = WidgetTree->ConstructWidget<UBorder>();
    NativePauseAccent->SetPadding(FMargin(0.0f));
    NativePauseAccent->SetRenderScale(FVector2D(0.5, 1.0));
    NativePauseAccent->SetRenderTransformPivot(FVector2D(0.0, 0.5));
    NativePauseAccent->SetVisibility(ESlateVisibility::HitTestInvisible);
    UOverlaySlot* AccentSlot = Layout->AddChildToOverlay(NativePauseAccent);
    AccentSlot->SetHorizontalAlignment(HAlign_Fill);
    AccentSlot->SetVerticalAlignment(VAlign_Fill);

    NativeTitle = AddText(Layout, NSLOCTEXT("V3DMenu", "PauseTitle", "Pause"), true);
    NativeTitle->SetAutoWrapText(false);
    UOverlaySlot* TitleSlot = Cast<UOverlaySlot>(NativeTitle->Slot);
    TitleSlot->SetPadding(FMargin(64.0f));
    TitleSlot->SetHorizontalAlignment(HAlign_Left);
    TitleSlot->SetVerticalAlignment(VAlign_Center);

    UUniformGridPanel* Buttons = WidgetTree->ConstructWidget<UUniformGridPanel>();
    Buttons->SetSlotPadding(FMargin(0.0f, 32.0f));
    UOverlaySlot* ButtonsSlot = Layout->AddChildToOverlay(Buttons);
    ButtonsSlot->SetHorizontalAlignment(HAlign_Right);
    ButtonsSlot->SetVerticalAlignment(VAlign_Center);
    auto AddPauseButton = [this, Buttons](const FText& Text, int32 Row)
    {
        UButton* Button = AddButton(Buttons, Text);
        USizeBox* Size = Cast<USizeBox>(Button->GetParent());
        Size->SetMinDesiredWidth(768.0f);
        UUniformGridSlot* Slot = Cast<UUniformGridSlot>(Size->Slot);
        Slot->SetRow(Row);
        Slot->SetHorizontalAlignment(HAlign_Fill);
        Slot->SetVerticalAlignment(VAlign_Fill);
        if (UTextBlock* Label = Cast<UTextBlock>(Button->GetContent()))
            Label->SetAutoWrapText(false);
        return Button;
    };
    NativeContinue = AddPauseButton(NSLOCTEXT("V3DMenu", "Continue", "Continue"), 0);
    NativeSettings = AddPauseButton(NSLOCTEXT("V3DMenu", "Settings", "Settings"), 1);
    NativeExit = AddPauseButton(NSLOCTEXT("V3DMenu", "Exit", "Exit"), 2);
}

void UPauseMenuWidget::ApplyNativeStyle()
{
    Super::ApplyNativeStyle();
    if (NativePauseAccent) NativePauseAccent->SetBrushColor(PauseAccentColor);
}

void UPauseMenuWidget::BindNativeWidgets()
{
    if (!IsUsingNativeLayout()) return;
    SetTitleText(NativeTitle);
    SetContinueButton(NativeContinue);
    SetSettingsButton(NativeSettings);
    SetExitButton(NativeExit);
}
