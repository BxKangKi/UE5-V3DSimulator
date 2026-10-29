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
    UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
    NativeWorldPanel->SetContent(Box);

    UOverlay* Header = WidgetTree->ConstructWidget<UOverlay>();
    Box->AddChildToVerticalBox(Header);
    UTextBlock* Title = AddText(Header, NSLOCTEXT("V3DMenu", "WorldTitle", "World"), true);
    Title->SetAutoWrapText(false);
    Title->SetJustification(ETextJustify::Center);
    UOverlaySlot* TitleSlot = Cast<UOverlaySlot>(Title->Slot);
    TitleSlot->SetHorizontalAlignment(HAlign_Center);
    TitleSlot->SetVerticalAlignment(VAlign_Top);

    auto AddNavigation = [this, Header](const FText& Text, bool bRight)
    {
        UButton* Button = AddButton(Header, Text);
        UOverlaySlot* Slot = Cast<UOverlaySlot>(Button->GetParent()->Slot);
        Slot->SetPadding(bRight ? FMargin(0.0f, 0.0f, 32.0f, 0.0f) : FMargin(32.0f, 0.0f, 0.0f, 0.0f));
        Slot->SetHorizontalAlignment(bRight ? HAlign_Right : HAlign_Left);
        Slot->SetVerticalAlignment(VAlign_Center);
        if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(Button->GetContent()->Slot))
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
    NativeWorldEmpty = AddText(Scroll, NSLOCTEXT("V3DMenu", "NoWorlds", "No built worlds found. Build a world project in Projects, then refresh this list."));
    NativeWorldEmpty->SetVisibility(IsDesignTime() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
    UVerticalBoxSlot* ListSlot = Box->AddChildToVerticalBox(Scroll);
    ListSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    if (IsDesignTime())
    {
        AddButton(Scroll, NSLOCTEXT("V3DMenu", "PreviewWorldOne", "Example World"));
        AddButton(Scroll, NSLOCTEXT("V3DMenu", "PreviewWorldTwo", "City Simulation"));
    }
}

void UWorldSelectionWidget::ApplyNativeStyle()
{
    Super::ApplyNativeStyle();
}

void UWorldSelectionWidget::BindNativeWidgets()
{
    if (!IsUsingNativeLayout()) return;
    SetSelectionListPanel(NativeWorldList);
    SetBackButton(NativeWorldBack);
    SetRefreshButton(NativeWorldRefresh);
}
