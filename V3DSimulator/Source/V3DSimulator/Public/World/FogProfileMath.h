// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>

// Value-only optics and transition math. No engine objects, allocation, or mutable global state.
namespace V3DFog
{
    enum EField : std::size_t
    {
        Density, HeightFalloff, StartDistanceCm, MaxOpacity,
        ColorR, ColorG, ColorB, ScatteringDistribution, ExtinctionScale,
        ViewDistanceCm, AlbedoR, AlbedoG, AlbedoB, FieldCount
    };

    struct FProfile
    {
        std::array<float, FieldCount> Values = {
            .0005f, .2f, 2000.f, .75f, .025f, .035f, .048f, .25f, .8f,
            8000.f, .78f, .82f, .86f
        };
        float& operator[](EField Field) { return Values[Field]; }
        float operator[](EField Field) const { return Values[Field]; }
        bool operator==(const FProfile& Other) const { return Values == Other.Values; }
        bool operator!=(const FProfile& Other) const { return !(*this == Other); }
    };

    struct FPatch
    {
        FProfile Values;
        std::array<bool, FieldCount> Overrides{};
        void Set(EField Field, float Value) { Values[Field] = Value; Overrides[Field] = true; }
        FProfile Apply(FProfile Base) const
        {
            for (std::size_t I = 0; I < FieldCount; ++I)
                if (Overrides[I]) Base.Values[I] = Values.Values[I];
            return Base;
        }
    };

    inline float Unit(float Value)
    {
        return !std::isfinite(Value) ? 0.f : (Value < 0.f ? 0.f : (Value > 1.f ? 1.f : Value));
    }

    inline float SmoothStep01(float Edge0, float Edge1, float Value)
    {
        if (!std::isfinite(Value) || Edge1 <= Edge0) return 0.f;
        const float T = Unit((Value - Edge0) / (Edge1 - Edge0));
        return T * T * (3.f - 2.f * T);
    }

    struct FDayNightLighting
    {
        std::array<float, 3> InscatteringScale = {1.f, 1.f, 1.f};
        std::array<float, 3> DirectionalScale = {.35f, .35f, .35f};
    };

    /**
     * Computes realistic day/twilight/night RGB multipliers for height fog inscattering
     * from solar and lunar elevation sines (-1..1).
     * Daytime brightens fog naturally with sky luminance; sunrise/sunset adds warm horizon
     * scatter; nighttime darkens fog deeply with subtle cool moonlight fill.
     */
    inline FDayNightLighting EvaluateDayNightLighting(float SunElevationSin, float MoonElevationSin)
    {
        const float SafeSun = std::isfinite(SunElevationSin) ? SunElevationSin : 0.5f;
        const float SafeMoon = std::isfinite(MoonElevationSin) ? MoonElevationSin : -1.f;

        const float DayFactor = SmoothStep01(-0.06f, 0.26f, SafeSun);
        const float HighNoonBoost = SmoothStep01(0.15f, 0.75f, SafeSun);
        const float NightFactor = 1.f - SmoothStep01(-0.08f, 0.10f, SafeSun);
        const float MoonFill = SmoothStep01(-0.02f, 0.25f, SafeMoon);
        const float TwilightFactor = SmoothStep01(-0.12f, 0.01f, SafeSun)
            * (1.f - SmoothStep01(0.02f, 0.24f, SafeSun));

        // Base ambient sky brightness: dark at night (0.045..0.115 with moonlight), bright by day (1.0..1.55).
        const float NightLuminance = NightFactor * (0.045f + 0.070f * MoonFill);
        const float DayLuminance = DayFactor * (1.00f + 0.55f * HighNoonBoost);

        // Combine cool moonlit night, warm golden twilight, and crisp daylight tints.
        FDayNightLighting Out;
        Out.InscatteringScale[0] = NightLuminance * 0.72f + TwilightFactor * 0.58f + DayLuminance * 1.00f;
        Out.InscatteringScale[1] = NightLuminance * 0.84f + TwilightFactor * 0.34f + DayLuminance * 1.02f;
        Out.InscatteringScale[2] = NightLuminance * 1.15f + TwilightFactor * 0.20f + DayLuminance * 1.06f;

        // Directional inscattering follows the active celestial light (warm sun vs faint cool moon).
        const float DirDay = DayFactor * 0.45f;
        const float DirTwilight = TwilightFactor * 0.35f;
        const float DirMoon = NightFactor * MoonFill * 0.035f;
        Out.DirectionalScale[0] = DirMoon * 0.75f + DirTwilight * 1.35f + DirDay * 1.00f;
        Out.DirectionalScale[1] = DirMoon * 0.88f + DirTwilight * 0.82f + DirDay * 0.98f;
        Out.DirectionalScale[2] = DirMoon * 1.15f + DirTwilight * 0.48f + DirDay * 0.94f;
        return Out;
    }

    inline FProfile Blend(const FProfile& From, const FProfile& To, float Weight)
    {
        const float T = Unit(Weight);
        if (T == 0.f) return From;
        if (T == 1.f) return To;
        FProfile Result;
        for (std::size_t I = 0; I < FieldCount; ++I)
            Result.Values[I] = From.Values[I] + (To.Values[I] - From.Values[I]) * T;
        return Result;
    }

    // Global is immutable. Omitted fields inherit; weather overrides only its authored fields.
    inline FProfile Resolve(const FProfile& Global, const FPatch* Biome,
        const FPatch* Weather, float WeatherWeight)
    {
        const FProfile Base = Biome ? Biome->Apply(Global) : Global;
        return Weather ? Blend(Base, Weather->Apply(Base), WeatherWeight) : Base;
    }

    struct FTransition
    {
        FProfile Current, Start, Target;
        float Elapsed = 0.f;
        bool Initialized = false;

        void Reset() { *this = FTransition(); }
        const FProfile& Update(const FProfile& Next, float DeltaSeconds, float Duration)
        {
            if (!Initialized || !std::isfinite(Duration) || Duration <= 0.f)
            {
                Current = Start = Target = Next;
                Elapsed = 0.f;
                Initialized = true;
                return Current;
            }
            if (Next != Target)
            {
                Start = Current; Target = Next; Elapsed = 0.f;
            }
            if (Current == Target) return Current;
            if (std::isfinite(DeltaSeconds) && DeltaSeconds > 0.f)
                Elapsed = DeltaSeconds >= Duration - Elapsed ? Duration : Elapsed + DeltaSeconds;
            const float T = Unit(Elapsed / Duration);
            Current = Blend(Start, Target, T * T * (3.f - 2.f * T));
            return Current;
        }
    };
}
