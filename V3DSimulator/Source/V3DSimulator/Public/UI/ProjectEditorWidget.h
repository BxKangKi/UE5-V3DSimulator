// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Styling/SlateTypes.h"
#include "System/ProjectEditorDocument.h"
#include "UI/HUDStyle.h"
#include "ProjectEditorWidget.generated.h"
class SVerticalBox;
class STextBlock;
class SEditableTextBox;
class SMultiLineEditableTextBox;
class UBuildStatusWidget;
class UProjectSourceModelBuilder;
class SV3DModelPreview;
class FV3DJsonSyntaxHighlighter;
struct FV3DFileBrowserEntry;
struct FV3DMaterialEntry;
template<typename ItemType> class SListView;

/** Runtime JSON authoring UI. Preview never changes authored transforms or starts a build. */
UCLASS()
class V3DSIMULATOR_API UProjectEditorWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    UProjectEditorWidget(const FObjectInitializer& ObjectInitializer);
    void OpenProject(const FString& ProjectName);
    bool IsDarkMode() const { return bDarkMode; }
    FSimpleDelegate OnClosed;
    virtual void ReleaseSlateResources(bool bReleaseChildren) override;
    virtual void NativeDestruct() override;
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
private:
    // Slate retains pointers to these styles; own them for the entire widget lifetime.
    UPROPERTY(Transient) TObjectPtr<UObject> FontObject;
    UPROPERTY(Transient) FTextBlockStyle EditorTextStyle;
    UPROPERTY(Transient) FButtonStyle EditorButtonStyle;
    UPROPERTY(Transient) FEditableTextBoxStyle EditorInputStyle;
    UPROPERTY(Transient) FComboBoxStyle EditorComboStyle;
    UPROPERTY(Transient) FTableRowStyle EditorRowStyle;
    UPROPERTY(Transient) FTableViewStyle EditorListStyle;
    UPROPERTY(Transient) FCheckBoxStyle EditorCheckStyle;
    FSlateFontInfo TitleFont;
    bool bDarkMode = false;
    V3DHUDStyle::FAuthoringPalette GetThemePalette() const;
    void ApplyEditorTheme();
    TSharedRef<SWidget> MakeEditorButton(const TCHAR* Label, TFunction<void()> Action);
    void InvalidatePreview();
    void QueueFormRefresh();
    bool bFormRefreshPending = false;
    FString Project, Message, Filter, RawDraft;
    TArray<TSharedPtr<FV3DEditorDocument>> Documents, VisibleDocuments;
    TSharedPtr<FV3DEditorDocument> Current;
    TSharedPtr<SListView<TSharedPtr<FV3DEditorDocument>>> List;
    TSharedPtr<SVerticalBox> Form;
    TSharedPtr<SMultiLineEditableTextBox> Raw;
    TSharedPtr<FV3DJsonSyntaxHighlighter> JsonHighlighter;

    // Built-in cross-platform 3D asset file browser modal state
    bool bShowFileBrowser = false;
    FString BrowserCurrentDir;
    FString BrowserFilter;
    FString BrowserStatus;
    TArray<TSharedPtr<FV3DFileBrowserEntry>> BrowserEntries;
    TSet<FString> BrowserSelectedFiles;
    TSharedPtr<SEditableTextBox> BrowserPathBox;
    TSharedPtr<SListView<TSharedPtr<FV3DFileBrowserEntry>>> BrowserListView;

    // Per-node settings (MeshData) mesh/node name search (excluding INST)
    FString NodeSearchQuery;
    FString CachedNodeNamesSourcePath;
    TArray<FString> CachedModelNodeNames;
    TSharedPtr<SVerticalBox> NodeSearchResultsBox;

    // Material search (Terrain: per texture layer, Foliage: default)
    FString MaterialSearchQuery;
    FString MaterialTargetSurface;
    FString CachedMaterialsScopeKey;
    TArray<TSharedPtr<FV3DMaterialEntry>> CachedMaterials;
    TArray<TSharedPtr<FString>> CachedSurfaceOptions;
    TSharedPtr<SVerticalBox> MaterialSearchResultsBox;

    bool bBusy = false, bRawDirty = false, bSettingRaw = false, bRebuildingForm = false;
    uint64 RequestGeneration = 0;
    int32 FormRowsRemaining = 1500;
    UPROPERTY(Transient) TObjectPtr<UBuildStatusWidget> BuildStatus;
    UPROPERTY(Transient) TObjectPtr<UProjectSourceModelBuilder> PreviewBuilder;
    TSharedPtr<SV3DModelPreview> Preview;
    FString PreviewDocument;
    void StartPreview();
    void PreviewFinished(UProjectSourceModelBuilder* Builder, bool bSuccess);
    void RefreshFiles();
    void RefreshProjectDocuments(const FString& PreferredPath = FString(), const FString& CompletionMessage = FString());
    void OpenFileBrowser();
    void CloseFileBrowser();
    void NavigateFileBrowser(const FString& TargetDir);
    void NavigateFileBrowserUp();
    void RefreshFileBrowserEntries();
    void ConfirmFileBrowserImport();
    void Import3DAssetFiles(TArray<FString> SelectedFiles);
    void RefreshForm();
    void EnsureModelNodeNamesLoaded(const FString& SourcePath);
    void RebuildNodeSearchResults();
    void AddNodeSettingsSection(const TSharedRef<SVerticalBox>& Box);
    TSharedPtr<FV3DEditorDocument> FindSoundsDocument() const;
    void EnsureMaterialsLoaded(bool bProjectWide, const FString& SourcePath);
    void RebuildMaterialSearchResults(bool bProjectWide);
    void AddMaterialSearchSection(const TSharedRef<SVerticalBox>& Box);
    void AddObject(const TSharedPtr<FJsonObject>& Object, const TSharedPtr<FJsonObject>& Suggested,
        const TSharedRef<SVerticalBox>& Box, int32 Depth = 0, const FString& Context = FString());
    void Remember();
    void Changed();
    bool ApplyRaw();
    void SaveAll(bool bBuild, bool bClose = false);
    void Close();
    void RequestBack();
    bool HasUnsavedChanges() const;
    bool bConfirmBack = false;
    virtual FReply NativeOnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
    void StartBuild();
    UFUNCTION() void BuildConfirmed();
};
