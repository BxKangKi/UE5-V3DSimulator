// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "UI/ResponsiveHUDWidget.h"
#include "Components/EditableTextBox.h"
#include "SimulatorChatWidget.generated.h"

class UButton;
class UTextBlock;
class UScrollBox;

/** Native custom chat: T/Enter opens, Enter/Send submits, Escape/Close closes. Works in Shipping and Mobile builds. */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API USimulatorChatWidget : public UResponsiveHUDWidget
{
    GENERATED_BODY()
public:
    USimulatorChatWidget();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Chat", meta=(ClampMin="1", ClampMax="64"))
    int32 MaxHistoryLines = 8;

    UFUNCTION(BlueprintCallable, Category="Chat")
    void FocusInput();
    UFUNCTION(BlueprintCallable, Category="Chat")
    void AppendMessage(const FString& Message);
protected:
    virtual void BuildNativeLayout() override;
    virtual void ApplyNativeStyle() override;
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual FReply NativeOnPreviewKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
private:
    UPROPERTY(Transient) TObjectPtr<UEditableTextBox> Input;
    UPROPERTY(Transient) TObjectPtr<UButton> SendButton;
    UPROPERTY(Transient) TObjectPtr<UButton> CloseButton;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Output;
    UPROPERTY(Transient) TObjectPtr<UScrollBox> OutputScroll;
    TArray<FString> History;
    void SubmitCurrentInput();
    UFUNCTION() void Commit(const FText& Text, ETextCommit::Type Method);
    UFUNCTION() void HandleSendClicked();
    UFUNCTION() void HandleCloseClicked();
};
