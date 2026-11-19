// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Materials/Material.h"
#include "Model/MaterialDefaultAsset.h"
#include "UObject/StrongObjectPtr.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DRuntimeMaterialReadinessTest,
    "V3DSimulator.Materials.RuntimeReadinessWithoutSynchronousLoading",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DRuntimeMaterialReadinessTest::RunTest(const FString&)
{
    TStrongObjectPtr<UMaterial> Parent(NewObject<UMaterial>());
    FV3DMaterialSoftAssetReferences Soft;
    Soft.MI_Opaque=Parent.Get(); Soft.MI_Toon=Parent.Get();
    FV3DMaterialAssetReferences Resolved; Resolved.Terrain=Parent.Get(); TArray<FString> Failures={TEXT("old")};
    TestTrue(TEXT("Resident assets need no synchronous load"),Soft.Resolve(Resolved,Failures,false));
    TestTrue(TEXT("Resident identity retained"),Resolved.MI_Opaque==Parent.Get() && Resolved.MI_Toon==Parent.Get());
    TestNull(TEXT("Previous world's terrain guard is cleared"),Resolved.Terrain.Get());
    TestTrue(TEXT("Previous diagnostics cleared"),Failures.IsEmpty());
    TArray<FSoftObjectPath> Paths;Soft.GetPreloadPaths(Paths);
    TestEqual(TEXT("Shared parents are preloaded only once"),Paths.Num(),1);
    const FString MissingName=TEXT("M_Missing_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const TSoftObjectPtr<UMaterialInterface> Missing(FSoftObjectPath(TEXT("/Game/__Missing__/")+MissingName+TEXT(".")+MissingName));
    Soft.MI_Opaque=Missing;
    TestTrue(TEXT("Unavailable references are inspected without blocking"),Soft.Resolve(Resolved,Failures,false));
    TestEqual(TEXT("Missing configured slot reports one failure"),Failures.Num(),1);
    TestNull(TEXT("Unavailable slot stays empty"),Resolved.MI_Opaque.Get());
    TestTrue(TEXT("Other resident slots survive"),Resolved.MI_Toon==Parent.Get());
    Parent->TwoSided=true;Soft.MI_Opaque.Reset();
    Soft.Resolve(Resolved,Failures,false);
    TestEqual(TEXT("Two-sided Toon is rejected"),Failures.Num(),1);
    TestTrue(TEXT("Rejected parents are never published"),Resolved.IsEmpty());
    TestTrue(TEXT("Registry validation never mutates authored parents"),Parent->IsTwoSided());
    Parent->TwoSided=false; Parent->BlendMode=BLEND_Masked;
    Soft.MI_Opaque=Parent.Get();Soft.Resolve(Resolved,Failures,false);
    TestEqual(TEXT("Wrong-alpha default slot fails validation"),Failures.Num(),1);
    TestNull(TEXT("Masked material cannot occupy Opaque slot"),Resolved.MI_Opaque.Get());
    TestTrue(TEXT("Toon keeps its authored Masked state"),Resolved.MI_Toon==Parent.Get());
    const FV3DMaterialSoftAssetReferences Empty;
    Empty.Resolve(Resolved,Failures,false);
    TestTrue(TEXT("An empty registry clears references and diagnostics"),Resolved.IsEmpty() && Failures.IsEmpty());
    return true;
}
#endif
