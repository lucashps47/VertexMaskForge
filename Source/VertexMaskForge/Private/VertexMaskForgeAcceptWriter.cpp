#include "VertexMaskForgeAcceptWriter.h"

#include "CoreGlobals.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "VertexMaskForgeWorkingMeshOwner.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

#define LOCTEXT_NAMESPACE "SVertexMaskForgePanel"

namespace VertexMaskForgeAcceptWriter
{
	bool WriteAcceptTargets(const TArray<VertexMaskForgeAcceptTargetBuilder::FAcceptTarget>& Targets, TArray<UStaticMesh*>& OutModifiedMeshes, FText& OutErrorText)
	{
		for (const VertexMaskForgeAcceptTargetBuilder::FAcceptTarget& Target : Targets)
		{
			UStaticMesh* Mesh = Target.Mesh.Get();
			const FMeshDescription* MeshDescription = IsValid(Mesh) ? Mesh->GetMeshDescription(0) : nullptr;
			const FStaticMeshRenderData* RenderData = IsValid(Mesh) ? Mesh->GetRenderData() : nullptr;
			const bool bLODValid = RenderData && RenderData->LODResources.IsValidIndex(0);

			if (!MeshDescription || !bLODValid
				|| RenderData->LODResources[0].WedgeMap.Num() != MeshDescription->VertexInstances().Num()
				|| Target.FinalColors.Num() != static_cast<int32>(RenderData->LODResources[0].GetNumVertices()))
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptWriteRevalidationFailedFormat", "'{0}' failed re-validation immediately before writing; aborting Accept (nothing was modified)."),
					FText::FromString(Target.AssetName));
				return false;
			}

			// AUDITED (V2-G, Thickness freshness): only applies when this entry's accepted result
			// actually depends on Thickness (Ready + a populated cache) -- never widens the deep
			// comparison to entries/generators that never used Thickness. No fingerprint short-circuit
			// here -- the full semantic comparison always runs, so a match can never be assumed from a
			// cheap proxy alone.
			if (Target.Entry.IsValid() && Target.Entry->GeneratorState.ThicknessMask.State == EVertexMaskForgeScalarMaskState::Ready
				&& Target.Entry->GeneratorState.ThicknessCache.IsValid()
				&& !VertexMaskForgeWorkingMeshTypes::AreThicknessGeometrySnapshotsExactlyEquivalent(*Target.Entry->GeneratorState.ThicknessCache, RenderData->LODResources[0]))
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptThicknessFreshnessMismatchFormat", "'{0}': geometry or normals changed since Thickness Mask was generated; aborting Accept (nothing was modified). Regenerate the mask and try again."),
					FText::FromString(Target.AssetName));
				return false;
			}
		}

		for (const VertexMaskForgeAcceptTargetBuilder::FAcceptTarget& Target : Targets)
		{
			UStaticMesh* Mesh = Target.Mesh.Get();
			FMeshDescription* MeshDescription = Mesh->GetMeshDescription(0);
			const FStaticMeshLODResources& LOD0 = Mesh->GetRenderData()->LODResources[0];

			// AUDITED (Undo/Redo fix): SetFlags is defensive (assets already loaded in the Editor are
			// transactional in practice) and matches the native reference exactly; Modify() captures
			// UStaticMesh's own properties, ModifyMeshDescription(0) captures the source data that
			// actually changes.
			Mesh->SetFlags(RF_Transactional);
			Mesh->Modify();
			Mesh->ModifyMeshDescription(0);

			FStaticMeshAttributes Attributes(*MeshDescription);
			TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();

			int32 VertexInstanceIndex = 0;
			for (const FVertexInstanceID VertexInstanceID : MeshDescription->VertexInstances().GetElementIDs())
			{
				const int32 RenderIndex = LOD0.WedgeMap[VertexInstanceIndex];
				if (RenderIndex != INDEX_NONE && Target.FinalColors.IsValidIndex(RenderIndex))
				{
					Colors[VertexInstanceID] = FLinearColor(Target.FinalColors[RenderIndex]);
				}
				++VertexInstanceIndex;
			}

			Mesh->CommitMeshDescription(0);
			OutModifiedMeshes.Add(Mesh);
		}

		return true;
	}

	bool WriteSourceTopologyAcceptTargets(const TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget>& Targets, TArray<UStaticMesh*>& OutModifiedMeshes, FText& OutErrorText)
	{
		using namespace UE::Geometry;

		for (const VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget& Target : Targets)
		{
			UStaticMesh* Mesh = Target.Mesh.Get();
			// AUDITED (M16-J.0B.1 corrective pass): Target.Entry->MeshOwner->GetWorkingMesh() is only
			// dereferenced once Target.Entry.IsValid() is already known true, via && short-circuiting --
			// mirrors the exact same safety the removed WorkingMesh reference member never actually needed
			// to provide (Entry itself being valid was always the real precondition).
			const bool bEntryValid = Target.Entry.IsValid() && Target.Entry->MeshOwner->GetWorkingMesh().Mesh.IsValid()
				&& !Target.Entry->MeshOwner->GetWorkingMesh().TriIDMap.IsEmpty();
			const FMeshDescription* MeshDescription = IsValid(Mesh) ? Mesh->GetMeshDescription(0) : nullptr;
			const int32 NumCorners = bEntryValid ? Target.Entry->MeshOwner->GetWorkingMesh().Mesh->TriangleCount() * 3 : 0;

			if (!MeshDescription || !bEntryValid || Target.FinalColors.Num() != NumCorners)
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptSourceTopologyWriteRevalidationFailedFormat", "'{0}' failed re-validation immediately before writing; aborting Accept (nothing was modified)."),
					FText::FromString(Target.AssetName));
				return false;
			}

			// bEntryValid == true (checked above) guarantees Target.Entry is valid from this point on, so
			// a short-lived local view is safe to take here (see FVertexMaskForgeSelectedMesh::MeshOwner's
			// own doc comment on why this is never cached beyond one function's own scope).
			const FVertexMaskForgeWorkingMesh& WorkingMesh = Target.Entry->MeshOwner->GetWorkingMesh();

			// AUDITED (commit preflight correction): full correspondence re-check, same as
			// BuildSourceTopologyAcceptTargets' own preflight -- nothing else can have touched these
			// assets between preflight and here (synchronous, same call), but re-proving it immediately
			// before the first Modify() matches WriteAcceptTargets' own re-validation discipline exactly.
			if (!VertexMaskForgeAcceptTargetBuilder::ValidateSourceTopologyCorrespondence(
				*WorkingMesh.Mesh, WorkingMesh.TriIDMap, *MeshDescription, Target.AssetName, OutErrorText))
			{
				return false;
			}

			// AUDITED (V2-G, Thickness freshness): ValidateSourceTopologyCorrespondence above only
			// proves structural/ID correspondence (TriangleID/VertexInstanceID validity) -- it never
			// compares position or normal VALUES, so a reimport/edit preserving every count and ID would
			// slip through it silently. Only applies when this entry's result depends on Thickness.
			if (Target.Entry->GeneratorState.ThicknessMask.State == EVertexMaskForgeScalarMaskState::Ready
				&& Target.Entry->GeneratorState.SourceTopologyThicknessCache.IsValid()
				&& !VertexMaskForgeWorkingMeshTypes::IsThicknessSourceTopologyContentUnchanged(
					*WorkingMesh.Mesh, WorkingMesh.TriIDMap, *MeshDescription))
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptSourceTopologyThicknessFreshnessMismatchFormat", "'{0}': geometry or normals changed since Thickness Mask was generated; aborting Accept (nothing was modified). Regenerate the mask and try again."),
					FText::FromString(Target.AssetName));
				return false;
			}
		}

		for (const VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget& Target : Targets)
		{
			UStaticMesh* Mesh = Target.Mesh.Get();
			FMeshDescription* MeshDescription = Mesh->GetMeshDescription(0);
			// AUDITED (M16-J.0B.1 corrective pass): this second loop only ever runs targets that already
			// survived the first loop's bEntryValid gate above (same Targets array, same Entry pointers),
			// so Target.Entry is known valid here too.
			const FDynamicMesh3& WorkingDynamicMesh = *Target.Entry->MeshOwner->GetWorkingMesh().Mesh;
			const TArray<FTriangleID>& TriIDMap = Target.Entry->MeshOwner->GetWorkingMesh().TriIDMap;

			// AUDITED (Undo/Redo fix): see WriteAcceptTargets' own doc comment -- ModifyMeshDescription
			// is the call that actually makes the source data participate in the Transaction Buffer.
			Mesh->SetFlags(RF_Transactional);
			Mesh->Modify();
			Mesh->ModifyMeshDescription(0);

			FStaticMeshAttributes Attributes(*MeshDescription);
			TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();

			int32 CornerIndex = 0;
			for (const int32 TriangleID : WorkingDynamicMesh.TriangleIndicesItr())
			{
				if (!TriIDMap.IsValidIndex(TriangleID))
				{
					// Re-validated above; never reachable in practice, but never crash or misalign the
					// remaining corners if it somehow were.
					CornerIndex += 3;
					continue;
				}
				const FTriangleID SourceTriangleID = TriIDMap[TriangleID];
				const TArrayView<const FVertexInstanceID> SourceInstances = MeshDescription->GetTriangleVertexInstances(SourceTriangleID);
				if (SourceInstances.Num() != 3)
				{
					CornerIndex += 3;
					continue;
				}
				for (int32 Corner = 0; Corner < 3; ++Corner, ++CornerIndex)
				{
					if (Target.FinalColors.IsValidIndex(CornerIndex))
					{
						Colors[SourceInstances[Corner]] = FLinearColor(Target.FinalColors[CornerIndex]);
					}
				}
			}

			Mesh->CommitMeshDescription(0);

			// AUDITED (BUG FIX round -- persistence verification, per explicit requirement): re-read
			// VertexInstanceColors from the LIVE MeshDescription (re-fetched, not the stale local
			// pointer/attributes-ref from before Commit) and compare against what was just written, via
			// the EXACT SAME TriangleID+corner walk -- never trust the preview's appearance alone as
			// proof the asset was actually updated. Aborts BEFORE Build()/notifying anything if the
			// write did not actually stick.
			{
				const FMeshDescription* VerifyMeshDescription = Mesh->GetMeshDescription(0);
				const FStaticMeshConstAttributes VerifyAttributes(*VerifyMeshDescription);
				const TVertexInstanceAttributesConstRef<FVector4f> VerifyColors = VerifyAttributes.GetVertexInstanceColors();

				int32 VerifyCornerIndex = 0;
				int32 NumMismatched = 0;
				for (const int32 TriangleID : WorkingDynamicMesh.TriangleIndicesItr())
				{
					if (!TriIDMap.IsValidIndex(TriangleID)) { VerifyCornerIndex += 3; continue; }
					const FTriangleID VerifySourceTriangleID = TriIDMap[TriangleID];
					const TArrayView<const FVertexInstanceID> VerifySourceInstances = VerifyMeshDescription->GetTriangleVertexInstances(VerifySourceTriangleID);
					if (VerifySourceInstances.Num() != 3) { VerifyCornerIndex += 3; continue; }
					for (int32 Corner = 0; Corner < 3; ++Corner, ++VerifyCornerIndex)
					{
						if (!Target.FinalColors.IsValidIndex(VerifyCornerIndex)) { continue; }
						const FVector4f Expected(FLinearColor(Target.FinalColors[VerifyCornerIndex]));
						const FVector4f Actual = VerifyColors.Get(VerifySourceInstances[Corner]);
						if (!Expected.Equals(Actual, 1.0f / 512.0f))
						{
							++NumMismatched;
						}
					}
				}

				if (NumMismatched > 0)
				{
					OutErrorText = FText::Format(
						LOCTEXT("AcceptSourceTopologyPersistenceVerificationFailedFormat",
							"'{0}': {1} Vertex Instance color(s) did not match what was written immediately after CommitMeshDescription; aborting Accept before Build/notify (the write did not persist as expected)."),
						FText::FromString(Target.AssetName), FText::AsNumber(NumMismatched));
					UE_LOG(LogVertexMaskForge, Error,
						TEXT("Vertex Mask Forge: Accept (Source Topology) persistence verification FAILED for '%s' -- %d mismatched Vertex Instance color(s)."),
						*Target.AssetName, NumMismatched);
					return false;
				}
			}

			UE_LOG(LogVertexMaskForge, Log,
				TEXT("Vertex Mask Forge: Accept (Source Topology) -- '%s': colors written, CommitMeshDescription succeeded, persistence verified."),
				*Target.AssetName);

			OutModifiedMeshes.Add(Mesh);
		}

		return true;
	}

	bool BuildModifiedMeshes(const TArray<UStaticMesh*>& Meshes, FText& OutErrorText)
	{
		for (UStaticMesh* Mesh : Meshes)
		{
			if (!IsValid(Mesh))
			{
				continue;
			}

			TGuardValue<ITransaction*> SuppressTransaction(GUndo, nullptr);
			TArray<FText> BuildErrors;
			Mesh->Build(/*bInSilent=*/true, &BuildErrors);
			if (!BuildErrors.IsEmpty())
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptBuildFailedFormat", "'{0}': Build failed after committing Vertex Colors: {1}"),
					FText::FromString(Mesh->GetName()), BuildErrors[0]);
				UE_LOG(LogVertexMaskForge, Error, TEXT("Vertex Mask Forge: Accept Build FAILED for '%s': %s"),
					*Mesh->GetName(), *BuildErrors[0].ToString());
				return false;
			}
			UE_LOG(LogVertexMaskForge, Log, TEXT("Vertex Mask Forge: Accept -- '%s': Build completed."), *Mesh->GetName());
		}

		return true;
	}
}

#undef LOCTEXT_NAMESPACE
