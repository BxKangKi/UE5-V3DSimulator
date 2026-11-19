// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/WorldSurfaceConfig.h"
#include "System/WorldArchive.h"

namespace V3DSurfaceConfigPrivate
{
    bool Number(const FJsonObject& Json, const TCHAR* Key, float& Out, double Min, double Max)
    {
        if (!Json.HasField(Key)) return true;
        double Value = 0;
        if (!Json.TryGetNumberField(Key, Value) || !FMath::IsFinite(Value) || Value < Min || Value > Max) return false;
        Out = static_cast<float>(Value);
        return true;
    }
    bool Boolean(const FJsonObject& Json, const TCHAR* Key, bool& Out)
    {
        return !Json.HasField(Key) || Json.TryGetBoolField(Key, Out);
    }
    bool Combine(const FJsonObject& Json, const TCHAR* Key, uint8& Out)
    {
        if (!Json.HasField(Key)) return true;
        FString Value;
        if (!Json.TryGetStringField(Key, Value)) return false;
        const TCHAR* Names[] = { TEXT("Average"), TEXT("Min"), TEXT("Multiply"), TEXT("Max") };
        for (uint8 Index = 0; Index < 4; ++Index)
            if (Value == Names[Index]) { Out = Index; return true; }
        return false;
    }
    bool Name(const FString& Value)
    {
        return !Value.IsEmpty() && Value.Len() <= 128 && Value == Value.TrimStartAndEnd()
            && !FName(*Value).IsNone();
    }
}

bool FV3DWorldSurfaceConfig::Parse(const TSharedPtr<FJsonObject>& Root, FV3DWorldSurfaceConfig& Out, FString& Error)
{
    using namespace V3DSurfaceConfigPrivate;
    Out = FV3DWorldSurfaceConfig();
    Error.Reset();
    FV3DWorldSurfaceConfig Parsed;
    auto Fail = [&Error](const FString& Message) { Error = TEXT("World surfaces: ") + Message; return false; };
    if (!Root.IsValid()) return Fail(TEXT("missing config object"));
    const TSharedPtr<FJsonObject>* Definitions = nullptr;
    if (Root->HasField(TEXT("PhysicalMaterials")))
    {
        if (!Root->TryGetObjectField(TEXT("PhysicalMaterials"), Definitions) || !Definitions->IsValid()
            || (*Definitions)->Values.Num() > 64) return Fail(TEXT("PhysicalMaterials must be an object with at most 64 entries"));
        for (const auto& Pair : (*Definitions)->Values)
        {
            // UE 5.8 JSON keys use shared string storage, not implicitly convertible FString values.
            const FString SurfaceName(*Pair.Key);
            if (!Name(SurfaceName) || !Pair.Value.IsValid() || Pair.Value->Type != EJson::Object
                || Parsed.PhysicalMaterials.Contains(FName(*SurfaceName)))
            {
                return Fail(TEXT("invalid/duplicate physical material name: ") + SurfaceName);
            }
            const TSharedPtr<FJsonObject> Json = Pair.Value->AsObject();
            if (!Json.IsValid()) return Fail(TEXT("invalid physical material object"));
            FV3DSurfaceDefinition Definition;
            if (!Number(*Json, TEXT("Friction"), Definition.Friction, 0, 10)
                || !Number(*Json, TEXT("StaticFriction"), Definition.StaticFriction, 0, 10)
                || !Number(*Json, TEXT("Restitution"), Definition.Restitution, 0, 1)
                || !Number(*Json, TEXT("Density"), Definition.Density, 0.001, 100)
                || !Combine(*Json, TEXT("FrictionCombineMode"), Definition.FrictionCombine)
                || !Combine(*Json, TEXT("RestitutionCombineMode"), Definition.RestitutionCombine))
            {
                return Fail(TEXT("invalid physical coefficients: ") + SurfaceName);
            }
            Parsed.PhysicalMaterials.Add(FName(*SurfaceName), Definition);
        }
    }
    if (Root->HasField(TEXT("DefaultPhysicalMaterial")))
    {
        FString Value;
        if (!Root->TryGetStringField(TEXT("DefaultPhysicalMaterial"), Value) || !Name(Value)
            || !Parsed.PhysicalMaterials.Contains(FName(*Value))) return Fail(TEXT("unknown DefaultPhysicalMaterial"));
        Parsed.DefaultPhysicalMaterial = FName(*Value);
    }
    const TArray<TSharedPtr<FJsonValue>>* Rules = nullptr;
    if (Root->HasField(TEXT("MaterialPhysicalMappings")))
    {
        if (!Root->TryGetArrayField(TEXT("MaterialPhysicalMappings"), Rules) || Rules->Num() > 4096)
            return Fail(TEXT("MaterialPhysicalMappings must be an array with at most 4096 entries"));
        TSet<FString> Seen;
        for (const auto& Value : *Rules)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object) return Fail(TEXT("invalid material mapping"));
            const auto Json = Value->AsObject();
            FV3DSurfaceMaterialRule Rule;
            FString Surface;
            if (!Json.IsValid() || !Json->TryGetStringField(TEXT("Material"), Rule.Material)
                || !Json->TryGetStringField(TEXT("PhysicalMaterial"), Surface)
                || Rule.Material.IsEmpty() || Rule.Material.Len() > 256
                || Rule.Material != Rule.Material.TrimStartAndEnd()
                || Seen.Contains(Rule.Material.ToLower()) || !Parsed.PhysicalMaterials.Contains(FName(*Surface)))
                return Fail(TEXT("empty/duplicate material pattern or unknown physical material"));
            Seen.Add(Rule.Material.ToLower());
            Rule.PhysicalMaterial = FName(*Surface);
            Parsed.MaterialPhysicalMappings.Add(MoveTemp(Rule));
        }
    }
    const TSharedPtr<FJsonObject>* Lists = nullptr;
    if (Root->HasField(TEXT("SoundLists")))
    {
        if (!Root->TryGetObjectField(TEXT("SoundLists"), Lists) || (*Lists)->Values.Num() > 256)
            return Fail(TEXT("SoundLists must be an object with at most 256 lists"));
        for (const auto& Pair : (*Lists)->Values)
        {
            const FString ListName(*Pair.Key);
            const FName Name(*ListName);
            const TSharedPtr<FJsonObject>* List = nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Sounds = nullptr;
            if (Name.IsNone() || ListName.Len() > 128 || ListName != ListName.TrimStartAndEnd()
                || Parsed.SoundLists.Contains(Name) || !Pair.Value.IsValid() || !Pair.Value->TryGetObject(List)
                || !(*List)->TryGetArrayField(TEXT("Sounds"), Sounds) || Sounds->IsEmpty() || Sounds->Num() > 64)
                return Fail(TEXT("Invalid SoundLists entry: ") + ListName);
            FV3DSoundListDefinition Definition;
            if (!Boolean(**List, TEXT("Loop"), Definition.bLoop)) return Fail(TEXT("SoundLists.Loop must be a boolean"));
            for (const auto& Value : *Sounds)
            {
                FString Reference; FGuid UUID;
                if (!Value.IsValid() || !Value->TryGetString(Reference) || !FGWorldArchive::ParseSoundReference(Reference, UUID))
                    return Fail(TEXT("SoundLists.Sounds requires v3d://sound/<UUID>"));
                Definition.Sounds.AddUnique(FGWorldArchive::MakeSoundReference(UUID));
            }
            Parsed.SoundLists.Add(Name, MoveTemp(Definition));
        }
    }
    const TSharedPtr<FJsonObject>* Events = nullptr;
    if (Root->HasField(TEXT("Events")))
    {
        if (!Root->TryGetObjectField(TEXT("Events"), Events) || (*Events)->Values.Num() > 256)
            return Fail(TEXT("Events must be an object with at most 256 events"));
        for (const auto& Pair : (*Events)->Values)
        {
            const FString EventName(*Pair.Key);
            const TSharedPtr<FJsonObject>* Event = nullptr;
            FString List; FV3DSoundEventDefinition Definition; const FName Name(*EventName);
            if (Name.IsNone() || EventName.Len() > 128 || EventName != EventName.TrimStartAndEnd()
                || Parsed.Events.Contains(Name) || !Pair.Value.IsValid() || !Pair.Value->TryGetObject(Event)
                || !(*Event)->TryGetStringField(TEXT("SoundList"), List) || !Parsed.SoundLists.Contains(FName(List))
                || !Number(**Event, TEXT("Volume"), Definition.Volume, 0, 4)
                || !Number(**Event, TEXT("PitchMin"), Definition.PitchMin, .5, 2)
                || !Number(**Event, TEXT("PitchMax"), Definition.PitchMax, .5, 2)
                || Definition.PitchMax < Definition.PitchMin
                || ((*Event)->HasField(TEXT("Spatial")) && !(*Event)->TryGetBoolField(TEXT("Spatial"), Definition.bSpatial)))
                return Fail(TEXT("Invalid Events entry: ") + EventName);
            Definition.SoundList = FName(List);
            Definition.bLoop = Parsed.SoundLists.FindChecked(Definition.SoundList).bLoop;
            if (!Boolean(**Event, TEXT("Loop"), Definition.bLoop)) return Fail(TEXT("Events.Loop must be a boolean"));
            Parsed.Events.Add(Name, Definition);
        }
    }
    const TSharedPtr<FJsonObject>* Footsteps = nullptr;
    TSet<FString> UniqueSounds;
    if (Root->HasField(TEXT("FootstepMappings")))
    {
        if (!Root->TryGetObjectField(TEXT("FootstepMappings"), Footsteps) || !Footsteps->IsValid()
            || (*Footsteps)->Values.Num() > 64) return Fail(TEXT("FootstepMappings must be an object with at most 64 entries"));
        for (const auto& Pair : (*Footsteps)->Values)
        {
            const FString SurfaceName(*Pair.Key);
            const FName Surface(*SurfaceName);
            if (!Parsed.PhysicalMaterials.Contains(Surface) || Parsed.FootstepMappings.Contains(Surface)
                || !Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
            {
                return Fail(TEXT("unknown/duplicate footstep surface: ") + SurfaceName);
            }
            const auto Json = Pair.Value->AsObject();
            if (!Json.IsValid()) return Fail(TEXT("invalid footstep object"));
            const TArray<TSharedPtr<FJsonValue>>* Sounds = nullptr;
            FV3DFootstepDefinition Definition;
            const int32 SourceCount = int32(Json->HasField(TEXT("Sounds")))
                + int32(Json->HasField(TEXT("SoundList"))) + int32(Json->HasField(TEXT("Event")));
            if (SourceCount != 1) return Fail(TEXT("Footstep requires exactly one of Sounds, SoundList or Event: ") + SurfaceName);
            FString Source;
            if (Json->HasField(TEXT("Event")))
            {
                if (!Json->TryGetStringField(TEXT("Event"), Source) || !Parsed.Events.Contains(FName(*Source)))
                    return Fail(TEXT("Unknown footstep Event: ") + SurfaceName);
                Definition.Event = FName(*Source);
                const auto& Event = Parsed.Events.FindChecked(Definition.Event);
                Definition.SoundList = Event.SoundList;
                Definition.Sounds = Parsed.SoundLists.FindChecked(Event.SoundList).Sounds;
                Definition.Volume = Event.Volume; Definition.PitchMin = Event.PitchMin; Definition.PitchMax = Event.PitchMax;
                Definition.bLoop = Event.bLoop;
            }
            else if (Json->HasField(TEXT("SoundList")))
            {
                if (!Json->TryGetStringField(TEXT("SoundList"), Source) || !Parsed.SoundLists.Contains(FName(*Source)))
                    return Fail(TEXT("Unknown footstep SoundList: ") + SurfaceName);
                Definition.SoundList = FName(*Source);
                const auto& List = Parsed.SoundLists.FindChecked(Definition.SoundList);
                Definition.Sounds = List.Sounds; Definition.bLoop = List.bLoop;
            }
            else
            {
                if (!Json->TryGetArrayField(TEXT("Sounds"), Sounds) || Sounds->IsEmpty() || Sounds->Num() > 64)
                    return Fail(TEXT("Footstep Sounds requires 1..64 entries: ") + SurfaceName);
                for (const auto& Sound : *Sounds)
                {
                    FString Reference; FGuid UUID;
                    if (!Sound.IsValid() || !Sound->TryGetString(Reference) || !FGWorldArchive::ParseSoundReference(Reference, UUID))
                        return Fail(TEXT("footstep Sounds must use v3d://sound/<UUID>"));
                    Definition.Sounds.AddUnique(FGWorldArchive::MakeSoundReference(UUID));
                }
            }
            if (!Number(*Json, TEXT("Volume"), Definition.Volume, 0, 4)
                || !Number(*Json, TEXT("PitchMin"), Definition.PitchMin, 0.5, 2)
                || !Number(*Json, TEXT("PitchMax"), Definition.PitchMax, 0.5, 2)
                || Definition.PitchMax < Definition.PitchMin || !Boolean(*Json, TEXT("Loop"), Definition.bLoop))
                return Fail(TEXT("Invalid footstep volume/pitch/Loop: ") + SurfaceName);
            for (const auto& Reference : Definition.Sounds) UniqueSounds.Add(Reference);
            if (UniqueSounds.Num() > 64) return Fail(TEXT("at most 64 unique footstep sounds are allowed"));
            Parsed.FootstepMappings.Add(Surface, MoveTemp(Definition));
        }
    }
    Out = MoveTemp(Parsed);
    return true;
}

bool FV3DWorldSurfaceConfig::ValidateSounds(const TSet<FGuid>& Available, FString& Error) const
{
    for (const auto& Pair : SoundLists)
        for (const FString& Reference : Pair.Value.Sounds)
        {
            FGuid UUID;
            if (!FGWorldArchive::ParseSoundReference(Reference, UUID) || !Available.Contains(UUID))
            { Error = TEXT("Sound list references a missing asset: ") + Reference; return false; }
        }
    for (const auto& Pair : FootstepMappings)
        for (const FString& Reference : Pair.Value.Sounds)
        {
            FGuid UUID;
            if (!FGWorldArchive::ParseSoundReference(Reference, UUID) || !Available.Contains(UUID))
            {
                Error = FString::Printf(TEXT("Footstep sound missing from project resources: %s (%s)"), *Reference, *Pair.Key.ToString());
                return false;
            }
        }
    return true;
}

FString FV3DWorldSurfaceConfig::SourceMaterialName(const FString& Slot)
{
    // glTFRuntime static meshes wrap the original name as LOD_<n>_Section_<n>_<name>.
    int32 Position = 4;
    if (!Slot.StartsWith(TEXT("LOD_"))) return Slot;
    auto Digits = [&Slot, &Position]()
    {
        const int32 Start = Position;
        while (Position < Slot.Len() && FChar::IsDigit(Slot[Position])) ++Position;
        return Position > Start;
    };
    if (!Digits() || Slot.Mid(Position, 9) != TEXT("_Section_")) return Slot;
    Position += 9;
    if (!Digits() || Position >= Slot.Len() || Slot[Position] != TEXT('_')) return Slot;
    return Slot.Mid(Position + 1);
}

FName FV3DWorldSurfaceConfig::ResolveMaterial(const FString& SlotName) const
{
    const FString Source = SourceMaterialName(SlotName);
    // Exact rules win regardless of array order. Wildcards use the first matching row.
    for (const auto& Rule : MaterialPhysicalMappings)
        if (!Rule.Material.Contains(TEXT("*")) && !Rule.Material.Contains(TEXT("?"))
            && (Source.Equals(Rule.Material, ESearchCase::IgnoreCase) || SlotName.Equals(Rule.Material, ESearchCase::IgnoreCase)))
            return Rule.PhysicalMaterial;
    for (const auto& Rule : MaterialPhysicalMappings)
        if (Source.MatchesWildcard(Rule.Material, ESearchCase::IgnoreCase)
            || SlotName.MatchesWildcard(Rule.Material, ESearchCase::IgnoreCase)) return Rule.PhysicalMaterial;
    return DefaultPhysicalMaterial;
}
