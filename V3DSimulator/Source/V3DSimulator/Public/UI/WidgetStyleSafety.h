// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "Components/Button.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/Slider.h"

/** Keep every style forwarded to Slate in the styled UWidget's own reflected storage. */
namespace V3DWidgetStyle
{
    inline void SetOwnedStyle(UEditableTextBox& Widget, const FEditableTextBoxStyle& Desired)
    {
        // UE 5.8's text-box setter forwards &InStyle to Slate. Passing a stack copy leaves
        // DetermineFont() with a dangling pointer during a later designer/thumbnail prepass.
        // This is a non-const UWidget; its getter exposes its mutable UPROPERTY as const-ref.
        FEditableTextBoxStyle& Owned = const_cast<FEditableTextBoxStyle&>(Widget.GetWidgetStyle());
        if (&Owned != &Desired) Owned = Desired;
        Widget.SetWidgetStyle(Owned);
    }

    inline void SetOwnedStyle(UComboBoxString& Widget, const FComboBoxStyle& Desired)
    {
        FComboBoxStyle& Owned = const_cast<FComboBoxStyle&>(Widget.GetWidgetStyle());
        if (&Owned != &Desired) Owned = Desired;
        Widget.SetWidgetStyle(Owned);
    }

    inline void SetOwnedItemStyle(UComboBoxString& Widget, const FTableRowStyle& Desired)
    {
        FTableRowStyle& Owned = const_cast<FTableRowStyle&>(Widget.GetItemStyle());
        if (&Owned != &Desired) Owned = Desired;
        Widget.SetItemStyle(Owned);
    }

    inline void SetOwnedStyle(UButton& Widget, const FButtonStyle& Desired)
    {
        FButtonStyle& Owned = const_cast<FButtonStyle&>(Widget.GetStyle());
        if (&Owned != &Desired) Owned = Desired;
        Widget.SetStyle(Owned);
    }

    inline void SetOwnedStyle(USlider& Widget, const FSliderStyle& Desired)
    {
        FSliderStyle& Owned = const_cast<FSliderStyle&>(Widget.GetWidgetStyle());
        if (&Owned != &Desired) Owned = Desired;
        Widget.SetWidgetStyle(Owned);
    }
}
