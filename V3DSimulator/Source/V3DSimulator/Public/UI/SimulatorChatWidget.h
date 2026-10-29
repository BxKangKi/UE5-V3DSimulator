// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "UI/ResponsiveHUDWidget.h"
#include "Components/EditableTextBox.h"
#include "SimulatorChatWidget.generated.h"

class UTextBlock;
class UScrollBox;

/** Native command chat: Enter opens, Enter submits, Escape closes. No WBP asset is required. */
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
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Output;
    UPROPERTY(Transient) TObjectPtr<UScrollBox> OutputScroll;
    TArray<FString> History;
    UFUNCTION() void Commit(const FText& Text, ETextCommit::Type Method);
};
