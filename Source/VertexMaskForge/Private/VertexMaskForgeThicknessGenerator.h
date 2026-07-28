#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

struct FVertexMaskForgeScalarMask;
struct FVertexMaskForgeThicknessCache;
struct FVertexMaskForgeSourceTopologyThicknessCache;
struct FVertexMaskForgeWorkingMesh;
struct FStaticMeshLODResources;
class UStaticMesh;

namespace VertexMaskForgeThicknessGenerator
{
	/**
	 * Non-Nanite Thickness generation (V2-G) -- render-vertex domain, Asset Local Space (NEVER
	 * ComponentTransform -- see the corrective audit's own proof that World Space would make two
	 * differently-scaled instances of the same asset require incompatible persisted results). Builds a
	 * private LocalMesh (positions/triangles from LOD0, degenerate triangles excluded, tangent-Z carried
	 * as a 1:1-per-render-vertex Normal Overlay via AppendElement+SetParentVertex -- see the corrective
	 * audit's own API confirmation that AppendElement has no parent-vertex parameter) cached alongside a
	 * FRESHNESS SNAPSHOT (positions/tangent-Z by RenderVertexIndex, triangle connectivity by
	 * TriangleOrdinal storing render-vertex-index triples -- NEVER Dynamic-Mesh-internal IDs) compared
	 * against the CURRENT asset immediately before Accept's first Modify() -- see
	 * AreThicknessGeometrySnapshotsExactlyEquivalent.
	 *
	 * PIPELINE: raw measured distances -> normalize -> thin=white flip -> Blur -> user Invert -> caller's
	 * ComposeMaskStack. Only the raycast (Layer 1+2) is cached; normalize/Blur/Invert are recomputed
	 * fresh every call directly from RawDistances -- same "cheap enough to just recompute" precedent as
	 * AO's own Levels/Invert and Directional Normal's own Blur/Invert.
	 */
	FVertexMaskForgeScalarMask GenerateThicknessMask(
		TUniquePtr<FVertexMaskForgeThicknessCache>& CachePtr,
		const UStaticMesh* Mesh,
		const FStaticMeshLODResources& LOD0,
		float RawMinThickness,
		float RawMaxThickness,
		float RawSearchDistance,
		float RawBias,
		float Blur,
		bool bInvert);

	/**
	 * Source-Topology sibling of GenerateThicknessMask -- CORNER-EXACT domain (Mesh.TriangleCount()*3),
	 * matching DirectionalNormalMask/MaterialSlotMask (never Vertex-ID/ElementID domain like AO's own
	 * Source-Topology cache). Builds a PRIVATE LocalMesh copy of WorkingMesh.Mesh's geometry (never
	 * mutates the SHARED WorkingMesh.Mesh other generators also depend on) with degenerate triangles
	 * excluded, used purely as the raycast spatial structure -- output is indexed by WorkingMesh.Mesh's
	 * OWN TriangleID/Corner (via TriangleIndicesItr() ordinal), never by LocalMesh's internal IDs.
	 */
	FVertexMaskForgeScalarMask GenerateThicknessMaskFromDynamicMesh(
		TUniquePtr<FVertexMaskForgeSourceTopologyThicknessCache>& CachePtr,
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		float RawMinThickness,
		float RawMaxThickness,
		float RawSearchDistance,
		float RawBias,
		float Blur,
		bool bInvert);
}
