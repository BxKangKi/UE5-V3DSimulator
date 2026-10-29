// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Character/MouseLookPolicy.h"
#include "Setting/GameSettings.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DMouseSensitivityPersistenceTest,
    "V3DSimulator.Settings.MouseSensitivityPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DMouseSensitivityPersistenceTest::RunTest(const FString&)
{
    TStrongObjectPtr<UGameSettings> Settings(NewObject<UGameSettings>());
    TestEqual(TEXT("Default input speed preserves existing behavior"), Settings->MouseSensitivity, 1.0f);
    Settings->MouseSensitivity = 2.35f;
    Settings->MaxRenderDistanceMeters = 4096;
    FString Serialized;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    TestTrue(TEXT("Serialize real settings JSON"), FJsonSerializer::Serialize(Settings->Serialization(), Writer));
    TSharedPtr<FJsonObject> Parsed;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Serialized);
    if (!TestTrue(TEXT("Parse serialized settings"), FJsonSerializer::Deserialize(Reader, Parsed))) return false;
    TStrongObjectPtr<UGameSettings> Reloaded(NewObject<UGameSettings>());
    TestTrue(TEXT("Reload saved settings"), Reloaded->Deserialization(Parsed));
    TestTrue(TEXT("Sensitivity round trips"), FMath::IsNearlyEqual(Reloaded->MouseSensitivity, 2.35f));
    TestEqual(TEXT("Other settings survive the same document"), Reloaded->MaxRenderDistanceMeters, 4096);

    TStrongObjectPtr<UGameSettings> Legacy(NewObject<UGameSettings>());
    Parsed->RemoveField(TEXT("MouseSensitivity"));
    TestTrue(TEXT("Old documents remain readable"), Legacy->Deserialization(Parsed));
    TestEqual(TEXT("Old documents retain the default input speed"), Legacy->MouseSensitivity, 1.0f);
    Parsed->SetStringField(TEXT("MouseSensitivity"), TEXT("invalid"));
    TestTrue(TEXT("Wrongly typed optional input field does not discard graphics settings"), Legacy->Deserialization(Parsed));
    TestEqual(TEXT("Wrongly typed input field keeps safe speed"), Legacy->MouseSensitivity, 1.0f);
    Parsed->SetNumberField(TEXT("MouseSensitivity"), -10.0);
    Legacy->Deserialization(Parsed);
    TestEqual(TEXT("Hand-edited low input speed is clamped"), Legacy->MouseSensitivity, 0.05f);
    Parsed->SetNumberField(TEXT("MouseSensitivity"), 500.0);
    Legacy->Deserialization(Parsed);
    TestEqual(TEXT("Hand-edited high input speed is clamped"), Legacy->MouseSensitivity, 5.0f);
    Settings->MouseSensitivity = std::numeric_limits<float>::quiet_NaN();
    TestEqual(TEXT("Invalid Blueprint values serialize as a finite default"),
        Settings->Serialization()->GetNumberField(TEXT("MouseSensitivity")), 1.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DMouseSensitivityInputSafetyTest,
    "V3DSimulator.Input.MouseSensitivityAndFiniteDeltas",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DMouseSensitivityInputSafetyTest::RunTest(const FString&)
{
    float Yaw = 0.0f;
    float Pitch = 0.0f;
    const float OriginalYaw = 8.0f;
    const float OriginalPitch = -4.0f;
    TestTrue(TEXT("Normal camera delta accepted"), V3DMouseLook::TryScaleInput(
        OriginalYaw, OriginalPitch, V3DMouseLook::EffectiveSensitivity(0.5f, 2.0f), Yaw, Pitch));
    TestEqual(TEXT("Saved multiplier is applied once to controller yaw"), Yaw, OriginalYaw);
    TestEqual(TEXT("Both axes preserve direction"), Pitch, OriginalPitch);
    V3DMouseLook::TryScaleInput(OriginalYaw, OriginalPitch,
        V3DMouseLook::EffectiveSensitivity(0.5f, 3.0f), Yaw, Pitch);
    TestEqual(TEXT("Next input uses changed settings without rebinding"), Yaw, 12.0f);
    TestEqual(TEXT("Next input applies the same speed to pitch"), Pitch, -6.0f);
    TestFalse(TEXT("Infinite input rejected"), V3DMouseLook::TryScaleInput(
        std::numeric_limits<double>::infinity(), 1.0, 1.0f, Yaw, Pitch));
    TestFalse(TEXT("NaN input rejected"), V3DMouseLook::TryScaleInput(
        1.0, std::numeric_limits<double>::quiet_NaN(), 1.0f, Yaw, Pitch));
    TestFalse(TEXT("Double values that overflow float camera APIs rejected"), V3DMouseLook::TryScaleInput(
        std::numeric_limits<double>::max(), 1.0, 5.0f, Yaw, Pitch));
    TestEqual(TEXT("Rejected input clears yaw output"), Yaw, 0.0f);
    TestEqual(TEXT("Rejected input clears pitch output"), Pitch, 0.0f);
    TestEqual(TEXT("Invalid base speed falls back safely"), V3DMouseLook::EffectiveSensitivity(
        std::numeric_limits<float>::quiet_NaN(), 2.0f), 2.0f);
    TestEqual(TEXT("Overflowing legacy speed falls back safely"), V3DMouseLook::EffectiveSensitivity(
        std::numeric_limits<float>::max(), 5.0f), 1.0f);
    return true;
}
#endif
