// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file BuildStatusWidget.h
 * Blueprint-facing project build progress widget with an explicit terminal confirmation step.
 */

#pragma once

#include "CoreMinimal.h"
#include "UI/ResponsiveHUDWidget.h"
#include "BuildStatusWidget.generated.h"

class UGameManagerSubSystem;
class UMenuButtonWidget;
class UPanelWidget;
class UProgressBar;
class UScrollBox;
class UTextBlock;
class UButton;

UENUM(BlueprintType)
enum class EProjectBuildPhase : uint8
{
    Idle UMETA(DisplayName="Idle"),
    Preparing UMETA(DisplayName="Preparing"),
    BuildingModels UMETA(DisplayName="Building Models"),
    WritingArchive UMETA(DisplayName="Writing Archive"),
    Completed UMETA(DisplayName="Completed"),
    Failed UMETA(DisplayName="Failed")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
    FBuildStatusUpdated, float, Progress, const FString&, Message, EProjectBuildPhase, Phase);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FBuildStatusFinished, bool, bSuccess, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FBuildStatusConfirmed);

/**
 * Native, resolution-independent build output with a designer preview in Blueprint children.
 * Explicit setters remain available for projects using a custom layout.
 */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API UBuildStatusWidget : public UResponsiveHUDWidget
{
    GENERATED_BODY()

public:
    UBuildStatusWidget(const FObjectInitializer& ObjectInitializer);
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    UPROPERTY(BlueprintAssignable, Category="Build Status")
    FBuildStatusUpdated OnBuildStatusUpdated;

    UPROPERTY(BlueprintAssignable, Category="Build Status")
    FBuildStatusFinished OnBuildFinished;

    UPROPERTY(BlueprintAssignable, Category="Build Status")
    FBuildStatusConfirmed OnConfirmed;

    UFUNCTION(BlueprintCallable, Category="Build Status|Widgets")
    void SetStatusTextBlock(UTextBlock* InTextBlock);

    /** Optional ScrollBox containing the status text. New terminal lines automatically scroll to the end. */
    UFUNCTION(BlueprintCallable, Category="Build Status|Widgets")
    void SetOutputScrollBox(UScrollBox* InScrollBox);

    UFUNCTION(BlueprintCallable, Category="Build Status|Widgets")
    void SetProgressBar(UProgressBar* InProgressBar);

    UFUNCTION(BlueprintCallable, Category="Build Status|Widgets")
    void SetPercentTextBlock(UTextBlock* InTextBlock);

    UFUNCTION(BlueprintCallable, Category="Build Status|Widgets")
    void SetProjectNameTextBlock(UTextBlock* InTextBlock);

    /** Clears the terminal-style accumulated build output. */
    UFUNCTION(BlueprintCallable, Category="Build Status|Output")
    void ClearBuildOutput();

    /** Returns the complete currently retained terminal-style build output. */
    UFUNCTION(BlueprintPure, Category="Build Status|Output")
    FString GetBuildOutputText() const { return BuildOutputText; }

    /**
     * Assigns an already placed UMenuButtonWidget as the Confirm button in a custom layout.
     * A Blueprint child can still provide its own styling and interactions explicitly.
     */
    UFUNCTION(BlueprintCallable, Category="Build Status|Confirm Button")
    void SetConfirmButtonWidget(UMenuButtonWidget* InWidget);

    /** Optional custom-layout host for a generated native Confirm button; no component asset is loaded. */
    UFUNCTION(BlueprintCallable, Category="Build Status|Confirm Button")
    void SetConfirmButtonHost(UPanelWidget* InHost);

    UFUNCTION(BlueprintPure, Category="Build Status|Confirm Button")
    UMenuButtonWidget* GetConfirmButtonWidget() const { return ConfirmButtonWidget.Get(); }

    /** Starts observing the build that was already started by ProjectSelectionWidget. */
    UFUNCTION(BlueprintCallable, Category="Build Status")
    void BeginObservingBuild(const FString& ProjectName);

    /** Provides a terminal, confirmable error when a request is rejected before workers start. */
    UFUNCTION(BlueprintCallable, Category="Build Status")
    void ReportBuildRejected(const FString& Message);

    UFUNCTION(BlueprintCallable, Category="Build Status")
    void StopObservingBuild();

    UFUNCTION(BlueprintCallable, Category="Build Status")
    void RefreshBuildStatus();

    /** Confirms a finished build. Ignored while the build is still active. */
    UFUNCTION(BlueprintCallable, Category="Build Status")
    void RequestConfirm();

    UFUNCTION(BlueprintPure, Category="Build Status")
    float GetDisplayedProgress() const { return DisplayedProgress; }

    UFUNCTION(BlueprintPure, Category="Build Status")
    FString GetDisplayedMessage() const { return DisplayedMessage; }

    UFUNCTION(BlueprintPure, Category="Build Status")
    EProjectBuildPhase GetBuildPhase() const { return BuildPhase; }

    UFUNCTION(BlueprintPure, Category="Build Status")
    bool IsBuildActive() const;

    UFUNCTION(BlueprintPure, Category="Build Status")
    bool HasTerminalResult() const { return bHasTerminalResult; }

    UFUNCTION(BlueprintPure, Category="Build Status")
    FString GetObservedProjectName() const { return ObservedProjectName; }

protected:
    virtual void BuildNativeLayout() override;
    virtual void BindNativeWidgets() override;
    virtual void ApplyNativeStyle() override;

    /** Maximum number of terminal output lines retained in the status text block. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Build Status|Output", meta=(ClampMin="1", ClampMax="512"))
    int32 MaxOutputLines = 64;

    /** Automatically closes a successfully completed build status window after this delay. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Build Status|Auto Close", meta=(ClampMin="0.0", UIMin="0.0", UIMax="10.0"))
    float AutoCloseDelaySeconds = 1.0f;

    /** Failed builds stay open so their terminal output can be inspected. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Build Status|Auto Close")
    bool bAutoCloseOnSuccess = true;

private:
    UPROPERTY(Transient)
    TObjectPtr<UTextBlock> NativeStatusText;
    UPROPERTY(Transient)
    TObjectPtr<UTextBlock> NativeProjectText;
    UPROPERTY(Transient)
    TObjectPtr<UTextBlock> NativePercentText;
    UPROPERTY(Transient)
    TObjectPtr<UProgressBar> NativeProgressBar;
    UPROPERTY(Transient)
    TObjectPtr<UScrollBox> NativeOutputScroll;
    UPROPERTY(Transient)
    TObjectPtr<UButton> NativeConfirmButton;

    UFUNCTION()
    void HandleBuildProgress(float Progress);

    UFUNCTION()
    void HandleBuildCompleted(bool bSuccess, const FString& Message);

    UFUNCTION()
    void HandleManagerStateChanged();

    UFUNCTION()
    void HandleConfirmButtonClicked(const FString& ButtonKey);

    void BindManager(UGameManagerSubSystem* InManager);
    void UnbindManager();
    void UpdateAssignedWidgets();
    void AppendOutputLine(const FString& Line);
    void RebuildOutputText();
    void ScheduleAutoClose();
    void CancelAutoClose();

    UFUNCTION()
    void HandleAutoCloseTimer();

    EProjectBuildPhase ResolvePhase() const;
    UMenuButtonWidget* EnsureConfirmButtonWidget();
    void ReleaseGeneratedConfirmButtonWidget();
    void BindConfirmButtonWidget(UMenuButtonWidget* InWidget);
    void UnbindConfirmButtonWidget(UMenuButtonWidget* InWidget);

    UPROPERTY(Transient)
    TObjectPtr<UGameManagerSubSystem> Manager;

    TWeakObjectPtr<UTextBlock> StatusTextBlock;
    TWeakObjectPtr<UScrollBox> OutputScrollBox;
    TWeakObjectPtr<UProgressBar> AssignedProgressBar;
    TWeakObjectPtr<UTextBlock> PercentTextBlock;
    TWeakObjectPtr<UTextBlock> ProjectNameTextBlock;
    TWeakObjectPtr<UPanelWidget> ConfirmButtonHost;

    UPROPERTY(Transient)
    TObjectPtr<UMenuButtonWidget> ConfirmButtonWidget;

    bool bGeneratedConfirmButtonWidget = false;

    TArray<FString> OutputLines;
    FString BuildOutputText;
    FTimerHandle AutoCloseTimerHandle;

    FString ObservedProjectName;
    FString DisplayedMessage = TEXT("Waiting for build status...");
    float DisplayedProgress = 0.0f;
    EProjectBuildPhase BuildPhase = EProjectBuildPhase::Idle;
    bool bLastBuildSucceeded = false;
    bool bHasTerminalResult = false;
    bool bConfirmationSent = false;
    uint64 ObservationRevision = 0;
};
