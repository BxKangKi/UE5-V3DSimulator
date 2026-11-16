// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "Widgets/SLeafWidget.h"
#include "System/WorldArchive.h"

/** Read-only, bounded wireframe. Drag or scroll changes the preview camera only. */
class SV3DModelPreview : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SV3DModelPreview) {} SLATE_END_ARGS()
    void Construct(const FArguments&) { SetClipping(EWidgetClipping::ClipToBounds); }
    void SetModel(const FGWorldBuildModel& Model);
    void ClearModel();
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(320,240); }
    virtual int32 OnPaint(const FPaintArgs&, const FGeometry&, const FSlateRect&, FSlateWindowElementList&, int32,
        const FWidgetStyle&, bool) const override;
    virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& Event) override;
    virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent& Event) override;
    virtual FReply OnMouseMove(const FGeometry&, const FPointerEvent& Event) override;
    virtual FReply OnMouseWheel(const FGeometry&, const FPointerEvent& Event) override;
private:
    TArray<TPair<FVector, FVector>> Edges;
    FVector Center = FVector::ZeroVector;
    float Radius = 1, Yaw = 30, Pitch = -20, Zoom = 1;
};
