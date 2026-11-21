#include "V3DPhysicalMaterialUpgrade.h"
#include "World/PhysicalMaterialProfile.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionQualitySwitch.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Engine/Texture2D.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace V3DMaterialDefaults
{
namespace
{
    void SaveChanged(UObject* Asset)
    {
        UPackage* Package = Asset->GetOutermost();
        Package->MarkPackageDirty();
        const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        if (!UPackage::SavePackage(Package, Asset, *Filename, Args))
            UE_LOG(LogTemp, Error, TEXT("Physical material migration could not save %s; do not cook stale assets."), *Filename);
    }

    UMaterialExpressionScalarParameter* Scalar(UMaterial* Material, const TCHAR* Name, const float Value, bool& Changed)
    {
        for (UMaterialExpression* Expression : Material->GetExpressions())
            if (auto* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression))
                if (Parameter->ParameterName == Name)
                {
                    if (!FMath::IsNearlyEqual(Parameter->DefaultValue, Value, 1.e-7f))
                    {
                        Parameter->Modify(); Parameter->DefaultValue = Value; Changed = true;
                    }
                    return Parameter;
                }
        auto* Parameter = CastChecked<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(
            Material, UMaterialExpressionScalarParameter::StaticClass()));
        Parameter->ParameterName = Name; Parameter->DefaultValue = Value; Changed = true;
        return Parameter;
    }

    // Connect newly created nodes through their declared output.
    bool Connect(FExpressionInput* Input, UMaterialExpression* Expression, int32 Output = 0)
    {
        if (!Input || !Expression || (Input->Expression == Expression && Input->OutputIndex == Output)) return false;
        Input->Connect(Output, Expression);
        return true;
    }

    void ApplyInstanceProfile(const TCHAR* Path, TConstArrayView<V3DPhysicalMaterial::FScalar> Scalars,
        TConstArrayView<V3DPhysicalMaterial::FVectorParameter> Vectors = {}, bool Water = false)
    {
        auto* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, Path);
        if (!Instance) return;
        bool Changed = false;
        for (const auto& P : Scalars)
        {
            float Current = 0;
            const FMaterialParameterInfo Info(P.Name);
            if (Instance->GetScalarParameterValue(Info, Current) && !FMath::IsNearlyEqual(Current, P.Value, 1.e-7f))
            { Instance->Modify(); Instance->SetScalarParameterValueEditorOnly(Info, P.Value); Changed = true; }
        }
        for (const auto& P : Vectors)
        {
            FLinearColor Current;
            const FMaterialParameterInfo Info(P.Name);
            if (Instance->GetVectorParameterValue(Info, Current) && !Current.Equals(P.Value, 1.e-8f))
            { Instance->Modify(); Instance->SetVectorParameterValueEditorOnly(Info, P.Value); Changed = true; }
        }
        if (Water && (Instance->BasePropertyOverrides.bOverride_BlendMode || Instance->BasePropertyOverrides.bOverride_ShadingModel))
        {
            Instance->Modify();
            Instance->BasePropertyOverrides.bOverride_BlendMode = false;
            Instance->BasePropertyOverrides.bOverride_ShadingModel = false;
            Changed = true;
        }
        if (Changed) { Instance->PostEditChange(); SaveChanged(Instance); }
    }

    void RestorePhysicalResponse(UMaterial* Material)
    {
        if (!Material) return;
        bool Changed = false;
        for (EMaterialProperty Property : {MP_Metallic, MP_Specular, MP_Roughness})
        {
            auto* Input = Material->GetExpressionInputForProperty(Property);
            auto* Switch = Input ? Cast<UMaterialExpressionQualitySwitch>(Input->Expression) : nullptr;
            if (Switch && Switch->Desc == TEXT("V3D_LowQualityMatte") && Switch->Default.IsConnected())
            {
                Material->Modify(); *Input = Switch->Default; Changed = true;
            }
        }
        // Keep source textures and per-material factors. Flattening metallic/roughness to
        // (0,1) on Low permanently changed glass/metal into chalk; scalability controls lighting.
        if (Changed) { Material->PostEditChange(); SaveChanged(Material); }
    }

    void UpgradeFallbackSurface(const TCHAR* Path)
    {
        auto* Material = LoadObject<UMaterial>(nullptr, Path);
        if (!Material) return;
        bool Generated = false;
        for (UMaterialExpression* Expression : Material->GetExpressions())
            if (auto* Custom = Cast<UMaterialExpressionCustom>(Expression))
            {
                if (Custom->Description == TEXT("V3D fallback PBR normal v1")) return;
                Generated |= Custom->Description == TEXT("glTF coverage and distance fade");
            }
        if (!Generated) return;
        auto* White = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
        auto* FlatNormal = LoadObject<UTexture2D>(nullptr, TEXT("/glTFRuntime/T_glTFRuntimeNormal.T_glTFRuntimeNormal"));
        auto* LinearWhite = LoadObject<UTexture2D>(nullptr, TEXT("/glTFRuntime/T_glTFRuntimeOne.T_glTFRuntimeOne"));
        if (!White || !FlatNormal || !LinearWhite) return;
        Material->Modify();
        bool Changed = false;
        auto Sample = [&](const TCHAR* Name, UTexture2D* Texture, EMaterialSamplerType Type)
        {
            auto* Node = CastChecked<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(
                Material, UMaterialExpressionTextureSampleParameter2D::StaticClass()));
            Node->ParameterName = Name; Node->Texture = Texture; Node->SamplerType = Type; return Node;
        };
        auto Multiply = [&](UMaterialExpression* A, int32 Channel, UMaterialExpression* B)
        {
            auto* Node = CastChecked<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(
                Material, UMaterialExpressionMultiply::StaticClass()));
            Node->A.Connect(Channel, A); Node->B.Connect(0, B); return Node;
        };
        auto Vector = [&](const TCHAR* Name, FLinearColor Value)
        {
            auto* Node = CastChecked<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(
                Material, UMaterialExpressionVectorParameter::StaticClass()));
            Node->ParameterName = Name; Node->DefaultValue = Value; return Node;
        };
        auto* ORM = Sample(TEXT("metallicRoughnessTexture"), LinearWhite, SAMPLERTYPE_LinearColor);
        const bool Glass = Material->BlendMode == BLEND_Translucent;
        Connect(Material->GetExpressionInputForProperty(MP_Metallic), Multiply(ORM, 3, Scalar(Material, TEXT("metallicFactor"), 0.0f, Changed)));
        Connect(Material->GetExpressionInputForProperty(MP_Roughness), Multiply(ORM, 2, Scalar(Material, TEXT("roughnessFactor"),
            Glass ? V3DPhysicalMaterial::GlassRoughness : 0.65f, Changed)));
        Connect(Material->GetExpressionInputForProperty(MP_Specular), Scalar(Material, TEXT("specularFactor"), 0.5f, Changed));
        Connect(Material->GetExpressionInputForProperty(MP_AmbientOcclusion), Sample(TEXT("occlusionTexture"), LinearWhite, SAMPLERTYPE_LinearColor), 1);
        auto* Emission = Multiply(Sample(TEXT("emissiveTexture"), White, SAMPLERTYPE_Color), 0,
            Vector(TEXT("emissiveFactor"), FLinearColor::Black));
        Connect(Material->GetExpressionInputForProperty(MP_EmissiveColor), Multiply(Emission, 0,
            Scalar(Material, TEXT("emissiveStrength"), 1.0f, Changed)));
        auto* Normal = CastChecked<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(
            Material, UMaterialExpressionCustom::StaticClass()));
        Normal->Description = TEXT("V3D fallback PBR normal v1"); Normal->OutputType = CMOT_Float3;
        Normal->Code = TEXT("float3 n = float3(N.xy * Scale.xy, N.z); return n * rsqrt(max(dot(n,n), 0.000001));");
        auto& N = Normal->Inputs.AddDefaulted_GetRef(); N.InputName = TEXT("N");
        N.Input.Connect(0, Sample(TEXT("normalTexture"), FlatNormal, SAMPLERTYPE_Normal));
        auto& Scale = Normal->Inputs.AddDefaulted_GetRef(); Scale.InputName = TEXT("Scale");
        Scale.Input.Connect(0, Vector(TEXT("normalTexScale"), FLinearColor::White));
        Connect(Material->GetExpressionInputForProperty(MP_Normal), Normal);
        if (Glass) Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
        Material->PostEditChange(); SaveChanged(Material);
    }
}

void UpgradePhysicalMaterials()
{
    UpgradeWaterReflectionMaterials();
    UpgradeCelestialMaterials();
    for (const TCHAR* Path : {TEXT("/Game/Resources/Materials/MI_Water.MI_Water"), TEXT("/Game/Resources/Materials/MI_Ocean.MI_Ocean")})
        ApplyInstanceProfile(Path, V3DPhysicalMaterial::WaterScalars, V3DPhysicalMaterial::WaterVectors, true);
    const V3DPhysicalMaterial::FScalar Sky[] = {
        {TEXT("MoonIntensity"), V3DPhysicalMaterial::MoonIntensity}, {TEXT("MoonSize"), V3DPhysicalMaterial::MoonRadiusRadians},
        {TEXT("MoonVisibility"), 1.0f}, {TEXT("SunVisibility"), 1.0f},
        {TEXT("SunAngularRadius"), V3DPhysicalMaterial::CelestialRadiusRadians},
        {TEXT("StarIntensity"), V3DPhysicalMaterial::StarIntensity}};
    ApplyInstanceProfile(TEXT("/Game/Resources/Materials/MI_Skybox.MI_Skybox"), Sky);
    const V3DPhysicalMaterial::FScalar Surface[] = {
        {TEXT("metallicFactor"), 0.0f}, {TEXT("roughnessFactor"), 0.65f}, {TEXT("specularFactor"), 0.5f}, {TEXT("normalTexScale"), 1.0f}};
    for (const TCHAR* Path : {TEXT("/Game/Resources/Materials/V3D/MaterialInstances/MI_Opaque.MI_Opaque"),
        TEXT("/Game/Resources/Materials/V3D/MaterialInstances/MI_Opaque_TwoSided.MI_Opaque_TwoSided"),
        TEXT("/Game/Resources/Materials/V3D/MaterialInstances/MI_Translucent.MI_Translucent"),
        TEXT("/Game/Resources/Materials/V3D/MaterialInstances/MI_Translucent_TwoSided.MI_Translucent_TwoSided")})
        ApplyInstanceProfile(Path, Surface);
    for (const TCHAR* Path : {TEXT("/Game/Resources/Materials/V3D/M_Terrain.M_Terrain"),
        TEXT("/Game/Resources/Materials/V3D/M_V3D.M_V3D"), TEXT("/Game/Resources/Materials/V3D/M_Toon.M_Toon"),
        TEXT("/Game/V3DGenerated/Materials/M_Opaque.M_Opaque"), TEXT("/Game/V3DGenerated/Materials/M_OpaqueTwoSided.M_OpaqueTwoSided")})
        RestorePhysicalResponse(LoadObject<UMaterial>(nullptr, Path));
    for (const TCHAR* Path : {TEXT("/Game/V3DGenerated/Materials/M_Opaque.M_Opaque"),
        TEXT("/Game/V3DGenerated/Materials/M_OpaqueTwoSided.M_OpaqueTwoSided"),
        TEXT("/Game/V3DGenerated/Materials/M_Glass.M_Glass"), TEXT("/Game/V3DGenerated/Materials/M_TintedGlass.M_TintedGlass")})
        UpgradeFallbackSurface(Path);
}
}
