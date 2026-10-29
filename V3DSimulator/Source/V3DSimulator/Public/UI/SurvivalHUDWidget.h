// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UI/ResponsiveHUDWidget.h"
#include "SurvivalHUDWidget.generated.h"

class UBorder;
class UProgressBar;
class UTextBlock;

/** Read-only survival health display. The owning controller supplies authoritative player data. */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API USurvivalHUDWidget : public UResponsiveHUDWidget
{
    GENERATED_BODY()

public:
    USurvivalHUDWidget(const FObjectInitializer& ObjectInitializer);

    /** Updates presentation only; never changes saved or replicated gameplay health. */
    UFUNCTION(BlueprintCallable, Category="Survival HUD")
    void SetHealth(float CurrentHealth, float MaximumHealth);

protected:
    virtual void BuildNativeLayout() override;
    virtual void ApplyNativeStyle() override;

private:
    UPROPERTY(Transient) TObjectPtr<UBorder> NativeHealthPanel;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> NativeHealthText;
    UPROPERTY(Transient) TObjectPtr<UProgressBar> NativeHealthBar;

    float DisplayedHealth = -1.0f;
    float DisplayedMaximumHealth = -1.0f;
};
