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
