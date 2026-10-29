// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/SurvivalHUDWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"

USurvivalHUDWidget::USurvivalHUDWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    BackgroundColor = FLinearColor::Transparent;
    PanelColor = FLinearColor(0.025f, 0.035f, 0.05f, 0.86f);
    PanelSize = FVector2D(340.0f, 100.0f);
    PanelPadding = 16.0f;
    BodyFontSize = 22;
    FontOutlineSize = 1;
    SetVisibility(ESlateVisibility::Collapsed);
}

void USurvivalHUDWidget::BuildNativeLayout()
{
    NativeHealthPanel = AddPanel(PanelSize, FVector2D(32.0f, -32.0f),
        FVector2D(0.0f, 1.0f), FVector2D(0.0f, 1.0f));
    UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>();
    NativeHealthPanel->SetContent(Body);
    NativeHealthText = AddText(Body, FText::GetEmpty());
    NativeHealthText->SetAutoWrapText(false);
    NativeHealthBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("SurvivalHealthBar"));
    NativeHealthBar->SetVisibility(ESlateVisibility::HitTestInvisible);
    UVerticalBoxSlot* BarSlot = Body->AddChildToVerticalBox(NativeHealthBar);
    BarSlot->SetPadding(FMargin(0.0f, 6.0f, 0.0f, 2.0f));
    BarSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    DisplayedHealth = -1.0f;
    DisplayedMaximumHealth = -1.0f;
}

void USurvivalHUDWidget::ApplyNativeStyle()
{
    Super::ApplyNativeStyle();
    if (IsDesignTime())
    {
        SetVisibility(ESlateVisibility::HitTestInvisible);
        SetHealth(75.0f, 100.0f);
    }
}

void USurvivalHUDWidget::SetHealth(float CurrentHealth, float MaximumHealth)
{
    if (!IsValid(NativeHealthText) || !IsValid(NativeHealthBar)
        || !FMath::IsFinite(CurrentHealth) || !FMath::IsFinite(MaximumHealth)
        || MaximumHealth <= 0.0f)
    {
        return;
    }

    const float Health = FMath::Clamp(CurrentHealth, 0.0f, MaximumHealth);
    if (Health == DisplayedHealth && MaximumHealth == DisplayedMaximumHealth) return;
    DisplayedHealth = Health;
    DisplayedMaximumHealth = MaximumHealth;

    FNumberFormattingOptions NumberFormat;
    NumberFormat.SetMaximumFractionalDigits(1);
    NativeHealthText->SetText(FText::Format(
        NSLOCTEXT("SurvivalHUD", "Health", "Health  {0} / {1}"),
        FText::AsNumber(Health, &NumberFormat), FText::AsNumber(MaximumHealth, &NumberFormat)));
    const float Fraction = FMath::Clamp(Health / MaximumHealth, 0.0f, 1.0f);
    NativeHealthBar->SetPercent(Fraction);
    NativeHealthBar->SetFillColorAndOpacity(Fraction <= 0.25f
        ? FLinearColor(0.9f, 0.08f, 0.06f, 1.0f)
        : FLinearColor(0.12f, 0.75f, 0.32f, 1.0f));
}
