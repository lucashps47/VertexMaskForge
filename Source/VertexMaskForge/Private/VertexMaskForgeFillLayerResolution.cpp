#include "VertexMaskForgeFillLayerResolution.h"

#include "VertexMaskForgeInstanceResultStore.h"
#include "VertexMaskForgeRecipeTypes.h"
#include "VertexMaskForgeSequentialEvaluator.h"
#include "VertexMaskForgeWorkingMeshTypes.h" // LogVertexMaskForge

namespace VertexMaskForgeFillLayerResolution
{
	bool EvaluateFillLayerFromKeyedResults(
		const FVertexMaskForgeFillLayer& FillLayer,
		const FVertexMaskForgeInstanceResultStore& ResultStore,
		const TConstArrayView<FVector3f> PerSampleBase,
		FVertexMaskForgeFillLayerEvaluationOutput& OutResult)
	{
		const int32 ExpectedCardinality = PerSampleBase.Num();
		if (ExpectedCardinality <= 0)
		{
			UE_LOG(LogVertexMaskForge, Warning,
				TEXT("VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults: PerSampleBase must be non-empty."));
			return false;
		}

		// Resolve and validate every Mask Instance's keyed result UP FRONT, strictly in the Fill Layer's
		// own MaskStack authoring order -- identity is exclusively InstanceId (never GeneratorType,
		// parameters, slot index, array position, or payload equality). Nothing is written to OutResult
		// until every entry below has been proven resolvable and cardinality-compatible.
		TArray<const FVertexMaskForgeInstanceMaskResult*> ResolvedResults;
		ResolvedResults.Reserve(FillLayer.MaskStack.Num());

		for (const FVertexMaskForgeMaskInstance& Instance : FillLayer.MaskStack)
		{
			if (!Instance.InstanceId.IsValid())
			{
				UE_LOG(LogVertexMaskForge, Warning,
					TEXT("VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults: Mask Instance has an invalid InstanceId."));
				return false;
			}

			const FVertexMaskForgeInstanceMaskResult* Found = ResultStore.Find(Instance.InstanceId);
			if (!Found)
			{
				UE_LOG(LogVertexMaskForge, Warning,
					TEXT("VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults: no keyed result for InstanceId %s -- generation is never implied by evaluation."),
					*Instance.InstanceId.ToString());
				return false;
			}

			if (Found->Values.Num() != ExpectedCardinality)
			{
				UE_LOG(LogVertexMaskForge, Warning,
					TEXT("VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults: InstanceId %s result cardinality %d does not match expected %d."),
					*Instance.InstanceId.ToString(), Found->Values.Num(), ExpectedCardinality);
				return false;
			}

			ResolvedResults.Add(Found);
		}

		// Compute entirely into local storage -- OutResult is only touched after total success (see this
		// function's own header doc comment for the full atomicity contract).
		TArray<float> LocalEffectiveMask;
		TArray<FVector3f> LocalComposite;
		LocalEffectiveMask.SetNumUninitialized(ExpectedCardinality);
		LocalComposite.SetNumUninitialized(ExpectedCardinality);

		TArray<FVertexMaskForgeMaskEvaluationInput> MaskInputs;
		MaskInputs.SetNum(FillLayer.MaskStack.Num());

		for (int32 Index = 0; Index < ExpectedCardinality; ++Index)
		{
			for (int32 InstanceIndex = 0; InstanceIndex < FillLayer.MaskStack.Num(); ++InstanceIndex)
			{
				const FVertexMaskForgeMaskInstance& Instance = FillLayer.MaskStack[InstanceIndex];
				float Value = 0.0f;
				if (!ResolvedResults[InstanceIndex]->TryGetValue(Index, Value))
				{
					UE_LOG(LogVertexMaskForge, Warning,
						TEXT("VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults: InstanceId %s has no value at index %d."),
						*Instance.InstanceId.ToString(), Index);
					return false;
				}

				FVertexMaskForgeMaskEvaluationInput& Input = MaskInputs[InstanceIndex];
				Input.MaskValue = Value;
				Input.BlendMode = Instance.BlendMode;
				Input.Opacity = Instance.Opacity;
				Input.bEnabled = Instance.bEnabled;
			}

			// Same primitive the future Mask Stack orchestration will use -- no independent math exists
			// in this module.
			const float Effective = VertexMaskForgeSequentialEvaluator::EvaluateMaskStack(MaskInputs);
			LocalEffectiveMask[Index] = Effective;

			FVertexMaskForgeLayerEvaluationInput LayerInput;
			LayerInput.FillValue = FillLayer.FillValue;
			LayerInput.BlendMode = FillLayer.BlendMode;
			LayerInput.Opacity = FillLayer.Opacity;
			LayerInput.bEnabled = FillLayer.bEnabled;
			LayerInput.EffectiveMask = Effective;

			// Exactly one Fill Layer -- a single-element view into the real Fill Layer evaluator, never a
			// reimplementation of EvaluateFillLayerStep's math. PerSampleBase[Index] is read verbatim, once,
			// for this exact index -- never averaged, reduced, broadcast from index 0, padded, or truncated.
			LocalComposite[Index] = VertexMaskForgeSequentialEvaluator::EvaluateFillLayers(
				PerSampleBase[Index], MakeArrayView(&LayerInput, 1));
		}

		OutResult.EffectiveMask = MoveTemp(LocalEffectiveMask);
		OutResult.Composite = MoveTemp(LocalComposite);
		return true;
	}
}
