#pragma once

#include "Containers/BitArray.h"
#include "Containers/StaticArray.h"
#include "CoreMinimal.h"
#include "Delegates/IDelegateInstance.h"
#include "EditorUndoClient.h"
#include "Engine/TimerHandle.h"
#include "Math/Vector4.h"
#include "MeshTypes.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtr.h"
#include "VertexMaskForgeMaskTypes.h"
#include "VertexMaskForgeWorkingMeshTypes.h"
#include "Widgets/SCompoundWidget.h"

class UWorld;

class AActor;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;
class UDynamicMeshComponent;
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

/** One of the six principal axis directions a surface normal can be compared against -- Unreal's own
 *  convention (X+ Forward, Y+ Right, Z+ Up). See VertexMaskForgePanel::GetNormalDirectionVector for the
 *  exact unit vectors. */
enum class EVertexMaskForgeNormalDirection : uint8
{
	PositiveX,
	NegativeX,
	PositiveY,
	NegativeY,
	PositiveZ,
	NegativeZ,
};

/** Which of FVertexMaskForgeWorkingMesh::CurvatureRawConvexCache/CurvatureRawConcaveCache (or their
 *  union) Curvature Type selects as the mask magnitude -- see
 *  VertexMaskForgePanel::ApplyCurvatureArtisticParams. */
enum class EVertexMaskForgeCurvatureType : uint8
{
	/** Mask = RawConvexCurvature -- convex edges/bulges only; concave regions read as 0 (black). */
	Convex,

	/** Mask = RawConcaveCurvature -- concave cavities/creases only; convex regions read as 0 (black). */
	Concave,

	/** Mask = max(RawConvexCurvature, RawConcaveCurvature) -- a union of the two INDEPENDENTLY
	 *  accumulated magnitude arrays (see FVertexMaskForgeWorkingMesh::CurvatureRawConvexCache's own doc
	 *  comment); convex and concave NEVER cancel each other out, since neither is ever computed as a
	 *  single signed sum in the first place. Default. */
	Both,
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

// EVertexMaskForgePreviewMode is now defined in VertexMaskForgeMaskTypes.h (M3 extraction, moved so
// VertexMaskForgeDisplayColorDerivation.h/.cpp can use it without including this header) -- see that
// header for the enum's own doc comment.

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

class SVertexMaskForgePanel : public SCompoundWidget, public FEditorUndoClient
{
public:
	SLATE_BEGIN_ARGS(SVertexMaskForgePanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Declared here and defined in the .cpp so FDynamicMesh3 only needs to be complete there. */
	virtual ~SVertexMaskForgePanel() override;

	/**
	 * FEditorUndoClient. Registered/unregistered via GEditor->RegisterForUndo/UnregisterForUndo in
	 * Construct()/the destructor. See HandlePostUndoRedo's own doc comment for the full contract --
	 * this is panel-state resync ONLY, never a substitute for Accept's own transactional write. */
	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override;

private:
	void HandlePostUndoRedo(bool bSuccess, bool bIsRedo);

	/**
	 * Bound to USelection::SelectionChangedEvent (registered once in Construct(), removed in the
	 * destructor via SelectionChangedDelegateHandle). Fires for Actor/Component/BSP scene selection
	 * changes only; Content Browser asset selection is a completely separate system and was never
	 * routed through this delegate, matching this panel no longer consulting the Content Browser at
	 * all (see CollectViewportSelection, the only collector left). NewSelection (which USelection
	 * instance changed) is unused.
	 *
	 * AUDITED (UX1, explicit Edit Vertex Mask session entry): a scene selection change NEVER starts or
	 * retargets an editing session by itself -- RefreshSelection() (the function that actually builds
	 * WorkingMeshes/PreviewComponents and enters Editing) is called ONLY from OnEditVertexMaskClicked()
	 * now. While bIsEditingVertexMask is true, an active session's targets (SelectedMeshes) must never
	 * be silently retargeted or discarded just because the scene selection changed underneath it -- so
	 * this does NOT call anything that touches SelectedMeshes in that case; it only records that a
	 * candidate resync is owed, via bSceneSelectionChangedDuringActiveOperation, so the panel can catch
	 * up automatically -- without requiring another viewport/World Outliner click -- the moment the
	 * session actually concludes (see SyncSelectionIfChangedDuringOperation). While NOT editing, this
	 * only refreshes the lightweight candidate list used to enable/disable the "Edit Vertex Mask"
	 * button (RefreshCandidateSelection()) -- never WorkingMeshes, never previews, never hides anything.
	 */
	void OnEditorSelectionChanged(UObject* NewSelection);

	/**
	 * Re-queries the scene (viewport/World Outliner) selection and rebuilds the working set --
	 * i.e. STARTS an editing session (WorkingMeshes, baseline colors, and -- once a generator produces
	 * a Ready mask -- PreviewComponents/hidden originals, all via the existing UpdateAllPreviews/
	 * ApplyPreviewToEntry lifecycle, entirely unchanged by UX1). Called ONLY from
	 * OnEditVertexMaskClicked() now -- see that function's own doc comment. Left otherwise unmodified
	 * by UX1: still the single, existing lifecycle entry point the button reuses verbatim.
	 */
	void RefreshSelection();

	/**
	 * Lightweight Idle-state counterpart to RefreshSelection(): re-derives CandidateMeshes from the
	 * CURRENT scene selection via CollectViewportSelection()/UpdateMeshDiagnostics() only -- never
	 * BuildWorkingMeshes(), never DestroyAllPreviews()/UpdateAllPreviews(), never touches SelectedMeshes.
	 * Used purely to keep the "Edit Vertex Mask" button's enabled state and the Idle status text
	 * current; has no effect on any session, preview, generator, or asset.
	 */
	void RefreshCandidateSelection();

	/**
	 * Set true by OnEditorSelectionChanged() when the scene selection changes while
	 * bIsEditingVertexMask is true (i.e. while a session is active against targets captured at the
	 * moment "Edit Vertex Mask" was clicked). Consumed -- and cleared -- by
	 * SyncSelectionIfChangedDuringOperation(), called at the tail of exactly the two actions that can
	 * legitimately conclude a session: OnCancelChangesClicked() and AcceptPendingChanges() (on
	 * success). Deliberately NOT consumed by any other RecomputeOperationState() call site (e.g.
	 * ordinary mask regeneration flipping PendingChanges<->Idle mid-session), so an incidental state
	 * flip unrelated to actually concluding the session never triggers an unwanted resync.
	 */
	bool bSceneSelectionChangedDuringActiveOperation = false;

	/**
	 * If the scene selection changed while the session that just concluded was active (see
	 * bSceneSelectionChangedDuringActiveOperation), re-derives CandidateMeshes from the CURRENT scene
	 * selection via RefreshCandidateSelection() and clears the flag -- no extra viewport/World Outliner
	 * click required to re-enable "Edit Vertex Mask" for the new selection. No-ops if the selection
	 * never changed during the just-concluded session. AUDITED (UX1): no longer calls RefreshSelection()
	 * -- concluding a session must never automatically start another one (see requirement #6).
	 *
	 * MUST only be called AFTER the session's own targets have been fully validated/written (Accept) or
	 * fully discarded (Cancel) against the ORIGINAL SelectedMeshes, and after bIsEditingVertexMask has
	 * already been cleared -- calling this any earlier would let a resync silently retarget or interrupt
	 * an in-flight session, which must never happen.
	 */
	void SyncSelectionIfChangedDuringOperation();

	/**
	 * AUDITED (UX1, explicit Edit Vertex Mask session entry): the ONLY thing that starts an editing
	 * session now. True from the moment "Edit Vertex Mask" is clicked (RefreshSelection() has captured
	 * the then-current scene selection into SelectedMeshes) until Accept succeeds or Cancel runs.
	 * Independent of OperationState -- a session can be Editing with OperationState still Idle (no
	 * generator enabled/Ready yet); OperationState only tracks "is there a Ready mask to Accept",
	 * never "does a session exist". While true: OnEditorSelectionChanged() never touches SelectedMeshes;
	 * while false: SelectedMeshes is always empty (no WorkingMeshes, no PreviewComponents, no hidden
	 * originals, no generator/live-preview/Accept/Cancel activity -- all of those already read from
	 * SelectedMeshes and are therefore no-ops by construction while this is false).
	 */
	bool bIsEditingVertexMask = false;

	/**
	 * Idle-state-only candidate list, kept current by RefreshCandidateSelection() (called from
	 * Construct()'s tail and from OnEditorSelectionChanged() while not editing). Used exclusively to
	 * decide whether "Edit Vertex Mask" is enabled (CanEditVertexMask()) and for the Idle status text
	 * (GetEditSessionStatusText()) -- never read by any generator, composition, preview, or Accept/
	 * Cancel code, which all operate on SelectedMeshes (the session's own targets) instead. Left empty
	 * while a session is active (not maintained during Editing -- see OnEditorSelectionChanged).
	 */
	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>> CandidateMeshes;

	/** True only while Idle (not already editing) and at least one valid candidate target is present. */
	bool CanEditVertexMask() const { return !bIsEditingVertexMask && !CandidateMeshes.IsEmpty(); }

	/**
	 * "Edit Vertex Mask" button handler -- the sole entry point into an editing session. Re-validates
	 * the selection is still eligible, then reuses RefreshSelection() UNCHANGED (it already re-queries
	 * the CURRENT scene selection and runs the existing WorkingMeshes/preview/baseline lifecycle) to
	 * capture a snapshot of the currently selected valid target(s) -- ALL of them, via the same
	 * CollectViewportSelection precedence/filtering every other entry point already uses, so multi-
	 * target sessions are preserved exactly as before. Idempotent against repeated clicks:
	 * CanEditVertexMask() (bound to the button's IsEnabled) is false the instant bIsEditingVertexMask
	 * becomes true, so a session can never be started twice.
	 */
	FReply OnEditVertexMaskClicked();

	/** Idle: reports the current candidate count ("no valid selection" / "N target(s) selected, click
	 *  Edit Vertex Mask to begin"). Editing: reports the ACTIVE SESSION's own target count (SelectedMeshes),
	 *  never the (possibly since-changed) external scene selection, to avoid ambiguity during Editing. */
	FText GetEditSessionStatusText() const;

	/**
	 * Gathers unique Static Meshes from UStaticMeshComponents in the CURRENT SCENE selection -- the
	 * ONLY selection source this panel consults (Content Browser asset selection was removed
	 * entirely; a UStaticMesh can only participate via a real, placed UStaticMeshComponent found
	 * here). Every candidate component, from either source below, passes through one centralized
	 * eligibility gate (IsEligibleComponent in the .cpp) that explicitly rejects invalid/pending-kill
	 * objects and this plugin's own transient preview components (by outer package and RF_Transient,
	 * not merely by the fact that they are never registered with any USelection set).
	 *
	 * Precedence between the two scene selection sources is DETERMINISTIC, not additive (audited
	 * against the UE 5.8 Level Editor's own click-handling source -- see the .cpp for the full
	 * evidence):
	 *   - If GEditor->GetSelectedComponents() contains ANY UStaticMeshComponent (an explicit
	 *     component-selection is active), those components ALONE are the targets -- their owning
	 *     Actor(s) are never auto-expanded to all of their other components, even though the owning
	 *     Actor legitimately remains present in GetSelectedActors() at the same time.
	 *   - Otherwise, falls back to every valid UStaticMeshComponent owned by every selected Actor
	 *     (GEditor->GetSelectedActors(), Actor->GetComponents<UStaticMeshComponent>()) -- the
	 *     pre-existing, already-validated Actor-granularity behavior.
	 * Never touches the Content Browser. See VertexMaskForgePanel::AddOrUpdateSelectedMesh for the
	 * per-component dedup (by pointer identity, never by UStaticMesh).
	 */
	void CollectViewportSelection(
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

	// --- Bounding Box Mask (Local X / Local Y / Local Z, each with independent Local/World Space) --

	/**
	 * Builds one axis's UI row: a first line with the Enable checkbox labeled "Local X"/"Local Y"/
	 * "Local Z" (no separate directional text), then a second line with Position/Falloff/Invert/
	 * Mirror/World Space controls, all bound via lambdas capturing Axis by value). Called 3 times
	 * from Construct(); keeps the 3 axis rows from triplicating Slate code even though the UI itself
	 * has 3 copies. The visible "Falloff" label maps to the TransitionWidth field/parameter, which
	 * keeps its name -- see FVertexMaskForgeAxisMaskParams::TransitionWidth.
	 */
	TSharedRef<SWidget> BuildBoundingBoxAxisRow(EVertexMaskForgeBoundsAxis Axis, const FText& Title);

	/**
	 * Shared handler for a DISCRETE per-axis control change (Enable/Mirror/World Space -- anything
	 * that isn't a continuously-dragged slider and isn't per-axis Invert, see OnAxisInvertChanged for
	 * that exception): invalidates the current mask, cancels any pending debounce (a stale
	 * continuous-slider callback must never apply after a discrete change), and always regenerates
	 * immediately.
	 */
	void OnAxisParamChangedDiscrete();

	/**
	 * AUDITED (BBox Invert exception, follow-up audit): per-axis Invert's OWN handler, deliberately
	 * NOT OnAxisParamChangedDiscrete. Each axis's Invert flag is baked into GenerateBoundingBoxMask's
	 * per-axis gradient BEFORE the max-combination across axes -- three independent per-axis flags
	 * cannot be correctly represented as a single post-hoc, composition-time invert (see
	 * ComposeMaskStack), so Option A (preserve raw un-inverted values, invert during composition) was
	 * judged disproportionate for this axis-based design; Option B is implemented instead: Invert
	 * regenerates BoundingBoxMask immediately (regeneration itself is cheap here -- unlike Ambient
	 * Occlusion, Bounding Box has no persistent geometry cache to needlessly rebuild). Calls
	 * RunAutoUpdatePreview with bIncludeAO false so Ambient Occlusion's own slot/AOCache are never
	 * touched, not even a harmless re-validation.
	 */
	void OnAxisInvertChanged(int32 AxisIndex, ECheckBoxState NewState);

	/**
	 * AUDITED (raw/composition separation checkpoint): resets ONLY every selected entry's
	 * BoundingBoxMask slot back to NotGenerated (AmbientOcclusionMask is completely untouched), without
	 * touching the working mesh (FDynamicMesh3) itself. Called whenever a Bounding Box RAW/geometric
	 * parameter changes (axis Position/Falloff/Invert/Mirror/World Space/Enable, Unified Bounds) --
	 * NEVER for a purely compositional change (Blend Mode, Opacity -- see RecomposeWorkingColors
	 * instead). Never touches a Constant Fill mask's meaning -- Fill results are independent of these
	 * axis parameters.
	 */
	void InvalidateBoundingBoxRawMask();

	/**
	 * AUDITED (renamed for precision per explicit follow-up audit -- was InvalidateAORawMask, which
	 * was misleading). Resets ONLY the ENTRY-LEVEL DERIVED slot, FVertexMaskForgeWorkingMesh::
	 * AmbientOcclusionMask (a cheap Ready/Unavailable + UsedAOParams snapshot -- see that field's own
	 * doc comment; it never holds real per-vertex Values). Does NOT invalidate FVertexMaskForgeAOCache::
	 * RawValues (the actual raycast results) -- those are preserved; GenerateAmbientOcclusionMask's own
	 * cache key (Mesh identity/DerivedDataKey/counts/Transform/Samples/MaxDistance/Bias) transparently
	 * decides, the next time it runs, whether RawValues still match and can be reused (zero raycasts)
	 * or must be recomputed. BoundingBoxMask is completely untouched either way. Called when an AO RAW
	 * parameter changes (Samples, Max Distance, Bias) or when AO Enable turns on for an entry with no
	 * valid derived slot yet (see OnAOEnableChanged -- an entry that already has a Ready derived slot
	 * is deliberately NOT passed through this function at all, so its AOCache is never even queried).
	 * Never for AO Blend Mode/Opacity/Invert/Enable-when-already-Ready -- see RecomposeWorkingColors.
	 */
	void InvalidateAODerivedMask();

	/**
	 * AUDITED (raw/composition separation checkpoint): the PURE composition path -- Blend Mode,
	 * Opacity, Invert (Bounding Box or Ambient Occlusion), Enable/Disable of an already-Ready layer,
	 * Channel Filter, Preview Mode. Touches NEITHER BoundingBoxMask NOR AmbientOcclusionMask/AOCache --
	 * simply calls UpdateAllPreviews(false), which re-reads all compositional state live every call.
	 * Recomposes immediately and correctly with ZERO raycasts and ZERO Tree rebuilds.
	 */
	void RecomposeWorkingColors();

	/** Panel-level parameters for each of the 3 axes, indexed by EVertexMaskForgeBoundsAxis. Shared
	 *  across every selected entry; per-instance World Space evaluation reads a component's own
	 *  transform separately (see GenerateBoundingBoxMask) -- these parameters themselves never vary
	 *  per entry or per component. AUDITED (pre-modularization UI/defaults pass): every axis starts
	 *  disabled (bEnabled == false, the struct's own default) -- a fresh panel generates nothing
	 *  automatically until the user explicitly enables an axis. */
	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> BoundingBoxAxisParams;

	// --- Bounding Box Mask: Blend Mode + Opacity --------------------------------------------

	TSharedRef<SWidget> OnGenerateBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const;

	/**
	 * Blend Mode is PURE composition -- it never affects the underlying mask's raw Values, only how
	 * ComposeMaskStack reads them -- so this recomposes immediately via RecomposeWorkingColors(),
	 * exactly like Channel Filter/Preview Mode.
	 */
	void OnBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetBlendModeButtonText() const;

	TArray<TSharedPtr<EVertexMaskForgeBlendMode>> BlendModeOptions;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>> BlendModeComboBox;

	/** How the Bounding Box Mask's scalar value combines with each enabled channel's existing value.
	 *  Default Copy: with Opacity 1.0, exactly reproduces the tool's pre-Blend-Mode behavior. */
	EVertexMaskForgeBlendMode BoundingBoxBlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Opacity slider/spinbox's inline lambda calls RecomposeWorkingColors() directly (raw/composition
	 *  separation checkpoint) -- PURE composition, recomposes immediately regardless of Auto Update
	 *  Preview, no separate named handler needed. Clamped to [0, 1]; default 1.0 (full effect of the
	 *  Blend Mode -- matches the pre-Blend-Mode behavior when Mode == Copy). */
	float BoundingBoxOpacity = 1.0f;

	// --- Ambient Occlusion Mask --------------------------------------------------------------
	// AUDITED (composition-stack checkpoint): bAOEnabled is NO LONGER mutually exclusive with
	// Bounding Box -- it simply controls whether the Ambient Occlusion layer participates in the
	// composition stack (see ApplyPreviewToEntry). Bounding Box participates whenever at least one
	// axis is enabled, completely independently of bAOEnabled. Both can be Ready and both compose
	// together; either can be the only one active; both can be off. See GetActiveMaskSourceText() for
	// the UI readout of which layer(s) currently participate.

	ECheckBoxState GetAOEnableState() const { return bAOEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** AUDITED (re-examined per explicit follow-up audit): turning OFF is always pure composition.
	 *  Turning ON is pure composition TOO when every entry already has a Ready AmbientOcclusionMask
	 *  (reuses it verbatim, zero raycasts) -- only entries WITHOUT a valid derived mask trigger real
	 *  (re)generation, always immediate. Enabling, by itself, is never treated as a reason to
	 *  invalidate an already-valid mask. See the .cpp definition for the exact per-entry decision. */
	void OnAOEnableChanged(ECheckBoxState NewState);

	ECheckBoxState GetAOInvertState() const { return bAOInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** Invert never touches the AO acceleration structure or the raw per-vertex samples (applied only
	 *  when FVertexMaskForgeScalarMask::Values is populated from the cached raw values -- see
	 *  VertexMaskForgePanel::GenerateAmbientOcclusionMask) -- still invalidates/regenerates through the
	 *  normal discrete-parameter path so the composed Preview picks up the new Values immediately. */
	void OnAOInvertChanged(ECheckBoxState NewState);

	/** Describes which layer(s) currently participate in the composition stack -- e.g. "Bounding Box +
	 *  Ambient Occlusion", "Bounding Box only", "Ambient Occlusion only", "None". */
	FText GetActiveMaskSourceText() const;

	TSharedRef<SWidget> OnGenerateAOBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const;

	/** Pure composition -- same treatment as OnBlendModeSelectionChanged (Bounding Box's own Blend
	 *  Mode combo): recomposes immediately via RecomposeWorkingColors(). */
	void OnAOBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetAOBlendModeButtonText() const;

	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>> AOBlendModeComboBox;

	/** How the Ambient Occlusion layer's scalar value combines with the RUNNING accumulated channel
	 *  value (i.e. whatever the Bounding Box layer, if also enabled, already produced -- see
	 *  ComposeMaskStack) -- independent of BoundingBoxBlendMode. Default Copy, matching
	 *  BoundingBoxBlendMode's own default/rationale. */
	EVertexMaskForgeBlendMode AOBlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Same contract/range/default as BoundingBoxOpacity, independent of it. */
	float AOOpacity = 1.0f;

	/** Default false: preserves the tool's pre-AO behavior exactly (Ambient Occlusion does not
	 *  participate) until the user explicitly opts in. */
	bool bAOEnabled = false;

	bool bAOInvert = false;

	/** Clamped again in GenerateAmbientOcclusionMask itself; UI SpinBox already clamps to the same
	 *  [8, 256] range. */
	int32 AOSamples = 16;

	/** Unreal units (World Space); clamped again in GenerateAmbientOcclusionMask to (0, 10000]. */
	float AOMaxDistance = 100.0f;

	/** Unreal units (World Space); clamped again in GenerateAmbientOcclusionMask to [0.001, 10.0]. */
	float AOBias = 0.1f;

	/** AUDITED (AO Levels): see FVertexMaskForgeAOParams::LevelsMin's own doc comment for the exact
	 *  pipeline. UI range [0, 1]; default 0.0. Purely compositional -- see OnAOLevelsChanged. */
	float AOLevelsMin = 0.0f;

	/** AUDITED (AO Levels): see FVertexMaskForgeAOParams::LevelsMax's own doc comment. UI range [0, 1];
	 *  default 1.0. Purely compositional -- see OnAOLevelsChanged. */
	float AOLevelsMax = 1.0f;

	/** Same treatment as OnAOInvertChanged -- Levels never touches the AO acceleration structure or the
	 *  raw per-vertex/per-element samples (applied only when Values is populated from RawValues -- see
	 *  ApplyAOLevelsAndInvert), but still invalidates/regenerates through the normal discrete-parameter
	 *  path so the composed Preview picks up the new Values immediately. */
	void OnAOLevelsChanged();

	// --- Curvature Mask ----------------------------------------------------------------------
	// A third, independent, optional composition-stack layer -- structurally a peer of Bounding Box
	// and Ambient Occlusion (see EVertexMaskForgeScalarMaskSource::Curvature and ComposeMaskStack's own
	// doc comment: "no fixed role", any future generator plugs in exactly the same way). UNLIKE Ambient
	// Occlusion, Curvature never depends on a component's transform (see FVertexMaskForgeWorkingMesh::
	// CurvatureMask's own doc comment) -- it is generated exactly once per entry, real values included,
	// and every tracked component of that entry reuses the SAME entry-level result directly.

	ECheckBoxState GetCurvatureEnableState() const { return bCurvatureEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** Same enable/disable contract as OnAOEnableChanged (see its own doc comment): turning OFF is
	 *  always pure composition; turning ON reuses an already-Ready entry immediately (zero re-analysis)
	 *  and always regenerates immediately if genuinely needed. */
	void OnCurvatureEnableChanged(ECheckBoxState NewState);

	TSharedRef<SWidget> OnGenerateCurvatureTypeRow(TSharedPtr<EVertexMaskForgeCurvatureType> InOption) const;
	void OnCurvatureTypeSelectionChanged(TSharedPtr<EVertexMaskForgeCurvatureType> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetCurvatureTypeButtonText() const;

	TArray<TSharedPtr<EVertexMaskForgeCurvatureType>> CurvatureTypeOptions;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeCurvatureType>>> CurvatureTypeComboBox;

	/** Convex/Concave/Both -- see EVertexMaskForgeCurvatureType. Default Both (per explicit requirement):
	 *  convex and concave regions both show, without cancelling each other out. */
	EVertexMaskForgeCurvatureType CurvatureType = EVertexMaskForgeCurvatureType::Concave;

	/** UI range [0, 10], default 1.0, clamped again in ApplyCurvatureArtisticParams to >= 0. Scales the
	 *  Type-extracted magnitude BEFORE Blur/Levels -- 0 removes the layer's contribution entirely, 1 is
	 *  the original analyzed intensity, >1 amplifies it. Never Opacity's role (see OnCurvatureParamChanged). */
	float CurvatureMultiplier = 1.0f;

	/** UI range [0, 10], default 0.0 (no blur). Integer part = full topological-averaging iterations;
	 *  fractional part = a further lerp toward one more iteration -- see
	 *  VertexMaskForgePanel::ApplyTopologicalCurvatureBlur for the exact algorithm. Purely a downstream
	 *  reprocessing of the cached raw magnitudes; never re-derives adjacency/dihedral angles. */
	float CurvatureBlur = 0.0f;

	/** Same semantics as AOLevelsMin, applied to the Type/Multiplier/Blur result (see
	 *  VertexMaskForgePanel::ApplyCurvatureLevels) -- UI range [0, 1], default 0.0. Purely compositional;
	 *  see OnCurvatureParamChanged. */
	float CurvatureLevelsMin = 0.0f;

	/** Same semantics as AOLevelsMax -- UI range [0, 1], default 1.0. */
	float CurvatureLevelsMax = 1.0f;

	ECheckBoxState GetCurvatureInvertState() const { return bCurvatureInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** Applied LAST, after Levels Min/Max, over the already-leveled [0,1] result (Mask = 1 - Mask) --
	 *  see OnCurvatureParamChanged's own doc comment for the exact pipeline order. Same treatment as
	 *  OnAOInvertChanged: never touches the cached raw Convex/Concave magnitudes, purely compositional,
	 *  routed through the same shared OnCurvatureParamChanged reprocess. Default false. */
	void OnCurvatureInvertChanged(ECheckBoxState NewState);

	bool bCurvatureInvert = false;

	/**
	 * Shared handler for Curvature Type / Multiplier / Blur / Levels Min / Levels Max / Invert: ALL of
	 * these are cheap, PURELY DOWNSTREAM reprocessing of the already-cached raw Convex/Concave magnitude
	 * arrays (see FVertexMaskForgeWorkingMesh::CurvatureRawConvexCache's own doc comment) -- none of
	 * them ever re-run the adjacency/dihedral-angle analysis or touch GeometryFingerprint, so (unlike
	 * Bounding Box axis parameters or AO Samples/Max Distance/Bias) this is UNCONDITIONAL and immediate,
	 * exactly like OnAOInvertChanged/OnAOLevelsChanged. Regenerates
	 * every SelectedMeshes entry's CurvatureMask directly from its cached raw magnitudes (entry-level,
	 * real values -- see FVertexMaskForgeWorkingMesh::CurvatureMask), then recomposes. Pipeline order:
	 * raw Convex/Concave -> Curvature Type -> Multiplier -> Blur -> Levels Min/Max -> Invert -> (Opacity
	 * + Blend Mode, applied downstream by ComposeMaskStack, unchanged).
	 */
	void OnCurvatureParamChanged();

	TSharedRef<SWidget> OnGenerateCurvatureBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const;
	void OnCurvatureBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetCurvatureBlendModeButtonText() const;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>> CurvatureBlendModeComboBox;

	/** Same contract/range as BoundingBoxBlendMode/AOBlendMode. Default Copy -- preserves this tool's
	 *  established "every new layer defaults to Copy" convention (BoundingBoxBlendMode and AOBlendMode
	 *  both default to Copy; Add was considered but rejected specifically to keep that convention
	 *  consistent -- see the Curvature implementation report). */
	EVertexMaskForgeBlendMode CurvatureBlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Same contract/range/default as BoundingBoxOpacity/AOOpacity, independent of them. Applied ONLY
	 *  during final composition (ComposeMaskStack), never to CurvatureMask.Values itself -- Opacity 0
	 *  leaves the layer with no influence without erasing what Bounding Box/Ambient Occlusion already
	 *  produced (same contract every existing layer's Opacity already has). */
	float CurvatureOpacity = 1.0f;

	bool bCurvatureEnabled = false;

	// --- Noise Mask (V1: procedural 3D Perlin/FBM, Local Space only) -------------------------
	// A fourth, independent, optional composition-stack layer -- structurally a peer of Bounding Box,
	// Ambient Occlusion, and Curvature (see EVertexMaskForgeScalarMaskSource::Noise and
	// ComposeMaskStack's own doc comment: "no fixed role"). Like Curvature, Noise never depends on a
	// component's transform (pure function of LOCAL-SPACE position) -- generated exactly once per
	// entry, real values included, reused directly by every tracked component of that entry. UNLIKE
	// Curvature, its raw pattern DOES depend on artist-chosen generative parameters (Scale/Offset/Seed/
	// Octaves/Roughness/Lacunarity/Type) -- see FVertexMaskForgeNoiseGenerativeParams and
	// FVertexMaskForgeWorkingMesh::NoiseRawCache's own doc comments for the resulting two-tier
	// invalidation contract (generative params gate a real, always-immediate regeneration, exactly
	// like Bounding Box axis params / AO Samples-MaxDistance-Bias; Multiplier/Levels/Invert/
	// Opacity/Blend Mode are cheap, immediate, unconditional reprocessing, exactly like Curvature's own
	// Type/Multiplier/Blur/Levels/Invert).

	ECheckBoxState GetNoiseEnableState() const { return bNoiseEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** Same enable/disable contract as OnAOEnableChanged/OnCurvatureEnableChanged (see their own doc
	 *  comments): turning OFF is always pure composition; turning ON reuses an already-Ready entry
	 *  immediately (zero re-evaluation) and always regenerates immediately if genuinely needed. */
	void OnNoiseEnableChanged(ECheckBoxState NewState);

	TSharedRef<SWidget> OnGenerateNoiseTypeRow(TSharedPtr<EVertexMaskForgeNoiseType> InOption) const;
	void OnNoiseTypeSelectionChanged(TSharedPtr<EVertexMaskForgeNoiseType> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetNoiseTypeButtonText() const;

	TArray<TSharedPtr<EVertexMaskForgeNoiseType>> NoiseTypeOptions;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeNoiseType>>> NoiseTypeComboBox;

	/** Perlin or FractalPerlin -- see EVertexMaskForgeNoiseType. Default FractalPerlin (per explicit
	 *  requirement). GENERATIVE -- see OnNoiseGenerativeParamChanged. */
	EVertexMaskForgeNoiseType NoiseType = EVertexMaskForgeNoiseType::FractalPerlin;

	/** Per-axis frequency multiplier. UI minimum a small positive epsilon (never exactly 0 -- see
	 *  ComputeRawNoiseValue's own clamp). Default 1.0 each. GENERATIVE. */
	float NoiseScaleX = 1.0f;
	float NoiseScaleY = 1.0f;
	float NoiseScaleZ = 1.0f;

	/** UI/workflow-only toggle (V2-A adjustment) -- when true, Scale Y/Z are driven from Scale X and
	 *  disabled for direct editing (see OnNoiseScaleXChanged/OnNoiseScaleAxesLockChanged). NOT part of
	 *  FVertexMaskForgeNoiseGenerativeParams: the generative result is already fully determined by the
	 *  final Scale X/Y/Z values themselves, so this flag has nothing left to contribute to the cache key.
	 *  Default false -- preserves the exact pre-existing independent-axes behavior. */
	bool bNoiseScaleAxesLocked = false;

	ECheckBoxState GetNoiseScaleAxesLockState() const { return bNoiseScaleAxesLocked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** Turning ON immediately snaps Y/Z to the current X (at most one InvalidateNoiseRawMask + one Auto
	 *  Update, only if a value actually changed). Turning OFF only flips the flag -- no value changes, so
	 *  no invalidation and no Auto Update (see OnNoiseScaleAxesLockChanged's own doc comment). */
	void OnNoiseScaleAxesLockChanged(ECheckBoxState NewState);

	/** Dedicated, atomic handler for the Scale X spin box -- when bNoiseScaleAxesLocked is true, also
	 *  applies the same final value to Scale Y and Scale Z in the SAME call, so a single edit invalidates
	 *  Raw Noise and requests Auto Update exactly ONCE (never three times via three separate handler
	 *  calls). When unlocked, behaves exactly like the pre-existing Scale X handler. */
	void OnNoiseScaleXChanged(float NewValue);

	/** Domain offset in noise space. Default 0 each. GENERATIVE. */
	float NoiseOffsetX = 0.0f;
	float NoiseOffsetY = 0.0f;
	float NoiseOffsetZ = 0.0f;

	/** Hashed deterministically into a 3D domain offset -- never engine RNG state. Default 0. GENERATIVE. */
	int32 NoiseSeed = 0;

	/** FractalPerlin only. UI range [1, 8]; default 4. GENERATIVE. */
	int32 NoiseOctaves = 4;

	/** FractalPerlin only. UI range [0, 1]; default 0.5. GENERATIVE. */
	float NoiseRoughness = 0.5f;

	/** FractalPerlin/Billow/Ridged/Turbulence only. UI minimum 1.0; default 2.0. GENERATIVE. */
	float NoiseLacunarity = 2.0f;

	/** Turbulence only. UI range [0, 5]; default 0.5. GENERATIVE. */
	float NoiseTurbulenceStrength = 0.5f;

	/** True for every Noise Type whose raw pattern is a weighted sum of octaves (FractalPerlin, Billow,
	 *  Ridged, Turbulence, Alligator) -- false for the single-sample Perlin AND for the three V2-B
	 *  cellular types (WorleyF1/WorleyF2MinusF1/Voronoi are not multi-octave; Octaves/Roughness/
	 *  Lacunarity are unused by ComputeRawNoiseValue's cellular branch). Small, localized helper so the
	 *  Octaves/Roughness/Lacunarity IsEnabled bindings don't each repeat their own type comparison. */
	bool UsesFractalParameters() const
	{
		switch (NoiseType)
		{
		case EVertexMaskForgeNoiseType::FractalPerlin:
		case EVertexMaskForgeNoiseType::Billow:
		case EVertexMaskForgeNoiseType::Ridged:
		case EVertexMaskForgeNoiseType::Turbulence:
		case EVertexMaskForgeNoiseType::Alligator:
			return true;
		default:
			return false;
		}
	}

	/**
	 * Shared handler for ALL generative Noise parameters (Type/Scale XYZ/Offset XYZ/Seed/Octaves/
	 * Roughness/Lacunarity/Turbulence Strength): these change WHAT the raw pattern looks like, so -- exactly like Bounding
	 * Box axis parameters (OnAxisParamChangedDiscrete) and AO's Samples/Max Distance/Bias
	 * (InvalidateAODerivedMask) -- this invalidates every selected entry's NoiseMask (NOT NoiseRawCache
	 * itself; EnsureNoiseRawCache's own GeometryFingerprint+params comparison decides reuse lazily, the
	 * next time Noise is actually (re)generated) and always regenerates immediately.
	 */
	void OnNoiseGenerativeParamChanged();

	/** Same contract/range as BoundingBoxOpacity/AOOpacity/CurvatureMultiplier's own scale role -- UI
	 *  range [0, 10], default 1.0. ARTISTIC -- see OnNoiseArtisticParamChanged. */
	float NoiseMultiplier = 1.0f;

	/** V2-C: universal blur, applied to the raw procedural field BEFORE Multiplier/Levels/Invert (see
	 *  ComputeRawNoiseValue's seven-tap kernel). UI range [0, 1]; default 0.0. GENERATIVE (unlike
	 *  Multiplier right next to it in the UI) -- see OnNoiseGenerativeParamChanged. Positioned in the UI
	 *  immediately after Multiplier (Seed -> Multiplier -> Blur) per the explicit layout requirement,
	 *  even though it is evaluated FIRST in the actual pipeline. */
	float NoiseBlur = 0.0f;

	/** Same semantics as AOLevelsMin/CurvatureLevelsMin. UI range [0, 1]; default 0.0. ARTISTIC. */
	float NoiseLevelsMin = 0.0f;

	/** Same semantics as AOLevelsMax/CurvatureLevelsMax. UI range [0, 1]; default 1.0. ARTISTIC. */
	float NoiseLevelsMax = 1.0f;

	ECheckBoxState GetNoiseInvertState() const { return bNoiseInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** Applied LAST, after Levels Min/Max, over the already-leveled [0,1] result (Mask = 1 - Mask) --
	 *  same treatment as OnCurvatureInvertChanged/OnAOInvertChanged. Default false. ARTISTIC. */
	void OnNoiseInvertChanged(ECheckBoxState NewState);

	bool bNoiseInvert = false;

	/**
	 * Shared handler for Multiplier / Levels Min / Levels Max / Invert: ALL cheap, PURELY DOWNSTREAM
	 * reprocessing of the already-cached NoiseRawCache (see that field's own doc comment) -- never the
	 * per-vertex Perlin/FBM evaluation, never GeometryFingerprint, never the generative-params
	 * comparison. So this is UNCONDITIONAL and immediate, exactly
	 * like OnCurvatureParamChanged. Regenerates every SelectedMeshes entry's NoiseMask directly from its
	 * cached NoiseRawCache (entry-level, real values -- see FVertexMaskForgeWorkingMesh::NoiseMask),
	 * then recomposes.
	 */
	void OnNoiseArtisticParamChanged();

	TSharedRef<SWidget> OnGenerateNoiseBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const;
	void OnNoiseBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetNoiseBlendModeButtonText() const;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>> NoiseBlendModeComboBox;

	/** Same contract/range/default (Copy) as BoundingBoxBlendMode/AOBlendMode/CurvatureBlendMode --
	 *  preserves the established "every new layer defaults to Copy" convention. ARTISTIC (pure
	 *  composition, see RecomposeWorkingColors). */
	EVertexMaskForgeBlendMode NoiseBlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Same contract/range/default as BoundingBoxOpacity/AOOpacity/CurvatureOpacity, independent of
	 *  them. Applied ONLY during final composition (ComposeMaskStack), never to NoiseMask.Values itself.
	 *  ARTISTIC. */
	float NoiseOpacity = 1.0f;

	bool bNoiseEnabled = false;

	/**
	 * AUDITED (Noise V1): resets ONLY every selected entry's NoiseMask slot back to NotGenerated (never
	 * touches NoiseRawCache/NoiseCacheFingerprint/NoiseCacheUsedParams directly -- see NoiseRawCache's
	 * own doc comment for why that is safe: the next real generation re-derives reuse-or-recompute from
	 * the fingerprint+params comparison on its own). Same "raw/composition separation" contract as
	 * InvalidateBoundingBoxRawMask/InvalidateAODerivedMask -- called ONLY by generative parameter
	 * changes (OnNoiseGenerativeParamChanged), never by artistic ones.
	 */
	void InvalidateNoiseRawMask();

	// --- Material Slot Mask (V2-D) -----------------------------------------------------------
	// A fifth, independent, optional composition-stack layer -- a structural peer of Bounding Box, AO,
	// Curvature, and Noise (see EVertexMaskForgeScalarMaskSource::MaterialSlot). Transform-independent
	// like Curvature/Noise (generated once per entry, real values, no per-component re-evaluation).
	// V1 SCOPE: requires exactly one selected mesh (the dropdown represents "the slots of THAT mesh") --
	// see IsMaterialSlotMaskAvailableForSelection.

	ECheckBoxState GetMaterialSlotMaskEnableState() const { return bMaterialSlotMaskEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** Same enable/disable contract as OnCurvatureEnableChanged/OnNoiseEnableChanged: turning OFF is
	 *  always pure composition; turning ON reuses an already-Ready entry immediately and always
	 *  regenerates immediately if genuinely needed. */
	void OnMaterialSlotMaskEnableChanged(ECheckBoxState NewState);
	bool bMaterialSlotMaskEnabled = false;

	/** True only when exactly one entry is selected -- the V1 scope requirement (see this section's own
	 *  header comment). Never true for zero or multiple selected meshes, regardless of bMaterialSlotMaskEnabled. */
	bool IsMaterialSlotMaskAvailableForSelection() const { return SelectedMeshes.Num() == 1; }

	/** Short, user-facing reason Material Slot Mask is currently unavailable/invalid for the current
	 *  single-mesh selection (empty if available and valid). Never touches any other generator. */
	FText GetMaterialSlotMaskDiagnosticText() const;

	TSharedRef<SWidget> OnGenerateMaterialSlotRow(TSharedPtr<FVertexMaskForgeMaterialSlotInfo> InOption) const;
	void OnMaterialSlotSelectionChanged(TSharedPtr<FVertexMaskForgeMaterialSlotInfo> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetMaterialSlotButtonText() const;

	/** Rebuilt every RefreshSelection from the single selected entry's WorkingMesh.MaterialSlotOptions
	 *  (see ReconcileMaterialSlotSelection) -- never stale across a mesh/selection change. */
	TArray<TSharedPtr<FVertexMaskForgeMaterialSlotInfo>> MaterialSlotOptions;
	TSharedPtr<SComboBox<TSharedPtr<FVertexMaskForgeMaterialSlotInfo>>> MaterialSlotComboBox;

	/** Real index into GetStaticMaterials() for the currently selected mesh. GENERATIVE -- see
	 *  OnMaterialSlotMaskGenerativeParamChanged. Reconciled (never left stale/out-of-range) by
	 *  ReconcileMaterialSlotSelection every RefreshSelection. */
	int32 SelectedMaterialSlotIndex = 0;

	/**
	 * Shared handler for Material Slot Mask's generative parameters (which slot is selected, Invert --
	 * both change WHAT the raw binary mask looks like): invalidates the entry's MaterialSlotMask (reset
	 * to NotGenerated) and always regenerates immediately -- same contract as
	 * OnNoiseGenerativeParamChanged.
	 */
	void OnMaterialSlotMaskGenerativeParamChanged();
	void InvalidateMaterialSlotMaskRawMask();

	ECheckBoxState GetMaterialSlotMaskInvertState() const { return bMaterialSlotMaskInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnMaterialSlotMaskInvertChanged(ECheckBoxState NewState);
	bool bMaterialSlotMaskInvert = false;

	TSharedRef<SWidget> OnGenerateMaterialSlotMaskBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const;
	void OnMaterialSlotMaskBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetMaterialSlotMaskBlendModeButtonText() const;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>> MaterialSlotMaskBlendModeComboBox;

	/** Same "every new layer defaults to Copy" convention as every other generator. ARTISTIC (pure
	 *  composition, see RecomposeWorkingColors) -- never regenerates the raw mask. */
	EVertexMaskForgeBlendMode MaterialSlotMaskBlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Same contract/range/default as BoundingBoxOpacity/AOOpacity/CurvatureOpacity/NoiseOpacity,
	 *  independent of them. Applied ONLY during final composition (ComposeMaskStack), never to
	 *  MaterialSlotMask.Values itself. ARTISTIC. */
	float MaterialSlotMaskOpacity = 1.0f;

	/**
	 * AUDITED (V2-D): rebuilds MaterialSlotOptions (the dropdown's own list) from the single selected
	 * entry's WorkingMesh.MaterialSlotOptions, and validates/reconciles SelectedMaterialSlotIndex
	 * against it -- preserves the previous index if it still exists in the new list, otherwise falls
	 * back to the first available slot (index 0 of GetStaticMaterials()), never leaving a stale index
	 * from a different mesh selected. Called at the end of RefreshSelection, unconditionally (cheap:
	 * only touches this section's own UI-facing arrays, never any other generator's state). A zero- or
	 * multi-mesh selection clears MaterialSlotOptions to empty (IsMaterialSlotMaskAvailableForSelection
	 * already gates the UI/generation in that case).
	 */
	void ReconcileMaterialSlotSelection();

	// --- Directional Normal Mask (V2-E) --------------------------------------------------------
	// A sixth, independent, optional composition-stack layer -- structural peer of Bounding Box/AO/
	// Curvature/Noise/Material Slot. VISUAL panel position is Curvature -> Directional Normal -> Noise
	// (see Construct()); enum value is appended AFTER MaterialSlot to preserve numeric stability of
	// every existing EVertexMaskForgeScalarMaskSource value (the two orderings are independent, per the
	// explicit "position and enum are decoupled" allowance).

	ECheckBoxState GetDirectionalNormalMaskEnableState() const { return bDirectionalNormalMaskEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnDirectionalNormalMaskEnableChanged(ECheckBoxState NewState);
	bool bDirectionalNormalMaskEnabled = false;

	TSharedRef<SWidget> OnGenerateNormalSpaceRow(TSharedPtr<EVertexMaskForgeNormalSpace> InOption) const;
	void OnNormalSpaceSelectionChanged(TSharedPtr<EVertexMaskForgeNormalSpace> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetNormalSpaceButtonText() const;
	TArray<TSharedPtr<EVertexMaskForgeNormalSpace>> NormalSpaceOptions;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeNormalSpace>>> NormalSpaceComboBox;

	/** UI default World (per the explicit suggested default) -- GENERATIVE, see
	 *  OnDirectionalNormalMaskGenerativeParamChanged. */
	EVertexMaskForgeNormalSpace DirectionalNormalSpace = EVertexMaskForgeNormalSpace::Local;

	TSharedRef<SWidget> OnGenerateNormalDirectionRow(TSharedPtr<EVertexMaskForgeNormalDirection> InOption) const;
	void OnNormalDirectionSelectionChanged(TSharedPtr<EVertexMaskForgeNormalDirection> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetNormalDirectionButtonText() const;
	TArray<TSharedPtr<EVertexMaskForgeNormalDirection>> NormalDirectionOptions;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeNormalDirection>>> NormalDirectionComboBox;

	/** Default Z+ (Up), per the explicit suggested default. GENERATIVE. */
	EVertexMaskForgeNormalDirection DirectionalNormalDirection = EVertexMaskForgeNormalDirection::PositiveZ;

	/** Degrees, UI range [0, 180]; default 90. GENERATIVE. */
	float DirectionalNormalAngle = 90.0f;

	/** Degrees, UI range [0, 180]; default 45 -- internally clamped to [0, Angle] at generation time
	 *  (see ComputeDirectionalNormalRawValue), never causing a division by zero or NaN. GENERATIVE. */
	float DirectionalNormalFalloff = 45.0f;

	/**
	 * Topological smoothing of the raw Directional Normal Mask, applied BEFORE Invert -- same algorithm
	 * shape, range, default, and "whole number = full iterations, fractional part blends toward one
	 * more" semantics as CurvatureBlur (see ApplyTopologicalCurvatureBlur's own doc comment), adapted to
	 * a domain-appropriate adjacency (render-vertex index-buffer adjacency for non-Nanite, corner/
	 * triangle-neighbor adjacency for Source-Topology -- see VertexMaskForgePanel::
	 * ApplyAdjacencyTopologicalBlur for why CurvatureBlur's own Dynamic-Mesh-Vertex-ID adjacency
	 * cannot be reused directly without collapsing split normals at hard edges/UV seams). UI range
	 * [0, 10]; default 0.0 -- Blur <= 0 is an exact no-op (bit-for-bit identical to no Blur at all).
	 * GENERATIVE.
	 */
	float DirectionalNormalBlur = 0.0f;

	/**
	 * Shared handler for Directional Normal Mask's generative parameters (Space/Direction/Angle/
	 * Falloff -- all change WHAT the raw angular pattern looks like): invalidates the entry's
	 * DirectionalNormalMask and always regenerates immediately -- same contract as
	 * OnNoiseGenerativeParamChanged/OnMaterialSlotMaskGenerativeParamChanged. Never touches
	 * AO/Curvature/Noise/Material Slot state or caches.
	 */
	void OnDirectionalNormalMaskGenerativeParamChanged();
	void InvalidateDirectionalNormalMaskRawMask();

	ECheckBoxState GetDirectionalNormalMaskInvertState() const { return bDirectionalNormalMaskInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnDirectionalNormalMaskInvertChanged(ECheckBoxState NewState);
	bool bDirectionalNormalMaskInvert = false;

	TSharedRef<SWidget> OnGenerateDirectionalNormalMaskBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const;
	void OnDirectionalNormalMaskBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetDirectionalNormalMaskBlendModeButtonText() const;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>> DirectionalNormalMaskBlendModeComboBox;

	/** Same "every new layer defaults to Copy" convention as every other generator. ARTISTIC (pure
	 *  composition, see RecomposeWorkingColors) -- never regenerates the raw mask. */
	EVertexMaskForgeBlendMode DirectionalNormalMaskBlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Same contract/range/default as every other generator's own Opacity. Applied ONLY during final
	 *  composition, never to DirectionalNormalMask.Values itself. ARTISTIC. */
	float DirectionalNormalMaskOpacity = 1.0f;

	/** Short, user-facing reason Directional Normal Mask is currently invalid/blocked (empty if valid) --
	 *  e.g. degenerate normal, unresolved corner mapping, degenerate World transform, or a World-Space
	 *  multi-instance conflict (see FVertexMaskForgeWorkingMesh::bDirectionalNormalWorldSpaceConflict). */
	FText GetDirectionalNormalMaskDiagnosticText() const;

	// --- Thickness Mask (V2-G) ---------------------------------------------------------------
	// A seventh, independent, optional composition-stack layer -- structural peer of every generator
	// above. VISUAL panel position is Directional Normal -> Thickness -> Noise (see Construct()); enum
	// value is appended after DirectionalNormal (see EVertexMaskForgeScalarMaskSource's own doc note).
	// Asset Local Space ONLY -- never a seletor, never depends on Component/Actor Transform.

	ECheckBoxState GetThicknessMaskEnableState() const { return bThicknessMaskEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnThicknessMaskEnableChanged(ECheckBoxState NewState);
	bool bThicknessMaskEnabled = false;

	TSharedRef<SWidget> OnGenerateThicknessMaskBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const;
	void OnThicknessMaskBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetThicknessMaskBlendModeButtonText() const;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>> ThicknessMaskBlendModeComboBox;

	/** Same "every new layer defaults to Copy" convention as every other generator. ARTISTIC. */
	EVertexMaskForgeBlendMode ThicknessMaskBlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Same contract/range/default as every other generator's own Opacity. ARTISTIC. */
	float ThicknessMaskOpacity = 1.0f;

	/** Measured thickness (local-space units) at or below this value reads as white (1.0) after
	 *  normalization, before Invert. UI range [0, 10000]; default 0.0. GENERATIVE (renormalizes only --
	 *  never triggers a new raycast, see GenerateThicknessMask's own cache contract). */
	float ThicknessMinThickness = 0.0f;

	/** Measured thickness at or above this value reads as black (0.0) after normalization, before
	 *  Invert. UI range [0, 10000]; default 50.0 -- strictly below the default SearchDistance (100.0) so
	 *  the initial range is a genuinely useful, non-degenerate interval (not merely "corrected" by
	 *  SanitizeThicknessParams at generation time) and the raycast retains headroom beyond the visible
	 *  saturation point. Sanitized together with MinThickness/SearchDistance/Bias at generation time --
	 *  see VertexMaskForgePanel::SanitizeThicknessParams. GENERATIVE (renormalizes only). */
	float ThicknessMaxThickness = 50.0f;

	/** Maximum physical raycast distance from the origin surface, in local-space units -- distinct from
	 *  MaxThickness (the artistic saturation point): a hit beyond MaxThickness but within SearchDistance
	 *  is still a valid "black" measurement, not a missing one. UI range [0, 10000]; default 100.0.
	 *  GENERATIVE (triggers a new raycast -- see GenerateThicknessMask's own cache contract). */
	float ThicknessSearchDistance = 100.0f;

	/** Ray origin offset along -Normal (into the mesh), local-space units, used ONLY to avoid self-hit;
	 *  reconstructed back out of the measured distance (MeasuredThickness = HitT + EffectiveBias) so it
	 *  never artistically shifts the result -- see ComputeThicknessRawValue. UI range [0.001, 10.0];
	 *  default 0.01. GENERATIVE (triggers a new raycast). */
	float ThicknessBias = 0.01f;

	/** Topological smoothing of the normalized Thickness mask, applied BEFORE Invert -- same algorithm/
	 *  adjacency/seam-awareness as DirectionalNormalBlur (reuses BuildCornerAdjacency/
	 *  BuildRenderVertexAdjacency/ApplyAdjacencyTopologicalBlur verbatim). UI range [0, 10]; default 0.0
	 *  -- Blur <= 0 is an exact no-op. GENERATIVE (post-processing only -- never triggers a new raycast).
	 */
	float ThicknessBlur = 0.0f;

	/**
	 * Shared handler for Thickness's raycast-affecting generative parameters (SearchDistance/Bias):
	 * invalidates the entry's ThicknessMask (raw hit distances + everything downstream) and always
	 * regenerates immediately. Never touches AO/Curvature/Noise/Material Slot/Directional Normal state
	 * or caches.
	 */
	void OnThicknessRaycastParamChanged();

	/** Shared handler for Min/Max Thickness/Blur (post-raycast only -- renormalizes/re-blurs the
	 *  already-cached raw hit distances, never repeats the raycast). */
	void OnThicknessPostProcessParamChanged();

	void InvalidateThicknessMaskRawMask();

	ECheckBoxState GetThicknessMaskInvertState() const { return bThicknessMaskInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnThicknessMaskInvertChanged(ECheckBoxState NewState);
	bool bThicknessMaskInvert = false;

	/** Short, user-facing reason Thickness Mask is currently invalid/blocked/partial (empty if fully
	 *  valid) -- e.g. no opposite surface found, degenerate geometry, Source-Topology mapping invalid. */
	FText GetThicknessMaskDiagnosticText() const;

	// --- Fill White / Fill Black utility masks ----------------------------------------------

	FReply OnFillWhiteClicked();
	FReply OnFillBlackClicked();

	/**
	 * Shared implementation for both Fill buttons: cancels any pending live-update debounce first (a
	 * Fill must never be overwritten moments later by a stale regeneration), then, for every
	 * SelectedMeshes entry that passes the SAME entry-level validity gating as live generation
	 * (WorkingMesh Ready, resolvable Static Mesh, valid LOD 0 render data), generates a dense
	 * constant-valued mask (VertexMaskForgePanel::GenerateConstantMask) and assigns it to that
	 * entry's mask. An entry that fails validation here is left COMPLETELY UNTOUCHED (its previous
	 * mask, if any, is preserved) rather than reset to Unavailable -- per the explicit "preserve the
	 * last valid Preview on failure" requirement. Ends with UpdateAllPreviews(), which recomposes/
	 * reapplies the transient Preview (reusing the exact same ApplyPreviewToEntry/UpdateWorkingColors
	 * path as every other mask) and marks Pending Changes via RecomputeOperationState(). Fill White/
	 * Black are explicit, standalone actions on the current base -- they are never required to make a
	 * live parameter change visible, and never repair/resize any buffer that live generation itself
	 * should already have prepared.
	 */
	void RunConstantFill(float ConstantValue, EVertexMaskForgeScalarMaskSource Source, const FText& SuccessMessage);

	/** Enabled only when at least one selected entry has a Ready working mesh, and not while Applying. */
	bool CanRunFill() const;

	FText GetMaskActionStatusText() const { return LastMaskActionStatusText; }

	/** Success/partial-failure message for the last Fill action; cleared by the next mask-changing
	 *  action (Fill, parameter change, Refresh Selection, Accept, Cancel). */
	FText LastMaskActionStatusText;

	// --- Live preview update (debounced automatic regeneration) ----------------------------

	/**
	 * Arms (or restarts) a short one-shot debounce timer via GEditor's FTimerManager -- not a
	 * per-frame Tick override -- so a burst of slider events (SSpinBox fires OnValueChanged
	 * continuously while dragging) collapses into a single regeneration ~150ms after the LAST
	 * event. Calling FTimerManager::SetTimer() again with the same FTimerHandle before it fires
	 * clears and re-adds it (confirmed in TimerManager.cpp), which is exactly "a new change resets
	 * the wait" -- the LAST value received before the timer fires is always what gets published, never
	 * an intermediate one. Uses FTimerDelegate::CreateSP (weak-safe: a pending timer harmlessly no-ops
	 * if this widget is destroyed first), but the timer is also explicitly cleared in
	 * OnWorldCleanup() and the destructor rather than relying on that alone. This is the tool's ONLY
	 * automatic-regeneration path -- there is no manual "Generate Mask" alternative; every parameter
	 * change either regenerates immediately (cheap/discrete changes) or arms this debounce (expensive/
	 * continuous changes), unconditionally.
	 */
	void ScheduleAutoUpdatePreview();

	/**
	 * Regenerates every enabled generator's mask for every eligible entry using the CURRENT
	 * parameters -- this IS the tool's single live-regeneration entry point (there is no separate
	 * manual "Generate Mask" action; every parameter-change callback in this panel funnels here,
	 * either immediately or via ScheduleAutoUpdatePreview's debounce). On a per-entry, per-generator
	 * basis: if the new mask comes back Ready, it replaces the entry's mask; otherwise
	 * (DegenerateBounds/Invalid/Unavailable, including "generator disabled") the entry's EXISTING mask
	 * is left untouched -- regeneration must never destroy a valid Preview on a transient failure.
	 * Never runs while Applying (guarded defensively; not reachable in practice since Accept is
	 * synchronous). Called by the debounce timer, immediately for discrete parameter changes
	 * (Enable/Invert/Mirror/World Space/raycast-affecting parameters), and once from RefreshSelection()
	 * when the newly selected entry/entries already have at least one generator enabled.
	 *
	 * bIncludeAO (AUDITED, BBox Invert exception -- default true, preserves all pre-existing call
	 * sites' behavior unchanged): when false, the Ambient Occlusion slot (AmbientOcclusionMask) is
	 * left COMPLETELY untouched for every entry -- not re-validated, not cleared, not re-snapshotted.
	 * Used exclusively by OnAxisInvertChanged (BBox per-axis Invert's immediate-regeneration
	 * exception), so that regenerating BoundingBoxMask immediately can never have any observable
	 * effect -- not even a harmless entry-level re-validation -- on Ambient Occlusion.
	 */
	void RunAutoUpdatePreview(bool bIncludeAO = true);

	FTimerHandle AutoUpdateDebounceTimerHandle;

	// --- Unified Bounds (global, all 3 axes, all selected meshes) --------------------------

	ECheckBoxState GetUnifiedBoundsState() const { return bUseUnifiedBounds ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/**
	 * Toggling Unified Bounds never recomputes just one mesh: it cancels any pending debounce,
	 * invalidates every entry's current Bounding-Box-sourced mask (InvalidateBoundingBoxRawMask()), and
	 * unconditionally immediately regenerates every eligible entry as one coherent batch
	 * (RunAutoUpdatePreview(), which itself computes the collective domain once, if applicable, and
	 * reuses it for every participant).
	 */
	void OnUnifiedBoundsChanged(ECheckBoxState NewState);

	/**
	 * False (default) preserves the tool's previously-validated behavior exactly: each component
	 * normalizes its own render vertices against its OWN individual per-axis bounds (see
	 * VertexMaskForgePanel::GenerateBoundingBoxMask's internal bounds pass). True: every enabled
	 * axis is normalized against a COLLECTIVE domain -- the union of that axis's coordinate across
	 * every participating component's render vertices (see VertexMaskForgePanel::
	 * ComputeCollectiveAxisBounds) -- computed fresh before each batch (every live regeneration,
	 * Preview refresh, and Accept validation), never cached across calls,
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

	FText GetPreviewStatusText() const;

	/**
	 * Applies or restores preview visualization for every selected entry, based on the current
	 * CurrentPreviewMode / Channel Filter / each entry's BoundingBoxMask state. Idempotent and
	 * side-effect-free with respect to the mask itself -- never generates or invalidates it.
	 *
	 * bCommit (audited, Channel Filter toggle fix): forwarded unchanged to ApplyPreviewToEntry /
	 * UpdateWorkingColors -- true ONLY for an explicit Fill White/Black action (promotes the
	 * freshly-composed WorkingColors to CommittedColors); false for every other trigger (live
	 * regeneration, Channel Filter toggle, Preview Mode change, RefreshSelection, mask invalidation)
	 * so none of those can silently consolidate a transient edit. See
	 * FVertexMaskForgePreviewComponentState::CommittedColors' own doc comment for the full contract.
	 */
	void UpdateAllPreviews(bool bCommit);

	/**
	 * Applies or restores preview for one entry, per current mode/filter/mask state.
	 * CollectiveBoundsPtr is non-null only when bUseUnifiedBounds is on AND UpdateAllPreviews()
	 * successfully computed a collective domain for this batch -- passed through unchanged to every
	 * per-component GenerateBoundingBoxMask() re-evaluation (see the audit note there); nullptr means
	 * "use each component's own individual bounds" (bUseUnifiedBounds off, or this entry's mask
	 * Source isn't BoundingBox, in which case it's simply unused). bCommit: see UpdateAllPreviews'
	 * own doc comment; forwarded unchanged to UpdateWorkingColors.
	 */
	void ApplyPreviewToEntry(
		const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry,
		const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>* CollectiveBoundsPtr,
		bool bCommit);

	/** Restores original materials and vertex colors on every tracked component of one entry --
	 *  session-end variant (resets BaselineColors/CommittedColors/WorkingColors/AOCache too). Idempotent.
	 *  Only called from DestroyAllPreviews() (Cancel, Accept, RefreshSelection, World cleanup) -- a
	 *  genuine session end. See RestorePreviewForEntryVisualOnly for the mid-session/compositional case. */
	void RestorePreviewForEntry(FVertexMaskForgeSelectedMesh& Entry);

	/** AUDITED (raw/composition separation checkpoint): visual-only restore on every tracked component
	 *  of one entry -- BaselineColors/CommittedColors/WorkingColors/AOCache are left completely
	 *  untouched (see VertexMaskForgePanel::RestorePreviewVisualOnly). Used by ApplyPreviewToEntry's
	 *  own early-return paths (Preview Mode == Original Material, working mesh not Ready, Mesh/
	 *  RenderData/debug material failed to resolve) -- none of these are a session end or a genuine
	 *  geometric invalidation, so the AO geometry cache must survive them untouched. */
	void RestorePreviewForEntryVisualOnly(FVertexMaskForgeSelectedMesh& Entry);

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

	/** Recomputes OperationState (Idle/PendingChanges only -- see enum doc) from current data: at
	 *  least one entry with an active PreviewComponent and a Ready mask, independent of the current
	 *  Preview Mode (Preview Mode only controls presentation, never persistence -- see
	 *  ApplyPreviewToEntry's bUseOriginalMaterials). Never touches LastOperationErrorText (callers
	 *  that need to report a failure set it themselves, AFTER calling anything that ends in this, so a
	 *  fresh message from this same pass is never clobbered). No-ops while Applying. */
	void RecomputeOperationState();

	bool CanAcceptChanges() const { return OperationState == EVertexMaskForgeOperationState::PendingChanges; }
	FReply OnAcceptChangesClicked();

	/**
	 * True if any currently selected entry's already-resolved Static Mesh has Nanite enabled
	 * (UStaticMesh::IsNaniteEnabled()). Cheap: reads Entry->Mesh.Get() only (never forces a
	 * synchronous load) -- by the time an entry exists in SelectedMeshes, RefreshSelection/
	 * BuildWorkingMeshes has already resolved it, so this is a pointer check, not an asset load.
	 */
	bool HasNaniteMeshInSelection() const;

	/** Visible only while HasNaniteMeshInSelection() is true -- shown next to the Accept row, explains
	 *  that Accept for a Nanite-enabled mesh writes to the Source Static Mesh asset and affects every
	 *  instance using it. */
	EVisibility GetNaniteNoticeVisibility() const { return HasNaniteMeshInSelection() ? EVisibility::Visible : EVisibility::Collapsed; }

	/** AUDITED (UX1): Cancel must be able to end ANY active session, not only one with a Ready mask to
	 *  discard -- e.g. a session started via "Edit Vertex Mask" where no generator was enabled yet
	 *  still has WorkingMeshes/baseline state that Cancel should be able to walk away from. Gated on
	 *  bIsEditingVertexMask (the session flag) rather than OperationState. */
	bool CanCancelChanges() const { return bIsEditingVertexMask; }
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

	/** See OnEditorSelectionChanged's doc comment. Registered/removed the same way as, and for the
	 *  same reason as, WorldCleanupDelegateHandle above. */
	FDelegateHandle SelectionChangedDelegateHandle;

	/**
	 * AUDITED (V2-E corrective pass, transform freshness): GEngine->OnActorMoved() -- the engine's own
	 * official notification that an Actor's viewport gizmo move/rotate/scale has FINISHED (confirmed by
	 * reading AActor::PostEditMove(bFinished=true) in ActorEditor.cpp, which unconditionally calls
	 * GEngine->BroadcastOnActorMoved(this) -- the same call chain LevelEditorViewport.cpp's own
	 * TrackingStopped path uses when a gizmo drag completes). Fires ONCE per completed drag (never
	 * continuously mid-drag, so no extra debounce is needed beyond the existing live-regeneration
	 * mechanism), for ANY actor moved anywhere in the level -- OnActorMovedForDirectionalNormal filters
	 * to only the actors actually relevant to this panel's own tracked components. Registered/removed
	 * the same way as WorldCleanupDelegateHandle/SelectionChangedDelegateHandle above.
	 */
	void OnActorMovedForDirectionalNormal(AActor* Actor);

	FDelegateHandle ActorMovedDelegateHandle;

	/**
	 * Centralized per-Actor hide ref-counting for the whole panel (not per-entry), since components
	 * from different FVertexMaskForgeSelectedMesh entries can share the same owning Actor. See
	 * FVertexMaskForgeActorHideState.
	 */
	TMap<TWeakObjectPtr<AActor>, FVertexMaskForgeActorHideState> ActorHideStates;

	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>> SelectedMeshes;

	// Preview Mode / Channel Filter are panel/session-transient: never saved on the asset, never
	// invalidate or regenerate BoundingBoxMask, and are fully independent of one another.
	EVertexMaskForgePreviewMode CurrentPreviewMode = EVertexMaskForgePreviewMode::OriginalMaterial;
	TArray<TSharedPtr<EVertexMaskForgePreviewMode>> PreviewModeOptions;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgePreviewMode>>> PreviewModeComboBox;

	bool bChannelFilterR = true;
	bool bChannelFilterG = true;
	bool bChannelFilterB = true;

	/** Resolved lazily via GetPreviewDebugMaterial(); weak because it is an asset the plugin does not own. */
	TWeakObjectPtr<UMaterialInterface> PreviewDebugMaterial;
};
