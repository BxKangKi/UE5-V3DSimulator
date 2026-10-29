// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/BuildStatusWidget.h"
#include "UI/HUDLegacyStyle.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/PanelWidget.h"
#include "Components/ProgressBar.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "System/GameManagerSubSystem.h"
#include "UI/MenuButtonWidget.h"

UBuildStatusWidget::UBuildStatusWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    PanelSize = FVector2D(1920.f, 1080.f);
    BackgroundColor = V3DHUDLegacyStyle::LoadingBackground;
    AccentColor = V3DHUDLegacyStyle::ProgressFill;
    BodyFontSize = 16;
    ButtonHeight = 44.f;
    FontOutlineSize = 1;
    SetIsFocusable(true);
}

void UBuildStatusWidget::BuildNativeLayout()
{
    UBorder* Panel = AddPanel(PanelSize, FVector2D::ZeroVector);
    UOverlay* Body = WidgetTree->ConstructWidget<UOverlay>();
    Panel->SetContent(Body);
    NativeProgressBar = WidgetTree->ConstructWidget<UProgressBar>();
    UOverlaySlot* ProgressSlot = Body->AddChildToOverlay(NativeProgressBar);
    ProgressSlot->SetHorizontalAlignment(HAlign_Fill);
    ProgressSlot->SetVerticalAlignment(VAlign_Bottom);
    ProgressSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 20.f));

    USizeBox* OutputSize = WidgetTree->ConstructWidget<USizeBox>();
    OutputSize->SetWidthOverride(900.f);
    UOverlaySlot* OutputPlacement = Body->AddChildToOverlay(OutputSize);
    OutputPlacement->SetHorizontalAlignment(HAlign_Right);
    OutputPlacement->SetVerticalAlignment(VAlign_Bottom);
    OutputPlacement->SetPadding(FMargin(60.f));
    UVerticalBox* Output = WidgetTree->ConstructWidget<UVerticalBox>();
    OutputSize->SetContent(Output);
    NativeProjectText = AddText(Output, FText::GetEmpty());
    NativeProjectText->SetJustification(ETextJustify::Right);
    NativePercentText = AddText(Output, FText::GetEmpty());
    NativePercentText->SetJustification(ETextJustify::Right);
    USizeBox* TerminalSize = WidgetTree->ConstructWidget<USizeBox>();
    TerminalSize->SetHeightOverride(180.f);
    Output->AddChildToVerticalBox(TerminalSize);
    NativeOutputScroll = WidgetTree->ConstructWidget<UScrollBox>();
    TerminalSize->SetContent(NativeOutputScroll);
    NativeStatusText = AddText(NativeOutputScroll, FText::GetEmpty());
    NativeStatusText->SetAutoWrapText(true);
    NativeStatusText->SetJustification(ETextJustify::Right);
    NativeConfirmButton = AddButton(Output, NSLOCTEXT("V3DHUD", "Confirm", "Confirm"));
}

void UBuildStatusWidget::BindNativeWidgets()
{
    if (!IsUsingNativeLayout()) return;
    StatusTextBlock = NativeStatusText;
    OutputScrollBox = NativeOutputScroll;
    AssignedProgressBar = NativeProgressBar;
    PercentTextBlock = NativePercentText;
    ProjectNameTextBlock = NativeProjectText;
    if (IsValid(NativeConfirmButton) && !IsDesignTime())
    {
        NativeConfirmButton->OnClicked.AddUniqueDynamic(this, &UBuildStatusWidget::RequestConfirm);
    }
}

void UBuildStatusWidget::ApplyNativeStyle()
{
    Super::ApplyNativeStyle();
    if (!IsUsingNativeLayout()) return;
    if (IsValid(NativeProgressBar)) NativeProgressBar->SetFillColorAndOpacity(AccentColor);
    if (IsDesignTime())
    {
        if (IsValid(NativeProjectText)) NativeProjectText->SetText(NSLOCTEXT("V3DHUD", "BuildExampleProject", "Example project"));
        if (IsValid(NativeStatusText)) NativeStatusText->SetText(NSLOCTEXT("V3DHUD", "BuildExampleStatus", "Reading project metadata...\nBuilding models in parallel...\nWriting archive data..."));
        if (IsValid(NativeProgressBar)) NativeProgressBar->SetPercent(0.62f);
        if (IsValid(NativePercentText)) NativePercentText->SetText(FText::FromString(TEXT("62%")));
        if (IsValid(NativeConfirmButton)) NativeConfirmButton->SetVisibility(ESlateVisibility::Visible);
        return;
    }
    UpdateAssignedWidgets();
}

void UBuildStatusWidget::NativeConstruct()
{
    Super::NativeConstruct();
    if (!IsDesignTime()) UpdateAssignedWidgets();
}

void UBuildStatusWidget::NativeDestruct()
{
    ++ObservationRevision;
    CancelAutoClose();
    UnbindManager();
    if (IsValid(NativeConfirmButton))
    {
        NativeConfirmButton->OnClicked.RemoveDynamic(this, &UBuildStatusWidget::RequestConfirm);
    }

    if (IsValid(ConfirmButtonWidget))
    {
        UnbindConfirmButtonWidget(ConfirmButtonWidget.Get());
    }
    ReleaseGeneratedConfirmButtonWidget();
    ConfirmButtonWidget = nullptr;

    ConfirmButtonHost.Reset();
    StatusTextBlock.Reset();
    OutputScrollBox.Reset();
    AssignedProgressBar.Reset();
    PercentTextBlock.Reset();
    ProjectNameTextBlock.Reset();
    Super::NativeDestruct();
}

void UBuildStatusWidget::SetStatusTextBlock(UTextBlock* InTextBlock)
{
    if (!AcceptWidgetAssignment(InTextBlock)) return;
    StatusTextBlock = InTextBlock;
    UpdateAssignedWidgets();
}

void UBuildStatusWidget::SetOutputScrollBox(UScrollBox* InScrollBox)
{
    if (!AcceptWidgetAssignment(InScrollBox)) return;
    OutputScrollBox = InScrollBox;
    UpdateAssignedWidgets();
}

void UBuildStatusWidget::SetProgressBar(UProgressBar* InProgressBar)
{
    if (!AcceptWidgetAssignment(InProgressBar)) return;
    AssignedProgressBar = InProgressBar;
    UpdateAssignedWidgets();
}

void UBuildStatusWidget::SetPercentTextBlock(UTextBlock* InTextBlock)
{
    if (!AcceptWidgetAssignment(InTextBlock)) return;
    PercentTextBlock = InTextBlock;
    UpdateAssignedWidgets();
}

void UBuildStatusWidget::SetProjectNameTextBlock(UTextBlock* InTextBlock)
{
    if (!AcceptWidgetAssignment(InTextBlock)) return;
    ProjectNameTextBlock = InTextBlock;
    UpdateAssignedWidgets();
}

void UBuildStatusWidget::ClearBuildOutput()
{
    OutputLines.Reset();
    BuildOutputText.Reset();
    UpdateAssignedWidgets();
}

void UBuildStatusWidget::SetConfirmButtonWidget(UMenuButtonWidget* InWidget)
{
    if (!AcceptWidgetAssignment(InWidget)) return;
    if (ConfirmButtonWidget.Get() == InWidget && !bGeneratedConfirmButtonWidget)
    {
        UpdateAssignedWidgets();
        return;
    }

    if (IsValid(ConfirmButtonWidget))
    {
        UnbindConfirmButtonWidget(ConfirmButtonWidget.Get());
    }
    ReleaseGeneratedConfirmButtonWidget();

    ConfirmButtonWidget = InWidget;
    bGeneratedConfirmButtonWidget = false;
    if (IsValid(InWidget))
    {
        BindConfirmButtonWidget(InWidget);
        InWidget->ConfigureButton(TEXT("Confirm"), TEXT("Confirm"));
    }

    UpdateAssignedWidgets();
}

void UBuildStatusWidget::SetConfirmButtonHost(UPanelWidget* InHost)
{
    if (!AcceptWidgetAssignment(InHost)) return;
    if (ConfirmButtonHost.Get() == InHost)
    {
        EnsureConfirmButtonWidget();
        UpdateAssignedWidgets();
        return;
    }

    if (bGeneratedConfirmButtonWidget)
    {
        if (IsValid(ConfirmButtonWidget))
        {
            UnbindConfirmButtonWidget(ConfirmButtonWidget.Get());
        }
        ReleaseGeneratedConfirmButtonWidget();
    }

    ConfirmButtonHost = InHost;
    EnsureConfirmButtonWidget();
    UpdateAssignedWidgets();
}

UMenuButtonWidget* UBuildStatusWidget::EnsureConfirmButtonWidget()
{
    if (IsDesignTime() || IsUsingNativeLayout()) return nullptr;
    if (IsValid(ConfirmButtonWidget))
    {
        return ConfirmButtonWidget.Get();
    }

    UPanelWidget* const Host = ConfirmButtonHost.Get();
    if (!IsValid(Host))
    {
        return nullptr;
    }

    // Custom top-level layouts may keep their host API, but shared controls are code-only.
    UMenuButtonWidget* const Widget = Cast<UMenuButtonWidget>(
        UUserWidget::CreateWidgetInstance(*this, UMenuButtonWidget::StaticClass(), NAME_None));

    if (!IsValid(Widget))
    {
        UE_LOG(LogTemp, Warning, TEXT("BuildStatusWidget failed to create its native Confirm button."));
        return nullptr;
    }

    ConfirmButtonWidget = Widget;
    bGeneratedConfirmButtonWidget = true;
    BindConfirmButtonWidget(Widget);
    Widget->ConfigureButton(TEXT("Confirm"), TEXT("Confirm"));
    Widget->ButtonMinSize.X = 0.0;
    Widget->FontObject = FontObject;
    Widget->FontOutlineSize = FontOutlineSize;
    Widget->ApplyButtonTheme(TextColor, ButtonColor, HoveredButtonTint, BodyFontSize, ButtonHeight);
    Host->AddChild(Widget);
    return Widget;
}

void UBuildStatusWidget::ReleaseGeneratedConfirmButtonWidget()
{
    if (!bGeneratedConfirmButtonWidget)
    {
        return;
    }

    if (IsValid(ConfirmButtonWidget))
    {
        ConfirmButtonWidget->RemoveFromParent();
    }
    ConfirmButtonWidget = nullptr;
    bGeneratedConfirmButtonWidget = false;
}

void UBuildStatusWidget::BindConfirmButtonWidget(UMenuButtonWidget* InWidget)
{
    if (!IsValid(InWidget))
    {
        return;
    }

    InWidget->OnButtonClicked.RemoveDynamic(this, &UBuildStatusWidget::HandleConfirmButtonClicked);
    InWidget->OnButtonClicked.AddDynamic(this, &UBuildStatusWidget::HandleConfirmButtonClicked);
}

void UBuildStatusWidget::UnbindConfirmButtonWidget(UMenuButtonWidget* InWidget)
{
    if (IsValid(InWidget))
    {
        InWidget->OnButtonClicked.RemoveDynamic(this, &UBuildStatusWidget::HandleConfirmButtonClicked);
    }
}

void UBuildStatusWidget::BeginObservingBuild(const FString& ProjectName)
{
    if (IsDesignTime()) return;
    ++ObservationRevision;
    bConfirmationSent = false;
    CancelAutoClose();
    ObservedProjectName = ProjectName.TrimStartAndEnd();
    bHasTerminalResult = false;
    bLastBuildSucceeded = false;
    DisplayedProgress = 0.0f;
    DisplayedMessage = FString::Printf(TEXT("Preparing project build: %s"), *ObservedProjectName);
    BuildPhase = EProjectBuildPhase::Preparing;
    OutputLines.Reset();
    BuildOutputText.Reset();
    AppendOutputLine(DisplayedMessage);
    BindManager(UGameManagerSubSystem::GetSubSystem(this));
    SetVisibility(ESlateVisibility::Visible);
    RefreshBuildStatus();
}

void UBuildStatusWidget::StopObservingBuild()
{
    ++ObservationRevision;
    CancelAutoClose();
    UnbindManager();
    ObservedProjectName.Reset();
    BuildPhase = EProjectBuildPhase::Idle;
}

void UBuildStatusWidget::ReportBuildRejected(const FString& Message)
{
    if (IsDesignTime() || bHasTerminalResult || IsBuildActive()) return;
    HandleBuildCompleted(false, Message);
}

bool UBuildStatusWidget::IsBuildActive() const
{
    return IsValid(Manager) && Manager->IsProjectBuildInProgress();
}

void UBuildStatusWidget::BindManager(UGameManagerSubSystem* InManager)
{
    if (Manager == InManager)
    {
        return;
    }
    UnbindManager();
    Manager = InManager;
    if (IsValid(Manager))
    {
        Manager->OnProjectBuildProgress.RemoveDynamic(this, &UBuildStatusWidget::HandleBuildProgress);
        Manager->OnProjectBuildProgress.AddDynamic(this, &UBuildStatusWidget::HandleBuildProgress);
        Manager->OnProjectBuildCompleted.RemoveDynamic(this, &UBuildStatusWidget::HandleBuildCompleted);
        Manager->OnProjectBuildCompleted.AddDynamic(this, &UBuildStatusWidget::HandleBuildCompleted);
        Manager->OnStateChanged.RemoveDynamic(this, &UBuildStatusWidget::HandleManagerStateChanged);
        Manager->OnStateChanged.AddDynamic(this, &UBuildStatusWidget::HandleManagerStateChanged);
    }
}

void UBuildStatusWidget::UnbindManager()
{
    if (IsValid(Manager))
    {
        Manager->OnProjectBuildProgress.RemoveDynamic(this, &UBuildStatusWidget::HandleBuildProgress);
        Manager->OnProjectBuildCompleted.RemoveDynamic(this, &UBuildStatusWidget::HandleBuildCompleted);
        Manager->OnStateChanged.RemoveDynamic(this, &UBuildStatusWidget::HandleManagerStateChanged);
    }
    Manager = nullptr;
}

void UBuildStatusWidget::HandleBuildProgress(const float Progress)
{
    if (bHasTerminalResult || bConfirmationSent) return;
    if (FMath::IsFinite(Progress))
        DisplayedProgress = FMath::Max(DisplayedProgress, FMath::Clamp(Progress, 0.0f, 1.0f));
    bHasTerminalResult = false;
    RefreshBuildStatus();
}

void UBuildStatusWidget::HandleBuildCompleted(const bool bSuccess, const FString& Message)
{
    const uint64 CompletedRevision = ObservationRevision;
    CancelAutoClose();
    bHasTerminalResult = true;
    bLastBuildSucceeded = bSuccess;
    DisplayedProgress = 1.0f;
    DisplayedMessage = Message.IsEmpty()
        ? (bSuccess ? TEXT("Build completed successfully.") : TEXT("Build failed."))
        : Message;
    BuildPhase = bSuccess ? EProjectBuildPhase::Completed : EProjectBuildPhase::Failed;
    AppendOutputLine(DisplayedMessage);
    UpdateAssignedWidgets();
    const FString TerminalMessage = DisplayedMessage;
    const EProjectBuildPhase TerminalPhase = BuildPhase;
    OnBuildStatusUpdated.Broadcast(1.0f, TerminalMessage, TerminalPhase);
    OnBuildFinished.Broadcast(bSuccess, TerminalMessage);

    // Blueprint callbacks may confirm this result, begin another build, or remove this view.
    if (CompletedRevision == ObservationRevision && !bConfirmationSent && bSuccess && bAutoCloseOnSuccess)
    {
        ScheduleAutoClose();
    }
}

void UBuildStatusWidget::HandleManagerStateChanged()
{
    RefreshBuildStatus();
}

void UBuildStatusWidget::HandleConfirmButtonClicked(const FString& ButtonKey)
{
    RequestConfirm();
}

EProjectBuildPhase UBuildStatusWidget::ResolvePhase() const
{
    if (bHasTerminalResult)
    {
        return bLastBuildSucceeded ? EProjectBuildPhase::Completed : EProjectBuildPhase::Failed;
    }
    if (!IsValid(Manager) || !Manager->IsProjectBuildInProgress())
    {
        return EProjectBuildPhase::Idle;
    }
    if (!Manager->IsProjectBuildInProgress() || DisplayedProgress < 0.05f)
    {
        return EProjectBuildPhase::Preparing;
    }
    if (DisplayedProgress >= 0.95f)
    {
        return EProjectBuildPhase::WritingArchive;
    }
    return EProjectBuildPhase::BuildingModels;
}

void UBuildStatusWidget::RefreshBuildStatus()
{
    if (IsDesignTime()) return;
    if (!IsValid(Manager))
    {
        BindManager(UGameManagerSubSystem::GetSubSystem(this));
    }
    if (IsValid(Manager))
    {
        const float Progress = Manager->GetProjectBuildProgress();
        if (FMath::IsFinite(Progress))
            DisplayedProgress = FMath::Max(DisplayedProgress, FMath::Clamp(Progress, 0.0f, 1.0f));
        const FString Message = Manager->GetLastMessage();
        if (!Message.IsEmpty() && Message != DisplayedMessage)
        {
            DisplayedMessage = Message;
            AppendOutputLine(DisplayedMessage);
        }
    }
    BuildPhase = ResolvePhase();
    UpdateAssignedWidgets();
    OnBuildStatusUpdated.Broadcast(DisplayedProgress, DisplayedMessage, BuildPhase);
}

void UBuildStatusWidget::AppendOutputLine(const FString& Line)
{
    FString Normalized = Line;
    Normalized.TrimStartAndEndInline();
    if (Normalized.IsEmpty())
    {
        return;
    }

    // A state delegate may fire repeatedly while the same operation is active. Avoid duplicate lines.
    if (OutputLines.Num() > 0 && OutputLines.Last() == Normalized)
    {
        return;
    }

    // Preserve embedded newlines as separate terminal rows.
    TArray<FString> NewLines;
    Normalized.ParseIntoArrayLines(NewLines, false);
    if (NewLines.Num() == 0)
    {
        NewLines.Add(Normalized);
    }

    for (FString& NewLine : NewLines)
    {
        NewLine.TrimStartAndEndInline();
        if (!NewLine.IsEmpty() && (OutputLines.Num() == 0 || OutputLines.Last() != NewLine))
        {
            OutputLines.Add(MoveTemp(NewLine));
        }
    }

    const int32 SafeMaxLines = FMath::Clamp(MaxOutputLines, 1, 512);
    if (OutputLines.Num() > SafeMaxLines)
    {
        OutputLines.RemoveAt(0, OutputLines.Num() - SafeMaxLines, EAllowShrinking::No);
    }

    RebuildOutputText();
}

void UBuildStatusWidget::RebuildOutputText()
{
    BuildOutputText = FString::Join(OutputLines, TEXT("\n"));
}

void UBuildStatusWidget::ScheduleAutoClose()
{
    if (IsDesignTime()) return;
    CancelAutoClose();

    if (!bHasTerminalResult || !bLastBuildSucceeded || !bAutoCloseOnSuccess)
    {
        return;
    }

    if (!FMath::IsFinite(AutoCloseDelaySeconds) || AutoCloseDelaySeconds <= KINDA_SMALL_NUMBER)
    {
        HandleAutoCloseTimer();
        return;
    }

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().SetTimer(
            AutoCloseTimerHandle,
            this,
            &UBuildStatusWidget::HandleAutoCloseTimer,
            AutoCloseDelaySeconds,
            false);
    }
}

void UBuildStatusWidget::CancelAutoClose()
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(AutoCloseTimerHandle);
    }
    AutoCloseTimerHandle.Invalidate();
}

void UBuildStatusWidget::HandleAutoCloseTimer()
{
    AutoCloseTimerHandle.Invalidate();
    if (bHasTerminalResult && bLastBuildSucceeded && !IsBuildActive())
    {
        RequestConfirm();
    }
}

void UBuildStatusWidget::UpdateAssignedWidgets()
{
    if (IsDesignTime()) return;
    bool bOutputChanged = false;
    if (UTextBlock* Text = StatusTextBlock.Get())
    {
        const FText Value = FText::FromString(BuildOutputText.IsEmpty() ? DisplayedMessage : BuildOutputText);
        bOutputChanged = !Text->GetText().EqualTo(Value);
        if (bOutputChanged) Text->SetText(Value);
    }
    if (UScrollBox* ScrollBox = OutputScrollBox.Get(); bOutputChanged && IsValid(ScrollBox))
    {
        ScrollBox->ScrollToEnd();
    }
    if (UProgressBar* Progress = AssignedProgressBar.Get())
    {
        Progress->SetPercent(FMath::Clamp(DisplayedProgress, 0.0f, 1.0f));
    }
    if (UTextBlock* Text = PercentTextBlock.Get())
    {
        Text->SetText(FText::FromString(FString::Printf(
            TEXT("%d%%"), FMath::RoundToInt(FMath::Clamp(DisplayedProgress, 0.0f, 1.0f) * 100.0f))));
    }
    if (UTextBlock* Text = ProjectNameTextBlock.Get())
    {
        Text->SetText(FText::FromString(ObservedProjectName));
    }

    const bool bShowConfirm = bHasTerminalResult && !bConfirmationSent && !IsBuildActive();
    if (IsUsingNativeLayout() && IsValid(NativeConfirmButton))
    {
        NativeConfirmButton->SetIsEnabled(bShowConfirm);
        NativeConfirmButton->SetVisibility(bShowConfirm ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    }
    else if (UMenuButtonWidget* ConfirmWidget = EnsureConfirmButtonWidget())
    {
        ConfirmWidget->SetIsEnabled(bShowConfirm);
        ConfirmWidget->SetVisibility(bShowConfirm ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    }
}

void UBuildStatusWidget::RequestConfirm()
{
    if (IsBuildActive() || !bHasTerminalResult || bConfirmationSent)
    {
        return;
    }

    CancelAutoClose();
    bConfirmationSent = true;
    const uint64 ConfirmedRevision = ObservationRevision;
    SetVisibility(ESlateVisibility::Collapsed);
    OnConfirmed.Broadcast();

    // Keep the terminal result available during the callbacks, then detach from the manager.
    if (ConfirmedRevision == ObservationRevision && IsValid(Manager))
    {
        StopObservingBuild();
    }
}
