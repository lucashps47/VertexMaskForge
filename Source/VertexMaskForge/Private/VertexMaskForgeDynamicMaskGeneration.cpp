#include "VertexMaskForgeDynamicMaskGeneration.h"

#include "VertexMaskForgeLayerTypes.h"
#include "VertexMaskForgeMaterialSlotGenerator.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace VertexMaskForgeDynamicMaskGeneration
{
	bool GenerateStoredResultForMaterialSlotInstance(
		const FVertexMaskForgeGeneratorMaskInstance& GeneratorMaskInstance,
		FVertexMaskForgeWorkingMesh& WorkingMesh,
		const bool bUseSourceTopology,
		const FStaticMeshLODResources* LOD0)
	{
		// AUDITED (M16-K.5E): same quiet, deterministic, non-crashing rejection policy already
		// established by VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult
		// (M16-E) for the Recipe model -- a plain Warning-level log, no ensure()/check(), no second
		// policy introduced here.
		if (!GeneratorMaskInstance.MaskInstanceId.IsValid())
		{
			UE_LOG(LogVertexMaskForge, Warning,
				TEXT("VertexMaskForgeDynamicMaskGeneration::GenerateStoredResultForMaterialSlotInstance: refusing to run for an invalid MaskInstanceId."));
			return false;
		}

		if (GeneratorMaskInstance.GeneratorType != EVertexMaskForgeGeneratorType::MaterialSlot)
		{
			UE_LOG(LogVertexMaskForge, Warning,
				TEXT("VertexMaskForgeDynamicMaskGeneration::GenerateStoredResultForMaterialSlotInstance: GeneratorMaskInstance %s is not a Material Slot instance."),
				*GeneratorMaskInstance.MaskInstanceId.ToString());
			return false;
		}

		// Defensive only -- GeneratorType/Params are expected to always stay coherent (see
		// FVertexMaskForgeDynamicLayerStack::SetLayerMaskGeneratorType/SetLayerMaskParams and
		// MakeVertexMaskForgeGeneratorParams), but this function never assumes that invariant holds
		// without checking.
		const FVertexMaskForgeMaterialSlotParams* Params = GeneratorMaskInstance.Params.TryGet<FVertexMaskForgeMaterialSlotParams>();
		if (!Params)
		{
			UE_LOG(LogVertexMaskForge, Warning,
				TEXT("VertexMaskForgeDynamicMaskGeneration::GenerateStoredResultForMaterialSlotInstance: GeneratorMaskInstance %s has GeneratorType MaterialSlot but Params does not hold FVertexMaskForgeMaterialSlotParams."),
				*GeneratorMaskInstance.MaskInstanceId.ToString());
			return false;
		}

		if (!bUseSourceTopology && !LOD0)
		{
			UE_LOG(LogVertexMaskForge, Warning,
				TEXT("VertexMaskForgeDynamicMaskGeneration::GenerateStoredResultForMaterialSlotInstance: GeneratorMaskInstance %s requires a valid LOD0 in the render-vertex domain."),
				*GeneratorMaskInstance.MaskInstanceId.ToString());
			return false;
		}

		// Reuses the SAME neutral generation entry points the Recipe model's own orchestrator
		// (GenerateMaterialSlotMaskInstanceResult) calls -- never a reimplementation of the Material
		// Slot algorithm. Computed entirely into a local value first; the keyed store is only touched
		// after a Ready result is confirmed below (atomicity).
		FVertexMaskForgeScalarMask Mask = bUseSourceTopology
			? VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(WorkingMesh, Params->SelectedSlotIndex, Params->bInvert)
			: VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMask(WorkingMesh, *LOD0, Params->SelectedSlotIndex, Params->bInvert);

		if (Mask.State != EVertexMaskForgeScalarMaskState::Ready)
		{
			// No store mutation -- any prior valid result for this exact MaskInstanceId is left
			// untouched.
			return false;
		}

		FVertexMaskForgeInstanceMaskResult Result;
		Result.Values = MoveTemp(Mask.Values);
		Result.bHasValue = MoveTemp(Mask.bHasValue);

		return WorkingMesh.InstanceResults.StoreOrReplace(GeneratorMaskInstance.MaskInstanceId, MoveTemp(Result));
	}
}
