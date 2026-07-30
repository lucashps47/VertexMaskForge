#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UStaticMesh;
class UStaticMeshComponent;
struct FVertexMaskForgeWorkingMesh;
struct FVertexMaskForgePreviewComponentState;
class FVertexMaskForgeWorkingMeshOwner;
class FVertexMaskForgeWorkingStateOwner;

/**
 * M16-J.0 / M16-J.0A / M16-J.0A.1: persistent PROVENANCE identity for a FVertexMaskForgeWorkingMesh
 * (owned exclusively by FVertexMaskForgeWorkingMeshOwner -- one per FVertexMaskForgeSelectedMesh entry),
 * explicit ALPHA AUTHORITY for a FVertexMaskForgePreviewComponentState's own Working Colors buffer
 * (owned exclusively by FVertexMaskForgeWorkingStateOwner -- one per component/PreviewComponentState), a
 * transient non-owning BINDING that pairs exactly one of each, and a read-only VALIDATION function that
 * proves (or disproves) their correspondence before any future generation/evaluation/publication is
 * attempted.
 *
 * AUDITED (M16-J.0A.1, split mesh/state ownership): the M16-J.0A single-owner design assumed a 1:1
 * relationship between a Working Mesh and a component's color state. The real panel's own
 * FVertexMaskForgeSelectedMesh has exactly ONE FVertexMaskForgeWorkingMesh SHARED by N
 * FVertexMaskForgePreviewComponentState (one Static Mesh, many placed components) -- Bounding Box/
 * Curvature/Noise/Material Slot/Thickness Mask are all documented elsewhere as "computed ONCE PER ENTRY,
 * shared across every component of this asset". Provenance therefore no longer carries a
 * SourceComponent (a shared Working Mesh has no single owning component) -- component identity now lives
 * exclusively on FVertexMaskForgePreviewComponentState::SourceComponent, checked directly. In its place,
 * Provenance and Authority both carry a MeshOwnerId (the owning FVertexMaskForgeWorkingMeshOwner's own
 * `this`, opaque, never dereferenced) -- this is what proves a given color Authority was actually
 * established against the SAME Working Mesh owner currently being checked, not merely one with
 * coincidentally-matching content. Authority additionally carries its own StateOwnerId, kept for
 * completeness/symmetry (see AlphaSourceOwnerMismatch's own doc comment) even though it cannot currently
 * mismatch anything (Authority always lives inside the same StateOwner instance that stamped it).
 *
 * This module does not generate, evaluate, or publish anything. It does not call M16-I or M16-H. It
 * does not touch WorkingMesh.InstanceResults, PreviewComponentState.InstanceResults, WorkingColors, or
 * any color buffer (other than reading Num() for a cardinality comparison). It is pure identity
 * bookkeeping and read-only comparison.
 */

/**
 * Persistent identity of WHO/WHAT a FVertexMaskForgeWorkingMesh was authoritatively built from -- never
 * the geometry itself (that remains GeometryFingerprint's job, untouched by this checkpoint). Invalid
 * (IsValid() == false) until FVertexMaskForgeWorkingMeshOwner::InstallWorkingMesh stamps it internally.
 *
 * Revision is the owning FVertexMaskForgeWorkingMeshOwner's own generation token at install time --
 * never computed from geometry content, never a timestamp, never random.
 */
class FVertexMaskForgeWorkingMeshProvenance
{
	friend class FVertexMaskForgeWorkingMeshOwner;

public:
	FVertexMaskForgeWorkingMeshProvenance() = default;

	bool IsValid() const { return bIsValid; }
	TWeakObjectPtr<UStaticMesh> GetSourceStaticMesh() const { return SourceStaticMesh; }
	bool GetUseSourceTopology() const { return bUseSourceTopology; }
	int32 GetLODIndex() const { return LODIndex; }
	uint64 GetRevision() const { return Revision; }
	int32 GetExpectedCardinality() const { return ExpectedCardinality; }
	const void* GetMeshOwnerId() const { return MeshOwnerId; }

private:
	/** The real Static Mesh asset this Working Mesh's geometry was built from. */
	TWeakObjectPtr<UStaticMesh> SourceStaticMesh;

	/** Same domain flag already authoritative throughout M16-E/M16-I (render-vertex vs Source-Topology). */
	bool bUseSourceTopology = false;

	/** Reserved for future multi-LOD support; always 0 today (this codebase only ever builds LOD0). */
	int32 LODIndex = 0;

	/** The owning FVertexMaskForgeWorkingMeshOwner's own generation token at install time. */
	uint64 Revision = 0;

	/** The per-sample cardinality this Working Mesh's geometry actually has in bUseSourceTopology's own
	 *  domain, captured once at install time, never recomputed by the validator. */
	int32 ExpectedCardinality = 0;

	/** AUDITED (M16-J.0A.1): identity of the owning FVertexMaskForgeWorkingMeshOwner instance itself
	 *  (its own `this` pointer, opaque -- never dereferenced). Since the owner is non-copyable/
	 *  non-movable, its address is stable for its entire lifetime, and since this WorkingMesh lives
	 *  INSIDE the owner, the owner can never be destroyed while this Provenance is still readable. */
	const void* MeshOwnerId = nullptr;

	bool bIsValid = false;
};

/**
 * Explicit authority over ONE FVertexMaskForgePreviewComponentState's own Working Colors buffer (either
 * WorkingColors or SourceTopologyWorkingColors, selected by GetUseSourceTopology()) -- proves the
 * buffer's alpha channel is CURRENTLY meaningful and corresponds to a specific Working Mesh Owner's
 * provenance, instead of merely having the "right" Num(). Invalid (IsValid() == false) until
 * FVertexMaskForgeWorkingStateOwner::InitializeColors stamps it internally.
 */
class FVertexMaskForgeWorkingColorsAuthority
{
	friend class FVertexMaskForgeWorkingStateOwner;

public:
	FVertexMaskForgeWorkingColorsAuthority() = default;

	bool IsValid() const { return bIsValid; }
	TWeakObjectPtr<UStaticMesh> GetSourceStaticMesh() const { return SourceStaticMesh; }
	bool GetUseSourceTopology() const { return bUseSourceTopology; }
	int32 GetLODIndex() const { return LODIndex; }
	uint64 GetRevision() const { return Revision; }
	int32 GetCardinality() const { return Cardinality; }
	const void* GetMeshOwnerId() const { return MeshOwnerId; }
	const void* GetStateOwnerId() const { return StateOwnerId; }

private:
	/** Must match the corresponding FVertexMaskForgeWorkingMeshProvenance's own SourceStaticMesh. Copied
	 *  from the ATTACHED FVertexMaskForgeWorkingMeshOwner's own current Provenance at InitializeColors
	 *  time -- never independently chosen. */
	TWeakObjectPtr<UStaticMesh> SourceStaticMesh;

	bool bUseSourceTopology = false;
	int32 LODIndex = 0;

	/** Must match the attached mesh owner's own Revision -- copied verbatim from
	 *  FVertexMaskForgeWorkingMeshProvenance::Revision at InitializeColors time (never independently
	 *  advanced), so a Working Mesh rebuilt to a new Revision without this Authority being
	 *  re-established represents exactly the "stale" condition this checkpoint must detect. */
	uint64 Revision = 0;

	/** The buffer's own cardinality at the moment authority was established. */
	int32 Cardinality = 0;

	/** AUDITED (M16-J.0A.1): identity of the FVertexMaskForgeWorkingMeshOwner this Authority was
	 *  established against, copied from that owner's own Provenance::MeshOwnerId at InitializeColors
	 *  time -- this is the check that proves a StateOwner's Authority still corresponds to the SAME mesh
	 *  owner currently being validated (see AlphaSourceOwnerMismatch's own doc comment), not merely one
	 *  with coincidentally-matching Static Mesh/LOD/domain/cardinality. */
	const void* MeshOwnerId = nullptr;

	/** AUDITED (M16-J.0A.1): this Authority's own owning FVertexMaskForgeWorkingStateOwner identity --
	 *  kept for completeness/symmetry with MeshOwnerId; cannot currently mismatch anything since
	 *  Authority always lives inside the same StateOwner instance that stamped it (same co-located-owner
	 *  reasoning M16-J.0 already established for the pre-split design). */
	const void* StateOwnerId = nullptr;

	bool bIsValid = false;
};

enum class EVertexMaskForgeWorkingColorsPublicationValidationStatus : uint8
{
	Success,
	InvalidOwner,
	InvalidWorkingMesh,
	InvalidTarget,

	/** AUDITED (M16-J.0A.1): unreachable via the composed (MeshOwner + StateOwner) validator -- a shared
	 *  Working Mesh has no single owning component to mismatch against; component identity is checked
	 *  directly via InvalidTarget instead. Kept in the enum only for continuity with the M16-J.0/M16-J.0A
	 *  single-owner design's own status list. */
	ComponentMismatch,

	MeshMismatch,
	LODMismatch,
	StaleWorkingMesh,
	DomainMismatch,
	CardinalityMismatch,
	InvalidAlphaSource,

	/** AUDITED (M16-J.0A.1): the PRIMARY cross-owner check in the composed design -- fires whenever
	 *  Authority.GetMeshOwnerId() != Provenance.GetMeshOwnerId(), i.e. the color state was authenticated
	 *  against a DIFFERENT (or no longer attached) mesh owner than the one currently being validated.
	 *  Genuinely reachable (unlike its pre-split namesake), checked early (step 5, right after Authority
	 *  validity), since it is the mesh-owner-attachment analogue of "owner identity" from the pre-split
	 *  design. */
	AlphaSourceOwnerMismatch,

	/** AUDITED: NOT independently checked -- a plain alias/duplicate of MeshMismatch's own comparison
	 *  given the co-located-authority design; kept in the enum only because the checkpoint's own contract
	 *  requires the name to exist. Never returned. Same reasoning applies to AlphaSourceLODMismatch/
	 *  AlphaSourceDomainMismatch/AlphaSourceRevisionMismatch below. */
	AlphaSourceMeshMismatch,
	AlphaSourceLODMismatch,
	AlphaSourceDomainMismatch,
	AlphaSourceRevisionMismatch,

	AlphaSourceCardinalityMismatch,
};

class FVertexMaskForgeWorkingColorPublicationBinding;

namespace VertexMaskForgeWorkingColorsProvenance
{
	EVertexMaskForgeWorkingColorsPublicationValidationStatus ValidateWorkingColorsPublicationBinding(
		const FVertexMaskForgeWorkingColorPublicationBinding& Binding, const FVertexMaskForgePreviewComponentState& Target);
}

/**
 * Weak, transient reference to exactly one FVertexMaskForgeWorkingMeshOwner -- safe to store indefinitely
 * (see AUDITED note below), never itself paired with a FVertexMaskForgePreviewComponentState.
 *
 * AUDITED (M16-J.0B.1, 1st corrective pass -- WorkingMesh raw-pointer finding): an earlier revision stored a
 * raw `const FVertexMaskForgeWorkingMesh*`, obtained by Pin()-ing the owner just long enough to take its
 * address -- CreateBinding() itself was always safe (the caller's own TSharedPtr kept the owner alive for
 * that one call), but a caller could store the RETURNED Binding beyond that call and validate it later,
 * after the owner was destroyed, dereferencing freed memory. Fixed by never storing a raw pointer to the
 * owner's geometry: MeshOwnerWeak is a TWeakPtr, Pin()-ed FRESH every time validation actually runs.
 *
 * AUDITED (M16-J.0B.1, 2nd corrective pass -- TargetPreviewState raw-pointer finding): the 1st corrective
 * pass ALSO made this type non-copyable/non-movable, reasoning that this prevented a Binding from being
 * stored -- WRONG. Mandatory copy elision (C++17) means `auto Binding = StateOwner.CreateBinding();` still
 * compiles and stores the Binding in a local variable regardless of deleted copy/move (elision needs
 * neither), so a Binding carrying a raw `FVertexMaskForgePreviewComponentState* TargetPreviewState` (this-
 * relative, into the StateOwner that created it) was STILL storable with a dangling-pointer hazard the
 * instant that StateOwner was destroyed -- non-copyability blocks smuggling a Binding into a container or a
 * class member, but never blocks a caller from simply holding one in a local variable past the lifetime of
 * the StateOwner that produced it.
 *
 * FIXED STRUCTURALLY, not by convention: TargetPreviewState is REMOVED from this type entirely.
 * FVertexMaskForgeWorkingStateOwner has no weak-referenceable infrastructure (it is held by TUniquePtr, not
 * TSharedPtr, in FVertexMaskForgeSelectedMesh::PreviewComponents, and giving it one would mean changing that
 * ownership model project-wide -- out of scope for this checkpoint), so instead of inventing a second,
 * narrower weak-reference mechanism just for this one field, ValidateWorkingColorsPublicationBinding now
 * takes the target FVertexMaskForgePreviewComponentState as a SEPARATE, live parameter, supplied by the
 * caller at the exact moment of validation (see FVertexMaskForgeWorkingStateOwner::ValidateBinding, which
 * passes `PreviewState` -- its own member, always valid for the duration of that call, since `this` is
 * alive). This Binding type therefore now carries ONLY a TWeakPtr -- there is no raw pointer left inside it
 * to go stale, so it is safe to store for as long as a caller likes; a caller who separately holds onto a
 * dangling `FVertexMaskForgePreviewComponentState&` and passes it to the validator anyway has made an
 * ordinary dangling-reference mistake identical to passing one to any other function in the codebase -- not
 * a hazard this TYPE could ever have prevented by construction, since the object it refers to belongs to a
 * class this module does not own.
 *
 * Still non-copyable/non-movable (kept as an auxiliary property, not lifetime proof -- see this note's own
 * corrective language above): CreateBinding() still returns via prvalue construction, so ordinary immediate
 * usage is unaffected; this only blocks smuggling a Binding into a container/member.
 */
class FVertexMaskForgeWorkingColorPublicationBinding
{
	friend class FVertexMaskForgeWorkingStateOwner;
	friend EVertexMaskForgeWorkingColorsPublicationValidationStatus VertexMaskForgeWorkingColorsProvenance::ValidateWorkingColorsPublicationBinding(
		const FVertexMaskForgeWorkingColorPublicationBinding&, const FVertexMaskForgePreviewComponentState&);

public:
	FVertexMaskForgeWorkingColorPublicationBinding() = default;

	// Auxiliary properties only (see this class's own module comment) -- NOT lifetime proof by themselves.
	FVertexMaskForgeWorkingColorPublicationBinding(const FVertexMaskForgeWorkingColorPublicationBinding&) = delete;
	FVertexMaskForgeWorkingColorPublicationBinding& operator=(const FVertexMaskForgeWorkingColorPublicationBinding&) = delete;
	FVertexMaskForgeWorkingColorPublicationBinding(FVertexMaskForgeWorkingColorPublicationBinding&&) = delete;
	FVertexMaskForgeWorkingColorPublicationBinding& operator=(FVertexMaskForgeWorkingColorPublicationBinding&&) = delete;

	/** A cheap, immediate liveness check on the MeshOwner side only -- can still go stale the instant after
	 *  it returns; the validator re-Pin()s independently and is the only result that matters. */
	bool IsWellFormed() const { return MeshOwnerWeak.IsValid(); }

private:
	/** AUDITED (M16-J.0B.1 corrective pass): private, friend-only, prvalue-only construction -- see this
	 *  class's own module comment on why a named local + `return Binding;` (relying on non-mandatory NRVO)
	 *  would be ill-formed against the deleted move constructor. */
	explicit FVertexMaskForgeWorkingColorPublicationBinding(TWeakPtr<const FVertexMaskForgeWorkingMeshOwner> InMeshOwnerWeak)
		: MeshOwnerWeak(MoveTemp(InMeshOwnerWeak))
	{
	}

	TWeakPtr<const FVertexMaskForgeWorkingMeshOwner> MeshOwnerWeak;
};

namespace VertexMaskForgeWorkingColorsProvenance
{
	/**
	 * Read-only, synchronous, deterministic, headless validation pairing Binding (WHICH mesh owner) against
	 * Target (WHICH component's color state), supplied live by the caller -- no mutation, no generation, no
	 * evaluation, no visual application, no persistence.
	 *
	 * Order (AUDITED, M16-J.0B.1 2nd corrective pass -- Target is now a live parameter, never a field
	 * stored inside Binding; the check order itself is otherwise unchanged from the M16-J.0A.1 revision):
	 *   1. Binding.IsWellFormed(), else InvalidOwner.
	 *   2. WorkingMesh's own Provenance.IsValid() and its SourceStaticMesh weak pointer still resolves,
	 *      else InvalidWorkingMesh.
	 *   3. Target's own SourceComponent weak pointer still resolves, else InvalidTarget.
	 *   4. Target->WorkingColorsAuthority.IsValid(), else InvalidAlphaSource.
	 *   5. Provenance.GetMeshOwnerId() == Authority.GetMeshOwnerId(), else AlphaSourceOwnerMismatch.
	 *   6. Provenance.GetSourceStaticMesh() == Authority.GetSourceStaticMesh(), else MeshMismatch.
	 *   7. Provenance.GetLODIndex() == Authority.GetLODIndex(), else LODMismatch.
	 *   8. Provenance.GetUseSourceTopology() == Authority.GetUseSourceTopology(), else DomainMismatch.
	 *   9. Provenance.GetRevision() == Authority.GetRevision(), else StaleWorkingMesh.
	 *  10. Provenance.GetExpectedCardinality() == the SELECTED buffer's live Num(), else CardinalityMismatch.
	 *  11. Authority.GetCardinality() == that same live Num(), else AlphaSourceCardinalityMismatch.
	 *  12. Success.
	 */
	EVertexMaskForgeWorkingColorsPublicationValidationStatus ValidateWorkingColorsPublicationBinding(
		const FVertexMaskForgeWorkingColorPublicationBinding& Binding, const FVertexMaskForgePreviewComponentState& Target);
}
