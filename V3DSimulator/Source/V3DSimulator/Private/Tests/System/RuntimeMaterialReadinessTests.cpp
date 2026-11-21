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
    Parent->BlendMode = BLEND_Masked;
    FV3DMaterialSoftAssetReferences Soft; Soft.ReservedMaterials.Reset();
    Soft.MI_Opaque = Parent.Get(); Soft.MI_Toon = Parent.Get();
    FV3DMaterialAssetReferences Resolved; Resolved.Terrain = Parent.Get(); TArray<FString> Failures = {TEXT("old")};
    TestTrue(TEXT("Resident references resolve without sync I/O"), Soft.Resolve(Resolved, Failures, false));
    TestTrue(TEXT("Opaque slot accepts Masked without losing identity"), Resolved.MI_Opaque == Parent.Get());
    TestNull(TEXT("Old terrain guard cleared"), Resolved.Terrain.Get());
    TestTrue(TEXT("Diagnostics cleared"), Failures.IsEmpty());
    TArray<FSoftObjectPath> Paths; Soft.GetPreloadPaths(Paths);
    TestEqual(TEXT("Shared authored parents deduplicated; four safety parents preloaded"), Paths.Num(), 5);
    Parent->BlendMode = BLEND_Opaque; Soft.MI_Opaque = Parent.Get(); Soft.MI_Toon.Reset();
    Soft.Resolve(Resolved, Failures, false);
    TestEqual(TEXT("Actual Opaque blending reports one fallback diagnostic"), Failures.Num(), 1);
    TestTrue(TEXT("Invalid authored parent never published or modified"), Resolved.MI_Opaque != Parent.Get() && Parent->GetBlendMode() == BLEND_Opaque);
    Parent->BlendMode = BLEND_Masked; Parent->TwoSided = true;
    Soft.MI_Opaque.Reset(); Soft.MI_OpaqueTwoSided = Parent.Get();
    Soft.Resolve(Resolved, Failures, false);
    TestTrue(TEXT("Two-sided Masked parent retained"), Resolved.MI_OpaqueTwoSided == Parent.Get());
    TestTrue(TEXT("Two-sided slot resolves without warnings"), Failures.IsEmpty());
    return true;
}
#endif
