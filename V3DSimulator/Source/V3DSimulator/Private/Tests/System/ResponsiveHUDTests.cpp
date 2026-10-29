// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/Button.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScaleBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "UI/BuildStatusWidget.h"
#include "UI/CreatorHUDWidget.h"
#include "UI/DebugHUDWidget.h"
#include "UI/LoadingHUDWidget.h"
#include "UI/PauseMenuWidget.h"
#include "UI/ProjectSelectionWidget.h"
#include "UI/SettingsMenuWidget.h"
#include "UI/SimulatorChatWidget.h"
#include "UI/SurvivalHUDWidget.h"
#include "UI/MainWorldWidget.h"
#include "UI/WorldSelectionWidget.h"
#include "UI/HUDLegacyStyle.h"
#include "UI/WidgetStyleSafety.h"
#include "Widgets/SWidget.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/Geometry.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DResponsiveNativeHUDTest,
    "V3DSimulator.UI.ResponsiveNativeHUD",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DResponsiveNativeHUDTest::RunTest(const FString&)
{
    // Designer instances deliberately have no player, game world, registry, or settings file.
    const TArray<UClass*> Classes = {
        UMainWorldWidget::StaticClass(), UMultiplayerMenuWidget::StaticClass(),
        UWorldSelectionWidget::StaticClass(), UProjectSelectionWidget::StaticClass(),
        UPauseMenuWidget::StaticClass(), USettingsMenuWidget::StaticClass(),
        UCreatorHUDWidget::StaticClass(), UBuildStatusWidget::StaticClass(),
        ULoadingHUDWidget::StaticClass(), UDebugHUDWidget::StaticClass(), USurvivalHUDWidget::StaticClass(),
        USimulatorChatWidget::StaticClass()
    };
    for (UClass* Class : Classes)
    {
        TStrongObjectPtr<UResponsiveHUDWidget> HUD(NewObject<UResponsiveHUDWidget>(GetTransientPackage(), Class));
        HUD->SetDesignerFlags(EWidgetDesignFlags::Designing);
        if (!TestTrue(*FString::Printf(TEXT("%s initializes without a world"), *Class->GetName()), HUD->Initialize())) continue;
        TSharedPtr<SWidget> Preview = HUD->TakeWidget();
        HUD->ForceLayoutPrepass();
        UOverlay* Viewport = Cast<UOverlay>(HUD->GetRootWidget());
        if (!TestNotNull(TEXT("HUD root fills the viewport"), Viewport)) continue;
        if (!TestEqual(TEXT("Background and controls have separate layers"), Viewport->GetChildrenCount(), 2)) continue;
        UBorder* Background = Cast<UBorder>(Viewport->GetChildAt(0));
        UScaleBox* Scale = Cast<UScaleBox>(Viewport->GetChildAt(1));
        if (!TestNotNull(TEXT("Background is behind the scaled controls"), Background)
            || !TestNotNull(TEXT("Controls retain their ScaleBox"), Scale)) continue;
        TestTrue(TEXT("Background preserves each screen's opacity and tint"), Background->GetBrushColor().Equals(HUD->BackgroundColor));
        TestEqual(TEXT("Background cannot intercept input"), Background->GetVisibility(), ESlateVisibility::HitTestInvisible);
        TestTrue(TEXT("Native tree active"), HUD->IsUsingNativeLayout());
        TestEqual(TEXT("Uniform fit preserves aspect ratio"), Scale->GetStretch(), EStretch::ScaleToFit);
        TestFalse(TEXT("Viewport DPI transform is preserved"), Scale->IsIgnoreInheritedScale());
        USizeBox* Reference = Cast<USizeBox>(Scale->GetContent());
        if (!TestNotNull(TEXT("Reference size under root"), Reference)) continue;
        UCanvasPanel* Layout = Cast<UCanvasPanel>(Reference->GetContent());
        if (!TestNotNull(TEXT("Canvas layout under reference size"), Layout)) continue;
        TestTrue(TEXT("Native layout contains content"), Layout->GetChildrenCount() > 0);
        const int32 ChildCount = Layout->GetChildrenCount();
        const UWidget* OriginalRoot = HUD->GetRootWidget();
        HUD->TakeWidget();
        HUD->SynchronizeProperties();
        HUD->ForceLayoutPrepass();
        TestTrue(TEXT("Repeated preview keeps same root"), OriginalRoot == HUD->GetRootWidget());
        TestEqual(TEXT("Repeated preview has no duplicate panels"), Layout->GetChildrenCount(), ChildCount);

        // Synchronization must update class defaults without rebuilding or querying gameplay.
        HUD->DesignResolution = FVector2D(2560, 1440);
        HUD->PanelSize = FVector2D(800, 600);
        HUD->SynchronizeProperties();
        HUD->ForceLayoutPrepass();
        TestEqual(TEXT("Updated reference width"), Reference->GetWidthOverride(), 2560.f);
        TestEqual(TEXT("Updated reference height"), Reference->GetHeightOverride(), 1440.f);
        HUD->DesignResolution = FVector2D(std::numeric_limits<double>::quiet_NaN(), -1);
        HUD->SynchronizeProperties();
        HUD->ForceLayoutPrepass();
        TestTrue(TEXT("Nonfinite width repaired"), FMath::IsFinite(Reference->GetWidthOverride()) && Reference->GetWidthOverride() > 0);
        TestTrue(TEXT("Invalid height repaired"), FMath::IsFinite(Reference->GetHeightOverride()) && Reference->GetHeightOverride() > 0);
        HUD->DesignResolution = FVector2D(1920, 1080);
        HUD->BodyFontSize += 1;
        HUD->TextColor = FLinearColor(.95f, .95f, .95f, 1.f);
        HUD->SynchronizeProperties();
        HUD->ForceLayoutPrepass();
        Preview.Reset();
        HUD->ReleaseSlateResources(true);
        Preview = HUD->TakeWidget();
        HUD->SynchronizeProperties();
        HUD->ForceLayoutPrepass();
        TestTrue(TEXT("Preview rebuild remains native"), HUD->IsUsingNativeLayout());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DViewportBackgroundCoverageTest,
    "V3DSimulator.UI.ViewportBackgroundCoverage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DViewportBackgroundCoverageTest::RunTest(const FString&)
{
    // Arrange the real Slate root without requiring a game viewport or window. Pixel sizes
    // and DPI scales vary independently, including the aspect ratios that used to show bars.
    const FVector2D PixelSizes[] = {
        FVector2D(1920, 1080), FVector2D(1024, 768), FVector2D(3440, 1440),
        FVector2D(5120, 1440), FVector2D(1080, 1920), FVector2D(800, 800),
        FVector2D(3840, 2160)
    };
    const float DPIScales[] = { .75f, 1.f, 1.5f, 2.f };
    const TArray<UClass*> Classes = {
        UMainWorldWidget::StaticClass(), UMultiplayerMenuWidget::StaticClass(),
        UWorldSelectionWidget::StaticClass(), UProjectSelectionWidget::StaticClass(),
        UPauseMenuWidget::StaticClass(), USettingsMenuWidget::StaticClass(),
        ULoadingHUDWidget::StaticClass(), UBuildStatusWidget::StaticClass(),
        UCreatorHUDWidget::StaticClass(), UDebugHUDWidget::StaticClass(),
        USurvivalHUDWidget::StaticClass(), USimulatorChatWidget::StaticClass()
    };
    for (UClass* Class : Classes)
    {
        TStrongObjectPtr<UResponsiveHUDWidget> HUD(NewObject<UResponsiveHUDWidget>(GetTransientPackage(), Class));
        HUD->SetDesignerFlags(EWidgetDesignFlags::Designing);
        if (!TestTrue(TEXT("Preview initializes without gameplay"), HUD->Initialize())) continue;
        TSharedPtr<SWidget> Preview = HUD->TakeWidget();
        UOverlay* Viewport = Cast<UOverlay>(HUD->GetRootWidget());
        if (!TestNotNull(TEXT("Viewport root is an Overlay"), Viewport)) continue;
        UBorder* Background = Cast<UBorder>(Viewport->GetChildAt(0));
        if (!TestNotNull(TEXT("Background is the bottom viewport layer"), Background)) continue;
        TSharedRef<SWidget> ViewportSlate = Viewport->TakeWidget();
        TSharedRef<SWidget> BackgroundSlate = Background->TakeWidget();
        for (const FVector2D& Pixels : PixelSizes)
        {
            for (const float DPI : DPIScales)
            {
                const FVector2f LocalSize(Pixels / DPI);
                const FGeometry Geometry = FGeometry::MakeRoot(LocalSize, FSlateLayoutTransform(DPI));
                ViewportSlate->SlatePrepass(DPI);
                FArrangedChildren Children(EVisibility::All);
                ViewportSlate->ArrangeChildren(Geometry, Children, true);
                bool bFoundBackground = false;
                for (int32 Index = 0; Index < Children.Num(); ++Index)
                {
                    const FArrangedWidget& Child = Children[Index];
                    if (Child.Widget != BackgroundSlate) continue;
                    bFoundBackground = true;
                    const FString Scenario = FString::Printf(TEXT("%s at %.0fx%.0f DPI %.2f"),
                        *Class->GetName(), Pixels.X, Pixels.Y, DPI);
                    TestTrue(*FString::Printf(TEXT("%s background covers the full viewport"), *Scenario),
                        Child.Geometry.GetLocalSize().Equals(LocalSize, .01f)
                        && Child.Geometry.GetAbsoluteSize().Equals(FVector2f(Pixels), .01f)
                        && Child.Geometry.GetAbsolutePosition().Equals(Geometry.GetAbsolutePosition(), .01f));
                }
                TestTrue(TEXT("Background participates in viewport layout"), bFoundBackground);
            }
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DLoadingBackLayoutTest,
    "V3DSimulator.UI.LoadingConnectionBack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DLoadingBackLayoutTest::RunTest(const FString&)
{
    TStrongObjectPtr<ULoadingHUDWidget> HUD(NewObject<ULoadingHUDWidget>());
    HUD->SetDesignerFlags(EWidgetDesignFlags::Designing);
    if (!TestTrue(TEXT("Loading preview initializes without gameplay"), HUD->Initialize())) return false;
    HUD->TakeWidget();
    UButton* Back = Cast<UButton>(HUD->GetWidgetFromName(TEXT("LoadingBackButton")));
    if (!TestNotNull(TEXT("Connection loading has a native Back button"), Back)) return false;
    TestTrue(TEXT("Back owns its click binding"), Back->OnClicked.IsBound());
    TestTrue(TEXT("Single-player loading keeps connection Back hidden"), Back->GetParent()->GetVisibility() == ESlateVisibility::Collapsed);
    HUD->BackFromConnection();
    HUD->RefreshLoadingStatus();
    TestTrue(TEXT("Designer Back does not require a world"), HUD->IsUsingNativeLayout());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DBlueprintHUDPreviewTest,
    "V3DSimulator.UI.BlueprintNativeHUDPreview",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DBlueprintHUDPreviewTest::RunTest(const FString&)
{
    const TCHAR* Paths[] = {
        TEXT("/Game/Blueprints/UI/WBP_StartMenu.WBP_StartMenu_C"),
        TEXT("/Game/Blueprints/UI/WBP_WorldSelection.WBP_WorldSelection_C"),
        TEXT("/Game/Blueprints/UI/WBP_ProjectSelection.WBP_ProjectSelection_C"),
        TEXT("/Game/Blueprints/UI/WBP_PauseMenuWidget.WBP_PauseMenuWidget_C"),
        TEXT("/Game/Blueprints/UI/WBP_Settings.WBP_Settings_C"),
        TEXT("/Game/Blueprints/UI/WBP_BuildStatus.WBP_BuildStatus_C")
    };
    for (const TCHAR* Path : Paths)
    {
        UClass* Class = LoadClass<UResponsiveHUDWidget>(nullptr, Path);
        if (!TestNotNull(TEXT("Shipped Blueprint derives from native HUD"), Class)) continue;
        TStrongObjectPtr<UResponsiveHUDWidget> HUD(NewObject<UResponsiveHUDWidget>(GetTransientPackage(), Class));
        HUD->SetDesignerFlags(EWidgetDesignFlags::Designing);
        HUD->Initialize();
        TSharedPtr<SWidget> Preview = HUD->TakeWidget();
        HUD->SynchronizeProperties();
        HUD->ForceLayoutPrepass();
        TestNotNull(TEXT("Blueprint child previews viewport-filling native root"), Cast<UOverlay>(HUD->GetRootWidget()));
        TestTrue(TEXT("Legacy designer tree replaced by native layout"), HUD->IsUsingNativeLayout());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DCompactCreatorHUDTest,
    "V3DSimulator.UI.CompactCreatorToolbar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DCompactCreatorHUDTest::RunTest(const FString&)
{
    TStrongObjectPtr<UCreatorHUDWidget> HUD(NewObject<UCreatorHUDWidget>());
    HUD->SetDesignerFlags(EWidgetDesignFlags::Designing);
    if (!TestTrue(TEXT("Creator preview initializes without gameplay"), HUD->Initialize())) return false;
    TSharedPtr<SWidget> Preview = HUD->TakeWidget();
    HUD->ForceLayoutPrepass();
    TestFalse(TEXT("Creator diagnostics are opt-in"), HUD->bShowStatusDetails);
    UUniformGridPanel* Toolbar = Cast<UUniformGridPanel>(HUD->GetWidgetFromName(TEXT("CreatorToolbarGrid")));
    if (!TestNotNull(TEXT("Toolbar is present"), Toolbar)) return false;
    TestEqual(TEXT("All seven toolbar slots remain usable"), Toolbar->GetChildrenCount(), 7);
    const auto CheckSquareSlots = [this, Toolbar](float Extent)
    {
        for (int32 Index = 0; Index < Toolbar->GetChildrenCount(); ++Index)
        {
            const USizeBox* Size = Cast<USizeBox>(Toolbar->GetChildAt(Index));
            if (!TestNotNull(TEXT("Slot has fixed bounds"), Size)) continue;
            TestEqual(TEXT("Slot width"), Size->GetWidthOverride(), Extent);
            TestEqual(TEXT("Slot height"), Size->GetHeightOverride(), Extent);
        }
    };
    CheckSquareSlots(54.f);
    const UCanvasPanelSlot* ToolbarPlacement = Cast<UCanvasPanelSlot>(Toolbar->GetParent()->Slot);
    if (TestNotNull(TEXT("Toolbar has compact reference-canvas bounds"), ToolbarPlacement))
        TestTrue(TEXT("Default toolbar occupies 432 by 72 pixels"), ToolbarPlacement->GetSize().Equals(FVector2D(432, 72)));

    TArray<UWidget*> Widgets;
    HUD->WidgetTree->GetAllWidgets(Widgets);
    bool bFoundStatus = false;
    for (const UWidget* Widget : Widgets)
    {
        const UTextBlock* Text = Cast<UTextBlock>(Widget);
        if (Text && Text->GetText().ToString().Contains(TEXT("Creator mode")))
        {
            bFoundStatus = true;
            TestEqual(TEXT("Creator status text is collapsed"), Text->GetVisibility(), ESlateVisibility::Collapsed);
        }
    }
    TestTrue(TEXT("Status remains available for optional diagnostics"), bFoundStatus);

    HUD->ToolbarSlotHeight = 80.f;
    HUD->ToolbarSize = FVector2D(4000, 1000); // Old Blueprint rectangle must not stretch slots.
    HUD->SynchronizeProperties();
    HUD->ForceLayoutPrepass();
    CheckSquareSlots(80.f);
    if (ToolbarPlacement) TestTrue(TEXT("Legacy size cannot expand the toolbar"), ToolbarPlacement->GetSize().Equals(FVector2D(614, 98)));
    HUD->ToolbarSlotHeight = std::numeric_limits<float>::quiet_NaN();
    HUD->SynchronizeProperties();
    HUD->ForceLayoutPrepass();
    CheckSquareSlots(54.f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DMouseSensitivityMenuTest,
    "V3DSimulator.UI.MouseSensitivityMenu",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DMouseSensitivityMenuTest::RunTest(const FString&)
{
    TStrongObjectPtr<USettingsMenuWidget> Menu(NewObject<USettingsMenuWidget>());
    Menu->SetDesignerFlags(EWidgetDesignFlags::Designing);
    Menu->Initialize();
    const TArray<ESettingsField> Fields = Menu->GetSettingFieldList();
    TestTrue(TEXT("Sensitivity is first visible setting"), Fields.Num() > 0 && Fields[0] == ESettingsField::MouseSensitivity);
    TestEqual(TEXT("Sensitivity uses a slider"), Menu->GetSettingControlType(ESettingsField::MouseSensitivity), ESettingsControlType::Slider);
    float Min = 0, Max = 0, Step = 0;
    TestTrue(TEXT("Sensitivity exposes a slider range"), Menu->GetSettingSliderRange(ESettingsField::MouseSensitivity, Min, Max, Step));
    TestEqual(TEXT("Sensitivity minimum"), Min, .05f);
    TestEqual(TEXT("Sensitivity maximum"), Max, 5.f);
    TestEqual(TEXT("Sensitivity increment"), Step, .05f);
    TestEqual(TEXT("Preview default is neutral"), Menu->GetPendingNumericSettingValue(ESettingsField::MouseSensitivity), 1.f);
    TSharedPtr<SWidget> Preview = Menu->TakeWidget();
    Menu->ForceLayoutPrepass();
    TestTrue(TEXT("Slider preview needs no game world"), Menu->IsUsingNativeLayout());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DOwnedTextBoxStyleTest,
    "V3DSimulator.UI.OwnedTextBoxStylePrepass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DOwnedTextBoxStyleTest::RunTest(const FString&)
{
    TStrongObjectPtr<UEditableTextBox> Input(NewObject<UEditableTextBox>());
    TSharedPtr<SWidget> Preview = Input->TakeWidget();
    const FEditableTextBoxStyle* Storage = &Input->GetWidgetStyle();
    for (int32 Index = 0; Index < 16; ++Index)
    {
        // Destroy the caller's style before the deferred layout, as thumbnail rendering does.
        {
            FEditableTextBoxStyle Temporary = Input->GetWidgetStyle();
            Temporary.TextStyle.Font.Size = 16 + Index;
            V3DWidgetStyle::SetOwnedStyle(*Input, Temporary);
        }
        TestTrue(TEXT("Style storage belongs to the input widget"), Storage == &Input->GetWidgetStyle());
        Input->ForceLayoutPrepass();
        TestEqual(TEXT("Font survives the deferred prepass"), double(Input->GetWidgetStyle().TextStyle.Font.Size), double(16 + Index));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DOriginalHUDStyleTest,
    "V3DSimulator.UI.OriginalHUDStyle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DOriginalHUDStyleTest::RunTest(const FString&)
{
    const UMainWorldWidget* Start = GetDefault<UMainWorldWidget>();
    const UPauseMenuWidget* Pause = GetDefault<UPauseMenuWidget>();
    const ULoadingHUDWidget* Loading = GetDefault<ULoadingHUDWidget>();
    TestTrue(TEXT("Start uses the original serialized background"), Start->BackgroundColor.Equals(V3DHUDLegacyStyle::MenuBackground));
    TestTrue(TEXT("Original blue-gray buttons restored"), Start->ButtonColor.Equals(V3DHUDLegacyStyle::ButtonBackground));
    TestTrue(TEXT("Original translucent pause background restored"), Pause->BackgroundColor.Equals(V3DHUDLegacyStyle::PauseBackground));
    TestTrue(TEXT("Original loading background restored"), Loading->BackgroundColor.Equals(V3DHUDLegacyStyle::LoadingBackground));
    TestTrue(TEXT("Original green progress restored"), Loading->AccentColor.Equals(V3DHUDLegacyStyle::ProgressFill));
    TestEqual(TEXT("Original start font size"), Start->BodyFontSize, 48);
    TestEqual(TEXT("Original pause font size"), Pause->BodyFontSize, 64);
    TestEqual(TEXT("Original pause title size"), Pause->TitleFontSize, 100);
    TestTrue(TEXT("Original Pretendard font retained"), IsValid(Start->FontObject.Get()) && Start->FontObject->GetName() == TEXT("FT_Pretendard"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DMainWorldTitleBoundsTest,
    "V3DSimulator.UI.MainWorldTitleBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DMainWorldTitleBoundsTest::RunTest(const FString&)
{
    TStrongObjectPtr<UMainWorldWidget> HUD(NewObject<UMainWorldWidget>());
    HUD->SetDesignerFlags(EWidgetDesignFlags::Designing);
    if (!TestTrue(TEXT("MainWorld initializes without a game world"), HUD->Initialize())) return false;
    TSharedPtr<SWidget> Preview = HUD->TakeWidget();
    HUD->ForceLayoutPrepass();
    USizeBox* Bounds = Cast<USizeBox>(HUD->GetWidgetFromName(TEXT("MainWorldTitleBounds")));
    if (!TestNotNull(TEXT("Title texture has explicit reference-space bounds"), Bounds)) return false;
    TestEqual(TEXT("Default logo width respects the menu width limit"), Bounds->GetWidthOverride(), 1152.f);
    TestEqual(TEXT("Default logo height"), Bounds->GetHeightOverride(), 140.f);
    const UScaleBox* Scale = Cast<UScaleBox>(Bounds->GetContent());
    if (TestNotNull(TEXT("Logo retains the configured title sizing"), Scale))
    {
        TestEqual(TEXT("Logo fills its title bounds"), Scale->GetStretch(), EStretch::Fill);
        TestNotNull(TEXT("Original title image is retained"), Cast<UImage>(Scale->GetContent()));
    }
    const UOverlaySlot* Slot = Cast<UOverlaySlot>(Bounds->Slot);
    if (TestNotNull(TEXT("Logo is anchored within the menu overlay"), Slot))
    {
        TestEqual(TEXT("Logo remains on the right"), Slot->GetHorizontalAlignment(), HAlign_Right);
        TestEqual(TEXT("Logo remains at the top"), Slot->GetVerticalAlignment(), VAlign_Top);
    }

    HUD->TitleImageSize = FVector2D(100000.0, 100000.0);
    HUD->SynchronizeProperties();
    HUD->ForceLayoutPrepass();
    TestTrue(TEXT("Oversized class defaults cannot fill the screen"),
        Bounds->GetWidthOverride() <= 1920.f * .6f && Bounds->GetHeightOverride() <= 1080.f * .25f);
    HUD->TitleImageSize = FVector2D(std::numeric_limits<double>::quiet_NaN(), -10.0);
    HUD->SynchronizeProperties();
    HUD->ForceLayoutPrepass();
    TestTrue(TEXT("Invalid logo bounds remain finite and positive"),
        FMath::IsFinite(Bounds->GetWidthOverride()) && Bounds->GetWidthOverride() > 0
        && FMath::IsFinite(Bounds->GetHeightOverride()) && Bounds->GetHeightOverride() > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DMainWorldClassRedirectTest,
    "V3DSimulator.UI.MainWorldClassRedirect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DMainWorldClassRedirectTest::RunTest(const FString&)
{
    TestTrue(TEXT("Existing Blueprint parent references resolve to MainWorld"),
        LoadClass<UMainWorldWidget>(nullptr, TEXT("/Script/V3DSimulator.StartWorldWidget"))
            == UMainWorldWidget::StaticClass());
    return true;
}
#endif
