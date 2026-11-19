#include "V3DRuntimeServices.h"
#include "System/SafeFileIO.h"
#include "System/V3DRuntimeSafety.h"
#include "System/V3DStreamingBudget.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
TFunction<int32(const UObject*)> FV3DRuntimeServices::TextureLimit;
TFunction<FString(const UObject*)> FV3DRuntimeServices::TerrainPath;
class FV3DRuntimeModule final : public IModuleInterface
{
    void ShutdownModule() override
    {
        FV3DStreamingBudget::Shutdown();
        FV3DRuntimeSafety::BeginShutdown();
        FSafeFileIO::BeginShutdown();
        FV3DRuntimeSafety::FlushPendingOperations(-1.0);
        FSafeFileIO::FlushPendingOperations(-1.0);
    }
    void StartupModule() override
    {
        const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("V3DRuntime"));
        if (!Plugin) return;
        const FString Directory = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
        AddShaderSourceDirectoryMapping(TEXT("/V3D"), Directory);
    }
};
IMPLEMENT_MODULE(FV3DRuntimeModule, V3DRuntime)
