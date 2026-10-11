// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "System/WorldSurfaceConfig.h"
#include "System/SafeFileIO.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DWorldSurfaceConfigTest, "V3DSimulator.World.Surfaces.Config",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DWorldSurfaceConfigTest::RunTest(const FString&)
{
    const auto Json = FSafeFileIO::ParseJsonText(TEXT(R"({
        "PhysicalMaterials":{"Stone":{"Friction":0.7},"Grass":{}},
        "DefaultPhysicalMaterial":"Stone",
        "MaterialPhysicalMappings":[
            {"Material":"*","PhysicalMaterial":"Stone"},
            {"Material":"M_Grass","PhysicalMaterial":"Grass"}],
        "FootstepMappings":{"Grass":{"Sounds":["gworld://sound/12345678-1234-1234-1234-123456789abc"]}}
    })"), TEXT("surface-test"));
    FV3DWorldSurfaceConfig Config;
    FString Error;
    TestTrue(TEXT("Valid surface schema"), FV3DWorldSurfaceConfig::Parse(Json.JsonObject, Config, Error));
    TestEqual(TEXT("Exact rule precedes wildcard"), Config.ResolveMaterial(TEXT("M_Grass")), FName(TEXT("Grass")));
    TestEqual(TEXT("Original GLB material resolves through runtime slot prefix"),
        Config.ResolveMaterial(TEXT("LOD_12_Section_23_M_Grass")), FName(TEXT("Grass")));
    TestEqual(TEXT("Names that merely resemble a slot prefix are preserved"),
        Config.SourceMaterialName(TEXT("LOD_bad_Section_0_M_Grass")), FString(TEXT("LOD_bad_Section_0_M_Grass")));
    TestFalse(TEXT("Unbundled sound rejects build"), Config.ValidateSounds(TSet<FGuid>(), Error));
    TSet<FGuid> Sounds;
    FGuid UUID;
    FGuid::Parse(TEXT("12345678-1234-1234-1234-123456789abc"), UUID);
    Sounds.Add(UUID);
    TestTrue(TEXT("Bundled sound passes"), Config.ValidateSounds(Sounds, Error));
    Json.JsonObject->SetStringField(TEXT("DefaultPhysicalMaterial"), TEXT("Unknown"));
    TestFalse(TEXT("Unknown physical material is rejected"), FV3DWorldSurfaceConfig::Parse(Json.JsonObject, Config, Error));
    TestTrue(TEXT("Failed parse does not publish partial state"), Config.PhysicalMaterials.IsEmpty());
    return true;
}
#endif
