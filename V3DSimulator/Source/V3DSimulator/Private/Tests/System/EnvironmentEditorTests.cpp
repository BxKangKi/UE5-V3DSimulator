// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "Misc/AutomationTest.h"
#include "System/SafeFileIO.h"
#include "System/ProjectEditorDocument.h"
#include "System/ProjectWorkspace.h"
#include "System/EntityArchive.h"
#include "Simulator/ModelDefinitionJson.h"
#include "System/JsonMetadata.h"
#include "World/WorldEnvironmentConfig.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DBiomeConfigurationTest,
    "V3DSimulator.Environment.BiomeValidationAndBounds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DBiomeConfigurationTest::RunTest(const FString&)
{
    const FString Text = TEXT(R"JSON({"Audio":{"bGlobalAcoustics":true,"Water":"none"},"Biomes":[
      {"Name":"Forest","Min":[-10,-10,-10],"Max":[10,10,10]},
      {"Name":"Cave","Shape":"Sphere","Center":[0,0,0],"Radius":5,"Priority":10}]})JSON");
    auto Root = FSafeFileIO::ParseJsonText(Text, TEXT("Test"));
    FV3DEnvironmentConfig C; FString Error;
    TestTrue(TEXT("Parse bounded settings"), FV3DEnvironmentConfig::Parse(Root.JsonObject, C, Error));
    if (C.Biomes.Num() != 2) return false;
    TestEqual(TEXT("Higher priority wins"), C.Biomes[0].Name, FName(TEXT("Cave")));
    TestTrue(TEXT("Sphere boundary inclusive"), C.Biomes[0].Contains(FVector(5,0,0)));
    TestFalse(TEXT("Sphere excludes outside point"), C.Biomes[0].Contains(FVector(5.01,0,0)));
    TestTrue(TEXT("Box boundary inclusive"), C.Biomes[1].Contains(FVector(10,10,10)));
    auto Biomes = Root.JsonObject->GetArrayField(TEXT("Biomes")); Biomes.Add(Biomes[0]);
    Root.JsonObject->SetArrayField(TEXT("Biomes"), Biomes);
    TestFalse(TEXT("Duplicate names rejected"), FV3DEnvironmentConfig::Parse(Root.JsonObject, C, Error));
    Root = FSafeFileIO::ParseJsonText(TEXT("{\"Audio\":{\"Absorption\":0}}"), TEXT("Test"));
    TestFalse(TEXT("Zero absorption rejected"), FV3DEnvironmentConfig::Parse(Root.JsonObject, C, Error));
    Root = FSafeFileIO::ParseJsonText(TEXT("{\"Audio\":{\"Water\":\"local.wav\"}}"), TEXT("Test"));
    TestFalse(TEXT("Runtime paths must be archive refs"), FV3DEnvironmentConfig::Parse(Root.JsonObject, C, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DEditorAssetSchemaTest,
    "V3DSimulator.Editor.AssetSchemaAndUnknownFields", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DEditorAssetSchemaTest::RunTest(const FString&)
{
    FV3DEditorDocument Doc;
    Doc.Path = TEXT("/Example/resources/car.json"); Doc.SourcePath = TEXT("/Example/resources/car.glb");
    Doc.Json = MakeShared<FJsonObject>();
    Doc.Json->SetStringField(TEXT("AssetType"), TEXT("Model"));
    Doc.Json->SetStringField(TEXT("ModelType"), TEXT("Dynamic"));
    Doc.Json->SetStringField(TEXT("EntityType"), TEXT("Vehicle"));
    auto Fields = V3DProjectEditor::Suggestions(Doc);
    TestTrue(TEXT("Vehicle tuning exposed"), Fields->HasField(TEXT("VehicleTuning")));
    TestTrue(TEXT("Mass exposed"), Fields->HasField(TEXT("MassKg")));
    TestTrue(TEXT("Gravity exposed"), Fields->HasField(TEXT("GravityField")));
    Doc.Json->SetStringField(TEXT("UUID"), FGuid::NewGuid().ToString());
    Doc.Json->SetStringField(TEXT("Version"), V3DSimulatorJsonMetadata::SchemaVersion);
    Doc.Json->SetStringField(TEXT("Name"), TEXT("car")); Doc.Json->SetStringField(TEXT("DisplayName"), TEXT("Car"));
    Doc.Json->SetStringField(TEXT("AuthorCustomKey"), TEXT("preserve"));
    FModelDefinition Model; FString Error, Canonical;
    TestTrue(TEXT("In-memory model validation succeeds"), ModelDefinitionJson::ValidateDefinition(Doc.Json, Doc.Path, Doc.SourcePath, Model, Error, &Canonical));
    TestTrue(TEXT("Unknown author fields survive"), Canonical.Contains(TEXT("AuthorCustomKey")));
    Doc.Json->SetStringField(TEXT("ItemType"), TEXT("Weapon"));
    TestFalse(TEXT("Mutually exclusive subtype rejected before write"), ModelDefinitionJson::ValidateDefinition(Doc.Json, Doc.Path, Doc.SourcePath, Model, Error));
    Doc.Json->SetStringField(TEXT("AssetType"), TEXT("Sound"));
    Fields = V3DProjectEditor::Suggestions(Doc);
    TestFalse(TEXT("Sound has no ModelType suggestions"), Fields->HasField(TEXT("ModelType")));
    TestFalse(TEXT("Sound has no vehicle suggestions"), Fields->HasField(TEXT("VehicleTuning")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DResumeCommitOrderTest,
    "V3DSimulator.Persistence.FinalPlayerCommitWins", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DResumeCommitOrderTest::RunTest(const FString&)
{
    const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Resume-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FString Error; auto Store = FEntityArchiveStore::Open(Root, true, Error);
    if (!TestTrue(TEXT("Archive opens"), Store.IsValid())) return false;
    const FString Path = Store->GetPath();
    FWorldRuntimeState Old, New;
    Old.WorldTime = 100; New.WorldTime = 200; New.SelectedPlayer = TEXT("avatar-uuid");
    FWorldPlayerRecord Player; Player.PlayerId = TEXT("Player"); Player.Location = FVector(1234.5,-654.25,9000.75);
    Player.Rotation = FRotator(31,123,0); New.Players.Add(Player);
    const uint64 OlderOrder = Store->ReserveRuntimeStateWrite();
    const uint64 FinalOrder = Store->ReserveRuntimeStateWrite();
    TestTrue(TEXT("Final lifecycle commit succeeds"), Store->SaveRuntimeState(New, FinalOrder).IsSuccess());
    TestTrue(TEXT("Older autosave is superseded"), Store->SaveRuntimeState(Old, OlderOrder).IsSuccess());
    Store.Reset(); Store = FEntityArchiveStore::Open(Root, false, Error);
    FWorldRuntimeState Loaded; bool Missing = true;
    if (TestTrue(TEXT("State reopens"), Store.IsValid() && Store->LoadRuntimeState(Loaded, Missing, Error)) && Loaded.Players.Num() == 1)
    {
        TestFalse(TEXT("State exists"), Missing);
        TestTrue(TEXT("Exact airborne location retained"), Loaded.Players[0].Location.Equals(Player.Location, 1.e-6));
        TestTrue(TEXT("Look rotation retained"), Loaded.Players[0].Rotation.Equals(Player.Rotation, 1.e-6));
        TestEqual(TEXT("Final world time retained"), Loaded.WorldTime, 200.f);
    }
    else AddError(TEXT("Expected one persisted local player."));
    Store.Reset(); IFileManager::Get().Delete(*Path);
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}
#endif
