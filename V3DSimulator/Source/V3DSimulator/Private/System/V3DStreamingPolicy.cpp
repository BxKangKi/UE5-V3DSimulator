// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/V3DStreamingPolicy.h"
#include "Setting/GameSettings.h"
#include "System/GameManagerSubSystem.h"

#include "Camera/PlayerCameraManager.h"
#include "CoreGlobals.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "SceneView.h"

double V3DStreamingPolicy::GetMaxRenderDistanceCm(const UObject* WorldContext)
{
    check(IsInGameThread());
    const UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(WorldContext);
    const UGameSettings* Settings = Manager ? Manager->GetGameSettings() : nullptr;
    return Settings ? Settings->GetMaxRenderDistanceCentimeters() : DefaultMaxRenderDistanceCm;
}

float V3DStreamingPolicy::GetScreenSizeDistance(const UObject* WorldContext)
{
    check(IsInGameThread());
    UWorld* World = IsValid(WorldContext) ? WorldContext->GetWorld() : nullptr;
    // One projection query per world/frame, even with hundreds of streaming groups.
    static TWeakObjectPtr<UWorld> CachedWorld;
    static uint64 CachedFrame = MAX_uint64;
    static float CachedDistance = float(ScreenSizeDistance(16.0 / 9.0));
    if (World && CachedWorld.Get() == World && CachedFrame == GFrameCounter)
        return CachedDistance;

    double ProjectionScale = 16.0 / 9.0; // 90-degree horizontal FOV, before the camera exists.
    if (APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr)
    {
        if (IsValid(PC->PlayerCameraManager))
        {
            const FMinimalViewInfo& View = PC->PlayerCameraManager->GetCameraCacheView();
            const double FOV = FMath::Clamp(double(View.FOV), 5.0, 170.0);
            ProjectionScale = FMath::Max(1.0, double(View.AspectRatio))
                / FMath::Tan(FMath::DegreesToRadians(FOV * 0.5));
        }
        if (ULocalPlayer* Player = PC->GetLocalPlayer())
        {
            FSceneViewProjectionData Projection;
            if (Player->ViewportClient && Player->ViewportClient->Viewport
                && Player->GetProjectionData(Player->ViewportClient->Viewport, Projection, INDEX_NONE)
                && Projection.ProjectionMatrix.M[3][3] == 0.0)
            {
                ProjectionScale = FMath::Max(FMath::Abs(double(Projection.ProjectionMatrix.M[0][0])),
                    FMath::Abs(double(Projection.ProjectionMatrix.M[1][1])));
            }
        }
    }
    CachedWorld = World;
    CachedFrame = GFrameCounter;
    CachedDistance = float(ScreenSizeDistance(FMath::IsFinite(ProjectionScale) && ProjectionScale > 0.0
        ? ProjectionScale : 16.0 / 9.0));
    return CachedDistance;
}
