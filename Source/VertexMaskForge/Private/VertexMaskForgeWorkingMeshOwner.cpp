#include "VertexMaskForgeWorkingMeshOwner.h"

#include "Engine/StaticMesh.h"
#include "Math/NumericLimits.h"

bool FVertexMaskForgeWorkingMeshOwner::ConfigureIdentity(UStaticMesh* StaticMesh, const int32 LODIndex, const bool bUseSourceTopology)
{
	if (bPermanentlyInvalid || !IsValid(StaticMesh))
	{
		return false;
	}

	const bool bEffectivelySameIdentity = bIdentityConfigured
		&& ConfiguredStaticMesh.Get() == StaticMesh
		&& ConfiguredLODIndex == LODIndex
		&& bConfiguredUseSourceTopology == bUseSourceTopology;

	if (bEffectivelySameIdentity)
	{
		return true;
	}

	ConfiguredStaticMesh = StaticMesh;
	ConfiguredLODIndex = LODIndex;
	bConfiguredUseSourceTopology = bUseSourceTopology;
	bIdentityConfigured = true;

	WorkingMesh = FVertexMaskForgeWorkingMesh();

	return true;
}

bool FVertexMaskForgeWorkingMeshOwner::InstallWorkingMesh(FVertexMaskForgeWorkingMesh&& NewWorkingMesh, const int32 ExpectedCardinality)
{
	if (bPermanentlyInvalid || !bIdentityConfigured)
	{
		return false;
	}

	if (Generation == TNumericLimits<uint64>::Max())
	{
		ensureMsgf(false, TEXT("VertexMaskForgeWorkingMeshOwner: Generation overflow -- permanently invalidating this owner rather than wrapping to 0."));
		bPermanentlyInvalid = true;
		return false;
	}

	++Generation;

	WorkingMesh = MoveTemp(NewWorkingMesh);

	// AUDITED: any Provenance the caller's WorkingMesh may already have carried is unconditionally
	// replaced -- never trusted, never merged. InstanceResults (if any) travels with the moved WorkingMesh
	// untouched -- this function never reads or writes that field.
	FVertexMaskForgeWorkingMeshProvenance NewProvenance;
	NewProvenance.SourceStaticMesh = ConfiguredStaticMesh;
	NewProvenance.bUseSourceTopology = bConfiguredUseSourceTopology;
	NewProvenance.LODIndex = ConfiguredLODIndex;
	NewProvenance.Revision = Generation;
	NewProvenance.ExpectedCardinality = ExpectedCardinality;
	NewProvenance.MeshOwnerId = this;
	NewProvenance.bIsValid = true;
	WorkingMesh.Provenance = NewProvenance;

	return true;
}

bool FVertexMaskForgeWorkingMeshOwner::Reset()
{
	if (bPermanentlyInvalid)
	{
		return false;
	}

	if (Generation == TNumericLimits<uint64>::Max())
	{
		ensureMsgf(false, TEXT("VertexMaskForgeWorkingMeshOwner: Generation overflow on Reset -- permanently invalidating this owner rather than wrapping to 0."));
		bPermanentlyInvalid = true;
		return false;
	}

	++Generation;
	WorkingMesh = FVertexMaskForgeWorkingMesh();
	return true;
}
