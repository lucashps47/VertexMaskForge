#pragma once

#include "Containers/BitArray.h"
#include "CoreMinimal.h"
#include "Math/Vector4.h"
#include "Misc/EnumClassFlags.h"
#include "UObject/SoftObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

class ITableRow;
class STableViewBase;
class STextBlock;
class UStaticMesh;
template <typename ItemType> class SListView;
enum class ECheckBoxState : uint8;

namespace UE::Geometry { class FDynamicMesh3; }

/** Where a selected Static Mesh was found during a selection refresh. */
enum class EVertexMaskForgeSelectionSource : uint8
{
	None = 0,
	Viewport = 1 << 0,
	ContentBrowser = 1 << 1,
};
ENUM_CLASS_FLAGS(EVertexMaskForgeSelectionSource)

/** Coverage state of the LOD 0 Color Vertex Buffer. */
enum class EVertexMaskForgeVertexColorState : uint8
{
	/** No Color Vertex Buffer, or it has zero entries. */
	None,

	/** Color Vertex Buffer present and its vertex count matches LOD 0's vertex count. */
	Present,

	/** Color Vertex Buffer present but its vertex count does not match LOD 0's vertex count. */
	PartialOrInvalid,
};

/**
 * Read-only technical diagnostics for a single Static Mesh, gathered from its
 * render data. Computed once per refresh; never mutates the mesh.
 */
struct FVertexMaskForgeMeshDiagnostics
{
	/** True once the diagnostics below were successfully computed from valid render data. */
	bool bValid = false;

	int32 NumLODs = 0;
	int32 LOD0NumVertices = 0;
	int32 LOD0NumTriangles = 0;
	int32 NumMaterialSlots = 0;

	EVertexMaskForgeVertexColorState VertexColorState = EVertexMaskForgeVertexColorState::None;
	int32 LOD0NumColorVertices = 0;

	bool bNaniteEnabled = false;
	bool bAllowCPUAccess = false;
};

/** Result of attempting to build a transient working copy of a mesh's LOD 0. */
enum class EVertexMaskForgeWorkingMeshState : uint8
{
	/** The working mesh was built and is safe to use. */
	Ready,

	/** UStaticMesh::GetMeshDescription(0) returned null. */
	SourceMeshDescriptionUnavailable,

	/** A MeshDescription was obtained, but conversion to FDynamicMesh3 did not produce a usable mesh. */
	ConversionFailed,

	/** The soft-referenced Static Mesh could not be resolved to a valid object. */
	InvalidSource,
};

/** Whether the Dynamic Mesh's per-triangle Material ID attribute mirrors the source's polygon groups. */
enum class EVertexMaskForgeMaterialIDState : uint8
{
	/** The working mesh has no Material ID attribute (only possible if the working copy is not Ready). */
	Unavailable,

	/** The working mesh has no Material ID attribute despite being Ready. */
	Missing,

	/** The working mesh has a Material ID attribute carried over from the source's polygon groups. */
	Preserved,
};

/**
 * Presence state of the working mesh's primary color overlay.
 * Ground truth for Present/Missing/Invalid comes from the LOD 0 RenderData diagnostic
 * (FVertexMaskForgeMeshDiagnostics::VertexColorState), not from whether the converter happened to
 * keep its overlay -- FMeshDescriptionToDynamicMesh::Convert() drops the overlay whenever every
 * source color exactly equals the attribute default (white), which must NOT be conflated with a
 * genuinely colorless source. See ValidateWorkingMesh() / ReconstructOmittedColorOverlay().
 */
enum class EVertexMaskForgeWorkingVertexColorState : uint8
{
	/** No color overlay (only possible if the working copy is not Ready). */
	Unavailable,

	/** RenderData has no Color Vertex Buffer at all: the source genuinely has no vertex colors. */
	Missing,

	/** RenderData's Color Vertex Buffer count does not match LOD 0's vertex count; not auto-corrected. */
	Invalid,

	/**
	 * RenderData proves a full Color Vertex Buffer exists. Elements may all be white -- that is
	 * still Present, including cases where the overlay had to be reconstructed because Convert()
	 * dropped it for being uniformly default-valued.
	 */
	Present,
};

/**
 * Aggregate statistics over a color overlay's elements. Never stores individual colors.
 * Tolerance-based comparisons use FVertexMaskForgeWorkingMesh::ColorChannelTolerance.
 */
struct FVertexMaskForgeColorStats
{
	int32 NumElements = 0;

	FVector4f MinColor = FVector4f(1.f, 1.f, 1.f, 1.f);
	FVector4f MaxColor = FVector4f(0.f, 0.f, 0.f, 0.f);

	/** Elements whose RGB differs from white (1,1,1) by more than the tolerance, on any channel. */
	int32 NumNonWhite = 0;

	/** Elements whose RGB differs from black (0,0,0) by more than the tolerance, on any channel. */
	int32 NumNonBlack = 0;
};

/** Result of attempting to generate a scalar mask on a working mesh. */
enum class EVertexMaskForgeScalarMaskState : uint8
{
	/** No generation has been attempted yet, or a prior result was invalidated. */
	NotGenerated,

	/** Generation succeeded and the values below are safe to use. */
	Ready,

	/** No valid working mesh (or no vertices) was available to generate from. */
	Unavailable,

	/** The mesh's local-space extent along the mask's axis is too small to normalize safely. */
	DegenerateBounds,

	/** Generation ran but produced values that failed validation (non-finite, out of [0,1], or a
	 *  valid-value count mismatch). Surfaced rather than hidden; never auto-corrected. */
	Invalid,
};

/**
 * A transient per-vertex scalar mask (e.g. the Bounding Box Z prototype), owned by exactly one
 * FVertexMaskForgeWorkingMesh. Exists only in memory; never written to the Primary Color Overlay,
 * MeshDescription, RenderData, or the source asset.
 *
 * FDynamicMesh3 Vertex IDs are not necessarily compact (VertexCount() can be less than
 * MaxVertexID()). Values/bHasValue are therefore sized to MaxVertexID() and only written at
 * indices that were valid Vertex IDs at generation time -- use TryGetValue() rather than indexing
 * Values directly, since unwritten slots are meaningless, not zero.
 */
struct FVertexMaskForgeScalarMask
{
	/** Small explicit tolerance for the Near Zero / Near One counters, matching color-diagnostic precedent. */
	static constexpr float Tolerance = 1.0f / 255.0f;

	EVertexMaskForgeScalarMaskState State = EVertexMaskForgeScalarMaskState::NotGenerated;

	/** Indexed directly by Dynamic Mesh Vertex ID; sized to MaxVertexID() when State == Ready. */
	TArray<float> Values;

	/** Parallel to Values: true only at indices that are valid, written Vertex IDs. */
	TBitArray<> bHasValue;

	/** Safe accessor: returns false (and leaves OutValue untouched) for any ID not actually stored. */
	bool TryGetValue(int32 VertexID, float& OutValue) const
	{
		if (!bHasValue.IsValidIndex(VertexID) || !bHasValue[VertexID])
		{
			return false;
		}
		OutValue = Values[VertexID];
		return true;
	}

	int32 NumValidValues = 0;
	float MinValue = 0.f;
	float MaxValue = 0.f;
	float MeanValue = 0.f;

	/** Values within Tolerance of 0.0 / 1.0, respectively. */
	int32 NumNearZero = 0;
	int32 NumNearOne = 0;

	/** Value at the vertex with the lowest local-space Z (Bottom). */
	float BottomValue = 0.f;

	/** Value at the vertex with the highest local-space Z (Top). */
	float TopValue = 0.f;

	/** Parameters used to produce this result, kept for diagnostic display. */
	float Position = 0.5f;
	float TransitionWidth = 1.0f;
	bool bInvert = false;
};

/**
 * A transient, independent working copy of a Static Mesh's LOD 0, used as the basis for future
 * mask generators. Never written back to the source asset. Owns its FDynamicMesh3 by pointer so
 * the (relatively heavy) mesh data is never copied when this struct is moved.
 */
struct FVertexMaskForgeWorkingMesh
{
	/** Small explicit tolerance used for the non-white / non-black element counts, in 0-1 color space. */
	static constexpr float ColorChannelTolerance = 1.0f / 255.0f;

	FVertexMaskForgeWorkingMesh() = default;

	/**
	 * Declared here, defined in the .cpp (after DynamicMesh3.h is included) so that
	 * TUniquePtr<FDynamicMesh3>'s destructor is only instantiated where FDynamicMesh3 is a
	 * complete type. This struct owns that lifetime itself; callers must not rely on some other
	 * class's destructor to make this safe.
	 */
	~FVertexMaskForgeWorkingMesh();

	/** Exclusive ownership of the FDynamicMesh3 via TUniquePtr; copying would be ambiguous/unsafe. */
	FVertexMaskForgeWorkingMesh(const FVertexMaskForgeWorkingMesh&) = delete;
	FVertexMaskForgeWorkingMesh& operator=(const FVertexMaskForgeWorkingMesh&) = delete;

	/** Declared here, defined in the .cpp for the same reason as the destructor. */
	FVertexMaskForgeWorkingMesh(FVertexMaskForgeWorkingMesh&&);
	FVertexMaskForgeWorkingMesh& operator=(FVertexMaskForgeWorkingMesh&&);

	EVertexMaskForgeWorkingMeshState State = EVertexMaskForgeWorkingMeshState::InvalidSource;

	/** Null unless State == Ready. Explicit, exclusive ownership; never shared or persisted elsewhere. */
	TUniquePtr<UE::Geometry::FDynamicMesh3> Mesh;

	int32 DynamicVertexCount = 0;
	int32 DynamicTriangleCount = 0;

	/** MeshDescription triangle count, for comparison against DynamicTriangleCount. */
	int32 SourceTriangleCount = 0;

	/** Max(0, SourceTriangleCount - DynamicTriangleCount): triangles dropped or welded away by conversion. */
	int32 DiscardedTriangleCount = 0;

	/** Result of FDynamicMesh3::CheckValidity() with permissive options, in ReturnOnly (non-asserting) mode. */
	bool bTopologyValid = false;

	EVertexMaskForgeMaterialIDState MaterialIDState = EVertexMaskForgeMaterialIDState::Unavailable;
	int32 DistinctMaterialIDCount = 0;

	/**
	 * True if every triangle's Material ID falls within [0, NumMaterialSlots) of the source mesh.
	 * Note: bUseCompactedPolygonGroupIDValues only compacts Polygon Group IDs into contiguous
	 * Section Indices -- it does not by itself prove a Section Index equals the correct Material
	 * Slot index on assets with manually remapped sections. This flag only catches out-of-range
	 * IDs, not silent slot mismatches within range; IDs are never remapped or corrected here.
	 */
	bool bMaterialIDsInRange = true;

	EVertexMaskForgeWorkingVertexColorState VertexColorState = EVertexMaskForgeWorkingVertexColorState::Unavailable;
	FVertexMaskForgeColorStats ColorStats;

	/**
	 * The Bounding Box Z mask prototype, if generated. A fresh FVertexMaskForgeWorkingMesh is
	 * always constructed on Refresh Selection (see BuildWorkingMeshForStaticMesh), so this starts
	 * at NotGenerated automatically every time the working mesh itself is rebuilt -- there is no
	 * separate invalidation step needed for that case. Parameter-change invalidation is handled by
	 * the panel explicitly resetting this field.
	 */
	FVertexMaskForgeScalarMask BoundingBoxZMask;
};

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

	FVertexMaskForgeMeshDiagnostics Diagnostics;

	FVertexMaskForgeWorkingMesh WorkingMesh;
};

class SVertexMaskForgePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVertexMaskForgePanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Declared here and defined in the .cpp so FDynamicMesh3 only needs to be complete there. */
	virtual ~SVertexMaskForgePanel() override;

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

	/**
	 * Resolves each entry's Static Mesh and computes its diagnostics.
	 * Soft pointers are only resolved here, for the duration of the refresh;
	 * no raw pointer is kept afterwards.
	 */
	void UpdateMeshDiagnostics(TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes) const;

	/**
	 * Builds a transient, independent working copy (FDynamicMesh3) of each entry's LOD 0 and
	 * validates it. The source Static Mesh is only resolved for the duration of this call, and
	 * the source FMeshDescription copy does not outlive it either; only the resulting working
	 * mesh and its computed statistics are kept on the entry.
	 */
	void BuildWorkingMeshes(TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes) const;

	TSharedRef<ITableRow> OnGenerateMeshRow(
		TSharedPtr<FVertexMaskForgeSelectedMesh> InItem,
		const TSharedRef<STableViewBase>& OwnerTable);

	FText GetSummaryText() const;
	EVisibility GetEmptyStateVisibility() const;
	EVisibility GetListVisibility() const;
	EVisibility GetRefreshedMessageVisibility() const;

	// --- Bounding Box Z Mask prototype ------------------------------------------------------

	/** Processes every selected entry's working mesh, generating or clearing its Bounding Box Z mask. */
	FReply OnGenerateBoundingBoxMaskClicked();

	float GetBoundingBoxMaskPosition() const { return BoundingBoxMaskPosition; }
	void OnBoundingBoxMaskPositionChanged(float NewValue);

	float GetBoundingBoxMaskTransitionWidth() const { return BoundingBoxMaskTransitionWidth; }
	void OnBoundingBoxMaskTransitionWidthChanged(float NewValue);

	ECheckBoxState GetBoundingBoxMaskInvertState() const;
	void OnBoundingBoxMaskInvertChanged(ECheckBoxState NewState);

	/**
	 * Resets every selected entry's Bounding Box Z mask back to NotGenerated, without touching the
	 * working mesh (FDynamicMesh3) itself. Called whenever Position/Transition Width/Invert change,
	 * so stale statistics are never left looking current; the user must click Generate Mask again.
	 */
	void InvalidateBoundingBoxMasks();

	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>> SelectedMeshes;

	TSharedPtr<SListView<TSharedPtr<FVertexMaskForgeSelectedMesh>>> ListView;
	TSharedPtr<STextBlock> SummaryText;

	bool bHasRefreshedOnce = false;

	// Bottom-to-Top Local Z prototype parameters; defaults reproduce the raw normalized gradient
	// (Bottom = 0, Top = 1) -- see VertexMaskForgePanel::GenerateBoundingBoxZMask() for the formula.
	float BoundingBoxMaskPosition = 0.5f;
	float BoundingBoxMaskTransitionWidth = 1.0f;
	bool bBoundingBoxMaskInvert = false;
};
