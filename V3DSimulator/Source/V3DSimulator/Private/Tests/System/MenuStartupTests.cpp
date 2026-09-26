// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/V3DSimulatorGameInstance.h"
#include "UI/StartWorldWidget.h"
#include "UI/WorldSelectionWidget.h"
#include "UI/SettingsMenuWidget.h"
#include "UI/ProjectSelectionWidget.h"
#include "UI/MenuButtonWidget.h"
#include "UI/SettingControlWidget.h"
#include "UObject/StrongObjectPtr.h"
#include "Components/TextBlock.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DMenuClassDefaultsTest,
    "V3DSimulator.UI.MenuClassDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DMenuClassDefaultsTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UV3DSimulatorAssetRegistry> Registry(NewObject<UV3DSimulatorAssetRegistry>());
    const FString CanonicalPath(TEXT("/Game/Blueprints/UI/WBP_ProjectSelection.WBP_ProjectSelection_C"));
    TestEqual(TEXT("Project browser uses the real generated class"),
        Registry->ProjectSelectionWidgetClass.ToSoftObjectPath().ToString(), CanonicalPath);
    Registry->StartMenuWidgetClass.Reset();
    Registry->SettingsMenuWidgetClass.Reset();
    Registry->BooleanSettingWidgetClass.Reset();
    Registry->FloatSettingWidgetClass.Reset();
    Registry->EnumSettingWidgetClass.Reset();
    Registry->ProjectSelectionWidgetClass = TSoftClassPtr<UProjectSelectionWidget>(FSoftObjectPath(
        TEXT("/Game/Blueprints/UI/WBP_BuildSelection.WBP_BuildSelection_C")));
    Registry->EnsureMenuDefaults();
    TestFalse(TEXT("Missing start menu repaired"), Registry->StartMenuWidgetClass.IsNull());
    TestFalse(TEXT("Missing settings menu repaired"), Registry->SettingsMenuWidgetClass.IsNull());
    TestFalse(TEXT("Missing toggle repaired"), Registry->BooleanSettingWidgetClass.IsNull());
    TestFalse(TEXT("Missing slider repaired"), Registry->FloatSettingWidgetClass.IsNull());
    TestFalse(TEXT("Missing dropdown repaired"), Registry->EnumSettingWidgetClass.IsNull());
    TestEqual(TEXT("Serialized legacy redirector repaired"),
        Registry->ProjectSelectionWidgetClass.ToSoftObjectPath().ToString(), CanonicalPath);
    const FSoftObjectPath CustomPath(TEXT("/Game/Custom/ProjectBrowser.ProjectBrowser_C"));
    Registry->ProjectSelectionWidgetClass = TSoftClassPtr<UProjectSelectionWidget>(CustomPath);
    Registry->EnsureMenuDefaults();
    TestEqual(TEXT("Explicit custom project browser preserved"),
        Registry->ProjectSelectionWidgetClass.ToSoftObjectPath().ToString(), CustomPath.ToString());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DMenuAssetsLoadTest,
    "V3DSimulator.UI.MenuAssetsLoad",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DMenuAssetsLoadTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UV3DSimulatorAssetRegistry> Registry(NewObject<UV3DSimulatorAssetRegistry>());
    const auto CheckClass = [this](const TCHAR* Label, UClass* Loaded, UClass* Expected)
    {
        TestTrue(Label, IsValid(Loaded) && Loaded->IsChildOf(Expected));
    };
    CheckClass(TEXT("Start menu"), Registry->StartMenuWidgetClass.LoadSynchronous(), UStartWorldWidget::StaticClass());
    CheckClass(TEXT("World selection"), Registry->WorldSelectionWidgetClass.LoadSynchronous(), UWorldSelectionWidget::StaticClass());
    CheckClass(TEXT("Settings"), Registry->SettingsMenuWidgetClass.LoadSynchronous(), USettingsMenuWidget::StaticClass());
    CheckClass(TEXT("Project selection"), Registry->ProjectSelectionWidgetClass.LoadSynchronous(), UProjectSelectionWidget::StaticClass());
    CheckClass(TEXT("Generated button"), Registry->SelectionEntryWidgetClass.LoadSynchronous(), UMenuButtonWidget::StaticClass());
    CheckClass(TEXT("Settings toggle"), Registry->BooleanSettingWidgetClass.LoadSynchronous(), UBooleanSettingWidget::StaticClass());
    CheckClass(TEXT("Settings slider"), Registry->FloatSettingWidgetClass.LoadSynchronous(), UFloatSettingWidget::StaticClass());
    CheckClass(TEXT("Settings dropdown"), Registry->EnumSettingWidgetClass.LoadSynchronous(), UEnumSettingWidget::StaticClass());
    CheckClass(TEXT("GameInstance Blueprint"), LoadClass<UV3DSimulatorGameInstance>(nullptr,
        TEXT("/Game/Blueprints/BP_GameInstance.BP_GameInstance_C")), UV3DSimulatorGameInstance::StaticClass());
    CheckClass(TEXT("AssetRegistry Blueprint"), LoadClass<UV3DSimulatorAssetRegistry>(nullptr,
        TEXT("/Game/Blueprints/BP_AssetRegistry.BP_AssetRegistry_C")), UV3DSimulatorAssetRegistry::StaticClass());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DNativeMenuRowsTest,
    "V3DSimulator.UI.NativeMenuRows",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DNativeMenuRowsTest::RunTest(const FString& Parameters)
{
    // Exercise the asset-independent fallback tree and post-Construct label binding.
    TStrongObjectPtr<UMenuButtonWidget> Button(NewObject<UMenuButtonWidget>());
    TestTrue(TEXT("Native button initializes"), Button->Initialize());
    Button->ConfigureButton(TEXT("create"), TEXT("Create Project"));
    Button->TakeWidget();
    UTextBlock* Label = Cast<UTextBlock>(Button->GetWidgetFromName(TEXT("Label")));
    TestNotNull(TEXT("Native button has a label"), Label);
    if (Label) TestEqual(TEXT("Configured label survives construction"), Label->GetText().ToString(), FString(TEXT("Create Project")));

    const auto CheckRow = [this](UClass* RowClass, const TCHAR* ControlName)
    {
        TStrongObjectPtr<UUserWidget> Row(NewObject<UUserWidget>(GetTransientPackage(), RowClass));
        TestTrue(TEXT("Native row initializes"), Row->Initialize());
        Row->TakeWidget();
        TestNotNull(TEXT("Native row has a root"), Row->GetRootWidget());
        TestNotNull(TEXT("Native row has a label"), Row->GetWidgetFromName(TEXT("Label")));
        TestNotNull(TEXT("Native row has an input control"), Row->GetWidgetFromName(FName(ControlName)));
    };
    CheckRow(UBooleanSettingWidget::StaticClass(), TEXT("Button"));
    CheckRow(UFloatSettingWidget::StaticClass(), TEXT("Slider"));
    CheckRow(UEnumSettingWidget::StaticClass(), TEXT("Dropdown"));
    return true;
}
#endif
