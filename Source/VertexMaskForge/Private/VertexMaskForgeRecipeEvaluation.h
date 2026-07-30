#pragma once

#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "CoreMinimal.h"
#include "Math/Vector.h"

struct FVertexMaskForgeRecipe;
class FVertexMaskForgeInstanceResultStore;

/**
 * M16-H: ordered, whole-recipe orchestration on top of the M16-F/G single-Fill-Layer boundary
 * (VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults) -- folds every Fill Layer of
 * EXACTLY ONE FVertexMaskForgeRecipe, in authoring order, threading each layer's own Composite output
 * into the next layer's PerSampleBase input.
 *
 * This module is deliberately ORCHESTRATION ONLY: it never resolves an InstanceId itself, never touches
 * FVertexMaskForgeInstanceResultStore beyond passing it through unchanged to the M16-G boundary, never
 * calls VertexMaskForgeSequentialEvaluator directly, and never executes a mask generator. Every actual
 * per-layer resolution/validation/evaluation is delegated verbatim to
 * VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults -- exactly one call per
 * EFFECTIVELY EVALUATED Fill Layer (a disabled Fill Layer is skipped without ever calling it -- see
 * EvaluateRecipeFillLayersFromKeyedResults's own doc comment for why).
 */

/**
 * Temporary, caller-owned output of evaluating one recipe's ordered Fill Layers -- never persisted
 * anywhere (not the recipe, not any Fill Layer, not the result store). Deliberately minimal: only the
 * final per-sample Composite is kept. Per-layer intermediate composites, EffectiveMasks, and resolved
 * payloads are all discarded once the fold moves past them -- nothing to reuse, trace, or resume from
 * exists here by design.
 */
struct FVertexMaskForgeRecipeEvaluationOutput
{
	/** Final per-sample composite after every enabled Fill Layer has been folded in authoring order.
	 *  Indexed identically to InitialPerSampleBase; dense, exactly InitialPerSampleBase.Num() entries. */
	TArray<FVector3f> Composite;
};

namespace VertexMaskForgeRecipeEvaluation
{
	/**
	 * Folds every Fill Layer in Recipe.FillLayers, strictly in array (authoring) order, starting from
	 * InitialPerSampleBase:
	 *
	 *   CurrentComposite = InitialPerSampleBase (copied once, locally)
	 *   for each FillLayer in Recipe.FillLayers (authoring order):
	 *       if !FillLayer.bEnabled: continue                          // no store lookup, no change
	 *       if !EvaluateFillLayerFromKeyedResults(FillLayer, ResultStore, CurrentComposite, LayerOutput):
	 *           return false                                          // whole evaluation fails, nothing published
	 *       CurrentComposite = LayerOutput.Composite
	 *   OutResult.Composite = CurrentComposite
	 *
	 * AUDITED (bEnabled): FVertexMaskForgeFillLayer::bEnabled is real, already-approved M16-A authoring
	 * state (default true), and VertexMaskForgeSequentialEvaluator::EvaluateFillLayers already has a
	 * tested contract for a disabled Fill Layer entry: pure passthrough, identical to removing the layer
	 * (see VertexMaskForgeSequentialEvaluatorTests.cpp's own "bEnabled=false -> passthrough" case). This
	 * function reproduces that exact passthrough semantics AT THE RECIPE LEVEL, but by skipping the call
	 * to EvaluateFillLayerFromKeyedResults entirely for a disabled layer -- not by calling it and letting
	 * it internally no-op -- because EvaluateFillLayerFromKeyedResults resolves every Mask Instance in
	 * FillLayer.MaskStack UNCONDITIONALLY, regardless of FillLayer.bEnabled; a disabled layer whose
	 * MaskStack references InstanceIds absent from ResultStore must never fail evaluation just because it
	 * is disabled (its masks are irrelevant to the output either way). Skipping the call is therefore
	 * required for correctness, not merely an optimization -- see this function's own audit for the full
	 * reasoning.
	 *
	 * AUDITED (empty recipe): Recipe.FillLayers.Empty() -> the loop body never runs -> CurrentComposite is
	 * published unchanged from InitialPerSampleBase -- the empty recipe acts as an identity transform,
	 * exactly matching the pattern an empty Mask Stack already has at the single-Fill-Layer level. Zero
	 * store lookups, zero EffectiveMask ever created.
	 *
	 * AUDITED (base cardinality): InitialPerSampleBase.Num() remains the sole domain-size authority, same
	 * as M16-G's own PerSampleBase -- there is no separate ExpectedCardinality parameter here either.
	 * InitialPerSampleBase must be non-empty (Num() > 0); an empty base fails deterministically, same
	 * policy EvaluateFillLayerFromKeyedResults already enforces (this function's own first validation
	 * step reproduces that check before folding a single layer, so an empty base fails identically whether
	 * the recipe is empty or not).
	 *
	 * Atomicity: OutResult is only assigned after every enabled Fill Layer has succeeded -- the fold's own
	 * per-iteration CurrentComposite/LayerOutput are purely local; on ANY layer's failure (including the
	 * very first or very last), the function returns false immediately and OutResult is left completely
	 * untouched, exactly like EvaluateFillLayerFromKeyedResults's own atomicity contract, which this
	 * function inherits rather than reimplements. InitialPerSampleBase, Recipe, and ResultStore are never
	 * mutated by this function -- see the const/view parameter types.
	 *
	 * Never validates non-finite (NaN/Inf) values anywhere -- neither M16-B nor M16-G's own
	 * EvaluateFillLayerFromKeyedResults do, and this function does not add that validation either (see
	 * this module's own module comment for why: preserving, not inventing, the existing contract).
	 */
	bool EvaluateRecipeFillLayersFromKeyedResults(
		const FVertexMaskForgeRecipe& Recipe,
		const FVertexMaskForgeInstanceResultStore& ResultStore,
		TConstArrayView<FVector3f> InitialPerSampleBase,
		FVertexMaskForgeRecipeEvaluationOutput& OutResult);
}
