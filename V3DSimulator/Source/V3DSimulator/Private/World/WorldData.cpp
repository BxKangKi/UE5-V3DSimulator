// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldData.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "World/WorldData.h"
#include "System/MacroLibrary.h"
#include "System/JsonMetadata.h"
#include "System/ProjectConfig.h"

namespace WorldDataJson
{
    static void SetColor(const TSharedRef<FJsonObject>& Json, const FString& FieldName, const FLinearColor& Color)
    {
        TSharedRef<FJsonObject> ColorJson = MakeShared<FJsonObject>();
        ColorJson->SetNumberField(TEXT("R"), Color.R);
        ColorJson->SetNumberField(TEXT("G"), Color.G);
        ColorJson->SetNumberField(TEXT("B"), Color.B);
        ColorJson->SetNumberField(TEXT("A"), Color.A);
        Json->SetObjectField(FieldName, ColorJson);
    }

    static void TryGetColor(const TSharedPtr<FJsonObject>& Json, const FString& FieldName, FLinearColor& OutColor)
    {
        const TSharedPtr<FJsonObject>* ColorObject = nullptr;
        if (!Json.IsValid() || !Json->TryGetObjectField(FieldName, ColorObject) || !ColorObject || !ColorObject->IsValid())
        {
            return;
        }

        double R = OutColor.R;
        double G = OutColor.G;
        double B = OutColor.B;
        double A = OutColor.A;
        (*ColorObject)->TryGetNumberField(TEXT("R"), R);
        (*ColorObject)->TryGetNumberField(TEXT("G"), G);
        (*ColorObject)->TryGetNumberField(TEXT("B"), B);
        (*ColorObject)->TryGetNumberField(TEXT("A"), A);
        OutColor = FLinearColor(R, G, B, A);
    }
}

TSharedRef<FJsonObject> FLevelCloudSettings::ToJson() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetBoolField(TEXT("bEnabled"), bEnabled);
    Json->SetNumberField(TEXT("Coverage"), Coverage);
    Json->SetNumberField(TEXT("Density"), Density);
    Json->SetNumberField(TEXT("Opacity"), Opacity);
    Json->SetNumberField(TEXT("WindSpeed"), WindSpeed);
    WorldDataJson::SetColor(Json, TEXT("Tint"), Tint);
    return Json;
}

bool FLevelCloudSettings::FromJson(const TSharedPtr<FJsonObject>& Json)
{
    if (!Json.IsValid())
    {
        return false;
    }

    Json->TryGetBoolField(TEXT("bEnabled"), bEnabled);
    Json->TryGetNumberField(TEXT("Coverage"), Coverage);
    Json->TryGetNumberField(TEXT("Density"), Density);
    Json->TryGetNumberField(TEXT("Opacity"), Opacity);
    Json->TryGetNumberField(TEXT("WindSpeed"), WindSpeed);
    WorldDataJson::TryGetColor(Json, TEXT("Tint"), Tint);
    return true;
}

TSharedRef<FJsonObject> FLevelWeatherSettings::ToJson() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetBoolField(TEXT("bEnabled"), bEnabled);
    Json->SetStringField(TEXT("Preset"), Preset);
    Json->SetNumberField(TEXT("Intensity"), Intensity);
    Json->SetNumberField(TEXT("TickIntervalSeconds"), TickIntervalSeconds);
    Json->SetBoolField(TEXT("bAutoCycle"), bAutoCycle);
    Json->SetNumberField(TEXT("MinDurationTicks"), MinDurationTicks);
    Json->SetNumberField(TEXT("MaxDurationTicks"), MaxDurationTicks);
    Json->SetNumberField(TEXT("ClearWeight"), ClearWeight);
    Json->SetNumberField(TEXT("RainWeight"), RainWeight);
    Json->SetNumberField(TEXT("SnowWeight"), SnowWeight);
    return Json;
}

bool FLevelWeatherSettings::FromJson(const TSharedPtr<FJsonObject>& Json)
{
    if (!Json.IsValid())
    {
        return false;
    }

    Json->TryGetBoolField(TEXT("bEnabled"), bEnabled);
    Json->TryGetStringField(TEXT("Preset"), Preset);
    Json->TryGetNumberField(TEXT("Intensity"), Intensity);

    double LoadedTickInterval = TickIntervalSeconds;
    if (Json->TryGetNumberField(TEXT("TickIntervalSeconds"), LoadedTickInterval) && FMath::IsFinite(LoadedTickInterval))
    {
        TickIntervalSeconds = FMath::Clamp(static_cast<float>(LoadedTickInterval), 0.05f, 3600.0f);
    }

    Json->TryGetBoolField(TEXT("bAutoCycle"), bAutoCycle);

    double LoadedMinTicks = MinDurationTicks;
    if (Json->TryGetNumberField(TEXT("MinDurationTicks"), LoadedMinTicks) && FMath::IsFinite(LoadedMinTicks))
    {
        MinDurationTicks = FMath::Clamp(FMath::RoundToInt(LoadedMinTicks), 1, 100000000);
    }

    double LoadedMaxTicks = MaxDurationTicks;
    if (Json->TryGetNumberField(TEXT("MaxDurationTicks"), LoadedMaxTicks) && FMath::IsFinite(LoadedMaxTicks))
    {
        MaxDurationTicks = FMath::Clamp(FMath::RoundToInt(LoadedMaxTicks), MinDurationTicks, 100000000);
    }
    MaxDurationTicks = FMath::Max(MinDurationTicks, MaxDurationTicks);

    auto LoadWeight = [Json](const TCHAR* FieldName, float& OutValue)
    {
        double LoadedValue = OutValue;
        if (Json->TryGetNumberField(FieldName, LoadedValue) && FMath::IsFinite(LoadedValue))
        {
            OutValue = FMath::Clamp(static_cast<float>(LoadedValue), 0.0f, 1000000.0f);
        }
    };
    LoadWeight(TEXT("ClearWeight"), ClearWeight);
    LoadWeight(TEXT("RainWeight"), RainWeight);
    LoadWeight(TEXT("SnowWeight"), SnowWeight);
    Intensity = FMath::Clamp(FMath::IsFinite(Intensity) ? Intensity : 1.0f, 0.0f, 10.0f);
    return true;
}

TSharedRef<FJsonObject> FLevelGameplaySettings::ToJson() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("WorldGameMode"), WorldGameMode);
    Json->SetBoolField(TEXT("bCheatsEnabled"), bCheatsEnabled);
    Json->SetNumberField(TEXT("PlayerMaxHealth"), FMath::Max(1.0f, PlayerMaxHealth));
    Json->SetNumberField(TEXT("PlayerMassKg"), FMath::Clamp(PlayerMassKg, 1.0f, 10000.0f));
    Json->SetNumberField(TEXT("PlayerPushTractionCoefficient"), FMath::Clamp(PlayerPushTractionCoefficient, 0.0f, 2.0f));
    return Json;
}

bool FLevelGameplaySettings::FromJson(const TSharedPtr<FJsonObject>& Json)
{
    if (!Json.IsValid())
    {
        return false;
    }

    Json->TryGetStringField(TEXT("WorldGameMode"), WorldGameMode);
    Json->TryGetBoolField(TEXT("bCheatsEnabled"), bCheatsEnabled);
    double LoadedMaxHealth = PlayerMaxHealth;
    if (Json->TryGetNumberField(TEXT("PlayerMaxHealth"), LoadedMaxHealth) && FMath::IsFinite(LoadedMaxHealth))
    {
        PlayerMaxHealth = FMath::Clamp(static_cast<float>(LoadedMaxHealth), 1.0f, 1000000000.0f);
    }

    double LoadedMassKg = PlayerMassKg;
    if (Json->TryGetNumberField(TEXT("PlayerMassKg"), LoadedMassKg) && FMath::IsFinite(LoadedMassKg))
    {
        PlayerMassKg = FMath::Clamp(static_cast<float>(LoadedMassKg), 1.0f, 10000.0f);
    }

    double LoadedTraction = PlayerPushTractionCoefficient;
    if (Json->TryGetNumberField(TEXT("PlayerPushTractionCoefficient"), LoadedTraction) && FMath::IsFinite(LoadedTraction))
    {
        PlayerPushTractionCoefficient = FMath::Clamp(static_cast<float>(LoadedTraction), 0.0f, 2.0f);
    }
    return true;
}

UWorldData::UWorldData()
{
    Version = V3DSimulatorJsonMetadata::SchemaVersion;
    WorldName = TEXT("New World");
    Latitude = 38.0f;
    Longitude = 127.0f;
    AxialTilt = 23.5f;
    OneYearDays = 365.0f;
    OneDayTime = 24.0f * 60.0f * 60.0f;
    // A new .dat starts at local noon. With no star-dome dependency, midnight is correctly
    // almost black and was easily mistaken for a missing WorldEnvManager during the first Play.
    WorldTime = OneDayTime * 0.5f;
    TimeSpeed = 60.0f;
    bOcean = false;
    OceanHeightCm = 0.0;
    bAllowProjectAssets = false;
    PlayerLocation = FVector::ZeroVector;
    // An empty value means that no external character has been selected yet. The previous
    // "Player" placeholder was indistinguishable from a real filename and blocked first-run GLBs.
    Player.Reset();
}

bool UWorldData::DeserializeData(UWorldData *Data, TSharedPtr<FJsonObject> Json)
{
    if (Json.IsValid())
    {
        Data->Version = V3DSimulatorJsonMetadata::SchemaVersion;
        Json->TryGetStringField(V3DSimulatorJsonMetadata::Version, Data->Version);
        Json->TryGetStringField(V3DSimulatorJsonMetadata::WorldName, Data->WorldName);
        Json->TryGetNumberField(LATITUDE, Data->Latitude);
        Json->TryGetNumberField(LONGITUDE, Data->Longitude);
        Json->TryGetNumberField(AXIAL_TILT, Data->AxialTilt);
        Json->TryGetNumberField(ONE_YEAR_DAYS, Data->OneYearDays);
        Json->TryGetNumberField(ONE_DAY_TIME, Data->OneDayTime);
        Json->TryGetNumberField(TIME_SPEED, Data->TimeSpeed);
        Json->TryGetBoolField(OCEAN, Data->bOcean);
        double LoadedOceanHeight = Data->OceanHeightCm;
        if (Json->TryGetNumberField(TEXT("OceanHeightCm"), LoadedOceanHeight) && FMath::IsFinite(LoadedOceanHeight))
        {
            Data->OceanHeightCm = LoadedOceanHeight;
        }
        Json->TryGetBoolField(V3DSimulatorJsonMetadata::AllowProjectAssets, Data->bAllowProjectAssets);

        const TSharedPtr<FJsonObject>* CloudObject = nullptr;
        if (Json->TryGetObjectField(TEXT("Cloud"), CloudObject) && CloudObject && CloudObject->IsValid())
        {
            Data->Cloud.FromJson(*CloudObject);
        }

        const TSharedPtr<FJsonObject>* WeatherObject = nullptr;
        if (Json->TryGetObjectField(TEXT("Weather"), WeatherObject) && WeatherObject && WeatherObject->IsValid())
        {
            Data->Weather.FromJson(*WeatherObject);
        }

        const TSharedPtr<FJsonObject>* GameplayObject = nullptr;
        if (Json->TryGetObjectField(TEXT("Gameplay"), GameplayObject) && GameplayObject && GameplayObject->IsValid())
        {
            Data->Gameplay.FromJson(*GameplayObject);
        }
        return true;
    }
    return false;
}
