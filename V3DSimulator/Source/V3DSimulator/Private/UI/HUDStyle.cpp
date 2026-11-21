// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "UI/HUDStyle.h"
#include "UI/WidgetStyleSafety.h"
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
    if (FontObject)
    {
        Font.FontObject = FontObject;
        Font.TypefaceFontName = TEXT("Default");
    }
    Font.Size = FMath::Clamp(Size, 8, 144);
    Font.OutlineSettings.OutlineSize = FMath::Clamp(OutlineSize, 0, 8);
    Font.OutlineSettings.OutlineColor = FLinearColor::Black;
}

void V3DHUDStyle::ApplyButton(UButton& Button, const FLinearColor& Color, const FLinearColor& HoverTint)
{
    FButtonStyle Style = Button.GetStyle();
    Style.Normal = FSlateColorBrush(FLinearColor::White);
    Style.Hovered = FSlateColorBrush(HoverTint);
    Style.Pressed = FSlateColorBrush(FLinearColor(.5f, .5f, .5f, 1.f));
    Style.Disabled = FSlateColorBrush(FLinearColor(.4f, .4f, .4f, .5f));
    Style.Normal.DrawAs = Style.Hovered.DrawAs = Style.Pressed.DrawAs = ESlateBrushDrawType::Box;
    Style.NormalPadding = Style.PressedPadding = FMargin(0.f);
    V3DWidgetStyle::SetOwnedStyle(Button, Style);
    Button.SetBackgroundColor(Color);
    Button.SetColorAndOpacity(FLinearColor::White);
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
