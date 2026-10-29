// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "System/WorldSurfaceConfig.h"
#include "System/SafeFileIO.h"
#include "System/WorldSurfaceSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/StrongObjectPtr.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DWorldSurfaceMaterialReuseTest,
    "V3DSimulator.World.Surfaces.RepeatedApplicationReusesMaterial",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DWorldSurfaceMaterialReuseTest::RunTest(const FString&)
{
    const UWorld::InitializationValues Values = UWorld::InitializationValues()
        .AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false)
        .CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None,
        nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("Test world"), World)) return false;
    TStrongObjectPtr<UWorldSurfaceSubsystem> Surfaces(NewObject<UWorldSurfaceSubsystem>(World));
    FV3DWorldSurfaceConfig Config;
    Config.DefaultPhysicalMaterial = TEXT("Stone");
    Config.PhysicalMaterials.Add(TEXT("Stone"), FV3DSurfaceDefinition());
    FString Error;
    TestTrue(TEXT("Configure surface"), Surfaces->Configure(Config, Error));

    TStrongObjectPtr<UStaticMesh> Mesh(NewObject<UStaticMesh>());
    UMaterialInterface* Base = UMaterial::GetDefaultMaterial(MD_Surface);
    Mesh->GetStaticMaterials().Add(FStaticMaterial(Base, TEXT("Stone")));
    TStrongObjectPtr<UStaticMeshComponent> First(NewObject<UStaticMeshComponent>(World));
    TStrongObjectPtr<UStaticMeshComponent> Second(NewObject<UStaticMeshComponent>(World));
    First->SetStaticMesh(Mesh.Get());
    Second->SetStaticMesh(Mesh.Get());
    Surfaces->ApplyToStaticMesh(First.Get());
    UMaterialInstanceDynamic* Wrapped = Cast<UMaterialInstanceDynamic>(First->GetMaterial(0));
    TestNotNull(TEXT("First application creates the physical material wrapper"), Wrapped);
    Surfaces->ApplyToStaticMesh(Second.Get());
    TestTrue(TEXT("Components using the same source share the wrapper"), Second->GetMaterial(0) == Wrapped);
    for (int32 Index = 0; Index < 256; ++Index)
    {
        Surfaces->ApplyToStaticMesh(First.Get());
        Surfaces->ApplyToStaticMesh(Second.Get());
    }
    TestTrue(TEXT("Repeated application retains the original material"), First->GetMaterial(0) == Wrapped);
    TestTrue(TEXT("Repeated application retains sharing"), Second->GetMaterial(0) == Wrapped);

    TestTrue(TEXT("Reconfigure surfaces"), Surfaces->Configure(Config, Error));
    Surfaces->ApplyToStaticMesh(First.Get());
    UMaterialInstanceDynamic* Reconfigured = Cast<UMaterialInstanceDynamic>(First->GetMaterial(0));
    TestTrue(TEXT("New configuration replaces the old physical material wrapper"),
        Reconfigured && Reconfigured != Wrapped);
    TestTrue(TEXT("Replacement preserves the original shader parent"), Reconfigured && Reconfigured->Parent == Base);
    Surfaces->Reset();
    World->DestroyWorld(false);
    return true;
}
#endif
