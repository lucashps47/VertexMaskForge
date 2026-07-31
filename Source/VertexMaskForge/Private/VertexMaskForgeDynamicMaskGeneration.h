#pragma once

#include "CoreMinimal.h"

struct FVertexMaskForgeGeneratorMaskInstance;
struct FVertexMaskForgeWorkingMesh;
struct FStaticMeshLODResources;

/**
 * M16-K.5E: the Dynamic Layer model's own sibling of VertexMaskForgeMaterialSlotGenerator::
 * GenerateMaterialSlotMaskInstanceResult (M16-E's real integration between the Recipe model's
 * FVertexMaskForgeMaskInstance and FVertexMaskForgeInstanceResultStore) -- focused to Material Slot
 * only, same reasoning as that precedent ("a focused function rather than a generic per-generator
 * dispatcher"). This is NOT a generic dispatcher over all 7 EVertexMaskForgeGeneratorType values --
 * only Material Slot has a real generation entry point wired to FVertexMaskForgeInstanceResultStore
 * today, on either identity model.
 *
 * Direction of dependency is deliberate: THIS module depends on both VertexMaskForgeLayerTypes.h (for
 * FVertexMaskForgeGeneratorMaskInstance) and VertexMaskForgeMaterialSlotGenerator.h (for the already-
 * neutral generation entry points) -- never the reverse. VertexMaskForgeMaterialSlotGenerator.h/.cpp
 * are completely unmodified by this checkpoint; they never learn about FVertexMaskForgeGeneratorMaskInstance
 * or the Dynamic Layer model at all.
 *
 * Reuses VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMask/
 * GenerateMaterialSlotMaskFromDynamicMesh verbatim (the SAME neutral, already-public generation math
 * the Recipe model's own orchestrator calls) -- zero duplicated generation logic. Only the
 * validation/orchestration shape (identity/type/params checks, StoreOrReplace) is written once more
 * here, because it operates on a structurally different identity type than
 * GenerateMaterialSlotMaskInstanceResult's own FVertexMaskForgeMaskInstance parameter.
 *
 * Stores under GeneratorMaskInstance.MaskInstanceId -- the Dynamic Layer model's OWN identity, never a
 * Recipe FVertexMaskForgeMaskInstance::InstanceId, never any manual GUID correlation between the two
 * models. No FVertexMaskForgeMaskInstance is ever constructed anywhere in this module.
 */
namespace VertexMaskForgeDynamicMaskGeneration
{
	/**
	 * Generates a Material Slot result for GeneratorMaskInstance and stores it in
	 * WorkingMesh.InstanceResults under GeneratorMaskInstance.MaskInstanceId.
	 *
	 * Rejects deterministically, with no store mutation, when:
	 *   - GeneratorMaskInstance.MaskInstanceId is invalid (FGuid::IsValid() == false);
	 *   - GeneratorMaskInstance.GeneratorType is not EVertexMaskForgeGeneratorType::MaterialSlot;
	 *   - GeneratorMaskInstance.Params does not actually hold a FVertexMaskForgeMaterialSlotParams
	 *     (defensive -- should be unreachable given the GeneratorType/Params coherence invariant
	 *     FVertexMaskForgeDynamicLayerStack's own controlled API already enforces, but never assumed);
	 *   - bUseSourceTopology is false and LOD0 is null (render-vertex domain requires it);
	 *   - the underlying generator call itself returns anything other than
	 *     EVertexMaskForgeScalarMaskState::Ready -- a prior valid result for the same MaskInstanceId, if
	 *     any, is left completely untouched on failure (mirrors GenerateMaterialSlotMaskInstanceResult's
	 *     own "auto-update never replaces a valid result with incomplete/degenerate data" contract).
	 *
	 * LOD0 is ignored when bUseSourceTopology is true (may be null). Returns true iff the result was
	 * computed and stored/replaced in WorkingMesh.InstanceResults.
	 */
	bool GenerateStoredResultForMaterialSlotInstance(
		const FVertexMaskForgeGeneratorMaskInstance& GeneratorMaskInstance,
		FVertexMaskForgeWorkingMesh& WorkingMesh,
		bool bUseSourceTopology,
		const FStaticMeshLODResources* LOD0);
}
