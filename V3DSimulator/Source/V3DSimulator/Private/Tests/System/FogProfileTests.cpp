// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "Misc/AutomationTest.h"
#include "World/WorldFogConfig.h"
#include "World/WorldData.h"
#include "System/SafeFileIO.h"
#include "System/ProjectEditorDocument.h"
#include "System/ProjectWorkspace.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DFogValidationTest,
    "V3DSimulator.Environment.Fog.ValidationAndInheritance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DFogValidationTest::RunTest(const FString&)
{
    FString Error;
    FV3DWorldFogConfig Config;
    TestEqual(TEXT("Clear default density is reduced"), Config.Global[V3DFog::Density], .0005f);
    TestEqual(TEXT("Rain default density is reduced"), Config.Resolve(NAME_None, TEXT("rain"), 1.f)[V3DFog::Density], .0015f);
    TestEqual(TEXT("Snow default density is reduced"), Config.Resolve(NAME_None, TEXT("snow"), 1.f)[V3DFog::Density], .0025f);
    const auto Good = FSafeFileIO::ParseJsonText(TEXT(R"JSON({"Version":1,"TransitionSeconds":2,
      "Global":{"Density":0.02,"HeightFalloff":0.001,"ScatteringDistribution":-0.9},
      "BiomeProfiles":{"Forest":{"Density":0.06,"Color":[0.2,0.3,0.4]}},
      "WeatherProfiles":{"clear":{},"rain":{"Density":0.08}}})JSON"), TEXT("fog-test"));
    if (!TestTrue(TEXT("Valid boundaries and partial overrides parse"), FV3DWorldFogConfig::Parse(Good.JsonObject, Config, Error))) return false;
    const auto Original = Config.Global;
    const auto Rain = Config.Resolve(TEXT("Forest"), TEXT("rain"), .5f);
    TestTrue(TEXT("Weather blends over biome density"), FMath::IsNearlyEqual(Rain[V3DFog::Density], .07f));
    TestEqual(TEXT("Weather omission inherits biome color"), Rain[V3DFog::ColorG], .3f);
    TestEqual(TEXT("Leaving biome restores global"), Config.Resolve(NAME_None, TEXT("clear"), 1.f)[V3DFog::Density], .02f);
    TestTrue(TEXT("Resolution never writes global"), Original == Config.Global);
    FV3DWorldFogConfig RoundTrip;
    TestTrue(TEXT("Compiled snapshot parses"), FV3DWorldFogConfig::Parse(Config.ToJson(), RoundTrip, Error));
    TestTrue(TEXT("Compiled snapshot has same effective profile"), RoundTrip.Resolve(TEXT("Forest"), TEXT("rain"), .5f) == Rain);
    for (const TCHAR* Bad : {
        TEXT("{\"Version\":2}"), TEXT("{\"TransitionSeconds\":-1}"), TEXT("{\"Global\":[]}"),
        TEXT("{\"Global\":{\"Density\":-0.1}}"), TEXT("{\"Global\":{\"HeightFalloff\":0}}"),
        TEXT("{\"Global\":{\"Enabled\":1}}"), TEXT("{\"Global\":{\"Color\":[1,1]}}"),
        TEXT("{\"Global\":{\"Enabled\":\"true\"}}"), TEXT("{\"Global\":{\"Density\":\"0.02\"}}"),
        TEXT("{\"Global\":{\"ViewDistanceCm\":200001}}"), TEXT("{\"Global\":{\"Densitty\":0.1}}"),
        TEXT("{\"WeatherProfiles\":{\"sandstorm\":{}}}"),
        TEXT("{\"WeatherProfiles\":{\"rain\":{\"Enabled\":false}}}"),
        TEXT("{\"BiomeProfiles\":{\"Forest\":{},\"forest\":{}}}"), TEXT("{\"BiomeProfiles\":{\" Forest\":{}}}")})
    {
        const auto Json = FSafeFileIO::ParseJsonText(Bad, TEXT("bad-fog"));
        TestFalse(Bad, FV3DWorldFogConfig::Parse(Json.JsonObject, Config, Error));
        TestTrue(TEXT("Failure explains the fog field"), Error.StartsWith(TEXT("scripts/environments.json:")));
        TestTrue(TEXT("Invalid input cannot partly overwrite active config"), Config.Global == Original);
    }
    // Exercise diagnostics and whitespace validation with keys produced by the engine JSON
    // parser (FSharedString in UE 5.8), rather than constructing a separate FString map.
    for (const auto& Case : {
        TPair<const TCHAR*, const TCHAR*>(TEXT("{\"Unexpected\":true}"), TEXT("scripts/environments.json: Unexpected is not supported.")),
        TPair<const TCHAR*, const TCHAR*>(TEXT("{\"BiomeProfiles\":{\"Forest \":{}}}"), TEXT("scripts/environments.json: BiomeProfiles.Forest  has an invalid name.")),
        TPair<const TCHAR*, const TCHAR*>(TEXT("{\"WeatherProfiles\":{\" rain\":{}}}"), TEXT("scripts/environments.json: WeatherProfiles. rain has an invalid name.")),
        TPair<const TCHAR*, const TCHAR*>(TEXT("{\"BiomeProfiles\":{\"Forest\":{\"Densitty\":0.1}}}"),
            TEXT("scripts/environments.json: BiomeProfiles.Forest.Densitty is not a supported profile field."))})
    {
        const auto Json = FSafeFileIO::ParseJsonText(Case.Key, TEXT("shared-key-fog"));
        TestFalse(TEXT("Invalid shared JSON key rejected"), FV3DWorldFogConfig::Parse(Json.JsonObject, Config, Error));
        TestEqual(TEXT("Diagnostic preserves the full original key path"), Error, FString(Case.Value));
        TestTrue(TEXT("Rejected key preserves the previous profile"), Config.Global == Original);
    }
    auto NonFinite = MakeShared<FJsonObject>();
    auto Base = MakeShared<FJsonObject>();
    Base->SetNumberField(TEXT("Density"), std::numeric_limits<double>::infinity());
    NonFinite->SetObjectField(TEXT("Global"), Base);
    TestFalse(TEXT("Infinity rejected"), FV3DWorldFogConfig::Parse(NonFinite, Config, Error));
    auto Many = MakeShared<FJsonObject>();
    for (int32 I = 0; I < 257; ++I) Many->SetObjectField(FString::Printf(TEXT("Biome%d"), I), MakeShared<FJsonObject>());
    NonFinite->RemoveField(TEXT("Global")); NonFinite->SetObjectField(TEXT("BiomeProfiles"), Many);
    TestFalse(TEXT("Biome profile count bounded"), FV3DWorldFogConfig::Parse(NonFinite, Config, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DFogAuthoringTest,
    "V3DSimulator.Environment.Fog.AuthoringAndRuntimeSnapshot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DFogAuthoringTest::RunTest(const FString&)
{
    FString Error;
    FV3DWorldFogConfig Config;
    const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Fog-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Path = Root / FV3DWorldFogConfig::MemberName();
    IFileManager::Get().MakeDirectory(*Root, true);
    TestTrue(TEXT("Missing environment script uses defaults"), FV3DWorldFogConfig::Load(Root, Config, Error));
    TestTrue(TEXT("New project creates scripts/environments.json"), FV3DWorldFogConfig::CreateDefaultFile(Root, Error));
    FString Before, After; FFileHelper::LoadFileToString(Before, *Path);
    TestTrue(TEXT("Created config validates"), FV3DWorldFogConfig::Load(Root, Config, Error));
    const auto Baseline = Config.Global;
    auto WorldJson = MakeShared<FJsonObject>();
    WorldJson->SetObjectField(TEXT("Fog"), Config.ToJson());
    UWorldData* Data = NewObject<UWorldData>();
    TestTrue(TEXT("Runtime reads compiled Fog snapshot"), UWorldData::DeserializeData(Data, WorldJson));
    TestTrue(TEXT("Snapshot preserves global"), Data->Fog.Global == Baseline);
    Data->Fog.Resolve(NAME_None, TEXT("rain"), 1.f);
    TestTrue(TEXT("Default creation does not overwrite authored files"), FV3DWorldFogConfig::CreateDefaultFile(Root, Error));
    FFileHelper::LoadFileToString(After, *Path);
    TestEqual(TEXT("Runtime and repeated creation cannot rewrite environments.json"), After, Before);
    TestTrue(TEXT("Old archive without snapshot still loads"), UWorldData::DeserializeData(Data, MakeShared<FJsonObject>()));
    WorldJson->SetStringField(TEXT("Fog"), TEXT("invalid"));
    TestFalse(TEXT("Malformed embedded snapshot rejected"), FV3DWorldFogConfig::ParseWorldConfig(WorldJson, Config, Error));

    FV3DEditorDocument Doc;
    Doc.Path = FSafeFileIO::NormalizeFilePath(V3DSimulatorProjectWorkspace::ProjectRoot(TEXT("FogTest")) / FV3DWorldFogConfig::MemberName());
    Doc.Label = FV3DWorldFogConfig::MemberName();
    Doc.Json = FV3DWorldFogConfig().ToJson();
    TestTrue(TEXT("Editor validates fog documents"), V3DProjectEditor::Validate(Doc, TEXT("FogTest"), Error));
    TestTrue(TEXT("Editor exposes fog fields"), V3DProjectEditor::Suggestions(Doc)->HasField(TEXT("Global")));
    Doc.Label = TEXT("resources/environments.json");
    TestFalse(TEXT("A resource named environments.json keeps its own schema"), V3DProjectEditor::Suggestions(Doc)->HasField(TEXT("Global")));
    Doc.Label = FV3DWorldFogConfig::MemberName();
    Doc.Json->SetNumberField(TEXT("TransitionSeconds"), -5);
    TestFalse(TEXT("Editor rejects bad fog before save/build"), V3DProjectEditor::Validate(Doc, TEXT("FogTest"), Error));

    auto Invalid = MakeShared<FJsonObject>(); Invalid->SetNumberField(TEXT("Version"), 99);
    TestTrue(TEXT("Invalid script fixture saved"), FSafeFileIO::SaveJsonBlocking(Invalid, Path).IsSuccess());
    TestFalse(TEXT("Invalid script is rejected"), FV3DWorldFogConfig::Load(Root, Config, Error));
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}
#endif
