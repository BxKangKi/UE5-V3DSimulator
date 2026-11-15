// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "SV3DModelPreview.h"
#include "Rendering/DrawElements.h"
#include "InputCoreTypes.h"

void SV3DModelPreview::SetModel(const FGWorldBuildModel& Model)
{
    Edges.Empty(); FBox Bounds(ForceInit);
    TMap<int32, const FGWorldNodeTransform*> Nodes;
    for (const auto& Node : Model.Metadata.NodeTransforms) Nodes.Add(Node.NodeIndex, &Node);
    for (const auto& Node : Model.Metadata.NodeTransforms)
    {
        if (Node.MeshIndex < 0) continue;
        FTransform Transform = Node.LocalTransform;
        int32 Parent = Node.ParentIndex, Depth = 0;
        while (const auto* P = Nodes.Find(Parent))
        {
            if (++Depth > 256) break;
            Transform = Transform * (*P)->LocalTransform; Parent = (*P)->ParentIndex;
        }
        const auto* Mesh = Model.BakedData.Meshes.FindByPredicate([&Node](const auto& M) { return M.MeshIndex == Node.MeshIndex; });
        if (!Mesh) continue;
        for (const auto& Primitive : Mesh->Primitives)
        {
            for (const auto& Point : Primitive.Positions) Bounds += Transform.TransformPosition(FVector(Point));
            const int32 Count = Primitive.Indices.Num();
            const int32 Stride = FMath::Max(1, Count / 3000) * 3;
            for (int32 I = 0; I + 2 < Count && Edges.Num() < 3000; I += Stride)
            {
                const int32 A = Primitive.Indices[I], B = Primitive.Indices[I+1], C = Primitive.Indices[I+2];
                if (!Primitive.Positions.IsValidIndex(A) || !Primitive.Positions.IsValidIndex(B) || !Primitive.Positions.IsValidIndex(C)) continue;
                const FVector P[] = {Transform.TransformPosition(FVector(Primitive.Positions[A])),
                    Transform.TransformPosition(FVector(Primitive.Positions[B])),Transform.TransformPosition(FVector(Primitive.Positions[C]))};
                Edges.Emplace(P[0], P[1]); Edges.Emplace(P[1], P[2]); Edges.Emplace(P[2], P[0]);
            }
        }
    }
    Center = Bounds.IsValid ? Bounds.GetCenter() : FVector::ZeroVector;
    Radius = Bounds.IsValid ? FMath::Max(1.f, float(Bounds.GetExtent().Size())) : 1.f;
    Zoom = 1; Invalidate(EInvalidateWidgetReason::Paint);
}
int32 SV3DModelPreview::OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& Clip,
    FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle& Style, bool bEnabled) const
{
    const FVector2D Size = Geometry.GetLocalSize();
    const FQuat Rotation = FRotator(Pitch, Yaw, 0).Quaternion();
    const float Scale = FMath::Min(Size.X, Size.Y) * .43f * Zoom / Radius;
    auto Project = [&](const FVector& Point)
    {
        const FVector P = Rotation.UnrotateVector(Point - Center);
        return FVector2D(Size.X * .5f + P.Y * Scale, Size.Y * .5f - P.Z * Scale);
    };
    for (const auto& Edge : Edges)
    {
        TArray<FVector2f> Points = {FVector2f(Project(Edge.Key)), FVector2f(Project(Edge.Value))};
        FSlateDrawElement::MakeLines(Out, Layer, Geometry.ToPaintGeometry(), MoveTemp(Points),
            ESlateDrawEffect::None, FLinearColor(.25f,.75f,1.f,.7f), true, 1.f);
    }
    return Layer;
}
FReply SV3DModelPreview::OnMouseButtonDown(const FGeometry&, const FPointerEvent& E)
{ return E.GetEffectingButton() == EKeys::LeftMouseButton ? FReply::Handled().CaptureMouse(SharedThis(this)) : FReply::Unhandled(); }
FReply SV3DModelPreview::OnMouseButtonUp(const FGeometry&, const FPointerEvent&)
{ return FReply::Handled().ReleaseMouseCapture(); }
FReply SV3DModelPreview::OnMouseMove(const FGeometry&, const FPointerEvent& E)
{
    if (!HasMouseCapture()) return FReply::Unhandled();
    Yaw += E.GetCursorDelta().X * .4f; Pitch = FMath::Clamp(Pitch + float(E.GetCursorDelta().Y) * .4f, -89.f, 89.f);
    Invalidate(EInvalidateWidgetReason::Paint); return FReply::Handled();
}
FReply SV3DModelPreview::OnMouseWheel(const FGeometry&, const FPointerEvent& E)
{ Zoom = FMath::Clamp(Zoom * FMath::Pow(1.15f, E.GetWheelDelta()), .2f, 5.f); Invalidate(EInvalidateWidgetReason::Paint); return FReply::Handled(); }
