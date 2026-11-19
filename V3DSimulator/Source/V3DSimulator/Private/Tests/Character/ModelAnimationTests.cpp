// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Character/CharacterAnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelAnimationOverrideTest,"V3DSimulator.Character.ModelAnimationOverrides",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FModelAnimationOverrideTest::RunTest(const FString&)
{
    auto* Instance=NewObject<UCharacterAnimInstance>();
    auto* Walk=NewObject<UAnimSequence>(Instance,TEXT("AS_Walk"));
    auto* Run=NewObject<UAnimSequence>(Instance,TEXT("AS_Run"));
    auto* Jump=NewObject<UAnimSequence>(Instance,TEXT("AS_Jump"));
    auto* CrouchWalk=NewObject<UAnimSequence>(Instance,TEXT("AS_Crouched_Walking"));
    auto* Swim=NewObject<UAnimSequence>(Instance,TEXT("AS_Swimming"));
    auto* Idle=NewObject<UAnimSequence>(Instance,TEXT("AS_Idle"));
    Instance->DefaultModelSequences={Walk,Run,Jump,CrouchWalk,Swim,Idle};
    auto* ModelWalk=NewObject<UAnimSequence>(Instance);
    auto* ModelRun=NewObject<UAnimSequence>(Instance);
    auto* ModelJump=NewObject<UAnimSequence>(Instance);
    auto* ModelCrouch=NewObject<UAnimSequence>(Instance);
    auto* ModelSwim=NewObject<UAnimSequence>(Instance);
    TMap<FName,TObjectPtr<UAnimSequence>> Clips;
    Clips.Add(TEXT("walk"),ModelWalk);Clips.Add(TEXT("Run"),ModelRun);Clips.Add(TEXT("Jump"),ModelJump);
    Clips.Add(TEXT("Crouch"),ModelCrouch);Clips.Add(TEXT("Swim"),ModelSwim);
    Instance->InstallModelAnimations(Clips);
    TestEqual(TEXT("Walk replacement"),Instance->ResolveModelAnimation(Walk),ModelWalk);
    TestEqual(TEXT("Run replacement"),Instance->ResolveModelAnimation(Run),ModelRun);
    TestEqual(TEXT("Jump replacement"),Instance->ResolveModelAnimation(Jump),ModelJump);
    TestEqual(TEXT("Crouch fallback"),Instance->ResolveModelAnimation(CrouchWalk),ModelCrouch);
    TestEqual(TEXT("Swim alias"),Instance->ResolveModelAnimation(Swim),ModelSwim);
    TestEqual(TEXT("Missing clip retains default"),Instance->ResolveModelAnimation(Idle),Idle);
    auto* Other=NewObject<UCharacterAnimInstance>();Other->DefaultModelSequences={Walk};
    TestEqual(TEXT("Another character is unaffected"),Other->ResolveModelAnimation(Walk),Walk);
    Instance->InstallModelAnimations({});
    TestEqual(TEXT("Reload clears stale override"),Instance->ResolveModelAnimation(Walk),Walk);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelBlendSpaceOverrideTest,"V3DSimulator.Character.ModelBlendSpaceOverrides",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FModelBlendSpaceOverrideTest::RunTest(const FString&)
{
    auto* Default = LoadObject<UBlendSpace>(nullptr,
        TEXT("/Game/Resources/Animations/BlendShapes/BS_Move.BS_Move"));
    if (!TestNotNull(TEXT("Humanoid movement BlendSpace exists"), Default)) return false;
    const FBlendSample* SourceSample = Default->GetBlendSamples().FindByPredicate(
        [](const FBlendSample& Sample) { return IsValid(Sample.Animation); });
    if (!TestNotNull(TEXT("Movement BlendSpace has an animation sample"), SourceSample)) return false;
    UAnimSequence* OriginalSequence = SourceSample->Animation;
    const bool OriginalMarkerSync = Default->bAllowMarkerBasedSync;
    UAnimationAsset* DefaultAnimation = Default;
    const TArray<FName>* SourceMarkers = DefaultAnimation->GetUniqueMarkerNames();
    const TArray<FName> OriginalMarkers = SourceMarkers ? *SourceMarkers : TArray<FName>();

    auto* Instance = NewObject<UCharacterAnimInstance>();
    Instance->DefaultModelBlendSpaces = {Default};
    auto* ModelSequence = NewObject<UAnimSequence>(Instance);
    TMap<FName, TObjectPtr<UAnimSequence>> Clips;
    Clips.Add(FName(*OriginalSequence->GetName()), ModelSequence);
    Instance->InstallModelAnimations(Clips);
    UBlendSpace* Replacement = Instance->ResolveModelBlendSpace(Default);
    if (!TestTrue(TEXT("Matching clip creates a private BlendSpace"), Replacement && Replacement != Default)) return false;
    TestFalse(TEXT("Imported samples use time-based synchronization"), Replacement->bAllowMarkerBasedSync);
    UAnimationAsset* ReplacementAnimation = Replacement;
    const TArray<FName>* ReplacementMarkers = ReplacementAnimation->GetUniqueMarkerNames();
    TestTrue(TEXT("Copied marker cache is empty"), !ReplacementMarkers || ReplacementMarkers->IsEmpty());
    TestEqual(TEXT("Sample count is unchanged"), Replacement->GetBlendSamples().Num(), Default->GetBlendSamples().Num());
    bool ReplacedSample = false;
    for (int32 Index = 0; Index < Default->GetBlendSamples().Num(); ++Index)
    {
        const auto& Original = Default->GetBlendSamples()[Index];
        const auto& Sample = Replacement->GetBlendSamples()[Index];
        TestEqual(TEXT("Sample coordinates are unchanged"), Sample.SampleValue, Original.SampleValue);
        if (Original.Animation == OriginalSequence)
        {
            TestTrue(TEXT("Matching sample uses the model clip"), Sample.Animation == ModelSequence);
            ReplacedSample = true;
        }
    }
    TestTrue(TEXT("At least one sample was replaced"), ReplacedSample);
    TestEqual(TEXT("Source marker policy remains unchanged"), Default->bAllowMarkerBasedSync, OriginalMarkerSync);
    SourceMarkers = DefaultAnimation->GetUniqueMarkerNames();
    TestTrue(TEXT("Source marker names remain unchanged"),
        (SourceMarkers ? *SourceMarkers : TArray<FName>()) == OriginalMarkers);
    TestTrue(TEXT("Source sequence remains unchanged"), SourceSample->Animation == OriginalSequence);
    Instance->InstallModelAnimations({});
    TestEqual(TEXT("Reload restores the original BlendSpace"), Instance->ResolveModelBlendSpace(Default), Default);
    return true;
}
#endif
