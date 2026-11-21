#include "V3DPhysicalMaterialUpgrade.h"
#include "World/PhysicalMaterialProfile.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionEyeAdaptation.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace V3DMaterialDefaults
{
namespace Celestial
{
    TArray<UMaterialExpression*> Snapshot(UObject* Asset)
    {
        TArray<UMaterialExpression*> Result;
        if (auto* Function = Cast<UMaterialFunction>(Asset))
            for (UMaterialExpression* E : Function->GetExpressions()) Result.Add(E);
        if (auto* Material = Cast<UMaterial>(Asset))
            for (UMaterialExpression* E : Material->GetExpressions()) Result.Add(E);
        return Result;
    }

    template<typename T> T* Add(UObject* Asset)
    {
        if (auto* Function = Cast<UMaterialFunction>(Asset))
            return CastChecked<T>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, T::StaticClass()));
        return CastChecked<T>(UMaterialEditingLibrary::CreateMaterialExpression(CastChecked<UMaterial>(Asset), T::StaticClass()));
    }

    void Save(UObject* Asset)
    {
        if (auto* Function = Cast<UMaterialFunction>(Asset)) UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
        else Asset->PostEditChange();
        UPackage* Package = Asset->GetOutermost();
        Package->MarkPackageDirty();
        FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
        const FString Path = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        if (!UPackage::SavePackage(Package, Asset, *Path, Args))
            UE_LOG(LogTemp, Error, TEXT("Celestial material migration could not save %s; do not cook stale assets."), *Path);
    }

    UMaterialExpressionScalarParameter* Scalar(UObject* Asset, const TCHAR* Name, float Value, bool& Changed)
    {
        for (UMaterialExpression* E : Snapshot(Asset))
            if (auto* P = Cast<UMaterialExpressionScalarParameter>(E))
                if (P->ParameterName == Name)
                {
                    if (P->DefaultValue != Value) { P->Modify(); P->DefaultValue = Value; Changed = true; }
                    return P;
                }
        auto* P = Add<UMaterialExpressionScalarParameter>(Asset);
        P->ParameterName = Name; P->DefaultValue = Value; Changed = true;
        return P;
    }

    UMaterialExpressionVectorParameter* Direction(UObject* Asset, const TCHAR* Name, FLinearColor Value, bool& Changed)
    {
        for (UMaterialExpression* E : Snapshot(Asset))
            if (auto* P = Cast<UMaterialExpressionVectorParameter>(E))
                if (P->ParameterName == Name)
                {
                    if (P->DefaultValue != Value) { P->Modify(); P->DefaultValue = Value; Changed = true; }
                    return P;
                }
        auto* P = Add<UMaterialExpressionVectorParameter>(Asset);
        P->ParameterName = Name; P->DefaultValue = Value; Changed = true;
        return P;
    }

    bool Connect(FExpressionInput& Input, UMaterialExpression* Source, int32 Output = 0)
    {
        if (Input.Expression == Source && Input.OutputIndex == Output) return false;
        Input.Connect(Output, Source); return true;
    }

    FCustomInput Input(const TCHAR* Name, UMaterialExpression* Source, int32 Output = 0)
    {
        FCustomInput Result; Result.InputName = Name; Result.Input.Connect(Output, Source); return Result;
    }

    UMaterialExpressionCustom* Custom(UObject* Asset, const TCHAR* Description, bool& Changed)
    {
        for (UMaterialExpression* E : Snapshot(Asset))
            if (auto* C = Cast<UMaterialExpressionCustom>(E))
                if (C->Description == Description) return C;
        auto* C = Add<UMaterialExpressionCustom>(Asset);
        C->Description = Description; Changed = true; return C;
    }

    bool Configure(UMaterialExpressionCustom* Node, const TCHAR* Code, ECustomMaterialOutputType Type,
        const TArray<FCustomInput>& Inputs)
    {
        const FString Include = TEXT("/Plugin/ShaderLibrary/Skybox.ush");
        bool Changed = Node->Code != Code || Node->OutputType != Type || !Node->IncludeFilePaths.Contains(Include)
            || Node->Inputs.Num() != Inputs.Num();
        if (!Changed)
            for (int32 I = 0; I < Inputs.Num(); ++I)
            {
                const auto& A = Node->Inputs[I]; const auto& B = Inputs[I];
                Changed |= A.InputName != B.InputName || A.Input.Expression != B.Input.Expression
                    || A.Input.OutputIndex != B.Input.OutputIndex || A.Input.Mask != B.Input.Mask
                    || A.Input.MaskR != B.Input.MaskR || A.Input.MaskG != B.Input.MaskG
                    || A.Input.MaskB != B.Input.MaskB || A.Input.MaskA != B.Input.MaskA;
            }
        if (Changed)
        {
            Node->Modify(); Node->Code = Code; Node->OutputType = Type;
            Node->IncludeFilePaths.AddUnique(Include); Node->Inputs = Inputs;
        }
        return Changed;
    }

    bool UpgradeMoon(UMaterialFunction* Function)
    {
        UMaterialExpressionTextureSampleParameter2D* Texture = nullptr;
        UMaterialExpressionCustom* UV = nullptr;
        UMaterialExpressionEyeAdaptation* Exposure = nullptr;
        TArray<UMaterialExpressionFunctionOutput*> Outputs;
        for (UMaterialExpression* E : Snapshot(Function))
        {
            if (auto* T = Cast<UMaterialExpressionTextureSampleParameter2D>(E))
                if (T->ParameterName == TEXT("Moon")) Texture = T;
            if (auto* C = Cast<UMaterialExpressionCustom>(E))
                if (C->Description == TEXT("MoonUV")) UV = C;
            if (auto* Eye = Cast<UMaterialExpressionEyeAdaptation>(E)) Exposure = Eye;
            if (auto* O = Cast<UMaterialExpressionFunctionOutput>(E)) Outputs.Add(O);
        }
        FCustomInput* LightInput = UV ? UV->Inputs.FindByPredicate([](const auto& I) { return I.InputName == TEXT("lightDir"); }) : nullptr;
        if (!Texture || !Texture->Texture || !LightInput || Outputs.IsEmpty())
        { UE_LOG(LogTemp, Error, TEXT("MF_Moon: required texture, UV direction or output is missing.")); return false; }
        bool Changed = false;
        auto* MoonDirection = Direction(Function, TEXT("V3DMoonDirection"), FLinearColor(0, 0, 1, 0), Changed);
        if (LightInput->Input.Expression != MoonDirection)
        { UV->Modify(); LightInput->Input.Connect(0, MoonDirection); Changed = true; }
        Scalar(Function, TEXT("MoonSize"), V3DPhysicalMaterial::MoonRadiusRadians, Changed);
        auto* Brightness = Scalar(Function, TEXT("MoonIntensity"), V3DPhysicalMaterial::MoonIntensity, Changed);
        if (!Exposure) { Exposure = Add<UMaterialExpressionEyeAdaptation>(Function); Changed = true; }
        auto* Tone = Custom(Function, TEXT("V3D exposure stable moon"), Changed);
        // Upgrade existing wrappers too. Do not skip older generated nodes by description.
        Changed |= Configure(Tone, TEXT("return V3DMoonRadiance(TextureColor, Brightness, Exposure);"), CMOT_Float3,
            {Input(TEXT("TextureColor"), Texture), Input(TEXT("Brightness"), Brightness), Input(TEXT("Exposure"), Exposure)});
        for (auto* Output : Outputs) Changed |= Connect(Output->A, Tone);
        if (Changed) Save(Function);
        return true;
    }

    void UpgradeStars(UMaterialFunction* Function)
    {
        UMaterialExpressionCustom* UV = nullptr;
        for (UMaterialExpression* E : Snapshot(Function))
            if (auto* C = Cast<UMaterialExpressionCustom>(E))
                if (C->Description == TEXT("StarUV")) UV = C;
        if (!UV || !UV->Inputs.ContainsByPredicate([](const auto& I) { return I.InputName == TEXT("MoonDir"); }))
        { UE_LOG(LogTemp, Error, TEXT("MF_Stars: star UV direction input is missing.")); return; }
        bool Changed = false;
        // Match MoonUV's observer-to-moon direction. The old atmosphere LightDir
        // input can remain constant in this sky pass and has a legacy sign inversion.
        auto* MoonDirection = Direction(Function, TEXT("V3DMoonDirection"), FLinearColor(0, 0, 1, 0), Changed);
        auto* Input = UV->Inputs.FindByPredicate([](const auto& I) { return I.InputName == TEXT("MoonDir"); });
        if (Input->Input.Expression != MoonDirection || Input->Input.OutputIndex != 0)
        { UV->Modify(); Input->Input.Connect(0, MoonDirection); Changed = true; }
        if (Changed) Save(Function);
    }

    void UpgradeSky(UMaterial* Sky)
    {
        auto* Emissive = Sky->GetExpressionInputForProperty(MP_EmissiveColor);
        UMaterialExpressionCameraVectorWS* Camera = nullptr;
        UMaterialExpressionMaterialFunctionCall* Moon = nullptr;
        UMaterialExpressionStaticSwitchParameter* LegacySun = nullptr;
        UMaterialExpressionCustom* SunDisk = nullptr;
        UMaterialExpressionCustom* Composite = nullptr;
        const auto Nodes = Snapshot(Sky);
        for (UMaterialExpression* E : Nodes)
        {
            if (auto* C = Cast<UMaterialExpressionCameraVectorWS>(E)) Camera = C;
            if (auto* F = Cast<UMaterialExpressionMaterialFunctionCall>(E))
                if (F->MaterialFunction && F->MaterialFunction->GetName() == TEXT("MF_Moon")) Moon = F;
            if (auto* S = Cast<UMaterialExpressionStaticSwitchParameter>(E))
                if (S->ParameterName == TEXT("LargeSun")) LegacySun = S;
            if (auto* C = Cast<UMaterialExpressionCustom>(E))
            {
                if (C->Description == TEXT("V3D matched sun disk")) SunDisk = C;
                if (C->Description == TEXT("V3D textured moon composite")) Composite = C;
            }
        }
        if (!Emissive || !Emissive->IsConnected() || !Moon)
        { UE_LOG(LogTemp, Error, TEXT("M_Skybox: moon call or emissive output is missing.")); return; }
        FExpressionInput Background = *Emissive;
        if (Composite && Emissive->Expression == Composite)
        {
            const auto* Existing = Composite->Inputs.FindByPredicate([](const auto& I) { return I.InputName == TEXT("Background"); });
            if (!Existing || !Existing->Input.IsConnected())
            { UE_LOG(LogTemp, Error, TEXT("M_Skybox: existing moon composite has no background.")); return; }
            Background = Existing->Input;
        }
        bool Changed = false;
        if (!Camera) { Camera = Add<UMaterialExpressionCameraVectorWS>(Sky); Changed = true; }
        auto* SunDirection = Direction(Sky, TEXT("V3DSunDirection"), FLinearColor(0, 0, -1, 0), Changed);
        auto* MoonDirection = Direction(Sky, TEXT("V3DMoonDirection"), FLinearColor(0, 0, 1, 0), Changed);
        auto* SunRadius = Scalar(Sky, TEXT("SunAngularRadius"), V3DPhysicalMaterial::CelestialRadiusRadians, Changed);
        auto* MoonRadius = Scalar(Sky, TEXT("MoonSize"), V3DPhysicalMaterial::MoonRadiusRadians, Changed);
        auto* SunVisibility = Scalar(Sky, TEXT("SunVisibility"), 1.f, Changed);
        auto* MoonVisibility = Scalar(Sky, TEXT("MoonVisibility"), 1.f, Changed);
        TArray<UMaterialExpressionMultiply*> SunConsumers;
        for (UMaterialExpression* E : Nodes)
        {
            if (auto* M = Cast<UMaterialExpressionMultiply>(E))
                if ((LegacySun && M->A.Expression == LegacySun) || (SunDisk && M->A.Expression == SunDisk)) SunConsumers.Add(M);
            if (auto* L = Cast<UMaterialExpressionLinearInterpolate>(E))
                if (L->A.Expression == Moon && L->B.IsConnected())
                {
                    // Remove the old RGB-as-alpha moon overlay from the background. Moon
                    // texture color is now unpremultiplied and composed only at final output.
                    L->Modify(); L->A = L->B; L->Alpha = FExpressionInput(); L->ConstAlpha = 0.f;
                    L->Desc = TEXT("V3D background without legacy moon overlay"); Changed = true;
                }
        }
        if (!SunDisk) SunDisk = Custom(Sky, TEXT("V3D matched sun disk"), Changed);
        Changed |= Configure(SunDisk, TEXT("return V3DCelestialCoverage(-ViewToCamera, BodyDirection, Radius, Visibility);"), CMOT_Float1,
            {Input(TEXT("ViewToCamera"), Camera), Input(TEXT("BodyDirection"), SunDirection),
             Input(TEXT("Radius"), SunRadius), Input(TEXT("Visibility"), SunVisibility)});
        for (auto* M : SunConsumers) Changed |= Connect(M->A, SunDisk);
        if (SunConsumers.IsEmpty()) UE_LOG(LogTemp, Error, TEXT("M_Skybox: sun mask is not connected to luminance."));
        auto* Coverage = Custom(Sky, TEXT("V3D moon disk coverage"), Changed);
        Changed |= Configure(Coverage, TEXT("return V3DCelestialCoverage(-ViewToCamera, BodyDirection, Radius, Visibility);"), CMOT_Float1,
            {Input(TEXT("ViewToCamera"), Camera), Input(TEXT("BodyDirection"), MoonDirection),
             Input(TEXT("Radius"), MoonRadius), Input(TEXT("Visibility"), MoonVisibility)});
        if (!Composite) Composite = Custom(Sky, TEXT("V3D textured moon composite"), Changed);
        FCustomInput BackgroundInput; BackgroundInput.InputName = TEXT("Background"); BackgroundInput.Input = Background;
        Changed |= Configure(Composite, TEXT("return V3DComposeMoon(Background, MoonColor, Coverage);"), CMOT_Float3,
            {BackgroundInput, Input(TEXT("MoonColor"), Moon), Input(TEXT("Coverage"), Coverage)});
        Changed |= Connect(*Emissive, Composite);
        if (!Sky->bIsSky || !Sky->GetShadingModels().HasOnlyShadingModel(MSM_Unlit))
        { Sky->bIsSky = true; Sky->SetShadingModel(MSM_Unlit); Changed = true; }
        if (Changed) Save(Sky);
    }
}

void UpgradeCelestialMaterials()
{
    auto* Moon = LoadObject<UMaterialFunction>(nullptr, TEXT("/ShaderLibrary/Skybox/Functions/MF_Moon.MF_Moon"));
    auto* Sky = LoadObject<UMaterial>(nullptr, TEXT("/ShaderLibrary/Skybox/M_Skybox.M_Skybox"));
    auto* Stars = LoadObject<UMaterialFunction>(nullptr, TEXT("/ShaderLibrary/Skybox/Functions/MF_Stars.MF_Stars"));
    if (!Moon || !Sky || !Stars) { UE_LOG(LogTemp, Error, TEXT("Required moon/stars/sky material assets are missing.")); return; }
    Celestial::UpgradeStars(Stars);
    if (Celestial::UpgradeMoon(Moon)) Celestial::UpgradeSky(Sky);
}
}
