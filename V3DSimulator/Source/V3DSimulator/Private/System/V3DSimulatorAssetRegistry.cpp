// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/V3DSimulatorAssetRegistry.h"

UV3DSimulatorAssetRegistry::UV3DSimulatorAssetRegistry(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // Shared UI component classes are centralized here. These native defaults keep existing
    // projects working after the per-widget class overrides were removed; a Blueprint registry
    // subclass can still override any path in one place. No widget instance is loaded by this CDO.
    ProjectSelectionWidgetClass = TSoftClassPtr<UProjectSelectionWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/WBP_ProjectSelection.WBP_ProjectSelection_C")));
    BuildStatusWidgetClass = TSoftClassPtr<UBuildStatusWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/WBP_BuildStatus.WBP_BuildStatus_C")));
    SelectionEntryWidgetClass = TSoftClassPtr<UMenuButtonWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/Components/WBP_Button.WBP_Button_C")));
    BuildStatusConfirmWidgetClass = TSoftClassPtr<UMenuButtonWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/Components/WBP_Button.WBP_Button_C")));
    BooleanSettingWidgetClass = TSoftClassPtr<UBooleanSettingWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/Components/WBP_Settings_Toggle.WBP_Settings_Toggle_C")));
    FloatSettingWidgetClass = TSoftClassPtr<UFloatSettingWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/Components/WBP_Settings_Slider.WBP_Settings_Slider_C")));
    EnumSettingWidgetClass = TSoftClassPtr<UEnumSettingWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/Components/WBP_Settings_Dropdown.WBP_Settings_Dropdown_C")));
    EnsureMenuDefaults();
}

void UV3DSimulatorAssetRegistry::EnsureMenuDefaults()
{
    if (StartMenuWidgetClass.IsNull())
        StartMenuWidgetClass = TSoftClassPtr<UStartWorldWidget>(FSoftObjectPath(
            TEXT("/Game/Blueprints/UI/WBP_StartMenu.WBP_StartMenu_C")));
    if (WorldSelectionWidgetClass.IsNull())
        WorldSelectionWidgetClass = TSoftClassPtr<UWorldSelectionWidget>(FSoftObjectPath(
            TEXT("/Game/Blueprints/UI/WBP_WorldSelection.WBP_WorldSelection_C")));
    if (SettingsMenuWidgetClass.IsNull())
        SettingsMenuWidgetClass = TSoftClassPtr<USettingsMenuWidget>(FSoftObjectPath(
            TEXT("/Game/Blueprints/UI/WBP_Settings.WBP_Settings_C")));
    // Old Blueprint defaults can retain this redirector even after the native CDO changes.
    if (ProjectSelectionWidgetClass.IsNull() || ProjectSelectionWidgetClass.ToSoftObjectPath()
        == FSoftObjectPath(TEXT("/Game/Blueprints/UI/WBP_BuildSelection.WBP_BuildSelection_C")))
        ProjectSelectionWidgetClass = TSoftClassPtr<UProjectSelectionWidget>(FSoftObjectPath(
            TEXT("/Game/Blueprints/UI/WBP_ProjectSelection.WBP_ProjectSelection_C")));
    if (SelectionEntryWidgetClass.IsNull())
        SelectionEntryWidgetClass = TSoftClassPtr<UMenuButtonWidget>(FSoftObjectPath(
            TEXT("/Game/Blueprints/UI/Components/WBP_Button.WBP_Button_C")));
    if (BooleanSettingWidgetClass.IsNull())
        BooleanSettingWidgetClass = TSoftClassPtr<UBooleanSettingWidget>(FSoftObjectPath(
            TEXT("/Game/Blueprints/UI/Components/WBP_Settings_Toggle.WBP_Settings_Toggle_C")));
    if (FloatSettingWidgetClass.IsNull())
        FloatSettingWidgetClass = TSoftClassPtr<UFloatSettingWidget>(FSoftObjectPath(
            TEXT("/Game/Blueprints/UI/Components/WBP_Settings_Slider.WBP_Settings_Slider_C")));
    if (EnumSettingWidgetClass.IsNull())
        EnumSettingWidgetClass = TSoftClassPtr<UEnumSettingWidget>(FSoftObjectPath(
            TEXT("/Game/Blueprints/UI/Components/WBP_Settings_Dropdown.WBP_Settings_Dropdown_C")));

}
