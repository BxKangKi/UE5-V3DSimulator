// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file CreatorHUDWidget.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "UI/CreatorHUDWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "System/GameUpdateSubSystem.h"
#include "UI/MenuButtonWidget.h"

UCreatorHUDWidget::UCreatorHUDWidget()
{
    BackgroundColor = FLinearColor::Transparent;
    PanelSize = FVector2D(1080, 760);
    PanelPadding = 16.f;
    PanelColor = FLinearColor(.082754f, .082754f, .082754f, .9f);
    BodyFontSize = 18;
    TitleFontSize = 30;
    FontOutlineSize = 1;
    ButtonHeight = 44.f;
}

void UCreatorHUDWidget::BuildNativeLayout()
{
    NativeToolbarButtons.Reset();
    NativeToolbarSizes.Reset();
    NativeInventoryButtons.Reset();
    VisibleInventorySignature.Reset();

    NativeStatusPanel = AddPanel(StatusPanelSize, StatusPanelOffset, FVector2D::ZeroVector, FVector2D::ZeroVector);
    UVerticalBox* StatusBox = WidgetTree->ConstructWidget<UVerticalBox>();
    NativeStatusPanel->SetContent(StatusBox);
    AddText(StatusBox, NSLOCTEXT("CreatorHUD", "Title", "V3D CREATOR"), true);
    NativeStatus = AddText(StatusBox, FText::FromString(TEXT("Creator mode\nSlot 1 / 7\nSelect an item to begin")));
    NativePlacement = AddText(StatusBox, FText::FromString(TEXT("X 0  Y 0  Z 0\nPlacement: SURFACE\nSnap: ON (100 cm)")));

    NativeMessage = AddText(StatusBox, FText::GetEmpty());

    NativeToolbarPanel = AddPanel(ToolbarSize, ToolbarOffset, FVector2D(.5, 1), FVector2D(.5, 1));
    NativeToolbar = WidgetTree->ConstructWidget<UUniformGridPanel>(UUniformGridPanel::StaticClass(), TEXT("CreatorToolbarGrid"));
    NativeToolbar->SetSlotPadding(FMargin(3));
    NativeToolbarPanel->SetContent(NativeToolbar);

    NativeItemsPanel = AddPanel(PanelSize, FVector2D::ZeroVector);
    if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(NativeItemsPanel->Slot)) Slot->SetZOrder(10);
    UVerticalBox* ItemsBox = WidgetTree->ConstructWidget<UVerticalBox>();
    NativeItemsPanel->SetContent(ItemsBox);
    AddText(ItemsBox, NSLOCTEXT("CreatorHUD", "Browser", "Available items"), true);
    AddText(ItemsBox, NSLOCTEXT("CreatorHUD", "BrowserHelp", "Choose an item for the selected toolbar slot."));
    NativeItemsScroll = WidgetTree->ConstructWidget<UScrollBox>();
    ItemsBox->AddChildToVerticalBox(NativeItemsScroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    NativeItemGrid = WidgetTree->ConstructWidget<UUniformGridPanel>();
    NativeItemGrid->SetSlotPadding(FMargin(4));
    NativeItemsScroll->AddChild(NativeItemGrid);
    // Keep scene actions available inside the on-demand browser (I), without a permanent rail.
    UHorizontalBox* Actions = WidgetTree->ConstructWidget<UHorizontalBox>();
    ItemsBox->AddChildToVerticalBox(Actions);
    AddButton(Actions, NSLOCTEXT("CreatorHUD", "Snap", "Toggle snap"))->OnClicked.AddUniqueDynamic(this, &UCreatorHUDWidget::ToggleSnapFromUI);
    AddButton(Actions, NSLOCTEXT("CreatorHUD", "Save", "Save scene"))->OnClicked.AddUniqueDynamic(this, &UCreatorHUDWidget::SaveSceneFromUI);
    AddButton(Actions, NSLOCTEXT("CreatorHUD", "Bake", "Build project"))->OnClicked.AddUniqueDynamic(this, &UCreatorHUDWidget::BuildActiveProjectArchiveFromUI);
    UHorizontalBox* Paging = WidgetTree->ConstructWidget<UHorizontalBox>();
    ItemsBox->AddChildToVerticalBox(Paging);
    NativePreviousPage = AddButton(Paging, NSLOCTEXT("CreatorHUD", "Previous", "Previous"));
    NativePreviousPage->OnClicked.AddUniqueDynamic(this, &UCreatorHUDWidget::PreviousItemPage);
    NativePageText = AddText(Paging, FText::GetEmpty());
    if (UHorizontalBoxSlot* Slot = Cast<UHorizontalBoxSlot>(NativePageText->Slot))
    {
        Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        Slot->SetVerticalAlignment(VAlign_Center);
    }
    NativePageText->SetJustification(ETextJustify::Center);
    NativeNextPage = AddButton(Paging, NSLOCTEXT("CreatorHUD", "Next", "Next"));
    NativeNextPage->OnClicked.AddUniqueDynamic(this, &UCreatorHUDWidget::NextItemPage);
    AddButton(Paging, NSLOCTEXT("CreatorHUD", "Close", "Close"))->OnClicked.AddUniqueDynamic(this, &UCreatorHUDWidget::ToggleItemListFromUI);
    BindNativeWidgets();
    BuildToolbarButtons();
    RefreshToolbar();
    RefreshItemList();
}

void UCreatorHUDWidget::BindNativeWidgets()
{
    AssignedToolbarGrid = NativeToolbar;
    AssignedItemListScrollBox = NativeItemsScroll;
    AssignedItemListPanel = NativeItemsPanel;
    AssignedStatusText = NativeStatus;
    AssignedPlacementText = NativePlacement;
    AssignedMessageText = NativeMessage;
}

void UCreatorHUDWidget::ApplyNativeStyle()
{
    Super::ApplyNativeStyle();
    const auto UpdatePanel = [](UBorder* Panel, const FVector2D& Size, const FVector2D& Position)
    {
        if (Panel && FMath::IsFinite(Size.X) && FMath::IsFinite(Size.Y))
            if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Panel->Slot))
            {
                Slot->SetSize(FVector2D(FMath::Clamp(Size.X, 100., 16384.), FMath::Clamp(Size.Y, 100., 16384.)));
                if (FMath::IsFinite(Position.X) && FMath::IsFinite(Position.Y)) Slot->SetPosition(Position);
            }
    };
    UpdatePanel(NativeStatusPanel, StatusPanelSize, StatusPanelOffset);
    const float SlotExtent = GetToolbarSlotExtent();
    if (NativeToolbarPanel)
    {
        // Exact bounds prevent the shared panel's 100px minimum and stale Blueprint defaults
        // from stretching the square slots into the previous full-width toolbar.
        NativeToolbarPanel->SetPadding(FMargin(6));
        if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(NativeToolbarPanel->Slot))
        {
            Slot->SetSize(FVector2D(7.f * (SlotExtent + 6.f) + 12.f, SlotExtent + 18.f));
            Slot->SetPosition(FVector2D(FMath::IsFinite(ToolbarOffset.X) ? ToolbarOffset.X : 0.,
                FMath::IsFinite(ToolbarOffset.Y) ? ToolbarOffset.Y : -16.));
        }
    }
    for (USizeBox* Size : NativeToolbarSizes)
        if (Size)
        {
            Size->SetWidthOverride(SlotExtent);
            Size->SetHeightOverride(SlotExtent);
        }
    if (NativeMessage) NativeMessage->SetColorAndOpacity(FSlateColor(MutedTextColor));
    for (UMenuButtonWidget* Button : NativeToolbarButtons)
        if (Button)
        {
            Button->ButtonMinSize = FVector2D(SlotExtent, SlotExtent);
            Button->ContentPadding = FMargin(2);
            Button->FontObject = FontObject;
            Button->FontOutlineSize = FontOutlineSize;
            Button->ApplyButtonTheme(TextColor, ButtonColor, HoveredButtonTint, FMath::Clamp(BodyFontSize, 10, 12), SlotExtent);
        }
    for (UMenuButtonWidget* Button : NativeInventoryButtons)
        if (Button)
        {
            Button->ButtonMinSize.X = 120.f;
            Button->FontObject = FontObject;
            Button->FontOutlineSize = FontOutlineSize;
            Button->ApplyButtonTheme(TextColor, ButtonColor, HoveredButtonTint, BodyFontSize, ButtonHeight);
        }
    ApplyStatusVisibility();
    UpdatePlacementRegistration();
    RefreshToolbar();
    if (IsDesignTime() && NativeItemGrid) RefreshItemList();
}

float UCreatorHUDWidget::GetToolbarSlotExtent() const
{
    return FMath::Clamp(FMath::IsFinite(ToolbarSlotHeight) ? ToolbarSlotHeight : 54.f, 32.f, 96.f);
}

void UCreatorHUDWidget::BuildToolbarButtons()
{
    if (!NativeToolbar || NativeToolbarButtons.Num() == 7) return;
    NativeToolbar->ClearChildren();
    NativeToolbarButtons.Reset();
    NativeToolbarSizes.Reset();
    for (int32 Index = 0; Index < 7; ++Index)
    {
        UMenuButtonWidget* Button = Cast<UMenuButtonWidget>(UUserWidget::CreateWidgetInstance(*this, UMenuButtonWidget::StaticClass(), NAME_None));
        if (!Button) continue;
        const float SlotExtent = GetToolbarSlotExtent();
        Button->ButtonMinSize = FVector2D(SlotExtent, SlotExtent);
        Button->ContentPadding = FMargin(2);
        Button->FontObject = FontObject;
        Button->FontOutlineSize = FontOutlineSize;
        Button->ApplyButtonTheme(TextColor, ButtonColor, HoveredButtonTint, FMath::Clamp(BodyFontSize, 10, 12), SlotExtent);
        Button->ConfigureButton(FString::Printf(TEXT("Toolbar:%d"), Index), FString::FromInt(Index + 1));
        Button->SetClipping(EWidgetClipping::ClipToBounds);
        Button->OnButtonClicked.AddUniqueDynamic(this, &UCreatorHUDWidget::HandleNativeAction);
        USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>();
        Size->SetWidthOverride(SlotExtent);
        Size->SetHeightOverride(SlotExtent);
        Size->SetContent(Button);
        UUniformGridSlot* GridSlot = NativeToolbar->AddChildToUniformGrid(Size, 0, Index);
        GridSlot->SetHorizontalAlignment(HAlign_Center);
        GridSlot->SetVerticalAlignment(VAlign_Center);
        NativeToolbarButtons.Add(Button);
        NativeToolbarSizes.Add(Size);
    }
}

void UCreatorHUDWidget::ClearNativeInventory()
{
    for (UMenuButtonWidget* Button : NativeInventoryButtons)
        if (Button) Button->OnButtonClicked.RemoveDynamic(this, &UCreatorHUDWidget::HandleNativeAction);
    NativeInventoryButtons.Reset();
    if (NativeItemGrid) NativeItemGrid->ClearChildren();
}

void UCreatorHUDWidget::HandleNativeAction(const FString& Key)
{
    FString Prefix, IndexText;
    if (!Key.Split(TEXT(":"), &Prefix, &IndexText) || !IndexText.IsNumeric()) return;
    const int32 Index = FCString::Atoi(*IndexText);
    if (Prefix == TEXT("Toolbar")) SelectToolbarSlotFromUI(Index);
    else if (Prefix == TEXT("Item")) SelectAvailableItemFromUI(Index);
}

void UCreatorHUDWidget::PreviousItemPage()
{
    CurrentItemPage = FMath::Max(0, CurrentItemPage - 1);
    RefreshItemList();
}

void UCreatorHUDWidget::NextItemPage()
{
    ++CurrentItemPage;
    RefreshItemList();
}

static FString MakeKindLabel(EToolbarItemKind Kind)
{
    switch (Kind)
    {
    case EToolbarItemKind::Static:
        return TEXT("Static");
    case EToolbarItemKind::Weapon:
        return TEXT("Weapon");
    case EToolbarItemKind::Vehicle:
        return TEXT("Vehicle");
    default:
        return TEXT("None");
    }
}

static void SetTextIfChanged(UTextBlock* TextBlock, const FText& NewText)
{
    if (IsValid(TextBlock) && !TextBlock->GetText().EqualTo(NewText))
    {
        TextBlock->SetText(NewText);
    }
}

void UCreatorHUDWidget::SetToolbarGrid(UUniformGridPanel* InToolbarGrid)
{
    if (!AcceptWidgetAssignment(InToolbarGrid)) return;
    AssignedToolbarGrid = InToolbarGrid;
    RefreshToolbar();
}

void UCreatorHUDWidget::SetItemListScrollBox(UScrollBox* InItemListScrollBox)
{
    if (!AcceptWidgetAssignment(InItemListScrollBox)) return;
    AssignedItemListScrollBox = InItemListScrollBox;
    RefreshItemList();
}

void UCreatorHUDWidget::SetItemListPanel(UBorder* InItemListPanel)
{
    if (!AcceptWidgetAssignment(InItemListPanel)) return;
    AssignedItemListPanel = InItemListPanel;
    RefreshItemList();
}

void UCreatorHUDWidget::SetStatusText(UTextBlock* InStatusText)
{
    if (!AcceptWidgetAssignment(InStatusText)) return;
    AssignedStatusText = InStatusText;
    RefreshStatus();
}

void UCreatorHUDWidget::SetPlacementText(UTextBlock* InPlacementText)
{
    if (!AcceptWidgetAssignment(InPlacementText)) return;
    AssignedPlacementText = InPlacementText;
    RefreshStatus();
}

void UCreatorHUDWidget::SetMessageText(UTextBlock* InMessageText)
{
    if (!AcceptWidgetAssignment(InMessageText)) return;
    AssignedMessageText = InMessageText;
    RefreshStatus();
}

void UCreatorHUDWidget::NativeConstruct()
{
    Super::NativeConstruct();
    if (IsDesignTime()) return;
    bHUDConstructed = true;
    RefreshManagerReference();
    BindManagerEvents();
    UpdatePlacementRegistration();

    RefreshStatus();
    RefreshToolbar();
    RefreshItemList();
}

void UCreatorHUDWidget::UpdatePlacementRegistration()
{
    if (!bHUDConstructed || IsDesignTime()) return;
    UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this);
    if (!GameUpdate) return;
    if (!bShowStatusDetails || !AssignedPlacementText.IsValid())
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
        GameUpdateTickHandle = INDEX_NONE;
        PlacementRefreshElapsed = 0.f;
    }
    else if (GameUpdateTickHandle == INDEX_NONE)
    {
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [WeakThis = TWeakObjectPtr<UCreatorHUDWidget>(this)](const float DeltaSeconds)
            {
                if (UCreatorHUDWidget* StrongThis = WeakThis.Get())
                {
                    StrongThis->UpdateFromGameUpdate(DeltaSeconds);
                }
            },
            60);
    }
}

void UCreatorHUDWidget::NativeDestruct()
{
    bHUDConstructed = false;
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;

    UnbindManagerEvents();
    CachedGameManager = nullptr;
    PlacementRefreshElapsed = 0.f;
    ClearNativeInventory();
    VisibleInventorySignature.Reset();
    Super::NativeDestruct();
}

void UCreatorHUDWidget::UpdateFromGameUpdate(float DeltaSeconds)
{
    if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.f) return;
    if (!bShowStatusDetails || !IsVisible() || !IsValid(AssignedPlacementText.Get())
        || !AssignedPlacementText->IsVisible())
    {
        return;
    }

    PlacementRefreshElapsed += FMath::Max(DeltaSeconds, 0.f);
    const float Interval = FMath::Clamp(FMath::IsFinite(PlacementTextRefreshInterval) ? PlacementTextRefreshInterval : .1f, .05f, 1.f);
    if (PlacementRefreshElapsed < Interval) return;
    PlacementRefreshElapsed = FMath::Fmod(PlacementRefreshElapsed, Interval);
    // Coordinates update at 10 Hz by default; buttons/messages remain event driven.
    SetTextIfChanged(AssignedPlacementText.Get(), GetPlacementInfoText());
}

void UCreatorHUDWidget::RefreshManagerReference()
{
    if (IsDesignTime()) return;
    UnbindManagerEvents();
    CachedGameManager = UGameManagerSubSystem::FindGameManager(this);
    BindManagerEvents();
}

void UCreatorHUDWidget::RefreshToolbar()
{
    if (!IsUsingNativeLayout()) return;
    BuildToolbarButtons();
    const UGameManagerSubSystem* Manager = GetGameManager();
    for (int32 Index = 0; Index < NativeToolbarButtons.Num(); ++Index)
    {
        UMenuButtonWidget* Button = NativeToolbarButtons[Index];
        if (!Button) continue;
        const FToolbarItem Item = Manager ? Manager->GetToolbarItemAtSlot(Index) : FToolbarItem();
        const FString Label = Item.DisplayName.IsEmpty() ? FString::FromInt(Index + 1)
            : FString::Printf(TEXT("%d\n%s"), Index + 1, *Item.DisplayName.Left(3));
        if (Button->GetDisplayLabel() != Label) Button->ConfigureButton(FString::Printf(TEXT("Toolbar:%d"), Index), Label);
        Button->SetToolTipText(FText::FromString(Item.DisplayName.IsEmpty()
            ? FString::Printf(TEXT("%d: Empty (I: items)"), Index + 1)
            : FString::Printf(TEXT("%d: %s"), Index + 1, *Item.DisplayName)));
        const FLinearColor SlotColor = Manager && Manager->GetSelectedToolbarSlotIndex() == Index
            ? FLinearColor::LerpUsingHSV(ButtonColor, HoveredButtonTint, .55f) : ButtonColor;
        if (!Button->BackgroundColor.Equals(SlotColor))
            Button->ApplyButtonTheme(TextColor, SlotColor, HoveredButtonTint,
                FMath::Clamp(BodyFontSize, 10, 12), GetToolbarSlotExtent());
    }
}

void UCreatorHUDWidget::RefreshItemList()
{
    const UGameManagerSubSystem* Manager = GetGameManager();
    const bool bOpen = IsDesignTime() || (Manager && Manager->IsItemListWindowOpen());

    if (UWidget* const ItemListPanel = AssignedItemListPanel.Get(); IsValid(ItemListPanel))
    {
        const ESlateVisibility NewVisibility = bOpen ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
        // Resolve the weak widget once so every access in this game-thread call observes one object.
        if (ItemListPanel->GetVisibility() != NewVisibility)
        {
            ItemListPanel->SetVisibility(NewVisibility);
        }
    }
    if (!IsUsingNativeLayout() || !NativeItemGrid || !bOpen) return;
    const int32 Total = Manager ? Manager->GetAvailableItemCount() : (IsDesignTime() ? 12 : 0);
    const int32 PageSize = FMath::Clamp(ItemsPerPage, 8, 128);
    const int32 Columns = FMath::Clamp(ItemColumns, 1, 8);
    const int32 Pages = FMath::Max(1, FMath::DivideAndRoundUp(Total, PageSize));
    CurrentItemPage = FMath::Clamp(CurrentItemPage, 0, Pages - 1);
    NativePageText->SetText(FText::FromString(FString::Printf(TEXT("Page %d / %d   (%d items)"), CurrentItemPage + 1, Pages, Total)));
    NativePreviousPage->SetIsEnabled(CurrentItemPage > 0);
    NativeNextPage->SetIsEnabled(CurrentItemPage + 1 < Pages);

    TArray<FString> Signature;
    Signature.Add(FString::Printf(TEXT("%d:%d:%d:%d"), Total, CurrentItemPage, PageSize, Columns));
    const int32 Begin = CurrentItemPage * PageSize;
    const int32 End = FMath::Min(Begin + PageSize, Total);
    TArray<FString> Labels;
    Labels.Reserve(End - Begin);
    for (int32 Index = Begin; Index < End; ++Index)
    {
        const FToolbarItem Item = Manager ? Manager->GetAvailableItemAtIndex(Index) : FToolbarItem();
        const FString Label = Manager ? Item.DisplayName : FString::Printf(TEXT("Preview item %d"), Index + 1);
        Labels.Add(Label);
        Signature.Add(FString::Printf(TEXT("%d:%s:%s"), int32(Item.Kind), *Label, *Item.ModelReference));
    }
    if (Signature == VisibleInventorySignature) return;
    ClearNativeInventory();
    VisibleInventorySignature = MoveTemp(Signature);
    for (int32 LocalIndex = 0; LocalIndex < Labels.Num(); ++LocalIndex)
    {
        UMenuButtonWidget* Button = Cast<UMenuButtonWidget>(UUserWidget::CreateWidgetInstance(*this, UMenuButtonWidget::StaticClass(), NAME_None));
        if (!Button) continue;
        Button->ButtonMinSize.X = 120.f;
        Button->FontObject = FontObject;
        Button->FontOutlineSize = FontOutlineSize;
        Button->ApplyButtonTheme(TextColor, ButtonColor, HoveredButtonTint, BodyFontSize, ButtonHeight);
        Button->ConfigureButton(FString::Printf(TEXT("Item:%d"), Begin + LocalIndex), Labels[LocalIndex]);
        Button->OnButtonClicked.AddUniqueDynamic(this, &UCreatorHUDWidget::HandleNativeAction);
        NativeItemGrid->AddChildToUniformGrid(Button, LocalIndex / Columns, LocalIndex % Columns)->SetHorizontalAlignment(HAlign_Fill);
        NativeInventoryButtons.Add(Button);
    }
    if (NativeItemsScroll) NativeItemsScroll->ScrollToStart();
}

void UCreatorHUDWidget::RefreshStatus()
{
    ApplyStatusVisibility();
    UpdatePlacementRegistration();
    if (IsDesignTime()) return;
    if (bShowStatusDetails && IsValid(AssignedStatusText.Get()))
    {
        SetTextIfChanged(AssignedStatusText.Get(), GetStatusText());
    }
    if (bShowStatusDetails && IsValid(AssignedPlacementText.Get()))
    {
        SetTextIfChanged(AssignedPlacementText.Get(), GetPlacementInfoText());
    }
    if (bShowStatusDetails && IsValid(AssignedMessageText.Get()))
    {
        SetTextIfChanged(AssignedMessageText.Get(), GetMessageText());
    }
    if (UWidget* const ItemListPanel = AssignedItemListPanel.Get(); IsValid(ItemListPanel))
    {
        const UGameManagerSubSystem* Manager = GetGameManager();
        const ESlateVisibility NewVisibility =
            IsValid(Manager) && Manager->IsItemListWindowOpen()
                ? ESlateVisibility::Visible
                : ESlateVisibility::Collapsed;
        // Resolve the weak widget once so every access in this game-thread call observes one object.
        if (ItemListPanel->GetVisibility() != NewVisibility)
        {
            ItemListPanel->SetVisibility(NewVisibility);
        }
    }
}

void UCreatorHUDWidget::ApplyStatusVisibility()
{
    const ESlateVisibility TextVisibility = bShowStatusDetails
        ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
    if (NativeStatusPanel) NativeStatusPanel->SetVisibility(TextVisibility);
    if (UTextBlock* Text = AssignedStatusText.Get()) Text->SetVisibility(TextVisibility);
    if (UTextBlock* Text = AssignedPlacementText.Get()) Text->SetVisibility(TextVisibility);
    if (UTextBlock* Text = AssignedMessageText.Get()) Text->SetVisibility(TextVisibility);
}

void UCreatorHUDWidget::SelectToolbarSlotFromUI(int32 SlotIndex)
{
    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->SelectToolbarSlot(SlotIndex);
    }
}

void UCreatorHUDWidget::SelectAvailableItemFromUI(int32 AvailableItemIndex)
{
    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->SelectAvailableItemForCurrentToolbarSlot(AvailableItemIndex, true);
    }
}

void UCreatorHUDWidget::ToggleItemListFromUI()
{
    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->ToggleItemListWindow();
    }
}

void UCreatorHUDWidget::SaveSceneFromUI()
{
    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->SaveScene();
    }
}

void UCreatorHUDWidget::BuildActiveProjectArchiveFromUI()
{
    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->BuildActiveProjectArchive();
    }
}

void UCreatorHUDWidget::ToggleSnapFromUI()
{
    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->ToggleSnap();
    }
}

void UCreatorHUDWidget::HandleStateChanged()
{
    RefreshStatus();
}

void UCreatorHUDWidget::HandleMessageChanged(const FString& Message)
{
    if (bShowStatusDetails) SetTextIfChanged(AssignedMessageText.Get(), FText::FromString(Message));
}

void UCreatorHUDWidget::HandleToolbarChanged()
{
    RefreshToolbar();
    RefreshItemList();
    RefreshStatus();
}

void UCreatorHUDWidget::HandleItemListWindowChanged(bool bOpen)
{
    RefreshItemList();
    RefreshStatus();
}

FText UCreatorHUDWidget::GetStatusText() const
{
    const UGameManagerSubSystem* Manager = GetGameManager();
    if (!IsValid(Manager))
    {
        return FText::FromString(TEXT("GameManager unavailable"));
    }

    const FToolbarItem SelectedItem = Manager->GetSelectedToolbarItem();
    const FString ModeText = Manager->GetPlayMode() == EPlayMode::Creator
        ? TEXT("Creator")
        : TEXT("RealLife");

    return FText::FromString(FString::Printf(
        TEXT("%s MODE\nSlot %d / %d\nItem: %s\nKind: %s\nTool: %d"),
        *ModeText,
        Manager->GetSelectedToolbarSlotIndex() + 1,
        Manager->GetToolbarSlotCount(),
        *SelectedItem.DisplayName,
        *MakeKindLabel(SelectedItem.Kind),
        static_cast<int32>(Manager->GetCurrentToolMode())));
}

FText UCreatorHUDWidget::GetPlacementInfoText() const
{
    const UGameManagerSubSystem* Manager = GetGameManager();
    if (!IsValid(Manager))
    {
        return FText::FromString(TEXT("Placement: NONE"));
    }

    const FVector Location = Manager->GetCurrentCrosshairWorldLocation();
    const FString PlacementMode = Manager->HasCrosshairPlacementLocation()
        ? (Manager->IsCrosshairFreeSpacePlacement() ? TEXT("AIR") : TEXT("SURFACE"))
        : TEXT("NONE");
    const FString SnapText = Manager->IsSnapEnabled() ? TEXT("ON") : TEXT("OFF");

    return FText::FromString(FString::Printf(
        TEXT("X %.0f  Y %.0f  Z %.0f\nPlacement: %s\nSnap: %s (%.0f cm)"),
        Location.X,
        Location.Y,
        Location.Z,
        *PlacementMode,
        *SnapText,
        Manager->GetGridSize()));
}

FText UCreatorHUDWidget::GetMessageText() const
{
    const UGameManagerSubSystem* Manager = GetGameManager();
    if (!IsValid(Manager))
    {
        return FText::GetEmpty();
    }

    return FText::FromString(Manager->GetLastMessage());
}

UGameManagerSubSystem* UCreatorHUDWidget::GetGameManager() const
{
    if (IsDesignTime() || IsTemplate() || !GetWorld()) return nullptr;
    if (IsValid(CachedGameManager))
    {
        return CachedGameManager.Get();
    }

    CachedGameManager = UGameManagerSubSystem::FindGameManager(this);
    return CachedGameManager.Get();
}

void UCreatorHUDWidget::BindManagerEvents()
{
    if (bGameplayEventsBound)
    {
        return;
    }

    UGameManagerSubSystem* Manager = GetGameManager();
    if (!IsValid(Manager))
    {
        return;
    }

    Manager->OnStateChanged.AddDynamic(this, &UCreatorHUDWidget::HandleStateChanged);
    Manager->OnMessageChanged.AddDynamic(this, &UCreatorHUDWidget::HandleMessageChanged);
    Manager->OnToolbarChanged.AddDynamic(this, &UCreatorHUDWidget::HandleToolbarChanged);
    Manager->OnItemListWindowChanged.AddDynamic(this, &UCreatorHUDWidget::HandleItemListWindowChanged);
    bGameplayEventsBound = true;
}

void UCreatorHUDWidget::UnbindManagerEvents()
{
    if (!bGameplayEventsBound)
    {
        return;
    }

    if (IsValid(CachedGameManager))
    {
        CachedGameManager->OnStateChanged.RemoveDynamic(this, &UCreatorHUDWidget::HandleStateChanged);
        CachedGameManager->OnMessageChanged.RemoveDynamic(this, &UCreatorHUDWidget::HandleMessageChanged);
        CachedGameManager->OnToolbarChanged.RemoveDynamic(this, &UCreatorHUDWidget::HandleToolbarChanged);
        CachedGameManager->OnItemListWindowChanged.RemoveDynamic(this, &UCreatorHUDWidget::HandleItemListWindowChanged);
    }

    bGameplayEventsBound = false;
}
