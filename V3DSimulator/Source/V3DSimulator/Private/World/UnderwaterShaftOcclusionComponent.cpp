#include "World/UnderwaterShaftOcclusionComponent.h"
#include "World/WaterActor.h"
#include "World/WaterQuerySubsystem.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Math/OrthoMatrix.h"

namespace
{
    // Resolutions are queried dynamically via Texture2D.GetDimensions in Underwater.ush.
    // 1024+1024 R32F uses 8 MiB VRAM (60% less than 20 MiB); mobile 512+512 uses 2 MiB (60% less than 5 MiB).
#if PLATFORM_ANDROID || PLATFORM_IOS
    constexpr int32 Resolutions[] = {512, 512};
#else
    constexpr int32 Resolutions[] = {1024, 1024};
#endif
    constexpr float Widths[] = {6400.0f, 25600.0f};
    constexpr double DepthExtent = 200000.0; // +/- 2 km along the refracted light ray.
    constexpr double WaterIOR = 1.3333;
    constexpr float IdleReleaseDelaySeconds = 5.0f;
    constexpr uint32 FarCascadeCaptureInterval = 2; // Re-capture 256m far cascade every 2nd frame unless moved/rotated.
    const FName TextureNames[] = {TEXT("V3DUWShadowNear"), TEXT("V3DUWShadowFar")};
    const FName OriginNames[] = {TEXT("V3DUWShadowNearOrigin"), TEXT("V3DUWShadowFarOrigin")};

    FLinearColor VectorParameter(const FVector& V, float W = 0.0f)
    {
        return FLinearColor(static_cast<float>(V.X), static_cast<float>(V.Y), static_cast<float>(V.Z), W);
    }
}

UUnderwaterShaftOcclusionComponent::UUnderwaterShaftOcclusionComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    // Camera and skeletal poses must be final before requesting the capture for this view.
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UUnderwaterShaftOcclusionComponent::ResetCachedParameters()
{
    bHasCachedLightBasis = false;
    bHasCachedOrigins[0] = false;
    bHasCachedOrigins[1] = false;
    bHasCapturedFarOnce = false;
    CaptureFrameCounter = 0;
}

void UUnderwaterShaftOcclusionComponent::SetReady(bool bReady)
{
    if (bReady == bLastReadyState && IsValid(TargetMaterial))
    {
        return;
    }
    bLastReadyState = bReady;
    if (IsValid(TargetMaterial) && !bReady)
    {
        const FLinearColor ZeroOrigin(0.f, 0.f, 0.f, 0.f);
        TargetMaterial->SetVectorParameterValue(OriginNames[0], ZeroOrigin);
        LastWrittenOrigins[0] = ZeroOrigin;
        bHasCachedOrigins[0] = true;
    }
}

void UUnderwaterShaftOcclusionComponent::Configure(UMaterialInstanceDynamic* Material,
    UDirectionalLightComponent* Sun, bool bOceanEnabled, float WaterHeight)
{
    check(IsInGameThread());
    if (TargetMaterial != Material)
    {
        bLastReadyState = true; // Force write of Ready=0.0 on the old material if valid.
        SetReady(false);
        bMaterialBindingsAvailable = false;
        bTexturesBound = false;
        ResetCachedParameters();
        if (IsValid(Material))
        {
            TArray<FMaterialParameterInfo> VectorInfos;
            TArray<FGuid> Ids;
            Material->GetAllVectorParameterInfo(VectorInfos, Ids);
            bMaterialBindingsAvailable = VectorInfos.ContainsByPredicate(
                [](const FMaterialParameterInfo& Info) { return Info.Name == TEXT("V3DUWShadowNearOrigin"); });
            if (!bMaterialBindingsAvailable)
            {
                UE_LOG(LogTemp, Error,
                    TEXT("Underwater shafts: %s has no light-space shadow bindings. Rebuild the Editor, open the project to upgrade MF_UnderWaterShaft, then recook ShaderLibrary."),
                    *GetNameSafe(Material));
            }
        }
    }
    TargetMaterial = Material;
    SunLight = Sun;
    WaterLevel = WaterHeight;
    const bool bEnabled = bOceanEnabled && bMaterialBindingsAvailable && IsValid(Material) && IsValid(Sun)
        && FMath::IsFinite(WaterHeight) && GetWorld() && GetWorld()->GetNetMode() != NM_DedicatedServer;
    SetComponentTickEnabled(bEnabled);
    if (!bEnabled) ReleaseResources();
}

bool UUnderwaterShaftOcclusionComponent::EnsureResources()
{
    if (Captures.Num() == 2 && DepthTargets.Num() == 2
        && IsValid(Captures[0]) && IsValid(Captures[1])
        && IsValid(DepthTargets[0]) && IsValid(DepthTargets[1]))
    {
        return true;
    }
    ReleaseResources();
    if (!GetOwner() || !GetWorld() || !GetWorld()->Scene) return false;

    Captures.Reserve(2);
    DepthTargets.Reserve(2);

    for (int32 Index = 0; Index < 2; ++Index)
    {
        auto* Target = NewObject<UTextureRenderTarget2D>(this);
        if (!IsValid(Target))
        {
            ReleaseResources();
            return false;
        }
        Target->RenderTargetFormat = RTF_R32f; // Signed linear cm, including negative orthographic depth.
        Target->bAutoGenerateMips = false;
        Target->Filter = TF_Nearest; // Compare depths BEFORE interpolation (manual bilinear PCF).
        Target->AddressX = TA_Clamp;
        Target->AddressY = TA_Clamp;
        Target->ClearColor = FLinearColor(static_cast<float>(DepthExtent), 0.f, 0.f, 0.f);
        Target->InitAutoFormat(Resolutions[Index], Resolutions[Index]);
        Target->UpdateResourceImmediate(true);
        DepthTargets.Add(Target);

        auto* Capture = NewObject<USceneCaptureComponent2D>(GetOwner());
        if (!IsValid(Capture))
        {
            ReleaseResources();
            return false;
        }
        Capture->bCaptureEveryFrame = false;
        Capture->bCaptureOnMovement = false;
        Capture->bAlwaysPersistRenderingState = false;
        Capture->ProjectionType = ECameraProjectionMode::Orthographic;
        Capture->OrthoWidth = Widths[Index];
        Capture->bAutoCalculateOrthoPlanes = false;
        Capture->bUpdateOrthoPlanes = false;
        Capture->bUseCustomProjectionMatrix = true;
        // Keep the view origin beside the player, so instance distance culling/LOD does not
        // discard nearby blockers as if the camera were kilometres above them. Orthographic
        // near depth is negative: geometry sunward of the camera is inside this frustum too.
        Capture->CustomProjectionMatrix = FReversedZOrthoMatrix(
            Widths[Index] * 0.5, Widths[Index] * 0.5, 1.0 / (DepthExtent * 2.0), DepthExtent);
        Capture->CaptureSource = SCS_SceneDepth;
        Capture->bRenderInMainRenderer = true; // Engine depth-only custom passes, no second lit scene.
        Capture->bMainViewCamera = false;
        Capture->bMainViewResolution = false;
        Capture->DetailMode = DM_Low;
        Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
        Capture->TextureTarget = Target;
        Capture->ShowFlags.SetPostProcessing(false);
        Capture->ShowFlags.SetLighting(false);
        Capture->ShowFlags.SetDynamicShadows(false);
        Capture->ShowFlags.SetFog(false);
        Capture->ShowFlags.SetAtmosphere(false);
        Capture->ShowFlags.SetVolumetricFog(false);
        Capture->ShowFlags.SetCloud(false);
        Capture->ShowFlags.SetTranslucency(false);
        Capture->ShowFlags.SetAntiAliasing(false);
        Capture->ShowFlags.SetMotionBlur(false);
        Capture->ShowFlags.SetDecals(false);
        Capture->ShowFlags.SetParticles(false);
        Capture->ShowFlags.SetBloom(false);
        Capture->ShowFlags.SetEyeAdaptation(false);
        Capture->ShowFlags.SetTemporalAA(false);
        GetOwner()->AddInstanceComponent(Capture);
        Capture->RegisterComponent();
        Captures.Add(Capture);
    }
    bTexturesBound = false;
    ResetCachedParameters();
    return true;
}

void UUnderwaterShaftOcclusionComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    const APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0);
    if (!IsValid(TargetMaterial) || !IsValid(SunLight) || !IsValid(Camera)) { SetReady(false); return; }

    const FVector CameraLocation = Camera->GetCameraLocation();
    const FVector ToSun = (-SunLight->GetForwardVector()).GetSafeNormal();
    if (CameraLocation.ContainsNaN() || ToSun.ContainsNaN() || ToSun.IsNearlyZero()
        || CameraLocation.Z >= WaterLevel - 1.f || ToSun.Z <= 0.001f)
    {
        SetReady(false);
        // Reclaim R32F capture targets promptly after staying above water or at night.
        if (Captures.Num() > 0 && FMath::IsFinite(DeltaTime) && DeltaTime > 0.0f)
        {
            InactiveElapsedSeconds += DeltaTime;
            if (InactiveElapsedSeconds >= IdleReleaseDelaySeconds)
            {
                ReleaseResources();
            }
        }
        return;
    }

    InactiveElapsedSeconds = 0.0f;
    if (!EnsureResources()) { SetReady(false); return; }

    // Exact Snell's law refraction at the horizontal air-water interface (n_air = 1.0 -> n_water = 1.3333).
    const double InvIOR = 1.0 / WaterIOR;
    const double RefractX = ToSun.X * InvIOR;
    const double RefractY = ToSun.Y * InvIOR;
    const double RefractZSq = FMath::Max(1.0 - (RefractX * RefractX + RefractY * RefractY), 1.0e-4);
    const FVector ToLight = FVector(RefractX, RefractY, FMath::Sqrt(RefractZSq)).GetSafeNormal();
    if (ToLight.ContainsNaN() || ToLight.IsNearlyZero()) { SetReady(false); return; }

    const FQuat Rotation = (-ToLight).ToOrientationQuat();
    const FVector Right = Rotation.GetRightVector();
    const FVector Up = Rotation.GetUpVector();
    const FVector Forward = Rotation.GetForwardVector();
    if (Right.ContainsNaN() || Up.ContainsNaN() || Forward.ContainsNaN()) { SetReady(false); return; }

    CachedWaterActors.Reset();
    if (UWaterQuerySubsystem* Water = UWaterQuerySubsystem::Get(this))
    {
        Water->GetRegisteredWaterActors(CachedWaterActors);
    }

    ++CaptureFrameCounter;
    for (int32 Index = 0; Index < 2; ++Index)
    {
        auto* Capture = Captures[Index].Get();
        if (!IsValid(Capture)) continue;
        const double Texel = Widths[Index] / static_cast<double>(Resolutions[Index]);
        // Snap in light space, never to screen pixels. Moving the camera by a fraction of a
        // texel cannot crawl the terrain silhouette across the shadow map.
        const double X = FVector::DotProduct(CameraLocation, Right);
        const double Y = FVector::DotProduct(CameraLocation, Up);
        const FVector Origin = CameraLocation + Right * (FMath::GridSnap(X, Texel) - X)
            + Up * (FMath::GridSnap(Y, Texel) - Y);

        if (!bTexturesBound)
        {
            TargetMaterial->SetTextureParameterValue(TextureNames[Index], DepthTargets[Index]);
        }

        // Small camera-relative offsets retain centimetre precision; NearOrigin.A packs ShadowReady=1.0.
        const FLinearColor PackedOrigin = VectorParameter(Origin - CameraLocation, Index == 0 ? 1.0f : 0.0f);
        if (!bHasCachedOrigins[Index] || !PackedOrigin.Equals(LastWrittenOrigins[Index], 1.0e-3f))
        {
            TargetMaterial->SetVectorParameterValue(OriginNames[Index], PackedOrigin);
            LastWrittenOrigins[Index] = PackedOrigin;
            bHasCachedOrigins[Index] = true;
        }

        // Capture Near every frame; throttle Far (256m) unless snapped origin or sun angle changed.
        bool bShouldCapture = (Index == 0) || !bHasCapturedFarOnce
            || ((CaptureFrameCounter % FarCascadeCaptureInterval) == 0);
        if (Index == 1 && !bShouldCapture)
        {
            const bool bMoved = !Origin.Equals(LastCapturedFarOrigin, 1.0);
            const bool bRotated = !Rotation.Equals(LastCapturedFarRotation, 1.0e-4);
            bShouldCapture = bMoved || bRotated;
        }

        if (bShouldCapture)
        {
            Capture->SetWorldLocationAndRotation(Origin, Rotation);
            Capture->HiddenActors.Reset();
            Capture->HiddenActors.Add(GetOwner()); // Authored sky dome, never terrain/characters.
            for (AWaterActor* Water : CachedWaterActors)
            {
                if (IsValid(Water)) Capture->HiddenActors.Add(Water);
            }
            // Deferred until component transforms have been sent; renders before the main view.
            Capture->CaptureSceneDeferred();
            if (Index == 1)
            {
                LastCapturedFarOrigin = Origin;
                LastCapturedFarRotation = Rotation;
                bHasCapturedFarOnce = true;
            }
        }
    }
    bTexturesBound = true;

    // Only write light basis vectors when sun direction actually changed.
    // Note: V3DUWShadowUp is derived in Underwater.ush via cross(Forward, Right) to minimize parameters.
    if (!bHasCachedLightBasis
        || !Right.Equals(LastWrittenRight, 1.0e-4)
        || !Forward.Equals(LastWrittenForward, 1.0e-4))
    {
        TargetMaterial->SetVectorParameterValue(TEXT("V3DUWShadowRight"), VectorParameter(Right));
        TargetMaterial->SetVectorParameterValue(TEXT("V3DUWShadowForward"), VectorParameter(Forward));
        LastWrittenRight = Right;
        LastWrittenForward = Forward;
        bHasCachedLightBasis = true;
    }
    SetReady(true);
}

void UUnderwaterShaftOcclusionComponent::ReleaseResources()
{
    SetReady(false);
    bTexturesBound = false;
    InactiveElapsedSeconds = 0.0f;
    ResetCachedParameters();
    for (const auto& CapturePtr : Captures)
    {
        auto* Capture = CapturePtr.Get();
        if (!IsValid(Capture)) continue;
        Capture->TextureTarget = nullptr;
        Capture->HiddenActors.Reset();
        if (GetOwner()) GetOwner()->RemoveInstanceComponent(Capture);
        Capture->DestroyComponent();
    }
    Captures.Empty();
    if (IsValid(TargetMaterial))
    {
        for (const FName Name : TextureNames) TargetMaterial->SetTextureParameterValue(Name, nullptr);
    }
    for (const auto& TargetPtr : DepthTargets)
    {
        if (UTextureRenderTarget2D* Target = TargetPtr.Get(); IsValid(Target))
        {
            Target->ReleaseResource();
            Target->MarkAsGarbage();
        }
    }
    DepthTargets.Empty();
    CachedWaterActors.Empty();
}

void UUnderwaterShaftOcclusionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ReleaseResources();
    TargetMaterial = nullptr;
    SunLight = nullptr;
    Super::EndPlay(EndPlayReason);
}

void UUnderwaterShaftOcclusionComponent::OnUnregister()
{
    ReleaseResources();
    Super::OnUnregister();
}
