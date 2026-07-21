#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class STextBlock;

class SVertexMaskForgePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVertexMaskForgePanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FReply OnRefreshSelectionClicked();

	TSharedPtr<STextBlock> SelectionStatusText;
};
