// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Styling/SlateTypes.h"

class UButton;
class UCheckBox;
class UEditableTextBox;
class UComboBoxString;

/** Values recovered from the shipped WBP assets, rather than their BGR thumbnail previews. */
namespace V3DHUDStyle
{
    // Unified palette shared across Editor and all runtime/menu UIs.
    inline const FLinearColor MenuBackground(.91f, .93f, .96f, 1.f);
    inline const FLinearColor ButtonBackground(.64f, .77f, .91f, 1.f);
    inline const FLinearColor LoadingBackground(.91f, .93f, .96f, 1.f);
    inline const FLinearColor ProgressFill(.50f, .70f, .91f, 1.f);
    inline const FLinearColor PauseBackground(.20f, .20f, .21f, .65f);
    inline const FLinearColor PauseAccent(.82f, .87f, .92f, .94f);

    struct FAuthoringPalette
    {
        FLinearColor Background;
        FLinearColor Panel;
        FLinearColor Input;
        FLinearColor Button;
        FLinearColor Hover;
        FLinearColor Pressed;
        FLinearColor Selection;
        FLinearColor Text;
        FLinearColor Muted;
        FLinearColor ModalScrim;
    };

    // Light palette used inside ProjectEditorWidget; menu defaults stay unchanged.
    namespace Authoring
    {
        inline const FLinearColor Background(.91f, .93f, .96f, 1.f);
        inline const FLinearColor Panel(.82f, .87f, .92f, 1.f);
        inline const FLinearColor Input(.97f, .98f, 1.f, 1.f);
        inline const FLinearColor Button(.64f, .77f, .91f, 1.f);
        inline const FLinearColor Hover(.78f, .86f, .96f, 1.f);
        inline const FLinearColor Pressed(.54f, .68f, .84f, 1.f);
        inline const FLinearColor Selection(.50f, .70f, .91f, 1.f);
        inline const FLinearColor Text(.018f, .028f, .05f, 1.f);
        inline const FLinearColor Muted(.15f, .20f, .28f, 1.f);
        inline const FLinearColor ModalScrim(.20f, .25f, .30f, .62f);
    }

    // Dark palette for ProjectEditorWidget dark-mode toggle.
    namespace AuthoringDark
    {
        inline const FLinearColor Background(.055f, .065f, .085f, 1.f);
        inline const FLinearColor Panel(.095f, .112f, .145f, 1.f);
        inline const FLinearColor Input(.035f, .042f, .056f, 1.f);
        inline const FLinearColor Button(.155f, .215f, .300f, 1.f);
        inline const FLinearColor Hover(.220f, .310f, .430f, 1.f);
        inline const FLinearColor Pressed(.115f, .165f, .240f, 1.f);
        inline const FLinearColor Selection(.190f, .340f, .520f, 1.f);
        inline const FLinearColor Text(.910f, .935f, .970f, 1.f);
        inline const FLinearColor Muted(.620f, .680f, .770f, 1.f);
        inline const FLinearColor ModalScrim(.01f, .015f, .025f, .74f);
    }

    V3DSIMULATOR_API bool IsDarkMode(const UObject* WorldContext);
    V3DSIMULATOR_API FAuthoringPalette GetAuthoringPalette(bool bDarkMode);
    V3DSIMULATOR_API void ApplyAuthoringRows(FTableRowStyle& Style, bool bDarkMode = false);

    /** Constructor-only asset lookup; callers retain the font in a reflected field. */
    V3DSIMULATOR_API UObject* FindFontForConstructor();
    /** Runtime-safe font lookup matching the main menu font (FT_Pretendard). */
    V3DSIMULATOR_API UObject* FindFontRuntime();
    V3DSIMULATOR_API void ApplyFont(FSlateFontInfo& Font, UObject* FontObject, int32 Size, int32 OutlineSize = 0);
    /** Applies Pretendard with explicit Bold/Regular typeface selection (with faux-bold fallback if single-weight). */
    V3DSIMULATOR_API void ApplyFontWithWeight(FSlateFontInfo& Font, UObject* FontObject, int32 Size, bool bBold,
        const FLinearColor& FauxBoldColor = FLinearColor::Transparent);
    V3DSIMULATOR_API void ApplyButton(UButton& Button, const FLinearColor& Color,
        const FLinearColor& HoverTint = FLinearColor(.78f, .86f, .96f, 1.f));
    V3DSIMULATOR_API void ApplyEditableTextBox(UEditableTextBox& TextBox, const FAuthoringPalette& Palette,
        UObject* FontObject, int32 FontSize, int32 OutlineSize = 0);
    V3DSIMULATOR_API void ApplyComboBox(UComboBoxString& Combo, const FAuthoringPalette& Palette,
        bool bDarkMode = false);
    V3DSIMULATOR_API void ApplyCheckBox(UCheckBox& CheckBox, const FAuthoringPalette& Palette,
        UObject* FontObject, int32 FontSize, int32 OutlineSize = 0);
}
