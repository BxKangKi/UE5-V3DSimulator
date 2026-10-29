// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/SelectionWidgetBase.h"

#include "Components/PanelWidget.h"
#include "UI/MenuButtonWidget.h"

void USelectionWidgetBase::NativeDestruct()
{
    ClearGeneratedSelectionEntries();
    SelectionListPanel.Reset();
    Super::NativeDestruct();
}

void USelectionWidgetBase::HandleSelectionButtonClicked(const FString& SelectionKey)
{
    const FString NormalizedKey = SelectionKey.TrimStartAndEnd();
    if (!NormalizedKey.IsEmpty())
    {
        OnSelectionEntryActivated(NormalizedKey);
    }
}

void USelectionWidgetBase::SetSelectionListPanel(UPanelWidget* InPanel)
{
    if (!AcceptWidgetAssignment(InPanel)) return;
    if (SelectionListPanel.Get() == InPanel)
    {
        return;
    }

    ClearGeneratedSelectionEntries();
    SelectionListPanel = InPanel;
}

UWidget* USelectionWidgetBase::AddGeneratedSelectionEntry(
    const FString& SelectionKey,
    const FString& DisplayName)
{
    UPanelWidget* const Panel = SelectionListPanel.Get();
    const FString NormalizedKey = SelectionKey.TrimStartAndEnd();
    if (!IsValid(Panel) || NormalizedKey.IsEmpty())
    {
        return nullptr;
    }

    // Selection rows are code-only components; the owning screen supplies their appearance.
    UMenuButtonWidget* const EntryWidget = Cast<UMenuButtonWidget>(
        UUserWidget::CreateWidgetInstance(*this, UMenuButtonWidget::StaticClass(), NAME_None));

    if (!IsValid(EntryWidget))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("SelectionWidgetBase failed to create a native selection entry for '%s'."),
            *NormalizedKey);
        return nullptr;
    }

    FString NormalizedDisplayName = DisplayName.TrimStartAndEnd();
    if (NormalizedDisplayName.IsEmpty())
    {
        NormalizedDisplayName = NormalizedKey;
    }

    EntryWidget->ButtonMinSize.X = 0.0;
    EntryWidget->FontObject = FontObject;
    EntryWidget->FontOutlineSize = FontOutlineSize;
    EntryWidget->ApplyButtonTheme(TextColor, ButtonColor, HoveredButtonTint, BodyFontSize, ButtonHeight);
    EntryWidget->ConfigureButton(NormalizedKey, NormalizedDisplayName);
    EntryWidget->OnButtonClicked.RemoveDynamic(this, &USelectionWidgetBase::HandleSelectionButtonClicked);
    EntryWidget->OnButtonClicked.AddDynamic(this, &USelectionWidgetBase::HandleSelectionButtonClicked);
    Panel->AddChild(EntryWidget);

    GeneratedSelectionEntries.Add(EntryWidget);
    return EntryWidget;
}

void USelectionWidgetBase::ClearGeneratedSelectionEntries()
{
    for (UMenuButtonWidget* EntryWidget : GeneratedSelectionEntries)
    {
        if (!IsValid(EntryWidget))
        {
            continue;
        }

        EntryWidget->OnButtonClicked.RemoveDynamic(this, &USelectionWidgetBase::HandleSelectionButtonClicked);
        EntryWidget->RemoveFromParent();
    }

    GeneratedSelectionEntries.Reset();
}

void USelectionWidgetBase::OnSelectionEntryActivated(const FString& SelectionKey)
{
    UE_LOG(LogTemp, Warning,
        TEXT("SelectionWidgetBase received an unhandled selection: %s"), *SelectionKey);
}


void USelectionWidgetBase::ApplyNativeStyle()
{
    Super::ApplyNativeStyle();
    for (UMenuButtonWidget* Entry : GeneratedSelectionEntries)
    {
        if (IsValid(Entry))
        {
            Entry->FontObject = FontObject;
            Entry->FontOutlineSize = FontOutlineSize;
            Entry->ApplyButtonTheme(TextColor, ButtonColor, HoveredButtonTint, BodyFontSize, ButtonHeight);
        }
    }
}
