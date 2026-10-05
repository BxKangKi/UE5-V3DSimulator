// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "Gravity/GravityFieldTypes.h"
#include "Dom/JsonObject.h"

bool FGravityFieldSettings::IsValid() const
{
    return FMath::IsFinite(RadiusCm) && RadiusCm >= 1.0 && RadiusCm <= 100000000.0
        && FMath::IsFinite(StrengthCmPerSecondSquared) && StrengthCmPerSecondSquared >= 0.0
        && StrengthCmPerSecondSquared <= 1000000.0 && !LocalCenter.ContainsNaN()
        && LocalCenter.GetAbs().GetMax() <= 100000000.0
        && static_cast<uint8>(Falloff) <= static_cast<uint8>(EGravityFieldFalloff::Linear);
}

bool FGravityFieldSettings::ReadJson(const TSharedPtr<FJsonObject>& Root, FGravityFieldSettings& Out, FString& Error)
{
    Out = FGravityFieldSettings();
    Error.Reset();
    if (!Root.IsValid()) { Error = TEXT("Model JSON is not an object"); return false; }
    if (!Root->HasField(TEXT("GravityField"))) return true;
    const TSharedPtr<FJsonObject>* Object = nullptr;
    if (!Root->TryGetObjectField(TEXT("GravityField"), Object) || !Object || !Object->IsValid())
    { Error = TEXT("GravityField must be an object"); return false; }
    const TSharedPtr<FJsonObject>& J = *Object;
    const TSet<FString> Keys = { TEXT("Enabled"), TEXT("RadiusCm"), TEXT("StrengthCmPerSecondSquared"),
        TEXT("Priority"), TEXT("Falloff"), TEXT("LocalCenter") };
    for (const auto& Pair : J->Values)
    {
        // UE 5.8 JSON keys use shared string storage; FString conversion is explicit.
        const FString FieldName(Pair.Key);
        if (!Keys.Contains(FieldName))
        {
            Error = TEXT("Unknown GravityField key: ") + FieldName;
            return false;
        }
    }
    FGravityFieldSettings S;
    bool Ok = true;
    if (J->HasField(TEXT("Enabled"))) Ok &= J->TryGetBoolField(TEXT("Enabled"), S.bEnabled);
    if (J->HasField(TEXT("RadiusCm"))) Ok &= J->TryGetNumberField(TEXT("RadiusCm"), S.RadiusCm);
    if (J->HasField(TEXT("StrengthCmPerSecondSquared")))
        Ok &= J->TryGetNumberField(TEXT("StrengthCmPerSecondSquared"), S.StrengthCmPerSecondSquared);
    if (J->HasField(TEXT("Priority")))
    {
        double Priority = 0.0;
        Ok &= J->TryGetNumberField(TEXT("Priority"), Priority);
        if (!FMath::IsFinite(Priority) || Priority < MIN_int32 || Priority > MAX_int32 || FMath::FloorToDouble(Priority) != Priority) Ok = false;
        else S.Priority = static_cast<int32>(Priority);
    }
    if (J->HasField(TEXT("Falloff")))
    {
        FString Falloff;
        Ok &= J->TryGetStringField(TEXT("Falloff"), Falloff);
        if (Falloff == TEXT("Linear")) S.Falloff = EGravityFieldFalloff::Linear;
        else if (Falloff != TEXT("Constant")) Ok = false;
    }
    if (J->HasField(TEXT("LocalCenter")))
    {
        const TSharedPtr<FJsonObject>* Center = nullptr;
        if (!J->TryGetObjectField(TEXT("LocalCenter"), Center) || !Center || !Center->IsValid()) Ok = false;
        else
        {
            Ok &= (*Center)->TryGetNumberField(TEXT("X"), S.LocalCenter.X);
            Ok &= (*Center)->TryGetNumberField(TEXT("Y"), S.LocalCenter.Y);
            Ok &= (*Center)->TryGetNumberField(TEXT("Z"), S.LocalCenter.Z);
            Ok &= (*Center)->Values.Num() == 3;
        }
    }
    if (!Ok || !S.IsValid())
    { Error = TEXT("Invalid GravityField value/type/range; use the canonical README schema"); return false; }
    Out = S;
    return true;
}
