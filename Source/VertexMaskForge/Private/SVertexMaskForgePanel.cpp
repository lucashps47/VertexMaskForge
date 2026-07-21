#include "SVertexMaskForgePanel.h"

#include "Logging/LogMacros.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogVertexMaskForge, Log, All);

#define LOCTEXT_NAMESPACE "SVertexMaskForgePanel"

void SVertexMaskForgePanel::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SBorder)
		.Padding(FMargin(12.f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 0.f, 0.f, 4.f))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PanelTitle", "Vertex Mask Forge"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 0.f, 0.f, 12.f))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PanelSubtitle", "Native Vertex Color authoring tool"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 0.f, 0.f, 12.f))
			[
				SNew(SSeparator)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 0.f, 0.f, 12.f))
			[
				SAssignNew(SelectionStatusText, STextBlock)
				.Text(LOCTEXT("NoSelection", "No mesh selected."))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Left)
			[
				SNew(SButton)
				.Text(LOCTEXT("RefreshSelection", "Refresh Selection"))
				.OnClicked(this, &SVertexMaskForgePanel::OnRefreshSelectionClicked)
			]
		]
	];
}

FReply SVertexMaskForgePanel::OnRefreshSelectionClicked()
{
	UE_LOG(LogVertexMaskForge, Log, TEXT("Refresh Selection clicked."));

	if (SelectionStatusText.IsValid())
	{
		SelectionStatusText->SetText(LOCTEXT("NoSelection", "No mesh selected."));
	}

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
