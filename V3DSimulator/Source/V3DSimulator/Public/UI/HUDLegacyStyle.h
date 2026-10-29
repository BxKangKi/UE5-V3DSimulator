// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Styling/SlateTypes.h"

class UButton;

/** Values recovered from the shipped WBP assets, rather than their BGR thumbnail previews. */
namespace V3DHUDLegacyStyle
{
    inline const FLinearColor MenuBackground(.830770f, .830770f, .830770f, 1.f);
    inline const FLinearColor ButtonBackground(.318547f, .391573f, .462077f, 1.f);
    inline const FLinearColor LoadingBackground(.082754f, .082754f, .082754f, 1.f);
    inline const FLinearColor ProgressFill(0.f, 1.f, .5f, 1.f);
    inline const FLinearColor PauseBackground(.9f, .9f, .9f, .3f);
    inline const FLinearColor PauseAccent(.1f, .3f, .7f, .3f);

    /** Constructor-only asset lookup; callers retain the font in a reflected field. */
    V3DSIMULATOR_API UObject* FindFontForConstructor();
    V3DSIMULATOR_API void ApplyFont(FSlateFontInfo& Font, UObject* FontObject, int32 Size, int32 OutlineSize = 1);
    V3DSIMULATOR_API void ApplyButton(UButton& Button, const FLinearColor& Color,
        const FLinearColor& HoverTint = FLinearColor(.8f, .8f, .8f, 1.f));
}
