#include "VertexMaskForgeDynamicAcceptTargetBuilder.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "VertexMaskForgeDynamicLayerStack.h"
#include "VertexMaskForgeDynamicSourceTopologyComposition.h"
#include "VertexMaskForgeWorkingMeshTypes.h"
#include "VertexMaskForgeWorkingStateOwner.h"

#define LOCTEXT_NAMESPACE "SVertexMaskForgePanel"

namespace VertexMaskForgeDynamicAcceptTargetBuilder
{
	bool BuildSourceTopologyAcceptTargets(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		const FVertexMaskForgeDynamicLayerStack& DynamicLayerStack,
		TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget>& OutTargets,
		FText& OutErrorText)
	{
		// AUDITED: builds into a local array, never touching OutTargets until the very end -- unlike
		// VertexMaskForgeAcceptTargetBuilder::BuildSourceTopologyAcceptTargets (which Resets() OutTargets
		// unconditionally at the top), this module's own contract requires OutTargets to be left
		// completely unchanged from the caller's own pre-call value on failure (see this function's own
		// header doc comment), so the Reset()-then-populate pattern cannot be reused here verbatim.
		TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> LocalTargets;

		for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
		{
			// AUDITED: no GeneratorState.*.State==Ready pre-filter -- that state is exclusively Legacy's
			// own. Dynamic's own eligibility is computed entirely below, from BaselineColors/orchestrator
			// success -- an empty or all-disabled DynamicLayerStack is a VALID computation (Baseline
			// passthrough), never pre-filtered out here.
			if (!Entry.IsValid() || !Entry->bUseSourceTopology || Entry->PreviewComponents.IsEmpty())
			{
				continue;
			}

			UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
			const FVertexMaskForgeWorkingMesh& WorkingMesh = Entry->MeshOwner->GetWorkingMesh();
			if (!IsValid(Mesh) || !WorkingMesh.Mesh.IsValid())
			{
				OutErrorText = FText::Format(
					LOCTEXT("DynamicAcceptSourceTopologyInvalidMeshFormat", "'{0}': Static Mesh or its working topology could not be resolved."),
					FText::FromString(Entry->AssetName));
				return false;
			}

			// AUDITED: composes independently per component, via the SAME orchestrator call the live
			// Dynamic preview branch (ApplyPreviewToEntry) already uses -- WorkingMesh, the single
			// caller-supplied DynamicLayerStack, and this component's own GetBaselineColors(). Requires
			// agreement across every component of this entry, mirroring
			// VertexMaskForgeAcceptTargetBuilder::BuildSourceTopologyAcceptTargets' own divergent-baseline
			// rule exactly.
			TArray<FColor> ReferenceColors;
			bool bHaveReference = false;
			for (const TUniquePtr<FVertexMaskForgeWorkingStateOwner>& StateOwner : Entry->PreviewComponents)
			{
				const FVertexMaskForgePreviewComponentState& State = StateOwner->GetPreviewState();
				const UStaticMeshComponent* SourceComponent = State.GetSourceComponent().Get();
				if (!IsValid(SourceComponent))
				{
					continue;
				}

				// AUDITED: a component with no captured Baseline yet contributes nothing -- mirrors the
				// Legacy builder's own "nothing composed yet for this component" skip, adapted to the
				// input this orchestrator actually needs (Baseline, never WorkingColors).
				if (!StateOwner->AreColorsInitialized() || StateOwner->GetBaselineColors().IsEmpty())
				{
					continue;
				}

				TArray<FColor> ComponentColors;
				// M16-K.6D-8G-B: the real transform of THIS component (SourceComponent, resolved above) --
				// never Identity. Unused by any generator dispatched this checkpoint; see the orchestrator's
				// own header doc comment.
				const bool bComposed = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(
					WorkingMesh, DynamicLayerStack, StateOwner->GetBaselineColors(), SourceComponent->GetComponentTransform(), ComponentColors);
				if (!bComposed)
				{
					OutErrorText = FText::Format(
						LOCTEXT("DynamicAcceptOrchestratorFailedFormat",
							"'{0}': the Dynamic layer stack could not be composed for one or more components (an enabled layer's mask is unsupported or failed to generate, or the Baseline no longer matches the current topology)."),
						FText::FromString(Entry->AssetName));
					return false;
				}

				if (!bHaveReference)
				{
					ReferenceColors = MoveTemp(ComponentColors);
					bHaveReference = true;
				}
				else if (ComponentColors != ReferenceColors)
				{
					OutErrorText = FText::Format(
						LOCTEXT("DynamicAcceptDivergentBaselineFormat",
							"'{0}': different instances of this asset produced different Dynamic results (their captured Baselines differ), so writing to the shared Static Mesh asset would be ambiguous. Accept is blocked for this operation; make the instances' Baselines consistent, or Cancel."),
						FText::FromString(Entry->AssetName));
					return false;
				}
			}

			if (!bHaveReference)
			{
				// Every tracked component became invalid, or none had a captured Baseline yet -- nothing
				// to accept for this entry; not an error, just nothing eligible here.
				continue;
			}

			// AUDITED: same correspondence validation the Legacy Source-Topology builder already performs
			// -- reused verbatim, never reimplemented -- so this function's own output is already safe for
			// a future writer to consume.
			const int32 NumCorners = WorkingMesh.Mesh->TriangleCount() * 3;
			const FMeshDescription* LiveMeshDescription = Mesh->GetMeshDescription(0);
			if (!LiveMeshDescription || ReferenceColors.Num() != NumCorners)
			{
				OutErrorText = FText::Format(
					LOCTEXT("DynamicAcceptCorrespondenceFormat",
						"'{0}': the triangle/corner correspondence needed to write vertex colors is unavailable or stale. Try Refresh Selection again."),
					FText::FromString(Entry->AssetName));
				return false;
			}
			if (!VertexMaskForgeAcceptTargetBuilder::ValidateSourceTopologyCorrespondence(
				*WorkingMesh.Mesh, WorkingMesh.TriIDMap, *LiveMeshDescription, Entry->AssetName, OutErrorText))
			{
				return false;
			}

			VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget Target;
			Target.Mesh = Mesh;
			Target.AssetName = Entry->AssetName;
			Target.FinalColors = MoveTemp(ReferenceColors);
			Target.Entry = Entry;
			LocalTargets.Add(MoveTemp(Target));
		}

		OutTargets = MoveTemp(LocalTargets);
		return true;
	}
}

#undef LOCTEXT_NAMESPACE
