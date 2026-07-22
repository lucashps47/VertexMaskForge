#pragma once

#include "Containers/BitArray.h"
#include "CoreMinimal.h"
#include "Delegates/IDelegateInstance.h"
#include "Math/Vector4.h"
#include "Misc/EnumClassFlags.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

class UWorld;

class ITableRow;
class STableViewBase;
class STextBlock;
class AActor;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;
template <typename ItemType> class SListView;
template <typename OptionType> class SComboBox;
enum class ECheckBoxState : uint8;
namespace ESelectInfo { enum Type : int; }

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
 * AUDITED: the Bounding Box Z mask (the only generator that exists so far) is computed directly in
 * RENDER VERTEX order (see VertexMaskForgePanel::GenerateBoundingBoxZMask), so for it, Values/
 * bHasValue are indexed by Render Vertex Index and are always dense (Values.Num() ==
 * bHasValue.Num() == RenderVertexCount == the LOD's PositionVertexBuffer.GetNumVertices(), every
 * slot written). This struct itself stays domain-agnostic (a future, genuinely topology-dependent
 * generator could still index by Dynamic Mesh Vertex ID and leave bHasValue sparse) -- always use
 * TryGetValue() rather than indexing Values directly, since an unwritten slot is meaningless, not
 * zero, whichever domain produced it.
 */
struct FVertexMaskForgeScalarMask
{
	/** Small explicit tolerance for the Near Zero / Near One counters, matching color-diagnostic precedent. */
	static constexpr float Tolerance = 1.0f / 255.0f;

	EVertexMaskForgeScalarMaskState State = EVertexMaskForgeScalarMaskState::NotGenerated;

	/** Indexed by Render Vertex Index for the Bounding Box Z mask (see the struct-level audit note). */
	TArray<float> Values;

	/** Parallel to Values: true only at indices that were actually written. */
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

	// --- Temporary diagnostics (audited render-vertex-order fix) ---------------------------
	// Added to make the Values.Num() == PositionVertexBuffer.GetNumVertices() invariant directly
	// verifiable from the panel UI. See VertexMaskForgePanel::GetBoundingBoxMaskSummaryText().

	/** LOD0 PositionVertexBuffer.GetNumVertices() at generation time; must equal Values.Num(). */
	int32 RenderVertexCount = 0;

	/** Local-space Z bounds used for normalization, over ALL render vertices of the LOD. */
	double BoundsMinZ = 0.0;
	double BoundsMaxZ = 0.0;
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
 * What the artist currently sees in the viewport for Vertex Color preview purposes.
 * Purely a session/panel-level setting -- never saved on the UStaticMesh, never affects the
 * Bounding Box Z mask, Position, Transition Width, or Invert.
 */
enum class EVertexMaskForgePreviewMode : uint8
{
	/** Restore the mesh's own materials; no debug visualization is shown. */
	OriginalMaterial,

	/** Show the temporarily composed RGBA color (mask blended into the Channel Filter's channels). */
	RGBVertexColor,

	/** Show the composed color's Red channel as grayscale (R, R, R, 1). */
	RedChannel,

	/** Show the composed color's Green channel as grayscale (G, G, G, 1). */
	GreenChannel,

	/** Show the composed color's Blue channel as grayscale (B, B, B, 1). */
	BlueChannel,
};

/**
 * Centralized per-Actor "temporarily hidden in editor" ownership. Hiding is necessarily Actor-level
 * (UE 5.8 has no transient-safe component-level visibility flag -- see the audit note on
 * ActivatePreviewForComponent), so when two or more previewed components belong to the SAME Actor
 * (either two entries in one FVertexMaskForgeSelectedMesh::PreviewComponents, or components from
 * two different FVertexMaskForgeSelectedMesh entries), naively snapshotting/restoring
 * IsTemporarilyHiddenInEditor() per-component would let one component's cleanup un-hide an Actor
 * that another still-active preview depends on staying hidden. This struct instead records the
 * Actor's original flag value once (on first acquire) and a reference count of how many active
 * previews currently depend on the Actor being hidden; only the release that brings the count to
 * zero actually restores the original value. Keyed externally by TWeakObjectPtr<AActor> in
 * SVertexMaskForgePanel::ActorHideStates.
 */
struct FVertexMaskForgeActorHideState
{
	bool bOriginalHiddenInEditor = false;
	int32 RefCount = 0;
};

/**
 * Non-destructive preview state for one selected UStaticMeshComponent.
 *
 * IMPORTANT (audited): UStaticMeshComponent::Serialize() unconditionally re-serializes LODData
 * (which holds OverrideVertexColors and PaintedVertices) via a custom `Ar << LODData`, regardless
 * of LODData's own `UPROPERTY(transient)` tag, and OverrideMaterials is an ordinary (non-transient)
 * UPROPERTY. This means a Save/Save All that occurs while a preview is active WOULD persist any
 * of those properties if they were written directly onto the real, selected component -- even
 * though the plugin never calls Modify()/MarkPackageDirty() itself, a package that is already
 * dirty (or being autosaved) can still be written to disk mid-preview. For that reason this struct
 * never mutates the selected component's own OverrideMaterials/OverrideVertexColors/PaintedVertices
 * at all: it tracks a separate, transient, RF_Transient duplicate component instead (see
 * EnsurePreviewComponent()), so there is nothing on the real component for a Save to persist.
 */
struct FVertexMaskForgePreviewComponentState
{
	/** The real, selected component. Read-only source for mesh/transform; never mutated. */
	TWeakObjectPtr<UStaticMeshComponent> SourceComponent;

	/**
	 * Transient duplicate created only while preview is active, destroyed on cleanup. Outer is
	 * GetTransientPackage() and it is never added to SourceComponent's owning Actor's component
	 * list, so no Actor/level serialization path can ever discover or write it, independent of
	 * RF_Transient. Overridden colors/materials live ONLY on this object.
	 *
	 * IMPORTANT (audited): RF_Transient only excludes an object from serialization; it does NOT by
	 * itself keep the object reachable for Garbage Collection. TStrongObjectPtr is UE's supported
	 * scoped-root mechanism for exactly this "keep a dynamically created UObject alive outside the
	 * normal owning-UPROPERTY graph" case (see UObject/StrongObjectPtr.h). It is released via
	 * .Reset() in RestoreComponentOriginal, so unlike AddToRoot() it never leaks a permanent root
	 * reference -- a Collect Garbage while preview is active cannot collect this object.
	 */
	TStrongObjectPtr<UStaticMeshComponent> PreviewComponent;

	/** True while PreviewComponent is active. */
	bool bOverrideActive = false;

	/**
	 * True while this State currently holds one reference-count token in
	 * SVertexMaskForgePanel::ActorHideStates for HiddenOwner. Tracked independently of
	 * SourceComponent's validity so a release can still happen correctly even if SourceComponent (or
	 * its owning Actor) is destroyed while preview is active -- see RestoreComponentOriginal.
	 */
	bool bHasAcquiredActorHide = false;

	/** The Actor this State acquired a hide token for, captured at acquire time. */
	TWeakObjectPtr<AActor> HiddenOwner;
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

	/**
	 * Static Mesh Components in the tracked viewport selection that reference this asset, with
	 * their non-destructive preview state. Populated only by CollectViewportSelection; empty for
	 * entries that came only from the Content Browser (Preview is unavailable for those -- see
	 * SVertexMaskForgePanel::ApplyPreviewToEntry).
	 */
	TArray<FVertexMaskForgePreviewComponentState> PreviewComponents;
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

	// --- Preview (Preview Mode + Channel Filter) --------------------------------------------

	TSharedRef<SWidget> OnGeneratePreviewModeRow(TSharedPtr<EVertexMaskForgePreviewMode> InOption) const;
	void OnPreviewModeSelectionChanged(TSharedPtr<EVertexMaskForgePreviewMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetPreviewModeButtonText() const;

	ECheckBoxState GetChannelFilterRState() const { return bChannelFilterR ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnChannelFilterRChanged(ECheckBoxState NewState);
	ECheckBoxState GetChannelFilterGState() const { return bChannelFilterG ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnChannelFilterGChanged(ECheckBoxState NewState);
	ECheckBoxState GetChannelFilterBState() const { return bChannelFilterB ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnChannelFilterBChanged(ECheckBoxState NewState);
	ECheckBoxState GetChannelFilterAState() const { return bChannelFilterA ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnChannelFilterAChanged(ECheckBoxState NewState);

	FText GetPreviewStatusText() const;

	/**
	 * Applies or restores preview visualization for every selected entry, based on the current
	 * CurrentPreviewMode / Channel Filter / each entry's BoundingBoxZMask state. Idempotent and
	 * side-effect-free with respect to the mask itself -- never generates or invalidates it.
	 */
	void UpdateAllPreviews();

	/** Applies or restores preview for one entry, per current mode/filter/mask state. */
	void ApplyPreviewToEntry(const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry);

	/** Restores original materials and vertex colors on every tracked component of one entry. Idempotent. */
	void RestorePreviewForEntry(FVertexMaskForgeSelectedMesh& Entry);

	/**
	 * Restores every currently-tracked entry's preview components. Called before Refresh Selection
	 * replaces SelectedMeshes, and from the destructor, so no transient override ever outlives the
	 * entries or the panel.
	 */
	void DestroyAllPreviews();

	/** Lazily loads the engine's built-in vertex-color debug material (never a new persistent asset). */
	UMaterialInterface* GetPreviewDebugMaterial();

	/**
	 * Bound to FWorldDelegates::OnWorldCleanup (registered once in Construct(), removed in the
	 * destructor via WorldCleanupDelegateHandle). Fires at the very start of UWorld::CleanupWorld()
	 * -- e.g. on level change/reload, or PIE end -- while World and its actors/components are still
	 * fully valid, and BEFORE UWorld::ClearWorldComponents() or any actor/component teardown. This is
	 * the correct point to synchronously restore/destroy every preview whose SourceComponent (or
	 * PreviewComponent, or hidden Actor) belongs to World, so this panel never keeps a
	 * TStrongObjectPtr (or an AActor::bHiddenEdTemporary override) referencing a World that is about
	 * to go away. Never calls RefreshSelection or recreates previews -- pure cleanup, safe to run
	 * during editor shutdown; does not assume GEditor is otherwise valid.
	 */
	void OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);

	FDelegateHandle WorldCleanupDelegateHandle;

	/**
	 * Centralized per-Actor hide ref-counting for the whole panel (not per-entry), since components
	 * from different FVertexMaskForgeSelectedMesh entries can share the same owning Actor. See
	 * FVertexMaskForgeActorHideState.
	 */
	TMap<TWeakObjectPtr<AActor>, FVertexMaskForgeActorHideState> ActorHideStates;

	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>> SelectedMeshes;

	TSharedPtr<SListView<TSharedPtr<FVertexMaskForgeSelectedMesh>>> ListView;
	TSharedPtr<STextBlock> SummaryText;

	bool bHasRefreshedOnce = false;

	// Preview Mode / Channel Filter are panel/session-transient: never saved on the asset, never
	// invalidate or regenerate BoundingBoxZMask, and are fully independent of one another.
	EVertexMaskForgePreviewMode CurrentPreviewMode = EVertexMaskForgePreviewMode::OriginalMaterial;
	TArray<TSharedPtr<EVertexMaskForgePreviewMode>> PreviewModeOptions;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgePreviewMode>>> PreviewModeComboBox;

	bool bChannelFilterR = true;
	bool bChannelFilterG = true;
	bool bChannelFilterB = true;
	bool bChannelFilterA = false;

	/** Resolved lazily via GetPreviewDebugMaterial(); weak because it is an asset the plugin does not own. */
	TWeakObjectPtr<UMaterialInterface> PreviewDebugMaterial;

	// Bottom-to-Top Local Z prototype parameters; defaults reproduce the raw normalized gradient
	// (Bottom = 0, Top = 1) -- see VertexMaskForgePanel::GenerateBoundingBoxZMask() for the formula.
	float BoundingBoxMaskPosition = 0.5f;
	float BoundingBoxMaskTransitionWidth = 1.0f;
	bool bBoundingBoxMaskInvert = false;
};
