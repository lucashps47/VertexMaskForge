#include "SVertexMaskForgePanel.h"

#include "AssetRegistry/AssetData.h"
#include "Components/StaticMeshComponent.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "IContentBrowserSingleton.h"
#include "Logging/LogMacros.h"
#include "Modules/ModuleManager.h"
#include "Selection.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

DEFINE_LOG_CATEGORY_STATIC(LogVertexMaskForge, Log, All);

#define LOCTEXT_NAMESPACE "SVertexMaskForgePanel"

namespace VertexMaskForgePanel
{
	static FText GetSourceLabel(const EVertexMaskForgeSelectionSource Sources)
	{
		const bool bViewport = EnumHasAnyFlags(Sources, EVertexMaskForgeSelectionSource::Viewport);
		const bool bContentBrowser = EnumHasAnyFlags(Sources, EVertexMaskForgeSelectionSource::ContentBrowser);

		if (bViewport && bContentBrowser)
		{
			return LOCTEXT("SourceBoth", "Viewport + Content Browser");
		}
		if (bViewport)
		{
			return LOCTEXT("SourceViewport", "Viewport");
		}
		if (bContentBrowser)
		{
			return LOCTEXT("SourceContentBrowser", "Content Browser");
		}
		return FText::GetEmpty();
	}

	/** Adds a mesh to the collected list, or merges its source flags if already present. */
	static void AddOrUpdateSelectedMesh(
		TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes,
		TMap<FString, int32>& InOutPathToIndex,
		const FString& AssetPathString,
		const FString& AssetName,
		const TSoftObjectPtr<UStaticMesh>& SoftMesh,
		const EVertexMaskForgeSelectionSource Source)
	{
		if (const int32* ExistingIndex = InOutPathToIndex.Find(AssetPathString))
		{
			InOutMeshes[*ExistingIndex]->Sources |= Source;
			return;
		}

		TSharedPtr<FVertexMaskForgeSelectedMesh> NewEntry = MakeShared<FVertexMaskForgeSelectedMesh>();
		NewEntry->Mesh = SoftMesh;
		NewEntry->AssetName = AssetName;
		NewEntry->AssetPathString = AssetPathString;
		NewEntry->Sources = Source;

		InOutPathToIndex.Add(AssetPathString, InOutMeshes.Num());
		InOutMeshes.Add(MoveTemp(NewEntry));
	}
}

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
			.Padding(FMargin(0.f, 0.f, 0.f, 8.f))
			[
				SAssignNew(SummaryText, STextBlock)
				.Text(this, &SVertexMaskForgePanel::GetSummaryText)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			.Padding(FMargin(0.f, 0.f, 0.f, 8.f))
			[
				SNew(SBox)
				.MinDesiredHeight(160.f)
				[
					SNew(SOverlay)

					+ SOverlay::Slot()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("EmptyState", "No Static Meshes selected"))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Visibility(this, &SVertexMaskForgePanel::GetEmptyStateVisibility)
					]

					+ SOverlay::Slot()
					[
						SAssignNew(ListView, SListView<TSharedPtr<FVertexMaskForgeSelectedMesh>>)
						.ListItemsSource(&SelectedMeshes)
						.SelectionMode(ESelectionMode::None)
						.OnGenerateRow(this, &SVertexMaskForgePanel::OnGenerateMeshRow)
						.Visibility(this, &SVertexMaskForgePanel::GetListVisibility)
					]
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Left)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("RefreshSelection", "Refresh Selection"))
					.OnClicked(this, &SVertexMaskForgePanel::OnRefreshSelectionClicked)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(8.f, 0.f, 0.f, 0.f))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SelectionRefreshed", "Selection refreshed"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Visibility(this, &SVertexMaskForgePanel::GetRefreshedMessageVisibility)
				]
			]
		]
	];

	RefreshSelection();
}

FReply SVertexMaskForgePanel::OnRefreshSelectionClicked()
{
	RefreshSelection();
	return FReply::Handled();
}

void SVertexMaskForgePanel::RefreshSelection()
{
	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>> NewSelection;
	TMap<FString, int32> PathToIndex;

	CollectViewportSelection(NewSelection, PathToIndex);
	CollectContentBrowserSelection(NewSelection, PathToIndex);

	SelectedMeshes = MoveTemp(NewSelection);

	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}

	bHasRefreshedOnce = true;

	UE_LOG(LogVertexMaskForge, Log, TEXT("Refreshed selection: %d unique Static Mesh asset(s)"), SelectedMeshes.Num());
}

void SVertexMaskForgePanel::CollectViewportSelection(
	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes,
	TMap<FString, int32>& InOutPathToIndex) const
{
	if (!GEditor)
	{
		return;
	}

	USelection* SelectedActors = GEditor->GetSelectedActors();
	if (!SelectedActors)
	{
		return;
	}

	TArray<AActor*> Actors;
	SelectedActors->GetSelectedObjects<AActor>(Actors);

	TArray<UStaticMeshComponent*> Components;

	for (AActor* Actor : Actors)
	{
		if (!IsValid(Actor))
		{
			continue;
		}

		Components.Reset();
		Actor->GetComponents<UStaticMeshComponent>(Components);

		for (const UStaticMeshComponent* Component : Components)
		{
			if (!IsValid(Component))
			{
				continue;
			}

			UStaticMesh* Mesh = Component->GetStaticMesh();
			if (!IsValid(Mesh))
			{
				continue;
			}

			VertexMaskForgePanel::AddOrUpdateSelectedMesh(
				InOutMeshes,
				InOutPathToIndex,
				FSoftObjectPath(Mesh).ToString(),
				Mesh->GetName(),
				TSoftObjectPtr<UStaticMesh>(Mesh),
				EVertexMaskForgeSelectionSource::Viewport);
		}
	}
}

void SVertexMaskForgePanel::CollectContentBrowserSelection(
	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes,
	TMap<FString, int32>& InOutPathToIndex) const
{
	FContentBrowserModule& ContentBrowserModule =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

	TArray<FAssetData> SelectedAssets;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

	const FTopLevelAssetPath StaticMeshClassPath = UStaticMesh::StaticClass()->GetClassPathName();

	for (const FAssetData& AssetData : SelectedAssets)
	{
		if (!AssetData.IsValid())
		{
			continue;
		}

		if (AssetData.AssetClassPath != StaticMeshClassPath)
		{
			continue;
		}

		const TSoftObjectPtr<UStaticMesh> SoftMesh(AssetData.GetSoftObjectPath());

		VertexMaskForgePanel::AddOrUpdateSelectedMesh(
			InOutMeshes,
			InOutPathToIndex,
			AssetData.GetSoftObjectPath().ToString(),
			AssetData.AssetName.ToString(),
			SoftMesh,
			EVertexMaskForgeSelectionSource::ContentBrowser);
	}
}

TSharedRef<ITableRow> SVertexMaskForgePanel::OnGenerateMeshRow(
	TSharedPtr<FVertexMaskForgeSelectedMesh> InItem,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	const FText SourceLabel = InItem.IsValid()
		? VertexMaskForgePanel::GetSourceLabel(InItem->Sources)
		: FText::GetEmpty();

	return SNew(STableRow<TSharedPtr<FVertexMaskForgeSelectedMesh>>, OwnerTable)
		.Padding(FMargin(4.f, 3.f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(InItem.IsValid() ? FText::FromString(InItem->AssetName) : FText::GetEmpty())
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(8.f, 0.f, 0.f, 0.f))
				[
					SNew(STextBlock)
					.Text(SourceLabel)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(InItem.IsValid() ? FText::FromString(InItem->AssetPathString) : FText::GetEmpty())
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
			]
		];
}

FText SVertexMaskForgePanel::GetSummaryText() const
{
	return FText::Format(LOCTEXT("SummaryFormat", "Selected Static Meshes: {0}"), FText::AsNumber(SelectedMeshes.Num()));
}

EVisibility SVertexMaskForgePanel::GetEmptyStateVisibility() const
{
	return SelectedMeshes.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SVertexMaskForgePanel::GetListVisibility() const
{
	return SelectedMeshes.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SVertexMaskForgePanel::GetRefreshedMessageVisibility() const
{
	return bHasRefreshedOnce ? EVisibility::Visible : EVisibility::Collapsed;
}

#undef LOCTEXT_NAMESPACE
