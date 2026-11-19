// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "Model/MaterialDefaultAsset.h"
#include "Format/material_policy.hpp"
#include "Materials/MaterialInterface.h"

const TCHAR* FV3DMaterialAssetReferences::SlotName(int32 Slot)
{
    static const TCHAR* Names[] = {TEXT("MI_Opaque"), TEXT("MI_Masked"), TEXT("MI_Translucent"), TEXT("MI_Toon")};
    return Slot >= 0 && Slot < 4 ? Names[Slot] : TEXT("InvalidSlot");
}

UMaterialInterface* FV3DMaterialAssetReferences::Get(int32 Slot) const
{
    UMaterialInterface* Slots[] = {MI_Opaque.Get(), MI_Masked.Get(), MI_Translucent.Get(), MI_Toon.Get()};
    return Slot >= 0 && Slot < 4 ? Slots[Slot] : nullptr;
}

bool FV3DMaterialAssetReferences::MatchesSlot(const UMaterialInterface* Material, int32 Slot)
{
    if (!IsValid(Material) || Material->IsTwoSided() || Slot < 0 || Slot > 3) return false;
    const EBlendMode Blend = Material->GetBlendMode();
    // A single Toon parent keeps its authored alpha mode; no runtime BlendMode override.
    if (Slot == 3) return Blend == BLEND_Opaque || Blend == BLEND_Masked || Blend == BLEND_Translucent;
    const EBlendMode Expected[] = {BLEND_Opaque, BLEND_Masked, BLEND_Translucent};
    return Blend == Expected[Slot];
}

bool FV3DMaterialAssetReferences::IsEmpty() const
{
    for (int32 Slot = 0; Slot < 4; ++Slot)
        if (IsValid(Get(Slot))) return false;
    return !IsValid(Terrain.Get());
}

const TSoftObjectPtr<UMaterialInterface>& FV3DMaterialSoftAssetReferences::Get(int32 Slot) const
{
    check(Slot >= 0 && Slot < 4);
    const TSoftObjectPtr<UMaterialInterface>* Slots[] = {&MI_Opaque, &MI_Masked, &MI_Translucent, &MI_Toon};
    return *Slots[Slot];
}

bool FV3DMaterialSoftAssetReferences::Resolve(FV3DMaterialAssetReferences& OutReferences,
    TArray<FString>& OutFailures, bool bAllowSynchronousLoading) const
{
    OutReferences.Reset(); OutFailures.Reset();
    OutReferences.MaterialMode = v3d::normalize_material_mode(static_cast<uint8>(MaterialMode));
    if (!ensureMsgf(IsInGameThread(), TEXT("Material references must resolve on the game thread"))) return false;
    TObjectPtr<UMaterialInterface>* Destinations[] = {&OutReferences.MI_Opaque, &OutReferences.MI_Masked, &OutReferences.MI_Translucent, &OutReferences.MI_Toon};
    for (int32 Slot = 0; Slot < 4; ++Slot)
    {
        const auto& Reference = Get(Slot);
        // Unused slots need not block world bootstrap. Missing required slots fail at use.
        if (Reference.IsNull()) continue;
        UMaterialInterface* Material = Reference.Get();
        if (!IsValid(Material) && bAllowSynchronousLoading) Material = Reference.LoadSynchronous();
        const TCHAR* Name = FV3DMaterialAssetReferences::SlotName(Slot);
        if (!IsValid(Material))
            OutFailures.Add(FString::Printf(TEXT("%s -> %s (not loaded)"), Name, *Reference.ToSoftObjectPath().ToString()));
        else if (!FV3DMaterialAssetReferences::MatchesSlot(Material, Slot))
            OutFailures.Add(FString::Printf(TEXT("%s -> %s: incompatible BlendMode or Two Sided enabled. Assign an existing matching single-sided parent."), Name, *Material->GetPathName()));
        else *Destinations[Slot] = Material;
    }
    return true;
}

void FV3DMaterialSoftAssetReferences::GetPreloadPaths(TArray<FSoftObjectPath>& Paths) const
{
    for (int32 Slot = 0; Slot < 4; ++Slot)
    {
        const auto& Reference = Get(Slot);
        if (!Reference.IsNull()) Paths.AddUnique(Reference.ToSoftObjectPath());
    }
}

int32 FV3DMaterialSoftAssetReferences::NumConfiguredReferences() const
{
    int32 Count = 0;
    for (int32 Slot = 0; Slot < 4; ++Slot)
        if (!Get(Slot).IsNull()) ++Count;
    return Count;
}
