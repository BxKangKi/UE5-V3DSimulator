#include "Misc/AutomationTest.h"
#include "System/ProjectScripts.h"
#include "System/SafeFileIO.h"
#include "System/SimulationCommandObject.h"
#include "System/SoundEventSubsystem.h"
#include "UObject/StrongObjectPtr.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DSoundEventScriptTest, "V3DSimulator.Audio.SoundListsAndEvents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DSoundEventScriptTest::RunTest(const FString&)
{
    FV3DProjectScripts Scripts;
    Scripts.SoundsJson = TEXT(R"({"PhysicalMaterials":{"Stone":{}},"SoundLists":{"Steps":{"Sounds":["v3d://sound/11111111-1111-1111-1111-111111111111","v3d://sound/22222222-2222-2222-2222-222222222222"]}},"Events":{"Walk":{"SoundList":"Steps","Volume":0.7},"Run":{"SoundList":"Steps","PitchMin":1.1,"PitchMax":1.2}},"FootstepMappings":{"Stone":{"Event":"Walk"}}})");
    FV3DWorldSurfaceConfig Config; FV3DSimulationCommandConfig Commands; FString Error;
    if (!TestTrue(TEXT("Two events can reference one list"), Scripts.Parse(Config,Commands,Error))) { AddError(Error); return false; }
    TestEqual(TEXT("Independent events"),Config.Events.Num(),2);
    TestEqual(TEXT("Two candidates"),Config.SoundLists.FindChecked(TEXT("Steps")).Sounds.Num(),2);
    TestEqual(TEXT("Footstep maps to event"),Config.FootstepMappings.FindChecked(TEXT("Stone")).Event,FName(TEXT("Walk")));
    TSet<FGuid> Available;
    FGuid A,B; FGuid::Parse(TEXT("11111111-1111-1111-1111-111111111111"),A); FGuid::Parse(TEXT("22222222-2222-2222-2222-222222222222"),B);
    Available.Add(A); TestFalse(TEXT("Missing list resource is rejected"),Config.ValidateSounds(Available,Error));
    Available.Add(B); TestTrue(TEXT("All list resources resolve"),Config.ValidateSounds(Available,Error));
    Scripts.SoundsJson.ReplaceInline(TEXT("\"SoundList\":\"Steps\""),TEXT("\"SoundList\":\"Missing\""));
    TestFalse(TEXT("Dangling event list is rejected"),Scripts.Parse(Config,Commands,Error));
    Scripts.SoundsJson = TEXT(R"({"SoundLists":{"Empty":{"Sounds":[]}},"Events":{}})");
    TestFalse(TEXT("Empty lists rejected"),Scripts.Parse(Config,Commands,Error));
    Scripts.SoundsJson = TEXT(R"({"PhysicalMaterials":{"Stone":{}},"FootstepMappings":{"Stone":{"Sounds":["v3d://sound/11111111-1111-1111-1111-111111111111"]}}})");
    TestTrue(TEXT("Direct random footstep array is supported"),Scripts.Parse(Config,Commands,Error));
    TStrongObjectPtr<USimulationCommandObject> Command(NewObject<USimulationCommandObject>());
    const FString Text = TEXT(R"({"Name":"overlap","Actions":[{"Type":"sound_event","Event":"Walk"},{"Type":"sound_event","Event":"Run"}]})");
    TestTrue(TEXT("Sound command parses"),Command->InitializeFromJson(Text,Error));
    TStrongObjectPtr<USimulationCommandObject> RoundTrip(NewObject<USimulationCommandObject>());
    TestTrue(TEXT("Sound command survives JSON export/import"),RoundTrip->InitializeFromJson(Command->ToJson(),Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DSoundEventPcmCacheTest, "V3DSimulator.Audio.EventPcmCacheBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DSoundEventPcmCacheTest::RunTest(const FString&)
{
    TStrongObjectPtr<USoundEventSubsystem> Events(NewObject<USoundEventSubsystem>());
    const auto MakePcm = [](int32 Size) -> FV3DPcmPtr
    {
        auto Data = MakeShared<FV3DDecodedPcm16, ESPMode::ThreadSafe>();
        Data->Bytes.SetNumZeroed(Size); Data->SampleRate = 48000;
        Data->NumChannels = 1; Data->NumFrames = Size / 2;
        return Data;
    };
    const auto Small = MakePcm(1024);
    for (int32 Index = 0; Index < 65; ++Index) Events->CachePcm(FString::FromInt(Index), Small);
    TestEqual(TEXT("Entry count is bounded"), Events->PcmCache.Num(), 64);
    TestFalse(TEXT("Oldest entry evicted"), Events->PcmCache.Contains(TEXT("0")));
    const int64 Bytes = Events->CachedPcmBytes;
    Events->CachePcm(TEXT("64"), Small);
    TestEqual(TEXT("Duplicate completion is not counted twice"), Events->CachedPcmBytes, Bytes);
    Events->CachePcm(TEXT("Large"), MakePcm(32 * 1024 * 1024));
    TestEqual(TEXT("Byte budget evicts small entries"), Events->PcmCache.Num(), 1);
    TestEqual(TEXT("Exact byte accounting"), Events->CachedPcmBytes, 32ll * 1024 * 1024);
    Events->CachePcm(TEXT("TooLarge"), MakePcm(32 * 1024 * 1024 + 2));
    TestFalse(TEXT("Oversized entry rejected"), Events->PcmCache.Contains(TEXT("TooLarge")));
    TestEqual(TEXT("A caller retains its immutable samples after eviction"), Small->Bytes.Num(), 1024);
    return true;
}
#endif
