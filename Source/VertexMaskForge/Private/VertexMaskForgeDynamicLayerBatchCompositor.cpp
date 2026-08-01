#include "VertexMaskForgeDynamicLayerBatchCompositor.h"

#include "VertexMaskForgeColorConversion.h"
#include "VertexMaskForgeDynamicLayerEvaluator.h"
#include "VertexMaskForgeDynamicLayerStack.h"
#include "VertexMaskForgeInstanceResultStore.h"

namespace VertexMaskForgeDynamicLayerBatchCompositor
{
	void ComposeColors(
		TConstArrayView<FColor> BaseColors,
		const FVertexMaskForgeDynamicLayerStack& Stack,
		const FVertexMaskForgeInstanceResultStore& ResultStore,
		TArray<FColor>& OutColors)
	{
		// Computed entirely into a private local buffer first -- OutColors is never written to while
		// BaseColors is still being read, so this is safe even when BaseColors and OutColors reference
		// the same underlying TArray (BaseColors[Index] is always read from the pre-call buffer, never
		// from a partially-overwritten one).
		TArray<FColor> LocalColors;
		LocalColors.Reserve(BaseColors.Num());

		for (int32 Index = 0; Index < BaseColors.Num(); ++Index)
		{
			const FVector4f BaseColor = VertexMaskForgeColorConversion::ToLinearColorF(BaseColors[Index]);

			const FVector4f ComposedColor = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(
				BaseColor,
				Stack,
				ResultStore,
				Index);

			LocalColors.Add(VertexMaskForgeColorConversion::ToDisplayFColor(ComposedColor));
		}

		OutColors = MoveTemp(LocalColors);
	}
}
