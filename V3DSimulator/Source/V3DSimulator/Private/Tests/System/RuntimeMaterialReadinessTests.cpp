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
    TStrongObjectPtr<UMaterial> LoadedMaterial(NewObject<UMaterial>(GetTransientPackage()));
    if (!TestNotNull(TEXT("Already resident material is available without an asset package"), LoadedMaterial.Get()))
        return false;

    FV3DMaterialSoftAssetReferences SoftReferences;
    SoftReferences.Opaque = LoadedMaterial.Get();
    SoftReferences.Masked = LoadedMaterial.Get();
    SoftReferences.TwoSided = LoadedMaterial.Get();
    SoftReferences.TwoSidedMasked = LoadedMaterial.Get();
    SoftReferences.Translucent = LoadedMaterial.Get();
    SoftReferences.TwoSidedTranslucent = LoadedMaterial.Get();
    SoftReferences.ByMaterialName.Add(TEXT("  ResidentOverride  "), LoadedMaterial.Get());
    SoftReferences.ByMaterialName.Add(TEXT("UnconfiguredOverride"), TSoftObjectPtr<UMaterialInterface>());
    SoftReferences.ByMaterialName.Add(TEXT("   "), LoadedMaterial.Get());

    // Reuse a previous world's output, including its independently resolved terrain material.
    FV3DMaterialAssetReferences Resolved;
    Resolved.Terrain = LoadedMaterial.Get();
    Resolved.ByMaterialName.Add(TEXT("PreviousWorldOverride"), LoadedMaterial.Get());
    TArray<FString> Failures = { TEXT("Previous world's failure") };
    TestTrue(TEXT("Resident material references resolve with synchronous loading disabled"),
        SoftReferences.Resolve(Resolved, Failures, false));
    TestTrue(TEXT("All resident material categories preserve their original object"),
        Resolved.Opaque.Get() == LoadedMaterial.Get()
        && Resolved.Masked.Get() == LoadedMaterial.Get()
        && Resolved.TwoSided.Get() == LoadedMaterial.Get()
        && Resolved.TwoSidedMasked.Get() == LoadedMaterial.Get()
        && Resolved.Translucent.Get() == LoadedMaterial.Get()
        && Resolved.TwoSidedTranslucent.Get() == LoadedMaterial.Get());
    TestTrue(TEXT("Successful resolution clears previous diagnostics"), Failures.IsEmpty());
    TestNull(TEXT("A previous world's terrain reference cannot leak into the new set"), Resolved.Terrain.Get());
    TestEqual(TEXT("Only the configured resident named override survives"), Resolved.ByMaterialName.Num(), 1);
    const TObjectPtr<UMaterialInterface>* ResidentOverride = Resolved.ByMaterialName.Find(TEXT("ResidentOverride"));
    TestTrue(TEXT("Named overrides keep the resident material and trim surrounding whitespace"),
        ResidentOverride && ResidentOverride->Get() == LoadedMaterial.Get());
    TestFalse(TEXT("The previous world's named override is discarded"),
        Resolved.ByMaterialName.Contains(TEXT("PreviousWorldOverride")));

    const FString MissingName = FString::Printf(TEXT("M_RuntimeReadiness_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FSoftObjectPath MissingPath(FString::Printf(TEXT("/Game/__V3DReadinessMissing__/%s.%s"),
        *MissingName, *MissingName));
    const TSoftObjectPtr<UMaterialInterface> MissingReference(MissingPath);
    TestNull(TEXT("Unique missing asset is not already resident"), MissingReference.Get());
    SoftReferences.Opaque = MissingReference;
    SoftReferences.Masked = MissingReference;
    SoftReferences.TwoSided = MissingReference;
    SoftReferences.TwoSidedMasked = MissingReference;
    SoftReferences.Translucent = MissingReference;
    SoftReferences.TwoSidedTranslucent = MissingReference;
    SoftReferences.ByMaterialName.Add(TEXT("  UnavailableOverride  "), MissingReference);

    // Do not expect/suppress load errors: an accidental LoadSynchronous fallback must fail
    // this test through the loader's unexpected missing-package errors, as well as diagnostics.
    TestTrue(TEXT("Unavailable references are inspected without a blocking fallback"),
        SoftReferences.Resolve(Resolved, Failures, false));
    TestTrue(TEXT("Unavailable material categories remain null instead of retaining the previous material"),
        !Resolved.Opaque && !Resolved.Masked && !Resolved.TwoSided
        && !Resolved.TwoSidedMasked && !Resolved.Translucent && !Resolved.TwoSidedTranslucent);
    TestEqual(TEXT("Each unavailable configured reference reports a failure"), Failures.Num(), 7);
    const TCHAR* FailureLabels[] = {
        TEXT("Opaque ->"), TEXT("Masked ->"), TEXT("TwoSided ->"), TEXT("TwoSidedMasked ->"),
        TEXT("Translucent ->"), TEXT("TwoSidedTranslucent ->"), TEXT("ByMaterialName[UnavailableOverride]")
    };
    for (const TCHAR* Label : FailureLabels)
    {
        TestTrue(*FString::Printf(TEXT("Failure identifies %s and its configured asset"), Label),
            Failures.ContainsByPredicate([Label, &MissingPath](const FString& Failure)
            {
                return Failure.StartsWith(Label) && Failure.Contains(MissingPath.ToString());
            }));
    }
    TestFalse(TEXT("An unavailable named override is omitted"),
        Resolved.ByMaterialName.Contains(TEXT("UnavailableOverride")));
    ResidentOverride = Resolved.ByMaterialName.Find(TEXT("ResidentOverride"));
    TestTrue(TEXT("A missing package does not discard another resident named override"),
        Resolved.ByMaterialName.Num() == 1 && ResidentOverride && ResidentOverride->Get() == LoadedMaterial.Get());
    TestNull(TEXT("Readiness inspection leaves the missing soft reference unloaded"), MissingReference.Get());
    TestFalse(TEXT("Readiness inspection retains the configured soft asset path"), MissingReference.IsNull());

    const FV3DMaterialSoftAssetReferences EmptyReferences;
    TestTrue(TEXT("A world without configured defaults also resolves without loading"),
        EmptyReferences.Resolve(Resolved, Failures, false));
    TestTrue(TEXT("Empty defaults clear all previous strong references"), Resolved.IsEmpty());
    TestTrue(TEXT("Empty defaults clear previous readiness failures"), Failures.IsEmpty());
    return true;
}
#endif
