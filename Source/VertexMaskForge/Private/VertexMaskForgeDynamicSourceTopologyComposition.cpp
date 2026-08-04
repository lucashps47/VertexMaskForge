// M16-K.6D-4: see VertexMaskForgeDynamicSourceTopologyComposition.h for the full module contract.
//
// ISOLATION RATIONALE for bypassing VertexMaskForgeDynamicLayerEvaluator::EvaluateColor's masked overload
// and VertexMaskForgeDynamicLayerBatchCompositor::ComposeColors: both require a
// FVertexMaskForgeInstanceResultStore parameter to resolve a masked layer's EffectiveMask by
// MaskInstanceId -- this checkpoint's own instructions forbid this orchestrator from receiving,
// constructing, or touching a FVertexMaskForgeInstanceResultStore of any kind (not even a local,
// function-scoped, never-escaping one), because that type's entire purpose (identity-keyed, mutation-
// tracked, cross-call persistence) is foreign to a purely local, caller-owned, single-call computation --
// introducing one here, even transiently, would blur the exact boundary this checkpoint exists to keep
// sharp. Instead, this module resolves each masked layer's EffectiveMask directly from the freshly
// generated FVertexMaskForgeScalarMask array Pass 1 below produces (positional lookup by corner index,
// never a MaskInstanceId/FGuid lookup of any kind), and reuses the exact same downstream blend-mode fold
// primitives EvaluateColor itself calls (VertexMaskForgeSequentialEvaluator::EvaluateFillLayerStep and
// VertexMaskForgeDynamicLayerEvaluator::TryResolveFillValue) so there is exactly one implementation of
// the actual composition math anywhere in the plugin -- only the "how is EffectiveMask resolved" step
// differs, by necessity.

#include "VertexMaskForgeDynamicSourceTopologyComposition.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "VertexMaskForgeBoundingBoxGenerator.h"
#include "VertexMaskForgeColorConversion.h"
#include "VertexMaskForgeDirectionalNormalGenerator.h"
#include "VertexMaskForgeDynamicLayerEvaluator.h"
#include "VertexMaskForgeDynamicLayerStack.h"
#include "VertexMaskForgeLayerTypes.h"
#include "VertexMaskForgeMaskTypes.h"
#include "VertexMaskForgeMaterialSlotGenerator.h"
#include "VertexMaskForgeRecipeTypes.h"
#include "VertexMaskForgeSequentialEvaluator.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	// M16-K.6D-8B: this orchestrator's masked layers are no longer all corner-domain -- Material Slot's
	// own generator already produces one value per Source-Topology corner (see
	// GenerateMaterialSlotMaskFromDynamicMesh), but Bounding Box's Source-Topology generator
	// (GenerateBoundingBoxMaskFromDynamicMesh) is indexed by Dynamic Mesh VertexID instead (sparse-safe,
	// TryGetValue-only -- see that function's own "INDEX SAFETY" doc note). Domain is tracked explicitly,
	// per generated layer mask, never inferred from Values.Num() (which could coincidentally equal the
	// corner count for a mesh where VertexCount()*something == TriangleCount()*3) -- this mirrors the
	// same per-generator domain distinction Legacy's own UpdateWorkingColorsSourceTopology already makes
	// (its IndexOverride switch: Material Slot by CornerIndex, Bounding Box/Curvature/Noise by Dynamic
	// Mesh VertexID via Mesh.GetTriangle(TriangleID)[Corner]) -- reimplemented here structurally
	// isolated from that Legacy code (no Legacy composition function is ever called), never copied from
	// it.
	enum class ELayerMaskDomain : uint8
	{
		Corner,
		DynamicMeshVertex,
	};

	struct FLayerGeneratedMask
	{
		FVertexMaskForgeScalarMask Mask;
		ELayerMaskDomain Domain = ELayerMaskDomain::Corner;
	};
}

bool VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(
	const FVertexMaskForgeWorkingMesh& WorkingMesh,
	const FVertexMaskForgeDynamicLayerStack& Stack,
	TConstArrayView<FColor> BaseColors,
	TArray<FColor>& OutComposedColors)
{
	if (!WorkingMesh.Mesh.IsValid())
	{
		return false;
	}

	const int32 ExpectedCornerCount = WorkingMesh.Mesh->TriangleCount() * 3;
	if (BaseColors.Num() != ExpectedCornerCount)
	{
		return false;
	}

	const TArray<FVertexMaskForgeLayer>& Layers = Stack.GetLayers();

	// --- Pass 1: resolve every ENABLED masked layer's scalar mask up front, all-or-nothing -- a
	// structural failure (unsupported generator type, or the generator itself not Ready) fails the WHOLE
	// call before any per-corner work begins; nothing is ever partially composed. ---
	TArray<FLayerGeneratedMask> LayerMasks;
	LayerMasks.SetNum(Layers.Num());

	// Lazily built (only if at least one enabled layer actually requests Bounding Box) -- maps this
	// call's own corner-domain index to the underlying Dynamic Mesh VertexID at that corner, walking
	// WorkingMesh.Mesh->TriangleIndicesItr() in order (NEVER assuming TriangleID is dense/contiguous) so
	// "CornerIndex N" means the identical physical corner GenerateMaterialSlotMaskFromDynamicMesh's own
	// CornerIndex assignment already uses (see that function's own TriangleIndicesItr()-driven loop) --
	// both domains agree on what a given CornerIndex physically is.
	TArray<int32> CornerToVertexID;
	bool bCornerToVertexIDBuilt = false;

	for (int32 LayerIndex = 0; LayerIndex < Layers.Num(); ++LayerIndex)
	{
		const FVertexMaskForgeLayer& Layer = Layers[LayerIndex];
		if (!Layer.bEnabled || !Layer.Mask.IsSet())
		{
			continue;
		}

		if (Layer.Mask->GeneratorType == EVertexMaskForgeGeneratorType::MaterialSlot)
		{
			const FVertexMaskForgeMaterialSlotParams* SlotParams = Layer.Mask->Params.TryGet<FVertexMaskForgeMaterialSlotParams>();
			if (!SlotParams)
			{
				// Defensive -- should be unreachable given FVertexMaskForgeDynamicLayerStack's own
				// GeneratorType/Params coherence invariant, but never assumed (mirrors every other
				// Material Slot caller's own defensive check in this codebase).
				return false;
			}

			FVertexMaskForgeScalarMask GeneratedMask = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(
				WorkingMesh, SlotParams->SelectedSlotIndex, SlotParams->bInvert);
			if (GeneratedMask.State != EVertexMaskForgeScalarMaskState::Ready || GeneratedMask.Values.Num() != ExpectedCornerCount)
			{
				return false;
			}

			LayerMasks[LayerIndex].Mask = MoveTemp(GeneratedMask);
			LayerMasks[LayerIndex].Domain = ELayerMaskDomain::Corner;
		}
		else if (Layer.Mask->GeneratorType == EVertexMaskForgeGeneratorType::BoundingBox)
		{
			const FVertexMaskForgeBoundingBoxParams* BBoxParams = Layer.Mask->Params.TryGet<FVertexMaskForgeBoundingBoxParams>();
			if (!BBoxParams)
			{
				// Defensive -- mirrors the Material Slot coherence check above.
				return false;
			}

			// M16-K.6D-8B scope: Local-space, per-mesh bounds ONLY. World Space and Unified Bounds are
			// explicitly REJECTED here (whole-call failure), never silently reinterpreted as Local Space
			// or per-mesh bounds -- both require inputs (a component transform; full-selection context)
			// this orchestrator does not receive in this checkpoint. bUseUnifiedBounds is geometric/
			// generator state (NOT a composition concern like Blend/Opacity), checked unconditionally
			// regardless of which axes are enabled, since Unified Bounds changes what "the bounds" even
			// are before any per-axis evaluation begins.
			if (BBoxParams->bUseUnifiedBounds)
			{
				return false;
			}
			for (const FVertexMaskForgeAxisMaskParams& AxisParams : BBoxParams->Axes)
			{
				if (AxisParams.bEnabled && AxisParams.bWorldSpace)
				{
					return false;
				}
			}

			// Every enabled axis has now been confirmed Local-space -- ResolveAxisCoordinate's own
			// Local-space branch never reads ComponentTransform, so FTransform::Identity is safe here
			// ONLY because that request was just proven, never supplied as a guess/placeholder ahead of
			// validation.
			FVertexMaskForgeScalarMask GeneratedMask = VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh(
				*WorkingMesh.Mesh, BBoxParams->Axes, FTransform::Identity);
			if (GeneratedMask.State != EVertexMaskForgeScalarMaskState::Ready)
			{
				// Covers "no enabled axes" and "degenerate bounds" alike -- both leave the generator's
				// own State at something other than Ready (Unavailable / DegenerateBounds respectively),
				// exactly as GenerateBoundingBoxMaskFromDynamicMesh's own contract already establishes;
				// no separate check is invented here.
				return false;
			}

			LayerMasks[LayerIndex].Mask = MoveTemp(GeneratedMask);
			LayerMasks[LayerIndex].Domain = ELayerMaskDomain::DynamicMeshVertex;

			if (!bCornerToVertexIDBuilt)
			{
				CornerToVertexID.SetNumUninitialized(ExpectedCornerCount);
				int32 RunningCornerIndex = 0;
				for (const int32 TriangleID : WorkingMesh.Mesh->TriangleIndicesItr())
				{
					const UE::Geometry::FIndex3i Tri = WorkingMesh.Mesh->GetTriangle(TriangleID);
					CornerToVertexID[RunningCornerIndex++] = Tri.A;
					CornerToVertexID[RunningCornerIndex++] = Tri.B;
					CornerToVertexID[RunningCornerIndex++] = Tri.C;
				}
				bCornerToVertexIDBuilt = true;
			}
		}
		else if (Layer.Mask->GeneratorType == EVertexMaskForgeGeneratorType::DirectionalNormal)
		{
			const FVertexMaskForgeDirectionalNormalParams* NormalParams = Layer.Mask->Params.TryGet<FVertexMaskForgeDirectionalNormalParams>();
			if (!NormalParams)
			{
				// Defensive -- mirrors the Material Slot/Bounding Box coherence checks above.
				return false;
			}

			// M16-K.6D-8D-B scope: Local-space only. World Space is explicitly REJECTED here (whole-call
			// failure), never silently reinterpreted as Local -- World Space requires a per-component
			// transform this orchestrator does not receive in this checkpoint, the same class of gap
			// that made Bounding Box's own World Space/Unified Bounds explicitly rejected in M16-K.6D-8B.
			if (NormalParams->Space != EVertexMaskForgeNormalSpace::Local)
			{
				return false;
			}

			// Local-space evaluation never reads ComponentTransform (confirmed directly from
			// GenerateDirectionalNormalMaskFromDynamicMesh's own header doc comment and its Space==Local
			// branch), so FTransform::Identity is safe here ONLY because Local-space was just proven
			// above, never supplied as a guess ahead of validation.
			FVertexMaskForgeScalarMask GeneratedMask = VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMaskFromDynamicMesh(
				WorkingMesh, NormalParams->Space, NormalParams->Direction, NormalParams->Angle, NormalParams->Falloff,
				NormalParams->Blur, NormalParams->bInvert, FTransform::Identity);
			if (GeneratedMask.State != EVertexMaskForgeScalarMaskState::Ready
				|| GeneratedMask.Values.Num() != ExpectedCornerCount
				|| GeneratedMask.NumValidValues != ExpectedCornerCount)
			{
				// The third check additionally requires EVERY corner to have resolved a real value --
				// GenerateDirectionalNormalMaskFromDynamicMesh's own contract allows an individual corner
				// to be left unwritten (missing Normal Overlay element, degenerate normal) while still
				// reporting Ready overall (NumValidValues > 0 is its own success threshold). This
				// orchestrator's Corner-domain Pass 2 reads Values[CornerIndex] directly and
				// unconditionally (Material Slot's own established precedent, always dense) -- a hole
				// here would silently read a zero-initialized placeholder as a real computed value,
				// exactly the guess FVertexMaskForgeScalarMask's own contract forbids. Rejecting the
				// whole call on any hole is the safe choice; it introduces no new domain, no per-corner
				// TryGetValue path, and no composition-path change.
				return false;
			}

			LayerMasks[LayerIndex].Mask = MoveTemp(GeneratedMask);
			LayerMasks[LayerIndex].Domain = ELayerMaskDomain::Corner;
		}
		else
		{
			// Explicit, whole-call failure -- any generator type beyond this checkpoint's own supported
			// set (Material Slot, Bounding Box Local-space, Directional Normal Local-space) never
			// silently skips or treats itself as Fill-only.
			return false;
		}
	}

	// --- Pass 2: fold, per corner, strictly in Stack order, into a private local buffer -- OutComposedColors
	// is only ever touched by the final MoveTemp below, on success. ---
	TArray<FColor> LocalOutput;
	LocalOutput.SetNumUninitialized(ExpectedCornerCount);

	for (int32 CornerIndex = 0; CornerIndex < ExpectedCornerCount; ++CornerIndex)
	{
		const FVector4f BaseColor = VertexMaskForgeColorConversion::ToLinearColorF(BaseColors[CornerIndex]);
		FVector3f Composite(BaseColor.X, BaseColor.Y, BaseColor.Z);

		for (int32 LayerIndex = 0; LayerIndex < Layers.Num(); ++LayerIndex)
		{
			const FVertexMaskForgeLayer& Layer = Layers[LayerIndex];
			if (!Layer.bEnabled)
			{
				continue;
			}

			float FillValue = 0.0f;
			if (!VertexMaskForgeDynamicLayerEvaluator::TryResolveFillValue(Layer.Fill, FillValue))
			{
				continue;
			}

			// Layer.Mask unset -> EffectiveMask implicitly 1.0, exactly mirroring EvaluateColor's own
			// "ResultStore is NEVER consulted for such a layer" contract (here: neither generated mask
			// array is ever consulted either). Layer.Mask set -> Pass 1 already guaranteed a Ready,
			// domain-tagged LayerMasks[LayerIndex] for every ENABLED masked layer -- looked up according
			// to that mask's own recorded domain, never inferred from array length.
			float EffectiveMask = 1.0f;
			if (Layer.Mask.IsSet())
			{
				const FLayerGeneratedMask& GeneratedLayerMask = LayerMasks[LayerIndex];
				if (GeneratedLayerMask.Domain == ELayerMaskDomain::Corner)
				{
					EffectiveMask = GeneratedLayerMask.Mask.Values[CornerIndex];
				}
				else
				{
					// DynamicMeshVertex domain (Bounding Box) -- resolve this corner's own underlying
					// Dynamic Mesh VertexID, then read the mask ONLY through TryGetValue (never a direct
					// Values[VertexID] index), exactly per FVertexMaskForgeScalarMask's own sparse-domain
					// contract. A miss here means the corner's VertexID was never written by the
					// generator -- stale/invalid topology -- and fails the WHOLE call safely rather than
					// substituting a guessed value; OutComposedColors is still untouched on this path
					// (LocalOutput is never assigned to it before this point).
					const int32 VertexID = CornerToVertexID[CornerIndex];
					if (!GeneratedLayerMask.Mask.TryGetValue(VertexID, EffectiveMask))
					{
						return false;
					}
				}
			}

			const FVector3f PaintValue = FVector3f(FillValue, FillValue, FillValue) * EffectiveMask;
			const FVector3f LayerOutput = VertexMaskForgeSequentialEvaluator::EvaluateFillLayerStep(Composite, PaintValue, Layer.BlendMode, Layer.Opacity);

			Composite.X = Layer.bAffectRed ? LayerOutput.X : Composite.X;
			Composite.Y = Layer.bAffectGreen ? LayerOutput.Y : Composite.Y;
			Composite.Z = Layer.bAffectBlue ? LayerOutput.Z : Composite.Z;
		}

		Composite.X = FMath::Clamp(Composite.X, 0.0f, 1.0f);
		Composite.Y = FMath::Clamp(Composite.Y, 0.0f, 1.0f);
		Composite.Z = FMath::Clamp(Composite.Z, 0.0f, 1.0f);

		const FVector4f FinalColor(Composite.X, Composite.Y, Composite.Z, BaseColor.W);
		LocalOutput[CornerIndex] = VertexMaskForgeColorConversion::ToDisplayFColor(FinalColor);
	}

	OutComposedColors = MoveTemp(LocalOutput);
	return true;
}
