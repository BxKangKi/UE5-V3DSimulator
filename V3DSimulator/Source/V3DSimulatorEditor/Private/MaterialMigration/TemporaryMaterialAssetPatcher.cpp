// Copyright © 2026 BxKangKi. Licensed under the MIT License.
//
// ============================================================================
// TEMPORARY MATERIAL-EDITING EDITOR CODE (ISOLATED FOR ONE-SHOT REMOVAL)
// ============================================================================
// Delete the `Source/V3DSimulatorEditor/Private/MaterialMigration/` directory
// and remove the marked `[TEMPORARY MATERIAL PATCHER]` block in
// `V3DSimulatorEditor.cpp` after assets are saved in Unreal Editor.
// ============================================================================

#include "MaterialMigration/TemporaryMaterialAssetPatcher.h"

#include "Engine/Engine.h"
#include "Framework/Commands/UIAction.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Textures/SlateIcon.h"
#include "ToolMenus.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "World/PhysicalMaterialProfile.h"

DEFINE_LOG_CATEGORY_STATIC(LogV3DTempMaterialPatcher, Log, All);

#define LOCTEXT_NAMESPACE "V3DTempMaterialPatcher"

namespace V3DTempMaterialPatcher
{
    namespace
    {
        FDelegateHandle PostEngineInitHandle;

        constexpr const TCHAR* ToonMaterialPath = TEXT("/Game/Resources/Materials/V3D/M_Toon.M_Toon");
        constexpr const TCHAR* ToonBaseColorNodeDesc = TEXT("V3D_ToonCastShadowBaseColor");
        constexpr const TCHAR* ToonEmissiveNodeDesc = TEXT("V3D_ToonCastShadowEmissive");
        constexpr const TCHAR* ToonShaderIncludePath = TEXT("/Plugin/ShaderLibrary/Toon.ush");
        constexpr const TCHAR* ToonBaseColorCode =
            TEXT("return V3DToonComputeBaseColor(Parameters, Albedo, ShadeColor, WorldNormal, ShadowThreshold, ShadowSmoothness);");
        constexpr const TCHAR* ToonEmissiveCode =
            TEXT("return V3DToonComputeShadowEmissive(Parameters, Albedo, ShadeColor, WorldNormal, ShadowThreshold, ShadowSmoothness, AmbientWeight, ExtraEmissive);");

        bool SaveAssetPackage(UObject* Asset)
        {
            if (!Asset) return false;
            UPackage* Package = Asset->GetOutermost();
            if (!Package) return false;
            Package->MarkPackageDirty();
            const FString Filename = FPackageName::LongPackageNameToFilename(
                Package->GetName(), FPackageName::GetAssetPackageExtension());
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            SaveArgs.SaveFlags = SAVE_NoError;
            if (!UPackage::SavePackage(Package, Asset, *Filename, SaveArgs))
            {
                UE_LOG(LogV3DTempMaterialPatcher, Error, TEXT("Failed to save patched material package: %s"), *Filename);
                return false;
            }
            UE_LOG(LogV3DTempMaterialPatcher, Display, TEXT("Saved patched material package: %s"), *Filename);
            return true;
        }

        const FExpressionInput* FindFunctionCallInput(const UMaterialExpressionMaterialFunctionCall* Call, const FName InputName)
        {
            if (!Call) return nullptr;
            for (const FFunctionExpressionInput& FuncInput : Call->FunctionInputs)
            {
                if (FuncInput.ExpressionInput && FuncInput.ExpressionInput->InputName == InputName && FuncInput.Input.Expression)
                {
                    return &FuncInput.Input;
                }
            }
            return nullptr;
        }

        void AddCustomInput(UMaterialExpressionCustom* Custom, const FName Name, const FExpressionInput* Source, UMaterialExpression* FallbackExpr)
        {
            FCustomInput& Entry = Custom->Inputs.AddDefaulted_GetRef();
            Entry.InputName = Name;
            if (Source && Source->Expression)
            {
                Entry.Input = *Source;
            }
            else if (FallbackExpr)
            {
                Entry.Input.Connect(0, FallbackExpr);
            }
        }

        UMaterialExpressionConstant* MakeScalarConstant(UMaterial* Material, float Value, int32 PosX, int32 PosY)
        {
            auto* Const = NewObject<UMaterialExpressionConstant>(Material);
            Const->R = Value;
            Const->MaterialExpressionEditorX = PosX;
            Const->MaterialExpressionEditorY = PosY;
            Material->GetExpressionCollection().AddExpression(Const);
            return Const;
        }

        UMaterialExpressionConstant3Vector* MakeColorConstant(UMaterial* Material, const FLinearColor& Color, int32 PosX, int32 PosY)
        {
            auto* Const = NewObject<UMaterialExpressionConstant3Vector>(Material);
            Const->Constant = Color;
            Const->MaterialExpressionEditorX = PosX;
            Const->MaterialExpressionEditorY = PosY;
            Material->GetExpressionCollection().AddExpression(Const);
            return Const;
        }

        void PatchToonMaterial()
        {
            UMaterial* Material = LoadObject<UMaterial>(nullptr, ToonMaterialPath);
            if (!Material)
            {
                UE_LOG(LogV3DTempMaterialPatcher, Warning, TEXT("M_Toon not found at %s; skipping Toon shadow patch."), ToonMaterialPath);
                return;
            }

            UMaterialEditorOnlyData* EditorData = Material->GetEditorOnlyData();
            if (!EditorData)
            {
                return;
            }

            UMaterialExpressionCustom* BaseColorCustom = nullptr;
            UMaterialExpressionCustom* EmissiveCustom = nullptr;
            UMaterialExpressionMaterialFunctionCall* ToonCall = nullptr;

            for (UMaterialExpression* Expr : Material->GetExpressionCollection().Expressions)
            {
                if (!Expr) continue;
                if (auto* Custom = Cast<UMaterialExpressionCustom>(Expr))
                {
                    if (Custom->Desc == ToonBaseColorNodeDesc) BaseColorCustom = Custom;
                    else if (Custom->Desc == ToonEmissiveNodeDesc) EmissiveCustom = Custom;
                }
                else if (auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
                {
                    if (Call->MaterialFunction && Call->MaterialFunction->GetName() == TEXT("MF_Toon"))
                    {
                        ToonCall = Call;
                    }
                }
            }

            const bool bAlreadyPatched =
                BaseColorCustom != nullptr
                && EmissiveCustom != nullptr
                && EditorData->BaseColor.Expression == BaseColorCustom
                && EditorData->EmissiveColor.Expression == EmissiveCustom
                && BaseColorCustom->Code == ToonBaseColorCode
                && EmissiveCustom->Code == ToonEmissiveCode
                && BaseColorCustom->IncludeFilePaths.Contains(ToonShaderIncludePath)
                && EmissiveCustom->IncludeFilePaths.Contains(ToonShaderIncludePath)
                && Material->GetShadingModels().HasShadingModel(MSM_DefaultLit);

            if (bAlreadyPatched)
            {
                return;
            }

            if (!ToonCall)
            {
                UE_LOG(LogV3DTempMaterialPatcher, Warning, TEXT("M_Toon does not contain an MF_Toon call node; skipping automatic rewire."));
                return;
            }

            Material->PreEditChange(nullptr);
            Material->Modify();
            Material->SetShadingModel(MSM_DefaultLit);

            const FExpressionInput* InAlbedo = FindFunctionCallInput(ToonCall, TEXT("BaseColor"));
            const FExpressionInput* InShadeColor = FindFunctionCallInput(ToonCall, TEXT("ShadeColor"));
            const FExpressionInput* InWorldNormal = FindFunctionCallInput(ToonCall, TEXT("WorldNormal"));
            const FExpressionInput* InThreshold = FindFunctionCallInput(ToonCall, TEXT("ShadowThreshold"));
            const FExpressionInput* InSmoothness = FindFunctionCallInput(ToonCall, TEXT("ShadowSmoothness"));
            const FExpressionInput* InAmbient = FindFunctionCallInput(ToonCall, TEXT("AmbientWeight"));
            const FExpressionInput* InEmissive = FindFunctionCallInput(ToonCall, TEXT("Emissive"));

            const int32 BaseX = ToonCall->MaterialExpressionEditorX + 260;
            const int32 BaseY = ToonCall->MaterialExpressionEditorY - 120;

            UMaterialExpression* FallbackAlbedo = InAlbedo ? nullptr : MakeColorConstant(Material, FLinearColor(0.5f, 0.5f, 0.5f, 1.f), BaseX - 200, BaseY - 80);
            UMaterialExpression* FallbackShade = InShadeColor ? nullptr : MakeColorConstant(Material, FLinearColor(0.55f, 0.60f, 0.72f, 1.f), BaseX - 200, BaseY - 20);
            UMaterialExpression* FallbackNormal = InWorldNormal ? nullptr : MakeColorConstant(Material, FLinearColor(0.f, 0.f, 1.f, 1.f), BaseX - 200, BaseY + 40);
            UMaterialExpression* FallbackThreshold = InThreshold ? nullptr : MakeScalarConstant(Material, 0.08f, BaseX - 200, BaseY + 100);
            UMaterialExpression* FallbackSmoothness = InSmoothness ? nullptr : MakeScalarConstant(Material, 0.06f, BaseX - 200, BaseY + 140);
            UMaterialExpression* FallbackAmbient = InAmbient ? nullptr : MakeScalarConstant(Material, 0.34f, BaseX - 200, BaseY + 180);
            UMaterialExpression* FallbackEmissive = InEmissive ? nullptr : MakeColorConstant(Material, FLinearColor::Black, BaseX - 200, BaseY + 220);

            if (!BaseColorCustom)
            {
                BaseColorCustom = NewObject<UMaterialExpressionCustom>(Material);
                Material->GetExpressionCollection().AddExpression(BaseColorCustom);
            }
            BaseColorCustom->Modify();
            BaseColorCustom->Desc = ToonBaseColorNodeDesc;
            BaseColorCustom->Description = TEXT("V3D Toon Shadow-Receiving BaseColor");
            BaseColorCustom->OutputType = CMOT_Float3;
            BaseColorCustom->IncludeFilePaths = { ToonShaderIncludePath };
            BaseColorCustom->Code = ToonBaseColorCode;
            BaseColorCustom->MaterialExpressionEditorX = BaseX;
            BaseColorCustom->MaterialExpressionEditorY = BaseY;
            BaseColorCustom->Inputs.Reset();
            AddCustomInput(BaseColorCustom, TEXT("Albedo"), InAlbedo, FallbackAlbedo);
            AddCustomInput(BaseColorCustom, TEXT("ShadeColor"), InShadeColor, FallbackShade);
            AddCustomInput(BaseColorCustom, TEXT("WorldNormal"), InWorldNormal, FallbackNormal);
            AddCustomInput(BaseColorCustom, TEXT("ShadowThreshold"), InThreshold, FallbackThreshold);
            AddCustomInput(BaseColorCustom, TEXT("ShadowSmoothness"), InSmoothness, FallbackSmoothness);

            if (!EmissiveCustom)
            {
                EmissiveCustom = NewObject<UMaterialExpressionCustom>(Material);
                Material->GetExpressionCollection().AddExpression(EmissiveCustom);
            }
            EmissiveCustom->Modify();
            EmissiveCustom->Desc = ToonEmissiveNodeDesc;
            EmissiveCustom->Description = TEXT("V3D Toon Shadow Floor Emissive");
            EmissiveCustom->OutputType = CMOT_Float3;
            EmissiveCustom->IncludeFilePaths = { ToonShaderIncludePath };
            EmissiveCustom->Code = ToonEmissiveCode;
            EmissiveCustom->MaterialExpressionEditorX = BaseX;
            EmissiveCustom->MaterialExpressionEditorY = BaseY + 240;
            EmissiveCustom->Inputs.Reset();
            AddCustomInput(EmissiveCustom, TEXT("Albedo"), InAlbedo, FallbackAlbedo);
            AddCustomInput(EmissiveCustom, TEXT("ShadeColor"), InShadeColor, FallbackShade);
            AddCustomInput(EmissiveCustom, TEXT("WorldNormal"), InWorldNormal, FallbackNormal);
            AddCustomInput(EmissiveCustom, TEXT("ShadowThreshold"), InThreshold, FallbackThreshold);
            AddCustomInput(EmissiveCustom, TEXT("ShadowSmoothness"), InSmoothness, FallbackSmoothness);
            AddCustomInput(EmissiveCustom, TEXT("AmbientWeight"), InAmbient, FallbackAmbient);
            AddCustomInput(EmissiveCustom, TEXT("ExtraEmissive"), InEmissive, FallbackEmissive);

            EditorData->BaseColor.Connect(0, BaseColorCustom);
            EditorData->EmissiveColor.Connect(0, EmissiveCustom);

            Material->PostEditChange();
            SaveAssetPackage(Material);
        }

        void PatchWaterMaterialInstance(const TCHAR* AssetPath)
        {
            UMaterialInstanceConstant* MIC = LoadObject<UMaterialInstanceConstant>(nullptr, AssetPath);
            if (!MIC)
            {
                return;
            }

            bool bChanged = false;
            const bool bSingleLayer = MIC->GetShadingModels().HasShadingModel(MSM_SingleLayerWater);

            for (const auto& Parameter : V3DPhysicalMaterial::WaterScalars)
            {
                float Current = 0.0f;
                const FMaterialParameterInfo Info(FName(Parameter.Name));
                if (!MIC->GetScalarParameterValue(Info, Current)) continue;
                const bool bOpacity = FName(Parameter.Name) == TEXT("Opacity") || FName(Parameter.Name) == TEXT("Water Opacity");
                const float TargetValue = (bOpacity && !bSingleLayer) ? 0.68f : Parameter.Value;
                if (!FMath::IsNearlyEqual(Current, TargetValue, 1.e-6f))
                {
                    if (!bChanged)
                    {
                        MIC->PreEditChange(nullptr);
                        MIC->Modify();
                        bChanged = true;
                    }
                    MIC->SetScalarParameterValueEditorOnly(Info, TargetValue);
                }
            }

            for (const auto& Parameter : V3DPhysicalMaterial::WaterVectors)
            {
                FLinearColor Current;
                const FMaterialParameterInfo Info(FName(Parameter.Name));
                if (!MIC->GetVectorParameterValue(Info, Current)) continue;
                if (!Current.Equals(Parameter.Value, 1.e-6f))
                {
                    if (!bChanged)
                    {
                        MIC->PreEditChange(nullptr);
                        MIC->Modify();
                        bChanged = true;
                    }
                    MIC->SetVectorParameterValueEditorOnly(Info, Parameter.Value);
                }
            }

            if (bChanged)
            {
                MIC->PostEditChange();
                SaveAssetPackage(MIC);
            }
        }
    }

    void ApplyAll()
    {
        if (!GIsEditor) return;
        PatchToonMaterial();
        PatchWaterMaterialInstance(TEXT("/Game/Resources/Materials/MI_Water.MI_Water"));
        PatchWaterMaterialInstance(TEXT("/Game/Resources/Materials/MI_Ocean.MI_Ocean"));
    }

    void Register()
    {
        if (GEngine)
        {
            ApplyAll();
        }
        else if (!PostEngineInitHandle.IsValid())
        {
            PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddStatic(&ApplyAll);
        }
    }

    void Unregister()
    {
        if (PostEngineInitHandle.IsValid())
        {
            FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
            PostEngineInitHandle.Reset();
        }
    }

    void RegisterMenuEntry(FToolMenuSection& Section)
    {
        Section.AddMenuEntry("RunTemporaryMaterialPatch",
            LOCTEXT("RunTempMaterialPatch", "Run Temporary Material Patch (Toon Shadows & Water)"),
            LOCTEXT("RunTempMaterialPatchTooltip", "Update M_Toon for real cast-shadow reception and sync MI_Water / MI_Ocean colors. Safe to delete MaterialMigration/ once saved."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([]
            {
                const FScopedTransaction Transaction(LOCTEXT("RunTempMaterialPatchTransaction", "Run Temporary Material Patch"));
                ApplyAll();
            })));
    }
}

#undef LOCTEXT_NAMESPACE
