#pragma once

#include "Containers/ArrayView.h"
#include "Containers/Array.h"
#include "Math/Color.h"

class FVertexMaskForgeDynamicLayerStack;
class FVertexMaskForgeInstanceResultStore;

/**
 * M16-K.5H: the FColor-domain batch sibling of VertexMaskForgeDynamicLayerEvaluator::EvaluateColor --
 * applies that same pure, scalar evaluator once per element of BaseColors, converting at the boundary
 * via VertexMaskForgeColorConversion::ToLinearColorF (in) and ::ToDisplayFColor (out). Zero new
 * composition/blend/conversion math of its own -- purely an indexing loop over already-approved,
 * already-tested primitives.
 *
 * Deliberately knows nothing about topology/domain (render-vertex vs. triangle-corner -- that choice is
 * entirely the caller's, by whatever it puts into BaseColors/ResultStore), Preview Mode, Channel Filter,
 * BaselineColors/CommittedColors/WorkingColors, the legacy VertexMaskForgeMaskStackComposer::ComposeStack
 * algorithm, or any production call site -- not wired into the panel, preview, Accept/Cancel, or
 * persistence by this checkpoint. See VertexMaskForgeDynamicLayerBatchCompositorTests.cpp for the
 * integration proof this checkpoint provides.
 */
namespace VertexMaskForgeDynamicLayerBatchCompositor
{
	/**
	 * Composes one FColor per element of BaseColors, in order, via exactly one
	 * VertexMaskForgeDynamicLayerEvaluator::EvaluateColor call per index (index is purely positional --
	 * this function never inspects or validates which domain/topology it corresponds to; that is the
	 * caller's own responsibility, by construction of BaseColors/ResultStore).
	 *
	 * OutColors is ALWAYS substituted wholesale with a new array of exactly BaseColors.Num() elements --
	 * never appended to, never left partially stale. An empty BaseColors produces an empty OutColors,
	 * discarding whatever OutColors held before the call.
	 *
	 * Safe to call with BaseColors and OutColors referencing the SAME TArray<FColor> (the whole array
	 * passed as both the source view and the destination) -- the implementation computes every element
	 * into a private local buffer first and only assigns it into OutColors as the very last step, so no
	 * write to OutColors ever occurs while BaseColors is still being read, and no reallocation of
	 * OutColors can invalidate an in-progress read of BaseColors.
	 *
	 * Never applies Preview Mode display reduction (see VertexMaskForgeDisplayColorDerivation for that,
	 * a separate, display-only concern) or Channel Filter (that lives entirely inside
	 * FVertexMaskForgeDynamicLayerStack's own per-layer bAffectRed/Green/Blue fields, already consumed
	 * by EvaluateColor itself). Never mutates Stack or ResultStore -- both are taken by const reference.
	 */
	void ComposeColors(
		TConstArrayView<FColor> BaseColors,
		const FVertexMaskForgeDynamicLayerStack& Stack,
		const FVertexMaskForgeInstanceResultStore& ResultStore,
		TArray<FColor>& OutColors);
}
