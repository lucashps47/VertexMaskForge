#pragma once

#include "CoreMinimal.h"

struct FVertexMaskForgeScalarMask;
struct FVertexMaskForgeWorkingMesh;
struct FStaticMeshLODResources;
struct FVertexMaskForgeMaskInstance;

namespace VertexMaskForgeMaterialSlotGenerator
{
	/**
	 * AUDITED (V2-D): the binary raw mask, render-vertex domain -- RawMask[i] = 1.0 iff
	 * WorkingMesh.RenderVertexToMaterialSlot[i] == SelectedSlotIndex, else 0.0; Invert complements
	 * (1.0<->0.0) AFTER that comparison, per the explicit formula. Refuses to generate (Unavailable) if
	 * the lookup itself is invalid/ambiguous (see BuildMaterialSlotLookups) or SelectedSlotIndex is out
	 * of range -- never silently produces a wrong/empty mask.
	 */
	FVertexMaskForgeScalarMask GenerateMaterialSlotMask(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		const FStaticMeshLODResources& LOD0,
		int32 SelectedSlotIndex,
		bool bInvert);

	/**
	 * AUDITED (V2-D): sibling of GenerateMaterialSlotMask for Source-Topology (Nanite) entries --
	 * CORNER-EXACT (Mesh.TriangleCount()*3, indexed by CornerIndex directly), deliberately NOT
	 * Dynamic-Mesh-Vertex-domain like Curvature/Noise: all three corners of a triangle share that
	 * triangle's OWN resolved slot (WorkingMesh.DynamicTriangleToMaterialSlot[TriangleID]), so two
	 * corners at the same position/VertexID on opposite sides of a material boundary correctly read
	 * different values -- see UpdateWorkingColorsSourceTopology's own IndexOverride switch (CornerIndex
	 * case) for how this is consumed.
	 */
	FVertexMaskForgeScalarMask GenerateMaterialSlotMaskFromDynamicMesh(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		int32 SelectedSlotIndex,
		bool bInvert);

	/**
	 * M16-E: the first real integration between a FVertexMaskForgeMaskInstance (M16-A authoring state)
	 * and FVertexMaskForgeInstanceResultStore (M16-C keyed result storage), scoped to Material Slot
	 * only -- see the M16-E checkpoint's own contract for why this is a focused function rather than a
	 * generic per-generator dispatcher.
	 *
	 * A thin orchestration layer, not a reimplementation: calls the SAME GenerateMaterialSlotMask /
	 * GenerateMaterialSlotMaskFromDynamicMesh entry points above (bUseSourceTopology selects which,
	 * exactly mirroring SVertexMaskForgePanel's own legacy call site), then converts the resulting
	 * FVertexMaskForgeScalarMask's Values/bHasValue into a FVertexMaskForgeInstanceMaskResult and stores
	 * it via WorkingMesh.InstanceResults.StoreOrReplace(MaskInstance.InstanceId, ...) -- never touches
	 * WorkingMesh.MaterialSlotMask (the named legacy result), never touches
	 * FVertexMaskForgePreviewComponentState::InstanceResults, never calls the sequential evaluator.
	 *
	 * Rejects deterministically, with no store mutation and no crash/ensure/exception, when:
	 *   - MaskInstance.InstanceId is invalid (FGuid::IsValid() == false);
	 *   - MaskInstance.GeneratorType is not EVertexMaskForgeGeneratorType::MaterialSlot;
	 *   - MaskInstance.Params does not actually hold a FVertexMaskForgeMaterialSlotParams (defensive --
	 *     should be unreachable given the GeneratorType/Params coherence invariant, but never assumed);
	 *   - bUseSourceTopology is false and LOD0 is null (render-vertex domain requires it);
	 *   - the underlying generator call itself returns anything other than
	 *     EVertexMaskForgeScalarMaskState::Ready (mirrors the legacy call site's own "auto-update never
	 *     replaces a valid Preview with incomplete/degenerate data" contract -- a prior valid result for
	 *     the same InstanceId, if any, is left completely untouched on failure).
	 *
	 * LOD0 is ignored when bUseSourceTopology is true (may be null). Returns true iff the result was
	 * computed and stored/replaced in WorkingMesh.InstanceResults.
	 */
	bool GenerateMaterialSlotMaskInstanceResult(
		const FVertexMaskForgeMaskInstance& MaskInstance,
		FVertexMaskForgeWorkingMesh& WorkingMesh,
		bool bUseSourceTopology,
		const FStaticMeshLODResources* LOD0);
}
