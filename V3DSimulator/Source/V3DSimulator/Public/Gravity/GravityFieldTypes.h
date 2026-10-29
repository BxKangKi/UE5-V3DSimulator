// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "GravityFieldTypes.generated.h"

class FJsonObject;

UENUM(BlueprintType)
enum class EGravityFieldFalloff : uint8 { Constant, Linear };

/** Acceleration is independent of the source object's mass and scale. Distances are centimetres. */
USTRUCT(BlueprintType)
struct V3DSIMULATOR_API FGravityFieldSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Gravity")
    bool bEnabled = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Gravity", meta=(ClampMin="1", ClampMax="100000000"))
    double RadiusCm = 1000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Gravity", meta=(ClampMin="0", ClampMax="1000000"))
    double StrengthCmPerSecondSquared = 980.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Gravity")
    int32 Priority = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Gravity")
    EGravityFieldFalloff Falloff = EGravityFieldFalloff::Constant;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Gravity")
    FVector LocalCenter = FVector::ZeroVector;

    bool IsValid() const;
    bool operator==(const FGravityFieldSettings& Other) const
    {
        return bEnabled == Other.bEnabled && RadiusCm == Other.RadiusCm
            && StrengthCmPerSecondSquared == Other.StrengthCmPerSecondSquared
            && Priority == Other.Priority && Falloff == Other.Falloff && LocalCenter == Other.LocalCenter;
    }
    /** Reads the optional GravityField object; missing means disabled, invalid means failure. */
    static bool ReadJson(const TSharedPtr<FJsonObject>& Root, FGravityFieldSettings& Out, FString& Error);
};

namespace V3DGravityMath
{
    /** A linear core prevents an undefined direction/infinite acceleration at the centre. */
    inline bool Evaluate(const FGravityFieldSettings& S, const FVector& Center, const FVector& Position,
        FVector& OutAcceleration, double& OutStrength, double& OutDistanceSquared)
    {
        OutAcceleration = FVector::ZeroVector;
        OutStrength = 0.0;
        OutDistanceSquared = 0.0;
        if (!S.bEnabled || !S.IsValid() || Center.ContainsNaN() || Position.ContainsNaN()) return false;
        const FVector Offset = Center - Position;
        const double DistanceSq = Offset.SizeSquared();
        if (!FMath::IsFinite(DistanceSq) || DistanceSq > FMath::Square(S.RadiusCm)) return false;
        OutDistanceSquared = DistanceSq;
        const double Distance = FMath::Sqrt(DistanceSq);
        const double CoreRadius = FMath::Min(10.0, S.RadiusCm * 0.01);
        const double Edge = S.Falloff == EGravityFieldFalloff::Linear ? 1.0 - Distance / S.RadiusCm : 1.0;
        const double Strength = S.StrengthCmPerSecondSquared * Edge;
        OutAcceleration = Offset * (Strength / FMath::Max(Distance, CoreRadius));
        OutStrength = OutAcceleration.Size();
        return true;
    }

    inline FVector PlanarForward(const FQuat& Rotation, const FVector& Up)
    {
        FVector Forward = FVector::VectorPlaneProject(Rotation.GetForwardVector(), Up).GetSafeNormal();
        if (Forward.IsNearlyZero())
            Forward = FVector::CrossProduct(Rotation.GetRightVector(), Up).GetSafeNormal();
        if (Forward.IsNearlyZero())
        {
            FVector Right;
            Up.FindBestAxisVectors(Forward, Right);
        }
        return Forward;
    }

    inline FQuat UprightRotation(const FQuat& Rotation, const FVector& Up)
    {
        return FRotationMatrix::MakeFromXZ(PlanarForward(Rotation, Up), Up).ToQuat();
    }
}
