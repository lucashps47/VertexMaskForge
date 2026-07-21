#pragma once

#include "CoreMinimal.h"
#include "Misc/EnumClassFlags.h"
#include "UObject/SoftObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

class ITableRow;
class STableViewBase;
class STextBlock;
class UStaticMesh;
template <typename ItemType> class SListView;

/** Where a selected Static Mesh was found during a selection refresh. */
enum class EVertexMaskForgeSelectionSource : uint8
{
	None = 0,
	Viewport = 1 << 0,
	ContentBrowser = 1 << 1,
};
ENUM_CLASS_FLAGS(EVertexMaskForgeSelectionSource)

/**
 * One unique Static Mesh found in the current selection.
 * Kept small and self-contained for this checkpoint; safe to relocate to a
 * shared header once processing is introduced.
 */
struct FVertexMaskForgeSelectedMesh
{
	/** Editor-safe soft reference to the asset. Does not force it to stay loaded. */
	TSoftObjectPtr<UStaticMesh> Mesh;

	/** Cached display name, so rows do not need to resolve the soft pointer. */
	FString AssetName;

	/** Cached full asset path string, used both for display and as the dedup key. */
	FString AssetPathString;

	EVertexMaskForgeSelectionSource Sources = EVertexMaskForgeSelectionSource::None;
};

class SVertexMaskForgePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVertexMaskForgePanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FReply OnRefreshSelectionClicked();

	/** Re-queries the viewport and Content Browser selections and rebuilds the list. */
	void RefreshSelection();

	/** Gathers unique Static Meshes from UStaticMeshComponents on selected actors. */
	void CollectViewportSelection(
		TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes,
		TMap<FString, int32>& InOutPathToIndex) const;

	/** Gathers Static Mesh assets selected in the Content Browser. */
	void CollectContentBrowserSelection(
		TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes,
		TMap<FString, int32>& InOutPathToIndex) const;

	TSharedRef<ITableRow> OnGenerateMeshRow(
		TSharedPtr<FVertexMaskForgeSelectedMesh> InItem,
		const TSharedRef<STableViewBase>& OwnerTable);

	FText GetSummaryText() const;
	EVisibility GetEmptyStateVisibility() const;
	EVisibility GetListVisibility() const;
	EVisibility GetRefreshedMessageVisibility() const;

	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>> SelectedMeshes;

	TSharedPtr<SListView<TSharedPtr<FVertexMaskForgeSelectedMesh>>> ListView;
	TSharedPtr<STextBlock> SummaryText;

	bool bHasRefreshedOnce = false;
};
