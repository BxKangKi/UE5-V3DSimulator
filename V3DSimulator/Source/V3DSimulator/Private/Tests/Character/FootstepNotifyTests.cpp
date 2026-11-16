// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Character/CharacterAnimInstance.h"
#include "Character/AnimNotify_V3DFootstep.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimNotifyQueue.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DFootstepNotifyContractTest,
    "V3DSimulator.Character.Footsteps.AnimInstanceAndPreviewSafety",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DFootstepNotifyContractTest::RunTest(const FString&)
{
    UClass* Class = UCharacterAnimInstance::StaticClass();
    for (const FName Name : {FName(TEXT("PlayFootstep")), FName(TEXT("TriggerFootstepTrace"))})
    {
        const UFunction* Function = Class->FindFunctionByName(Name);
        if (!TestNotNull(TEXT("Footstep entry point is reflected on AnimInstance"), Function)) continue;
        TestTrue(TEXT("AnimBP event graph can call footstep entry point"), Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
        TestFalse(TEXT("Audio/trace calls are not exposed as pure animation evaluation"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
        TestNotNull(TEXT("Foot side remains selectable"), FindFProperty<FProperty>(Function, TEXT("FootSide")));
    }
    TestNotNull(TEXT("Left named notify is discoverable"), Class->FindFunctionByName(TEXT("AnimNotify_FootstepLeft")));
    TestNotNull(TEXT("Right named notify is discoverable"), Class->FindFunctionByName(TEXT("AnimNotify_FootstepRight")));

    // Animation asset previews have no gameplay character/world. Every entry must safely no-op.
    TStrongObjectPtr<USkeletalMeshComponent> Mesh(NewObject<USkeletalMeshComponent>());
    TStrongObjectPtr<UCharacterAnimInstance> Instance(NewObject<UCharacterAnimInstance>(Mesh.Get()));
    Instance->PlayFootstep(EControllerHand::Left);
    Instance->TriggerFootstepTrace(EControllerHand::Right);
    Instance->PlayFootstep(EControllerHand::AnyHand);
    Instance->AnimNotify_FootstepLeft();
    Instance->AnimNotify_FootstepRight();
    TStrongObjectPtr<UAnimNotify_V3DFootstep> Notify(NewObject<UAnimNotify_V3DFootstep>());
    const FAnimNotifyEventReference Event;
    Notify->Notify(nullptr, nullptr, Event);
    Notify->Notify(Mesh.Get(), nullptr, Event);
    Notify->FootSide = EControllerHand::Left;
    TestEqual(TEXT("Left notify label"), Notify->GetNotifyName_Implementation(), FString(TEXT("V3D Footstep Left")));
    Notify->FootSide = EControllerHand::Right;
    TestEqual(TEXT("Right notify label"), Notify->GetNotifyName_Implementation(), FString(TEXT("V3D Footstep Right")));
    return true;
}
#endif
