#include "VertexMaskForgeAcceptTargetBuilder.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshResources.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

#define LOCTEXT_NAMESPACE "SVertexMaskForgePanel"

namespace VertexMaskForgeAcceptTargetBuilder
{
	bool ValidateSourceTopologyCorrespondence(
		const UE::Geometry::FDynamicMesh3& Mesh,
		const TArray<FTriangleID>& TriIDMap,
		const FMeshDescription& MeshDescription,
		const FString& AssetName,
		FText& OutErrorText)
	{
		TSet<int32> SeenDestinationTriangles;
		SeenDestinationTriangles.Reserve(Mesh.TriangleCount());

		for (const int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			if (!TriIDMap.IsValidIndex(TriangleID))
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptSourceTopologyMissingTriIDMapEntryFormat",
						"'{0}': the triangle/corner correspondence is missing an entry for one or more triangles. Try Refresh Selection again."),
					FText::FromString(AssetName));
				return false;
			}

			const FTriangleID SourceTriangleID = TriIDMap[TriangleID];
			if (!MeshDescription.IsTriangleValid(SourceTriangleID))
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptSourceTopologyStaleTriangleFormat",
						"'{0}': the correspondence points to a triangle that no longer exists in the Static Mesh's source data (it may have been rebuilt or reimported since the Preview was generated). Try Refresh Selection again."),
					FText::FromString(AssetName));
				return false;
			}

			bool bAlreadySeen = false;
			SeenDestinationTriangles.Add(SourceTriangleID.GetValue(), &bAlreadySeen);
			if (bAlreadySeen)
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptSourceTopologyDuplicateTriangleFormat",
						"'{0}': two different triangles map to the same source triangle -- refusing to write an ambiguous correspondence. Try Refresh Selection again."),
					FText::FromString(AssetName));
				return false;
			}

			const TArrayView<const FVertexInstanceID> SourceInstances = MeshDescription.GetTriangleVertexInstances(SourceTriangleID);
			if (SourceInstances.Num() != 3)
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptSourceTopologyBadInstanceCountFormat",
						"'{0}': a destination triangle does not have exactly three Vertex Instances. Try Refresh Selection again."),
					FText::FromString(AssetName));
				return false;
			}
			for (const FVertexInstanceID InstanceID : SourceInstances)
			{
				if (!MeshDescription.IsVertexInstanceValid(InstanceID))
				{
					OutErrorText = FText::Format(
						LOCTEXT("AcceptSourceTopologyInvalidInstanceFormat",
							"'{0}': the correspondence points to a Vertex Instance that no longer exists. Try Refresh Selection again."),
						FText::FromString(AssetName));
					return false;
				}
			}
		}

		return true;
	}

	bool BuildAcceptTargets(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		const bool bDirectionalNormalMaskEnabled,
		const EVertexMaskForgeNormalSpace DirectionalNormalSpace,
		TArray<FAcceptTarget>& OutTargets,
		FText& OutErrorText)
	{
		OutTargets.Reset();

		for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
		{
			// AUDITED (composition-stack checkpoint): eligible if EITHER slot is Ready -- this gate is
			// only a coarse pre-filter anyway; the actual data persisted is always State.WorkingColors
			// (read verbatim below), never re-derived from either mask here.
			//
			// AUDITED (Nanite source-topology support): a Source-Topology entry (every Nanite-enabled
			// mesh -- see FVertexMaskForgeSelectedMesh::bUseSourceTopology) is handled EXCLUSIVELY by
			// BuildSourceTopologyAcceptTargets/WriteSourceTopologyAcceptTargets, never by this function
			// -- skip it here unconditionally, rather than letting it fall through to the WedgeMap
			// check below (which would incorrectly reject a Nanite mesh that happens to have a valid
			// WedgeMap too, since Source-Topology mode is chosen unconditionally for Nanite, not just
			// when WedgeMap is invalid).
			if (!Entry.IsValid() || Entry->bUseSourceTopology || Entry->PreviewComponents.IsEmpty()
				|| (Entry->WorkingMesh.BoundingBoxMask.State != EVertexMaskForgeScalarMaskState::Ready
					&& Entry->WorkingMesh.AmbientOcclusionMask.State != EVertexMaskForgeScalarMaskState::Ready
					&& Entry->WorkingMesh.CurvatureMask.State != EVertexMaskForgeScalarMaskState::Ready
					&& Entry->WorkingMesh.NoiseMask.State != EVertexMaskForgeScalarMaskState::Ready
					&& Entry->WorkingMesh.MaterialSlotMask.State != EVertexMaskForgeScalarMaskState::Ready
					&& Entry->WorkingMesh.DirectionalNormalMask.State != EVertexMaskForgeScalarMaskState::Ready
					&& Entry->WorkingMesh.ThicknessMask.State != EVertexMaskForgeScalarMaskState::Ready))
			{
				continue;
			}

			// AUDITED (V2-E CORRECTIVE PASS, transform freshness): re-checked LIVE, right here,
			// immediately before any Modify() -- NEVER trusts the cached WorkingMesh.
			// bDirectionalNormalWorldSpaceConflict flag alone (that flag can only ever be as fresh as the
			// last live regeneration pass; a component could have been moved since, faster than the
			// debounce). Only evaluated when Directional Normal Mask is enabled AND in World Space --
			// false/skipped in every other case, so this never blocks Accept for any other generator or
			// for Local Space.
			if (bDirectionalNormalMaskEnabled && DirectionalNormalSpace == EVertexMaskForgeNormalSpace::World)
			{
				float LiveDeviation = 0.0f;
				if (VertexMaskForgeWorkingMeshTypes::HasConflictingWorldSpaceNormalTransforms(Entry->PreviewComponents, LiveDeviation))
				{
					OutErrorText = FText::Format(
						LOCTEXT("AcceptDirectionalNormalWorldSpaceConflictFormat",
							"'{0}': World Space Directional Normal Mask cannot write conflicting results from differently transformed instances of the same Static Mesh asset."),
						FText::FromString(Entry->AssetName));
					return false;
				}
			}

			UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
			if (!IsValid(Mesh) || !Mesh->HasValidRenderData(/*bCheckLODForVerts=*/true, /*LODIndex=*/0))
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptInvalidMeshFormat", "'{0}': Static Mesh could not be resolved or has no valid LOD 0 render data."),
					FText::FromString(Entry->AssetName));
				return false;
			}

			const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
			if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptNoRenderDataFormat", "'{0}': no LOD 0 render data available."),
					FText::FromString(Entry->AssetName));
				return false;
			}
			const FStaticMeshLODResources& LOD0 = RenderData->LODResources[0];

			// Compose independently per component and require agreement -- see the divergent-
			// baseline case in the doc comment above.
			TArray<FColor> ReferenceColors;
			bool bHaveReference = false;
			for (const FVertexMaskForgePreviewComponentState& State : Entry->PreviewComponents)
			{
				const UStaticMeshComponent* SourceComponent = State.SourceComponent.Get();
				if (!IsValid(SourceComponent))
				{
					continue;
				}

				// AUDITED (recompute-at-Accept fix): read-only -- see this function's own doc comment.
				// Empty WorkingColors means this component currently has no valid composed result
				// (mirrors the old "per-instance mask not Ready" skip).
				if (State.WorkingColors.IsEmpty())
				{
					continue;
				}
				// AUDITED (Preview-Mode-cannot-affect-Accept fix): the raw multi-channel result,
				// verbatim -- never DeriveDisplayColors, never CurrentPreviewMode.
				TArray<FColor> ComponentColors = State.WorkingColors;

				if (!bHaveReference)
				{
					ReferenceColors = MoveTemp(ComponentColors);
					bHaveReference = true;
				}
				else if (ComponentColors != ReferenceColors)
				{
					OutErrorText = FText::Format(
						LOCTEXT("AcceptDivergentBaselineFormat",
							"'{0}': different instances of this asset produced different Preview results (their per-instance Vertex Color overrides differ), so writing to the shared Static Mesh asset would be ambiguous. Accept is blocked for this operation; make the instances' overrides consistent, or Cancel."),
						FText::FromString(Entry->AssetName));
					return false;
				}
			}

			if (!bHaveReference)
			{
				// Every tracked component became invalid since the Preview was last updated; nothing
				// to accept for this entry -- not an error, just nothing eligible here.
				continue;
			}

			// Deterministic wedge->render-vertex mapping check (audited): never approximate by
			// position. Read-only check here (element count only); the mutable pass happens in
			// WriteAcceptTargets.
			//
			// AUDITED (Nanite): confirmed against the actual UE 5.8 Nanite build source
			// (StaticMeshBuilder.cpp's BuildNanite()) that for a Nanite-enabled asset WITHOUT an
			// explicit HiRes Source Model (the common/default case), LODResources[0] is NOT a
			// full-fidelity copy of the source mesh -- it is Nanite's own decimated/re-clustered
			// fallback proxy (FallbackRelativeError defaults to 1.0, i.e. reduction IS applied), and
			// FStaticMeshLODResources::WedgeMap for LOD 0 is never populated on that path at all (the
			// Nanite build explicitly passes bNeedWedgeMap=false -- "mainly used by non-Nanite mesh
			// painting"). So this same WedgeMap check that already protects every other asset ALSO
			// correctly refuses Nanite assets on that default path -- it must NOT be bypassed or
			// "fixed" with a position-based remap, which would silently paint the wrong (reduced)
			// vertex set. Only the message differs, so the user is told the true, actionable reason
			// instead of the generic "try rebuilding" text (rebuilding will not fix this for Nanite --
			// the fallback is decimated by design).
			const FMeshDescription* MeshDescription = Mesh->GetMeshDescription(0);
			const bool bWedgeMapValid = MeshDescription
				&& LOD0.WedgeMap.Num() != 0
				&& LOD0.WedgeMap.Num() == MeshDescription->VertexInstances().Num()
				&& ReferenceColors.Num() == static_cast<int32>(LOD0.GetNumVertices());
			if (!bWedgeMapValid)
			{
				if (Mesh->IsNaniteEnabled())
				{
					OutErrorText = FText::Format(
						LOCTEXT("AcceptNaniteReducedFallbackFormat",
							"'{0}': this Static Mesh has Nanite enabled and its LOD 0 fallback is a reduced/re-clustered proxy (not 1:1 with the source mesh), so Vertex Mask Forge cannot currently write results back into it safely. Assigning an explicit High Res Source Model to this asset (Nanite Settings) restores a full-fidelity LOD 0 and may allow Accept to succeed; otherwise, source-topology write support for Nanite is not yet implemented."),
						FText::FromString(Entry->AssetName));
				}
				else
				{
					OutErrorText = FText::Format(
						LOCTEXT("AcceptNoWedgeMapFormat",
							"'{0}': no deterministic wedge-to-render-vertex mapping is available (FStaticMeshLODResources::WedgeMap missing or stale for LOD 0). Refusing to write -- an approximate position-based remap could paint seams incorrectly. Try rebuilding this Static Mesh (Build) and Accept again."),
						FText::FromString(Entry->AssetName));
				}
				return false;
			}

			FAcceptTarget Target;
			Target.Mesh = Mesh;
			Target.AssetName = Entry->AssetName;
			Target.FinalColors = MoveTemp(ReferenceColors);
			Target.Entry = Entry;
			OutTargets.Add(MoveTemp(Target));
		}

		// AUDITED (BUG FIX -- Nanite Accept root cause): this function used to treat an empty
		// OutTargets as a hard failure on its own ("AcceptNothingEligible"). That was correct back when
		// this was the ONLY Accept target builder, but since the Source-Topology split, a selection
		// that is ENTIRELY Nanite (bUseSourceTopology, skipped above unconditionally) legitimately
		// produces zero render-vertex targets while still having real, valid Source-Topology targets
		// waiting in BuildSourceTopologyAcceptTargets. Returning false HERE made AcceptPendingChanges
		// bail out immediately -- before BuildSourceTopologyAcceptTargets was ever called -- so a
		// pure-Nanite Accept always failed with "No eligible pending changes to accept.", even though
		// nothing had actually been validated yet, let alone written. "Nothing eligible in EITHER
		// domain" is now decided ONCE, by the caller, after combining both builders' results -- see
		// AcceptPendingChanges. An empty result here is not, by itself, an error.
		return true;
	}

	bool BuildSourceTopologyAcceptTargets(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		const bool bDirectionalNormalMaskEnabled,
		const EVertexMaskForgeNormalSpace DirectionalNormalSpace,
		TArray<FSourceTopologyAcceptTarget>& OutTargets,
		FText& OutErrorText)
	{
		OutTargets.Reset();

		for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
		{
			if (!Entry.IsValid() || !Entry->bUseSourceTopology || Entry->PreviewComponents.IsEmpty()
				|| (Entry->WorkingMesh.BoundingBoxMask.State != EVertexMaskForgeScalarMaskState::Ready
					&& Entry->WorkingMesh.AmbientOcclusionMask.State != EVertexMaskForgeScalarMaskState::Ready
					&& Entry->WorkingMesh.CurvatureMask.State != EVertexMaskForgeScalarMaskState::Ready
					&& Entry->WorkingMesh.NoiseMask.State != EVertexMaskForgeScalarMaskState::Ready
					&& Entry->WorkingMesh.MaterialSlotMask.State != EVertexMaskForgeScalarMaskState::Ready
					&& Entry->WorkingMesh.DirectionalNormalMask.State != EVertexMaskForgeScalarMaskState::Ready
					&& Entry->WorkingMesh.ThicknessMask.State != EVertexMaskForgeScalarMaskState::Ready))
			{
				continue;
			}

			// AUDITED (V2-E CORRECTIVE PASS, transform freshness): same LIVE re-check as
			// BuildAcceptTargets' own -- see its doc comment. Never trusts the cached
			// bDirectionalNormalWorldSpaceConflict flag alone.
			if (bDirectionalNormalMaskEnabled && DirectionalNormalSpace == EVertexMaskForgeNormalSpace::World)
			{
				float LiveDeviation = 0.0f;
				if (VertexMaskForgeWorkingMeshTypes::HasConflictingWorldSpaceNormalTransforms(Entry->PreviewComponents, LiveDeviation))
				{
					OutErrorText = FText::Format(
						LOCTEXT("AcceptSourceTopologyDirectionalNormalWorldSpaceConflictFormat",
							"'{0}': World Space Directional Normal Mask cannot write conflicting results from differently transformed instances of the same Static Mesh asset."),
						FText::FromString(Entry->AssetName));
					return false;
				}
			}

			UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
			if (!IsValid(Mesh) || !Entry->WorkingMesh.Mesh.IsValid())
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptSourceTopologyInvalidMeshFormat", "'{0}': Static Mesh or its working topology could not be resolved."),
					FText::FromString(Entry->AssetName));
				return false;
			}

			// Compose independently per component and require agreement -- same divergent-baseline
			// rule as BuildAcceptTargets' own doc comment (no per-instance override in this domain, but
			// World Space Bounding Box axes and Ambient Occlusion are both still per-instance-transform-
			// dependent, so two components CAN legitimately disagree).
			TArray<FColor> ReferenceColors;
			bool bHaveReference = false;
			for (const FVertexMaskForgePreviewComponentState& State : Entry->PreviewComponents)
			{
				const UStaticMeshComponent* SourceComponent = State.SourceComponent.Get();
				if (!IsValid(SourceComponent))
				{
					continue;
				}
				if (State.SourceTopologyWorkingColors.IsEmpty())
				{
					continue;
				}
				TArray<FColor> ComponentColors = State.SourceTopologyWorkingColors;

				if (!bHaveReference)
				{
					ReferenceColors = MoveTemp(ComponentColors);
					bHaveReference = true;
				}
				else if (ComponentColors != ReferenceColors)
				{
					OutErrorText = FText::Format(
						LOCTEXT("AcceptSourceTopologyDivergentBaselineFormat",
							"'{0}': different instances of this asset produced different Preview results (World Space Bounding Box and/or Ambient Occlusion depend on each instance's own transform), so writing to the shared Static Mesh asset would be ambiguous. Accept is blocked for this operation; make the instances' transforms/results consistent, or Cancel."),
						FText::FromString(Entry->AssetName));
					return false;
				}
			}

			if (!bHaveReference)
			{
				continue;
			}

			// Correspondence check: FinalColors must match the CURRENT corner count exactly -- never
			// approximate -- and the full TriIDMap -> FTriangleID -> VertexInstanceID chain must be
			// formally valid against the LIVE MeshDescription (see ValidateSourceTopologyCorrespondence
			// for the complete list of what this proves).
			const int32 NumCorners = Entry->WorkingMesh.Mesh->TriangleCount() * 3;
			const FMeshDescription* LiveMeshDescription = Mesh->GetMeshDescription(0);
			if (!LiveMeshDescription || ReferenceColors.Num() != NumCorners)
			{
				OutErrorText = FText::Format(
					LOCTEXT("AcceptSourceTopologyCorrespondenceFormat",
						"'{0}': the triangle/corner correspondence needed to write vertex colors is unavailable or stale. Try Refresh Selection again."),
					FText::FromString(Entry->AssetName));
				return false;
			}
			if (!ValidateSourceTopologyCorrespondence(
				*Entry->WorkingMesh.Mesh, Entry->WorkingMesh.TriIDMap, *LiveMeshDescription, Entry->AssetName, OutErrorText))
			{
				return false;
			}

			FSourceTopologyAcceptTarget Target;
			Target.Mesh = Mesh;
			Target.AssetName = Entry->AssetName;
			Target.FinalColors = MoveTemp(ReferenceColors);
			Target.Entry = Entry;
			OutTargets.Add(MoveTemp(Target));
		}

		return true;
	}
}

#undef LOCTEXT_NAMESPACE
