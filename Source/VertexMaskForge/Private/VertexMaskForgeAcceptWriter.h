#pragma once

#include "CoreMinimal.h"
#include "VertexMaskForgeAcceptTargetBuilder.h"

class UStaticMesh;

namespace VertexMaskForgeAcceptWriter
{
	/**
	 * Writes every target's FinalColors into its Static Mesh asset's LOD 0 MeshDescription (via the
	 * SAME WedgeMap-based approach as UMeshPaintingSubsystem::PropagateColorsToRawMesh). Re-validates
	 * every target BEFORE modifying anything -- once the write loop starts, every step is expected to
	 * succeed by construction (validated moments earlier, synchronously, nothing else runs in between),
	 * so there is no mid-loop rollback path: if this function ever returns false, NOTHING has been
	 * modified.
	 *
	 * Does NOT open its own FScopedTransaction or call Mesh->Build() -- the caller owns ONE outer
	 * transaction spanning this function and WriteSourceTopologyAcceptTargets together, and calls
	 * BuildModifiedMeshes() for both domains' results only AFTER that transaction closes. Every
	 * successfully-written Mesh is appended to OutModifiedMeshes for that later pass.
	 */
	bool WriteAcceptTargets(
		const TArray<VertexMaskForgeAcceptTargetBuilder::FAcceptTarget>& Targets,
		TArray<UStaticMesh*>& OutModifiedMeshes,
		FText& OutErrorText);

	/**
	 * Sibling of WriteAcceptTargets for Source-Topology entries. Writes ONLY VertexInstanceColors on
	 * the SOURCE MeshDescription (Mesh->GetMeshDescription(0)) -- never positions, topology, normals,
	 * UVs, polygon groups, or any other attribute -- via the TriangleID+corner correspondence
	 * (Entry->MeshOwner->GetWorkingMesh().TriIDMap), reproducing exactly what
	 * UE::Geometry::FDynamicMeshToMeshDescription::UpdateVertexColors does for the native Paint Vertex
	 * Colors tool's own commit. Same two-pass (re-validate everything, THEN write) discipline as
	 * WriteAcceptTargets -- if this returns false, nothing was modified. Like its render-vertex
	 * sibling, does NOT open its own FScopedTransaction or call Mesh->Build().
	 */
	bool WriteSourceTopologyAcceptTargets(
		const TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget>& Targets,
		TArray<UStaticMesh*>& OutModifiedMeshes,
		FText& OutErrorText);

	/**
	 * Rebuilds RenderData (and Nanite cluster data, for Nanite-enabled assets) for every mesh whose
	 * MeshDescription was just committed by WriteAcceptTargets/WriteSourceTopologyAcceptTargets.
	 *
	 * Called by the caller AFTER its outer FScopedTransaction has already closed -- RenderData/Nanite
	 * data is DERIVED, deterministically regenerated from the (now transacted) MeshDescription on both
	 * Accept and Undo/Redo, so it must never itself be captured inside a transaction. GUndo is
	 * suppressed for the duration of each Build() call, so an already-closed outer transaction (or the
	 * complete absence of one) can never be reopened or polluted by whatever Modify() calls Build()
	 * triggers internally (e.g. component re-registration).
	 */
	bool BuildModifiedMeshes(
		const TArray<UStaticMesh*>& Meshes,
		FText& OutErrorText);
}
