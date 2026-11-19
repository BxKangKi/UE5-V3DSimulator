// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "Model/V3DModelAnimation.h"
#include "System/WorldBakedData.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Dom/JsonObject.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeParser.h"

namespace
{
    std::string Utf8(const FString& S) { FTCHARToUTF8 U(*S); return std::string(U.Get(), U.Length()); }
    v3d::vec3 Portable(const FVector& V) { return {float(V.X),float(V.Y),float(V.Z)}; }
    v3d::vec4 Portable(const FQuat& Q) { return {float(Q.X),float(Q.Y),float(Q.Z),float(Q.W)}; }
    template<class T> const T& Key(const TArray<T>& Keys, int32 Frame) { return Keys[FMath::Min(Frame,Keys.Num()-1)]; }
}

bool V3DModelAnimation::Capture(UglTFRuntimeAsset* Source, const TMap<FString, FString>& BoneAliases,
    FGWorldBakedModel& Model, FString& Error)
{
    check(IsInGameThread());
    Model.Animations.Reset();
    if (!IsValid(Source) || !Source->GetParser().IsValid()) { Error=TEXT("Missing animation source parser"); return false; }
    auto Parser=Source->GetParser();
    const auto JsonAnimations=Parser->GetAnimations();
    if (JsonAnimations.IsEmpty() || Model.Skins.IsEmpty()) return true;
    if (int64(JsonAnimations.Num())*Model.Skins.Num()>4096) { Error=TEXT("Too many model animation/skin pairs"); return false; }

    for (const FGWorldBakedSkin& Skin : Model.Skins)
    {
        // Same hierarchy and name mapping as CaptureSkin, retaining the ORIGINAL bone bases.
        FglTFRuntimeSkeletonConfig SkeletonConfig;
        SkeletonConfig.CacheMode=EglTFRuntimeCacheMode::None;
        SkeletonConfig.BonesNameMap=BoneAliases;
        SkeletonConfig.bClearRotations=false;
        SkeletonConfig.CopyRotationsFrom=nullptr;
        SkeletonConfig.RootBoneName=TEXT("Root");
        SkeletonConfig.RootNodeIndex=-1;
        SkeletonConfig.MaxNodesTreeDepth=-1;
        SkeletonConfig.bAddRootBone=false;
        SkeletonConfig.bAddRootNodeIfMissing=BoneAliases.IsEmpty();
        FTransform RootParents=FTransform::Identity;
        TArray<FglTFRuntimeNode> RootParentNodes;
        if (!BoneAliases.IsEmpty())
        {
            for (const auto& Pair : BoneAliases) if (Pair.Value==TEXT("Root")) SkeletonConfig.ForceRootNode=Pair.Key;
            SkeletonConfig.bApplyParentNodesTransformsToRoot=true;
            FglTFRuntimeNode Node;
            if (!Parser->LoadNodeByName(SkeletonConfig.ForceRootNode,Node)) { Error=TEXT("Animation source Root is missing"); return false; }
            TSet<int32> Seen;
            while (Node.ParentIndex>=0)
            {
                if (Seen.Contains(Node.ParentIndex) || !Parser->LoadNode(Node.ParentIndex,Node)) { Error=TEXT("Invalid animation root ancestry"); return false; }
                Seen.Add(Node.Index); RootParents=RootParents*Node.Transform;RootParentNodes.Add(Node);
            }
        }
        USkeleton* Original=Source->LoadSkeleton(Skin.SkinIndex,SkeletonConfig);
        if (!Original) { Error=TEXT("Could not read animation reference skeleton"); return false; }
        const FReferenceSkeleton& Ref=Original->GetReferenceSkeleton();
        const int32 Bones=Skin.Bones.Num();
        if (Ref.GetNum()!=Bones) { Error=TEXT("Animation and baked skeleton bone counts differ"); return false; }
        TArray<FTransform> SourceRestCS,TargetRestCS;
        SourceRestCS.SetNum(Bones); TargetRestCS.SetNum(Bones);
        for (int32 B=0;B<Bones;++B)
        {
            if (Ref.GetBoneName(B)!=FName(*Skin.Bones[B].Name) || Ref.GetParentIndex(B)!=Skin.Bones[B].ParentIndex)
            { Error=TEXT("Animation and baked skeleton hierarchies differ"); return false; }
            const int32 P=Ref.GetParentIndex(B);
            SourceRestCS[B]=P<0?Ref.GetRefBonePose()[B]:Ref.GetRefBonePose()[B]*SourceRestCS[P];
            TargetRestCS[B]=P<0?Skin.Bones[B].Transform:Skin.Bones[B].Transform*TargetRestCS[P];
        }
        for (int32 AnimationIndex=0;AnimationIndex<JsonAnimations.Num();++AnimationIndex)
        {
            TMap<FString,FRawAnimSequenceTrack> SourceTracks;
            TMap<FName,TArray<TPair<float,float>>> Curves;
            FglTFRuntimeSkeletalAnimationConfig Config;
            Config.FramesPerSecond=30.f;
            float Duration=0;
            if (!Parser->LoadAnimationAsTracksAndMorphTargets(AnimationIndex,SourceTracks,Curves,Duration,Config))
            { Error=FString::Printf(TEXT("Could not decode model animation %d"),AnimationIndex); return false; }
            if (!FMath::IsFinite(Duration) || Duration<=0 || Duration>3600.f)
            { Error=TEXT("Model animation duration must be between 0 and 3600 seconds"); return false; }
            TMap<FString,FRawAnimSequenceTrack> Tracks;
            int32 Frames=FMath::Max(1,FMath::RoundToInt(Duration*Config.FramesPerSecond));
            for (auto& Pair:SourceTracks)
            {
                Frames=FMath::Max(Frames,FMath::Max3(Pair.Value.PosKeys.Num(),Pair.Value.RotKeys.Num(),Pair.Value.ScaleKeys.Num()));
                const FString* Alias=BoneAliases.Find(Pair.Key);
                const FString Name=Alias?*Alias:Pair.Key;
                if (Ref.FindBoneIndex(FName(*Name))==INDEX_NONE) continue;
                if (Tracks.Contains(Name)) { Error=TEXT("Animation tracks collide after bone mapping"); return false; }
                Tracks.Add(Name,MoveTemp(Pair.Value));
            }
            const bool HasParentAnimation=RootParentNodes.ContainsByPredicate([&](const auto& Node) { return SourceTracks.Contains(Node.Name); });
            if (Tracks.IsEmpty() && Curves.IsEmpty() && !HasParentAnimation) continue;
            if (int64(Frames)*Bones*3>24000000) { Error=TEXT("Model animation exceeds sampled track limit"); return false; }
            v3d::animation Clip;
            Clip.animation_id=Model.Animations.Num(); Clip.skin_index=Skin.SkinIndex;
            FString Name; JsonAnimations[AnimationIndex]->TryGetStringField(TEXT("name"),Name);
            if (Name.IsEmpty()) Name=FString::Printf(TEXT("Animation_%d"),AnimationIndex);
            Clip.name=Utf8(Name); Clip.duration=Duration; Clip.frames_per_second=Config.FramesPerSecond; Clip.frame_count=Frames;
            Clip.tracks.resize(Bones);
            for (int32 B=0;B<Bones;++B) Clip.tracks[B].bone_name=Utf8(Skin.Bones[B].Name);
            TArray<FTransform> SourcePose,TargetPose;
            SourcePose.SetNum(Bones);TargetPose.SetNum(Bones);
            for (int32 Frame=0;Frame<Frames;++Frame)
            {
                FTransform AnimatedParents=FTransform::Identity;
                for (const auto& ParentNode:RootParentNodes)
                {
                    FTransform ParentLocal=ParentNode.Transform;
                    if (const auto* Track=SourceTracks.Find(ParentNode.Name))
                    {
                        if (!Track->PosKeys.IsEmpty()) ParentLocal.SetTranslation(FVector(Key(Track->PosKeys,Frame)));
                        if (!Track->RotKeys.IsEmpty()) ParentLocal.SetRotation(FQuat(Key(Track->RotKeys,Frame)).GetNormalized());
                        if (!Track->ScaleKeys.IsEmpty()) ParentLocal.SetScale3D(FVector(Key(Track->ScaleKeys,Frame)));
                    }
                    AnimatedParents=AnimatedParents*ParentLocal;
                }
                for (int32 B=0;B<Bones;++B)
                {
                    FTransform Local=Ref.GetRefBonePose()[B];
                    const auto* Track=Tracks.Find(Skin.Bones[B].Name);
                    const int32 P=Ref.GetParentIndex(B);
                    // Root keys are node-local; the canonical hierarchy prunes helper parents.
                    if (P<0 && !BoneAliases.IsEmpty()) Local=Local.GetRelativeTransform(RootParents);
                    if (Track)
                    {
                        if (!Track->PosKeys.IsEmpty()) Local.SetTranslation(FVector(Key(Track->PosKeys,Frame)));
                        if (!Track->RotKeys.IsEmpty()) Local.SetRotation(FQuat(Key(Track->RotKeys,Frame)).GetNormalized());
                        if (!Track->ScaleKeys.IsEmpty()) Local.SetScale3D(FVector(Key(Track->ScaleKeys,Frame)));
                    }
                    if (P<0 && !BoneAliases.IsEmpty()) Local=Local*AnimatedParents;
                    SourcePose[B]=P<0?Local:Local*SourcePose[P];
                    // Preserve the animated deformation while changing each bone's reference basis.
                    TargetPose[B]=TargetRestCS[B]*SourceRestCS[B].Inverse()*SourcePose[B];
                    FTransform TargetLocal=P<0?TargetPose[B]:TargetPose[B].GetRelativeTransform(TargetPose[P]);
                    TargetLocal.NormalizeRotation();
                    auto& Out=Clip.tracks[B];
                    Out.positions.push_back(Portable(TargetLocal.GetTranslation()));
                    Out.rotations.push_back(Portable(TargetLocal.GetRotation()));
                    Out.scales.push_back(Portable(TargetLocal.GetScale3D()));
                }
            }
            TArray<FName> CurveNames; Curves.GetKeys(CurveNames);CurveNames.Sort(FNameLexicalLess());
            for (FName CurveName:CurveNames)
            {
                v3d::animation_curve Curve;Curve.name=Utf8(CurveName.ToString());
                for (const auto& K:Curves.FindChecked(CurveName)) Curve.keys.push_back({K.Key,K.Value});
                if (!Curve.keys.empty()) Clip.morph_curves.push_back(MoveTemp(Curve));
            }
            std::string Detail;
            if (!v3d::validate(Clip,Detail)) { Error=UTF8_TO_TCHAR(Detail.c_str()); return false; }
            Model.Animations.Add(MoveTemp(Clip));
        }
    }
    return true;
}

UAnimSequence* V3DModelAnimation::Create(const v3d::animation& Clip, USkeletalMesh* Mesh, FString& Error)
{
    check(IsInGameThread());
    std::string Detail;
    if (!IsValid(Mesh) || !v3d::validate(Clip,Detail)) { Error=TEXT("Invalid runtime animation or mesh");return nullptr; }
    TMap<FString,FRawAnimSequenceTrack> Tracks;
    for (const auto& T:Clip.tracks)
    {
        const FString Name=UTF8_TO_TCHAR(T.bone_name.c_str());
        if (Mesh->GetRefSkeleton().FindBoneIndex(FName(*Name))==INDEX_NONE) continue;
        auto& Track=Tracks.Add(Name);
        for (const auto& V:T.positions) Track.PosKeys.Add(FVector3f(V.x,V.y,V.z));
        for (const auto& Q:T.rotations) Track.RotKeys.Add(FQuat4f(Q.x,Q.y,Q.z,Q.w));
        for (const auto& V:T.scales) Track.ScaleKeys.Add(FVector3f(V.x,V.y,V.z));
    }
    TMap<FName,TArray<TPair<float,float>>> Curves;
    for (const auto& C:Clip.morph_curves)
    {
        auto& Curve=Curves.Add(FName(UTF8_TO_TCHAR(C.name.c_str())));
        for (const auto& K:C.keys) Curve.Emplace(K.x,K.y);
    }
    FglTFRuntimeSkeletalAnimationConfig Config; Config.FramesPerSecond=Clip.frames_per_second;
    // Data-free parser: no source GLB is reopened at runtime.
    auto Parser=MakeShared<FglTFRuntimeParser>(MakeShared<FJsonObject>(),FMatrix::Identity,1.f);
    UAnimSequence* Sequence=Parser->LoadSkeletalAnimationFromTracksAndMorphTargets(Mesh,Tracks,Curves,Clip.duration,Config);
    if (!Sequence) Error=FString::Printf(TEXT("Could not create runtime animation '%s'"),UTF8_TO_TCHAR(Clip.name.c_str()));
    return Sequence;
}
