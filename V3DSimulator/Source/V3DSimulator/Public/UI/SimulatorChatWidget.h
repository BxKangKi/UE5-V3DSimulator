// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/EditableTextBox.h"
#include "SimulatorChatWidget.generated.h"

class UTextBlock;

/** Native command chat: Enter opens, Enter submits, Escape closes. No WBP asset is required. */
UCLASS()
class V3DSIMULATOR_API USimulatorChatWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    void FocusInput();
    void AppendMessage(const FString& Message);
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeConstruct() override;
    virtual FReply NativeOnPreviewKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
private:
    UPROPERTY(Transient) TObjectPtr<UEditableTextBox> Input;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Output;
    TArray<FString> History;
    UFUNCTION() void Commit(const FText& Text, ETextCommit::Type Method);
};
