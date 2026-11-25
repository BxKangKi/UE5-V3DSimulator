// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "Widgets/SLeafWidget.h"
#include "Dom/JsonObject.h"
#include "System/WorldArchive.h"

/** Interactive read-only 3D wireframe + live JSON overlay preview (colliders, lights, transform, hand grips, muzzle). */
class SV3DModelPreview : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SV3DModelPreview) {} SLATE_END_ARGS()
    void Construct(const FArguments&) { SetClipping(EWidgetClipping::ClipToBounds); }
    void SetModel(const FGWorldBuildModel& Model);
    void ClearModel();
    void SetOverlaysFromJson(const TSharedPtr<FJsonObject>& Json);
    void ClearOverlays();
    void ResetCamera();

    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(320, 320); }
    virtual int32 OnPaint(const FPaintArgs&, const FGeometry&, const FSlateRect&, FSlateWindowElementList&, int32,
        const FWidgetStyle&, bool) const override;
    virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& Event) override;
    virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent& Event) override;
    virtual FReply OnMouseButtonDoubleClick(const FGeometry&, const FPointerEvent& Event) override;
    virtual FReply OnMouseMove(const FGeometry&, const FPointerEvent& Event) override;
    virtual FReply OnMouseWheel(const FGeometry&, const FPointerEvent& Event) override;

private:
    struct FOverlayLine
    {
        FVector A = FVector::ZeroVector;
        FVector B = FVector::ZeroVector;
        FLinearColor Color = FLinearColor::White;
        float Thickness = 1.5f;
    };

    struct FOverlayLabel
    {
        FVector Position = FVector::ZeroVector;
        FString Text;
        FLinearColor Color = FLinearColor::White;
    };

    void UpdateEffectiveBounds();
    void AddBoxLines(const FVector& LocalCenter, const FVector& HalfExtents, const FTransform& Xform,
        const FLinearColor& Color, float Thickness, FBox& OutBounds);
    void AddSphereLines(const FVector& LocalCenter, float SphereRadius, const FTransform& Xform,
        const FLinearColor& Color, float Thickness, FBox& OutBounds, int32 Segments = 20);
    void AddCapsuleLines(const FVector& LocalCenter, float CapsuleRadius, float HalfHeight, const FTransform& Xform,
        const FLinearColor& Color, float Thickness, FBox& OutBounds);
    void AddAxisGizmo(const FTransform& Xform, float AxisLength, const FLinearColor& HighlightColor, FBox& OutBounds);
    void AddCrossMarker(const FVector& Pos, float Size, const FLinearColor& Color, FBox& OutBounds);

    TArray<TPair<FVector, FVector>> Edges;
    TMap<FString, FTransform> NodeWorldTransforms;
    TArray<FOverlayLine> OverlayLines;
    TArray<FOverlayLabel> OverlayLabels;

    FVector ModelCenter = FVector::ZeroVector;
    float ModelRadius = 1.f;
    bool bHasModel = false;

    FVector OverlayCenter = FVector::ZeroVector;
    float OverlayRadius = 1.f;
    bool bHasOverlays = false;

    FVector Center = FVector::ZeroVector;
    FVector PanOffset = FVector::ZeroVector;
    float Radius = 100.f;
    float Yaw = 30.f;
    float Pitch = -20.f;
    float Zoom = 1.f;
    bool bOrbiting = false;
    bool bPanning = false;
};
