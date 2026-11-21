// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "Model/MaterialDefaultAsset.h"
#include "Format/v3d.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"

FV3DMaterialSoftAssetReferences::FV3DMaterialSoftAssetReferences()
{
    ReservedMaterials.Add(TEXT("glass"),
        TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/V3DGenerated/Materials/M_Glass.M_Glass"))));
    ReservedMaterials.Add(TEXT("tinted_glass"),
        TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/V3DGenerated/Materials/M_TintedGlass.M_TintedGlass"))));
}

const TCHAR* FV3DMaterialAssetReferences::SlotName(int32 Slot)
{
    static const TCHAR* Names[] = {TEXT("MI_Opaque"), TEXT("MI_Opaque"), TEXT("MI_Translucent"), TEXT("MI_Toon"),
        TEXT("MI_OpaqueTwoSided"), TEXT("MI_OpaqueTwoSided"), TEXT("MI_TranslucentTwoSided"), TEXT("MI_ToonTwoSided")};
    return Slot >= 0 && Slot < SlotCount ? Names[Slot] : TEXT("InvalidSlot");
}
UMaterialInterface* FV3DMaterialAssetReferences::Get(int32 Slot) const
{
    UMaterialInterface* Slots[] = {MI_Opaque.Get(), MI_Opaque.Get(), MI_Translucent.Get(), MI_Toon.Get(),
        MI_OpaqueTwoSided.Get(), MI_OpaqueTwoSided.Get(), MI_TranslucentTwoSided.Get(), MI_ToonTwoSided.Get()};
    return Slot >= 0 && Slot < SlotCount ? Slots[Slot] : nullptr;
}
bool FV3DMaterialAssetReferences::IsSupportedParent(const UMaterialInterface* Material)
{
    return IsValid(Material) && Material->GetMaterial() && Material->GetMaterial()->MaterialDomain == MD_Surface
        && (Material->GetBlendMode() == BLEND_Masked || Material->GetBlendMode() == BLEND_Translucent);
}
bool FV3DMaterialAssetReferences::MatchesSlot(const UMaterialInterface* Material, int32 Slot)
{
    if (Slot < 0 || Slot >= SlotCount || !IsSupportedParent(Material) || Material->IsTwoSided() != (Slot >= 4)) return false;
    return Slot % 4 == 3 || Material->GetBlendMode() == (Slot % 4 == 2 ? BLEND_Translucent : BLEND_Masked);
}
FString FV3DMaterialAssetReferences::NormalizeMaterialName(const FString& Name)
{
    const FString Trimmed = Name.TrimStartAndEnd().ToLower();
    return Trimmed.Left(static_cast<int32>(v3d::exporter_stem_length(*Trimmed, Trimmed.Len())));
}
const TCHAR* FV3DMaterialAssetReferences::FallbackPath(bool bTwoSided)
{
    return bTwoSided ? TEXT("/Game/V3DGenerated/Materials/M_OpaqueTwoSided.M_OpaqueTwoSided")
        : TEXT("/Game/V3DGenerated/Materials/M_Opaque.M_Opaque");
}
UMaterialInterface* FV3DMaterialAssetReferences::LoadFallback(bool bTwoSided, bool bAllowSynchronousLoading)
{
    const TSoftObjectPtr<UMaterialInterface> Ref(FSoftObjectPath(FallbackPath(bTwoSided)));
    auto* Material = Ref.Get();
    if (!IsValid(Material) && bAllowSynchronousLoading) Material = Ref.LoadSynchronous();
    if (MatchesSlot(Material, bTwoSided ? 4 : 0)) return Material;
    // glTFRuntime ships these cooked Masked parents. This emergency path still keeps geometry
    // visible if an editor-generated asset was omitted from a package; it never selects Opaque blending.
    const TSoftObjectPtr<UMaterialInterface> Builtin(FSoftObjectPath(bTwoSided
        ? TEXT("/glTFRuntime/M_glTFRuntimeTwoSidedMasked_Inst.M_glTFRuntimeTwoSidedMasked_Inst")
        : TEXT("/glTFRuntime/M_glTFRuntimeMasked_Inst.M_glTFRuntimeMasked_Inst")));
    Material = Builtin.Get();
    if (!IsValid(Material) && bAllowSynchronousLoading) Material = Builtin.LoadSynchronous();
    return MatchesSlot(Material, bTwoSided ? 4 : 0) ? Material : nullptr;
}
bool FV3DMaterialAssetReferences::IsEmpty() const
{
    for (int32 Slot = 0; Slot < SlotCount; ++Slot) if (IsValid(Get(Slot))) return false;
    for (const auto& Pair : ReservedMaterials) if (IsValid(Pair.Value)) return false;
    return !IsValid(Terrain.Get()) && !IsValid(Foliage.Get());
}
const TSoftObjectPtr<UMaterialInterface>& FV3DMaterialSoftAssetReferences::Get(int32 Slot) const
{
    check(Slot >= 0 && Slot < FV3DMaterialAssetReferences::SlotCount);
    const TSoftObjectPtr<UMaterialInterface>* Slots[] = {&MI_Opaque, &MI_Opaque, &MI_Translucent, &MI_Toon,
        &MI_OpaqueTwoSided, &MI_OpaqueTwoSided, &MI_TranslucentTwoSided, &MI_ToonTwoSided};
    return *Slots[Slot];
}
bool FV3DMaterialSoftAssetReferences::Resolve(FV3DMaterialAssetReferences& OutReferences,
    TArray<FString>& OutFailures, bool bAllowSynchronousLoading) const
{
    OutReferences.Reset(); OutFailures.Reset();
    OutReferences.MaterialMode = v3d::normalize_material_mode(static_cast<uint8>(MaterialMode));
    if (!ensureMsgf(IsInGameThread(), TEXT("Material references must resolve on the game thread"))) return false;
    const auto Load = [bAllowSynchronousLoading](const TSoftObjectPtr<UMaterialInterface>& Ref)
    {
        UMaterialInterface* Material = Ref.Get();
        if (!Ref.IsNull() && !IsValid(Material) && bAllowSynchronousLoading) Material = Ref.LoadSynchronous();
        return Material;
    };
    TObjectPtr<UMaterialInterface>* Destinations[] = {&OutReferences.MI_Opaque, &OutReferences.MI_Masked,
        &OutReferences.MI_Translucent, &OutReferences.MI_Toon, &OutReferences.MI_OpaqueTwoSided,
        &OutReferences.MI_OpaqueTwoSided, &OutReferences.MI_TranslucentTwoSided, &OutReferences.MI_ToonTwoSided};
    for (int32 Slot : {0, 2, 3, 4, 6, 7})
    {
        const auto& Ref = Get(Slot);
        UMaterialInterface* Material = Load(Ref);
        if (FV3DMaterialAssetReferences::MatchesSlot(Material, Slot)) *Destinations[Slot] = Material;
        else if (!Ref.IsNull()) OutFailures.Add(FString::Printf(
            TEXT("%s -> %s: missing or incompatible blend/sidedness; using Opaque (Masked) fallback."),
            FV3DMaterialAssetReferences::SlotName(Slot), *Ref.ToSoftObjectPath().ToString()));
    }
    if (!OutReferences.MI_Opaque) OutReferences.MI_Opaque = FV3DMaterialAssetReferences::LoadFallback(false, bAllowSynchronousLoading);
    if (!OutReferences.MI_OpaqueTwoSided) OutReferences.MI_OpaqueTwoSided = FV3DMaterialAssetReferences::LoadFallback(true, bAllowSynchronousLoading);
    OutReferences.MI_Masked = OutReferences.MI_Opaque;
    const auto AddReserved = [&](const FString& Name, const TSoftObjectPtr<UMaterialInterface>& Ref)
    {
        const FString Key = FV3DMaterialAssetReferences::NormalizeMaterialName(Name);
        if (Key.IsEmpty()) return;
        auto* Material = Load(Ref);
        if (!FV3DMaterialAssetReferences::IsSupportedParent(Material))
        {
            if (!Ref.IsNull()) OutFailures.Add(FString::Printf(TEXT("Reserved material '%s' -> %s: unavailable or unsupported; using Opaque (Masked)."), *Key, *Ref.ToSoftObjectPath().ToString()));
            Material = nullptr;
        }
        OutReferences.ReservedMaterials.Add(Key, Material);
    };
    // Sorted keys make collisions after case/suffix normalization deterministic.
    TArray<FString> Keys; ReservedMaterials.GetKeys(Keys); Keys.Sort();
    for (const FString& Key : Keys) AddReserved(Key, ReservedMaterials.FindChecked(Key));
    return true;
}
void FV3DMaterialSoftAssetReferences::GetPreloadPaths(TArray<FSoftObjectPath>& Paths) const
{
    const auto Add = [&Paths](const TSoftObjectPtr<UMaterialInterface>& Ref)
    { if (!Ref.IsNull()) Paths.AddUnique(Ref.ToSoftObjectPath()); };
    for (int32 Slot : {0, 2, 3, 4, 6, 7}) Add(Get(Slot));
    for (const auto& Pair : ReservedMaterials) Add(Pair.Value);
    Paths.AddUnique(FSoftObjectPath(FV3DMaterialAssetReferences::FallbackPath(false)));
    Paths.AddUnique(FSoftObjectPath(FV3DMaterialAssetReferences::FallbackPath(true)));
    Paths.AddUnique(FSoftObjectPath(TEXT("/glTFRuntime/M_glTFRuntimeMasked_Inst.M_glTFRuntimeMasked_Inst")));
    Paths.AddUnique(FSoftObjectPath(TEXT("/glTFRuntime/M_glTFRuntimeTwoSidedMasked_Inst.M_glTFRuntimeTwoSidedMasked_Inst")));
}
int32 FV3DMaterialSoftAssetReferences::NumConfiguredReferences() const
{
    int32 Count = 0;
    for (int32 Slot : {0, 2, 3, 4, 6, 7}) if (!Get(Slot).IsNull()) ++Count;
    return Count;
}
