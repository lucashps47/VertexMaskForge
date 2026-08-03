#pragma once

#include "Containers/Array.h"
#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "VertexMaskForgeLayerTypes.h"

/**
 * M16-K.3A: pure, stateless-except-for-its-own-array, Slate-free domain owning an ordered stack of
 * FVertexMaskForgeLayer entries. This is the dynamic-identity successor audited in M16-K.3R --
 * position is implied entirely by index inside the internal TArray; the only stored identity is each
 * layer's own LayerId (FGuid). Mirrors VertexMaskForgeLayerOrder's "pure math/no side effects, fails
 * without mutating on any invalid input" shape, generalized from a fixed 7-slot enum permutation to an
 * arbitrary-length, arbitrary-content dynamic list.
 *
 * Knows nothing about SVertexMaskForgePanel, Slate, Working Mesh, GeneratorState, caches,
 * InstanceResults, Recipe, WorkingColors, the evaluator, or the generator bridge -- see the M16-K.3A
 * report for the exact list of excluded concerns. GeneratorLayerOrder remains the sole production order
 * owner; nothing here is wired into any composition/production call site this checkpoint.
 *
 * Selection is deliberately NOT represented anywhere in this class. A UI/controller layer (M16-K.4) will
 * own a separate, transient "currently selected LayerId" outside this domain -- this class only exposes
 * the raw information a controller needs to implement its own selection policy after a removal (the
 * removed index and the remaining layer count/order, via RemoveLayer's FRemoveResult) without this class
 * ever deciding or storing that policy itself.
 */
class FVertexMaskForgeDynamicLayerStack
{
public:
	/** Result of RemoveLayer -- pure information for a future controller's own selection policy (e.g.
	 *  "select whatever now occupies RemovedIndex, else the previous index, else nothing"). This struct
	 *  never chooses or stores a selection; it only reports what happened. */
	struct FRemoveResult
	{
		/** True iff a layer matching the requested LayerId was found and removed. */
		bool bRemoved = false;

		/** The index the removed layer occupied immediately before removal, or INDEX_NONE if bRemoved is
		 *  false. Transient -- meaningful only immediately after this call, before any further mutation. */
		int32 RemovedIndex = INDEX_NONE;

		/** Layers.Num() immediately after the removal (or before, if bRemoved is false -- i.e. always the
		 *  stack's current count either way). */
		int32 RemainingNum = 0;
	};

	/**
	 * Returns a NEW stack containing exactly one layer -- Name "Base Layer", Fill=None, otherwise every
	 * FVertexMaskForgeLayer default (bEnabled=true, BlendMode=Copy, Opacity=1.0, all channels affected).
	 * Empty and neutral: adding this stack's single layer to a composition must not alter its result
	 * (M16-K.3B's concern to prove; this checkpoint only guarantees the data is neutral, not that any
	 * composition path reads it). Not wired into SVertexMaskForgePanel by this checkpoint.
	 */
	static FVertexMaskForgeDynamicLayerStack MakeInitialStack();

	/**
	 * Appends a new layer at the end of the stack with a freshly generated, unique FGuid, the given Name
	 * (no uniqueness or non-empty requirement -- see FVertexMaskForgeLayer::Name), and every other field
	 * at its FVertexMaskForgeLayer default (critically Fill=None -- never White, never a disabled/zero-
	 * opacity substitute). Existing layers and their relative order are entirely untouched. Returns the
	 * new layer's LayerId.
	 */
	FGuid AddLayer(const FString& Name);

	/**
	 * Removes the layer matching LayerId, if present. Preserves the relative order of every other layer.
	 * Removing the last remaining layer is explicitly supported and leaves the stack empty and IsValid()
	 * (an empty stack is a valid stack -- see IsValid). For an unknown LayerId, this is a complete no-op:
	 * the array is left byte-for-byte as it was and FRemoveResult::bRemoved is false. Never touches or
	 * knows about caches/generators/Working Mesh -- they do not exist at this layer of the domain.
	 * Never chooses or stores which layer (if any) should become selected afterward -- see FRemoveResult.
	 */
	FRemoveResult RemoveLayer(const FGuid& LayerId);

	/**
	 * Const-only lookup by identity; nullptr if LayerId is not present. The returned pointer is only
	 * valid until the next mutating call on this stack (Add/Remove/Move all may reallocate the internal
	 * array) -- never store it across such a call.
	 *
	 * M16-K.3A corrective audit: there is deliberately no mutable overload. FVertexMaskForgeLayer::LayerId
	 * is a public field (see that struct's own doc comment), so a mutable FVertexMaskForgeLayer* returned
	 * here would let a caller overwrite LayerId directly (corrupting/duplicating identity) or replace the
	 * whole element via `*Ptr = OtherLayer` (the implicitly-defined, public copy-assignment operator
	 * copies every member, private or not, so making LayerId private alone would NOT have closed this --
	 * only removing mutable element access does). Every field-level mutation this checkpoint supports
	 * (currently just Name, via RenameLayer) goes through a dedicated controlled operation instead; this
	 * class is the sole surface that can ever produce or relocate a LayerId.
	 */
	const FVertexMaskForgeLayer* FindLayerById(const FGuid& LayerId) const;

	/** Current index of the layer matching LayerId, or INDEX_NONE if absent. Transient -- callers must
	 *  never cache this across a mutating call; always re-resolve by LayerId. */
	int32 FindLayerIndexById(const FGuid& LayerId) const;

	/** Sets the Name of the layer matching LayerId. Returns false (no-op) for an unknown LayerId. Accepts
	 *  an empty NewName -- no non-empty or uniqueness policy is imposed at this layer (see
	 *  FVertexMaskForgeLayer::Name). Never affects LayerId, position, or any other field. */
	bool RenameLayer(const FGuid& LayerId, const FString& NewName);

	/**
	 * Moves the layer matching LayerId to array index NewIndex -- the FINAL desired index after the
	 * move (not an insertion-before-removal index). Preserves the relative order of every other layer
	 * (a plain remove-then-insert, not a chain of adjacent swaps). Returns false and leaves the stack
	 * completely unmodified if: LayerId is not present; NewIndex is out of the current [0, Num()-1]
	 * range (never clamped -- an out-of-range request is rejected outright, matching this domain's
	 * "never silently normalize" contract, same as VertexMaskForgeLayerOrder). If NewIndex already equals
	 * the layer's current index, this is a no-op that still returns true (there is nothing invalid about
	 * the request, simply nothing to move). Never regenerates LayerId or copies the layer as a new
	 * identity -- the same FVertexMaskForgeLayer value (including its LayerId) is relocated.
	 */
	bool MoveLayer(const FGuid& LayerId, int32 NewIndex);

	/** Moves the layer matching LayerId one position earlier (an adjacent swap with its immediate
	 *  predecessor, resolved by LayerId's own current array position -- never cached/assumed). Returns
	 *  true and performs exactly that one swap iff LayerId is present and not already at index 0.
	 *  Returns false and leaves the stack completely unmodified otherwise (unknown LayerId, or LayerId
	 *  already first). Never uses Source, an enum, or any index arithmetic beyond the one swap. */
	bool MoveLayerUp(const FGuid& LayerId);

	/** Sibling of MoveLayerUp -- moves one position later (swap with immediate successor). Same
	 *  success/failure contract, mirrored: fails (unmodified) if LayerId is absent or already last. */
	bool MoveLayerDown(const FGuid& LayerId);

	/**
	 * M16-K.3B: controlled, field-specific mutations -- the smallest set needed to make the composition
	 * fields (Fill/BlendMode/Opacity/bEnabled/channels) actually configurable for evaluator tests, without
	 * reopening the mutable-element seam the M16-K.3A corrective audit closed. Every one of these:
	 *   - operates by LayerId (never index);
	 *   - returns false and leaves the stack COMPLETELY unmodified for an unknown LayerId;
	 *   - never touches LayerId, Name, or position;
	 *   - preserves IsValid() -- an out-of-contract input is rejected outright, never clamped/normalized.
	 * There is deliberately no single "set the whole layer" mutator (that would reopen the identity-
	 * replacement seam via `*Ptr = OtherLayer`) and no setters yet for anything this checkpoint doesn't
	 * need (a future MaskInstance, selection, etc.) -- see this class's own module doc comment.
	 */

	/** Sets Fill. Rejects (returns false, unmodified) an unknown LayerId or a Fill value that is not one
	 *  of EVertexMaskForgeLayerFill's three real enumerators (None/Black/White) -- same validation
	 *  IsValid() itself performs, never a silent fallback to None. */
	bool SetLayerFill(const FGuid& LayerId, EVertexMaskForgeLayerFill Fill);

	/** Sets BlendMode. Rejects (returns false, unmodified) an unknown LayerId or a BlendMode value that is
	 *  not one of EVertexMaskForgeBlendMode's seven real enumerators -- same validation IsValid() itself
	 *  performs. */
	bool SetLayerBlendMode(const FGuid& LayerId, EVertexMaskForgeBlendMode BlendMode);

	/** Sets Opacity. Rejects (returns false, unmodified) an unknown LayerId, a non-finite value (NaN or
	 *  +/-Infinity), or a value outside [0, 1] -- never clamps a rejected value into range. */
	bool SetLayerOpacity(const FGuid& LayerId, float Opacity);

	/** Sets bEnabled. Only an unknown LayerId can fail (a bool has no invalid state of its own). */
	bool SetLayerEnabled(const FGuid& LayerId, bool bEnabled);

	/** Sets all three per-layer Channel Filter bits together. Accepts any combination, including all
	 *  false (a compositionally no-op layer, still a valid stack state -- see EvaluateColor's own
	 *  contract). Only an unknown LayerId can fail. */
	bool SetLayerChannelFilter(const FGuid& LayerId, bool bAffectRed, bool bAffectGreen, bool bAffectBlue);

	/**
	 * M16-K.5B: assigns or replaces the layer's procedural mask generator instance -- the ONLY way this
	 * stack ever creates or replaces a FVertexMaskForgeGeneratorMaskInstance (mirrors every other
	 * SetLayer* mutator's "sole controlled entry point" contract).
	 *   - LayerId unknown: returns false, stack completely unmodified.
	 *   - Layer has no mask yet: creates one -- fresh FGuid::NewGuid() MaskInstanceId, GeneratorType, and
	 *     MakeVertexMaskForgeGeneratorParams(GeneratorType) defaults. Returns true.
	 *   - Layer already has a mask with GeneratorType == the requested type: a NO-OP -- MaskInstanceId and
	 *     Params are left completely untouched (not just value-equal, but not even re-assigned), the
	 *     instance is not recreated. Returns true (the request's postcondition -- "this layer has a mask
	 *     of this type" -- already held).
	 *   - Layer already has a mask with a DIFFERENT GeneratorType: replaced outright -- a fresh
	 *     MaskInstanceId, the new GeneratorType, and that type's own default Params, discarding the old
	 *     instance's Params entirely (never a partial/merged carry-over). Returns true.
	 */
	bool SetLayerMaskGeneratorType(const FGuid& LayerId, EVertexMaskForgeGeneratorType GeneratorType);

	/**
	 * Clears the layer's mask, if any -- the layer itself, its LayerId, and every other field are
	 * completely unaffected. Idempotent: calling this on a layer that already has no mask is a harmless
	 * no-op that still returns true (the postcondition -- "this layer has no mask" -- already held).
	 * Returns false only for an unknown LayerId, leaving the stack completely unmodified.
	 */
	bool ClearLayerMask(const FGuid& LayerId);

	/**
	 * M16-K.5C-B: replaces ONLY the Params payload of the layer's existing generator mask instance --
	 * never its GeneratorType, MaskInstanceId, or any other field of the layer or instance. This is the
	 * sole controlled entry point for editing an already-assigned mask's parameters (mirrors every other
	 * SetLayer.../SetLayerMask... mutator's "sole controlled entry point" contract).
	 *
	 * ExpectedMaskInstanceId guards against a stale caller (e.g. a future parameter UI that captured a
	 * MaskInstanceId before SetLayerMaskGeneratorType replaced it with a different instance) silently
	 * editing the WRONG instance -- LayerId alone cannot distinguish "still the instance I opened" from
	 * "this layer now holds a different instance that happens to share the same LayerId".
	 *
	 * Fails (returns false, stack completely unmodified) if:
	 *   - ExpectedMaskInstanceId is not a valid FGuid (checked before any lookup);
	 *   - LayerId does not resolve to a layer;
	 *   - the layer currently has no mask;
	 *   - the mask's current MaskInstanceId does not equal ExpectedMaskInstanceId;
	 *   - the mask's currently stored GeneratorType/Params pairing is not itself coherent (this
	 *     checkpoint never repairs an already-inconsistent instance -- such an instance must be replaced
	 *     or cleared via SetLayerMaskGeneratorType/ClearLayerMask instead);
	 *   - the new Params' active alternative does not match the mask's GeneratorType.
	 * On success, only Mask->Params is assigned (via MoveTemp) -- MaskInstanceId, GeneratorType, LayerId,
	 * position, and every other field are left completely untouched.
	 */
	bool SetLayerMaskParams(const FGuid& LayerId, const FGuid& ExpectedMaskInstanceId, FVertexMaskForgeGeneratorParams Params);

	/**
	 * Const-only lookup of the layer's mask instance. Returns nullptr for BOTH an unknown LayerId and a
	 * known layer with no mask set -- callers that must distinguish "layer doesn't exist" from "layer
	 * exists but has no mask" should resolve the layer itself first (e.g. via FindLayerById) rather than
	 * rely on this function to disambiguate; no such need exists in this checkpoint. Same lifetime caveat
	 * as FindLayerById -- the returned pointer is only valid until the next mutating call on this stack.
	 */
	const FVertexMaskForgeGeneratorMaskInstance* GetLayerMask(const FGuid& LayerId) const;

	/** Read-only view of the current layers, in composition order (index 0 first). */
	const TArray<FVertexMaskForgeLayer>& GetLayers() const { return Layers; }

	int32 Num() const { return Layers.Num(); }
	bool IsEmpty() const { return Layers.IsEmpty(); }

	/**
	 * M16-K.6D-7B: true iff at least one layer has bEnabled == true. Deliberately distinct from
	 * !IsEmpty() -- a non-empty stack with every layer disabled must be reported as having no enabled
	 * layer. Used by the panel's Dynamic Accept eligibility check (see SVertexMaskForgePanel::
	 * RecomputeOperationState) as a cheap, shallow UI-eligibility signal; it says nothing about whether
	 * those enabled layer(s) will actually compose successfully (unsupported generator, stale topology,
	 * etc.) -- that authoritative validation belongs solely to
	 * VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets, invoked only when
	 * Accept is actually pressed.
	 */
	bool HasAnyEnabledLayer() const
	{
		for (const FVertexMaskForgeLayer& Layer : Layers)
		{
			if (Layer.bEnabled)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * True iff, for every layer:
	 *   - LayerId is valid (non-default-constructed FGuid);
	 *   - LayerId is unique within the stack (no duplicates);
	 *   - Opacity is finite (rejects NaN and +/-Infinity) AND within [0, 1];
	 *   - Fill is one of EVertexMaskForgeLayerFill's three real enumerators (None/Black/White);
	 *   - BlendMode is one of EVertexMaskForgeBlendMode's seven real enumerators (Copy/Add/Subtract/
	 *     Multiply/Overlay/Screen/Linear) -- validated by explicit switch, never a numeric range check,
	 *     since that enum carries no Count/Max sentinel and no documented contiguity contract.
	 *   - M16-K.5C-C: IF the layer has a Mask (TOptional set -- an unset Mask, including on the Base
	 *     Layer, imposes no further requirement), then MaskInstanceId is valid (non-default FGuid) AND
	 *     GeneratorType/Params are coherent (GeneratorType is one of the seven real generators AND Params'
	 *     active alternative matches it) -- the same structural check SetLayerMaskParams itself performs
	 *     via DoGeneratorTypeAndParamsMatch. MaskInstanceId uniqueness ACROSS layers is deliberately NOT
	 *     required -- no such contract exists anywhere in this model, unlike LayerId's own uniqueness
	 *     above. Semantic validity of a Params struct's own field values (ranges, NaN, artistic bounds) is
	 *     explicitly out of scope -- this method only checks the Type/Params PAIRING, never field content.
	 * An empty stack is valid (there is nothing to violate). Pure observation -- never mutates,
	 * normalizes, clamps, or repairs anything; an out-of-contract value is reported, never corrected.
	 *
	 * Because Layers is only ever mutated through this class's own API (AddLayer always assigns a fresh
	 * FGuid::NewGuid() and every FVertexMaskForgeLayer default already satisfies every clause above;
	 * Remove/Move/Rename never touch LayerId, Opacity, Fill, or BlendMode), every stack reachable through
	 * the public API is IsValid() by construction -- this method exists as an explicit, testable proof of
	 * that invariant, not because the public API is expected to ever produce an invalid stack in practice.
	 */
	bool IsValid() const;

private:
	/** Internal-only mutable lookup, used exclusively by this class's own controlled mutation operations
	 *  (currently RenameLayer) -- never exposed publicly. See FindLayerById's own doc comment for why no
	 *  public mutable accessor exists. */
	FVertexMaskForgeLayer* FindLayerByIdMutable(const FGuid& LayerId);

	TArray<FVertexMaskForgeLayer> Layers;
};
