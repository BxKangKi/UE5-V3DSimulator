// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "V3DSimulatorEditor.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Editor/TransBuffer.h"
#include "Features/IModularFeatures.h"
#include "PhysicsEngine/PhysicsAsset.h"

FV3DSimulatorEditorModule& FV3DSimulatorEditorModule::Get()
{
    return FModuleManager::LoadModuleChecked<FV3DSimulatorEditorModule>(TEXT("V3DSimulatorEditor"));
}

bool FV3DSimulatorEditorModule::IsAvailable()
{
    return FModuleManager::Get().IsModuleLoaded(TEXT("V3DSimulatorEditor"));
}

void FV3DSimulatorEditorModule::StartupModule()
{
    IModularFeatures::Get().RegisterModularFeature(
        IV3DSimulatorEditorServices::GetModularFeatureName(),
        this);
    bEditorServicesRegistered = true;

    // Keep PIE/SIE transaction cleanup out of gameplay classes. The callback is editor-only and is
    // active before OpenLevel tears down the current play world, which is exactly when stale REINST
    // widget references must be released from the editor undo buffer.
    PreLoadMapHandle = FCoreUObjectDelegates::PreLoadMapWithContext.AddRaw(
        this,
        &FV3DSimulatorEditorModule::HandlePreLoadMap);


}

void FV3DSimulatorEditorModule::ShutdownModule()
{
    if (PreLoadMapHandle.IsValid())
    {
        FCoreUObjectDelegates::PreLoadMapWithContext.Remove(PreLoadMapHandle);
        PreLoadMapHandle.Reset();
    }

    if (bEditorServicesRegistered)
    {
        IModularFeatures::Get().UnregisterModularFeature(
            IV3DSimulatorEditorServices::GetModularFeatureName(),
            this);
        bEditorServicesRegistered = false;
    }
}

void FV3DSimulatorEditorModule::HandlePreLoadMap(
    const FWorldContext& WorldContext,
    const FString& MapName)
{
    // PreLoadMapWithContext lets the editor module distinguish PIE travel from a normal level open,
    // so a designer's ordinary editor undo history is never cleared by this gameplay workaround.
    if (WorldContext.WorldType != EWorldType::PIE || !GEditor || !GEditor->Trans)
    {
        return;
    }

    const FString Reason = MapName.IsEmpty()
        ? FString(TEXT("V3DSimulator PIE world travel"))
        : FString::Printf(TEXT("V3DSimulator PIE world travel: %s"), *MapName);

    GEditor->Trans->Reset(FText::FromString(Reason));
    UE_LOG(LogTemp, Display,
        TEXT("[V3DSimulatorEditor] Editor transaction buffer reset before PIE world travel: %s"),
        *MapName);
}

void FV3DSimulatorEditorModule::RefreshPhysicsAsset(UPhysicsAsset* PhysicsAsset)
{
    if (!IsValid(PhysicsAsset))
    {
        return;
    }

    PhysicsAsset->InvalidateAllPhysicsMeshes();
    PhysicsAsset->RefreshPhysicsAssetChange();
}

IMPLEMENT_MODULE(FV3DSimulatorEditorModule, V3DSimulatorEditor);
