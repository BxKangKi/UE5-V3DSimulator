// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldArchive.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/WorldArchive.h"
#include "Format/v3d.hpp"

#include "Async/ParallelFor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Crc.h"
#include "Misc/Compression.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "System/SafeFileIO.h"
#include "System/WorldArchiveCodec.h"

namespace GWorldArchivePrivate
{
    // v3d.hpp owns revision 6 and the legacy GWORLD revision 4/5 compatibility reader.
    constexpr int32 HeaderBytes = v3d::archive_header_bytes;
    constexpr int64 PayloadAlignment = v3d::archive_alignment;
    constexpr int32 MaxNodesPerModel = 2000000;
    constexpr int32 MaxMeshesPerModel = 1000000;
    constexpr int32 MaxBonesPerModel = 65536;
    constexpr int32 MaxCollidersPerMesh = 65536;
    constexpr int32 MaxLightsPerMesh = 65536;
    constexpr int64 MaxArchiveBytes = static_cast<int64>(v3d::archive_max_bytes);
    constexpr double FineChunkCm = 512.0 * 100.0;
    constexpr double CoarseChunkCm = 8192.0 * 100.0;
    constexpr int64 FinePerCoarse = 16;

    bool TryComputeSpatialChunk(const FVector& Location, FIntVector& OutCoarse, FIntVector& OutFine)
    {
        const auto Floor32 = [](const double Value, const double Cell, int64& Out)
        {
            if (!FMath::IsFinite(Value)) return false;
            const double Floored = FMath::FloorToDouble(Value / Cell);
            if (!FMath::IsFinite(Floored)
                || Floored < static_cast<double>(MIN_int32)
                || Floored > static_cast<double>(MAX_int32)) return false;
            Out = static_cast<int64>(Floored);
            return true;
        };

        int64 CoarseX = 0, CoarseY = 0, CoarseZ = 0;
        int64 FineXGlobal = 0, FineYGlobal = 0, FineZGlobal = 0;
        if (!Floor32(Location.X, CoarseChunkCm, CoarseX)
            || !Floor32(Location.Y, CoarseChunkCm, CoarseY)
            || !Floor32(Location.Z, CoarseChunkCm, CoarseZ)
            || !Floor32(Location.X, FineChunkCm, FineXGlobal)
            || !Floor32(Location.Y, FineChunkCm, FineYGlobal)
            || !Floor32(Location.Z, FineChunkCm, FineZGlobal)) return false;

        const int64 FineX = FineXGlobal - CoarseX * FinePerCoarse;
        const int64 FineY = FineYGlobal - CoarseY * FinePerCoarse;
        const int64 FineZ = FineZGlobal - CoarseZ * FinePerCoarse;
        if (FineX < 0 || FineX >= FinePerCoarse
            || FineY < 0 || FineY >= FinePerCoarse
            || FineZ < 0 || FineZ >= FinePerCoarse) return false;

        OutCoarse = FIntVector(static_cast<int32>(CoarseX), static_cast<int32>(CoarseY), static_cast<int32>(CoarseZ));
        OutFine = FIntVector(static_cast<int32>(FineX), static_cast<int32>(FineY), static_cast<int32>(FineZ));
        return true;
    }

    /** Matches fixed-width GUID digit ordering without allocation inside O(N log N) sorts. */
    bool IsGuidLess(const FGuid& Left, const FGuid& Right)
    {
        if (Left.A != Right.A) return Left.A < Right.A;
        if (Left.B != Right.B) return Left.B < Right.B;
        if (Left.C != Right.C) return Left.C < Right.C;
        return Left.D < Right.D;
    }

    bool IsFiniteVector(const FVector& Value)
    {
        return !Value.ContainsNaN() && FMath::IsFinite(Value.X)
            && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
    }

    bool IsFiniteQuat(const FQuat& Value)
    {
        return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y)
            && FMath::IsFinite(Value.Z) && FMath::IsFinite(Value.W);
    }

    bool IsFiniteTransform(const FTransform& Value)
    {
        return !Value.ContainsNaN() && IsFiniteVector(Value.GetLocation())
            && IsFiniteVector(Value.GetScale3D()) && IsFiniteQuat(Value.GetRotation())
            && Value.GetRotation().IsNormalized();
    }

    bool IsSaneMeshSettings(const FMeshData& Data)
    {
        if (Data.Colliders.Num() > MaxCollidersPerMesh
            || Data.Lights.Num() > MaxLightsPerMesh)
        {
            return false;
        }
        for (const FModelCollider& Collider : Data.Colliders)
        {
            if (static_cast<uint8>(Collider.Collider) > static_cast<uint8>(EColliderType::Box)
                || !IsFiniteVector(Collider.Center) || !IsFiniteVector(Collider.Size)
                || Collider.Size.GetMin() < 0.0)
            {
                return false;
            }
        }
        for (const FLightData& Light : Data.Lights)
        {
            if (!IsFiniteVector(Light.Location) || !FMath::IsFinite(Light.Intensity)
                || !FMath::IsFinite(Light.SourceRadius)
                || !FMath::IsFinite(Light.SoftSourceRadius)
                || !FMath::IsFinite(Light.AttenuationRadius)
                || !FMath::IsFinite(Light.Length)
                || static_cast<uint8>(Light.Unit) > static_cast<uint8>(ELightUnits::Nits)
                || Light.SourceRadius < 0.0f || Light.SoftSourceRadius < 0.0f
                || Light.AttenuationRadius < 0.0f || Light.Length < 0.0f)
            {
                return false;
            }
        }
        return true;
    }

    // The byte layout is owned by v3d.hpp. This layer only translates Unreal values.
    std::string ToPortable(const FString& Value)
    {
        FTCHARToUTF8 Converted(*Value);
        return std::string(Converted.Get(), static_cast<size_t>(Converted.Length()));
    }

    FString ToUnreal(const std::string& Value)
    {
        FUTF8ToTCHAR Converted(Value.data(), static_cast<int32>(Value.size()));
        return FString(Converted.Length(), Converted.Get());
    }

    v3d::guid ToPortable(const FGuid& V) { return {{V.A, V.B, V.C, V.D}}; }
    FGuid ToUnreal(const v3d::guid& V) { return FGuid(V[0], V[1], V[2], V[3]); }
    v3d::dvec3 ToPortable(const FVector& V) { return {V.X, V.Y, V.Z}; }
    FVector ToUnreal(const v3d::dvec3& V) { return FVector(V.x, V.y, V.z); }

    v3d::transform ToPortable(const FTransform& V)
    {
        v3d::transform Result;
        const FQuat Q = V.GetRotation();
        Result.rotation = {Q.X, Q.Y, Q.Z, Q.W};
        Result.translation = ToPortable(V.GetLocation());
        Result.scale = ToPortable(V.GetScale3D());
        return Result;
    }

    FTransform ToUnreal(const v3d::transform& V)
    {
        return FTransform(FQuat(V.rotation.x, V.rotation.y, V.rotation.z, V.rotation.w),
            ToUnreal(V.translation), ToUnreal(V.scale));
    }

    v3d::range ToPortable(const FGWorldArchiveRange& V)
    {
        v3d::range R;
        R.name = ToPortable(V.Name); R.offset = V.Offset; R.stored_size = V.StoredSize;
        R.uncompressed_size = V.UncompressedSize; R.crc = V.Crc; R.codec = V.Codec;
        return R;
    }

    FGWorldArchiveRange ToUnreal(const v3d::range& V)
    {
        FGWorldArchiveRange R;
        R.Name = ToUnreal(V.name); R.Offset = V.offset; R.StoredSize = V.stored_size;
        R.UncompressedSize = V.uncompressed_size; R.Crc = V.crc; R.Codec = V.codec;
        return R;
    }

    v3d::model_summary ToPortable(const FGWorldModelSummary& V)
    {
        v3d::model_summary R;
        R.source_node_count = V.SourceNodeCount; R.source_mesh_count = V.SourceMeshCount;
        R.source_material_count = V.SourceMaterialCount; R.source_texture_count = V.SourceTextureCount;
        R.center = ToPortable(V.Center); R.size = ToPortable(V.Size);
        return R;
    }

    FGWorldModelSummary ToUnreal(const v3d::model_summary& V)
    {
        FGWorldModelSummary R;
        R.SourceNodeCount = V.source_node_count; R.SourceMeshCount = V.source_mesh_count;
        R.SourceMaterialCount = V.source_material_count; R.SourceTextureCount = V.source_texture_count;
        R.Center = ToUnreal(V.center); R.Size = ToUnreal(V.size);
        return R;
    }

    v3d::mesh_settings ToPortable(const FMeshData& V)
    {
        v3d::mesh_settings R;
        R.complex_collision = V.bComplexCollision; R.simple_collision = V.bSimpleCollision;
        R.is_entity = V.bIsEntity;
        R.colliders.reserve(V.Colliders.Num()); R.lights.reserve(V.Lights.Num());
        for (const FModelCollider& C : V.Colliders)
        {
            v3d::collider P;
            P.type = static_cast<uint8>(C.Collider); P.center = ToPortable(C.Center); P.size = ToPortable(C.Size);
            R.colliders.push_back(std::move(P));
        }
        for (const FLightData& L : V.Lights)
        {
            v3d::light P;
            P.location = ToPortable(L.Location); P.intensity = L.Intensity;
            P.source_radius = L.SourceRadius; P.soft_source_radius = L.SoftSourceRadius;
            P.attenuation_radius = L.AttenuationRadius; P.length = L.Length; P.unit = static_cast<uint8>(L.Unit);
            R.lights.push_back(std::move(P));
        }
        return R;
    }

    FMeshData ToUnreal(const v3d::mesh_settings& V)
    {
        FMeshData R;
        R.bComplexCollision = V.complex_collision; R.bSimpleCollision = V.simple_collision;
        R.bIsEntity = V.is_entity;
        R.Colliders.Reserve(static_cast<int32>(V.colliders.size())); R.Lights.Reserve(static_cast<int32>(V.lights.size()));
        for (const v3d::collider& C : V.colliders)
        {
            FModelCollider& P = R.Colliders.AddDefaulted_GetRef();
            P.Collider = static_cast<EColliderType>(C.type); P.Center = ToUnreal(C.center); P.Size = ToUnreal(C.size);
        }
        for (const v3d::light& L : V.lights)
        {
            FLightData& P = R.Lights.AddDefaulted_GetRef();
            P.Location = ToUnreal(L.location); P.Intensity = L.intensity;
            P.SourceRadius = L.source_radius; P.SoftSourceRadius = L.soft_source_radius;
            P.AttenuationRadius = L.attenuation_radius; P.Length = L.length; P.Unit = static_cast<ELightUnits>(L.unit);
        }
        return R;
    }

    v3d::model_metadata ToPortable(const FGWorldModelMetadata& V, const FGWorldModelSummary& Summary)
    {
        v3d::model_metadata R;
        R.summary = ToPortable(Summary);
        R.nodes.reserve(V.NodeTransforms.Num());
        for (const FGWorldNodeTransform& N : V.NodeTransforms)
        {
            v3d::node_transform P;
            P.node_index = N.NodeIndex; P.parent_index = N.ParentIndex; P.mesh_index = N.MeshIndex;
            P.skin_index = N.SkinIndex; P.name = ToPortable(N.Name); P.local_transform = ToPortable(N.LocalTransform);
            R.nodes.push_back(std::move(P));
        }
        for (const auto& Pair : V.SceneData.NodeMap)
        {
            v3d::scene_instance P;
            const auto& N = Pair.Value;
            P.mesh_name = ToPortable(N.MeshName.ToString()); P.world_transform = ToPortable(N.Transform);
            P.coarse_chunk = {{N.CoarseChunk.X, N.CoarseChunk.Y, N.CoarseChunk.Z}};
            P.fine_chunk = {{N.FineChunk.X, N.FineChunk.Y, N.FineChunk.Z}};
            P.always_loaded = N.bAlwaysLoaded;
            R.instances.emplace(ToPortable(Pair.Key.ToString()), std::move(P));
        }
        for (const auto& Pair : V.SceneData.WaterNodeMap)
        {
            v3d::water_instance P;
            P.world_transform = ToPortable(Pair.Value.Transform); P.stream_radius = Pair.Value.StreamRadius;
            R.waters.emplace(ToPortable(Pair.Key.ToString()), std::move(P));
        }
        for (const auto& Pair : V.SceneData.MeshMap)
        {
            v3d::scene_mesh P;
            const auto& M = Pair.Value;
            P.lods = {{M.LOD0, M.LOD1, M.LOD2, M.LOD3}}; P.extent = ToPortable(M.Extent);
            P.size = ToPortable(M.Size); P.settings = ToPortable(M.Data);
            R.meshes.emplace(ToPortable(Pair.Key.ToString()), std::move(P));
        }
        for (const auto& Pair : V.SceneData.ModelData.MeshData)
            R.settings.emplace(ToPortable(Pair.Key.ToString()), ToPortable(Pair.Value));
        return R;
    }

    bool ToUnreal(const v3d::model_metadata& V, FGWorldModelMetadata& Out, FString& Error)
    {
        FGWorldModelMetadata R;
        R.SourceNodeCount = V.summary.source_node_count; R.SourceMeshCount = V.summary.source_mesh_count;
        R.SourceMaterialCount = V.summary.source_material_count; R.SourceTextureCount = V.summary.source_texture_count;
        R.SceneData.ModelData.Center = ToUnreal(V.summary.center); R.SceneData.ModelData.Size = ToUnreal(V.summary.size);
        R.NodeTransforms.Reserve(static_cast<int32>(V.nodes.size()));
        for (const auto& N : V.nodes)
        {
            auto& P = R.NodeTransforms.AddDefaulted_GetRef();
            P.NodeIndex = N.node_index; P.ParentIndex = N.parent_index; P.MeshIndex = N.mesh_index;
            P.SkinIndex = N.skin_index; P.Name = ToUnreal(N.name); P.LocalTransform = ToUnreal(N.local_transform);
        }
        const auto DuplicateName = [&Error]()
        {
            Error = TEXT("V3D metadata has names that collide under Unreal's FName comparison");
            return false;
        };
        for (const auto& Pair : V.instances)
        {
            const FName Name(*ToUnreal(Pair.first));
            if (R.SceneData.NodeMap.Contains(Name)) return DuplicateName();
            const auto& N = Pair.second;
            FModelNodeData P;
            P.MeshName = FName(*ToUnreal(N.mesh_name)); P.Transform = ToUnreal(N.world_transform);
            P.CoarseChunk = FIntVector(N.coarse_chunk[0], N.coarse_chunk[1], N.coarse_chunk[2]);
            P.FineChunk = FIntVector(N.fine_chunk[0], N.fine_chunk[1], N.fine_chunk[2]);
            P.bAlwaysLoaded = N.always_loaded; R.SceneData.NodeMap.Add(Name, MoveTemp(P));
        }
        for (const auto& Pair : V.waters)
        {
            const FName Name(*ToUnreal(Pair.first));
            if (R.SceneData.WaterNodeMap.Contains(Name)) return DuplicateName();
            FWaterStreamNodeData P;
            P.Transform = ToUnreal(Pair.second.world_transform); P.StreamRadius = Pair.second.stream_radius;
            R.SceneData.WaterNodeMap.Add(Name, MoveTemp(P));
        }
        for (const auto& Pair : V.meshes)
        {
            const FName Name(*ToUnreal(Pair.first));
            if (R.SceneData.MeshMap.Contains(Name)) return DuplicateName();
            const auto& M = Pair.second;
            FModelMeshData P;
            P.LOD0 = M.lods[0]; P.LOD1 = M.lods[1]; P.LOD2 = M.lods[2]; P.LOD3 = M.lods[3];
            P.Extent = ToUnreal(M.extent); P.Size = ToUnreal(M.size); P.Data = ToUnreal(M.settings);
            R.SceneData.MeshMap.Add(Name, MoveTemp(P));
        }
        for (const auto& Pair : V.settings)
        {
            const FName Name(*ToUnreal(Pair.first));
            if (R.SceneData.ModelData.MeshData.Contains(Name)) return DuplicateName();
            R.SceneData.ModelData.MeshData.Add(Name, ToUnreal(Pair.second));
        }
        if (!R.IsSane(&Error)) return false;
        R.SceneData.bSuccess = true;
        Out = MoveTemp(R);
        return true;
    }

    bool CopyPortableBytes(const v3d::bytes& Bytes, TArray<uint8>& Out, FString& Error)
    {
        if (Bytes.empty() || Bytes.size() > static_cast<size_t>(FGWorldArchive::MaxDirectoryBytes))
        {
            Error = TEXT("V3D directory member exceeds the simulator allocation limit");
            return false;
        }
        Out.Reset(static_cast<int32>(Bytes.size()));
        Out.Append(Bytes.data(), static_cast<int32>(Bytes.size()));
        Error.Reset();
        return true;
    }

    template <typename T>
    bool EncodePortable(const T& Value, TArray<uint8>& Out, FString& Error)
    {
        v3d::bytes Bytes; std::string Failure;
        if (!v3d::encode(Value, Bytes, Failure)) { Error = ToUnreal(Failure); return false; }
        return CopyPortableBytes(Bytes, Out, Error);
    }

    bool SerializeDefinitionData(const FModelDefinition& Definition, const FString& DefinitionJson,
        TArray<uint8>& OutBytes, FString& OutError)
    {
        v3d::model_definition Value;
        Value.json = ToPortable(DefinitionJson);
        for (const auto& Pair : Definition.Bones)
            Value.bones.emplace(ToPortable(Pair.Key), ToPortable(Pair.Value));
        return EncodePortable(Value, OutBytes, OutError);
    }

    bool DeserializeDefinitionData(const TArray<uint8>& Bytes, TMap<FString, FString>& OutBones,
        FString& OutDefinitionJson, FString& OutError)
    {
        v3d::model_definition Value; std::string Error;
        if (!v3d::decode(Bytes.GetData(), Bytes.Num(), Value, Error)) { OutError = ToUnreal(Error); return false; }
        TMap<FString, FString> Bones;
        for (const auto& Pair : Value.bones) Bones.Add(ToUnreal(Pair.first), ToUnreal(Pair.second));
        OutBones = MoveTemp(Bones); OutDefinitionJson = ToUnreal(Value.json); OutError.Reset();
        return true;
    }

    bool SerializeMetadataData(const FGWorldModelMetadata& Metadata, const FGWorldModelSummary& Summary,
        TArray<uint8>& OutBytes, FString& OutError)
    {
        return EncodePortable(ToPortable(Metadata, Summary), OutBytes, OutError);
    }

    bool DeserializeMetadataData(const TArray<uint8>& Bytes, FGWorldModelMetadata& OutMetadata, FString& OutError)
    {
        v3d::model_metadata Value; std::string Error;
        if (!v3d::decode(Bytes.GetData(), Bytes.Num(), Value, Error)) { OutError = ToUnreal(Error); return false; }
        return ToUnreal(Value, OutMetadata, OutError);
    }

    bool SerializeWorldConfig(const FString& Json, TArray<uint8>& OutBytes, FString& OutError)
    {
        if (Json.TrimStartAndEnd().IsEmpty()) { OutError = TEXT("config.json is empty"); return false; }
        v3d::bytes Bytes; std::string Error;
        if (!v3d::encode_json(ToPortable(Json), Bytes, Error)) { OutError = ToUnreal(Error); return false; }
        return CopyPortableBytes(Bytes, OutBytes, OutError);
    }

    bool DeserializeWorldConfig(const TArray<uint8>& Bytes, FString& OutJson, FString& OutError)
    {
        std::string Json, Error;
        if (!v3d::decode_json(Bytes.GetData(), Bytes.Num(), Json, Error)) { OutError = ToUnreal(Error); return false; }
        OutJson = ToUnreal(Json);
        if (OutJson.TrimStartAndEnd().IsEmpty()) { OutError = TEXT("V3D JSON member is empty"); return false; }
        OutError.Reset(); return true;
    }

    v3d::model_record ToPortable(const FGWorldModelRecord& V)
    {
        v3d::model_record R;
        const auto& D = V.Definition;
        R.uuid = ToPortable(D.UUID); R.asset_type = static_cast<uint8>(D.AssetType);
        R.model_type = static_cast<uint8>(D.ModelType); R.entity_type = static_cast<uint8>(D.EntityType);
        R.item_type = static_cast<uint8>(D.ItemType); R.name = ToPortable(D.Name); R.display_name = ToPortable(D.DisplayName);
        R.gravity.enabled = D.GravityField.bEnabled; R.gravity.falloff = static_cast<uint8>(D.GravityField.Falloff);
        R.gravity.priority = D.GravityField.Priority; R.gravity.radius_cm = D.GravityField.RadiusCm;
        R.gravity.strength_cm_per_second_squared = D.GravityField.StrengthCmPerSecondSquared;
        R.gravity.local_center = ToPortable(D.GravityField.LocalCenter);
        R.summary = ToPortable(V.Summary); R.definition_range = ToPortable(V.DefinitionRange);
        R.metadata_range = ToPortable(V.MetadataRange); R.manifest_range = ToPortable(V.ManifestRange);
        return R;
    }

    bool ToUnreal(const v3d::model_record& V, FGWorldModelRecord& R)
    {
        auto& D = R.Definition;
        D.UUID = ToUnreal(V.uuid); D.AssetType = static_cast<EAssetDefinitionType>(V.asset_type);
        D.ModelType = static_cast<EModelDefinitionType>(V.model_type);
        D.EntityType = static_cast<EModelEntityType>(V.entity_type); D.ItemType = static_cast<EModelItemType>(V.item_type);
        D.Name = ToUnreal(V.name); D.DisplayName = ToUnreal(V.display_name);
        D.GravityField.bEnabled = V.gravity.enabled; D.GravityField.Falloff = static_cast<EGravityFieldFalloff>(V.gravity.falloff);
        D.GravityField.Priority = V.gravity.priority; D.GravityField.RadiusCm = V.gravity.radius_cm;
        D.GravityField.StrengthCmPerSecondSquared = V.gravity.strength_cm_per_second_squared;
        D.GravityField.LocalCenter = ToUnreal(V.gravity.local_center);
        R.Summary = ToUnreal(V.summary); R.DefinitionRange = ToUnreal(V.definition_range);
        R.MetadataRange = ToUnreal(V.metadata_range); R.ManifestRange = ToUnreal(V.manifest_range);
        D.GlbPath = FGWorldArchive::MakeModelReference(D.UUID); D.JsonPath = D.GlbPath + TEXT("#definition");
        return D.UUID.IsValid() && D.GravityField.IsValid() && R.Summary.IsSane();
    }

    v3d::sound_record ToPortable(const FGWorldSoundRecord& V)
    {
        v3d::sound_record R;
        R.uuid = ToPortable(V.Definition.UUID); R.name = ToPortable(V.Definition.Name);
        R.display_name = ToPortable(V.Definition.DisplayName); R.definition_range = ToPortable(V.DefinitionRange);
        R.wave_range = ToPortable(V.WaveRange);
        return R;
    }

    FGWorldSoundRecord ToUnreal(const v3d::sound_record& V)
    {
        FGWorldSoundRecord R;
        R.Definition.AssetType = EAssetDefinitionType::Sound; R.Definition.UUID = ToUnreal(V.uuid);
        R.Definition.Name = ToUnreal(V.name); R.Definition.DisplayName = ToUnreal(V.display_name);
        R.Definition.WavPath = FGWorldArchive::MakeSoundReference(R.Definition.UUID);
        R.Definition.JsonPath = R.Definition.WavPath + TEXT("#definition");
        R.DefinitionRange = ToUnreal(V.definition_range); R.WaveRange = ToUnreal(V.wave_range);
        return R;
    }

    bool IsValidMemberRange(const FGWorldArchiveRange& Range, uint64 DirectoryOffset, uint64 MaximumSize)
    {
        return v3d::archive_detail::valid_range(ToPortable(Range), DirectoryOffset, MaximumSize);
    }

    bool SerializeDirectory(const FGWorldArchiveRange& ConfigRange, const TArray<FGWorldArchiveRange>& ScriptRanges,
        const TArray<FGWorldModelRecord>& Records, const TArray<FGWorldSoundRecord>& SoundRecords,
        TArray<uint8>& OutBytes, FString& OutError)
    {
        v3d::archive_directory D;
        D.config_range = ToPortable(ConfigRange);
        for (const auto& Range : ScriptRanges) D.script_ranges.push_back(ToPortable(Range));
        for (const auto& Record : Records) D.models.push_back(ToPortable(Record));
        for (const auto& Record : SoundRecords) D.sounds.push_back(ToPortable(Record));
        v3d::bytes Bytes; std::string Error;
        if (!v3d::encode_directory(D, v3d::archive_revision, Bytes, Error)) { OutError = ToUnreal(Error); return false; }
        return CopyPortableBytes(Bytes, OutBytes, OutError);
    }

    bool DeserializeDirectory(const TArray<uint8>& Bytes, uint64 DirectoryOffset,
        uint32 ExpectedModels, uint32 ExpectedSounds, uint64 Revision, uint32 DirectoryCrc, FGWorldArchiveRange& OutConfigRange,
        TArray<FGWorldArchiveRange>& OutScriptRanges, TMap<FGuid, FGWorldModelRecord>& OutRecords,
        TMap<FGuid, FGWorldSoundRecord>& OutSoundRecords, FString& OutError)
    {
        v3d::archive_header H;
        H.directory_offset = DirectoryOffset; H.directory_size = Bytes.Num(); H.revision = Revision;
        H.model_count = ExpectedModels; H.sound_count = ExpectedSounds; H.directory_crc = DirectoryCrc;
        v3d::archive_directory D; std::string Error;
        if (!v3d::decode_directory(Bytes.GetData(), Bytes.Num(), H, D, Error)) { OutError = ToUnreal(Error); return false; }
        OutConfigRange = ToUnreal(D.config_range); OutScriptRanges.Reset(); OutRecords.Reset(); OutSoundRecords.Reset();
        for (const auto& Range : D.script_ranges) OutScriptRanges.Add(ToUnreal(Range));
        for (const auto& Record : D.models)
        {
            FGWorldModelRecord R;
            if (!ToUnreal(Record, R) || OutRecords.Contains(R.Definition.UUID))
            { OutError = TEXT("V3D directory model cannot be represented by the Unreal adapter"); return false; }
            const FGuid Id = R.Definition.UUID;
            OutRecords.Add(Id, MoveTemp(R));
        }
        for (const auto& Record : D.sounds)
        {
            FGWorldSoundRecord R = ToUnreal(Record);
            if (OutRecords.Contains(R.Definition.UUID) || OutSoundRecords.Contains(R.Definition.UUID))
            { OutError = TEXT("V3D directory contains a duplicate asset UUID"); return false; }
            const FGuid Id = R.Definition.UUID;
            OutSoundRecords.Add(Id, MoveTemp(R));
        }
        OutError.Reset(); return true;
    }

    bool ReadExact(IFileHandle& Handle, uint8* Destination, int64 Count)
    {
        return Count >= 0 && (Count == 0 || (Destination && Handle.Read(Destination, Count)));
    }

    bool WriteExact(IFileHandle& Handle, const uint8* Source, int64 Count)
    {
        return Count >= 0 && (Count == 0 || (Source && Handle.Write(Source, Count)));
    }

    bool ReadChecksummedRange(
        IFileHandle& Handle,
        int64 ExpectedFileSize,
        uint64 DataEndOffset,
        const FGWorldArchiveRange& Range,
        int64 MaximumBytes,
        TArray<uint8>& OutBytes,
        FString& OutError)
    {
        OutBytes.Reset();
        const bool bValidName = Range.Name.EndsWith(TEXT(".dat"), ESearchCase::CaseSensitive)
            || Range.Name.EndsWith(TEXT(".wav"), ESearchCase::CaseSensitive)
            || Range.Name.EndsWith(TEXT(".json"), ESearchCase::CaseSensitive);
        if (ExpectedFileSize < HeaderBytes || Range.Name.IsEmpty()
            || !bValidName
            || Range.StoredSize == 0 || Range.UncompressedSize == 0 || Range.Codec > 1
            || Range.StoredSize > static_cast<uint64>(MaximumBytes)
            || Range.UncompressedSize > static_cast<uint64>(MaximumBytes)
            || Range.StoredSize > static_cast<uint64>(MAX_int32)
            || Range.UncompressedSize > static_cast<uint64>(MAX_int32)
            || Range.Offset > DataEndOffset
            || Range.StoredSize > DataEndOffset - Range.Offset
            || (Range.Codec == 0 && Range.StoredSize != Range.UncompressedSize)
            || (Range.Codec == 1 && Range.StoredSize >= Range.UncompressedSize))
        {
            OutError = TEXT("The requested .v3d member range is invalid or exceeds its memory limit");
            return false;
        }

        if (Handle.Size() != ExpectedFileSize
            || !Handle.Seek(static_cast<int64>(Range.Offset)))
        {
            OutError = TEXT("The .v3d file changed or could not be opened for a member range read");
            return false;
        }

        TArray<uint8> Stored;
        Stored.SetNumUninitialized(static_cast<int32>(Range.StoredSize));
        if (!ReadExact(Handle, Stored.GetData(), Stored.Num()))
        {
            OutError = TEXT("The requested .v3d member could not be read completely");
            return false;
        }

        if (Range.Codec == 0)
        {
            if (Range.StoredSize != Range.UncompressedSize)
            {
                OutError = TEXT("An uncompressed archive member has inconsistent lengths");
                return false;
            }
            OutBytes = MoveTemp(Stored);
        }
        else
        {
            OutBytes.SetNumUninitialized(static_cast<int32>(Range.UncompressedSize));
            if (!FCompression::UncompressMemory(
                    NAME_Zlib,
                    OutBytes.GetData(), static_cast<int64>(OutBytes.Num()),
                    Stored.GetData(), static_cast<int64>(Stored.Num()),
                    COMPRESS_NoFlags,
                    0))
            {
                OutBytes.Reset();
                OutError = TEXT("The requested archive member failed zlib decompression");
                return false;
            }
        }
        // Release compressed input before scanning the expanded member for its checksum.
        Stored.Empty();
        if (FCrc::MemCrc32(OutBytes.GetData(), OutBytes.Num()) != Range.Crc)
        {
            OutBytes.Reset();
            OutError = TEXT("The requested archive member failed its uncompressed CRC check");
            return false;
        }
        return true;
    }

    bool ReadChecksummedRange(
        const FString& Path,
        const int64 ExpectedFileSize,
        const uint64 DataEndOffset,
        const FGWorldArchiveRange& Range,
        const int64 MaximumBytes,
        TArray<uint8>& OutBytes,
        FString& OutError)
    {
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*Path));
        if (!Handle.IsValid())
        {
            OutBytes.Reset();
            OutError = TEXT("The .v3d file could not be opened for a member range read");
            return false;
        }
        return ReadChecksummedRange(
            *Handle, ExpectedFileSize, DataEndOffset, Range, MaximumBytes,
            OutBytes, OutError);
    }

    bool WritePadding(IFileHandle& Handle, int64& InOutOffset, int64 Alignment)
    {
        const int64 AlignedOffset = Align(InOutOffset, Alignment);
        const int64 Padding = AlignedOffset - InOutOffset;
        if (Padding <= 0) return true;
        TArray<uint8> Zeros;
        Zeros.SetNumZeroed(static_cast<int32>(Padding));
        if (!WriteExact(Handle, Zeros.GetData(), Padding)) return false;
        InOutOffset = AlignedOffset;
        return true;
    }

    bool WriteDatMember(
        IFileHandle& Handle,
        int64& InOutOffset,
        const FString& MemberName,
        const TArray<uint8>& RawBytes,
        FGWorldArchiveRange& OutRange,
        FString& OutError,
        bool bPreferFastRead = false)
    {
        // Buffered archive members are metadata only. WAV payloads are intentionally forbidden
        // here so they can only use WriteStoredFileMember(), which streams them in bounded chunks
        // instead of creating another full-file TArray during the commit tail.
        const bool bValidName = MemberName.EndsWith(TEXT(".dat"), ESearchCase::CaseSensitive)
            || MemberName.EndsWith(TEXT(".json"), ESearchCase::CaseSensitive);
        const bool bSafeName = !MemberName.IsEmpty()
            && !MemberName.Contains(TEXT("\\"))
            && !MemberName.Contains(TEXT("../"))
            && !MemberName.StartsWith(TEXT("/"), ESearchCase::CaseSensitive)
            && !MemberName.Contains(TEXT(":"));
        if (!bSafeName || !bValidName
            || RawBytes.IsEmpty() || RawBytes.Num() > FGWorldArchive::MaxDatMemberBytes
            || !WritePadding(Handle, InOutOffset, PayloadAlignment))
        {
            OutError = FString::Printf(TEXT("Invalid or unalignable archive member: %s"), *MemberName);
            return false;
        }

        TArray<uint8> Compressed;
        // Use the failure-reporting 64-bit compression API. The legacy return-value overload may
        // assert when a bound cannot be represented, which is unacceptable for author-supplied
        // models even though each final TArray member is capped below MAX_int32.
        int64 Bound64 = 0;
        const bool bBoundFitsArray = FCompression::CompressMemoryBound(
                NAME_Zlib,
                Bound64,
                static_cast<int64>(RawBytes.Num()),
                0)
            && Bound64 > 0
            && Bound64 <= FGWorldArchive::MaxDatMemberBytes
            && Bound64 <= MAX_int32;
        const int32 Bound = bBoundFitsArray ? static_cast<int32>(Bound64) : 0;
        int64 CompressedSize = Bound64;
        // Mesh/texture payloads are latency-sensitive range-read members.  Compressing hundreds or
        // thousands of them creates a long zlib tail at the end of a world build and forces the
        // first stream of each member to decompress again.  Metadata remains compressed normally.
        bool bUseCompression = bBoundFitsArray && !bPreferFastRead;
        if (bUseCompression)
        {
            Compressed.SetNumUninitialized(Bound);
            bUseCompression = FCompression::CompressMemory(
                NAME_Zlib,
                Compressed.GetData(), CompressedSize,
                RawBytes.GetData(), static_cast<int64>(RawBytes.Num()),
                COMPRESS_BiasSpeed,
                0)
                && CompressedSize > 0
                && CompressedSize < static_cast<int64>(RawBytes.Num())
                && CompressedSize <= MAX_int32;


            if (bUseCompression)
            {
                Compressed.SetNum(static_cast<int32>(CompressedSize), EAllowShrinking::No);
            }
            else Compressed.Reset();
        }

        const uint8* StoredData = bUseCompression ? Compressed.GetData() : RawBytes.GetData();
        const int32 StoredSize = bUseCompression ? Compressed.Num() : RawBytes.Num();
        if (InOutOffset > MaxArchiveBytes || StoredSize > MaxArchiveBytes - InOutOffset
            || !WriteExact(Handle, StoredData, StoredSize))
        {
            OutError = FString::Printf(TEXT("Could not write archive member: %s"), *MemberName);
            return false;
        }

        OutRange.Name = MemberName;
        OutRange.Offset = static_cast<uint64>(InOutOffset);
        OutRange.StoredSize = static_cast<uint64>(StoredSize);
        OutRange.UncompressedSize = static_cast<uint64>(RawBytes.Num());
        OutRange.Crc = FCrc::MemCrc32(RawBytes.GetData(), RawBytes.Num());
        OutRange.Codec = bUseCompression ? 1 : 0;
        InOutOffset += StoredSize;
        return true;
    }


    bool WriteStoredFileMember(
        IFileHandle& Output,
        int64& InOutOffset,
        const FString& MemberName,
        const FString& SourcePath,
        const int64 ExpectedSize,
        FGWorldArchiveRange& OutRange,
        FString& OutError)
    {
        const bool bValidName = MemberName.EndsWith(TEXT(".wav"), ESearchCase::CaseSensitive);
        const bool bSafeName = !MemberName.IsEmpty()
            && !MemberName.Contains(TEXT("\\"))
            && !MemberName.Contains(TEXT("../"))
            && !MemberName.StartsWith(TEXT("/"), ESearchCase::CaseSensitive)
            && !MemberName.Contains(TEXT(":"));
        if (!bSafeName || !bValidName || ExpectedSize < 12
            || ExpectedSize > FGWorldArchive::MaxSoundBytes
            || !WritePadding(Output, InOutOffset, PayloadAlignment))
        {
            OutError = FString::Printf(TEXT("Invalid or unalignable streamed Sound member: %s"), *MemberName);
            return false;
        }

        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        TUniquePtr<IFileHandle> Input(PlatformFile.OpenRead(*SourcePath));
        if (!Input.IsValid() || Input->Size() != ExpectedSize)
        {
            OutError = FString::Printf(TEXT("Sound WAV changed or cannot be opened: %s"), *SourcePath);
            return false;
        }

        uint8 WaveHeader[12];
        if (!Input->Read(WaveHeader, UE_ARRAY_COUNT(WaveHeader))
            || FMemory::Memcmp(WaveHeader, "RIFF", 4) != 0
            || FMemory::Memcmp(WaveHeader + 8, "WAVE", 4) != 0
            || !Input->Seek(0))
        {
            OutError = FString::Printf(TEXT("Sound member is not a RIFF/WAVE file: %s"), *SourcePath);
            return false;
        }

        if (InOutOffset > MaxArchiveBytes || ExpectedSize > MaxArchiveBytes - InOutOffset)
        {
            OutError = FString::Printf(TEXT("Sound member exceeds the archive size limit: %s"), *SourcePath);
            return false;
        }

        constexpr int64 ChunkBytes = 4ll * 1024ll * 1024ll;
        TArray<uint8> Buffer;
        Buffer.SetNumUninitialized(static_cast<int32>(ChunkBytes));
        int64 Remaining = ExpectedSize;
        uint32 Crc = 0;
        while (Remaining > 0)
        {
            const int32 ThisRead = static_cast<int32>(FMath::Min<int64>(Remaining, ChunkBytes));
            if (!Input->Read(Buffer.GetData(), ThisRead)
                || !WriteExact(Output, Buffer.GetData(), ThisRead))
            {
                OutError = FString::Printf(TEXT("Could not stream Sound member into archive: %s"), *SourcePath);
                return false;
            }
            Crc = FCrc::MemCrc32(Buffer.GetData(), ThisRead, Crc);
            Remaining -= ThisRead;
        }

        OutRange.Name = MemberName;
        OutRange.Offset = static_cast<uint64>(InOutOffset);
        OutRange.StoredSize = static_cast<uint64>(ExpectedSize);
        OutRange.UncompressedSize = static_cast<uint64>(ExpectedSize);
        OutRange.Crc = Crc;
        OutRange.Codec = 0;
        InOutOffset += ExpectedSize;
        return true;
    }

    bool VerifyStoredWaveRangeStreaming(
        IFileHandle& Handle,
        const int64 ExpectedFileSize,
        const uint64 DataEndOffset,
        const FGWorldArchiveRange& Range,
        FString& OutError)
    {
        if (Range.Codec != 0 || Range.StoredSize != Range.UncompressedSize
            || Range.StoredSize < 12 || Range.StoredSize > static_cast<uint64>(FGWorldArchive::MaxSoundBytes)
            || Range.Offset > DataEndOffset || Range.StoredSize > DataEndOffset - Range.Offset
            || Range.Offset > static_cast<uint64>(ExpectedFileSize)
            || Range.StoredSize > static_cast<uint64>(ExpectedFileSize) - Range.Offset)
        {
            OutError = TEXT("Sound verification range is invalid.");
            return false;
        }
        if (!Handle.Seek(static_cast<int64>(Range.Offset)))
        {
            OutError = TEXT("Sound verification could not seek to the WAV member.");
            return false;
        }

        constexpr int64 ChunkBytes = 4ll * 1024ll * 1024ll;
        TArray<uint8> Buffer;
        Buffer.SetNumUninitialized(static_cast<int32>(ChunkBytes));
        uint64 Remaining = Range.StoredSize;
        uint32 Crc = 0;
        bool bCheckedHeader = false;
        while (Remaining > 0)
        {
            const int32 ThisRead = static_cast<int32>(FMath::Min<uint64>(Remaining, static_cast<uint64>(ChunkBytes)));
            if (!Handle.Read(Buffer.GetData(), ThisRead))
            {
                OutError = TEXT("Sound verification could not read the complete WAV member.");
                return false;
            }
            if (!bCheckedHeader)
            {
                if (ThisRead < 12 || FMemory::Memcmp(Buffer.GetData(), "RIFF", 4) != 0
                    || FMemory::Memcmp(Buffer.GetData() + 8, "WAVE", 4) != 0)
                {
                    OutError = TEXT("Sound verification found a non-RIFF/WAVE member.");
                    return false;
                }
                bCheckedHeader = true;
            }
            Crc = FCrc::MemCrc32(Buffer.GetData(), ThisRead, Crc);
            Remaining -= static_cast<uint64>(ThisRead);
        }
        if (!bCheckedHeader || Crc != Range.Crc)
        {
            OutError = TEXT("Sound verification CRC mismatch.");
            return false;
        }
        return true;
    }

    TArray<uint8> BuildHeader(
        uint64 DirectoryOffset,
        uint64 DirectorySize,
        uint32 DirectoryCrc,
        uint32 ModelCount,
        uint32 SoundCount,
        const FGuid& BuildId)
    {
        v3d::archive_header H;
        H.directory_offset = DirectoryOffset; H.directory_size = DirectorySize;
        H.directory_crc = DirectoryCrc; H.model_count = ModelCount; H.sound_count = SoundCount;
        H.build_id = ToPortable(BuildId);
        v3d::bytes Encoded; std::string Error;
        TArray<uint8> Bytes;
        if (v3d::encode_header(H, Encoded, Error))
            Bytes.Append(Encoded.data(), static_cast<int32>(Encoded.size()));
        return Bytes;
    }

    bool BuildBlocking(
        const FString& ArchivePathInput,
        const TArray<FGWorldBuildModel>& Models,
        const TArray<FGWorldBuildSound>& Sounds,
        FString& OutArchivePath,
        FString& OutError,
        const TFunction<bool()>& ShouldCancel,
        const FString& WorldConfigJson,
        const FV3DProjectScripts& Scripts)
    {
        OutArchivePath.Reset();
        OutError.Reset();
        const FString ArchivePath = FSafeFileIO::NormalizeFilePath(ArchivePathInput);
        const auto Cancelled = [&ShouldCancel]() { return ShouldCancel && ShouldCancel(); };
        if (ArchivePath.IsEmpty() || (Models.IsEmpty() && Sounds.IsEmpty())
            || Models.Num() > FGWorldArchive::MaxModels
            || Sounds.Num() > FGWorldArchive::MaxSounds || Cancelled())
        {
            OutError = TEXT("Archive path, model count, or cancellation state is invalid");
            return false;
        }

        TArray<const FGWorldBuildModel*> Sorted;
        Sorted.Reserve(Models.Num());
        for (const FGWorldBuildModel& Model : Models) Sorted.Add(&Model);
        Sorted.Sort([](const FGWorldBuildModel& A, const FGWorldBuildModel& B)
        {
            return IsGuidLess(A.Definition.UUID, B.Definition.UUID);
        });
        TArray<const FGWorldBuildSound*> SortedSounds;
        SortedSounds.Reserve(Sounds.Num());
        for (const FGWorldBuildSound& Sound : Sounds) SortedSounds.Add(&Sound);
        SortedSounds.Sort([](const FGWorldBuildSound& A, const FGWorldBuildSound& B)
        {
            return IsGuidLess(A.Definition.UUID, B.Definition.UUID);
        });

        IFileManager& FileManager = IFileManager::Get();
        TSet<FGuid> UUIDs;
        for (const FGWorldBuildModel* Model : Sorted)
        {
            FString Reason;
            if (!Model || Model->Definition.AssetType != EAssetDefinitionType::Model
                || !Model->Definition.UUID.IsValid() || UUIDs.Contains(Model->Definition.UUID)
                || Model->Definition.Name.IsEmpty() || Model->Definition.DisplayName.IsEmpty()
                || !Model->Definition.GravityField.IsValid()
                || Model->Definition.GlbPath.IsEmpty() || Model->DefinitionJson.IsEmpty()
                || Model->SourceFileSize <= 0 || !Model->Metadata.IsSane(&Reason)
                || !Model->BakedData.IsSane(&Reason)
                || Model->Metadata.SourceMeshCount != Model->BakedData.Meshes.Num()
                || Model->Metadata.SourceMaterialCount != Model->BakedData.Materials.Num()
                || Model->Metadata.SourceTextureCount != Model->BakedData.Textures.Num())
            {
                OutError = FString::Printf(TEXT("Invalid decoded world-build input. UUID=%s Reason=%s"),
                    Model ? *Model->Definition.UUID.ToString() : TEXT("<null>"),
                    Reason.IsEmpty() ? TEXT("identity/count validation failed") : *Reason);
                return false;
            }

            TSet<int32> CapturedSkinIds;
            for (const FGWorldBakedSkin& Skin : Model->BakedData.Skins)
            {
                CapturedSkinIds.Add(Skin.SkinIndex);
            }
            for (const FGWorldNodeTransform& Node : Model->Metadata.NodeTransforms)
            {
                if (Node.MeshIndex != INDEX_NONE && Node.SkinIndex != INDEX_NONE
                    && !CapturedSkinIds.Contains(Node.SkinIndex))
                {
                    OutError = FString::Printf(
                        TEXT("A skinned node has no decoded skin.dat. UUID=%s Node=%d Skin=%d"),
                        *Model->Definition.UUID.ToString(), Node.NodeIndex, Node.SkinIndex);
                    return false;
                }
            }
            UUIDs.Add(Model->Definition.UUID);
        }
        for (const FGWorldBuildSound* Sound : SortedSounds)
        {
            if (!Sound || Sound->Definition.AssetType != EAssetDefinitionType::Sound
                || !Sound->Definition.UUID.IsValid() || UUIDs.Contains(Sound->Definition.UUID)
                || Sound->Definition.Name.IsEmpty() || Sound->Definition.DisplayName.IsEmpty()
                || Sound->DefinitionJson.IsEmpty() || Sound->Definition.WavPath.IsEmpty()
                || Sound->SourceFileSize <= 0 || Sound->SourceFileSize > FGWorldArchive::MaxSoundBytes
                || FileManager.FileSize(*Sound->Definition.WavPath) != Sound->SourceFileSize)
            {
                OutError = TEXT("A Sound build input is incomplete, duplicated, missing, or exceeds its safety limit");
                return false;
            }
            UUIDs.Add(Sound->Definition.UUID);
        }

        if (!FileManager.MakeDirectory(*FPaths::GetPath(ArchivePath), true))
        {
            OutError = TEXT("Could not create the world output directory");
            return false;
        }

        const FString TemporaryPath = ArchivePath + TEXT(".tmp.")
            + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        bool bPublished = false;
        ON_SCOPE_EXIT { if (!bPublished) FileManager.Delete(*TemporaryPath, false, true, true); };
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        TUniquePtr<IFileHandle> Output(PlatformFile.OpenWrite(*TemporaryPath, false, false));
        if (!Output.IsValid())
        {
            OutError = TEXT("Could not create the temporary .v3d file");
            return false;
        }
        TArray<uint8> HeaderPlaceholder; HeaderPlaceholder.SetNumZeroed(HeaderBytes);
        if (!WriteExact(*Output, HeaderPlaceholder.GetData(), HeaderPlaceholder.Num()))
        {
            OutError = TEXT("Could not reserve the .v3d header");
            return false;
        }

        int64 Offset = HeaderBytes;
        const FString ConfigJson = WorldConfigJson.TrimStartAndEnd().IsEmpty()
            ? FString::Printf(TEXT("{\"WorldName\":\"%s\"}"), *FPaths::GetBaseFilename(ArchivePath))
            : WorldConfigJson;
        FGWorldArchiveRange ConfigRange;
        TArray<uint8> ConfigBytes;
        if (!SerializeWorldConfig(ConfigJson, ConfigBytes, OutError)
            || !WriteDatMember(*Output, Offset, TEXT("config.json"), ConfigBytes, ConfigRange, OutError))
        {
            return false;
        }

        TArray<FGWorldArchiveRange> ScriptRanges;
        for (int32 Index = 0; Index < 3; ++Index)
        {
            TArray<uint8> Bytes;
            FGWorldArchiveRange Range;
            if (!SerializeWorldConfig(Scripts.Text(Index), Bytes, OutError)
                || Bytes.Num() > FV3DProjectScripts::MaxScriptBytes + 8)
            { if (OutError.IsEmpty()) OutError = TEXT("Script member exceeds 2 MiB"); return false; }
            if (!WriteDatMember(*Output, Offset, FV3DProjectScripts::MemberName(Index), Bytes, Range, OutError)) return false;
            ScriptRanges.Add(MoveTemp(Range));
        }

        TArray<FGWorldModelRecord> Records;
        Records.Reserve(Sorted.Num());
        for (const FGWorldBuildModel* Model : Sorted)
        {
            if (Cancelled()) { OutError = TEXT("The .v3d build was cancelled"); return false; }
            const int64 CurrentSourceSize = FileManager.FileSize(*Model->Definition.GlbPath);
            const FDateTime CurrentSourceTime = FileManager.GetTimeStamp(*Model->Definition.GlbPath);
            if (CurrentSourceSize != Model->SourceFileSize
                || (Model->SourceTimestamp != FDateTime() && CurrentSourceTime != Model->SourceTimestamp))
            {
                OutError = FString::Printf(TEXT("Source GLB changed after decoding: %s"), *Model->Definition.GlbPath);
                return false;
            }

            FGWorldModelRecord& Record = Records.AddDefaulted_GetRef();
            Record.Definition.AssetType = EAssetDefinitionType::Model;
            Record.Definition.UUID = Model->Definition.UUID;
            Record.Definition.Name = Model->Definition.Name;
            Record.Definition.DisplayName = Model->Definition.DisplayName;
            Record.Definition.ModelType = Model->Definition.ModelType;
            Record.Definition.EntityType = Model->Definition.EntityType;
            Record.Definition.ItemType = Model->Definition.ItemType;
            Record.Definition.GravityField = Model->Definition.GravityField;
            Record.Summary.SourceNodeCount = Model->Metadata.SourceNodeCount;
            Record.Summary.SourceMeshCount = Model->Metadata.SourceMeshCount;
            Record.Summary.SourceMaterialCount = Model->Metadata.SourceMaterialCount;
            Record.Summary.SourceTextureCount = Model->Metadata.SourceTextureCount;
            Record.Summary.Center = Model->Metadata.SceneData.ModelData.Center;
            Record.Summary.Size = Model->Metadata.SceneData.ModelData.Size;

            const FString Prefix = FString::Printf(TEXT("models/%s/"),
                *Model->Definition.UUID.ToString(EGuidFormats::Digits));
            TArray<uint8> Bytes;
            if (!SerializeDefinitionData(Model->Definition, Model->DefinitionJson, Bytes, OutError)
                || !WriteDatMember(*Output, Offset, Prefix + TEXT("definition.dat"), Bytes,
                    Record.DefinitionRange, OutError)
                || !SerializeMetadataData(Model->Metadata, Record.Summary, Bytes, OutError)
                || !WriteDatMember(*Output, Offset, Prefix + TEXT("metadata.dat"), Bytes,
                    Record.MetadataRange, OutError))
            {
                return false;
            }

            FGWorldModelManifest Manifest;
            for (const FGWorldBakedMesh& Mesh : Model->BakedData.Meshes)
            {
                FGWorldArchiveRange Range;
                const FString Name = Prefix + FString::Printf(TEXT("meshes/%d.dat"), Mesh.MeshIndex);
                if (!WorldArchiveCodec::SerializeMesh(Mesh, Bytes, OutError)
                    || !WriteDatMember(*Output, Offset, Name, Bytes, Range, OutError, true)) return false;
                Manifest.MeshRanges.Add(Mesh.MeshIndex, MoveTemp(Range));
                Manifest.MeshNames.Add(Mesh.MeshIndex, Mesh.Name);
            }
            for (const FGWorldBakedSkin& Skin : Model->BakedData.Skins)
            {
                FGWorldArchiveRange Range;
                const FString Name = Prefix + FString::Printf(TEXT("skins/%d.dat"), Skin.SkinIndex);
                if (!WorldArchiveCodec::SerializeSkin(Skin, Bytes, OutError)
                    || !WriteDatMember(*Output, Offset, Name, Bytes, Range, OutError, true)) return false;
                Manifest.SkinRanges.Add(Skin.SkinIndex, MoveTemp(Range));
            }
            for (const FGWorldBakedMaterial& Material : Model->BakedData.Materials)
            {
                FGWorldArchiveRange Range;
                const FString Name = Prefix + FString::Printf(TEXT("materials/%d.dat"), Material.MaterialId);
                if (!WorldArchiveCodec::SerializeMaterial(Material, Bytes, OutError)
                    || !WriteDatMember(*Output, Offset, Name, Bytes, Range, OutError, true)) return false;
                Manifest.MaterialRanges.Add(Material.MaterialId, MoveTemp(Range));
            }
            for (const FGWorldBakedTexture& Texture : Model->BakedData.Textures)
            {
                FGWorldArchiveRange Range;
                const FString Name = Prefix + FString::Printf(TEXT("textures/%d.dat"), Texture.TextureId);
                if (!WorldArchiveCodec::SerializeTexture(Texture, Bytes, OutError)
                    || !WriteDatMember(*Output, Offset, Name, Bytes, Range, OutError, true)) return false;
                Manifest.TextureRanges.Add(Texture.TextureId, MoveTemp(Range));
            }
            if (!Manifest.IsSane(Record.Summary, &OutError)
                || !WorldArchiveCodec::SerializeManifest(Manifest, Bytes, OutError)
                || !WriteDatMember(*Output, Offset, Prefix + TEXT("manifest.dat"), Bytes,
                    Record.ManifestRange, OutError)) return false;

            // Catch an editor save that happened while this model's members were being written.
            if (FileManager.FileSize(*Model->Definition.GlbPath) != Model->SourceFileSize
                || (Model->SourceTimestamp != FDateTime()
                    && FileManager.GetTimeStamp(*Model->Definition.GlbPath) != Model->SourceTimestamp))
            {
                OutError = FString::Printf(TEXT("Source GLB changed during archive construction: %s"),
                    *Model->Definition.GlbPath);
                return false;
            }
        }

        TArray<FGWorldSoundRecord> SoundRecords;
        SoundRecords.Reserve(SortedSounds.Num());
        for (const FGWorldBuildSound* Sound : SortedSounds)
        {
            if (Cancelled()) { OutError = TEXT("The .v3d build was cancelled"); return false; }
            const int64 CurrentSourceSize = FileManager.FileSize(*Sound->Definition.WavPath);
            const FDateTime CurrentSourceTime = FileManager.GetTimeStamp(*Sound->Definition.WavPath);
            if (CurrentSourceSize != Sound->SourceFileSize
                || (Sound->SourceTimestamp != FDateTime() && CurrentSourceTime != Sound->SourceTimestamp))
            {
                OutError = FString::Printf(TEXT("Source WAV changed after build descriptor capture: %s"), *Sound->Definition.WavPath);
                return false;
            }

            FGWorldSoundRecord& Record = SoundRecords.AddDefaulted_GetRef();
            Record.Definition.AssetType = EAssetDefinitionType::Sound;
            Record.Definition.UUID = Sound->Definition.UUID;
            Record.Definition.Name = Sound->Definition.Name;
            Record.Definition.DisplayName = Sound->Definition.DisplayName;
            const FString Prefix = FString::Printf(TEXT("sounds/%s/"),
                *Sound->Definition.UUID.ToString(EGuidFormats::Digits));
            FTCHARToUTF8 DefinitionUtf8(*Sound->DefinitionJson);
            TArray<uint8> DefinitionBytes;
            DefinitionBytes.Append(reinterpret_cast<const uint8*>(DefinitionUtf8.Get()), DefinitionUtf8.Length());

            // Stream the WAV directly into the archive. Peak build memory is now bounded by the
            // 4 MiB transfer buffer instead of the largest Sound file (up to MaxSoundBytes).
            if (DefinitionBytes.IsEmpty()
                || !WriteDatMember(*Output, Offset, Prefix + TEXT("definition.json"), DefinitionBytes,
                    Record.DefinitionRange, OutError)
                || !WriteStoredFileMember(*Output, Offset, Prefix + TEXT("audio.wav"),
                    Sound->Definition.WavPath, Sound->SourceFileSize, Record.WaveRange, OutError))
            {
                return false;
            }
            if (FileManager.FileSize(*Sound->Definition.WavPath) != Sound->SourceFileSize
                || (Sound->SourceTimestamp != FDateTime()
                    && FileManager.GetTimeStamp(*Sound->Definition.WavPath) != Sound->SourceTimestamp))
            {
                OutError = FString::Printf(TEXT("Source WAV changed during archive construction: %s"),
                    *Sound->Definition.WavPath);
                return false;
            }
        }

        if (!WritePadding(*Output, Offset, PayloadAlignment))
        { OutError = TEXT("Could not align the .v3d directory"); return false; }
        const uint64 DirectoryOffset = static_cast<uint64>(Offset);
        TArray<uint8> Directory;
        if (!SerializeDirectory(ConfigRange, ScriptRanges, Records, SoundRecords, Directory, OutError)
            || Offset > MaxArchiveBytes - Directory.Num()
            || !WriteExact(*Output, Directory.GetData(), Directory.Num()))
        { if (OutError.IsEmpty()) OutError = TEXT("Could not write the .v3d directory"); return false; }

        const FGuid BuildId = FGuid::NewGuid();
        const TArray<uint8> Header = BuildHeader(DirectoryOffset, Directory.Num(),
            FCrc::MemCrc32(Directory.GetData(), Directory.Num()), Records.Num(), SoundRecords.Num(), BuildId);
        if (Header.Num() != HeaderBytes || !Output->Seek(0)
            || !WriteExact(*Output, Header.GetData(), Header.Num()) || !Output->Flush(true))
        { OutError = TEXT("Could not flush the .v3d transaction"); return false; }
        Output.Reset();

        FString VerifyError;
        TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Verification =
            FGWorldArchiveReader::Open(TemporaryPath, VerifyError);
        if (!Verification.IsValid())
        { OutError = FString::Printf(TEXT("Temporary .v3d verification failed: %s"), *VerifyError); return false; }
        const int64 TemporaryFileSize = FileManager.FileSize(*TemporaryPath);
        TUniquePtr<IFileHandle> VerificationHandle(PlatformFile.OpenRead(*TemporaryPath));
        if (!VerificationHandle.IsValid() || VerificationHandle->Size() != TemporaryFileSize)
        {
            OutError = TEXT("Temporary .v3d could not be reopened for payload verification");
            return false;
        }
        {
            FV3DProjectScripts VerifiedScripts;
            if (!Verification->ReadProjectScripts(VerifiedScripts, VerifyError))
            { OutError = TEXT("Temporary archive scripts failed verification: ") + VerifyError; return false; }
            for (int32 Index = 0; Index < 3; ++Index)
                if (VerifiedScripts.Text(Index) != Scripts.Text(Index))
                { OutError = TEXT("Temporary archive script content mismatch"); return false; }
            FString VerifiedConfig;
            if (!Verification->ReadWorldConfig(VerifiedConfig, VerifyError) || VerifiedConfig != ConfigJson)
            {
                OutError = FString::Printf(TEXT("config.json verification failed: %s"), *VerifyError);
                return false;
            }
        }
        for (const FGWorldModelRecord& Record : Records)
        {
            if (Cancelled())
            {
                OutError = TEXT("The .v3d build was cancelled during verification");
                return false;
            }

            // Root members are payloads too. Open() deliberately validates only the fixed header
            // and compact directory, so publication must explicitly range-read their flushed bytes
            // before claiming that the temporary archive is complete.
            {
                TMap<FString, FString> VerifiedBones;
                FString VerifiedDefinitionJson;
                if (!Verification->ReadModelDefinition(
                        Record.Definition.UUID,
                        VerifiedBones,
                        VerifiedDefinitionJson,
                        VerifyError))
                {
                    OutError = FString::Printf(
                        TEXT("definition.dat verification failed: %s"), *VerifyError);
                    return false;
                }
            }
            {
                FGWorldModelMetadata VerifiedMetadata;
                if (!Verification->ReadModelMetadata(
                        Record.Definition.UUID,
                        VerifiedMetadata,
                        VerifyError))
                {
                    OutError = FString::Printf(
                        TEXT("metadata.dat verification failed: %s"), *VerifyError);
                    return false;
                }
            }

            FGWorldModelManifest Manifest;
            if (!Verification->ReadModelManifest(Record.Definition.UUID, Manifest, VerifyError))
            { OutError = FString::Printf(TEXT("Manifest verification failed: %s"), *VerifyError); return false; }

            // Verify every payload from the bytes actually flushed to disk. Only one member is
            // decompressed at a time, avoiding a second full-model copy during publication.
            TArray<uint8> VerifiedBytes;
            for (const TPair<int32, FGWorldArchiveRange>& Pair : Manifest.MeshRanges)
            {
                if (Cancelled()) { OutError = TEXT("The .v3d build was cancelled during verification"); return false; }
                FGWorldBakedMesh Mesh;
                if (!ReadChecksummedRange(*VerificationHandle, TemporaryFileSize, DirectoryOffset,
                        Pair.Value, FGWorldArchive::MaxDatMemberBytes, VerifiedBytes, VerifyError)
                    || !WorldArchiveCodec::DeserializeMesh(VerifiedBytes, Mesh, VerifyError)
                    || Mesh.MeshIndex != Pair.Key)
                {
                    OutError = FString::Printf(TEXT("mesh.dat verification failed: %s"), *VerifyError);
                    return false;
                }
                VerifiedBytes.Empty();
                for (const FGWorldBakedPrimitive& Primitive : Mesh.Primitives)
                {
                    if (Primitive.MaterialId != INDEX_NONE
                        && !Manifest.MaterialRanges.Contains(Primitive.MaterialId))
                    {
                        OutError = TEXT("mesh.dat verification found a missing material reference");
                        return false;
                    }
                }
            }
            for (const TPair<int32, FGWorldArchiveRange>& Pair : Manifest.MaterialRanges)
            {
                FGWorldBakedMaterial Material;
                if (!ReadChecksummedRange(*VerificationHandle, TemporaryFileSize, DirectoryOffset,
                        Pair.Value, FGWorldArchive::MaxDatMemberBytes, VerifiedBytes, VerifyError)
                    || !WorldArchiveCodec::DeserializeMaterial(VerifiedBytes, Material, VerifyError)
                    || Material.MaterialId != Pair.Key)
                {
                    OutError = FString::Printf(TEXT("material.dat verification failed: %s"), *VerifyError);
                    return false;
                }
                VerifiedBytes.Empty();
                for (const FGWorldBakedTextureParameter& Parameter : Material.Textures)
                {
                    if (Parameter.TextureId != INDEX_NONE
                        && !Manifest.TextureRanges.Contains(Parameter.TextureId))
                    {
                        OutError = TEXT("material.dat verification found a missing texture reference");
                        return false;
                    }
                }
            }
            for (const TPair<int32, FGWorldArchiveRange>& Pair : Manifest.TextureRanges)
            {
                FGWorldBakedTexture Texture;
                if (!ReadChecksummedRange(*VerificationHandle, TemporaryFileSize, DirectoryOffset,
                        Pair.Value, FGWorldArchive::MaxDatMemberBytes, VerifiedBytes, VerifyError)
                    || !WorldArchiveCodec::DeserializeTexture(VerifiedBytes, Texture, VerifyError)
                    || Texture.TextureId != Pair.Key)
                {
                    OutError = FString::Printf(TEXT("texture.dat verification failed: %s"), *VerifyError);
                    return false;
                }
                VerifiedBytes.Empty();
            }
            for (const TPair<int32, FGWorldArchiveRange>& Pair : Manifest.SkinRanges)
            {
                FGWorldBakedSkin Skin;
                if (!ReadChecksummedRange(*VerificationHandle, TemporaryFileSize, DirectoryOffset,
                        Pair.Value, FGWorldArchive::MaxDatMemberBytes, VerifiedBytes, VerifyError)
                    || !WorldArchiveCodec::DeserializeSkin(VerifiedBytes, Skin, VerifyError)
                    || Skin.SkinIndex != Pair.Key)
                {
                    OutError = FString::Printf(TEXT("skin.dat verification failed: %s"), *VerifyError);
                    return false;
                }
                VerifiedBytes.Empty();
            }
        }
        for (const FGWorldSoundRecord& Record : SoundRecords)
        {
            if (Cancelled()) { OutError = TEXT("The .v3d build was cancelled during sound verification"); return false; }
            FString VerifiedJson;
            if (!Verification->ReadSoundDefinition(Record.Definition.UUID, VerifiedJson, VerifyError)
                || VerifiedJson.IsEmpty()
                || !VerifyStoredWaveRangeStreaming(
                    *VerificationHandle, TemporaryFileSize, DirectoryOffset, Record.WaveRange, VerifyError))
            {
                OutError = FString::Printf(TEXT("Sound verification failed: %s"), *VerifyError);
                return false;
            }
        }
        VerificationHandle.Reset();
        Verification.Reset();

        // A model written early in a large world may be edited while later models are being
        // serialized and verified. Recheck the complete source snapshot at the publication
        // boundary so a successful build always describes one stable authoring generation.
        for (const FGWorldBuildModel* Model : Sorted)
        {
            if (Cancelled())
            {
                OutError = TEXT("The .v3d build was cancelled before publication");
                return false;
            }
            if (!Model
                || FileManager.FileSize(*Model->Definition.GlbPath) != Model->SourceFileSize
                || (Model->SourceTimestamp != FDateTime()
                    && FileManager.GetTimeStamp(*Model->Definition.GlbPath)
                        != Model->SourceTimestamp))
            {
                OutError = FString::Printf(
                    TEXT("Source GLB changed before archive publication: %s"),
                    Model ? *Model->Definition.GlbPath : TEXT("<null>"));
                return false;
            }
        }
        for (const FGWorldBuildSound* Sound : SortedSounds)
        {
            if (!Sound || FileManager.FileSize(*Sound->Definition.WavPath) != Sound->SourceFileSize
                || (Sound->SourceTimestamp != FDateTime()
                    && FileManager.GetTimeStamp(*Sound->Definition.WavPath) != Sound->SourceTimestamp))
            {
                OutError = FString::Printf(TEXT("Source WAV changed before archive publication: %s"),
                    Sound ? *Sound->Definition.WavPath : TEXT("<null>"));
                return false;
            }
        }

        if (Cancelled()) { OutError = TEXT("The .v3d build was cancelled before publication"); return false; }
        const bool bHadPrevious = FileManager.FileExists(*ArchivePath);
        const FString BackupPath = ArchivePath + TEXT(".bak.") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        if (bHadPrevious && !FileManager.Move(*BackupPath, *ArchivePath, true, true, false, true))
        {
            OutError = TEXT("Could not preserve the previous .v3d before publication");
            return false;
        }
        if (!FileManager.Move(*ArchivePath, *TemporaryPath, true, true, false, true))
        {
            if (bHadPrevious) FileManager.Move(*ArchivePath, *BackupPath, true, true, false, true);
            OutError = TEXT("Could not publish the verified .v3d file");
            return false;
        }
        bPublished = true;
        TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Published = FGWorldArchiveReader::Open(ArchivePath, VerifyError);
        FString PublishedConfig;
        if (!Published.IsValid() || !Published->ReadWorldConfig(PublishedConfig, VerifyError) || PublishedConfig != ConfigJson)
        {
            FileManager.Delete(*ArchivePath, false, true, true);
            if (bHadPrevious) FileManager.Move(*ArchivePath, *BackupPath, true, true, false, true);
            OutError = FString::Printf(TEXT("Published .v3d verification failed: %s"), *VerifyError);
            return false;
        }
        if (bHadPrevious) FileManager.Delete(*BackupPath, false, true, true);
        OutArchivePath = ArchivePath;
        return true;
    }
}

bool FGWorldModelSummary::IsSane(FString* OutError) const
{
    using namespace GWorldArchivePrivate;
    const auto Fail = [OutError](const TCHAR* Message)
    {
        if (OutError) *OutError = Message;
        return false;
    };

    if (SourceNodeCount < 0 || SourceNodeCount > MaxNodesPerModel
        || SourceMeshCount < 0 || SourceMeshCount > MaxMeshesPerModel
        || SourceMaterialCount < 0 || SourceMaterialCount > MaxMeshesPerModel
        || SourceTextureCount < 0 || SourceTextureCount > MaxMeshesPerModel)
    {
        return Fail(TEXT("source object counts are outside their safety limits"));
    }
    if (!IsFiniteVector(Center) || !IsFiniteVector(Size) || Size.GetMin() < 0.0)
    {
        return Fail(TEXT("model bounds are invalid"));
    }
    return true;
}

bool FGWorldModelMetadata::IsSane(FString* OutError) const
{
    using namespace GWorldArchivePrivate;
    FGWorldModelSummary Summary;
    Summary.SourceNodeCount = SourceNodeCount;
    Summary.SourceMeshCount = SourceMeshCount;
    Summary.SourceMaterialCount = SourceMaterialCount;
    Summary.SourceTextureCount = SourceTextureCount;
    Summary.Center = SceneData.ModelData.Center;
    Summary.Size = SceneData.ModelData.Size;
    if (!Summary.IsSane(OutError)) return false;

    const auto Fail = [OutError](const TCHAR* Message)
    {
        if (OutError) *OutError = Message;
        return false;
    };
    if (NodeTransforms.Num() != SourceNodeCount
        || SceneData.NodeMap.Num() > MaxNodesPerModel || SceneData.WaterNodeMap.Num() > MaxNodesPerModel
        || SceneData.MeshMap.Num() > MaxMeshesPerModel || SceneData.ModelData.MeshData.Num() > MaxMeshesPerModel)
    {
        return Fail(TEXT("model metadata collection counts are invalid"));
    }

    // NodeIndex is the stable identity because glTF names may be empty or duplicated. Validate the
    // complete graph with a compact bit array and reject dangling parent/mesh references before it
    // can be consumed by gameplay code.
    TBitArray<> SeenNodeIndices(false, SourceNodeCount);
    TArray<int32> ParentByNode;
    ParentByNode.Init(INDEX_NONE, SourceNodeCount);
    for (const FGWorldNodeTransform& Node : NodeTransforms)
    {
        if (Node.NodeIndex < 0 || Node.NodeIndex >= SourceNodeCount
            || SeenNodeIndices[Node.NodeIndex]
            || (Node.ParentIndex != INDEX_NONE
                && (Node.ParentIndex < 0 || Node.ParentIndex >= SourceNodeCount
                    || Node.ParentIndex == Node.NodeIndex))
            || (Node.MeshIndex != INDEX_NONE
                && (Node.MeshIndex < 0 || Node.MeshIndex >= SourceMeshCount))
            || Node.SkinIndex < INDEX_NONE
            || !IsFiniteTransform(Node.LocalTransform))
        {
            return Fail(TEXT("the complete glTF node-transform table is invalid"));
        }
        SeenNodeIndices[Node.NodeIndex] = true;
        ParentByNode[Node.NodeIndex] = Node.ParentIndex;
    }

    // Parent bounds alone do not rule out A->B->A. A three-state walk rejects every cycle in O(N)
    // without recursion, so a malicious hierarchy cannot overflow the stack of later consumers.
    TArray<uint8> VisitState;
    VisitState.Init(0, SourceNodeCount); // 0=unseen, 1=current chain, 2=finished.
    for (int32 StartNode = 0; StartNode < SourceNodeCount; ++StartNode)
    {
        int32 NodeIndex = StartNode;
        while (NodeIndex != INDEX_NONE && VisitState[NodeIndex] == 0)
        {
            VisitState[NodeIndex] = 1;
            NodeIndex = ParentByNode[NodeIndex];
        }
        if (NodeIndex != INDEX_NONE && VisitState[NodeIndex] == 1)
        {
            return Fail(TEXT("the complete glTF node-transform table contains a parent cycle"));
        }

        NodeIndex = StartNode;
        while (NodeIndex != INDEX_NONE && VisitState[NodeIndex] == 1)
        {
            VisitState[NodeIndex] = 2;
            NodeIndex = ParentByNode[NodeIndex];
        }
    }
    if (static_cast<int64>(SceneData.NodeMap.Num()) + SceneData.WaterNodeMap.Num() > SourceNodeCount
        || SceneData.MeshMap.Num() > SourceMeshCount)
    {
        return Fail(TEXT("baked metadata refers to more nodes or meshes than the source GLB"));
    }
    for (const TPair<FName, FModelNodeData>& Pair : SceneData.NodeMap)
    {
        const FModelMeshData* Mesh = SceneData.MeshMap.Find(Pair.Value.MeshName);
        if (Pair.Key.IsNone() || Pair.Value.MeshName.IsNone()
            || !IsFiniteTransform(Pair.Value.Transform) || !Mesh)
        {
            return Fail(TEXT("a model node has an invalid name, mesh key or transform"));
        }

        if (Pair.Value.FineChunk.X < 0 || Pair.Value.FineChunk.X >= FinePerCoarse
            || Pair.Value.FineChunk.Y < 0 || Pair.Value.FineChunk.Y >= FinePerCoarse
            || Pair.Value.FineChunk.Z < 0 || Pair.Value.FineChunk.Z >= FinePerCoarse)
        {
            return Fail(TEXT("a model node has an invalid 512 m fine-chunk coordinate"));
        }

        const FBox TransformedMeshBounds =
            FBox(-Mesh->Extent.GetAbs(), Mesh->Extent.GetAbs()).TransformBy(Pair.Value.Transform);
        const FVector EffectiveSize = TransformedMeshBounds.IsValid
            ? TransformedMeshBounds.GetSize().GetAbs()
            : FVector::ZeroVector;
        if (!TransformedMeshBounds.IsValid || !IsFiniteVector(EffectiveSize))
            return Fail(TEXT("a model node has a non-finite transformed mesh size"));

        if (!Pair.Value.bAlwaysLoaded)
        {
            FIntVector ExpectedCoarse = FIntVector::ZeroValue;
            FIntVector ExpectedFine = FIntVector::ZeroValue;
            if (EffectiveSize.GetMax() > CoarseChunkCm
                || !TryComputeSpatialChunk(
                    Pair.Value.Transform.GetLocation(), ExpectedCoarse, ExpectedFine)
                || Pair.Value.CoarseChunk != ExpectedCoarse
                || Pair.Value.FineChunk != ExpectedFine)
            {
                return Fail(TEXT("a streamed model node has inconsistent hierarchical chunk metadata"));
            }
        }
    }
    for (const TPair<FName, FWaterStreamNodeData>& Pair : SceneData.WaterNodeMap)
    {
        if (Pair.Key.IsNone() || !IsFiniteTransform(Pair.Value.Transform)
            || !FMath::IsFinite(Pair.Value.StreamRadius) || Pair.Value.StreamRadius <= 0.0f)
            return Fail(TEXT("a water node has an invalid name, transform or radius"));
    }
    for (const TPair<FName, FModelMeshData>& Pair : SceneData.MeshMap)
    {
        const auto IsValidLod = [this](const int32 Index)
        {
            return Index == INDEX_NONE || (Index >= 0 && Index < SourceMeshCount);
        };
        const bool bHasLod = Pair.Value.LOD0 != INDEX_NONE || Pair.Value.LOD1 != INDEX_NONE
            || Pair.Value.LOD2 != INDEX_NONE || Pair.Value.LOD3 != INDEX_NONE;
        if (Pair.Key.IsNone() || !IsFiniteVector(Pair.Value.Extent) || !IsFiniteVector(Pair.Value.Size)
            || Pair.Value.Extent.GetMin() < 0.0 || Pair.Value.Size.GetMin() < 0.0
            || !bHasLod || !IsValidLod(Pair.Value.LOD0) || !IsValidLod(Pair.Value.LOD1)
            || !IsValidLod(Pair.Value.LOD2) || !IsValidLod(Pair.Value.LOD3)
            || !IsSaneMeshSettings(Pair.Value.Data))
            return Fail(TEXT("a mesh row has an invalid key or size"));
    }
    for (const TPair<FName, FMeshData>& Pair : SceneData.ModelData.MeshData)
    {
        if (Pair.Key.IsNone() || !IsSaneMeshSettings(Pair.Value))
            return Fail(TEXT("a model mesh setting row is invalid"));
    }
    return true;
}

bool FGWorldModelManifest::IsSane(
    const FGWorldModelSummary& Summary,
    FString* OutError) const
{
    const auto Fail = [OutError](const TCHAR* Message)
    {
        if (OutError) *OutError = Message;
        return false;
    };
    if (!Summary.IsSane(OutError)
        || MeshRanges.Num() != Summary.SourceMeshCount
        || MaterialRanges.Num() != Summary.SourceMaterialCount
        || TextureRanges.Num() != Summary.SourceTextureCount
        || MeshNames.Num() != MeshRanges.Num()
        || SkinRanges.Num() > GWorldArchivePrivate::MaxBonesPerModel)
    {
        return Fail(TEXT("manifest collection counts do not match the model summary"));
    }

    TSet<FString> Names;
    const auto ValidateMap = [&Names](
        const TMap<int32, FGWorldArchiveRange>& Map,
        const FString& Prefix)
    {
        for (const TPair<int32, FGWorldArchiveRange>& Pair : Map)
        {
            const FGWorldArchiveRange& Range = Pair.Value;
            if (Pair.Key < 0 || Range.Name.IsEmpty()
                || !Range.Name.StartsWith(Prefix, ESearchCase::CaseSensitive)
                || !Range.Name.EndsWith(TEXT(".dat"), ESearchCase::CaseSensitive)
                || Range.Offset < static_cast<uint64>(GWorldArchivePrivate::HeaderBytes)
                || Range.Offset % static_cast<uint64>(GWorldArchivePrivate::PayloadAlignment) != 0
                || Range.StoredSize == 0
                || Range.UncompressedSize == 0
                || Range.StoredSize > static_cast<uint64>(FGWorldArchive::MaxDatMemberBytes)
                || Range.UncompressedSize > static_cast<uint64>(FGWorldArchive::MaxDatMemberBytes)
                || Range.Codec > 1
                || (Range.Codec == 0 && Range.StoredSize != Range.UncompressedSize)
                || (Range.Codec == 1 && Range.StoredSize >= Range.UncompressedSize)
                || Range.Name.Contains(TEXT("..")) || Range.Name.Contains(TEXT("\\"))
                || Names.Contains(Range.Name))
            {
                return false;
            }
            Names.Add(Range.Name);
        }
        return true;
    };
    if (!ValidateMap(MeshRanges, TEXT("models/"))
        || !ValidateMap(SkinRanges, TEXT("models/"))
        || !ValidateMap(MaterialRanges, TEXT("models/"))
        || !ValidateMap(TextureRanges, TEXT("models/")))
    {
        return Fail(TEXT("manifest contains an invalid or duplicate .dat member range"));
    }
    for (int32 MeshIndex = 0; MeshIndex < Summary.SourceMeshCount; ++MeshIndex)
    {
        const FString* MeshName = MeshNames.Find(MeshIndex);
        if (!MeshRanges.Contains(MeshIndex) || !MeshName || MeshName->IsEmpty())
            return Fail(TEXT("manifest mesh indices are not contiguous"));
    }
    for (int32 MaterialIndex = 0; MaterialIndex < Summary.SourceMaterialCount; ++MaterialIndex)
    {
        if (!MaterialRanges.Contains(MaterialIndex))
            return Fail(TEXT("manifest material indices are not contiguous"));
    }
    for (int32 TextureIndex = 0; TextureIndex < Summary.SourceTextureCount; ++TextureIndex)
    {
        if (!TextureRanges.Contains(TextureIndex))
            return Fail(TEXT("manifest texture indices are not contiguous"));
    }
    if (OutError) OutError->Reset();
    return true;
}

FString FGWorldArchive::MakeArchivePath(const FString& WorldRoot)
{
    const FString Normalized = FSafeFileIO::NormalizeFilePath(WorldRoot);
    if (Normalized.IsEmpty()) return FString();
    if (Normalized.EndsWith(TEXT(".v3d"), ESearchCase::CaseSensitive)) return Normalized;
    const FString WorldName = FPaths::GetCleanFilename(Normalized);
    const FString Parent = FPaths::GetPath(Normalized);
    return WorldName.IsEmpty() ? FString() : FPaths::Combine(Parent, WorldName + TEXT(".v3d"));
}

FString FGWorldArchive::MakeModelReference(const FGuid& UUID)
{
    return UUID.IsValid()
        ? FString::Printf(TEXT("gworld://model/%s"), *UUID.ToString(EGuidFormats::DigitsWithHyphensLower))
        : FString();
}

bool FGWorldArchive::ParseModelReference(const FString& Reference, FGuid& OutUUID)
{
    OutUUID.Invalidate();
    constexpr TCHAR Prefix[] = TEXT("gworld://model/");
    FString Clean = Reference.TrimStartAndEnd();
    if (!Clean.StartsWith(Prefix, ESearchCase::CaseSensitive)) return false;
    Clean.RightChopInline(UE_ARRAY_COUNT(Prefix) - 1, EAllowShrinking::No);
    if (!FGuid::Parse(Clean, OutUUID) || !OutUUID.IsValid()) return false;
    return Clean == OutUUID.ToString(EGuidFormats::DigitsWithHyphensLower);
}

FString FGWorldArchive::MakeSoundReference(const FGuid& UUID)
{
    return UUID.IsValid()
        ? FString::Printf(TEXT("gworld://sound/%s"), *UUID.ToString(EGuidFormats::DigitsWithHyphensLower))
        : FString();
}

bool FGWorldArchive::ParseSoundReference(const FString& Reference, FGuid& OutUUID)
{
    OutUUID.Invalidate();
    constexpr TCHAR Prefix[] = TEXT("gworld://sound/");
    FString Clean = Reference.TrimStartAndEnd();
    if (!Clean.StartsWith(Prefix, ESearchCase::CaseSensitive)) return false;
    Clean.RightChopInline(UE_ARRAY_COUNT(Prefix) - 1, EAllowShrinking::No);
    if (!FGuid::Parse(Clean, OutUUID) || !OutUUID.IsValid()) return false;
    return Clean == OutUUID.ToString(EGuidFormats::DigitsWithHyphensLower);
}

bool FGWorldArchive::BuildBlocking(
    const FString& WorldRoot,
    const TArray<FGWorldBuildModel>& Models,
    const TArray<FGWorldBuildSound>& Sounds,
    FString& OutArchivePath,
    FString& OutError,
    TFunction<bool()> ShouldCancel,
    const FString& WorldConfigJson,
    const FV3DProjectScripts& Scripts)
{
    using namespace GWorldArchivePrivate;
    return GWorldArchivePrivate::BuildBlocking(
        MakeArchivePath(WorldRoot), Models, Sounds, OutArchivePath, OutError, ShouldCancel, WorldConfigJson, Scripts);
}

bool FGWorldArchive::BuildBlockingToArchivePath(
    const FString& ArchivePath,
    const TArray<FGWorldBuildModel>& Models,
    const TArray<FGWorldBuildSound>& Sounds,
    FString& OutArchivePath,
    FString& OutError,
    TFunction<bool()> ShouldCancel,
    const FString& ArchiveConfigJson,
    const FV3DProjectScripts& Scripts)
{
    using namespace GWorldArchivePrivate;
    return GWorldArchivePrivate::BuildBlocking(
        ArchivePath, Models, Sounds, OutArchivePath, OutError, ShouldCancel, ArchiveConfigJson, Scripts);
}

TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> FGWorldArchiveReader::Open(
    const FString& ArchivePath,
    FString& OutError)
{
    using namespace GWorldArchivePrivate;
    OutError.Reset();
    const FString NormalizedPath = FSafeFileIO::NormalizeFilePath(ArchivePath);
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*NormalizedPath));
    if (!Handle.IsValid())
    {
        OutError = FString::Printf(TEXT("The archive file is missing or unreadable: %s"), *NormalizedPath);
        return nullptr;
    }
    const int64 TotalSize = Handle->Size();
    if (TotalSize < HeaderBytes || TotalSize > MaxArchiveBytes)
    {
        OutError = TEXT("The archive file is outside its bounded size");
        return nullptr;
    }

    TArray<uint8> HeaderBytesArray;
    HeaderBytesArray.SetNumUninitialized(HeaderBytes);
    if (!ReadExact(*Handle, HeaderBytesArray.GetData(), HeaderBytesArray.Num()))
    {
        OutError = TEXT("The .v3d header could not be read");
        return nullptr;
    }
    v3d::archive_header Header;
    std::string HeaderError;
    if (!v3d::decode_header(HeaderBytesArray.GetData(), HeaderBytesArray.Num(),
            static_cast<uint64>(TotalSize), Header, HeaderError))
    {
        OutError = ToUnreal(HeaderError);
        return nullptr;
    }
    const uint32 SoundCount = Header.sound_count;
    const uint64 DirectoryOffset = Header.directory_offset;
    const uint64 DirectorySize = Header.directory_size;
    const uint32 DirectoryCrc = Header.directory_crc;
    const uint32 ModelCount = Header.model_count;
    const FGuid BuildId = ToUnreal(Header.build_id);
    const uint64 ReadFormatRevision = Header.revision;
    if (ModelCount > FGWorldArchive::MaxModels || SoundCount > FGWorldArchive::MaxSounds
        || (ModelCount == 0 && SoundCount == 0)
        || DirectorySize > static_cast<uint64>(FGWorldArchive::MaxDirectoryBytes)
        || DirectorySize > static_cast<uint64>(MAX_int32))
    {
        OutError = TEXT("The V3D archive exceeds the simulator's model/sound/directory limits");
        return nullptr;
    }

    TArray<uint8> Directory;
    Directory.SetNumUninitialized(static_cast<int32>(DirectorySize));
    if (!Handle->Seek(static_cast<int64>(DirectoryOffset))
        || !ReadExact(*Handle, Directory.GetData(), Directory.Num()))
    {
        OutError = TEXT("The .v3d directory range could not be read");
        return nullptr;
    }
    TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Result =
        MakeShared<FGWorldArchiveReader, ESPMode::ThreadSafe>();
    if (!DeserializeDirectory(Directory, DirectoryOffset, ModelCount, SoundCount, ReadFormatRevision, DirectoryCrc,
            Result->WorldConfigRange, Result->ScriptRanges, Result->Records, Result->SoundRecords, OutError))
    {
        return nullptr;
    }
    Result->Path = NormalizedPath;
    Result->BuildId = BuildId;
    Result->FileSize = TotalSize;
    Result->DataEndOffset = DirectoryOffset;
    Result->RootMemberRanges.Reserve(Result->Records.Num() * 3 + Result->SoundRecords.Num() * 2 + 1 + Result->ScriptRanges.Num());
    Result->RootMemberRanges.Emplace(Result->WorldConfigRange.Offset, Result->WorldConfigRange.StoredSize);
    for (const auto& Range : Result->ScriptRanges) Result->RootMemberRanges.Emplace(Range.Offset, Range.StoredSize);
    for (const TPair<FGuid, FGWorldModelRecord>& Pair : Result->Records)
    {
        const FGWorldModelRecord& Record = Pair.Value;
        Result->RootMemberRanges.Emplace(
            Record.DefinitionRange.Offset, Record.DefinitionRange.StoredSize);
        Result->RootMemberRanges.Emplace(
            Record.MetadataRange.Offset, Record.MetadataRange.StoredSize);
        Result->RootMemberRanges.Emplace(
            Record.ManifestRange.Offset, Record.ManifestRange.StoredSize);
    }
    for (const TPair<FGuid, FGWorldSoundRecord>& Pair : Result->SoundRecords)
    {
        const FGWorldSoundRecord& Record = Pair.Value;
        Result->RootMemberRanges.Emplace(Record.DefinitionRange.Offset, Record.DefinitionRange.StoredSize);
        Result->RootMemberRanges.Emplace(Record.WaveRange.Offset, Record.WaveRange.StoredSize);
    }
    Result->RootMemberRanges.Sort(
        [](const TPair<uint64, uint64>& A, const TPair<uint64, uint64>& B)
        {
            return A.Key < B.Key;
        });
    return Result;
}

void FGWorldArchiveReader::GetRecords(TArray<FGWorldModelRecord>& OutRecords) const
{
    Records.GenerateValueArray(OutRecords);
    OutRecords.Sort([](const FGWorldModelRecord& A, const FGWorldModelRecord& B)
    {
        return GWorldArchivePrivate::IsGuidLess(A.Definition.UUID, B.Definition.UUID);
    });
}

void FGWorldArchiveReader::GetSoundRecords(TArray<FGWorldSoundRecord>& OutRecords) const
{
    SoundRecords.GenerateValueArray(OutRecords);
    OutRecords.Sort([](const FGWorldSoundRecord& A, const FGWorldSoundRecord& B)
    {
        return GWorldArchivePrivate::IsGuidLess(A.Definition.UUID, B.Definition.UUID);
    });
}

bool FGWorldArchiveReader::FindSoundRecord(const FGuid& UUID, FGWorldSoundRecord& OutRecord) const
{
    const FGWorldSoundRecord* Record = SoundRecords.Find(UUID);
    if (!Record) return false;
    OutRecord = *Record;
    return true;
}

bool FGWorldArchiveReader::FindRecord(const FGuid& UUID, FGWorldModelRecord& OutRecord) const
{
    const FGWorldModelRecord* Record = Records.Find(UUID);
    if (!Record) return false;
    OutRecord = *Record;
    return true;
}

bool FGWorldArchiveReader::FindRecordByReference(
    const FString& Reference,
    FGWorldModelRecord& OutRecord) const
{
    FGuid UUID;
    return FGWorldArchive::ParseModelReference(Reference, UUID) && FindRecord(UUID, OutRecord);
}

bool FGWorldArchiveReader::GetSummary(
    const FGuid& UUID,
    FGWorldModelSummary& OutSummary) const
{
    const FGWorldModelRecord* Record = Records.Find(UUID);
    if (!Record) return false;
    OutSummary = Record->Summary;
    return true;
}

bool FGWorldArchiveReader::ReadProjectScripts(FV3DProjectScripts& OutScripts, FString& OutError) const
{
    using namespace GWorldArchivePrivate;
    OutError.Reset(); OutScripts = FV3DProjectScripts();
    if (ScriptRanges.IsEmpty()) return true; // Revision 4: caller reads legacy surface fields.
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*Path));
    if (!Handle.IsValid()) { OutError = TEXT("Could not open .v3d script members"); return false; }
    FV3DProjectScripts Parsed;
    for (int32 Index = 0; Index < ScriptRanges.Num(); ++Index)
    {
        TArray<uint8> Bytes;
        if (!ReadChecksummedRange(*Handle, FileSize, DataEndOffset, ScriptRanges[Index],
                FV3DProjectScripts::MaxScriptBytes + 8, Bytes, OutError)
            || !DeserializeWorldConfig(Bytes, Parsed.Text(Index), OutError)) return false;
    }
    OutScripts = MoveTemp(Parsed); return true;
}

bool FGWorldArchiveReader::ReadWorldConfig(FString& OutConfigJson, FString& OutError) const
{
    using namespace GWorldArchivePrivate;
    OutConfigJson.Reset();
    TArray<uint8> Bytes;
    return ReadChecksummedRange(Path, FileSize, DataEndOffset, WorldConfigRange,
            FGWorldArchive::MaxDirectoryBytes, Bytes, OutError)
        && DeserializeWorldConfig(Bytes, OutConfigJson, OutError);
}

bool FGWorldArchiveReader::ReadSoundDefinition(
    const FGuid& UUID,
    FString& OutDefinitionJson,
    FString& OutError) const
{
    using namespace GWorldArchivePrivate;
    OutDefinitionJson.Reset();
    const FGWorldSoundRecord* Record = SoundRecords.Find(UUID);
    if (!Record)
    {
        OutError = TEXT("Sound UUID is not present in this .v3d archive");
        return false;
    }
    TArray<uint8> Bytes;
    if (!ReadChecksummedRange(Path, FileSize, DataEndOffset, Record->DefinitionRange,
            FGWorldArchive::MaxDirectoryBytes, Bytes, OutError)) return false;
    if (Bytes.IsEmpty() || Bytes.Contains(0))
    {
        OutError = TEXT("Sound definition JSON is empty or contains embedded NUL bytes");
        return false;
    }
    FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    OutDefinitionJson = FString(Utf8.Length(), Utf8.Get());
    if (OutDefinitionJson.TrimStartAndEnd().IsEmpty())
    {
        OutError = TEXT("Sound definition JSON decoded to an empty string");
        return false;
    }
    return true;
}

bool FGWorldArchiveReader::ReadSoundWave(
    const FGuid& UUID,
    TArray<uint8>& OutWaveBytes,
    FString& OutError) const
{
    using namespace GWorldArchivePrivate;
    OutWaveBytes.Reset();
    const FGWorldSoundRecord* Record = SoundRecords.Find(UUID);
    if (!Record)
    {
        OutError = TEXT("Sound UUID is not present in this .v3d archive");
        return false;
    }
    if (!ReadChecksummedRange(Path, FileSize, DataEndOffset, Record->WaveRange,
            FGWorldArchive::MaxSoundBytes, OutWaveBytes, OutError)) return false;
    if (OutWaveBytes.Num() < 12
        || FMemory::Memcmp(OutWaveBytes.GetData(), "RIFF", 4) != 0
        || FMemory::Memcmp(OutWaveBytes.GetData() + 8, "WAVE", 4) != 0)
    {
        OutWaveBytes.Reset();
        OutError = TEXT("Sound member is not a RIFF/WAVE file");
        return false;
    }
    return true;
}

bool FGWorldArchiveReader::ReadModelDefinition(
    const FGuid& UUID,
    TMap<FString, FString>& OutBones,
    FString& OutDefinitionJson,
    FString& OutError) const
{
    using namespace GWorldArchivePrivate;
    OutBones.Reset();
    OutDefinitionJson.Reset();
    OutError.Reset();
    const FGWorldModelRecord* Record = Records.Find(UUID);
    if (!Record)
    {
        OutError = FString::Printf(
            TEXT("Model UUID is absent from the .v3d directory: %s"), *UUID.ToString());
        return false;
    }

    TArray<uint8> Bytes;
    return ReadChecksummedRange(
            Path, FileSize, DataEndOffset, Record->DefinitionRange, FGWorldArchive::MaxDirectoryBytes,
            Bytes, OutError)
        && DeserializeDefinitionData(Bytes, OutBones, OutDefinitionJson, OutError);
}

bool FGWorldArchiveReader::ReadModelMetadata(
    const FGuid& UUID,
    FGWorldModelMetadata& OutMetadata,
    FString& OutError) const
{
    using namespace GWorldArchivePrivate;
    OutMetadata = FGWorldModelMetadata();
    OutError.Reset();
    const FGWorldModelRecord* Record = Records.Find(UUID);
    if (!Record)
    {
        OutError = FString::Printf(
            TEXT("Model UUID is absent from the .v3d directory: %s"), *UUID.ToString());
        return false;
    }

    TArray<uint8> Bytes;
    if (!ReadChecksummedRange(
            Path, FileSize, DataEndOffset, Record->MetadataRange, FGWorldArchive::MaxDirectoryBytes,
            Bytes, OutError)
        || !DeserializeMetadataData(Bytes, OutMetadata, OutError))
    {
        return false;
    }
    if (OutMetadata.SourceNodeCount != Record->Summary.SourceNodeCount
        || OutMetadata.SourceMeshCount != Record->Summary.SourceMeshCount
        || OutMetadata.SourceMaterialCount != Record->Summary.SourceMaterialCount
        || OutMetadata.SourceTextureCount != Record->Summary.SourceTextureCount
        || OutMetadata.SceneData.ModelData.Center != Record->Summary.Center
        || OutMetadata.SceneData.ModelData.Size != Record->Summary.Size)
    {
        OutMetadata = FGWorldModelMetadata();
        OutError = TEXT("The streamed metadata does not match its .v3d directory summary");
        return false;
    }
    return true;
}

bool FGWorldArchiveReader::ReadModelManifest(
    const FGuid& UUID,
    FGWorldModelManifest& OutManifest,
    FString& OutError) const
{
    using namespace GWorldArchivePrivate;
    OutManifest = FGWorldModelManifest();
    OutError.Reset();
    const FGWorldModelRecord* Record = Records.Find(UUID);
    if (!Record)
    {
        OutError = FString::Printf(TEXT("Model UUID is absent from .v3d: %s"), *UUID.ToString());
        return false;
    }
    TArray<uint8> Bytes;
    if (!ReadChecksummedRange(Path, FileSize, DataEndOffset, Record->ManifestRange,
            FGWorldArchive::MaxDirectoryBytes, Bytes, OutError)
        || !WorldArchiveCodec::DeserializeManifest(Bytes, OutManifest, OutError)
        || !OutManifest.IsSane(Record->Summary, &OutError))
    {
        OutManifest = FGWorldModelManifest();
        return false;
    }

    const FString ExpectedPrefix = FString::Printf(TEXT("models/%s/"),
        *UUID.ToString(EGuidFormats::Digits));
    TArray<TPair<uint64, uint64>> Ranges;
    const auto ValidateMap = [this, &ExpectedPrefix, &Ranges](
        const TMap<int32, FGWorldArchiveRange>& Map,
        const TCHAR* Category)
    {
        for (const TPair<int32, FGWorldArchiveRange>& Pair : Map)
        {
            const FGWorldArchiveRange& Range = Pair.Value;
            const FString ExpectedName = ExpectedPrefix + Category
                + FString::Printf(TEXT("%d.dat"), Pair.Key);
            if (!Range.Name.Equals(ExpectedName, ESearchCase::CaseSensitive)
                || Range.Offset > DataEndOffset || Range.StoredSize > DataEndOffset - Range.Offset)
                return false;
            Ranges.Emplace(Range.Offset, Range.StoredSize);
        }
        return true;
    };
    if (!ValidateMap(OutManifest.MeshRanges, TEXT("meshes/"))
        || !ValidateMap(OutManifest.SkinRanges, TEXT("skins/"))
        || !ValidateMap(OutManifest.MaterialRanges, TEXT("materials/"))
        || !ValidateMap(OutManifest.TextureRanges, TEXT("textures/")))
    {
        OutManifest = FGWorldModelManifest();
        OutError = TEXT("A manifest .dat range escapes its model namespace or archive data area");
        return false;
    }
    Ranges.Sort([](const TPair<uint64,uint64>& A,const TPair<uint64,uint64>& B){return A.Key<B.Key;});
    for (int32 Index = 1; Index < Ranges.Num(); ++Index)
    {
        if (Ranges[Index-1].Key + Ranges[Index-1].Value > Ranges[Index].Key)
        {
            OutManifest = FGWorldModelManifest();
            OutError = TEXT("Manifest .dat ranges overlap");
            return false;
        }
    }

    // Child members must also stay disjoint from every model's definition/metadata/manifest rows.
    // Both interval lists are sorted, so one cursor replaces the previous model-count × child-count
    // search and keeps large-world manifest initialization predictable.
    const auto Overlaps = [](const uint64 AOffset, const uint64 ASize,
        const uint64 BOffset, const uint64 BSize)
    {
        return AOffset < BOffset + BSize && BOffset < AOffset + ASize;
    };
    int32 RootIndex = 0;
    for (const TPair<uint64, uint64>& Child : Ranges)
    {
        while (RootMemberRanges.IsValidIndex(RootIndex)
            && RootMemberRanges[RootIndex].Key + RootMemberRanges[RootIndex].Value
                <= Child.Key)
        {
            ++RootIndex;
        }
        if (RootMemberRanges.IsValidIndex(RootIndex)
            && Overlaps(Child.Key, Child.Value,
                RootMemberRanges[RootIndex].Key, RootMemberRanges[RootIndex].Value))
        {
            OutManifest = FGWorldModelManifest();
            OutError = TEXT("A manifest .dat member overlaps an archive root member");
            return false;
        }
    }
    return true;
}

bool FGWorldArchiveReader::ReadTextureRange(
    const FGWorldArchiveRange& Range, FGWorldBakedTexture& OutTexture,
    FString& OutError, const int32 MaxResolution) const
{
    OutTexture = FGWorldBakedTexture();
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*Path));
    TArray<uint8> Bytes;
    if (!Handle.IsValid() || Handle->Size() != FileSize)
    {
        OutError = TEXT("Texture backing archive changed or is unavailable");
        return false;
    }
    return GWorldArchivePrivate::ReadChecksummedRange(*Handle, FileSize, DataEndOffset, Range,
        FGWorldArchive::MaxDatMemberBytes, Bytes, OutError)
        && WorldArchiveCodec::DeserializeTexture(Bytes, OutTexture, OutError, MaxResolution);
}

bool FGWorldArchiveReader::ReadMeshBundle(
    const FGuid& UUID,
    const FGWorldModelManifest& Manifest,
    const TArray<int32>& MeshIndices,
    const int32 SkinIndex,
    const bool bLoadMaterialDependencies,
    FGWorldBakedAssetBundle& OutBundle,
    FString& OutError,
    const TSet<int32>* SkipTextureIds,
    const TSet<int32>* SkipMaterialIds,
    const int32 MaxTextureResolution,
    const bool bDependenciesOnly) const
{
    using namespace GWorldArchivePrivate;
    OutBundle.Reset();
    OutBundle.MaxTextureResolution = MaxTextureResolution;
    OutError.Reset();
    const FGWorldModelRecord* Record = Records.Find(UUID);
    const bool bManifestCountsMatch = Record
        && Manifest.MeshRanges.Num() == Record->Summary.SourceMeshCount
        && Manifest.MaterialRanges.Num() == Record->Summary.SourceMaterialCount
        && Manifest.TextureRanges.Num() == Record->Summary.SourceTextureCount;
    if (!Record || MeshIndices.IsEmpty() || !bManifestCountsMatch)
    {
        OutError = TEXT("Invalid model, manifest counts, or empty mesh request");
        return false;
    }

    // Runtime bundle reads are pure file/CPU work and are frequently dominated by texture members.
    // Do NOT open one OS file handle per .dat member: large materials can fan out to hundreds of
    // texture ranges and handle churn becomes measurable even on NVMe. Instead partition each stage
    // into a few independent shards. Every shard owns exactly one seek cursor and consumes several
    // members serially, while CRC + binary deserialization still run across shards in parallel.
    // Keeping the shard count modest is important because several baked-model requests may already
    // be running on the engine thread pool at the same time.
    auto ParallelReadShards = [this](const int32 ItemCount, auto&& ReadOne)
    {
        if (ItemCount <= 0)
        {
            return;
        }
        const int32 HardwareWorkers = FMath::Max(1, FPlatformMisc::NumberOfCores() - 1);
        const int32 ShardCount = FMath::Clamp(FMath::Min(ItemCount, HardwareWorkers), 1, 4);
        ParallelFor(ShardCount, [this, ItemCount, ShardCount, &ReadOne](const int32 ShardIndex)
        {
            IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
            TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*Path));
            FString OpenError;
            if (!Handle.IsValid() || Handle->Size() != FileSize)
            {
                OpenError = TEXT("The .v3d file changed or could not be opened for a sharded bundle read");
                Handle.Reset();
            }

            for (int32 Index = ShardIndex; Index < ItemCount; Index += ShardCount)
            {
                ReadOne(Index, Handle.Get(), OpenError);
            }
        });
    };

    auto ReadMemberBytes = [this](IFileHandle* Handle, const FGWorldArchiveRange& Range,
        TArray<uint8>& Bytes, FString& Error) -> bool
    {
        if (!Handle)
        {
            if (Error.IsEmpty())
            {
                Error = TEXT("The .v3d shard file handle is unavailable");
            }
            return false;
        }
        return ReadChecksummedRange(*Handle, FileSize, DataEndOffset, Range,
            FGWorldArchive::MaxDatMemberBytes, Bytes, Error);
    };

    TArray<int32> UniqueMeshes;
    UniqueMeshes = MeshIndices;
    UniqueMeshes.Sort();
    int32 UniqueCount = 0;
    for (const int32 MeshIndex : UniqueMeshes)
    {
        if (UniqueCount == 0 || UniqueMeshes[UniqueCount - 1] != MeshIndex)
        {
            UniqueMeshes[UniqueCount++] = MeshIndex;
        }
    }
    UniqueMeshes.SetNum(UniqueCount, EAllowShrinking::No);

    TArray<FGWorldBakedMesh> MeshResults;
    MeshResults.SetNum(UniqueMeshes.Num());
    TArray<FString> MeshErrors;
    MeshErrors.SetNum(UniqueMeshes.Num());
    TArray<uint8> MeshSuccess;
    MeshSuccess.Init(0, UniqueMeshes.Num());

    ParallelReadShards(UniqueMeshes.Num(),
        [this, &Manifest, &UniqueMeshes, &MeshResults, &MeshErrors, &MeshSuccess, &ReadMemberBytes, bDependenciesOnly](
            const int32 Index, IFileHandle* Handle, const FString& OpenError)
    {
        const int32 MeshIndex = UniqueMeshes[Index];
        const FGWorldArchiveRange* Range = Manifest.MeshRanges.Find(MeshIndex);
        if (!Range)
        {
            MeshErrors[Index] = TEXT("Requested mesh.dat is missing");
            return;
        }
        TArray<uint8> Bytes;
        FGWorldBakedMesh Mesh;
        FString Error = OpenError;
        if (!ReadMemberBytes(Handle, *Range, Bytes, Error)
            || !WorldArchiveCodec::DeserializeMesh(Bytes, Mesh, Error, bDependenciesOnly)
            || Mesh.MeshIndex != MeshIndex)
        {
            MeshErrors[Index] = Error.IsEmpty()
                ? TEXT("Requested mesh.dat has a mismatched id") : MoveTemp(Error);
            return;
        }
        MeshResults[Index] = MoveTemp(Mesh);
        MeshSuccess[Index] = 1;
    });

    TSet<int32> MaterialIds;
    for (int32 Index = 0; Index < MeshResults.Num(); ++Index)
    {
        if (MeshSuccess[Index] == 0)
        {
            OutError = MeshErrors[Index].IsEmpty()
                ? TEXT("Requested mesh.dat could not be read") : MeshErrors[Index];
            OutBundle.Reset();
            return false;
        }
        TSet<int32> MeshDependencies;
        for (const FGWorldBakedPrimitive& Primitive : MeshResults[Index].Primitives)
        {
            if (Primitive.MaterialId == INDEX_NONE)
            {
                continue;
            }
            if (!Manifest.MaterialRanges.Contains(Primitive.MaterialId))
            {
                OutBundle.Reset();
                OutError = TEXT("Requested mesh.dat refers to a missing material member");
                return false;
            }
            MeshDependencies.Add(Primitive.MaterialId);
            if (bLoadMaterialDependencies
                && (!SkipMaterialIds || !SkipMaterialIds->Contains(Primitive.MaterialId)))
            {
                MaterialIds.Add(Primitive.MaterialId);
            }
        }
        OutBundle.MeshMaterialDependencies.Add(MeshResults[Index].MeshIndex, MeshDependencies.Array());
    }
    if (!bDependenciesOnly) OutBundle.Meshes = MoveTemp(MeshResults);
    else MeshResults.Empty();

    TArray<int32> SortedMaterials = MaterialIds.Array();
    SortedMaterials.Sort();
    TArray<FGWorldBakedMaterial> MaterialResults;
    MaterialResults.SetNum(SortedMaterials.Num());
    TArray<FString> MaterialErrors;
    MaterialErrors.SetNum(SortedMaterials.Num());
    TArray<uint8> MaterialSuccess;
    MaterialSuccess.Init(0, SortedMaterials.Num());

    ParallelReadShards(SortedMaterials.Num(),
        [this, &Manifest, &SortedMaterials, &MaterialResults, &MaterialErrors, &MaterialSuccess, &ReadMemberBytes](
            const int32 Index, IFileHandle* Handle, const FString& OpenError)
    {
        const int32 MaterialId = SortedMaterials[Index];
        const FGWorldArchiveRange* Range = Manifest.MaterialRanges.Find(MaterialId);
        if (!Range)
        {
            MaterialErrors[Index] = TEXT("Referenced material.dat is missing");
            return;
        }
        TArray<uint8> Bytes;
        FGWorldBakedMaterial Material;
        FString Error = OpenError;
        if (!ReadMemberBytes(Handle, *Range, Bytes, Error)
            || !WorldArchiveCodec::DeserializeMaterial(Bytes, Material, Error)
            || Material.MaterialId != MaterialId)
        {
            MaterialErrors[Index] = Error.IsEmpty()
                ? TEXT("Referenced material.dat has a mismatched id") : MoveTemp(Error);
            return;
        }
        MaterialResults[Index] = MoveTemp(Material);
        MaterialSuccess[Index] = 1;
    });

    TSet<int32> TextureIds;
    for (int32 Index = 0; Index < MaterialResults.Num(); ++Index)
    {
        if (MaterialSuccess[Index] == 0)
        {
            OutError = MaterialErrors[Index].IsEmpty()
                ? TEXT("Referenced material.dat could not be read") : MaterialErrors[Index];
            OutBundle.Reset();
            return false;
        }
        for (const FGWorldBakedTextureParameter& Parameter : MaterialResults[Index].Textures)
        {
            if (Parameter.TextureId != INDEX_NONE)
            {
                if (!Manifest.TextureRanges.Contains(Parameter.TextureId))
                {
                    OutBundle.Reset();
                    OutError = TEXT("Referenced material.dat refers to a missing texture member");
                    return false;
                }
                TextureIds.Add(Parameter.TextureId);
            }
        }
    }
    OutBundle.Materials = MoveTemp(MaterialResults);

    // A facade-level runtime cache can guarantee that selected materials/textures remain alive.
    // Cached materials were removed before material.dat I/O above, which also removes all of their
    // texture dependencies from this request. For uncached materials, skip any texture.dat member
    // already retained by the texture cache.
    // Dependency ids were still validated against the manifest above before any id is skipped.
    if (SkipTextureIds)
    {
        for (const int32 TextureId : *SkipTextureIds)
        {
            TextureIds.Remove(TextureId);
        }
    }

    // Dependency discovery validates IDs/material members without allocating or reading pixels.
    // The later admitted payload read still performs the full texture CRC/layout validation.
    if (bDependenciesOnly) TextureIds.Reset();
    TArray<int32> SortedTextures = TextureIds.Array();
    SortedTextures.Sort();
    TArray<FGWorldBakedTexture> TextureResults;
    TextureResults.SetNum(SortedTextures.Num());
    TArray<FString> TextureErrors;
    TextureErrors.SetNum(SortedTextures.Num());
    TArray<uint8> TextureSuccess;
    TextureSuccess.Init(0, SortedTextures.Num());

    ParallelReadShards(SortedTextures.Num(),
        [this, &Manifest, &SortedTextures, &TextureResults, &TextureErrors, &TextureSuccess, &ReadMemberBytes, MaxTextureResolution](
            const int32 Index, IFileHandle* Handle, const FString& OpenError)
    {
        const int32 TextureId = SortedTextures[Index];
        const FGWorldArchiveRange* Range = Manifest.TextureRanges.Find(TextureId);
        if (!Range)
        {
            TextureErrors[Index] = TEXT("Referenced texture.dat is missing");
            return;
        }
        TArray<uint8> Bytes;
        FGWorldBakedTexture Texture;
        FString Error = OpenError;
        if (!ReadMemberBytes(Handle, *Range, Bytes, Error)
            || !WorldArchiveCodec::DeserializeTexture(Bytes, Texture, Error, MaxTextureResolution)
            || Texture.TextureId != TextureId)
        {
            TextureErrors[Index] = Error.IsEmpty()
                ? TEXT("Referenced texture.dat has a mismatched id") : MoveTemp(Error);
            return;
        }
        TextureResults[Index] = MoveTemp(Texture);
        TextureSuccess[Index] = 1;
    });

    for (int32 Index = 0; Index < TextureResults.Num(); ++Index)
    {
        if (TextureSuccess[Index] == 0)
        {
            OutError = TextureErrors[Index].IsEmpty()
                ? TEXT("Referenced texture.dat could not be read") : TextureErrors[Index];
            OutBundle.Reset();
            return false;
        }
    }
    OutBundle.Textures = MoveTemp(TextureResults);

    if (!bDependenciesOnly && SkinIndex != INDEX_NONE)
    {
        const FGWorldArchiveRange* Range = Manifest.SkinRanges.Find(SkinIndex);
        if (!Range)
        {
            OutBundle.Reset();
            OutError = TEXT("Requested skin.dat is missing");
            return false;
        }
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*Path));
        if (!Handle.IsValid() || Handle->Size() != FileSize)
        {
            OutBundle.Reset();
            OutError = TEXT("The .v3d file changed or could not be opened for skin.dat");
            return false;
        }
        TArray<uint8> Bytes;
        FGWorldBakedSkin Skin;
        if (!ReadMemberBytes(Handle.Get(), *Range, Bytes, OutError)
            || !WorldArchiveCodec::DeserializeSkin(Bytes, Skin, OutError)
            || Skin.SkinIndex != SkinIndex)
        {
            OutBundle.Reset();
            if (OutError.IsEmpty())
            {
                OutError = TEXT("Requested skin.dat has a mismatched id");
            }
            return false;
        }
        OutBundle.Skins.Add(MoveTemp(Skin));
    }
    return true;
}
