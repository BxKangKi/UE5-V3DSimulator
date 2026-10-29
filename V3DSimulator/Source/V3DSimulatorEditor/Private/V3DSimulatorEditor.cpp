// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "NativeWidgetDesignerLibrary.h"
#include "V3DTerrainMaterialPreparation.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Framework/Commands/UIAction.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Textures/SlateIcon.h"
#include "ToolMenus.h"
#include "UI/MenuButtonWidget.h"
#include "UI/ResponsiveHUDWidget.h"
#include "UI/SettingControlWidget.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

#define LOCTEXT_NAMESPACE "V3DSimulatorEditor"

class FV3DSimulatorEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        // This module loads PostEngineInit. Cook commandlets need the same material usage
        // preparation as the interactive editor, without registering any editor UI hooks.
        V3DTerrainMaterialPreparation::PrepareShippedMaterial();
        if (IsRunningCommandlet()) return;
        bRegistered = true;
        // Layouts are only generated for assets the user creates, opens, or changes in Class
        // Defaults. Startup never scans, loads, saves, or rewrites the project's widget assets.
        for (UClass* Class : { UResponsiveHUDWidget::StaticClass(), UMenuButtonWidget::StaticClass(),
            USettingControlWidget::StaticClass() })
        {
            FKismetEditorUtilities::RegisterOnBlueprintCreatedCallback(this, Class,
                FKismetEditorUtilities::FOnBlueprintCreated::CreateRaw(this,
                    &FV3DSimulatorEditorModule::OnBlueprintCreated));
        }
        PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(this,
            &FV3DSimulatorEditorModule::OnObjectPropertyChanged);
        if (GEditor) RegisterAssetEditorHook();
        else PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this,
            &FV3DSimulatorEditorModule::RegisterAssetEditorHook);
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this,
            &FV3DSimulatorEditorModule::RegisterMenus));
    }

    virtual void ShutdownModule() override
    {
        if (!bRegistered) return;
        FKismetEditorUtilities::UnregisterAutoBlueprintNodeCreation(this);
        FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
        FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
        if (UAssetEditorSubsystem* Editors = AssetEditors.Get())
            Editors->OnEditorOpeningPreWidgets().Remove(OpeningHandle);
        if (RefreshTicker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(RefreshTicker);
        PendingRefresh.Reset();
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
    }

private:
    void RegisterAssetEditorHook()
    {
        if (!GEditor || OpeningHandle.IsValid()) return;
        AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (UAssetEditorSubsystem* Editors = AssetEditors.Get())
        {
            OpeningHandle = Editors->OnEditorOpeningPreWidgets().AddRaw(this,
                &FV3DSimulatorEditorModule::OnEditorOpeningPreWidgets);
        }
    }

    void OnBlueprintCreated(UBlueprint* Blueprint)
    {
        // Widget factories can still be finishing class generation here; the pre-widgets hook
        // makes a second, idempotent attempt when the new asset is opened.
        UNativeWidgetDesignerLibrary::RefreshNativeWidgetHierarchy(Cast<UWidgetBlueprint>(Blueprint));
    }

    void OnEditorOpeningPreWidgets(const TArray<UObject*>& Assets, IAssetEditorInstance*)
    {
        for (UObject* Asset : Assets)
            UNativeWidgetDesignerLibrary::RefreshNativeWidgetHierarchy(Cast<UWidgetBlueprint>(Asset));
    }

    void OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
    {
        if (!Object || !Object->HasAnyFlags(RF_ClassDefaultObject) || !Event.Property
            || (Event.ChangeType & EPropertyChangeType::Interactive) != 0) return;
        UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(Object->GetClass()->ClassGeneratedBy);
        if (!UNativeWidgetDesignerLibrary::UsesNativeWidgetLayout(Blueprint)) return;
        PendingRefresh.Add(Blueprint);
        if (!RefreshTicker.IsValid())
        {
            // Defer past property editing/compilation instead of rebuilding a widget tree from
            // inside its Details callback. Interactive slider changes coalesce into one refresh.
            RefreshTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this,
                &FV3DSimulatorEditorModule::FlushPendingRefresh), 0.1f);
        }
    }

    bool FlushPendingRefresh(float)
    {
        RefreshTicker.Reset();
        TSet<TWeakObjectPtr<UWidgetBlueprint>> Pending = MoveTemp(PendingRefresh);
        PendingRefresh.Reset();
        for (const TWeakObjectPtr<UWidgetBlueprint>& Blueprint : Pending)
        {
            if (Blueprint.IsValid())
                UNativeWidgetDesignerLibrary::RefreshNativeWidgetHierarchy(Blueprint.Get());
        }
        return false;
    }

    void RegisterMenus()
    {
        FToolMenuOwnerScoped Owner(this);
        UToolMenu* Tools = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
        FToolMenuSection& Section = Tools->FindOrAddSection("V3DSimulator");
        Section.AddMenuEntry("RefreshNativeWidgetHierarchy",
            LOCTEXT("RefreshHierarchy", "Refresh Native HUD Hierarchies"),
            LOCTEXT("RefreshHierarchyTooltip", "Regenerate native Hierarchy templates for open Widget Blueprints from Class Defaults."),
            FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([]
            {
                const FScopedTransaction Transaction(LOCTEXT("RefreshHierarchyTransaction", "Refresh Native HUD Hierarchies"));
                UNativeWidgetDesignerLibrary::RefreshOpenNativeWidgetHierarchies(true);
            })));
    }

    TWeakObjectPtr<UAssetEditorSubsystem> AssetEditors;
    FDelegateHandle OpeningHandle;
    FDelegateHandle PostEngineInitHandle;
    FDelegateHandle PropertyChangedHandle;
    FTSTicker::FDelegateHandle RefreshTicker;
    TSet<TWeakObjectPtr<UWidgetBlueprint>> PendingRefresh;
    bool bRegistered = false;
};

IMPLEMENT_MODULE(FV3DSimulatorEditorModule, V3DSimulatorEditor)
#undef LOCTEXT_NAMESPACE
