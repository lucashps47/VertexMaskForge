#pragma once

#include "Containers/BitArray.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "CoreMinimal.h"
#include "Misc/Guid.h"

/**
 * M16-C: keyed instance-result storage for the future Fill Layer / Mask Stack architecture -- see the
 * M15/M15.1/M15.2/M15.2.1 architectural audits and VertexMaskForgeRecipeTypes.h's own module comment
 * for the full derivation.
 *
 * This is a THIRD, distinct domain from the other two already established:
 *   1. Recipe authoring state (VertexMaskForgeRecipeTypes.h) -- identity, generator selection,
 *      parameters, composition weights. Never generated data.
 *   2. Instance result storage (THIS file) -- one generator's already-generated raw scalar result for
 *      one Mask Instance, in one specific entry/component. Keyed by FGuid, substitutable, removable,
 *      never authoritative recipe state, never a shared geometry cache.
 *   3. Shared geometry caches (existing, per-generator: FVertexMaskForgeAOCache,
 *      FVertexMaskForgeThicknessCache, etc., already living on FVertexMaskForgeWorkingMesh/
 *      FVertexMaskForgePreviewComponentState) -- untouched, unmoved, unreorganized by this checkpoint.
 *
 * No generator writes to this storage yet, and nothing reads from it yet -- the legacy fixed panel
 * flow continues to read/write only its own named fields (GeneratorState.CurvatureMask -- see
 * M16-J.0B.1's WorkingMesh Domain Split, which moved this and its sibling artistic-state fields off
 * FVertexMaskForgeWorkingMesh -- PreviewComponentState.AOCache, etc.), completely unaware this storage
 * exists. Coexistence is strictly additive.
 */

/**
 * One Mask Instance's already-generated raw scalar result, for one specific entry/component. Contains
 * ONLY the derived sample data -- no identity (the map key already provides InstanceId), no Blend
 * Mode/Opacity/Enabled, no generator parameters, no EffectiveMask/Coverage/FillValue, no
 * WorkingColors/CommittedColors/BaselineColors, no component/mesh pointer, no cache ownership, no
 * parameter fingerprint. Domain-agnostic, exactly like the existing
 * FVertexMaskForgeScalarMask::Values/bHasValue pair -- which generator-and-topology-mode combination
 * populated this (render vertex, Dynamic Mesh Vertex ID, corner/CornerIndex, Normal Overlay Element
 * ID, ...) is established by whatever future code populates the slot, not represented here.
 *
 * Deliberately NOT a reuse of FVertexMaskForgeScalarMask: that type additionally carries State,
 * Source (a generator-type/Fill-Layer-slot identity concept), and per-generator diagnostic snapshots
 * (UsedAxisParams, UsedAOParams, RenderVertexCount, bUnifiedBounds, SelectionMeshCount) -- all legacy
 * composition/diagnostic baggage this checkpoint's own constraints explicitly exclude (no generator
 * parameters, no identity duplicated inside the value). This type is the minimal, domain-appropriate
 * alternative.
 */
struct FVertexMaskForgeInstanceMaskResult
{
	TArray<float> Values;

	/** Parallel to Values: true only at indices that were actually written -- same sparse-safe
	 *  contract as FVertexMaskForgeScalarMask::bHasValue. */
	TBitArray<> bHasValue;

	/** Safe accessor: returns false (and leaves OutValue untouched) for any index not actually
	 *  stored -- same contract as FVertexMaskForgeScalarMask::TryGetValue. */
	bool TryGetValue(const int32 Index, float& OutValue) const
	{
		if (!bHasValue.IsValidIndex(Index) || !bHasValue[Index])
		{
			return false;
		}
		OutValue = Values[Index];
		return true;
	}
};

/**
 * Keyed instance-result storage for ONE owner (one FVertexMaskForgeWorkingMesh entry, or one
 * FVertexMaskForgePreviewComponentState component -- see those types' own new InstanceResults field
 * for where this actually lives). At most one current result slot per FGuid within this store; storing
 * again for an already-present InstanceId replaces that slot in place -- it never accumulates a second
 * version, and Num() never grows from repeated stores to the same key. There is no cross-owner
 * sharing: two different FVertexMaskForgeInstanceResultStore instances (e.g. two different components)
 * are completely independent, even if they happen to hold a result for the identical InstanceId.
 */
class FVertexMaskForgeInstanceResultStore
{
public:
	/**
	 * Stores Result for InstanceId, replacing any existing slot for that exact InstanceId in place --
	 * Num() is unchanged if InstanceId was already present, +1 otherwise. An invalid InstanceId
	 * (FGuid::IsValid() == false) is rejected: no slot is created, Num() is unchanged, and the call
	 * returns false. The rejection is reported via a quiet Warning-level UE_LOG -- deliberately not
	 * check()/ensure(), both of which trigger a full stack walk and are not reliably suppressible in an
	 * automation test; this is a routine, expected rejection, not a programming-error assertion.
	 */
	bool StoreOrReplace(const FGuid& InstanceId, FVertexMaskForgeInstanceMaskResult&& Result);

	/** Returns a pointer to InstanceId's current result, or nullptr if absent. The pointer is only
	 *  valid until the next mutating call on this store. */
	const FVertexMaskForgeInstanceMaskResult* Find(const FGuid& InstanceId) const;

	bool Contains(const FGuid& InstanceId) const;

	int32 Num() const;

	/** Removes InstanceId's result slot, if present. Returns true iff a slot was actually removed.
	 *  Removing an absent (including invalid) InstanceId is a well-defined no-op that returns false --
	 *  never an error, never touches any other slot. */
	bool Remove(const FGuid& InstanceId);

	/**
	 * Removes every result slot whose InstanceId is NOT present in LiveInstanceIds -- the authoritative
	 * set of Mask Instance identities a future caller determines are still live in the recipe. Slots
	 * for IDs present in LiveInstanceIds are preserved untouched; no new slot is ever created by this
	 * call. Returns the number of slots actually removed. Calling with an empty LiveInstanceIds set
	 * removes every slot (equivalent to Reset()).
	 */
	int32 PruneToInstanceIds(const TSet<FGuid>& LiveInstanceIds);

	/** Removes every result slot this store holds. Touches nothing else -- no geometry, no named
	 *  legacy masks, no shared caches, no recipe, no Working/Committed/Baseline Colors. */
	void Reset();

private:
	TMap<FGuid, FVertexMaskForgeInstanceMaskResult> Results;
};
