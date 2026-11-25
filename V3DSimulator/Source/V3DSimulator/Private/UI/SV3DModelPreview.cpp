// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "SV3DModelPreview.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "InputCoreTypes.h"
#include "Simulator/NodeTokenLibrary.h"

namespace
{
    double ReadJsonNum(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double DefaultValue = 0.0)
    {
        if (!Obj.IsValid()) return DefaultValue;
        double Out = DefaultValue;
        return Obj->TryGetNumberField(Key, Out) && FMath::IsFinite(Out) ? Out : DefaultValue;
    }

    FVector ReadJsonVec(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Prefix = TEXT(""), double DefaultVal = 0.0)
    {
        const FString P(Prefix);
        return FVector(
            ReadJsonNum(Obj, *(P + TEXT("X")), DefaultVal),
            ReadJsonNum(Obj, *(P + TEXT("Y")), DefaultVal),
            ReadJsonNum(Obj, *(P + TEXT("Z")), DefaultVal));
    }

    FTransform ReadJsonTransform(const TSharedPtr<FJsonObject>& Obj)
    {
        if (!Obj.IsValid()) return FTransform::Identity;
        const FVector Loc = ReadJsonVec(Obj, TEXT(""), 0.0);
        const FRotator Rot(
            ReadJsonNum(Obj, TEXT("Pitch"), 0.0),
            ReadJsonNum(Obj, TEXT("Yaw"), 0.0),
            ReadJsonNum(Obj, TEXT("Roll"), 0.0));
        const FVector Scale(
            FMath::Clamp(ReadJsonNum(Obj, TEXT("ScaleX"), 1.0), 0.01, 1000.0),
            FMath::Clamp(ReadJsonNum(Obj, TEXT("ScaleY"), 1.0), 0.01, 1000.0),
            FMath::Clamp(ReadJsonNum(Obj, TEXT("ScaleZ"), 1.0), 0.01, 1000.0));
        return FTransform(Rot, Loc, Scale);
    }
}

void SV3DModelPreview::ResetCamera()
{
    Yaw = 30.f;
    Pitch = -20.f;
    Zoom = 1.f;
    PanOffset = FVector::ZeroVector;
    Invalidate(EInvalidateWidgetReason::Paint);
}

void SV3DModelPreview::ClearModel()
{
    Edges.Reset();
    NodeWorldTransforms.Reset();
    ModelCenter = FVector::ZeroVector;
    ModelRadius = 1.f;
    bHasModel = false;
    UpdateEffectiveBounds();
    Invalidate(EInvalidateWidgetReason::Paint);
}

void SV3DModelPreview::ClearOverlays()
{
    OverlayLines.Reset();
    OverlayLabels.Reset();
    OverlayCenter = FVector::ZeroVector;
    OverlayRadius = 1.f;
    bHasOverlays = false;
    UpdateEffectiveBounds();
    Invalidate(EInvalidateWidgetReason::Paint);
}

void SV3DModelPreview::UpdateEffectiveBounds()
{
    if (bHasModel)
    {
        Center = ModelCenter;
        Radius = FMath::Max(1.f, ModelRadius);
    }
    else if (bHasOverlays)
    {
        Center = OverlayCenter;
        Radius = FMath::Max(10.f, OverlayRadius);
    }
    else
    {
        Center = FVector::ZeroVector;
        Radius = 100.f;
    }
}

void SV3DModelPreview::SetModel(const FGWorldBuildModel& Model)
{
    Edges.Reset(3000);
    NodeWorldTransforms.Reset();
    FBox Bounds(ForceInit);
    TMap<int32, const FGWorldNodeTransform*> Nodes;
    Nodes.Reserve(Model.Metadata.NodeTransforms.Num());
    for (const auto& Node : Model.Metadata.NodeTransforms) Nodes.Add(Node.NodeIndex, &Node);
    for (const auto& Node : Model.Metadata.NodeTransforms)
    {
        if (Node.MeshIndex < 0) continue;
        FTransform Transform = Node.LocalTransform;
        int32 Parent = Node.ParentIndex, Depth = 0;
        while (const auto* P = Nodes.Find(Parent))
        {
            if (++Depth > 256) break;
            Transform = Transform * (*P)->LocalTransform;
            Parent = (*P)->ParentIndex;
        }
        const FSimulatorParsedNodeName Parsed = USimulatorNodeTokenLibrary::ParseNodeName(Node.Name);
        if (!Parsed.HasEffectiveToken(FName(TEXT("INST"))) && !Parsed.BaseName.IsEmpty())
        {
            NodeWorldTransforms.FindOrAdd(Parsed.BaseName) = Transform;
        }
        const auto* Mesh = Model.BakedData.Meshes.FindByPredicate([&Node](const auto& M) { return M.MeshIndex == Node.MeshIndex; });
        if (!Mesh) continue;
        for (const auto& Primitive : Mesh->Primitives)
        {
            for (const auto& Point : Primitive.Positions)
            {
                const FVector WorldPoint = Transform.TransformPosition(FVector(Point));
                if (!WorldPoint.ContainsNaN()) Bounds += WorldPoint;
            }
            const int32 Count = Primitive.Indices.Num();
            const int32 Stride = FMath::Max(1, Count / 3000) * 3;
            for (int32 I = 0; I + 2 < Count && Edges.Num() <= 2997; I += Stride)
            {
                const int32 A = Primitive.Indices[I], B = Primitive.Indices[I + 1], C = Primitive.Indices[I + 2];
                if (!Primitive.Positions.IsValidIndex(A) || !Primitive.Positions.IsValidIndex(B) || !Primitive.Positions.IsValidIndex(C)) continue;
                const FVector P[] = {
                    Transform.TransformPosition(FVector(Primitive.Positions[A])),
                    Transform.TransformPosition(FVector(Primitive.Positions[B])),
                    Transform.TransformPosition(FVector(Primitive.Positions[C]))
                };
                if (P[0].ContainsNaN() || P[1].ContainsNaN() || P[2].ContainsNaN()) continue;
                Edges.Emplace(P[0], P[1]);
                Edges.Emplace(P[1], P[2]);
                Edges.Emplace(P[2], P[0]);
            }
        }
    }
    bHasModel = Bounds.IsValid && Edges.Num() >= 3;
    ModelCenter = Bounds.IsValid ? Bounds.GetCenter() : FVector::ZeroVector;
    ModelRadius = Bounds.IsValid ? FMath::Max(1.f, static_cast<float>(Bounds.GetExtent().Size())) : 1.f;
    Zoom = 1.f;
    PanOffset = FVector::ZeroVector;
    UpdateEffectiveBounds();
    Invalidate(EInvalidateWidgetReason::Paint);
}

void SV3DModelPreview::AddBoxLines(const FVector& LocalCenter, const FVector& HalfExtents, const FTransform& Xform,
    const FLinearColor& Color, float Thickness, FBox& OutBounds)
{
    const FVector E(FMath::Abs(HalfExtents.X), FMath::Abs(HalfExtents.Y), FMath::Abs(HalfExtents.Z));
    const FVector Corners[8] = {
        Xform.TransformPosition(LocalCenter + FVector(-E.X, -E.Y, -E.Z)),
        Xform.TransformPosition(LocalCenter + FVector( E.X, -E.Y, -E.Z)),
        Xform.TransformPosition(LocalCenter + FVector( E.X,  E.Y, -E.Z)),
        Xform.TransformPosition(LocalCenter + FVector(-E.X,  E.Y, -E.Z)),
        Xform.TransformPosition(LocalCenter + FVector(-E.X, -E.Y,  E.Z)),
        Xform.TransformPosition(LocalCenter + FVector( E.X, -E.Y,  E.Z)),
        Xform.TransformPosition(LocalCenter + FVector( E.X,  E.Y,  E.Z)),
        Xform.TransformPosition(LocalCenter + FVector(-E.X,  E.Y,  E.Z))
    };
    for (int32 I = 0; I < 8; ++I) if (!Corners[I].ContainsNaN()) OutBounds += Corners[I];
    static constexpr int32 EdgeIdx[12][2] = {
        {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7}
    };
    for (int32 I = 0; I < 12; ++I)
    {
        OverlayLines.Add({Corners[EdgeIdx[I][0]], Corners[EdgeIdx[I][1]], Color, Thickness});
    }
}

void SV3DModelPreview::AddSphereLines(const FVector& LocalCenter, float SphereRadius, const FTransform& Xform,
    const FLinearColor& Color, float Thickness, FBox& OutBounds, int32 Segments)
{
    const float R = FMath::Max(0.5f, SphereRadius);
    const int32 Segs = FMath::Clamp(Segments, 8, 32);
    for (int32 Plane = 0; Plane < 3; ++Plane)
    {
        for (int32 I = 0; I < Segs; ++I)
        {
            const float A0 = (2.f * PI * I) / Segs;
            const float A1 = (2.f * PI * (I + 1)) / Segs;
            FVector L0, L1;
            if (Plane == 0) { L0 = FVector(FMath::Cos(A0) * R, FMath::Sin(A0) * R, 0.f); L1 = FVector(FMath::Cos(A1) * R, FMath::Sin(A1) * R, 0.f); }
            else if (Plane == 1) { L0 = FVector(FMath::Cos(A0) * R, 0.f, FMath::Sin(A0) * R); L1 = FVector(FMath::Cos(A1) * R, 0.f, FMath::Sin(A1) * R); }
            else { L0 = FVector(0.f, FMath::Cos(A0) * R, FMath::Sin(A0) * R); L1 = FVector(0.f, FMath::Cos(A1) * R, FMath::Sin(A1) * R); }
            const FVector W0 = Xform.TransformPosition(LocalCenter + L0);
            const FVector W1 = Xform.TransformPosition(LocalCenter + L1);
            if (!W0.ContainsNaN() && !W1.ContainsNaN())
            {
                OutBounds += W0; OutBounds += W1;
                OverlayLines.Add({W0, W1, Color, Thickness});
            }
        }
    }
}

void SV3DModelPreview::AddCapsuleLines(const FVector& LocalCenter, float CapsuleRadius, float HalfHeight, const FTransform& Xform,
    const FLinearColor& Color, float Thickness, FBox& OutBounds)
{
    const float R = FMath::Max(0.5f, CapsuleRadius);
    const float CylinderHalfH = FMath::Max(0.f, HalfHeight - R);
    constexpr int32 Segs = 16;
    for (int32 EndSign : {-1, 1})
    {
        const float Z = EndSign * CylinderHalfH;
        for (int32 I = 0; I < Segs; ++I)
        {
            const float A0 = (2.f * PI * I) / Segs;
            const float A1 = (2.f * PI * (I + 1)) / Segs;
            const FVector W0 = Xform.TransformPosition(LocalCenter + FVector(FMath::Cos(A0) * R, FMath::Sin(A0) * R, Z));
            const FVector W1 = Xform.TransformPosition(LocalCenter + FVector(FMath::Cos(A1) * R, FMath::Sin(A1) * R, Z));
            OutBounds += W0; OutBounds += W1;
            OverlayLines.Add({W0, W1, Color, Thickness});
        }
        // Top / bottom hemisphere arcs in XZ and YZ
        for (int32 I = 0; I < 8; ++I)
        {
            const float T0 = (PI * I) / 8.f;
            const float T1 = (PI * (I + 1)) / 8.f;
            const float Sign = static_cast<float>(EndSign);
            const FVector X0 = Xform.TransformPosition(LocalCenter + FVector(FMath::Cos(T0) * R, 0.f, Z + Sign * FMath::Sin(T0) * R));
            const FVector X1 = Xform.TransformPosition(LocalCenter + FVector(FMath::Cos(T1) * R, 0.f, Z + Sign * FMath::Sin(T1) * R));
            const FVector Y0 = Xform.TransformPosition(LocalCenter + FVector(0.f, FMath::Cos(T0) * R, Z + Sign * FMath::Sin(T0) * R));
            const FVector Y1 = Xform.TransformPosition(LocalCenter + FVector(0.f, FMath::Cos(T1) * R, Z + Sign * FMath::Sin(T1) * R));
            OutBounds += X0; OutBounds += X1; OutBounds += Y0; OutBounds += Y1;
            OverlayLines.Add({X0, X1, Color, Thickness});
            OverlayLines.Add({Y0, Y1, Color, Thickness});
        }
    }
    const FVector Ribs[4] = { FVector(R, 0, 0), FVector(-R, 0, 0), FVector(0, R, 0), FVector(0, -R, 0) };
    for (int32 I = 0; I < 4; ++I)
    {
        const FVector Top = Xform.TransformPosition(LocalCenter + Ribs[I] + FVector(0, 0, CylinderHalfH));
        const FVector Bot = Xform.TransformPosition(LocalCenter + Ribs[I] - FVector(0, 0, CylinderHalfH));
        OverlayLines.Add({Bot, Top, Color, Thickness});
    }
}

void SV3DModelPreview::AddAxisGizmo(const FTransform& Xform, float AxisLength, const FLinearColor& HighlightColor, FBox& OutBounds)
{
    const FVector Origin = Xform.GetLocation();
    const FVector XEnd = Origin + Xform.GetUnitAxis(EAxis::X) * AxisLength;
    const FVector YEnd = Origin + Xform.GetUnitAxis(EAxis::Y) * AxisLength;
    const FVector ZEnd = Origin + Xform.GetUnitAxis(EAxis::Z) * AxisLength;
    OutBounds += Origin; OutBounds += XEnd; OutBounds += YEnd; OutBounds += ZEnd;
    OverlayLines.Add({Origin, XEnd, FLinearColor(1.f, 0.3f, 0.3f, 1.f), 2.f});
    OverlayLines.Add({Origin, YEnd, FLinearColor(0.3f, 0.95f, 0.4f, 1.f), 2.f});
    OverlayLines.Add({Origin, ZEnd, FLinearColor(0.35f, 0.65f, 1.f, 1.f), 2.f});
    AddCrossMarker(Origin, AxisLength * 0.25f, HighlightColor, OutBounds);
}

void SV3DModelPreview::AddCrossMarker(const FVector& Pos, float Size, const FLinearColor& Color, FBox& OutBounds)
{
    const float S = FMath::Max(1.f, Size);
    OutBounds += Pos + FVector(S); OutBounds += Pos - FVector(S);
    OverlayLines.Add({Pos - FVector(S, 0, 0), Pos + FVector(S, 0, 0), Color, 2.f});
    OverlayLines.Add({Pos - FVector(0, S, 0), Pos + FVector(0, S, 0), Color, 2.f});
    OverlayLines.Add({Pos - FVector(0, 0, S), Pos + FVector(0, 0, S), Color, 2.f});
}

void SV3DModelPreview::SetOverlaysFromJson(const TSharedPtr<FJsonObject>& Json)
{
    OverlayLines.Reset();
    OverlayLabels.Reset();
    FBox Bounds(ForceInit);

    if (!Json.IsValid())
    {
        ClearOverlays();
        return;
    }

    const float GizmoScale = bHasModel ? FMath::Clamp(ModelRadius * 0.16f, 8.f, 80.f) : 20.f;

    // 1. Dynamic Transform (Position / Rotation / Scale)
    const TSharedPtr<FJsonObject>* TransformObj = nullptr;
    if (Json->TryGetObjectField(TEXT("Transform"), TransformObj) && TransformObj && TransformObj->IsValid())
    {
        const FTransform DynXform = ReadJsonTransform(*TransformObj);
        const FLinearColor TransformColor(0.35f, 0.80f, 1.0f, 0.9f);
        AddAxisGizmo(DynXform, GizmoScale * 1.25f, TransformColor, Bounds);
        const FVector ScaleVec = DynXform.GetScale3D();
        if (!ScaleVec.Equals(FVector::OneVector, 0.01) || !DynXform.GetLocation().IsNearlyZero(0.1))
        {
            const FVector PreviewHalf = FVector(50.f) * ScaleVec;
            AddBoxLines(FVector::ZeroVector, PreviewHalf, FTransform(DynXform.GetRotation(), DynXform.GetLocation(), FVector::OneVector),
                FLinearColor(0.35f, 0.80f, 1.0f, 0.45f), 1.f, Bounds);
        }
        OverlayLabels.Add({DynXform.GetLocation(), FString::Printf(TEXT("Transform (%.0f,%.0f,%.0f) S:%.2f"),
            DynXform.GetLocation().X, DynXform.GetLocation().Y, DynXform.GetLocation().Z, ScaleVec.X), TransformColor});
    }

    // 2. MeshData -> Per-Node Colliders & Lights
    const TSharedPtr<FJsonObject>* MeshDataObj = nullptr;
    if (Json->TryGetObjectField(TEXT("MeshData"), MeshDataObj) && MeshDataObj && MeshDataObj->IsValid())
    {
        const FLinearColor ColliderColor(0.20f, 0.95f, 0.55f, 0.95f);
        const FLinearColor LightCoreColor(1.0f, 0.90f, 0.25f, 1.0f);
        const FLinearColor LightAttenColor(1.0f, 0.72f, 0.15f, 0.40f);

        for (const auto& Pair : (*MeshDataObj)->Values)
        {
            if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object) continue;
            const TSharedPtr<FJsonObject> NodeObj = Pair.Value->AsObject();
            if (!NodeObj.IsValid()) continue;

            const FString NodeKey(Pair.Key);
            const FTransform* FoundNodeXform = NodeWorldTransforms.Find(NodeKey);
            const FTransform NodeXform = FoundNodeXform ? *FoundNodeXform : FTransform::Identity;

            // Colliders
            const TArray<TSharedPtr<FJsonValue>>* CollidersArr = nullptr;
            if (NodeObj->TryGetArrayField(TEXT("Colliders"), CollidersArr) && CollidersArr)
            {
                for (int32 Idx = 0; Idx < CollidersArr->Num(); ++Idx)
                {
                    const auto& Val = (*CollidersArr)[Idx];
                    if (!Val.IsValid() || Val->Type != EJson::Object) continue;
                    const TSharedPtr<FJsonObject> ColObj = Val->AsObject();
                    FString ColType = TEXT("Box");
                    ColObj->TryGetStringField(TEXT("Type"), ColType);
                    if (ColType.Equals(TEXT("None"), ESearchCase::IgnoreCase)) continue;

                    const FVector ColCenter = ReadJsonVec(ColObj, TEXT(""), 0.0);
                    const FVector ColSize = ReadJsonVec(ColObj, TEXT("D"), 50.0);
                    if (ColType.Equals(TEXT("Sphere"), ESearchCase::IgnoreCase))
                    {
                        const float R = static_cast<float>(FMath::Max3(FMath::Abs(ColSize.X), FMath::Abs(ColSize.Y), FMath::Abs(ColSize.Z)));
                        AddSphereLines(ColCenter, R > 0.f ? R : 50.f, NodeXform, ColliderColor, 1.8f, Bounds);
                    }
                    else if (ColType.Equals(TEXT("Capsule"), ESearchCase::IgnoreCase))
                    {
                        const float R = static_cast<float>(FMath::Max(1.0, FMath::Abs(ColSize.X)));
                        const float HalfH = static_cast<float>(FMath::Max(static_cast<double>(R), FMath::Abs(ColSize.Z)));
                        AddCapsuleLines(ColCenter, R, HalfH, NodeXform, ColliderColor, 1.8f, Bounds);
                    }
                    else
                    {
                        const FVector HalfExtents(
                            FMath::Max(1.0, FMath::Abs(ColSize.X) * 0.5),
                            FMath::Max(1.0, FMath::Abs(ColSize.Y) * 0.5),
                            FMath::Max(1.0, FMath::Abs(ColSize.Z) * 0.5));
                        AddBoxLines(ColCenter, HalfExtents, NodeXform, ColliderColor, 1.8f, Bounds);
                    }
                    const FVector WorldLabelPos = NodeXform.TransformPosition(ColCenter);
                    OverlayLabels.Add({WorldLabelPos, FString::Printf(TEXT("Col[%d] %s (%s)"), Idx, *ColType, *NodeKey), ColliderColor});
                }
            }

            // Lights
            const TArray<TSharedPtr<FJsonValue>>* LightsArr = nullptr;
            if (NodeObj->TryGetArrayField(TEXT("Lights"), LightsArr) && LightsArr)
            {
                for (int32 Idx = 0; Idx < LightsArr->Num(); ++Idx)
                {
                    const auto& Val = (*LightsArr)[Idx];
                    if (!Val.IsValid() || Val->Type != EJson::Object) continue;
                    const TSharedPtr<FJsonObject> LightObj = Val->AsObject();

                    const FVector LightLoc = ReadJsonVec(LightObj, TEXT(""), 0.0);
                    const float Intensity = static_cast<float>(ReadJsonNum(LightObj, TEXT("Intensity"), 10.0));
                    const float SourceRadius = static_cast<float>(ReadJsonNum(LightObj, TEXT("SourceRadius"), 10.0));
                    const float AttenRadius = static_cast<float>(ReadJsonNum(LightObj, TEXT("AttenuationRadius"), 1000.0));
                    const float Length = static_cast<float>(ReadJsonNum(LightObj, TEXT("Length"), 0.0));
                    FString Unit = TEXT("Lumens");
                    LightObj->TryGetStringField(TEXT("Unit"), Unit);

                    const FVector WorldLightLoc = NodeXform.TransformPosition(LightLoc);
                    AddCrossMarker(WorldLightLoc, FMath::Max(6.f, GizmoScale * 0.45f), LightCoreColor, Bounds);
                    if (SourceRadius > 0.5f)
                    {
                        AddSphereLines(LightLoc, SourceRadius, NodeXform, LightCoreColor, 1.6f, Bounds, 14);
                    }
                    if (AttenRadius > 1.f)
                    {
                        AddSphereLines(LightLoc, AttenRadius, NodeXform, LightAttenColor, 1.0f, Bounds, 20);
                    }
                    if (Length > 0.5f)
                    {
                        const FVector L0 = NodeXform.TransformPosition(LightLoc - FVector(Length * 0.5f, 0, 0));
                        const FVector L1 = NodeXform.TransformPosition(LightLoc + FVector(Length * 0.5f, 0, 0));
                        OverlayLines.Add({L0, L1, LightCoreColor, 2.5f});
                    }
                    OverlayLabels.Add({WorldLightLoc, FString::Printf(TEXT("Light[%d] %.0f %s (%s)"), Idx, Intensity, *Unit, *NodeKey), LightCoreColor});
                }
            }
        }
    }

    // 2b. Vehicle / Train WheelMeshNames visualization (bogie axle chord + wheel markers)
    {
        const TSharedPtr<FJsonObject>* TuningObj = nullptr;
        const bool bHasTune = (Json->TryGetObjectField(TEXT("TrainTuning"), TuningObj) && TuningObj && TuningObj->IsValid())
            || (Json->TryGetObjectField(TEXT("VehicleTuning"), TuningObj) && TuningObj && TuningObj->IsValid());
        if (bHasTune)
        {
            const TArray<TSharedPtr<FJsonValue>>* WheelArr = nullptr;
            if ((*TuningObj)->TryGetArrayField(TEXT("WheelMeshNames"), WheelArr) && WheelArr)
            {
                const FLinearColor WheelColor(0.15f, 0.88f, 1.0f, 0.95f);
                const FLinearColor AxleColor(1.0f, 0.78f, 0.15f, 0.90f);
                TArray<FVector> WheelPositions;
                for (const TSharedPtr<FJsonValue>& WVal : *WheelArr)
                {
                    if (!WVal.IsValid() || WVal->Type != EJson::String) continue;
                    const FString WName = WVal->AsString().TrimStartAndEnd();
                    if (const FTransform* WXform = NodeWorldTransforms.Find(WName))
                    {
                        const FVector WPos = WXform->GetLocation();
                        WheelPositions.Add(WPos);
                        AddSphereLines(FVector::ZeroVector, FMath::Max(12.f, GizmoScale * 0.6f), *WXform, WheelColor, 2.0f, Bounds, 14);
                        AddCrossMarker(WPos, FMath::Max(6.f, GizmoScale * 0.4f), AxleColor, Bounds);
                        OverlayLabels.Add({WPos, TEXT("Wheel: ") + WName, WheelColor});
                    }
                }
                if (WheelPositions.Num() >= 2)
                {
                    double MinX = DBL_MAX, MaxX = -DBL_MAX;
                    for (const FVector& P : WheelPositions) { MinX = FMath::Min(MinX, P.X); MaxX = FMath::Max(MaxX, P.X); }
                    const double MidX = 0.5 * (MinX + MaxX);
                    FVector FrontSum = FVector::ZeroVector, RearSum = FVector::ZeroVector;
                    int32 FC = 0, RC = 0;
                    for (const FVector& P : WheelPositions)
                    {
                        if (P.X >= MidX) { FrontSum += P; ++FC; }
                        else { RearSum += P; ++RC; }
                    }
                    if (FC > 0 && RC > 0)
                    {
                        const FVector FrontPivot = FrontSum / static_cast<double>(FC);
                        const FVector RearPivot = RearSum / static_cast<double>(RC);
                        OverlayLines.Add({RearPivot, FrontPivot, AxleColor, 2.4f});
                        OverlayLabels.Add({FrontPivot, TEXT("Front Bogie"), AxleColor});
                        OverlayLabels.Add({RearPivot, TEXT("Rear Bogie"), AxleColor});
                    }
                }
            }
        }
    }

    // 3. Item HoldTransform, RightHandIK, LeftHandIK
    const TSharedPtr<FJsonObject>* HoldObj = nullptr;
    if (Json->TryGetObjectField(TEXT("HoldTransform"), HoldObj) && HoldObj && HoldObj->IsValid())
    {
        const FTransform HoldXform = ReadJsonTransform(*HoldObj);
        const FLinearColor HoldColor(1.0f, 0.58f, 0.18f, 1.0f);
        AddAxisGizmo(HoldXform, GizmoScale, HoldColor, Bounds);
        OverlayLabels.Add({HoldXform.GetLocation(), TEXT("HoldTransform"), HoldColor});
    }

    const TSharedPtr<FJsonObject>* RightIKObj = nullptr;
    if (Json->TryGetObjectField(TEXT("RightHandIK"), RightIKObj) && RightIKObj && RightIKObj->IsValid())
    {
        const FVector Pos = ReadJsonVec(*RightIKObj, TEXT(""), 0.0);
        const FLinearColor Color(0.98f, 0.35f, 0.85f, 1.0f);
        AddSphereLines(Pos, FMath::Max(3.f, GizmoScale * 0.28f), FTransform::Identity, Color, 1.8f, Bounds, 12);
        AddCrossMarker(Pos, FMath::Max(4.f, GizmoScale * 0.4f), Color, Bounds);
        OverlayLabels.Add({Pos, TEXT("RightHandIK"), Color});
    }

    const TSharedPtr<FJsonObject>* LeftIKObj = nullptr;
    if (Json->TryGetObjectField(TEXT("LeftHandIK"), LeftIKObj) && LeftIKObj && LeftIKObj->IsValid())
    {
        const FVector Pos = ReadJsonVec(*LeftIKObj, TEXT(""), 0.0);
        const FLinearColor Color(0.68f, 0.45f, 1.0f, 1.0f);
        AddSphereLines(Pos, FMath::Max(3.f, GizmoScale * 0.28f), FTransform::Identity, Color, 1.8f, Bounds, 12);
        AddCrossMarker(Pos, FMath::Max(4.f, GizmoScale * 0.4f), Color, Bounds);
        OverlayLabels.Add({Pos, TEXT("LeftHandIK"), Color});
    }

    // 4. EquipmentInteraction RightGrip / LeftGrip AttachmentOffset
    const TSharedPtr<FJsonObject>* EquipObj = nullptr;
    if (Json->TryGetObjectField(TEXT("EquipmentInteraction"), EquipObj) && EquipObj && EquipObj->IsValid())
    {
        auto DrawGrip = [&](const TCHAR* GripKey, const TCHAR* Label, const FLinearColor& Color)
        {
            const TSharedPtr<FJsonObject>* GripObj = nullptr;
            if (!(*EquipObj)->TryGetObjectField(GripKey, GripObj) || !GripObj || !GripObj->IsValid()) return;
            bool bGripEnabled = false;
            if (!(*GripObj)->TryGetBoolField(TEXT("bEnabled"), bGripEnabled) || !bGripEnabled) return;
            const TSharedPtr<FJsonObject>* OffsetObj = nullptr;
            if (!(*GripObj)->TryGetObjectField(TEXT("AttachmentOffset"), OffsetObj) || !OffsetObj || !OffsetObj->IsValid()) return;
            const FTransform GripXform = ReadJsonTransform(*OffsetObj);
            AddAxisGizmo(GripXform, GizmoScale * 0.9f, Color, Bounds);
            OverlayLabels.Add({GripXform.GetLocation(), Label, Color});
        };
        DrawGrip(TEXT("RightGrip"), TEXT("R-Grip"), FLinearColor(1.0f, 0.42f, 0.72f, 1.0f));
        DrawGrip(TEXT("LeftGrip"), TEXT("L-Grip"), FLinearColor(0.45f, 0.85f, 1.0f, 1.0f));
    }

    // 5. Weapon MuzzleOffset
    const TSharedPtr<FJsonObject>* MuzzleObj = nullptr;
    if (Json->TryGetObjectField(TEXT("MuzzleOffset"), MuzzleObj) && MuzzleObj && MuzzleObj->IsValid())
    {
        const FVector MuzzlePos = ReadJsonVec(*MuzzleObj, TEXT(""), 0.0);
        const FLinearColor MuzzleColor(1.0f, 0.25f, 0.25f, 1.0f);
        AddCrossMarker(MuzzlePos, FMath::Max(4.f, GizmoScale * 0.4f), MuzzleColor, Bounds);
        const FVector AimTip = MuzzlePos + FVector(GizmoScale * 1.6f, 0.f, 0.f);
        Bounds += AimTip;
        OverlayLines.Add({MuzzlePos, AimTip, MuzzleColor, 2.2f});
        OverlayLabels.Add({MuzzlePos, TEXT("Muzzle"), MuzzleColor});
    }

    // 6. GravityField
    const TSharedPtr<FJsonObject>* GravityObj = nullptr;
    if (Json->TryGetObjectField(TEXT("GravityField"), GravityObj) && GravityObj && GravityObj->IsValid())
    {
        bool bGravEnabled = false;
        if ((*GravityObj)->TryGetBoolField(TEXT("Enabled"), bGravEnabled) && bGravEnabled)
        {
            const TSharedPtr<FJsonObject>* CenterObj = nullptr;
            const FVector LocalCenter = ((*GravityObj)->TryGetObjectField(TEXT("LocalCenter"), CenterObj) && CenterObj && CenterObj->IsValid())
                ? ReadJsonVec(*CenterObj, TEXT(""), 0.0)
                : FVector::ZeroVector;
            const float GravR = static_cast<float>(ReadJsonNum(*GravityObj, TEXT("RadiusCm"), 1000.0));
            const FLinearColor GravColor(0.30f, 0.65f, 1.0f, 0.65f);
            AddSphereLines(LocalCenter, GravR, FTransform::Identity, GravColor, 1.2f, Bounds, 20);
            OverlayLabels.Add({LocalCenter, FString::Printf(TEXT("Gravity R=%.0fcm"), GravR), GravColor});
        }
    }

    bHasOverlays = Bounds.IsValid && !OverlayLines.IsEmpty();
    OverlayCenter = Bounds.IsValid ? Bounds.GetCenter() : FVector::ZeroVector;
    OverlayRadius = Bounds.IsValid ? FMath::Max(10.f, static_cast<float>(Bounds.GetExtent().Size())) : 1.f;
    UpdateEffectiveBounds();
    Invalidate(EInvalidateWidgetReason::Paint);
}

int32 SV3DModelPreview::OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& Clip,
    FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle& Style, bool bEnabled) const
{
    const FVector2D Size = Geometry.GetLocalSize();
    if ((!bHasModel && !bHasOverlays) || Size.X <= 1.0 || Size.Y <= 1.0 || !FMath::IsFinite(Radius) || Radius <= KINDA_SMALL_NUMBER)
    {
        return Layer;
    }

    const FQuat Rotation = FRotator(Pitch, Yaw, 0).Quaternion();
    const float Scale = static_cast<float>(FMath::Min(Size.X, Size.Y)) * .43f * Zoom / Radius;
    const FLinearColor LineColor = FLinearColor(.75f, .86f, 1.f, .65f) * Style.GetColorAndOpacityTint();
    const FPaintGeometry PaintGeometry = Geometry.ToPaintGeometry();
    const FVector ViewTarget = Center + PanOffset;

    auto Project = [&](const FVector& Point) -> FVector2f
    {
        const FVector P = Rotation.UnrotateVector(Point - ViewTarget);
        return FVector2f(static_cast<float>(Size.X * .5 + P.Y * Scale), static_cast<float>(Size.Y * .5 - P.Z * Scale));
    };

    // World origin reference axes (X=Red, Y=Green, Z=Blue)
    const float AxisLen = FMath::Max(10.f, Radius * 0.25f);
    const FVector2f Origin2D = Project(FVector::ZeroVector);
    {
        TArray<FVector2f> AxisPts;
        AxisPts.SetNumUninitialized(2);
        AxisPts[0] = Origin2D;
        AxisPts[1] = Project(FVector(AxisLen, 0, 0));
        FSlateDrawElement::MakeLines(Out, Layer, PaintGeometry, AxisPts, ESlateDrawEffect::None, FLinearColor(0.95f, 0.25f, 0.25f, 0.55f), true, 1.2f);
        AxisPts[1] = Project(FVector(0, AxisLen, 0));
        FSlateDrawElement::MakeLines(Out, Layer, PaintGeometry, AxisPts, ESlateDrawEffect::None, FLinearColor(0.25f, 0.90f, 0.35f, 0.55f), true, 1.2f);
        AxisPts[1] = Project(FVector(0, 0, AxisLen));
        FSlateDrawElement::MakeLines(Out, Layer, PaintGeometry, AxisPts, ESlateDrawEffect::None, FLinearColor(0.30f, 0.60f, 1.0f, 0.55f), true, 1.2f);
    }

    // Mesh wireframe edges
    if (Edges.Num() >= 3)
    {
        TArray<FVector2f> Points;
        Points.SetNumUninitialized(4);
        for (int32 Index = 0; Index + 2 < Edges.Num(); Index += 3)
        {
            Points[0] = Project(Edges[Index].Key);
            Points[1] = Project(Edges[Index].Value);
            Points[2] = Project(Edges[Index + 1].Value);
            Points[3] = Project(Edges[Index + 2].Value);
            FSlateDrawElement::MakeLines(Out, Layer, PaintGeometry, Points,
                ESlateDrawEffect::None, LineColor, true, 1.f);
        }
    }

    // Live JSON overlay lines (Colliders, Lights, Transform, Hand IK, Grips, Muzzle, Gravity)
    if (!OverlayLines.IsEmpty())
    {
        const int32 OverlayLayer = Layer + 1;
        TArray<FVector2f> Seg;
        Seg.SetNumUninitialized(2);
        for (const FOverlayLine& Line : OverlayLines)
        {
            Seg[0] = Project(Line.A);
            Seg[1] = Project(Line.B);
            FSlateDrawElement::MakeLines(Out, OverlayLayer, PaintGeometry, Seg,
                ESlateDrawEffect::None, Line.Color * Style.GetColorAndOpacityTint(), true, Line.Thickness);
        }
    }

    // Overlay labels
    if (!OverlayLabels.IsEmpty())
    {
        const int32 LabelLayer = Layer + 2;
        const FSlateFontInfo LabelFont = FCoreStyle::GetDefaultFontStyle("Regular", 9);
        for (const FOverlayLabel& Lbl : OverlayLabels)
        {
            const FVector2f ScreenPos = Project(Lbl.Position);
            if (ScreenPos.X < -40.f || ScreenPos.X > Size.X + 40.f || ScreenPos.Y < -20.f || ScreenPos.Y > Size.Y + 20.f)
            {
                continue;
            }
            const FPaintGeometry TextGeom = Geometry.ToPaintGeometry(
                FVector2f(220.f, 18.f),
                FSlateLayoutTransform(FVector2f(ScreenPos.X + 5.f, ScreenPos.Y - 8.f)));
            FSlateDrawElement::MakeText(Out, LabelLayer, TextGeom, Lbl.Text, LabelFont,
                ESlateDrawEffect::None, Lbl.Color * Style.GetColorAndOpacityTint());
        }
        return LabelLayer;
    }

    return Layer + 1;
}

FReply SV3DModelPreview::OnMouseButtonDown(const FGeometry&, const FPointerEvent& E)
{
    if (E.GetEffectingButton() == EKeys::LeftMouseButton)
    {
        bOrbiting = true;
        bPanning = false;
        return FReply::Handled().CaptureMouse(SharedThis(this));
    }
    if (E.GetEffectingButton() == EKeys::RightMouseButton)
    {
        bPanning = true;
        bOrbiting = false;
        return FReply::Handled().CaptureMouse(SharedThis(this));
    }
    return FReply::Unhandled();
}

FReply SV3DModelPreview::OnMouseButtonUp(const FGeometry&, const FPointerEvent&)
{
    bOrbiting = false;
    bPanning = false;
    return FReply::Handled().ReleaseMouseCapture();
}

FReply SV3DModelPreview::OnMouseButtonDoubleClick(const FGeometry&, const FPointerEvent& E)
{
    if (E.GetEffectingButton() == EKeys::LeftMouseButton)
    {
        ResetCamera();
        return FReply::Handled();
    }
    return FReply::Unhandled();
}

FReply SV3DModelPreview::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& E)
{
    if (!HasMouseCapture()) return FReply::Unhandled();
    const FVector2D Delta = E.GetCursorDelta();
    if (bOrbiting)
    {
        Yaw += static_cast<float>(Delta.X) * .4f;
        Pitch = FMath::Clamp(Pitch + static_cast<float>(Delta.Y) * .4f, -89.f, 89.f);
        Invalidate(EInvalidateWidgetReason::Paint);
        return FReply::Handled();
    }
    if (bPanning)
    {
        const FVector2D Size = Geometry.GetLocalSize();
        const float Scale = FMath::Max(0.0001f, static_cast<float>(FMath::Min(Size.X, Size.Y)) * .43f * Zoom / FMath::Max(1.f, Radius));
        const FQuat Rotation = FRotator(Pitch, Yaw, 0).Quaternion();
        const FVector RightDir = Rotation.RotateVector(FVector::RightVector);
        const FVector UpDir = Rotation.RotateVector(FVector::UpVector);
        PanOffset += (-RightDir * (Delta.X / Scale)) + (UpDir * (Delta.Y / Scale));
        Invalidate(EInvalidateWidgetReason::Paint);
        return FReply::Handled();
    }
    return FReply::Unhandled();
}

FReply SV3DModelPreview::OnMouseWheel(const FGeometry&, const FPointerEvent& E)
{
    Zoom = FMath::Clamp(Zoom * FMath::Pow(1.15f, E.GetWheelDelta()), .15f, 10.f);
    Invalidate(EInvalidateWidgetReason::Paint);
    return FReply::Handled();
}
