#include "VertexMaskForgeRecipeEvaluation.h"

#include "VertexMaskForgeFillLayerResolution.h"
#include "VertexMaskForgeRecipeTypes.h"
#include "VertexMaskForgeWorkingMeshTypes.h" // LogVertexMaskForge

namespace VertexMaskForgeRecipeEvaluation
{
	bool EvaluateRecipeFillLayersFromKeyedResults(
		const FVertexMaskForgeRecipe& Recipe,
		const FVertexMaskForgeInstanceResultStore& ResultStore,
		const TConstArrayView<FVector3f> InitialPerSampleBase,
		FVertexMaskForgeRecipeEvaluationOutput& OutResult)
	{
		if (InitialPerSampleBase.Num() <= 0)
		{
			UE_LOG(LogVertexMaskForge, Warning,
				TEXT("VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults: InitialPerSampleBase must be non-empty."));
			return false;
		}

		// Local fold state only -- OutResult is never touched until the very end, after every enabled
		// Fill Layer has succeeded (see this function's own header doc comment for the full atomicity
		// contract). Copied once, up front, from the caller's read-only view.
		TArray<FVector3f> CurrentComposite(InitialPerSampleBase.GetData(), InitialPerSampleBase.Num());

		for (const FVertexMaskForgeFillLayer& Layer : Recipe.FillLayers)
		{
			if (!Layer.bEnabled)
			{
				// AUDITED: skip the M16-G boundary call entirely -- no store lookup for this layer's Mask
				// Instances, composite carried forward unchanged. See this module's own header doc
				// comment for why this must be a skip, not a call that internally no-ops.
				continue;
			}

			FVertexMaskForgeFillLayerEvaluationOutput LayerOutput;
			const bool bLayerSucceeded = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(
				Layer, ResultStore, CurrentComposite, LayerOutput);

			if (!bLayerSucceeded)
			{
				UE_LOG(LogVertexMaskForge, Warning,
					TEXT("VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults: Fill Layer %s failed evaluation -- whole recipe evaluation fails, no partial composite is published."),
					*Layer.LayerId.ToString());
				return false;
			}

			CurrentComposite = MoveTemp(LayerOutput.Composite);
		}

		OutResult.Composite = MoveTemp(CurrentComposite);
		return true;
	}
}
