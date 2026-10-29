// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "UI/NativeWidgetTreeNames.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "PropertyPathHelpers.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

void V3DNativeWidgetTree::StabilizeNames(UWidgetTree* Tree)
{
    if (!IsValid(Tree)) return;
    TArray<UWidget*> Widgets;
    Tree->GetAllWidgets(Widgets);
    struct FPendingName { UWidget* Widget; FName Name; };
    TArray<FPendingName> Pending;
    for (int32 Index = 0; Index < Widgets.Num(); ++Index)
    {
        UWidget* Widget = Widgets[Index];
        if (!IsValid(Widget)) continue;
        // Explicit roles such as Label, Slider, NativeProjectList and MainWorldTitleImage
        // are public binding contracts. Only replace NewObject's class-based auto names.
        const FName Name = Widget->GetFName();
        if (Name.GetNumber() == 0 || Name.GetPlainNameString() != Widget->GetClass()->GetName()) continue;
        Pending.Add({Widget, FName(*FString::Printf(TEXT("Native_%s_%04d"), *Widget->GetClass()->GetName(), Index))});
    }
    // Free all old names before assigning final names; avoid name swaps or detached template
    // objects changing the result. Renaming these instance controls must not dirty assets.
    const ERenameFlags Flags = REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional;
    for (const FPendingName& Item : Pending)
    {
        const FName Temporary = MakeUniqueObjectName(Item.Widget->GetOuter(), Item.Widget->GetClass(), TEXT("V3DTemporaryWidget"));
        Item.Widget->Rename(*Temporary.ToString(), nullptr, Flags);
    }
    for (const FPendingName& Item : Pending)
    {
        FName Target = Item.Name;
        for (int32 Suffix = 1; FindObjectFast<UObject>(Item.Widget->GetOuter(), Target); ++Suffix)
            Target = FName(*FString::Printf(TEXT("%s_Generated%d"), *Item.Name.ToString(), Suffix));
        Item.Widget->Rename(*Target.ToString(), nullptr, Flags);
    }
}

void V3DNativeWidgetTree::BindBlueprintWidgets(UUserWidget* Owner)
{
    if (!IsValid(Owner) || !IsValid(Owner->WidgetTree)) return;
    const UWidgetBlueprintGeneratedClass* GeneratedClass = Cast<UWidgetBlueprintGeneratedClass>(Owner->GetClass());
    if (!GeneratedClass) return;

    // Initialize() originally assigned variables and event targets to the generated class's
    // cloned template tree. Replacing that tree must move those references to the controls that
    // are actually displayed. This runs once per new native tree, never during ordinary styling.
    if (!Owner->IsDesignTime()) UBlueprintGeneratedClass::UnbindDynamicDelegates(Owner->GetClass(), Owner);
    TArray<UWidget*> Widgets;
    Owner->WidgetTree->GetAllWidgets(Widgets);
    TMap<FName, UWidget*> WidgetsByName;
    WidgetsByName.Reserve(Widgets.Num());
    for (UWidget* Widget : Widgets) WidgetsByName.Add(Widget->GetFName(), Widget);
    TMap<FName, FObjectPropertyBase*> Properties;
    for (TFieldIterator<FObjectPropertyBase> It(Owner->GetClass()); It; ++It)
    {
        FObjectPropertyBase* Property = *It;
        if (!Property->PropertyClass || !Property->PropertyClass->IsChildOf(UWidget::StaticClass())) continue;
        UWidget* const* FoundWidget = WidgetsByName.Find(Property->GetFName());
        UWidget* Widget = FoundWidget ? *FoundWidget : nullptr;
        if (!Widget || !Widget->IsA(Property->PropertyClass)) continue;
        Property->SetObjectPropertyValue_InContainer(Owner, Widget);
        Properties.Add(Property->GetFName(), Property);
    }
    // Designer instances need the same variable references for selection, but must not execute
    // Blueprint property getters or install gameplay callbacks while generating thumbnails.
    if (Owner->IsDesignTime()) return;
    TArray<const UWidgetBlueprintGeneratedClass*> WidgetClasses;
    for (const UClass* Class = Owner->GetClass(); Class; Class = Class->GetSuperClass())
    {
        if (const UWidgetBlueprintGeneratedClass* WidgetClass = Cast<UWidgetBlueprintGeneratedClass>(Class))
            WidgetClasses.Add(WidgetClass);
    }
    // Apply inherited property bindings before child overrides, matching normal class defaults.
    for (int32 Index = WidgetClasses.Num() - 1; Index >= 0; --Index)
    {
        for (const FDelegateRuntimeBinding& Binding : WidgetClasses[Index]->Bindings)
        {
            FObjectPropertyBase* const* FoundProperty = Properties.Find(FName(*Binding.ObjectName));
            UWidget* Widget = FoundProperty
                ? Cast<UWidget>((*FoundProperty)->GetObjectPropertyValue_InContainer(Owner))
                : WidgetsByName.FindRef(FName(*Binding.ObjectName));
            if (!IsValid(Widget)) continue;
            FDelegateProperty* DelegateProperty = FindFProperty<FDelegateProperty>(
                Widget->GetClass(), FName(*(Binding.PropertyName.ToString() + TEXT("Delegate"))));
            if (!DelegateProperty)
                DelegateProperty = FindFProperty<FDelegateProperty>(Widget->GetClass(), Binding.PropertyName);
            if (!DelegateProperty) continue;
            if (Binding.SourcePath.IsValid() && Widget->AddBinding(DelegateProperty, Owner, Binding.SourcePath))
                continue;
            if (FScriptDelegate* ScriptDelegate = DelegateProperty->GetPropertyValuePtr_InContainer(Widget))
                ScriptDelegate->BindUFunction(Owner, Binding.FunctionName);
        }
    }
    UBlueprintGeneratedClass::BindDynamicDelegates(Owner->GetClass(), Owner);
}
