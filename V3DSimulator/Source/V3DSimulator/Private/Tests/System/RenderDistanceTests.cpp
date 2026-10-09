// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "System/V3DStreamingPolicy.h"
#include "Setting/GameSettings.h"
#include "UI/SettingsMenuWidget.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DMaxRenderDistanceBoundsTest,
    "V3DSimulator.Streaming.MaxRenderDistance.Bounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DMaxRenderDistanceBoundsTest::RunTest(const FString&)
{
    using namespace V3DStreamingPolicy;
    const double MaxCm = 102400.0;
    const double ScreenDistance = ScreenSizeDistance(16.0 / 9.0);
    const FVector Size(20000.0);
    const FVector Observer = FVector::ZeroVector;
    const FTransform Touching(FVector(MaxCm + 10000.0, 0, 0));
    const FTransform Outside(FVector(MaxCm + 10001.0, 0, 0));

    TestTrue(TEXT("Pivot outside, bound touching range is retained"),
        MeshInRange(Size, Touching, Observer, ScreenDistance, MaxCm));
    TestTrue(TEXT("Fixture would occupy over 5 percent of the screen"),
        (16.0 / 9.0) * Size.Size() * 0.5 / Outside.GetLocation().Size() > MinScreenDiameter);
    TestFalse(TEXT("Visible size cannot override maximum distance"),
        MeshInRange(Size, Outside, Observer, ScreenDistance, MaxCm));
    TestFalse(TEXT("Always-loaded cannot override maximum distance"),
        MeshInRange(Size, Outside, Observer, ScreenDistance, MaxCm, true));
    TestFalse(TEXT("Unload hysteresis cannot extend maximum distance"),
        MeshInRange(Size, Outside, Observer, ScreenDistance, MaxCm, false, 2.0));
    TestFalse(TEXT("Coarse scene also rejects fully distant bounds"),
        SceneInRange(FVector::ZeroVector, Size, Outside, Observer, ScreenDistance, 2.0, MaxCm));

    const FVector LongSize(40000, 2000, 1000);
    FTransform Rotated(FRotator(0, 90, 0), FVector(MaxCm + 3000, 0, 0), FVector(-2, 3, 1));
    TestTrue(TEXT("Rotation and mirrored non-uniform scale keep touching bounds"),
        BoundsInRenderRange(FVector::ZeroVector, LongSize, Rotated, Observer, MaxCm + 0.001));
    Rotated.SetLocation(FVector(MaxCm + 3010, 0, 0));
    TestFalse(TEXT("Scaled rotated bounds outside range are rejected"),
        BoundsInRenderRange(FVector::ZeroVector, LongSize, Rotated, Observer, MaxCm));

    TestFalse(TEXT("Zero-sized distant bounds are culled"),
        BoundsInRenderRange(FVector::ZeroVector, FVector::ZeroVector, Outside, Observer, MaxCm));
    TestTrue(TEXT("Larger setting admits a previously culled mesh"),
        MeshInRange(Size, Outside, Observer, ScreenDistance, 204800.0));
    TestTrue(TEXT("Broad phase includes bound radius beyond camera range"),
        FMath::IsNearlyEqual(LoadRadius(Size, 1.0, ScreenDistance, MaxCm), MaxCm + Size.Size() * 0.5));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DMaxRenderDistanceSettingsTest,
    "V3DSimulator.Streaming.MaxRenderDistance.Settings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DMaxRenderDistanceSettingsTest::RunTest(const FString&)
{
    using namespace V3DStreamingPolicy;
    TStrongObjectPtr<UGameSettings> Settings(NewObject<UGameSettings>());
    TStrongObjectPtr<UGameSettings> Restored(NewObject<UGameSettings>());
    TestEqual(TEXT("Old files get the default"), Settings->GetClampedMaxRenderDistanceMeters(), 8192);
    TestTrue(TEXT("Old JSON without distance remains readable"), Restored->Deserialization(MakeShared<FJsonObject>()));
    TestEqual(TEXT("Missing JSON field preserves the default"), Restored->GetClampedMaxRenderDistanceMeters(), 8192);
    TestEqual(TEXT("Minimum clamp"), NormalizeMaxRenderDistanceMeters(MIN_int32), 1024);
    TestEqual(TEXT("Maximum clamp"), NormalizeMaxRenderDistanceMeters(MAX_int32), 32768);
    TestEqual(TEXT("Edited values snap to 1024 m"), NormalizeMaxRenderDistanceMeters(1800), 2048);
    for (int32 Meters = 1024; Meters <= 32768; Meters += 1024)
    {
        Settings->MaxRenderDistanceMeters = Meters;
        TestTrue(TEXT("Round trip reads"), Restored->Deserialization(Settings->Serialization()));
        TestEqual(TEXT("Round trip retains each option"), Restored->MaxRenderDistanceMeters, Meters);
        TestEqual(TEXT("MPC and streaming use cm"), Restored->GetMaxRenderDistanceCentimeters(), float(Meters * 100));
    }
    Settings->MaxRenderDistanceMeters = 1800;
    TestTrue(TEXT("Unsnapped JSON is normalized on write"), Restored->Deserialization(Settings->Serialization()));
    TestEqual(TEXT("Serialized option is legal"), Restored->MaxRenderDistanceMeters, 2048);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DMaxRenderDistanceMenuTest,
    "V3DSimulator.UI.MaxRenderDistance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DMaxRenderDistanceMenuTest::RunTest(const FString&)
{
    TStrongObjectPtr<USettingsMenuWidget> Menu(NewObject<USettingsMenuWidget>());
    const ESettingsField Field = ESettingsField::MaxRenderDistanceMeters;
    const TArray<FText> Options = Menu->GetSettingOptionTexts(Field);
    TestEqual(TEXT("Exactly 32 distance choices"), Options.Num(), 32);
    if (Options.Num() != 32) return false;
    for (int32 Index = 0; Index < Options.Num(); ++Index)
    {
        const FString Expected = FString::Printf(TEXT("%d m"), (Index + 1) * 1024);
        TestEqual(TEXT("Choice spacing and metre labels"), Options[Index].ToString(), Expected);
        Menu->SetSettingFromDropdownSelection(Field, Expected);
        TestEqual(TEXT("Choice updates pending value"), Menu->GetPendingSettingValueText(Field).ToString(), Expected);
    }
    Menu->CycleMaxRenderDistanceMetersFromUI();
    TestEqual(TEXT("Legacy cycle button wraps to minimum"),
        Menu->GetPendingSettingValueText(Field).ToString(), FString(TEXT("1024 m")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DQualityProfilesTest,
    "V3DSimulator.Settings.QualityProfiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DQualityProfilesTest::RunTest(const FString&)
{
    TStrongObjectPtr<UGameSettings> Settings(NewObject<UGameSettings>());
    TStrongObjectPtr<UGameSettings> Restored(NewObject<UGameSettings>());
    TStrongObjectPtr<USettingsMenuWidget> Menu(NewObject<USettingsMenuWidget>());
    const TArray<FText> Options = Menu->GetSettingOptionTexts(ESettingsField::QualityProfile);
    if (!TestEqual(TEXT("Four presets and custom display"), Options.Num(), 5)) return false;
    Settings->Exposure = 2.0f;
    Settings->CelShadingMode = 0.0f;
    for (int32 Q = 0; Q < 4; ++Q)
    {
        Settings->ApplyQualityProfile(static_cast<EQualitySettings>(Q));
        TestEqual(TEXT("Preset recognized"), Settings->GetQualityProfileIndex(), Q);
        TestEqual(TEXT("Preset render budget"), Settings->GetClampedMaxRenderDistanceMeters(), 1024 << Q);
        TestEqual(TEXT("Preset preserves exposure"), Settings->Exposure, 2.0f);
        TestEqual(TEXT("Preset preserves art style"), Settings->CelShadingMode, 0.0f);
        TestTrue(TEXT("Preset serializes"), Restored->Deserialization(Settings->Serialization()));
        TestEqual(TEXT("Preset survives reload"), Restored->GetQualityProfileIndex(), Q);
        Menu->SetSettingFromDropdownSelection(ESettingsField::QualityProfile, Options[Q].ToString());
        TestEqual(TEXT("Menu recognizes preset"), Menu->GetPendingSettingValueText(ESettingsField::QualityProfile).ToString(), Options[Q].ToString());
        TestEqual(TEXT("Menu distance follows preset"), Menu->GetPendingSettingValueText(ESettingsField::MaxRenderDistanceMeters).ToString(),
            FString::Printf(TEXT("%d m"), 1024 << Q));
    }
    Settings->MaxRenderDistanceMeters = 32768;
    TestEqual(TEXT("Manual changes become custom"), Settings->GetQualityProfileIndex(), INDEX_NONE);
    Menu->SetSettingFromDropdownSelection(ESettingsField::MaxRenderDistanceMeters, TEXT("32768 m"));
    TestEqual(TEXT("Menu recognizes manual customization"),
        Menu->GetPendingSettingValueText(ESettingsField::QualityProfile).ToString(), FString(TEXT("Custom")));
    return true;
}
#endif
