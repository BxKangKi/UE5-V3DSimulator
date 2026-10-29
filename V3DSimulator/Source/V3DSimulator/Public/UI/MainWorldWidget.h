// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file MainWorldWidget.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Types/SlateEnums.h"
#include "UI/SelectionWidgetBase.h"
#include "MainWorldWidget.generated.h"

class AMainGameMode;
class UButton;
class UEditableTextBox;
class UComboBoxString;
class UProjectSelectionWidget;
class UImage;
class USizeBox;

UENUM(BlueprintType)
enum class EMainWorldScreen : uint8
{
    Start,
    Multiplayer
};

/**
 * Base class for MainWorld UI widgets.
 *
 * MainGameMode can construct this top-level widget directly from the central registry. Native layout
 * is visible in Blueprint children; custom layout mode can still assign controls through the setters.
 */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API UMainWorldWidget : public USelectionWidgetBase
{
    GENERATED_BODY()

public:
    UMainWorldWidget(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    /** Fixed reference-space bounds: the logo never inherits the full-screen Overlay size. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Title", meta=(DesignerRebuild))
    FVector2D TitleImageSize = FVector2D(1200.0, 140.0);

    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    /** Stores the owning MainGameMode. MainGameMode assigns this automatically for registry-created widgets. */
    UFUNCTION(BlueprintCallable, Category="Main World|Actor")
    void SetMainGameMode(AMainGameMode* InMainGameMode);

    /** Returns the owning MainGameMode registered for this widget. */
    UFUNCTION(BlueprintPure, Category="Main World|Actor")
    AMainGameMode* GetMainGameMode() const { return ResolveMainGameMode(); }

    /** Opens the world-selection screen through the owning MainGameMode. Bind this to the start button. */
    UFUNCTION(BlueprintCallable, Category="Main World|Actions")
    void ExecuteStartGame();

    /** Returns from the world-selection screen to the main start screen. Bind this to the Back button. */
    UFUNCTION(BlueprintCallable, Category="Main World|Actions")
    void ExecuteReturnToMainMenuFromWorldSelection();

    /** Opens the main start screen through the owning MainGameMode. */
    UFUNCTION(BlueprintCallable, Category="Main World|Actions")
    void ExecuteShowStartMenu();

    /** Opens the world-selection screen through the owning MainGameMode. */
    UFUNCTION(BlueprintCallable, Category="Main World|Actions")
    void ExecuteShowWorldSelectionMenu();

    /** Opens the multiplayer menu through the owning MainGameMode. */
    UFUNCTION(BlueprintCallable, Category="Main World|Actions")
    void ExecuteShowMultiplayerMenu();

    /** Opens the common Settings widget registered on MainGameMode. */
    UFUNCTION(BlueprintCallable, Category="Main World|Actions")
    void ExecuteShowSettingsMenu();

    UFUNCTION(BlueprintCallable, Category="Main World|Actions")
    void ExecuteQuitGame();

    /** Shows the project-selection widget created/registered from the central registry. */
    UFUNCTION(BlueprintCallable, Category="Main World|Projects")
    void ExecuteShowProjectSelectionWidget();

    /** Hides the registered project-selection widget and restores this start widget. */
    UFUNCTION(BlueprintCallable, Category="Main World|Projects")
    void CloseProjectSelectionWidget();

    /** Toggle helper for custom Blueprint flows. */
    UFUNCTION(BlueprintCallable, Category="Main World|Projects")
    void ToggleProjectSelectionWidget();

    /** Register or replace the project-selection widget instance. MainGameMode fills this automatically from the registry when missing. */
    UFUNCTION(BlueprintCallable, Category="Main World|Projects")
    void SetProjectSelectionWidget(UProjectSelectionWidget* InWidget);

    UFUNCTION(BlueprintPure, Category="Main World|Projects")
    UProjectSelectionWidget* GetProjectSelectionWidget() const { return ProjectSelectionWidget.Get(); }

    /** Creates and attaches the project browser on demand; never hides the menu on failure. */
    bool EnsureProjectSelectionWidget();

    /** Internal lifecycle callback used when a project-selection widget is removed externally. */
    void HandleProjectSelectionWidgetRemoved(UProjectSelectionWidget* RemovedWidget);

    /** Re-scans world folders and refreshes the world-selection widget. */
    UFUNCTION(BlueprintCallable, Category="Main World|Actions")
    void ExecuteRefreshWorldSelectionData();

    /** Rebinds delegates for buttons that were assigned through the explicit button setter functions. */
    UFUNCTION(BlueprintCallable, Category="Main World|Actions")
    void BindDefaultButtons();

    /** Removes C++ button delegates. NativeDestruct calls this automatically. */
    UFUNCTION(BlueprintCallable, Category="Main World|Actions")
    void UnbindDefaultButtons();

    UFUNCTION(BlueprintCallable, Category="Main World|Widgets")
    void SetStartButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Main World|Widgets")
    void SetWorldSelectionButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Main World|Widgets")
    void SetBackButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Main World|Widgets")
    void SetRefreshButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Main World|Widgets")
    void SetMultiplayerButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Main World|Widgets")
    void SetSettingsButton(UButton* InButton);

    /** Assign this from a derived WBP Construct event to enable the main Projects button. */
    UFUNCTION(BlueprintCallable, Category="Main World|Widgets")
    void SetProjectsButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Main World|Widgets")
    void SetHostButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Main World|Widgets")
    void SetClientButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Main World|Widgets")
    void SetJoinButton(UButton* InButton);

    /** Selects a world by folder key. */
    UFUNCTION(BlueprintCallable, Category="Main World|World Selection")
    void SetSelectedWorldFolderName(const FString& InWorldFolderName);

    /** Returns the currently selected world folder key. */
    UFUNCTION(BlueprintPure, Category="Main World|World Selection")
    FString GetSelectedWorldFolderName() const { return SelectedWorldFolderName; }

    /** Validates an exact folder key against the current index of .v3d files under Worlds. */
    UFUNCTION(BlueprintCallable, Category="Main World|World Selection")
    bool ResolveWorldFolderName(const FString& FolderOrDisplayName, FString& OutWorldFolderName) const;

    /** Opens the selected world folder. */
    UFUNCTION(BlueprintCallable, Category="Main World|World Selection")
    bool OpenSelectedWorld();

    /** Resolves the selected immutable world key under Worlds; does not create directories or initiate travel. */
    UFUNCTION(BlueprintPure, Category="World")
    FString GetSelectedWorldRootPath() const;

    /** Opens a world by exact folder key. */
    UFUNCTION(BlueprintCallable, Category="Main World|World Selection")
    bool OpenWorldByFolderName(const FString& WorldFolderName);

    UFUNCTION(BlueprintCallable, Category="Main World|Multiplayer")
    void ExecuteHostSelectedWorld();

    UFUNCTION(BlueprintCallable, Category="Main World|Multiplayer")
    bool HostSelectedWorld();

    UFUNCTION(BlueprintCallable, Category="Main World|Multiplayer")
    bool HostWorldByFolderName(const FString& WorldFolderName);

    UFUNCTION(BlueprintCallable, Category="Main World|Multiplayer")
    void ExecuteJoinSelectedWorld();

    UFUNCTION(BlueprintCallable, Category="Main World|Multiplayer")
    bool JoinSelectedWorld();

    UFUNCTION(BlueprintCallable, Category="Main World|Multiplayer")
    bool JoinServer(const FString& InServerAddress);

    UFUNCTION(BlueprintCallable, Category="Main World|Multiplayer")
    void ExecuteOpenClientConnectionWorld();

    UFUNCTION(BlueprintCallable, Category="Main World|Multiplayer")
    bool OpenClientConnectionWorld();

    UFUNCTION(BlueprintCallable, Category="Main World|Multiplayer")
    void SetServerAddress(const FString& InServerAddress);

    UFUNCTION(BlueprintPure, Category="Main World|Multiplayer")
    FString GetServerAddress() const { return ServerAddress; }

    /** Returns the last world-folder map delivered to this widget. */
    UFUNCTION(BlueprintPure, Category="Main World|Data")
    TMap<FString, FString> GetCachedWorldSelectionData() const { return CachedWorldSelectionData; }

    /** Returns the current world-folder map from MainGameMode when available, otherwise the cached copy. */
    UFUNCTION(BlueprintPure, Category="Main World|Data")
    TMap<FString, FString> GetFolderNameMap() const;

    /** Updates the cached world-selection data. Derived native widgets can override this directly. */
    UFUNCTION(BlueprintCallable, Category="Main World|Data")
    virtual void SetWorldSelectionData(const TMap<FString, FString>& Values);


protected:
    virtual void BuildNativeLayout() override;
    virtual void ApplyNativeStyle() override;
    virtual void BindNativeWidgets() override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUD|Start Menu", meta=(DesignerRebuild))
    EMainWorldScreen ScreenMode = EMainWorldScreen::Start;

    /** Last world-folder map passed from MainGameMode. */
    UPROPERTY(BlueprintReadOnly, Category="Main World|Data")
    TMap<FString, FString> CachedWorldSelectionData;

    /** Last selected world folder key. */
    UPROPERTY(BlueprintReadOnly, Category="Main World|World Selection")
    FString SelectedWorldFolderName;

    UPROPERTY(BlueprintReadOnly, Category="Main World|Multiplayer")
    FString ServerAddress = TEXT("127.0.0.1:7777");

private:
    UPROPERTY(Transient) TObjectPtr<USizeBox> NativeTitleBounds;
    UPROPERTY(Transient) TObjectPtr<UImage> NativeTitleImage;
    UFUNCTION()
    void HandleNativeServerAddressChanged(const FText& Text);

    UFUNCTION()
    void HandleNativeWorldChoiceChanged(FString SelectedItem, ESelectInfo::Type SelectionType);
    UFUNCTION()
    UWidget* GenerateNativeWorldChoice(FString Item);
    void RefreshNativeWorldChoices();

    AMainGameMode* ResolveMainGameMode() const;
    void ApplyProjectSelectionInputMode(UUserWidget* FocusWidget) const;

    UPROPERTY(Transient)
    TObjectPtr<UProjectSelectionWidget> ProjectSelectionWidget;

    ESlateVisibility VisibilityBeforeProjectSelection = ESlateVisibility::Visible;

    /** Weak on purpose: explicit WBP-owned widget references should not keep stale REINST widgets alive. */
    TWeakObjectPtr<UButton> StartButton;
    TWeakObjectPtr<UButton> WorldSelectionButton;
    TWeakObjectPtr<UButton> BackButton;
    TWeakObjectPtr<UButton> RefreshButton;
    TWeakObjectPtr<UButton> MultiplayerButton;
    TWeakObjectPtr<UButton> SettingsButton;
    TWeakObjectPtr<UButton> ProjectsButton;
    TWeakObjectPtr<UButton> HostButton;
    TWeakObjectPtr<UButton> ClientButton;
    TWeakObjectPtr<UButton> JoinButton;

    UPROPERTY(Transient) TObjectPtr<UBorder> NativeMenuPanel;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> NativeMenuTitle;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeStart;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeProjects;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeMultiplayer;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeSettings;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeQuit;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeBack;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeHost;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeClient;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeJoin;
    UPROPERTY(Transient) TObjectPtr<UEditableTextBox> NativeServerAddress;
    UPROPERTY(Transient) TObjectPtr<UComboBoxString> NativeWorldChoice;

    /** Weak on purpose: editor undo buffers can retain REINST widgets, and strong world refs cause stale world leaks. */
    TWeakObjectPtr<AMainGameMode> MainGameMode;
};

/** Native multiplayer screen with the same actions and styling contract as the start screen. */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API UMultiplayerMenuWidget : public UMainWorldWidget
{
    GENERATED_BODY()
public:
    UMultiplayerMenuWidget(const FObjectInitializer& ObjectInitializer);
};
