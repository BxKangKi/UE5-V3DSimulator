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
    FCheckBoxStyle Style = CheckBox.GetWidgetStyle();
    Style.CheckBoxType = ESlateCheckBoxType::ToggleButton;
    Style.UncheckedImage = FSlateColorBrush(Palette.Panel);
    Style.UncheckedHoveredImage = FSlateColorBrush(Palette.Hover);
    Style.UncheckedPressedImage = FSlateColorBrush(Palette.Pressed);
    Style.CheckedImage = FSlateColorBrush(Palette.Selection);
    Style.CheckedHoveredImage = FSlateColorBrush(Palette.Hover);
    Style.CheckedPressedImage = FSlateColorBrush(Palette.Pressed);
    Style.UndeterminedImage = FSlateColorBrush(Palette.Panel);
    Style.UndeterminedHoveredImage = FSlateColorBrush(Palette.Hover);
    Style.UndeterminedPressedImage = FSlateColorBrush(Palette.Pressed);
    Style.UncheckedImage.DrawAs = Style.UncheckedHoveredImage.DrawAs = Style.UncheckedPressedImage.DrawAs =
        Style.CheckedImage.DrawAs = Style.CheckedHoveredImage.DrawAs = Style.CheckedPressedImage.DrawAs =
        Style.UndeterminedImage.DrawAs = Style.UndeterminedHoveredImage.DrawAs = Style.UndeterminedPressedImage.DrawAs =
        ESlateBrushDrawType::Box;
    Style.Padding = FMargin(16.f, 10.f);
    Style.ForegroundColor = Style.HoveredForeground = Style.PressedForeground =
        Style.CheckedForeground = Style.CheckedHoveredForeground = Style.CheckedPressedForeground =
        Style.UndeterminedForeground = FSlateColor(Palette.Text);
    V3DWidgetStyle::SetOwnedStyle(CheckBox, Style);

    if (UTextBlock* Label = Cast<UTextBlock>(CheckBox.GetContent()))
    {
        FSlateFontInfo Font = Label->GetFont();
        ApplyFont(Font, FontObject, FontSize, OutlineSize);
        Label->SetFont(Font);
        Label->SetColorAndOpacity(FSlateColor(Palette.Text));
    }
}
