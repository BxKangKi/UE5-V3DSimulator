// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "World/WorldEnvironmentConfig.h"
#include "System/WorldArchive.h"

bool FV3DBiomeDefinition::Contains(const FVector& Point) const
{
    if (Point.ContainsNaN()) return false;
    return bSphere ? FVector::DistSquared(Point, Center) <= FMath::Square(double(Radius))
        : FBox(Min, Max).IsInsideOrOn(Point);
}

bool FV3DEnvironmentConfig::Parse(const TSharedPtr<FJsonObject>& Root, FV3DEnvironmentConfig& Out, FString& Error)
{
    Out = {}; Error.Reset();
    if (!Root.IsValid()) { Error = TEXT("Environment config must be an object."); return false; }
    auto Fail = [&Error](const FString& Field) { Error = TEXT("Invalid environment field: ") + Field; return false; };
    auto Number = [](const TSharedPtr<FJsonObject>& J, const TCHAR* Key, double& V, double Min, double Max)
    { return !J->HasField(Key) || (J->HasTypedField<EJson::Number>(Key) && J->TryGetNumberField(Key, V) && FMath::IsFinite(V) && V >= Min && V <= Max); };
    auto Reference = [](const TSharedPtr<FJsonObject>& J, const TCHAR* Key, FString& V)
    {
        if (!J->HasField(Key)) return true;
        if (!J->TryGetStringField(Key, V)) return false;
        FGuid Id;
        return V.IsEmpty() || V == TEXT("none") || (V.StartsWith(TEXT("event://")) && V.Len() > 8 && V.Len() <= 136) || FGWorldArchive::ParseSoundReference(V, Id);
    };
    const TSharedPtr<FJsonObject>* A = nullptr;
    if (Root->HasField(TEXT("Audio")))
    {
        if (!Root->TryGetObjectField(TEXT("Audio"), A)) return Fail(TEXT("Audio"));
        auto& S = Out.Audio;
        for (const auto& Field : {TPair<const TCHAR*, TOptional<bool>*>(TEXT("DefaultFootstepLoop"), &S.DefaultFootstepLoop),
             TPair<const TCHAR*, TOptional<bool>*>(TEXT("WaterLoop"), &S.WaterLoop),
             TPair<const TCHAR*, TOptional<bool>*>(TEXT("UnderwaterLoop"), &S.UnderwaterLoop),
             TPair<const TCHAR*, TOptional<bool>*>(TEXT("EnvironmentMusicLoop"), &S.EnvironmentMusicLoop)})
        {
            bool Value = false;
            if ((*A)->HasField(Field.Key))
            {
                if (!(*A)->HasTypedField<EJson::Boolean>(Field.Key) || !(*A)->TryGetBoolField(Field.Key, Value)) return Fail(TEXT("Audio Loop must be a boolean"));
                *Field.Value = Value;
            }
        }
        for (auto Pair : {TPair<const TCHAR*, bool*>(TEXT("bGlobalAcoustics"), &S.bGlobalAcoustics),
                         TPair<const TCHAR*, bool*>(TEXT("bEnvironmentMusic"), &S.bEnvironmentMusic),
                         TPair<const TCHAR*, bool*>(TEXT("bBiomeAudioCrossfade"), &S.bBiomeAudioCrossfade)})
            if ((*A)->HasField(Pair.Key) && (!(*A)->HasTypedField<EJson::Boolean>(Pair.Key) || !(*A)->TryGetBoolField(Pair.Key, *Pair.Value))) return Fail(Pair.Key);
        if (!Reference(*A, TEXT("DefaultFootstep"), S.DefaultFootstep) || !Reference(*A, TEXT("Water"), S.Water)
            || !Reference(*A, TEXT("Underwater"), S.Underwater) || !Reference(*A, TEXT("EnvironmentMusic"), S.EnvironmentMusic))
            return Fail(TEXT("Audio sound reference"));
        double Volume = S.MusicVolume, Absorption = S.Absorption, Distance = S.ProbeDistanceCm, CrossfadeSeconds = S.BiomeCrossfadeSeconds;
        if (!Number(*A, TEXT("MusicVolume"), Volume, 0, 2) || !Number(*A, TEXT("Absorption"), Absorption, .02, 1)
            || !Number(*A, TEXT("ProbeDistanceCm"), Distance, 100, 20000)
            || !Number(*A, TEXT("BiomeCrossfadeSeconds"), CrossfadeSeconds, 0, 30)) return Fail(TEXT("Audio range"));
        S.MusicVolume = Volume; S.Absorption = Absorption; S.ProbeDistanceCm = Distance; S.BiomeCrossfadeSeconds = CrossfadeSeconds;
    }
    const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
    if (!Root->HasField(TEXT("Biomes"))) return true;
    if (!Root->TryGetArrayField(TEXT("Biomes"), Items) || Items->Num() > 256) return Fail(TEXT("Biomes (max 256)"));
    TSet<FName> Names;
    auto Vector = [](const TSharedPtr<FJsonObject>& J, const TCHAR* Key, FVector& V)
    {
        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (!J->TryGetArrayField(Key, Array) || Array->Num() != 3) return false;
        double X, Y, Z;
        if (!(*Array)[0].IsValid() || !(*Array)[1].IsValid() || !(*Array)[2].IsValid()
            || !(*Array)[0]->TryGetNumber(X) || !(*Array)[1]->TryGetNumber(Y) || !(*Array)[2]->TryGetNumber(Z)) return false;
        V = FVector(X, Y, Z);
        return !V.ContainsNaN() && V.GetAbs().GetMax() <= 1.e9;
    };
    for (const auto& Value : *Items)
    {
        const auto J = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
        FString Name, Shape = TEXT("Box"), Enter, Exit;
        FV3DBiomeDefinition B;
        if (!J.IsValid() || !J->TryGetStringField(TEXT("Name"), Name) || Name.IsEmpty() || Name.Len() > 128) return Fail(TEXT("Biome.Name"));
        B.Name = FName(Name); if (B.Name.IsNone() || Name != Name.TrimStartAndEnd() || Names.Contains(B.Name)) return Fail(TEXT("duplicate Biome.Name")); Names.Add(B.Name);
        if (J->HasField(TEXT("Shape")) && !J->TryGetStringField(TEXT("Shape"), Shape)) return Fail(TEXT("Biome.Shape"));
        B.bSphere = Shape == TEXT("Sphere");
        if (B.bSphere)
        {
            double Radius = B.Radius;
            if (!Vector(J, TEXT("Center"), B.Center) || !Number(J, TEXT("Radius"), Radius, 1, 1.e8)) return Fail(TEXT("Biome sphere"));
            B.Radius = Radius;
        }
        else if (Shape != TEXT("Box") || !Vector(J, TEXT("Min"), B.Min) || !Vector(J, TEXT("Max"), B.Max)
            || B.Min.X >= B.Max.X || B.Min.Y >= B.Max.Y || B.Min.Z >= B.Max.Z) return Fail(TEXT("Biome box"));
        double Priority = 0;
        if (J->HasField(TEXT("MusicLoop")))
        {
            bool bMusicLoop = false;
            if (!J->HasTypedField<EJson::Boolean>(TEXT("MusicLoop")) || !J->TryGetBoolField(TEXT("MusicLoop"), bMusicLoop)) return Fail(TEXT("Biome MusicLoop must be a boolean"));
            B.MusicLoop = bMusicLoop;
        }
        if (!Number(J, TEXT("Priority"), Priority, -100000, 100000) || !Reference(J, TEXT("Music"), B.Music)) return Fail(TEXT("Biome priority/music"));
        B.Priority = int32(Priority);
        if ((J->HasField(TEXT("EnterCommand")) && !J->TryGetStringField(TEXT("EnterCommand"), Enter))
            || (J->HasField(TEXT("ExitCommand")) && !J->TryGetStringField(TEXT("ExitCommand"), Exit))) return Fail(TEXT("Biome command"));
        B.EnterCommand = FName(Enter); B.ExitCommand = FName(Exit);
        Out.Biomes.Add(MoveTemp(B));
    }
    // Stable tie-break: earlier config entry wins for music. All overlaps still trigger.
    Out.Biomes.StableSort([](const auto& L, const auto& R) { return L.Priority > R.Priority; });
    return true;
}
