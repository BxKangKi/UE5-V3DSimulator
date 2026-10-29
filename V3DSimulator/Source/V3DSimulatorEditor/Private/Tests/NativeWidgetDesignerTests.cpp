// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "NativeWidgetDesignerLibrary.h"

#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/Overlay.h"
#include "Components/ScaleBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "UI/MenuButtonWidget.h"
#include "UI/LoadingHUDWidget.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "Widgets/SWidget.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DNativeWidgetDesignerTest,
    "V3DSimulator.UI.NativeBlueprintHierarchy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DNativeWidgetDesignerTest::RunTest(const FString&)
{
    UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/V3DNativeHierarchy_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    TStrongObjectPtr<UWidgetBlueprint> Unrelated(Cast<UWidgetBlueprint>(
        FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), Package,
            TEXT("WBP_Debug"), BPTYPE_Normal, UWidgetBlueprint::StaticClass(),
            UWidgetBlueprintGeneratedClass::StaticClass(), TEXT("NativeHierarchyTest"))));
    if (!TestNotNull(TEXT("Unrelated generic widget created"), Unrelated.Get())) return false;
    TestFalse(TEXT("Same asset name at another path does not trigger shipped HUD migration"),
        UNativeWidgetDesignerLibrary::RefreshNativeWidgetHierarchy(Unrelated.Get(), true));
    TestTrue(TEXT("Unrelated generic widget keeps its parent"), Unrelated->ParentClass == UUserWidget::StaticClass());
    TStrongObjectPtr<UWidgetBlueprint> Blueprint(Cast<UWidgetBlueprint>(
        FKismetEditorUtilities::CreateBlueprint(UMenuButtonWidget::StaticClass(), Package,
            TEXT("WBP_NativeHierarchyTest"), BPTYPE_Normal, UWidgetBlueprint::StaticClass(),
            UWidgetBlueprintGeneratedClass::StaticClass(), TEXT("NativeHierarchyTest"))));
    if (!TestNotNull(TEXT("Widget Blueprint created"), Blueprint.Get())) return false;

    // A referenced legacy panel must retain its variable/type, without restoring its full old
    // layout. An unused sibling and the referenced panel's unreferenced children should vanish.
    Blueprint->WidgetTree = NewObject<UWidgetTree>(Blueprint.Get(), NAME_None, RF_Transactional);
    UCanvasPanel* LegacyRoot = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>();
    Blueprint->WidgetTree->RootWidget = LegacyRoot;
    UVerticalBox* LegacyPanel = Blueprint->WidgetTree->ConstructWidget<UVerticalBox>(
        UVerticalBox::StaticClass(), TEXT("LegacyContainer"));
    LegacyPanel->bIsVariable = true;
    LegacyRoot->AddChild(LegacyPanel);
    LegacyPanel->AddChild(Blueprint->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("LegacyUnreferencedChild")));
    LegacyRoot->AddChild(Blueprint->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("LegacyUnused")));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint.Get());
    FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
    if (!TestTrue(TEXT("Legacy source compiles"), Blueprint->Status != BS_Error)) return false;
    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint.Get());
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph)) return false;
    UK2Node_VariableGet* Reference = NewObject<UK2Node_VariableGet>(EventGraph);
    Reference->VariableReference.SetSelfMember(TEXT("LegacyContainer"));
    EventGraph->AddNode(Reference, false, false);
    Reference->AllocateDefaultPins();

    TestTrue(TEXT("Native templates are materialized"),
        UNativeWidgetDesignerLibrary::RefreshNativeWidgetHierarchy(Blueprint.Get(), true));
    UWidgetTree* Tree = Blueprint->WidgetTree;
    if (!TestNotNull(TEXT("Asset tree exists"), Tree)) return false;
    TestTrue(TEXT("Serialized tree is owned by the Blueprint asset"), Tree->GetOuter() == Blueprint.Get());
    TestTrue(TEXT("Designer hierarchy begins with ScaleBox"), Tree->RootWidget->IsA<UScaleBox>());
    UWidget* Compatibility = Tree->FindWidget(TEXT("LegacyBindings_Only"));
    if (TestNotNull(TEXT("Only referenced legacy bindings are retained"), Compatibility))
        TestEqual(TEXT("Compatibility controls never occupy layout space"), Compatibility->GetVisibility(), ESlateVisibility::Collapsed);
    UVerticalBox* Placeholder = Cast<UVerticalBox>(Tree->FindWidget(TEXT("LegacyContainer")));
    if (TestNotNull(TEXT("Referenced variable keeps its original type/name"), Placeholder))
        TestEqual(TEXT("Legacy panel descendants are not copied"), Placeholder->GetChildrenCount(), 0);
    TestNull(TEXT("Unused legacy widget removed"), Tree->FindWidget(TEXT("LegacyUnused")));
    TestNull(TEXT("Unreferenced legacy descendant removed"), Tree->FindWidget(TEXT("LegacyUnreferencedChild")));

    TArray<UObject*> Templates;
    GetObjectsWithOuter(Tree, Templates);
    Templates.Add(Tree);
    for (UObject* Object : Templates)
        TestFalse(TEXT("Every generated template/slot can be saved"), Object->HasAnyFlags(RF_Transient));
    TArray<UWidget*> Widgets;
    Tree->GetAllWidgets(Widgets);
    FName ButtonName;
    for (UWidget* Widget : Widgets)
    {
        if (UButton* Button = Cast<UButton>(Widget))
        {
            ButtonName = Button->GetFName();
            TestFalse(TEXT("Serialized button has no transient preview event target"), Button->OnClicked.IsBound());
            Button->bIsVariable = true;
        }
    }
    TestFalse(TEXT("Reopening an unchanged native tree does not rewrite the asset"),
        UNativeWidgetDesignerLibrary::RefreshNativeWidgetHierarchy(Blueprint.Get()));
    TestTrue(TEXT("Idempotent refresh keeps the same tree"), Blueprint->WidgetTree == Tree);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint.Get());
    FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
    TestTrue(TEXT("Preserved graph variables compile against the native template tree"), Blueprint->Status != BS_Error);
    TStrongObjectPtr<UMenuButtonWidget> Preview(NewObject<UMenuButtonWidget>(GetTransientPackage(), Blueprint->GeneratedClass));
    Preview->SetDesignerFlags(EWidgetDesignFlags::Designing);
    TestTrue(TEXT("Materialized Blueprint initializes without a world"), Preview->Initialize());
    TSharedPtr<SWidget> Slate = Preview->TakeWidget();
    TestEqual(TEXT("Regenerated preview root name matches the hierarchy template"),
        Preview->GetRootWidget()->GetFName(), Tree->RootWidget->GetFName());
    for (UWidget* Widget : Widgets)
    {
        if (Widget == Compatibility || Widget == Placeholder
            || Widget->GetFName() == TEXT("NativeCompatibilityLayout")) continue;
        UWidget* PreviewWidget = Preview->WidgetTree->FindWidget(Widget->GetFName());
        TestNotNull(*FString::Printf(TEXT("Native hierarchy selection resolves %s"), *Widget->GetName()), PreviewWidget);
    }
    if (const FObjectPropertyBase* ButtonVariable = FindFProperty<FObjectPropertyBase>(Preview->GetClass(), ButtonName))
    {
        TestTrue(TEXT("Blueprint widget variable points at the visible regenerated control"),
            ButtonVariable->GetObjectPropertyValue_InContainer(Preview.Get()) == Preview->WidgetTree->FindWidget(ButtonName));
    }
    else AddError(TEXT("The materialized native button variable was not generated."));
    Slate.Reset();
    Preview->ReleaseSlateResources(true);

    UMenuButtonWidget* Defaults = Blueprint->GeneratedClass->GetDefaultObject<UMenuButtonWidget>();
    Defaults->ButtonMinSize.X += 50.0;
    const float ExpectedWidth = static_cast<float>(Defaults->ButtonMinSize.X);
    TestTrue(TEXT("Class Defaults changes update the hierarchy template"),
        UNativeWidgetDesignerLibrary::RefreshNativeWidgetHierarchy(Blueprint.Get()));
    Tree = Blueprint->WidgetTree;
    // Compatibility wrapper is an Overlay; the original native SizeBox remains discoverable.
    Widgets.Reset();
    Tree->GetAllWidgets(Widgets);
    for (UWidget* Widget : Widgets)
    {
        if (USizeBox* Size = Cast<USizeBox>(Widget))
            TestEqual(TEXT("Generated template uses the class-default width"), Size->GetMinDesiredWidth(), ExpectedWidth);
    }

    Defaults = Blueprint->GeneratedClass->GetDefaultObject<UMenuButtonWidget>();
    Defaults->bUseNativeLayout = false;
    TestFalse(TEXT("Custom Blueprint layouts opt out of forced generation"),
        UNativeWidgetDesignerLibrary::RefreshNativeWidgetHierarchy(Blueprint.Get(), true));
    TestTrue(TEXT("Opt-out retains the existing asset tree"), Blueprint->WidgetTree == Tree);
    Package->SetDirtyFlag(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DNativeHUDDesignerViewportTest,
    "V3DSimulator.UI.NativeHUDViewportHierarchy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DNativeHUDDesignerViewportTest::RunTest(const FString&)
{
    UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/V3DNativeHUD_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    TStrongObjectPtr<UWidgetBlueprint> Blueprint(Cast<UWidgetBlueprint>(
        FKismetEditorUtilities::CreateBlueprint(ULoadingHUDWidget::StaticClass(), Package,
            TEXT("WBP_NativeHUDViewportTest"), BPTYPE_Normal, UWidgetBlueprint::StaticClass(),
            UWidgetBlueprintGeneratedClass::StaticClass(), TEXT("NativeHUDViewportTest"))));
    if (!TestNotNull(TEXT("HUD Blueprint created"), Blueprint.Get())) return false;
    TestTrue(TEXT("Viewport hierarchy materializes without gameplay"),
        UNativeWidgetDesignerLibrary::RefreshNativeWidgetHierarchy(Blueprint.Get(), true));
    UOverlay* Root = Cast<UOverlay>(Blueprint->WidgetTree->RootWidget);
    if (!TestNotNull(TEXT("Designer root fills the viewport"), Root)) return false;
    if (!TestEqual(TEXT("Designer separates background from controls"), Root->GetChildrenCount(), 2)) return false;
    TestNotNull(TEXT("Viewport background is below the controls"), Cast<UBorder>(Root->GetChildAt(0)));
    TestNotNull(TEXT("Designer controls retain uniform scaling"), Cast<UScaleBox>(Root->GetChildAt(1)));
    TestFalse(TEXT("Unchanged viewport hierarchy is not rewritten"),
        UNativeWidgetDesignerLibrary::RefreshNativeWidgetHierarchy(Blueprint.Get()));
    FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
    if (!TestTrue(TEXT("Viewport hierarchy compiles"), Blueprint->Status != BS_Error)) return false;
    TStrongObjectPtr<ULoadingHUDWidget> Preview(NewObject<ULoadingHUDWidget>(GetTransientPackage(), Blueprint->GeneratedClass));
    Preview->SetDesignerFlags(EWidgetDesignFlags::Designing);
    if (!TestTrue(TEXT("HUD preview initializes without a world"), Preview->Initialize())) return false;
    TSharedPtr<SWidget> Slate = Preview->TakeWidget();
    if (!TestNotNull(TEXT("Runtime preview preserves the viewport root"), Cast<UOverlay>(Preview->GetRootWidget()))) return false;
    TestEqual(TEXT("Designer and runtime root names match"), Preview->GetRootWidget()->GetFName(), Root->GetFName());
    Slate.Reset();
    Preview->ReleaseSlateResources(true);
    Package->SetDirtyFlag(false);
    return true;
}
#endif
