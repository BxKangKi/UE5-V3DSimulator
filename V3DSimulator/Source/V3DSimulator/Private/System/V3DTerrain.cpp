// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/V3DTerrain.h"
#include "Dom/JsonObject.h"
#include "Materials/MaterialInterface.h"
#include "Math/Float16Color.h"
#include "Misc/ConfigCacheIni.h"
#include "PixelFormat.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/V3DSimulatorGameInstance.h"
#include "glTFRuntimeAsset.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
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
        bool TextureInfo(const TSharedPtr<FJsonObject>& Info,
            const FGWorldBakedMaterial& Material, const TCHAR* Parameter,
            const FGWorldBakedModel& Model, const TSharedPtr<FJsonObject>& Root,
            const FGWorldBakedTexture*& Out, FString& Error)
        {
            Out = nullptr;
            if (!Info.IsValid()) return true;
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
            const TArray<TSharedPtr<FJsonValue>>* Textures = nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Samplers = nullptr;
            const int32 TextureIndex = static_cast<int32>(Number(Info, TEXT("index"), -1));
            if (!Root->TryGetArrayField(TEXT("textures"), Textures) || !Textures->IsValidIndex(TextureIndex))
            { Error = TEXT("Terrain glTF texture index is invalid"); return false; }
            const auto SourceTexture = (*Textures)[TextureIndex]->AsObject();
            const int32 SamplerIndex = static_cast<int32>(Number(SourceTexture, TEXT("sampler"), -1));
            if (SamplerIndex >= 0)
            {
                if (!Root->TryGetArrayField(TEXT("samplers"), Samplers) || !Samplers->IsValidIndex(SamplerIndex))
                { Error = TEXT("Terrain glTF sampler index is invalid"); return false; }
                const auto Sampler = (*Samplers)[SamplerIndex]->AsObject();
                const int32 Min = static_cast<int32>(Number(Sampler, TEXT("minFilter"), 9987));
                if (Number(Sampler, TEXT("wrapS"), 10497) != 10497
                    || Number(Sampler, TEXT("wrapT"), 10497) != 10497
                    || Number(Sampler, TEXT("magFilter"), 9729) != 9729
                    || Min != 9987)
                { Error = TEXT("Terrain array samplers require repeat wrapping, linear magnification and trilinear minification (9987)"); return false; }
            }
            for (const auto& P : Material.Textures)
                if (P.Name == Parameter && Model.Textures.IsValidIndex(P.TextureId))
                {
                    Out = &Model.Textures[P.TextureId];
                    break;
                }
            if (!Out)
            { Error = FString::Printf(TEXT("Terrain %s requires embedded decoded texture parameter %s; use the glTFRuntime PBR parent when baking"), *Material.Name, Parameter); return false; }
            if (Out->NumLayers != 1 || Out->Mips.IsEmpty()
                || (Out->PixelFormat != PF_B8G8R8A8 && Out->PixelFormat != PF_R8G8B8A8
                    && Out->PixelFormat != PF_FloatRGBA && Out->PixelFormat != PF_G8))
            { Error = TEXT("Terrain sources must decode to uncompressed RGBA8/BGRA8/RGBA16F/R8 (DDS/BC source pixels need decoding before baking)"); return false; }
            return true;
        }
        bool ReadLayer(const TSharedPtr<FJsonObject>& Json, const TSharedPtr<FJsonObject>& Root,
            const FGWorldBakedMaterial& Material, const FGWorldBakedModel& Model, FLayer& Layer, FString& Error)
        {
            FString AlphaMode = TEXT("OPAQUE"); Json->TryGetStringField(TEXT("alphaMode"), AlphaMode);
            bool TwoSided = false; Json->TryGetBoolField(TEXT("doubleSided"), TwoSided);
            if (AlphaMode != TEXT("OPAQUE") || TwoSided)
            { Error = TEXT("Terrain blending currently supports opaque, single-sided glTF PBR materials"); return false; }
            const auto Extensions = Object(Json, TEXT("extensions"));
            if (Extensions.IsValid())
                for (const auto& Pair : Extensions->Values)
                    if (Pair.Key != TEXT("KHR_materials_emissive_strength"))
                    { Error = FString::Printf(TEXT("Terrain material extension is unsupported: %s"), *Pair.Key); return false; }
            const auto PBR = Object(Json, TEXT("pbrMetallicRoughness"));
            Layer.MaterialId = Material.MaterialId;
            Layer.BaseFactor = Factor(PBR, TEXT("baseColorFactor"), FLinearColor::White, 4);
            Layer.Metallic = Number(PBR, TEXT("metallicFactor"), 1);
            Layer.Roughness = Number(PBR, TEXT("roughnessFactor"), 1);
            Layer.EmissiveFactor = Factor(Json, TEXT("emissiveFactor"), FLinearColor::Black, 3);
            const float Strength = Number(Object(Extensions, TEXT("KHR_materials_emissive_strength")), TEXT("emissiveStrength"), 1);
            Layer.EmissiveFactor *= Strength;
            const auto NormalInfo = Object(Json, TEXT("normalTexture"));
            const auto AOInfo = Object(Json, TEXT("occlusionTexture"));
            Layer.NormalScale = Number(NormalInfo, TEXT("scale"), 1);
            Layer.Occlusion = Number(AOInfo, TEXT("strength"), 1);
            if (!Unit(Layer.BaseFactor.R) || !Unit(Layer.BaseFactor.G) || !Unit(Layer.BaseFactor.B)
                || !Unit(Layer.BaseFactor.A) || !Unit(Layer.Metallic) || !Unit(Layer.Roughness)
                || !Unit(Layer.Occlusion) || !FMath::IsFinite(Layer.NormalScale)
                || Layer.NormalScale < 0 || !FiniteColor(Layer.EmissiveFactor)
                || Layer.EmissiveFactor.GetMin() < 0 || Layer.EmissiveFactor.GetMax() > 65504)
            { Error = TEXT("Terrain PBR factors are invalid or exceed the RGBA16F emissive range"); return false; }
            return TextureInfo(Object(PBR, TEXT("baseColorTexture")), Material, TEXT("baseColorTexture"), Model, Root, Layer.Base, Error)
                && TextureInfo(Object(PBR, TEXT("metallicRoughnessTexture")), Material, TEXT("metallicRoughnessTexture"), Model, Root, Layer.MR, Error)
                && TextureInfo(NormalInfo, Material, TEXT("normalTexture"), Model, Root, Layer.Normal, Error)
                && TextureInfo(AOInfo, Material, TEXT("occlusionTexture"), Model, Root, Layer.AO, Error)
                && TextureInfo(Object(Json, TEXT("emissiveTexture")), Material, TEXT("emissiveTexture"), Model, Root, Layer.Emissive, Error);
        }
        FLinearColor Pixel(const FGWorldBakedTexture& Texture, int32 X, int32 Y, const bool SRGB)
        {
            const auto& Mip = Texture.Mips[0];
            X = (X % Mip.SizeX + Mip.SizeX) % Mip.SizeX;
            Y = (Y % Mip.SizeY + Mip.SizeY) % Mip.SizeY;
            const int32 Index = Y * Mip.SizeX + X;
            if (Texture.PixelFormat == PF_FloatRGBA)
            {
                FFloat16Color Value;
                FMemory::Memcpy(&Value, Mip.Bytes.GetData() + int64(Index) * sizeof(Value), sizeof(Value));
                return Value.GetFloats();
            }
            if (Texture.PixelFormat == PF_G8)
            {
                const uint8 Value = Mip.Bytes[Index];
                const FColor Color(Value, Value, Value, 255);
                return SRGB ? FLinearColor::FromSRGBColor(Color) : Color.ReinterpretAsLinear();
            }
            const uint8* Bytes = Mip.Bytes.GetData() + int64(Index) * 4;
            const FColor Color(Texture.PixelFormat == PF_B8G8R8A8 ? Bytes[2] : Bytes[0], Bytes[1],
                Texture.PixelFormat == PF_B8G8R8A8 ? Bytes[0] : Bytes[2], Bytes[3]);
            return SRGB ? FLinearColor::FromSRGBColor(Color) : Color.ReinterpretAsLinear();
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
        void AppendMip(FGWorldBakedTexture& Texture, const TArray<FLinearColor>& Pixels, int32 Width, int32 Height)
        {
            auto& Mip = Texture.Mips.AddDefaulted_GetRef();
            Mip.SizeX = Width; Mip.SizeY = Height; Mip.SizeZ = Layers;
            const bool Half = Texture.PixelFormat == PF_FloatRGBA;
            Mip.Bytes.SetNumUninitialized(Pixels.Num() * (Half ? sizeof(FFloat16Color) : 4));
            for (int32 I = 0; I < Pixels.Num(); ++I)
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
        }
        bool BuildArray(const TArray<FLayer>& Palette, const int32 Semantic, const int32 MaxSize,
            const bool FlipGreen, FGWorldBakedTexture& Texture, FString& Error)
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
            for (int32 Slice = 0; Slice < Layers; ++Slice)
                for (int32 Y = 0; Y < Height; ++Y)
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
                        { Error = TEXT("Terrain resampled pixel exceeds supported finite RGBA16F range"); return false; }
                        Pixels[(Slice*Height+Y)*Width+X] = C;
                    }
            while (true)
            {
                AppendMip(Texture, Pixels, Width, Height);
                if (Width == 1 && Height == 1) break;
                const int32 NewW = FMath::Max(1, Width/2), NewH = FMath::Max(1, Height/2);
                TArray<FLinearColor> Next; Next.SetNumUninitialized(NewW * NewH * Layers);
                for (int32 Slice=0; Slice<Layers; ++Slice)
                    for (int32 Y=0; Y<NewH; ++Y)
                        for (int32 X=0; X<NewW; ++X)
                        {
                            FLinearColor C(0,0,0,0);
                            for (int32 DY=0; DY<2; ++DY) for (int32 DX=0; DX<2; ++DX)
                                C += Pixels[(Slice*Height+FMath::Min(Y*2+DY,Height-1))*Width+FMath::Min(X*2+DX,Width-1)] * .25f;
                            Next[(Slice*NewH+Y)*NewW+X] = Semantic == 1 ? NormalizeNormal(C) : C;
                        }
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
            for (auto& Mesh : Model.Meshes)
            {
                TMap<FVector3f,uint8> Masks;
                for (int32 PrimitiveIndex=0; PrimitiveIndex<Mesh.Primitives.Num(); ++PrimitiveIndex)
                {
                    const auto& P = Mesh.Primitives[PrimitiveIndex];
                    const int32 Layer = MaterialIds.Find(P.MaterialId);
                    if (Layer == INDEX_NONE) continue;
                    if (P.Mode != 4 || !P.Joints.IsEmpty() || !P.MorphTargets.IsEmpty() || P.UVs.IsEmpty())
                    { Error = TEXT("Terrain requires unskinned triangle meshes with UV0 and no morph targets"); return false; }
                    const bool Transformed = Mesh.AdditionalTransforms.Num() == Mesh.Primitives.Num();
                    for (const auto& Position : P.Positions)
                    {
                        const FVector3f Key = PositionKey(Position, Transformed ? &Mesh.AdditionalTransforms[PrimitiveIndex] : nullptr);
                        if (!FMath::IsFinite(Key.X) || !FMath::IsFinite(Key.Y) || !FMath::IsFinite(Key.Z))
                        { Error = TEXT("Terrain transformed position exceeds the finite float32 range"); return false; }
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

    UMaterialInterface* ResolveMaterial(const UObject* WorldContextObject)
    {
        if (!IsInGameThread() || IsGarbageCollecting())
        {
            return nullptr;
        }

        if (const UV3DSimulatorAssetRegistry* Registry =
                UV3DSimulatorGameInstance::GetAssetRegistryFromContext(WorldContextObject))
        {
            if (UMaterialInterface* Material = Registry->ResolveTerrainMaterial())
            {
                return Material;
            }
        }
        return nullptr;
    }

    bool Bake(UglTFRuntimeAsset* Source, FGWorldBakedModel& Model, FString& Error, const UObject* WorldContextObject)
    {
        TArray<int32> TerrainIds;
        for (const auto& Material : Model.Materials)
            if (IsTerrainName(Material.Name)) TerrainIds.Add(Material.MaterialId);
        if (TerrainIds.IsEmpty()) return true;
#if PLATFORM_ANDROID || PLATFORM_IOS
        // This plugin swizzles normal textures to mobile LA encoding during BuildTexture.
        // Build on desktop so portable arrays always contain canonical RGB tangent normals.
        Error = TEXT("Bake .terrain sources on Windows, macOS or Linux; mobile glTFRuntime normal pixels use platform LA encoding");
        return false;
#endif
        if (TerrainIds.Num() > 4)
        { Error = TEXT("Terrain blending supports at most four .terrain materials per model; split the model into palettes"); return false; }
        if (!Source || !Source->GetParser().IsValid())
        { Error = TEXT("Terrain bake needs the original glTF material JSON"); return false; }
        TerrainIds.Sort([&Model](int32 A,int32 B) { return Model.Materials[A].Name < Model.Materials[B].Name; });
        TMap<FString,TSharedPtr<FJsonObject>> JsonByName;
        for (const auto& Json : Source->GetParser()->GetMaterials())
        {
            FString Name; Json->TryGetStringField(TEXT("name"),Name);
            if (!IsTerrainName(Name)) continue;
            if (JsonByName.Contains(Name))
            { Error = TEXT("Terrain source material names must be unique"); return false; }
            JsonByName.Add(Name,Json);
        }
        const auto Root = Source->GetParser()->GetJsonRoot();
        TArray<Private::FLayer> Palette;
        for (const int32 Id : TerrainIds)
        {
            const auto* Json = JsonByName.Find(Model.Materials[Id].Name);
            if (!Json) { Error = TEXT("Baked terrain name cannot be resolved in the source glTF"); return false; }
            auto& Layer = Palette.AddDefaulted_GetRef();
            if (!Private::ReadLayer(*Json,Root,Model.Materials[Id],Model,Layer,Error)) return false;
        }
        const UObject* EffectiveContext = IsValid(WorldContextObject) ? WorldContextObject : Source;
        const FString MaterialPath = ResolveMaterialPath(EffectiveContext);
        int32 MaxSize = 1024; bool FlipGreen = false;
        if (GConfig)
        {
            GConfig->GetInt(TEXT("V3D.Terrain"),TEXT("MaxTextureSize"),MaxSize,GGameIni);
            GConfig->GetBool(TEXT("V3D.Terrain"),TEXT("FlipNormalGreen"),FlipGreen,GGameIni);
        }
        MaxSize = FMath::Clamp(MaxSize,1,2048);
        if (MaterialPath.IsEmpty()) { Error = TEXT("V3D.Terrain Material path is empty"); return false; }
        FGWorldBakedMaterial Shared;
        Shared.MaterialId = Model.Materials.Num(); Shared.Name = TEXT("V3D.TerrainPalette");
        Shared.BaseMaterialPath = MaterialPath;
        auto& Marker = Shared.Scalars.AddDefaulted_GetRef(); Marker.Name = TEXT("V3D_Terrain"); Marker.Value=1;
        const TCHAR* Names[] = {TEXT("V3D_BaseColorArray"),TEXT("V3D_NormalArray"),TEXT("V3D_ORMArray"),TEXT("V3D_EmissiveArray")};
        // Palette holds pointers into Model.Textures: build all arrays before changing that array.
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
        Model.Textures.Append(MoveTemp(Arrays)); Model.Materials.Add(MoveTemp(Shared));
        Error.Reset(); return true;
    }
}

#if WITH_DEV_AUTOMATION_TESTS
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
    return true;
}
#endif
