// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/SourceModelImport.h"

#include "Animation/Skeleton.h"
#include "Character/CharacterBoneSchema.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "System/GlbValidation.h"
#include "System/WorldBakedData.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "glTFRuntimeAsset.h"

namespace V3DSourceModelImport
{
    EFormat GetFormat(const FString& Path)
    {
        const FString Extension = FPaths::GetExtension(Path).ToLower();
        if (Extension == TEXT("glb")) return EFormat::Glb;
        if (Extension == TEXT("gltf")) return EFormat::Gltf;
        if (Extension == TEXT("obj")) return EFormat::Obj;
        if (Extension == TEXT("fbx")) return EFormat::Fbx;
        if (Extension == TEXT("stl")) return EFormat::Stl;
        return EFormat::Unsupported;
    }

    bool IsModelSource(const FString& Path) { return GetFormat(Path) != EFormat::Unsupported; }
    bool UsesBlob(EFormat Format) { return Format == EFormat::Obj || Format == EFormat::Fbx || Format == EFormat::Stl; }
    const TCHAR* RequiredPlugin(EFormat Format)
    {
        switch (Format)
        {
            case EFormat::Obj: return TEXT("glTFRuntimeOBJ");
            case EFormat::Fbx: return TEXT("glTFRuntimeFBX");
            case EFormat::Stl: return TEXT("glTFRuntimeSTL");
            default: return nullptr;
        }
    }

    bool ValidateSourceFile(const FString& Path, FString& OutError)
    {
        OutError.Reset();
        const EFormat Format = GetFormat(Path);
        if (Format == EFormat::Glb || Format == EFormat::Gltf)
            return GlbValidation::ValidateSourceFile(Path, OutError);
        if (!UsesBlob(Format))
        {
            OutError = TEXT("unsupported source extension; expected GLB, glTF, OBJ, FBX or STL");
            return false;
        }
        const int64 Size = IFileManager::Get().FileSize(*Path);
        if (Size <= 0 || Size > MAX_int32)
        {
            OutError = TEXT("source model is missing, empty, or exceeds the 2 GiB native decoder limit");
            return false;
        }
        // Blob bytes are never passed to the GLB header/JSON reader. Format semantics belong to
        // the corresponding extension; preflight remains file-only and safe in discovery lanes.
        return true;
    }

    bool CheckAvailability(const FString& Path, FString& OutError)
    {
        check(IsInGameThread());
        OutError.Reset();
        const EFormat Format = GetFormat(Path);
        if (Format == EFormat::Unsupported)
        {
            OutError = TEXT("unsupported source model extension");
            return false;
        }
        const TCHAR* PluginName = RequiredPlugin(Format);
        if (!PluginName) return true;
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
        if (!Plugin.IsValid())
        {
            OutError = FString::Printf(TEXT(".%s import requires the optional %s plugin; install it in Plugins, enable it, regenerate and rebuild"),
                *FPaths::GetExtension(Path), PluginName);
            return false;
        }
        if (!Plugin->IsEnabled())
        {
            OutError = FString::Printf(TEXT("%s is installed but disabled; enable it and restart the editor"), PluginName);
            return false;
        }
        if (!FModuleManager::Get().LoadModule(FName(PluginName)))
        {
            OutError = FString::Printf(TEXT("%s is enabled but its runtime module could not load; rebuild the plugin for this engine/platform"), PluginName);
            return false;
        }
        return true;
    }

    bool ValidateHierarchy(const TArray<FglTFRuntimeNode>& Nodes, FString& OutError)
    {
        OutError.Reset();
        if (Nodes.Num() > 100000) { OutError = TEXT("source hierarchy exceeds the node safety limit"); return false; }
        TMap<int32, int32> IndexToRow;
        IndexToRow.Reserve(Nodes.Num());
        for (int32 Row = 0; Row < Nodes.Num(); ++Row)
        {
            const FglTFRuntimeNode& Node = Nodes[Row];
            if (Node.Index < 0 || IndexToRow.Contains(Node.Index) || Node.ParentIndex < INDEX_NONE
                || Node.Transform.ContainsNaN() || !Node.Transform.GetRotation().IsNormalized())
            { OutError = TEXT("source hierarchy contains an invalid/duplicate index or transform"); return false; }
            IndexToRow.Add(Node.Index, Row);
        }
        TArray<uint8> State;
        State.Init(0, Nodes.Num());
        for (int32 Start = 0; Start < Nodes.Num(); ++Start)
        {
            if (State[Start] == 2) continue;
            TArray<int32> Chain;
            int32 Row = Start;
            while (Row != INDEX_NONE && State[Row] == 0)
            {
                State[Row] = 1;
                Chain.Add(Row);
                const int32 Parent = Nodes[Row].ParentIndex;
                if (Parent == INDEX_NONE) { Row = INDEX_NONE; break; }
                const int32* ParentRow = IndexToRow.Find(Parent);
                if (!ParentRow) { OutError = TEXT("source hierarchy references a missing parent"); return false; }
                Row = *ParentRow;
            }
            if (Row != INDEX_NONE && State[Row] == 1)
            { OutError = TEXT("cycle detected in source node hierarchy"); return false; }
            for (const int32 Visited : Chain) State[Visited] = 2;
        }
        return true;
    }

    namespace
    {
        UClass* FindImporterLibrary(const TCHAR* PluginName, FString& Error)
        {
            const FString ScriptPackage = FString::Printf(TEXT("/Script/%s"), PluginName);
            const FString ConventionalPath = FString::Printf(TEXT("%s.%sFunctionLibrary"), *ScriptPackage, PluginName);
            if (UClass* Class = FindObject<UClass>(nullptr, *ConventionalPath)) return Class;
            UClass* Found = nullptr;
            // Some extension releases change the library class name. Resolve only within the
            // already-enabled plugin's script package and require a unique function library.
            for (TObjectIterator<UClass> It; It; ++It)
            {
                UClass* Class = *It;
                if (Class->GetOutermost()->GetName() != ScriptPackage
                    || !Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass())) continue;
                if (Found) { Error = FString::Printf(TEXT("%s exposes multiple importer libraries; this plugin version needs an explicit adapter"), PluginName); return nullptr; }
                Found = Class;
            }
            if (!Found) Error = FString::Printf(TEXT("%s has no runtime importer function library; rebuild a compatible extension version"), PluginName);
            return Found;
        }

        /** Reflection keeps optional plugins optional at both UBT and shipping link time. The
         * function layout is checked before ProcessEvent; API changes become a useful error,
         * never a guessed parameter buffer or a call through an incompatible C++ signature. */
        struct FCall
        {
            UFunction* Function = nullptr;
            TSharedPtr<FStructOnScope> Arguments;
            UObject* Target = nullptr;

            bool Prepare(UClass* Class, const TCHAR* Name, UglTFRuntimeAsset* Asset,
                const FStructOnScope* Node, const FglTFRuntimeMaterialsConfig* Materials, FString& Error)
            {
                Error.Reset();
                Function = Class ? Class->FindFunctionByName(FName(Name)) : nullptr;
                if (!Function || !Function->HasAnyFunctionFlags(FUNC_Static))
                {
                    Error = FString::Printf(TEXT("optional importer API %s is unavailable; use a compatible glTFRuntime extension version"), Name);
                    return false;
                }
                Target = Class->GetDefaultObject();
                Arguments = MakeShared<FStructOnScope>(Function);
                for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
                {
                    FProperty* Property = *It;
                    if (Property->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
                    void* Address = Property->ContainerPtrToValuePtr<void>(Arguments->GetStructMemory());
                    if (FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property);
                        Object && Object->PropertyClass == UglTFRuntimeAsset::StaticClass())
                    {
                        Object->SetObjectPropertyValue(Address, Asset);
                        continue;
                    }
                    if (FStructProperty* Struct = CastField<FStructProperty>(Property))
                    {
                        if (Node && Struct->Struct == Node->GetStruct())
                            Struct->Struct->CopyScriptStruct(Address, Node->GetStructMemory());
                        else if (Materials && Struct->Struct == FglTFRuntimeMaterialsConfig::StaticStruct())
                            *static_cast<FglTFRuntimeMaterialsConfig*>(Address) = *Materials;
                        // Other config structs retain their native constructor defaults. Runtime
                        // LOD outputs also need construction because their arrays are native fields.
                        else if (!(Struct->Struct == FglTFRuntimeMeshLOD::StaticStruct()
                                && Property->HasAnyPropertyFlags(CPF_OutParm)
                                && !Property->HasAnyPropertyFlags(CPF_ConstParm))
                            && !Struct->Struct->GetName().EndsWith(TEXT("Config")))
                        {
                            Error = FString::Printf(TEXT("%s has an unsupported required struct parameter %s"), Name, *Property->GetName());
                            return false;
                        }
                        continue;
                    }
                    if (Property->HasAnyPropertyFlags(CPF_OutParm)
                        && !Property->HasAnyPropertyFlags(CPF_ConstParm)
                        && CastField<FBoolProperty>(Property)) continue;
                    FString Default;
#if WITH_METADATA
                    Default = Function->GetMetaData(FName(*FString::Printf(TEXT("CPP_Default_%s"), *Property->GetName())));
#endif
                    if (!Default.IsEmpty())
                    {
                        if (!Property->ImportText_Direct(*Default, Address, Target, PPF_None, nullptr))
                        {
                            Error = FString::Printf(TEXT("%s has an invalid default for %s"), Name, *Property->GetName());
                            return false;
                        }
                    }
                    else
                    {
                        Error = FString::Printf(TEXT("%s has an unsupported required parameter %s"), Name, *Property->GetName());
                        return false;
                    }
                }
                return true;
            }
            void Invoke() { check(IsInGameThread()); Target->ProcessEvent(Function, Arguments->GetStructMemory()); }
            FProperty* Return() const { return Function ? Function->GetReturnProperty() : nullptr; }
            bool Succeeded() const
            {
                const FBoolProperty* Result = CastField<FBoolProperty>(Return());
                return !Result || Result->GetPropertyValue_InContainer(Arguments->GetStructMemory());
            }
        };

        bool ReadNodeArray(UClass* Class, const TCHAR* FunctionName, UglTFRuntimeAsset* Asset,
            const FStructOnScope* Node, TArray<TSharedPtr<FStructOnScope>>& OutNodes, FString& Error)
        {
            FCall Call;
            if (!Call.Prepare(Class, FunctionName, Asset, Node, nullptr, Error)) return false;
            FArrayProperty* Array = CastField<FArrayProperty>(Call.Return());
            FStructProperty* Element = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
            if (!Element)
            {
                Error = FString::Printf(TEXT("%s must return an array of FBX nodes"), FunctionName);
                return false;
            }
            Call.Invoke();
            FScriptArrayHelper Values(Array, Array->ContainerPtrToValuePtr<void>(Call.Arguments->GetStructMemory()));
            if (Values.Num() > 100000)
            {
                Error = TEXT("imported hierarchy exceeds the node safety limit");
                return false;
            }
            OutNodes.Reserve(Values.Num());
            for (int32 Index = 0; Index < Values.Num(); ++Index)
            {
                TSharedPtr<FStructOnScope> Copy = MakeShared<FStructOnScope>(Element->Struct);
                Element->Struct->CopyScriptStruct(Copy->GetStructMemory(), Values.GetRawPtr(Index));
                OutNodes.Add(MoveTemp(Copy));
            }
            return true;
        }

        FString NodeKey(const FStructOnScope& Node)
        {
            for (const TCHAR* Field : { TEXT("Id"), TEXT("NodeId"), TEXT("Index"), TEXT("NodeIndex") })
            {
                const FNumericProperty* Id = FindFProperty<FNumericProperty>(Node.GetStruct(), Field);
                if (Id && Id->IsInteger())
                {
                    const void* Address = Id->ContainerPtrToValuePtr<void>(Node.GetStructMemory());
                    return FString::Printf(TEXT("%s:%llu"), Field, Id->GetUnsignedIntPropertyValue(Address));
                }
            }
            FString Key;
            const UScriptStruct* Struct = CastChecked<UScriptStruct>(Node.GetStruct());
            Struct->ExportText(Key, Node.GetStructMemory(), nullptr, nullptr, PPF_None, nullptr, true);
            return Key;
        }

        bool ReadNode(UClass* Class, UglTFRuntimeAsset* Asset, const FStructOnScope& Native,
            FglTFRuntimeNode& Node, FString& Error)
        {
            const UStruct* Struct = Native.GetStruct();
            const FStrProperty* Name = FindFProperty<FStrProperty>(Struct, TEXT("Name"));
            if (!Name) { Error = TEXT("FBX node Name API is unavailable"); return false; }
            Node.Name = Name->GetPropertyValue_InContainer(Native.GetStructMemory());
            // Supported extension versions either expose Transform on the node or a typed
            // GetFBXNodeTransform function. Do not silently flatten an unknown hierarchy API.
            const FStructProperty* Transform = FindFProperty<FStructProperty>(Struct, TEXT("Transform"));
            if (Transform && Transform->Struct == TBaseStructure<FTransform>::Get())
            {
                Node.Transform = *Transform->ContainerPtrToValuePtr<FTransform>(Native.GetStructMemory());
                return true;
            }
            FCall Call;
            if (!Call.Prepare(Class, TEXT("GetFBXNodeTransform"), Asset, &Native, nullptr, Error)) return false;
            Transform = CastField<FStructProperty>(Call.Return());
            if (!Transform || Transform->Struct != TBaseStructure<FTransform>::Get())
            { Error = TEXT("FBX node transform API has an incompatible return type"); return false; }
            Call.Invoke();
            Node.Transform = *Transform->ContainerPtrToValuePtr<FTransform>(Call.Arguments->GetStructMemory());
            return true;
        }

        bool PrepareDecoder(UClass* Class, const TCHAR* PreferredName, UglTFRuntimeAsset* Asset,
            const FStructOnScope* Node, const FglTFRuntimeMaterialsConfig& Materials,
            FCall& OutCall, FString& Error)
        {
            if (!Class) { Error = TEXT("optional importer library was unloaded"); return false; }
            auto IsDecoderShape = [](const UFunction* Function)
            {
                if (!Function || !Function->HasAnyFunctionFlags(FUNC_Static)) return false;
                const FProperty* Result = Function->GetReturnProperty();
                const FStructProperty* StructResult = CastField<FStructProperty>(Result);
                if (!CastField<FBoolProperty>(Result)
                    && (!StructResult || StructResult->Struct != FglTFRuntimeMeshLOD::StaticStruct())) return false;
                bool bHasAsset = false, bHasLODOutput = false;
                for (TFieldIterator<FProperty> Param(Function); Param && Param->HasAnyPropertyFlags(CPF_Parm); ++Param)
                {
                    if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(*Param))
                        bHasAsset |= Object->PropertyClass == UglTFRuntimeAsset::StaticClass();
                    if (const FStructProperty* Struct = CastField<FStructProperty>(*Param))
                        bHasLODOutput |= Struct->Struct == FglTFRuntimeMeshLOD::StaticStruct()
                            && Struct->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm)
                            && !Struct->HasAnyPropertyFlags(CPF_ConstParm);
                }
                return bHasAsset && bHasLODOutput;
            };
            if (IsDecoderShape(Class->FindFunctionByName(FName(PreferredName)))
                && OutCall.Prepare(Class, PreferredName, Asset, Node, &Materials, Error)) return true;
            if (Error.IsEmpty()) Error = FString::Printf(TEXT("%s has an incompatible RuntimeLOD decoder signature"), PreferredName);
            FString OriginalError = Error;
            const FString Preferred(PreferredName);
            const TCHAR* FormatToken = Preferred.Contains(TEXT("FBX")) ? TEXT("FBX")
                : Preferred.Contains(TEXT("STL")) ? TEXT("STL") : TEXT("OBJ");
            FName CandidateName = NAME_None;
            for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
            {
                UFunction* Function = *It;
                const FString Name = Function->GetName();
                if (!IsDecoderShape(Function) || !Name.Contains(FormatToken) || !Name.Contains(TEXT("RuntimeLOD"))
                    || Name.Contains(TEXT("Async")) || Name.Contains(TEXT("Recursive"))) continue;
                FCall Check;
                FString SignatureError;
                if (!Check.Prepare(Class, *Name, Asset, Node, &Materials, SignatureError)) continue;
                if (!CandidateName.IsNone())
                {
                    Error = TEXT("extension exposes multiple compatible RuntimeLOD decoders; a version-specific adapter is required");
                    return false;
                }
                CandidateName = Function->GetFName();
            }
            if (CandidateName.IsNone()) { Error = MoveTemp(OriginalError); return false; }
            // The fallback is selected by reflected input/output types, not by guessing a C++
            // symbol. No function is invoked until its full parameter layout has been checked.
            return OutCall.Prepare(Class, *CandidateName.ToString(), Asset, Node, &Materials, Error);
        }
    }

    FSession::~FSession() = default;

    bool FSession::Initialize(UglTFRuntimeAsset* Asset, const FString& SourcePath, FString& OutError)
    {
        check(IsInGameThread());
        Nodes.Reset(); MeshNames.Reset(); MeshNodes.Reset();
        if (!IsValid(Asset)) { OutError = TEXT("source asset is invalid"); return false; }
        if (!UsesBlob(Format))
        {
            Nodes = Asset->GetNodes();
            const int32 Count = Asset->GetNumMeshes();
            if (Count < 0 || Count > 65536) { OutError = TEXT("source mesh count exceeds the safety limit"); return false; }
            const TArray<TSharedRef<FJsonObject>> Meshes = Asset->GetParser()->GetMeshes();
            MeshNames.SetNum(Count);
            for (int32 Index = 0; Index < Count; ++Index)
            {
                if (!Meshes.IsValidIndex(Index) || !Meshes[Index]->TryGetStringField(TEXT("name"), MeshNames[Index])
                    || MeshNames[Index].TrimStartAndEnd().IsEmpty())
                    MeshNames[Index] = FString::Printf(TEXT("Mesh_%d"), Index);
            }
            return ValidateHierarchy(Nodes, OutError);
        }
        UClass* Class = FindImporterLibrary(RequiredPlugin(Format), OutError);
        if (!Class) return false;
        LibraryClass = Class;
        if (Format != EFormat::Fbx)
        {
            // OBJ/STL decoders return one runtime LOD containing all material primitives.
            FglTFRuntimeNode& Node = Nodes.AddDefaulted_GetRef();
            Node.Index = 0; Node.MeshIndex = 0; Node.Name = FPaths::GetBaseFilename(SourcePath);
            MeshNames.Add(Node.Name);
            return ValidateHierarchy(Nodes, OutError);
        }

        TArray<TSharedPtr<FStructOnScope>> AllNodes;
        if (!ReadNodeArray(Class, TEXT("GetFBXNodes"), Asset, nullptr, AllNodes, OutError)
            || !ReadNodeArray(Class, TEXT("GetFBXNodesMeshes"), Asset, nullptr, MeshNodes, OutError)) return false;
        if (MeshNodes.IsEmpty() || MeshNodes.Num() > 65536) { OutError = TEXT("FBX mesh count is empty or exceeds the safety limit"); return false; }
        TMap<FString, int32> Indices;
        for (int32 Index = 0; Index < AllNodes.Num(); ++Index)
        {
            const FString Key = NodeKey(*AllNodes[Index]);
            if (Indices.Contains(Key))
            {
                // Older plugins may expose only Name/Transform and keep their node handle in a
                // non-reflected field. Such metadata is sufficient only while it is unique.
                OutError = TEXT("FBX nodes have indistinguishable public metadata; use unique node names or an extension exposing stable node Id/Index fields");
                return false;
            }
            Indices.Add(Key, Index);
            FglTFRuntimeNode& Node = Nodes.AddDefaulted_GetRef();
            Node.Index = Index;
            if (!ReadNode(Class, Asset, *AllNodes[Index], Node, OutError)) return false;
        }
        for (int32 Index = 0; Index < MeshNodes.Num(); ++Index)
        {
            const int32* NodeIndex = Indices.Find(NodeKey(*MeshNodes[Index]));
            if (!NodeIndex) { OutError = TEXT("FBX mesh references a missing hierarchy node"); return false; }
            Nodes[*NodeIndex].MeshIndex = Index;
            MeshNames.Add(Nodes[*NodeIndex].Name);
        }
        for (int32 Index = 0; Index < AllNodes.Num(); ++Index)
        {
            TArray<TSharedPtr<FStructOnScope>> Children;
            if (!ReadNodeArray(Class, TEXT("GetFBXNodeChildren"), Asset, AllNodes[Index].Get(), Children, OutError)) return false;
            for (const TSharedPtr<FStructOnScope>& Child : Children)
            {
                const int32* ChildIndex = Indices.Find(NodeKey(*Child));
                if (!ChildIndex || *ChildIndex == Index || Nodes[*ChildIndex].ParentIndex != INDEX_NONE)
                { OutError = TEXT("FBX hierarchy contains a missing, duplicate or self-parented child"); return false; }
                Nodes[*ChildIndex].ParentIndex = Index;
                Nodes[Index].ChildrenIndices.Add(*ChildIndex);
            }
        }
        return ValidateHierarchy(Nodes, OutError);
    }

    bool FSession::DecodeMesh(UglTFRuntimeAsset* Asset, int32 MeshIndex,
        const FglTFRuntimeMaterialsConfig& Materials, FglTFRuntimeMeshLOD& OutLOD,
        bool& bOutSkeletal, FString& OutError)
    {
        check(IsInGameThread());
        bOutSkeletal = false;
        if (!UsesBlob(Format)) return Asset->LoadMeshAsRuntimeLOD(MeshIndex, OutLOD, Materials);
        if (!MeshNames.IsValidIndex(MeshIndex)) { OutError = TEXT("source mesh index is out of range"); return false; }
        const TCHAR* FunctionName = Format == EFormat::Fbx ? TEXT("LoadFBXAsRuntimeLODByNode")
            : Format == EFormat::Obj ? TEXT("LoadOBJAsRuntimeLOD") : TEXT("LoadSTLAsRuntimeLOD");
        FCall Call;
        if (!PrepareDecoder(LibraryClass.Get(), FunctionName, Asset,
            MeshNodes.IsValidIndex(MeshIndex) ? MeshNodes[MeshIndex].Get() : nullptr, Materials, Call, OutError)) return false;
        FStructProperty* LOD = nullptr;
        for (TFieldIterator<FProperty> It(Call.Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
            if (FStructProperty* Struct = CastField<FStructProperty>(*It);
                Struct && Struct->Struct == FglTFRuntimeMeshLOD::StaticStruct()
                && Struct->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm)
                && !Struct->HasAnyPropertyFlags(CPF_ConstParm)) LOD = Struct;
        if (!LOD) { OutError = FString::Printf(TEXT("%s has no compatible RuntimeLOD output"), FunctionName); return false; }
        Call.Invoke();
        if (!Call.Succeeded()) { OutError = FString::Printf(TEXT("%s could not decode the source; inspect the glTFRuntime plugin log"), FunctionName); return false; }
        OutLOD = MoveTemp(*LOD->ContainerPtrToValuePtr<FglTFRuntimeMeshLOD>(Call.Arguments->GetStructMemory()));
        bOutSkeletal = !OutLOD.Skeleton.IsEmpty();
        if (FBoolProperty* Skeletal = FindFProperty<FBoolProperty>(Call.Function, TEXT("bIsSkeletal")))
            bOutSkeletal |= Skeletal->GetPropertyValue_InContainer(Call.Arguments->GetStructMemory());
        for (FglTFRuntimeNode& Node : Nodes)
            if (Node.MeshIndex == MeshIndex) Node.SkinIndex = bOutSkeletal ? MeshIndex : INDEX_NONE;
        return !OutLOD.Primitives.IsEmpty();
    }

    bool FSession::CaptureSkin(FglTFRuntimeMeshLOD& LOD, int32 MeshIndex,
        const TMap<FString, FString>& BoneAliases, USkeleton* TargetSkeleton,
        FGWorldBakedSkin& OutSkin, FString& OutError)
    {
        check(IsInGameThread());
        if (LOD.Skeleton.IsEmpty() || LOD.Skeleton.Num() > 65535)
        { OutError = TEXT("extension returned an empty or oversized skeleton"); return false; }
        // The extension's LOD already contains final local reference transforms and authoritative
        // per-primitive joint maps. Retain them rather than asking a blob parser for glTF skins.
        // Canonical characters require an exact supported hierarchy; never silently retarget an
        // arbitrary FBX reference pose onto the project's animation skeleton.
        OutSkin = FGWorldBakedSkin();
        OutSkin.SkinIndex = MeshIndex;
        TSet<FName> Names;
        int32 RootCount = 0;
        for (int32 Index = 0; Index < LOD.Skeleton.Num(); ++Index)
        {
            FglTFRuntimeBone& Source = LOD.Skeleton[Index];
            if (const FString* Alias = BoneAliases.Find(Source.BoneName)) Source.BoneName = *Alias;
            const FName Name(*Source.BoneName);
            if (Name.IsNone() || Names.Contains(Name) || Source.ParentIndex >= Index || Source.ParentIndex < INDEX_NONE
                || Source.Transform.ContainsNaN())
            { OutError = TEXT("extension skeleton has duplicate names, invalid transforms or invalid parent order"); return false; }
            Names.Add(Name);
            RootCount += Source.ParentIndex == INDEX_NONE ? 1 : 0;
            FGWorldBakedBone& Bone = OutSkin.Bones.AddDefaulted_GetRef();
            Bone.Name = Source.BoneName; Bone.ParentIndex = Source.ParentIndex; Bone.Transform = Source.Transform;
        }
        if (RootCount != 1) { OutError = TEXT("extension skeleton must contain exactly one root bone"); return false; }
        if (!BoneAliases.IsEmpty())
        {
            if (!CharacterBoneSchema::ValidateSourceToCanonicalMap(BoneAliases, OutError)) return false;
            if (!IsValid(TargetSkeleton)) { OutError = TEXT("FBX Character build requires DefaultCharacterSkeleton"); return false; }
            const FReferenceSkeleton& Target = TargetSkeleton->GetReferenceSkeleton();
            for (const FName Required : CharacterBoneSchema::GetRequiredKeys())
                if (!Names.Contains(Required)) { OutError = FString::Printf(TEXT("FBX Character is missing canonical bone %s"), *Required.ToString()); return false; }
            for (const FGWorldBakedBone& Bone : OutSkin.Bones)
            {
                const int32 TargetIndex = Target.FindBoneIndex(FName(*Bone.Name));
                if (TargetIndex == INDEX_NONE || !Bone.Transform.Equals(Target.GetRefBonePose()[TargetIndex], 0.001f))
                {
                    OutError = TEXT("FBX Character reference pose differs from DefaultCharacterSkeleton; export the canonical bind pose or use glTF for canonical retargeting");
                    return false;
                }
                const int32 TargetParent = Target.GetParentIndex(TargetIndex);
                const FName ParentName = Bone.ParentIndex >= 0 ? FName(*OutSkin.Bones[Bone.ParentIndex].Name) : NAME_None;
                if (ParentName != (TargetParent >= 0 ? Target.GetBoneName(TargetParent) : NAME_None))
                {
                    OutError = TEXT("FBX Character parent hierarchy differs from DefaultCharacterSkeleton");
                    return false;
                }
            }
        }
        for (FglTFRuntimePrimitive& Primitive : LOD.Primitives)
        {
            const bool bUsesSkeletonIndices = Primitive.OverrideBoneMap.IsEmpty() && !Primitive.Joints.IsEmpty();
            if (bUsesSkeletonIndices)
            {
                // glTFRuntime::FillLODSkeleton explicitly defines the fallback joint namespace
                // as LOD skeleton array indices. Materialize that same map before serializing.
                for (int32 Index = 0; Index < LOD.Skeleton.Num(); ++Index)
                    Primitive.OverrideBoneMap.Add(Index, FName(*LOD.Skeleton[Index].BoneName));
            }
            for (TPair<int32, FName>& Pair : Primitive.OverrideBoneMap)
            {
                if (!bUsesSkeletonIndices)
                {
                    if (const FString* Alias = BoneAliases.Find(Pair.Value.ToString())) Pair.Value = FName(**Alias);
                }
                if (!Names.Contains(Pair.Value)) { OutError = TEXT("FBX joint map references a missing skeleton bone"); return false; }
                const FName* Existing = OutSkin.JointBoneMap.Find(Pair.Key);
                if (Existing && *Existing != Pair.Value) { OutError = TEXT("FBX primitives disagree about a joint index"); return false; }
                OutSkin.JointBoneMap.Add(Pair.Key, Pair.Value);
            }
        }
        return true;
    }

#if WITH_DEV_AUTOMATION_TESTS
    bool DecodeWithLibraryForTesting(UClass* Library, UglTFRuntimeAsset* Asset,
        const FglTFRuntimeMaterialsConfig& Materials, FglTFRuntimeMeshLOD& OutLOD, FString& OutError)
    {
        check(IsInGameThread());
        FCall Call;
        if (!PrepareDecoder(Library, TEXT("LoadOBJAsRuntimeLOD"), Asset, nullptr, Materials, Call, OutError)) return false;
        FStructProperty* LOD = nullptr;
        for (TFieldIterator<FProperty> It(Call.Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
            if (FStructProperty* Struct = CastField<FStructProperty>(*It);
                Struct && Struct->Struct == FglTFRuntimeMeshLOD::StaticStruct()
                    && Struct->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm)
                    && !Struct->HasAnyPropertyFlags(CPF_ConstParm)) LOD = Struct;
        if (!LOD) return false;
        Call.Invoke();
        if (!Call.Succeeded()) return false;
        OutLOD = MoveTemp(*LOD->ContainerPtrToValuePtr<FglTFRuntimeMeshLOD>(Call.Arguments->GetStructMemory()));
        return true;
    }
#endif
}
