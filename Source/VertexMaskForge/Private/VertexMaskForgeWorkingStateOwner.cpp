#include "VertexMaskForgeWorkingStateOwner.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"

void FVertexMaskForgeWorkingStateOwner::InvalidateColors()
{
	PreviewState.BaselineColors.Reset();
	PreviewState.CommittedColors.Reset();
	PreviewState.WorkingColors.Reset();
	PreviewState.SourceTopologyBaselineColors.Reset();
	PreviewState.SourceTopologyCommittedColors.Reset();
	PreviewState.SourceTopologyWorkingColors.Reset();
	PreviewState.WorkingColorsAuthority = FVertexMaskForgeWorkingColorsAuthority();
	bColorsInitialized = false;
}

bool FVertexMaskForgeWorkingStateOwner::ConfigureTarget(UStaticMeshComponent* Component)
{
	if (bPermanentlyInvalid || !IsValid(Component))
	{
		return false;
	}

	const bool bEffectivelySameTarget = bTargetConfigured && ConfiguredComponent.Get() == Component;
	if (bEffectivelySameTarget)
	{
		return true;
	}

	ConfiguredComponent = Component;
	bTargetConfigured = true;
	PreviewState.SourceComponent = Component;

	InvalidateColors();

	return true;
}

bool FVertexMaskForgeWorkingStateOwner::AttachToMeshOwner(const TSharedPtr<FVertexMaskForgeWorkingMeshOwner>& InMeshOwner)
{
	if (bPermanentlyInvalid || !InMeshOwner.IsValid())
	{
		return false;
	}

	const bool bEffectivelySameOwner = AttachedMeshOwner.Pin().Get() == InMeshOwner.Get();
	if (bEffectivelySameOwner)
	{
		return true;
	}

	AttachedMeshOwner = InMeshOwner;
	InvalidateColors();

	return true;
}

void FVertexMaskForgeWorkingStateOwner::DetachFromMeshOwner()
{
	if (!AttachedMeshOwner.IsValid())
	{
		return;
	}

	AttachedMeshOwner.Reset();
	InvalidateColors();
}

bool FVertexMaskForgeWorkingStateOwner::InitializeColors(TArray<FColor> CapturedColors)
{
	if (bPermanentlyInvalid || !bTargetConfigured)
	{
		return false;
	}

	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> Pinned = AttachedMeshOwner.Pin();
	if (!Pinned.IsValid())
	{
		return false;
	}

	const FVertexMaskForgeWorkingMeshProvenance& MeshProvenance = Pinned->GetWorkingMesh().Provenance;
	if (!MeshProvenance.IsValid())
	{
		return false;
	}

	if (CapturedColors.Num() != MeshProvenance.GetExpectedCardinality())
	{
		return false;
	}

	bUseSourceTopologyMirror = MeshProvenance.GetUseSourceTopology();

	MutableBaselineColors() = CapturedColors;
	MutableCommittedColors() = CapturedColors;
	MutableWorkingColors() = MoveTemp(CapturedColors);

	FVertexMaskForgeWorkingColorsAuthority NewAuthority;
	NewAuthority.SourceStaticMesh = MeshProvenance.GetSourceStaticMesh();
	NewAuthority.bUseSourceTopology = MeshProvenance.GetUseSourceTopology();
	NewAuthority.LODIndex = MeshProvenance.GetLODIndex();
	// AUDITED: copied verbatim from the attached mesh owner's own Provenance -- never independently
	// advanced -- see this class's own module comment on the single generation-advance rule.
	NewAuthority.Revision = MeshProvenance.GetRevision();
	NewAuthority.Cardinality = MutableWorkingColors().Num();
	NewAuthority.MeshOwnerId = MeshProvenance.GetMeshOwnerId();
	NewAuthority.StateOwnerId = this;
	NewAuthority.bIsValid = true;
	PreviewState.WorkingColorsAuthority = NewAuthority;

	bColorsInitialized = true;
	return true;
}

bool FVertexMaskForgeWorkingStateOwner::EnsureBaselineCaptured(TArray<FColor> CapturedColors)
{
	if (bColorsInitialized)
	{
		// AUDITED: idempotent -- CapturedColors is simply discarded. This is the behavioral equivalent
		// of the real algorithm's own "BaselineColors.Num() != NumRenderVerts" gate evaluating false.
		return true;
	}

	return InitializeColors(MoveTemp(CapturedColors));
}

bool FVertexMaskForgeWorkingStateOwner::ApplyComposedColorsRGB(TArray<FColor> FinalRGB, const EColorCommitMode CommitMode)
{
	if (bPermanentlyInvalid || !bColorsInitialized)
	{
		return false;
	}

	const TArray<FColor>& Baseline = MutableBaselineColors();
	if (FinalRGB.Num() != Baseline.Num())
	{
		// AUDITED: cardinality is validated HERE, by the owner, before anything is written -- no
		// partial application. FinalRGB (the caller's only handle on the data) is simply dropped.
		return false;
	}

	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> Pinned = AttachedMeshOwner.Pin();
	if (!Pinned.IsValid())
	{
		return false;
	}

	const FVertexMaskForgeWorkingMeshProvenance& MeshProvenance = Pinned->GetWorkingMesh().Provenance;
	if (!MeshProvenance.IsValid())
	{
		return false;
	}

	TArray<FColor>& Working = MutableWorkingColors();
	Working = MoveTemp(FinalRGB);

	// AUDITED: Alpha is preserved byte-identical BY CONSTRUCTION -- every element's Alpha byte is
	// overwritten from this owner's own current Baseline right here, unconditionally. The caller's
	// FinalRGB Alpha bytes (whatever they were) are never read for this purpose.
	for (int32 i = 0; i < Working.Num(); ++i)
	{
		Working[i].A = Baseline[i].A;
	}

	if (CommitMode == EColorCommitMode::Consolidate)
	{
		MutableCommittedColors() = Working;
	}

	// Authority re-derived as a direct consequence of this validated, successful application -- never
	// accepted as a parameter here or anywhere else.
	FVertexMaskForgeWorkingColorsAuthority NewAuthority;
	NewAuthority.SourceStaticMesh = MeshProvenance.GetSourceStaticMesh();
	NewAuthority.bUseSourceTopology = MeshProvenance.GetUseSourceTopology();
	NewAuthority.LODIndex = MeshProvenance.GetLODIndex();
	NewAuthority.Revision = MeshProvenance.GetRevision();
	NewAuthority.Cardinality = Working.Num();
	NewAuthority.MeshOwnerId = MeshProvenance.GetMeshOwnerId();
	NewAuthority.StateOwnerId = this;
	NewAuthority.bIsValid = true;
	PreviewState.WorkingColorsAuthority = NewAuthority;

	return true;
}

bool FVertexMaskForgeWorkingStateOwner::UpdateWorkingColorsRGB(TArrayView<const FColor> NewRGB)
{
	if (bPermanentlyInvalid || !bColorsInitialized)
	{
		return false;
	}

	TArray<FColor>& Working = MutableWorkingColors();
	if (NewRGB.Num() != Working.Num())
	{
		return false;
	}

	for (int32 i = 0; i < Working.Num(); ++i)
	{
		const uint8 PreservedAlpha = Working[i].A;
		Working[i] = NewRGB[i];
		Working[i].A = PreservedAlpha;
	}

	MutableCommittedColors() = Working;
	return true;
}

bool FVertexMaskForgeWorkingStateOwner::FillBlack()
{
	if (bPermanentlyInvalid || !bColorsInitialized)
	{
		return false;
	}

	TArray<FColor>& Working = MutableWorkingColors();
	for (FColor& Color : Working)
	{
		Color.R = 0;
		Color.G = 0;
		Color.B = 0;
	}

	MutableCommittedColors() = Working;
	return true;
}

bool FVertexMaskForgeWorkingStateOwner::FillWhite()
{
	if (bPermanentlyInvalid || !bColorsInitialized)
	{
		return false;
	}

	TArray<FColor>& Working = MutableWorkingColors();
	for (FColor& Color : Working)
	{
		Color.R = 255;
		Color.G = 255;
		Color.B = 255;
	}

	MutableCommittedColors() = Working;
	return true;
}

bool FVertexMaskForgeWorkingStateOwner::RestoreFromBaseline()
{
	if (bPermanentlyInvalid || !bColorsInitialized)
	{
		return false;
	}

	// AUDITED: deliberately does NOT consolidate -- see this class's own header doc comment.
	MutableWorkingColors() = MutableBaselineColors();
	return true;
}

bool FVertexMaskForgeWorkingStateOwner::RestoreFromCommitted()
{
	if (bPermanentlyInvalid || !bColorsInitialized)
	{
		return false;
	}

	MutableWorkingColors() = MutableCommittedColors();
	return true;
}

bool FVertexMaskForgeWorkingStateOwner::Reset()
{
	if (bPermanentlyInvalid)
	{
		return false;
	}

	InvalidateColors();
	return true;
}

FVertexMaskForgeWorkingColorPublicationBinding FVertexMaskForgeWorkingStateOwner::CreateBinding() const
{
	// AUDITED (M16-J.0B.1 corrective pass): a single prvalue return -- see the Binding class's own private
	// constructor doc comment for why this specific shape (not a named local) is required now that the
	// type is non-copyable/non-movable. AttachedMeshOwner (a TWeakPtr) is passed through as-is -- never
	// Pin()-ed here -- so the Binding never carries a raw pointer whose validity depends on this function's
	// own, long-since-returned stack frame; the validator Pin()s independently, at the moment it actually
	// needs the owner. M16-J.0B.1 2nd corrective pass: no longer takes &PreviewState at all -- the Binding
	// carries ONLY the weak MeshOwner reference now (see that class's own module comment on why).
	return FVertexMaskForgeWorkingColorPublicationBinding(AttachedMeshOwner);
}

EVertexMaskForgeWorkingColorsPublicationValidationStatus FVertexMaskForgeWorkingStateOwner::ValidateBinding() const
{
	// AUDITED (M16-J.0B.1 2nd corrective pass): PreviewState is passed LIVE, in this same call -- `this` is
	// guaranteed alive for the duration of this member function, so there is no window in which a stale
	// reference to it could be read. Never stored inside the Binding itself.
	return VertexMaskForgeWorkingColorsProvenance::ValidateWorkingColorsPublicationBinding(CreateBinding(), PreviewState);
}

EVertexMaskForgeWorkingColorsPublicationValidationStatus FVertexMaskForgeWorkingStateOwner::ValidateAgainst(const TSharedPtr<FVertexMaskForgeWorkingMeshOwner>& OtherMeshOwner) const
{
	// AUDITED (M16-J.0B.1 corrective pass): OtherMeshOwner is now a TSharedPtr (was a raw `const&` before),
	// specifically so a TWeakPtr can be derived from it here -- a raw C++ reference carries no ownership
	// information a TWeakPtr could be built from. Same prvalue-return shape as CreateBinding() above; same
	// live-PreviewState-parameter shape as ValidateBinding() above.
	return VertexMaskForgeWorkingColorsProvenance::ValidateWorkingColorsPublicationBinding(
		FVertexMaskForgeWorkingColorPublicationBinding(TWeakPtr<const FVertexMaskForgeWorkingMeshOwner>(OtherMeshOwner)), PreviewState);
}

EVertexMaskForgeWorkingColorsPublicationValidationStatus FVertexMaskForgeWorkingStateOwner::ValidateBindingAgainstThis(const FVertexMaskForgeWorkingColorPublicationBinding& ExternalBinding) const
{
	// AUDITED (M16-J.0B.1 2nd corrective pass): the caller-supplied ExternalBinding may have been created
	// by ANY StateOwner (including one that no longer exists) -- but since Binding carries only a weak
	// MeshOwner reference (never a pointer into whatever StateOwner created it), there is nothing here that
	// could ever dereference that original StateOwner. PreviewState supplied is THIS owner's own, live.
	return VertexMaskForgeWorkingColorsProvenance::ValidateWorkingColorsPublicationBinding(ExternalBinding, PreviewState);
}
