// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/LoadingHUDWidget.h"
#include "UI/HUDLegacyStyle.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/SizeBox.h"
#include "InputCoreTypes.h"
#include "System/MultiplayerWorldSubSystem.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "System/GameManagerSubSystem.h"
#include "System/GameUpdateSubSystem.h"

ULoadingHUDWidget::ULoadingHUDWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    PanelSize = FVector2D(1920.f, 1080.f);
    BackgroundColor = V3DHUDLegacyStyle::LoadingBackground;
    AccentColor = V3DHUDLegacyStyle::ProgressFill;
    BodyFontSize = 32;
    FontOutlineSize = 1;
    SetIsFocusable(true);
}

void ULoadingHUDWidget::BuildNativeLayout()
{
    UBorder* Panel = AddPanel(PanelSize, FVector2D::ZeroVector);
    UOverlay* Body = WidgetTree->ConstructWidget<UOverlay>();
    Panel->SetContent(Body);
    NativeTitle = nullptr;
    NativeProgress = WidgetTree->ConstructWidget<UProgressBar>();
    UOverlaySlot* ProgressSlot = Body->AddChildToOverlay(NativeProgress);
    ProgressSlot->SetHorizontalAlignment(HAlign_Fill);
    ProgressSlot->SetVerticalAlignment(VAlign_Bottom);
    ProgressSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 20.f));
    NativeBack = AddButton(Body, NSLOCTEXT("V3DHUD", "LoadingBack", "Back"));
    NativeBack->Rename(TEXT("LoadingBackButton"), WidgetTree, REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
    if (USizeBox* BackSize = Cast<USizeBox>(NativeBack->GetParent()))
    {
        BackSize->SetWidthOverride(180.f);
        if (UOverlaySlot* BackSlot = Cast<UOverlaySlot>(BackSize->Slot))
        {
            BackSlot->SetHorizontalAlignment(HAlign_Left);
            BackSlot->SetVerticalAlignment(VAlign_Bottom);
            BackSlot->SetPadding(FMargin(60.f, 0.f, 0.f, 60.f));
        }
    }
    NativeBack->OnClicked.AddUniqueDynamic(this, &ULoadingHUDWidget::BackFromConnection);
    NativeBack->GetParent()->SetVisibility(ESlateVisibility::Collapsed);
    NativeStatus = AddText(Body, LoadingTitle);
    NativeStatus->SetJustification(ETextJustify::Right);
    if (UOverlaySlot* Slot = Cast<UOverlaySlot>(NativeStatus->Slot))
    {
        Slot->SetHorizontalAlignment(HAlign_Right);
        Slot->SetVerticalAlignment(VAlign_Bottom);
        Slot->SetPadding(FMargin(0.f, 0.f, 60.f, 60.f));
    }
}

void ULoadingHUDWidget::ApplyNativeStyle()
{
    Super::ApplyNativeStyle();
    if (IsValid(NativeTitle)) NativeTitle->SetText(LoadingTitle);
    if (IsValid(NativeProgress)) NativeProgress->SetFillColorAndOpacity(AccentColor);
    if (IsDesignTime())
    {
        if (IsValid(NativeProgress)) NativeProgress->SetPercent(0.65f);
        if (IsValid(NativeStatus)) NativeStatus->SetText(NSLOCTEXT("V3DHUD", "LoadingPreview", "Loading world data... 65%"));
    }
}

void ULoadingHUDWidget::NativeConstruct()
{
    Super::NativeConstruct();
    if (IsDesignTime()) return;
    if (IsValid(NativeBack)) NativeBack->OnClicked.AddUniqueDynamic(this, &ULoadingHUDWidget::BackFromConnection);
    RefreshLoadingStatus();
    if (UpdateHandle == INDEX_NONE)
    {
        if (UGameUpdateSubSystem* Updates = UGameUpdateSubSystem::Get(this))
        {
            UpdateHandle = Updates->RegisterUpdate(this,
                [WeakThis = TWeakObjectPtr<ULoadingHUDWidget>(this)](float DeltaSeconds)
                {
                    if (ULoadingHUDWidget* Widget = WeakThis.Get()) Widget->UpdateStatus(DeltaSeconds);
                }, 60);
        }
    }
}

void ULoadingHUDWidget::NativeDestruct()
{
    if (UGameUpdateSubSystem* Updates = UGameUpdateSubSystem::Get(this))
        Updates->UnregisterUpdate(UpdateHandle);
    if (IsValid(NativeBack)) NativeBack->OnClicked.RemoveDynamic(this, &ULoadingHUDWidget::BackFromConnection);
    UpdateHandle = INDEX_NONE;
    RefreshElapsed = 0.0f;
    Super::NativeDestruct();
}

void ULoadingHUDWidget::UpdateStatus(float DeltaSeconds)
{
    if (!IsVisible() || !FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.0f) return;
    RefreshElapsed += DeltaSeconds;
    const float Interval = FMath::IsFinite(RefreshIntervalSeconds)
        ? FMath::Clamp(RefreshIntervalSeconds, 0.05f, 1.0f) : 0.1f;
    if (RefreshElapsed >= Interval)
    {
        RefreshElapsed = 0.0f;
        RefreshLoadingStatus();
    }
}

void ULoadingHUDWidget::RefreshLoadingStatus()
{
    if (IsDesignTime() || !IsUsingNativeLayout()) return;
    const UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this);
    const bool bConnection = Multiplayer && Multiplayer->CanCancelConnection();
    if (IsValid(NativeBack) && NativeBack->GetParent())
        NativeBack->GetParent()->SetVisibility(bConnection ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    if (IsValid(NativeProgress)) NativeProgress->SetIsMarquee(bConnection && Multiplayer->IsConnectionPending());
    if (bConnection)
    {
        if (IsValid(NativeStatus)) NativeStatus->SetText(Multiplayer->GetConnectionStatusText());
        return;
    }
    const UGameManagerSubSystem* Manager = UGameManagerSubSystem::FindGameManager(this);
    const float ReportedProgress = IsValid(Manager) ? Manager->GetWorldSystemsLoadingStatus() : 0.0f;
    const float Progress = FMath::IsFinite(ReportedProgress) ? FMath::Clamp(ReportedProgress, 0.0f, 1.0f) : 0.0f;
    if (IsValid(NativeProgress) && !FMath::IsNearlyEqual(NativeProgress->GetPercent(), Progress))
        NativeProgress->SetPercent(Progress);
    const FText Status = FText::Format(NSLOCTEXT("V3DHUD", "LoadingPercent", "Loading world data... {0}%"),
        FText::AsNumber(FMath::RoundToInt(Progress * 100.0f)));
    if (IsValid(NativeStatus) && !NativeStatus->GetText().EqualTo(Status)) NativeStatus->SetText(Status);
}

void ULoadingHUDWidget::BackFromConnection()
{
    if (IsDesignTime()) return;
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
        Multiplayer->CancelClientConnection(this);
}

FReply ULoadingHUDWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
    if (InKeyEvent.GetKey() == EKeys::Escape || InKeyEvent.GetKey() == EKeys::Gamepad_FaceButton_Right)
    {
        if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this);
            Multiplayer && Multiplayer->CanCancelConnection())
        {
            BackFromConnection();
            return FReply::Handled();
        }
    }
    return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}
