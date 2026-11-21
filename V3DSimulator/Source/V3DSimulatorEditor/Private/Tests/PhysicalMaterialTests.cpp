#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "MaterialEditingLibrary.h"
#include "../V3DPhysicalMaterialUpgrade.h"
#include "World/PhysicalMaterialProfile.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionEyeAdaptation.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionGetMaterialAttributes.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTwoSidedSign.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DWaterOpacityOutputCreationTest,
    "V3DSimulator.Rendering.Physical.WaterOpacityOutputCreation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DWaterOpacityOutputCreationTest::RunTest(const FString&)
{
    // Exercise the production creation path without relying on a pre-migrated water asset.
    // The second case catches assumptions that Opacity is always output 1.
    for (const bool WithExistingOutput : {false, true})
    {
        auto* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
        auto* Read = CastChecked<UMaterialExpressionGetMaterialAttributes>(UMaterialEditingLibrary::CreateMaterialExpression(
            Material, UMaterialExpressionGetMaterialAttributes::StaticClass()));
        if (WithExistingOutput) Read->CreateOrGetOutputAttribute(MP_Roughness);
        const TArray<FGuid> ExistingAttributes = Read->AttributeGetTypes;
        const int32 OriginalOutputs = Read->GetOutputs().Num();
        bool Changed = false;
        const int32 Output = V3DMaterialDefaults::EnsureWaterOpacityOutput(Read, Changed);
        if (!TestTrue(TEXT("Opacity creation succeeds on a fresh node"), Output != INDEX_NONE)) return false;
        TestTrue(TEXT("New opacity output is reported as a change"), Changed);
        TestEqual(TEXT("Attribute IDs and pins remain synchronized"), Read->GetOutputs().Num(), Read->AttributeGetTypes.Num() + 1);
        TestEqual(TEXT("Exactly one output is added"), Read->GetOutputs().Num(), OriginalOutputs + 1);
        if (TestTrue(TEXT("Returned slot has a matching attribute"), Read->AttributeGetTypes.IsValidIndex(Output - 1)))
            TestTrue(TEXT("Returned slot is opacity"), Read->AttributeGetTypes[Output - 1] == FMaterialAttributeDefinitionMap::GetID(MP_Opacity));
        for (int32 Index = 0; Index < ExistingAttributes.Num(); ++Index)
            TestTrue(TEXT("Existing attribute slots are preserved"), Read->AttributeGetTypes[Index] == ExistingAttributes[Index]);
        // This was the startup crash point when only AttributeGetTypes was assigned.
        Read->PostEditChange();
        Changed = false;
        TestEqual(TEXT("Repeated initialization reuses the output"), V3DMaterialDefaults::EnsureWaterOpacityOutput(Read, Changed), Output);
        TestFalse(TEXT("Repeated initialization does not dirty the node"), Changed);
        TestEqual(TEXT("Repeated initialization cannot add pins"), Read->GetOutputs().Num(), OriginalOutputs + 1);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DWaterFinalOutputTest,
    "V3DSimulator.Rendering.Physical.WaterFinalOutput", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DWaterFinalOutputTest::RunTest(const FString&)
{
    auto* Material = LoadObject<UMaterial>(nullptr, TEXT("/ShaderLibrary/Water/WaterSurface/M_Water.M_Water"));
    if (!TestNotNull(TEXT("Water material is available"), Material)) return false;
    TestTrue(TEXT("SingleLayerWater shading is active"), Material->GetShadingModels().HasOnlyShadingModel(MSM_SingleLayerWater));
    TestTrue(TEXT("Water uses a supported surface pass"), Material->BlendMode == BLEND_Masked || Material->BlendMode == BLEND_Opaque);
    FExpressionInput* Opacity = Material->GetExpressionInputForProperty(MP_Opacity);
    if (Material->bUseMaterialAttributes)
    {
        auto* Final = Material->GetExpressionInputForProperty(MP_MaterialAttributes);
        auto* Set = Final ? Cast<UMaterialExpressionSetMaterialAttributes>(Final->Expression) : nullptr;
        if (!TestNotNull(TEXT("Final bundle explicitly controls water optics"), Set)) return false;
        Opacity = nullptr;
        auto Inputs = Set->GetInputsView();
        for (int32 Index = 0; Index < Inputs.Num(); ++Index)
            if (Set->GetInputName(Index) == TEXT("Opacity")) Opacity = Inputs[Index];
    }
    auto* Read = Opacity ? Cast<UMaterialExpressionGetMaterialAttributes>(Opacity->Expression) : nullptr;
    if (TestNotNull(TEXT("Final opacity preserves the underside output"), Read))
    {
        const int32 AttributeIndex = Read->AttributeGetTypes.IndexOfByKey(FMaterialAttributeDefinitionMap::GetID(MP_Opacity));
        TestTrue(TEXT("Reader extracts opacity"), AttributeIndex != INDEX_NONE);
        TestEqual(TEXT("Opacity uses the extracted attribute output"), Opacity->OutputIndex, AttributeIndex + 1);
        TestEqual(TEXT("Reader pins match attribute IDs"), Read->GetOutputs().Num(), Read->AttributeGetTypes.Num() + 1);
        const auto* Input = Read->GetInput(0);
        const auto* Underside = Input ? Cast<UMaterialExpressionMaterialFunctionCall>(Input->Expression) : nullptr;
        TestTrue(TEXT("Critical-angle opacity cannot be bypassed"), Underside
            && GetNameSafe(Underside->MaterialFunction) == TEXT("MF_Water_Underside") && Input->OutputIndex == 0);
    }
    TestTrue(TEXT("Water is two-sided with tangent-space PNO refraction"), Material->TwoSided
        && Material->bTangentSpaceNormal && Material->RefractionMethod == RM_PixelNormalOffset);
    bool FoundWaterOutput = false;
    for (UMaterialExpression* Expression : Material->GetExpressions())
        if (auto* Water = Cast<UMaterialExpressionSingleLayerWaterMaterialOutput>(Expression))
        {
            FoundWaterOutput = true;
            TestTrue(TEXT("Absorption is connected"), Water->AbsorptionCoefficients.IsConnected());
            TestTrue(TEXT("Scattering is connected"), Water->ScatteringCoefficients.IsConnected());
            TestTrue(TEXT("Phase is connected"), Water->PhaseG.IsConnected());
            TestTrue(TEXT("Background luminance is connected"), Water->ColorScaleBehindWater.IsConnected());
        }
    TestTrue(TEXT("Volume output exists"), FoundWaterOutput);
    for (const TCHAR* Path : {TEXT("/Game/Resources/Materials/MI_Water.MI_Water"), TEXT("/Game/Resources/Materials/MI_Ocean.MI_Ocean")})
    {
        auto* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, Path);
        if (!TestNotNull(Path, Instance)) continue;
        float Value = -1.0f;
        TestTrue(TEXT("Cooked instance exposes water opacity"), Instance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Water Opacity")), Value));
        TestEqual(TEXT("Serialized overrides retain the moderate surface opacity"), Value, V3DPhysicalMaterial::WaterSurfaceOpacity);
        for (const auto& P : V3DPhysicalMaterial::WaterScalars)
            if (Instance->GetScalarParameterValue(FMaterialParameterInfo(P.Name), Value))
                TestTrue(FString::Printf(TEXT("Cooked and runtime %s agree"), P.Name), FMath::IsNearlyEqual(Value, P.Value, 1.e-7f));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DWaterReflectionBindingsTest,
    "V3DSimulator.Rendering.Physical.WaterReflectionBindings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DWaterReflectionBindingsTest::RunTest(const FString&)
{
    auto* Attributes = LoadObject<UMaterialFunction>(nullptr, TEXT("/ShaderLibrary/Water/MaterialFunction/MF_WaterAttributes.MF_WaterAttributes"));
    auto* Normals = LoadObject<UMaterialFunction>(nullptr, TEXT("/ShaderLibrary/Water/MaterialFunction/MF_WaterTextureSurface.MF_WaterTextureSurface"));
    auto* Underside = LoadObject<UMaterialFunction>(nullptr, TEXT("/ShaderLibrary/Water/MaterialFunction/MF_Water_Underside.MF_Water_Underside"));
    if (!Attributes || !Normals || !Underside) { AddError(TEXT("Required water functions missing")); return false; }
    auto FindPin = [](UMaterialFunction* Function, const TCHAR* Name) -> FExpressionInput*
    {
        for (UMaterialExpression* E : Function->GetExpressions())
            if (auto* O = Cast<UMaterialExpressionFunctionOutput>(E))
                if (auto* Set = Cast<UMaterialExpressionSetMaterialAttributes>(O->A.Expression))
                {
                    const auto Inputs = Set->GetInputsView();
                    for (int32 I = 0; I < Inputs.Num(); ++I)
                        if (Set->GetInputName(I) == Name) return Inputs[I];
                }
        return nullptr;
    };
    auto* Color = FindPin(Attributes, TEXT("Base Color"));
    TestTrue(TEXT("Base color cannot divide by caustic intensity"), Color
        && Cast<UMaterialExpressionVectorParameter>(Color->Expression));
    auto* Opacity = FindPin(Attributes, TEXT("Opacity"));
    auto* SurfaceOpacity = Opacity ? Cast<UMaterialExpressionCustom>(Opacity->Expression) : nullptr;
    if (!TestNotNull(TEXT("Above-water opacity wrapper exists"), SurfaceOpacity)) return false;
    const auto* Value = SurfaceOpacity->Inputs.FindByPredicate([](const auto& I) { return I.InputName == TEXT("Opacity"); });
    const auto* Face = SurfaceOpacity->Inputs.FindByPredicate([](const auto& I) { return I.InputName == TEXT("FaceSign"); });
    auto* OpacityScalar = Value ? Cast<UMaterialExpressionScalarParameter>(Value->Input.Expression) : nullptr;
    TestTrue(TEXT("Above-water Fresnel opacity is restricted to the front face"), Face && Cast<UMaterialExpressionTwoSidedSign>(Face->Input.Expression)
        && SurfaceOpacity->Code.Contains(TEXT("saturate(FaceSign)"))
        && SurfaceOpacity->Code.Contains(TEXT("Parameters.CameraVector.z")));
    TestTrue(TEXT("Surface opacity is applied before the underside function"), OpacityScalar
        && OpacityScalar->ParameterName == TEXT("Water Opacity") && OpacityScalar->DefaultValue == V3DPhysicalMaterial::WaterSurfaceOpacity);
    auto* NormalPin = FindPin(Normals, TEXT("Normal"));
    auto* Normal = NormalPin ? Cast<UMaterialExpressionCustom>(NormalPin->Expression) : nullptr;
    if (TestNotNull(TEXT("Wave normal reaches the function output"), Normal))
    {
        TestTrue(TEXT("Stable wavelength shader is bound"), Normal->Code.Contains(TEXT("V3DWaterSurfaceNormal("))
            && Normal->IncludeFilePaths.Contains(TEXT("/Plugin/ShaderLibrary/WaterSurface.ush")));
        for (const auto& Input : Normal->Inputs)
            TestTrue(FString::Printf(TEXT("Normal input %s is bound"), *Input.InputName.ToString()), Input.Input.IsConnected());
        TestEqual(TEXT("All normal inputs are present"), Normal->Inputs.Num(), 11);
    }
    auto GetScalar = [](UMaterialFunction* Function, const TCHAR* Name) -> float
    {
        for (UMaterialExpression* E : Function->GetExpressions())
            if (auto* P = Cast<UMaterialExpressionScalarParameter>(E))
                if (P->ParameterName == Name) return P->DefaultValue;
        return -999.0f;
    };
    TestEqual(TEXT("Neutral distant PNO is one"), GetScalar(Attributes, TEXT("Refraction Far")), 1.0f);
    TestEqual(TEXT("Underside lip cannot produce reverse refraction"), GetScalar(Underside, TEXT("Lip Refraction")), 1.0f);
    const float Dot = GetScalar(Underside, TEXT("Critical Angle Dot"));
    const float Width = GetScalar(Underside, TEXT("Critical Angle Width"));
    const float CriticalCos = FMath::Sqrt(1.0f - 1.0f / (1.3333f * 1.3333f));
    TestTrue(TEXT("Snell transition is centred at the water-air critical angle"), FMath::IsNearlyEqual(Dot - Width * 0.5f, CriticalCos, 1.e-5f));
    TestTrue(TEXT("Snell transition has a finite non-zero width"), Width >= 0.02f && Width <= 0.08f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DMoonExposureBindingTest,
    "V3DSimulator.Rendering.Physical.MoonExposureBinding", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DMoonExposureBindingTest::RunTest(const FString&)
{
    auto* Function = LoadObject<UMaterialFunction>(nullptr, TEXT("/ShaderLibrary/Skybox/Functions/MF_Moon.MF_Moon"));
    if (!TestNotNull(TEXT("Moon texture function exists"), Function)) return false;
    int32 BoundOutputs = 0;
    for (UMaterialExpression* Expression : Function->GetExpressions())
        if (auto* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
        {
            auto* Tone = Cast<UMaterialExpressionCustom>(Output->A.Expression);
            if (!TestNotNull(TEXT("Textured output is exposure controlled"), Tone)) continue;
            const FCustomInput* Color = Tone->Inputs.FindByPredicate([](const FCustomInput& Input) { return Input.InputName == TEXT("TextureColor"); });
            const FCustomInput* Exposure = Tone->Inputs.FindByPredicate([](const FCustomInput& Input) { return Input.InputName == TEXT("Exposure"); });
            auto* Texture = Color ? Cast<UMaterialExpressionTextureSampleParameter2D>(Color->Input.Expression) : nullptr;
            TestTrue(TEXT("Actual moon texture drives brightness directly"), Texture && Texture->ParameterName == TEXT("Moon") && Texture->Texture);
            TestTrue(TEXT("Existing exposure wrappers receive the corrected formula"), Tone->Code.Contains(TEXT("V3DMoonRadiance(")));
            TestTrue(TEXT("Exposure declares a real engine binding"), Exposure && Cast<UMaterialExpressionEyeAdaptation>(Exposure->Input.Expression));
            ++BoundOutputs;
        }
    TestTrue(TEXT("At least one moon output checked"), BoundOutputs > 0);
    auto* Sky = LoadObject<UMaterial>(nullptr, TEXT("/ShaderLibrary/Skybox/M_Skybox.M_Skybox"));
    if (TestNotNull(TEXT("Sky parent exists"), Sky))
        TestTrue(TEXT("Sky is unlit and uses the sky pass"), Sky->bIsSky && Sky->GetShadingModels().HasOnlyShadingModel(MSM_Unlit));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DPhysicalMigrationIdempotenceTest,
    "V3DSimulator.Rendering.Physical.MigrationIdempotence", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DPhysicalMigrationIdempotenceTest::RunTest(const FString&)
{
    auto* Water = LoadObject<UMaterial>(nullptr, TEXT("/ShaderLibrary/Water/WaterSurface/M_Water.M_Water"));
    auto* Moon = LoadObject<UMaterialFunction>(nullptr, TEXT("/ShaderLibrary/Skybox/Functions/MF_Moon.MF_Moon"));
    auto* Sky = LoadObject<UMaterial>(nullptr, TEXT("/ShaderLibrary/Skybox/M_Skybox.M_Skybox"));
    if (!Water || !Moon || !Sky) { AddError(TEXT("Required water/moon/sky content missing")); return false; }
    V3DMaterialDefaults::UpgradePhysicalMaterials();
    const int32 WaterCount = Water->GetExpressions().Num();
    const int32 MoonCount = Moon->GetExpressions().Num();
    const int32 SkyCount = Sky->GetExpressions().Num();
    TArray<UMaterialFunction*> WaterFunctions;
    TArray<int32> WaterFunctionCounts;
    for (const TCHAR* Name : {TEXT("MF_WaterAttributes"), TEXT("MF_WaterTextureSurface"), TEXT("MF_Water_Underside")})
    {
        const FString Path = FString::Printf(TEXT("/ShaderLibrary/Water/MaterialFunction/%s.%s"), Name, Name);
        auto* Function = LoadObject<UMaterialFunction>(nullptr, *Path);
        if (!TestNotNull(Name, Function)) return false;
        WaterFunctions.Add(Function); WaterFunctionCounts.Add(Function->GetExpressions().Num());
    }
    V3DMaterialDefaults::UpgradePhysicalMaterials();
    TestEqual(TEXT("Restarting cannot stack water wrappers"), Water->GetExpressions().Num(), WaterCount);
    TestEqual(TEXT("Restarting cannot multiply exposure compensation"), Moon->GetExpressions().Num(), MoonCount);
    TestEqual(TEXT("Restarting cannot stack sun disk wrappers"), Sky->GetExpressions().Num(), SkyCount);
    for (int32 Index = 0; Index < WaterFunctions.Num(); ++Index)
        TestEqual(TEXT("Restarting cannot stack water function nodes"), WaterFunctions[Index]->GetExpressions().Num(), WaterFunctionCounts[Index]);
    for (const TCHAR* Path : {TEXT("/Game/V3DGenerated/Materials/M_Opaque.M_Opaque"), TEXT("/Game/V3DGenerated/Materials/M_Glass.M_Glass")})
    {
        auto* Material = LoadObject<UMaterial>(nullptr, Path);
        if (!TestNotNull(Path, Material)) continue;
        for (EMaterialProperty Property : {MP_Metallic, MP_Roughness, MP_Specular, MP_Normal, MP_AmbientOcclusion, MP_EmissiveColor})
        {
            auto* Input = Material->GetExpressionInputForProperty(Property);
            TestTrue(TEXT("Fallback supports authored PBR material channels"), Input && Input->IsConnected());
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DMatchedCelestialDisksTest,
    "V3DSimulator.Rendering.Physical.MatchedCelestialDisks", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DMatchedCelestialDisksTest::RunTest(const FString&)
{
    auto* Sky = LoadObject<UMaterial>(nullptr, TEXT("/ShaderLibrary/Skybox/M_Skybox.M_Skybox"));
    auto* Moon = LoadObject<UMaterialFunction>(nullptr, TEXT("/ShaderLibrary/Skybox/Functions/MF_Moon.MF_Moon"));
    if (!Sky || !Moon) { AddError(TEXT("Sky content missing")); return false; }
    UMaterialExpressionCustom* Disk = nullptr;
    float MoonRadius = -1;
    for (UMaterialExpression* E : Sky->GetExpressions())
        if (auto* C = Cast<UMaterialExpressionCustom>(E))
            if (C->Description == TEXT("V3D matched sun disk")) Disk = C;
    for (UMaterialExpression* E : Moon->GetExpressions())
        if (auto* P = Cast<UMaterialExpressionScalarParameter>(E))
            if (P->ParameterName == TEXT("MoonSize")) MoonRadius = P->DefaultValue;
    if (!TestNotNull(TEXT("Sun uses shared angular mask"), Disk)) return false;
    const auto* Radius = Disk->Inputs.FindByPredicate([](const auto& I) { return I.InputName == TEXT("Radius"); });
    auto* Scalar = Radius ? Cast<UMaterialExpressionScalarParameter>(Radius->Input.Expression) : nullptr;
    if (!TestNotNull(TEXT("Sun radius is bound"), Scalar)) return false;
    TestEqual(TEXT("Sun and moon have equal angular radii"), Scalar->DefaultValue, MoonRadius);
    TestEqual(TEXT("Authored and runtime radii match"), MoonRadius, V3DPhysicalMaterial::CelestialRadiusRadians);
    TestTrue(TEXT("Shared mask is compiled"), Disk->Code.Contains(TEXT("V3DCelestialCoverage("))
        && Disk->IncludeFilePaths.Contains(TEXT("/Plugin/ShaderLibrary/Skybox.ush")));
    const auto* Visibility = Disk->Inputs.FindByPredicate([](const auto& I) { return I.InputName == TEXT("Visibility"); });
    auto* VisibilityScalar = Visibility ? Cast<UMaterialExpressionScalarParameter>(Visibility->Input.Expression) : nullptr;
    TestTrue(TEXT("Night can disable the HDR sun independently"), VisibilityScalar && VisibilityScalar->ParameterName == TEXT("SunVisibility"));
    bool Bound = false;
    for (UMaterialExpression* E : Sky->GetExpressions())
        if (auto* Multiply = Cast<UMaterialExpressionMultiply>(E)) Bound |= Multiply->A.Expression == Disk;
    TestTrue(TEXT("Mask reaches sun luminance"), Bound);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DMoonCompositeTest,
    "V3DSimulator.Rendering.Physical.MoonFinalComposite", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DMoonCompositeTest::RunTest(const FString&)
{
    auto* Sky = LoadObject<UMaterial>(nullptr, TEXT("/ShaderLibrary/Skybox/M_Skybox.M_Skybox"));
    auto* Function = LoadObject<UMaterialFunction>(nullptr, TEXT("/ShaderLibrary/Skybox/Functions/MF_Moon.MF_Moon"));
    if (!Sky || !Function) { AddError(TEXT("Sky/moon content missing")); return false; }
    auto* Emissive = Sky->GetExpressionInputForProperty(MP_EmissiveColor);
    auto* Composite = Emissive ? Cast<UMaterialExpressionCustom>(Emissive->Expression) : nullptr;
    if (!TestNotNull(TEXT("Moon composition is the final emissive output"), Composite)) return false;
    TestTrue(TEXT("Background is blended, not added over the moon"), Composite->Code.Contains(TEXT("V3DComposeMoon(")));
    const auto* MoonInput = Composite->Inputs.FindByPredicate([](const auto& I) { return I.InputName == TEXT("MoonColor"); });
    const auto* CoverageInput = Composite->Inputs.FindByPredicate([](const auto& I) { return I.InputName == TEXT("Coverage"); });
    const auto* BackgroundInput = Composite->Inputs.FindByPredicate([](const auto& I) { return I.InputName == TEXT("Background"); });
    auto* Moon = MoonInput ? Cast<UMaterialExpressionMaterialFunctionCall>(MoonInput->Input.Expression) : nullptr;
    TestTrue(TEXT("Texture function is connected outside sky brightness"), Moon && Moon->MaterialFunction == Function);
    TestTrue(TEXT("Original background is retained without a self reference"), BackgroundInput
        && BackgroundInput->Input.IsConnected() && BackgroundInput->Input.Expression != Composite);
    auto* Coverage = CoverageInput ? Cast<UMaterialExpressionCustom>(CoverageInput->Input.Expression) : nullptr;
    if (!TestNotNull(TEXT("Coverage has its own angular mask"), Coverage)) return false;
    const auto* DirectionInput = Coverage->Inputs.FindByPredicate([](const auto& I) { return I.InputName == TEXT("BodyDirection"); });
    auto* Direction = DirectionInput ? Cast<UMaterialExpressionVectorParameter>(DirectionInput->Input.Expression) : nullptr;
    TestTrue(TEXT("Coverage uses the runtime moon direction"), Direction && Direction->ParameterName == TEXT("V3DMoonDirection"));
    bool UVBound = false;
    for (UMaterialExpression* E : Function->GetExpressions())
        if (auto* UV = Cast<UMaterialExpressionCustom>(E))
            if (UV->Description == TEXT("MoonUV"))
            {
                const auto* Light = UV->Inputs.FindByPredicate([](const auto& I) { return I.InputName == TEXT("lightDir"); });
                auto* P = Light ? Cast<UMaterialExpressionVectorParameter>(Light->Input.Expression) : nullptr;
                UVBound = P && P->ParameterName == TEXT("V3DMoonDirection");
            }
    TestTrue(TEXT("Texture UVs and coverage use the same direction"), UVBound);
    for (UMaterialExpression* E : Sky->GetExpressions())
        if (auto* Lerp = Cast<UMaterialExpressionLinearInterpolate>(E))
            TestFalse(TEXT("Old RGB-dependent moon overlay is bypassed"), Moon && Lerp->A.Expression == Moon);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DStarsDirectionBindingTest,
    "V3DSimulator.Rendering.Physical.StarsFollowMoon", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DStarsDirectionBindingTest::RunTest(const FString&)
{
    auto* Stars = LoadObject<UMaterialFunction>(nullptr, TEXT("/ShaderLibrary/Skybox/Functions/MF_Stars.MF_Stars"));
    if (!TestNotNull(TEXT("Stars function exists"), Stars)) return false;
    V3DMaterialDefaults::UpgradeCelestialMaterials();
    const int32 Count = Stars->GetExpressions().Num();
    V3DMaterialDefaults::UpgradeCelestialMaterials();
    TestEqual(TEXT("Repeated migration cannot stack star direction nodes"), Stars->GetExpressions().Num(), Count);
    bool Bound = false;
    for (UMaterialExpression* E : Stars->GetExpressions())
        if (auto* UV = Cast<UMaterialExpressionCustom>(E))
            if (UV->Description == TEXT("StarUV"))
            {
                const auto* Input = UV->Inputs.FindByPredicate([](const auto& I) { return I.InputName == TEXT("MoonDir"); });
                const auto* Direction = Input ? Cast<UMaterialExpressionVectorParameter>(Input->Input.Expression) : nullptr;
                Bound = Direction && Direction->ParameterName == TEXT("V3DMoonDirection") && Input->Input.OutputIndex == 0;
            }
    TestTrue(TEXT("Stars use the same runtime direction as moon UV and coverage"), Bound);
    return true;
}
#endif
