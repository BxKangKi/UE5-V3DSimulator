// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/ProjectSelectionWidget.h"
#include "UI/ProjectEditorWidget.h"
#include "UI/WidgetStyleSafety.h"
#include "UI/HUDStyle.h"

#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/Border.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ComboBoxString.h"
#include "Components/CheckBox.h"
#include "Components/MultiLineEditableTextBox.h"
#include "System/ProjectRepository.h"
#include "System/ProjectWorkspace.h"
#include "System/SafeFileIO.h"
#include "System/JsonMetadata.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/PanelWidget.h"
#include "Components/ScrollBox.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateColorBrush.h"
#include "GameFramework/PlayerController.h"
#include "System/GameManagerSubSystem.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/V3DSimulatorGameInstance.h"
#include "UI/BuildStatusWidget.h"
#include "UI/MenuButtonWidget.h"
#include "System/MacroLibrary.h"
#include "System/SimulatorFileServices.h"
#include "System/ProjectConfig.h"
#include "UI/MainWorldWidget.h"
#include "World/WorldData.h"
#include "InputCoreTypes.h"

namespace
{
    const FString CreateProjectSelectionKey = TEXT("__create_project__");
}

UProjectSelectionWidget::UProjectSelectionWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    PanelSize = FVector2D(1920.0, 1080.0);
    PanelPadding = 0.0f;
    ButtonHeight = 64.0f;
    BodyFontSize = 32;
    TitleFontSize = 100;
    SetIsFocusable(true);
}

void UProjectSelectionWidget::NativeConstruct()
{
    Super::NativeConstruct();
    if (IsDesignTime()) return;
    ClearFlags(RF_Transactional);
    if (!IsValid(GetSelectionListPanel()))
        SetSelectionListPanel(Cast<UPanelWidget>(GetWidgetFromName(TEXT("ProjectListPanel"))));
    if (!BackButton.IsValid())
        SetBackButton(Cast<UButton>(GetWidgetFromName(TEXT("BackButton"))));
    if (!RefreshButton.IsValid())
        SetRefreshButton(Cast<UButton>(GetWidgetFromName(TEXT("RefreshButton"))));
    BindNavigationButtons();
    SetCreateProjectMode(false, false);

    RefreshProjects();
}

void UProjectSelectionWidget::NativeDestruct()
{
    if (EditorWidget) { EditorWidget->OnClosed.Unbind(); EditorWidget->RemoveFromParent(); EditorWidget = nullptr; }
    UnbindNavigationButtons();
    if (NativeProjectType) NativeProjectType->OnSelectionChanged.RemoveDynamic(this, &UProjectSelectionWidget::HandleNativeProjectTypeChanged);
    if (NativeCreateBackButton) NativeCreateBackButton->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::HandleCreateProjectCancelClicked);
    if (NativeCreateCancelButton) NativeCreateCancelButton->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::HandleCreateProjectCancelClicked);
    if (NativeCreateConfirmButton) NativeCreateConfirmButton->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::HandleCreateProjectConfirmClicked);
    if (IsValid(BuildStatusWidget))
    {
        BuildStatusWidget->OnConfirmed.RemoveDynamic(
            this, &UProjectSelectionWidget::HandleBuildStatusConfirmed);
        BuildStatusWidget->StopObservingBuild();
        if (bGeneratedBuildStatusWidget)
        {
            BuildStatusWidget->RemoveFromParent();
        }
    }
    bGeneratedBuildStatusWidget = false;

    if (UMainWorldWidget* Owner = OwnerMainWidget.Get())
    {
        Owner->HandleProjectSelectionWidgetRemoved(this);
    }
    SetCreateProjectMode(false, true);
    OwnerMainWidget.Reset();
    BackButton.Reset();
    RefreshButton.Reset();
    NewProjectNameTextBox.Reset();
    BuildStatusWidget = nullptr;

    Super::NativeDestruct();
}

FReply UProjectSelectionWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
    if (!InKeyEvent.IsRepeat()
        && (InKeyEvent.GetKey() == EKeys::Escape || InKeyEvent.GetKey() == EKeys::Gamepad_FaceButton_Right))
    {
        if (bCreateProjectMode && IsValid(NativeCreateProjectScreen))
        {
            SetCreateProjectMode(false, true);
            return FReply::Handled();
        }
        CloseProjectSelection();
        return FReply::Handled();
    }
    return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UProjectSelectionWidget::SetOwnerMainWidget(UMainWorldWidget* InOwnerWidget)
{
    OwnerMainWidget = InOwnerWidget;
}

void UProjectSelectionWidget::SetProjectListPanel(UPanelWidget* InPanel)
{
    if (!AcceptWidgetAssignment(InPanel)) return;
    SetCreateProjectMode(false, true);
    SetSelectionListPanel(InPanel);
    RefreshProjects();
}

void UProjectSelectionWidget::SetBackButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Existing = BackButton.Get())
    {
        Existing->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::CloseProjectSelection);
    }

    BackButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::CloseProjectSelection);
        InButton->OnClicked.AddDynamic(this, &UProjectSelectionWidget::CloseProjectSelection);
    }
}

void UProjectSelectionWidget::SetRefreshButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Existing = RefreshButton.Get())
    {
        Existing->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::RefreshProjects);
    }

    RefreshButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::RefreshProjects);
        InButton->OnClicked.AddDynamic(this, &UProjectSelectionWidget::RefreshProjects);
    }
}

void UProjectSelectionWidget::SetNewProjectNameTextBox(UEditableTextBox* InTextBox)
{
    if (!AcceptWidgetAssignment(InTextBox)) return;
    NewProjectNameTextBox = InTextBox;
}

void UProjectSelectionWidget::SetCreateProjectMode(const bool bEnabled, const bool bClearText)
{
    bCreateProjectMode = bEnabled;
    if (IsValid(NativeProjectListScreen))
    {
        NativeProjectListScreen->SetVisibility(bCreateProjectMode ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
    }
    if (IsValid(NativeCreateProjectScreen))
    {
        NativeCreateProjectScreen->SetVisibility(bCreateProjectMode ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    }
    if (bClearText)
    {
        if (UEditableTextBox* TextBox = NewProjectNameTextBox.Get())
        {
            TextBox->SetText(FText::GetEmpty());
        }
        if (IsValid(NativeProjectNotes))
        {
            NativeProjectNotes->SetText(FText::GetEmpty());
        }
        if (IsValid(NativeCreateErrorText))
        {
            NativeCreateErrorText->SetText(FText::GetEmpty());
            NativeCreateErrorText->SetVisibility(ESlateVisibility::Collapsed);
        }
    }
    UpdateWorldOptionsVisibility();
}

void UProjectSelectionWidget::UpdateWorldOptionsVisibility()
{
    if (IsValid(NativeWorldOptionsBox))
    {
        NativeWorldOptionsBox->SetVisibility(
            NewProjectType == EV3DSimulatorProjectType::World
                ? ESlateVisibility::Visible
                : ESlateVisibility::Collapsed);
    }
}

void UProjectSelectionWidget::SetBuildStatusWidget(UBuildStatusWidget* InWidget)
{
    if (BuildStatusWidget == InWidget)
    {
        return;
    }
    if (IsValid(BuildStatusWidget))
    {
        BuildStatusWidget->OnConfirmed.RemoveDynamic(
            this, &UProjectSelectionWidget::HandleBuildStatusConfirmed);
        if (bGeneratedBuildStatusWidget)
        {
            BuildStatusWidget->RemoveFromParent();
        }
    }
    bGeneratedBuildStatusWidget = false;
    BuildStatusWidget = InWidget;
    if (IsValid(BuildStatusWidget))
    {
        BuildStatusWidget->OnConfirmed.RemoveDynamic(
            this, &UProjectSelectionWidget::HandleBuildStatusConfirmed);
        BuildStatusWidget->OnConfirmed.AddDynamic(
            this, &UProjectSelectionWidget::HandleBuildStatusConfirmed);
        BuildStatusWidget->SetVisibility(ESlateVisibility::Collapsed);
    }
}

UBuildStatusWidget* UProjectSelectionWidget::EnsureBuildStatusWidget()
{
    if (IsValid(BuildStatusWidget))
    {
        return BuildStatusWidget.Get();
    }

    APlayerController* PlayerController = GetOwningPlayer();
    UV3DSimulatorAssetRegistry* Registry =
        UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this);
    if (!IsValid(PlayerController)) return nullptr;
    UClass* WidgetClass = IsValid(Registry) && !Registry->BuildStatusWidgetClass.IsNull()
        ? Registry->BuildStatusWidgetClass.LoadSynchronous() : nullptr;
    if (!IsValid(WidgetClass) || !WidgetClass->IsChildOf(UBuildStatusWidget::StaticClass())
        || WidgetClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
    {
        UE_LOG(LogTemp, Warning, TEXT("Build status class unavailable; using the native build status widget."));
        WidgetClass = UBuildStatusWidget::StaticClass();
    }

    UBuildStatusWidget* Widget = CreateWidget<UBuildStatusWidget>(PlayerController, WidgetClass);
    if (!IsValid(Widget))
    {
        return nullptr;
    }
    if (!Widget->AddToPlayerScreen(40)) return nullptr;
    SetBuildStatusWidget(Widget);
    bGeneratedBuildStatusWidget = true;
    return BuildStatusWidget.Get();
}

void UProjectSelectionWidget::ShowBuildStatus(const FString& ProjectName)
{
    if (UBuildStatusWidget* Widget = EnsureBuildStatusWidget())
    {
        SetVisibility(ESlateVisibility::Collapsed);
        Widget->BeginObservingBuild(ProjectName);
        Widget->SetVisibility(ESlateVisibility::Visible);
    }
}

void UProjectSelectionWidget::HandleBuildStatusConfirmed()
{
    if (IsValid(BuildStatusWidget))
    {
        BuildStatusWidget->SetVisibility(ESlateVisibility::Collapsed);
        BuildStatusWidget->StopObservingBuild();
    }
    SetVisibility(ESlateVisibility::Visible);
    RefreshProjects();
}

void UProjectSelectionWidget::BindNavigationButtons()
{
    if (UButton* Button = BackButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::CloseProjectSelection);
        Button->OnClicked.AddDynamic(this, &UProjectSelectionWidget::CloseProjectSelection);
    }

    if (UButton* Button = RefreshButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::RefreshProjects);
        Button->OnClicked.AddDynamic(this, &UProjectSelectionWidget::RefreshProjects);
    }
}

void UProjectSelectionWidget::UnbindNavigationButtons()
{
    if (UButton* Button = BackButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::CloseProjectSelection);
    }

    if (UButton* Button = RefreshButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::RefreshProjects);
    }
}

void UProjectSelectionWidget::RefreshProjects()
{
    if (IsDesignTime()) return;
    if (!IsValid(GetSelectionListPanel()))
        SetSelectionListPanel(Cast<UPanelWidget>(GetWidgetFromName(TEXT("ProjectListPanel"))));
    if (!IsValid(GetSelectionListPanel()) && IsValid(WidgetTree))
    {
        TArray<UWidget*> Widgets;
        WidgetTree->GetAllWidgets(Widgets);
        UScrollBox* Candidate = nullptr;
        int32 Count = 0;
        for (UWidget* Widget : Widgets)
            if (UScrollBox* Box = Cast<UScrollBox>(Widget)) { Candidate = Box; ++Count; }
        if (Count == 1) SetSelectionListPanel(Candidate);
    }
    SetCreateProjectMode(false, true);
    ClearGeneratedSelectionEntries();

    if (!IsValid(GetSelectionListPanel()))
    {
        FSimulatorFileServices::WriteLogAsync(TEXT("UI"), FString::Printf(
            TEXT("Project host missing. Widget=%s Root=%s"), *GetClass()->GetPathName(), *GetNameSafe(GetRootWidget())));
        UE_LOG(LogTemp, Warning,
            TEXT("ProjectSelectionWidget requires SetProjectListPanel() from the derived WBP."));
        return;
    }

    UWidget* CreateEntry = AddGeneratedSelectionEntry(CreateProjectSelectionKey, TEXT("Create Project"));
    if (!IsValid(CreateEntry))
    {
        UE_LOG(LogTemp, Warning, TEXT("ProjectSelectionWidget could not create the project-creation entry."));
    }
    FSimulatorFileServices::WriteLogAsync(TEXT("UI"), FString::Printf(
        TEXT("Project list: Host=%s CreateEntry=%s"), *GetNameSafe(GetSelectionListPanel()), *GetNameSafe(CreateEntry)));

    UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this);
    if (!IsValid(Manager))
    {
        UE_LOG(LogTemp, Error, TEXT("ProjectSelectionWidget cannot enumerate projects because GameManagerSubSystem is unavailable."));
        return;
    }

    TArray<FV3DSimulatorProjectSummary> Projects;
    Manager->GetProjectSummaries(Projects);
    for (const FV3DSimulatorProjectSummary& Project : Projects)
    {
        const FString Label = FString::Printf(
            TEXT("%s [%s]"),
            *Project.DisplayName,
            *V3DSimulatorProjectTypes::ToString(Project.ProjectType));
        if (!IsValid(AddGeneratedSelectionEntry(Project.FolderName, Label)))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("ProjectSelectionWidget failed to create a generated button for %s."),
                *Project.FolderName);
        }
    }
}

bool UProjectSelectionWidget::CreateNewProject(const FString& ProjectName)
{
    const FString TrimmedName = ProjectName.TrimStartAndEnd();
    if (TrimmedName.IsEmpty())
    {
        if (IsValid(NativeCreateErrorText))
        {
            NativeCreateErrorText->SetText(NSLOCTEXT("V3DMenu", "EmptyProjectNameError", "Please enter a project name."));
            NativeCreateErrorText->SetVisibility(ESlateVisibility::Visible);
        }
        return false;
    }

    UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this);
    if (!IsValid(Manager) || !Manager->CreateProject(TrimmedName, NewProjectType))
    {
        const FString Error = IsValid(Manager) ? Manager->GetLastMessage()
            : TEXT("Project service is unavailable.");
        if (IsValid(NativeCreateErrorText))
        {
            NativeCreateErrorText->SetText(FText::FromString(Error));
            NativeCreateErrorText->SetVisibility(ESlateVisibility::Visible);
        }
        return false;
    }

    const FString NotesText = IsValid(NativeProjectNotes)
        ? NativeProjectNotes->GetText().ToString().TrimStartAndEnd()
        : FString();

    FV3DSimulatorProjectConfig Config;
    TSharedPtr<FJsonObject> Json;
    FString LoadError;
    if (V3DSimulatorProjectRepository::LoadProject(TrimmedName, Config, &Json, LoadError) && Json.IsValid())
    {
        bool bModifiedConfig = false;
        if (!NotesText.IsEmpty())
        {
            Json->SetStringField(V3DSimulatorJsonMetadata::Notes, NotesText);
            bModifiedConfig = true;
        }
        if (NewProjectType == EV3DSimulatorProjectType::World)
        {
            Json->SetBoolField(TEXT("bAllowProjectAssets"), NewProjectExternalAssets && NewProjectExternalAssets->IsChecked());
            auto Audio = MakeShared<FJsonObject>();
            Audio->SetBoolField(TEXT("bGlobalAcoustics"), NewProjectAcoustics && NewProjectAcoustics->IsChecked());
            Audio->SetBoolField(TEXT("bEnvironmentMusic"), !NewProjectMusic || NewProjectMusic->IsChecked());
            Json->SetObjectField(TEXT("Audio"), Audio);
            bModifiedConfig = true;
        }
        if (bModifiedConfig)
        {
            const auto Saved = FSafeFileIO::SaveJsonBlocking(Json.ToSharedRef(), V3DSimulatorProjectWorkspace::ConfigPath(TrimmedName));
            if (!Saved.IsSuccess()) UE_LOG(LogTemp, Error, TEXT("New project options: %s"), *Saved.Error);
        }
    }

    // Transition directly to the JSON editor so the user can start working immediately.
    SetCreateProjectMode(false, true);
    RefreshProjects();
    OpenEditor(TrimmedName);
    return true;
}

bool UProjectSelectionWidget::BuildProjectByName(const FString& ProjectName)
{
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        return Manager->BuildProjectByName(ProjectName);
    }

    UE_LOG(LogTemp, Error, TEXT("Project build failed because GameManagerSubSystem is unavailable."));
    return false;
}

bool UProjectSelectionWidget::GetProjectConfiguration(
    const FString& ProjectName,
    EV3DSimulatorProjectType& OutProjectType,
    bool& bOutAllowProjectAssets,
    FString& OutDisplayName) const
{
    if (const UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        return Manager->GetProjectConfigurationByName(
            ProjectName, OutProjectType, bOutAllowProjectAssets, OutDisplayName);
    }
    return false;
}

bool UProjectSelectionWidget::SetProjectType(
    const FString& ProjectName,
    const EV3DSimulatorProjectType ProjectType)
{
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        const bool bChanged = Manager->SetProjectTypeByName(ProjectName, ProjectType);
        if (bChanged) RefreshProjects();
        return bChanged;
    }
    return false;
}

bool UProjectSelectionWidget::SetWorldProjectAssetsAllowed(
    const FString& ProjectName,
    const bool bAllowed)
{
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        const bool bChanged = Manager->SetWorldProjectAssetsAllowedByName(ProjectName, bAllowed);
        if (bChanged) RefreshProjects();
        return bChanged;
    }
    return false;
}

void UProjectSelectionWidget::CloseProjectSelection()
{
    SetCreateProjectMode(false, true);

    if (UMainWorldWidget* Owner = OwnerMainWidget.Get())
    {
        Owner->CloseProjectSelectionWidget();
        return;
    }

    SetVisibility(ESlateVisibility::Collapsed);
}

void UProjectSelectionWidget::OnSelectionEntryActivated(const FString& SelectionKey)
{
    if (SelectionKey == CreateProjectSelectionKey)
    {
        SetCreateProjectMode(true, true);
        if (UEditableTextBox* TextBox = NewProjectNameTextBox.Get())
        {
            TextBox->SetKeyboardFocus();
        }
        return;
    }

    SetCreateProjectMode(false, true);
    OpenEditor(SelectionKey);
}

void UProjectSelectionWidget::HandleCreateProjectConfirmClicked()
{
    if (IsDesignTime()) return;
    UEditableTextBox* TextBox = NewProjectNameTextBox.Get();
    const FString ProjectName = IsValid(TextBox)
        ? TextBox->GetText().ToString().TrimStartAndEnd()
        : FString();
    CreateNewProject(ProjectName);
}

void UProjectSelectionWidget::HandleCreateProjectCancelClicked()
{
    if (IsDesignTime()) return;
    SetCreateProjectMode(false, true);
}

void UProjectSelectionWidget::BuildNativeLayout()
{
    SetIsFocusable(true);
    NativeProjectPanel = AddPanel(PanelSize, FVector2D::ZeroVector);
    UOverlay* ScreenStack = WidgetTree->ConstructWidget<UOverlay>();
    NativeProjectPanel->SetContent(ScreenStack);

    auto MakeNavigationHeader = [this](UVerticalBox* ParentBox, const FText& HeaderTitle,
        TObjectPtr<UButton>& OutBack, TObjectPtr<UButton>& OutRightButton, const FText& RightText)
    {
        UOverlay* Header = WidgetTree->ConstructWidget<UOverlay>();
        ParentBox->AddChildToVerticalBox(Header);
        UTextBlock* Title = AddText(Header, HeaderTitle, true);
        Title->SetAutoWrapText(false);
        Title->SetJustification(ETextJustify::Center);
        UOverlaySlot* TitleSlot = Cast<UOverlaySlot>(Title->Slot);
        TitleSlot->SetHorizontalAlignment(HAlign_Center);
        TitleSlot->SetVerticalAlignment(VAlign_Top);

        auto AddNavigation = [this, Header](const FText& Text, bool bRight)
        {
            UButton* Button = AddButton(Header, Text);
            if (UWidget* ParentWidget = Button ? Button->GetParent() : nullptr)
                if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(ParentWidget->Slot))
                {
                    OverlaySlot->SetPadding(bRight ? FMargin(0.0f, 0.0f, 32.0f, 0.0f) : FMargin(32.0f, 0.0f, 0.0f, 0.0f));
                    OverlaySlot->SetHorizontalAlignment(bRight ? HAlign_Right : HAlign_Left);
                    OverlaySlot->SetVerticalAlignment(VAlign_Center);
                }
            if (UWidget* ContentWidget = Button ? Button->GetContent() : nullptr)
                if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(ContentWidget->Slot))
                    ContentSlot->SetPadding(FMargin(12.0f));
            if (UTextBlock* Label = Cast<UTextBlock>(Button->GetContent()))
            {
                Label->SetMinDesiredWidth(128.0f);
                Label->SetAutoWrapText(false);
            }
            return Button;
        };

        OutBack = AddNavigation(NSLOCTEXT("V3DMenu", "Back", "Back"), false);
        if (!RightText.IsEmpty())
        {
            OutRightButton = AddNavigation(RightText, true);
        }
    };

    // --- Screen 1: Existing project browser ---
    NativeProjectListScreen = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("NativeProjectListScreen"));
    if (UOverlaySlot* ListScreenSlot = ScreenStack->AddChildToOverlay(NativeProjectListScreen))
    {
        ListScreenSlot->SetHorizontalAlignment(HAlign_Fill);
        ListScreenSlot->SetVerticalAlignment(VAlign_Fill);
    }
    MakeNavigationHeader(
        NativeProjectListScreen,
        NSLOCTEXT("V3DMenu", "EditorTitle", "Editor"),
        NativeProjectBack,
        NativeProjectRefresh,
        NSLOCTEXT("V3DMenu", "Refresh", "Refresh"));

    UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("NativeProjectList"));
    NativeProjectList = Scroll;
    UVerticalBoxSlot* ListSlot = NativeProjectListScreen->AddChildToVerticalBox(Scroll);
    ListSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    ListSlot->SetPadding(FMargin(32.0f, 16.0f, 32.0f, 24.0f));
#if WITH_EDITOR
    if (IsDesignTime())
    {
        AddButton(Scroll, NSLOCTEXT("V3DMenu", "CreateProject", "Create Project"));
        AddButton(Scroll, NSLOCTEXT("V3DMenu", "PreviewProjectOne", "Example Project [World]"));
        AddButton(Scroll, NSLOCTEXT("V3DMenu", "PreviewProjectTwo", "Character Library [Character]"));
    }
#endif

    // --- Screen 2: Dedicated New Project creation screen ---
    NativeCreateProjectScreen = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("NativeCreateProjectScreen"));
    NativeCreateProjectScreen->SetVisibility(ESlateVisibility::Collapsed);
    if (UOverlaySlot* CreateScreenSlot = ScreenStack->AddChildToOverlay(NativeCreateProjectScreen))
    {
        CreateScreenSlot->SetHorizontalAlignment(HAlign_Fill);
        CreateScreenSlot->SetVerticalAlignment(VAlign_Fill);
    }

    TObjectPtr<UButton> UnusedRight = nullptr;
    MakeNavigationHeader(
        NativeCreateProjectScreen,
        NSLOCTEXT("V3DMenu", "CreateProjectScreenTitle", "New Project"),
        NativeCreateBackButton,
        UnusedRight,
        FText::GetEmpty());

    UScrollBox* FormScroll = WidgetTree->ConstructWidget<UScrollBox>();
    UVerticalBoxSlot* FormScrollSlot = NativeCreateProjectScreen->AddChildToVerticalBox(FormScroll);
    FormScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    FormScrollSlot->SetPadding(FMargin(32.0f, 16.0f, 32.0f, 32.0f));

    USizeBox* CardContainer = WidgetTree->ConstructWidget<USizeBox>();
    CardContainer->SetMinDesiredWidth(920.0f);
    CardContainer->SetMaxDesiredWidth(1120.0f);
    FormScroll->AddChild(CardContainer);

    NativeCreateProjectCard = WidgetTree->ConstructWidget<UBorder>();
    NativeCreateProjectCard->SetPadding(FMargin(36.0f, 28.0f));
    CardContainer->SetContent(NativeCreateProjectCard);

    UVerticalBox* FormBox = WidgetTree->ConstructWidget<UVerticalBox>();
    NativeCreateProjectCard->SetContent(FormBox);

    // 1. Project Name
    UTextBlock* NameLabel = AddText(FormBox, NSLOCTEXT("V3DMenu", "ProjectNameLabel", "Project Name"));
    if (UVerticalBoxSlot* S = Cast<UVerticalBoxSlot>(NameLabel->Slot)) S->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
    USizeBox* NameSize = WidgetTree->ConstructWidget<USizeBox>();
    NameSize->SetMinDesiredHeight(ButtonHeight);
    FormBox->AddChildToVerticalBox(NameSize)->SetPadding(FMargin(0.f, 0.f, 0.f, 20.f));
    NativeProjectName = WidgetTree->ConstructWidget<UEditableTextBox>();
    NativeProjectName->SetHintText(NSLOCTEXT("V3DMenu", "ProjectNameHint", "Enter project name..."));
    NameSize->SetContent(NativeProjectName);

    // 2. Project Type
    UTextBlock* TypeLabel = AddText(FormBox, NSLOCTEXT("V3DMenu", "ProjectTypeLabel", "Project Type"));
    if (UVerticalBoxSlot* S = Cast<UVerticalBoxSlot>(TypeLabel->Slot)) S->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
    USizeBox* TypeSize = WidgetTree->ConstructWidget<USizeBox>();
    TypeSize->SetMinDesiredHeight(ButtonHeight);
    FormBox->AddChildToVerticalBox(TypeSize)->SetPadding(FMargin(0.f, 0.f, 0.f, 20.f));
    NativeProjectType = WidgetTree->ConstructWidget<UComboBoxString>();
    NativeProjectType->OnGenerateWidgetEvent.BindDynamic(this, &UProjectSelectionWidget::GenerateNativeProjectType);
    for (const EV3DSimulatorProjectType Type : { EV3DSimulatorProjectType::World,
        EV3DSimulatorProjectType::Prefab, EV3DSimulatorProjectType::Character, EV3DSimulatorProjectType::Dynamic })
    {
        NativeProjectType->AddOption(V3DSimulatorProjectTypes::ToString(Type));
    }
    NativeProjectType->SetSelectedOption(V3DSimulatorProjectTypes::ToString(NewProjectType));
    TypeSize->SetContent(NativeProjectType);

    // 3. Notes / Memo
    UTextBlock* NotesLabel = AddText(FormBox, NSLOCTEXT("V3DMenu", "ProjectNotesLabel", "Notes / Memo"));
    if (UVerticalBoxSlot* S = Cast<UVerticalBoxSlot>(NotesLabel->Slot)) S->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
    USizeBox* NotesSize = WidgetTree->ConstructWidget<USizeBox>();
    NotesSize->SetMinDesiredHeight(150.0f);
    FormBox->AddChildToVerticalBox(NotesSize)->SetPadding(FMargin(0.f, 0.f, 0.f, 20.f));
    NativeProjectNotes = WidgetTree->ConstructWidget<UMultiLineEditableTextBox>();
    NativeProjectNotes->SetHintText(NSLOCTEXT("V3DMenu", "ProjectNotesHint", "Optional description, goals, or memos..."));
    NotesSize->SetContent(NativeProjectNotes);

    // 4. World-specific options (shown when ProjectType == World)
    NativeWorldOptionsBox = WidgetTree->ConstructWidget<UVerticalBox>();
    FormBox->AddChildToVerticalBox(NativeWorldOptionsBox)->SetPadding(FMargin(0.f, 0.f, 0.f, 20.f));
    UTextBlock* OptionsLabel = AddText(NativeWorldOptionsBox, NSLOCTEXT("V3DMenu", "WorldOptionsLabel", "World Options"));
    if (UVerticalBoxSlot* S = Cast<UVerticalBoxSlot>(OptionsLabel->Slot)) S->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));

    UHorizontalBox* Options = WidgetTree->ConstructWidget<UHorizontalBox>();
    NativeWorldOptionsBox->AddChildToVerticalBox(Options);
    auto AddOption = [this, Options](const TCHAR* Label, bool bChecked)
    {
        UCheckBox* Check = WidgetTree->ConstructWidget<UCheckBox>();
        Check->SetIsChecked(bChecked);
        if (UHorizontalBoxSlot* OptionSlot = Options->AddChildToHorizontalBox(Check))
        {
            OptionSlot->SetPadding(FMargin(0.f, 0.f, 16.f, 0.f));
            OptionSlot->SetVerticalAlignment(VAlign_Center);
        }
        UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>();
        Text->SetText(FText::FromString(Label));
        Text->SetJustification(ETextJustify::Center);
        Check->SetContent(Text);
        return Check;
    };
    NewProjectAcoustics = AddOption(TEXT("World: geometry acoustics"), false);
    NewProjectMusic = AddOption(TEXT("Environment music"), true);
    NewProjectExternalAssets = AddOption(TEXT("Use external project assets"), false);

    // 5. Validation Error text
    NativeCreateErrorText = AddText(FormBox, FText::GetEmpty());
    NativeCreateErrorText->SetVisibility(ESlateVisibility::Collapsed);
    if (UVerticalBoxSlot* S = Cast<UVerticalBoxSlot>(NativeCreateErrorText->Slot)) S->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));

    // 6. Action Buttons (Cancel / Create & Open Editor)
    UHorizontalBox* ActionsRow = WidgetTree->ConstructWidget<UHorizontalBox>();
    FormBox->AddChildToVerticalBox(ActionsRow)->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));

    NativeCreateCancelButton = AddButton(ActionsRow, NSLOCTEXT("V3DMenu", "Cancel", "Cancel"));
    if (UWidget* ParentW = NativeCreateCancelButton ? NativeCreateCancelButton->GetParent() : nullptr)
    {
        if (UHorizontalBoxSlot* HBoxSlot = Cast<UHorizontalBoxSlot>(ParentW->Slot))
        {
            HBoxSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
            HBoxSlot->SetPadding(FMargin(0.f, 0.f, 16.f, 0.f));
        }
    }

    NativeCreateConfirmButton = AddButton(ActionsRow, NSLOCTEXT("V3DMenu", "CreateAndOpenEditor", "Create & Open Editor"));
    if (UWidget* ParentW = NativeCreateConfirmButton ? NativeCreateConfirmButton->GetParent() : nullptr)
    {
        if (UHorizontalBoxSlot* HBoxSlot = Cast<UHorizontalBoxSlot>(ParentW->Slot))
        {
            HBoxSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        }
    }

    UpdateWorldOptionsVisibility();
}

void UProjectSelectionWidget::ApplyNativeStyle()
{
    Super::ApplyNativeStyle();
    const bool bDarkMode = V3DHUDStyle::IsDarkMode(this);
    const V3DHUDStyle::FAuthoringPalette Palette = V3DHUDStyle::GetAuthoringPalette(bDarkMode);
    if (IsValid(NativeCreateProjectCard))
    {
        NativeCreateProjectCard->SetBrushColor(Palette.Panel);
    }
    if (NativeProjectName)
    {
        V3DHUDStyle::ApplyEditableTextBox(*NativeProjectName, Palette, FontObject, BodyFontSize, FontOutlineSize);
    }
    if (NativeProjectNotes)
    {
        FEditableTextBoxStyle Style = NativeProjectNotes->WidgetStyle;
        V3DHUDStyle::ApplyFont(Style.TextStyle.Font, FontObject, FMath::Clamp(FMath::RoundToInt(BodyFontSize * 0.80f), 16, 28), FontOutlineSize);
        Style.TextStyle.ColorAndOpacity = FSlateColor(Palette.Text);
        Style.BackgroundImageNormal = FSlateColorBrush(Palette.Input);
        Style.BackgroundImageHovered = FSlateColorBrush(Palette.Hover);
        Style.BackgroundImageFocused = FSlateColorBrush(Palette.Input);
        Style.BackgroundImageReadOnly = FSlateColorBrush(Palette.Panel);
        Style.BackgroundImageNormal.DrawAs = Style.BackgroundImageHovered.DrawAs =
            Style.BackgroundImageFocused.DrawAs = Style.BackgroundImageReadOnly.DrawAs = ESlateBrushDrawType::Box;
        Style.ForegroundColor = FSlateColor(Palette.Text);
        Style.FocusedForegroundColor = FSlateColor(Palette.Text);
        Style.ReadOnlyForegroundColor = FSlateColor(Palette.Muted);
        Style.Padding = FMargin(12.f, 10.f);
        NativeProjectNotes->WidgetStyle = Style;
        NativeProjectNotes->SetTextStyle(Style.TextStyle);
    }
    if (NativeCreateErrorText)
    {
        NativeCreateErrorText->SetColorAndOpacity(FSlateColor(
            bDarkMode ? FLinearColor(1.0f, 0.45f, 0.45f, 1.f) : FLinearColor(0.75f, 0.12f, 0.12f, 1.f)));
    }
    if (NativeProjectType)
    {
        V3DHUDStyle::ApplyComboBox(*NativeProjectType, Palette, bDarkMode);
        NativeProjectType->RefreshOptions();
    }
    const int32 ToggleFontSize = FMath::Clamp(FMath::RoundToInt(BodyFontSize * 0.72f), 16, 26);
    for (UCheckBox* Toggle : { NewProjectAcoustics.Get(), NewProjectMusic.Get(), NewProjectExternalAssets.Get() })
    {
        if (IsValid(Toggle))
        {
            V3DHUDStyle::ApplyCheckBox(*Toggle, Palette, FontObject, ToggleFontSize, FontOutlineSize);
        }
    }
}

void UProjectSelectionWidget::BindNativeWidgets()
{
    if (!IsUsingNativeLayout()) return;
    SetSelectionListPanel(NativeProjectList);
    SetBackButton(NativeProjectBack);
    SetRefreshButton(NativeProjectRefresh);
    SetNewProjectNameTextBox(NativeProjectName);
    if (!IsDesignTime())
    {
        if (NativeProjectType)
        {
            NativeProjectType->OnSelectionChanged.AddUniqueDynamic(this, &UProjectSelectionWidget::HandleNativeProjectTypeChanged);
        }
        if (NativeCreateBackButton)
        {
            NativeCreateBackButton->OnClicked.AddUniqueDynamic(this, &UProjectSelectionWidget::HandleCreateProjectCancelClicked);
        }
        if (NativeCreateCancelButton)
        {
            NativeCreateCancelButton->OnClicked.AddUniqueDynamic(this, &UProjectSelectionWidget::HandleCreateProjectCancelClicked);
        }
        if (NativeCreateConfirmButton)
        {
            NativeCreateConfirmButton->OnClicked.AddUniqueDynamic(this, &UProjectSelectionWidget::HandleCreateProjectConfirmClicked);
        }
    }
}

void UProjectSelectionWidget::HandleNativeProjectTypeChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
    EV3DSimulatorProjectType Type;
    if (!IsDesignTime() && V3DSimulatorProjectTypes::TryParse(SelectedItem, Type))
    {
        NewProjectType = Type;
        UpdateWorldOptionsVisibility();
    }
}

UWidget* UProjectSelectionWidget::GenerateNativeProjectType(FString Item)
{
    UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>();
    Label->SetText(FText::FromString(Item));
    FSlateFontInfo Font = Label->GetFont();
    V3DHUDStyle::ApplyFont(Font, FontObject, BodyFontSize, FontOutlineSize);
    Label->SetFont(Font);
    Label->SetColorAndOpacity(FSlateColor(TextColor));
    return Label;
}

void UProjectSelectionWidget::SetNewProjectType(EV3DSimulatorProjectType ProjectType)
{
    NewProjectType = ProjectType;
    if (NativeProjectType)
    {
        const FString Option = V3DSimulatorProjectTypes::ToString(ProjectType);
        if (NativeProjectType->GetSelectedOption() != Option) NativeProjectType->SetSelectedOption(Option);
    }
    UpdateWorldOptionsVisibility();
}

void UProjectSelectionWidget::OpenEditor(const FString& ProjectName)
{
    if (auto* Manager = UGameManagerSubSystem::GetSubSystem(this); Manager && Manager->IsProjectBuildInProgress()) return;
    EditorWidget = CreateWidget<UProjectEditorWidget>(GetOwningPlayer());
    if (!EditorWidget) return;
    EditorWidget->OnClosed.BindUObject(this, &UProjectSelectionWidget::HandleEditorClosed);
    EditorWidget->AddToPlayerScreen(50);
    SetVisibility(ESlateVisibility::Collapsed);
    EditorWidget->OpenProject(ProjectName);
}

void UProjectSelectionWidget::HandleEditorClosed()
{
    EditorWidget = nullptr;
    SetVisibility(ESlateVisibility::Visible);
    RefreshProjects();
}
