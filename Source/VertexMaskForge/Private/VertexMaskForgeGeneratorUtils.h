#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"

struct FStaticMeshLODResources;

namespace UE::Geometry
{
	class FDynamicMesh3;
}

namespace VertexMaskForgeGeneratorUtils
{
	/**
	 * AUDITED (Nanite source-topology support, AO cache robustness fix -- CORRECTED per follow-up
	 * review): content fingerprint of Mesh, covering everything that can affect the AABBTree/raycast
	 * result: vertex positions, triangle CONNECTIVITY (not just counts), the normals used for ray
	 * origins, and the corner -> Normal Element association. Combined via GetTypeHash/HashCombine in a
	 * fixed, deterministic order (VertexIndicesItr / TriangleIndicesItr / ElementIndicesItr, all stable
	 * for an unedited mesh -- never assumed dense; TriangleIndicesItr never assumed to be enumerated by
	 * TriangleID value alone, see below). Computed ONCE, at working-mesh build time (see
	 * BuildWorkingMeshForStaticMesh), and reused for the rest of the session -- Mesh is never mutated in
	 * place afterward (EnsureNormalOverlay, the one exception, always runs BEFORE this is computed).
	 *
	 * CORRECTED: the original version hashed only positions and normal VALUES, in isolation from each
	 * other and from topology -- two meshes with identical vertex positions/normal values but different
	 * TRIANGLE CONNECTIVITY (e.g. a re-triangulated quad, or a corner rewired to a different Normal
	 * Element) would have produced the SAME fingerprint despite the raycast result genuinely differing
	 * (different triangles occlude different rays; a corner's ray now originates from a different
	 * normal). Fixed by additionally hashing, per triangle (iterated via TriangleIndicesItr(), which
	 * never assumes TriangleID is dense): the triangle's own ordinal position in that iteration (an
	 * explicit delimiter -- see below) plus its three VertexIDs (Mesh.GetTriangle(TriangleID), corner
	 * order preserved) plus, when a Normal Overlay is present and this triangle is set in it
	 * (NormalOverlay->IsSetTriangle(TriangleID)), its three Normal Element IDs
	 * (NormalOverlay->GetTriangle(TriangleID), corner order preserved).
	 *
	 * DELIMITER (per explicit requirement -- "impedir sequências ambíguas"): the running per-triangle
	 * ordinal (0, 1, 2, ...) is hashed in BEFORE each triangle's own three-ID group, and the final
	 * ordinal count is hashed in once more at the very end. This binds every VertexID/Normal Element ID
	 * triple to its exact position in the iteration, so two different triangulations cannot produce the
	 * same flattened ID sequence by coincidence (e.g. triangle boundaries shifting) the way an
	 * undelimited flat concatenation of IDs could.
	 */
	uint32 ComputeDynamicMeshGeometryFingerprint(const UE::Geometry::FDynamicMesh3& Mesh);

	/**
	 * AUDITED (V2-F, Directional Normal Blur): the SAME iterative algorithm shape as
	 * ApplyTopologicalCurvatureBlur (self-plus-neighbors average, repeated FullIterations times, plus a
	 * fractional-iteration lerp toward one more pass -- see that function's own doc comment for the
	 * "whole number = full iterations, fractional part blends toward one more" contract, preserved
	 * verbatim here) -- but DELIBERATELY NOT ApplyTopologicalCurvatureBlur ITSELF: that function's
	 * adjacency (Mesh.VtxVerticesItr(VertexID)) is Dynamic-Mesh-VERTEX-ID domain, one value per vertex --
	 * correct for Curvature (a genuinely per-vertex geometric property) but WRONG for Directional Normal
	 * Mask, whose raw values are deliberately CORNER-EXACT/per-render-vertex (never collapsed, so a hard
	 * edge/UV seam's several corners at the same position can legitimately differ -- see
	 * GenerateDirectionalNormalMaskFromDynamicMesh's own doc note). Blurring through
	 * ApplyTopologicalCurvatureBlur would require collapsing to one value per Vertex ID first, silently
	 * destroying exactly the split-normal independence V2-E was built to preserve. This generic function
	 * instead takes an EXPLICIT, domain-appropriate adjacency list (built once per generation call by
	 * BuildRenderVertexAdjacency or BuildCornerAdjacency below) -- everything else about the algorithm is
	 * identical. Never allocates per element inside the hot loop (Adjacency is built once, up front).
	 */
	TArray<float> ApplyAdjacencyTopologicalBlur(const TArray<TArray<int32>>& Adjacency, const TArray<float>& Input, const TArray<bool>& bHasValue, float BlurAmount);

	/**
	 * AUDITED (V2-F): render-vertex adjacency, built directly from LOD0's own IndexBuffer (the SAME
	 * render-vertex domain GenerateDirectionalNormalMask itself reads normals from) -- render vertex i's
	 * neighbors are the OTHER two render vertices of every triangle i participates in. A hard edge/UV
	 * seam is ALREADY represented as physically SEPARATE render vertex entries in this domain (that is
	 * what makes VertexTangentZ per-render-vertex correct for split normals in the first place -- see
	 * GenerateDirectionalNormalMask's own doc note), so this adjacency never needs any special-casing to
	 * avoid crossing a seam: a split vertex's two "halves" simply belong to disjoint triangle fans with
	 * their own, separate neighbor sets by construction. Built once per generation call (not cached --
	 * Directional Normal Mask has no raw cache of its own, matching Material Slot Mask's own "cheap
	 * enough to just recompute" precedent).
	 */
	TArray<TArray<int32>> BuildRenderVertexAdjacency(const FStaticMeshLODResources& LOD0, int32 NumRenderVerts);

	/**
	 * AUDITED (V2-F corrective pass): CORNER-exact adjacency for the Source-Topology domain, built from
	 * real triangle topology (Mesh.GetTriNeighbourTris(TriangleID)/Mesh.GetTriEdge(), the SAME official
	 * GeometryCore edge-adjacency query -- never a second/parallel topology representation). Corner c of
	 * triangle T is connected to the OTHER TWO corners of T itself (unconditional -- a single face is
	 * always internally continuous), plus, for each of T's up-to-3 edge-adjacent triangles, ONLY the ONE
	 * corner of that neighbor that shares c's actual Mesh VertexID at that edge (matched by VertexID, not
	 * by local Corner slot -- winding order is not guaranteed to agree across an edge) -- and ONLY when
	 * NormalOverlay->IsSeamEdge() reports the PrimaryNormals overlay is CONTINUOUS there (no split/hard
	 * edge). A boundary edge, an edge whose neighbor could not be resolved, or a genuine normal-overlay
	 * seam all simply contribute no cross-triangle neighbor there. Deliberately keyed to the NORMAL
	 * overlay specifically (not any UV overlay): a UV seam does not imply a normal split and must not by
	 * itself interrupt this Blur. NEVER collapses distinct corners: every corner keeps its OWN entry in
	 * the output CornerIndex-domain array, even when several corners share a position/Dynamic Mesh
	 * VertexID with genuinely different normals (that is the entire reason this exists instead of reusing
	 * ApplyTopologicalCurvatureBlur's own Vertex-ID-domain adjacency).
	 */
	TArray<TArray<int32>> BuildCornerAdjacency(const UE::Geometry::FDynamicMesh3& Mesh, const UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay, int32 NumCorners);

	/**
	 * AUDITED (Curvature layer): Levels Min/Max remap, same epsilon-safe-denominator/clamp contract as
	 * ApplyAOLevelsAndInvert (see its own doc comment for the DIVIDE-BY-ZERO/NaN safety rationale --
	 * LevelsMax <= LevelsMin degenerates to a deterministic hard step, never NaN/Inf) -- deliberately a
	 * SEPARATE, smaller function rather than reusing ApplyAOLevelsAndInvert itself, since that function's
	 * BaseAO = 1 - RawAO vanilla-inversion step and its trailing user Invert are AO-specific conventions
	 * that do not apply to Curvature (Curvature has no Invert control at all, per the explicit
	 * requirement) -- reusing it here would either silently invert Curvature or require threading a
	 * meaningless bInvert=false through every call site. AO's own Levels behavior is untouched.
	 */
	float ApplyCurvatureLevels(float Value, float LevelsMin, float LevelsMax);
}
