// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "UI/ProjectEditorWidget.h"
#include "Widgets/SOverlay.h"
#include "InputCoreTypes.h"
#include "UI/BuildStatusWidget.h"
#include "UI/HUDStyle.h"
#include "Setting/GameSettings.h"
#include "Styling/CoreStyle.h"
#include "Brushes/SlateColorBrush.h"
#include "Widgets/Layout/SWrapBox.h"
#include "SV3DModelPreview.h"
#include "Widgets/Layout/SBox.h"
#include "System/ProjectSourceModelBuilder.h"
#include "Simulator/ModelDefinitionJson.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Animation/Skeleton.h"
#include "Materials/MaterialInterface.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/V3DSimulatorGameInstance.h"
#include "System/GameManagerSubSystem.h"
#include "System/SafeFileIO.h"
#include "System/ProjectWorkspace.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Text/STextBlock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace V3DEditorUI
{
    FText Text(const FString& S) { return FText::FromString(S); }
    FString ValueText(const TSharedPtr<FJsonValue>& V)
    {
        if (!V.IsValid()) return FString();
        if (V->Type == EJson::String) return V->AsString();
        auto Wrapper = MakeShared<FJsonObject>(); Wrapper->SetField(TEXT("v"), V);
        FString S; FJsonSerializer::Serialize(Wrapper, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&S));
        const int32 Colon = S.Find(TEXT(":"));
        return Colon != INDEX_NONE && S.Len() > Colon + 2 ? S.Mid(Colon + 1, S.Len() - Colon - 2) : FString();
    }

    FSlateBrush MakeOutlinedBrush(const FLinearColor& FillColor, const FLinearColor& OutlineColor,
        const float CornerRadius = 5.f, const float OutlineWidth = 2.f)
    {
        FSlateBrush Brush = FSlateColorBrush(FillColor);
        Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
        Brush.OutlineSettings.CornerRadii = FVector4(CornerRadius, CornerRadius, CornerRadius, CornerRadius);
        Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
        Brush.OutlineSettings.Color = FSlateColor(OutlineColor);
        Brush.OutlineSettings.Width = OutlineWidth;
        return Brush;
    }

    FLinearColor CardBorderColor(const bool bDarkMode, const bool bOptional = false)
    {
        if (bDarkMode)
        {
            return bOptional
                ? FLinearColor(0.20f, 0.24f, 0.32f, 1.f)
                : FLinearColor(0.28f, 0.35f, 0.46f, 1.f);
        }
        return bOptional
            ? FLinearColor(0.76f, 0.81f, 0.88f, 1.f)
            : FLinearColor(0.64f, 0.72f, 0.83f, 1.f);
    }

    FLinearColor CardFillColor(const V3DHUDStyle::FAuthoringPalette& Palette, const bool bDarkMode, const int32 RowIndex, const bool bOptional = false)
    {
        if (bOptional)
        {
            return Palette.Background;
        }
        if (bDarkMode)
        {
            return (RowIndex % 2 == 0)
                ? FLinearColor(0.082f, 0.098f, 0.130f, 1.f)
                : FLinearColor(0.112f, 0.132f, 0.172f, 1.f);
        }
        return (RowIndex % 2 == 0)
            ? FLinearColor(0.865f, 0.900f, 0.945f, 1.f)
            : FLinearColor(0.935f, 0.955f, 0.980f, 1.f);
    }
}

UProjectEditorWidget::UProjectEditorWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    FontObject = V3DHUDStyle::FindFontForConstructor();
    ApplyEditorTheme();
}

V3DHUDStyle::FAuthoringPalette UProjectEditorWidget::GetThemePalette() const
{
    return V3DHUDStyle::GetAuthoringPalette(bDarkMode);
}

void UProjectEditorWidget::ApplyEditorTheme()
{
    if (!IsValid(FontObject))
    {
        FontObject = V3DHUDStyle::FindFontRuntime();
    }
    const V3DHUDStyle::FAuthoringPalette Palette = GetThemePalette();

    EditorTextStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
    V3DHUDStyle::ApplyFont(EditorTextStyle.Font, FontObject, 16, 0);
    EditorTextStyle.ColorAndOpacity = FSlateColor(Palette.Text);

    TitleFont = EditorTextStyle.Font;
    V3DHUDStyle::ApplyFont(TitleFont, FontObject, 36, 0);

    EditorButtonStyle = FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button");
    EditorButtonStyle.Normal = FSlateColorBrush(Palette.Button);
    EditorButtonStyle.Hovered = FSlateColorBrush(Palette.Hover);
    EditorButtonStyle.Pressed = FSlateColorBrush(Palette.Pressed);
    EditorButtonStyle.Disabled = FSlateColorBrush(Palette.Panel);
    EditorButtonStyle.NormalPadding = EditorButtonStyle.PressedPadding = FMargin(0);
    EditorButtonStyle.NormalForeground = EditorButtonStyle.HoveredForeground =
        EditorButtonStyle.PressedForeground = FSlateColor(Palette.Text);

    EditorInputStyle = FCoreStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");
    EditorInputStyle.TextStyle = EditorTextStyle;
    EditorInputStyle.BackgroundImageNormal = FSlateColorBrush(Palette.Input);
    EditorInputStyle.BackgroundImageHovered = FSlateColorBrush(Palette.Input);
    EditorInputStyle.BackgroundImageFocused = FSlateColorBrush(Palette.Hover);
    EditorInputStyle.BackgroundImageReadOnly = FSlateColorBrush(Palette.Background);
    EditorInputStyle.ForegroundColor = EditorInputStyle.FocusedForegroundColor = FSlateColor(Palette.Text);
    EditorInputStyle.Padding = FMargin(10, 7);

    EditorComboStyle = FCoreStyle::Get().GetWidgetStyle<FComboBoxStyle>("ComboBox");
    EditorComboStyle.ComboButtonStyle.ButtonStyle = EditorButtonStyle;
    EditorComboStyle.ComboButtonStyle.DownArrowImage.TintColor = FSlateColor(Palette.Text);
    EditorComboStyle.ComboButtonStyle.MenuBorderBrush = FSlateColorBrush(Palette.Panel);

    EditorRowStyle = FCoreStyle::Get().GetWidgetStyle<FTableRowStyle>("TableView.Row");
    V3DHUDStyle::ApplyAuthoringRows(EditorRowStyle, bDarkMode);
    EditorListStyle.BackgroundBrush = FSlateColorBrush(Palette.Input);

    const FLinearColor UncheckedBorder = bDarkMode
        ? FLinearColor(0.42f, 0.48f, 0.60f, 1.f)
        : FLinearColor(0.45f, 0.54f, 0.66f, 1.f);
    const FLinearColor CheckedFill = bDarkMode
        ? FLinearColor(0.16f, 0.46f, 0.84f, 1.f)
        : FLinearColor(0.14f, 0.44f, 0.82f, 1.f);
    const FLinearColor CheckedHover = bDarkMode
        ? FLinearColor(0.24f, 0.56f, 0.94f, 1.f)
        : FLinearColor(0.22f, 0.53f, 0.92f, 1.f);
    const FLinearColor CheckedPressed = bDarkMode
        ? FLinearColor(0.11f, 0.35f, 0.68f, 1.f)
        : FLinearColor(0.10f, 0.34f, 0.66f, 1.f);
    const FLinearColor CheckedBorder = bDarkMode
        ? FLinearColor(0.55f, 0.80f, 1.0f, 1.f)
        : FLinearColor(0.08f, 0.28f, 0.58f, 1.f);
    const FLinearColor CheckedText(0.98f, 0.99f, 1.0f, 1.f);

    EditorCheckStyle = FCoreStyle::Get().GetWidgetStyle<FCheckBoxStyle>("Checkbox");
    EditorCheckStyle.CheckBoxType = ESlateCheckBoxType::ToggleButton;
    EditorCheckStyle.UncheckedImage = V3DEditorUI::MakeOutlinedBrush(Palette.Input, UncheckedBorder, 6.f, 2.f);
    EditorCheckStyle.UncheckedHoveredImage = V3DEditorUI::MakeOutlinedBrush(Palette.Hover, CheckedBorder, 6.f, 2.f);
    EditorCheckStyle.UncheckedPressedImage = V3DEditorUI::MakeOutlinedBrush(Palette.Pressed, CheckedBorder, 6.f, 2.f);
    EditorCheckStyle.CheckedImage = V3DEditorUI::MakeOutlinedBrush(CheckedFill, CheckedBorder, 6.f, 2.f);
    EditorCheckStyle.CheckedHoveredImage = V3DEditorUI::MakeOutlinedBrush(CheckedHover, CheckedBorder, 6.f, 2.f);
    EditorCheckStyle.CheckedPressedImage = V3DEditorUI::MakeOutlinedBrush(CheckedPressed, CheckedBorder, 6.f, 2.f);
    EditorCheckStyle.UndeterminedImage = V3DEditorUI::MakeOutlinedBrush(Palette.Panel, UncheckedBorder, 6.f, 2.f);
    EditorCheckStyle.UndeterminedHoveredImage = V3DEditorUI::MakeOutlinedBrush(Palette.Hover, CheckedBorder, 6.f, 2.f);
    EditorCheckStyle.UndeterminedPressedImage = V3DEditorUI::MakeOutlinedBrush(Palette.Pressed, CheckedBorder, 6.f, 2.f);
    EditorCheckStyle.Padding = FMargin(10.f, 6.f);
    EditorCheckStyle.ForegroundColor = FSlateColor(Palette.Muted);
    EditorCheckStyle.HoveredForeground = FSlateColor(Palette.Text);
    EditorCheckStyle.PressedForeground = FSlateColor(Palette.Text);
    EditorCheckStyle.CheckedForeground = EditorCheckStyle.CheckedHoveredForeground =
        EditorCheckStyle.CheckedPressedForeground = FSlateColor(CheckedText);

    if (Raw.IsValid())
    {
        Raw->SetTextStyle(&EditorTextStyle);
    }
    if (List.IsValid())
    {
        List->RequestListRefresh();
    }
}

TSharedRef<SWidget> UProjectEditorWidget::MakeEditorButton(const TCHAR* Label, TFunction<void()> Action)
{
    return SNew(SButton).ButtonStyle(&EditorButtonStyle).ContentPadding(FMargin(14, 9))
        .OnClicked_Lambda([Action = MoveTemp(Action)]() { Action(); return FReply::Handled(); })
        [SNew(STextBlock).TextStyle(&EditorTextStyle).Text(FText::FromString(Label))];
}

TSharedRef<SWidget> UProjectEditorWidget::RebuildWidget()
{
    using namespace V3DEditorUI;
    SetIsFocusable(true);
    if (const UGameManagerSubSystem* Manager = UGameManagerSubSystem::FindGameManager(this))
    {
        if (const UGameSettings* Settings = Manager->GetGameSettings())
        {
            bDarkMode = Settings->bEditorDarkMode;
        }
    }
    ApplyEditorTheme();

    auto Toolbar = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(8, 8));
    Toolbar->AddSlot()[MakeEditorButton(TEXT("Save"), [this] { SaveAll(false); })];
    Toolbar->AddSlot()[MakeEditorButton(TEXT("Save + Build"), [this] { SaveAll(true); })];
    Toolbar->AddSlot()[MakeEditorButton(TEXT("Undo"), [this]
    {
        if (!ApplyRaw() || !Current || Current->Undo.IsEmpty()) return;
        const auto Parsed = FSafeFileIO::ParseJsonText(Current->Undo.Pop(), TEXT("Undo"));
        if (Parsed.IsSuccess()) { Current->Json = Parsed.JsonObject; Changed(); }
    })];
    Toolbar->AddSlot()[MakeEditorButton(TEXT("Apply JSON"), [this] { ApplyRaw(); })];
    Toolbar->AddSlot()[MakeEditorButton(TEXT("Reload file (discard)"), [this]
    {
        if (bBusy || !Current) return;
        Remember(); bBusy = true; const auto Doc = Current;
        const uint64 Token = RequestGeneration; const FString Path = Doc->Path;
        TWeakObjectPtr<UProjectEditorWidget> Weak(this);
        const bool Queued = FSafeFileIO::RunTrackedWorker([Weak, Token, Doc, Path]
        {
            FSafeJsonLimits Limits; Limits.MaxFileBytes = 16ll * 1024 * 1024;
            Limits.MaxDepth = 32;
            const auto Bytes = FSafeFileIO::LoadBinaryBlocking(Path, Limits.MaxFileBytes);
            const bool Read = Bytes.IsSuccess();
            FString TextOnDisk;
            if (Read && !Bytes.Data.IsEmpty()) FFileHelper::BufferToString(TextOnDisk, Bytes.Data.GetData(), Bytes.Data.Num());
            const auto Loaded = FSafeFileIO::ParseJsonText(TextOnDisk, Path, Limits);
            FSafeFileIO::DispatchTrackedGameThread([Weak, Token, Doc, Loaded, Read, TextOnDisk]
            {
                auto* Self = Weak.Get(); if (!Self || Self->RequestGeneration != Token) return;
                Self->bBusy = false;
                if (Read)
                {
                    Doc->DiskText = TextOnDisk; Doc->Json = Loaded.JsonObject; Doc->bDirty = Doc->bNewFile = false;
                    Self->InvalidatePreview(); Self->bRawDirty = false; Self->RefreshForm();
                    Self->Message = Loaded.IsSuccess()
                        ? TEXT("Reloaded from disk. Undo can restore the previous in-memory version.") : Loaded.Error;
                }
                else Self->Message = TEXT("Reload failed. Unsaved changes were retained.");
            });
        });
        if (!Queued) { bBusy = false; Message = TEXT("Reload queue unavailable."); }
    })];
    Toolbar->AddSlot()[MakeEditorButton(TEXT("Back"), [this] { RequestBack(); })];

    auto Left = SNew(SVerticalBox);
    Left->AddSlot().AutoHeight().Padding(0,0,0,12)[SNew(STextBlock).TextStyle(&EditorTextStyle).Text(Text(TEXT("Resources / Scripts")))];
    Left->AddSlot().AutoHeight().Padding(0,0,0,12)[SNew(SEditableTextBox).Style(&EditorInputStyle).HintText(Text(TEXT("Filter resources / scripts")))
        .OnTextChanged_Lambda([this](const FText& T) { Filter = T.ToString(); RefreshFiles(); })];
    Left->AddSlot().FillHeight(1)[SAssignNew(List, SListView<TSharedPtr<FV3DEditorDocument>>)
        .ListViewStyle(&EditorListStyle).ListItemsSource(&VisibleDocuments).SelectionMode(ESelectionMode::Single)
        .OnGenerateRow_Lambda([this](TSharedPtr<FV3DEditorDocument> Doc, const TSharedRef<STableViewBase>& Owner)
        {
            return SNew(STableRow<TSharedPtr<FV3DEditorDocument>>, Owner).Style(&EditorRowStyle)
                [SNew(STextBlock).TextStyle(&EditorTextStyle).Text_Lambda([Doc] { return Doc.IsValid() ? Text((Doc->bDirty ? TEXT("* ") : TEXT("")) + Doc->Label) : FText::GetEmpty(); }).Margin(FMargin(4,6))];
        })
        .OnSelectionChanged_Lambda([this](TSharedPtr<FV3DEditorDocument> Doc, ESelectInfo::Type Info)
        {
            if (bBusy || !Doc || Doc == Current || Info == ESelectInfo::Direct) return;
            if (!ApplyRaw()) { if (List) List->SetSelection(Current, ESelectInfo::Direct); return; }
            InvalidatePreview(); Current = Doc; bRawDirty = false; RefreshForm();
        })];
    auto Right = SNew(SVerticalBox);
    Right->AddSlot().AutoHeight().Padding(0,0,0,8)[MakeEditorButton(TEXT("Preview model (read only)"), [this] { StartPreview(); })];
    Right->AddSlot().AutoHeight()[SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
        .BorderBackgroundColor_Lambda([this] { return FSlateColor(GetThemePalette().Panel); }).Padding(8)
        [SNew(SBox).HeightOverride(240)[SAssignNew(Preview, SV3DModelPreview)]]];
    Right->AddSlot().AutoHeight().Padding(0,8)[SNew(STextBlock).TextStyle(&EditorTextStyle).AutoWrapText(true).Text(Text(TEXT("JSON / advanced edit · Apply JSON to update fields")))];
    Right->AddSlot().FillHeight(1)[SAssignNew(Raw, SMultiLineEditableTextBox).Style(&EditorInputStyle).Font(EditorTextStyle.Font).AutoWrapText(false)
        .OnTextChanged_Lambda([this](const FText& Value)
        { if (!bSettingRaw) { bRawDirty = true; RawDraft = Value.ToString(); } })];
    auto Body = SNew(SSplitter)
        + SSplitter::Slot().Value(.2f)[Left]
        + SSplitter::Slot().Value(.45f)[SNew(SScrollBox) + SScrollBox::Slot().Padding(8, 0)[SAssignNew(Form, SVerticalBox)]]
        + SSplitter::Slot().Value(.35f)[Right];
    auto Root = SNew(SBorder).Padding(32).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
        .BorderBackgroundColor_Lambda([this] { return FSlateColor(GetThemePalette().Background); })
        [SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0,0,0,20)
                [SNew(STextBlock).TextStyle(&EditorTextStyle).Font(TitleFont).AutoWrapText(true)
                    .Text_Lambda([this] { return Text(TEXT("Editor / ") + Project); })]
            + SVerticalBox::Slot().AutoHeight()[SNew(SBox).IsEnabled_Lambda([this] { return !bBusy && !bFormRefreshPending; })[Toolbar]]
            + SVerticalBox::Slot().AutoHeight().Padding(0,12)
                [SNew(SBorder).Padding(12).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                    .BorderBackgroundColor_Lambda([this] { return FSlateColor(GetThemePalette().Panel); })
                    [SNew(STextBlock).TextStyle(&EditorTextStyle).AutoWrapText(true).Text_Lambda([this] { return Text(Message); })]]
            + SVerticalBox::Slot().FillHeight(1)[SNew(SBox).IsEnabled_Lambda([this] { return !bBusy && !bFormRefreshPending; })[Body]]];
    RefreshFiles();
    RefreshForm();
    if (List && Current) List->SetSelection(Current, ESelectInfo::Direct);
    auto Choices = SNew(SHorizontalBox);
    Choices->AddSlot().AutoWidth().Padding(6)[MakeEditorButton(TEXT("Save"), [this] { bConfirmBack = false; SaveAll(false, true); })];
    Choices->AddSlot().AutoWidth().Padding(6)[MakeEditorButton(TEXT("Discard"), [this] { bConfirmBack = false; Close(); })];
    Choices->AddSlot().AutoWidth().Padding(6)[MakeEditorButton(TEXT("Cancel"), [this] { bConfirmBack = false; })];
    return SNew(SOverlay)
        + SOverlay::Slot()[SNew(SBox).IsEnabled_Lambda([this] { return !bConfirmBack; })[Root]]
        + SOverlay::Slot()[SNew(SBorder)
            .Visibility_Lambda([this] { return bConfirmBack ? EVisibility::Visible : EVisibility::Collapsed; })
            .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
            .BorderBackgroundColor_Lambda([this] { return FSlateColor(GetThemePalette().ModalScrim); })
            .HAlign(HAlign_Center).VAlign(VAlign_Center)
            [SNew(SBorder).Padding(28).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                .BorderBackgroundColor_Lambda([this] { return FSlateColor(GetThemePalette().Background); })
                [SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().Padding(0,0,0,16)
                        [SNew(STextBlock).TextStyle(&EditorTextStyle).Text(Text(TEXT("Unsaved changes. Save before leaving?")))]
                    + SVerticalBox::Slot().AutoHeight()[Choices]]]];
}

void UProjectEditorWidget::OpenProject(const FString& ProjectName)
{
    if (bBusy || BuildStatus) { Message = TEXT("Wait for the current operation before opening another project."); return; }
    InvalidatePreview();
    Current.Reset(); Documents.Reset(); VisibleDocuments.Reset(); bRawDirty = false; RawDraft.Reset();
    if (Form) Form->ClearChildren();
    if (Raw) { TGuardValue<bool> Guard(bSettingRaw, true); Raw->SetText(FText::GetEmpty()); }
    RefreshFiles();
    Project = ProjectName; bBusy = true; Message = TEXT("Reading resources and generating missing JSON definitions...");
    const uint64 Token = ++RequestGeneration;
    const TWeakObjectPtr<UProjectEditorWidget> Weak(this);
    const bool Queued = FSafeFileIO::RunTrackedWorker([Weak, Token, ProjectName]
    {
        TArray<TSharedPtr<FV3DEditorDocument>> Found; FString Error;
        const bool Success = V3DProjectEditor::Discover(ProjectName, Found, Error);
        FSafeFileIO::DispatchTrackedGameThread([Weak, Token, Found = MoveTemp(Found), Error, Success]() mutable
        {
            auto* Self = Weak.Get(); if (!Self || Self->RequestGeneration != Token) return;
            Self->bBusy = false; Self->Documents = MoveTemp(Found);
            Self->Message = Success ? TEXT("Choose a file. Optional keys have Add buttons. Changes are saved only with Save. Build is explicit.") : Error;
            Self->RefreshFiles();
            if (!Self->Documents.IsEmpty())
            {
                Self->Current = Self->Documents[0]; Self->RefreshForm();
                if (Self->List) Self->List->SetSelection(Self->Current, ESelectInfo::Direct);
            }
        });
    });
    if (!Queued) { bBusy = false; Message = TEXT("Editor file queue is unavailable."); }
}

void UProjectEditorWidget::RefreshFiles()
{
    VisibleDocuments.Reset();
    for (const auto& Doc : Documents) if (Doc.IsValid() && (Filter.IsEmpty() || Doc->Label.Contains(Filter))) VisibleDocuments.Add(Doc);
    if (List) List->RequestListRefresh();
}
void UProjectEditorWidget::Remember()
{
    if (!Current || !Current->Json) return;
    FString Snapshot = V3DProjectEditor::Serialize(Current->Json);
    if (Snapshot.Len() * sizeof(TCHAR) > 4 * 1024 * 1024) return;
    Current->Undo.Add(MoveTemp(Snapshot));
    int64 Bytes = 0; int32 Count = 0;
    for (const auto& Doc : Documents) if (Doc.IsValid()) for (const auto& Entry : Doc->Undo) { Bytes += Entry.Len() * sizeof(TCHAR); ++Count; }
    for (const auto& Doc : Documents)
        if (Doc.IsValid())
            while (!Doc->Undo.IsEmpty() && (Bytes > 4 * 1024 * 1024 || Count > 32))
            { Bytes -= Doc->Undo[0].Len() * sizeof(TCHAR); --Count; Doc->Undo.RemoveAt(0); }

}
void UProjectEditorWidget::Changed()
{
    if (Current) Current->bDirty = true;
    bRawDirty = false; InvalidatePreview(); QueueFormRefresh();
}

void UProjectEditorWidget::QueueFormRefresh()
{
    if (bFormRefreshPending || !Form) return;
    bFormRefreshPending = true;
    Form->RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateWeakLambda(this,
        [this](double, float)
        {
            if (bFormRefreshPending) RefreshForm();
            return EActiveTimerReturnType::Stop;
        }));
}

void UProjectEditorWidget::InvalidatePreview()
{
    PreviewDocument.Reset();
    if (PreviewBuilder)
    {
        PreviewBuilder->OnFinished.RemoveAll(this);
        PreviewBuilder->Cancel();
        PreviewBuilder = nullptr;
    }
    if (Preview) Preview->ClearModel();
}

bool UProjectEditorWidget::ApplyRaw()
{
    if (!Current || !Raw || !bRawDirty) return true;
    FSafeJsonLimits Limits; Limits.MaxFileBytes = 16ll * 1024 * 1024; Limits.MaxDepth = 32;
    const auto Parsed = FSafeFileIO::ParseJsonText(RawDraft, Current->Path, Limits);
    if (!Parsed.IsSuccess()) { Message = Parsed.Error; return false; }
    Remember(); Current->Json = Parsed.JsonObject; Changed(); return true;
}

void UProjectEditorWidget::RefreshForm()
{
    using namespace V3DEditorUI;
    bFormRefreshPending = false;
    if (!Form || !Raw || !Current) return;
    TGuardValue<bool> RebuildGuard(bRebuildingForm, true);
    Form->ClearChildren(); FormRowsRemaining = 1500;
    bSettingRaw = true;
    if (!bRawDirty) RawDraft = Current->Json ? V3DProjectEditor::Serialize(Current->Json) : Current->DiskText;
    Raw->SetText(Text(RawDraft));
    bSettingRaw = false;
    Form->AddSlot().AutoHeight().Padding(4, 4, 4, 10)[SNew(STextBlock).TextStyle(&EditorTextStyle).AutoWrapText(true).Text(Text(Current->Label +
        (Current->SourcePath.IsEmpty() ? TEXT("") : TEXT("\nSource: ") + FPaths::GetCleanFilename(Current->SourcePath))))];
    if (!Current->Json) { Message = TEXT("Invalid source JSON: repair it in the advanced editor, then Apply JSON."); return; }
    // A new asset can choose AssetType even if its metadata was incomplete.
    if (!Current->SourcePath.IsEmpty() && !Current->Json->HasField(TEXT("AssetType")))
        Form->AddSlot().AutoHeight().Padding(0, 0, 0, 8)[MakeEditorButton(TEXT("Add AssetType"), [this]
        { Remember(); Current->Json->SetStringField(TEXT("AssetType"), FPaths::GetExtension(Current->SourcePath).Equals(TEXT("wav"), ESearchCase::IgnoreCase) ? TEXT("Sound") : TEXT("Model")); Changed(); })];
    AddObject(Current->Json, V3DProjectEditor::Suggestions(*Current), Form.ToSharedRef());
}

void UProjectEditorWidget::AddObject(const TSharedPtr<FJsonObject>& Object, const TSharedPtr<FJsonObject>& Suggested,
    const TSharedRef<SVerticalBox>& Box, int32 Depth, const FString& Context)
{
    using namespace V3DEditorUI;
    if (!Object || Depth > 16) return;
    const V3DHUDStyle::FAuthoringPalette Palette = GetThemePalette();
    const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush("WhiteBrush");
    TArray<FString> Keys;
    for (const auto& Pair : Object->Values) Keys.Add(FString(Pair.Key));
    if (Suggested) for (const auto& Pair : Suggested->Values) Keys.AddUnique(FString(Pair.Key));
    Keys.Sort();

    int32 RowIndex = 0;
    for (const auto& Key : Keys)
    {
        if (--FormRowsRemaining < 0)
        {
            Box->AddSlot().AutoHeight()[SNew(STextBlock).TextStyle(&EditorTextStyle).Text(Text(TEXT("Form limit reached; remaining keys are available in JSON.")))];
            break;
        }
        const auto* Existing = Object->Values.Find(*Key);
        const auto* Hint = Suggested ? Suggested->Values.Find(*Key) : nullptr;
        if (!Existing)
        {
            if (!Hint || !(*Hint).IsValid()) continue;
            const auto DefaultValue = *Hint;
            Box->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
            [
                SNew(SBorder)
                .BorderImage(WhiteBrush)
                .BorderBackgroundColor(CardBorderColor(bDarkMode, true))
                .Padding(1.f)
                [
                    SNew(SBorder)
                    .BorderImage(WhiteBrush)
                    .BorderBackgroundColor(CardFillColor(Palette, bDarkMode, RowIndex, true))
                    .Padding(FMargin(10.f, 7.f))
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                        [
                            SNew(STextBlock)
                            .TextStyle(&EditorTextStyle)
                            .ColorAndOpacity(FSlateColor(Palette.Muted))
                            .Text(Text(Key + TEXT(" (optional)")))
                        ]
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                        [
                            MakeEditorButton(TEXT("Add"), [this, Object, Key, DefaultValue]
                            { if (bRawDirty) { Message = TEXT("Apply JSON first."); return; } Remember(); Object->SetField(Key, FJsonValue::Duplicate(DefaultValue)); Changed(); })
                        ]
                    ]
                ]
            ];
            ++RowIndex;
            continue;
        }
        const auto Value = *Existing;
        if (!Value.IsValid()) continue;

        auto PairCardBody = SNew(SVerticalBox);
        auto HeaderRow = SNew(SHorizontalBox);
        HeaderRow->AddSlot().FillWidth(1).VAlign(VAlign_Center)
        [
            SNew(STextBlock)
            .TextStyle(&EditorTextStyle)
            .ColorAndOpacity(FSlateColor(Palette.Text))
            .Text(Text(Key))
        ];
        HeaderRow->AddSlot().AutoWidth().VAlign(VAlign_Center)
        [
            MakeEditorButton(TEXT("Remove"), [this, Object, Key]
            { if (bRawDirty) { Message = TEXT("Apply JSON first."); return; } Remember(); Object->RemoveField(Key); Changed(); })
        ];
        PairCardBody->AddSlot().AutoHeight().Padding(0, 0, 0, 6)[HeaderRow];

        if (Value->Type == EJson::Object)
        {
            auto Nested = SNew(SVerticalBox);
            const auto HintObject = Hint && (*Hint).IsValid() && (*Hint)->Type == EJson::Object ? (*Hint)->AsObject() : V3DProjectEditor::EntryTemplate(Context);
            AddObject(Value->AsObject(), HintObject, Nested, Depth + 1, Key);
            PairCardBody->AddSlot().AutoHeight().Padding(4, 2, 0, 0)[SNew(SExpandableArea).InitiallyCollapsed(true)
                .BorderImage(WhiteBrush)
                .BorderBackgroundColor(Palette.Panel)
                .BodyBorderImage(WhiteBrush)
                .BodyBorderBackgroundColor(Palette.Background)
                .Padding(FMargin(8.f, 8.f))
                .HeaderContent()[SNew(STextBlock).TextStyle(&EditorTextStyle).Text(Text(Key + TEXT(" fields")))]
                .BodyContent()[Nested]];
        }
        else if (Value->Type == EJson::Array)
        {
            auto Nested = SNew(SVerticalBox);
            const auto& Array = Value->AsArray();
            for (int32 I = 0; I < FMath::Min(Array.Num(), 128); ++I)
            {
                if (--FormRowsRemaining < 0) break;
                auto Entry = Array[I];
                if (!Entry.IsValid()) continue;

                auto ItemBody = SNew(SVerticalBox);
                ItemBody->AddSlot().AutoHeight().Padding(0, 0, 0, 6)[MakeEditorButton(*FString::Printf(TEXT("Remove [%d]"), I), [this, Object, Key, I]
                {
                    if (bRawDirty) return;
                    const TArray<TSharedPtr<FJsonValue>>* ExistingArr = nullptr;
                    if (!Object->TryGetArrayField(Key, ExistingArr) || !ExistingArr) return;
                    Remember(); TArray<TSharedPtr<FJsonValue>> Values = *ExistingArr;
                    if (Values.IsValidIndex(I)) Values.RemoveAt(I);
                    Object->SetArrayField(Key, Values); Changed();
                })];
                if (Entry->Type == EJson::Object) AddObject(Entry->AsObject(), V3DProjectEditor::EntryTemplate(Key), ItemBody, Depth + 1, Key);
                else ItemBody->AddSlot().AutoHeight()[SNew(SEditableTextBox).Style(&EditorInputStyle).Text(Text(ValueText(Entry)))
                    .OnTextCommitted_Lambda([this, Object, Key, I, Entry](const FText& T, ETextCommit::Type)
                    {
                        if (bBusy || bRawDirty || bRebuildingForm || bFormRefreshPending) return;
                        TSharedPtr<FJsonValue> Updated;
                        if (Entry->Type == EJson::String) Updated = MakeShared<FJsonValueString>(T.ToString());
                        else
                        {
                            const auto P = FSafeFileIO::ParseJsonText(TEXT("{\"v\":") + T.ToString() + TEXT("}"), TEXT("Array item"));
                            if (!P.IsSuccess() || !P.JsonObject.IsValid()) { Message = P.Error; return; }
                            const auto* Found = P.JsonObject->Values.Find(TEXT("v"));
                            if (!Found || !Found->IsValid()) { Message = TEXT("Invalid array item value."); return; }
                            Updated = *Found;
                        }
                        const TArray<TSharedPtr<FJsonValue>>* ExistingArr = nullptr;
                        if (!Object->TryGetArrayField(Key, ExistingArr) || !ExistingArr) return;
                        Remember(); TArray<TSharedPtr<FJsonValue>> Values = *ExistingArr;
                        if (Values.IsValidIndex(I)) Values[I] = Updated;
                        Object->SetArrayField(Key, Values); Changed();
                    })];

                Nested->AddSlot().AutoHeight().Padding(0, 0, 0, 8)
                [
                    SNew(SBorder)
                    .BorderImage(WhiteBrush)
                    .BorderBackgroundColor(CardBorderColor(bDarkMode, false))
                    .Padding(1.f)
                    [
                        SNew(SBorder)
                        .BorderImage(WhiteBrush)
                        .BorderBackgroundColor(CardFillColor(Palette, bDarkMode, I, false))
                        .Padding(FMargin(8.f, 6.f))
                        [
                            ItemBody
                        ]
                    ]
                ];
            }
            Nested->AddSlot().AutoHeight().Padding(0, 2, 0, 0)[MakeEditorButton(TEXT("Append item"), [this, Object, Key]
            {
                if (bRawDirty) return;
                const TArray<TSharedPtr<FJsonValue>>* ExistingArr = nullptr;
                if (!Object->TryGetArrayField(Key, ExistingArr) || !ExistingArr) return;
                Remember(); TArray<TSharedPtr<FJsonValue>> Values = *ExistingArr;
                if (Key == TEXT("Sounds") || Key == TEXT("WheelMeshNames")) Values.Add(MakeShared<FJsonValueString>(TEXT("")));
                else if (Key == TEXT("WheelHeightOffsets")) Values.Add(MakeShared<FJsonValueNumber>(0));
                else Values.Add(MakeShared<FJsonValueObject>(V3DProjectEditor::EntryTemplate(Key)));
                Object->SetArrayField(Key, Values); Changed();
            })];
            PairCardBody->AddSlot().AutoHeight().Padding(4, 2, 0, 0)[SNew(SExpandableArea).InitiallyCollapsed(true)
                .BorderImage(WhiteBrush)
                .BorderBackgroundColor(Palette.Panel)
                .BodyBorderImage(WhiteBrush)
                .BodyBorderBackgroundColor(Palette.Background)
                .Padding(FMargin(8.f, 8.f))
                .HeaderContent()[SNew(STextBlock).TextStyle(&EditorTextStyle).Text(Text(FString::Printf(TEXT("%s [%d items]"), *Key, Array.Num())))]
                .BodyContent()[Nested]];
        }
        else if (Value->Type == EJson::Boolean)
        {
            const bool bChecked = Value->AsBool();
            const FLinearColor IndicatorBorder = bChecked
                ? FLinearColor(0.95f, 0.98f, 1.0f, 1.f)
                : (bDarkMode ? FLinearColor(0.55f, 0.62f, 0.74f, 1.f) : FLinearColor(0.38f, 0.46f, 0.58f, 1.f));
            const FLinearColor IndicatorFill = bChecked
                ? FLinearColor(0.08f, 0.30f, 0.62f, 1.f)
                : Palette.Background;
            const FLinearColor StateTextColor = bChecked
                ? FLinearColor(0.98f, 0.99f, 1.0f, 1.f)
                : Palette.Muted;

            PairCardBody->AddSlot().AutoHeight().HAlign(HAlign_Left).Padding(0, 2, 0, 0)
            [
                SNew(SCheckBox)
                .Style(&EditorCheckStyle)
                .IsChecked(bChecked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
                .OnCheckStateChanged_Lambda([this, Object, Key](ECheckBoxState State)
                { if (bBusy || bRawDirty || bRebuildingForm || bFormRefreshPending) return; Remember(); Object->SetBoolField(Key, State == ECheckBoxState::Checked); Changed(); })
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                    [
                        SNew(SBorder)
                        .BorderImage(WhiteBrush)
                        .BorderBackgroundColor(IndicatorBorder)
                        .Padding(2.f)
                        [
                            SNew(SBox).WidthOverride(18.f).HeightOverride(18.f)
                            [
                                SNew(SBorder)
                                .BorderImage(WhiteBrush)
                                .BorderBackgroundColor(IndicatorFill)
                                .HAlign(HAlign_Center)
                                .VAlign(VAlign_Center)
                                .Padding(0.f)
                                [
                                    SNew(STextBlock)
                                    .TextStyle(&EditorTextStyle)
                                    .ColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 1.f)))
                                    .Text(Text(bChecked ? TEXT("✓") : TEXT("")))
                                ]
                            ]
                        ]
                    ]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .TextStyle(&EditorTextStyle)
                        .ColorAndOpacity(FSlateColor(StateTextColor))
                        .Text(Text(bChecked ? TEXT("ON (true)") : TEXT("OFF (false)")))
                    ]
                ]
            ];
        }
        else if (Key == TEXT("Shader") || Key == TEXT("MaterialMode") || Key == TEXT("AssetType") || Key == TEXT("ModelType")
            || Key == TEXT("EntityType") || Key == TEXT("ItemType") || Key == TEXT("ProjectType")
            || Key == TEXT("DominantHand") || Key == TEXT("PrimaryHandOverride") || Key == TEXT("Hand")
            || Key == TEXT("Role") || Key == TEXT("Falloff") || Key == TEXT("Shape")
            || Key == TEXT("FrictionCombineMode") || Key == TEXT("RestitutionCombineMode"))
        {
            auto Options = MakeShared<TArray<TSharedPtr<FString>>>();
            FString DisplayValue = ValueText(Value);
            TArray<FString> Names;
            if (Key == TEXT("Shader")) Names = {TEXT("Default"), TEXT("Toon")};
            else if (Key == TEXT("MaterialMode")) Names = {TEXT("Auto"), TEXT("Opaque"), TEXT("Opaque Two Sided"), TEXT("Translucent"), TEXT("Translucent Two Sided")};
            else if (Key == TEXT("AssetType")) Names = {TEXT("Model"), TEXT("Sound")};
            else if (Key == TEXT("ModelType")) Names = {TEXT("Static"), TEXT("Dynamic"), TEXT("Character")};
            else if (Key == TEXT("EntityType")) Names = {TEXT("Prop"), TEXT("Vehicle"), TEXT("Animal")};
            else if (Key == TEXT("ItemType")) Names = {TEXT("Misc"), TEXT("Weapon"), TEXT("Tool")};
            else if (Key == TEXT("ProjectType")) Names = {TEXT("World"), TEXT("Prefab"), TEXT("Dynamic"), TEXT("Character")};
            else if (Key == TEXT("DominantHand") || Key == TEXT("PrimaryHandOverride") || Key == TEXT("Hand")) Names = {TEXT("Right"), TEXT("Left")};
            else if (Key == TEXT("Role")) Names = {TEXT("Disabled"), TEXT("Primary"), TEXT("Secondary"), TEXT("Support")};
            else if (Key == TEXT("Falloff")) Names = {TEXT("Constant"), TEXT("Linear"), TEXT("InverseSquare")};
            else if (Key == TEXT("Shape")) Names = {TEXT("Box"), TEXT("Sphere")};
            else if (Key == TEXT("FrictionCombineMode") || Key == TEXT("RestitutionCombineMode")) Names = {TEXT("Average"), TEXT("Min"), TEXT("Multiply"), TEXT("Max")};
            for (const auto& Name : Names) Options->Add(MakeShared<FString>(Name));
            PairCardBody->AddSlot().AutoHeight()[SNew(SComboBox<TSharedPtr<FString>>).ComboBoxStyle(&EditorComboStyle).ItemStyle(&EditorRowStyle).OptionsSource(&Options.Get())
                .OnGenerateWidget_Lambda([this, Options](TSharedPtr<FString> Item) { return SNew(STextBlock).TextStyle(&EditorTextStyle).Text(Item.IsValid() ? Text(*Item) : FText::GetEmpty()); })
                .OnSelectionChanged_Lambda([this, Object, Key, Options](TSharedPtr<FString> Item, ESelectInfo::Type Info)
                {
                    if (!Item || bBusy || bRawDirty || bRebuildingForm || bFormRefreshPending || Info == ESelectInfo::Direct) return;
                    Remember(); Object->SetStringField(Key, *Item);
                    // Only remove mutually exclusive discriminators, preserving all unrelated authored data.
                    if (Key == TEXT("AssetType") && *Item == TEXT("Sound")) Object->RemoveField(TEXT("ModelType"));
                    if (Key == TEXT("ModelType") && *Item != TEXT("Dynamic"))
                    { Object->RemoveField(TEXT("EntityType")); Object->RemoveField(TEXT("ItemType")); }
                    if (Key == TEXT("EntityType")) Object->RemoveField(TEXT("ItemType"));
                    if (Key == TEXT("ItemType")) Object->RemoveField(TEXT("EntityType"));
                    if (Key == TEXT("ModelType") && *Item == TEXT("Character")) Object->RemoveField(TEXT("GravityField"));
                    Changed();
                })[SNew(STextBlock).TextStyle(&EditorTextStyle).Text(Text(DisplayValue))]];
        }
        else
        {
            const EJson ExpectedType = Value->Type;
            const bool IsString = ExpectedType == EJson::String;
            PairCardBody->AddSlot().AutoHeight()[SNew(SEditableTextBox).Style(&EditorInputStyle).Text(Text(ValueText(Value)))
                .OnTextCommitted_Lambda([this, Object, Key, IsString, ExpectedType](const FText& TextValue, ETextCommit::Type)
                {
                    if (bBusy || bRawDirty || bRebuildingForm || bFormRefreshPending) { Message = TEXT("Apply pending JSON before editing fields."); return; }
                    if (IsString)
                    {
                        FString ExistingStr;
                        if (Object->TryGetStringField(Key, ExistingStr) && ExistingStr == TextValue.ToString()) return;
                        Remember(); Object->SetStringField(Key, TextValue.ToString()); Changed(); return;
                    }
                    const auto Parsed = FSafeFileIO::ParseJsonText(TEXT("{\"v\":") + TextValue.ToString() + TEXT("}"), TEXT("Field"));
                    if (!Parsed.IsSuccess() || !Parsed.JsonObject.IsValid())
                    { Message = TEXT("Invalid value/type for ") + Key + TEXT(". Use advanced JSON to change types."); return; }
                    const auto* Found = Parsed.JsonObject->Values.Find(TEXT("v"));
                    if (!Found || !Found->IsValid() || (*Found)->Type != ExpectedType)
                    { Message = TEXT("Invalid value/type for ") + Key + TEXT(". Use advanced JSON to change types."); return; }
                    Remember(); Object->SetField(Key, *Found); Changed();
                })];
        }

        Box->AddSlot().AutoHeight().Padding(0, 0, 0, 8)
        [
            SNew(SBorder)
            .BorderImage(WhiteBrush)
            .BorderBackgroundColor(CardBorderColor(bDarkMode, false))
            .Padding(1.f)
            [
                SNew(SBorder)
                .BorderImage(WhiteBrush)
                .BorderBackgroundColor(CardFillColor(Palette, bDarkMode, RowIndex, false))
                .Padding(FMargin(10.f, 8.f))
                [
                    PairCardBody
                ]
            ]
        ];
        ++RowIndex;
    }

    auto NewType = MakeShared<FString>(TEXT("String"));
    auto Types = MakeShared<TArray<TSharedPtr<FString>>>();
    for (const TCHAR* T : {TEXT("String"), TEXT("Number"), TEXT("Boolean"), TEXT("Object"), TEXT("Array")}) Types->Add(MakeShared<FString>(T));
    auto TypePicker = SNew(SComboBox<TSharedPtr<FString>>).ComboBoxStyle(&EditorComboStyle).ItemStyle(&EditorRowStyle).OptionsSource(&Types.Get())
        .OnGenerateWidget_Lambda([this, Types](TSharedPtr<FString> T) { return SNew(STextBlock).TextStyle(&EditorTextStyle).Text(T.IsValid() ? Text(*T) : FText::GetEmpty()); })
        .OnSelectionChanged_Lambda([NewType, Types](TSharedPtr<FString> T, ESelectInfo::Type) { if (T) *NewType = *T; })
        [SNew(STextBlock).TextStyle(&EditorTextStyle).Text_Lambda([NewType] { return Text(*NewType); })];
    auto NewKey = SNew(SEditableTextBox).Style(&EditorInputStyle).HintText(Text(TEXT("New key (including custom keys)")));
    Box->AddSlot().AutoHeight().Padding(0, 4, 0, 8)
    [
        SNew(SBorder)
        .BorderImage(WhiteBrush)
        .BorderBackgroundColor(CardBorderColor(bDarkMode, true))
        .Padding(1.f)
        [
            SNew(SBorder)
            .BorderImage(WhiteBrush)
            .BorderBackgroundColor(Palette.Panel)
            .Padding(FMargin(10.f, 8.f))
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 6, 0)[NewKey]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)[TypePicker]
                + SHorizontalBox::Slot().AutoWidth()[MakeEditorButton(TEXT("Add key"), [this, Object, NewKey, NewType, Context]
                {
                    const FString Key = NewKey->GetText().ToString().TrimStartAndEnd();
                    if (Key.IsEmpty() || Object->HasField(Key) || bRawDirty) { Message = TEXT("Use a new non-empty key; Apply JSON first."); return; }
                    Remember();
                    const auto EntryDefaults = V3DProjectEditor::EntryTemplate(Context);
                    if (!EntryDefaults->Values.IsEmpty()) Object->SetObjectField(Key, EntryDefaults);
                    else if (*NewType == TEXT("Number")) Object->SetNumberField(Key, 0);
                    else if (*NewType == TEXT("Boolean")) Object->SetBoolField(Key, false);
                    else if (*NewType == TEXT("Object")) Object->SetObjectField(Key, MakeShared<FJsonObject>());
                    else if (*NewType == TEXT("Array")) Object->SetArrayField(Key, TArray<TSharedPtr<FJsonValue>>());
                    else Object->SetStringField(Key, TEXT(""));
                    Changed();
                })]
            ]
        ]
    ];
}

void UProjectEditorWidget::SaveAll(bool bBuild, bool bClose)
{
    if (bBusy || !ApplyRaw()) return;
    if (PreviewBuilder) { Message = TEXT("Wait for the preview to finish before Save/Build."); return; }
    bBusy = true; Message = TEXT("Validating and saving JSON...");
    // Detached snapshots only. UI is disabled for the whole transaction.
    TArray<FV3DEditorDocument> Copies;
    for (const auto& Doc : Documents) if (Doc.IsValid() && Doc->bDirty)
    {
        FV3DEditorDocument Copy;
        Copy.Path = Doc->Path; Copy.SourcePath = Doc->SourcePath; Copy.Label = Doc->Label;
        Copy.DiskText = Doc->DiskText; Copy.bDirty = true; Copy.bNewFile = Doc->bNewFile;
        Copy.Json = V3DProjectEditor::CloneObject(Doc->Json);
        Copies.Add(MoveTemp(Copy));
    }
    const FString Name = Project; const uint64 Token = RequestGeneration;
    TWeakObjectPtr<UProjectEditorWidget> Weak(this);
    const bool Queued = FSafeFileIO::RunTrackedWorker([Weak, Token, Name, Copies = MoveTemp(Copies), bBuild, bClose]() mutable
    {
        FString Error; bool Success = true;
        // Reject schema errors before writing any file.
        for (auto& Doc : Copies) if (!V3DProjectEditor::Validate(Doc, Name, Error, &Copies)) { Success = false; break; }
        if (Success) for (auto& Doc : Copies) if (!V3DProjectEditor::Save(Doc, Name, Error, &Copies)) { Success = false; break; }
        FSafeFileIO::DispatchTrackedGameThread([Weak, Token, Copies = MoveTemp(Copies), Error, Success, bBuild, bClose]() mutable
        {
            auto* Self = Weak.Get(); if (!Self || Self->RequestGeneration != Token) return;
            Self->bBusy = false;
            TMap<FString, const FV3DEditorDocument*> SavedByPath;
            for (const auto& Saved : Copies) if (!Saved.bDirty) SavedByPath.Add(Saved.Path, &Saved);
            for (auto& Doc : Self->Documents)
                if (Doc.IsValid())
                    if (const auto* Saved = SavedByPath.Find(Doc->Path))
                    { Doc->DiskText = (*Saved)->DiskText; Doc->bDirty = Doc->bNewFile = false; }
            Self->Message = Success ? TEXT("Saved. Existing built worlds are replaced only when Build succeeds.") : Error;
            if (Success && bBuild) Self->StartBuild();
            else if (Success && bClose) Self->Close();
        });
    });
    if (!Queued) { bBusy = false; Message = TEXT("Save queue is unavailable. Changes remain in Editor."); }
}

void UProjectEditorWidget::StartBuild()
{
    auto* Manager = UGameManagerSubSystem::GetSubSystem(this);
    if (!Manager || Manager->IsProjectBuildInProgress()) { Message = TEXT("A build is already running."); return; }
    BuildStatus = CreateWidget<UBuildStatusWidget>(GetOwningPlayer());
    if (!BuildStatus) { Message = TEXT("Cannot create Build status."); return; }
    BuildStatus->OnConfirmed.AddDynamic(this, &UProjectEditorWidget::BuildConfirmed);
    BuildStatus->AddToPlayerScreen(60);
    BuildStatus->BeginObservingBuild(Project);
    SetVisibility(ESlateVisibility::Collapsed);
    if (!Manager->BuildProjectByName(Project)) BuildStatus->ReportBuildRejected(Manager->GetLastMessage());
}
void UProjectEditorWidget::BuildConfirmed()
{
    if (IsValid(BuildStatus))
    {
        BuildStatus->OnConfirmed.RemoveDynamic(this, &UProjectEditorWidget::BuildConfirmed);
        BuildStatus->StopObservingBuild();
        BuildStatus->RemoveFromParent();
    }
    BuildStatus = nullptr;
    SetVisibility(ESlateVisibility::Visible);
}
bool UProjectEditorWidget::HasUnsavedChanges() const
{
    if (bRawDirty) return true;
    for (const auto& Document : Documents) if (Document && Document->bDirty) return true;
    return false;
}
void UProjectEditorWidget::RequestBack()
{
    if (bBusy || BuildStatus) return;
    if (HasUnsavedChanges()) bConfirmBack = true;
    else Close();
}
FReply UProjectEditorWidget::NativeOnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
{
    if (Event.GetKey() == EKeys::Escape)
    {
        if (bConfirmBack) bConfirmBack = false; else RequestBack();
        return FReply::Handled();
    }
    return Super::NativeOnKeyDown(Geometry, Event);
}
void UProjectEditorWidget::Close()
{
    if (bBusy) return;
    RemoveFromParent(); OnClosed.ExecuteIfBound();
}
void UProjectEditorWidget::NativeDestruct()
{
    ++RequestGeneration;
    bBusy = false; bFormRefreshPending = false;
    InvalidatePreview();
    if (IsValid(BuildStatus))
    {
        BuildStatus->OnConfirmed.RemoveDynamic(this, &UProjectEditorWidget::BuildConfirmed);
        BuildStatus->StopObservingBuild();
        BuildStatus->RemoveFromParent();
    }
    BuildStatus = nullptr;
    Super::NativeDestruct();
}
void UProjectEditorWidget::ReleaseSlateResources(bool bReleaseChildren)
{
    Super::ReleaseSlateResources(bReleaseChildren);
    bFormRefreshPending = false;
    List.Reset(); Form.Reset(); Raw.Reset(); Preview.Reset();
}

void UProjectEditorWidget::StartPreview()
{
    if (bBusy || !Current || !ApplyRaw()) return;
    if (auto* M = UGameManagerSubSystem::GetSubSystem(this); M && M->IsProjectBuildInProgress()) return;
    if (PreviewBuilder) { Message = TEXT("A model preview is already loading."); return; }
    if (Current->SourcePath.IsEmpty() || FPaths::GetExtension(Current->SourcePath).Equals(TEXT("wav"), ESearchCase::IgnoreCase))
    { Message = TEXT("Select a model resource for geometry preview."); return; }
    if (IFileManager::Get().FileSize(*Current->SourcePath) > 32ll * 1024 * 1024)
    { Message = TEXT("Preview source limit is 32 MiB. JSON editing and full builds remain available."); return; }
    FModelDefinition Definition; FString Error;
    if (!ModelDefinitionJson::ValidateDefinition(Current->Json, Current->Path, Current->SourcePath, Definition, Error))
    { Message = Error; return; }
    PreviewBuilder = NewObject<UProjectSourceModelBuilder>(this);
    if (auto* R = UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this); R && Definition.ModelType == EModelDefinitionType::Character)
    {
        PreviewBuilder->SetCharacterSkeletonOverride(R->DefaultCharacterSkeleton.LoadSynchronous());
    }
    PreviewDocument = Current->Path;
    PreviewBuilder->OnFinished.AddUObject(this, &UProjectEditorWidget::PreviewFinished);
    Message = TEXT("Loading read-only wireframe preview...");
    if (!PreviewBuilder->Start(Definition, V3DProjectEditor::Serialize(Current->Json)) && PreviewBuilder)
    { Message = PreviewBuilder->GetError(); PreviewBuilder = nullptr; }
}
void UProjectEditorWidget::PreviewFinished(UProjectSourceModelBuilder* Builder, bool bSuccess)
{
    if (Builder != PreviewBuilder) return;
    if (bSuccess && Preview && Current && Current->Path == PreviewDocument)
    {
        const auto Model = Builder->TakeBuildModel();
        Preview->SetModel(Model);
        Message = TEXT("Read-only wireframe (up to 3000 edges). Drag to orbit; scroll to zoom. No positions are changed.");
    }
    else if (!bSuccess && IsValid(Builder)) Message = Builder->GetError();
    PreviewBuilder = nullptr;
}
