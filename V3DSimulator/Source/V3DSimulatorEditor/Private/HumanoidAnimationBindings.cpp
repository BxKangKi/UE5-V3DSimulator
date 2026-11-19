// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "HumanoidAnimationBindings.h"
#include "Character/CharacterAnimInstance.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompiler.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

void HumanoidAnimationBindings::Upgrade()
{
    // This binding setup exposes the player asset inputs. The state machines,
    // transitions, movement variables, ragdoll recovery, IK and notifies remain authored in ABP.
    UAnimBlueprint* Blueprint=LoadObject<UAnimBlueprint>(nullptr,
        TEXT("/Game/Resources/Animations/ABP_Humanoid.ABP_Humanoid"));
    if (!Blueprint || !Blueprint->ParentClass || !Blueprint->ParentClass->IsChildOf(UCharacterAnimInstance::StaticClass()))
    {
        UE_LOG(LogTemp,Error,TEXT("ABP_Humanoid model animation binding requires CharacterAnimInstance as its parent"));
        return;
    }
    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);
    TArray<TObjectPtr<UAnimSequence>> Sequences;
    TArray<TObjectPtr<UBlendSpace>> BlendSpaces;
    bool Changed=false,Failed=false;
    for (UEdGraph* Graph:Graphs)
    {
        const auto Nodes=Graph->Nodes; // Adding resolver nodes must not invalidate iteration.
        for (UEdGraphNode* GraphNode:Nodes)
        {
            UAnimGraphNode_Base* Player=nullptr;
            UObject* Default=nullptr;
            FName PinName,FunctionName,InputName;
            if (auto* SequenceNode=Cast<UAnimGraphNode_SequencePlayer>(GraphNode))
            {
                Player=SequenceNode;Default=SequenceNode->GetAnimationAsset();PinName=TEXT("Sequence");
                FunctionName=GET_FUNCTION_NAME_CHECKED(UCharacterAnimInstance,ResolveModelAnimation);InputName=TEXT("DefaultSequence");
            }
            else if (auto* BlendSpaceNode=Cast<UAnimGraphNode_BlendSpacePlayer>(GraphNode))
            {
                Player=BlendSpaceNode;Default=BlendSpaceNode->GetAnimationAsset();PinName=TEXT("BlendSpace");
                FunctionName=GET_FUNCTION_NAME_CHECKED(UCharacterAnimInstance,ResolveModelBlendSpace);InputName=TEXT("DefaultBlendSpace");
            }
            if (!Player) continue;
            UEdGraphPin* AssetPin=Player->FindPin(PinName);
            bool AlreadyBound=false;
            if (AssetPin && !AssetPin->LinkedTo.IsEmpty())
            {
                auto* Call=Cast<UK2Node_CallFunction>(AssetPin->LinkedTo[0]->GetOwningNode());
                if (Call && Call->FunctionReference.GetMemberName()==FunctionName)
                {
                    UEdGraphPin* Input=Call->FindPin(InputName);
                    Default=Input?Input->DefaultObject:nullptr;AlreadyBound=true;
                }
                else continue; // Preserve intentional user-authored dynamic asset bindings.
            }
            if (auto* Sequence=Cast<UAnimSequence>(Default)) Sequences.AddUnique(Sequence);
            else if (auto* Blend=Cast<UBlendSpace>(Default)) BlendSpaces.AddUnique(Blend);
            else continue;
            if (AlreadyBound) continue;
            Blueprint->Modify();Player->Modify();Graph->Modify();
            for (FOptionalPinFromProperty& Property:Player->ShowPinForProperties)
                if (Property.PropertyName==PinName) Property.bShowPin=true;
            Player->ReconstructNode();
            AssetPin=Player->FindPin(PinName);
            UFunction* Function=UCharacterAnimInstance::StaticClass()->FindFunctionByName(FunctionName);
            if (!AssetPin || !Function) { Failed=true;continue; }
            auto* Call=NewObject<UK2Node_CallFunction>(Graph);
            Graph->AddNode(Call,false,false);Call->CreateNewGuid();Call->SetFromFunction(Function);
            Call->PostPlacedNewNode();Call->AllocateDefaultPins();
            Call->NodePosX=Player->NodePosX-360;Call->NodePosY=Player->NodePosY+100;
            Call->NodeComment=TEXT("Model clip override; falls back to the original animation");
            UEdGraphPin* Input=Call->FindPin(InputName);
            UEdGraphPin* Output=Call->FindPin(UEdGraphSchema_K2::PN_ReturnValue);
            if (!Input || !Output) { Graph->RemoveNode(Call);Failed=true;continue; }
            Input->DefaultObject=Default;
            if (!Graph->GetSchema()->TryCreateConnection(Output,AssetPin))
            { Graph->RemoveNode(Call);Failed=true;continue; }
            Changed=true;
        }
    }
    if (Failed)
    {
        UE_LOG(LogTemp,Error,TEXT("ABP_Humanoid model animation binding failed; package has not been saved. Check asset player pins."));
        return;
    }
    auto* Defaults=Blueprint->GeneratedClass?Cast<UCharacterAnimInstance>(Blueprint->GeneratedClass->GetDefaultObject()):nullptr;
    const bool DefaultsChanged=!Defaults || (Defaults->DefaultModelSequences!=Sequences || Defaults->DefaultModelBlendSpaces!=BlendSpaces);
    if (!Changed && !DefaultsChanged) return;
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FCompilerResultsLog Results;
    FKismetEditorUtilities::CompileBlueprint(Blueprint,EBlueprintCompileOptions::None,&Results);
    if (Results.NumErrors>0 || !Blueprint->GeneratedClass)
    {
        UE_LOG(LogTemp,Error,TEXT("ABP_Humanoid model animation binding did not compile; package has not been saved"));return;
    }
    Defaults=Cast<UCharacterAnimInstance>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!Defaults) return;
    Defaults->Modify();Defaults->DefaultModelSequences=Sequences;Defaults->DefaultModelBlendSpaces=BlendSpaces;
    UPackage* Package=Blueprint->GetOutermost();Package->MarkPackageDirty();
    const FString Filename=FPackageName::LongPackageNameToFilename(Package->GetName(),FPackageName::GetAssetPackageExtension());
    FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;Args.SaveFlags=SAVE_NoError;
    if (!UPackage::SavePackage(Package,Blueprint,*Filename,Args))
    {
        UE_LOG(LogTemp,Error,TEXT("Could not save upgraded ABP_Humanoid: %s"),*Filename);
    }
    else
    {
        UE_LOG(LogTemp,Display,TEXT("ABP_Humanoid: bound %d sequences and %d BlendSpaces to model animations"),Sequences.Num(),BlendSpaces.Num());
    }
}
