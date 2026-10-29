// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Character/PlayerCharacterController.h"
#include "Components/ProgressBar.h"
#include "UI/SurvivalHUDWidget.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DSurvivalHealthPresentationTest,
    "V3DSimulator.UI.SurvivalHealthPresentation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DSurvivalHealthPresentationTest::RunTest(const FString&)
{
    const FPlayerHealthDisplayState UnrestoredState;
    TestFalse(TEXT("No fictional health before authority restores the record"), UnrestoredState.bHasState);

    TStrongObjectPtr<USurvivalHUDWidget> HUD(NewObject<USurvivalHUDWidget>());
    if (!TestTrue(TEXT("Health HUD initializes without a gameplay world"), HUD->Initialize())) return false;
    HUD->PrepareNativeDesignerLayout();
    TestEqual(TEXT("HUD starts hidden until the controller has a valid survival record"),
        HUD->GetVisibility(), ESlateVisibility::Collapsed);
    UProgressBar* Bar = Cast<UProgressBar>(HUD->GetWidgetFromName(TEXT("SurvivalHealthBar")));
    if (!TestNotNull(TEXT("Native health bar exists without Blueprint assets"), Bar)) return false;

    HUD->SetHealth(30.0f, 120.0f);
    TestEqual(TEXT("Configured maximum determines the fraction"), Bar->GetPercent(), 0.25f);
    HUD->SetHealth(240.0f, 120.0f);
    TestEqual(TEXT("Over-healed data stays within the bar"), Bar->GetPercent(), 1.0f);
    HUD->SetHealth(-10.0f, 120.0f);
    TestEqual(TEXT("Negative health displays an empty bar"), Bar->GetPercent(), 0.0f);
    HUD->SetHealth(60.0f, 120.0f);
    HUD->SetHealth(std::numeric_limits<float>::quiet_NaN(), 120.0f);
    HUD->SetHealth(60.0f, 0.0f);
    HUD->SetHealth(60.0f, std::numeric_limits<float>::infinity());
    TestEqual(TEXT("Invalid health never enters Slate's bar fraction"), Bar->GetPercent(), 0.5f);

    HUD->SetDesignerFlags(EWidgetDesignFlags::Designing);
    HUD->SynchronizeProperties();
    TestEqual(TEXT("Designer preview has no world dependency"), Bar->GetPercent(), 0.75f);
    TestEqual(TEXT("Read-only preview never captures gameplay input"),
        HUD->GetVisibility(), ESlateVisibility::HitTestInvisible);
    return true;
}
#endif
