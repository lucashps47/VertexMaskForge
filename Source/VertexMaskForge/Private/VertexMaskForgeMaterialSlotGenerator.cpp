#include "VertexMaskForgeMaterialSlotGenerator.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "StaticMeshResources.h"
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
}
