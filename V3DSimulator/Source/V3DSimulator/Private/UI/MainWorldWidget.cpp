// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file MainWorldWidget.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "UI/MainWorldWidget.h"
#include "UI/WidgetStyleSafety.h"
#include "UI/HUDStyle.h"

#include "Components/Button.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/Image.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/ScaleBox.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Texture2D.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "GameMode/MainGameMode.h"
#include "UI/ProjectSelectionWidget.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/V3DSimulatorGameInstance.h"
#include "System/GameManagerSubSystem.h"
#include "System/MacroLibrary.h"
#include "System/SimulatorPaths.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Kismet/KismetSystemLibrary.h"
#include "InputCoreTypes.h"

namespace
{
    FString NormalizeMainWorldText(FString Value)
    {
        Value.TrimStartAndEndInline();
        return Value;
    }
}

UMainWorldWidget::UMainWorldWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    PanelSize = FVector2D(1920.0, 1080.0);
    PanelPadding = 0.0f;
    ButtonHeight = 96.0f;
    BodyFontSize = 48;
    TitleFontSize = 100;
    SetIsFocusable(true);
}

void UMainWorldWidget::NativeConstruct()
{
    Super::NativeConstruct();
    if (IsDesignTime()) return;
    ClearFlags(RF_Transactional);
    BindDefaultButtons();
}

void UMainWorldWidget::NativeDestruct()
{
    UnbindDefaultButtons();
    if (NativeQuit) NativeQuit->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteQuitGame);
    if (NativeServerAddress) NativeServerAddress->OnTextChanged.RemoveDynamic(this, &UMainWorldWidget::HandleNativeServerAddressChanged);
    if (NativeWorldChoice) NativeWorldChoice->OnSelectionChanged.RemoveDynamic(this, &UMainWorldWidget::HandleNativeWorldChoiceChanged);
    if (IsValid(ProjectSelectionWidget))
    {
        ProjectSelectionWidget->SetOwnerMainWidget(nullptr);
        ProjectSelectionWidget = nullptr;
    }
    StartButton.Reset();
    WorldSelectionButton.Reset();
    BackButton.Reset();
    RefreshButton.Reset();
    MultiplayerButton.Reset();
    SettingsButton.Reset();
    EditorButton.Reset();
    HostButton.Reset();
    ClientButton.Reset();
    JoinButton.Reset();
    MainGameMode.Reset();
    Super::NativeDestruct();
}

FReply UMainWorldWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
    if (!InKeyEvent.IsRepeat()
        && (InKeyEvent.GetKey() == EKeys::Escape || InKeyEvent.GetKey() == EKeys::Gamepad_FaceButton_Right))
    {
        if (UButton* Button = BackButton.Get();
            IsValid(Button) && Button->IsVisible() && Button->GetIsEnabled())
        {
            ExecuteReturnToMainMenuFromWorldSelection();
            return FReply::Handled();
        }
    }
    return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UMainWorldWidget::SetMainGameMode(AMainGameMode* InMainGameMode)
{
    MainGameMode = InMainGameMode;
}

AMainGameMode* UMainWorldWidget::ResolveMainGameMode() const
{
    UWorld* World = GetWorld();
    if (!World) return nullptr;
    if (AMainGameMode* Assigned = MainGameMode.Get())
    {
        if (Assigned->GetWorld() == World) return Assigned;
    }
    // A Blueprint-created/reconstructed widget may not have received SetMainGameMode yet.
    return Cast<AMainGameMode>(World->GetAuthGameMode());
}

void UMainWorldWidget::ExecuteStartGame()
{
    if (AMainGameMode* Owner = ResolveMainGameMode())
    {
        Owner->StartGame();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("MainWorldWidget cannot start the game because MainGameMode is not assigned."));
}

void UMainWorldWidget::ExecuteReturnToMainMenuFromWorldSelection()
{
    if (AMainGameMode* Owner = ResolveMainGameMode())
    {
        Owner->ReturnToMainMenuFromWorldSelection();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("MainWorldWidget cannot return to the main menu because MainGameMode is not assigned."));
}

void UMainWorldWidget::ExecuteShowStartMenu()
{
    if (AMainGameMode* Owner = ResolveMainGameMode())
    {
        Owner->ShowStartMenu();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("MainWorldWidget cannot show the start menu because MainGameMode is not assigned."));
}

void UMainWorldWidget::ExecuteShowWorldSelectionMenu()
{
    if (AMainGameMode* Owner = ResolveMainGameMode())
    {
        Owner->ShowWorldSelectionMenu();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("MainWorldWidget cannot show the world-selection menu because MainGameMode is not assigned."));
}

void UMainWorldWidget::ExecuteShowMultiplayerMenu()
{
    if (AMainGameMode* Owner = ResolveMainGameMode())
    {
        Owner->ShowMultiplayerMenu();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("MainWorldWidget cannot show the multiplayer menu because MainGameMode is not assigned."));
}

void UMainWorldWidget::ExecuteShowSettingsMenu()
{
    if (AMainGameMode* Owner = ResolveMainGameMode())
    {
        Owner->ShowSettingsMenu();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("MainWorldWidget cannot show settings because MainGameMode is not assigned."));
}

bool UMainWorldWidget::EnsureProjectSelectionWidget()
{
    APlayerController* PlayerController = GetOwningPlayer();
    if (!IsValid(PlayerController) || !PlayerController->IsLocalController()
        || !PlayerController->GetLocalPlayer())
        return false;

    if (!IsValid(ProjectSelectionWidget))
    {
        UV3DSimulatorAssetRegistry* Registry =
            UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this);
        if (IsValid(Registry)) Registry->EnsureMenuDefaults();
        UClass* WidgetClass = IsValid(Registry) ? Registry->ProjectSelectionWidgetClass.LoadSynchronous() : nullptr;
        if (!IsValid(WidgetClass) || !WidgetClass->IsChildOf(UProjectSelectionWidget::StaticClass())
            || WidgetClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
        {
            UE_LOG(LogTemp, Warning, TEXT("Project browser class unavailable; using the native project browser."));
            WidgetClass = UProjectSelectionWidget::StaticClass();
        }
        SetProjectSelectionWidget(CreateWidget<UProjectSelectionWidget>(PlayerController, WidgetClass));
    }
    if (!IsValid(ProjectSelectionWidget)) return false;
    if (!ProjectSelectionWidget->IsInViewport() && !ProjectSelectionWidget->AddToPlayerScreen(30))
        return false;
    ProjectSelectionWidget->SetOwnerMainWidget(this);
    return true;
}

void UMainWorldWidget::ExecuteShowProjectSelectionWidget()
{
    if (!EnsureProjectSelectionWidget())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Editor button requires ProjectSelectionWidgetClass in the central AssetRegistry or an explicit SetProjectSelectionWidget() override."));
        return;
    }

    VisibilityBeforeProjectSelection = GetVisibility();
    SetVisibility(ESlateVisibility::Collapsed);
    ProjectSelectionWidget->SetOwnerMainWidget(this);
    ProjectSelectionWidget->SetVisibility(ESlateVisibility::Visible);
    ProjectSelectionWidget->RefreshProjects();
    ApplyProjectSelectionInputMode(ProjectSelectionWidget.Get());
}

void UMainWorldWidget::CloseProjectSelectionWidget()
{
    if (IsValid(ProjectSelectionWidget))
    {
        ProjectSelectionWidget->SetVisibility(ESlateVisibility::Collapsed);
    }

    if (IsInViewport())
    {
        SetVisibility(VisibilityBeforeProjectSelection);
        ApplyProjectSelectionInputMode(this);
    }
}

void UMainWorldWidget::ToggleProjectSelectionWidget()
{
    if (IsValid(ProjectSelectionWidget)
        && ProjectSelectionWidget->GetVisibility() != ESlateVisibility::Collapsed
        && ProjectSelectionWidget->GetVisibility() != ESlateVisibility::Hidden)
    {
        CloseProjectSelectionWidget();
    }
    else
    {
        ExecuteShowProjectSelectionWidget();
    }
}

void UMainWorldWidget::SetProjectSelectionWidget(UProjectSelectionWidget* InWidget)
{
    if (ProjectSelectionWidget == InWidget)
    {
        return;
    }

    if (IsValid(ProjectSelectionWidget))
    {
        ProjectSelectionWidget->SetOwnerMainWidget(nullptr);
    }

    ProjectSelectionWidget = InWidget;
    if (IsValid(ProjectSelectionWidget))
    {
        ProjectSelectionWidget->SetOwnerMainWidget(this);
        ProjectSelectionWidget->SetVisibility(ESlateVisibility::Collapsed);
    }
}

void UMainWorldWidget::HandleProjectSelectionWidgetRemoved(UProjectSelectionWidget* RemovedWidget)
{
    if (ProjectSelectionWidget.Get() != RemovedWidget)
    {
        return;
    }

    ProjectSelectionWidget = nullptr;
    if (IsInViewport())
    {
        SetVisibility(VisibilityBeforeProjectSelection);
        ApplyProjectSelectionInputMode(this);
    }
}

void UMainWorldWidget::ApplyProjectSelectionInputMode(UUserWidget* FocusWidget) const
{
    APlayerController* PlayerController = GetOwningPlayer();
    if (!IsValid(PlayerController))
    {
        return;
    }

    FInputModeUIOnly InputMode;
    // UIOnly logs an engine error if the requested Slate widget cannot accept keyboard focus.
    // Mouse-only menu/loading roots do not need explicit focus.
    if (IsValid(FocusWidget) && FocusWidget->IsFocusable())
    {
        InputMode.SetWidgetToFocus(FocusWidget->TakeWidget());
    }
    InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
    PlayerController->SetInputMode(InputMode);
    PlayerController->bShowMouseCursor = true;
}

void UMainWorldWidget::ExecuteRefreshWorldSelectionData()
{
    if (AMainGameMode* Owner = ResolveMainGameMode())
    {
        Owner->RefreshWorldFolderNameMap();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("MainWorldWidget cannot refresh world-selection data because MainGameMode is not assigned."));
}

void UMainWorldWidget::BindDefaultButtons()
{
    if (UButton* Button = StartButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteStartGame);
        Button->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteStartGame);
    }

    if (UButton* Button = WorldSelectionButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowWorldSelectionMenu);
        if (Button != StartButton.Get())
        {
            Button->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteShowWorldSelectionMenu);
        }
    }

    if (UButton* Button = BackButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteReturnToMainMenuFromWorldSelection);
        Button->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteReturnToMainMenuFromWorldSelection);
    }

    if (UButton* Button = RefreshButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteRefreshWorldSelectionData);
        Button->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteRefreshWorldSelectionData);
    }

    if (UButton* Button = MultiplayerButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowMultiplayerMenu);
        Button->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteShowMultiplayerMenu);
    }

    if (UButton* Button = SettingsButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowSettingsMenu);
        Button->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteShowSettingsMenu);
    }

    if (UButton* Button = EditorButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowProjectSelectionWidget);
        Button->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteShowProjectSelectionWidget);
    }

    if (UButton* Button = HostButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteHostSelectedWorld);
        Button->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteHostSelectedWorld);
    }

    if (UButton* Button = ClientButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteOpenClientConnectionWorld);
        Button->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteOpenClientConnectionWorld);
    }

    if (UButton* Button = JoinButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteJoinSelectedWorld);
        Button->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteJoinSelectedWorld);
    }
}

void UMainWorldWidget::UnbindDefaultButtons()
{
    if (UButton* Button = StartButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteStartGame);
    }

    if (UButton* Button = WorldSelectionButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowWorldSelectionMenu);
    }

    if (UButton* Button = BackButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteReturnToMainMenuFromWorldSelection);
    }

    if (UButton* Button = RefreshButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteRefreshWorldSelectionData);
    }

    if (UButton* Button = MultiplayerButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowMultiplayerMenu);
    }

    if (UButton* Button = SettingsButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowSettingsMenu);
    }

    if (UButton* Button = EditorButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowProjectSelectionWidget);
    }

    if (UButton* Button = HostButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteHostSelectedWorld);
    }

    if (UButton* Button = ClientButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteOpenClientConnectionWorld);
    }

    if (UButton* Button = JoinButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteJoinSelectedWorld);
    }
}

void UMainWorldWidget::SetStartButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = StartButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteStartGame);
    }

    StartButton = InButton;

    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowWorldSelectionMenu);
        InButton->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteStartGame);
        InButton->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteStartGame);
    }
}

void UMainWorldWidget::SetWorldSelectionButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = WorldSelectionButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowWorldSelectionMenu);
    }

    WorldSelectionButton = InButton;

    if (IsValid(InButton) && InButton != StartButton.Get())
    {
        InButton->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowWorldSelectionMenu);
        InButton->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteShowWorldSelectionMenu);
    }
}

void UMainWorldWidget::SetBackButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = BackButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteReturnToMainMenuFromWorldSelection);
    }

    BackButton = InButton;

    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteReturnToMainMenuFromWorldSelection);
        InButton->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteReturnToMainMenuFromWorldSelection);
    }
}

void UMainWorldWidget::SetRefreshButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = RefreshButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteRefreshWorldSelectionData);
    }

    RefreshButton = InButton;

    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteRefreshWorldSelectionData);
        InButton->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteRefreshWorldSelectionData);
    }
}

void UMainWorldWidget::SetMultiplayerButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = MultiplayerButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowMultiplayerMenu);
    }

    MultiplayerButton = InButton;

    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowMultiplayerMenu);
        InButton->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteShowMultiplayerMenu);
    }
}

void UMainWorldWidget::SetSettingsButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = SettingsButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowSettingsMenu);
    }
    SettingsButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowSettingsMenu);
        InButton->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteShowSettingsMenu);
    }
}

void UMainWorldWidget::SetEditorButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = EditorButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowProjectSelectionWidget);
    }
    EditorButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteShowProjectSelectionWidget);
        InButton->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteShowProjectSelectionWidget);
    }
}

void UMainWorldWidget::SetHostButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = HostButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteHostSelectedWorld);
    }

    HostButton = InButton;

    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteHostSelectedWorld);
        InButton->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteHostSelectedWorld);
    }
}

void UMainWorldWidget::SetClientButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = ClientButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteOpenClientConnectionWorld);
    }

    ClientButton = InButton;

    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteOpenClientConnectionWorld);
        InButton->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteOpenClientConnectionWorld);
    }
}

void UMainWorldWidget::SetJoinButton(UButton* InButton)
{
    if (!AcceptWidgetAssignment(InButton)) return;
    if (UButton* Button = JoinButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteJoinSelectedWorld);
    }

    JoinButton = InButton;

    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UMainWorldWidget::ExecuteJoinSelectedWorld);
        InButton->OnClicked.AddDynamic(this, &UMainWorldWidget::ExecuteJoinSelectedWorld);
    }
}

void UMainWorldWidget::SetSelectedWorldFolderName(const FString& InWorldFolderName)
{
    FString ResolvedFolderName;
    if (ResolveWorldFolderName(InWorldFolderName, ResolvedFolderName))
    {
        SelectedWorldFolderName = ResolvedFolderName;
        return;
    }

    SelectedWorldFolderName = NormalizeMainWorldText(InWorldFolderName);
}

bool UMainWorldWidget::ResolveWorldFolderName(const FString& FolderOrDisplayName, FString& OutWorldFolderName) const
{
    const FString ExactKey = FolderOrDisplayName.TrimStartAndEnd();
    FString SafeKey;
    const TMap<FString, FString> FolderMap = GetFolderNameMap();
    if (!UGameManagerSubSystem::TryNormalizeWorldFolderName(ExactKey, SafeKey, false)
        || SafeKey != ExactKey || !FolderMap.Contains(ExactKey))
    {
        OutWorldFolderName.Reset();
        return false;
    }
    OutWorldFolderName = ExactKey;
    return true;
}

FString UMainWorldWidget::GetSelectedWorldRootPath() const
{
    FString Folder;
    if (!UGameManagerSubSystem::TryNormalizeWorldFolderName(SelectedWorldFolderName, Folder, true))
        return FString();
    return FPaths::ConvertRelativePathToFull(FPaths::Combine(V3DSimulatorPaths::WorldsRoot(), Folder));
}

bool UMainWorldWidget::OpenSelectedWorld()
{
    return OpenWorldByFolderName(SelectedWorldFolderName);
}

bool UMainWorldWidget::OpenWorldByFolderName(const FString& WorldFolderName)
{
    FString ResolvedFolderName;
    if (!ResolveWorldFolderName(WorldFolderName, ResolvedFolderName))
    {
        UE_LOG(LogTemp, Warning, TEXT("MainWorldWidget cannot open unknown world: %s"), *WorldFolderName);
        return false;
    }

    SelectedWorldFolderName = ResolvedFolderName;
    if (AMainGameMode* Owner = ResolveMainGameMode())
    {
        Owner->OpenSinglePlayerWorldByFolderName(ResolvedFolderName);
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("MainWorldWidget cannot open world %s because MainGameMode is not assigned."), *ResolvedFolderName);
    return false;
}

void UMainWorldWidget::ExecuteHostSelectedWorld()
{
    HostSelectedWorld();
}

bool UMainWorldWidget::HostSelectedWorld()
{
    return HostWorldByFolderName(SelectedWorldFolderName);
}

bool UMainWorldWidget::HostWorldByFolderName(const FString& WorldFolderName)
{
    FString ResolvedFolderName;
    if (!ResolveWorldFolderName(WorldFolderName, ResolvedFolderName))
    {
        UE_LOG(LogTemp, Warning, TEXT("MainWorldWidget cannot host unknown world: %s"), *WorldFolderName);
        return false;
    }

    SelectedWorldFolderName = ResolvedFolderName;
    if (AMainGameMode* Owner = ResolveMainGameMode())
    {
        Owner->HostMultiplayerWorldByFolderName(ResolvedFolderName);
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("MainWorldWidget cannot host world because MainGameMode is not assigned."));
    return false;
}

void UMainWorldWidget::ExecuteJoinSelectedWorld()
{
    JoinSelectedWorld();
}

bool UMainWorldWidget::JoinSelectedWorld()
{
    if (AMainGameMode* Owner = ResolveMainGameMode())
    {
        Owner->JoinMultiplayerServer(ServerAddress, SelectedWorldFolderName);
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("MainWorldWidget cannot join multiplayer because MainGameMode is not assigned."));
    return false;
}

bool UMainWorldWidget::JoinServer(const FString& InServerAddress)
{
    SetServerAddress(InServerAddress);
    if (AMainGameMode* Owner = ResolveMainGameMode())
    {
        Owner->JoinMultiplayerServer(ServerAddress, SelectedWorldFolderName);
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("MainWorldWidget cannot join server because MainGameMode is not assigned."));
    return false;
}

void UMainWorldWidget::ExecuteOpenClientConnectionWorld()
{
    OpenClientConnectionWorld();
}

bool UMainWorldWidget::OpenClientConnectionWorld()
{
    if (AMainGameMode* Owner = ResolveMainGameMode())
    {
        Owner->OpenClientConnectionWorld(ServerAddress);
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("MainWorldWidget cannot open ClientWorld because MainGameMode is not assigned."));
    return false;
}

void UMainWorldWidget::SetServerAddress(const FString& InServerAddress)
{
    ServerAddress = NormalizeMainWorldText(InServerAddress);
    if (ServerAddress.IsEmpty())
    {
        ServerAddress = TEXT("127.0.0.1:7777");
    }

    if (AMainGameMode* Owner = ResolveMainGameMode())
    {
        Owner->SetPendingServerAddress(ServerAddress);
    }
}

TMap<FString, FString> UMainWorldWidget::GetFolderNameMap() const
{
    if (const AMainGameMode* Owner = ResolveMainGameMode())
    {
        return Owner->GetFolderNameMap();
    }

    return CachedWorldSelectionData;
}

void UMainWorldWidget::SetWorldSelectionData(const TMap<FString, FString>& Values)
{
    CachedWorldSelectionData = Values;
    RefreshNativeWorldChoices();
}



UMultiplayerMenuWidget::UMultiplayerMenuWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    ScreenMode = EMainWorldScreen::Multiplayer;
    TitleFontSize = 44;
    BodyFontSize = 24;
    ButtonHeight = 52.0f;
    PanelPadding = 28.0f;
}

void UMainWorldWidget::BuildNativeLayout()
{
    NativeStart = nullptr;
    NativeEditor = nullptr;
    NativeMultiplayer = nullptr;
    NativeSettings = nullptr;
    NativeQuit = nullptr;
    NativeBack = nullptr;
    NativeHost = nullptr;
    NativeClient = nullptr;
    NativeJoin = nullptr;
    NativeServerAddress = nullptr;
    NativeWorldChoice = nullptr;
    NativeMenuTitle = nullptr;
    NativeMultiplayerCard = nullptr;
    NativeTitleBounds = nullptr;
    NativeTitlePrefix = nullptr;
    NativeTitleSuffix = nullptr;
    SetIsFocusable(true);

    NativeMenuPanel = AddPanel(PanelSize, FVector2D::ZeroVector);
    UOverlay* Layout = WidgetTree->ConstructWidget<UOverlay>();
    NativeMenuPanel->SetContent(Layout);
    const bool bMultiplayer = ScreenMode == EMainWorldScreen::Multiplayer;
    if (bMultiplayer)
    {
        USizeBox* FormSize = WidgetTree->ConstructWidget<USizeBox>();
        FormSize->SetMinDesiredWidth(540.0f);
        FormSize->SetMaxDesiredWidth(600.0f);
        UOverlaySlot* FormSlot = Layout->AddChildToOverlay(FormSize);
        FormSlot->SetPadding(FMargin(24.0f));
        FormSlot->SetHorizontalAlignment(HAlign_Center);
        FormSlot->SetVerticalAlignment(VAlign_Center);

        UBorder* Card = WidgetTree->ConstructWidget<UBorder>();
        NativeMultiplayerCard = Card;
        Card->SetBrushColor(PanelColor);
        Card->SetPadding(FMargin(28.0f, 24.0f));
        FormSize->SetContent(Card);

        UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
        Card->SetContent(Box);

        NativeMenuTitle = AddText(Box, NSLOCTEXT("V3DMenu", "MultiplayerTitle", "Multiplayer"), true);
        NativeMenuTitle->SetJustification(ETextJustify::Center);
        if (UVerticalBoxSlot* TitleBoxSlot = Cast<UVerticalBoxSlot>(NativeMenuTitle->Slot))
        {
            TitleBoxSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 16.0f));
        }

        AddText(Box, NSLOCTEXT("V3DMenu", "HostWorldLabel", "World to host"));
        USizeBox* WorldChoiceSize = WidgetTree->ConstructWidget<USizeBox>();
        WorldChoiceSize->SetMinDesiredHeight(46.0f);
        Box->AddChildToVerticalBox(WorldChoiceSize)->SetPadding(FMargin(0.0f, 4.0f, 0.0f, 12.0f));
        NativeWorldChoice = WidgetTree->ConstructWidget<UComboBoxString>();
        NativeWorldChoice->OnGenerateWidgetEvent.BindDynamic(this, &UMainWorldWidget::GenerateNativeWorldChoice);
        WorldChoiceSize->SetContent(NativeWorldChoice);
#if WITH_EDITOR
        if (IsDesignTime())
        {
            NativeWorldChoice->AddOption(TEXT("Example World"));
            NativeWorldChoice->SetSelectedIndex(0);
        }
#endif
        AddText(Box, NSLOCTEXT("V3DMenu", "ServerAddressLabel", "Server address"));
        USizeBox* ServerAddressSize = WidgetTree->ConstructWidget<USizeBox>();
        ServerAddressSize->SetMinDesiredHeight(46.0f);
        Box->AddChildToVerticalBox(ServerAddressSize)->SetPadding(FMargin(0.0f, 4.0f, 0.0f, 16.0f));
        NativeServerAddress = WidgetTree->ConstructWidget<UEditableTextBox>();
        NativeServerAddress->SetHintText(NSLOCTEXT("V3DMenu", "ServerAddressHint", "127.0.0.1:7777"));
        NativeServerAddress->SetText(FText::FromString(ServerAddress));
        ServerAddressSize->SetContent(NativeServerAddress);

        NativeHost = AddButton(Box, NSLOCTEXT("V3DMenu", "HostWorld", "Host selected world"));
        NativeJoin = AddButton(Box, NSLOCTEXT("V3DMenu", "JoinServer", "Join server"));
        NativeClient = AddButton(Box, NSLOCTEXT("V3DMenu", "ClientWorld", "Open client connection world"));
        NativeBack = AddButton(Box, NSLOCTEXT("V3DMenu", "Back", "Back"));
    }
    else
    {
        // Render the title using Pretendard typography: bold "V3D" seamlessly joined with regular "Simulator"
        // (no space), supporting both light and dark themes automatically.
        NativeTitleBounds = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("MainWorldTitleBounds"));
        UScaleBox* TitleScale = WidgetTree->ConstructWidget<UScaleBox>();
        TitleScale->SetStretch(EStretch::ScaleToFit);
        TitleScale->SetStretchDirection(EStretchDirection::DownOnly);
        NativeTitleBounds->SetContent(TitleScale);

        UHorizontalBox* TitleRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("MainWorldTitleRow"));
        TitleRow->SetVisibility(ESlateVisibility::HitTestInvisible);
        TitleScale->SetContent(TitleRow);

        NativeTitlePrefix = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("MainWorldTitlePrefix"));
        NativeTitlePrefix->SetText(FText::FromString(TEXT("V3D")));
        NativeTitlePrefix->SetAutoWrapText(false);
        NativeTitlePrefix->SetVisibility(ESlateVisibility::HitTestInvisible);
        if (UHorizontalBoxSlot* PrefixSlot = TitleRow->AddChildToHorizontalBox(NativeTitlePrefix))
        {
            PrefixSlot->SetPadding(FMargin(0.0f));
            PrefixSlot->SetVerticalAlignment(VAlign_Bottom);
        }

        NativeTitleSuffix = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("MainWorldTitleSuffix"));
        NativeTitleSuffix->SetText(FText::FromString(TEXT("Simulator")));
        NativeTitleSuffix->SetAutoWrapText(false);
        NativeTitleSuffix->SetVisibility(ESlateVisibility::HitTestInvisible);
        if (UHorizontalBoxSlot* SuffixSlot = TitleRow->AddChildToHorizontalBox(NativeTitleSuffix))
        {
            SuffixSlot->SetPadding(FMargin(0.0f));
            SuffixSlot->SetVerticalAlignment(VAlign_Bottom);
        }

        Layout->AddChildToOverlay(NativeTitleBounds);
        UOverlaySlot* TitleSlot = Cast<UOverlaySlot>(NativeTitleBounds->Slot);
        TitleSlot->SetPadding(FMargin(0.0f, 128.0f, 32.0f, 0.0f));
        TitleSlot->SetHorizontalAlignment(HAlign_Right);
        TitleSlot->SetVerticalAlignment(VAlign_Top);

        auto MakeGroup = [this, Layout](EHorizontalAlignment Horizontal, EVerticalAlignment Vertical)
        {
            UUniformGridPanel* Grid = WidgetTree->ConstructWidget<UUniformGridPanel>();
            Grid->SetSlotPadding(FMargin(0.0f, 16.0f));
            Grid->SetMinDesiredSlotHeight(128.0f);
            UOverlaySlot* Slot = Layout->AddChildToOverlay(Grid);
            Slot->SetHorizontalAlignment(Horizontal);
            Slot->SetVerticalAlignment(Vertical);
            return Grid;
        };
        auto AddMenuButton = [this](UUniformGridPanel* Grid, const FText& Text, int32 Row,
            float Width, ETextJustify::Type Justification)
        {
            UButton* Button = AddButton(Grid, Text);
            USizeBox* Size = Cast<USizeBox>(Button->GetParent());
            Size->SetMinDesiredWidth(Width);
            UUniformGridSlot* Slot = Cast<UUniformGridSlot>(Size->Slot);
            Slot->SetRow(Row);
            Slot->SetHorizontalAlignment(HAlign_Fill);
            Slot->SetVerticalAlignment(VAlign_Fill);
            if (UTextBlock* Label = Cast<UTextBlock>(Button->GetContent()))
            {
                Label->SetJustification(Justification);
                Label->SetAutoWrapText(false);
            }
            return Button;
        };

        UUniformGridPanel* Gameplay = MakeGroup(HAlign_Left, VAlign_Center);
        NativeStart = AddMenuButton(Gameplay, NSLOCTEXT("V3DMenu", "Singleplay", "Singleplay"), 0, 768.0f, ETextJustify::Right);
        NativeMultiplayer = AddMenuButton(Gameplay, NSLOCTEXT("V3DMenu", "Multiplay", "Multiplay"), 1, 768.0f, ETextJustify::Right);
        UUniformGridPanel* Control = MakeGroup(HAlign_Right, VAlign_Bottom);
        NativeEditor = AddMenuButton(Control, NSLOCTEXT("V3DMenu", "Editor", "Editor"), 0, 512.0f, ETextJustify::Center);
        NativeSettings = AddMenuButton(Control, NSLOCTEXT("V3DMenu", "Settings", "Settings"), 1, 512.0f, ETextJustify::Center);
        NativeQuit = AddMenuButton(Control, NSLOCTEXT("V3DMenu", "Exit", "Exit"), 2, 512.0f, ETextJustify::Center);
    }
}

void UMainWorldWidget::ApplyNativeStyle()
{
    Super::ApplyNativeStyle();
    if (NativeTitleBounds)
    {
        const FVector2D Resolution = GetSafeDesignResolution();
        const double Width = FMath::IsFinite(TitleImageSize.X) ? TitleImageSize.X : 800.0;
        const double Height = FMath::IsFinite(TitleImageSize.Y) ? TitleImageSize.Y : 100.0;
        NativeTitleBounds->SetWidthOverride(FMath::Clamp(Width, 8.0, Resolution.X * .6));
        NativeTitleBounds->SetHeightOverride(FMath::Clamp(Height, 8.0, Resolution.Y * .25));
    }
    const bool bDarkMode = V3DHUDStyle::IsDarkMode(this);
    const V3DHUDStyle::FAuthoringPalette Palette = V3DHUDStyle::GetAuthoringPalette(bDarkMode);
    if (IsValid(NativeMultiplayerCard))
    {
        NativeMultiplayerCard->SetBrushColor(Palette.Panel);
    }
    if (NativeTitlePrefix && NativeTitleSuffix && ScreenMode == EMainWorldScreen::Start)
    {
        const FLinearColor TitleColor = Palette.Text;
        NativeTitlePrefix->SetColorAndOpacity(FSlateColor(TitleColor));
        NativeTitleSuffix->SetColorAndOpacity(FSlateColor(TitleColor));
        FSlateFontInfo PrefixFont = NativeTitlePrefix->GetFont();
        V3DHUDStyle::ApplyFontWithWeight(PrefixFont, FontObject, TitleFontSize, true, TitleColor);
        NativeTitlePrefix->SetFont(PrefixFont);

        FSlateFontInfo SuffixFont = NativeTitleSuffix->GetFont();
        V3DHUDStyle::ApplyFontWithWeight(SuffixFont, FontObject, TitleFontSize, false);
        NativeTitleSuffix->SetFont(SuffixFont);
    }
    if (NativeServerAddress)
    {
        V3DHUDStyle::ApplyEditableTextBox(*NativeServerAddress, Palette, FontObject, BodyFontSize, FontOutlineSize);
    }
    if (NativeWorldChoice)
    {
        V3DHUDStyle::ApplyComboBox(*NativeWorldChoice, Palette, bDarkMode);
        NativeWorldChoice->RefreshOptions();
    }
}

void UMainWorldWidget::BindNativeWidgets()
{
    if (!IsUsingNativeLayout()) return;
    SetStartButton(NativeStart);
    SetWorldSelectionButton(nullptr);
    SetEditorButton(NativeEditor);
    SetMultiplayerButton(NativeMultiplayer);
    SetSettingsButton(NativeSettings);
    SetBackButton(NativeBack);
    SetRefreshButton(nullptr);
    SetHostButton(NativeHost);
    SetClientButton(NativeClient);
    SetJoinButton(NativeJoin);
    if (!IsDesignTime())
    {
        if (NativeQuit) NativeQuit->OnClicked.AddUniqueDynamic(this, &UMainWorldWidget::ExecuteQuitGame);
        if (NativeServerAddress)
            NativeServerAddress->OnTextChanged.AddUniqueDynamic(this, &UMainWorldWidget::HandleNativeServerAddressChanged);
        if (NativeWorldChoice)
            NativeWorldChoice->OnSelectionChanged.AddUniqueDynamic(this, &UMainWorldWidget::HandleNativeWorldChoiceChanged);
        RefreshNativeWorldChoices();
    }
}

void UMainWorldWidget::HandleNativeServerAddressChanged(const FText& Text)
{
    if (!IsDesignTime()) SetServerAddress(Text.ToString());
}

void UMainWorldWidget::HandleNativeWorldChoiceChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
    if (!IsDesignTime() && !SelectedItem.IsEmpty()) SetSelectedWorldFolderName(SelectedItem);
}

void UMainWorldWidget::RefreshNativeWorldChoices()
{
    if (IsDesignTime() || !NativeWorldChoice) return;
    TArray<FString> Keys;
    CachedWorldSelectionData.GetKeys(Keys);
    Keys.Sort();
    NativeWorldChoice->ClearOptions();
    for (const FString& Key : Keys) NativeWorldChoice->AddOption(Key);
    if (!Keys.Contains(SelectedWorldFolderName))
        SelectedWorldFolderName = Keys.IsEmpty() ? FString() : Keys[0];
    if (!SelectedWorldFolderName.IsEmpty()) NativeWorldChoice->SetSelectedOption(SelectedWorldFolderName);
    if (NativeHost) NativeHost->SetIsEnabled(!Keys.IsEmpty());
}


UWidget* UMainWorldWidget::GenerateNativeWorldChoice(FString Item)
{
    UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>();
    Label->SetText(FText::FromString(Item));
    FSlateFontInfo Font = Label->GetFont();
    V3DHUDStyle::ApplyFont(Font, FontObject, BodyFontSize, FontOutlineSize);
    Label->SetFont(Font);
    Label->SetColorAndOpacity(FSlateColor(TextColor));
    return Label;
}


void UMainWorldWidget::ExecuteQuitGame()
{
    if (!IsDesignTime()) UKismetSystemLibrary::QuitGame(this, GetOwningPlayer(), EQuitPreference::Quit, false);
}
