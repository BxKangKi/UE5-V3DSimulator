#include "ShaderLibraryModule.h"

#include "Interfaces/IPluginManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#if WITH_EDITOR
#include "Engine/Engine.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogShaderLibraryModule, Log, All);

void FShaderLibraryModule::StartupModule()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ShaderLibrary"));
    if (!Plugin.IsValid())
    {
        UE_LOG(LogShaderLibraryModule, Error, TEXT("Unable to locate ShaderLibrary plugin while registering shader source directory."));
        return;
    }

    const FString ShaderDirectory = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
    if (!FPaths::DirectoryExists(ShaderDirectory))
    {
        UE_LOG(LogShaderLibraryModule, Error, TEXT("Shader source directory does not exist: %s"), *ShaderDirectory);
        return;
    }

#if WITH_EDITOR
    {
        TArray<FString> HlslFiles;
        IFileManager::Get().FindFiles(HlslFiles, *FPaths::Combine(ShaderDirectory, TEXT("*.hlsl")), true, false);
        for (const FString& FileName : HlslFiles)
        {
            const FString HlslPath = FPaths::Combine(ShaderDirectory, FileName);
            const FString UshPath = FPaths::ChangeExtension(HlslPath, TEXT("ush"));
            TArray<uint8> SrcBytes, DstBytes;
            if (FFileHelper::LoadFileToArray(SrcBytes, *HlslPath))
            {
                if (!FFileHelper::LoadFileToArray(DstBytes, *UshPath) || SrcBytes != DstBytes)
                {
                    FFileHelper::SaveArrayToFile(SrcBytes, *UshPath);
                }
            }
        }
    }
#endif
    AddShaderSourceDirectoryMapping(TEXT("/Plugin/ShaderLibrary"), ShaderDirectory);
#if WITH_EDITOR
    // Content packages cannot be loaded during PostConfigInit. Upgrade the existing asset only
    // after the engine is initialized; packaged games use the resulting cooked collection.
    if (GEngine) EnsureShaderLibraryParameters();
    else PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
        this, &FShaderLibraryModule::EnsureShaderLibraryParameters);
#endif
    UE_LOG(LogShaderLibraryModule, Log, TEXT("Registered shader source mapping /Plugin/ShaderLibrary -> %s"), *ShaderDirectory);
}

void FShaderLibraryModule::ShutdownModule()
{
#if WITH_EDITOR
    FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
#endif
}

#if WITH_EDITOR
void FShaderLibraryModule::EnsureShaderLibraryParameters()
{
    if (!GIsEditor) return;
    UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(
        nullptr, TEXT("/ShaderLibrary/MPC_ShaderLibrary.MPC_ShaderLibrary"));
    if (!Collection)
    {
        UE_LOG(LogShaderLibraryModule, Error, TEXT("MPC_ShaderLibrary could not be loaded."));
        return;
    }

    const FName ParameterName(TEXT("MaxRenderDistance"));
    const bool bAddDistance = !Collection->GetParameterId(ParameterName).IsValid();
    const FName RemovedParameter(TEXT("CelShadingMode"));
    const bool bRemoveCel = Collection->GetParameterId(RemovedParameter).IsValid();

    const FName FoliageVectorNames[] = {
        TEXT("FoliageInteractor0"),
        TEXT("FoliageInteractor1"),
        TEXT("FoliageInteractor2"),
        TEXT("FoliageInteractor3")
    };
    bool bAddFoliageVectors = false;
    for (const FName& VecName : FoliageVectorNames)
    {
        if (!Collection->GetParameterId(VecName).IsValid())
        {
            bAddFoliageVectors = true;
            break;
        }
    }

    if (!bAddDistance && !bRemoveCel && !bAddFoliageVectors) return;

    Collection->Modify();
    if (bAddDistance || bRemoveCel)
    {
        FProperty* ScalarProperty = FindFProperty<FProperty>(UMaterialParameterCollection::StaticClass(),
            GET_MEMBER_NAME_CHECKED(UMaterialParameterCollection, ScalarParameters));
        Collection->PreEditChange(ScalarProperty);
        Collection->ScalarParameters.RemoveAll([RemovedParameter](const FCollectionScalarParameter& Item)
            { return Item.ParameterName == RemovedParameter; });
        if (bAddDistance)
        {
            FCollectionScalarParameter Parameter;
            Parameter.ParameterName = ParameterName;
            Parameter.Id = FGuid::NewGuid();
            Parameter.DefaultValue = 819200.0f;
            Collection->ScalarParameters.Add(Parameter);
        }
        FPropertyChangedEvent Event(ScalarProperty, EPropertyChangeType::ValueSet);
        Collection->PostEditChangeProperty(Event);
    }

    if (bAddFoliageVectors)
    {
        FProperty* VectorProperty = FindFProperty<FProperty>(UMaterialParameterCollection::StaticClass(),
            GET_MEMBER_NAME_CHECKED(UMaterialParameterCollection, VectorParameters));
        Collection->PreEditChange(VectorProperty);
        for (const FName& VecName : FoliageVectorNames)
        {
            if (Collection->GetParameterId(VecName).IsValid()) continue;
            FCollectionVectorParameter VecParam;
            VecParam.ParameterName = VecName;
            VecParam.Id = FGuid::NewGuid();
            VecParam.DefaultValue = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
            Collection->VectorParameters.Add(VecParam);
        }
        FPropertyChangedEvent Event(VectorProperty, EPropertyChangeType::ValueSet);
        Collection->PostEditChangeProperty(Event);
    }

    Collection->MarkPackageDirty();

    UPackage* Package = Collection->GetOutermost();
    const FString Filename = FPackageName::LongPackageNameToFilename(
        Package->GetName(), FPackageName::GetAssetPackageExtension());
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    if (!UPackage::SavePackage(Package, Collection, *Filename, SaveArgs))
    {
        UE_LOG(LogShaderLibraryModule, Error,
            TEXT("MPC_ShaderLibrary was updated but %s could not be saved. Make the plugin asset writable and save it before cooking."),
            *Filename);
        return;
    }
    UE_LOG(LogShaderLibraryModule, Log, TEXT("Updated MPC_ShaderLibrary parameters in %s."), *Filename);
}
#endif

IMPLEMENT_MODULE(FShaderLibraryModule, ShaderLibrary)
