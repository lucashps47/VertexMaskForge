// M16-K.6D-4: see VertexMaskForgeDynamicSourceTopologyComposition.h for the full module contract.
//
// ISOLATION RATIONALE for bypassing VertexMaskForgeDynamicLayerEvaluator::EvaluateColor's masked overload
// and VertexMaskForgeDynamicLayerBatchCompositor::ComposeColors: both require a
// FVertexMaskForgeInstanceResultStore parameter to resolve a masked layer's EffectiveMask by
// MaskInstanceId -- this checkpoint's own instructions forbid this orchestrator from receiving,
// constructing, or touching a FVertexMaskForgeInstanceResultStore of any kind (not even a local,
// function-scoped, never-escaping one), because that type's entire purpose (identity-keyed, mutation-
// tracked, cross-call persistence) is foreign to a purely local, caller-owned, single-call computation --
// introducing one here, even transiently, would blur the exact boundary this checkpoint exists to keep
// sharp. Instead, this module resolves each masked layer's EffectiveMask directly from the freshly
// generated FVertexMaskForgeScalarMask array Pass 1 below produces (positional lookup by corner index,
// never a MaskInstanceId/FGuid lookup of any kind), and reuses the exact same downstream blend-mode fold
// primitives EvaluateColor itself calls (VertexMaskForgeSequentialEvaluator::EvaluateFillLayerStep and
// VertexMaskForgeDynamicLayerEvaluator::TryResolveFillValue) so there is exactly one implementation of
// the actual composition math anywhere in the plugin -- only the "how is EffectiveMask resolved" step
// differs, by necessity.

#include "VertexMaskForgeDynamicSourceTopologyComposition.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "VertexMaskForgeColorConversion.h"
#include "VertexMaskForgeDynamicLayerEvaluator.h"
#include "VertexMaskForgeDynamicLayerStack.h"
#include "VertexMaskForgeLayerTypes.h"
#include "VertexMaskForgeMaskTypes.h"
#include "VertexMaskForgeMaterialSlotGenerator.h"
#include "VertexMaskForgeRecipeTypes.h"
#include "VertexMaskForgeSequentialEvaluator.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

bool VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(
	const FVertexMaskForgeWorkingMesh& WorkingMesh,
	const FVertexMaskForgeDynamicLayerStack& Stack,
	TConstArrayView<FColor> BaseColors,
	TArray<FColor>& OutComposedColors)
{
	if (!WorkingMesh.Mesh.IsValid())
	{
		return false;
	}

	const int32 ExpectedCornerCount = WorkingMesh.Mesh->TriangleCount() * 3;
	if (BaseColors.Num() != ExpectedCornerCount)
	{
		return false;
	}

	const TArray<FVertexMaskForgeLayer>& Layers = Stack.GetLayers();

	// --- Pass 1: resolve every ENABLED masked layer's Material Slot scalar array up front, all-or-
	// nothing -- a structural failure (unsupported generator type, or the generator itself not Ready)
	// fails the WHOLE call before any per-corner work begins; nothing is ever partially composed. ---
	TArray<FVertexMaskForgeScalarMask> LayerMaterialSlotMasks;
	LayerMaterialSlotMasks.SetNum(Layers.Num());

	for (int32 LayerIndex = 0; LayerIndex < Layers.Num(); ++LayerIndex)
	{
		const FVertexMaskForgeLayer& Layer = Layers[LayerIndex];
		if (!Layer.bEnabled || !Layer.Mask.IsSet())
		{
			continue;
		}

		if (Layer.Mask->GeneratorType != EVertexMaskForgeGeneratorType::MaterialSlot)
		{
			// Explicit, whole-call failure -- this checkpoint's own Material-Slot-only vertical slice
			// never silently skips or treats an unsupported generator type as Fill-only.
			return false;
		}

		const FVertexMaskForgeMaterialSlotParams* SlotParams = Layer.Mask->Params.TryGet<FVertexMaskForgeMaterialSlotParams>();
		if (!SlotParams)
		{
			// Defensive -- should be unreachable given FVertexMaskForgeDynamicLayerStack's own
			// GeneratorType/Params coherence invariant, but never assumed (mirrors every other
			// Material Slot caller's own defensive check in this codebase).
			return false;
		}

		FVertexMaskForgeScalarMask GeneratedMask = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(
			WorkingMesh, SlotParams->SelectedSlotIndex, SlotParams->bInvert);
		if (GeneratedMask.State != EVertexMaskForgeScalarMaskState::Ready || GeneratedMask.Values.Num() != ExpectedCornerCount)
		{
			return false;
		}

		LayerMaterialSlotMasks[LayerIndex] = MoveTemp(GeneratedMask);
	}

	// --- Pass 2: fold, per corner, strictly in Stack order, into a private local buffer -- OutComposedColors
	// is only ever touched by the final MoveTemp below, on success. ---
	TArray<FColor> LocalOutput;
	LocalOutput.SetNumUninitialized(ExpectedCornerCount);

	for (int32 CornerIndex = 0; CornerIndex < ExpectedCornerCount; ++CornerIndex)
	{
		const FVector4f BaseColor = VertexMaskForgeColorConversion::ToLinearColorF(BaseColors[CornerIndex]);
		FVector3f Composite(BaseColor.X, BaseColor.Y, BaseColor.Z);

		for (int32 LayerIndex = 0; LayerIndex < Layers.Num(); ++LayerIndex)
		{
			const FVertexMaskForgeLayer& Layer = Layers[LayerIndex];
			if (!Layer.bEnabled)
			{
				continue;
			}

			float FillValue = 0.0f;
			if (!VertexMaskForgeDynamicLayerEvaluator::TryResolveFillValue(Layer.Fill, FillValue))
			{
				continue;
			}

			// Layer.Mask unset -> EffectiveMask implicitly 1.0, exactly mirroring EvaluateColor's own
			// "ResultStore is NEVER consulted for such a layer" contract (here: the local Material Slot
			// array is never consulted either). Layer.Mask set -> Pass 1 already guaranteed a Ready,
			// exactly-sized LayerMaterialSlotMasks[LayerIndex] for every ENABLED masked layer.
			const float EffectiveMask = Layer.Mask.IsSet() ? LayerMaterialSlotMasks[LayerIndex].Values[CornerIndex] : 1.0f;

			const FVector3f PaintValue = FVector3f(FillValue, FillValue, FillValue) * EffectiveMask;
			const FVector3f LayerOutput = VertexMaskForgeSequentialEvaluator::EvaluateFillLayerStep(Composite, PaintValue, Layer.BlendMode, Layer.Opacity);

			Composite.X = Layer.bAffectRed ? LayerOutput.X : Composite.X;
			Composite.Y = Layer.bAffectGreen ? LayerOutput.Y : Composite.Y;
			Composite.Z = Layer.bAffectBlue ? LayerOutput.Z : Composite.Z;
		}

		Composite.X = FMath::Clamp(Composite.X, 0.0f, 1.0f);
		Composite.Y = FMath::Clamp(Composite.Y, 0.0f, 1.0f);
		Composite.Z = FMath::Clamp(Composite.Z, 0.0f, 1.0f);

		const FVector4f FinalColor(Composite.X, Composite.Y, Composite.Z, BaseColor.W);
		LocalOutput[CornerIndex] = VertexMaskForgeColorConversion::ToDisplayFColor(FinalColor);
	}

	OutComposedColors = MoveTemp(LocalOutput);
	return true;
}
