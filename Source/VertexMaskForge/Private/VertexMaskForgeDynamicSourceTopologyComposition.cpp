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
#include "VertexMaskForgeCurvatureGenerator.h"
#include "VertexMaskForgeDirectionalNormalGenerator.h"
#include "VertexMaskForgeDynamicLayerEvaluator.h"
#include "VertexMaskForgeDynamicLayerStack.h"
#include "VertexMaskForgeLayerTypes.h"
#include "VertexMaskForgeMaskTypes.h"
#include "VertexMaskForgeMaterialSlotGenerator.h"
#include "VertexMaskForgeNoiseGenerator.h"
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
	const FTransform& ComponentTransform,
	TArray<FColor>& OutComposedColors)
{
	// M16-K.6D-8G-B: ComponentTransform is intentionally unused this checkpoint -- see this function's
	// own header doc comment. Referencing it here only to keep -Wunused-parameter silent would invent
	// behavior; the parameter is simply threaded through, unread, until M16-K.6D-8G-C's Ambient Occlusion
	// dispatch branch consumes it.
	(void)ComponentTransform;

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

	// M16-K.6D-8E-B: ONE function-local FVertexMaskForgeGeneratorState, declared here (outside the
	// per-layer loop below) and reused for every enabled Curvature layer THIS call evaluates -- never
	// per-layer, never keyed by LayerId, never the Legacy per-entry FVertexMaskForgeGeneratorState (never
	// reachable from this orchestrator), never static/persistent, never exposed through this function's
	// own public signature, and never retained beyond this single call (destroyed when this function
	// returns). GenerateCurvatureMaskFromDynamicMesh's own EnsureCurvatureRawCache internally gates its
	// expensive raw Convex/Concave computation on this state's own CurvatureCacheFingerprint versus
	// WorkingMesh.GeometryFingerprint -- since that fingerprint never changes within one call, the FIRST
	// enabled Curvature layer computes the raw arrays and every SUBSEQUENT enabled Curvature layer in the
	// same call reuses them for free, while each layer still applies its own Type/Multiplier/Blur/Levels/
	// Invert independently (see the Curvature branch below). This provides WITHIN-CALL sharing only --
	// a later, separate orchestrator invocation (e.g. a subsequent Auto Update Preview recomposition)
	// starts with a fresh, empty CurvatureLocalGeneratorState and recomputes the raw arrays again, even
	// if geometry has not changed; persistent cross-call caching is explicitly deferred, not implemented
	// here.
	FVertexMaskForgeGeneratorState CurvatureLocalGeneratorState;

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
		else if (Layer.Mask->GeneratorType == EVertexMaskForgeGeneratorType::Curvature)
		{
			const FVertexMaskForgeCurvatureParams* CurvatureParams = Layer.Mask->Params.TryGet<FVertexMaskForgeCurvatureParams>();
			if (!CurvatureParams)
			{
				// Defensive -- mirrors the Material Slot/Bounding Box/Directional Normal coherence checks
				// above.
				return false;
			}

			// M16-K.6D-8E-B scope: Curvature has no Local/World-space concept and no ComponentTransform
			// dependency at all (confirmed directly from GenerateCurvatureMaskFromDynamicMesh's own
			// signature) -- unlike Bounding Box/Directional Normal, there is no space-rejection check
			// here. Every authoritative field (Type/Multiplier/Blur/LevelsMin/LevelsMax/bInvert) is
			// forwarded unchanged; none are normalized, swapped, or rewritten here -- the generator's own
			// ApplyCurvatureArtisticParams already clamps/defends each field internally (e.g.
			// LevelsMin<=LevelsMax is never enforced by this orchestrator, since ApplyCurvatureLevels'
			// own Max(LevelsMax-LevelsMin, Epsilon) denominator already makes any stored relationship
			// well-defined).
			//
			// CurvatureLocalGeneratorState (declared once, above Pass 1) is reused here across every
			// enabled Curvature layer in THIS call -- see its own declaration comment for the full
			// within-call-sharing/no-cross-call-persistence contract.
			//
			// GenerateCurvatureMaskFromDynamicMesh returns its FVertexMaskForgeScalarMask entirely BY
			// VALUE (its own Values/bHasValue arrays are freshly allocated inside that function every
			// call, from ApplyCurvatureArtisticParams' own freshly-returned-by-value TArray<float> --
			// neither is ever aliased to CurvatureLocalGeneratorState's cached raw arrays) -- so the
			// MoveTemp below is always safe: a later Curvature layer's call can never retroactively
			// mutate an earlier layer's already-stored completed mask.
			FVertexMaskForgeScalarMask GeneratedMask = VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
				WorkingMesh, CurvatureLocalGeneratorState, CurvatureParams->Type, CurvatureParams->Multiplier,
				CurvatureParams->Blur, CurvatureParams->LevelsMin, CurvatureParams->LevelsMax, CurvatureParams->bInvert);
			if (GeneratedMask.State != EVertexMaskForgeScalarMaskState::Ready)
			{
				// Mirrors Bounding Box's own check exactly -- Curvature's output is DynamicMeshVertex-
				// domain and sparse-safe (resolved only via TryGetValue in Pass 2 below), so no separate
				// cardinality check is required here, matching
				// GenerateBoundingBoxMaskFromDynamicMesh's own established precedent.
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
		else if (Layer.Mask->GeneratorType == EVertexMaskForgeGeneratorType::Noise)
		{
			const FVertexMaskForgeNoiseParams* NoiseParams = Layer.Mask->Params.TryGet<FVertexMaskForgeNoiseParams>();
			if (!NoiseParams)
			{
				// Defensive -- mirrors the Material Slot/Bounding Box/Directional Normal/Curvature
				// coherence checks above.
				return false;
			}

			// M16-K.6D-8F-B scope: Noise has no Local/World-space concept and no ComponentTransform
			// dependency at all (confirmed directly from GenerateNoiseMaskFromDynamicMesh's own signature)
			// -- no space-rejection check here, mirroring Curvature's own precedent. Every authoritative
			// field is forwarded unchanged; none are normalized, swapped, or rewritten here.
			//
			// UNLIKE Curvature's single shared CurvatureLocalGeneratorState (reused across every enabled
			// Curvature layer in one call, safe because Curvature's raw-cache key is geometry-only), Noise's
			// own raw-cache key additionally includes its full FVertexMaskForgeNoiseGenerativeParams (see
			// that type's own operator==) -- two Noise layers commonly differ in Type/Scale/Offset/Seed/
			// Octaves/Roughness/Lacunarity/TurbulenceStrength/Blur, so sharing one state across them would
			// merely thrash (each layer's own call would invalidate the previous layer's cached raw pattern
			// via that struct's own field-by-field comparison) rather than ever hit a genuine cache reuse.
			// Accordingly, this branch constructs a NEW, INDEPENDENT, function-local
			// FVertexMaskForgeGeneratorState for THIS Noise layer only -- never shared with another Noise
			// layer, never the Legacy per-entry FVertexMaskForgeGeneratorState (never reachable from this
			// orchestrator), never static/persistent, never exposed through this function's own public
			// signature, and never retained beyond this single layer's evaluation (destroyed at the end of
			// this loop iteration). This is a deliberate design difference from Curvature, not a claim that
			// Noise is cheaper or more/less expensive to evaluate -- no comparative performance measurement
			// was made.
			FVertexMaskForgeGeneratorState NoiseLocalGeneratorState;

			FVertexMaskForgeNoiseGenerativeParams GenerativeParams;
			GenerativeParams.NoiseType = NoiseParams->Type;
			GenerativeParams.ScaleX = NoiseParams->ScaleX;
			GenerativeParams.ScaleY = NoiseParams->ScaleY;
			GenerativeParams.ScaleZ = NoiseParams->ScaleZ;
			GenerativeParams.OffsetX = NoiseParams->OffsetX;
			GenerativeParams.OffsetY = NoiseParams->OffsetY;
			GenerativeParams.OffsetZ = NoiseParams->OffsetZ;
			GenerativeParams.Seed = NoiseParams->Seed;
			GenerativeParams.Octaves = NoiseParams->Octaves;
			GenerativeParams.Roughness = NoiseParams->Roughness;
			GenerativeParams.Lacunarity = NoiseParams->Lacunarity;
			GenerativeParams.TurbulenceStrength = NoiseParams->TurbulenceStrength;
			GenerativeParams.Blur = NoiseParams->Blur;

			// GenerateNoiseMaskFromDynamicMesh returns its FVertexMaskForgeScalarMask entirely BY VALUE
			// (freshly allocated inside that function every call, from ApplyNoiseArtisticParams' own
			// freshly-returned-by-value TArray<float> -- never aliased to NoiseLocalGeneratorState's own
			// cached raw array) -- so the MoveTemp below is always safe, exactly like Curvature's own
			// established precedent.
			FVertexMaskForgeScalarMask GeneratedMask = VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
				WorkingMesh, NoiseLocalGeneratorState, GenerativeParams, NoiseParams->Multiplier,
				NoiseParams->LevelsMin, NoiseParams->LevelsMax, NoiseParams->bInvert);
			if (GeneratedMask.State != EVertexMaskForgeScalarMaskState::Ready)
			{
				// Mirrors Curvature's/Bounding Box's own check exactly -- Noise's output is
				// DynamicMeshVertex-domain and sparse-safe (resolved only via TryGetValue in Pass 2 below),
				// so no separate cardinality check is required here.
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
		else
		{
			// Explicit, whole-call failure -- any generator type beyond this checkpoint's own supported
			// set (Material Slot, Bounding Box Local-space, Directional Normal Local-space, Curvature,
			// Noise) never silently skips or treats itself as Fill-only.
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
