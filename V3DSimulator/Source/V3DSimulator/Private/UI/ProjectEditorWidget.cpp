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
#include "Simulator/NodeTokenLibrary.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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
#include "System/SourceModelImport.h"
#include "System/V3DTerrain.h"
#include "Model/V3DMaterialAssetReferences.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Text/BaseTextLayoutMarshaller.h"
#include "Framework/Text/SlateTextRun.h"
#include "Framework/Text/TextLayout.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

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


/**
 * Slate syntax-highlighting marshaller for JSON documents.
 * Distinguishes keys, string values, numbers, booleans/null, curly braces, square brackets,
 * and punctuation with dedicated high-contrast palettes for both Dark and Light modes.
 */
class FV3DJsonSyntaxHighlighter : public FBaseTextLayoutMarshaller
{
public:
    static TSharedRef<FV3DJsonSyntaxHighlighter> Create(const bool bInDarkMode, const FSlateFontInfo& InFont)
    {
        return MakeShareable(new FV3DJsonSyntaxHighlighter(bInDarkMode, InFont));
    }

    void SetTheme(const bool bInDarkMode, const FSlateFontInfo& InFont)
    {
        const FTextBlockStyle Base = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
        auto MakeStyle = [&Base, &InFont](const FLinearColor& Color)
        {
            FTextBlockStyle Style = Base;
            Style.SetFont(InFont);
            Style.SetColorAndOpacity(FSlateColor(Color));
            return Style;
        };

        if (bInDarkMode)
        {
            NormalStyle      = MakeStyle(FLinearColor(0.88f, 0.91f, 0.96f, 1.f));
            KeyStyle         = MakeStyle(FLinearColor(0.48f, 0.80f, 1.00f, 1.f));
            StringStyle      = MakeStyle(FLinearColor(0.96f, 0.68f, 0.50f, 1.f));
            NumberStyle      = MakeStyle(FLinearColor(0.64f, 0.90f, 0.62f, 1.f));
            KeywordStyle     = MakeStyle(FLinearColor(0.76f, 0.60f, 1.00f, 1.f));
            BraceStyle       = MakeStyle(FLinearColor(1.00f, 0.84f, 0.35f, 1.f));
            BracketStyle     = MakeStyle(FLinearColor(0.38f, 0.88f, 0.82f, 1.f));
            PunctuationStyle = MakeStyle(FLinearColor(0.66f, 0.72f, 0.80f, 1.f));
        }
        else
        {
            NormalStyle      = MakeStyle(FLinearColor(0.08f, 0.11f, 0.16f, 1.f));
            KeyStyle         = MakeStyle(FLinearColor(0.05f, 0.28f, 0.62f, 1.f));
            StringStyle      = MakeStyle(FLinearColor(0.68f, 0.16f, 0.12f, 1.f));
            NumberStyle      = MakeStyle(FLinearColor(0.05f, 0.45f, 0.22f, 1.f));
            KeywordStyle     = MakeStyle(FLinearColor(0.48f, 0.12f, 0.64f, 1.f));
            BraceStyle       = MakeStyle(FLinearColor(0.66f, 0.38f, 0.02f, 1.f));
            BracketStyle     = MakeStyle(FLinearColor(0.02f, 0.46f, 0.50f, 1.f));
            PunctuationStyle = MakeStyle(FLinearColor(0.32f, 0.38f, 0.46f, 1.f));
        }
        MakeDirty();
    }

    virtual void SetText(const FString& SourceString, FTextLayout& TargetTextLayout) override
    {
        TArray<FTextRange> LineRanges;
        FTextRange::CalculateLineRangesFromString(SourceString, LineRanges);

        TArray<FTextLayout::FNewLineData> LinesToAdd;
        LinesToAdd.Reserve(LineRanges.Num());

        for (const FTextRange& LineRange : LineRanges)
        {
            TSharedRef<FString> LineText = MakeShared<FString>(
                SourceString.Mid(LineRange.BeginIndex, LineRange.Len()));
            TArray<TSharedRef<IRun>> Runs;
            TokenizeLine(LineText, Runs);
            if (Runs.IsEmpty())
            {
                Runs.Add(FSlateTextRun::Create(FRunInfo(), LineText, NormalStyle, FTextRange(0, LineText->Len())));
            }
            LinesToAdd.Add(FTextLayout::FNewLineData(MoveTemp(LineText), MoveTemp(Runs)));
        }

        TargetTextLayout.AddLines(LinesToAdd);
    }

    virtual void GetText(FString& TargetString, const FTextLayout& SourceTextLayout) override
    {
        SourceTextLayout.GetAsText(TargetString);
    }

    virtual bool RequiresLiveUpdate() const override
    {
        return true;
    }

private:
    FV3DJsonSyntaxHighlighter(const bool bInDarkMode, const FSlateFontInfo& InFont)
    {
        SetTheme(bInDarkMode, InFont);
    }

    static bool IsIdentifierChar(const TCHAR Ch)
    {
        return FChar::IsAlnum(Ch) || Ch == TEXT('_');
    }

    void TokenizeLine(const TSharedRef<FString>& LineText, TArray<TSharedRef<IRun>>& OutRuns) const
    {
        const FString& Str = *LineText;
        const int32 Len = Str.Len();
        OutRuns.Reserve(FMath::Clamp(Len / 4 + 4, 8, 256));
        int32 Index = 0;

        auto Emit = [&LineText, &OutRuns](const int32 Start, const int32 End, const FTextBlockStyle& Style)
        {
            if (End > Start)
            {
                OutRuns.Add(FSlateTextRun::Create(FRunInfo(), LineText, Style, FTextRange(Start, End)));
            }
        };

        while (Index < Len)
        {
            const TCHAR Ch = Str[Index];

            // Whitespace run
            if (FChar::IsWhitespace(Ch))
            {
                const int32 Start = Index;
                while (Index < Len && FChar::IsWhitespace(Str[Index])) ++Index;
                Emit(Start, Index, NormalStyle);
                continue;
            }

            // String literal (JSON key if followed by ':', otherwise string value)
            if (Ch == TEXT('"'))
            {
                const int32 Start = Index++;
                bool bEscaped = false;
                while (Index < Len)
                {
                    const TCHAR C = Str[Index++];
                    if (bEscaped) { bEscaped = false; continue; }
                    if (C == TEXT('\\')) { bEscaped = true; continue; }
                    if (C == TEXT('"')) break;
                }
                int32 Lookahead = Index;
                while (Lookahead < Len && FChar::IsWhitespace(Str[Lookahead])) ++Lookahead;
                const bool bIsKey = (Lookahead < Len && Str[Lookahead] == TEXT(':'));
                Emit(Start, Index, bIsKey ? KeyStyle : StringStyle);
                continue;
            }

            // Curly braces
            if (Ch == TEXT('{') || Ch == TEXT('}'))
            {
                Emit(Index, Index + 1, BraceStyle);
                ++Index;
                continue;
            }

            // Square brackets
            if (Ch == TEXT('[') || Ch == TEXT(']'))
            {
                Emit(Index, Index + 1, BracketStyle);
                ++Index;
                continue;
            }

            // Punctuation
            if (Ch == TEXT(':') || Ch == TEXT(','))
            {
                Emit(Index, Index + 1, PunctuationStyle);
                ++Index;
                continue;
            }

            // Number literal
            if (FChar::IsDigit(Ch) || (Ch == TEXT('-') && Index + 1 < Len && FChar::IsDigit(Str[Index + 1])))
            {
                const int32 Start = Index++;
                while (Index < Len)
                {
                    const TCHAR C = Str[Index];
                    if (FChar::IsDigit(C) || C == TEXT('.') || C == TEXT('e') || C == TEXT('E')
                        || ((C == TEXT('+') || C == TEXT('-')) && Index > Start
                            && (Str[Index - 1] == TEXT('e') || Str[Index - 1] == TEXT('E'))))
                    {
                        ++Index;
                    }
                    else
                    {
                        break;
                    }
                }
                Emit(Start, Index, NumberStyle);
                continue;
            }

            // Keywords: true, false, null
            auto MatchKeyword = [&Str, Len, Index](const TCHAR* Keyword, const int32 KeywordLen)
            {
                if (Index + KeywordLen > Len) return false;
                if (FCString::Strncmp(&Str[Index], Keyword, KeywordLen) != 0) return false;
                return (Index + KeywordLen == Len) || !IsIdentifierChar(Str[Index + KeywordLen]);
            };
            if (MatchKeyword(TEXT("true"), 4))
            {
                Emit(Index, Index + 4, KeywordStyle);
                Index += 4;
                continue;
            }
            if (MatchKeyword(TEXT("false"), 5))
            {
                Emit(Index, Index + 5, KeywordStyle);
                Index += 5;
                continue;
            }
            if (MatchKeyword(TEXT("null"), 4))
            {
                Emit(Index, Index + 4, KeywordStyle);
                Index += 4;
                continue;
            }

            // Fallback character
            Emit(Index, Index + 1, NormalStyle);
            ++Index;
        }
    }

    FTextBlockStyle NormalStyle;
    FTextBlockStyle KeyStyle;
    FTextBlockStyle StringStyle;
    FTextBlockStyle NumberStyle;
    FTextBlockStyle KeywordStyle;
    FTextBlockStyle BraceStyle;
    FTextBlockStyle BracketStyle;
    FTextBlockStyle PunctuationStyle;
};

struct FV3DFileBrowserEntry
{
    FString DisplayName;
    FString FullPath;
    int64 FileSizeBytes = 0;
    bool bIsDirectory = false;
};

namespace V3DFileBrowserUtils
{
    FString GetUserHomeDir()
    {
#if PLATFORM_WINDOWS
        FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
#else
        FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
#endif
        Home.TrimStartAndEndInline();
        if (!Home.IsEmpty() && IFileManager::Get().DirectoryExists(*Home))
        {
            FPaths::NormalizeDirectoryName(Home);
            return Home;
        }
        FString Fallback = FPlatformProcess::UserDir();
        FPaths::NormalizeDirectoryName(Fallback);
        return Fallback;
    }

    FString FormatFileSize(const int64 Bytes)
    {
        if (Bytes < 0) return TEXT("");
        if (Bytes < 1024) return FString::Printf(TEXT("%lld B"), Bytes);
        if (Bytes < 1024ll * 1024ll) return FString::Printf(TEXT("%.1f KB"), Bytes / 1024.0);
        return FString::Printf(TEXT("%.2f MB"), Bytes / (1024.0 * 1024.0));
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

    const FLinearColor FocusOutlineColor = bDarkMode
        ? FLinearColor(0.35f, 0.56f, 0.85f, 1.f)
        : FLinearColor(0.24f, 0.48f, 0.80f, 1.f);
    EditorInputStyle = FCoreStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");
    EditorInputStyle.TextStyle = EditorTextStyle;
    EditorInputStyle.BackgroundImageNormal = FSlateColorBrush(Palette.Input);
    EditorInputStyle.BackgroundImageHovered = FSlateColorBrush(Palette.Input);
    EditorInputStyle.BackgroundImageFocused = V3DEditorUI::MakeOutlinedBrush(Palette.Input, FocusOutlineColor, 4.f, 1.5f);
    EditorInputStyle.BackgroundImageReadOnly = FSlateColorBrush(Palette.Background);
    EditorInputStyle.BackgroundImageNormal.DrawAs = EditorInputStyle.BackgroundImageHovered.DrawAs =
        EditorInputStyle.BackgroundImageReadOnly.DrawAs = ESlateBrushDrawType::Box;
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

    if (!JsonHighlighter.IsValid())
    {
        JsonHighlighter = FV3DJsonSyntaxHighlighter::Create(bDarkMode, EditorTextStyle.Font);
    }
    else
    {
        JsonHighlighter->SetTheme(bDarkMode, EditorTextStyle.Font);
    }
    if (Raw.IsValid())
    {
        Raw->SetTextStyle(&EditorTextStyle);
        TGuardValue<bool> Guard(bSettingRaw, true);
        Raw->SetText(V3DEditorUI::Text(RawDraft));
    }
    if (List.IsValid())
    {
        List->RequestListRefresh();
    }
    if (BrowserListView.IsValid())
    {
        BrowserListView->RequestListRefresh();
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
    Toolbar->AddSlot()[MakeEditorButton(TEXT("Refresh"), [this] { RefreshProjectDocuments(); })];
    Toolbar->AddSlot()[MakeEditorButton(TEXT("Import 3D Asset"), [this] { OpenFileBrowser(); })];
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
    TWeakObjectPtr<UProjectEditorWidget> WeakPreviewSelf(this);
    Right->AddSlot().AutoHeight().Padding(0,0,0,6)
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1).Padding(0,0,6,0)[MakeEditorButton(TEXT("Preview model + overlays"), [WeakPreviewSelf] { if (auto* Self = WeakPreviewSelf.Get()) Self->StartPreview(); })]
        + SHorizontalBox::Slot().AutoWidth()[MakeEditorButton(TEXT("Reset View"), [WeakPreviewSelf] { if (auto* Self = WeakPreviewSelf.Get(); Self && Self->Preview) Self->Preview->ResetCamera(); })]
    ];
    Right->AddSlot().AutoHeight()[SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
        .BorderBackgroundColor_Lambda([this] { return FSlateColor(GetThemePalette().Panel); }).Padding(8)
        [SNew(SBox).HeightOverride(320)[SAssignNew(Preview, SV3DModelPreview)]]];
    Right->AddSlot().AutoHeight().Padding(2,4,2,4)
    [
        SNew(STextBlock).TextStyle(&EditorTextStyle).AutoWrapText(true)
        .ColorAndOpacity_Lambda([this] { return FSlateColor(GetThemePalette().Muted); })
        .Text(Text(TEXT("LMB: Orbit · RMB: Pan · Wheel: Zoom · Double-Click: Reset  |  Green: Collider · Gold: Light · Orange/Pink: Hand/Grip · Red: Muzzle")))
    ];
    Right->AddSlot().AutoHeight().Padding(0,8)[SNew(STextBlock).TextStyle(&EditorTextStyle).AutoWrapText(true).Text(Text(TEXT("JSON / advanced edit · Apply JSON to update fields")))];
    Right->AddSlot().FillHeight(1)[SAssignNew(Raw, SMultiLineEditableTextBox).Style(&EditorInputStyle).Font(EditorTextStyle.Font).Marshaller(JsonHighlighter).AutoWrapText(false)
        .OnTextChanged_Lambda([this](const FText& Value)
        { if (!bSettingRaw) { bRawDirty = true; RawDraft = Value.ToString(); } })];
    auto Body = SNew(SSplitter)
        + SSplitter::Slot().Value(.2f)[Left]
        + SSplitter::Slot().Value(.45f)[SAssignNew(FormScrollBox, SScrollBox) + SScrollBox::Slot().Padding(8, 0)[SAssignNew(Form, SVerticalBox)]]
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
    const TWeakObjectPtr<UProjectEditorWidget> WeakSelf(this);
    auto QuickBar = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(8, 6));
    QuickBar->AddSlot()[MakeEditorButton(TEXT("Up (..)"), [WeakSelf] { if (auto* S = WeakSelf.Get()) S->NavigateFileBrowserUp(); })];
    QuickBar->AddSlot()[MakeEditorButton(TEXT("Home"), [WeakSelf] { if (auto* S = WeakSelf.Get()) S->NavigateFileBrowser(V3DFileBrowserUtils::GetUserHomeDir()); })];
    QuickBar->AddSlot()[MakeEditorButton(TEXT("Desktop"), [WeakSelf] { if (auto* S = WeakSelf.Get()) S->NavigateFileBrowser(FPaths::Combine(V3DFileBrowserUtils::GetUserHomeDir(), TEXT("Desktop"))); })];
    QuickBar->AddSlot()[MakeEditorButton(TEXT("Documents"), [WeakSelf] { if (auto* S = WeakSelf.Get()) S->NavigateFileBrowser(FPaths::Combine(V3DFileBrowserUtils::GetUserHomeDir(), TEXT("Documents"))); })];
    QuickBar->AddSlot()[MakeEditorButton(TEXT("Downloads"), [WeakSelf] { if (auto* S = WeakSelf.Get()) S->NavigateFileBrowser(FPaths::Combine(V3DFileBrowserUtils::GetUserHomeDir(), TEXT("Downloads"))); })];
    QuickBar->AddSlot()[MakeEditorButton(TEXT("Project"), [WeakSelf] { if (auto* S = WeakSelf.Get()) S->NavigateFileBrowser(V3DSimulatorProjectWorkspace::ProjectRoot(S->Project)); })];

    auto BrowserModal = SNew(SBorder)
        .Visibility_Lambda([WeakSelf] { auto* S = WeakSelf.Get(); return (S && S->bShowFileBrowser) ? EVisibility::Visible : EVisibility::Collapsed; })
        .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
        .BorderBackgroundColor_Lambda([this] { return FSlateColor(GetThemePalette().ModalScrim); })
        .HAlign(HAlign_Center).VAlign(VAlign_Center)
        [SNew(SBox).WidthOverride(940.f).HeightOverride(660.f)
            [SNew(SBorder).Padding(24).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                .BorderBackgroundColor_Lambda([this] { return FSlateColor(GetThemePalette().Background); })
                [SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                        [SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                                [SNew(STextBlock).TextStyle(&EditorTextStyle).Font(TitleFont)
                                    .Text(Text(TEXT("Import 3D Assets (.glb, .gltf, .fbx, .obj, .stl)")))]
                            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                                [MakeEditorButton(TEXT("Close"), [this] { CloseFileBrowser(); })]]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)[QuickBar]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
                        [SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 8, 0)
                                [SAssignNew(BrowserPathBox, SEditableTextBox)
                                    .Style(&EditorInputStyle)
                                    .HintText(Text(TEXT("Directory path...")))
                                    .OnTextCommitted_Lambda([this](const FText& NewPath, ETextCommit::Type CommitType)
                                    {
                                        if (CommitType == ETextCommit::OnEnter)
                                        {
                                            NavigateFileBrowser(NewPath.ToString().TrimStartAndEnd());
                                        }
                                    })]
                            + SHorizontalBox::Slot().AutoWidth()
                                [MakeEditorButton(TEXT("Go"), [this]
                                {
                                    if (BrowserPathBox.IsValid())
                                    {
                                        NavigateFileBrowser(BrowserPathBox->GetText().ToString().TrimStartAndEnd());
                                    }
                                })]]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
                        [SNew(SEditableTextBox)
                            .Style(&EditorInputStyle)
                            .HintText(Text(TEXT("Filter folders / 3D assets in this directory...")))
                            .OnTextChanged_Lambda([this](const FText& Val)
                            {
                                BrowserFilter = Val.ToString();
                                RefreshFileBrowserEntries();
                            })]
                    + SVerticalBox::Slot().FillHeight(1).Padding(0, 0, 0, 12)
                        [SNew(SBorder).Padding(4).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                            .BorderBackgroundColor_Lambda([this] { return FSlateColor(GetThemePalette().Panel); })
                            [SAssignNew(BrowserListView, SListView<TSharedPtr<FV3DFileBrowserEntry>>)
                                .ListViewStyle(&EditorListStyle)
                                .ListItemsSource(&BrowserEntries)
                                .SelectionMode(ESelectionMode::None)
                                .OnGenerateRow_Lambda([this](TSharedPtr<FV3DFileBrowserEntry> Entry, const TSharedRef<STableViewBase>& Owner)
                                {
                                    return SNew(STableRow<TSharedPtr<FV3DFileBrowserEntry>>, Owner).Style(&EditorRowStyle)
                                        [SNew(SButton)
                                            .ButtonStyle(&EditorButtonStyle)
                                            .ContentPadding(FMargin(10, 6))
                                            .OnClicked_Lambda([this, Entry]()
                                            {
                                                if (!Entry.IsValid()) return FReply::Handled();
                                                if (Entry->bIsDirectory)
                                                {
                                                    NavigateFileBrowser(Entry->FullPath);
                                                }
                                                else
                                                {
                                                    if (BrowserSelectedFiles.Contains(Entry->FullPath))
                                                        BrowserSelectedFiles.Remove(Entry->FullPath);
                                                    else
                                                        BrowserSelectedFiles.Add(Entry->FullPath);
                                                    if (BrowserListView.IsValid()) BrowserListView->RequestListRefresh();
                                                }
                                                return FReply::Handled();
                                            })
                                            [SNew(SHorizontalBox)
                                                + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                                                    [SNew(STextBlock).TextStyle(&EditorTextStyle)
                                                        .Text_Lambda([this, Entry]
                                                        {
                                                            if (!Entry.IsValid()) return FText::GetEmpty();
                                                            if (Entry->bIsDirectory)
                                                            {
                                                                return Text(TEXT("[DIR]  ") + Entry->DisplayName);
                                                            }
                                                            const bool bSelected = BrowserSelectedFiles.Contains(Entry->FullPath);
                                                            return Text((bSelected ? TEXT("[✓]  ") : TEXT("[ ]  ")) + Entry->DisplayName);
                                                        })]
                                                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0, 0, 0)
                                                    [SNew(STextBlock).TextStyle(&EditorTextStyle)
                                                        .ColorAndOpacity_Lambda([this] { return FSlateColor(GetThemePalette().Muted); })
                                                        .Text(Entry.IsValid() && !Entry->bIsDirectory
                                                            ? Text(V3DFileBrowserUtils::FormatFileSize(Entry->FileSizeBytes))
                                                            : FText::GetEmpty())]]];
                                })]]
                    + SVerticalBox::Slot().AutoHeight()
                        [SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                                [SNew(STextBlock).TextStyle(&EditorTextStyle)
                                    .Text_Lambda([this]
                                    {
                                        if (!BrowserStatus.IsEmpty()) return Text(BrowserStatus);
                                        return Text(FString::Printf(TEXT("%d file(s) selected"), BrowserSelectedFiles.Num()));
                                    })]
                            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                                [MakeEditorButton(TEXT("Cancel"), [this] { CloseFileBrowser(); })]
                            + SHorizontalBox::Slot().AutoWidth()
                                [MakeEditorButton(TEXT("Import Selected"), [this] { ConfirmFileBrowserImport(); })]]]]];

    return SNew(SOverlay)
        + SOverlay::Slot()[SNew(SBox).IsEnabled_Lambda([this] { return !bConfirmBack && !bShowFileBrowser; })[Root]]
        + SOverlay::Slot()[BrowserModal]
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

void UProjectEditorWidget::RefreshProjectDocuments(const FString& PreferredPath, const FString& CompletionMessage)
{
    if (bBusy || BuildStatus || Project.IsEmpty()) return;
    if (!ApplyRaw()) return;

    const FString TargetSelection = !PreferredPath.IsEmpty()
        ? FSafeFileIO::NormalizeFilePath(PreferredPath)
        : (Current.IsValid() ? Current->Path : FString());

    bBusy = true;
    Message = TEXT("Refreshing project files...");
    const FString ProjectName = Project;
    const uint64 Token = ++RequestGeneration;
    const TWeakObjectPtr<UProjectEditorWidget> Weak(this);
    const bool Queued = FSafeFileIO::RunTrackedWorker([Weak, Token, ProjectName, TargetSelection, CompletionMessage]
    {
        TArray<TSharedPtr<FV3DEditorDocument>> Found;
        FString Error;
        const bool bSuccess = V3DProjectEditor::Discover(ProjectName, Found, Error);
        FSafeFileIO::DispatchTrackedGameThread([Weak, Token, Found = MoveTemp(Found), Error, bSuccess, TargetSelection, CompletionMessage]() mutable
        {
            auto* Self = Weak.Get();
            if (!Self || Self->RequestGeneration != Token) return;
            Self->bBusy = false;
            if (!bSuccess)
            {
                Self->Message = Error.IsEmpty() ? TEXT("Refresh failed.") : Error;
                return;
            }

            // Preserve any unsaved in-memory edits while adopting newly discovered/updated files on disk.
            TMap<FString, TSharedPtr<FV3DEditorDocument>> DirtyByPath;
            for (const auto& Existing : Self->Documents)
            {
                if (Existing.IsValid() && Existing->bDirty)
                {
                    DirtyByPath.Add(Existing->Path.ToLower(), Existing);
                }
            }
            bool bPreservedDirty = false;
            for (int32 Index = 0; Index < Found.Num(); ++Index)
            {
                if (!Found[Index].IsValid()) continue;
                if (const auto* DirtyDoc = DirtyByPath.Find(Found[Index]->Path.ToLower()))
                {
                    Found[Index] = *DirtyDoc;
                    bPreservedDirty = true;
                }
            }

            Self->InvalidatePreview();
            Self->Documents = MoveTemp(Found);
            Self->RefreshFiles();

            TSharedPtr<FV3DEditorDocument> NextSelection;
            if (!TargetSelection.IsEmpty())
            {
                for (const auto& Doc : Self->Documents)
                {
                    if (Doc.IsValid() && Doc->Path.Equals(TargetSelection, ESearchCase::IgnoreCase))
                    {
                        NextSelection = Doc;
                        break;
                    }
                }
            }
            if (!NextSelection.IsValid() && !Self->Documents.IsEmpty())
            {
                NextSelection = Self->Documents[0];
            }

            Self->Current = NextSelection;
            Self->bRawDirty = false;
            Self->RefreshForm();
            if (Self->List && Self->Current)
            {
                Self->List->SetSelection(Self->Current, ESelectInfo::Direct);
            }
            if (!CompletionMessage.IsEmpty())
            {
                Self->Message = CompletionMessage;
            }
            else
            {
                Self->Message = bPreservedDirty
                    ? TEXT("Project refreshed (unsaved edits preserved).")
                    : TEXT("Project files refreshed.");
            }
        });
    });
    if (!Queued)
    {
        bBusy = false;
        Message = TEXT("Editor refresh queue is unavailable.");
    }
}

void UProjectEditorWidget::OpenFileBrowser()
{
    if (bBusy || BuildStatus || Project.IsEmpty()) return;
    if (!ApplyRaw()) return;

    BrowserSelectedFiles.Reset();
    BrowserFilter.Reset();
    BrowserStatus.Reset();
    bShowFileBrowser = true;

    FString StartDir = BrowserCurrentDir;
    if (StartDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*StartDir))
    {
        StartDir = V3DFileBrowserUtils::GetUserHomeDir();
    }
    NavigateFileBrowser(StartDir);
}

void UProjectEditorWidget::CloseFileBrowser()
{
    bShowFileBrowser = false;
    BrowserSelectedFiles.Reset();
    BrowserStatus.Reset();
}

void UProjectEditorWidget::NavigateFileBrowser(const FString& TargetDir)
{
    FString Normalized = TargetDir;
    Normalized.TrimStartAndEndInline();
    FPaths::NormalizeDirectoryName(Normalized);
    if (Normalized.IsEmpty() || !IFileManager::Get().DirectoryExists(*Normalized))
    {
        BrowserStatus = TEXT("Directory does not exist: ") + TargetDir;
        return;
    }
    BrowserCurrentDir = Normalized;
    BrowserStatus.Reset();
    if (BrowserPathBox.IsValid())
    {
        BrowserPathBox->SetText(FText::FromString(BrowserCurrentDir));
    }
    RefreshFileBrowserEntries();
}

void UProjectEditorWidget::NavigateFileBrowserUp()
{
    if (BrowserCurrentDir.IsEmpty())
    {
        NavigateFileBrowser(V3DFileBrowserUtils::GetUserHomeDir());
        return;
    }
    FString Parent = FPaths::GetPath(BrowserCurrentDir);
    FPaths::NormalizeDirectoryName(Parent);
#if !PLATFORM_WINDOWS
    if (Parent.IsEmpty())
    {
        Parent = TEXT("/");
    }
#endif
    if (!Parent.IsEmpty() && IFileManager::Get().DirectoryExists(*Parent))
    {
        NavigateFileBrowser(Parent);
    }
}

void UProjectEditorWidget::RefreshFileBrowserEntries()
{
    BrowserEntries.Reset();
    if (BrowserCurrentDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*BrowserCurrentDir))
    {
        if (BrowserListView.IsValid()) BrowserListView->RequestListRefresh();
        return;
    }

    IFileManager& Files = IFileManager::Get();
    const FString SearchWildcard = FPaths::Combine(BrowserCurrentDir, TEXT("*"));

    TArray<FString> SubDirs;
    Files.FindFiles(SubDirs, *SearchWildcard, false, true);
    SubDirs.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::IgnoreCase) < 0; });

    for (const FString& DirName : SubDirs)
    {
        if (DirName.StartsWith(TEXT("."))) continue;
        if (!BrowserFilter.IsEmpty() && !DirName.Contains(BrowserFilter, ESearchCase::IgnoreCase)) continue;
        auto Entry = MakeShared<FV3DFileBrowserEntry>();
        Entry->DisplayName = DirName;
        Entry->FullPath = FPaths::Combine(BrowserCurrentDir, DirName);
        Entry->bIsDirectory = true;
        BrowserEntries.Add(MoveTemp(Entry));
    }

    TArray<FString> FoundFiles;
    Files.FindFiles(FoundFiles, *SearchWildcard, true, false);
    FoundFiles.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::IgnoreCase) < 0; });

    for (const FString& FileName : FoundFiles)
    {
        if (FileName.StartsWith(TEXT("."))) continue;
        const FString FullPath = FPaths::Combine(BrowserCurrentDir, FileName);
        if (!V3DSourceModelImport::IsModelSource(FullPath)) continue;
        if (!BrowserFilter.IsEmpty() && !FileName.Contains(BrowserFilter, ESearchCase::IgnoreCase)) continue;
        auto Entry = MakeShared<FV3DFileBrowserEntry>();
        Entry->DisplayName = FileName;
        Entry->FullPath = FullPath;
        Entry->FileSizeBytes = Files.FileSize(*FullPath);
        Entry->bIsDirectory = false;
        BrowserEntries.Add(MoveTemp(Entry));
    }

    if (BrowserListView.IsValid())
    {
        BrowserListView->RequestListRefresh();
    }
}

void UProjectEditorWidget::ConfirmFileBrowserImport()
{
    if (BrowserSelectedFiles.IsEmpty())
    {
        BrowserStatus = TEXT("Select one or more 3D asset files first.");
        return;
    }
    TArray<FString> Selected = BrowserSelectedFiles.Array();
    CloseFileBrowser();
    Import3DAssetFiles(MoveTemp(Selected));
}

void UProjectEditorWidget::Import3DAssetFiles(TArray<FString> SelectedFiles)
{
    if (bBusy || BuildStatus || Project.IsEmpty() || SelectedFiles.IsEmpty()) return;

    bBusy = true;
    Message = TEXT("Importing 3D asset(s)...");
    const FString ProjectName = Project;
    const uint64 Token = ++RequestGeneration;
    const TWeakObjectPtr<UProjectEditorWidget> Weak(this);
    const bool Queued = FSafeFileIO::RunTrackedWorker([Weak, Token, ProjectName, SelectedFiles = MoveTemp(SelectedFiles)]
    {
        const FString ResourcesDir = V3DSimulatorProjectWorkspace::ProjectResourcesRoot(ProjectName);
        IFileManager& Files = IFileManager::Get();
        Files.MakeDirectory(*ResourcesDir, true);

        int32 ImportedCount = 0;
        FString LastImportedJsonPath;
        TArray<FString> Skipped;

        for (const FString& SourceFile : SelectedFiles)
        {
            if (!Files.FileExists(*SourceFile) || !V3DSourceModelImport::IsModelSource(SourceFile))
            {
                Skipped.Add(FPaths::GetCleanFilename(SourceFile));
                continue;
            }

            const FString BaseName = FPaths::GetBaseFilename(SourceFile);
            const FString Ext = FPaths::GetExtension(SourceFile, true);
            FString DestPath = FPaths::Combine(ResourcesDir, BaseName + Ext);
            int32 Suffix = 1;
            while (Files.FileExists(*DestPath) && Suffix < 10000)
            {
                DestPath = FPaths::Combine(ResourcesDir, FString::Printf(TEXT("%s_%d%s"), *BaseName, Suffix++, *Ext));
            }

            if (Files.Copy(*DestPath, *SourceFile, false, true) == COPY_OK)
            {
                ++ImportedCount;
                LastImportedJsonPath = FPaths::ChangeExtension(DestPath, TEXT("json"));
            }
            else
            {
                Skipped.Add(FPaths::GetCleanFilename(SourceFile));
            }
        }

        FSafeFileIO::DispatchTrackedGameThread([Weak, Token, ImportedCount, LastImportedJsonPath, Skipped = MoveTemp(Skipped)]
        {
            auto* Self = Weak.Get();
            if (!Self || Self->RequestGeneration != Token) return;
            Self->bBusy = false;
            if (ImportedCount > 0)
            {
                const FString DoneMsg = Skipped.IsEmpty()
                    ? FString::Printf(TEXT("Imported %d 3D asset(s) into resources/."), ImportedCount)
                    : FString::Printf(TEXT("Imported %d 3D asset(s); skipped %d unsupported/failed file(s)."), ImportedCount, Skipped.Num());
                Self->RefreshProjectDocuments(LastImportedJsonPath, DoneMsg);
            }
            else
            {
                Self->Message = TEXT("No supported 3D assets were imported (.glb, .gltf, .fbx, .obj, .stl).");
            }
        });
    });
    if (!Queued)
    {
        bBusy = false;
        Message = TEXT("Import queue is unavailable.");
    }
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
    bRawDirty = false;
    if (Preview && Current)
    {
        if (PreviewDocument != Current->Path)
        {
            InvalidatePreview();
        }
        Preview->SetOverlaysFromJson(Current->Json);
    }
    QueueFormRefresh();
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
    if (Preview) { Preview->ClearModel(); Preview->ClearOverlays(); }
}

bool UProjectEditorWidget::ApplyRaw()
{
    if (!Current || !Raw || !bRawDirty) return true;
    FSafeJsonLimits Limits; Limits.MaxFileBytes = 16ll * 1024 * 1024; Limits.MaxDepth = 32;
    const auto Parsed = FSafeFileIO::ParseJsonText(RawDraft, Current->Path, Limits);
    if (!Parsed.IsSuccess()) { Message = Parsed.Error; return false; }
    Remember(); Current->Json = Parsed.JsonObject; Changed(); return true;
}


namespace V3DNodeDiscovery
{
    static bool IsInstancedDirectiveName(const FString& RawName, const FSimulatorParsedNodeName& Parsed)
    {
        if (Parsed.HasEffectiveToken(FName(TEXT("INST"))))
        {
            return true;
        }
        if (RawName.Contains(TEXT(";INST"), ESearchCase::IgnoreCase)
            || RawName.EndsWith(TEXT("_INST"), ESearchCase::IgnoreCase)
            || RawName.Contains(TEXT("_INST_"), ESearchCase::IgnoreCase)
            || RawName.Contains(TEXT(".INST"), ESearchCase::IgnoreCase))
        {
            return true;
        }
        return false;
    }

    static void TryCollectMeshName(const FString& RawName, TSet<FString>& OutNames)
    {
        const FString Trimmed = RawName.TrimStartAndEnd();
        if (Trimmed.IsEmpty())
        {
            return;
        }
        const FSimulatorParsedNodeName Parsed = USimulatorNodeTokenLibrary::ParseNodeName(Trimmed);
        if (IsInstancedDirectiveName(Trimmed, Parsed))
        {
            return;
        }
        const FString Base = Parsed.BaseName.TrimStartAndEnd();
        if (!Base.IsEmpty())
        {
            OutNames.Add(Base);
        }
    }

    static void ExtractFromGltfJsonText(const FString& JsonText, TSet<FString>& OutNames)
    {
        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            return;
        }

        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        if (Root->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
        {
            for (const TSharedPtr<FJsonValue>& Val : *Nodes)
            {
                if (!Val.IsValid() || Val->Type != EJson::Object) continue;
                const TSharedPtr<FJsonObject> NodeObj = Val->AsObject();
                if (!NodeObj.IsValid()) continue;
                double MeshIdx = -1.0;
                if (!NodeObj->TryGetNumberField(TEXT("mesh"), MeshIdx)) continue;
                FString NodeName;
                if (NodeObj->TryGetStringField(TEXT("name"), NodeName))
                {
                    TryCollectMeshName(NodeName, OutNames);
                }
            }
        }
    }

    static void ExtractModelNodeNames(const FString& SourcePath, TArray<FString>& OutSortedNames)
    {
        OutSortedNames.Reset();
        if (SourcePath.IsEmpty() || !FPaths::FileExists(SourcePath))
        {
            return;
        }

        TSet<FString> UniqueNames;
        const FString Ext = FPaths::GetExtension(SourcePath).ToLower();
        if (Ext == TEXT("glb"))
        {
            TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*SourcePath));
            if (Reader && Reader->TotalSize() >= 20)
            {
                uint32 Magic = 0, Version = 0, TotalLength = 0;
                uint32 ChunkLen = 0, ChunkType = 0;
                *Reader << Magic << Version << TotalLength << ChunkLen << ChunkType;
                constexpr uint32 GlbMagic = 0x46546C67u;
                constexpr uint32 JsonChunkType = 0x4E4F534Au;
                constexpr uint32 MaxJsonChunkBytes = 32u * 1024u * 1024u;
                if (Magic == GlbMagic && ChunkType == JsonChunkType && ChunkLen > 0 && ChunkLen <= MaxJsonChunkBytes
                    && static_cast<int64>(20 + ChunkLen) <= Reader->TotalSize())
                {
                    TArray<uint8> JsonBytes;
                    JsonBytes.SetNumUninitialized(static_cast<int32>(ChunkLen));
                    Reader->Serialize(JsonBytes.GetData(), ChunkLen);
                    FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(JsonBytes.GetData()), JsonBytes.Num());
                    ExtractFromGltfJsonText(FString(Conv.Length(), Conv.Get()), UniqueNames);
                }
            }
        }
        else if (Ext == TEXT("gltf"))
        {
            const int64 FileSize = IFileManager::Get().FileSize(*SourcePath);
            if (FileSize > 0 && FileSize <= 32ll * 1024 * 1024)
            {
                FString JsonText;
                if (FFileHelper::LoadFileToString(JsonText, *SourcePath))
                {
                    ExtractFromGltfJsonText(JsonText, UniqueNames);
                }
            }
        }
        else if (Ext == TEXT("obj"))
        {
            const int64 FileSize = IFileManager::Get().FileSize(*SourcePath);
            if (FileSize > 0 && FileSize <= 64ll * 1024 * 1024)
            {
                TArray<FString> Lines;
                if (FFileHelper::LoadFileToStringArray(Lines, *SourcePath))
                {
                    for (const FString& Line : Lines)
                    {
                        if (Line.StartsWith(TEXT("o ")) || Line.StartsWith(TEXT("g ")))
                        {
                            TryCollectMeshName(Line.Mid(2), UniqueNames);
                        }
                    }
                }
            }
        }

        OutSortedNames = UniqueNames.Array();
        OutSortedNames.Sort();
    }
}

void UProjectEditorWidget::EnsureModelNodeNamesLoaded(const FString& SourcePath)
{
    if (CachedNodeNamesSourcePath == SourcePath)
    {
        return;
    }
    CachedNodeNamesSourcePath = SourcePath;
    V3DNodeDiscovery::ExtractModelNodeNames(SourcePath, CachedModelNodeNames);
}

void UProjectEditorWidget::RebuildNodeSearchResults()
{
    using namespace V3DEditorUI;
    if (!NodeSearchResultsBox.IsValid() || !Current || !Current->Json)
    {
        return;
    }

    NodeSearchResultsBox->ClearChildren();
    const V3DHUDStyle::FAuthoringPalette Palette = GetThemePalette();
    const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush("WhiteBrush");

    const TSharedPtr<FJsonObject>* MeshDataPtr = nullptr;
    TSharedPtr<FJsonObject> MeshDataObj;
    if (Current->Json->TryGetObjectField(TEXT("MeshData"), MeshDataPtr) && MeshDataPtr && MeshDataPtr->IsValid())
    {
        MeshDataObj = *MeshDataPtr;
    }

    const FString Query = NodeSearchQuery.TrimStartAndEnd();
    TArray<FString> Matches;
    Matches.Reserve(CachedModelNodeNames.Num());
    for (const FString& Name : CachedModelNodeNames)
    {
        if (Query.IsEmpty() || Name.Contains(Query, ESearchCase::IgnoreCase))
        {
            Matches.Add(Name);
        }
    }

    const int32 TotalCount = CachedModelNodeNames.Num();
    const int32 MatchCount = Matches.Num();
    const int32 ShowLimit = FMath::Min(MatchCount, 60);

    NodeSearchResultsBox->AddSlot().AutoHeight().Padding(2, 2, 2, 6)
    [
        SNew(STextBlock)
        .TextStyle(&EditorTextStyle)
        .ColorAndOpacity(FSlateColor(Palette.Muted))
        .Text(Text(TotalCount > 0
            ? FString::Printf(TEXT("Nodes found: %d / %d (INST excluded)"), MatchCount, TotalCount)
            : TEXT("No embedded node names found (type a node name above and click + Add Custom Node).")))
    ];

    TWeakObjectPtr<UProjectEditorWidget> WeakSelf(this);
    for (int32 I = 0; I < ShowLimit; ++I)
    {
        const FString NodeName = Matches[I];
        const bool bConfigured = MeshDataObj.IsValid() && MeshDataObj->HasField(NodeName);

        FString CurDynType, CurVehType;
        Current->Json->TryGetStringField(TEXT("DynamicType"), CurDynType);
        Current->Json->TryGetStringField(TEXT("VehicleType"), CurVehType);
        const bool bIsVehicleDoc = CurDynType == TEXT("Vehicle");
        const FString TuningKey = (CurVehType == TEXT("Train")) ? TEXT("TrainTuning") : TEXT("VehicleTuning");

        bool bIsWheel = false;
        if (bIsVehicleDoc)
        {
            const TSharedPtr<FJsonObject>* TuneObjPtr = nullptr;
            if (Current->Json->TryGetObjectField(TuningKey, TuneObjPtr) && TuneObjPtr && TuneObjPtr->IsValid())
            {
                const TArray<TSharedPtr<FJsonValue>>* WheelArr = nullptr;
                if ((*TuneObjPtr)->TryGetArrayField(TEXT("WheelMeshNames"), WheelArr) && WheelArr)
                {
                    for (const TSharedPtr<FJsonValue>& WVal : *WheelArr)
                    {
                        if (WVal.IsValid() && WVal->Type == EJson::String
                            && WVal->AsString().Equals(NodeName, ESearchCase::IgnoreCase))
                        {
                            bIsWheel = true;
                            break;
                        }
                    }
                }
            }
        }

        FString NodeDisplay = NodeName;
        if (bIsWheel) NodeDisplay += TEXT("  [Wheel]");
        if (bConfigured) NodeDisplay += TEXT("  [Configured]");

        auto RowHBox = SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .TextStyle(&EditorTextStyle)
                .ColorAndOpacity(FSlateColor((bConfigured || bIsWheel) ? Palette.Text : Palette.Muted))
                .Text(Text(NodeDisplay))
            ];

        if (bIsVehicleDoc)
        {
            RowHBox->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
            [
                MakeEditorButton(bIsWheel ? TEXT("- Wheel") : TEXT("+ Wheel"), [WeakSelf, NodeName, bIsWheel, TuningKey]
                {
                    UProjectEditorWidget* Self = WeakSelf.Get();
                    if (!Self || Self->bBusy || !Self->Current || !Self->Current->Json) return;
                    if (Self->bRawDirty) { Self->Message = TEXT("Apply JSON first."); return; }
                    Self->Remember();
                    const TSharedPtr<FJsonObject>* TunePtr = nullptr;
                    TSharedPtr<FJsonObject> TuneObj;
                    if (Self->Current->Json->TryGetObjectField(TuningKey, TunePtr) && TunePtr && TunePtr->IsValid())
                    {
                        TuneObj = *TunePtr;
                    }
                    else
                    {
                        TuneObj = MakeShared<FJsonObject>();
                        Self->Current->Json->SetObjectField(TuningKey, TuneObj);
                    }
                    TArray<TSharedPtr<FJsonValue>> UpdatedWheels;
                    const TArray<TSharedPtr<FJsonValue>>* ExistingWheels = nullptr;
                    if (TuneObj->TryGetArrayField(TEXT("WheelMeshNames"), ExistingWheels) && ExistingWheels)
                    {
                        for (const TSharedPtr<FJsonValue>& Val : *ExistingWheels)
                        {
                            if (!Val.IsValid() || Val->Type != EJson::String) continue;
                            if (bIsWheel && Val->AsString().Equals(NodeName, ESearchCase::IgnoreCase)) continue;
                            UpdatedWheels.Add(Val);
                        }
                    }
                    if (!bIsWheel)
                    {
                        UpdatedWheels.Add(MakeShared<FJsonValueString>(NodeName));
                    }
                    TuneObj->SetArrayField(TEXT("WheelMeshNames"), UpdatedWheels);
                    Self->Message = bIsWheel
                        ? FString::Printf(TEXT("Removed wheel '%s' from %s.WheelMeshNames"), *NodeName, *TuningKey)
                        : FString::Printf(TEXT("Added wheel '%s' to %s.WheelMeshNames"), *NodeName, *TuningKey);
                    Self->Changed();
                })
            ];
        }

        RowHBox->AddSlot().AutoWidth().VAlign(VAlign_Center)
        [
            MakeEditorButton(bConfigured ? TEXT("Remove") : TEXT("+ Add Settings"), [WeakSelf, NodeName, bConfigured]
                        {
                            UProjectEditorWidget* Self = WeakSelf.Get();
                            if (!Self || Self->bBusy || !Self->Current || !Self->Current->Json) return;
                            if (Self->bRawDirty) { Self->Message = TEXT("Apply JSON first."); return; }
                            Self->Remember();
                            const TSharedPtr<FJsonObject>* ExistingMap = nullptr;
                            TSharedPtr<FJsonObject> MapObj;
                            if (Self->Current->Json->TryGetObjectField(TEXT("MeshData"), ExistingMap) && ExistingMap && ExistingMap->IsValid())
                            {
                                MapObj = *ExistingMap;
                            }
                            else
                            {
                                MapObj = MakeShared<FJsonObject>();
                                Self->Current->Json->SetObjectField(TEXT("MeshData"), MapObj);
                            }
                            if (bConfigured)
                            {
                                MapObj->RemoveField(NodeName);
                                Self->Message = TEXT("Removed node settings: ") + NodeName;
                            }
                            else
                            {
                                MapObj->SetObjectField(NodeName, V3DProjectEditor::EntryTemplate(TEXT("MeshData")));
                                Self->Message = TEXT("Added node settings (Collision/Lights): ") + NodeName;
                            }
                            Self->Changed();
                        })
        ];

        NodeSearchResultsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
        [
            SNew(SBorder)
            .BorderImage(WhiteBrush)
            .BorderBackgroundColor(CardBorderColor(bDarkMode, !(bConfigured || bIsWheel)))
            .Padding(1.f)
            [
                SNew(SBorder)
                .BorderImage(WhiteBrush)
                .BorderBackgroundColor(CardFillColor(Palette, bDarkMode, I, !(bConfigured || bIsWheel)))
                .Padding(FMargin(8.f, 4.f))
                [
                    RowHBox
                ]
            ]
        ];
    }

    if (MatchCount > ShowLimit)
    {
        NodeSearchResultsBox->AddSlot().AutoHeight().Padding(2, 4, 2, 2)
        [
            SNew(STextBlock)
            .TextStyle(&EditorTextStyle)
            .ColorAndOpacity(FSlateColor(Palette.Muted))
            .Text(Text(FString::Printf(TEXT("... %d more matches. Refine search to narrow results."), MatchCount - ShowLimit)))
        ];
    }
}

void UProjectEditorWidget::AddNodeSettingsSection(const TSharedRef<SVerticalBox>& Box)
{
    using namespace V3DEditorUI;
    if (!Current || !Current->Json || Current->SourcePath.IsEmpty())
    {
        return;
    }

    FString AssetType;
    Current->Json->TryGetStringField(TEXT("AssetType"), AssetType);
    const FString Ext = FPaths::GetExtension(Current->SourcePath).ToLower();
    const bool bIsModelFile = Ext == TEXT("glb") || Ext == TEXT("gltf") || Ext == TEXT("fbx") || Ext == TEXT("obj") || Ext == TEXT("stl");
    if (!bIsModelFile || (!AssetType.IsEmpty() && AssetType != TEXT("Model")))
    {
        return;
    }

    EnsureModelNodeNamesLoaded(Current->SourcePath);

    const V3DHUDStyle::FAuthoringPalette Palette = GetThemePalette();
    const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush("WhiteBrush");
    TWeakObjectPtr<UProjectEditorWidget> WeakSelf(this);

    auto SearchBox = SNew(SEditableTextBox)
        .Style(&EditorInputStyle)
        .Text(Text(NodeSearchQuery))
        .HintText(Text(TEXT("Search node name for MeshData (INST excluded)...")))
        .OnTextChanged_Lambda([WeakSelf](const FText& NewText)
        {
            if (UProjectEditorWidget* Self = WeakSelf.Get())
            {
                Self->NodeSearchQuery = NewText.ToString();
                Self->RebuildNodeSearchResults();
            }
        });

    NodeSearchResultsBox = SNew(SVerticalBox);
    RebuildNodeSearchResults();

    auto SectionBody = SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
        [
            SNew(STextBlock)
            .TextStyle(&EditorTextStyle)
            .ColorAndOpacity(FSlateColor(Palette.Text))
            .Text(Text(TEXT("Node Settings (Collision / Colliders / Lights) — MeshData")))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 6, 0)[SearchBox]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
            [
                MakeEditorButton(TEXT("+ Add Custom Node"), [WeakSelf]
                {
                    UProjectEditorWidget* Self = WeakSelf.Get();
                    if (!Self || Self->bBusy || !Self->Current || !Self->Current->Json) return;
                    if (Self->bRawDirty) { Self->Message = TEXT("Apply JSON first."); return; }
                    const FString RawName = Self->NodeSearchQuery.TrimStartAndEnd();
                    const FSimulatorParsedNodeName Parsed = USimulatorNodeTokenLibrary::ParseNodeName(RawName);
                    if (RawName.IsEmpty() || V3DNodeDiscovery::IsInstancedDirectiveName(RawName, Parsed))
                    {
                        Self->Message = TEXT("Enter a non-empty, non-INST node/mesh name.");
                        return;
                    }
                    const FString CleanName = Parsed.BaseName.TrimStartAndEnd();
                    if (CleanName.IsEmpty()) return;
                    Self->Remember();
                    const TSharedPtr<FJsonObject>* ExistingMap = nullptr;
                    TSharedPtr<FJsonObject> MapObj;
                    if (Self->Current->Json->TryGetObjectField(TEXT("MeshData"), ExistingMap) && ExistingMap && ExistingMap->IsValid())
                    {
                        MapObj = *ExistingMap;
                    }
                    else
                    {
                        MapObj = MakeShared<FJsonObject>();
                        Self->Current->Json->SetObjectField(TEXT("MeshData"), MapObj);
                    }
                    if (!MapObj->HasField(CleanName))
                    {
                        MapObj->SetObjectField(CleanName, V3DProjectEditor::EntryTemplate(TEXT("MeshData")));
                        Self->Message = TEXT("Added node settings: ") + CleanName;
                        Self->Changed();
                    }
                    else
                    {
                        Self->Message = TEXT("Node settings already exist for: ") + CleanName;
                    }
                })
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                MakeEditorButton(TEXT("Rescan"), [WeakSelf]
                {
                    if (UProjectEditorWidget* Self = WeakSelf.Get())
                    {
                        Self->CachedNodeNamesSourcePath.Reset();
                        if (Self->Current) Self->EnsureModelNodeNamesLoaded(Self->Current->SourcePath);
                        Self->RebuildNodeSearchResults();
                    }
                })
            ]
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SBox).MaxDesiredHeight(180.f)
            [
                SNew(SScrollBox)
                + SScrollBox::Slot()[NodeSearchResultsBox.ToSharedRef()]
            ]
        ];

    Box->AddSlot().AutoHeight().Padding(0, 0, 0, 10)
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
                SectionBody
            ]
        ]
    ];
}

struct FV3DMaterialEntry
{
    FString KeyName;
    FString TextureName;
    FString DisplayLabel;
    FString SearchText;
    bool bIsTerrainLayer = false;
    bool bIsFoliageDefault = false;
};

namespace V3DMaterialDiscovery
{
    static void RegisterMaterialName(
        const FString& RawName,
        const FString& TextureHint,
        TMap<FString, TSharedPtr<FV3DMaterialEntry>>& OutMap)
    {
        const FString Trimmed = RawName.TrimStartAndEnd();
        if (Trimmed.IsEmpty()) return;
        if (Trimmed.Equals(TEXT("V3D.TerrainPalette"), ESearchCase::IgnoreCase)
            || Trimmed.Equals(TEXT("V3D.Impostor"), ESearchCase::IgnoreCase))
        {
            return;
        }

        const FString Normalized = FV3DMaterialAssetReferences::NormalizeMaterialName(Trimmed);

        // Reserved foliage materials are treated as a single default material entry ("foliage").
        if (Normalized == TEXT("foliage") || Normalized.EndsWith(TEXT(".foliage")))
        {
            const FString DedupKey = TEXT("foliage");
            if (!OutMap.Contains(DedupKey))
            {
                auto Entry = MakeShared<FV3DMaterialEntry>();
                Entry->KeyName = TEXT("foliage");
                Entry->DisplayLabel = TEXT("foliage  [Foliage (Default)]");
                Entry->SearchText = TEXT("foliage default");
                Entry->bIsFoliageDefault = true;
                OutMap.Add(DedupKey, MoveTemp(Entry));
            }
            return;
        }

        // Reserved terrain materials (*.terrain) are listed individually per terrain texture/layer.
        if (V3DTerrain::IsTerrainName(Trimmed))
        {
            const FString DedupKey = Trimmed.ToLower();
            TSharedPtr<FV3DMaterialEntry>* Existing = OutMap.Find(DedupKey);
            if (Existing && Existing->IsValid())
            {
                if ((*Existing)->TextureName.IsEmpty() && !TextureHint.IsEmpty())
                {
                    (*Existing)->TextureName = TextureHint;
                    (*Existing)->DisplayLabel = FString::Printf(TEXT("%s  [Terrain Texture: %s]"), *Trimmed, *TextureHint);
                    (*Existing)->SearchText = Trimmed + TEXT(" ") + TextureHint + TEXT(" terrain texture");
                }
                return;
            }

            auto Entry = MakeShared<FV3DMaterialEntry>();
            Entry->KeyName = Trimmed;
            Entry->TextureName = TextureHint;
            Entry->bIsTerrainLayer = true;
            Entry->DisplayLabel = TextureHint.IsEmpty()
                ? (Trimmed + TEXT("  [Terrain Texture]"))
                : FString::Printf(TEXT("%s  [Terrain Texture: %s]"), *Trimmed, *TextureHint);
            Entry->SearchText = Trimmed + TEXT(" ") + TextureHint + TEXT(" terrain texture");
            OutMap.Add(DedupKey, MoveTemp(Entry));
            return;
        }

        // Regular material
        const FString DedupKey = Trimmed.ToLower();
        if (!OutMap.Contains(DedupKey))
        {
            auto Entry = MakeShared<FV3DMaterialEntry>();
            Entry->KeyName = Trimmed;
            Entry->TextureName = TextureHint;
            Entry->DisplayLabel = Trimmed;
            Entry->SearchText = Trimmed + (TextureHint.IsEmpty() ? FString() : (TEXT(" ") + TextureHint));
            OutMap.Add(DedupKey, MoveTemp(Entry));
        }
    }

    static FString ResolveBaseColorTextureName(
        const TSharedPtr<FJsonObject>& MatObj,
        const TArray<TSharedPtr<FJsonValue>>* TexturesArr,
        const TArray<TSharedPtr<FJsonValue>>* ImagesArr)
    {
        if (!MatObj.IsValid() || !TexturesArr || !ImagesArr) return FString();
        const TSharedPtr<FJsonObject>* PbrObj = nullptr;
        if (!MatObj->TryGetObjectField(TEXT("pbrMetallicRoughness"), PbrObj) || !PbrObj || !PbrObj->IsValid()) return FString();
        const TSharedPtr<FJsonObject>* BaseTexObj = nullptr;
        if (!(*PbrObj)->TryGetObjectField(TEXT("baseColorTexture"), BaseTexObj) || !BaseTexObj || !BaseTexObj->IsValid()) return FString();

        double TexIdxNum = -1.0;
        if (!(*BaseTexObj)->TryGetNumberField(TEXT("index"), TexIdxNum)) return FString();
        const int32 TexIdx = FMath::FloorToInt(TexIdxNum);
        if (!TexturesArr->IsValidIndex(TexIdx) || !(*TexturesArr)[TexIdx].IsValid() || (*TexturesArr)[TexIdx]->Type != EJson::Object) return FString();

        const TSharedPtr<FJsonObject> TexObj = (*TexturesArr)[TexIdx]->AsObject();
        double SrcIdxNum = -1.0;
        if (!TexObj.IsValid() || !TexObj->TryGetNumberField(TEXT("source"), SrcIdxNum)) return FString();
        const int32 ImgIdx = FMath::FloorToInt(SrcIdxNum);
        if (!ImagesArr->IsValidIndex(ImgIdx) || !(*ImagesArr)[ImgIdx].IsValid() || (*ImagesArr)[ImgIdx]->Type != EJson::Object) return FString();

        const TSharedPtr<FJsonObject> ImgObj = (*ImagesArr)[ImgIdx]->AsObject();
        if (!ImgObj.IsValid()) return FString();

        FString ImgName;
        if (ImgObj->TryGetStringField(TEXT("name"), ImgName) && !ImgName.TrimStartAndEnd().IsEmpty())
        {
            return ImgName.TrimStartAndEnd();
        }
        FString ImgUri;
        if (ImgObj->TryGetStringField(TEXT("uri"), ImgUri) && !ImgUri.StartsWith(TEXT("data:"), ESearchCase::IgnoreCase))
        {
            return FPaths::GetBaseFilename(ImgUri);
        }
        return FString();
    }

    static void ExtractMaterialsFromGltfJsonText(
        const FString& JsonText,
        TMap<FString, TSharedPtr<FV3DMaterialEntry>>& OutMap)
    {
        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return;

        const TArray<TSharedPtr<FJsonValue>>* MaterialsArr = nullptr;
        if (!Root->TryGetArrayField(TEXT("materials"), MaterialsArr) || !MaterialsArr) return;

        const TArray<TSharedPtr<FJsonValue>>* TexturesArr = nullptr;
        Root->TryGetArrayField(TEXT("textures"), TexturesArr);
        const TArray<TSharedPtr<FJsonValue>>* ImagesArr = nullptr;
        Root->TryGetArrayField(TEXT("images"), ImagesArr);

        for (const TSharedPtr<FJsonValue>& Val : *MaterialsArr)
        {
            if (!Val.IsValid() || Val->Type != EJson::Object) continue;
            const TSharedPtr<FJsonObject> MatObj = Val->AsObject();
            if (!MatObj.IsValid()) continue;
            FString MatName;
            if (!MatObj->TryGetStringField(TEXT("name"), MatName)) continue;
            const FString TexName = ResolveBaseColorTextureName(MatObj, TexturesArr, ImagesArr);
            RegisterMaterialName(MatName, TexName, OutMap);
        }
    }

    static void CollectMaterialsFromSourceFile(
        const FString& SourcePath,
        TMap<FString, TSharedPtr<FV3DMaterialEntry>>& OutMap)
    {
        if (SourcePath.IsEmpty() || !FPaths::FileExists(SourcePath)) return;
        const FString Ext = FPaths::GetExtension(SourcePath).ToLower();

        if (Ext == TEXT("glb"))
        {
            TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*SourcePath));
            if (Reader && Reader->TotalSize() >= 20)
            {
                uint32 Magic = 0, Version = 0, TotalLength = 0;
                uint32 ChunkLen = 0, ChunkType = 0;
                *Reader << Magic << Version << TotalLength << ChunkLen << ChunkType;
                constexpr uint32 GlbMagic = 0x46546C67u;
                constexpr uint32 JsonChunkType = 0x4E4F534Au;
                constexpr uint32 MaxJsonChunkBytes = 32u * 1024u * 1024u;
                if (Magic == GlbMagic && ChunkType == JsonChunkType && ChunkLen > 0 && ChunkLen <= MaxJsonChunkBytes
                    && static_cast<int64>(20 + ChunkLen) <= Reader->TotalSize())
                {
                    TArray<uint8> JsonBytes;
                    JsonBytes.SetNumUninitialized(static_cast<int32>(ChunkLen));
                    Reader->Serialize(JsonBytes.GetData(), ChunkLen);
                    FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(JsonBytes.GetData()), JsonBytes.Num());
                    ExtractMaterialsFromGltfJsonText(FString(Conv.Length(), Conv.Get()), OutMap);
                }
            }
        }
        else if (Ext == TEXT("gltf"))
        {
            const int64 FileSize = IFileManager::Get().FileSize(*SourcePath);
            if (FileSize > 0 && FileSize <= 32ll * 1024 * 1024)
            {
                FString JsonText;
                if (FFileHelper::LoadFileToString(JsonText, *SourcePath))
                {
                    ExtractMaterialsFromGltfJsonText(JsonText, OutMap);
                }
            }
        }
        else if (Ext == TEXT("obj"))
        {
            const int64 FileSize = IFileManager::Get().FileSize(*SourcePath);
            if (FileSize > 0 && FileSize <= 64ll * 1024 * 1024)
            {
                TArray<FString> Lines;
                if (FFileHelper::LoadFileToStringArray(Lines, *SourcePath))
                {
                    for (const FString& Line : Lines)
                    {
                        if (Line.StartsWith(TEXT("usemtl ")))
                        {
                            RegisterMaterialName(Line.Mid(7), FString(), OutMap);
                        }
                    }
                }
            }
        }
    }

    static FString FindMappedSurface(const TSharedPtr<FJsonObject>& SoundsJson, const FString& MaterialKey)
    {
        if (!SoundsJson.IsValid()) return FString();
        const TArray<TSharedPtr<FJsonValue>>* Mappings = nullptr;
        if (!SoundsJson->TryGetArrayField(TEXT("MaterialPhysicalMappings"), Mappings) || !Mappings) return FString();
        for (const TSharedPtr<FJsonValue>& EntryVal : *Mappings)
        {
            if (!EntryVal.IsValid() || EntryVal->Type != EJson::Object) continue;
            const TSharedPtr<FJsonObject> Obj = EntryVal->AsObject();
            FString MatStr, SurfStr;
            if (Obj.IsValid() && Obj->TryGetStringField(TEXT("Material"), MatStr)
                && MatStr.Equals(MaterialKey, ESearchCase::IgnoreCase))
            {
                Obj->TryGetStringField(TEXT("PhysicalMaterial"), SurfStr);
                return SurfStr;
            }
        }
        return FString();
    }

    static bool SetOrRemoveMaterialMapping(
        const TSharedPtr<FJsonObject>& SoundsJson,
        const FString& MaterialKey,
        const FString& SurfaceName,
        const bool bRemove)
    {
        if (!SoundsJson.IsValid() || MaterialKey.IsEmpty()) return false;
        TArray<TSharedPtr<FJsonValue>> Updated;
        const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
        if (SoundsJson->TryGetArrayField(TEXT("MaterialPhysicalMappings"), Existing) && Existing)
        {
            Updated = *Existing;
        }

        bool bFound = false;
        for (int32 Idx = Updated.Num() - 1; Idx >= 0; --Idx)
        {
            if (!Updated[Idx].IsValid() || Updated[Idx]->Type != EJson::Object) continue;
            const TSharedPtr<FJsonObject> Obj = Updated[Idx]->AsObject();
            FString MatStr;
            if (Obj.IsValid() && Obj->TryGetStringField(TEXT("Material"), MatStr)
                && MatStr.Equals(MaterialKey, ESearchCase::IgnoreCase))
            {
                bFound = true;
                if (bRemove)
                {
                    Updated.RemoveAt(Idx);
                }
                else
                {
                    Obj->SetStringField(TEXT("PhysicalMaterial"), SurfaceName);
                }
            }
        }

        if (!bRemove && !bFound)
        {
            auto NewEntry = MakeShared<FJsonObject>();
            NewEntry->SetStringField(TEXT("Material"), MaterialKey);
            NewEntry->SetStringField(TEXT("PhysicalMaterial"), SurfaceName.IsEmpty() ? TEXT("SurfaceName") : SurfaceName);
            Updated.Add(MakeShared<FJsonValueObject>(NewEntry));
        }

        SoundsJson->SetArrayField(TEXT("MaterialPhysicalMappings"), Updated);
        return true;
    }
}

TSharedPtr<FV3DEditorDocument> UProjectEditorWidget::FindSoundsDocument() const
{
    for (const TSharedPtr<FV3DEditorDocument>& Doc : Documents)
    {
        if (Doc.IsValid() && Doc->Path.EndsWith(TEXT("scripts/sounds.json"), ESearchCase::IgnoreCase))
        {
            return Doc;
        }
    }
    return nullptr;
}

void UProjectEditorWidget::EnsureMaterialsLoaded(const bool bProjectWide, const FString& SourcePath)
{
    const FString ScopeKey = bProjectWide
        ? FString::Printf(TEXT("PROJECT:%s:%d"), *Project, Documents.Num())
        : (TEXT("MODEL:") + SourcePath);
    if (CachedMaterialsScopeKey == ScopeKey)
    {
        return;
    }
    CachedMaterialsScopeKey = ScopeKey;
    CachedMaterials.Reset();

    TMap<FString, TSharedPtr<FV3DMaterialEntry>> Collected;
    if (bProjectWide)
    {
        for (const TSharedPtr<FV3DEditorDocument>& Doc : Documents)
        {
            if (!Doc.IsValid() || Doc->SourcePath.IsEmpty()) continue;
            V3DMaterialDiscovery::CollectMaterialsFromSourceFile(Doc->SourcePath, Collected);
        }
    }
    else
    {
        V3DMaterialDiscovery::CollectMaterialsFromSourceFile(SourcePath, Collected);
    }

    Collected.GenerateValueArray(CachedMaterials);
    CachedMaterials.Sort([](const TSharedPtr<FV3DMaterialEntry>& A, const TSharedPtr<FV3DMaterialEntry>& B)
    {
        if (A->bIsTerrainLayer != B->bIsTerrainLayer) return A->bIsTerrainLayer;
        if (A->bIsFoliageDefault != B->bIsFoliageDefault) return A->bIsFoliageDefault;
        return A->KeyName.Compare(B->KeyName, ESearchCase::IgnoreCase) < 0;
    });
}

void UProjectEditorWidget::RebuildMaterialSearchResults(const bool bProjectWide)
{
    using namespace V3DEditorUI;
    if (!MaterialSearchResultsBox.IsValid() || !Current || !Current->Json) return;

    MaterialSearchResultsBox->ClearChildren();
    const V3DHUDStyle::FAuthoringPalette Palette = GetThemePalette();
    const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush("WhiteBrush");

    const TSharedPtr<FV3DEditorDocument> SoundsDoc = bProjectWide ? Current : FindSoundsDocument();
    const TSharedPtr<FJsonObject> SoundsJson = SoundsDoc.IsValid() ? SoundsDoc->Json : nullptr;

    const FString Query = MaterialSearchQuery.TrimStartAndEnd();
    TArray<TSharedPtr<FV3DMaterialEntry>> Matches;
    Matches.Reserve(CachedMaterials.Num());
    for (const TSharedPtr<FV3DMaterialEntry>& Entry : CachedMaterials)
    {
        if (!Entry.IsValid()) continue;
        if (Query.IsEmpty() || Entry->SearchText.Contains(Query, ESearchCase::IgnoreCase)
            || Entry->KeyName.Contains(Query, ESearchCase::IgnoreCase))
        {
            Matches.Add(Entry);
        }
    }

    const int32 TotalCount = CachedMaterials.Num();
    const int32 MatchCount = Matches.Num();
    const int32 ShowLimit = FMath::Min(MatchCount, 60);

    MaterialSearchResultsBox->AddSlot().AutoHeight().Padding(2, 2, 2, 6)
    [
        SNew(STextBlock)
        .TextStyle(&EditorTextStyle)
        .ColorAndOpacity(FSlateColor(Palette.Muted))
        .Text(Text(TotalCount > 0
            ? FString::Printf(TEXT("Materials found: %d / %d (Terrain: per texture, Foliage: default)"), MatchCount, TotalCount)
            : TEXT("No embedded materials found in scanned 3D models.")))
    ];

    TWeakObjectPtr<UProjectEditorWidget> WeakSelf(this);
    for (int32 Idx = 0; Idx < ShowLimit; ++Idx)
    {
        const TSharedPtr<FV3DMaterialEntry>& Entry = Matches[Idx];
        const FString MatKey = Entry->KeyName;
        const FString MappedSurface = V3DMaterialDiscovery::FindMappedSurface(SoundsJson, MatKey);
        const bool bMapped = !MappedSurface.IsEmpty();

        const FString RowLabel = bMapped
            ? FString::Printf(TEXT("%s  [-> %s]"), *Entry->DisplayLabel, *MappedSurface)
            : Entry->DisplayLabel;

        MaterialSearchResultsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
        [
            SNew(SBorder)
            .BorderImage(WhiteBrush)
            .BorderBackgroundColor(CardBorderColor(bDarkMode, !bMapped))
            .Padding(1.f)
            [
                SNew(SBorder)
                .BorderImage(WhiteBrush)
                .BorderBackgroundColor(CardFillColor(Palette, bDarkMode, Idx, !bMapped))
                .Padding(FMargin(8.f, 4.f))
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .TextStyle(&EditorTextStyle)
                        .ColorAndOpacity(FSlateColor(bMapped ? Palette.Text : Palette.Muted))
                        .Text(Text(RowLabel))
                    ]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [
                        MakeEditorButton(
                            bMapped ? TEXT("Remove") : (bProjectWide ? TEXT("+ Map") : TEXT("+ Map in sounds.json")),
                            [WeakSelf, MatKey, bMapped, bProjectWide]
                            {
                                UProjectEditorWidget* Self = WeakSelf.Get();
                                if (!Self || Self->bBusy || !Self->Current) return;
                                if (Self->bRawDirty) { Self->Message = TEXT("Apply JSON first."); return; }
                                TSharedPtr<FV3DEditorDocument> TargetDoc = bProjectWide ? Self->Current : Self->FindSoundsDocument();
                                if (!TargetDoc.IsValid() || !TargetDoc->Json.IsValid())
                                {
                                    Self->Message = TEXT("scripts/sounds.json was not found in this project.");
                                    return;
                                }
                                if (TargetDoc == Self->Current)
                                {
                                    Self->Remember();
                                }
                                const FString SurfaceToAssign = Self->MaterialTargetSurface.TrimStartAndEnd().IsEmpty()
                                    ? TEXT("SurfaceName") : Self->MaterialTargetSurface.TrimStartAndEnd();
                                V3DMaterialDiscovery::SetOrRemoveMaterialMapping(TargetDoc->Json, MatKey, SurfaceToAssign, bMapped);
                                TargetDoc->bDirty = true;
                                Self->Message = bMapped
                                    ? (TEXT("Removed material mapping: ") + MatKey)
                                    : FString::Printf(TEXT("Mapped '%s' -> '%s' in scripts/sounds.json"), *MatKey, *SurfaceToAssign);
                                if (TargetDoc == Self->Current)
                                {
                                    Self->Changed();
                                }
                                else
                                {
                                    Self->RebuildMaterialSearchResults(bProjectWide);
                                }
                            })
                    ]
                ]
            ]
        ];
    }

    if (MatchCount > ShowLimit)
    {
        MaterialSearchResultsBox->AddSlot().AutoHeight().Padding(2, 4, 2, 2)
        [
            SNew(STextBlock)
            .TextStyle(&EditorTextStyle)
            .ColorAndOpacity(FSlateColor(Palette.Muted))
            .Text(Text(FString::Printf(TEXT("... %d more matches. Refine search to narrow results."), MatchCount - ShowLimit)))
        ];
    }
}

void UProjectEditorWidget::AddMaterialSearchSection(const TSharedRef<SVerticalBox>& Box)
{
    using namespace V3DEditorUI;
    if (!Current || !Current->Json) return;

    const bool bIsSoundsScript = Current->Path.EndsWith(TEXT("scripts/sounds.json"), ESearchCase::IgnoreCase);
    bool bIsModelDoc = false;
    if (!bIsSoundsScript && !Current->SourcePath.IsEmpty())
    {
        FString AssetType;
        Current->Json->TryGetStringField(TEXT("AssetType"), AssetType);
        const FString Ext = FPaths::GetExtension(Current->SourcePath).ToLower();
        const bool bModelExt = Ext == TEXT("glb") || Ext == TEXT("gltf") || Ext == TEXT("obj");
        bIsModelDoc = bModelExt && (AssetType.IsEmpty() || AssetType == TEXT("Model"));
    }

    if (!bIsSoundsScript && !bIsModelDoc) return;

    EnsureMaterialsLoaded(bIsSoundsScript, Current->SourcePath);

    // Collect available PhysicalMaterials surface names from scripts/sounds.json for target selection
    TSharedPtr<FV3DEditorDocument> SoundsDoc = bIsSoundsScript ? Current : FindSoundsDocument();
    CachedSurfaceOptions.Reset();
    if (SoundsDoc.IsValid() && SoundsDoc->Json.IsValid())
    {
        const TSharedPtr<FJsonObject>* PhysMap = nullptr;
        if (SoundsDoc->Json->TryGetObjectField(TEXT("PhysicalMaterials"), PhysMap) && PhysMap && PhysMap->IsValid())
        {
            TArray<FString> SortedSurfaces;
            for (const auto& Pair : (*PhysMap)->Values)
            {
                SortedSurfaces.Add(FString(Pair.Key));
            }
            SortedSurfaces.Sort();
            for (const FString& SurfName : SortedSurfaces)
            {
                CachedSurfaceOptions.Add(MakeShared<FString>(SurfName));
            }
        }
    }
    if (MaterialTargetSurface.IsEmpty() && !CachedSurfaceOptions.IsEmpty())
    {
        MaterialTargetSurface = *CachedSurfaceOptions[0];
    }
    else if (MaterialTargetSurface.IsEmpty())
    {
        MaterialTargetSurface = TEXT("SurfaceName");
    }

    const V3DHUDStyle::FAuthoringPalette Palette = GetThemePalette();
    const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush("WhiteBrush");
    TWeakObjectPtr<UProjectEditorWidget> WeakSelf(this);

    auto MatSearchBox = SNew(SEditableTextBox)
        .Style(&EditorInputStyle)
        .Text(Text(MaterialSearchQuery))
        .HintText(Text(TEXT("Search material or terrain texture name...")))
        .OnTextChanged_Lambda([WeakSelf, bIsSoundsScript](const FText& NewText)
        {
            if (UProjectEditorWidget* Self = WeakSelf.Get())
            {
                Self->MaterialSearchQuery = NewText.ToString();
                Self->RebuildMaterialSearchResults(bIsSoundsScript);
            }
        });

    auto SurfaceInput = SNew(SEditableTextBox)
        .Style(&EditorInputStyle)
        .Text(Text(MaterialTargetSurface))
        .HintText(Text(TEXT("Target PhysicalMaterial...")))
        .OnTextChanged_Lambda([WeakSelf](const FText& NewText)
        {
            if (UProjectEditorWidget* Self = WeakSelf.Get())
            {
                Self->MaterialTargetSurface = NewText.ToString();
            }
        });

    MaterialSearchResultsBox = SNew(SVerticalBox);
    RebuildMaterialSearchResults(bIsSoundsScript);

    auto SurfaceRow = SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
        [
            SNew(STextBlock)
            .TextStyle(&EditorTextStyle)
            .ColorAndOpacity(FSlateColor(Palette.Muted))
            .Text(Text(TEXT("Target Surface:")))
        ]
        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 6, 0)[SurfaceInput];

    if (!CachedSurfaceOptions.IsEmpty())
    {
        TSharedPtr<FString> SelectedOpt = CachedSurfaceOptions[0];
        for (const TSharedPtr<FString>& Opt : CachedSurfaceOptions)
        {
            if (Opt.IsValid() && Opt->Equals(MaterialTargetSurface, ESearchCase::IgnoreCase))
            {
                SelectedOpt = Opt;
                break;
            }
        }
        SurfaceRow->AddSlot().AutoWidth().VAlign(VAlign_Center)
        [
            SNew(SComboBox<TSharedPtr<FString>>)
            .ComboBoxStyle(&EditorComboStyle)
            .OptionsSource(&CachedSurfaceOptions)
            .InitiallySelectedItem(SelectedOpt)
            .OnGenerateWidget_Lambda([this](TSharedPtr<FString> Opt)
            {
                return SNew(STextBlock).TextStyle(&EditorTextStyle).Text(Text(Opt.IsValid() ? *Opt : FString()));
            })
            .OnSelectionChanged_Lambda([WeakSelf, SurfaceInput](TSharedPtr<FString> Opt, ESelectInfo::Type)
            {
                if (UProjectEditorWidget* Self = WeakSelf.Get(); Self && Opt.IsValid())
                {
                    Self->MaterialTargetSurface = *Opt;
                    SurfaceInput->SetText(Text(*Opt));
                }
            })
            [
                SNew(STextBlock).TextStyle(&EditorTextStyle)
                .Text_Lambda([WeakSelf]()
                {
                    if (const UProjectEditorWidget* Self = WeakSelf.Get())
                    {
                        return Text(Self->MaterialTargetSurface);
                    }
                    return FText::GetEmpty();
                })
            ]
        ];
    }

    auto SectionBody = SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
        [
            SNew(STextBlock)
            .TextStyle(&EditorTextStyle)
            .ColorAndOpacity(FSlateColor(Palette.Text))
            .Text(Text(bIsSoundsScript
                ? TEXT("Material Search (MaterialPhysicalMappings — Terrain: per texture, Foliage: default)")
                : TEXT("Model Materials (Terrain: per texture, Foliage: default)")))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)[SurfaceRow]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 6, 0)[MatSearchBox]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
            [
                MakeEditorButton(TEXT("+ Map Custom"), [WeakSelf, bIsSoundsScript]
                {
                    UProjectEditorWidget* Self = WeakSelf.Get();
                    if (!Self || Self->bBusy || !Self->Current) return;
                    if (Self->bRawDirty) { Self->Message = TEXT("Apply JSON first."); return; }
                    FString CustomMat = Self->MaterialSearchQuery.TrimStartAndEnd();
                    if (CustomMat.IsEmpty())
                    {
                        Self->Message = TEXT("Enter a material name to map.");
                        return;
                    }
                    const FString NormCustom = FV3DMaterialAssetReferences::NormalizeMaterialName(CustomMat);
                    if (NormCustom == TEXT("foliage") || NormCustom.EndsWith(TEXT(".foliage")))
                    {
                        CustomMat = TEXT("foliage");
                    }
                    TSharedPtr<FV3DEditorDocument> TargetDoc = bIsSoundsScript ? Self->Current : Self->FindSoundsDocument();
                    if (!TargetDoc.IsValid() || !TargetDoc->Json.IsValid())
                    {
                        Self->Message = TEXT("scripts/sounds.json was not found in this project.");
                        return;
                    }
                    if (TargetDoc == Self->Current) Self->Remember();
                    const FString SurfaceToAssign = Self->MaterialTargetSurface.TrimStartAndEnd().IsEmpty()
                        ? TEXT("SurfaceName") : Self->MaterialTargetSurface.TrimStartAndEnd();
                    V3DMaterialDiscovery::SetOrRemoveMaterialMapping(TargetDoc->Json, CustomMat, SurfaceToAssign, false);
                    TargetDoc->bDirty = true;
                    Self->Message = FString::Printf(TEXT("Mapped '%s' -> '%s' in scripts/sounds.json"), *CustomMat, *SurfaceToAssign);
                    if (TargetDoc == Self->Current) Self->Changed();
                    else Self->RebuildMaterialSearchResults(bIsSoundsScript);
                })
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                MakeEditorButton(TEXT("Rescan"), [WeakSelf, bIsSoundsScript]
                {
                    if (UProjectEditorWidget* Self = WeakSelf.Get())
                    {
                        Self->CachedMaterialsScopeKey.Reset();
                        if (Self->Current) Self->EnsureMaterialsLoaded(bIsSoundsScript, Self->Current->SourcePath);
                        Self->RebuildMaterialSearchResults(bIsSoundsScript);
                    }
                })
            ]
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SBox).MaxDesiredHeight(180.f)
            [
                SNew(SScrollBox)
                + SScrollBox::Slot()[MaterialSearchResultsBox.ToSharedRef()]
            ]
        ];

    Box->AddSlot().AutoHeight().Padding(0, 0, 0, 10)
    [
        SNew(SBorder)
        .BorderImage(WhiteBrush)
        .BorderBackgroundColor(CardBorderColor(bDarkMode, false))
        .Padding(1.f)
        [
            SNew(SBorder)
            .BorderImage(WhiteBrush)
            .BorderBackgroundColor(CardFillColor(Palette, bDarkMode, 1, false))
            .Padding(FMargin(10.f, 8.f))
            [
                SectionBody
            ]
        ]
    ];
}

void UProjectEditorWidget::RefreshForm()
{
    using namespace V3DEditorUI;
    bFormRefreshPending = false;
    if (!Form || !Raw || !Current) return;
    const float SavedScrollOffset = FormScrollBox.IsValid() ? FormScrollBox->GetScrollOffset() : 0.0f;
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
    if (Preview) Preview->SetOverlaysFromJson(Current->Json);
    AddNodeSettingsSection(Form.ToSharedRef());
    AddMaterialSearchSection(Form.ToSharedRef());
    AddObject(Current->Json, V3DProjectEditor::Suggestions(*Current), Form.ToSharedRef(), 0, FString(), Current->Path);
    if (FormScrollBox.IsValid() && SavedScrollOffset > 0.0f)
    {
        FormScrollBox->SetScrollOffset(SavedScrollOffset);
    }
}

void UProjectEditorWidget::AddObject(const TSharedPtr<FJsonObject>& Object, const TSharedPtr<FJsonObject>& Suggested,
    const TSharedRef<SVerticalBox>& Box, int32 Depth, const FString& Context, const FString& PathPrefix)
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

        const FString SectionPath = PathPrefix.IsEmpty() ? Key : (PathPrefix + TEXT("/") + Key);
        TWeakObjectPtr<UProjectEditorWidget> WeakExpandSelf(this);

        if (Value->Type == EJson::Object)
        {
            auto Nested = SNew(SVerticalBox);
            const auto HintObject = Hint && (*Hint).IsValid() && (*Hint)->Type == EJson::Object ? (*Hint)->AsObject() : V3DProjectEditor::EntryTemplate(Context);
            AddObject(Value->AsObject(), HintObject, Nested, Depth + 1, Key, SectionPath);
            const bool bDefaultExpandObj = (Key == TEXT("MeshData") && !Value->AsObject()->Values.IsEmpty()) || Context == TEXT("MeshData");
            const bool* SavedExpand = ExpandedSectionStates.Find(SectionPath);
            const bool bExpandObj = SavedExpand ? *SavedExpand : bDefaultExpandObj;
            PairCardBody->AddSlot().AutoHeight().Padding(4, 2, 0, 0)[SNew(SExpandableArea).InitiallyCollapsed(!bExpandObj)
                .OnAreaExpansionChanged_Lambda([WeakExpandSelf, SectionPath](bool bIsExpanded)
                {
                    if (UProjectEditorWidget* Self = WeakExpandSelf.Get())
                    {
                        Self->ExpandedSectionStates.Add(SectionPath, bIsExpanded);
                    }
                })
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
                if (Entry->Type == EJson::Object) AddObject(Entry->AsObject(), V3DProjectEditor::EntryTemplate(Key), ItemBody, Depth + 1, Key, FString::Printf(TEXT("%s[%d]"), *SectionPath, I));
                else ItemBody->AddSlot().AutoHeight()[SNew(SEditableTextBox).Style(&EditorInputStyle).Text(Text(ValueText(Entry)))
                    .OnTextCommitted_Lambda([this, Object, Key, I, Entry](const FText& T, ETextCommit::Type)
                    {
                        if (bBusy || bRawDirty || bRebuildingForm || bFormRefreshPending) return;
                        if (T.ToString() == ValueText(Entry)) return;
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
            const bool* SavedArrExpand = ExpandedSectionStates.Find(SectionPath);
            const bool bExpandArr = SavedArrExpand ? *SavedArrExpand : false;
            PairCardBody->AddSlot().AutoHeight().Padding(4, 2, 0, 0)[SNew(SExpandableArea).InitiallyCollapsed(!bExpandArr)
                .OnAreaExpansionChanged_Lambda([WeakExpandSelf, SectionPath](bool bIsExpanded)
                {
                    if (UProjectEditorWidget* Self = WeakExpandSelf.Get())
                    {
                        Self->ExpandedSectionStates.Add(SectionPath, bIsExpanded);
                    }
                })
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
            || Key == TEXT("DynamicType") || Key == TEXT("ItemType") || Key == TEXT("VehicleType") || Key == TEXT("ProjectType")
            || Key == TEXT("DominantHand") || Key == TEXT("PrimaryHandOverride") || Key == TEXT("Hand")
            || Key == TEXT("Role") || Key == TEXT("Falloff") || Key == TEXT("Shape")
            || Key == TEXT("FrictionCombineMode") || Key == TEXT("RestitutionCombineMode") || (Context == TEXT("Colliders") && Key == TEXT("Type")) || (Context == TEXT("Lights") && Key == TEXT("Unit")))
        {
            auto Options = MakeShared<TArray<TSharedPtr<FString>>>();
            FString DisplayValue = ValueText(Value);
            TArray<FString> Names;
            if (Key == TEXT("Shader")) Names = {TEXT("Default"), TEXT("Toon")};
            else if (Key == TEXT("MaterialMode")) Names = {TEXT("Auto"), TEXT("Opaque"), TEXT("Opaque Two Sided"), TEXT("Translucent"), TEXT("Translucent Two Sided")};
            else if (Key == TEXT("AssetType")) Names = {TEXT("Model"), TEXT("Sound")};
            else if (Key == TEXT("ModelType")) Names = {TEXT("Static"), TEXT("Dynamic")};
            else if (Key == TEXT("DynamicType")) Names = {TEXT("Humanoid"), TEXT("Vehicle"), TEXT("Prop"), TEXT("Creature"), TEXT("Item")};
            else if (Key == TEXT("ItemType")) Names = {TEXT("Weapon"), TEXT("Tool"), TEXT("Misc")};
            else if (Key == TEXT("VehicleType")) Names = {TEXT("Car"), TEXT("Train")};
            else if (Key == TEXT("ProjectType")) Names = {TEXT("World"), TEXT("Prefab"), TEXT("Dynamic"), TEXT("Character")};
            else if (Key == TEXT("DominantHand") || Key == TEXT("PrimaryHandOverride") || Key == TEXT("Hand")) Names = {TEXT("Right"), TEXT("Left")};
            else if (Key == TEXT("Role")) Names = {TEXT("Disabled"), TEXT("Primary"), TEXT("Secondary"), TEXT("Support")};
            else if (Key == TEXT("Falloff")) Names = {TEXT("Constant"), TEXT("Linear"), TEXT("InverseSquare")};
            else if (Key == TEXT("Shape")) Names = {TEXT("Box"), TEXT("Sphere")};
            else if (Key == TEXT("FrictionCombineMode") || Key == TEXT("RestitutionCombineMode")) Names = {TEXT("Average"), TEXT("Min"), TEXT("Multiply"), TEXT("Max")};
            else if (Context == TEXT("Colliders") && Key == TEXT("Type")) Names = {TEXT("None"), TEXT("Sphere"), TEXT("Capsule"), TEXT("Box")};
            else if (Context == TEXT("Lights") && Key == TEXT("Unit")) Names = {TEXT("Unitless"), TEXT("Candelas"), TEXT("Lumens"), TEXT("EV")};
            for (const auto& Name : Names) Options->Add(MakeShared<FString>(Name));
            PairCardBody->AddSlot().AutoHeight()[SNew(SComboBox<TSharedPtr<FString>>).ComboBoxStyle(&EditorComboStyle).ItemStyle(&EditorRowStyle).OptionsSource(&Options.Get())
                .OnGenerateWidget_Lambda([this, Options](TSharedPtr<FString> Item) { return SNew(STextBlock).TextStyle(&EditorTextStyle).Text(Item.IsValid() ? Text(*Item) : FText::GetEmpty()); })
                .OnSelectionChanged_Lambda([this, Object, Key, Options](TSharedPtr<FString> Item, ESelectInfo::Type Info)
                {
                    if (!Item || bBusy || bRawDirty || bRebuildingForm || bFormRefreshPending || Info == ESelectInfo::Direct) return;
                    Remember(); Object->SetStringField(Key, *Item);
                    // Keep mutually exclusive discriminators and sub-configs consistent.
                    if (Key == TEXT("AssetType") && *Item == TEXT("Sound")) Object->RemoveField(TEXT("ModelType"));
                    if (Key == TEXT("ModelType") && *Item != TEXT("Dynamic"))
                    {
                        Object->RemoveField(TEXT("DynamicType"));
                        Object->RemoveField(TEXT("ItemType"));
                        Object->RemoveField(TEXT("VehicleType"));
                        Object->RemoveField(TEXT("Bones"));
                        Object->RemoveField(TEXT("VehicleTuning"));
                        Object->RemoveField(TEXT("TrainTuning"));
                    }
                    else if (Key == TEXT("DynamicType"))
                    {
                        if (*Item != TEXT("Item")) Object->RemoveField(TEXT("ItemType"));
                        else if (!Object->HasField(TEXT("ItemType"))) Object->SetStringField(TEXT("ItemType"), TEXT("Misc"));

                        if (*Item != TEXT("Vehicle"))
                        {
                            Object->RemoveField(TEXT("VehicleType"));
                            Object->RemoveField(TEXT("VehicleTuning"));
                            Object->RemoveField(TEXT("TrainTuning"));
                        }
                        else if (!Object->HasField(TEXT("VehicleType")))
                        {
                            Object->SetStringField(TEXT("VehicleType"), TEXT("Car"));
                        }

                        if (*Item == TEXT("Humanoid"))
                        {
                            Object->RemoveField(TEXT("GravityField"));
                        }
                        else
                        {
                            Object->RemoveField(TEXT("Bones"));
                        }
                    }
                    else if (Key == TEXT("VehicleType"))
                    {
                        if (*Item == TEXT("Train"))
                        {
                            Object->RemoveField(TEXT("VehicleTuning"));
                        }
                        else
                        {
                            Object->RemoveField(TEXT("TrainTuning"));
                        }
                    }
                    Changed();
                })[SNew(STextBlock).TextStyle(&EditorTextStyle).Text(Text(DisplayValue))]];
        }
        else
        {
            const EJson ExpectedType = Value->Type;
            const bool IsString = ExpectedType == EJson::String;
            auto InputWidget = SNew(SEditableTextBox).Style(&EditorInputStyle).Text(Text(ValueText(Value)))
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
                    if (ExpectedType == EJson::Number)
                    {
                        double ExistingNum = 0.0;
                        if (Object->TryGetNumberField(Key, ExistingNum) && FMath::IsNearlyEqual(ExistingNum, (*Found)->AsNumber(), 1e-6))
                        {
                            return;
                        }
                    }
                    Remember(); Object->SetField(Key, *Found); Changed();
                });

            if (ExpectedType == EJson::Number)
            {
                double StepSize = 1.0;
                if (Key == TEXT("ScaleX") || Key == TEXT("ScaleY") || Key == TEXT("ScaleZ")) StepSize = 0.1;
                else if (Key == TEXT("Intensity")) StepSize = 100.0;
                else if (Key == TEXT("AttenuationRadius") || Key == TEXT("RadiusCm") || Key == TEXT("Range")) StepSize = 50.0;
                else if (Key == TEXT("X") || Key == TEXT("Y") || Key == TEXT("Z")
                    || Key == TEXT("DX") || Key == TEXT("DY") || Key == TEXT("DZ")
                    || Key == TEXT("Pitch") || Key == TEXT("Yaw") || Key == TEXT("Roll")
                    || Key == TEXT("SourceRadius") || Key == TEXT("SoftSourceRadius") || Key == TEXT("Length")) StepSize = 5.0;

                TWeakObjectPtr<UProjectEditorWidget> WeakNudgeSelf(this);
                auto MakeNudge = [WeakNudgeSelf, Object, Key](const double Delta)
                {
                    return [WeakNudgeSelf, Object, Key, Delta]
                    {
                        UProjectEditorWidget* Self = WeakNudgeSelf.Get();
                        if (!Self || Self->bBusy || Self->bRawDirty || Self->bRebuildingForm || Self->bFormRefreshPending) return;
                        double CurVal = 0.0;
                        if (!Object->TryGetNumberField(Key, CurVal) || !FMath::IsFinite(CurVal)) return;
                        Self->Remember();
                        Object->SetNumberField(Key, FMath::RoundToDouble((CurVal + Delta) * 1000.0) / 1000.0);
                        Self->Changed();
                    };
                };

                PairCardBody->AddSlot().AutoHeight()
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)[MakeEditorButton(TEXT("-"), MakeNudge(-StepSize))]
                    + SHorizontalBox::Slot().FillWidth(1)[InputWidget]
                    + SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)[MakeEditorButton(TEXT("+"), MakeNudge(StepSize))]
                ];
            }
            else
            {
                PairCardBody->AddSlot().AutoHeight()[InputWidget];
            }
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
        if (bShowFileBrowser) CloseFileBrowser(); else if (bConfirmBack) bConfirmBack = false; else RequestBack();
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
    bBusy = false; bFormRefreshPending = false; bShowFileBrowser = false;
    BrowserEntries.Reset(); BrowserSelectedFiles.Reset(); CachedModelNodeNames.Reset(); CachedNodeNamesSourcePath.Reset();
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
    List.Reset(); FormScrollBox.Reset(); Form.Reset(); Raw.Reset(); Preview.Reset(); JsonHighlighter.Reset(); BrowserPathBox.Reset(); BrowserListView.Reset(); NodeSearchResultsBox.Reset(); MaterialSearchResultsBox.Reset();
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
    if (auto* R = UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this); R && Definition.IsHumanoid())
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
        Preview->SetOverlaysFromJson(Current->Json);
        Message = TEXT("Live preview ready (wireframe + Collider/Light/Transform/Hand overlays). LMB: Orbit, RMB: Pan, Wheel: Zoom.");
    }
    else if (!bSuccess && IsValid(Builder)) Message = Builder->GetError();
    PreviewBuilder = nullptr;
}
