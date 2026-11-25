// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/WorldSelectionWidget.h"

#include "Components/PanelWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/BorderSlot.h"
#include "Components/SizeBox.h"
#include "UI/HUDStyle.h"

namespace
{
    FString NormalizeWorldSelectionText(FString Value)
    {
        Value.TrimStartAndEndInline();
        return Value;
    }
}

UWorldSelectionWidget::UWorldSelectionWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    PanelSize = FVector2D(1920.0, 1080.0);
    PanelPadding = 0.0f;
    ButtonHeight = 64.0f;
    BodyFontSize = 32;
    TitleFontSize = 100;
}

void UWorldSelectionWidget::NativeConstruct()
{
    Super::NativeConstruct();
    if (IsDesignTime()) return;

    if (IsValid(GetSelectionListPanel()))
    {
        RebuildWorldButtons();
    }
}

void UWorldSelectionWidget::SetWorldSelectionData(const TMap<FString, FString>& Values)
{
    Super::SetWorldSelectionData(Values);
    RebuildWorldButtons();
}

void UWorldSelectionWidget::SetWorldListPanel(UPanelWidget* InWorldListPanel)
{
    if (!AcceptWidgetAssignment(InWorldListPanel)) return;
    SetSelectionListPanel(InWorldListPanel);
    RebuildWorldButtons();
}

void UWorldSelectionWidget::RebuildWorldButtons()
{
    if (IsDesignTime()) return;
    ClearGeneratedSelectionEntries();

    if (!IsValid(GetSelectionListPanel()))
    {
        // Blueprint commonly assigns the panel from Construct after selection data arrives.
        // Defer generation silently; SetWorldListPanel() rebuilds the list immediately.
        return;
    }

    TArray<TPair<FString, FString>> SortedWorlds;
    const TMap<FString, FString> FolderMap = GetFolderNameMap();
    for (const TPair<FString, FString>& Pair : FolderMap)
    {
        const FString FolderName = NormalizeWorldSelectionText(Pair.Key);
        if (FolderName.IsEmpty())
        {
            continue;
        }

        FString DisplayName = NormalizeWorldSelectionText(Pair.Value);
        if (DisplayName.IsEmpty())
        {
            DisplayName = FolderName;
        }
        SortedWorlds.Emplace(FolderName, DisplayName);
    }

    SortedWorlds.Sort([](const TPair<FString, FString>& Left, const TPair<FString, FString>& Right)
    {
        const int32 DisplayCompare = Left.Value.Compare(Right.Value, ESearchCase::IgnoreCase);
        return DisplayCompare != 0
            ? DisplayCompare < 0
            : Left.Key.Compare(Right.Key, ESearchCase::IgnoreCase) < 0;
    });

    if (NativeWorldEmpty)
        NativeWorldEmpty->SetVisibility(SortedWorlds.IsEmpty() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);

    for (const TPair<FString, FString>& EntryData : SortedWorlds)
    {
        if (!IsValid(AddGeneratedSelectionEntry(EntryData.Key, EntryData.Value)))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("WorldSelectionWidget failed to create a generated button for %s."),
                *EntryData.Key);
        }
    }
}

void UWorldSelectionWidget::ClearWorldButtons()
{
    ClearGeneratedSelectionEntries();
}

void UWorldSelectionWidget::OnSelectionEntryActivated(const FString& SelectionKey)
{
    // Disable before travel so a second Blueprint/parent-button callback from the same mouse
    // release cannot route the UI back to the start screen.
    SetIsEnabled(false);
    if (!OpenWorldByFolderName(SelectionKey))
    {
        SetIsEnabled(true);
    }
}


void UWorldSelectionWidget::BuildNativeLayout()
{
    SetIsFocusable(true);
    NativeWorldPanel = AddPanel(PanelSize, FVector2D::ZeroVector);
    UOverlay* PanelRoot = WidgetTree->ConstructWidget<UOverlay>();
    NativeWorldPanel->SetContent(PanelRoot);
    UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
    if (UOverlaySlot* BoxSlot = PanelRoot->AddChildToOverlay(Box))
    {
        BoxSlot->SetHorizontalAlignment(HAlign_Fill);
        BoxSlot->SetVerticalAlignment(VAlign_Fill);
    }

    UOverlay* Header = WidgetTree->ConstructWidget<UOverlay>();
    Box->AddChildToVerticalBox(Header);
    UTextBlock* Title = AddText(Header, NSLOCTEXT("V3DMenu", "V3DWorldsTitle", "V3D Worlds"), true);
    Title->SetAutoWrapText(false);
    Title->SetJustification(ETextJustify::Center);
    UOverlaySlot* TitleSlot = Cast<UOverlaySlot>(Title->Slot);
    TitleSlot->SetHorizontalAlignment(HAlign_Center);
    TitleSlot->SetVerticalAlignment(VAlign_Top);

    auto AddNavigation = [this, Header](const FText& Text, bool bRight)
    {
        UButton* Button = AddButton(Header, Text);
        if (UWidget* ParentWidget = Button ? Button->GetParent() : nullptr)
            if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(ParentWidget->Slot))
            {
                OverlaySlot->SetPadding(bRight ? FMargin(0.0f, 0.0f, 32.0f, 0.0f) : FMargin(32.0f, 0.0f, 0.0f, 0.0f));
                OverlaySlot->SetHorizontalAlignment(bRight ? HAlign_Right : HAlign_Left);
                OverlaySlot->SetVerticalAlignment(VAlign_Center);
            }
        if (UWidget* ContentWidget = Button ? Button->GetContent() : nullptr)
            if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(ContentWidget->Slot))
                ContentSlot->SetPadding(FMargin(12.0f));
        if (UTextBlock* Label = Cast<UTextBlock>(Button->GetContent()))
        {
            Label->SetMinDesiredWidth(128.0f);
            Label->SetAutoWrapText(false);
        }
        return Button;
    };
    NativeWorldBack = AddNavigation(NSLOCTEXT("V3DMenu", "Back", "Back"), false);
    NativeWorldRefresh = AddNavigation(NSLOCTEXT("V3DMenu", "Refresh", "Refresh"), true);

    UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("NativeWorldList"));
    NativeWorldList = Scroll;
    NativeWorldEmpty = AddText(Scroll, NSLOCTEXT("V3DMenu", "NoWorlds", "No built worlds found in V3Ds/Worlds. Build a World project in Editor, then refresh this list."));
    NativeWorldEmpty->SetVisibility(IsDesignTime() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
    UVerticalBoxSlot* ListSlot = Box->AddChildToVerticalBox(Scroll);
    ListSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
#if WITH_EDITOR
    if (IsDesignTime())
    {
        AddButton(Scroll, NSLOCTEXT("V3DMenu", "PreviewWorldOne", "Example World"));
        AddButton(Scroll, NSLOCTEXT("V3DMenu", "PreviewWorldTwo", "City Simulation"));
    }
#endif

    // Modal warning overlay for invalid / non-World .v3d archives
    NativeWarningOverlay = WidgetTree->ConstructWidget<UBorder>();
    NativeWarningOverlay->SetBrushColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.65f));
    NativeWarningOverlay->SetVisibility(ESlateVisibility::Collapsed);
    if (UOverlaySlot* WarnOverlaySlot = PanelRoot->AddChildToOverlay(NativeWarningOverlay))
    {
        WarnOverlaySlot->SetHorizontalAlignment(HAlign_Fill);
        WarnOverlaySlot->SetVerticalAlignment(VAlign_Fill);
    }

    USizeBox* WarnCardSize = WidgetTree->ConstructWidget<USizeBox>();
    WarnCardSize->SetWidthOverride(780.0f);
    NativeWarningOverlay->SetContent(WarnCardSize);
    if (UBorderSlot* OverlayContentSlot = Cast<UBorderSlot>(WarnCardSize->Slot))
    {
        OverlayContentSlot->SetHorizontalAlignment(HAlign_Center);
        OverlayContentSlot->SetVerticalAlignment(VAlign_Center);
    }

    NativeWarningCard = WidgetTree->ConstructWidget<UBorder>();
    NativeWarningCard->SetPadding(FMargin(36.0f, 28.0f));
    WarnCardSize->SetContent(NativeWarningCard);

    UVerticalBox* WarnBox = WidgetTree->ConstructWidget<UVerticalBox>();
    NativeWarningCard->SetContent(WarnBox);

    NativeWarningTitle = AddText(WarnBox, FText::FromString(TEXT("Warning")), true);
    NativeWarningTitle->SetAutoWrapText(true);
    if (UVerticalBoxSlot* TitleWarnSlot = Cast<UVerticalBoxSlot>(NativeWarningTitle->Slot))
    {
        TitleWarnSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 16.0f));
    }

    NativeWarningMessage = AddText(WarnBox, FText::GetEmpty(), false);
    NativeWarningMessage->SetAutoWrapText(true);
    if (UVerticalBoxSlot* MsgWarnSlot = Cast<UVerticalBoxSlot>(NativeWarningMessage->Slot))
    {
        MsgWarnSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 24.0f));
    }

    NativeWarningOkButton = AddButton(WarnBox, FText::FromString(TEXT("OK")));
    if (UWidget* OkParent = NativeWarningOkButton ? NativeWarningOkButton->GetParent() : nullptr)
    {
        if (UVerticalBoxSlot* OkSlot = Cast<UVerticalBoxSlot>(OkParent->Slot))
        {
            OkSlot->SetHorizontalAlignment(HAlign_Right);
        }
    }
}

void UWorldSelectionWidget::ApplyNativeStyle()
{
    Super::ApplyNativeStyle();
    const bool bDarkMode = V3DHUDStyle::IsDarkMode(this);
    const V3DHUDStyle::FAuthoringPalette Palette = V3DHUDStyle::GetAuthoringPalette(bDarkMode);
    if (IsValid(NativeWarningCard))
    {
        NativeWarningCard->SetBrushColor(Palette.Panel);
    }
    if (IsValid(NativeWarningTitle))
    {
        FSlateFontInfo WarnTitleFont = NativeWarningTitle->GetFont();
        V3DHUDStyle::ApplyFontWithWeight(WarnTitleFont, FontObject, 38, true, FLinearColor(0.95f, 0.35f, 0.28f, 1.0f));
        NativeWarningTitle->SetFont(WarnTitleFont);
        NativeWarningTitle->SetColorAndOpacity(FSlateColor(FLinearColor(0.95f, 0.35f, 0.28f, 1.0f)));
    }
    if (IsValid(NativeWarningMessage))
    {
        FSlateFontInfo WarnMsgFont = NativeWarningMessage->GetFont();
        V3DHUDStyle::ApplyFontWithWeight(WarnMsgFont, FontObject, 24, false, Palette.Text);
        NativeWarningMessage->SetFont(WarnMsgFont);
        NativeWarningMessage->SetColorAndOpacity(FSlateColor(Palette.Text));
    }
}

void UWorldSelectionWidget::BindNativeWidgets()
{
    if (!IsUsingNativeLayout()) return;
    SetSelectionListPanel(NativeWorldList);
    SetBackButton(NativeWorldBack);
    SetRefreshButton(NativeWorldRefresh);
    if (!IsDesignTime() && IsValid(NativeWarningOkButton))
    {
        NativeWarningOkButton->OnClicked.RemoveDynamic(this, &UWorldSelectionWidget::HideWarningDialog);
        NativeWarningOkButton->OnClicked.AddDynamic(this, &UWorldSelectionWidget::HideWarningDialog);
    }
}

void UWorldSelectionWidget::ShowWarningDialog(const FString& InTitle, const FString& InMessage)
{
    SetIsEnabled(true);
    if (IsValid(NativeWarningTitle))
    {
        NativeWarningTitle->SetText(FText::FromString(InTitle));
    }
    if (IsValid(NativeWarningMessage))
    {
        NativeWarningMessage->SetText(FText::FromString(InMessage));
    }
    if (IsValid(NativeWarningOverlay))
    {
        NativeWarningOverlay->SetVisibility(ESlateVisibility::Visible);
    }
}

void UWorldSelectionWidget::HideWarningDialog()
{
    if (IsValid(NativeWarningOverlay))
    {
        NativeWarningOverlay->SetVisibility(ESlateVisibility::Collapsed);
    }
    SetIsEnabled(true);
}
