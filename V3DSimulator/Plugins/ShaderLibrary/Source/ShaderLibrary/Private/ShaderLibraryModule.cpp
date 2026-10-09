#include "ShaderLibraryModule.h"

#include "Interfaces/IPluginManager.h"
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

    AddShaderSourceDirectoryMapping(TEXT("/Plugin/ShaderLibrary"), ShaderDirectory);
#if WITH_EDITOR
    // Content packages cannot be loaded during PostConfigInit. Upgrade the existing asset only
    // after the engine is initialized; packaged games use the resulting cooked collection.
    if (GEngine) EnsureShaderLibraryParameters();
    else PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(
        this, &FShaderLibraryModule::EnsureShaderLibraryParameters);
#endif
    UE_LOG(LogShaderLibraryModule, Log, TEXT("Registered shader source mapping /Plugin/ShaderLibrary -> %s"), *ShaderDirectory);
}

void FShaderLibraryModule::ShutdownModule()
{
#if WITH_EDITOR
    FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
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
    if (Collection->GetParameterId(ParameterName).IsValid()) return;

    FProperty* Property = FindFProperty<FProperty>(UMaterialParameterCollection::StaticClass(),
        GET_MEMBER_NAME_CHECKED(UMaterialParameterCollection, ScalarParameters));
    Collection->Modify();
    Collection->PreEditChange(Property);
    FCollectionScalarParameter Parameter;
    Parameter.ParameterName = ParameterName;
    Parameter.Id = FGuid::NewGuid();
    Parameter.DefaultValue = 819200.0f; // Unreal centimetres; runtime settings replace this value.
    Collection->ScalarParameters.Add(Parameter);
    FPropertyChangedEvent Event(Property, EPropertyChangeType::ArrayAdd);
    Collection->PostEditChangeProperty(Event);
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
            TEXT("MaxRenderDistance was added but %s could not be saved. Make the plugin asset writable and save it before cooking."),
            *Filename);
        return;
    }
    UE_LOG(LogShaderLibraryModule, Log, TEXT("Added MaxRenderDistance to %s."), *Filename);
}
#endif

IMPLEMENT_MODULE(FShaderLibraryModule, ShaderLibrary)
