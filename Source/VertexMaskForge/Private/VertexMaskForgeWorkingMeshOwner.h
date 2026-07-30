#pragma once

#include "CoreMinimal.h"
#include "VertexMaskForgeWorkingColorsProvenance.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

class UStaticMesh;

/**
 * M16-J.0A.1: headless owner of ONE FVertexMaskForgeWorkingMesh -- exactly one instance per
 * FVertexMaskForgeSelectedMesh entry (one Static Mesh asset), SHARED (read, never owned or copied) by
 * every FVertexMaskForgeWorkingStateOwner (one per component/PreviewComponentState) attached to it.
 *
 * AUDITED (why this class exists, split from FVertexMaskForgeWorkingStateOwner): the M16-J.0A audit
 * found that FVertexMaskForgeSelectedMesh has exactly ONE Working Mesh SHARED by N
 * FVertexMaskForgePreviewComponentState (one Static Mesh asset, many placed components referencing it)
 * -- Bounding Box/Curvature/Noise/Material Slot/Thickness Mask are all "computed ONCE PER ENTRY, shared
 * across every component of this asset" throughout the whole codebase. The original M16-J.0A
 * FVertexMaskForgeWorkingStateOwner assumed a 1:1 owner-to-Working-Mesh relationship (InstallWorkingMesh
 * MOVED the mesh in, exclusively) -- incompatible with this real 1:N structure, since a move-only
 * FVertexMaskForgeWorkingMesh cannot be installed into N different owners, and duplicating the geometry
 * per component would be both wasteful and would break the "computed ONCE PER ENTRY" invariant. This
 * class now owns the geometry + generation exclusively; FVertexMaskForgeWorkingStateOwner owns only
 * per-component color state and ATTACHES to (never owns or copies) the mesh owner of its own entry.
 *
 * HEADLESS: no dependency on Slate, SVertexMaskForgePanel, global selection, viewport, render-state
 * invalidation, persistence, async/threads/locks.
 *
 * LIFETIME: non-copyable, non-movable. Its own address is used as MeshOwnerId (see
 * FVertexMaskForgeWorkingMeshProvenance::MeshOwnerId's own doc comment). Callers must hold it via
 * TSharedPtr<FVertexMaskForgeWorkingMeshOwner> (one per FVertexMaskForgeSelectedMesh entry) so that
 * FVertexMaskForgeWorkingStateOwner instances can attach via TWeakPtr -- never a raw pointer, so a
 * StateOwner can never observe a destroyed mesh owner regardless of destruction order (TWeakPtr::Pin()
 * fails cleanly instead of dangling).
 */
class FVertexMaskForgeWorkingMeshOwner
{
public:
	FVertexMaskForgeWorkingMeshOwner() = default;
	~FVertexMaskForgeWorkingMeshOwner() = default;

	FVertexMaskForgeWorkingMeshOwner(const FVertexMaskForgeWorkingMeshOwner&) = delete;
	FVertexMaskForgeWorkingMeshOwner& operator=(const FVertexMaskForgeWorkingMeshOwner&) = delete;
	FVertexMaskForgeWorkingMeshOwner(FVertexMaskForgeWorkingMeshOwner&&) = delete;
	FVertexMaskForgeWorkingMeshOwner& operator=(FVertexMaskForgeWorkingMeshOwner&&) = delete;

	/**
	 * Registers WHICH Static Mesh asset, LOD, and domain this owner's future Working Mesh will belong to
	 * -- the per-ENTRY identity axes (never a component; a shared Working Mesh has no single owning
	 * component). Repeated calls with the SAME effective identity are a no-op (no invalidation, no
	 * generation change). A call with a DIFFERENT identity (or the first call) invalidates any
	 * currently-installed Working Mesh (reset to default, invalid) but does NOT itself advance
	 * Generation -- Generation only advances on InstallWorkingMesh/Reset, mirroring
	 * FVertexMaskForgeWorkingStateOwner's own single generation-advance rule (see that class's own module
	 * comment).
	 *
	 * Returns false (leaving the owner unconfigured) if StaticMesh is null or this owner has been
	 * permanently invalidated by generation overflow.
	 */
	bool ConfigureIdentity(UStaticMesh* StaticMesh, int32 LODIndex, bool bUseSourceTopology);

	bool IsIdentityConfigured() const { return bIdentityConfigured; }

	/**
	 * Installs NewWorkingMesh as this owner's current Working Mesh, unconditionally advancing Generation
	 * by exactly one, and REPLACES whatever Provenance NewWorkingMesh may already carry with fresh
	 * Provenance stamped internally (SourceStaticMesh/LOD/domain from this owner's configured identity,
	 * Revision = new Generation, MeshOwnerId = this). ExpectedCardinality is caller-supplied (the real
	 * render-vertex count or Mesh->TriangleCount()*3 for Source-Topology) -- never treated as identity
	 * proof by the validator (cardinality never authenticates identity).
	 *
	 * Requires IsIdentityConfigured(). Never performs geometric generation itself -- NewWorkingMesh's
	 * actual Mesh/State/diagnostics/InstanceResults are whatever the caller already built; this function
	 * only ever touches Provenance and Generation, and never clears InstanceResults (M16-C/M16-H/M16-I's
	 * own store, entirely out of this checkpoint's scope).
	 *
	 * OVERFLOW POLICY: identical to FVertexMaskForgeWorkingStateOwner's own -- if Generation is already
	 * TNumericLimits<uint64>::Max(), this call fails (ensure + permanent invalidation) rather than
	 * wrapping to 0.
	 */
	bool InstallWorkingMesh(FVertexMaskForgeWorkingMesh&& NewWorkingMesh, int32 ExpectedCardinality);

	/**
	 * Read-only access to the currently-installed Working Mesh. M16-J.0B.1 (WorkingMesh Domain Split):
	 * there is NO mutable accessor -- GetWorkingMeshMutable() was removed entirely. Generator Artistic
	 * State (BoundingBoxMask, CurvatureMask, NoiseMask, MaterialSlotMask, DirectionalNormalMask,
	 * ThicknessMask, their raw caches) no longer lives on FVertexMaskForgeWorkingMesh at all -- it moved
	 * to FVertexMaskForgeGeneratorState (see FVertexMaskForgeSelectedMesh::GeneratorState), which the
	 * panel owns directly and mutates freely, with no owner/generation contract of its own (it was never
	 * part of the authenticated identity this class protects). The only thing that ever needs to mutate
	 * FVertexMaskForgeWorkingMesh itself, post-install, is this class's own InstallWorkingMesh/Reset.
	 */
	const FVertexMaskForgeWorkingMesh& GetWorkingMesh() const { return WorkingMesh; }

	/**
	 * Invalidates the Working Mesh (reset to a fresh default-constructed, State == InvalidSource
	 * instance) and unconditionally advances Generation by exactly one -- so any Provenance/Authority/
	 * Binding captured before Reset can never validate again, even if an identical-looking Working Mesh
	 * is later reinstalled. Configured identity (Static Mesh/LOD/domain) is preserved across Reset --
	 * matches FVertexMaskForgeWorkingStateOwner::Reset's own contract.
	 */
	bool Reset();

	uint64 GetGeneration() const { return Generation; }
	TWeakObjectPtr<UStaticMesh> GetConfiguredStaticMesh() const { return ConfiguredStaticMesh; }
	int32 GetConfiguredLODIndex() const { return ConfiguredLODIndex; }
	bool GetConfiguredUseSourceTopology() const { return bConfiguredUseSourceTopology; }

private:
	uint64 Generation = 0;
	bool bPermanentlyInvalid = false;

	bool bIdentityConfigured = false;
	TWeakObjectPtr<UStaticMesh> ConfiguredStaticMesh;
	int32 ConfiguredLODIndex = 0;
	bool bConfiguredUseSourceTopology = false;

	FVertexMaskForgeWorkingMesh WorkingMesh;
};
