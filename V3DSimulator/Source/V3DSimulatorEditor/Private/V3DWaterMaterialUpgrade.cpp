#include "V3DPhysicalMaterialUpgrade.h"
#include "World/PhysicalMaterialProfile.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionGetMaterialAttributes.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionTwoSidedSign.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace V3DMaterialDefaults
{
int32 EnsureWaterOpacityOutput(UMaterialExpressionGetMaterialAttributes* Read, bool& Changed)
{
    if (!Read) return INDEX_NONE;
    // UE keeps the attribute IDs and output pins in parallel, with a leading bundle pin.
    // A raw AttributeGetTypes assignment followed by PostEditChange leaves Outputs stale
    // and asserts in UE 5.8. Let the engine update both arrays together.
    if (Read->GetOutputs().Num() != Read->AttributeGetTypes.Num() + 1)
    {
        UE_LOG(LogTemp, Error, TEXT("Water opacity reader has inconsistent attribute/output counts; migration stopped."));
        return INDEX_NONE;
    }
    const FGuid OpacityID = FMaterialAttributeDefinitionMap::GetID(MP_Opacity);
    if (!Read->AttributeGetTypes.Contains(OpacityID))
    {
        Read->Modify();
        Read->CreateOrGetOutputAttribute(MP_Opacity);
        Changed = true;
    }
    const int32 AttributeIndex = Read->AttributeGetTypes.IndexOfByKey(OpacityID);
    const int32 OutputIndex = AttributeIndex + 1;
    if (AttributeIndex == INDEX_NONE || Read->GetOutputs().Num() != Read->AttributeGetTypes.Num() + 1
        || !Read->GetOutputs().IsValidIndex(OutputIndex))
    {
        UE_LOG(LogTemp, Error, TEXT("Cannot create a consistent water opacity output; migration stopped."));
        return INDEX_NONE;
    }
    return OutputIndex;
}

namespace
{
    template<class AssetType>
    bool ApplyDefaults(AssetType* Asset)
    {
        bool Changed = false;
        for (UMaterialExpression* Expression : Asset->GetExpressions())
        {
            if (auto* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression))
                for (const auto& P : V3DPhysicalMaterial::WaterScalars)
                    if (Scalar->ParameterName == P.Name && !FMath::IsNearlyEqual(Scalar->DefaultValue, P.Value, 1.e-7f))
                    { Scalar->Modify(); Scalar->DefaultValue = P.Value; Changed = true; }
            if (auto* Vector = Cast<UMaterialExpressionVectorParameter>(Expression))
                for (const auto& P : V3DPhysicalMaterial::WaterVectors)
                    if (Vector->ParameterName == P.Name && !Vector->DefaultValue.Equals(P.Value, 1.e-8f))
                    { Vector->Modify(); Vector->DefaultValue = P.Value; Changed = true; }
        }
        return Changed;
    }

    void Save(UObject* Asset)
    {
        if (auto* Function = Cast<UMaterialFunction>(Asset))
            UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
        else Asset->PostEditChange();
        UPackage* Package = Asset->GetOutermost();
        Package->MarkPackageDirty();
        const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        if (!UPackage::SavePackage(Package, Asset, *Filename, Args))
            UE_LOG(LogTemp, Error, TEXT("Water reflection migration could not save %s; resolve this before cooking."), *Filename);
    }

    bool Bind(UMaterialExpressionSetMaterialAttributes* Set, EMaterialProperty Property,
        const TCHAR* Pin, UMaterialExpression* Source, int32 Output = 0)
    {
        const auto Inputs = Set->GetInputsView();
        for (int32 Index = 0; Index < Inputs.Num(); ++Index)
            if (Set->GetInputName(Index) == Pin && Inputs[Index]
                && Inputs[Index]->Expression == Source && Inputs[Index]->OutputIndex == Output) return false;
        Set->Modify();
        Set->CreateOrGetInputAttribute(Property);
        if (!Set->ConnectInputAttribute(Property, Source, Output))
        { UE_LOG(LogTemp, Error, TEXT("Cannot bind water attribute %s"), Pin); return false; }
        return true;
    }

    UMaterialExpressionSetMaterialAttributes* OutputAttributes(UMaterialFunction* Function)
    {
        for (UMaterialExpression* Expression : Function->GetExpressions())
            if (auto* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
                if (auto* Set = Cast<UMaterialExpressionSetMaterialAttributes>(Output->A.Expression)) return Set;
        return nullptr;
    }

    void UpgradeAttributes(UMaterialFunction* Function)
    {
        auto* Set = OutputAttributes(Function);
        if (!Set) { UE_LOG(LogTemp, Error, TEXT("MF_WaterAttributes has no attribute output.")); return; }
        bool Changed = ApplyDefaults(Function);
        UMaterialExpressionScalarParameter* Opacity = nullptr;
        UMaterialExpressionVectorParameter* Albedo = nullptr;
        UMaterialExpressionCustom* SurfaceOpacity = nullptr;
        UMaterialExpressionTwoSidedSign* FaceSign = nullptr;
        for (UMaterialExpression* Expression : Function->GetExpressions())
        {
            if (auto* P = Cast<UMaterialExpressionScalarParameter>(Expression))
                if (P->ParameterName == TEXT("Water Opacity")) Opacity = P;
            if (auto* P = Cast<UMaterialExpressionVectorParameter>(Expression))
                if (P->ParameterName == TEXT("Water Albedo")) Albedo = P;
            if (auto* P = Cast<UMaterialExpressionCustom>(Expression))
                if (P->Description == TEXT("V3D above-water opacity")) SurfaceOpacity = P;
            if (auto* P = Cast<UMaterialExpressionTwoSidedSign>(Expression)) FaceSign = P;
        }
        if (!Albedo) { UE_LOG(LogTemp, Error, TEXT("MF_WaterAttributes is missing Water Albedo.")); return; }
        if (!Opacity)
        {
            Opacity = CastChecked<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
                Function, UMaterialExpressionScalarParameter::StaticClass()));
            Opacity->ParameterName = TEXT("Water Opacity"); Opacity->DefaultValue = V3DPhysicalMaterial::WaterSurfaceOpacity; Changed = true;
        }
        if (!FaceSign)
        {
            FaceSign = CastChecked<UMaterialExpressionTwoSidedSign>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
                Function, UMaterialExpressionTwoSidedSign::StaticClass()));
            Changed = true;
        }
        const FString AboveWaterCode = TEXT(
            "float front = saturate(FaceSign);\n"
            "float ndv = saturate(abs(Parameters.CameraVector.z));\n"
            "float fresnel = pow(1.0f - ndv, 4.0f);\n"
            "return saturate(lerp(saturate(Opacity), 0.92f, fresnel)) * front;");
        if (!SurfaceOpacity)
        {
            SurfaceOpacity = CastChecked<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
                Function, UMaterialExpressionCustom::StaticClass()));
            SurfaceOpacity->Description = TEXT("V3D above-water opacity");
            SurfaceOpacity->OutputType = CMOT_Float1;
            SurfaceOpacity->Code = AboveWaterCode;
            SurfaceOpacity->Inputs.Reset(); SurfaceOpacity->Inputs.SetNum(2);
            SurfaceOpacity->Inputs[0].InputName = TEXT("Opacity"); SurfaceOpacity->Inputs[0].Input.Connect(0, Opacity);
            SurfaceOpacity->Inputs[1].InputName = TEXT("FaceSign"); SurfaceOpacity->Inputs[1].Input.Connect(0, FaceSign);
            Changed = true;
        }
        else if (SurfaceOpacity->Code != AboveWaterCode)
        {
            SurfaceOpacity->Modify();
            SurfaceOpacity->Code = AboveWaterCode;
            Changed = true;
        }
        // The front face gets the new haze; the back face starts at zero so the existing
        // underside function keeps its transparent Snell window and opaque TIR region.
        Changed |= Bind(Set, MP_Opacity, TEXT("Opacity"), SurfaceOpacity);
        // The attached graph divides albedo by CausticsMaxIntensity, whose default is zero.
        // Surface reflectance must not depend on a decal intensity (or divide by zero).
        Changed |= Bind(Set, MP_BaseColor, TEXT("Base Color"), Albedo);
        if (Changed) Save(Function);
    }

    void UpgradeNormals(UMaterialFunction* Function)
    {
        auto* Set = OutputAttributes(Function);
        UMaterialExpressionWorldPosition* Position = nullptr;
        UMaterialExpressionTextureObject* Texture = nullptr;
        UMaterialExpressionPixelDepth* Depth = nullptr;
        UMaterialExpressionCustom* Normal = nullptr;
        TMap<FName, UMaterialExpression*> Sources;
        for (UMaterialExpression* Expression : Function->GetExpressions())
        {
            if (auto* P = Cast<UMaterialExpressionWorldPosition>(Expression)) Position = P;
            if (auto* P = Cast<UMaterialExpressionTextureObject>(Expression)) Texture = P;
            if (auto* P = Cast<UMaterialExpressionPixelDepth>(Expression)) Depth = P;
            if (auto* P = Cast<UMaterialExpressionFunctionInput>(Expression)) Sources.Add(P->InputName, P);
            if (auto* P = Cast<UMaterialExpressionScalarParameter>(Expression)) Sources.Add(P->ParameterName, P);
            if (auto* P = Cast<UMaterialExpressionCustom>(Expression))
                if (P->Description == TEXT("V3D continuous water normals")) Normal = P;
        }
        const FName Required[] = {TEXT("Time"), TEXT("Speed"), TEXT("Near Scale"), TEXT("Distant Scale"),
            TEXT("Near Normal Strength"), TEXT("Far Normal Strength"),
            TEXT("Water Distant Normal Offset"), TEXT("Water Distant Normal Length")};
        if (!Set || !Position || !Texture || !Depth)
        { UE_LOG(LogTemp, Error, TEXT("MF_WaterTextureSurface is missing its wave inputs.")); return; }
        for (const FName Name : Required)
            if (!Sources.Contains(Name))
            { UE_LOG(LogTemp, Error, TEXT("MF_WaterTextureSurface is missing %s."), *Name.ToString()); return; }
        bool Changed = ApplyDefaults(Function);
        if (!Normal)
        {
            Normal = CastChecked<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
                Function, UMaterialExpressionCustom::StaticClass()));
            Normal->Description = TEXT("V3D continuous water normals"); Changed = true;
        }
        const FString Code = TEXT("return V3DWaterSurfaceNormal(WaveTexture, WaveTextureSampler, Position.xy, Time, Speed, NearScale, FarScale, NearStrength, FarStrength, ViewDepth, BlendStart, BlendLength);");
        const FString Include = TEXT("/Plugin/ShaderLibrary/WaterSurface.ush");
        if (Normal->Code != Code || Normal->OutputType != CMOT_Float3
            || Normal->IncludeFilePaths.Num() != 1 || Normal->IncludeFilePaths[0] != Include)
        {
            Normal->Modify(); Normal->Code = Code; Normal->OutputType = CMOT_Float3;
            Normal->IncludeFilePaths = {Include}; Changed = true;
        }
        TArray<FCustomInput> Inputs;
        auto Input = [&](const TCHAR* Name, UMaterialExpression* Source)
        { FCustomInput& I = Inputs.AddDefaulted_GetRef(); I.InputName = Name; I.Input.Connect(0, Source); };
        Input(TEXT("WaveTexture"), Texture); Input(TEXT("Position"), Position); Input(TEXT("ViewDepth"), Depth);
        const TCHAR* Names[] = {TEXT("Time"), TEXT("Speed"), TEXT("NearScale"), TEXT("FarScale"),
            TEXT("NearStrength"), TEXT("FarStrength"), TEXT("BlendStart"), TEXT("BlendLength")};
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(Required); ++Index) Input(Names[Index], Sources.FindChecked(Required[Index]));
        bool InputsChanged = Normal->Inputs.Num() != Inputs.Num();
        if (!InputsChanged)
            for (int32 Index = 0; Index < Inputs.Num(); ++Index)
                InputsChanged |= Normal->Inputs[Index].InputName != Inputs[Index].InputName
                    || Normal->Inputs[Index].Input.Expression != Inputs[Index].Input.Expression
                    || Normal->Inputs[Index].Input.OutputIndex != 0;
        if (InputsChanged) { Normal->Modify(); Normal->Inputs = MoveTemp(Inputs); Changed = true; }
        // Only the Normal pin changes. WPO, shoreline/exclusion masks and wave time stay intact.
        Changed |= Bind(Set, MP_Normal, TEXT("Normal"), Normal);
        if (Changed) Save(Function);
    }

    void PreserveUnderside(UMaterial* Material)
    {
        bool Changed = ApplyDefaults(Material);
        if (!Material->TwoSided) { Material->TwoSided = true; Changed = true; }
        if (!Material->bTangentSpaceNormal) { Material->bTangentSpaceNormal = true; Changed = true; }
        if (Material->RefractionMethod != RM_PixelNormalOffset)
        { Material->RefractionMethod = RM_PixelNormalOffset; Changed = true; }
        FExpressionInput* Final = Material->GetExpressionInputForProperty(MP_MaterialAttributes);
        auto* Set = Final ? Cast<UMaterialExpressionSetMaterialAttributes>(Final->Expression) : nullptr;
        // This patch targets the supplied project's final wrapper. Refuse an unknown graph
        // instead of silently disconnecting authored water/exclusion/underside functions.
        if (!Material->bUseMaterialAttributes || !Set || Set->Desc != TEXT("V3D final water optics")
            || !Set->GetInput(0) || !Set->GetInput(0)->IsConnected())
        { UE_LOG(LogTemp, Error, TEXT("M_Water does not have the expected final optics bundle.")); return; }
        UMaterialExpressionGetMaterialAttributes* Read = nullptr;
        for (UMaterialExpression* Expression : Material->GetExpressions())
            if (auto* P = Cast<UMaterialExpressionGetMaterialAttributes>(Expression))
                if (P->Desc == TEXT("V3D preserve underwater opacity")) Read = P;
        if (!Read)
        {
            Read = CastChecked<UMaterialExpressionGetMaterialAttributes>(UMaterialEditingLibrary::CreateMaterialExpression(
                Material, UMaterialExpressionGetMaterialAttributes::StaticClass()));
            Read->Desc = TEXT("V3D preserve underwater opacity"); Changed = true;
        }
        const int32 OpacityOutput = EnsureWaterOpacityOutput(Read, Changed);
        if (OpacityOutput == INDEX_NONE) return;
        const FExpressionInput& Upstream = *Set->GetInput(0);
        FExpressionInput* Bundle = Read->GetInput(0);
        if (!Bundle) { UE_LOG(LogTemp, Error, TEXT("Cannot read upstream water attributes.")); return; }
        if (Bundle->Expression != Upstream.Expression || Bundle->OutputIndex != Upstream.OutputIndex)
        { Read->Modify(); *Bundle = Upstream; Changed = true; }
        // Use the actual attribute slot, including when other outputs already exist.
        // Above: modest Water Opacity. Below: preserve the underside's critical-angle mask.
        Changed |= Bind(Set, MP_Opacity, TEXT("Opacity"), Read, OpacityOutput);
        if (Changed) Save(Material);
    }
}

void UpgradeWaterReflectionMaterials()
{
    auto* Attributes = LoadObject<UMaterialFunction>(nullptr, TEXT("/ShaderLibrary/Water/MaterialFunction/MF_WaterAttributes.MF_WaterAttributes"));
    auto* Normals = LoadObject<UMaterialFunction>(nullptr, TEXT("/ShaderLibrary/Water/MaterialFunction/MF_WaterTextureSurface.MF_WaterTextureSurface"));
    auto* Underside = LoadObject<UMaterialFunction>(nullptr, TEXT("/ShaderLibrary/Water/MaterialFunction/MF_Water_Underside.MF_Water_Underside"));
    auto* Water = LoadObject<UMaterial>(nullptr, TEXT("/ShaderLibrary/Water/WaterSurface/M_Water.M_Water"));
    if (!Attributes || !Normals || !Underside || !Water)
    { UE_LOG(LogTemp, Error, TEXT("Water reflection migration requires the supplied ShaderLibrary water assets.")); return; }
    UpgradeAttributes(Attributes);
    if (ApplyDefaults(Underside)) Save(Underside);
    UpgradeNormals(Normals);
    PreserveUnderside(Water);
}
}
