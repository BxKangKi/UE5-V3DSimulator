// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/V3DTerrain.h"
#include "Async/ParallelFor.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "Materials/MaterialInterface.h"
#include "Math/Float16Color.h"
#include "Misc/ConfigCacheIni.h"
#include "PixelFormat.h"
#include "Setting/GameSettings.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/V3DSimulatorGameInstance.h"
#include "glTFRuntimeAsset.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#if WITH_EDITOR
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "UObject/FindObjectFlags.h"
#include "UObject/UObjectHash.h"
#endif
#endif

namespace V3DTerrain
{
    bool IsTerrainName(const FString& Name)
    {
        return Name.EndsWith(TEXT(".terrain"), ESearchCase::CaseSensitive);
    }
    bool SourceHasTerrain(UglTFRuntimeAsset* Source)
    {
        if (!Source || !Source->GetParser().IsValid()) return false;
        for (const auto& Material : Source->GetParser()->GetMaterials())
        {
            FString Name;
            if (Material->TryGetStringField(TEXT("name"), Name) && IsTerrainName(Name)) return true;
        }
        return false;
    }
    bool IsPaletteMaterial(const FGWorldBakedMaterial& Material)
    {
        for (const auto& Scalar : Material.Scalars)
            if (Scalar.Name == TEXT("V3D_Terrain") && Scalar.Value == 1.0f) return true;
        return false;
    }

    bool IsCompatibleMaterial(UMaterialInterface* Material)
    {
        if (!IsInGameThread() || IsGarbageCollecting() || !IsValid(Material)) return false;
        for (const TCHAR* Name : {TEXT("V3D_BaseColorArray"), TEXT("V3D_NormalArray"),
                TEXT("V3D_ORMArray"), TEXT("V3D_EmissiveArray")})
        {
            UTexture* Default = nullptr;
            if (!Material->GetTextureParameterValue(FMaterialParameterInfo(FName(Name)), Default)
                || !Cast<UTexture2DArray>(Default)) return false;
        }
        return true;
    }

    namespace Private
    {
        constexpr int32 Layers = 4;
        struct FLayer
        {
            int32 MaterialId = INDEX_NONE;
            FLinearColor BaseFactor = FLinearColor::White;
            FLinearColor EmissiveFactor = FLinearColor::Black;
            float Metallic = 1, Roughness = 1, NormalScale = 1, Occlusion = 1;
            const FGWorldBakedTexture* Base = nullptr;
            const FGWorldBakedTexture* Normal = nullptr;
            const FGWorldBakedTexture* MR = nullptr;
            const FGWorldBakedTexture* AO = nullptr;
            const FGWorldBakedTexture* Emissive = nullptr;
        };
        // Terrain reads authored images, not the current MID's defaults. A custom PBR
        // parent can omit a parameter, and glTFRuntime leaves neutral defaults in place
        // when a source image fails to load. Neither is a valid source for the palette.
        struct FSourceTextureCache
        {
            UglTFRuntimeAsset* Source = nullptr;
            int32 MaxSize = 1024;
            // Heap-owned values keep FLayer pointers stable as the cache grows.
            TMap<int64, TUniquePtr<FGWorldBakedTexture>> Textures;
            bool Resolve(int32 TextureIndex, bool bSRGB,
                const FGWorldBakedTexture*& Out, FString& Error);
        };
        float Number(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, float Default)
        {
            double Value = Default;
            if (Object.IsValid()) Object->TryGetNumberField(Name, Value);
            return static_cast<float>(Value);
        }
        TSharedPtr<FJsonObject> Object(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name)
        {
            const TSharedPtr<FJsonObject>* Result = nullptr;
            return Parent.IsValid() && Parent->TryGetObjectField(Name, Result) ? *Result : nullptr;
        }
        FLinearColor Factor(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name,
            const FLinearColor Default, const int32 Count)
        {
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (!Parent.IsValid() || !Parent->TryGetArrayField(Name, Values) || Values->Num() != Count)
                return Default;
            FLinearColor Result = Default;
            for (int32 I = 0; I < Count; ++I) Result.Component(I) = static_cast<float>((*Values)[I]->AsNumber());
            return Result;
        }
        bool FiniteColor(const FLinearColor& C)
        {
            return FMath::IsFinite(C.R) && FMath::IsFinite(C.G)
                && FMath::IsFinite(C.B) && FMath::IsFinite(C.A);
        }
        bool Unit(const float X) { return FMath::IsFinite(X) && X >= 0 && X <= 1; }
        bool ObjectField(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name, FString& Error)
        {
            if (!Parent.IsValid() || !Parent->HasField(Name) || Object(Parent, Name).IsValid()) return true;
            Error = FString::Printf(TEXT("Terrain %s must be a JSON object"), Name);
            return false;
        }
        bool NumberField(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name,
            const double Min, const double Max, FString& Error)
        {
            if (!Parent.IsValid() || !Parent->HasField(Name)) return true;
            double Value = 0;
            if (Parent->HasTypedField<EJson::Number>(Name) && Parent->TryGetNumberField(Name, Value)
                && FMath::IsFinite(Value) && Value >= Min && Value <= Max)
                return true;
            Error = FString::Printf(TEXT("Terrain %s must be a finite number in [%g, %g]"), Name, Min, Max);
            return false;
        }
        bool FactorField(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name,
            const int32 Count, const double Min, const double Max, FString& Error)
        {
            if (!Parent.IsValid() || !Parent->HasField(Name)) return true;
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (Parent->TryGetArrayField(Name, Values) && Values->Num() == Count)
            {
                bool bValid = true;
                for (const auto& Entry : *Values)
                {
                    double Value = 0;
                    bValid &= Entry.IsValid() && Entry->Type == EJson::Number && Entry->TryGetNumber(Value)
                        && FMath::IsFinite(Value) && Value >= Min && Value <= Max;
                }
                if (bValid) return true;
            }
            Error = FString::Printf(TEXT("Terrain %s must contain %d finite numeric components in [%g, %g]"),
                Name, Count, Min, Max);
            return false;
        }
        bool IndexField(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name,
            const int32 Default, int32& Out, FString& Error)
        {
            Out = Default;
            if (!Parent.IsValid() || !Parent->HasField(Name)) return true;
            double Value = 0;
            // Validate before narrowing: truncating a fractional or huge JSON index can select
            // the wrong image, or invoke an out-of-range floating-point to integer conversion.
            if (Parent->HasTypedField<EJson::Number>(Name) && Parent->TryGetNumberField(Name, Value) && FMath::IsFinite(Value)
                && Value >= 0 && Value <= MAX_int32 && FMath::FloorToDouble(Value) == Value)
            {
                Out = static_cast<int32>(Value);
                return true;
            }
            Error = FString::Printf(TEXT("Terrain %s must be a nonnegative integer index"), Name);
            return false;
        }
        bool TextureReference(const TSharedPtr<FJsonObject>& Info, const TSharedPtr<FJsonObject>& Root,
            FString& Error)
        {
            if (!Info.IsValid()) return true;
            int32 Index = INDEX_NONE, Coordinate = 0;
            if (!IndexField(Info, TEXT("index"), INDEX_NONE, Index, Error)
                || !IndexField(Info, TEXT("texCoord"), 0, Coordinate, Error)
                || !ObjectField(Info, TEXT("extensions"), Error)) return false;
            const TArray<TSharedPtr<FJsonValue>>* Textures = nullptr;
            if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("textures"), Textures)
                || !Textures->IsValidIndex(Index) || !(*Textures)[Index].IsValid()
                || (*Textures)[Index]->Type != EJson::Object)
            { Error = TEXT("Terrain glTF texture index is invalid"); return false; }
            const auto Texture = (*Textures)[Index]->AsObject();
            int32 SamplerIndex = INDEX_NONE;
            if (!IndexField(Texture, TEXT("sampler"), INDEX_NONE, SamplerIndex, Error)) return false;
            if (SamplerIndex != INDEX_NONE)
            {
                const TArray<TSharedPtr<FJsonValue>>* Samplers = nullptr;
                if (!Root->TryGetArrayField(TEXT("samplers"), Samplers) || !Samplers->IsValidIndex(SamplerIndex)
                    || !(*Samplers)[SamplerIndex].IsValid() || (*Samplers)[SamplerIndex]->Type != EJson::Object)
                { Error = TEXT("Terrain glTF sampler index is invalid"); return false; }
                const auto Sampler = (*Samplers)[SamplerIndex]->AsObject();
                int32 Ignored = 0;
                for (const TCHAR* Field : {TEXT("wrapS"), TEXT("wrapT"), TEXT("minFilter"), TEXT("magFilter")})
                    if (!IndexField(Sampler, Field, 0, Ignored, Error)) return false;
                for (const TCHAR* Field : {TEXT("wrapS"), TEXT("wrapT")})
                {
                    const float Wrap = Number(Sampler, Field, 10497);
                    if (Wrap != 10497 && Wrap != 33071 && Wrap != 33648)
                    { Error = TEXT("Terrain glTF sampler has an invalid wrapping mode"); return false; }
                }
                const float Min = Number(Sampler, TEXT("minFilter"), 9987);
                const float Mag = Number(Sampler, TEXT("magFilter"), 9729);
                if ((Min != 9728 && Min != 9729 && Min != 9984 && Min != 9985 && Min != 9986 && Min != 9987)
                    || (Mag != 9728 && Mag != 9729))
                { Error = TEXT("Terrain glTF sampler has an invalid filtering mode"); return false; }
            }
            const auto Extensions = Object(Info, TEXT("extensions"));
            if (Extensions.IsValid())
            {
                for (const auto& Pair : Extensions->Values)
                    if (!ObjectField(Extensions, *Pair.Key, Error)) return false;
                const auto Transform = Object(Extensions, TEXT("KHR_texture_transform"));
                int32 Ignored = 0;
                if (!FactorField(Transform, TEXT("offset"), 2, -MAX_flt, MAX_flt, Error)
                    || !FactorField(Transform, TEXT("scale"), 2, -MAX_flt, MAX_flt, Error)
                    || !NumberField(Transform, TEXT("rotation"), -MAX_flt, MAX_flt, Error)
                    || !IndexField(Transform, TEXT("texCoord"), 0, Ignored, Error)) return false;
            }
            return true;
        }
        bool SourceImageIndex(const TSharedPtr<FJsonObject>& Root, const int32 TextureIndex,
            int32& ImageIndex, FString& Error)
        {
            ImageIndex = INDEX_NONE;
            const TArray<TSharedPtr<FJsonValue>>* TextureRows = nullptr;
            if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("textures"), TextureRows)
                || !TextureRows->IsValidIndex(TextureIndex) || !(*TextureRows)[TextureIndex].IsValid()
                || (*TextureRows)[TextureIndex]->Type != EJson::Object)
            { Error = TEXT("source texture row is missing or invalid"); return false; }
            const auto Texture = (*TextureRows)[TextureIndex]->AsObject();
            if (!IndexField(Texture, TEXT("source"), INDEX_NONE, ImageIndex, Error)) return false;
            if (ImageIndex == INDEX_NONE)
            {
                if (!ObjectField(Texture, TEXT("extensions"), Error)) return false;
                const auto Extensions = Object(Texture, TEXT("extensions"));
                for (const TCHAR* Name : {TEXT("KHR_texture_basisu"), TEXT("EXT_texture_webp"), TEXT("MSFT_texture_dds")})
                {
                    if (!ObjectField(Extensions, Name, Error)) return false;
                    const auto Extension = Object(Extensions, Name);
                    if (!Extension.IsValid()) continue;
                    if (!IndexField(Extension, TEXT("source"), INDEX_NONE, ImageIndex, Error)) return false;
                    if (ImageIndex != INDEX_NONE) break;
                }
            }
            const TArray<TSharedPtr<FJsonValue>>* Images = nullptr;
            if (!Root->TryGetArrayField(TEXT("images"), Images) || !Images->IsValidIndex(ImageIndex)
                || !(*Images)[ImageIndex].IsValid() || (*Images)[ImageIndex]->Type != EJson::Object)
            { Error = TEXT("source image index is missing or outside the images array"); return false; }
            return true;
        }
        bool MaterialExtensions(const TSharedPtr<FJsonObject>& Json, const TSharedPtr<FJsonObject>& Root,
            FString& FallbackReason, FString& Error)
        {
            FallbackReason.Reset();
            if (!ObjectField(Json, TEXT("extensions"), Error)) return false;
            const auto Extensions = Object(Json, TEXT("extensions"));
            if (!Extensions.IsValid()) return true;
            TArray<FString> Unsupported;
            for (const auto& Pair : Extensions->Values)
            {
                if (!ObjectField(Extensions, *Pair.Key, Error)) return false;
                const auto Extension = Object(Extensions, *Pair.Key);
                if (Pair.Key == TEXT("KHR_materials_emissive_strength"))
                {
                    if (!NumberField(Extension, TEXT("emissiveStrength"), 0, MAX_flt, Error)) return false;
                    continue;
                }
                if (Pair.Key == TEXT("KHR_materials_clearcoat"))
                {
                    if (!NumberField(Extension, TEXT("clearcoatFactor"), 0, 1, Error)
                        || !NumberField(Extension, TEXT("clearcoatRoughnessFactor"), 0, 1, Error)) return false;
                    for (const TCHAR* Field : {TEXT("clearcoatTexture"), TEXT("clearcoatRoughnessTexture"), TEXT("clearcoatNormalTexture")})
                        if (!ObjectField(Extension, Field, Error)
                            || !TextureReference(Object(Extension, Field), Root, Error)) return false;
                    if (!NumberField(Object(Extension, TEXT("clearcoatNormalTexture")), TEXT("scale"), 0, MAX_flt, Error)) return false;
                    // .terrain explicitly opts into the vertex-weight palette. Ignore the coat
                    // regardless of its factor: keeping the original shader skips PaintWeights
                    // for every layer and produces hard material boundaries. Other glTF
                    // materials retain their clearcoat; malformed source data still fails above.
                    continue;
                }
                // Validate the standard extension fields decoded by glTFRuntime as well. A
                // fallback changes the shader choice, never the validity rules for source data.
                TArray<const TCHAR*> TextureFields;
                // The .terrain suffix selects one opaque/default-lit palette contract. Known
                // additional lighting lobes cannot select another parent and disable weights.
                bool bTerrainLightingLobe = true;
                if (Pair.Key == TEXT("KHR_materials_transmission"))
                {
                    if (!NumberField(Extension,TEXT("transmissionFactor"),0,1,Error)) return false;
                    TextureFields = {TEXT("transmissionTexture")};
                }
                else if (Pair.Key == TEXT("KHR_materials_ior"))
                {
                    if (!NumberField(Extension,TEXT("ior"),1,MAX_flt,Error)) return false;
                }
                else if (Pair.Key == TEXT("KHR_materials_specular"))
                {
                    if (!NumberField(Extension,TEXT("specularFactor"),0,1,Error)
                        || !FactorField(Extension,TEXT("specularColorFactor"),3,0,1,Error)) return false;
                    TextureFields = {TEXT("specularTexture"),TEXT("specularColorTexture")};
                }
                else if (Pair.Key == TEXT("KHR_materials_sheen"))
                {
                    if (!NumberField(Extension,TEXT("sheenRoughnessFactor"),0,1,Error)
                        || !FactorField(Extension,TEXT("sheenColorFactor"),3,0,1,Error)) return false;
                    TextureFields = {TEXT("sheenColorTexture"),TEXT("sheenRoughnessTexture")};
                }
                else if (Pair.Key == TEXT("KHR_materials_volume"))
                {
                    if (!NumberField(Extension,TEXT("thicknessFactor"),0,MAX_flt,Error)
                        || !NumberField(Extension,TEXT("attenuationDistance"),0,MAX_flt,Error)
                        || !FactorField(Extension,TEXT("attenuationColor"),3,0,1,Error)) return false;
                    if (Extension->HasField(TEXT("attenuationDistance"))
                        && Number(Extension,TEXT("attenuationDistance"),1) <= 0)
                    { Error = TEXT("Terrain attenuationDistance must be positive"); return false; }
                    TextureFields = {TEXT("thicknessTexture")};
                }
                else if (Pair.Key == TEXT("KHR_materials_iridescence"))
                {
                    if (!NumberField(Extension,TEXT("iridescenceFactor"),0,1,Error)
                        || !NumberField(Extension,TEXT("iridescenceIor"),1,MAX_flt,Error)
                        || !NumberField(Extension,TEXT("iridescenceThicknessMinimum"),0,MAX_flt,Error)
                        || !NumberField(Extension,TEXT("iridescenceThicknessMaximum"),0,MAX_flt,Error)) return false;
                    if (Number(Extension,TEXT("iridescenceThicknessMinimum"),100)
                        > Number(Extension,TEXT("iridescenceThicknessMaximum"),400))
                    { Error = TEXT("Terrain iridescence minimum thickness exceeds its maximum"); return false; }
                    TextureFields = {TEXT("iridescenceTexture"),TEXT("iridescenceThicknessTexture")};
                }
                else if (Pair.Key == TEXT("KHR_materials_unlit"))
                {
                    // Terrain is intentionally lit, even if the source exporter marked it unlit.
                }
                else if (Pair.Key == TEXT("KHR_materials_pbrSpecularGlossiness"))
                {
                    if (!FactorField(Extension,TEXT("diffuseFactor"),4,0,1,Error)
                        || !FactorField(Extension,TEXT("specularFactor"),3,0,1,Error)
                        || !NumberField(Extension,TEXT("glossinessFactor"),0,1,Error)) return false;
                    TextureFields = {TEXT("diffuseTexture"),TEXT("specularGlossinessTexture")};
                    // Legacy diffuse/glossiness has different base texture semantics; do not
                    // silently bake white in place of a source diffuse map.
                    bTerrainLightingLobe = false;
                }
                else
                {
                    bTerrainLightingLobe = false;
                }
                for (const TCHAR* Field : TextureFields)
                    if (!ObjectField(Extension,Field,Error)
                        || !TextureReference(Object(Extension,Field),Root,Error)) return false;
                if (!bTerrainLightingLobe) Unsupported.Add(FString(Pair.Key));
            }
            if (!Unsupported.IsEmpty())
            {
                Unsupported.Sort();
                FallbackReason = FString::Printf(TEXT("material extensions not represented by the terrain shader: %s"),
                    *FString::Join(Unsupported, TEXT(", ")));
            }
            return true;
        }
        bool TextureInfo(const TSharedPtr<FJsonObject>& Info,
            const FGWorldBakedMaterial& Material, const TCHAR* Parameter,
            const FGWorldBakedModel& Model, const TSharedPtr<FJsonObject>& Root,
            const FGWorldBakedTexture*& Out, FString& Error,
            FSourceTextureCache* SourceTextures = nullptr)
        {
            Out = nullptr;
            if (!Info.IsValid()) return true;
            if (!TextureReference(Info, Root, Error)) return false;
            if (Number(Info, TEXT("texCoord"), 0) != 0)
            { Error = TEXT("Terrain textures currently require TEXCOORD_0"); return false; }
            const auto Extensions = Object(Info, TEXT("extensions"));
            if (Extensions.IsValid())
            {
                for (const auto& Pair : Extensions->Values)
                    if (Pair.Key != TEXT("KHR_texture_transform"))
                    { Error = TEXT("Terrain texture has an unsupported extension"); return false; }
                const auto Transform = Object(Extensions, TEXT("KHR_texture_transform"));
                if (Transform.IsValid())
                {
                    const FLinearColor Offset = Factor(Transform, TEXT("offset"), FLinearColor(0,0,0,0), 2);
                    const FLinearColor Scale = Factor(Transform, TEXT("scale"), FLinearColor(1,1,0,0), 2);
                    if (Number(Transform, TEXT("rotation"), 0) != 0 || Number(Transform, TEXT("texCoord"), 0) != 0
                        || Offset.R != 0 || Offset.G != 0 || Scale.R != 1 || Scale.G != 1)
                    { Error = TEXT("Terrain currently requires identity texture transforms and UV0"); return false; }
                }
            }
            // All palette slices share the repeat/trilinear sampler selected by BuildArray.
            // TextureReference already validates the source sampler's enums, indices and types.
            // Accept every valid source mode: baking resamples its pixels and rebuilds the mip
            // chain for the terrain contract instead of requiring matching exporter settings.
            if (SourceTextures)
            {
                int32 TextureIndex = INDEX_NONE;
                if (!IndexField(Info, TEXT("index"), INDEX_NONE, TextureIndex, Error)) return false;
                const bool bColor = FCString::Strcmp(Parameter, TEXT("baseColorTexture")) == 0
                    || FCString::Strcmp(Parameter, TEXT("emissiveTexture")) == 0;
                if (!SourceTextures->Resolve(TextureIndex, bColor, Out, Error))
                {
                    Error = FString::Printf(TEXT("Terrain %s, %s, glTF texture %d: %s"),
                        *Material.Name, Parameter, TextureIndex, *Error);
                    return false;
                }
            }
            else
            {
                for (const auto& P : Material.Textures)
                    if (P.Name == Parameter && Model.Textures.IsValidIndex(P.TextureId))
                    {
                        Out = &Model.Textures[P.TextureId];
                        break;
                    }
            }
            if (!Out)
            { Error = FString::Printf(TEXT("Terrain %s requires embedded decoded texture parameter %s; use the glTFRuntime PBR parent when baking"), *Material.Name, Parameter); return false; }
            if (Out->NumLayers != 1 || Out->Mips.IsEmpty()
                || (Out->PixelFormat != PF_B8G8R8A8 && Out->PixelFormat != PF_R8G8B8A8
                    && Out->PixelFormat != PF_FloatRGBA && Out->PixelFormat != PF_G8))
            { Error = TEXT("Terrain sources must decode to uncompressed RGBA8/BGRA8/RGBA16F/R8 (DDS/BC source pixels need decoding before baking)"); return false; }
            const auto& Mip = Out->Mips[0];
            const int32 BytesPerPixel = Out->PixelFormat == PF_FloatRGBA ? sizeof(FFloat16Color)
                : Out->PixelFormat == PF_G8 ? 1 : 4;
            if (Out->SizeX <= 0 || Out->SizeY <= 0 || Mip.SizeX != Out->SizeX || Mip.SizeY != Out->SizeY
                || Mip.SizeZ != 1 || int64(Mip.SizeX) * Mip.SizeY > MAX_int32 / BytesPerPixel
                || int64(Mip.SizeX) * Mip.SizeY * BytesPerPixel != Mip.Bytes.Num())
            { Error = TEXT("Terrain source mip dimensions or pixel byte count are invalid"); return false; }
            return true;
        }
        bool ReadLayer(const TSharedPtr<FJsonObject>& Json, const TSharedPtr<FJsonObject>& Root,
            const FGWorldBakedMaterial& Material, const FGWorldBakedModel& Model, FLayer& Layer,
            FString& FallbackReason, FString& Error, const bool bForArray = true,
            FSourceTextureCache* SourceTextures = nullptr)
        {
            if (!Json.IsValid() || !Root.IsValid())
            { Error = TEXT("Terrain bake needs valid source material JSON"); return false; }
            FString AlphaMode = TEXT("OPAQUE");
            bool TwoSided = false;
            if ((Json->HasField(TEXT("alphaMode")) && (!Json->HasTypedField<EJson::String>(TEXT("alphaMode"))
                    || !Json->TryGetStringField(TEXT("alphaMode"), AlphaMode)))
                || (AlphaMode != TEXT("OPAQUE") && AlphaMode != TEXT("MASK") && AlphaMode != TEXT("BLEND"))
                || (Json->HasField(TEXT("doubleSided")) && (!Json->HasTypedField<EJson::Boolean>(TEXT("doubleSided"))
                    || !Json->TryGetBoolField(TEXT("doubleSided"), TwoSided))))
            { Error = TEXT("Terrain alphaMode or doubleSided has an invalid glTF value"); return false; }
            if (!ObjectField(Json, TEXT("pbrMetallicRoughness"), Error)
                || !ObjectField(Json, TEXT("normalTexture"), Error)
                || !ObjectField(Json, TEXT("occlusionTexture"), Error)
                || !ObjectField(Json, TEXT("emissiveTexture"), Error)
                || !MaterialExtensions(Json, Root, FallbackReason, Error)) return false;
            // The .terrain suffix explicitly selects the opaque, single-sided, default-lit
            // palette. Valid exporter alpha/doubleSided flags cannot switch shader families
            // and suppress its vertex weights; malformed values are still rejected above.
            const auto Extensions = Object(Json, TEXT("extensions"));
            const auto PBR = Object(Json, TEXT("pbrMetallicRoughness"));
            const auto NormalInfo = Object(Json, TEXT("normalTexture"));
            const auto AOInfo = Object(Json, TEXT("occlusionTexture"));
            if (!FactorField(PBR, TEXT("baseColorFactor"), 4, 0, 1, Error)
                || !FactorField(Json, TEXT("emissiveFactor"), 3, 0, 1, Error)
                || !NumberField(PBR, TEXT("metallicFactor"), 0, 1, Error)
                || !NumberField(PBR, TEXT("roughnessFactor"), 0, 1, Error)
                || !NumberField(NormalInfo, TEXT("scale"), 0, MAX_flt, Error)
                || !NumberField(AOInfo, TEXT("strength"), 0, 1, Error)
                || !NumberField(Json, TEXT("alphaCutoff"), 0, MAX_flt, Error)
                || !ObjectField(PBR, TEXT("baseColorTexture"), Error)
                || !ObjectField(PBR, TEXT("metallicRoughnessTexture"), Error)) return false;
            for (const auto& Info : {Object(PBR, TEXT("baseColorTexture")), Object(PBR, TEXT("metallicRoughnessTexture")),
                    NormalInfo, AOInfo, Object(Json, TEXT("emissiveTexture"))})
                if (!TextureReference(Info, Root, Error)) return false;
            Layer.MaterialId = Material.MaterialId;
            Layer.BaseFactor = Factor(PBR, TEXT("baseColorFactor"), FLinearColor::White, 4);
            Layer.Metallic = Number(PBR, TEXT("metallicFactor"), 1);
            Layer.Roughness = Number(PBR, TEXT("roughnessFactor"), 1);
            Layer.EmissiveFactor = Factor(Json, TEXT("emissiveFactor"), FLinearColor::Black, 3);
            const float Strength = Number(Object(Extensions, TEXT("KHR_materials_emissive_strength")), TEXT("emissiveStrength"), 1);
            Layer.EmissiveFactor *= Strength;
            // glTF emission has three components; strength must not amplify the unused alpha
            // and spuriously reject a black material with a large, otherwise valid strength.
            Layer.EmissiveFactor.A = 1;
            Layer.NormalScale = Number(NormalInfo, TEXT("scale"), 1);
            Layer.Occlusion = Number(AOInfo, TEXT("strength"), 1);
            if (!Unit(Layer.BaseFactor.R) || !Unit(Layer.BaseFactor.G) || !Unit(Layer.BaseFactor.B)
                || !Unit(Layer.BaseFactor.A) || !Unit(Layer.Metallic) || !Unit(Layer.Roughness)
                || !Unit(Layer.Occlusion) || !FMath::IsFinite(Layer.NormalScale)
                || Layer.NormalScale < 0 || !FiniteColor(Layer.EmissiveFactor)
                || Layer.EmissiveFactor.GetMin() < 0 || Layer.EmissiveFactor.GetMax() > 65504)
            { Error = TEXT("Terrain PBR factors are invalid or exceed the RGBA16F emissive range"); return false; }
            // Unknown shader contracts and legacy diffuse/glossiness retain their captured
            // parent. Extra lighting lobes and surface flags do not suppress terrain weights.
            if (!FallbackReason.IsEmpty() || !bForArray) return true;
            return TextureInfo(Object(PBR, TEXT("baseColorTexture")), Material, TEXT("baseColorTexture"), Model, Root, Layer.Base, Error, SourceTextures)
                && TextureInfo(Object(PBR, TEXT("metallicRoughnessTexture")), Material, TEXT("metallicRoughnessTexture"), Model, Root, Layer.MR, Error, SourceTextures)
                && TextureInfo(NormalInfo, Material, TEXT("normalTexture"), Model, Root, Layer.Normal, Error, SourceTextures)
                && TextureInfo(AOInfo, Material, TEXT("occlusionTexture"), Model, Root, Layer.AO, Error, SourceTextures)
                && TextureInfo(Object(Json, TEXT("emissiveTexture")), Material, TEXT("emissiveTexture"), Model, Root, Layer.Emissive, Error, SourceTextures);
        }
        FLinearColor PixelBytes(const uint8* Data, const int32 PixelFormat,
            const int32 Width, const int32 Height, int32 X, int32 Y, const bool SRGB)
        {
            // One remainder suffices; adding the extent unconditionally can overflow for
            // otherwise valid very wide R8 inputs and wastes a second integer division.
            X %= Width; if (X < 0) X += Width;
            Y %= Height; if (Y < 0) Y += Height;
            const int64 Index = int64(Y) * Width + X;
            if (PixelFormat == PF_FloatRGBA)
            {
                FFloat16Color Value;
                FMemory::Memcpy(&Value, Data + Index * sizeof(Value), sizeof(Value));
                return Value.GetFloats();
            }
            if (PixelFormat == PF_G8)
            {
                const uint8 Value = Data[Index];
                const FColor Color(Value, Value, Value, 255);
                return SRGB ? FLinearColor::FromSRGBColor(Color) : Color.ReinterpretAsLinear();
            }
            const uint8* Bytes = Data + Index * 4;
            const FColor Color(PixelFormat == PF_B8G8R8A8 ? Bytes[2] : Bytes[0], Bytes[1],
                PixelFormat == PF_B8G8R8A8 ? Bytes[0] : Bytes[2], Bytes[3]);
            return SRGB ? FLinearColor::FromSRGBColor(Color) : Color.ReinterpretAsLinear();
        }
        FLinearColor Pixel(const FGWorldBakedTexture& Texture, int32 X, int32 Y, const bool SRGB)
        {
            const auto& Mip = Texture.Mips[0];
            return PixelBytes(Mip.Bytes.GetData(), Texture.PixelFormat, Mip.SizeX, Mip.SizeY, X, Y, SRGB);
        }
        FLinearColor Sample(const FGWorldBakedTexture* Texture, const float U, const float V,
            const bool SRGB, const FLinearColor Default)
        {
            if (!Texture) return Default;
            const float X = U * Texture->Mips[0].SizeX - .5f, Y = V * Texture->Mips[0].SizeY - .5f;
            const int32 X0 = FMath::FloorToInt(X), Y0 = FMath::FloorToInt(Y);
            return FMath::Lerp(FMath::Lerp(Pixel(*Texture, X0, Y0, SRGB), Pixel(*Texture, X0+1, Y0, SRGB), X-X0),
                FMath::Lerp(Pixel(*Texture, X0, Y0+1, SRGB), Pixel(*Texture, X0+1, Y0+1, SRGB), X-X0), Y-Y0);
        }
        FLinearColor NormalizeNormal(const FLinearColor C)
        {
            FVector3f N(C.R * 2 - 1, C.G * 2 - 1, C.B * 2 - 1);
            if (!N.Normalize()) N = FVector3f(0,0,1);
            return FLinearColor(N.X * .5f + .5f, N.Y * .5f + .5f, N.Z * .5f + .5f, 1);
        }
        template<typename Function>
        void PixelRows(const int32 RowCount, const int32 Width, const bool bAllowParallel, const Function& Body)
        {
            // Workers read immutable pixels/factors and own complete destination rows. Small
            // mips stay serial, avoiding task overhead and preserving the same per-pixel order.
            const bool bLarge = int64(RowCount) * Width >= 65536;
            ParallelFor(RowCount, Body, bAllowParallel && bLarge
                ? EParallelForFlags::None : EParallelForFlags::ForceSingleThread);
        }
        bool FSourceTextureCache::Resolve(const int32 TextureIndex, const bool bSRGB,
            const FGWorldBakedTexture*& Out, FString& Error)
        {
            Out = nullptr;
            if (TextureIndex < 0)
            { Error = TEXT("source texture index is invalid"); return false; }
            const int64 Key = int64(TextureIndex) * 2 + (bSRGB ? 1 : 0);
            if (const auto* Existing = Textures.Find(Key))
            {
                Out = Existing->Get();
                return true;
            }
            if (!Source || !Source->GetParser().IsValid())
            { Error = TEXT("source parser is unavailable during image decode"); return false; }
            FglTFRuntimeMaterialsConfig Config;
            Config.CacheMode = EglTFRuntimeCacheMode::None;
            Config.bGeneratesMipMaps = false;
            Config.bLoadMipMaps = false;
            Config.ImagesConfig.bCompressMips = false;
            Config.ImagesConfig.bStreaming = false;
            Config.ImagesConfig.bSRGB = bSRGB;
            Config.ImagesConfig.MaxWidth = MaxSize;
            Config.ImagesConfig.MaxHeight = MaxSize;
            TArray<FglTFRuntimeMipMap> Mips;
            FglTFRuntimeTextureSampler Sampler;
            // LoadTexture returns either a cached GPU texture OR newly decoded CPU mips.
            // Material CacheMode=None does not invalidate textures built during mesh capture.
            // Cached GPU data can also contain irreversible platform normal-map swizzles, so
            // recover source pixels through LoadImage instead of reading back that texture.
            UTexture2D* CachedTexture = Source->GetParser()->LoadTexture(TextureIndex, Mips, bSRGB, Config, Sampler);
            if (Mips.IsEmpty())
            {
                int32 ImageIndex = INDEX_NONE;
                if (!SourceImageIndex(Source->GetParser()->GetJsonRoot(), TextureIndex, ImageIndex, Error)) return false;
                Mips.Emplace(TextureIndex);
                auto& Image = Mips.Last();
                const int32 PreviousErrors = Source->GetParser()->GetErrors().Num();
                if (!Source->GetParser()->LoadImage(ImageIndex, Image.Pixels,
                        Image.Width, Image.Height, Image.PixelFormat, Config.ImagesConfig)
                    || Image.Pixels.IsEmpty())
                {
                    const auto& ParserErrors = Source->GetParser()->GetErrors();
                    TArray<FString> Details;
                    for (int32 I = FMath::Max(PreviousErrors, ParserErrors.Num() - 4); I < ParserErrors.Num(); ++I)
                        Details.Add(ParserErrors[I].Left(512));
                    Error = FString::Printf(TEXT("source image %d could not be decoded (texture cache %s). %s"),
                        ImageIndex, IsValid(CachedTexture) ? TEXT("hit") : TEXT("miss"),
                        Details.IsEmpty() ? TEXT("Check its image bytes/URI and image-format decoder.") : *FString::Join(Details,TEXT("; ")));
                    return false;
                }
            }
            const auto& SourceMip = Mips[0];
            const int32 Format = SourceMip.PixelFormat;
            const int32 Stride = Format == PF_FloatRGBA ? sizeof(FFloat16Color) : Format == PF_G8 ? 1 : 4;
            if (Format != PF_B8G8R8A8 && Format != PF_R8G8B8A8 && Format != PF_FloatRGBA && Format != PF_G8)
            {
                Error = FString::Printf(TEXT("image decoder returned pixel format %d; terrain needs decoded RGBA8/BGRA8/RGBA16F/R8 pixels"), Format);
                return false;
            }
            if (SourceMip.Width <= 0 || SourceMip.Height <= 0
                || int64(SourceMip.Width) * SourceMip.Height > MAX_int32 / Stride
                || int64(SourceMip.Width) * SourceMip.Height * Stride != SourceMip.Pixels.Num())
            { Error = TEXT("decoded image dimensions or byte count are invalid or exceed the pixel safety limit"); return false; }

            auto Texture = MakeUnique<FGWorldBakedTexture>();
            Texture->Name = FString::Printf(TEXT("glTF texture %d"), TextureIndex);
            Texture->bSRGB = bSRGB;
            Texture->PixelFormat = Format;
            int32 Cap = 1;
            while (Cap <= MaxSize / 2) Cap *= 2;
            Texture->SizeX = FMath::Min(SourceMip.Width, Cap);
            Texture->SizeY = FMath::Min(SourceMip.Height, Cap);
            auto& Mip = Texture->Mips.AddDefaulted_GetRef();
            Mip.SizeX = Texture->SizeX; Mip.SizeY = Texture->SizeY; Mip.SizeZ = 1;
            if (SourceMip.Width == Mip.SizeX && SourceMip.Height == Mip.SizeY)
            {
                Mip.Bytes.Append(SourceMip.Pixels.GetData(), static_cast<int32>(SourceMip.Pixels.Num()));
            }
            else
            {
                // Enforce the terrain cap here even on plugin versions that resize only in
                // BuildTexture. Only the capped copy survives after this one decode finishes.
                Texture->PixelFormat = Format == PF_FloatRGBA ? PF_FloatRGBA : PF_R8G8B8A8;
                const int32 OutputStride = Format == PF_FloatRGBA ? sizeof(FFloat16Color) : 4;
                Mip.Bytes.SetNumUninitialized(Mip.SizeX * Mip.SizeY * OutputStride);
                PixelRows(Mip.SizeY, Mip.SizeX, true, [&](const int32 Y)
                {
                    const float V = (Y + .5f) * SourceMip.Height / Mip.SizeY - .5f;
                    const int32 Y0 = FMath::FloorToInt(V);
                    for (int32 X = 0; X < Mip.SizeX; ++X)
                    {
                        const float U = (X + .5f) * SourceMip.Width / Mip.SizeX - .5f;
                        const int32 X0 = FMath::FloorToInt(U);
                        const auto At = [&](const int32 SX, const int32 SY)
                        {
                            return PixelBytes(SourceMip.Pixels.GetData(), Format,
                                SourceMip.Width, SourceMip.Height, SX, SY, bSRGB);
                        };
                        const FLinearColor C = FMath::Lerp(FMath::Lerp(At(X0,Y0),At(X0+1,Y0),U-X0),
                            FMath::Lerp(At(X0,Y0+1),At(X0+1,Y0+1),U-X0),V-Y0);
                        uint8* Bytes = Mip.Bytes.GetData() + (Y * Mip.SizeX + X) * OutputStride;
                        if (Format == PF_FloatRGBA)
                        {
                            const FFloat16Color Half(C);
                            FMemory::Memcpy(Bytes, &Half, sizeof(Half));
                        }
                        else
                        {
                            const FColor Color = C.ToFColor(bSRGB);
                            Bytes[0] = Color.R; Bytes[1] = Color.G; Bytes[2] = Color.B; Bytes[3] = Color.A;
                        }
                    }
                });
            }
            Out = Texture.Get();
            Textures.Add(Key, MoveTemp(Texture));
            return true;
        }
        void AppendMip(FGWorldBakedTexture& Texture, const TArray<FLinearColor>& Pixels,
            int32 Width, int32 Height, const bool bAllowParallel)
        {
            auto& Mip = Texture.Mips.AddDefaulted_GetRef();
            Mip.SizeX = Width; Mip.SizeY = Height; Mip.SizeZ = Layers;
            const bool Half = Texture.PixelFormat == PF_FloatRGBA;
            Mip.Bytes.SetNumUninitialized(Pixels.Num() * (Half ? sizeof(FFloat16Color) : 4));
            PixelRows(Height * Layers, Width, bAllowParallel, [&](const int32 Row)
            {
                for (int32 I = Row * Width; I < (Row + 1) * Width; ++I)
                {
                    if (Half)
                    {
                        const FFloat16Color Color(Pixels[I]);
                        FMemory::Memcpy(Mip.Bytes.GetData() + int64(I) * sizeof(Color), &Color, sizeof(Color));
                    }
                    else
                    {
                        const FColor Color = Pixels[I].ToFColor(Texture.bSRGB);
                        Mip.Bytes[I*4] = Color.R; Mip.Bytes[I*4+1] = Color.G;
                        Mip.Bytes[I*4+2] = Color.B; Mip.Bytes[I*4+3] = Color.A;
                    }
                }
            });
        }
        bool BuildArray(const TArray<FLayer>& Palette, const int32 Semantic, const int32 MaxSize,
            const bool FlipGreen, FGWorldBakedTexture& Texture, FString& Error, const bool bAllowParallel = true)
        {
            int32 Width = 1, Height = 1;
            for (const auto& L : Palette)
            {
                const FGWorldBakedTexture* Source = Semantic == 0 ? L.Base : Semantic == 1 ? L.Normal : Semantic == 2 ? L.MR : L.Emissive;
                if (Source) { Width = FMath::Max(Width, Source->SizeX); Height = FMath::Max(Height, Source->SizeY); }
                if (Semantic == 2 && L.AO) { Width = FMath::Max(Width, L.AO->SizeX); Height = FMath::Max(Height, L.AO->SizeY); }
            }
            // Power-of-two array extents give every mip a complete 2x2 reduction footprint.
            const auto Extent = [MaxSize](int32 SourceSize)
            {
                int32 Result = 1;
                while (Result < SourceSize && Result <= MaxSize / 2) Result *= 2;
                return Result;
            };
            Width = Extent(Width); Height = Extent(Height);
            Texture.SizeX = Width; Texture.SizeY = Height; Texture.NumLayers = Layers; Texture.bTextureArray = true;
            Texture.PixelFormat = Semantic == 3 ? PF_FloatRGBA : PF_R8G8B8A8;
            Texture.AddressX = TA_Wrap; Texture.AddressY = TA_Wrap; Texture.Filter = TF_Trilinear;
            Texture.LODGroup = TEXTUREGROUP_World; Texture.bSRGB = Semantic == 0;
            TArray<FLinearColor> Pixels; Pixels.SetNumUninitialized(Width * Height * Layers);
            TArray<uint8> InvalidRows; InvalidRows.SetNumZeroed(Height * Layers);
            PixelRows(Height * Layers, Width, bAllowParallel, [&](const int32 Row)
            {
                const int32 Slice = Row / Height, Y = Row % Height;
                for (int32 X = 0; X < Width; ++X)
                {
                    FLinearColor C = Semantic == 1 ? FLinearColor(.5f,.5f,1,1)
                        : Semantic == 2 ? FLinearColor(1,1,0,1)
                        : Semantic == 3 ? FLinearColor::Black : FLinearColor::White;
                    if (Palette.IsValidIndex(Slice))
                    {
                        const FLayer& L = Palette[Slice];
                        const float U = (X + .5f) / Width, V = (Y + .5f) / Height;
                        if (Semantic == 0) C = Sample(L.Base,U,V,true,FLinearColor::White) * L.BaseFactor;
                        else if (Semantic == 1)
                        {
                            C = Sample(L.Normal,U,V,false,FLinearColor(.5f,.5f,1,1));
                            C.R = (C.R * 2 - 1) * L.NormalScale * .5f + .5f;
                            C.G = (C.G * 2 - 1) * L.NormalScale * (FlipGreen ? -.5f : .5f) + .5f;
                            C = NormalizeNormal(C);
                        }
                        else if (Semantic == 2)
                        {
                            const FLinearColor MR = Sample(L.MR,U,V,false,FLinearColor::White);
                            const float AO = Sample(L.AO,U,V,false,FLinearColor::White).R;
                            C = FLinearColor(FMath::Lerp(1.f,AO,L.Occlusion), MR.G * L.Roughness, MR.B * L.Metallic, 1);
                        }
                        else C = Sample(L.Emissive,U,V,true,FLinearColor::White) * L.EmissiveFactor;
                    }
                    if (!FiniteColor(C) || C.GetMin() < 0 || C.GetMax() > 65504)
                    { InvalidRows[Row] = 1; return; }
                    Pixels[(Slice*Height+Y)*Width+X] = C;
                }
            });
            // Error reporting remains on the calling thread; workers never race on FString.
            if (InvalidRows.Contains(1))
            { Error = TEXT("Terrain resampled pixel exceeds supported finite RGBA16F range"); return false; }
            while (true)
            {
                AppendMip(Texture, Pixels, Width, Height, bAllowParallel);
                if (Width == 1 && Height == 1) break;
                const int32 NewW = FMath::Max(1, Width/2), NewH = FMath::Max(1, Height/2);
                TArray<FLinearColor> Next; Next.SetNumUninitialized(NewW * NewH * Layers);
                PixelRows(NewH * Layers, NewW, bAllowParallel, [&](const int32 Row)
                {
                    const int32 Slice = Row / NewH, Y = Row % NewH;
                    for (int32 X=0; X<NewW; ++X)
                    {
                        FLinearColor C(0,0,0,0);
                        for (int32 DY=0; DY<2; ++DY) for (int32 DX=0; DX<2; ++DX)
                            C += Pixels[(Slice*Height+FMath::Min(Y*2+DY,Height-1))*Width+FMath::Min(X*2+DX,Width-1)] * .25f;
                        Next[(Slice*NewH+Y)*NewW+X] = Semantic == 1 ? NormalizeNormal(C) : C;
                    }
                });
                Pixels = MoveTemp(Next); Width = NewW; Height = NewH;
            }
            return true;
        }
        FVector3f PositionKey(const FVector3f& Position, const FTransform* Transform)
        {
            FVector3f Key = Transform ? FVector3f(Transform->TransformPosition(FVector(Position))) : Position;
            // FVector equality treats signed zero alike; normalize before its bit-based hash.
            if (Key.X == 0) Key.X = 0.f;
            if (Key.Y == 0) Key.Y = 0.f;
            if (Key.Z == 0) Key.Z = 0.f;
            return Key;
        }
        bool PaintWeights(FGWorldBakedModel& Model, const TArray<int32>& MaterialIds,
            const int32 PaletteId, FString& Error)
        {
            if (MaterialIds.Num() > Layers)
            { Error = TEXT("Terrain blending supports at most four .terrain materials per model; split the model into palettes"); return false; }
            // Validate the complete input before changing any mesh. A bad later mesh must not
            // leave earlier meshes pointing at a palette that was never committed.
            for (const auto& Mesh : Model.Meshes)
            {
                for (int32 PrimitiveIndex=0; PrimitiveIndex<Mesh.Primitives.Num(); ++PrimitiveIndex)
                {
                    const auto& P = Mesh.Primitives[PrimitiveIndex];
                    if (!MaterialIds.Contains(P.MaterialId)) continue;
                    if (P.Mode != 4 || !P.Joints.IsEmpty() || !P.MorphTargets.IsEmpty()
                        || P.Positions.IsEmpty() || P.UVs.IsEmpty()
                        || P.UVs[0].Num() != P.Positions.Num())
                    { Error = TEXT("Terrain requires unskinned triangle meshes with complete UV0 and no morph targets"); return false; }
                    const bool Transformed = Mesh.AdditionalTransforms.Num() == Mesh.Primitives.Num();
                    for (const auto& Position : P.Positions)
                    {
                        const FVector3f Key = PositionKey(Position, Transformed ? &Mesh.AdditionalTransforms[PrimitiveIndex] : nullptr);
                        if (!FMath::IsFinite(Key.X) || !FMath::IsFinite(Key.Y) || !FMath::IsFinite(Key.Z))
                        { Error = TEXT("Terrain transformed position exceeds the finite float32 range"); return false; }
                    }
                }
            }
            for (auto& Mesh : Model.Meshes)
            {
                TMap<FVector3f,uint8> Masks;
                for (int32 PrimitiveIndex=0; PrimitiveIndex<Mesh.Primitives.Num(); ++PrimitiveIndex)
                {
                    const auto& P = Mesh.Primitives[PrimitiveIndex];
                    const int32 Layer = MaterialIds.Find(P.MaterialId);
                    if (Layer == INDEX_NONE) continue;
                    const bool Transformed = Mesh.AdditionalTransforms.Num() == Mesh.Primitives.Num();
                    for (const auto& Position : P.Positions)
                    {
                        const FVector3f Key = PositionKey(Position, Transformed ? &Mesh.AdditionalTransforms[PrimitiveIndex] : nullptr);
                        Masks.FindOrAdd(Key) |= uint8(1u << Layer);
                    }
                }
                for (int32 PrimitiveIndex=0; PrimitiveIndex<Mesh.Primitives.Num(); ++PrimitiveIndex)
                {
                    auto& P = Mesh.Primitives[PrimitiveIndex];
                    const int32 Layer = MaterialIds.Find(P.MaterialId);
                    if (Layer == INDEX_NONE) continue;
                    P.Colors.SetNumUninitialized(P.Positions.Num());
                    const bool Transformed = Mesh.AdditionalTransforms.Num() == Mesh.Primitives.Num();
                    for (int32 I=0; I<P.Positions.Num(); ++I)
                    {
                        const FVector3f Key = PositionKey(P.Positions[I], Transformed ? &Mesh.AdditionalTransforms[PrimitiveIndex] : nullptr);
                        const uint8 Mask = Masks.FindChecked(Key);
                        FVector4f W(0,0,0,0); float Sum=0;
                        for (int32 J=0; J<Layers; ++J) { W[J] = (Mask & (1u<<J)) ? 1.f : 0.f; Sum += W[J]; }
                        P.Colors[I] = W / Sum;
                    }
                    Mesh.bHasVertexColors = true;
                    P.MaterialId = PaletteId; P.MaterialName = TEXT("V3D.TerrainPalette"); P.bHasMaterial = true;
                }
            }
            return true;
        }
    }

    FString ResolveMaterialPath(const UObject* WorldContextObject)
    {
        if (IsInGameThread() && !IsGarbageCollecting())
        {
            if (const UV3DSimulatorAssetRegistry* Registry =
                    UV3DSimulatorGameInstance::GetAssetRegistryFromContext(WorldContextObject))
            {
                const FString RegistryPath =
                    Registry->TerrainMaterial.ToSoftObjectPath().ToString();
                if (!RegistryPath.IsEmpty())
                {
                    return RegistryPath;
                }
            }
        }

        FString MaterialPath = DefaultMaterialPath;
        if (GConfig)
        {
            GConfig->GetString(TEXT("V3D.Terrain"), TEXT("Material"), MaterialPath, GGameIni);
        }
        return MaterialPath;
    }

    UMaterialInterface* ResolveMaterial(const UObject* WorldContextObject, const FString& ArchiveMaterialPath)
    {
        if (!IsInGameThread() || IsGarbageCollecting())
        {
            return nullptr;
        }

        if (const UV3DSimulatorAssetRegistry* Registry =
                UV3DSimulatorGameInstance::GetAssetRegistryFromContext(WorldContextObject))
        {
            if (UMaterialInterface* Material = Registry->ResolveTerrainMaterial(); IsCompatibleMaterial(Material))
            {
                return Material;
            }
        }
        // Preserve a portable archive's explicit parent when no registry override resolves.
        // A configured generic glTF material can load successfully while lacking every array
        // parameter. Continue to the shipped palette in that case instead of rejecting all
        // terrain mesh dependencies later in CreateMaterial.
        TArray<FString> Candidates;
        if (!ArchiveMaterialPath.IsEmpty()) Candidates.AddUnique(ArchiveMaterialPath);
        Candidates.AddUnique(ResolveMaterialPath(WorldContextObject));
        FString ConfiguredPath;
        if (GConfig && GConfig->GetString(TEXT("V3D.Terrain"), TEXT("Material"), ConfiguredPath, GGameIni)
            && !ConfiguredPath.IsEmpty()) Candidates.AddUnique(ConfiguredPath);
        Candidates.AddUnique(DefaultMaterialPath);
        for (const FString& Path : Candidates)
        {
            if (Path.IsEmpty()) continue;
            if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *Path); IsCompatibleMaterial(Material))
                return Material;
        }
        return nullptr;
    }

    bool Bake(UglTFRuntimeAsset* Source, FGWorldBakedModel& Model, FString& Error, const UObject* WorldContextObject)
    {
        TArray<int32> TerrainIds;
        for (int32 Id = 0; Id < Model.Materials.Num(); ++Id)
            if (IsTerrainName(Model.Materials[Id].Name))
            {
                if (Model.Materials[Id].MaterialId != Id)
                { Error = TEXT("Terrain material IDs must match their baked array indices"); return false; }
                TerrainIds.Add(Id);
            }
        if (TerrainIds.IsEmpty()) return true;
        if (!Source || !Source->GetParser().IsValid())
        { Error = TEXT("Terrain bake needs the original glTF material JSON"); return false; }
        TerrainIds.Sort([&Model](int32 A,int32 B) { return Model.Materials[A].Name < Model.Materials[B].Name; });
        TMap<FString,TSharedPtr<FJsonObject>> JsonByName;
        for (const auto& Json : Source->GetParser()->GetMaterials())
        {
            // Parser material rows are TSharedRef<FJsonObject> and cannot be null.
            FString Name; Json->TryGetStringField(TEXT("name"),Name);
            if (!IsTerrainName(Name)) continue;
            if (JsonByName.Contains(Name))
            { Error = TEXT("Terrain source material names must be unique"); return false; }
            JsonByName.Add(Name,Json);
        }
        const auto Root = Source->GetParser()->GetJsonRoot();
        TArray<Private::FLayer> Palette;
        TArray<FString> FallbackReasons;
        for (const int32 Id : TerrainIds)
        {
            const auto* Json = JsonByName.Find(Model.Materials[Id].Name);
            if (!Json) { Error = TEXT("Baked terrain name cannot be resolved in the source glTF"); return false; }
            auto& Layer = Palette.AddDefaulted_GetRef();
            FString Reason;
            if (!Private::ReadLayer(*Json,Root,Model.Materials[Id],Model,Layer,Reason,Error,false)) return false;
            if (!Reason.IsEmpty())
                FallbackReasons.Add(FString::Printf(TEXT("%s (%s)"), *Model.Materials[Id].Name, *Reason));
        }
        if (!FallbackReasons.IsEmpty())
        {
            // Conversion is all-or-nothing: welding some layers to a palette while retaining
            // others loses shared-edge weights. This early exit leaves the entire captured
            // model untouched, including the original advanced shader and its parameters.
            UE_LOG(LogTemp, Warning, TEXT("[V3DTerrain] Keeping original decoded materials instead of terrain blending: %s"),
                *FString::Join(FallbackReasons, TEXT("; ")));
            Error.Reset();
            return true;
        }
        if (TerrainIds.Num() > Private::Layers)
        { Error = TEXT("Terrain blending supports at most four .terrain materials per model; split the model into palettes"); return false; }
        const UObject* EffectiveContext = IsValid(WorldContextObject) ? WorldContextObject : Source;
        int32 MaxSize = 1024; bool FlipGreen = false;
        if (GConfig)
        {
            GConfig->GetInt(TEXT("V3D.Terrain"),TEXT("MaxTextureSize"),MaxSize,GGameIni);
            GConfig->GetBool(TEXT("V3D.Terrain"),TEXT("FlipNormalGreen"),FlipGreen,GGameIni);
        }
        MaxSize = FMath::Min(FMath::Clamp(MaxSize,1,2048),
            FMath::Max(1,UGameSettings::ResolveMaxTextureResolution(EffectiveContext)));
        Private::FSourceTextureCache SourceTextures;
        SourceTextures.Source = Source;
        SourceTextures.MaxSize = MaxSize;
        for (int32 Index = 0; Index < TerrainIds.Num(); ++Index)
        {
            const int32 Id = TerrainIds[Index];
            FString Reason;
            if (!Private::ReadLayer(JsonByName.FindChecked(Model.Materials[Id].Name),Root,
                Model.Materials[Id],Model,Palette[Index],Reason,Error,true,&SourceTextures)) return false;
        }
        UMaterialInterface* Parent = ResolveMaterial(EffectiveContext);
        if (!Parent)
        {
            Error = TEXT("Terrain palette shader is unavailable or lacks the four Texture2DArray parameters; restore M_V3DTerrain before building");
            return false;
        }
        const FString MaterialPath = Parent->GetPathName();
        const FString ConfiguredMaterialPath = ResolveMaterialPath(EffectiveContext);
        if (ConfiguredMaterialPath != MaterialPath)
        {
            UE_LOG(LogTemp, Warning, TEXT("[V3DTerrain] Incompatible or missing terrain parent %s; using %s"),
                *ConfiguredMaterialPath, *MaterialPath);
        }
        FGWorldBakedMaterial Shared;
        Shared.MaterialId = Model.Materials.Num(); Shared.Name = TEXT("V3D.TerrainPalette");
        Shared.BaseMaterialPath = MaterialPath;
        auto& Marker = Shared.Scalars.AddDefaulted_GetRef(); Marker.Name = TEXT("V3D_Terrain"); Marker.Value=1;
        const TCHAR* Names[] = {TEXT("V3D_BaseColorArray"),TEXT("V3D_NormalArray"),TEXT("V3D_ORMArray"),TEXT("V3D_EmissiveArray")};
        // Palette pointers belong to SourceTextures until all array conversions have joined.
        TArray<FGWorldBakedTexture> Arrays;
        for (int32 Semantic=0; Semantic<4; ++Semantic)
        {
            auto& Texture = Arrays.AddDefaulted_GetRef();
            Texture.TextureId = Model.Textures.Num()+Semantic; Texture.Name = Names[Semantic];
            if (!Private::BuildArray(Palette,Semantic,MaxSize,FlipGreen,Texture,Error)) return false;
            auto& Parameter = Shared.Textures.AddDefaulted_GetRef();
            Parameter.Name = Names[Semantic]; Parameter.TextureId = Texture.TextureId;
        }
        if (!Private::PaintWeights(Model,TerrainIds,Shared.MaterialId,Error)) return false;
        for (int32 Layer=0; Layer<TerrainIds.Num(); ++Layer)
        {
            auto& SourceMaterial = Model.Materials[TerrainIds[Layer]];
            SourceMaterial.Scalars.RemoveAll([](const FGWorldBakedScalarParameter& P) { return P.Name == TEXT("V3D_TerrainLayer"); });
            SourceMaterial.Vectors.RemoveAll([](const FGWorldBakedVectorParameter& P) { return P.Name == TEXT("V3D_TerrainColor"); });
            auto& Index = SourceMaterial.Scalars.AddDefaulted_GetRef(); Index.Name=TEXT("V3D_TerrainLayer"); Index.Value=Layer;
            auto& Color = SourceMaterial.Vectors.AddDefaulted_GetRef(); Color.Name=TEXT("V3D_TerrainColor");
            Color.Value=FLinearColor(0,0,0,0); Color.Value.Component(Layer)=1;
            auto& Name = Shared.Scalars.AddDefaulted_GetRef();
            Name.Name=FString::Printf(TEXT("V3D_SourceMaterial%d"),Layer); Name.Value=TerrainIds[Layer];
        }
        UE_LOG(LogTemp, Display,
            TEXT("[V3DTerrain] Baked %d source textures into %d layers: Base=%dx%d Normal=%dx%d ORM=%dx%d Emissive=%dx%d (4 slices each)"),
            SourceTextures.Textures.Num(), TerrainIds.Num(),
            Arrays[0].SizeX, Arrays[0].SizeY, Arrays[1].SizeX, Arrays[1].SizeY,
            Arrays[2].SizeX, Arrays[2].SizeY, Arrays[3].SizeX, Arrays[3].SizeY);
        Model.Textures.Append(MoveTemp(Arrays)); Model.Materials.Add(MoveTemp(Shared));
        Error.Reset(); return true;
    }
}

#if WITH_DEV_AUTOMATION_TESTS
#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DTerrainMaterialContractTest,"V3D.Format.TerrainMaterialContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DTerrainMaterialContractTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr,V3DTerrain::DefaultMaterialPath);
    if (!TestNotNull(TEXT("Shipped terrain material resolves"),Parent)) return false;
    TestTrue(TEXT("Shipped terrain satisfies the runtime palette contract"),V3DTerrain::IsCompatibleMaterial(Parent));
    UMaterialInterface* Generic = UMaterial::GetDefaultMaterial(MD_Surface);
    TestFalse(TEXT("Generic glTF parents cannot replace palette array parameters"),V3DTerrain::IsCompatibleMaterial(Generic));
    TestTrue(TEXT("A legacy generic archive parent falls back to a compatible terrain parent"),
        V3DTerrain::IsCompatibleMaterial(V3DTerrain::ResolveMaterial(nullptr,Generic->GetPathName())));
    for (const TCHAR* Name : {TEXT("V3D_BaseColorArray"),TEXT("V3D_NormalArray"),TEXT("V3D_ORMArray"),TEXT("V3D_EmissiveArray")})
    {
        UTexture* Texture = nullptr;
        TestTrue(FString::Printf(TEXT("Terrain exposes array parameter %s"),Name),
            Parent->GetTextureParameterValue(FMaterialParameterInfo(FName(Name)),Texture)
                && Cast<UTexture2DArray>(Texture) != nullptr);
    }
    TestFalse(TEXT("Terrain ignores clearcoat in its final shader"),
        Parent->GetMaterial()->GetShadingModels().HasShadingModel(MSM_ClearCoat));

    // Inspect the saved graph as well as its shader source. A disconnected Weights input
    // silently produces the first palette layer even when mesh colors and arrays are valid.
    TArray<UObject*> Objects;
    GetObjectsWithOuter(Parent->GetMaterial(),Objects,EGetObjectsFlags::IncludeNestedObjects);
    bool bHasRGBAWeights = false;
    for (UObject* Object : Objects)
    {
        const auto* Custom = Cast<UMaterialExpressionCustom>(Object);
        if (!Custom || !Custom->Code.Contains(TEXT("V3D_SampleTerrain"))) continue;
        for (const auto& Input : Custom->Inputs)
        {
            if (Input.InputName != TEXT("Weights")) continue;
            const auto* Append = Cast<UMaterialExpressionAppendVector>(Input.Input.Expression);
            if (!Append) continue;
            const auto* Vertex = Cast<UMaterialExpressionVertexColor>(Append->A.Expression);
            const auto& Alpha = Append->B;
            const bool bAlphaOutput = Alpha.OutputIndex == 4
                || (Alpha.Mask && Alpha.MaskA && !Alpha.MaskR && !Alpha.MaskG && !Alpha.MaskB);
            bHasRGBAWeights |= Vertex && Append->A.OutputIndex == 0
                && Alpha.Expression == Vertex && bAlphaOutput;
        }
    }
    TestTrue(TEXT("Terrain sampler consumes vertex RGB plus alpha as four layer weights"),bHasRGBAWeights);
    return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DTerrainWeightsTest,"V3D.Format.TerrainWeights",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DTerrainWeightsTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("Exact suffix"), V3DTerrain::IsTerrainName(TEXT("grass.terrain")));
    TestFalse(TEXT("Suffix must be final"), V3DTerrain::IsTerrainName(TEXT("grass.terrain.extra")));
    TestFalse(TEXT("Suffix is case-sensitive"), V3DTerrain::IsTerrainName(TEXT("grass.TERRAIN")));
    TestEqual(TEXT("Signed-zero positions have identical weld hashes"),
        GetTypeHash(V3DTerrain::Private::PositionKey(FVector3f(-0.f,0.f,0.f),nullptr)),
        GetTypeHash(V3DTerrain::Private::PositionKey(FVector3f(0.f,0.f,0.f),nullptr)));
    FGWorldBakedModel Model; auto& Mesh=Model.Meshes.AddDefaulted_GetRef();
    for(int32 Material=0; Material<2; ++Material)
    {
        auto& P=Mesh.Primitives.AddDefaulted_GetRef(); P.MaterialId=Material; P.Mode=4;
        P.Positions={FVector3f(0,0,0),FVector3f(Material?1:-1,0,0),FVector3f(Material?1:-1,1,0)};
        P.UVs.Add({FVector2f(0,0),FVector2f(1,0),FVector2f(1,1)});
    }
    const FGWorldBakedModel Before = Model;
    FString Error;
    TestTrue(TEXT("Paint"),V3DTerrain::Private::PaintWeights(Model,{0,1},2,Error));
    TestTrue(TEXT("Shared position averages channels"),Mesh.Primitives[0].Colors[0].Equals(FVector4f(.5f,.5f,0,0)));
    TestTrue(TEXT("Interior one-hot red"),Mesh.Primitives[0].Colors[1].Equals(FVector4f(1,0,0,0)));
    TestTrue(TEXT("Interior one-hot green"),Mesh.Primitives[1].Colors[1].Equals(FVector4f(0,1,0,0)));
    TestEqual(TEXT("Shared palette"),Mesh.Primitives[1].MaterialId,2);
    TestEqual(TEXT("Explicit runtime data-color marker"),Mesh.Primitives[1].MaterialName,FString(TEXT("V3D.TerrainPalette")));
    // The patched glTFRuntime primitive option is essential: RGB and alpha are all weights.
    FglTFRuntimePrimitive RuntimePrimitive;
    TestTrue(TEXT("Ordinary glTF keeps its legacy transfer"),RuntimePrimitive.bVertexColorsSRGB);
    RuntimePrimitive.bVertexColorsSRGB=false;
    const FColor WeightBytes=FLinearColor(.25f,.25f,.25f,.25f).ToFColor(RuntimePrimitive.bVertexColorsSRGB);
    TestEqual(TEXT("RGB and alpha receive the same linear quantization"),WeightBytes.R,WeightBytes.A);
    const FLinearColor DescriptionWeights=WeightBytes.ReinterpretAsLinear();
    TestTrue(TEXT("Editor mesh-description weights remain linear"),FMath::IsNearlyEqual(DescriptionWeights.R,DescriptionWeights.A));
    TestFalse(TEXT("Five layers rejected"),V3DTerrain::Private::PaintWeights(Model,{0,1,2,3,4},5,Error));
    FGWorldBakedModel Invalid = Before;
    Invalid.Meshes.Add(Before.Meshes[0]);
    Invalid.Meshes.Last().Primitives[0].UVs[0].Pop();
    TestFalse(TEXT("Incomplete UVs in a later mesh reject the complete palette"),
        V3DTerrain::Private::PaintWeights(Invalid,{0,1},2,Error));
    TestEqual(TEXT("Failed terrain conversion preserves earlier material IDs"),Invalid.Meshes[0].Primitives[0].MaterialId,0);
    TestTrue(TEXT("Failed terrain conversion preserves earlier color data"),Invalid.Meshes[0].Primitives[0].Colors.IsEmpty());
    TestFalse(TEXT("Failed terrain conversion cannot mark an earlier mesh as painted"),Invalid.Meshes[0].bHasVertexColors);
    return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DTerrainSourceTextureTest,"V3D.Format.TerrainSourceTextureDecode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DTerrainSourceTextureTest::RunTest(const FString&)
{
    // A real PNG and triangle exercise image decoding, runtime-MID capture and palette
    // generation together. The forced parent deliberately has no glTF texture parameter.
    const FString Json = TEXT(R"({"asset":{"version":"2.0"},
        "buffers":[{"byteLength":66,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIA"}],
        "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24},{"buffer":0,"byteOffset":60,"byteLength":6}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
            {"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0}]}],
        "materials":[{"name":"paint.terrain","pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicFactor":0}}],
        "textures":[{"source":0}],
        "images":[{"uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFElEQVR4nGP4z8DwHwyBNBAw/AcAR8oI+ItOQ4UAAAAASUVORK5CYII="}]})");
    UglTFRuntimeAsset* Source = NewObject<UglTFRuntimeAsset>();
    if (!TestTrue(TEXT("Embedded textured triangle parses"),Source->LoadFromString(Json,FglTFRuntimeConfig()))) return false;
    FglTFRuntimeMaterialsConfig Config;
    Config.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    Config.ForceMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
    Config.ImagesConfig.bCompressMips = false;
    Config.ImagesConfig.bStreaming = false;
    FglTFRuntimeMeshLOD LOD;
    if (!TestTrue(TEXT("Textured source decodes through a parent without glTF parameters"),
        Source->LoadMeshAsRuntimeLOD(0,LOD,Config))) return false;
    FGWorldBakedModel Model;
    TMap<TWeakObjectPtr<UMaterialInterface>,int32> MaterialIds;
    TMap<TWeakObjectPtr<UTexture2D>,int32> TextureIds;
    FString Error;
    if (!TestTrue(TEXT("Runtime material and UVs capture"),FGWorldBakedDataCapture::CaptureMesh(
        0,TEXT("Triangle"),LOD,Model,MaterialIds,TextureIds,Error)))
    { AddError(Error); return false; }
    if (!TestEqual(TEXT("One authored source material survives capture"),Model.Materials.Num(),1)) return false;
    TestFalse(TEXT("Fixture reproduces missing MID texture binding"),
        Model.Materials[0].Textures.ContainsByPredicate([](const FGWorldBakedTextureParameter& P)
            { return P.Name == TEXT("baseColorTexture"); }));

    V3DTerrain::Private::FSourceTextureCache Pixels;
    Pixels.Source = Source; Pixels.MaxSize = 2;
    const FGWorldBakedTexture* Decoded = nullptr;
    if (!TestTrue(TEXT("Source pixels decode without creating another GPU texture"),Pixels.Resolve(0,true,Decoded,Error)))
    { AddError(Error); return false; }
    if (!TestNotNull(TEXT("Decoded source pixels exist"),Decoded)) return false;
    TestEqual(TEXT("PNG width survives"),Decoded->SizeX,2);
    TestEqual(TEXT("PNG height survives"),Decoded->SizeY,2);
    const FLinearColor Expected[] = {FLinearColor::Red,FLinearColor::Green,FLinearColor::Blue,FLinearColor::Yellow};
    for (int32 I=0; I<4; ++I)
        TestTrue(TEXT("PNG channel order and source colors survive CPU decode"),
            V3DTerrain::Private::Pixel(*Decoded,I%2,I/2,true).Equals(Expected[I],.001f));
    const FGWorldBakedTexture* Again = nullptr;
    TestTrue(TEXT("Shared image decode is cached"),Pixels.Resolve(0,true,Again,Error) && Again == Decoded);
    TestEqual(TEXT("Shared image has one native cached copy"),Pixels.Textures.Num(),1);

    V3DTerrain::Private::FSourceTextureCache Capped;
    Capped.Source = Source; Capped.MaxSize = 1;
    const FGWorldBakedTexture* Reduced = nullptr;
    if (!TestTrue(TEXT("CPU decode enforces the terrain cap"),Capped.Resolve(0,true,Reduced,Error))) return false;
    TestEqual(TEXT("Capped source width"),Reduced->SizeX,1);
    TestEqual(TEXT("Capped source height"),Reduced->SizeY,1);

    const int32 OriginalTextures = Model.Textures.Num();
    if (!TestTrue(TEXT("Required image bakes despite absent material parameter"),V3DTerrain::Bake(Source,Model,Error)))
    { AddError(Error); return false; }
    TestEqual(TEXT("Only final four arrays are added to the archive"),Model.Textures.Num(),OriginalTextures+4);
    const auto& Base = Model.Textures[OriginalTextures];
    TestTrue(TEXT("Palette has source colors instead of a silent white default"),
        Base.Mips[0].Bytes[1] < 250 || Base.Mips[0].Bytes[2] < 250);
    TestTrue(TEXT("Decoded texture still reaches the weighted palette"),V3DTerrain::IsPaletteMaterial(Model.Materials.Last()));

    // A mesh/material preload can populate the parser's UTexture cache. A subsequent
    // LoadTexture returns that object with no output mips, even with CacheMode=None.
    UglTFRuntimeAsset* WarmSource = NewObject<UglTFRuntimeAsset>();
    if (!TestTrue(TEXT("Warm-cache fixture parses"),WarmSource->LoadFromString(Json,FglTFRuntimeConfig()))) return false;
    FglTFRuntimeMaterialsConfig PrimeConfig;
    PrimeConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    PrimeConfig.ImagesConfig.bCompressMips = false;
    PrimeConfig.ImagesConfig.bStreaming = false;
    UTexture2D* PrimedTexture = WarmSource->LoadTexture(0,PrimeConfig);
    if (!TestNotNull(TEXT("Ordinary texture loading primes the parser cache"),PrimedTexture)) return false;
    FglTFRuntimeMaterialsConfig ProbeConfig = PrimeConfig;
    ProbeConfig.CacheMode = EglTFRuntimeCacheMode::None;
    TArray<FglTFRuntimeMipMap> WarmMips;
    FglTFRuntimeTextureSampler WarmSampler;
    TestTrue(TEXT("A cached GPU texture can be returned without decoder output mips"),
        WarmSource->GetParser()->LoadTexture(0,WarmMips,true,ProbeConfig,WarmSampler) == PrimedTexture
            && WarmMips.IsEmpty());

    FglTFRuntimeMeshLOD WarmLOD;
    if (!TestTrue(TEXT("Warm-cache mesh loads"),WarmSource->LoadMeshAsRuntimeLOD(0,WarmLOD,Config))) return false;
    FGWorldBakedModel WarmModel;
    TMap<TWeakObjectPtr<UMaterialInterface>,int32> WarmMaterialIds;
    TMap<TWeakObjectPtr<UTexture2D>,int32> WarmTextureIds;
    if (!TestTrue(TEXT("Warm-cache mesh captures"),FGWorldBakedDataCapture::CaptureMesh(
        0,TEXT("WarmTriangle"),WarmLOD,WarmModel,WarmMaterialIds,WarmTextureIds,Error)))
    { AddError(Error); return false; }
    V3DTerrain::Private::FSourceTextureCache WarmPixels;
    WarmPixels.Source = WarmSource; WarmPixels.MaxSize = 2;
    const FGWorldBakedTexture* WarmDecoded = nullptr;
    if (!TestTrue(TEXT("Terrain resolves pixels after an ordinary texture preload"),
        WarmPixels.Resolve(0,true,WarmDecoded,Error)))
    { AddError(Error); return false; }
    if (!TestNotNull(TEXT("Warm-cache source pixels exist"),WarmDecoded)) return false;
    if (!TestEqual(TEXT("Warm-cache PNG width survives"),WarmDecoded->SizeX,2)
        || !TestEqual(TEXT("Warm-cache PNG height survives"),WarmDecoded->SizeY,2)) return false;
    for (int32 I=0; I<4; ++I)
        TestTrue(TEXT("Warm-cache decode preserves every PNG color and channel"),
            V3DTerrain::Private::Pixel(*WarmDecoded,I%2,I/2,true).Equals(Expected[I],.001f));
    V3DTerrain::Private::FSourceTextureCache WarmCapped;
    WarmCapped.Source = WarmSource; WarmCapped.MaxSize = 1;
    const FGWorldBakedTexture* WarmReduced = nullptr;
    if (!TestTrue(TEXT("Warm-cache decode still enforces the terrain cap"),
        WarmCapped.Resolve(0,true,WarmReduced,Error)))
    { AddError(Error); return false; }
    if (!TestNotNull(TEXT("Capped warm-cache source pixels exist"),WarmReduced)) return false;
    TestEqual(TEXT("Capped warm-cache width"),WarmReduced->SizeX,1);
    TestEqual(TEXT("Capped warm-cache height"),WarmReduced->SizeY,1);
    const int32 WarmOriginalTextures = WarmModel.Textures.Num();
    if (!TestTrue(TEXT("Full terrain bake accepts a preloaded source texture"),V3DTerrain::Bake(WarmSource,WarmModel,Error)))
    { AddError(Error); return false; }
    if (!TestEqual(TEXT("Warm-cache bake appends exactly four final arrays"),WarmModel.Textures.Num(),WarmOriginalTextures+4)) return false;
    const auto& WarmBase = WarmModel.Textures[WarmOriginalTextures];
    if (!TestEqual(TEXT("Warm-cache palette width"),WarmBase.SizeX,2)
        || !TestEqual(TEXT("Warm-cache palette height"),WarmBase.SizeY,2)) return false;
    for (int32 I=0; I<4; ++I)
        TestTrue(TEXT("Warm-cache terrain palette retains every source PNG color"),
            V3DTerrain::Private::Pixel(WarmBase,I%2,I/2,true).Equals(Expected[I],.001f));
    TestTrue(TEXT("Warm-cache bake creates the weighted terrain material"),
        V3DTerrain::IsPaletteMaterial(WarmModel.Materials.Last()));

    // An unreadable required image must fail before vertex/material rewrites occur.
    UglTFRuntimeAsset* Missing = NewObject<UglTFRuntimeAsset>();
    if (!TestTrue(TEXT("Missing-image fixture parses"),Missing->LoadFromString(Json.Replace(
        TEXT("data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFElEQVR4nGP4z8DwHwyBNBAw/AcAR8oI+ItOQ4UAAAAASUVORK5CYII="),
        TEXT("missing-terrain-texture.png")),FglTFRuntimeConfig()))) return false;
    FGWorldBakedModel Invalid;
    Invalid.Materials.Add(Model.Materials[0]);
    Invalid.Meshes.Add(Model.Meshes[0]);
    Invalid.Meshes[0].Primitives[0].MaterialId = 0;
    AddExpectedError(TEXT("LoadImage"),EAutomationExpectedErrorFlags::Contains,0);
    AddExpectedError(TEXT("LoadTexture"),EAutomationExpectedErrorFlags::Contains,0);
    TestFalse(TEXT("Missing required image never bakes neutral pixels"),V3DTerrain::Bake(Missing,Invalid,Error));
    TestTrue(TEXT("Image failure identifies material and source texture"),
        Error.Contains(TEXT("paint.terrain")) && Error.Contains(TEXT("baseColorTexture")) && Error.Contains(TEXT("texture 0")));
    TestEqual(TEXT("Failed image decode preserves source material ids"),Invalid.Meshes[0].Primitives[0].MaterialId,0);
    return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DTerrainClearcoatTest,"V3D.Format.TerrainClearcoatIgnored",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DTerrainClearcoatTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const auto Root = MakeShared<FJsonObject>();
    const auto Json = MakeShared<FJsonObject>();
    const auto Extensions = MakeShared<FJsonObject>();
    const auto Coat = MakeShared<FJsonObject>();
    const auto PBR = MakeShared<FJsonObject>();
    Json->SetObjectField(TEXT("extensions"), Extensions);
    Json->SetObjectField(TEXT("pbrMetallicRoughness"), PBR);
    Extensions->SetObjectField(TEXT("KHR_materials_clearcoat"), Coat);
    FGWorldBakedModel Model;
    FGWorldBakedMaterial Material; Material.MaterialId = 0; Material.Name = TEXT("paint.terrain");
    V3DTerrain::Private::FLayer Layer;
    FString Reason, Error;
    TestTrue(TEXT("Default clearcoat factor zero is valid"),
        V3DTerrain::Private::ReadLayer(Json,Root,Material,Model,Layer,Reason,Error));
    TestTrue(TEXT("Default clearcoat keeps terrain blending"),Reason.IsEmpty());
    Coat->SetNumberField(TEXT("clearcoatFactor"), 0);
    Coat->SetNumberField(TEXT("clearcoatRoughnessFactor"), .7);
    TestTrue(TEXT("Blender's explicit inactive coat can bake"),
        V3DTerrain::Private::ReadLayer(Json,Root,Material,Model,Layer,Reason,Error));
    TestTrue(TEXT("Inactive coat roughness has no rendering effect"),Reason.IsEmpty());
    Coat->SetNumberField(TEXT("clearcoatFactor"), .8);
    TestTrue(TEXT("Active coat can bake with vertex-weight blending"),
        V3DTerrain::Private::ReadLayer(Json,Root,Material,Model,Layer,Reason,Error));
    TestTrue(TEXT("Active coat never disables terrain blending"),Reason.IsEmpty());
    PBR->SetStringField(TEXT("roughnessFactor"),TEXT("0.5"));
    TestFalse(TEXT("Ignored coat cannot hide malformed PBR factors"),
        V3DTerrain::Private::ReadLayer(Json,Root,Material,Model,Layer,Reason,Error));
    PBR->RemoveField(TEXT("roughnessFactor"));
    Coat->SetNumberField(TEXT("clearcoatFactor"), -1);
    TestFalse(TEXT("Negative coat factor is invalid"),
        V3DTerrain::Private::ReadLayer(Json,Root,Material,Model,Layer,Reason,Error));
    Coat->SetNumberField(TEXT("clearcoatFactor"), 0);
    const auto TextureInfo = MakeShared<FJsonObject>();
    TextureInfo->SetNumberField(TEXT("index"), .5);
    Coat->SetObjectField(TEXT("clearcoatTexture"), TextureInfo);
    TestFalse(TEXT("An inactive coat still rejects fractional texture indices"),
        V3DTerrain::Private::ReadLayer(Json,Root,Material,Model,Layer,Reason,Error));
    Coat->RemoveField(TEXT("clearcoatTexture"));
    Extensions->SetArrayField(TEXT("KHR_materials_clearcoat"),TArray<TSharedPtr<FJsonValue>>());
    TestFalse(TEXT("Extension payload must be an object"),
        V3DTerrain::Private::ReadLayer(Json,Root,Material,Model,Layer,Reason,Error));
    Extensions->RemoveField(TEXT("KHR_materials_clearcoat"));
    const auto Transmission = MakeShared<FJsonObject>();
    Extensions->SetObjectField(TEXT("KHR_materials_transmission"),Transmission);
    Transmission->SetNumberField(TEXT("transmissionFactor"),.4);
    TestTrue(TEXT("Terrain ignores other decoded lighting lobes"),
        V3DTerrain::Private::ReadLayer(Json,Root,Material,Model,Layer,Reason,Error));
    TestTrue(TEXT("Transmission cannot suppress terrain weights"),Reason.IsEmpty());
    Transmission->SetStringField(TEXT("transmissionFactor"),TEXT("0.4"));
    TestFalse(TEXT("Ignored lighting lobes still validate extension factor types"),
        V3DTerrain::Private::ReadLayer(Json,Root,Material,Model,Layer,Reason,Error));
    Extensions->RemoveField(TEXT("KHR_materials_transmission"));
    const auto IOR = MakeShared<FJsonObject>(); IOR->SetNumberField(TEXT("ior"),1.5);
    const auto Specular = MakeShared<FJsonObject>(); Specular->SetNumberField(TEXT("specularFactor"),1);
    Extensions->SetObjectField(TEXT("KHR_materials_ior"),IOR);
    Extensions->SetObjectField(TEXT("KHR_materials_specular"),Specular);
    TestTrue(TEXT("Common Blender lighting flags can bake"),
        V3DTerrain::Private::ReadLayer(Json,Root,Material,Model,Layer,Reason,Error));
    TestTrue(TEXT("Exporter defaults cannot suppress terrain weights"),Reason.IsEmpty());
    Json->SetBoolField(TEXT("doubleSided"),true);
    Json->SetStringField(TEXT("alphaMode"),TEXT("BLEND"));
    TestTrue(TEXT("Terrain deliberately ignores valid alpha and double-sided flags"),
        V3DTerrain::Private::ReadLayer(Json,Root,Material,Model,Layer,Reason,Error));
    TestTrue(TEXT("Surface flags cannot suppress terrain weights"),Reason.IsEmpty());
    Json->SetStringField(TEXT("doubleSided"),TEXT("true"));
    TestFalse(TEXT("Ignored double-sided flag still requires a boolean"),
        V3DTerrain::Private::ReadLayer(Json,Root,Material,Model,Layer,Reason,Error));
    Json->SetBoolField(TEXT("doubleSided"),true);
    Json->SetStringField(TEXT("alphaMode"),TEXT("INVALID"));
    TestFalse(TEXT("Ignored alpha mode still requires a valid glTF enum"),
        V3DTerrain::Private::ReadLayer(Json,Root,Material,Model,Layer,Reason,Error));
    Json->SetStringField(TEXT("alphaMode"),TEXT("BLEND"));
    const auto Emissive = MakeShared<FJsonObject>();
    Extensions->SetObjectField(TEXT("KHR_materials_emissive_strength"),Emissive);
    Emissive->SetNumberField(TEXT("emissiveStrength"),100000);
    TestTrue(TEXT("Unused emission alpha cannot reject black RGB with a high strength"),
        V3DTerrain::Private::ReadLayer(Json,Root,Material,Model,Layer,Reason,Error));

    // A source with active clearcoat must take the same vertex-weight path as a source
    // without it, including shared-edge interpolation, texture arrays and palette identity.
#if !(PLATFORM_ANDROID || PLATFORM_IOS)
    UglTFRuntimeAsset* Source = NewObject<UglTFRuntimeAsset>();
    const FString SourceJson = TEXT(R"({"asset":{"version":"2.0"},"materials":[
        {"name":"paint.terrain","doubleSided":true,"alphaMode":"BLEND",
         "pbrMetallicRoughness":{"baseColorFactor":[0.3,0.5,0.2,0.4],"metallicFactor":0.2,"roughnessFactor":0.7},
         "extensions":{
            "KHR_materials_clearcoat":{"clearcoatFactor":0.8},"KHR_materials_ior":{"ior":1.5},
            "KHR_materials_specular":{"specularFactor":1}}},
        {"name":"ground.terrain"}]})");
    if (!TestTrue(TEXT("Coated terrain fixture parses"),Source->LoadFromString(SourceJson,FglTFRuntimeConfig()))) return false;
    auto& Mesh = Model.Meshes.AddDefaulted_GetRef();
    for (int32 Id = 0; Id < 2; ++Id)
    {
        auto& Captured = Model.Materials.AddDefaulted_GetRef();
        Captured.MaterialId = Id;
        Captured.Name = Id == 0 ? TEXT("paint.terrain") : TEXT("ground.terrain");
        Captured.BaseMaterialPath = TEXT("/glTFRuntime/Materials/M_glTFRuntimeClearCoat.M_glTFRuntimeClearCoat");
        auto& Scalar = Captured.Scalars.AddDefaulted_GetRef();
        Scalar.Name = TEXT("clearcoatFactor"); Scalar.Value = .8f;
        auto& Primitive = Mesh.Primitives.AddDefaulted_GetRef();
        Primitive.Mode = 4; Primitive.MaterialId = Id; Primitive.MaterialName = Captured.Name;
        Primitive.Positions = {FVector3f(0,0,0),FVector3f(Id?1:-1,0,0),FVector3f(Id?1:-1,1,0)};
        Primitive.UVs.Add({FVector2f(0,0),FVector2f(1,0),FVector2f(0,1)});
        Primitive.Colors.Init(FVector4f(.1f,.2f,.3f,.4f),3);
    }
    const FGWorldBakedModel Before = Model;
    Error = TEXT("stale error");
    if (!TestTrue(TEXT("Active coat completes a full terrain palette bake"),V3DTerrain::Bake(Source,Model,Error)))
    { AddError(Error); return false; }
    TestTrue(TEXT("Successful coat bake clears stale error"),Error.IsEmpty());
    TestEqual(TEXT("Active coat produces one shared material"),Model.Materials.Num(),Before.Materials.Num()+1);
    TestEqual(TEXT("Active coat produces the four PBR texture arrays"),Model.Textures.Num(),Before.Textures.Num()+4);
    TestTrue(TEXT("Active coat selects terrain's runtime contract"),V3DTerrain::IsPaletteMaterial(Model.Materials.Last()));
    TestTrue(TEXT("Coated shared edge receives smooth weights"),
        Model.Meshes[0].Primitives[0].Colors[0].Equals(FVector4f(.5f,.5f,0,0)));
    TestEqual(TEXT("Coated terrain primitive uses the palette material"),Model.Meshes[0].Primitives[0].MaterialId,Before.Materials.Num());

    UglTFRuntimeAsset* NeutralSource = NewObject<UglTFRuntimeAsset>();
    if (!TestTrue(TEXT("Neutral coat fixture parses"),NeutralSource->LoadFromString(
        SourceJson.Replace(TEXT("0.8"),TEXT("0.0"))
            .Replace(TEXT("\"doubleSided\":true,"),TEXT(""))
            .Replace(TEXT("\"alphaMode\":\"BLEND\","),TEXT("")),FglTFRuntimeConfig()))) return false;
    FGWorldBakedModel Neutral = Before;
    if (!TestTrue(TEXT("Inactive clearcoat completes a full terrain palette bake"),V3DTerrain::Bake(NeutralSource,Neutral,Error)))
    { AddError(Error); return false; }
    bool bSamePalette = Model.Textures.Num() == Neutral.Textures.Num();
    for (int32 Id = 0; Id < Model.Textures.Num() && bSamePalette; ++Id)
    {
        const auto& A = Model.Textures[Id]; const auto& B = Neutral.Textures[Id];
        bSamePalette = A.PixelFormat == B.PixelFormat && A.NumLayers == B.NumLayers && A.Mips.Num() == B.Mips.Num();
        for (int32 Mip = 0; Mip < A.Mips.Num() && bSamePalette; ++Mip)
            bSamePalette = A.Mips[Mip].Bytes == B.Mips[Mip].Bytes;
    }
    TestTrue(TEXT("Clearcoat and surface flags do not affect terrain pixels or mips"),bSamePalette);
    TestTrue(TEXT("Clearcoat and surface flags do not affect vertex weights"),
        Model.Meshes[0].Primitives[0].Colors == Neutral.Meshes[0].Primitives[0].Colors);

    // Unknown shader contracts still cannot partially rewrite a model.
    UglTFRuntimeAsset* UnknownSource = NewObject<UglTFRuntimeAsset>();
    if (!TestTrue(TEXT("Unknown extension fixture parses"),UnknownSource->LoadFromString(
        SourceJson.Replace(TEXT("KHR_materials_clearcoat"),TEXT("EXT_unknown_shader")),FglTFRuntimeConfig()))) return false;
    FGWorldBakedModel Advanced = Before;
    TestTrue(TEXT("Unknown material extension retains a valid original shader"),V3DTerrain::Bake(UnknownSource,Advanced,Error));
    TestEqual(TEXT("Unknown shader fallback adds no palette"),Advanced.Materials.Num(),Before.Materials.Num());
    TestTrue(TEXT("Unknown shader fallback leaves original colors intact"),
        Advanced.Meshes[0].Primitives[0].Colors == Before.Meshes[0].Primitives[0].Colors);
#endif
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DTerrainSamplerNormalizationTest,"V3D.Format.TerrainSamplerNormalization",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DTerrainSamplerNormalizationTest::RunTest(const FString&)
{
#if !(PLATFORM_ANDROID || PLATFORM_IOS)
    // The image is embedded for a valid glTF reference. Bake consumes the captured pixels
    // below, so the regression needs neither a source GLB on disk nor an image decoder.
    UglTFRuntimeAsset* Source = NewObject<UglTFRuntimeAsset>();
    const FString SourceJson = TEXT(R"({"asset":{"version":"2.0"},
        "materials":[
            {"name":"a.terrain","pbrMetallicRoughness":{"baseColorTexture":{"index":0}}},
            {"name":"b.terrain","pbrMetallicRoughness":{"baseColorTexture":{"index":0},"baseColorFactor":[0.25,1,0.5,1]}}],
        "textures":[{"source":0,"sampler":0}],"samplers":[{}],
        "images":[{"uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAMAAAADCAYAAABWKLW/AAAAEUlEQVR4nGMI6DnxH4YZcHIAxKwXvF8gpQgAAAAASUVORK5CYII="}]})");
    if (!TestTrue(TEXT("Sampler regression glTF parses"),Source->LoadFromString(SourceJson,FglTFRuntimeConfig()))) return false;
    const auto Root = Source->GetParser()->GetJsonRoot();
    const auto Sampler = Root->GetArrayField(TEXT("samplers"))[0]->AsObject();
    const auto SourceTexture = Root->GetArrayField(TEXT("textures"))[0]->AsObject();
    FGWorldBakedModel Original;
    auto& Pixels = Original.Textures.AddDefaulted_GetRef();
    Pixels.TextureId=0; Pixels.SizeX=Pixels.SizeY=3; Pixels.PixelFormat=PF_R8G8B8A8;
    auto& Top = Pixels.Mips.AddDefaulted_GetRef();
    Top.SizeX=Top.SizeY=3; Top.Bytes.SetNumUninitialized(3*3*4);
    for (int32 Index=0; Index<9; ++Index)
    {
        Top.Bytes[Index*4]=80; Top.Bytes[Index*4+1]=140;
        Top.Bytes[Index*4+2]=200; Top.Bytes[Index*4+3]=255;
    }
    auto& Mesh=Original.Meshes.AddDefaulted_GetRef();
    for (int32 Id=0; Id<2; ++Id)
    {
        auto& Material=Original.Materials.AddDefaulted_GetRef();
        Material.MaterialId=Id; Material.Name=Id?TEXT("b.terrain"):TEXT("a.terrain");
        auto& Parameter=Material.Textures.AddDefaulted_GetRef();
        Parameter.Name=TEXT("baseColorTexture"); Parameter.TextureId=0;
        auto& Primitive=Mesh.Primitives.AddDefaulted_GetRef();
        Primitive.Mode=4; Primitive.MaterialId=Id; Primitive.MaterialName=Material.Name;
        Primitive.Positions={FVector3f(0,0,0),FVector3f(Id?1:-1,0,0),FVector3f(Id?1:-1,1,0)};
        Primitive.UVs.Add({FVector2f(0,0),FVector2f(1,0),FVector2f(0,1)});
    }
    FString Error;
    TArray<FGWorldBakedTexture> ReferenceArrays;
    const auto CheckBake = [this,Source,&Original,&ReferenceArrays,&Error]()
    {
        FGWorldBakedModel Baked=Original;
        if (!TestTrue(TEXT("Valid source sampler completes the terrain bake"),V3DTerrain::Bake(Source,Baked,Error)))
        { AddError(Error); return false; }
        if (!TestEqual(TEXT("Four palette arrays are generated"),Baked.Textures.Num(),5)) return false;
        TestTrue(TEXT("Sampler conversion preserves smooth shared-edge weights"),
            Baked.Meshes[0].Primitives[0].Colors[0].Equals(FVector4f(.5f,.5f,0,0)));
        TestTrue(TEXT("Sampler conversion selects the vertex palette material"),
            V3DTerrain::IsPaletteMaterial(Baked.Materials.Last()));
        for (int32 Index=1; Index<Baked.Textures.Num(); ++Index)
        {
            const auto& Array=Baked.Textures[Index];
            TestEqual(TEXT("Terrain normalizes horizontal wrapping"),Array.AddressX,uint8(TA_Wrap));
            TestEqual(TEXT("Terrain normalizes vertical wrapping"),Array.AddressY,uint8(TA_Wrap));
            TestEqual(TEXT("Terrain normalizes filtering"),Array.Filter,uint8(TF_Trilinear));
            TestEqual(TEXT("All four palette slices survive"),Array.NumLayers,4);
            if (!TestTrue(TEXT("Palette has mip data"),!Array.Mips.IsEmpty())) return false;
            TestEqual(TEXT("Palette mip chain reaches one texel"),Array.Mips.Last().SizeX,1);
            TestEqual(TEXT("Palette mip chain reaches one row"),Array.Mips.Last().SizeY,1);
            int32 Width=Array.SizeX, Height=Array.SizeY;
            for (const auto& Mip : Array.Mips)
            {
                TestEqual(TEXT("Mip width halves without gaps"),Mip.SizeX,Width);
                TestEqual(TEXT("Mip height halves without gaps"),Mip.SizeY,Height);
                TestEqual(TEXT("Mip retains all palette slices"),Mip.SizeZ,4);
                Width=FMath::Max(1,Width/2); Height=FMath::Max(1,Height/2);
            }
            if (!ReferenceArrays.IsEmpty())
            {
                const auto& Reference=ReferenceArrays[Index-1];
                if (!TestEqual(TEXT("Source sampler does not alter canonical mip count"),Array.Mips.Num(),Reference.Mips.Num())) return false;
                for (int32 Mip=0; Mip<Array.Mips.Num(); ++Mip)
                    TestTrue(TEXT("Source sampler does not alter canonical palette pixels"),Array.Mips[Mip].Bytes==Reference.Mips[Mip].Bytes);
            }
        }
        if (ReferenceArrays.IsEmpty())
            for (int32 Index=1; Index<Baked.Textures.Num(); ++Index) ReferenceArrays.Add(MoveTemp(Baked.Textures[Index]));
        return true;
    };
    // All legal glTF wrapping, magnification and minification combinations, including 9729
    // and 9986 exported by authoring tools, must be accepted instead of requiring 9987.
    int32 Combinations=0;
    for (const int32 WrapS : {10497,33071,33648})
        for (const int32 WrapT : {10497,33071,33648})
            for (const int32 Mag : {9728,9729})
                for (const int32 Min : {9728,9729,9984,9985,9986,9987})
                {
                    Sampler->SetNumberField(TEXT("wrapS"),WrapS); Sampler->SetNumberField(TEXT("wrapT"),WrapT);
                    Sampler->SetNumberField(TEXT("magFilter"),Mag); Sampler->SetNumberField(TEXT("minFilter"),Min);
                    if (!CheckBake()) return false;
                    ++Combinations;
                }
    TestEqual(TEXT("Every legal sampler combination is exercised"),Combinations,108);
    Sampler->Values.Empty();
    if (!CheckBake()) return false; // Default fields.
    SourceTexture->RemoveField(TEXT("sampler"));
    if (!CheckBake()) return false; // No sampler object referenced.
    SourceTexture->SetNumberField(TEXT("sampler"),0);
    const auto RejectMalformed = [this,Source,&Original,&Error]()
    {
        FGWorldBakedModel Baked=Original;
        TestFalse(TEXT("Malformed sampler is still rejected"),V3DTerrain::Bake(Source,Baked,Error));
        TestFalse(TEXT("Malformed sampler reports its reason"),Error.IsEmpty());
        TestEqual(TEXT("Malformed sampler cannot add a partial palette"),Baked.Materials.Num(),Original.Materials.Num());
    };
    for (const TCHAR* Field : {TEXT("wrapS"),TEXT("wrapT"),TEXT("magFilter"),TEXT("minFilter")})
    {
        Sampler->SetNumberField(Field,123); RejectMalformed(); Sampler->RemoveField(Field);
        Sampler->SetStringField(Field,TEXT("9729")); RejectMalformed(); Sampler->RemoveField(Field);
        Sampler->SetNumberField(Field,9729.5); RejectMalformed(); Sampler->RemoveField(Field);
    }
    SourceTexture->SetNumberField(TEXT("sampler"),99); RejectMalformed();
    SourceTexture->SetStringField(TEXT("sampler"),TEXT("0")); RejectMalformed();
#endif
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DTerrainParallelPixelsTest,"V3D.Format.TerrainParallelPixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DTerrainParallelPixelsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    // Non-power-of-two input forces bilinear resampling. The four output slices exceed the
    // worker threshold; the unoccupied fourth slice and every mip must match a serial bake.
    FGWorldBakedTexture Source; Source.SizeX = 129; Source.SizeY = 65; Source.PixelFormat = PF_R8G8B8A8;
    auto& Mip = Source.Mips.AddDefaulted_GetRef(); Mip.SizeX = Source.SizeX; Mip.SizeY = Source.SizeY;
    Mip.Bytes.SetNumUninitialized(Mip.SizeX * Mip.SizeY * 4);
    for (int32 I = 0; I < Mip.SizeX * Mip.SizeY; ++I)
    {
        Mip.Bytes[I*4] = uint8(I*31); Mip.Bytes[I*4+1] = uint8(I*17+25);
        Mip.Bytes[I*4+2] = uint8(I*7+128); Mip.Bytes[I*4+3] = 255;
    }
    TArray<V3DTerrain::Private::FLayer> Palette;
    for (int32 I = 0; I < 3; ++I)
    {
        auto& Layer = Palette.AddDefaulted_GetRef();
        Layer.Base = Layer.Normal = Layer.MR = Layer.AO = Layer.Emissive = &Source;
        Layer.BaseFactor = FLinearColor(.2f*(I+1),.3f,.7f,1);
        Layer.EmissiveFactor = FLinearColor(4,2,.5f,1);
        Layer.Roughness = .2f*(I+1); Layer.Metallic = .25f; Layer.Occlusion = .6f;
        Layer.NormalScale = .5f*(I+1);
    }
    FString Error;
    for (int32 Semantic = 0; Semantic < 4; ++Semantic)
    {
        FGWorldBakedTexture Serial, Parallel;
        if (!TestTrue(TEXT("Serial reference bakes"),V3DTerrain::Private::BuildArray(Palette,Semantic,256,true,Serial,Error,false))
            || !TestTrue(TEXT("Parallel conversion bakes"),V3DTerrain::Private::BuildArray(Palette,Semantic,256,true,Parallel,Error)))
        { AddError(Error); return false; }
        bool bEqual = Serial.SizeX == Parallel.SizeX && Serial.SizeY == Parallel.SizeY
            && Serial.NumLayers == Parallel.NumLayers && Serial.PixelFormat == Parallel.PixelFormat
            && Serial.Mips.Num() == Parallel.Mips.Num();
        for (int32 I = 0; I < Serial.Mips.Num() && bEqual; ++I)
            bEqual = Serial.Mips[I].SizeX == Parallel.Mips[I].SizeX && Serial.Mips[I].SizeY == Parallel.Mips[I].SizeY
                && Serial.Mips[I].SizeZ == Parallel.Mips[I].SizeZ && Serial.Mips[I].Bytes == Parallel.Mips[I].Bytes;
        TestTrue(FString::Printf(TEXT("Semantic %d is byte-identical across parallel and serial mip chains"),Semantic),bEqual);
    }
    FGWorldBakedTexture Invalid = Source;
    Invalid.PixelFormat = PF_FloatRGBA;
    Invalid.Mips[0].Bytes.Reset();
    Invalid.Mips[0].Bytes.SetNumZeroed(Invalid.SizeX * Invalid.SizeY * sizeof(FFloat16Color));
    const uint16 HalfNaN = 0x7e00;
    FMemory::Memcpy(Invalid.Mips[0].Bytes.GetData(),&HalfNaN,sizeof(HalfNaN));
    Palette[0].Base = &Invalid;
    FGWorldBakedTexture InvalidSerial, InvalidParallel;
    FString SerialError, ParallelError;
    TestFalse(TEXT("Serial conversion rejects a decoded nonfinite half pixel"),
        V3DTerrain::Private::BuildArray(Palette,0,256,false,InvalidSerial,SerialError,false));
    TestFalse(TEXT("Parallel conversion rejects the same invalid source"),
        V3DTerrain::Private::BuildArray(Palette,0,256,false,InvalidParallel,ParallelError));
    TestEqual(TEXT("Workers report the same error on the calling thread"),ParallelError,SerialError);
    TestTrue(TEXT("Invalid pixels cannot reach mip packing"),InvalidSerial.Mips.IsEmpty() && InvalidParallel.Mips.IsEmpty());
    return true;
}
#endif
