// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "World/WorldFogConfig.h"
#include "System/SafeFileIO.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
    using namespace V3DFog;
    struct FField { const TCHAR* Name; EField Index; double Min; double Max; };
    const FField Fields[] = {
        {TEXT("Density"), Density, 0.f, 1.f},
        {TEXT("HeightFalloff"), HeightFalloff, .001, 5.},
        {TEXT("StartDistanceCm"), StartDistanceCm, 0.f, 1.e7f},
        {TEXT("MaxOpacity"), MaxOpacity, 0.f, 1.f},
        {TEXT("ScatteringDistribution"), ScatteringDistribution, -.9, .9},
        {TEXT("ExtinctionScale"), ExtinctionScale, 0.f, 10.f},
        {TEXT("ViewDistanceCm"), ViewDistanceCm, 100.f, 200000.f}
    };

    bool Fail(FString& Error, const FString& Path, const TCHAR* Reason)
    {
        Error = FString::Printf(TEXT("%s: %s %s"), FV3DWorldFogConfig::MemberName(), *Path, Reason);
        return false;
    }

    bool ParseProfile(const TSharedPtr<FJsonObject>& Json, const FString& Path,
        FPatch& Out, FString& Error, bool bGlobal = false)
    {
        if (!Json.IsValid()) return Fail(Error, Path, TEXT("must be an object."));
        for (const auto& Pair : Json->Values)
        {
            // UE 5.8 stores JSON keys as FSharedString. Own an FString before validation
            // and diagnostics; no implicit conversion or mutable string API is assumed.
            const FString FieldName(Pair.Key);
            if (bGlobal && (FieldName == TEXT("Enabled") || FieldName == TEXT("Volumetric"))) continue;
            bool bKnown = false;
            for (const auto& Field : Fields)
            {
                if (FieldName != Field.Name) continue;
                double Value = 0;
                if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Number || !Pair.Value->TryGetNumber(Value) || !FMath::IsFinite(Value)
                    || Value < Field.Min || Value > Field.Max)
                    return Fail(Error, Path + TEXT(".") + FieldName, TEXT("has an invalid numeric value or range."));
                Out.Set(Field.Index, float(Value));
                bKnown = true;
                break;
            }
            if (bKnown) continue;
            const bool bColor = FieldName == TEXT("Color");
            if (!bColor && FieldName != TEXT("VolumetricAlbedo"))
                return Fail(Error, Path + TEXT(".") + FieldName, TEXT("is not a supported profile field."));
            const TArray<TSharedPtr<FJsonValue>>* RGB = nullptr;
            if (!Pair.Value.IsValid() || !Pair.Value->TryGetArray(RGB) || RGB->Num() != 3)
                return Fail(Error, Path + TEXT(".") + FieldName, TEXT("must be [R,G,B], linear values from 0 to 1."));
            for (int32 I = 0; I < 3; ++I)
            {
                double Value = 0;
                if (!(*RGB)[I].IsValid() || (*RGB)[I]->Type != EJson::Number || !(*RGB)[I]->TryGetNumber(Value) || !FMath::IsFinite(Value) || Value < 0 || Value > 1)
                    return Fail(Error, Path + TEXT(".") + FieldName, TEXT("must contain finite values from 0 to 1."));
                Out.Set(EField((bColor ? ColorR : AlbedoR) + I), float(Value));
            }
        }
        return true;
    }

    TSharedRef<FJsonObject> ProfileJson(const FProfile& Values, const std::array<bool, FieldCount>* Mask = nullptr)
    {
        auto Json = MakeShared<FJsonObject>();
        for (const auto& Field : Fields)
            if (!Mask || (*Mask)[Field.Index]) Json->SetNumberField(Field.Name, Values[Field.Index]);
        for (const auto& Pair : {TPair<const TCHAR*, EField>(TEXT("Color"), ColorR),
            TPair<const TCHAR*, EField>(TEXT("VolumetricAlbedo"), AlbedoR)})
        {
            if (Mask && !(*Mask)[Pair.Value]) continue;
            TArray<TSharedPtr<FJsonValue>> RGB;
            for (int32 I = 0; I < 3; ++I) RGB.Add(MakeShared<FJsonValueNumber>(Values.Values[Pair.Value + I]));
            Json->SetArrayField(Pair.Key, RGB);
        }
        return Json;
    }
}

FV3DWorldFogConfig::FV3DWorldFogConfig()
{
    V3DFog::FPatch Rain, Snow;
    Rain.Set(V3DFog::Density, .0015f); Rain.Set(V3DFog::ExtinctionScale, 1.1f);
    Snow.Set(V3DFog::Density, .0025f); Snow.Set(V3DFog::ExtinctionScale, 1.3f);
    WeatherProfiles.Add(TEXT("clear"), V3DFog::FPatch());
    WeatherProfiles.Add(TEXT("rain"), Rain);
    WeatherProfiles.Add(TEXT("snow"), Snow);
}

V3DFog::FProfile FV3DWorldFogConfig::Resolve(FName Biome, FName Weather, float Intensity) const
{
    // The weather system uses 0..10 for particles. Fog reaches its authored profile at 1.
    static const FName Clear(TEXT("clear"));
    const FName Key = Weather.IsNone() ? Clear : Weather;
    return V3DFog::Resolve(Global, BiomeProfiles.Find(Biome), WeatherProfiles.Find(Key),
        Key == Clear ? 1.f : Intensity);
}

TSharedRef<FJsonObject> FV3DWorldFogConfig::ToJson() const
{
    auto Json = MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("Version"), 1);
    Json->SetNumberField(TEXT("TransitionSeconds"), TransitionSeconds);
    auto Base = ProfileJson(Global);
    Base->SetBoolField(TEXT("Enabled"), bEnabled);
    Base->SetBoolField(TEXT("Volumetric"), bVolumetric);
    Json->SetObjectField(TEXT("Global"), Base);
    auto WriteMap = [&Json](const TCHAR* Key, const TMap<FName, V3DFog::FPatch>& Map)
    {
        auto Object = MakeShared<FJsonObject>();
        // Stable output helps source control and repeatable archive builds.
        TArray<FName> Names; Map.GetKeys(Names);
        Names.Sort([](FName A, FName B) { return A.LexicalLess(B); });
        for (FName Name : Names)
        {
            const auto& Patch = Map.FindChecked(Name);
            Object->SetObjectField(Name.ToString(), ProfileJson(Patch.Values, &Patch.Overrides));
        }
        Json->SetObjectField(Key, Object);
    };
    WriteMap(TEXT("WeatherProfiles"), WeatherProfiles);
    WriteMap(TEXT("BiomeProfiles"), BiomeProfiles);
    return Json;
}

TSharedRef<FJsonObject> FV3DWorldFogConfig::ProfileTemplate()
{
    return ProfileJson(V3DFog::FProfile());
}

bool FV3DWorldFogConfig::Parse(const TSharedPtr<FJsonObject>& Json, FV3DWorldFogConfig& Out, FString& Error)
{
    Error.Reset();
    if (!Json.IsValid()) return Fail(Error, TEXT("root"), TEXT("must be an object."));
    for (const auto& Pair : Json->Values)
    {
        const FString FieldName(Pair.Key);
        if (FieldName != TEXT("Version") && FieldName != TEXT("TransitionSeconds") && FieldName != TEXT("Global")
            && FieldName != TEXT("WeatherProfiles") && FieldName != TEXT("BiomeProfiles"))
        {
            return Fail(Error, FieldName, TEXT("is not supported."));
        }
    }
    FV3DWorldFogConfig Parsed;
    double Version = 1, Seconds = Parsed.TransitionSeconds;
    if (Json->HasField(TEXT("Version")) && (!Json->HasTypedField<EJson::Number>(TEXT("Version"))
        || !Json->TryGetNumberField(TEXT("Version"), Version) || Version != 1))
        return Fail(Error, TEXT("Version"), TEXT("must be 1."));
    if (Json->HasField(TEXT("TransitionSeconds")) && (!Json->HasTypedField<EJson::Number>(TEXT("TransitionSeconds"))
        || !Json->TryGetNumberField(TEXT("TransitionSeconds"), Seconds)
        || !FMath::IsFinite(Seconds) || Seconds < 0 || Seconds > 120))
        return Fail(Error, TEXT("TransitionSeconds"), TEXT("must be between 0 and 120."));
    Parsed.TransitionSeconds = float(Seconds);
    if (Json->HasField(TEXT("Global")))
    {
        const TSharedPtr<FJsonObject>* Base = nullptr;
        if (!Json->TryGetObjectField(TEXT("Global"), Base)) return Fail(Error, TEXT("Global"), TEXT("must be an object."));
        V3DFog::FPatch Patch;
        if (!ParseProfile(*Base, TEXT("Global"), Patch, Error, true)) return false;
        for (const auto& Pair : {TPair<const TCHAR*, bool*>(TEXT("Enabled"), &Parsed.bEnabled),
            TPair<const TCHAR*, bool*>(TEXT("Volumetric"), &Parsed.bVolumetric)})
            if ((*Base)->HasField(Pair.Key) && (!(*Base)->HasTypedField<EJson::Boolean>(Pair.Key)
                || !(*Base)->TryGetBoolField(Pair.Key, *Pair.Value)))
                return Fail(Error, FString(TEXT("Global.")) + Pair.Key, TEXT("must be a boolean."));
        Parsed.Global = Patch.Apply(Parsed.Global);
    }
    auto ReadMap = [&](const TCHAR* Key, TMap<FName, V3DFog::FPatch>& Map, bool bWeather)
    {
        if (!Json->HasField(Key)) return true;
        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (!Json->TryGetObjectField(Key, Object) || (*Object)->Values.Num() > (bWeather ? 3 : 256))
            return Fail(Error, Key, TEXT("must be an object (max 3 weather / 256 biome profiles)."));
        Map.Empty();
        for (const auto& Pair : (*Object)->Values)
        {
            const FString ProfileName(Pair.Key);
            const FString Path = FString(Key) + TEXT(".") + ProfileName;
            if (ProfileName.IsEmpty() || ProfileName.Len() > 128 || ProfileName != ProfileName.TrimStartAndEnd())
                return Fail(Error, Path, TEXT("has an invalid name."));
            const FName Name(*ProfileName);
            if (Name.IsNone() || Map.Contains(Name)) return Fail(Error, Path, TEXT("has an empty or duplicate name."));
            if (bWeather && Name != TEXT("clear") && Name != TEXT("rain") && Name != TEXT("snow"))
                return Fail(Error, Path, TEXT("must be clear, rain, or snow."));
            const TSharedPtr<FJsonObject>* Profile = nullptr;
            if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(Profile)) return Fail(Error, Path, TEXT("must be an object."));
            V3DFog::FPatch Patch;
            if (!ParseProfile(*Profile, Path, Patch, Error)) return false;
            Map.Add(Name, Patch);
        }
        return true;
    };
    if (!ReadMap(TEXT("WeatherProfiles"), Parsed.WeatherProfiles, true)
        || !ReadMap(TEXT("BiomeProfiles"), Parsed.BiomeProfiles, false)) return false;
    Out = MoveTemp(Parsed);
    return true;
}

bool FV3DWorldFogConfig::ParseWorldConfig(const TSharedPtr<FJsonObject>& Json, FV3DWorldFogConfig& Out, FString& Error)
{
    Error.Reset();
    if (!Json.IsValid()) return Fail(Error, TEXT("world config"), TEXT("must be an object."));
    if (!Json->HasField(TEXT("Fog"))) { Out = FV3DWorldFogConfig(); return true; }
    const TSharedPtr<FJsonObject>* Fog = nullptr;
    if (!Json->TryGetObjectField(TEXT("Fog"), Fog)) return Fail(Error, TEXT("Fog snapshot"), TEXT("must be an object."));
    return Parse(*Fog, Out, Error);
}

bool FV3DWorldFogConfig::Load(const FString& ProjectRoot, FV3DWorldFogConfig& Out, FString& Error)
{
    FSafeJsonLimits Limits; Limits.MaxFileBytes = 1024 * 1024; Limits.MaxDepth = 8;
    Limits.bAllowBackupRecovery = false;
    const auto Loaded = FSafeFileIO::LoadJsonBlocking(FPaths::Combine(ProjectRoot, MemberName()), Limits);
    if (Loaded.Status == ESafeFileIOStatus::Missing) { Out = FV3DWorldFogConfig(); Error.Reset(); return true; }
    if (!Loaded.IsSuccess()) { Error = Loaded.Error; return false; }
    return Parse(Loaded.JsonObject, Out, Error);
}

bool FV3DWorldFogConfig::CreateDefaultFile(const FString& ProjectRoot, FString& Error)
{
    const FString Path = FPaths::Combine(ProjectRoot, MemberName());
    if (IFileManager::Get().FileExists(*Path)) return true;
    FV3DWorldFogConfig Config;
    if (!Load(ProjectRoot, Config, Error)) return false;
    if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true))
    { Error = TEXT("Could not create the project scripts directory."); return false; }
    const auto Saved = FSafeFileIO::SaveJsonBlocking(Config.ToJson(), Path);
    if (!Saved.IsSuccess()) { Error = Saved.Error; return false; }
    return true;
}
