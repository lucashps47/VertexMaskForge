#pragma once

#include "Containers/BitArray.h"
#include "Containers/StaticArray.h"
#include "CoreMinimal.h"
#include "Delegates/IDelegateInstance.h"
#include "Engine/TimerHandle.h"
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

/**
 * Explicit state of the Pending Changes workflow (Accept/Cancel). Never inferred merely from
 * whether a PreviewComponent exists -- see SVertexMaskForgePanel::RecomputeOperationState().
 */
enum class EVertexMaskForgeOperationState : uint8
{
	/** No valid, acceptable Preview exists (no active Preview Mode, or no Ready mask being shown). */
	Idle,

	/** A valid mask is displayed in the transient Preview and can be Accepted. */
	PendingChanges,

	/** Accept is currently writing to Static Mesh asset(s). Synchronous in this checkpoint, but
	 *  modeled explicitly so no other operation (auto-update, another Accept) can interleave. */
	Applying,

	/** The last Accept attempt was blocked or failed; the Preview that was attempted is preserved
	 *  unchanged. Cleared by the next explicit user action (regenerate, Accept, Cancel). */
	Failed,
};

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

/** Which generator produced a FVertexMaskForgeScalarMask -- purely for UI/diagnostic labeling. */
enum class EVertexMaskForgeScalarMaskSource : uint8
{
	/** GenerateBoundingBoxMask: combination (by maximum) of up to 3 independent axis gradients. */
	BoundingBox,

	/** GenerateConstantMask(1.0): every render vertex set to 1.0 (Fill White). */
	ConstantWhite,

	/** GenerateConstantMask(0.0): every render vertex set to 0.0 (Fill Black). */
	ConstantBlack,
};

/** Which local/world axis one FVertexMaskForgeAxisMaskParams evaluates. */
enum class EVertexMaskForgeBoundsAxis : uint8
{
	X,
	Y,
	Z,
};

/**
 * Independent parameters for ONE axis of the Bounding Box Mask (the "Local X" / "Local Y" /
 * "Local Z" rows in the UI). Panel-level (SVertexMaskForgePanel::BoundingBoxAxisParams, indexed by
 * EVertexMaskForgeBoundsAxis) -- shared across every selected entry, exactly like the single-axis
 * Position/TransitionWidth/bInvert panel members this replaces.
 */
struct FVertexMaskForgeAxisMaskParams
{
	bool bEnabled = false;
	float Position = 0.5f;
	float TransitionWidth = 1.0f;
	bool bInvert = false;

	/** Adds a second, symmetric gradient in the opposite direction, combined with the base gradient
	 *  by maximum (see VertexMaskForgePanel::GenerateBoundingBoxMask). Never alters Position or
	 *  TransitionWidth. */
	bool bMirror = false;

	/** False (default): evaluate this axis in the Static Mesh's own local space. True: evaluate in
	 *  World Space, using the specific previewed instance's own ComponentTransform. Independent per
	 *  axis and independent of Position/TransitionWidth/bInvert/bMirror. */
	bool bWorldSpace = false;
};

/**
 * Per-axis bounds, either INDIVIDUAL (one Static Mesh's own LOD0, in its own chosen space) or
 * COLLECTIVE (Unified Bounds -- the union across every participating component's render vertices,
 * in that axis's chosen space), depending on which pass produced it. Domain-shape only; carries no
 * opinion about which mode produced it -- see VertexMaskForgePanel::GenerateBoundingBoxMask's
 * CollectiveBounds parameter and VertexMaskForgePanel::ComputeCollectiveAxisBounds.
 */
struct FVertexMaskForgeAxisBoundsResult
{
	double MinCoord = 0.0;
	double MaxCoord = 0.0;
	bool bDegenerate = false;

	/**
	 * Only meaningful for an enabled Local-space axis (bWorldSpace == false) when these bounds are
	 * COLLECTIVE (Unified Bounds on). World-space direction that every participating component's own
	 * local axis maps to -- validated compatible across all participants (see
	 * VertexMaskForgePanel::ResolveSharedLocalAxis) -- used to project each component's own
	 * world-space render-vertex positions onto one shared coordinate line without discarding
	 * translation between instances. Normalized. Zero vector / unused for World-space axes and for
	 * Individual (non-Unified) bounds.
	 */
	FVector SharedLocalAxisDirection = FVector::ZeroVector;

	/** World-space length of the (pre-normalization) transformed local axis vector for the same
	 *  axis -- i.e. the shared scale along that axis. See SharedLocalAxisDirection. */
	double SharedLocalAxisScale = 1.0;
};

/**
 * A transient per-vertex scalar mask (e.g. the Bounding Box Mask), owned by exactly one
 * FVertexMaskForgeWorkingMesh. Exists only in memory; never written to the Primary Color Overlay,
 * MeshDescription, RenderData, or the source asset.
 *
 * AUDITED: the Bounding Box Mask (the only spatial generator that exists so far) is computed
 * directly in RENDER VERTEX order (see VertexMaskForgePanel::GenerateBoundingBoxMask), so for it,
 * Values/bHasValue are indexed by Render Vertex Index and are always dense (Values.Num() ==
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

	/** Indexed by Render Vertex Index for the Bounding Box Mask (see the struct-level audit note). */
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

	/** Which generator produced this mask -- so the UI never mislabels a Fill result as Bounding Box. */
	EVertexMaskForgeScalarMaskSource Source = EVertexMaskForgeScalarMaskSource::BoundingBox;

	/**
	 * Snapshot of the per-axis parameters used to generate this mask (Source == BoundingBox only;
	 * left default-constructed, all bEnabled == false, for Constant sources), indexed by
	 * EVertexMaskForgeBoundsAxis -- purely for diagnostic display (which axes/Mirror/World Space
	 * were active). Never affects composition; composition only ever reads Values/bHasValue.
	 */
	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> UsedAxisParams;

	// --- Temporary diagnostics (audited render-vertex-order fix) ---------------------------
	// Added to make the Values.Num() == PositionVertexBuffer.GetNumVertices() invariant directly
	// verifiable from the panel UI. See VertexMaskForgePanel::GetBoundingBoxMaskSummaryText().

	/** LOD0 PositionVertexBuffer.GetNumVertices() at generation time; must equal Values.Num(). */
	int32 RenderVertexCount = 0;

	/** True if this mask (Source == BoundingBox only) was generated against a collective, Unified
	 *  Bounds domain rather than this one mesh's own individual bounds. Diagnostic display only. */
	bool bUnifiedBounds = false;

	/** Total number of meshes in the selection at generation time (diagnostic display only; not a
	 *  per-mesh count -- Individual and Unified masks generated from the same selection both report
	 *  the same selection size, matching the checkpoint's diagnostic examples). */
	int32 SelectionMeshCount = 0;
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
	 * The active mask (Bounding Box across up to 3 axes, or a Constant Fill), if generated. This is
	 * the ENTRY-LEVEL reference: generated using the first live PreviewComponent's transform (or
	 * FTransform::Identity if none), used for gating (Ready check), the row summary text, and
	 * Content-Browser-only entries. When Source == BoundingBox and at least one axis uses World
	 * Space, actual Preview/Accept composition RE-EVALUATES the mask per component with that
	 * component's own transform (see ApplyPreviewToEntry / BuildAcceptTargets) rather than reusing
	 * this shared reference -- so two instances of this asset can legitimately show different
	 * results, exactly like a divergent per-instance OverrideVertexColors baseline already does. A
	 * fresh FVertexMaskForgeWorkingMesh is always constructed on Refresh Selection (see
	 * BuildWorkingMeshForStaticMesh), so this starts at NotGenerated automatically every time the
	 * working mesh itself is rebuilt -- there is no separate invalidation step needed for that case.
	 * Parameter-change invalidation is handled by the panel explicitly resetting this field.
	 */
	FVertexMaskForgeScalarMask BoundingBoxMask;
};

/**
 * What the artist currently sees in the viewport for Vertex Color preview purposes.
 * Purely a session/panel-level setting -- never saved on the UStaticMesh, never affects the
 * Bounding Box Mask or its per-axis parameters.
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

	/** Show the composed color's Alpha channel as grayscale (A, A, A, 1) -- visualization only, never
	 *  alters the real RGBA values held in the composed Preview/Accept data. */
	AlphaChannel,
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

	// --- Bounding Box Mask (Local X / Local Y / Local Z, each with independent Local/World Space) --

	/**
	 * Builds one axis's UI row (title "Local X"/"Local Y"/"Local Z" -- no directional text -- plus
	 * Enable/Position/Transition Width/Invert/Mirror/World Space controls, all bound via lambdas
	 * capturing Axis by value). Called 3 times from Construct(); keeps the 3 axis rows from
	 * triplicating Slate code even though the UI itself has 3 copies.
	 */
	TSharedRef<SWidget> BuildBoundingBoxAxisRow(EVertexMaskForgeBoundsAxis Axis, const FText& Title);

	/**
	 * Shared handler for a DISCRETE per-axis control change (Enable/Invert/Mirror/World Space --
	 * anything that isn't a continuously-dragged slider): invalidates the current mask, then, if
	 * Auto Update Preview is on, cancels any pending debounce (a stale continuous-slider callback
	 * must never apply after a discrete change) and regenerates immediately.
	 */
	void OnAxisParamChangedDiscrete();

	/** Processes every selected entry's working mesh, generating or clearing its Bounding Box Mask. */
	FReply OnGenerateBoundingBoxMaskClicked();

	/**
	 * Resets every selected entry's Bounding Box Mask back to NotGenerated, without touching the
	 * working mesh (FDynamicMesh3) itself. Called whenever any axis parameter changes, so stale
	 * statistics are never left looking current; the user must click Generate Mask again (if Auto
	 * Update Preview is off), or ScheduleAutoUpdatePreview()/RunAutoUpdatePreview() take over
	 * automatically (if it is on). Never touches a Constant Fill mask's meaning -- Fill results are
	 * independent of these axis parameters (only Generate Mask/Auto Update read them).
	 */
	void InvalidateBoundingBoxMasks();

	/** Panel-level parameters for each of the 3 axes, indexed by EVertexMaskForgeBoundsAxis. Shared
	 *  across every selected entry; per-instance World Space evaluation reads a component's own
	 *  transform separately (see GenerateBoundingBoxMask) -- these parameters themselves never vary
	 *  per entry or per component. Z starts bEnabled == true (matching the previously-validated
	 *  single-axis default); X and Y start disabled, so a fresh panel reproduces the exact prior
	 *  Local-Z-only behavior until the user explicitly enables another axis. */
	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> BoundingBoxAxisParams;

	// --- Fill White / Fill Black utility masks ----------------------------------------------

	FReply OnFillWhiteClicked();
	FReply OnFillBlackClicked();

	/**
	 * Shared implementation for both Fill buttons: cancels any pending Auto Update Preview debounce
	 * first (a Fill must never be overwritten moments later by a stale regeneration), then, for every
	 * SelectedMeshes entry that passes the SAME entry-level validity gating as Generate Mask
	 * (WorkingMesh Ready, resolvable Static Mesh, valid LOD 0 render data), generates a dense
	 * constant-valued mask (VertexMaskForgePanel::GenerateConstantMask) and assigns it to that
	 * entry's mask. Unlike a manual Generate Mask click, an entry that fails validation here is left
	 * COMPLETELY UNTOUCHED (its previous mask, if any, is preserved) rather than reset to
	 * Unavailable -- per the explicit "preserve the last valid Preview on failure" requirement.
	 * Ends with UpdateAllPreviews(), which recomposes/reapplies the transient Preview (reusing the
	 * exact same ApplyPreviewToEntry/ComposeRenderOrderPreviewColors path as every other mask) and
	 * marks Pending Changes via RecomputeOperationState().
	 */
	void RunConstantFill(float ConstantValue, EVertexMaskForgeScalarMaskSource Source, const FText& SuccessMessage);

	/** Enabled only when at least one selected entry has a Ready working mesh, and not while Applying. */
	bool CanRunFill() const;

	FText GetMaskActionStatusText() const { return LastMaskActionStatusText; }

	/** Success/partial-failure message for the last Generate Mask / Fill action; cleared by the next
	 *  mask-changing action (Generate Mask, Fill, parameter change, Refresh Selection, Accept, Cancel). */
	FText LastMaskActionStatusText;

	// --- Auto Update Preview (debounced automatic regeneration) ----------------------------

	ECheckBoxState GetAutoUpdatePreviewState() const { return bAutoUpdatePreview ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnAutoUpdatePreviewChanged(ECheckBoxState NewState);

	/**
	 * Arms (or restarts) a short one-shot debounce timer via GEditor's FTimerManager -- not a
	 * per-frame Tick override -- so a burst of slider events (SSpinBox fires OnValueChanged
	 * continuously while dragging) collapses into a single regeneration ~150ms after the LAST
	 * event. Calling FTimerManager::SetTimer() again with the same FTimerHandle before it fires
	 * clears and re-adds it (confirmed in TimerManager.cpp), which is exactly "a new change resets
	 * the wait". No-ops if Auto Update Preview is off. Uses FTimerDelegate::CreateSP (weak-safe: a
	 * pending timer harmlessly no-ops if this widget is destroyed first), but the timer is also
	 * explicitly cleared in OnWorldCleanup() and the destructor rather than relying on that alone.
	 */
	void ScheduleAutoUpdatePreview();

	/**
	 * Regenerates the Bounding Box Mask for every eligible entry using the CURRENT per-axis
	 * parameters. On a per-entry basis: if the new mask comes back Ready, it replaces the entry's
	 * mask; otherwise (DegenerateBounds/Invalid/Unavailable, including "no axis enabled") the
	 * entry's EXISTING mask is left untouched -- an auto-triggered regeneration must never destroy a
	 * valid Preview, unlike a manual Generate Mask click (OnGenerateBoundingBoxMaskClicked), which
	 * still unconditionally overwrites, per the already-validated checkpoint behavior. Never runs
	 * while Applying (guarded defensively; not reachable in practice since Accept is synchronous).
	 * Called by the debounce timer, or immediately for discrete parameter changes
	 * (Enable/Invert/Mirror/World Space).
	 */
	void RunAutoUpdatePreview();

	bool bAutoUpdatePreview = true;
	FTimerHandle AutoUpdateDebounceTimerHandle;

	// --- Unified Bounds (global, all 3 axes, all selected meshes) --------------------------

	ECheckBoxState GetUnifiedBoundsState() const { return bUseUnifiedBounds ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/**
	 * Toggling Unified Bounds never recomputes just one mesh: it cancels any pending debounce,
	 * invalidates every entry's current Bounding-Box-sourced mask (InvalidateBoundingBoxMasks()), and
	 * -- only if Auto Update Preview is on -- immediately regenerates every eligible entry as one
	 * coherent batch (RunAutoUpdatePreview(), which itself computes the collective domain once, if
	 * applicable, and reuses it for every participant). If Auto Update Preview is off, parameters are
	 * updated and results invalidated, but no new Preview is generated until the user clicks Generate
	 * Mask -- same contract as every other axis parameter.
	 */
	void OnUnifiedBoundsChanged(ECheckBoxState NewState);

	/**
	 * False (default) preserves the tool's previously-validated behavior exactly: each component
	 * normalizes its own render vertices against its OWN individual per-axis bounds (see
	 * VertexMaskForgePanel::GenerateBoundingBoxMask's internal bounds pass). True: every enabled
	 * axis is normalized against a COLLECTIVE domain -- the union of that axis's coordinate across
	 * every participating component's render vertices (see VertexMaskForgePanel::
	 * ComputeCollectiveAxisBounds) -- computed fresh before each batch (Generate Mask click, Auto
	 * Update regeneration, every Preview refresh, and Accept validation), never cached across calls,
	 * consistent with the rest of the panel's "always recompute, never stale" design. Global for all
	 * 3 axes and the whole selection -- there is no per-axis or per-mesh Unified Bounds mode.
	 */
	bool bUseUnifiedBounds = false;

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
	 * CurrentPreviewMode / Channel Filter / each entry's BoundingBoxMask state. Idempotent and
	 * side-effect-free with respect to the mask itself -- never generates or invalidates it.
	 */
	void UpdateAllPreviews();

	/**
	 * Applies or restores preview for one entry, per current mode/filter/mask state.
	 * CollectiveBoundsPtr is non-null only when bUseUnifiedBounds is on AND UpdateAllPreviews()
	 * successfully computed a collective domain for this batch -- passed through unchanged to every
	 * per-component GenerateBoundingBoxMask() re-evaluation (see the audit note there); nullptr means
	 * "use each component's own individual bounds" (bUseUnifiedBounds off, or this entry's mask
	 * Source isn't BoundingBox, in which case it's simply unused).
	 */
	void ApplyPreviewToEntry(
		const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry,
		const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>* CollectiveBoundsPtr);

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

	// --- Pending Changes: Accept / Cancel ---------------------------------------------------

	EVertexMaskForgeOperationState GetOperationState() const { return OperationState; }

	/** Recomputes OperationState (Idle/PendingChanges only -- see enum doc) from current data: a
	 *  Preview Mode other than OriginalMaterial, and at least one entry with an active
	 *  PreviewComponent and a Ready mask. Never touches LastOperationErrorText (callers that need to
	 *  report a failure set it themselves, AFTER calling anything that ends in this, so a fresh
	 *  message from this same pass is never clobbered). No-ops while Applying. */
	void RecomputeOperationState();

	bool CanAcceptChanges() const { return OperationState == EVertexMaskForgeOperationState::PendingChanges; }
	FReply OnAcceptChangesClicked();

	bool CanCancelChanges() const { return OperationState == EVertexMaskForgeOperationState::PendingChanges || OperationState == EVertexMaskForgeOperationState::Failed; }
	FReply OnCancelChangesClicked();

	FText GetOperationStatusText() const;

	/**
	 * Validates every eligible entry, confirms the destination with the user, and -- only if both
	 * succeed -- writes permanently to the Static Mesh asset(s) inside one FScopedTransaction. See
	 * the .cpp for the full validate-then-write contract (VertexMaskForgePanel::BuildAcceptTargets /
	 * WriteAcceptTargets). Returns true only on a fully successful Accept.
	 */
	bool AcceptPendingChanges();

	/** Records the reason the last Accept (or auto-update regeneration) was blocked/failed, shown in
	 *  GetOperationStatusText(). Cleared explicitly at the START of each fresh attempt -- never by
	 *  RecomputeOperationState(), so it survives whatever UpdateAllPreviews() call follows within the
	 *  same attempt. */
	FText LastOperationErrorText;

	EVertexMaskForgeOperationState OperationState = EVertexMaskForgeOperationState::Idle;

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
	// invalidate or regenerate BoundingBoxMask, and are fully independent of one another.
	EVertexMaskForgePreviewMode CurrentPreviewMode = EVertexMaskForgePreviewMode::OriginalMaterial;
	TArray<TSharedPtr<EVertexMaskForgePreviewMode>> PreviewModeOptions;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgePreviewMode>>> PreviewModeComboBox;

	bool bChannelFilterR = true;
	bool bChannelFilterG = true;
	bool bChannelFilterB = true;
	bool bChannelFilterA = false;

	/** Resolved lazily via GetPreviewDebugMaterial(); weak because it is an asset the plugin does not own. */
	TWeakObjectPtr<UMaterialInterface> PreviewDebugMaterial;
};
