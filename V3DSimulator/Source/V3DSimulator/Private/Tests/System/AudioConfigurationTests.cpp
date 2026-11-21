// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "Misc/AutomationTest.h"
#include "System/ProjectScripts.h"
#include "System/SafeFileIO.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/V3DSoundPlayback.h"
#include "System/SimulationCommandObject.h"
#include "World/WorldEnvironmentConfig.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "UObject/StrongObjectPtr.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DAudioConfigurationTest, "V3DSimulator.Audio.SoundsJsonPhysicsAndLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DAudioConfigurationTest::RunTest(const FString&)
{
    FV3DProjectScripts Scripts;
    Scripts.SoundsJson = TEXT(R"({
      "PhysicalMaterials":{"Stone":{"Friction":0.9,"Restitution":0.2},"Grass":{}},
      "DefaultPhysicalMaterial":"Stone",
      "MaterialPhysicalMappings":[{"Material":"*","PhysicalMaterial":"Stone"},{"Material":"Floor_Grass","PhysicalMaterial":"Grass"}],
      "SoundLists":{"Steps":{"Sounds":["v3d://sound/11111111-1111-1111-1111-111111111111","v3d://sound/22222222-2222-2222-2222-222222222222"],"Loop":true}},
      "Events":{"Walk":{"SoundList":"Steps","Loop":false},"Slide":{"SoundList":"Steps"}},
      "FootstepMappings":{"Stone":{"SoundList":"Steps","Loop":false},"Grass":{"Event":"Slide","Volume":0.4}}
    })");
    FV3DWorldSurfaceConfig Surface; FV3DSimulationCommandConfig Commands; FString Error;
    if (!TestTrue(TEXT("All surface/sound settings work in sounds.json alone"), Scripts.Parse(Surface, Commands, Error))) { AddError(Error); return false; }
    TestEqual(TEXT("Physical coefficient"), Surface.PhysicalMaterials.FindChecked(TEXT("Stone")).Friction, .9f);
    TestEqual(TEXT("Exact name wins over wildcard"), Surface.ResolveMaterial(TEXT("LOD_0_Section_2_Floor_Grass")), FName(TEXT("Grass")));
    TestFalse(TEXT("Event overrides list loop"), Surface.Events.FindChecked(TEXT("Walk")).bLoop);
    TestTrue(TEXT("Absent event Loop inherits list"), Surface.Events.FindChecked(TEXT("Slide")).bLoop);
    TestFalse(TEXT("Footstep can override list loop"), Surface.FootstepMappings.FindChecked(TEXT("Stone")).bLoop);
    TestTrue(TEXT("Footstep inherits event loop"), Surface.FootstepMappings.FindChecked(TEXT("Grass")).bLoop);
    TestEqual(TEXT("Named lists preserve random candidates"), Surface.FootstepMappings.FindChecked(TEXT("Stone")).Sounds.Num(), 2);
    TestEqual(TEXT("Footstep overrides event volume"), Surface.FootstepMappings.FindChecked(TEXT("Grass")).Volume, .4f);
    const FString Valid = Scripts.SoundsJson;
    Scripts.SoundsJson.ReplaceInline(TEXT("\"Loop\":true"), TEXT("\"Loop\":\"true\""));
    TestFalse(TEXT("String is not a loop boolean"), Scripts.Parse(Surface, Commands, Error));
    Scripts.SoundsJson = Valid;
    Scripts.MaterialsJson = TEXT(R"({"PhysicalMaterials":{"Ignored":{}}})");
    TestFalse(TEXT("Physics outside sounds.json is rejected"), Scripts.Parse(Surface, Commands, Error));
    Scripts.MaterialsJson = TEXT("{}");
    TestTrue(TEXT("Current script schema parses"), Scripts.Parse(Surface, Commands, Error));
    Scripts.SoundsJson = TEXT(R"({"PhysicalMaterials":{"Stone":{}},"FootstepMappings":{"Stone":{"Sounds":[],"Event":"Missing"}}})");
    TestFalse(TEXT("Multiple footstep sources rejected"), Scripts.Parse(Surface, Commands, Error));

    FV3DEnvironmentConfig Environment;
    auto Json = FSafeFileIO::ParseJsonText(TEXT(R"({"Audio":{"WaterLoop":false,"EnvironmentMusicLoop":true}})"), TEXT("environment"));
    TestTrue(TEXT("Channel loop overrides parse"), FV3DEnvironmentConfig::Parse(Json.JsonObject, Environment, Error));
    TestTrue(TEXT("Explicit false is retained"), Environment.Audio.WaterLoop.IsSet() && !Environment.Audio.WaterLoop.GetValue());
    TestFalse(TEXT("Unspecified loop inherits defaults"), Environment.Audio.DefaultFootstepLoop.IsSet());
    Json.JsonObject->GetObjectField(TEXT("Audio"))->SetStringField(TEXT("WaterLoop"), TEXT("false"));
    TestFalse(TEXT("Invalid channel loop rejected"), FV3DEnvironmentConfig::Parse(Json.JsonObject, Environment, Error));

    auto CrossfadeJson = FSafeFileIO::ParseJsonText(TEXT(R"({"Audio":{"bBiomeAudioCrossfade":true,"BiomeCrossfadeSeconds":2.5}})"), TEXT("crossfade"));
    TestTrue(TEXT("Biome audio crossfade settings parse"), FV3DEnvironmentConfig::Parse(CrossfadeJson.JsonObject, Environment, Error));
    TestTrue(TEXT("Crossfade enabled"), Environment.Audio.bBiomeAudioCrossfade);
    TestEqual(TEXT("Crossfade duration preserved"), Environment.Audio.BiomeCrossfadeSeconds, 2.5f);
    CrossfadeJson.JsonObject->GetObjectField(TEXT("Audio"))->SetNumberField(TEXT("BiomeCrossfadeSeconds"), -1.0);
    TestFalse(TEXT("Negative crossfade duration rejected"), FV3DEnvironmentConfig::Parse(CrossfadeJson.JsonObject, Environment, Error));

    TStrongObjectPtr<USimulationCommandObject> Stop(NewObject<USimulationCommandObject>());
    TestTrue(TEXT("Loop stop command parses"), Stop->InitializeFromJson(TEXT(R"({"Name":"stop_slide","Actions":[{"Type":"sound_event","Event":"Slide","Stop":true}]})"), Error));
    TestTrue(TEXT("Loop stop survives serialization"), Stop->ToJson().Contains(TEXT("\"Stop\": true")) || Stop->ToJson().Contains(TEXT("\"Stop\":true")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DAudioDefaultsTest, "V3DSimulator.Audio.UnifiedDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DAudioDefaultsTest::RunTest(const FString&)
{
    TStrongObjectPtr<UV3DSimulatorAssetRegistry> Registry(NewObject<UV3DSimulatorAssetRegistry>());
    TStrongObjectPtr<USoundWave> A(NewObject<USoundWave>()), B(NewObject<USoundWave>());
    Registry->AudioDefaults.Footstep.Sounds = {TSoftObjectPtr<USoundBase>(A.Get()), TSoftObjectPtr<USoundBase>(B.Get())};
    Registry->AudioDefaults.Footsteps.AddDefaulted_GetRef().Sounds.Add(TSoftObjectPtr<USoundBase>(B.Get()));
    Registry->EnsureMenuDefaults();
    TestEqual(TEXT("Random candidates remain authored"), Registry->AudioDefaults.Footstep.Sounds.Num(), 2);
    TestEqual(TEXT("Physical bindings share one group"), Registry->AudioDefaults.Footsteps.Num(), 1);
    TestFalse(TEXT("Footsteps default to one-shot"), Registry->AudioDefaults.Footstep.bLoop);
    TestTrue(TEXT("Ambience defaults to loop"), Registry->AudioDefaults.Water.bLoop);
    Registry->AudioDefaults.Footstep.Sounds.Empty();
    Registry->EnsureMenuDefaults();
    TestTrue(TEXT("Empty assignments stay empty"), Registry->AudioDefaults.Footstep.Sounds.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DAudioPrivateLoopTest, "V3DSimulator.Audio.LoopDoesNotMutateSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DAudioPrivateLoopTest::RunTest(const FString&)
{
    TStrongObjectPtr<USoundWave> Source(NewObject<USoundWave>());
    Source->Duration = 1.25f; Source->bLooping = true;
    TStrongObjectPtr<USoundCue> Once(Cast<USoundCue>(V3DSoundPlayback::CreateAsset(GetTransientPackage(), Source.Get(), false)));
    TStrongObjectPtr<USoundCue> Loop(Cast<USoundCue>(V3DSoundPlayback::CreateAsset(GetTransientPackage(), Source.Get(), true)));
    if (!TestNotNull(TEXT("One-shot wrapper"), Once.Get()) || !TestNotNull(TEXT("Loop wrapper"), Loop.Get())) return false;
    const auto* OncePlayer = Cast<USoundNodeWavePlayer>(Once->FirstNode);
    const auto* LoopPlayer = Cast<USoundNodeWavePlayer>(Loop->FirstNode);
    TestTrue(TEXT("Different playbacks have independent loop flags"), OncePlayer && LoopPlayer && !OncePlayer->bLooping && LoopPlayer->bLooping);
    TestTrue(TEXT("Original cooked wave not edited"), Source->bLooping);
    TStrongObjectPtr<USoundCue> Clone(Cast<USoundCue>(V3DSoundPlayback::CreateAsset(GetTransientPackage(), Loop.Get(), false)));
    if (!TestNotNull(TEXT("Cue playback cloned"), Clone.Get())) return false;
    const auto* ClonePlayer = Cast<USoundNodeWavePlayer>(Clone->FirstNode);
    TestTrue(TEXT("Looping cue can play once without changing the source graph"), ClonePlayer && !ClonePlayer->bLooping && LoopPlayer->bLooping && ClonePlayer != LoopPlayer);
    return true;
}
#endif
