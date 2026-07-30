#pragma once

#include "CoreMinimal.h"
#include "VertexMaskForgeWorkingColorsProvenance.h"
#include "VertexMaskForgeWorkingMeshOwner.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

class UStaticMesh;
class UStaticMeshComponent;

/**
 * M16-J.0A / M16-J.0A.1: headless owner of ONE component's color lifecycle -- exactly one instance per
 * FVertexMaskForgePreviewComponentState (one placed UStaticMeshComponent). Owns BaselineColors/
 * CommittedColors/WorkingColors and the alpha Authority over them; ATTACHES to (never owns, never
 * copies) the FVertexMaskForgeWorkingMeshOwner of its own FVertexMaskForgeSelectedMesh entry -- see that
 * class's own module comment for why geometry ownership was split out.
 *
 * IMPORTANT (M16-J.0A.1 vs M16-J.0B): this class is still NOT wired into SVertexMaskForgePanel in this
 * checkpoint. Connecting the panel's real call sites to this owner (and to FVertexMaskForgeWorkingMeshOwner)
 * remains M16-J.0B's job.
 *
 * RESPONSIBILITIES: component identity, an internally-controlled color-lifecycle "initialized" state,
 * BaselineColors/CommittedColors/WorkingColors, and the authoritative alpha metadata over WorkingColors.
 * Generation is NEVER owned here -- it belongs exclusively to the attached FVertexMaskForgeWorkingMeshOwner;
 * this class only ever COPIES (never advances) that owner's current generation into its own Authority at
 * InitializeColors time.
 *
 * DELIBERATELY NOT this class's responsibility: FVertexMaskForgeWorkingMesh itself (owned by
 * FVertexMaskForgeWorkingMeshOwner), InstanceResults, mask generation, RGB composition/ComposeMaskStack,
 * FVector3f->FColor conversion, visual application, persistence, Slate, viewport selection, Preview Mode,
 * Channel Filter.
 *
 * HEADLESS: no dependency on Slate, SVertexMaskForgePanel, global selection, viewport, render-state
 * invalidation, persistence, async/threads/locks.
 *
 * LIFETIME: non-copyable, non-movable. Its own address is used as StateOwnerId. Attaches to a mesh owner
 * via TWeakPtr<FVertexMaskForgeWorkingMeshOwner> (see AttachToMeshOwner) -- never a raw pointer, so this
 * class can never observe a destroyed mesh owner.
 */
class FVertexMaskForgeWorkingStateOwner
{
public:
	FVertexMaskForgeWorkingStateOwner() = default;
	~FVertexMaskForgeWorkingStateOwner() = default;

	FVertexMaskForgeWorkingStateOwner(const FVertexMaskForgeWorkingStateOwner&) = delete;
	FVertexMaskForgeWorkingStateOwner& operator=(const FVertexMaskForgeWorkingStateOwner&) = delete;
	FVertexMaskForgeWorkingStateOwner(FVertexMaskForgeWorkingStateOwner&&) = delete;
	FVertexMaskForgeWorkingStateOwner& operator=(FVertexMaskForgeWorkingStateOwner&&) = delete;

	/**
	 * Registers WHICH real UStaticMeshComponent this owner's color state belongs to -- the per-COMPONENT
	 * identity axis (Static Mesh/LOD/domain are per-ENTRY axes, owned by the attached
	 * FVertexMaskForgeWorkingMeshOwner instead -- see that class's own ConfigureIdentity).
	 *
	 * Repeated calls with the SAME component are a no-op. A call with a DIFFERENT component (or the
	 * first call) invalidates any currently-initialized colors/authority. Does not itself touch the
	 * mesh-owner attachment.
	 *
	 * Returns false (leaving the owner unconfigured) if Component is null or this owner has been
	 * permanently invalidated by generation overflow.
	 */
	bool ConfigureTarget(UStaticMeshComponent* Component);

	bool IsTargetConfigured() const { return bTargetConfigured; }

	/**
	 * Attaches this owner to InMeshOwner -- the FVertexMaskForgeWorkingMeshOwner of this owner's own
	 * FVertexMaskForgeSelectedMesh entry. Stored internally as a TWeakPtr (never a raw pointer, never a
	 * copy of the mesh owner) -- see this class's own module comment on why. The caller supplies the
	 * real, already-existing mesh owner handle; it can never fabricate MeshOwnerId, generation,
	 * provenance, or authority through this call.
	 *
	 * Attaching to a DIFFERENT mesh owner than the one currently attached (or attaching for the first
	 * time) invalidates any currently-initialized colors/authority -- an Authority established against
	 * one mesh owner is never carried over to a different one.
	 *
	 * Returns false if InMeshOwner is null/expired.
	 */
	bool AttachToMeshOwner(const TSharedPtr<FVertexMaskForgeWorkingMeshOwner>& InMeshOwner);

	/** Detaches from the current mesh owner (if any) and invalidates colors/authority. Safe to call when
	 *  already detached (no-op). Does not touch target component configuration. */
	void DetachFromMeshOwner();

	bool IsAttached() const { return AttachedMeshOwner.IsValid(); }

	/**
	 * Authoritatively initializes BaselineColors/CommittedColors/WorkingColors from CapturedColors. All
	 * three arrays are seeded identically, verbatim, byte-for-byte.
	 *
	 * Requires IsTargetConfigured(), IsAttached(), and the attached mesh owner's own Working Mesh
	 * Provenance to be valid (i.e. FVertexMaskForgeWorkingMeshOwner::InstallWorkingMesh has already
	 * succeeded for it) -- returns false otherwise, leaving any prior color state untouched. Requires
	 * CapturedColors.Num() == the attached mesh owner's own Provenance::ExpectedCardinality.
	 *
	 * On success, establishes WorkingColorsAuthority stamped with SourceStaticMesh/domain/LOD/Revision
	 * COPIED VERBATIM from the attached mesh owner's own current Provenance (never independently
	 * chosen), plus this owner's own StateOwnerId and the attached mesh owner's own MeshOwnerId, and
	 * Cardinality == CapturedColors.Num().
	 */
	bool InitializeColors(TArray<FColor> CapturedColors);

	bool AreColorsInitialized() const { return bColorsInitialized; }

	/** Sets every element's R/G/B to 0, preserving Alpha byte-identical, then consolidates
	 *  (CommittedColors = WorkingColors). See this class's own AUDITED note: a deliberately MINIMAL,
	 *  self-contained analog of the real panel's Fill Black (which composes through ComposeMaskStack) --
	 *  not a replacement for it. Requires AreColorsInitialized(). */
	bool FillBlack();

	/** Sibling of FillBlack -- sets every element's R/G/B to 255. */
	bool FillWhite();

	/** Overwrites WorkingColors' R/G/B channels from NewRGB (Alpha preserved byte-identical), then
	 *  consolidates. Requires NewRGB.Num() == WorkingColors.Num(). */
	bool UpdateWorkingColorsRGB(TArrayView<const FColor> NewRGB);

	/** Restores WorkingColors from BaselineColors (byte-for-byte). Deliberately does NOT consolidate --
	 *  CommittedColors is left untouched (see FillBlack/FillWhite's own consolidating contract, which
	 *  this intentionally does not share). Requires AreColorsInitialized(). */
	bool RestoreFromBaseline();

	/** Restores WorkingColors from CommittedColors. Same non-consolidating, internal-buffer-only
	 *  contract as RestoreFromBaseline. Requires AreColorsInitialized(). */
	bool RestoreFromCommitted();

	/**
	 * Invalidates colors/authority (cleared/reset to invalid) without touching target configuration or
	 * mesh-owner attachment. Any Binding/Authority captured before Reset can never validate again.
	 */
	bool Reset();

	/**
	 * Builds a weak-referencing Binding to the CURRENTLY ATTACHED mesh owner. No parameters, so there is no
	 * way to substitute it with an object belonging to a different owner. Produces a not-well-formed
	 * Binding if not currently attached, or if the attached mesh owner has since been destroyed
	 * (TWeakPtr::Pin() fails). Safe to store indefinitely (see FVertexMaskForgeWorkingColorPublicationBinding's
	 * own module comment) -- M16-J.0B.1 2nd corrective pass: no longer carries a raw pointer to this owner's
	 * own PreviewComponentState at all, so validating it later never risks dereferencing this owner after
	 * it has been destroyed -- see ValidateBinding()/ValidateWorkingColorsPublicationBinding for how the
	 * target is supplied instead.
	 */
	FVertexMaskForgeWorkingColorPublicationBinding CreateBinding() const;

	/** Runs VertexMaskForgeWorkingColorsProvenance::ValidateWorkingColorsPublicationBinding against
	 *  CreateBinding()'s own result, paired with THIS owner's own live PreviewState (never a stored/stale
	 *  reference -- supplied fresh, in this same call, since `this` is guaranteed alive for its duration).
	 *  Read-only; never mutates any state. */
	EVertexMaskForgeWorkingColorsPublicationValidationStatus ValidateBinding() const;

	/**
	 * M16-J.0B.1 2nd corrective pass: validates an EXPLICITLY-SUPPLIED Binding (e.g. one captured earlier,
	 * possibly from a StateOwner that no longer exists) against THIS owner's own live PreviewState. This is
	 * the seam a caller uses to safely check an old Binding without ever needing (or being able) to
	 * dereference whatever StateOwner originally created it -- the Binding itself never carried a reference
	 * to that StateOwner's data in the first place.
	 */
	EVertexMaskForgeWorkingColorsPublicationValidationStatus ValidateBindingAgainstThis(const FVertexMaskForgeWorkingColorPublicationBinding& ExternalBinding) const;

	/**
	 * AUDITED (M16-J.0A.1): validates this owner's OWN PreviewComponentState/Authority against a
	 * DIFFERENT, explicitly-supplied mesh owner's Working Mesh -- without changing this owner's own
	 * attachment. A real, legitimate diagnostic (e.g. proving a reassociation would be safe before
	 * committing to it), not a test-only backdoor: it only ever accepts a real, already-existing
	 * FVertexMaskForgeWorkingMeshOwner, never raw pointers to its internals.
	 *
	 * M16-J.0B.1 corrective pass: takes a TSharedPtr (was a raw `const&`) so a TWeakPtr can be derived from
	 * it for the resulting Binding -- see FVertexMaskForgeWorkingColorPublicationBinding's own doc comment
	 * on why the Binding never stores a raw pointer to the owner's geometry.
	 */
	EVertexMaskForgeWorkingColorsPublicationValidationStatus ValidateAgainst(const TSharedPtr<FVertexMaskForgeWorkingMeshOwner>& OtherMeshOwner) const;

	const TArray<FColor>& GetBaselineColors() const { return bUseSourceTopologyMirror ? PreviewState.GetSourceTopologyBaselineColors() : PreviewState.GetBaselineColors(); }
	const TArray<FColor>& GetCommittedColors() const { return bUseSourceTopologyMirror ? PreviewState.GetSourceTopologyCommittedColors() : PreviewState.GetCommittedColors(); }
	const TArray<FColor>& GetWorkingColors() const { return bUseSourceTopologyMirror ? PreviewState.GetSourceTopologyWorkingColors() : PreviewState.GetWorkingColors(); }

	/** Read-only access to the internal preview component state. BaselineColors/CommittedColors/
	 *  WorkingColors/SourceTopology* variants/WorkingColorsAuthority/InstanceResults are PRIVATE fields
	 *  of FVertexMaskForgePreviewComponentState itself (friended only to this class) -- callers reach
	 *  them only through that struct's own public const getters (GetBaselineColors(), etc.), never by
	 *  direct field access, even through this const reference. */
	const FVertexMaskForgePreviewComponentState& GetPreviewState() const { return PreviewState; }

	/**
	 * M16-J.0B (rejection-corrective pass): mutable access to ONLY this owner's panel-owned visual/
	 * session bookkeeping (PreviewComponent, SourceTopologyPreviewComponent, AOCache,
	 * SourceTopologyAOCache, HiddenOwner, bOverrideActive, bHasAcquiredActorHide) -- these remain plain
	 * PUBLIC fields of FVertexMaskForgePreviewComponentState (never part of the authenticated contract,
	 * explicitly excluded from it per this checkpoint's own scope restrictions). This method DOES return
	 * a mutable reference to the whole struct, but that is no longer a meaningful capability grant for
	 * the authenticated fields: SourceComponent/BaselineColors/CommittedColors/WorkingColors/SourceTopology*
	 * variants/WorkingColorsAuthority/InstanceResults are PRIVATE on that struct, friended only to this
	 * class -- the compiler, not a naming convention or a doc comment, refuses any external write to them
	 * even through this reference. There is no other way to reach AOCache/PreviewComponent/etc from
	 * outside this owner (no per-field setter exists for each -- that would just be more surface for the
	 * same, already-narrow set of legitimate panel responsibilities), so this remains the minimal real API
	 * for that specific, out-of-contract category.
	 */
	FVertexMaskForgePreviewComponentState& GetVisualSessionStateMutable() { return PreviewState; }

	/**
	 * M16-J.0B (2nd rejection-corrective pass): idempotent baseline capture. If AreColorsInitialized() is
	 * already true, this is a no-op that returns true immediately (CapturedColors is discarded) --
	 * mirrors the real UpdateWorkingColors/UpdateWorkingColorsSourceTopology's own "BaselineColors.Num()
	 * != NumRenderVerts" one-time-capture gate: under this owner's own invalidation discipline,
	 * !AreColorsInitialized() is true in EXACTLY the same cases that gate would have been (never
	 * captured yet, or invalidated by a real rebuild/reattach/reset), so this is behaviorally equivalent,
	 * not merely similar.
	 *
	 * When colors are NOT yet initialized, delegates to InitializeColors(CapturedColors) -- same
	 * cardinality/attachment/Provenance requirements, same Authority-stamping contract. The caller
	 * supplies the already-read effective original bytes (from InstanceOverrideColors/AssetRenderColors/
	 * white, or the color overlay for Source-Topology) -- this owner never reads render data itself,
	 * staying headless.
	 */
	bool EnsureBaselineCaptured(TArray<FColor> CapturedColors);

	enum class EColorCommitMode : uint8
	{
		/** WorkingColors becomes FinalRGB (Alpha forced from Baseline); CommittedColors is NOT touched --
		 *  ordinary live recomposition, Channel Filter toggle, Preview Mode, Auto Update. */
		PreviewOnly,

		/** WorkingColors becomes FinalRGB (Alpha forced from Baseline); CommittedColors is ALSO promoted
		 *  to match -- the real panel's own bCommit == true path, exclusively a Fill Black/White action. */
		Consolidate,
	};

	/**
	 * M16-J.0B (2nd rejection-corrective pass, replaces the removed GetComposedColorArraysForRealPipeline()/
	 * CommitComposedColors()): the real artistic composition pipeline (UpdateWorkingColors/
	 * UpdateWorkingColorsSourceTopology, still unchanged in what math they perform) now reads Baseline/
	 * Committed only through this owner's own const getters (GetBaselineColors()/GetCommittedColors()),
	 * computes a fresh, independent FinalRGB array LOCALLY (never touching this owner's storage while
	 * computing), and hands the FINISHED result here, by value -- once this call returns, the caller
	 * retains no reference into this owner's internal storage at all, mutable or otherwise.
	 *
	 * This operation, NOT the caller, is what:
	 *   - validates FinalRGB.Num() against the live Baseline cardinality (rejects on mismatch, no partial
	 *     write -- returns false, WorkingColors/CommittedColors left exactly as they were);
	 *   - preserves Alpha byte-identical -- every element's Alpha is OVERWRITTEN from this owner's own
	 *     current Baseline right here, unconditionally, regardless of whatever Alpha byte FinalRGB
	 *     happened to carry (the caller's Alpha values are simply never read);
	 *   - applies WorkingColors = FinalRGB (with Alpha so overwritten);
	 *   - promotes CommittedColors = WorkingColors ONLY when CommitMode == Consolidate -- the one
	 *     explicitly-named, caller-selected transition, never inferred from arbitrary array content;
	 *   - re-derives Authority as a consequence of a successful, validated application -- never accepted
	 *     as a parameter, never possible to skip or spoof.
	 *
	 * Requires AreColorsInitialized() (i.e. EnsureBaselineCaptured succeeded at least once for the
	 * current generation) -- returns false otherwise.
	 */
	bool ApplyComposedColorsRGB(TArray<FColor> FinalRGB, EColorCommitMode CommitMode);

private:
	TArray<FColor>& MutableWorkingColors() { return bUseSourceTopologyMirror ? PreviewState.SourceTopologyWorkingColors : PreviewState.WorkingColors; }
	TArray<FColor>& MutableBaselineColors() { return bUseSourceTopologyMirror ? PreviewState.SourceTopologyBaselineColors : PreviewState.BaselineColors; }
	TArray<FColor>& MutableCommittedColors() { return bUseSourceTopologyMirror ? PreviewState.SourceTopologyCommittedColors : PreviewState.CommittedColors; }

	void InvalidateColors();

	bool bPermanentlyInvalid = false;

	bool bTargetConfigured = false;
	TWeakObjectPtr<UStaticMeshComponent> ConfiguredComponent;

	TWeakPtr<FVertexMaskForgeWorkingMeshOwner> AttachedMeshOwner;

	/** Mirrors the attached mesh owner's own domain flag at the moment colors were last (re)initialized
	 *  -- purely a local convenience for GetBaselineColors()/GetWorkingColors()/etc. to select the right
	 *  underlying array; never itself part of any identity comparison (Authority::GetUseSourceTopology()
	 *  is the authoritative copy used by the validator). */
	bool bUseSourceTopologyMirror = false;

	FVertexMaskForgePreviewComponentState PreviewState;
	bool bColorsInitialized = false;
};
