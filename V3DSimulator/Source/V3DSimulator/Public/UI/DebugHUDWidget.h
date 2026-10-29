// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UI/ResponsiveHUDWidget.h"
#include "DebugHUDWidget.generated.h"

class UTextBlock;

/** Low-frequency hardware, pawn motion and active-camera diagnostics; designer preview is world-free. */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API UDebugHUDWidget : public UResponsiveHUDWidget
{
    GENERATED_BODY()

public:
    UDebugHUDWidget(const FObjectInitializer& ObjectInitializer);
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

protected:
    virtual void BuildNativeLayout() override;
    virtual void ApplyNativeStyle() override;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Debug HUD", meta=(ClampMin="0.1", ClampMax="2.0"))
    float RefreshIntervalSeconds = 0.25f;

private:
    UPROPERTY(Transient)
    TObjectPtr<UTextBlock> NativeDiagnostics;

    void UpdateDiagnostics(float DeltaSeconds);
    int32 UpdateHandle = INDEX_NONE;
    int32 SampleFrames = 0;
    float SampleSeconds = 0.0f;
    double VisibleSeconds = 0.0;
    uint64 VisibleFrames = 0;
    FString HardwareDescription;
    int32 TotalMemoryMB = 0;
};
