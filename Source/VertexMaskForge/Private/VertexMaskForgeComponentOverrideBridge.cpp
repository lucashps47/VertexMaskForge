#include "VertexMaskForgeComponentOverrideBridge.h"

#include "ComponentReregisterContext.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "MeshPaintModeHelpers.h"
#include "RenderingThread.h"
#include "Rendering/ColorVertexBuffer.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshResources.h"
#include "VertexMaskForgeWorkingMeshTypes.h"
#include "VertexMaskForgeWorkingStateOwner.h"

#define LOCTEXT_NAMESPACE "SVertexMaskForgePanel"

namespace VertexMaskForgeComponentOverrideBridge
{
	bool BuildTransferTargets(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		TArray<FTransferTarget>& OutTargets,
		TArray<FText>& OutIneligibleReasons,
		FText& OutErrorText)
	{
		OutTargets.Reset();
		OutIneligibleReasons.Reset();

		for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
		{
			if (!Entry.IsValid() || Entry->PreviewComponents.IsEmpty())
			{
				continue;
			}

			UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
			if (!IsValid(Mesh) || !Mesh->HasValidRenderData(/*bCheckLODForVerts=*/true, /*LODIndex=*/0))
			{
				OutIneligibleReasons.Add(FText::Format(
					LOCTEXT("BridgeInvalidMeshFormat", "'{0}': Static Mesh could not be resolved or has no valid LOD 0 render data. Skipped."),
					FText::FromString(Entry->AssetName)));
				continue;
			}

			const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
			const FStaticMeshLODResources& LOD0 = RenderData->LODResources[0];
			const FColorVertexBuffer& AcceptedColorBuffer = LOD0.VertexBuffers.ColorVertexBuffer;

			// AUDITED (color-domain correctness, M20-E): this is the whole point of the restored
			// bridge -- read the ALREADY-ACCEPTED, already-rebuilt asset ColorVertexBuffer verbatim.
			// A zero-vertex buffer means the asset has never gone through a Forge Accept (or any other
			// vertex-color bake) -- there is nothing accepted to transfer yet.
			if (AcceptedColorBuffer.GetNumVertices() == 0)
			{
				OutIneligibleReasons.Add(FText::Format(
					LOCTEXT("BridgeNoAcceptedColorsFormat",
						"'{0}': the Static Mesh asset has no baked-in vertex colors yet. Click Accept first, then Send to Mesh Paint Texture."),
					FText::FromString(Entry->AssetName)));
				continue;
			}

			TArray<FColor> AcceptedColors;
			AcceptedColors.Reserve(static_cast<int32>(AcceptedColorBuffer.GetNumVertices()));
			for (uint32 VertexIndex = 0; VertexIndex < AcceptedColorBuffer.GetNumVertices(); ++VertexIndex)
			{
				AcceptedColors.Add(AcceptedColorBuffer.VertexColor(VertexIndex));
			}

			for (const TUniquePtr<FVertexMaskForgeWorkingStateOwner>& StateOwner : Entry->PreviewComponents)
			{
				UStaticMeshComponent* SourceComponent = StateOwner->GetPreviewState().GetSourceComponent().Get();
				if (!IsValid(SourceComponent))
				{
					continue;
				}

				if (!SourceComponent->CanMeshPaintTextureColors())
				{
					OutIneligibleReasons.Add(FText::Format(
						LOCTEXT("BridgeCannotMeshPaintTextureFormat",
							"'{0}' ({1}): Mesh Paint Texture support is not enabled for this component/asset (bSupportMeshPainting, bEnableTextureColorMeshPainting, Static Mesh Paint Support, or platform Virtual Texture support). Skipped."),
						FText::FromString(Entry->AssetName), FText::FromString(SourceComponent->GetName())));
					continue;
				}

				FTransferTarget Target;
				Target.Component = SourceComponent;
				Target.AssetName = Entry->AssetName;
				Target.AcceptedColors = AcceptedColors;
				OutTargets.Add(MoveTemp(Target));
			}
		}

		return true;
	}

	namespace
	{
		/** Captures exactly the pieces of LOD 0 override state this bridge must restore byte-for-byte. */
		struct FCapturedOverrideState
		{
			bool bHadOverride = false;
			TArray<FColor> OriginalColors;
			TArray<FPaintedVertex> OriginalPaintedVertices;
		};

		FCapturedOverrideState CaptureOverrideState(const UStaticMeshComponent& Component)
		{
			FCapturedOverrideState State;
			if (!Component.LODData.IsValidIndex(0))
			{
				return State;
			}

			const FStaticMeshComponentLODInfo& LODInfo = Component.LODData[0];
			State.OriginalPaintedVertices = LODInfo.PaintedVertices;

			if (LODInfo.OverrideVertexColors != nullptr && LODInfo.OverrideVertexColors->GetNumVertices() > 0)
			{
				State.bHadOverride = true;
				State.OriginalColors.Reserve(static_cast<int32>(LODInfo.OverrideVertexColors->GetNumVertices()));
				for (uint32 VertexIndex = 0; VertexIndex < LODInfo.OverrideVertexColors->GetNumVertices(); ++VertexIndex)
				{
					State.OriginalColors.Add(LODInfo.OverrideVertexColors->VertexColor(VertexIndex));
				}
			}

			return State;
		}

		/** Installs Colors as a temporary LOD 0 OverrideVertexColors buffer, releasing whatever was
		 *  there before. CPU-only (see this module's header comment on TransferToMeshPaintTexture for
		 *  why no BeginInitResource call is needed here). */
		void InstallTemporaryOverride(UStaticMeshComponent& Component, const TArray<FColor>& Colors)
		{
			const int32 NumLODs = FMath::Max(Component.GetStaticMesh()->GetNumLODs(), 1);
			Component.SetLODDataCount(NumLODs, NumLODs);
			FStaticMeshComponentLODInfo& LODInfo = Component.LODData[0];

			if (LODInfo.OverrideVertexColors != nullptr)
			{
				LODInfo.ReleaseOverrideVertexColorsAndBlock();
			}

			LODInfo.OverrideVertexColors = new FColorVertexBuffer;
			LODInfo.OverrideVertexColors->InitFromColorArray(Colors);
			LODInfo.PaintedVertices.Empty();
		}

		/** Restores exactly the state CaptureOverrideState observed -- absence, or byte-for-byte
		 *  presence plus PaintedVertices -- regardless of what InstallTemporaryOverride did in between. */
		void RestoreOverrideState(UStaticMeshComponent& Component, const FCapturedOverrideState& State)
		{
			if (!Component.LODData.IsValidIndex(0))
			{
				return;
			}

			FStaticMeshComponentLODInfo& LODInfo = Component.LODData[0];
			if (LODInfo.OverrideVertexColors != nullptr)
			{
				LODInfo.ReleaseOverrideVertexColorsAndBlock();
			}

			if (State.bHadOverride)
			{
				LODInfo.OverrideVertexColors = new FColorVertexBuffer;
				LODInfo.OverrideVertexColors->InitFromColorArray(State.OriginalColors);
				BeginInitResource(LODInfo.OverrideVertexColors);
			}

			LODInfo.PaintedVertices = State.OriginalPaintedVertices;
			Component.MarkRenderStateDirty();
		}

		/**
		 * ImportMeshPaintTextureFromVertexColors returns void -- there is no direct success/failure
		 * signal. Grounded directly against the installed implementation (MeshPaintModeHelpers.cpp):
		 * the ONLY early-return points are a failed Cast<UStaticMeshComponent> and a null StaticMesh,
		 * neither of which is possible here (this bridge only ever passes an already-validated
		 * UStaticMeshComponent with an already-valid Static Mesh); every other path through the
		 * function ends by calling Component->SetMeshPaintTexture(NewTexture) with a freshly
		 * NewObject'd, non-null texture. Comparing the component's own GetMeshPaintTexture() pointer
		 * before and after the call is therefore a faithful, engine-grounded success signal: unchanged
		 * (still the same pointer, including still-null) means the function returned early without
		 * doing the bake.
		 */
		bool DidImportMeshPaintTextureSucceed(UStaticMeshComponent& Component, UTexture* PreviousMeshPaintTexture)
		{
			return Component.GetMeshPaintTexture() != nullptr && Component.GetMeshPaintTexture() != PreviousMeshPaintTexture;
		}
	}

	bool TransferToMeshPaintTexture(
		const TArray<FTransferTarget>& Targets,
		TArray<UStaticMeshComponent*>& OutSucceededComponents,
		FText& OutErrorText,
		TFunction<bool(UStaticMeshComponent&)> InNativeImportOverride)
	{
		OutSucceededComponents.Reset();

		const bool bUseOverride = static_cast<bool>(InNativeImportOverride);

		UMeshPaintModeSubsystem* MeshPaintSubsystem = nullptr;
		if (!bUseOverride)
		{
			if (!GEditor)
			{
				OutErrorText = LOCTEXT("BridgeNoEditor", "Send to Mesh Paint Texture requires the Editor (GEditor); this seam is Editor-only.");
				return false;
			}

			MeshPaintSubsystem = GEditor->GetEditorSubsystem<UMeshPaintModeSubsystem>();
			if (MeshPaintSubsystem == nullptr)
			{
				OutErrorText = LOCTEXT("BridgeNoSubsystem", "UMeshPaintModeSubsystem is unavailable; cannot invoke the native From Vertex import.");
				return false;
			}
		}

		bool bAllSucceeded = true;
		for (const FTransferTarget& Target : Targets)
		{
			UStaticMeshComponent* Component = Target.Component.Get();
			if (!IsValid(Component))
			{
				OutErrorText = FText::Format(
					LOCTEXT("BridgeComponentGoneFormat", "'{0}': the target component no longer exists."),
					FText::FromString(Target.AssetName));
				bAllSucceeded = false;
				continue;
			}

			// AUDITED (transient-only, Phase 6): every mutation below is undone before this iteration
			// ends, success or failure -- capture happens BEFORE any mutation, restore happens in every
			// exit path (including the two continue-on-failure paths above/below).
			const FCapturedOverrideState CapturedState = CaptureOverrideState(*Component);
			UTexture* PreviousMeshPaintTexture = Component->GetMeshPaintTexture();

			Component->SetFlags(RF_Transactional);
			Component->Modify();

			const bool bWasRegistered = Component->IsRegistered();
			FComponentReregisterContext ReregisterContext(Component);
			if (bWasRegistered)
			{
				FlushRenderingCommands();
			}

			InstallTemporaryOverride(*Component, Target.AcceptedColors);

			bool bThisSucceeded = false;
			if (bUseOverride)
			{
				// Test-only seam: exercises the exact production capture/install/restore path above and
				// below, without invoking the real GPU-touching native bake.
				bThisSucceeded = InNativeImportOverride(*Component);
			}
			else
			{
				MeshPaintSubsystem->ImportMeshPaintTextureFromVertexColors(Component);
				bThisSucceeded = DidImportMeshPaintTextureSucceed(*Component, PreviousMeshPaintTexture);
			}

			// Restoration is unconditional -- runs whether the import above succeeded or not. The
			// ReregisterContext above only re-registers the component once it goes out of scope at the
			// end of this iteration, i.e. AFTER RestoreOverrideState below has already run, so nothing
			// ever renders with the temporary buffer.
			RestoreOverrideState(*Component, CapturedState);

			if (!bThisSucceeded)
			{
				bAllSucceeded = false;
				OutErrorText = FText::Format(
					LOCTEXT("BridgeImportFailedFormat", "'{0}' ({1}): the native From Vertex import did not produce a Mesh Paint Texture."),
					FText::FromString(Target.AssetName), FText::FromString(Component->GetName()));
				continue;
			}

			OutSucceededComponents.Add(Component);
		}

		return bAllSucceeded;
	}
}

#undef LOCTEXT_NAMESPACE
