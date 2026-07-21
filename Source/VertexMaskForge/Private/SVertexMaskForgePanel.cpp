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
#include "StaticMeshResources.h"
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

	/**
	 * Reads LOD 0 / material / Nanite / CPU access diagnostics from a Static Mesh's
	 * render data. Read-only: never touches SourceModel, MeshDescription, or RenderData.
	 * Safe to call with a null or not-yet-built mesh.
	 */
	static FVertexMaskForgeMeshDiagnostics InspectStaticMesh(const UStaticMesh* Mesh)
	{
		FVertexMaskForgeMeshDiagnostics Diagnostics;

		if (!IsValid(Mesh))
		{
			return Diagnostics;
		}

		Diagnostics.NumLODs = Mesh->GetNumLODs();
		Diagnostics.NumMaterialSlots = Mesh->GetStaticMaterials().Num();
		Diagnostics.bAllowCPUAccess = Mesh->bAllowCPUAccess != 0;
		Diagnostics.bNaniteEnabled = Mesh->HasValidNaniteData();

		if (!Mesh->HasValidRenderData(/*bCheckLODForVerts=*/true, /*LODIndex=*/0))
		{
			return Diagnostics;
		}

		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
		{
			return Diagnostics;
		}

		const FStaticMeshLODResources& LOD0 = RenderData->LODResources[0];

		Diagnostics.LOD0NumVertices = LOD0.GetNumVertices();
		Diagnostics.LOD0NumTriangles = LOD0.GetNumTriangles();

		const int32 ColorVertexCount = static_cast<int32>(LOD0.VertexBuffers.ColorVertexBuffer.GetNumVertices());
		Diagnostics.LOD0NumColorVertices = ColorVertexCount;

		if (ColorVertexCount <= 0)
		{
			Diagnostics.VertexColorState = EVertexMaskForgeVertexColorState::None;
		}
		else if (ColorVertexCount == Diagnostics.LOD0NumVertices)
		{
			Diagnostics.VertexColorState = EVertexMaskForgeVertexColorState::Present;
		}
		else
		{
			Diagnostics.VertexColorState = EVertexMaskForgeVertexColorState::PartialOrInvalid;
		}

		Diagnostics.bValid = true;

		return Diagnostics;
	}

	static FText GetVertexColorStateLabel(const EVertexMaskForgeVertexColorState State)
	{
		switch (State)
		{
		case EVertexMaskForgeVertexColorState::Present:
			return LOCTEXT("VertexColorPresent", "Present");
		case EVertexMaskForgeVertexColorState::PartialOrInvalid:
			return LOCTEXT("VertexColorPartial", "Partial/Invalid");
		case EVertexMaskForgeVertexColorState::None:
		default:
			return LOCTEXT("VertexColorNone", "None");
		}
	}

	static FText GetEnabledDisabledLabel(const bool bEnabled)
	{
		return bEnabled ? LOCTEXT("StateEnabled", "Enabled") : LOCTEXT("StateDisabled", "Disabled");
	}

	/** Builds the compact diagnostics line shown under each mesh row. */
	static FText GetDiagnosticsSummaryText(const FVertexMaskForgeMeshDiagnostics& Diagnostics)
	{
		if (!Diagnostics.bValid)
		{
			return LOCTEXT("DiagnosticsUnavailable", "Render data unavailable");
		}

		return FText::Format(
			LOCTEXT("DiagnosticsFormat",
				"LODs: {0}   LOD 0 Vertices: {1}   LOD 0 Triangles: {2}   Material Slots: {3}   Vertex Colors: {4} ({5} / {6})   Nanite: {7}   Allow CPU Access: {8}"),
			FText::AsNumber(Diagnostics.NumLODs),
			FText::AsNumber(Diagnostics.LOD0NumVertices),
			FText::AsNumber(Diagnostics.LOD0NumTriangles),
			FText::AsNumber(Diagnostics.NumMaterialSlots),
			GetVertexColorStateLabel(Diagnostics.VertexColorState),
			FText::AsNumber(Diagnostics.LOD0NumColorVertices),
			FText::AsNumber(Diagnostics.LOD0NumVertices),
			GetEnabledDisabledLabel(Diagnostics.bNaniteEnabled),
			GetEnabledDisabledLabel(Diagnostics.bAllowCPUAccess));
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
	UpdateMeshDiagnostics(NewSelection);

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

void SVertexMaskForgePanel::UpdateMeshDiagnostics(TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes) const
{
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : InOutMeshes)
	{
		if (!Entry.IsValid())
		{
			continue;
		}

		// Resolved only for the duration of this refresh; no raw pointer is stored on Entry.
		const UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
		Entry->Diagnostics = VertexMaskForgePanel::InspectStaticMesh(Mesh);
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

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				.Text(InItem.IsValid()
					? VertexMaskForgePanel::GetDiagnosticsSummaryText(InItem->Diagnostics)
					: FText::GetEmpty())
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.AutoWrapText(true)
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
