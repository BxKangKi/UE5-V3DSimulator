// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "InputCoreTypes.h"
#include "AnimNotify_V3DFootstep.generated.h"

/** Place at foot contact in an animation; no per-frame polling or sound loading. */
UCLASS(meta=(DisplayName="V3D Footstep"))
class V3DSIMULATOR_API UAnimNotify_V3DFootstep : public UAnimNotify
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Footstep")
    EControllerHand FootSide = EControllerHand::Left;

    virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
        const FAnimNotifyEventReference& EventReference) override;
    virtual FString GetNotifyName_Implementation() const override;
};
