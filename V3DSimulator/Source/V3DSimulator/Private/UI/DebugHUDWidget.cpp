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
#include "World/WorldData.h"

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
    FontOutlineSize = 0;
    BackgroundColor = FLinearColor::Transparent;
    SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UDebugHUDWidget::BuildNativeLayout()
{
    UBorder* Panel = AddPanel(PanelSize, FVector2D::ZeroVector);
    UOverlay* Body = WidgetTree->ConstructWidget<UOverlay>();
    Panel->SetContent(Body);

    NativeDiagnosticsBox = WidgetTree->ConstructWidget<UBorder>();
    NativeDiagnosticsBox->SetVisibility(ESlateVisibility::HitTestInvisible);
    NativeDiagnosticsBox->SetPadding(FMargin(18.f, 14.f));
    NativeDiagnosticsBox->SetBrushColor(DiagnosticsBoxColor);
    if (UOverlaySlot* BoxSlot = Body->AddChildToOverlay(NativeDiagnosticsBox))
    {
        BoxSlot->SetHorizontalAlignment(HAlign_Right);
        BoxSlot->SetVerticalAlignment(VAlign_Center);
        BoxSlot->SetPadding(FMargin(24.f));
    }

    NativeDiagnostics = AddText(NativeDiagnosticsBox, FText::GetEmpty());
    NativeDiagnostics->SetAutoWrapText(false);
    NativeDiagnostics->SetJustification(ETextJustify::Right);
}

void UDebugHUDWidget::ApplyNativeStyle()
{
    Super::ApplyNativeStyle();
    if (IsValid(NativeDiagnosticsBox))
    {
        NativeDiagnosticsBox->SetBrushColor(DiagnosticsBoxColor);
    }
    if (IsValid(NativeDiagnostics))
    {
        NativeDiagnostics->SetColorAndOpacity(FSlateColor(DiagnosticsTextColor));
    }
#if WITH_EDITOR
    if (IsDesignTime() && IsValid(NativeDiagnostics))
        NativeDiagnostics->SetText(NSLOCTEXT("V3DHUD", "DebugPreview", "CPU: Example CPU (10 cores)\nGPU: Example GPU\nMemory: 2048 / 32768 MB\nFPS: 60.0  Average: 60.0\nCharacter: X 0.0  Y 0.0  Z 90.0 cm\nCharacter rotation: Pitch 0.0  Yaw 45.0  Roll 0.0 deg\nCamera: X 0.0  Y 0.0  Z 180.0 cm\nCamera rotation: Pitch -10.0  Yaw 45.0  Roll 0.0 deg\nVelocity: X 3.0  Y 4.0  Z 0.0 m/s\nSpeed: 5.0 m/s\nNetwork: Standalone\nWorld: Ready"));
#endif
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
    FString WorldTimeText = TEXT("World time: N/A");
    if (IsValid(Manager))
    {
        if (const UWorldData* WorldData = Manager->GetWorldData();
            IsValid(WorldData) && FMath::IsFinite(WorldData->WorldTime)
            && FMath::IsFinite(WorldData->OneDayTime) && WorldData->OneDayTime > UE_SMALL_NUMBER)
        {
            const double TotalSec = FMath::Max(0.0, static_cast<double>(WorldData->WorldTime));
            const double DayLen = static_cast<double>(WorldData->OneDayTime);
            const int32 DayIndex = static_cast<int32>(FMath::FloorToDouble(TotalSec / DayLen)) + 1;
            const double SecInDay = FMath::Fmod(TotalSec, DayLen);
            const double NormSeconds24h = (SecInDay / DayLen) * 86400.0;
            const int32 Hours = static_cast<int32>(NormSeconds24h / 3600.0) % 24;
            const int32 Minutes = static_cast<int32>(NormSeconds24h / 60.0) % 60;
            const int32 Seconds = static_cast<int32>(NormSeconds24h) % 60;
            WorldTimeText = FString::Printf(TEXT("World time: Day %d  %02d:%02d:%02d"), DayIndex, Hours, Minutes, Seconds);
        }
    }
    const ENetMode Mode = GetWorld() ? GetWorld()->GetNetMode() : NM_Standalone;
    const TCHAR* Network = Mode == NM_Client ? TEXT("Client")
        : Mode == NM_ListenServer ? TEXT("Listen server")
        : Mode == NM_DedicatedServer ? TEXT("Dedicated server") : TEXT("Standalone");
    const FText Value = FText::FromString(FString::Printf(
        TEXT("%s\nMemory: %d / %d MB\nFPS: %.1f  Average: %.1f\n%s\n%s\n%s\n%s\n%s\n%s\n%s\nNetwork: %s\nWorld: %s"),
        *HardwareDescription, USystemInfoFunctionLibrary::GetUsedMemory(), TotalMemoryMB,
        FramesPerSecond, static_cast<double>(VisibleFrames) / FMath::Max(VisibleSeconds, UE_DOUBLE_SMALL_NUMBER),
        *CharacterPosition, *CharacterRotation, *CameraPosition, *CameraRotation, *VelocityText, *SpeedText,
        *WorldTimeText, Network,
        IsValid(Manager) && Manager->IsWorldLoading() ? TEXT("Loading") : TEXT("Ready")));
    if (!NativeDiagnostics->GetText().EqualTo(Value)) NativeDiagnostics->SetText(Value);
}
