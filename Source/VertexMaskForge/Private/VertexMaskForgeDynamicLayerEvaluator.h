#pragma once

#include "CoreMinimal.h"
#include "Math/Vector4.h"
#include "VertexMaskForgeDynamicLayerStack.h"
#include "VertexMaskForgeLayerTypes.h"

class FVertexMaskForgeInstanceResultStore;

/**
 * M16-K.3B: pure, stateless composition core for FVertexMaskForgeDynamicLayerStack -- deliberately a
 * SEPARATE component from the stack itself (the stack is domain/identity; this is evaluation -- see this
 * checkpoint's own architectural principle). Knows nothing about Slate, Working Mesh, Static Mesh,
 * selected components, caches, generators, or side effects of any kind; it never mutates the stack it is
 * given, and every input/output is explicit (never BaselineColors/CommittedColors/WorkingColors, never
 * panel state -- see EvaluateColor's own doc comment on why the base color is always a caller-supplied
 * parameter).
 *
 * REUSE, NOT DUPLICATION: this module introduces no new blend-mode math. Per-layer blending is delegated
 * verbatim to VertexMaskForgeSequentialEvaluator::EvaluateFillLayerStep (itself a thin wrapper over
 * VertexMaskForgeMaskStackComposer::BlendMaskValueUnclamped, the single real implementation of all seven
 * EVertexMaskForgeBlendMode formulas anywhere in this plugin). EvaluateFillLayerStep is reused rather than
 * the higher-level EvaluateFillLayers (the existing multi-layer fold) because EvaluateFillLayers has no
 * concept of a PER-LAYER Channel Filter -- it folds all three channels uniformly across the whole array,
 * whereas this checkpoint's Channel Filter (bAffectRed/Green/Blue on FVertexMaskForgeLayer) must be
 * applied between every individual layer step, not once at the end (a disabled channel must resume
 * accumulating from whatever the PREVIOUS layer left it at, not from the base color and not from a
 * post-hoc override). Calling the single-layer-step primitive once per (layer, enabled) and merging
 * per-channel afterward reproduces this exactly, with zero new blend arithmetic.
 *
 * No MaskInstance, procedural mask, or generator exists yet -- every layer's EffectiveMask this checkpoint
 * is implicitly 1.0 (see EvaluateColor's own doc comment); associating a procedural mask with a layer is
 * explicitly deferred to a future checkpoint.
 *
 * Not wired into SVertexMaskForgePanel, GeneratorLayerOrder, the generator bridge, or either production
 * composition call site (ComputeComposedColorsRGB/ComputeComposedColorsRGBSourceTopology) -- those remain
 * completely unmodified and continue to be the sole active production path.
 */
namespace VertexMaskForgeDynamicLayerEvaluator
{
	/**
	 * Resolves a layer's Fill into its scalar composition content.
	 *   - None: returns false. None means "no content of its own" -- NOT the same as Black (which IS
	 *     content, just the value 0.0f). A caller must treat a false return as "this layer contributes
	 *     nothing", never substitute a default value.
	 *   - Black: returns true, OutValue = 0.0f.
	 *   - White: returns true, OutValue = 1.0f.
	 *   - Any other value (only reachable by bypassing FVertexMaskForgeDynamicLayerStack's own
	 *     SetLayerFill validation, e.g. a directly-constructed FVertexMaskForgeLayer with a cast-invalid
	 *     Fill): returns false, OutValue left unmodified. This is NOT a silent "invalid treated as None"
	 *     fallback in spirit -- it is a distinct, defensive rejection that happens to share None's return
	 *     value because both mean "no composable content here"; see EvaluateColor's own doc comment for
	 *     why this is safe (an invalid Fill degrades to a no-op layer, never a crash or corruption).
	 */
	bool TryResolveFillValue(EVertexMaskForgeLayerFill Fill, float& OutValue);

	/**
	 * Evaluates one BaseColor (RGBA) against Stack's layers, strictly in Stack.GetLayers() array order
	 * (index 0 first) -- the TArray itself is the sole ordering authority, exactly mirroring
	 * FVertexMaskForgeDynamicLayerStack's own "position is only ever the index" contract. Per layer:
	 *   1. bEnabled == false -> complete no-op (skip).
	 *   2. TryResolveFillValue fails (Fill == None, or defensively, an invalid enumerator) -> complete
	 *      no-op (skip) -- regardless of BlendMode/Opacity/Channel Filter.
	 *   3. Otherwise: PaintValue = the resolved scalar, broadcast to all 3 channels (EffectiveMask is
	 *      implicitly 1.0 this checkpoint -- no procedural mask exists yet, so PaintValue IS the Fill
	 *      value directly, never Fill multiplied by anything). LayerOutput =
	 *      VertexMaskForgeSequentialEvaluator::EvaluateFillLayerStep(CompositeBelow, PaintValue,
	 *      Layer.BlendMode, Layer.Opacity) -- UNCLAMPED, exactly the shared primitive, Opacity passed
	 *      through untouched (never pre-multiplied into PaintValue -- that would apply it twice).
	 *   4. Per-channel Channel Filter: a channel with bAffect*==true adopts LayerOutput's value for that
	 *      channel; bAffect*==false preserves whatever the composite already was BEFORE this layer's step
	 *      (i.e. this layer contributes nothing to that one channel, but earlier layers' contributions to
	 *      it are preserved, and later layers can still affect it).
	 * Alpha is carried from BaseColor.W to the result completely untouched -- no layer, Channel Filter, or
	 * BlendMode ever reads or writes it (there is no bAffectAlpha and none will be added here).
	 * Clamp01 is applied EXACTLY ONCE, component-wise (R/G/B only), to the final composite after every
	 * layer has been folded -- never between layers, matching VertexMaskForgeSequentialEvaluator's own
	 * final-clamp-only policy exactly (see VertexMaskForgeDynamicLayerEvaluatorTests.cpp's
	 * FinalClampOnly test for a worked out-of-[0,1]-intermediate proof).
	 *
	 * An empty stack, or MakeInitialStack()'s single Fill=None Base Layer, both return BaseColor
	 * unmodified (RGB unclamped-but-already-valid passthrough, Alpha verbatim) -- no layer having
	 * contributed is NOT the same as a White fill; there is no automatic fallback to 1.0 anywhere in this
	 * function (that legacy "empty mask stack = full coverage" convention belongs to a FUTURE procedural
	 * mask's own EffectiveMask resolution, once one exists -- it does not apply to Fill/layer-presence
	 * here).
	 *
	 * BaseColor is an explicit caller-supplied parameter -- this function never reads
	 * BaselineColors/CommittedColors/WorkingColors or any panel/session state; deciding which color set to
	 * pass is entirely a future controller's responsibility.
	 *
	 * Contract for a Stack that is not IsValid(): this function performs no precondition check, ensure, or
	 * assert, and never normalizes/repairs Stack. It is deterministic and crash-safe regardless (an
	 * out-of-[0,1] Opacity is simply passed through to Lerp; an unrecognized Fill/BlendMode value is
	 * handled by TryResolveFillValue's/the underlying switch's own defensive default case, degrading to a
	 * no-op rather than undefined behavior) -- but its result is only a CONTRACTUALLY MEANINGFUL
	 * composition when Stack.IsValid() is true. Callers are responsible for only feeding this a valid
	 * stack if they need the documented semantics above.
	 */
	FVector4f EvaluateColor(const FVector4f& BaseColor, const FVertexMaskForgeDynamicLayerStack& Stack);

	/**
	 * M16-K.5D: sibling overload that additionally resolves each layer's EffectiveMask from a real
	 * FVertexMaskForgeInstanceResultStore, by the layer's own Mask->MaskInstanceId, at VertexIndex --
	 * still pure and read-only (never executes a generator, never writes to ResultStore, never
	 * fabricates/repairs/normalizes a result). This is the ONLY overload that ever resolves
	 * MaskInstanceId; the two-argument overload above continues to ignore Mask entirely (unchanged),
	 * exactly preserving its own pre-K.5D observable behavior and semantics.
	 *
	 * Per layer (bEnabled/Fill resolution identical to the two-argument overload -- see EvaluateColor's
	 * own doc comment above for those):
	 *   - Layer.Mask unset: PaintValue = FillValue (broadcast) -- ResultStore is NEVER consulted for such
	 *     a layer, regardless of what it may or may not contain.
	 *   - Layer.Mask set: PaintValue = FillValue * EffectiveMask, where EffectiveMask is:
	 *       - the real stored scalar, IF ResultStore.Find(Layer.Mask->MaskInstanceId) succeeds AND the
	 *         found result's TryGetValue(VertexIndex, ...) succeeds;
	 *       - 0.0f otherwise -- covers BOTH "MaskInstanceId not present in ResultStore" and "present but
	 *         no value stored at VertexIndex" identically (a Mask configured but without a resolved
	 *         result represents explicit filtering with no coverage yet, never full coverage -- see this
	 *         checkpoint's own architectural decision; "empty stack = full coverage" remains scoped to
	 *         Fill/layer-presence only, never extended to an existing-but-unresolved Mask Instance).
	 * The Base Layer receives no special-casing anywhere in this resolution -- it is governed by exactly
	 * the same two rules above as any other layer.
	 * BlendMode, Opacity, Channel Filter, fold order, the single final Clamp01, and Alpha passthrough are
	 * all identical to the two-argument overload -- this overload only changes how PaintValue is derived,
	 * nothing else in the fold.
	 */
	FVector4f EvaluateColor(const FVector4f& BaseColor, const FVertexMaskForgeDynamicLayerStack& Stack, const FVertexMaskForgeInstanceResultStore& ResultStore, int32 VertexIndex);
}
