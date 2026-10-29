// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/V3DSimulatorAssetRegistry.h"
#include "System/V3DTerrain.h"
#include "Materials/MaterialInterface.h"
#include "UI/BuildStatusWidget.h"
#include "UI/CreatorHUDWidget.h"
#include "UI/DebugHUDWidget.h"
#include "UI/LoadingHUDWidget.h"
#include "UI/PauseMenuWidget.h"
#include "UI/MainWorldWidget.h"

UV3DSimulatorAssetRegistry::UV3DSimulatorAssetRegistry(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // Preserve top-level screen Blueprint overrides without registering code-only components.
    // These soft paths do not load widget instances while constructing the registry CDO.
    ProjectSelectionWidgetClass = TSoftClassPtr<UProjectSelectionWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/WBP_ProjectSelection.WBP_ProjectSelection_C")));
    BuildStatusWidgetClass = TSoftClassPtr<UBuildStatusWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/WBP_BuildStatus.WBP_BuildStatus_C")));
    EnsureMenuDefaults();
}

void UV3DSimulatorAssetRegistry::EnsureMenuDefaults()
{
    if (TerrainMaterial.IsNull())
        TerrainMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(V3DTerrain::DefaultMaterialPath));
    if (ShaderLibraryMPC.IsNull())
        ShaderLibraryMPC = TSoftObjectPtr<UMaterialParameterCollection>(FSoftObjectPath(
            TEXT("/ShaderLibrary/MPC_ShaderLibrary.MPC_ShaderLibrary")));
    if (CreatorHUDWidgetClass.IsNull())
        CreatorHUDWidgetClass = TSoftClassPtr<UCreatorHUDWidget>(UCreatorHUDWidget::StaticClass());
    if (PauseMenuWidgetClass.IsNull())
        PauseMenuWidgetClass = TSoftClassPtr<UPauseMenuWidget>(UPauseMenuWidget::StaticClass());
    if (DebugWidgetClass.IsNull())
        DebugWidgetClass = TSoftClassPtr<UUserWidget>(UDebugHUDWidget::StaticClass());
    if (LoadingWidgetClass.IsNull())
        LoadingWidgetClass = TSoftClassPtr<UUserWidget>(ULoadingHUDWidget::StaticClass());
    if (BuildStatusWidgetClass.IsNull())
        BuildStatusWidgetClass = TSoftClassPtr<UBuildStatusWidget>(UBuildStatusWidget::StaticClass());
    if (MultiplayerMenuWidgetClass.IsNull())
        MultiplayerMenuWidgetClass = TSoftClassPtr<UMainWorldWidget>(UMultiplayerMenuWidget::StaticClass());
    if (StartMenuWidgetClass.IsNull())
        StartMenuWidgetClass = TSoftClassPtr<UMainWorldWidget>(FSoftObjectPath(
            TEXT("/Game/Blueprints/UI/WBP_StartMenu.WBP_StartMenu_C")));
    if (WorldSelectionWidgetClass.IsNull())
        WorldSelectionWidgetClass = TSoftClassPtr<UWorldSelectionWidget>(FSoftObjectPath(
            TEXT("/Game/Blueprints/UI/WBP_WorldSelection.WBP_WorldSelection_C")));
    if (SettingsMenuWidgetClass.IsNull())
        SettingsMenuWidgetClass = TSoftClassPtr<USettingsMenuWidget>(FSoftObjectPath(
            TEXT("/Game/Blueprints/UI/WBP_Settings.WBP_Settings_C")));
    if (ProjectSelectionWidgetClass.IsNull())
        ProjectSelectionWidgetClass = TSoftClassPtr<UProjectSelectionWidget>(FSoftObjectPath(
            TEXT("/Game/Blueprints/UI/WBP_ProjectSelection.WBP_ProjectSelection_C")));
}

UMaterialInterface* UV3DSimulatorAssetRegistry::ResolveTerrainMaterial() const
{
    if (!IsInGameThread() || IsGarbageCollecting() || TerrainMaterial.IsNull())
    {
        return nullptr;
    }

    if (UMaterialInterface* Existing = TerrainMaterial.Get())
    {
        if (IsValid(Existing))
        {
            return Existing;
        }
    }

    return TerrainMaterial.LoadSynchronous();
}
