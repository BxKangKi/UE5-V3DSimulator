// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "NativeWidgetDesignerLibrary.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/Overlay.h"
#include "Components/PanelWidget.h"
#include "Components/ScaleBox.h"
#include "Editor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Crc.h"
#include "Misc/ScopeExit.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UI/MenuButtonWidget.h"
#include "UI/DebugHUDWidget.h"
#include "UI/LoadingHUDWidget.h"
#include "UI/ResponsiveHUDWidget.h"
#include "UI/SettingControlWidget.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "Widgets/SWidget.h"

DEFINE_LOG_CATEGORY_STATIC(LogV3DNativeDesigner, Log, All);

namespace V3DNativeDesigner
{
    // Increment when a native tree's structure changes. Existing assets update on their next open.
    constexpr uint32 LayoutSchemaVersion = 2;
    const FName DefaultsKey(TEXT("V3D.NativeLayoutDefaults"));
    const FName TreeKey(TEXT("V3D.NativeLayoutTree"));
    bool bRefreshing = false;

    UClass* GetWidgetClass(const UWidgetBlueprint* Blueprint)
    {
        if (!IsValid(Blueprint)) return nullptr;
        UClass* Class = Blueprint->GeneratedClass;
        return IsValid(Class) && Class->IsChildOf(Blueprint->ParentClass)
            ? Class : static_cast<UClass*>(Blueprint->ParentClass);
    }

    bool IsNativeClass(const UClass* Class)
    {
        return Class && (Class->IsChildOf(UResponsiveHUDWidget::StaticClass())
            || Class->IsChildOf(UMenuButtonWidget::StaticClass())
            || Class->IsChildOf(USettingControlWidget::StaticClass()));
    }

    void UpgradeShippedGenericHUD(UWidgetBlueprint* Blueprint)
    {
        if (Blueprint->ParentClass != UUserWidget::StaticClass()) return;
        const FString Path = Blueprint->GetPathName();
        UClass* NativeParent = nullptr;
        if (Path == TEXT("/Game/Blueprints/UI/WBP_Debug.WBP_Debug"))
            NativeParent = UDebugHUDWidget::StaticClass();
        else if (Path == TEXT("/Game/Blueprints/UI/WBP_Loading.WBP_Loading"))
            NativeParent = ULoadingHUDWidget::StaticClass();
        if (!NativeParent) return;
        // Only the two shipped generic assets need migration. Custom parents and all other
        // generic UserWidgets retain their own behavior. Both targets inherit the same original
        // UUserWidget contract, so refresh their graphs and regenerate the class after changing
        // the parent. Saving remains an explicit normal asset-save operation.
        TGuardValue<bool> MigrationGuard(bRefreshing, true);
        Blueprint->Modify();
        Blueprint->ParentClass = NativeParent;
        FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }

    FString DefaultsFingerprint(const UWidgetBlueprint* Blueprint, const UClass* Class)
    {
        const UObject* Defaults = Class->GetDefaultObject();
        TArray<FString> Values;
        for (TFieldIterator<FProperty> It(Class); It; ++It)
        {
            const FProperty* Property = *It;
            if (!Property->HasAnyPropertyFlags(CPF_Edit)
                || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
            for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
            {
                FString Value;
                Property->ExportText_InContainer(Index, Value, Defaults, nullptr, nullptr, PPF_None, nullptr);
                Values.Add(FString::Printf(TEXT("%s[%d]=%s"), *Property->GetName(), Index, *Value));
            }
        }
        Values.Sort();
        const FString Text = FString::Join(Values, TEXT("\n"));
        return FString::Printf(TEXT("%u:%s:%08x"), LayoutSchemaVersion,
            *GetPathNameSafe(Blueprint->ParentClass), FCrc::StrCrc32(*Text));
    }

    FString TreeFingerprint(const UWidgetTree* Tree)
    {
        if (!IsValid(Tree) || !IsValid(Tree->RootWidget)) return FString();
        TArray<UWidget*> Widgets;
        Tree->GetAllWidgets(Widgets);
        FString Text;
        for (const UWidget* Widget : Widgets)
        {
            Text += FString::Printf(TEXT("%s:%s:%s\n"), *Widget->GetName(),
                *Widget->GetClass()->GetPathName(), *GetNameSafe(Widget->GetParent()));
        }
        return FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*Text));
    }

    void MakeSerializable(UWidgetTree* Tree, UUserWidget* PreviewOwner)
    {
        TArray<UObject*> Objects;
        GetObjectsWithOuter(Tree, Objects);
        Objects.Add(Tree);
        for (UObject* Object : Objects)
        {
            // Native trees are intentionally transient at runtime. Their asset-owned copies must
            // survive save/reload, including panel slots and nested component subobjects.
            Object->ClearFlags(RF_Transient | RF_DuplicateTransient | RF_TextExportTransient
                | RF_Public | RF_Standalone);
            Object->SetFlags(RF_Transactional);
            for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
            {
                if (CastField<FDelegateProperty>(*It) || CastField<FMulticastDelegateProperty>(*It))
                {
                    // Generated event targets belong to the temporary preview instance. Runtime
                    // native builders bind their own controls after rebuilding their fresh tree.
                    for (int32 Index = 0; Index < It->ArrayDim; ++Index)
                        It->ClearValue_InContainer(Object, Index);
                }
            }
        }
        TMap<UObject*, UObject*> Replacements;
        Replacements.Add(PreviewOwner, nullptr);
        FArchiveReplaceObjectRef<UObject> RemovePreviewOwner(Tree, Replacements,
            EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
    }

    void PreserveReferencedVariables(UWidgetBlueprint* Blueprint, const UWidgetTree* PreviousTree,
        UWidgetTree* NativeTree)
    {
        if (!IsValid(PreviousTree)) return;
        TArray<UWidget*> PreviousWidgets;
        PreviousTree->GetAllWidgets(PreviousWidgets);
        UCanvasPanel* CompatibilityHost = nullptr;
        for (const UWidget* Previous : PreviousWidgets)
        {
            const FName Name = Previous->GetFName();
            const bool bHasEvents = FKismetEditorUtilities::PropertyHasBoundEvents(Blueprint, Name);
            if (!bHasEvents && (!Previous->bIsVariable
                || !FBlueprintEditorUtils::IsVariableUsed(Blueprint, Name, nullptr))) continue;
            if (UWidget* NativeWidget = NativeTree->FindWidget(Name))
            {
                if (NativeWidget->IsA(Previous->GetClass())) NativeWidget->bIsVariable = true;
                else UE_LOG(LogV3DNativeDesigner, Warning,
                    TEXT("%s: legacy variable %s conflicts with a native widget type."),
                    *Blueprint->GetPathName(), *Name.ToString());
                continue;
            }
            UClass* LegacyClass = Previous->GetClass();
            if (LegacyClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
                continue;
            if (!CompatibilityHost)
            {
                // HUD roots overlay a viewport background and scaled canvas; reusable widgets
                // have panel/button content under a ScaleBox.
                // A collapsed sibling on an existing panel preserves referenced property types
                // without restoring an obsolete layout or copying its callbacks/child trees.
                TArray<UWidget*> NativeWidgets;
                NativeTree->GetAllWidgets(NativeWidgets);
                UPanelWidget* HostPanel = nullptr;
                for (UWidget* Widget : NativeWidgets)
                {
                    UPanelWidget* Panel = Cast<UPanelWidget>(Widget);
                    if (Panel && Panel->CanHaveMultipleChildren()) { HostPanel = Panel; break; }
                }
                if (!HostPanel)
                {
                    // Reusable button trees have only single-child panels. Wrap the ScaleBox's
                    // original content in an Overlay so its layout remains unchanged.
                    UScaleBox* Scale = Cast<UScaleBox>(NativeTree->RootWidget);
                    if (!Scale) continue;
                    UWidget* Content = Scale->GetContent();
                    Scale->ClearChildren();
                    UOverlay* Wrapper = NativeTree->ConstructWidget<UOverlay>(
                        UOverlay::StaticClass(), TEXT("NativeCompatibilityLayout"));
                    Scale->SetContent(Wrapper);
                    Wrapper->AddChildToOverlay(Content);
                    HostPanel = Wrapper;
                }
                CompatibilityHost = NativeTree->ConstructWidget<UCanvasPanel>(
                    UCanvasPanel::StaticClass(), TEXT("LegacyBindings_Only"));
                CompatibilityHost->SetVisibility(ESlateVisibility::Collapsed);
                CompatibilityHost->bIsVariable = false;
                HostPanel->AddChild(CompatibilityHost);
            }
            // Only the referenced class/name contract is needed. Copying an old panel wholesale
            // would retain all descendants and stale instance event targets unnecessarily.
            // Unlike ConstructWidget, NewObject does not Initialize a nested UserWidget and
            // duplicate its old component tree. Only the serialized variable type is retained.
            UWidget* Placeholder = NewObject<UWidget>(NativeTree, LegacyClass, Name, RF_Transactional);
            Placeholder->bIsVariable = true;
            Placeholder->SetVisibility(ESlateVisibility::Collapsed);
            CompatibilityHost->AddChild(Placeholder);
        }
    }
}

bool UNativeWidgetDesignerLibrary::UsesNativeWidgetLayout(const UWidgetBlueprint* Blueprint)
{
    UClass* Class = V3DNativeDesigner::GetWidgetClass(Blueprint);
    if (!V3DNativeDesigner::IsNativeClass(Class)) return false;
    const FBoolProperty* Enabled = FindFProperty<FBoolProperty>(Class, TEXT("bUseNativeLayout"));
    return Enabled && Enabled->GetPropertyValue_InContainer(Class->GetDefaultObject());
}

bool UNativeWidgetDesignerLibrary::RefreshNativeWidgetHierarchy(UWidgetBlueprint* Blueprint, bool bForce)
{
    using namespace V3DNativeDesigner;
    if (!IsInGameThread() || bRefreshing || !IsValid(Blueprint)) return false;
    UpgradeShippedGenericHUD(Blueprint);
    if (!UsesNativeWidgetLayout(Blueprint)) return false;
    for (UClass* Parent = Blueprint->ParentClass; Parent; Parent = Parent->GetSuperClass())
    {
        UWidgetBlueprint* ParentBlueprint = Cast<UWidgetBlueprint>(Parent->ClassGeneratedBy);
        if (ParentBlueprint && ParentBlueprint->WidgetTree && ParentBlueprint->WidgetTree->RootWidget)
        {
            // UMG permits only one owning tree in a Blueprint inheritance chain. Keep the
            // inherited child valid instead of adding a second tree that the compiler rejects.
            RefreshNativeWidgetHierarchy(ParentBlueprint, bForce);
            return false;
        }
    }
    UClass* Class = GetWidgetClass(Blueprint);
    if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) return false;
    auto& Metadata = Blueprint->GetOutermost()->GetMetaData();
    const FString DefaultsHash = DefaultsFingerprint(Blueprint, Class);
    if (!bForce && Blueprint->WidgetTree && Blueprint->WidgetTree->RootWidget
        && Metadata.GetValue(Blueprint, DefaultsKey) == DefaultsHash
        && Metadata.GetValue(Blueprint, TreeKey) == TreeFingerprint(Blueprint->WidgetTree)) return false;

    TGuardValue<bool> RefreshGuard(bRefreshing, true);
    TStrongObjectPtr<UUserWidget> Preview(NewObject<UUserWidget>(GetTransientPackage(), Class));
    // Designing alone deliberately excludes ExecutePreConstruct: no user graphs, world queries,
    // player creation, or gameplay construction should run while making an asset template.
    Preview->SetDesignerFlags(EWidgetDesignFlags::Designing);
    TSharedPtr<SWidget> SlatePreview;
    ON_SCOPE_EXIT
    {
        SlatePreview.Reset();
        Preview->ReleaseSlateResources(true);
    };
    if (!Preview->Initialize()) return false;
    if (UResponsiveHUDWidget* HUD = Cast<UResponsiveHUDWidget>(Preview.Get()))
        HUD->PrepareNativeDesignerLayout();
    else
        SlatePreview = Preview->TakeWidget();
    UWidgetTree* SourceTree = Preview->WidgetTree;
    if (!IsValid(SourceTree) || !IsValid(SourceTree->RootWidget)) return false;

    FObjectDuplicationParameters Parameters(SourceTree, Blueprint);
    Parameters.DestName = MakeUniqueObjectName(Blueprint, UWidgetTree::StaticClass(), TEXT("WidgetTree"));
    Parameters.FlagMask = RF_AllFlags & ~RF_Transient;
    Parameters.ApplyFlags = RF_Transactional;
    UWidgetTree* NewTree = Cast<UWidgetTree>(StaticDuplicateObjectEx(Parameters));
    if (!NewTree) return false;
    MakeSerializable(NewTree, Preview.Get());

    Blueprint->Modify();
    PreserveReferencedVariables(Blueprint, Blueprint->WidgetTree, NewTree);
    Blueprint->WidgetTree = NewTree;
    // Bindings to widgets removed with the unused legacy layout have no target to compile.
    Blueprint->Bindings.RemoveAll([NewTree](const FDelegateEditorBinding& Binding)
    {
        return NewTree->FindWidget(FName(*Binding.ObjectName)) == nullptr;
    });
    Metadata.SetValue(Blueprint, DefaultsKey, *DefaultsHash);
    Metadata.SetValue(Blueprint, TreeKey, *TreeFingerprint(NewTree));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    UE_LOG(LogV3DNativeDesigner, Log, TEXT("Materialized native Designer hierarchy: %s"),
        *Blueprint->GetPathName());
    return true;
}

int32 UNativeWidgetDesignerLibrary::RefreshOpenNativeWidgetHierarchies(bool bForce)
{
    if (!GEditor) return 0;
    UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!Editors) return 0;
    int32 Refreshed = 0;
    for (UObject* Asset : Editors->GetAllEditedAssets())
    {
        if (RefreshNativeWidgetHierarchy(Cast<UWidgetBlueprint>(Asset), bForce)) ++Refreshed;
    }
    return Refreshed;
}
