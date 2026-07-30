#include "VertexMaskForgeInstanceResultStore.h"

bool FVertexMaskForgeInstanceResultStore::StoreOrReplace(const FGuid& InstanceId, FVertexMaskForgeInstanceMaskResult&& Result)
{
	// AUDITED (M16-C): a plain, quiet Warning-level log -- deliberately NOT ensure()/ensureMsgf(). An
	// ensure triggers a full stack walk and a multi-line "Handled ensure" report (and attempts crash-
	// reporter submission), which is both slow and not reliably suppressible by a single automation-
	// test AddExpectedError() call (each callstack line logs at Error severity independently). This
	// path is a routine, expected rejection, not a programming-error assertion -- a quiet log plus a
	// deterministic `false` return is the correct, test-friendly, non-crashing policy.
	if (!InstanceId.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FVertexMaskForgeInstanceResultStore::StoreOrReplace: refusing to store a result for an invalid FGuid."));
		return false;
	}

	// TMap::Add replaces the value in place for an already-present key -- exactly one entry per
	// InstanceId, never a second version, Num() unchanged on replace.
	Results.Add(InstanceId, MoveTemp(Result));
	return true;
}

const FVertexMaskForgeInstanceMaskResult* FVertexMaskForgeInstanceResultStore::Find(const FGuid& InstanceId) const
{
	return Results.Find(InstanceId);
}

bool FVertexMaskForgeInstanceResultStore::Contains(const FGuid& InstanceId) const
{
	return Results.Contains(InstanceId);
}

int32 FVertexMaskForgeInstanceResultStore::Num() const
{
	return Results.Num();
}

bool FVertexMaskForgeInstanceResultStore::Remove(const FGuid& InstanceId)
{
	return Results.Remove(InstanceId) > 0;
}

int32 FVertexMaskForgeInstanceResultStore::PruneToInstanceIds(const TSet<FGuid>& LiveInstanceIds)
{
	TArray<FGuid> ToRemove;
	ToRemove.Reserve(Results.Num());
	for (const TPair<FGuid, FVertexMaskForgeInstanceMaskResult>& Pair : Results)
	{
		if (!LiveInstanceIds.Contains(Pair.Key))
		{
			ToRemove.Add(Pair.Key);
		}
	}
	for (const FGuid& Id : ToRemove)
	{
		Results.Remove(Id);
	}
	return ToRemove.Num();
}

void FVertexMaskForgeInstanceResultStore::Reset()
{
	Results.Reset();
}
