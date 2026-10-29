// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "UI/HUDLegacyStyle.h"
#include "UI/WidgetStyleSafety.h"
#include "Engine/Font.h"
#include "Brushes/SlateColorBrush.h"
#include "UObject/ConstructorHelpers.h"

UObject* V3DHUDLegacyStyle::FindFontForConstructor()
{
    static ConstructorHelpers::FObjectFinder<UFont> OriginalFont(
        TEXT("/Game/Resources/Fonts/FT_Pretendard.FT_Pretendard"));
    return OriginalFont.Object;
}

void V3DHUDLegacyStyle::ApplyFont(FSlateFontInfo& Font, UObject* FontObject, int32 Size, int32 OutlineSize)
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

void V3DHUDLegacyStyle::ApplyButton(UButton& Button, const FLinearColor& Color, const FLinearColor& HoverTint)
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
