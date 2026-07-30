#pragma once

#include "Containers/Array.h"
#include "CoreMinimal.h"
#include "Math/Vector.h"

struct FVertexMaskForgeFillLayer;
class FVertexMaskForgeInstanceResultStore;

/**
 * M16-F: the first real integration between authoring state (FVertexMaskForgeFillLayer/
 * FVertexMaskForgeMaskInstance, M16-A) and the pure sequential math (VertexMaskForgeSequentialEvaluator,
 * M16-B), resolved through keyed result storage (FVertexMaskForgeInstanceResultStore, M16-C) -- scoped
 * to evaluating EXACTLY ONE Fill Layer against an explicitly-supplied result store owner.
 *
 * This module is deliberately a thin RESOLUTION layer, not a reimplementation of the evaluator math: it
 * looks up each Mask Instance's already-generated result by InstanceId, validates it, and hands the
 * resolved scalars to VertexMaskForgeSequentialEvaluator::EvaluateMaskStack / EvaluateFillLayers
 * verbatim. VertexMaskForgeSequentialEvaluator.h/.cpp are untouched by this checkpoint and gain no
 * awareness of the recipe types or the result store -- see that header's own module comment, which this
 * checkpoint deliberately preserves.
 *
 * Read-only with respect to FVertexMaskForgeInstanceResultStore: only Find() is ever called. Never
 * StoreOrReplace/Remove/PruneToInstanceIds/Reset. Never executes a mask generator (Material Slot or any
 * other) -- a Mask Instance whose InstanceId has no existing entry in the supplied store fails
 * deterministically rather than generating one implicitly.
 */

/**
 * Temporary, caller-owned output of evaluating one Fill Layer -- never persisted anywhere (not the
 * recipe, not the Fill Layer, not the Mask Instance, not the result store, not WorkingMesh/
 * PreviewComponentState). Both arrays are indexed identically to the resolved Mask Instance results
 * (whatever domain/cardinality the caller's ExpectedCardinality represents -- this module never
 * interprets or converts between domains).
 */
struct FVertexMaskForgeFillLayerEvaluationOutput
{
	/** Per-index EffectiveMask -- the resolved Mask Stack's own output, before Fill Layer Opacity is
	 *  applied. Exposed alongside Composite mainly for testability; not required by any consumer yet. */
	TArray<float> EffectiveMask;

	/** Per-index Composite = EvaluateFillLayers(BaseValue, {this one layer's resolved input}) at that
	 *  index -- the Fill Layer's own temporary result. */
	TArray<FVector3f> Composite;
};

namespace VertexMaskForgeFillLayerResolution
{
	/**
	 * Resolves and evaluates exactly one FillLayer's Mask Stack against ResultStore, then evaluates the
	 * Fill Layer itself, producing OutResult.EffectiveMask/Composite -- both sized ExpectedCardinality.
	 *
	 * ExpectedCardinality is supplied by the caller, never inferred or defaulted, since an empty Mask
	 * Stack (see EvaluateMaskStack's own empty-array contract, reused verbatim: EffectiveMask=1.0
	 * uniformly, ResultStore is never queried) carries no array to infer a domain size from -- inventing
	 * an arbitrary size would be exactly the kind of silent domain assumption this checkpoint's own
	 * contract forbids.
	 *
	 * Validation (in order, matching this function's own atomicity contract -- OutResult is only written
	 * after every step below succeeds; on any failure OutResult is left completely untouched and
	 * ResultStore is never mutated):
	 *   1. ExpectedCardinality must be > 0.
	 *   2. Every FillLayer.MaskStack entry's InstanceId must be valid (FGuid::IsValid()).
	 *   3. Every InstanceId must resolve via ResultStore.Find() (a missing entry fails outright -- no
	 *      generator is ever run to produce one, no fallback to white/black/legacy/another GUID).
	 *   4. Every resolved result's Values.Num() must equal ExpectedCardinality exactly (no resampling,
	 *      remapping, padding, or truncation across mismatched cardinalities/domains).
	 *   5. Every resolved result must have TryGetValue() succeed at every index in [0, ExpectedCardinality)
	 *      (a sparse/partially-unwritten result -- bHasValue[i]==false anywhere in range -- fails).
	 *
	 * Identity is exclusively InstanceId -- GeneratorType, parameters, slot index, array position, and
	 * payload equality are never consulted for resolution. Iteration order over FillLayer.MaskStack is
	 * the array's own authoring order, unchanged, matching EvaluateMaskStack's own "no reordering, no
	 * grouping by Blend Mode" contract.
	 *
	 * BaseValue is the composite this one layer blends onto (the "CompositeBelow" EvaluateFillLayerStep
	 * expects) -- uniform across every index in this checkpoint, since no underlying per-vertex composite
	 * (baseline colors, a lower Fill Layer's own output, ...) is wired in yet; that remains a future,
	 * separately-authorized orchestration checkpoint's concern.
	 */
	bool EvaluateFillLayerFromKeyedResults(
		const FVertexMaskForgeFillLayer& FillLayer,
		const FVertexMaskForgeInstanceResultStore& ResultStore,
		int32 ExpectedCardinality,
		const FVector3f& BaseValue,
		FVertexMaskForgeFillLayerEvaluationOutput& OutResult);
}
