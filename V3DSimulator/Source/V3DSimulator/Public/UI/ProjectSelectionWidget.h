// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Types/SlateEnums.h"
#include "System/ProjectConfig.h"
#include "UI/SelectionWidgetBase.h"
#include "ProjectSelectionWidget.generated.h"

class UBorder;
class UButton;
class UEditableTextBox;
class UMultiLineEditableTextBox;
class UPanelWidget;
class UMenuButtonWidget;
class UMainWorldWidget;
class UBuildStatusWidget;
class UComboBoxString;
class UCheckBox;
class UProjectEditorWidget;
class UTextBlock;
class UVerticalBox;

/**
 * Project browser and dedicated New Project creation screen shown over the start menu.
 *
 * The complete layout and entry lifecycle are implemented in C++ and previewed in Blueprint children.
 * Native code discovers valid Projects/<Project> folders, provides a dedicated project creation screen
 * (name, project type, notes/memo, and world options), and opens the JSON editor immediately after creation.
 */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API UProjectSelectionWidget : public USelectionWidgetBase
{
    GENERATED_BODY()

public:
    UProjectSelectionWidget(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

    /** Internal owner assigned when this widget is registered with UMainWorldWidget. */
    void SetOwnerMainWidget(UMainWorldWidget* InOwnerWidget);

    /** Assign the panel that receives generated project buttons. */
    UFUNCTION(BlueprintCallable, Category="Projects|Widgets")
    void SetProjectListPanel(UPanelWidget* InPanel);

    /** Assign the Back button. */
    UFUNCTION(BlueprintCallable, Category="Projects|Widgets")
    void SetBackButton(UButton* InButton);

    /** Optional Refresh button. */
    UFUNCTION(BlueprintCallable, Category="Projects|Widgets")
    void SetRefreshButton(UButton* InButton);

    /** Text box used for the new project name input. */
    UFUNCTION(BlueprintCallable, Category="Projects|Widgets")
    void SetNewProjectNameTextBox(UEditableTextBox* InTextBox);

    /** Optional explicit BuildStatus widget. If unset, BuildStatusWidgetClass is loaded from the central registry. */
    UFUNCTION(BlueprintCallable, Category="Projects|Widgets")
    void SetBuildStatusWidget(UBuildStatusWidget* InWidget);

    UFUNCTION(BlueprintPure, Category="Projects|Widgets")
    UBuildStatusWidget* GetBuildStatusWidget() const { return BuildStatusWidget.Get(); }

    /** Re-scan V3DSimulator/Projects and rebuild the generated project list. */
    UFUNCTION(BlueprintCallable, Category="Projects")
    void RefreshProjects();

    /** Creates Projects/<Name>/config.json with the selected ProjectType, notes, and options, then opens the Editor. */
    UFUNCTION(BlueprintCallable, Category="Projects")
    bool CreateNewProject(const FString& ProjectName);

    /** Starts the selected project build. World projects emit .v3d; Prefab/Character/Dynamic projects emit Resources/<Project>.v3d. */
    UFUNCTION(BlueprintCallable, Category="Projects")
    bool BuildProjectByName(const FString& ProjectName);

    UFUNCTION(BlueprintCallable, Category="Projects|Configuration")
    bool GetProjectConfiguration(
        const FString& ProjectName,
        EV3DSimulatorProjectType& OutProjectType,
        bool& bOutAllowProjectAssets,
        FString& OutDisplayName) const;

    UFUNCTION(BlueprintCallable, Category="Projects|Configuration")
    bool SetProjectType(const FString& ProjectName, EV3DSimulatorProjectType ProjectType);

    UFUNCTION(BlueprintCallable, Category="Projects|Configuration")
    void SetNewProjectType(EV3DSimulatorProjectType ProjectType);

    UFUNCTION(BlueprintPure, Category="Projects|Configuration")
    EV3DSimulatorProjectType GetNewProjectType() const { return NewProjectType; }

    UFUNCTION(BlueprintCallable, Category="Projects|Configuration")
    bool SetWorldProjectAssetsAllowed(const FString& ProjectName, bool bAllowed);

    /** Hide this registered widget and restore the start widget. */
    UFUNCTION(BlueprintCallable, Category="Projects")
    void CloseProjectSelection();

protected:
    virtual void BuildNativeLayout() override;
    virtual void ApplyNativeStyle() override;
    virtual void BindNativeWidgets() override;
    virtual void OnSelectionEntryActivated(const FString& SelectionKey) override;

private:
    UFUNCTION()
    void HandleNativeProjectTypeChanged(FString SelectedItem, ESelectInfo::Type SelectionType);
    UFUNCTION()
    UWidget* GenerateNativeProjectType(FString Item);
    UFUNCTION()
    void HandleCreateProjectConfirmClicked();
    UFUNCTION()
    void HandleCreateProjectCancelClicked();

    void OpenEditor(const FString& ProjectName);
    void HandleEditorClosed();
    UPROPERTY(Transient) TObjectPtr<UProjectEditorWidget> EditorWidget;
    void BindNavigationButtons();
    void UnbindNavigationButtons();
    UBuildStatusWidget* EnsureBuildStatusWidget();
    void ShowBuildStatus(const FString& ProjectName);
    void SetCreateProjectMode(bool bEnabled, bool bClearText = false);
    void UpdateWorldOptionsVisibility();

    UFUNCTION()
    void HandleBuildStatusConfirmed();

    TWeakObjectPtr<UMainWorldWidget> OwnerMainWidget;
    TWeakObjectPtr<UButton> BackButton;
    TWeakObjectPtr<UButton> RefreshButton;
    TWeakObjectPtr<UEditableTextBox> NewProjectNameTextBox;
    bool bCreateProjectMode = false;
    EV3DSimulatorProjectType NewProjectType = EV3DSimulatorProjectType::World;

    UPROPERTY(Transient)
    TObjectPtr<UBuildStatusWidget> BuildStatusWidget;

    bool bGeneratedBuildStatusWidget = false;

    UPROPERTY(Transient) TObjectPtr<UBorder> NativeProjectPanel;
    UPROPERTY(Transient) TObjectPtr<UVerticalBox> NativeProjectListScreen;
    UPROPERTY(Transient) TObjectPtr<UVerticalBox> NativeCreateProjectScreen;
    UPROPERTY(Transient) TObjectPtr<UBorder> NativeCreateProjectCard;
    UPROPERTY(Transient) TObjectPtr<UPanelWidget> NativeProjectList;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeProjectBack;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeProjectRefresh;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeCreateBackButton;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeCreateConfirmButton;
    UPROPERTY(Transient) TObjectPtr<UButton> NativeCreateCancelButton;
    UPROPERTY(Transient) TObjectPtr<UEditableTextBox> NativeProjectName;
    UPROPERTY(Transient) TObjectPtr<UComboBoxString> NativeProjectType;
    UPROPERTY(Transient) TObjectPtr<UMultiLineEditableTextBox> NativeProjectNotes;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> NativeCreateErrorText;
    UPROPERTY(Transient) TObjectPtr<UVerticalBox> NativeWorldOptionsBox;
    UPROPERTY(Transient) TObjectPtr<UCheckBox> NewProjectAcoustics;
    UPROPERTY(Transient) TObjectPtr<UCheckBox> NewProjectMusic;
    UPROPERTY(Transient) TObjectPtr<UCheckBox> NewProjectExternalAssets;
};
