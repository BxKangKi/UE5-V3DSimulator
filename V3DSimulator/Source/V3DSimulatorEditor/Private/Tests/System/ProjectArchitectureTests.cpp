// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "System/JsonMetadata.h"
#include "System/JsonMetadataNormalizer.h"
#include "System/ProjectBuildPolicy.h"
#include "System/ProjectConfig.h"
#include "System/ProjectWorkspace.h"
#include "System/SafeFileIO.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace ProjectArchitectureTestsPrivate
{
    FString MakeTempRoot()
    {
        return FPaths::Combine(
            FPaths::ProjectSavedDir(), TEXT("Automation"),
            FString::Printf(TEXT("ProjectArchitecture-%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectConfigCanonicalizationTest,
    "V3DSimulator.Project.ConfigCanonicalization",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProjectConfigCanonicalizationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString Root = ProjectArchitectureTestsPrivate::MakeTempRoot();
    IFileManager::Get().MakeDirectory(*Root, true);
    const FString ConfigPath = FPaths::Combine(Root, TEXT("config.json"));

    const TSharedRef<FJsonObject> LegacyLike = MakeShared<FJsonObject>();
    LegacyLike->SetNumberField(TEXT("CustomValue"), 42.0);
    TestTrue(TEXT("Seed config saves"),
        FSafeFileIO::SaveJsonBlocking(LegacyLike, ConfigPath).IsSuccess());

    FV3DSimulatorProjectConfig Config;
    TSharedPtr<FJsonObject> Canonical;
    FString Error;
    TestTrue(TEXT("Config loads and canonicalizes"),
        V3DSimulatorProjectConfig::Load(ConfigPath, TEXT("ExampleWorld"), Config, &Canonical, Error));
    TestTrue(TEXT("Canonical DOM exists"), Canonical.IsValid());
    if (Canonical.IsValid())
    {
        TestTrue(TEXT("UUID exists"), Canonical->HasField(V3DSimulatorJsonMetadata::UUID));
        TestEqual(TEXT("Name uses folder fallback"),
            Canonical->GetStringField(V3DSimulatorJsonMetadata::Name), FString(TEXT("ExampleWorld")));
        TestEqual(TEXT("Version is canonical"),
            Canonical->GetStringField(V3DSimulatorJsonMetadata::Version),
            FString(V3DSimulatorJsonMetadata::SchemaVersion));
        TestEqual(TEXT("ProjectType defaults to World"),
            Canonical->GetStringField(V3DSimulatorJsonMetadata::ProjectType), FString(TEXT("World")));
        TestFalse(TEXT("config never contains AssetType"),
            Canonical->HasField(V3DSimulatorJsonMetadata::AssetType));
        TestTrue(TEXT("Unknown project-specific fields are preserved"), Canonical->HasField(TEXT("CustomValue")));
    }

    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetJsonCanonicalizationTest,
    "V3DSimulator.Project.AssetJsonCanonicalization",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetJsonCanonicalizationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString Root = ProjectArchitectureTestsPrivate::MakeTempRoot();
    IFileManager::Get().MakeDirectory(*Root, true);
    const FString JsonPath = FPaths::Combine(Root, TEXT("House.json"));

    const TSharedRef<FJsonObject> Existing = MakeShared<FJsonObject>();
    Existing->SetStringField(TEXT("AuthorNote"), TEXT("keep-me"));
    TestTrue(TEXT("Seed asset JSON saves"),
        FSafeFileIO::SaveJsonBlocking(Existing, JsonPath).IsSuccess());

    bool bChanged = false;
    FString Error;
    TestTrue(TEXT("Existing model JSON upgrades"),
        V3DSimulatorJsonMetadataNormalizer::EnsureAssetJson(
            JsonPath, TEXT("House"), EAssetDefinitionType::Model,
            EModelDefinitionType::Static, bChanged, Error));
    TestTrue(TEXT("Upgrade reports mutation"), bChanged);

    const FSafeJsonLoadResult Loaded = FSafeFileIO::LoadJsonBlocking(JsonPath);
    TestTrue(TEXT("Upgraded JSON reloads"), Loaded.IsSuccess() && Loaded.JsonObject.IsValid());
    if (Loaded.JsonObject.IsValid())
    {
        TestTrue(TEXT("UUID exists"), Loaded.JsonObject->HasField(V3DSimulatorJsonMetadata::UUID));
        TestEqual(TEXT("Name is generated"), Loaded.JsonObject->GetStringField(V3DSimulatorJsonMetadata::Name), FString(TEXT("House")));
        TestEqual(TEXT("AssetType is Model"), Loaded.JsonObject->GetStringField(V3DSimulatorJsonMetadata::AssetType), FString(TEXT("Model")));
        TestEqual(TEXT("ModelType is Static"), Loaded.JsonObject->GetStringField(V3DSimulatorJsonMetadata::ModelType), FString(TEXT("Static")));
        TestFalse(TEXT("asset JSON never contains ProjectType"), Loaded.JsonObject->HasField(V3DSimulatorJsonMetadata::ProjectType));
        TestEqual(TEXT("Unknown authored field is preserved"), Loaded.JsonObject->GetStringField(TEXT("AuthorNote")), FString(TEXT("keep-me")));
    }

    const TSharedRef<FJsonObject> Conflict = MakeShared<FJsonObject>();
    Conflict->SetStringField(V3DSimulatorJsonMetadata::AssetType, TEXT("Sound"));
    TestTrue(TEXT("Conflicting seed saves"),
        FSafeFileIO::SaveJsonBlocking(Conflict, JsonPath).IsSuccess());
    bChanged = false;
    Error.Reset();
    TestFalse(TEXT("Conflicting AssetType is rejected, not silently rewritten"),
        V3DSimulatorJsonMetadataNormalizer::EnsureAssetJson(
            JsonPath, TEXT("House"), EAssetDefinitionType::Model,
            EModelDefinitionType::Static, bChanged, Error));

    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectBuildPolicyTest,
    "V3DSimulator.Project.BuildPolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProjectBuildPolicyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    TestTrue(TEXT("World accepts Static"),
        V3DSimulatorProjectBuildPolicy::AllowsModelType(EV3DSimulatorProjectType::World, EModelDefinitionType::Static));
    TestTrue(TEXT("World accepts Dynamic"),
        V3DSimulatorProjectBuildPolicy::AllowsModelType(EV3DSimulatorProjectType::World, EModelDefinitionType::Dynamic));
    TestTrue(TEXT("World accepts Character"),
        V3DSimulatorProjectBuildPolicy::AllowsModelType(EV3DSimulatorProjectType::World, EModelDefinitionType::Character));
    TestTrue(TEXT("Prefab accepts Static"),
        V3DSimulatorProjectBuildPolicy::AllowsModelType(EV3DSimulatorProjectType::Prefab, EModelDefinitionType::Static));
    TestFalse(TEXT("Prefab rejects Dynamic"),
        V3DSimulatorProjectBuildPolicy::AllowsModelType(EV3DSimulatorProjectType::Prefab, EModelDefinitionType::Dynamic));
    TestTrue(TEXT("Character accepts Character"),
        V3DSimulatorProjectBuildPolicy::AllowsModelType(EV3DSimulatorProjectType::Character, EModelDefinitionType::Character));
    TestTrue(TEXT("Dynamic accepts Dynamic"),
        V3DSimulatorProjectBuildPolicy::AllowsModelType(EV3DSimulatorProjectType::Dynamic, EModelDefinitionType::Dynamic));

    const FString ProjectResources = V3DSimulatorProjectWorkspace::ProjectResourcesRoot(TEXT("ExamplePrefab"));
    TestTrue(TEXT("Authoring assets belong to Projects/<Project>/resources"),
        ProjectResources.EndsWith(TEXT("Projects/ExamplePrefab/resources"))
        || ProjectResources.EndsWith(TEXT("Projects\\ExamplePrefab\\resources")));

    const FString ArchivePath = V3DSimulatorProjectWorkspace::AssetArchivePath(TEXT("ExamplePrefab"));
    TestTrue(TEXT("Built asset archive belongs to top-level Resources/<Project>.v3d"),
        ArchivePath.EndsWith(TEXT("Resources/ExamplePrefab.v3d"))
        || ArchivePath.EndsWith(TEXT("Resources\\ExamplePrefab.v3d")));
    return true;
}
#endif
