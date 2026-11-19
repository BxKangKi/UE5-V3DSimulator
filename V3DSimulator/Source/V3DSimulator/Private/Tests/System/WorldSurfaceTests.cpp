// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "System/WorldSurfaceConfig.h"
#include "System/SafeFileIO.h"
#include "System/WorldSurfaceSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/StrongObjectPtr.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Misc/ScopeExit.h"

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
        "FootstepMappings":{"Grass":{"Sounds":["v3d://sound/12345678-1234-1234-1234-123456789abc"]}}
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DRegistryPhysicalMaterialTest,
    "V3DSimulator.World.Surfaces.RegistryDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DRegistryPhysicalMaterialTest::RunTest(const FString&)
{
    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None,
        nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("World"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    TStrongObjectPtr<UV3DSimulatorAssetRegistry> Registry(NewObject<UV3DSimulatorAssetRegistry>());
    TStrongObjectPtr<UPhysicalMaterial> Default(NewObject<UPhysicalMaterial>());
    TStrongObjectPtr<UPhysicalMaterial> Authored(NewObject<UPhysicalMaterial>());
    TStrongObjectPtr<UPhysicalMaterial> External(NewObject<UPhysicalMaterial>());
    TestNull(TEXT("Unconfigured registry preserves engine fallback"), Registry->ResolveDefaultPhysicalMaterial());
    Registry->DefaultPhysicalMaterial = Default.Get();
    TestTrue(TEXT("Registry resolves configured asset"), Registry->ResolveDefaultPhysicalMaterial() == Default.Get());
    TStrongObjectPtr<UWorldSurfaceSubsystem> Surfaces(NewObject<UWorldSurfaceSubsystem>(World));
    FString Error;
    TestTrue(TEXT("Empty project configuration is legal"), Surfaces->Configure(FV3DWorldSurfaceConfig(), Error));
    Surfaces->DefaultPhysicalMaterial = Registry->ResolveDefaultPhysicalMaterial();
    TStrongObjectPtr<UBoxComponent> Collider(NewObject<UBoxComponent>(World));
    Surfaces->ApplyToPrimitive(Collider.Get());
    TestTrue(TEXT("Simple collider receives registry default"), Collider->GetPhysicsMaterialOverride() == Default.Get());
    Collider->SetPhysMaterialOverride(External.Get());
    Surfaces->ApplyToPrimitive(Collider.Get());
    TestTrue(TEXT("Simple collider keeps an explicit override"), Collider->GetPhysicsMaterialOverride() == External.Get());
    TStrongObjectPtr<UStaticMesh> Mesh(NewObject<UStaticMesh>());
    UMaterialInterface* Base = UMaterial::GetDefaultMaterial(MD_Surface);
    Mesh->GetStaticMaterials().Add(FStaticMaterial(Base, TEXT("Unassigned")));
    TStrongObjectPtr<UStaticMeshComponent> First(NewObject<UStaticMeshComponent>(World));
    TStrongObjectPtr<UStaticMeshComponent> Second(NewObject<UStaticMeshComponent>(World));
    First->SetStaticMesh(Mesh.Get());
    Second->SetStaticMesh(Mesh.Get());
    Surfaces->ApplyToMesh(First.Get());
    Surfaces->ApplyToMesh(Second.Get());
    TestTrue(TEXT("Default applies without JSON definitions"), First->GetPhysicsMaterialOverride() == Default.Get());
    TestTrue(TEXT("Complex collision material also receives fallback"), First->GetMaterial(0)->GetPhysicalMaterial() == Default.Get());
    TestTrue(TEXT("Fallback wrappers are shared"), First->GetMaterial(0) == Second->GetMaterial(0));
    TestTrue(TEXT("Simple collider resolves same fallback"), Surfaces->GetSimplePhysicalMaterial(First.Get()) == Default.Get());
    UMaterialInterface* Wrapped = First->GetMaterial(0);
    Surfaces->ApplyToMesh(First.Get());
    TestTrue(TEXT("Repeated application does not stack wrappers"), Wrapped == First->GetMaterial(0));
    First->SetPhysMaterialOverride(External.Get());
    Surfaces->Reset();
    TestTrue(TEXT("Reset preserves an override installed later by another system"), First->GetPhysicsMaterialOverride() == External.Get());
    TestNull(TEXT("Reset detaches its own override"), Second->GetPhysicsMaterialOverride());
    TestNull(TEXT("Reset drops the world's asset pin"), Surfaces->DefaultPhysicalMaterial.Get());

    Surfaces->DefaultPhysicalMaterial = Default.Get();
    TStrongObjectPtr<UMaterialInstanceDynamic> AuthoredMaterial(UMaterialInstanceDynamic::Create(Base, World));
    AuthoredMaterial->PhysMaterial = Authored.Get();
    AuthoredMaterial->bOverridePhysMaterial = true;
    Second->SetMaterial(0, AuthoredMaterial.Get());
    Surfaces->ApplyToMesh(Second.Get());
    TestTrue(TEXT("Authored material wins over registry fallback"), Second->GetMaterial(0) == AuthoredMaterial.Get());
    TestTrue(TEXT("Simple collider preserves authored material"), Surfaces->GetSimplePhysicalMaterial(Second.Get()) == Authored.Get());
    Mesh->GetStaticMaterials().Add(FStaticMaterial(Base, TEXT("Unassigned2")));
    Surfaces->ApplyToMesh(Second.Get());
    TestTrue(TEXT("Mixed slot fallback applies only to unmapped slot"), Second->GetMaterial(1)->GetPhysicalMaterial() == Default.Get());
    TestNull(TEXT("Mixed surfaces never get a blanket component override"), Second->GetPhysicsMaterialOverride());

    FV3DWorldSurfaceConfig Project;
    Project.DefaultPhysicalMaterial = TEXT("ProjectSurface");
    Project.PhysicalMaterials.Add(TEXT("ProjectSurface"), FV3DSurfaceDefinition());
    TestTrue(TEXT("Project surface configuration"), Surfaces->Configure(Project, Error));
    Surfaces->DefaultPhysicalMaterial = Default.Get();
    Surfaces->ApplyToMesh(Second.Get());
    TestTrue(TEXT("Project default wins over registry and authored slot"),
        Second->GetMaterial(0)->GetPhysicalMaterial() == Surfaces->PhysicalMaterials.FindRef(TEXT("ProjectSurface")));
    Surfaces->Reset();
    return true;
}
#endif
