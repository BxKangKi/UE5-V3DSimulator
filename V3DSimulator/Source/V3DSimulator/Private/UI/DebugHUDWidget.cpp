// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/DebugHUDWidget.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "System/GameManagerSubSystem.h"
#include "System/GameUpdateSubSystem.h"
#include "System/SystemInfoFunctionLibrary.h"

namespace DebugHUDWidgetPrivate
{
    FString FormatVector(const TCHAR* Label, const FVector& Value, const TCHAR* Units)
    {
        if (!FMath::IsFinite(Value.X) || !FMath::IsFinite(Value.Y) || !FMath::IsFinite(Value.Z))
            return FString::Printf(TEXT("%s: N/A"), Label);
        return FString::Printf(TEXT("%s: X %.1f  Y %.1f  Z %.1f %s"),
            Label, Value.X, Value.Y, Value.Z, Units);
    }

    FString FormatRotation(const TCHAR* Label, const FRotator& Value)
    {
        if (!FMath::IsFinite(Value.Pitch) || !FMath::IsFinite(Value.Yaw) || !FMath::IsFinite(Value.Roll))
            return FString::Printf(TEXT("%s: N/A"), Label);
        const FRotator Rotation = Value.GetNormalized();
        return FString::Printf(TEXT("%s: Pitch %.1f  Yaw %.1f  Roll %.1f deg"),
            Label, Rotation.Pitch, Rotation.Yaw, Rotation.Roll);
    }
}

UDebugHUDWidget::UDebugHUDWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    PanelSize = FVector2D(1920.f, 1080.f);
    BodyFontSize = 15;
    FontOutlineSize = 1;
    BackgroundColor = FLinearColor::Transparent;
    SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UDebugHUDWidget::BuildNativeLayout()
{
    UBorder* Panel = AddPanel(PanelSize, FVector2D::ZeroVector);
    UOverlay* Body = WidgetTree->ConstructWidget<UOverlay>();
    Panel->SetContent(Body);
    NativeDiagnostics = AddText(Body, FText::GetEmpty());
    NativeDiagnostics->SetAutoWrapText(true);
    NativeDiagnostics->SetJustification(ETextJustify::Right);
    if (UOverlaySlot* Slot = Cast<UOverlaySlot>(NativeDiagnostics->Slot))
    {
        Slot->SetHorizontalAlignment(HAlign_Fill);
        Slot->SetVerticalAlignment(VAlign_Center);
    }
}

void UDebugHUDWidget::ApplyNativeStyle()
{
    Super::ApplyNativeStyle();
    if (IsDesignTime() && IsValid(NativeDiagnostics))
        NativeDiagnostics->SetText(NSLOCTEXT("V3DHUD", "DebugPreview", "CPU: Example CPU (10 cores)\nGPU: Example GPU\nMemory: 2048 / 32768 MB\nFPS: 60.0  Average: 60.0\nCharacter: X 0.0  Y 0.0  Z 90.0 cm\nCharacter rotation: Pitch 0.0  Yaw 45.0  Roll 0.0 deg\nCamera: X 0.0  Y 0.0  Z 180.0 cm\nCamera rotation: Pitch -10.0  Yaw 45.0  Roll 0.0 deg\nVelocity: X 3.0  Y 4.0  Z 0.0 m/s\nSpeed: 5.0 m/s\nNetwork: Standalone\nWorld: Ready"));
}

void UDebugHUDWidget::NativeConstruct()
{
    Super::NativeConstruct();
    if (IsDesignTime()) return;
    // Capability names are stable during a session; query them once instead of every UI update.
    if (HardwareDescription.IsEmpty())
    {
        const FSystemHardwareInfo Hardware = USystemInfoFunctionLibrary::GetSystemHardwareInfo();
        HardwareDescription = FString::Printf(TEXT("CPU: %s (%d cores)\nGPU: %s"),
            *Hardware.CPUBrand, Hardware.CoreCount, *Hardware.GPUBrand);
        TotalMemoryMB = USystemInfoFunctionLibrary::GetTotalMemory();
    }
    if (UpdateHandle == INDEX_NONE)
    {
        if (UGameUpdateSubSystem* Updates = UGameUpdateSubSystem::Get(this))
        {
            UpdateHandle = Updates->RegisterUpdate(this,
                [WeakThis = TWeakObjectPtr<UDebugHUDWidget>(this)](float DeltaSeconds)
                {
                    if (UDebugHUDWidget* Widget = WeakThis.Get()) Widget->UpdateDiagnostics(DeltaSeconds);
                }, 60);
        }
    }
}

void UDebugHUDWidget::NativeDestruct()
{
    if (UGameUpdateSubSystem* Updates = UGameUpdateSubSystem::Get(this))
        Updates->UnregisterUpdate(UpdateHandle);
    UpdateHandle = INDEX_NONE;
    SampleFrames = 0;
    SampleSeconds = 0.0f;
    Super::NativeDestruct();
}

void UDebugHUDWidget::UpdateDiagnostics(float DeltaSeconds)
{
    if (!IsVisible() || !IsUsingNativeLayout() || !IsValid(NativeDiagnostics))
    {
        SampleFrames = 0;
        SampleSeconds = 0.0f;
        return;
    }
    if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.0f) return;
    SampleSeconds += DeltaSeconds;
    ++SampleFrames;
    VisibleSeconds += DeltaSeconds;
    ++VisibleFrames;
    const float Interval = FMath::IsFinite(RefreshIntervalSeconds)
        ? FMath::Clamp(RefreshIntervalSeconds, 0.1f, 2.0f) : 0.25f;
    if (SampleSeconds < Interval) return;
    const float FramesPerSecond = static_cast<float>(SampleFrames) / SampleSeconds;
    SampleSeconds = 0.0f;
    SampleFrames = 0;

    APlayerController* Controller = GetOwningPlayer();
    APawn* Character = IsValid(Controller) ? Controller->GetPawn() : nullptr;
    FString CharacterPosition = TEXT("Character: N/A");
    FString CharacterRotation = TEXT("Character rotation: N/A");
    FString VelocityText = TEXT("Velocity: N/A");
    FString SpeedText = TEXT("Speed: N/A");
    if (IsValid(Character))
    {
        CharacterPosition = DebugHUDWidgetPrivate::FormatVector(TEXT("Character"), Character->GetActorLocation(), TEXT("cm"));
        CharacterRotation = DebugHUDWidgetPrivate::FormatRotation(TEXT("Character rotation"), Character->GetActorRotation());
        // Engine movement velocities are cm/s. Use m/s for both vector components and magnitude.
        const FVector Velocity = Character->GetVelocity() / 100.0;
        VelocityText = DebugHUDWidgetPrivate::FormatVector(TEXT("Velocity"), Velocity, TEXT("m/s"));
        const double Speed = Velocity.Size();
        if (FMath::IsFinite(Speed)) SpeedText = FString::Printf(TEXT("Speed: %.2f m/s"), Speed);
    }

    FString CameraPosition = TEXT("Camera: N/A");
    FString CameraRotation = TEXT("Camera rotation: N/A");
    if (IsValid(Controller))
    {
        FVector ViewLocation = FVector::ZeroVector;
        FRotator ViewRotation = FRotator::ZeroRotator;
        // PlayerViewPoint includes the active camera/view target, rather than control rotation.
        Controller->GetPlayerViewPoint(ViewLocation, ViewRotation);
        CameraPosition = DebugHUDWidgetPrivate::FormatVector(TEXT("Camera"), ViewLocation, TEXT("cm"));
        CameraRotation = DebugHUDWidgetPrivate::FormatRotation(TEXT("Camera rotation"), ViewRotation);
    }
    const UGameManagerSubSystem* Manager = UGameManagerSubSystem::FindGameManager(this);
    const ENetMode Mode = GetWorld() ? GetWorld()->GetNetMode() : NM_Standalone;
    const TCHAR* Network = Mode == NM_Client ? TEXT("Client")
        : Mode == NM_ListenServer ? TEXT("Listen server")
        : Mode == NM_DedicatedServer ? TEXT("Dedicated server") : TEXT("Standalone");
    const FText Value = FText::FromString(FString::Printf(
        TEXT("%s\nMemory: %d / %d MB\nFPS: %.1f  Average: %.1f\n%s\n%s\n%s\n%s\n%s\n%s\nNetwork: %s\nWorld: %s"),
        *HardwareDescription, USystemInfoFunctionLibrary::GetUsedMemory(), TotalMemoryMB,
        FramesPerSecond, static_cast<double>(VisibleFrames) / FMath::Max(VisibleSeconds, UE_DOUBLE_SMALL_NUMBER),
        *CharacterPosition, *CharacterRotation, *CameraPosition, *CameraRotation, *VelocityText, *SpeedText, Network,
        IsValid(Manager) && Manager->IsWorldLoading() ? TEXT("Loading") : TEXT("Ready")));
    if (!NativeDiagnostics->GetText().EqualTo(Value)) NativeDiagnostics->SetText(Value);
}
