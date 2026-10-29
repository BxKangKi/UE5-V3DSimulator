// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UI/ResponsiveHUDWidget.h"
#include "LoadingHUDWidget.generated.h"

class UButton;
class UProgressBar;
class UTextBlock;

/** Native loading view. Blueprint children preview its layout without loading a world. */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API ULoadingHUDWidget : public UResponsiveHUDWidget
{
    GENERATED_BODY()

public:
    ULoadingHUDWidget(const FObjectInitializer& ObjectInitializer);
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    UFUNCTION(BlueprintCallable, Category="Loading HUD")
    void RefreshLoadingStatus();

    /** Also available to custom Blueprint loading layouts. */
    UFUNCTION(BlueprintCallable, Category="Loading HUD")
    void BackFromConnection();

protected:
    virtual void BuildNativeLayout() override;
    virtual void ApplyNativeStyle() override;
    virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Loading HUD")
    FText LoadingTitle = NSLOCTEXT("V3DHUD", "LoadingTitle", "Loading...");

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Loading HUD", meta=(ClampMin="0.05", ClampMax="1.0"))
    float RefreshIntervalSeconds = 0.1f;

private:
    UPROPERTY(Transient)
    TObjectPtr<UTextBlock> NativeTitle;
    UPROPERTY(Transient)
    TObjectPtr<UTextBlock> NativeStatus;
    UPROPERTY(Transient)
    TObjectPtr<UProgressBar> NativeProgress;
    UPROPERTY(Transient)
    TObjectPtr<UButton> NativeBack;

    void UpdateStatus(float DeltaSeconds);
    int32 UpdateHandle = INDEX_NONE;
    float RefreshElapsed = 0.0f;
};
