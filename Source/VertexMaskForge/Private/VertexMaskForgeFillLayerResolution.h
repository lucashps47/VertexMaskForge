#pragma once

#include "Containers/Array.h"
#include "Containers/ArrayView.h"
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
 * M16-G: generalized the single uniform FVector3f BaseValue (M16-F's own identified architectural risk
 * -- a single value cannot represent per-sample BaselineColors, a lower Fill Layer's own output, or any
 * spatially-varying composite) into a per-sample base view, PerSampleBase. There was no production call
 * site for the M16-F signature (only its own test file used it), so the signature was REPLACED outright
 * rather than kept alongside a convenience overload -- see this checkpoint's own audit for why no
 * uniform-base overload was preserved.
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
 * PreviewComponentState). Both arrays are indexed identically to PerSampleBase and to the resolved Mask
 * Instance results (whatever domain/cardinality PerSampleBase.Num() represents -- this module never
 * interprets or converts between domains).
 *
 * AUDITED (M16-G): Composite itself is exactly the kind of per-sample array a lower Fill Layer's output
 * needs to become -- passing this struct's own Composite as a future call's PerSampleBase is the
 * intended composability path (see EvaluateFillLayerFromKeyedResults's own doc comment). No orchestration
 * exists to do this automatically; the caller performs it explicitly, one call at a time.
 */
struct FVertexMaskForgeFillLayerEvaluationOutput
{
	/** Per-index EffectiveMask -- the resolved Mask Stack's own output, before Fill Layer Opacity is
	 *  applied. Exposed alongside Composite mainly for testability; not required by any consumer yet. */
	TArray<float> EffectiveMask;

	/** Per-index Composite = EvaluateFillLayers(PerSampleBase[Index], {this one layer's resolved input})
	 *  -- the Fill Layer's own temporary result, one entry per sample. */
	TArray<FVector3f> Composite;
};

namespace VertexMaskForgeFillLayerResolution
{
	/**
	 * Resolves and evaluates exactly one FillLayer's Mask Stack against ResultStore, then evaluates the
	 * Fill Layer itself against PerSampleBase, producing OutResult.EffectiveMask/Composite -- both sized
	 * PerSampleBase.Num().
	 *
	 * AUDITED (M16-G): PerSampleBase.Num() IS the expected cardinality -- there is no separate
	 * ExpectedCardinality parameter, so there is exactly one source of truth for the domain size (the
	 * M16-F signature took BOTH an explicit ExpectedCardinality and an unrelated uniform BaseValue,
	 * which could never actually diverge from each other in practice but still represented two
	 * independent inputs; this signature removes that redundancy). PerSampleBase must be non-empty
	 * (Num() > 0) -- same "domain size must be known and positive, never invented" contract the M16-F
	 * ExpectedCardinality parameter already enforced, now expressed through the base array itself.
	 *
	 * Validation (in order, matching this function's own atomicity contract -- OutResult is only written
	 * after every step below succeeds; on any failure OutResult is left completely untouched, PerSampleBase
	 * is never mutated, and ResultStore is never mutated):
	 *   1. PerSampleBase.Num() must be > 0.
	 *   2. Every FillLayer.MaskStack entry's InstanceId must be valid (FGuid::IsValid()).
	 *   3. Every InstanceId must resolve via ResultStore.Find() (a missing entry fails outright -- no
	 *      generator is ever run to produce one, no fallback to white/black/legacy/another GUID).
	 *   4. Every resolved result's Values.Num() must equal PerSampleBase.Num() exactly (no resampling,
	 *      remapping, padding, or truncation across mismatched cardinalities/domains).
	 *   5. Every resolved result must have TryGetValue() succeed at every index in [0, PerSampleBase.Num())
	 *      (a sparse/partially-unwritten result -- bHasValue[i]==false anywhere in range -- fails).
	 *
	 * Identity is exclusively InstanceId -- GeneratorType, parameters, slot index, array position, and
	 * payload equality are never consulted for resolution. Iteration order over FillLayer.MaskStack is
	 * the array's own authoring order, unchanged, matching EvaluateMaskStack's own "no reordering, no
	 * grouping by Blend Mode" contract -- reorder only ever changes the EffectiveMask fold, never which
	 * PerSampleBase[Index] a given sample's Composite is computed from.
	 *
	 * PerSampleBase[Index] is the composite this one layer blends onto AT THAT SAMPLE (the "CompositeBelow"
	 * EvaluateFillLayerStep expects) -- read exactly once per index, verbatim, never averaged, reduced to
	 * a uniform value, broadcast from index 0, padded, or truncated. An empty Mask Stack still reads every
	 * PerSampleBase[Index] individually (EffectiveMask is uniformly 1.0 in that case -- see
	 * EvaluateMaskStack's own empty-array contract -- but Composite[Index] is still computed per-sample
	 * from PerSampleBase[Index], so a stack-empty Fill Layer over a spatially-varying base still produces
	 * a spatially-varying Composite).
	 */
	bool EvaluateFillLayerFromKeyedResults(
		const FVertexMaskForgeFillLayer& FillLayer,
		const FVertexMaskForgeInstanceResultStore& ResultStore,
		TConstArrayView<FVector3f> PerSampleBase,
		FVertexMaskForgeFillLayerEvaluationOutput& OutResult);
}
