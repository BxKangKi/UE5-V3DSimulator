// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "UI/HUDStyle.h"
#include "UI/WidgetStyleSafety.h"
#include "System/GameManagerSubSystem.h"
#include "Setting/GameSettings.h"
#include "Components/TextBlock.h"
#include "Engine/Font.h"
#include "Brushes/SlateColorBrush.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
    constexpr const TCHAR* MainFontAssetPath = TEXT("/Game/Resources/Fonts/FT_Pretendard.FT_Pretendard");

    FSlateBrush MakeOutlinedBoxBrush(const FLinearColor& FillColor, const FLinearColor& OutlineColor,
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
}

UObject* V3DHUDStyle::FindFontForConstructor()
{
    static ConstructorHelpers::FObjectFinder<UFont> OriginalFont(MainFontAssetPath);
    return OriginalFont.Object;
}

UObject* V3DHUDStyle::FindFontRuntime()
{
    return LoadObject<UFont>(nullptr, MainFontAssetPath);
}

void V3DHUDStyle::ApplyFont(FSlateFontInfo& Font, UObject* FontObject, int32 Size, int32 OutlineSize)
{
    (void)OutlineSize;
    if (FontObject)
    {
        Font.FontObject = FontObject;
        Font.TypefaceFontName = TEXT("Default");
    }
    Font.Size = FMath::Clamp(Size, 8, 144);
    Font.OutlineSettings.OutlineSize = 0;
    Font.OutlineSettings.OutlineColor = FLinearColor::Transparent;
}

void V3DHUDStyle::ApplyFontWithWeight(FSlateFontInfo& Font, UObject* FontObject, int32 Size, bool bBold,
    const FLinearColor& FauxBoldColor)
{
    const int32 ClampedSize = FMath::Clamp(Size, 8, 144);
    Font.Size = ClampedSize;
    Font.OutlineSettings.OutlineSize = 0;
    Font.OutlineSettings.OutlineColor = FLinearColor::Transparent;

    if (!FontObject)
    {
        Font.TypefaceFontName = bBold ? TEXT("Bold") : TEXT("Regular");
        return;
    }

    Font.FontObject = FontObject;
    FName SelectedTypeface = TEXT("Default");
    bool bMatchedExplicitBold = false;

    if (const UFont* FontAsset = Cast<UFont>(FontObject))
    {
        if (const FCompositeFont* Composite = FontAsset->GetCompositeFont())
        {
            for (const FTypefaceEntry& Entry : Composite->DefaultTypeface.Fonts)
            {
                const FString NameStr = Entry.Name.ToString();
                if (bBold)
                {
                    if (NameStr.Equals(TEXT("Bold"), ESearchCase::IgnoreCase))
                    {
                        SelectedTypeface = Entry.Name;
                        bMatchedExplicitBold = true;
                        break;
                    }
                    if (!bMatchedExplicitBold && NameStr.Contains(TEXT("Bold"), ESearchCase::IgnoreCase))
                    {
                        SelectedTypeface = Entry.Name;
                        bMatchedExplicitBold = true;
                    }
                }
                else
                {
                    if (NameStr.Equals(TEXT("Regular"), ESearchCase::IgnoreCase)
                        || NameStr.Equals(TEXT("Normal"), ESearchCase::IgnoreCase))
                    {
                        SelectedTypeface = Entry.Name;
                        break;
                    }
                }
            }
        }
    }

    Font.TypefaceFontName = SelectedTypeface;
    // If FT_Pretendard is single-weight without a separate Bold typeface entry, apply a crisp
    // same-color outline so "V3D" remains visually bold next to regular "Simulator".
    if (bBold && !bMatchedExplicitBold && FauxBoldColor.A > 0.01f)
    {
        Font.OutlineSettings.OutlineSize = FMath::Clamp(FMath::RoundToInt(ClampedSize * 0.022f), 1, 3);
        Font.OutlineSettings.OutlineColor = FauxBoldColor;
        Font.OutlineSettings.bSeparateFillAlpha = false;
    }
}

void V3DHUDStyle::ApplyButton(UButton& Button, const FLinearColor& Color, const FLinearColor& HoverTint)
{
    FButtonStyle Style = Button.GetStyle();
    const FLinearColor PressedColor(Color.R * 0.84f, Color.G * 0.84f, Color.B * 0.84f, Color.A);
    const FLinearColor DisabledColor(Color.R * 0.55f, Color.G * 0.55f, Color.B * 0.55f, Color.A * 0.65f);
    Style.Normal = FSlateColorBrush(Color);
    Style.Hovered = FSlateColorBrush(HoverTint);
    Style.Pressed = FSlateColorBrush(PressedColor);
    Style.Disabled = FSlateColorBrush(DisabledColor);
    Style.Normal.DrawAs = Style.Hovered.DrawAs = Style.Pressed.DrawAs = Style.Disabled.DrawAs = ESlateBrushDrawType::Box;
    Style.NormalPadding = Style.PressedPadding = FMargin(0.f);
    V3DWidgetStyle::SetOwnedStyle(Button, Style);
    Button.SetBackgroundColor(FLinearColor::White);
    Button.SetColorAndOpacity(FLinearColor::White);
}

bool V3DHUDStyle::IsDarkMode(const UObject* WorldContext)
{
    if (const UGameManagerSubSystem* Manager = UGameManagerSubSystem::FindGameManager(WorldContext))
    {
        if (const UGameSettings* Settings = Manager->GetGameSettings())
        {
            return Settings->bEditorDarkMode;
        }
    }
    return false;
}

V3DHUDStyle::FAuthoringPalette V3DHUDStyle::GetAuthoringPalette(const bool bDarkMode)
{
    if (bDarkMode)
    {
        return {
            AuthoringDark::Background,
            AuthoringDark::Panel,
            AuthoringDark::Input,
            AuthoringDark::Button,
            AuthoringDark::Hover,
            AuthoringDark::Pressed,
            AuthoringDark::Selection,
            AuthoringDark::Text,
            AuthoringDark::Muted,
            AuthoringDark::ModalScrim
        };
    }
    return {
        Authoring::Background,
        Authoring::Panel,
        Authoring::Input,
        Authoring::Button,
        Authoring::Hover,
        Authoring::Pressed,
        Authoring::Selection,
        Authoring::Text,
        Authoring::Muted,
        Authoring::ModalScrim
    };
}

void V3DHUDStyle::ApplyAuthoringRows(FTableRowStyle& Style, const bool bDarkMode)
{
    const FAuthoringPalette Palette = GetAuthoringPalette(bDarkMode);
    Style.EvenRowBackgroundBrush = FSlateColorBrush(Palette.Input);
    Style.OddRowBackgroundBrush = FSlateColorBrush(Palette.Background);
    Style.EvenRowBackgroundHoveredBrush = Style.OddRowBackgroundHoveredBrush = FSlateColorBrush(Palette.Hover);
    Style.ActiveBrush = Style.InactiveBrush = FSlateColorBrush(Palette.Selection);
    Style.ActiveHoveredBrush = Style.InactiveHoveredBrush = FSlateColorBrush(Palette.Hover);
    Style.TextColor = Style.SelectedTextColor = FSlateColor(Palette.Text);
}

void V3DHUDStyle::ApplyEditableTextBox(UEditableTextBox& TextBox, const FAuthoringPalette& Palette,
    UObject* FontObject, int32 FontSize, int32 OutlineSize)
{
    FEditableTextBoxStyle Style = TextBox.GetWidgetStyle();
    ApplyFont(Style.TextStyle.Font, FontObject, FontSize, OutlineSize);
    Style.TextStyle.ColorAndOpacity = FSlateColor(Palette.Text);
    Style.BackgroundImageNormal = FSlateColorBrush(Palette.Input);
    Style.BackgroundImageHovered = FSlateColorBrush(Palette.Hover);
    Style.BackgroundImageFocused = FSlateColorBrush(Palette.Input);
    Style.BackgroundImageReadOnly = FSlateColorBrush(Palette.Panel);
    Style.BackgroundImageNormal.DrawAs = Style.BackgroundImageHovered.DrawAs =
        Style.BackgroundImageFocused.DrawAs = Style.BackgroundImageReadOnly.DrawAs = ESlateBrushDrawType::Box;
    Style.ForegroundColor = FSlateColor(Palette.Text);
    Style.FocusedForegroundColor = FSlateColor(Palette.Text);
    Style.ReadOnlyForegroundColor = FSlateColor(Palette.Muted);
    V3DWidgetStyle::SetOwnedStyle(TextBox, Style);
    TextBox.SetForegroundColor(Palette.Text);
}

void V3DHUDStyle::ApplyComboBox(UComboBoxString& Combo, const FAuthoringPalette& Palette, const bool bDarkMode)
{
    FComboBoxStyle Style = Combo.GetWidgetStyle();
    Style.ComboButtonStyle.ButtonStyle.Normal = FSlateColorBrush(Palette.Button);
    Style.ComboButtonStyle.ButtonStyle.Hovered = FSlateColorBrush(Palette.Hover);
    Style.ComboButtonStyle.ButtonStyle.Pressed = FSlateColorBrush(Palette.Pressed);
    Style.ComboButtonStyle.ButtonStyle.Normal.DrawAs =
        Style.ComboButtonStyle.ButtonStyle.Hovered.DrawAs =
        Style.ComboButtonStyle.ButtonStyle.Pressed.DrawAs = ESlateBrushDrawType::Box;
    Style.ComboButtonStyle.MenuBorderBrush = FSlateColorBrush(Palette.Panel);
    Style.ComboButtonStyle.DownArrowImage.TintColor = FSlateColor(Palette.Text);
    V3DWidgetStyle::SetOwnedStyle(Combo, Style);
    FTableRowStyle Item = Combo.GetItemStyle();
    ApplyAuthoringRows(Item, bDarkMode);
    V3DWidgetStyle::SetOwnedItemStyle(Combo, Item);
}

void V3DHUDStyle::ApplyCheckBox(UCheckBox& CheckBox, const FAuthoringPalette& Palette,
    UObject* FontObject, int32 FontSize, int32 OutlineSize)
{
    const bool bDarkPalette = Palette.Background.R < 0.25f;
    const FLinearColor UncheckedFill = Palette.Input;
    const FLinearColor UncheckedHover = Palette.Hover;
    const FLinearColor UncheckedBorder = bDarkPalette
        ? FLinearColor(0.42f, 0.48f, 0.60f, 1.f)
        : FLinearColor(0.45f, 0.54f, 0.66f, 1.f);

    const FLinearColor CheckedFill = bDarkPalette
        ? FLinearColor(0.16f, 0.46f, 0.84f, 1.f)
        : FLinearColor(0.14f, 0.44f, 0.82f, 1.f);
    const FLinearColor CheckedHover = bDarkPalette
        ? FLinearColor(0.24f, 0.56f, 0.94f, 1.f)
        : FLinearColor(0.22f, 0.53f, 0.92f, 1.f);
    const FLinearColor CheckedPressed = bDarkPalette
        ? FLinearColor(0.11f, 0.35f, 0.68f, 1.f)
        : FLinearColor(0.10f, 0.34f, 0.66f, 1.f);
    const FLinearColor CheckedBorder = bDarkPalette
        ? FLinearColor(0.55f, 0.80f, 1.0f, 1.f)
        : FLinearColor(0.08f, 0.28f, 0.58f, 1.f);
    const FLinearColor CheckedText(0.98f, 0.99f, 1.0f, 1.f);

    FCheckBoxStyle Style = CheckBox.GetWidgetStyle();
    Style.CheckBoxType = ESlateCheckBoxType::ToggleButton;
    Style.UncheckedImage = MakeOutlinedBoxBrush(UncheckedFill, UncheckedBorder, 6.f, 2.f);
    Style.UncheckedHoveredImage = MakeOutlinedBoxBrush(UncheckedHover, CheckedBorder, 6.f, 2.f);
    Style.UncheckedPressedImage = MakeOutlinedBoxBrush(Palette.Pressed, CheckedBorder, 6.f, 2.f);
    Style.CheckedImage = MakeOutlinedBoxBrush(CheckedFill, CheckedBorder, 6.f, 2.f);
    Style.CheckedHoveredImage = MakeOutlinedBoxBrush(CheckedHover, CheckedBorder, 6.f, 2.f);
    Style.CheckedPressedImage = MakeOutlinedBoxBrush(CheckedPressed, CheckedBorder, 6.f, 2.f);
    Style.UndeterminedImage = MakeOutlinedBoxBrush(Palette.Panel, UncheckedBorder, 6.f, 2.f);
    Style.UndeterminedHoveredImage = MakeOutlinedBoxBrush(UncheckedHover, CheckedBorder, 6.f, 2.f);
    Style.UndeterminedPressedImage = MakeOutlinedBoxBrush(Palette.Pressed, CheckedBorder, 6.f, 2.f);
    Style.Padding = FMargin(16.f, 10.f);
    Style.ForegroundColor = FSlateColor(Palette.Muted);
    Style.HoveredForeground = FSlateColor(Palette.Text);
    Style.PressedForeground = FSlateColor(Palette.Text);
    Style.CheckedForeground = Style.CheckedHoveredForeground = Style.CheckedPressedForeground = FSlateColor(CheckedText);
    Style.UndeterminedForeground = FSlateColor(Palette.Text);
    V3DWidgetStyle::SetOwnedStyle(CheckBox, Style);

    if (UTextBlock* Label = Cast<UTextBlock>(CheckBox.GetContent()))
    {
        FSlateFontInfo Font = Label->GetFont();
        ApplyFont(Font, FontObject, FontSize, OutlineSize);
        Label->SetFont(Font);
        Label->SetColorAndOpacity(FSlateColor::UseForeground());
    }
}
