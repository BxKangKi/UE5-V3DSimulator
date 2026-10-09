#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FShaderLibraryModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

#if WITH_EDITOR
private:
    void EnsureShaderLibraryParameters();
    FDelegateHandle PostEngineInitHandle;
#endif
};
