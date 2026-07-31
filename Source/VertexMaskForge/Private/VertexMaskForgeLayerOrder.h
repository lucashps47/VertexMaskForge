#pragma once

#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "CoreMinimal.h"
#include "VertexMaskForgeMaskTypes.h"

/**
 * M16-K.1: pure, stateless value-domain for representing and manipulating the persistent order of the
 * seven generator layers -- Bounding Box, Ambient Occlusion, Curvature, Noise, Material Slot,
 * Directional Normal, Thickness. Identity is EVertexMaskForgeScalarMaskSource itself (see
 * IsGeneratorLayer below for the exact 7-value allowlist) -- no new enum is introduced here (M16-K.0's
 * own audited decision: the panel's existing FVertexMaskForgeMaskLayerParams already carries this type
 * via Mask->Source, and identity duplication was explicitly rejected).
 *
 * This module knows nothing about SVertexMaskForgePanel, Slate, Working Mesh, GeneratorState, caches,
 * InstanceResults, Recipe/M16-H/M16-I, WorkingColorPublicationBinding, or the composition path
 * (ComposeGeneratorLayersSequential/EvaluateFillLayers) -- it operates exclusively on a small
 * TArray<EVertexMaskForgeScalarMaskSource>, exactly mirroring VertexMaskForgeSequentialEvaluator's own
 * "pure math, no side effects" shape. Nothing in this checkpoint is wired into any real composition
 * path or into SVertexMaskForgePanel -- both Sort()-by-Mask->Source call sites remain untouched; that
 * connection is explicitly deferred to M16-K.2.
 */
namespace VertexMaskForgeLayerOrder
{
	/**
	 * Returns a NEW, valid ordering of the seven generator layers, explicitly written out in the
	 * approved default sequence (M16-K.0's own audited decision) -- never derived from the enum's own
	 * numeric value, declaration order, range, Max/Count, cast, sort, or reflection, so this stays
	 * stable even if EVertexMaskForgeScalarMaskSource's own declaration order changes in the future:
	 *   BoundingBox, AmbientOcclusion, Curvature, Noise, MaterialSlot, DirectionalNormal, Thickness.
	 * Always a fresh TArray by value -- never a reference into any static/global storage.
	 */
	TArray<EVertexMaskForgeScalarMaskSource> MakeDefault();

	/**
	 * True iff Source is one of the seven generator layer identities -- BoundingBox, AmbientOcclusion,
	 * Curvature, Noise, MaterialSlot, DirectionalNormal, Thickness. ConstantWhite/ConstantBlack (Fill
	 * overrides, not generators) and any other value (including an out-of-range cast) return false.
	 */
	bool IsGeneratorLayer(EVertexMaskForgeScalarMaskSource Source);

	/**
	 * True iff Order is a complete, exact permutation of the seven generator layer identities: exactly
	 * 7 elements, every element IsGeneratorLayer()==true, no duplicates, no missing generator, no
	 * ConstantWhite/ConstantBlack, no unknown/cast-invalid value. Pure observation -- never mutates,
	 * normalizes, reorders, inserts into, or removes anything from Order.
	 */
	bool IsValid(TConstArrayView<EVertexMaskForgeScalarMaskSource> Order);

	/**
	 * Moves Source one position earlier in Order (an adjacent swap with its immediate predecessor).
	 * Returns true and performs exactly that one swap iff: Order (the input) IsValid(); Source
	 * IsGeneratorLayer(); Source is present in Order; Source is not already at index 0. Returns false
	 * and leaves Order completely unmodified (logically identical to the input) for every other case --
	 * never a partial mutation. The neighbor is resolved from Order's own current array position, never
	 * from enum arithmetic.
	 */
	bool MoveUp(TArray<EVertexMaskForgeScalarMaskSource>& Order, EVertexMaskForgeScalarMaskSource Source);

	/**
	 * Sibling of MoveUp -- moves Source one position later in Order (an adjacent swap with its
	 * immediate successor). Same success/failure contract as MoveUp, mirrored: fails (Order unmodified)
	 * if Order is invalid, Source is not a generator layer, Source is absent, or Source is already at
	 * the last index.
	 */
	bool MoveDown(TArray<EVertexMaskForgeScalarMaskSource>& Order, EVertexMaskForgeScalarMaskSource Source);
}
