#pragma once

#include "CoreMinimal.h"
#include "Math/Transform.h"

struct FVertexMaskForgeScalarMask;
struct FVertexMaskForgeWorkingMesh;
struct FStaticMeshLODResources;
enum class EVertexMaskForgeNormalSpace : uint8;
enum class EVertexMaskForgeNormalDirection : uint8;

namespace VertexMaskForgeDirectionalNormalGenerator
{
	/**
	 * AUDITED (V2-E): render-vertex domain -- one Directional Normal value per RenderIndex, read from
	 * LOD0.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ, the SAME real render normal / SAME
	 * render-vertex domain AO already uses for its own World Space transform (see
	 * GenerateAmbientOcclusionMask's own NORMALS doc note) -- preserves split normals/hard edges exactly
	 * (never collapsed by position or source Vertex ID). Space==World transforms via ComponentTransform
	 * (see ComputeWorldSpaceNormalMatrix/TransformNormalToWorldSpace); Space==Local uses the render
	 * normal as-is (ComponentTransform ignored entirely). A degenerate/non-finite render normal, or (in
	 * World Space) a degenerate ComponentTransform, marks that ONE element unwritten (bHasValue false) --
	 * never guessed -- and the whole mask still reports Ready as long as at least one element resolved.
	 */
	FVertexMaskForgeScalarMask GenerateDirectionalNormalMask(
		const FStaticMeshLODResources& LOD0,
		EVertexMaskForgeNormalSpace Space,
		EVertexMaskForgeNormalDirection Direction,
		float Angle,
		float Falloff,
		float Blur,
		bool bInvert,
		const FTransform& ComponentTransform);

	/**
	 * AUDITED (V2-E): sibling of GenerateDirectionalNormalMask for Source-Topology (Nanite) entries --
	 * CORNER-EXACT (Mesh.TriangleCount()*3, indexed by CornerIndex directly, like MaterialSlotMask),
	 * reading each corner's normal from the Dynamic Mesh's own Normal Overlay
	 * (Mesh.Attributes()->PrimaryNormals(), populated from the source MeshDescription's own
	 * VertexInstanceNormals by FMeshDescriptionToDynamicMesh::Convert at working-mesh build time -- see
	 * EnsureNormalOverlay) via NormalOverlay->GetTriangle(TriangleID)[Corner] -- the EXACT SAME Normal
	 * Element domain GenerateAmbientOcclusionMaskFromDynamicMesh already uses for AO's own hard-edge-
	 * preserving normal lookup, so hard edges/split normals/UV-seam-distinct corners are preserved
	 * identically, with no separate correspondence table needed (the Normal Overlay's own per-corner
	 * structure already IS that correspondence, already audited for this exact purpose). A corner
	 * without a set Normal Overlay entry, or a degenerate normal, is left unwritten -- never guessed.
	 */
	FVertexMaskForgeScalarMask GenerateDirectionalNormalMaskFromDynamicMesh(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		EVertexMaskForgeNormalSpace Space,
		EVertexMaskForgeNormalDirection Direction,
		float Angle,
		float Falloff,
		float Blur,
		bool bInvert,
		const FTransform& ComponentTransform);
}
