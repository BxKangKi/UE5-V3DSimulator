// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "UI/ResponsiveHUDWidget.h"
#include "UI/HUDLegacyStyle.h"
#include "UI/NativeWidgetTreeNames.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelWidget.h"
#include "Components/ScaleBox.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBoxSlot.h"

namespace
{
    double SafeDimension(double Value, double Fallback)
    {
        return FMath::Clamp(FMath::IsFinite(Value) ? Value : Fallback, 100.0, 16384.0);
    }
}

UResponsiveHUDWidget::UResponsiveHUDWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    FontObject = V3DHUDLegacyStyle::FindFontForConstructor();
}

FVector2D UResponsiveHUDWidget::GetSafeDesignResolution() const
{
    return FVector2D(SafeDimension(DesignResolution.X, 1920.0), SafeDimension(DesignResolution.Y, 1080.0));
}

bool UResponsiveHUDWidget::IsUsingNativeLayout() const
{
    return bUseNativeLayout && IsValid(WidgetTree) && IsValid(NativeViewportRoot)
        && WidgetTree->RootWidget == NativeViewportRoot && IsValid(NativeScaleRoot)
        && IsValid(NativeLayout);
}

bool UResponsiveHUDWidget::AcceptWidgetAssignment(const UWidget* Widget) const
{
    if (!IsUsingNativeLayout() || !Widget) return true;
    for (const UWidget* Current = Widget; Current; Current = Current->GetParent())
    {
        if (Current == NativeLayout) return true;
    }
    return false;
}

void UResponsiveHUDWidget::EnsureNativeLayout()
{
    if (!bUseNativeLayout || IsUsingNativeLayout()) return;
    // A fresh tree excludes hidden legacy Blueprint controls and makes generated names
    // independent of old designer nodes retained by undo/reinstancing.
    WidgetTree = NewObject<UWidgetTree>(this, NAME_None, RF_Transient);

    StyledPanels.Reset();
    StyledTexts.Reset();
    StyledTitles.Reset();
    StyledButtons.Reset();
    StyledButtonSizes.Reset();
    PositionedPanels.Reset();

    // All generated children belong to this instance's tree, never a CDO or Blueprint template.
    NativeViewportRoot = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
    WidgetTree->RootWidget = NativeViewportRoot;
    NativeViewportRoot->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

    // Backgrounds use the entire allotted viewport, independently of the controls' aspect
    // ratio. Keeping this outside ScaleToFit prevents uncovered bars on tall or wide screens.
    NativeBackground = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
    NativeBackground->SetVisibility(ESlateVisibility::HitTestInvisible);
    UOverlaySlot* BackgroundSlot = NativeViewportRoot->AddChildToOverlay(NativeBackground);
    BackgroundSlot->SetHorizontalAlignment(HAlign_Fill);
    BackgroundSlot->SetVerticalAlignment(VAlign_Fill);

    NativeScaleRoot = WidgetTree->ConstructWidget<UScaleBox>(UScaleBox::StaticClass());
    UOverlaySlot* ContentSlot = NativeViewportRoot->AddChildToOverlay(NativeScaleRoot);
    ContentSlot->SetHorizontalAlignment(HAlign_Fill);
    ContentSlot->SetVerticalAlignment(VAlign_Fill);
    NativeScaleRoot->SetStretch(EStretch::ScaleToFit);
    NativeScaleRoot->SetStretchDirection(EStretchDirection::Both);
    // ScaleToFit already receives the viewport's DPI-adjusted space. Preserve that transform;
    // cancelling it would shrink the reference canvas a second time on high-DPI displays.
    NativeScaleRoot->SetIgnoreInheritedScale(false);
    NativeScaleRoot->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

    NativeReferenceSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
    NativeReferenceSize->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
    NativeScaleRoot->SetContent(NativeReferenceSize);
    if (UScaleBoxSlot* ReferenceSizeSlot = Cast<UScaleBoxSlot>(NativeReferenceSize->Slot))
    {
        ReferenceSizeSlot->SetHorizontalAlignment(HAlign_Center);
        ReferenceSizeSlot->SetVerticalAlignment(VAlign_Center);
    }

    NativeLayout = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
    NativeLayout->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
    NativeReferenceSize->SetContent(NativeLayout);

    BuildNativeLayout();
    ApplyNativeStyle();
    BindNativeWidgets();
    V3DNativeWidgetTree::StabilizeNames(WidgetTree);
    V3DNativeWidgetTree::BindBlueprintWidgets(this);
}

#if WITH_EDITOR
void UResponsiveHUDWidget::PrepareNativeDesignerLayout()
{
    EnsureNativeLayout();
}
#endif

TSharedRef<SWidget> UResponsiveHUDWidget::RebuildWidget()
{
    EnsureNativeLayout();
    return Super::RebuildWidget();
}

void UResponsiveHUDWidget::NativePreConstruct()
{
    Super::NativePreConstruct();
    if (IsUsingNativeLayout())
    {
        BindNativeWidgets();
        ApplyNativeStyle();
    }
}

void UResponsiveHUDWidget::NativeConstruct()
{
    Super::NativeConstruct();
    if (IsUsingNativeLayout())
    {
        BindNativeWidgets();
        ApplyNativeStyle();
    }
}

void UResponsiveHUDWidget::SynchronizeProperties()
{
    Super::SynchronizeProperties();
    if (IsUsingNativeLayout()) ApplyNativeStyle();
}

void UResponsiveHUDWidget::RefreshAppearance()
{
    if (IsUsingNativeLayout()) ApplyNativeStyle();
}

UBorder* UResponsiveHUDWidget::AddPanel(FVector2D Size, FVector2D Position, FVector2D Anchors, FVector2D Alignment)
{
    if (!IsValid(NativeLayout) || !IsValid(WidgetTree)) return nullptr;
    UBorder* Panel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
    Panel->SetBrushColor(PanelColor);
    Panel->SetPadding(FMargin(FMath::Clamp(PanelPadding, 0.f, 200.f)));
    UCanvasPanelSlot* PanelCanvasSlot = NativeLayout->AddChildToCanvas(Panel);
    PanelCanvasSlot->SetAnchors(FAnchors(Anchors.X, Anchors.Y));
    PanelCanvasSlot->SetAlignment(Alignment);
    PanelCanvasSlot->SetPosition(Position);
    PanelCanvasSlot->SetSize(FVector2D(SafeDimension(Size.X, 960), SafeDimension(Size.Y, 720)));
    StyledPanels.Add(Panel);
    if (Size.Equals(PanelSize)) PositionedPanels.Add(Panel);
    return Panel;
}

UTextBlock* UResponsiveHUDWidget::AddText(UPanelWidget* Parent, const FText& Text, bool bTitle)
{
    if (!IsValid(Parent) || !IsValid(WidgetTree)) return nullptr;
    UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
    Label->SetText(Text);
    Label->SetAutoWrapText(true);
    Label->SetColorAndOpacity(FSlateColor(TextColor));
    Label->SetVisibility(ESlateVisibility::HitTestInvisible);
    FSlateFontInfo Font = Label->GetFont();
    V3DHUDLegacyStyle::ApplyFont(Font, FontObject, bTitle ? TitleFontSize : BodyFontSize, FontOutlineSize);
    Label->SetFont(Font);
    Parent->AddChild(Label);
    if (bTitle) StyledTitles.Add(Label); else StyledTexts.Add(Label);
    if (UVerticalBoxSlot* TextSlot = Cast<UVerticalBoxSlot>(Label->Slot)) TextSlot->SetPadding(FMargin(0, 4));
    return Label;
}

UButton* UResponsiveHUDWidget::AddButton(UPanelWidget* Parent, const FText& Text)
{
    if (!IsValid(Parent) || !IsValid(WidgetTree)) return nullptr;
    USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
    Size->SetMinDesiredHeight(FMath::Clamp(ButtonHeight, 20.f, 200.f));
    Parent->AddChild(Size);
    if (UVerticalBoxSlot* ButtonContainerSlot = Cast<UVerticalBoxSlot>(Size->Slot)) ButtonContainerSlot->SetPadding(FMargin(0, 4));
    UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
    V3DHUDLegacyStyle::ApplyButton(*Button, ButtonColor, HoveredButtonTint);
    Size->SetContent(Button);
    UTextBlock* Label = AddText(Button, Text);
    Label->SetJustification(ETextJustify::Center);
    if (USizeBoxSlot* ButtonSizeSlot = Cast<USizeBoxSlot>(Button->Slot)) ButtonSizeSlot->SetVerticalAlignment(VAlign_Fill);
    StyledButtonSizes.Add(Size);
    StyledButtons.Add(Button);
    return Button;
}

void UResponsiveHUDWidget::ApplyNativeStyle()
{
    const FVector2D Resolution = GetSafeDesignResolution();
    if (NativeReferenceSize)
    {
        NativeReferenceSize->SetWidthOverride(Resolution.X);
        NativeReferenceSize->SetHeightOverride(Resolution.Y);
    }
    if (NativeBackground) NativeBackground->SetBrushColor(BackgroundColor);
    const float SafePanelPadding = FMath::Clamp(FMath::IsFinite(PanelPadding) ? PanelPadding : 24.f, 0.f, 200.f);
    for (UBorder* Panel : StyledPanels)
    {
        if (!Panel) continue;
        Panel->SetBrushColor(PanelColor);
        Panel->SetPadding(FMargin(SafePanelPadding));
    }
    for (UBorder* Panel : PositionedPanels)
    {
        if (Panel)
        {
            if (UCanvasPanelSlot* PanelCanvasSlot = Cast<UCanvasPanelSlot>(Panel->Slot))
                PanelCanvasSlot->SetSize(FVector2D(SafeDimension(PanelSize.X, 960), SafeDimension(PanelSize.Y, 720)));
        }
    }
    auto StyleText = [this](const TArray<TObjectPtr<UTextBlock>>& Labels, int32 FontSize)
    {
        for (UTextBlock* Label : Labels)
        {
            if (!Label) continue;
            FSlateFontInfo Font = Label->GetFont();
            V3DHUDLegacyStyle::ApplyFont(Font, FontObject, FontSize, FontOutlineSize);
            Label->SetFont(Font);
            Label->SetColorAndOpacity(FSlateColor(TextColor));
        }
    };
    StyleText(StyledTexts, BodyFontSize);
    StyleText(StyledTitles, TitleFontSize);
    for (UButton* Button : StyledButtons) if (Button) V3DHUDLegacyStyle::ApplyButton(*Button, ButtonColor, HoveredButtonTint);
    for (USizeBox* Size : StyledButtonSizes) if (Size)
        Size->SetMinDesiredHeight(FMath::Clamp(FMath::IsFinite(ButtonHeight) ? ButtonHeight : 44.f, 20.f, 200.f));
}
