#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"
#include "Math/Transform.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

struct FVertexMaskForgeScalarMask;
struct FStaticMeshLODResources;
struct FVertexMaskForgeSelectedMesh;
namespace UE::Geometry { class FDynamicMesh3; }

namespace VertexMaskForgeBoundingBoxGenerator
{
	/**
	 * Generates the Bounding Box Mask directly in RENDER VERTEX order for one Static Mesh's LOD 0,
	 * evaluating up to 3 independent axes (AxisParams, indexed by EVertexMaskForgeBoundsAxis) and
	 * combining every ENABLED axis by maximum.
	 *
	 * AUDITED ARCHITECTURAL CORRECTION (render-vertex order): this mask is computed directly from
	 * LOD0.VertexBuffers.PositionVertexBuffer -- ONE value per RenderVertexIndex, guaranteeing
	 * Mask.Values.Num() == PositionVertexBuffer.GetNumVertices() exactly (enforced by construction
	 * below). Render vertices that share a position (a seam) each still get their own array slot,
	 * but since the mask value is a pure function of position, they necessarily compute to the same
	 * value -- consistent with baseline colors remaining independent per render vertex. No
	 * FDynamicMesh3, no position matching (BuildPositionBuckets/FindMatchingVertexID remain unused,
	 * reserved for a future topology-dependent generator).
	 *
	 * LOCAL vs WORLD SPACE (per axis, audited): for an axis with bWorldSpace == false, the
	 * evaluation position is LocalPosition (LOD0's own render-vertex position) unchanged. For
	 * bWorldSpace == true, EvaluationPosition = ComponentTransform.TransformPosition(LocalPosition)
	 * -- i.e. the FULL affine transform (translation, rotation, and uniform or non-uniform scale),
	 * never just a direction/vector transform, so translation is never dropped. ComponentTransform
	 * is the SPECIFIC previewed instance's transform passed in by the caller (FTransform::Identity
	 * for the entry-level Local-only reference evaluation used for gating/display -- see the audit
	 * note on FVertexMaskForgeWorkingMesh::BoundingBoxMask). Position/bounds are ALWAYS computed in
	 * the SAME axis's chosen space -- never local position against world bounds or vice versa.
	 *
	 * WORLD BOUNDS (audited): computed by transforming EVERY relevant render vertex and taking
	 * min/max of the transformed coordinate -- NEVER by transforming just the 8 corners of the local
	 * bounding box, which would be wrong for a rotated component (a rotated box's world-space AABB
	 * is not simply the transform of its local AABB corners' min/max in the general case here because
	 * we need the exact per-axis extent of the actual geometry, not an AABB-of-an-AABB approximation).
	 * The bounds pass and the value pass therefore both iterate all NumRenderVerts render vertices,
	 * per enabled axis.
	 *
	 * MIRROR / INVERT ORDER (audited, exact contract from the checkpoint spec): for axis A,
	 *   T = (Coordinate - BoundsMin) / (BoundsMax - BoundsMin)
	 *   BaseGradient = EvaluateAxisBaseGradient(T, Position, SafeTransitionWidth)
	 *   AxisMask = bMirror ? max(BaseGradient, EvaluateAxisBaseGradient(1-T, Position, SafeTransitionWidth)) : BaseGradient
	 *   if (bInvert) AxisMask = 1 - AxisMask
	 *   AxisMask = clamp(AxisMask, 0, 1)
	 * Invert is applied to the COMBINED (post-Mirror) result, never to BaseGradient and the mirrored
	 * gradient individually before the maximum -- inverting each side separately would change the
	 * operation mathematically (max(1-a,1-b) != 1-max(a,b) in general) and is explicitly prohibited.
	 *
	 * AXIS COMBINATION (audited): CombinedMask = max over every ENABLED axis's own AxisMask (0.0 if
	 * no axis is enabled, but callers must check bAnyAxisEnabled themselves BEFORE calling this --
	 * see the "Enable at least one Bounding Box axis" contract in RunAutoUpdatePreview -- this
	 * function still safely returns Unavailable if called with none).
	 * Never touches the Primary Color Overlay, MeshDescription, FDynamicMesh3, RenderData, the
	 * source asset, or ComponentTransform's owning component/actor -- only reads FPositionVertexBuffer
	 * positions (read-only) and ComponentTransform (read-only, by value) and writes into the
	 * returned FVertexMaskForgeScalarMask. Render-vertex order, one entry per render vertex, and
	 * seam independence are all preserved exactly as in the single-axis Local Z version.
	 */
	FVertexMaskForgeScalarMask GenerateBoundingBoxMask(
		const FStaticMeshLODResources& LOD0,
		const TStaticArray<FVertexMaskForgeAxisMaskParams, 3>& AxisParams,
		const FTransform& ComponentTransform,
		const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>* CollectiveBounds = nullptr);

	/**
	 * AUDITED (Nanite source-topology support): sibling of GenerateBoundingBoxMask, used ONLY for
	 * entries in Source-Topology mode. INDIVIDUAL BOUNDS ONLY -- Unified Bounds is not supported for
	 * this domain in this checkpoint (an explicit, scoped-down decision, not an oversight: combining a
	 * render-vertex-domain bounds pass with a Dynamic-Mesh-vertex-domain one in the same collective
	 * bounds computation would require its own design, and the common case is one Nanite mesh edited at
	 * a time). CollectiveBounds is therefore never accepted here; ComponentTransform is always this
	 * specific instance's own transform (never a shared/identity reference the way the render-vertex
	 * path's entry-level evaluation uses).
	 *
	 * Otherwise identical math to GenerateBoundingBoxMask (same ResolveAxisCoordinate/
	 * EvaluateAxisBaseGradient/Mirror/Invert/axis-combination-by-maximum contract, unchanged and not
	 * repeated here) -- the only difference is the vertex source: SourceMesh.GetVertex(VertexID)
	 * (world-transformed) instead of LOD0's PositionVertexBuffer.
	 *
	 * INDEX SAFETY (corrected): indexed by Dynamic Mesh Vertex ID, but NEVER assumes SourceMesh is
	 * compact -- Mask.Values/bHasValue are sized by SourceMesh.MaxVertexID() (not VertexCount()) and
	 * written only at indices actually yielded by VertexIndicesItr() (which never yields an invalid
	 * ID); a caller must use TryGetValue(), never index Values directly, exactly per
	 * FVertexMaskForgeScalarMask's own struct-level contract for a sparse domain.
	 */
	FVertexMaskForgeScalarMask GenerateBoundingBoxMaskFromDynamicMesh(
		const UE::Geometry::FDynamicMesh3& SourceMesh,
		const TStaticArray<FVertexMaskForgeAxisMaskParams, 3>& AxisParams,
		const FTransform& ComponentTransform);

	/**
	 * Phase A of the Unified Bounds two-phase pipeline: computes the collective per-axis domain
	 * across every PARTICIPATING component -- every FVertexMaskForgePreviewComponentState with a
	 * live SourceComponent, belonging to a participating SelectedMeshes entry. Fully validates
	 * (compatibility, finiteness, degeneracy) BEFORE returning true -- callers must not touch any
	 * Preview until this returns true, per the two-phase contract. Never approximates: bounds are
	 * accumulated from the SAME real render-vertex positions (LOD0.VertexBuffers.PositionVertexBuffer)
	 * that the evaluation phase and the Accept path both read -- no ComponentBounds, no pivot, no
	 * Actor origin, no position matching.
	 *
	 * bForGeneration selects which entries count as "participating", since this function is shared
	 * by two different moments in the pipeline:
	 *   - true (RunAutoUpdatePreview, BEFORE this pass's regeneration has written anything): an
	 *     entry participates if its WorkingMesh itself is
	 *     Ready -- its CURRENT BoundingBoxMask may still be NotGenerated/stale, since generation is
	 *     what is about to (re)populate it.
	 *   - false (UpdateAllPreviews / BuildAcceptTargets, AFTER generation already ran): an entry
	 *     participates only if it is actually showing a Ready, Source == BoundingBox mask -- Content-
	 *     Browser-only or Fill-sourced entries never participate here.
	 */
	bool ComputeCollectiveAxisBounds(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		const TStaticArray<FVertexMaskForgeAxisMaskParams, 3>& AxisParams,
		const bool bForGeneration,
		TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>& OutBounds,
		FText& OutErrorText);
}
