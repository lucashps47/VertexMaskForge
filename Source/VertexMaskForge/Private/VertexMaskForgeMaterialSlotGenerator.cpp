#include "VertexMaskForgeMaterialSlotGenerator.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "StaticMeshResources.h"
#include "VertexMaskForgeRecipeTypes.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace VertexMaskForgeMaterialSlotGenerator
{
	FVertexMaskForgeScalarMask GenerateMaterialSlotMask(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		const FStaticMeshLODResources& LOD0,
		const int32 SelectedSlotIndex,
		const bool bInvert)
	{
		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::MaterialSlot;

		const int32 NumRenderVerts = static_cast<int32>(LOD0.VertexBuffers.PositionVertexBuffer.GetNumVertices());
		Mask.RenderVertexCount = NumRenderVerts;

		if (NumRenderVerts <= 0
			|| !WorkingMesh.bMaterialSlotResolutionValid
			|| WorkingMesh.bRenderVertexMaterialSlotAmbiguous
			|| WorkingMesh.RenderVertexToMaterialSlot.Num() != NumRenderVerts
			|| !WorkingMesh.MaterialSlotOptions.IsValidIndex(SelectedSlotIndex))
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		Mask.Values.SetNumUninitialized(NumRenderVerts);
		Mask.bHasValue.Init(true, NumRenderVerts);

		double Sum = 0.0;
		for (int32 i = 0; i < NumRenderVerts; ++i)
		{
			const bool bSelected = WorkingMesh.RenderVertexToMaterialSlot[i] == SelectedSlotIndex;
			const float Value = (bSelected != bInvert) ? 1.0f : 0.0f;
			Mask.Values[i] = Value;
			++Mask.NumValidValues;
			Sum += Value;
			Mask.MinValue = (Mask.NumValidValues == 1) ? Value : FMath::Min(Mask.MinValue, Value);
			Mask.MaxValue = (Mask.NumValidValues == 1) ? Value : FMath::Max(Mask.MaxValue, Value);
			if (Value <= FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearZero; }
			if (Value >= 1.0f - FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearOne; }
		}
		Mask.MeanValue = (Mask.NumValidValues > 0) ? static_cast<float>(Sum / Mask.NumValidValues) : 0.0f;
		Mask.State = (Mask.NumValidValues > 0) ? EVertexMaskForgeScalarMaskState::Ready : EVertexMaskForgeScalarMaskState::Unavailable;

		return Mask;
	}

	FVertexMaskForgeScalarMask GenerateMaterialSlotMaskFromDynamicMesh(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		const int32 SelectedSlotIndex,
		const bool bInvert)
	{
		using namespace UE::Geometry;

		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::MaterialSlot;

		if (!WorkingMesh.Mesh.IsValid() || !WorkingMesh.bMaterialSlotResolutionValid
			|| !WorkingMesh.MaterialSlotOptions.IsValidIndex(SelectedSlotIndex))
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		const FDynamicMesh3& Mesh = *WorkingMesh.Mesh;
		const int32 NumCorners = Mesh.TriangleCount() * 3;
		Mask.RenderVertexCount = NumCorners;

		if (NumCorners <= 0)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		Mask.Values.SetNumUninitialized(NumCorners);
		Mask.bHasValue.Init(true, NumCorners);

		double Sum = 0.0;
		int32 CornerIndex = 0;
		for (const int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			const int32 ResolvedSlot = WorkingMesh.DynamicTriangleToMaterialSlot.IsValidIndex(TriangleID)
				? WorkingMesh.DynamicTriangleToMaterialSlot[TriangleID]
				: INDEX_NONE;
			const bool bSelected = ResolvedSlot == SelectedSlotIndex;
			const float Value = (bSelected != bInvert) ? 1.0f : 0.0f;
			for (int32 Corner = 0; Corner < 3; ++Corner, ++CornerIndex)
			{
				Mask.Values[CornerIndex] = Value;
				++Mask.NumValidValues;
				Sum += Value;
				Mask.MinValue = (Mask.NumValidValues == 1) ? Value : FMath::Min(Mask.MinValue, Value);
				Mask.MaxValue = (Mask.NumValidValues == 1) ? Value : FMath::Max(Mask.MaxValue, Value);
				if (Value <= FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearZero; }
				if (Value >= 1.0f - FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearOne; }
			}
		}
		Mask.MeanValue = (Mask.NumValidValues > 0) ? static_cast<float>(Sum / Mask.NumValidValues) : 0.0f;
		Mask.State = (Mask.NumValidValues > 0) ? EVertexMaskForgeScalarMaskState::Ready : EVertexMaskForgeScalarMaskState::Unavailable;

		return Mask;
	}

	bool GenerateMaterialSlotMaskInstanceResult(
		const FVertexMaskForgeMaskInstance& MaskInstance,
		FVertexMaskForgeWorkingMesh& WorkingMesh,
		const bool bUseSourceTopology,
		const FStaticMeshLODResources* LOD0)
	{
		// AUDITED (M16-E): same quiet, deterministic, non-crashing rejection policy already established
		// by FVertexMaskForgeInstanceResultStore::StoreOrReplace (M16-C) -- a plain Warning-level log, no
		// ensure()/check(), no second policy introduced here.
		if (!MaskInstance.InstanceId.IsValid())
		{
			UE_LOG(LogVertexMaskForge, Warning,
				TEXT("VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult: refusing to run for an invalid InstanceId."));
			return false;
		}

		if (MaskInstance.GeneratorType != EVertexMaskForgeGeneratorType::MaterialSlot)
		{
			UE_LOG(LogVertexMaskForge, Warning,
				TEXT("VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult: MaskInstance %s is not a Material Slot instance."),
				*MaskInstance.InstanceId.ToString());
			return false;
		}

		// Defensive only -- GeneratorType/Params are expected to always stay coherent (see
		// FVertexMaskForgeMaskInstance::Make/MakeVertexMaskForgeGeneratorParams), but this function never
		// assumes that invariant holds without checking.
		const FVertexMaskForgeMaterialSlotParams* Params = MaskInstance.Params.TryGet<FVertexMaskForgeMaterialSlotParams>();
		if (!Params)
		{
			UE_LOG(LogVertexMaskForge, Warning,
				TEXT("VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult: MaskInstance %s has GeneratorType MaterialSlot but Params does not hold FVertexMaskForgeMaterialSlotParams."),
				*MaskInstance.InstanceId.ToString());
			return false;
		}

		if (!bUseSourceTopology && !LOD0)
		{
			UE_LOG(LogVertexMaskForge, Warning,
				TEXT("VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult: MaskInstance %s requires a valid LOD0 in the render-vertex domain."),
				*MaskInstance.InstanceId.ToString());
			return false;
		}

		// Same generator entry points the legacy panel call site uses (SVertexMaskForgePanel::
		// RunAutoUpdatePreview) -- never a reimplementation of the Material Slot algorithm. Computed
		// entirely into a local value first; the keyed store is only touched after a Ready result is
		// confirmed below (atomicity -- see the function's own header doc comment).
		FVertexMaskForgeScalarMask Mask = bUseSourceTopology
			? GenerateMaterialSlotMaskFromDynamicMesh(WorkingMesh, Params->SelectedSlotIndex, Params->bInvert)
			: GenerateMaterialSlotMask(WorkingMesh, *LOD0, Params->SelectedSlotIndex, Params->bInvert);

		if (Mask.State != EVertexMaskForgeScalarMaskState::Ready)
		{
			// No store mutation -- any prior valid result for this exact InstanceId is left untouched,
			// exactly mirroring the legacy call site's own "auto-update never replaces a valid Preview
			// with incomplete/degenerate data" contract.
			return false;
		}

		FVertexMaskForgeInstanceMaskResult Result;
		Result.Values = MoveTemp(Mask.Values);
		Result.bHasValue = MoveTemp(Mask.bHasValue);

		return WorkingMesh.InstanceResults.StoreOrReplace(MaskInstance.InstanceId, MoveTemp(Result));
	}
}
