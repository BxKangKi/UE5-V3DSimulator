// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file ModelData.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Model/ModelData.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "System/StringHelper.h"
#include "System/JsonHelper.h"
#include "System/MacroLibrary.h"
#include "System/JsonMetadata.h"

// ==========================================
// FModelCollider
// ==========================================

TSharedRef<FJsonObject> FModelCollider::Serialization() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    FJsonHelper::SetEnumField<EColliderType>(Json, TEXT("Type"), Collider);
    if (Collider == EColliderType::None)
    {
        return Json;
    }

    FJsonHelper::SetVector(Json, Center);
    if (Collider == EColliderType::Sphere)
    {
        const double Radius = FMath::Max(1.0, FMath::Abs(Size.X));
        Json->SetNumberField(TEXT("Radius"), Radius);
    }
    else if (Collider == EColliderType::Capsule)
    {
        const double Radius = FMath::Max(1.0, FMath::Abs(Size.X));
        const double HalfHeight = FMath::Max(Radius, FMath::Max(FMath::Abs(Size.Z), FMath::Abs(Size.Y)));
        Json->SetNumberField(TEXT("Radius"), Radius);
        Json->SetNumberField(TEXT("Height"), HalfHeight * 2.0);
    }
    else
    {
        FJsonHelper::SetVector(Json, Size.GetAbs(), TEXT("D"));
    }
    return Json;
}

bool FModelCollider::Deserialization(const TSharedPtr<FJsonObject> &Json)
{
    if (!Json.IsValid())
        return false;

    Collider = EColliderType::None;
    Center = FVector::ZeroVector;
    Size = FVector::ZeroVector;

    FJsonHelper::TryGetEnumField<EColliderType>(Json, TEXT("Type"), Collider);
    if (Collider == EColliderType::None)
    {
        return true;
    }

    FJsonHelper::TryGetVector(Json, Center);
    FVector LegacyD = FVector::ZeroVector;
    FJsonHelper::TryGetVector(Json, LegacyD, TEXT("D"));

    if (Collider == EColliderType::Sphere)
    {
        double Radius = 0.0;
        if (!Json->TryGetNumberField(TEXT("Radius"), Radius))
        {
            Radius = FMath::Max3(FMath::Abs(LegacyD.X), FMath::Abs(LegacyD.Y), FMath::Abs(LegacyD.Z));
        }
        const double SafeRadius = FMath::Max(0.0, FMath::Abs(Radius));
        Size = FVector(SafeRadius, SafeRadius, SafeRadius);
    }
    else if (Collider == EColliderType::Capsule)
    {
        double Radius = 0.0;
        if (!Json->TryGetNumberField(TEXT("Radius"), Radius))
        {
            Radius = FMath::Abs(LegacyD.X);
        }
        double HalfHeight = 0.0;
        double FullHeight = 0.0;
        if (Json->TryGetNumberField(TEXT("Height"), FullHeight))
        {
            HalfHeight = FMath::Abs(FullHeight) * 0.5;
        }
        else if (Json->TryGetNumberField(TEXT("HalfHeight"), HalfHeight))
        {
            HalfHeight = FMath::Abs(HalfHeight);
        }
        else
        {
            HalfHeight = FMath::Max(FMath::Abs(LegacyD.Z), FMath::Abs(LegacyD.Y));
        }
        const double SafeRadius = FMath::Max(0.0, FMath::Abs(Radius));
        const double SafeHalfHeight = FMath::Max(SafeRadius, HalfHeight);
        Size = FVector(SafeRadius, SafeHalfHeight, SafeHalfHeight);
    }
    else
    {
        Size = LegacyD.GetAbs();
    }
    return true;
}

// ==========================================
// FLightData
// ==========================================

TSharedRef<FJsonObject> FLightData::Serialization() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    FJsonHelper::SetEnumField<ELightType>(Json, TEXT("Type"), Type);
    FJsonHelper::SetVector(Json, Location);
    Json->SetNumberField(TEXT("Pitch"), Rotation.Pitch);
    Json->SetNumberField(TEXT("Yaw"), Rotation.Yaw);
    Json->SetNumberField(TEXT("Roll"), Rotation.Roll);
    Json->SetNumberField(TEXT("ColorR"), Color.R);
    Json->SetNumberField(TEXT("ColorG"), Color.G);
    Json->SetNumberField(TEXT("ColorB"), Color.B);
    FJsonHelper::SetEnumField<ELightUnits>(Json, TEXT("Unit"), Unit);
    Json->SetNumberField(TEXT("Intensity"), Intensity);
    Json->SetNumberField(TEXT("SourceRadius"), SourceRadius);
    Json->SetNumberField(TEXT("SoftSourceRadius"), SoftSourceRadius);
    Json->SetNumberField(TEXT("AttenuationRadius"), AttenuationRadius);
    Json->SetNumberField(TEXT("Length"), Length);
    if (Type == ELightType::Spot)
    {
        Json->SetNumberField(TEXT("InnerConeAngle"), InnerConeAngle);
        Json->SetNumberField(TEXT("OuterConeAngle"), OuterConeAngle);
    }
    else if (Type == ELightType::Rect)
    {
        Json->SetNumberField(TEXT("SourceWidth"), SourceWidth);
        Json->SetNumberField(TEXT("SourceHeight"), SourceHeight);
        Json->SetNumberField(TEXT("BarnDoorAngle"), BarnDoorAngle);
        Json->SetNumberField(TEXT("BarnDoorLength"), BarnDoorLength);
    }

    return Json;
}

bool FLightData::Deserialization(const TSharedPtr<FJsonObject> &Json)
{
    if (!Json.IsValid())
        return false;

    Type = ELightType::Point;
    FString TypeStr;
    if (Json->TryGetStringField(TEXT("Type"), TypeStr))
    {
        if (TypeStr.Equals(TEXT("Spot"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("SpotLight"), ESearchCase::IgnoreCase))
            Type = ELightType::Spot;
        else if (TypeStr.Equals(TEXT("Rect"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("RectLight"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("Area"), ESearchCase::IgnoreCase))
            Type = ELightType::Rect;
        else
            Type = ELightType::Point;
    }
    else
    {
        FJsonHelper::TryGetEnumField<ELightType>(Json, TEXT("Type"), Type);
    }
    FJsonHelper::TryGetVector(Json, Location);
    double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
    Json->TryGetNumberField(TEXT("Pitch"), Pitch);
    Json->TryGetNumberField(TEXT("Yaw"), Yaw);
    Json->TryGetNumberField(TEXT("Roll"), Roll);
    Rotation = FRotator(Pitch, Yaw, Roll);
    double CR = 1.0, CG = 1.0, CB = 1.0;
    Json->TryGetNumberField(TEXT("ColorR"), CR);
    Json->TryGetNumberField(TEXT("ColorG"), CG);
    Json->TryGetNumberField(TEXT("ColorB"), CB);
    Color = FLinearColor(static_cast<float>(CR), static_cast<float>(CG), static_cast<float>(CB), 1.0f);
    FJsonHelper::TryGetEnumField<ELightUnits>(Json, TEXT("Unit"), Unit);
    Json->TryGetNumberField(TEXT("Intensity"), Intensity);
    Json->TryGetNumberField(TEXT("SourceRadius"), SourceRadius);
    Json->TryGetNumberField(TEXT("SoftSourceRadius"), SoftSourceRadius);
    Json->TryGetNumberField(TEXT("AttenuationRadius"), AttenuationRadius);
    Json->TryGetNumberField(TEXT("Length"), Length);
    Json->TryGetNumberField(TEXT("InnerConeAngle"), InnerConeAngle);
    Json->TryGetNumberField(TEXT("OuterConeAngle"), OuterConeAngle);
    Json->TryGetNumberField(TEXT("SourceWidth"), SourceWidth);
    Json->TryGetNumberField(TEXT("SourceHeight"), SourceHeight);
    Json->TryGetNumberField(TEXT("BarnDoorAngle"), BarnDoorAngle);
    Json->TryGetNumberField(TEXT("BarnDoorLength"), BarnDoorLength);
    return true;
}

// ==========================================
// FMeshData
// ==========================================

TSharedRef<FJsonObject> FMeshData::Serialization() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetBoolField(TEXT("ComplexCollision"), bComplexCollision);
    Json->SetBoolField(TEXT("SimpleCollision"), bSimpleCollision);
    Json->SetBoolField(TEXT("IsEntity"), bIsEntity);
    Json->SetBoolField(TEXT("GenerateImpostor"), bGenerateImpostor);

    // Automates TArray struct serialization using a lambda capture.
    FJsonHelper::SetArray<FModelCollider>(Json, TEXT("Colliders"), Colliders, [](const FModelCollider &Item)
                                          { return Item.Serialization(); });

    FJsonHelper::SetArray<FLightData>(Json, TEXT("Lights"), Lights, [](const FLightData &Item)
                                             { return Item.Serialization(); });

    return Json;
}

bool FMeshData::Deserialization(const TSharedPtr<FJsonObject> &Json)
{
    if (!Json.IsValid())
        return false;

    Json->TryGetBoolField(TEXT("ComplexCollision"), bComplexCollision);
    Json->TryGetBoolField(TEXT("SimpleCollision"), bSimpleCollision);
    Json->TryGetBoolField(TEXT("IsEntity"), bIsEntity);
    Json->TryGetBoolField(TEXT("GenerateImpostor"), bGenerateImpostor);

    // Automates TArray struct deserialization.
    FJsonHelper::TryGetArray<FModelCollider>(Json, TEXT("Colliders"), Colliders, [](const TSharedPtr<FJsonObject> &Obj, FModelCollider &OutItem)
                                             { return OutItem.Deserialization(Obj); });

    FJsonHelper::TryGetArray<FLightData>(Json, TEXT("Lights"), Lights, [](const TSharedPtr<FJsonObject> &Obj, FLightData &OutItem)
                                                { return OutItem.Deserialization(Obj); });

    return true;
}

// ==========================================
// FModelData
// ==========================================

TSharedRef<FJsonObject> FModelData::Serialization() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(V3DSimulatorJsonMetadata::Version, V3DSimulatorJsonMetadata::SchemaVersion);
    // JSON is external, user-authored input. Program-owned bounds are emitted only into .v3d
    // metadata and must never be mixed into this settings document.
    // Automates TMap struct serialization without duplicate loops.
    FJsonHelper::SetMap<FMeshData>(Json, TEXT("MeshData"), MeshData, [](const FMeshData &Item)
                                          { return Item.Serialization(); });

    return Json;
}

bool FModelData::Deserialization(const TSharedPtr<FJsonObject> &Json)
{
    if (!Json.IsValid())
        return false;

    Center = FVector::ZeroVector;
    Size = FVector::ZeroVector;
    MeshData.Empty();
    // Automates TMap struct deserialization while safely restoring JSON keys as FName values.
    FJsonHelper::TryGetMap<FMeshData>(Json, TEXT("MeshData"), MeshData, [](const TSharedPtr<FJsonObject> &Obj, FMeshData &OutItem)
                                             { return OutItem.Deserialization(Obj); });

    return true;
}

