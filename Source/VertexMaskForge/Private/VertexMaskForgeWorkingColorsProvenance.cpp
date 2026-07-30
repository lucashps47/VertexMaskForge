#include "VertexMaskForgeWorkingColorsProvenance.h"

#include "VertexMaskForgeWorkingMeshOwner.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace VertexMaskForgeWorkingColorsProvenance
{
	EVertexMaskForgeWorkingColorsPublicationValidationStatus ValidateWorkingColorsPublicationBinding(
		const FVertexMaskForgeWorkingColorPublicationBinding& Binding, const FVertexMaskForgePreviewComponentState& Target)
	{
		using EStatus = EVertexMaskForgeWorkingColorsPublicationValidationStatus;

		if (!Binding.IsWellFormed())
		{
			return EStatus::InvalidOwner;
		}

		// AUDITED (M16-J.0B.1 corrective pass): Pin() fresh, right here, right now -- IsWellFormed() above
		// only proved the owner was alive an instant ago; if it has since been destroyed (last TSharedPtr
		// dropped between IsWellFormed() and this line, or the Binding was stored and validated long after
		// CreateBinding() originally ran), Pin() simply fails and this becomes a clean InvalidWorkingMesh
		// rejection -- never a dereference of freed memory. Target itself is supplied live by the caller
		// (see this function's own doc comment) -- this module never stores or dereferences a stale
		// reference to it.
		const TSharedPtr<const FVertexMaskForgeWorkingMeshOwner> PinnedMeshOwner = Binding.MeshOwnerWeak.Pin();
		if (!PinnedMeshOwner.IsValid())
		{
			return EStatus::InvalidWorkingMesh;
		}

		const FVertexMaskForgeWorkingMesh& WorkingMesh = PinnedMeshOwner->GetWorkingMesh();
		const FVertexMaskForgeWorkingMeshProvenance& Provenance = WorkingMesh.Provenance;
		const FVertexMaskForgeWorkingColorsAuthority& Authority = Target.GetWorkingColorsAuthority();

		if (!Provenance.IsValid() || !Provenance.GetSourceStaticMesh().IsValid())
		{
			return EStatus::InvalidWorkingMesh;
		}

		if (!Target.GetSourceComponent().IsValid())
		{
			return EStatus::InvalidTarget;
		}

		if (!Authority.IsValid())
		{
			return EStatus::InvalidAlphaSource;
		}

		// AUDITED (M16-J.0A.1, step 5): mesh-owner ATTACHMENT identity -- proves Authority was
		// established against the SAME FVertexMaskForgeWorkingMeshOwner this WorkingMesh currently
		// belongs to, not merely one with coincidentally-matching content.
		if (Provenance.GetMeshOwnerId() != Authority.GetMeshOwnerId())
		{
			return EStatus::AlphaSourceOwnerMismatch;
		}

		if (Provenance.GetSourceStaticMesh().Get() != Authority.GetSourceStaticMesh().Get())
		{
			return EStatus::MeshMismatch;
		}

		if (Provenance.GetLODIndex() != Authority.GetLODIndex())
		{
			return EStatus::LODMismatch;
		}

		if (Provenance.GetUseSourceTopology() != Authority.GetUseSourceTopology())
		{
			return EStatus::DomainMismatch;
		}

		if (Provenance.GetRevision() != Authority.GetRevision())
		{
			return EStatus::StaleWorkingMesh;
		}

		const TArray<FColor>& SelectedBuffer = Provenance.GetUseSourceTopology()
			? Target.GetSourceTopologyWorkingColors()
			: Target.GetWorkingColors();

		if (Provenance.GetExpectedCardinality() != SelectedBuffer.Num())
		{
			return EStatus::CardinalityMismatch;
		}

		if (Authority.GetCardinality() != SelectedBuffer.Num())
		{
			return EStatus::AlphaSourceCardinalityMismatch;
		}

		return EStatus::Success;
	}
}
