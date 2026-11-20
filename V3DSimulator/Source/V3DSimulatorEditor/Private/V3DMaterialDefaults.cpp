// Generated fallback shaders are editor assets, never runtime BlendMode overrides.
#include "V3DMaterialDefaults.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionPerInstanceFadeAmount.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionScreenPosition.h"
#include "Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionObjectPositionWS.h"
#include "Materials/MaterialExpressionObjectRadius.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialFunction.h"
#include "MaterialSceneTextureId.h"
#include "Engine/RendererSettings.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Texture2D.h"
#include "Engine/BlendableInterface.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Settings/ProjectPackagingSettings.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace V3DMaterialDefaults
{
    void UpgradeUnderwaterBindings()
    {
        auto* Function = LoadObject<UMaterialFunction>(nullptr,
            TEXT("/ShaderLibrary/Water/MaterialFunction/MF_UnderWaterShaft.MF_UnderWaterShaft"));
        if (!Function)
        {
            UE_LOG(LogTemp, Error, TEXT("Underwater shaft material migration requires ShaderLibrary/MF_UnderWaterShaft. Install the matching ShaderLibrary content before cooking."));
            return;
        }
        TArray<UMaterialExpressionCustom*> ShaftNodes;
        for (UMaterialExpression* Expression : Function->GetExpressions())
            if (auto* Custom = Cast<UMaterialExpressionCustom>(Expression))
                if (Custom->Code.Contains(TEXT("UnderWaterShaft("))
                    || Custom->Code.Contains(TEXT("UnderWaterShaftShadowed("))
                    || Custom->Code.Contains(TEXT("UnderWaterShaftCompact("))
                    || Custom->Code.Contains(TEXT("UnderWaterShaftMatrix("))) ShaftNodes.Add(Custom);
        bool bChanged = false;
        for (auto* Custom : ShaftNodes)
        {
            const FString UnderwaterInclude = TEXT("/Plugin/ShaderLibrary/Underwater.ush");
            if (!Custom->IncludeFilePaths.Contains(UnderwaterInclude))
            {
                Custom->Modify();
                Custom->IncludeFilePaths.Add(UnderwaterInclude);
                bChanged = true;
            }
            // A .ush include alone cannot declare material compiler/RDG scene-texture needs.
            // Compile explicit inputs so GBuffer and CustomDepth stay bound in cooked shaders.
            const FName Names[] = {TEXT("V3DSceneDepthBinding"), TEXT("V3DShadingModelBinding"), TEXT("V3DCustomDepthBinding")};
            const ESceneTextureId Ids[] = {PPI_SceneDepth, PPI_ShadingModelID, PPI_CustomDepth};
            for (int32 Index = 0; Index < 3; ++Index)
            {
                if (Custom->Inputs.ContainsByPredicate([&](const FCustomInput& Input) { return Input.InputName == Names[Index]; })) continue;
                auto* Texture = CastChecked<UMaterialExpressionSceneTexture>(
                    UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionSceneTexture::StaticClass(), -600, Index * 180));
                Texture->SceneTextureId = Ids[Index];
                FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
                Input.InputName = Names[Index]; Input.Input.Connect(0, Texture);
                bChanged = true;
            }
            // Texture objects must be explicit Custom inputs: this declares their samplers
            // and retains the parameters in cooked material instances.
            const TCHAR* MapNames[] = {TEXT("V3DUWShadowNear"), TEXT("V3DUWShadowFar")};
            for (const TCHAR* Name : MapNames)
            {
                if (Custom->Inputs.ContainsByPredicate([&](const FCustomInput& I) { return I.InputName == FName(Name); })) continue;
                auto* Texture = CastChecked<UMaterialExpressionTextureObjectParameter>(
                    UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionTextureObjectParameter::StaticClass()));
                Texture->ParameterName = Name;
                Texture->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
                Texture->SamplerType = SAMPLERTYPE_Color;
                FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
                Input.InputName = Name; Input.Input.Connect(0, Texture);
                bChanged = true;
            }
            // Only the 4 active light-space vectors are bound; V3DUWShadowNearOrigin connects via
            // OutputIndex 5 (RGBA = float4) so .w carries ShadowReady without an extra scalar parameter.
            struct FVectorBinding { const TCHAR* Name; int32 OutputIndex; };
            const FVectorBinding VectorBindings[] = {
                {TEXT("V3DUWShadowNearOrigin"), 5}, // RGBA (float4: NearOrigin.xyz + Ready)
                {TEXT("V3DUWShadowFarOrigin"), 0},  // RGB (float3)
                {TEXT("V3DUWShadowRight"), 0},      // RGB (float3)
                {TEXT("V3DUWShadowForward"), 0}     // RGB (float3)
            };
            for (const FVectorBinding& Binding : VectorBindings)
            {
                FCustomInput* Existing = Custom->Inputs.FindByPredicate([&](const FCustomInput& I) { return I.InputName == FName(Binding.Name); });
                if (Existing)
                {
                    if (Existing->Input.Expression && Existing->Input.OutputIndex != Binding.OutputIndex)
                    {
                        Custom->Modify();
                        Existing->Input.Connect(Binding.OutputIndex, Existing->Input.Expression);
                        bChanged = true;
                    }
                    continue;
                }
                auto* Vector = CastChecked<UMaterialExpressionVectorParameter>(
                    UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionVectorParameter::StaticClass()));
                Vector->ParameterName = Binding.Name;
                Vector->DefaultValue = FLinearColor::Black;
                FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
                Input.InputName = Binding.Name; Input.Input.Connect(Binding.OutputIndex, Vector);
                bChanged = true;
            }
            // Prune unused legacy inputs (V3DUWShadowUp is derived via cross(Forward, Right),
            // and V3DUWShadowReady is packed into V3DUWShadowNearOrigin.w).
            const TCHAR* PruneNames[] = {
                TEXT("WorldPos"), TEXT("SunDir"), TEXT("NumSamples"), TEXT("Weight"), TEXT("Exposure"),
                TEXT("NoiseScale"), TEXT("RefractionStrength"), TEXT("Activation"),
                TEXT("V3DUWShadowUp"), TEXT("V3DUWShadowReady")
            };
            const int32 RemovedInputs = Custom->Inputs.RemoveAll([&PruneNames](const FCustomInput& In)
            {
                for (const TCHAR* Name : PruneNames)
                    if (In.InputName == FName(Name)) return true;
                return false;
            });
            if (RemovedInputs > 0) bChanged = true;

            // Pack all 16 scalar/vector inputs into a single float4x4 matrix inside the Custom node
            // and early-out before invoking UnderWaterShaftMatrix (5 arguments total).
            const FString MatrixCode = TEXT(
                "[branch] if (!(V3DUWShadowNearOrigin.w >= 0.5f)) return float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
                "float4x4 ShadowParams = float4x4(\n"
                "    V3DUWShadowNearOrigin,\n"
                "    float4(V3DUWShadowFarOrigin.xyz, WaterLevel),\n"
                "    float4(V3DUWShadowRight.xyz, ScreenUV.x),\n"
                "    float4(V3DUWShadowForward.xyz, ScreenUV.y)\n"
                ");\n"
                "return UnderWaterShaftMatrix(V3DUWShadowNear, V3DUWShadowNearSampler, V3DUWShadowFar, V3DUWShadowFarSampler, ShadowParams);");
            if (Custom->Code != MatrixCode)
            {
                Custom->Modify();
                Custom->Code = MatrixCode;
                bChanged = true;
            }
        }
        // Remove orphaned V3DUWShadowUp / V3DUWShadowReady parameter nodes so they don't bloat MIDs.
        TArray<UMaterialExpression*> DeadParams;
        for (UMaterialExpression* Expr : Function->GetExpressions())
        {
            if (auto* Vec = Cast<UMaterialExpressionVectorParameter>(Expr))
            {
                if (Vec->ParameterName == TEXT("V3DUWShadowUp")) DeadParams.Add(Vec);
            }
            else if (auto* Scalar = Cast<UMaterialExpressionScalarParameter>(Expr))
            {
                if (Scalar->ParameterName == TEXT("V3DUWShadowReady")) DeadParams.Add(Scalar);
            }
        }
        for (UMaterialExpression* Dead : DeadParams)
        {
            Function->Modify();
            UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Function, Dead);
            bChanged = true;
        }
        // Depth-based effects after tonemapping operate after temporal AA and produce jagged
        // silhouettes at reduced screen percentage. Run the stock underwater pass before AA.
        if (auto* Material = LoadObject<UMaterial>(nullptr,
            TEXT("/ShaderLibrary/Water/PostProcessing/MPP_UnderWater.MPP_UnderWater")))
        {
            if (Material->BlendableLocation != BL_SceneColorBeforeDOF)
            {
                Material->Modify();
                Material->BlendableLocation = BL_SceneColorBeforeDOF;
                Material->PostEditChange();
                UPackage* Package = Material->GetOutermost(); Package->MarkPackageDirty();
                const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
                FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
                if (!UPackage::SavePackage(Package, Material, *Filename, Args))
                    UE_LOG(LogTemp, Error, TEXT("Could not save underwater pre-AA material: %s"), *Filename);
            }
        }
        if (bChanged)
        {
            UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
            UPackage* Package = Function->GetOutermost(); Package->MarkPackageDirty();
            const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
            FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
            if (!UPackage::SavePackage(Package, Function, *Filename, Args))
                UE_LOG(LogTemp, Error, TEXT("Could not save underwater depth bindings: %s"), *Filename);
        }
        // Compact MF_UnderWaterHazeBlur Custom node: include Underwater.ush directly and pack
        // (ScreenUV, WaterLevel) into a single float3 argument.
        if (auto* HazeFn = LoadObject<UMaterialFunction>(nullptr,
            TEXT("/ShaderLibrary/Water/MaterialFunction/MF_UnderWaterHazeBlur.MF_UnderWaterHazeBlur")))
        {
            bool bHazeChanged = false;
            for (UMaterialExpression* Expr : HazeFn->GetExpressions())
            {
                if (auto* Custom = Cast<UMaterialExpressionCustom>(Expr))
                {
                    if (Custom->Code.Contains(TEXT("UnderWaterHazeBlur"))
                        || Custom->Code.Contains(TEXT("UnderWaterHazeCompact"))
                        || Custom->Code.Contains(TEXT("UnderWaterHaze(")))
                    {
                        const int32 Pruned = Custom->Inputs.RemoveAll([](const FCustomInput& In)
                        {
                            return In.InputName != TEXT("ScreenUV") && In.InputName != TEXT("WaterLevel");
                        });
                        const FString UnderwaterInclude = TEXT("/Plugin/ShaderLibrary/Underwater.ush");
                        const bool bNeedIncludeUpdate = (Custom->IncludeFilePaths.Num() != 1)
                            || (Custom->IncludeFilePaths[0] != UnderwaterInclude);
                        const FString CompactHaze = TEXT("return UnderWaterHaze(float3(ScreenUV, WaterLevel));");
                        if (Pruned > 0 || bNeedIncludeUpdate || Custom->Code != CompactHaze)
                        {
                            HazeFn->Modify();
                            Custom->Modify();
                            Custom->IncludeFilePaths.Reset();
                            Custom->IncludeFilePaths.Add(UnderwaterInclude);
                            Custom->Code = CompactHaze;
                            bHazeChanged = true;
                        }
                    }
                }
            }
            if (bHazeChanged)
            {
                UMaterialEditingLibrary::UpdateMaterialFunction(HazeFn, nullptr);
                UPackage* Package = HazeFn->GetOutermost();
                Package->MarkPackageDirty();
                const FString Filename = FPackageName::LongPackageNameToFilename(
                    Package->GetName(), FPackageName::GetAssetPackageExtension());
                FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
                UPackage::SavePackage(Package, HazeFn, *Filename, Args);
            }
        }

        // Unify ALL SceneColor and SceneDepth pixel coordinates inside MF_UnderWater:
        // Bypass every MF_Distortion output across the entire function graph so foreground
        // characters never split into multiple ghosted/offset layers.
        if (auto* UnderWaterFn = LoadObject<UMaterialFunction>(nullptr,
            TEXT("/ShaderLibrary/Water/MaterialFunction/MF_UnderWater.MF_UnderWater")))
        {
            bool bUWChanged = false;
            UMaterialExpressionScreenPosition* CanonicalScreenPos = nullptr;
            TSet<UMaterialExpression*> DistortionCalls;
            for (UMaterialExpression* Expr : UnderWaterFn->GetExpressions())
            {
                if (!CanonicalScreenPos)
                {
                    if (auto* SP = Cast<UMaterialExpressionScreenPosition>(Expr))
                        CanonicalScreenPos = SP;
                }
                if (auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
                {
                    if (GetNameSafe(Call->MaterialFunction) == TEXT("MF_Distortion"))
                        DistortionCalls.Add(Call);
                }
            }
            if (!CanonicalScreenPos)
            {
                CanonicalScreenPos = CastChecked<UMaterialExpressionScreenPosition>(
                    UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
                        UnderWaterFn, UMaterialExpressionScreenPosition::StaticClass(), -1200, -400));
                bUWChanged = true;
            }
            for (UMaterialExpression* Expr : UnderWaterFn->GetExpressions())
            {
                if (!Expr) continue;
                // Rewire any expression input that consumed MF_Distortion directly to canonical ScreenPosition.
                for (FExpressionInputIterator It{Expr}; It; ++It)
                {
                    if (It->Expression && DistortionCalls.Contains(It->Expression))
                    {
                        UnderWaterFn->Modify();
                        Expr->Modify();
                        It->Connect(0, CanonicalScreenPos);
                        bUWChanged = true;
                    }
                }
                if (auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
                {
                    const FString FnName = GetNameSafe(Call->MaterialFunction);
                    if (FnName == TEXT("MF_UnderWaterHazeBlur") || FnName == TEXT("MF_UnderWaterShaft"))
                    {
                        for (FFunctionExpressionInput& FnInput : Call->FunctionInputs)
                        {
                            if (FnInput.ExpressionInput && FnInput.ExpressionInput->InputName == TEXT("ScreenUV"))
                            {
                                if (FnInput.Input.Expression != CanonicalScreenPos || FnInput.Input.OutputIndex != 0)
                                {
                                    UnderWaterFn->Modify();
                                    FnInput.Input.Connect(0, CanonicalScreenPos);
                                    bUWChanged = true;
                                }
                            }
                        }
                    }
                }
                else if (auto* SceneTex = Cast<UMaterialExpressionSceneTexture>(Expr))
                {
                    // Leaving Coordinates unconnected guarantees pixel-exact default SceneTextureUV for both depth and color.
                    if ((SceneTex->SceneTextureId == PPI_SceneDepth || SceneTex->SceneTextureId == PPI_PostProcessInput0)
                        && SceneTex->Coordinates.IsConnected())
                    {
                        UnderWaterFn->Modify();
                        SceneTex->Coordinates.Expression = nullptr;
                        bUWChanged = true;
                    }
                }
            }
            if (bUWChanged)
            {
                UMaterialEditingLibrary::UpdateMaterialFunction(UnderWaterFn, nullptr);
                UPackage* Package = UnderWaterFn->GetOutermost();
                Package->MarkPackageDirty();
                const FString Filename = FPackageName::LongPackageNameToFilename(
                    Package->GetName(), FPackageName::GetAssetPackageExtension());
                FSavePackageArgs Args;
                Args.TopLevelFlags = RF_Public | RF_Standalone;
                Args.SaveFlags = SAVE_NoError;
                if (!UPackage::SavePackage(Package, UnderWaterFn, *Filename, Args))
                    UE_LOG(LogTemp, Error, TEXT("Could not save unified underwater UV function: %s"), *Filename);
            }
        }

        // Apply physically-based Beer-Lambert absorption & darker deep-ocean scattering defaults
        // directly to M_Water so SingleLayerWater changes color naturally with water depth.
        if (auto* WaterMat = LoadObject<UMaterial>(nullptr,
            TEXT("/ShaderLibrary/Water/WaterSurface/M_Water.M_Water")))
        {
            const FLinearColor BeerLambertAbsorption(0.00460f, 0.00140f, 0.00065f, 1.0f);
            const FLinearColor DeepOceanScattering(0.000018f, 0.000068f, 0.000105f, 1.0f);
            const FLinearColor DarkWaterAlbedo(0.0005f, 0.0020f, 0.0040f, 1.0f);
            bool bWaterChanged = false;
            bool bHasAbsorptionParam = false;
            bool bHasScatteringParam = false;

            for (UMaterialExpression* Expr : WaterMat->GetExpressions())
            {
                if (auto* VecParam = Cast<UMaterialExpressionVectorParameter>(Expr))
                {
                    if (VecParam->ParameterName == TEXT("AbsorptionCoefficients")
                        || VecParam->ParameterName == TEXT("Absorption"))
                    {
                        bHasAbsorptionParam = true;
                        if (!VecParam->DefaultValue.Equals(BeerLambertAbsorption, 1.0e-6f))
                        {
                            WaterMat->Modify();
                            VecParam->Modify();
                            VecParam->DefaultValue = BeerLambertAbsorption;
                            bWaterChanged = true;
                        }
                    }
                    else if (VecParam->ParameterName == TEXT("ScatteringCoefficients")
                        || VecParam->ParameterName == TEXT("Scattering"))
                    {
                        bHasScatteringParam = true;
                        if (!VecParam->DefaultValue.Equals(DeepOceanScattering, 1.0e-6f))
                        {
                            WaterMat->Modify();
                            VecParam->Modify();
                            VecParam->DefaultValue = DeepOceanScattering;
                            bWaterChanged = true;
                        }
                    }
                    else if (VecParam->ParameterName == TEXT("Water Albedo"))
                    {
                        if (!VecParam->DefaultValue.Equals(DarkWaterAlbedo, 1.0e-5f))
                        {
                            WaterMat->Modify();
                            VecParam->Modify();
                            VecParam->DefaultValue = DarkWaterAlbedo;
                            bWaterChanged = true;
                        }
                    }
                }
                else if (auto* ScalarParam = Cast<UMaterialExpressionScalarParameter>(Expr))
                {
                    if (ScalarParam->ParameterName == TEXT("PhaseG")
                        && !FMath::IsNearlyEqual(ScalarParam->DefaultValue, 0.65f, 1.0e-4f))
                    {
                        WaterMat->Modify();
                        ScalarParam->Modify();
                        ScalarParam->DefaultValue = 0.65f;
                        bWaterChanged = true;
                    }
                }
            }

            // If M_Water wired constants into SingleLayerWaterMaterialOutput instead of named parameters,
            // expose AbsorptionCoefficients & ScatteringCoefficients so Beer-Lambert depth physics takes effect.
            if (!bHasAbsorptionParam || !bHasScatteringParam)
            {
                for (UMaterialExpression* Expr : WaterMat->GetExpressions())
                {
                    if (auto* SLW = Cast<UMaterialExpressionSingleLayerWaterMaterialOutput>(Expr))
                    {
                        WaterMat->Modify();
                        if (!bHasAbsorptionParam)
                        {
                            auto* AbsNode = CastChecked<UMaterialExpressionVectorParameter>(
                                UMaterialEditingLibrary::CreateMaterialExpression(
                                    WaterMat, UMaterialExpressionVectorParameter::StaticClass(),
                                    SLW->MaterialExpressionEditorX - 350, SLW->MaterialExpressionEditorY + 120));
                            AbsNode->ParameterName = TEXT("AbsorptionCoefficients");
                            AbsNode->DefaultValue = BeerLambertAbsorption;
                            SLW->AbsorptionCoefficients.Connect(0, AbsNode);
                            bWaterChanged = true;
                        }
                        if (!bHasScatteringParam)
                        {
                            auto* ScatNode = CastChecked<UMaterialExpressionVectorParameter>(
                                UMaterialEditingLibrary::CreateMaterialExpression(
                                    WaterMat, UMaterialExpressionVectorParameter::StaticClass(),
                                    SLW->MaterialExpressionEditorX - 350, SLW->MaterialExpressionEditorY - 120));
                            ScatNode->ParameterName = TEXT("ScatteringCoefficients");
                            ScatNode->DefaultValue = DeepOceanScattering;
                            SLW->ScatteringCoefficients.Connect(0, ScatNode);
                            bWaterChanged = true;
                        }
                        break;
                    }
                }
            }

            if (bWaterChanged)
            {
                WaterMat->PostEditChange();
                UPackage* Package = WaterMat->GetOutermost();
                Package->MarkPackageDirty();
                const FString Filename = FPackageName::LongPackageNameToFilename(
                    Package->GetName(), FPackageName::GetAssetPackageExtension());
                FSavePackageArgs Args;
                Args.TopLevelFlags = RF_Public | RF_Standalone;
                Args.SaveFlags = SAVE_NoError;
                if (!UPackage::SavePackage(Package, WaterMat, *Filename, Args))
                    UE_LOG(LogTemp, Error, TEXT("Could not save Beer-Lambert M_Water defaults: %s"), *Filename);
            }
        }

        auto* Rendering = GetMutableDefault<URendererSettings>();
        if (Rendering->CustomDepthStencil == ECustomDepthStencil::Disabled)
        {
            Rendering->CustomDepthStencil = ECustomDepthStencil::Enabled;
            Rendering->TryUpdateDefaultConfigFile();
            if (auto* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CustomDepth"))) CVar->Set(1, ECVF_SetByProjectSetting);
        }
    }

    template<class T> T* Expression(UMaterial* Material)
    { return CastChecked<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), 0, 0)); }

    UMaterialExpressionScalarParameter* Scalar(UMaterial* Material, const TCHAR* Name, float Value)
    {
        auto* Node = Expression<UMaterialExpressionScalarParameter>(Material);
        Node->ParameterName = Name; Node->DefaultValue = Value;
        return Node;
    }

    void UpgradeGeneratedCaptureFade(UMaterial* Material)
    {
        bool bChanged = false;
        for (UMaterialExpression* Node : Material->GetExpressions())
        {
            auto* Custom = Cast<UMaterialExpressionCustom>(Node);
            if (!Custom || Custom->Description != TEXT("glTF coverage and distance fade")) continue;
            // Signed orthographic depth must not erase a sunward character with FadeDistance=50.
            // Only our known generated expression is upgraded; authored graphs are preserved.
            const FString Old(TEXT("NearFade > 0 ?"));
            if (!Custom->Code.Contains(Old)) continue;
            Custom->Modify();
            Custom->Code = Custom->Code.Replace(*Old, TEXT("NearFade > 0 && ResolvedView.ViewToClip[3][3] < 0.5 ?"));
            bChanged = true;
        }
        if (!bChanged) return;
        Material->PostEditChange();
        UPackage* Package = Material->GetOutermost(); Package->MarkPackageDirty();
        const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
        if (!UPackage::SavePackage(Package, Material, *Filename, Args))
            UE_LOG(LogTemp, Error, TEXT("Could not save orthographic capture fade: %s"), *Filename);
    }

    bool CreateParent(const TCHAR* Name, bool bTwoSided, float GlassOpacity)
    {
        const FString PackageName = FString(TEXT("/Game/V3DGenerated/Materials/")) + Name;
        const FString ObjectPath = PackageName + TEXT(".") + Name;
        // These files are generated once. Never overwrite an existing authored graph on startup.
        if (UMaterial* Existing = LoadObject<UMaterial>(nullptr, *ObjectPath))
        {
            const EBlendMode Expected = GlassOpacity < 1.f ? BLEND_Translucent : BLEND_Masked;
            const bool bValid = Existing->BlendMode == Expected && Existing->TwoSided == bTwoSided;
            if (!bValid) UE_LOG(LogTemp, Error, TEXT("V3D generated parent has incompatible settings: %s"), *ObjectPath);
            if (bValid) UpgradeGeneratedCaptureFade(Existing);
            return bValid;
        }
        auto* White = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
        auto* DitherFunction = LoadObject<UMaterialFunctionInterface>(nullptr,
            TEXT("/Engine/Functions/Engine_MaterialFunctions02/Utility/DitherTemporalAA.DitherTemporalAA"));
        if (!White || !DitherFunction)
        {
            UE_LOG(LogTemp, Error, TEXT("V3D fallback generation requires engine WhiteSquareTexture and DitherTemporalAA assets."));
            return false;
        }
        UPackage* Package = CreatePackage(*PackageName);
        UMaterial* Material = NewObject<UMaterial>(Package, FName(Name), RF_Public | RF_Standalone);
        Material->BlendMode = GlassOpacity < 1.f ? BLEND_Translucent : BLEND_Masked;
        Material->TwoSided = bTwoSided;
        Material->SetShadingModel(MSM_DefaultLit);
        Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
        Material->SetUsageByFlag(MATUSAGE_SkeletalMesh, true);
        Material->DitheredLODTransition = true;
        Material->OpacityMaskClipValue = 0.333f;

        auto* Color = Expression<UMaterialExpressionVectorParameter>(Material);
        Color->ParameterName = TEXT("baseColorFactor"); Color->DefaultValue = FLinearColor::White;
        auto* Texture = Expression<UMaterialExpressionTextureSampleParameter2D>(Material);
        Texture->ParameterName = TEXT("baseColorTexture"); Texture->Texture = White; Texture->SamplerType = SAMPLERTYPE_Color;
        auto* Surface = Expression<UMaterialExpressionMultiply>(Material);
        Surface->A.Connect(0, Color); Surface->B.Connect(0, Texture);
        auto* Tint = Expression<UMaterialExpressionVectorParameter>(Material);
        Tint->ParameterName = TEXT("V3D_GlassTint");
        Tint->DefaultValue = GlassOpacity >= 0.5f && GlassOpacity < 1.f ? FLinearColor(0.3f, 0.4f, 0.5f, 1.f) : FLinearColor::White;
        auto* TintedSurface = Expression<UMaterialExpressionMultiply>(Material);
        TintedSurface->A.Connect(0, Surface); TintedSurface->B.Connect(0, Tint);
        UMaterialEditingLibrary::ConnectMaterialProperty(TintedSurface, TEXT(""), MP_BaseColor);
        UMaterialEditingLibrary::ConnectMaterialProperty(Scalar(Material, TEXT("metallicFactor"), 0.f), TEXT(""), MP_Metallic);
        UMaterialEditingLibrary::ConnectMaterialProperty(Scalar(Material, TEXT("roughnessFactor"), GlassOpacity < 1.f ? 0.1f : 0.8f), TEXT(""), MP_Roughness);

        auto* Coverage = Expression<UMaterialExpressionCustom>(Material);
        Coverage->OutputType = CMOT_Float1;
        Coverage->Description = TEXT("glTF coverage and distance fade");
        Coverage->Code = GlassOpacity < 1.f
            ? TEXT("return saturate(Alpha * TintAlpha * Glass * InstanceFade * (NearFade > 0 && ResolvedView.ViewToClip[3][3] < 0.5 ? saturate(Depth / max(NearFade, 0.001)) : 1));")
            : TEXT("float covered = UseMask > 0.5 ? step(Cutoff, Alpha * TintAlpha) : 1; return covered * saturate(InstanceFade) * (NearFade > 0 && ResolvedView.ViewToClip[3][3] < 0.5 ? saturate(Depth / max(NearFade, 0.001)) : 1);");
        auto AddInput = [Coverage](const TCHAR* InputName, UMaterialExpression* Node, int32 Output = 0)
        {
            FCustomInput& Input = Coverage->Inputs.AddDefaulted_GetRef();
            Input.InputName = InputName; Input.Input.Connect(Output, Node);
        };
        Coverage->Inputs.Reset();
        AddInput(TEXT("Alpha"), Texture, 4); AddInput(TEXT("TintAlpha"), Color, 4);
        AddInput(TEXT("Glass"), Scalar(Material, TEXT("V3D_GlassOpacity"), GlassOpacity));
        AddInput(TEXT("UseMask"), Scalar(Material, TEXT("V3D_UseAlphaMask"), 1.f));
        AddInput(TEXT("Cutoff"), Scalar(Material, TEXT("alphaCutoff"), 0.5f));
        AddInput(TEXT("NearFade"), Scalar(Material, TEXT("FadeDistance"), -1.f));
        AddInput(TEXT("Depth"), Expression<UMaterialExpressionPixelDepth>(Material));
        AddInput(TEXT("InstanceFade"), Expression<UMaterialExpressionPerInstanceFadeAmount>(Material));
        if (GlassOpacity < 1.f)
            UMaterialEditingLibrary::ConnectMaterialProperty(Coverage, TEXT(""), MP_Opacity);
        else
        {
            auto* Dither = Expression<UMaterialExpressionMaterialFunctionCall>(Material);
            Dither->SetMaterialFunction(DitherFunction);
            const bool bConnected = UMaterialEditingLibrary::ConnectMaterialExpressions(Coverage, TEXT(""), Dither, TEXT("Alpha Threshold"))
                || UMaterialEditingLibrary::ConnectMaterialExpressions(Coverage, TEXT(""), Dither, TEXT("AlphaThreshold"));
            if (!bConnected)
            {
                UE_LOG(LogTemp, Error, TEXT("DitherTemporalAA alpha input could not be bound; fallback not saved: %s"), *ObjectPath);
                Material->ClearFlags(RF_Public | RF_Standalone);
                Material->MarkAsGarbage();
                return false;
            }
            UMaterialEditingLibrary::ConnectMaterialProperty(Dither, TEXT(""), MP_OpacityMask);
        }
        UMaterialEditingLibrary::LayoutMaterialExpressions(Material);
        Material->PostEditChange();
        FAssetRegistryModule::AssetCreated(Material);
        Package->MarkPackageDirty();
        const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
        FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
        const bool bSaved = UPackage::SavePackage(Package, Material, *Filename, Args);
        UE_LOG(LogTemp, Display, TEXT("V3D fallback parent: %s Saved=%s"), *ObjectPath, bSaved ? TEXT("true") : TEXT("false"));
        return bSaved;
    }

    bool CreateFoliageMaterial()
    {
        const FString PackageName = TEXT("/ShaderLibrary/Foliage/M_Foliage");
        const FString ObjectPath = PackageName + TEXT(".M_Foliage");
        if (UMaterial* Existing = LoadObject<UMaterial>(nullptr, *ObjectPath))
        {
            return Existing->BlendMode == BLEND_Masked && Existing->TwoSided;
        }
        auto* White = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
        auto* MPC = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/ShaderLibrary/MPC_ShaderLibrary.MPC_ShaderLibrary"));
        if (!White || !MPC)
        {
            UE_LOG(LogTemp, Warning, TEXT("M_Foliage generation skipped: WhiteSquareTexture or MPC_ShaderLibrary unavailable."));
            return false;
        }

        UPackage* Package = CreatePackage(*PackageName);
        UMaterial* Material = NewObject<UMaterial>(Package, FName(TEXT("M_Foliage")), RF_Public | RF_Standalone);
        Material->BlendMode = BLEND_Masked;
        Material->TwoSided = true;
        Material->SetShadingModel(MSM_TwoSidedFoliage);
        Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
        Material->SetUsageByFlag(MATUSAGE_SkeletalMesh, true);
        Material->OpacityMaskClipValue = 0.333f;

        // Inherit baseColorTexture, baseColorFactor, alphaCutoff, roughnessFactor from the glTF/V3D material.
        auto* Color = Expression<UMaterialExpressionVectorParameter>(Material);
        Color->ParameterName = TEXT("baseColorFactor");
        Color->DefaultValue = FLinearColor::White;

        auto* Texture = Expression<UMaterialExpressionTextureSampleParameter2D>(Material);
        Texture->ParameterName = TEXT("baseColorTexture");
        Texture->Texture = White;
        Texture->SamplerType = SAMPLERTYPE_Color;

        auto* Surface = Expression<UMaterialExpressionMultiply>(Material);
        Surface->A.Connect(0, Color);
        Surface->B.Connect(0, Texture);
        UMaterialEditingLibrary::ConnectMaterialProperty(Surface, TEXT(""), MP_BaseColor);

        // Natural backlit foliage subsurface tint derived directly from the material's texture color.
        auto* SubsurfaceScale = Expression<UMaterialExpressionConstant>(Material);
        SubsurfaceScale->R = 0.45f;
        auto* Subsurface = Expression<UMaterialExpressionMultiply>(Material);
        Subsurface->A.Connect(0, Surface);
        Subsurface->B.Connect(0, SubsurfaceScale);
        UMaterialEditingLibrary::ConnectMaterialProperty(Subsurface, TEXT(""), MP_SubsurfaceColor);

        UMaterialEditingLibrary::ConnectMaterialProperty(Scalar(Material, TEXT("metallicFactor"), 0.0f), TEXT(""), MP_Metallic);
        UMaterialEditingLibrary::ConnectMaterialProperty(Scalar(Material, TEXT("roughnessFactor"), 0.65f), TEXT(""), MP_Roughness);

        // Alpha-masked leaf cutout using baseColorTexture alpha and alphaCutoff.
        auto* MaskCustom = Expression<UMaterialExpressionCustom>(Material);
        MaskCustom->OutputType = CMOT_Float1;
        MaskCustom->Description = TEXT("Foliage alpha mask");
        MaskCustom->Code = TEXT("return step(Cutoff, Alpha * TintAlpha);");
        MaskCustom->Inputs.Reset();
        auto AddCustomInput = [](UMaterialExpressionCustom* Node, const TCHAR* Name, UMaterialExpression* Expr, int32 OutIdx = 0)
        {
            FCustomInput& In = Node->Inputs.AddDefaulted_GetRef();
            In.InputName = Name;
            In.Input.Connect(OutIdx, Expr);
        };
        AddCustomInput(MaskCustom, TEXT("Alpha"), Texture, 4);
        AddCustomInput(MaskCustom, TEXT("TintAlpha"), Color, 4);
        AddCustomInput(MaskCustom, TEXT("Cutoff"), Scalar(Material, TEXT("alphaCutoff"), 0.35f));
        UMaterialEditingLibrary::ConnectMaterialProperty(MaskCustom, TEXT(""), MP_OpacityMask);

        // Compact 5-input interactive vertex bending + wind WPO via /Plugin/ShaderLibrary/Foliage.ush.
        auto* BendCustom = Expression<UMaterialExpressionCustom>(Material);
        BendCustom->OutputType = CMOT_Float3;
        BendCustom->Description = TEXT("Foliage interactive bending");
        BendCustom->IncludeFilePaths.Add(TEXT("/Plugin/ShaderLibrary/Foliage.ush"));
        BendCustom->Code = TEXT("return FoliageInteractiveBendCompact(WorldPos, Interactor0, Interactor1, Interactor2, Interactor3);");
        BendCustom->Inputs.Reset();

        auto MakeMPCParam = [&](const FName ParamName)
        {
            auto* Node = Expression<UMaterialExpressionCollectionParameter>(Material);
            Node->Collection = MPC;
            Node->ParameterName = ParamName;
            Node->ParameterId = MPC->GetParameterId(ParamName);
            return Node;
        };

        AddCustomInput(BendCustom, TEXT("WorldPos"), Expression<UMaterialExpressionWorldPosition>(Material));
        AddCustomInput(BendCustom, TEXT("Interactor0"), MakeMPCParam(TEXT("FoliageInteractor0")));
        AddCustomInput(BendCustom, TEXT("Interactor1"), MakeMPCParam(TEXT("FoliageInteractor1")));
        AddCustomInput(BendCustom, TEXT("Interactor2"), MakeMPCParam(TEXT("FoliageInteractor2")));
        AddCustomInput(BendCustom, TEXT("Interactor3"), MakeMPCParam(TEXT("FoliageInteractor3")));
        UMaterialEditingLibrary::ConnectMaterialProperty(BendCustom, TEXT(""), MP_WorldPositionOffset);

        UMaterialEditingLibrary::LayoutMaterialExpressions(Material);
        Material->PostEditChange();
        FAssetRegistryModule::AssetCreated(Material);
        Package->MarkPackageDirty();
        const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        const bool bSaved = UPackage::SavePackage(Package, Material, *Filename, Args);
        UE_LOG(LogTemp, Display, TEXT("ShaderLibrary Foliage material: %s Saved=%s"), *ObjectPath, bSaved ? TEXT("true") : TEXT("false"));
        return bSaved;
    }

    void EnsureCookedParents()
    {
        UpgradeUnderwaterBindings();
        CreateFoliageMaterial();
        bool bReady = CreateParent(TEXT("M_Opaque"), false, 1.f);
        bReady &= CreateParent(TEXT("M_OpaqueTwoSided"), true, 1.f);
        bReady &= CreateParent(TEXT("M_Glass"), true, 0.25f);
        bReady &= CreateParent(TEXT("M_TintedGlass"), true, 0.5f);
        if (!bReady) return;
        UProjectPackagingSettings* Packaging = GetMutableDefault<UProjectPackagingSettings>();
        const FString Directory = TEXT("/Game/V3DGenerated/Materials");
        if (!Packaging->DirectoriesToAlwaysCook.ContainsByPredicate([&](const FDirectoryPath& Path) { return Path.Path == Directory; }))
        {
            FDirectoryPath Path; Path.Path = Directory; Packaging->DirectoriesToAlwaysCook.Add(Path);
            if (!Packaging->TryUpdateDefaultConfigFile())
                UE_LOG(LogTemp, Error, TEXT("V3D fallback cook directory could not be saved to DefaultGame.ini: %s"), *Directory);
        }
    }
}
