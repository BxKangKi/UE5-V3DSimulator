// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "Character/AnimNotify_V3DFootstep.h"
#include "Character/CharacterAnimInstance.h"
#include "Components/SkeletalMeshComponent.h"

void UAnimNotify_V3DFootstep::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
    const FAnimNotifyEventReference& EventReference)
{
    Super::Notify(MeshComp, Animation, EventReference);
    if (!IsInGameThread() || !IsValid(MeshComp)) return;
    if (auto* Instance = Cast<UCharacterAnimInstance>(MeshComp->GetAnimInstance()))
        Instance->PlayFootstep(FootSide);
}

FString UAnimNotify_V3DFootstep::GetNotifyName_Implementation() const
{
    return FootSide == EControllerHand::Left ? TEXT("V3D Footstep Left") : TEXT("V3D Footstep Right");
}
