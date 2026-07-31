#include "VertexMaskForgeLayerOrder.h"

namespace VertexMaskForgeLayerOrder
{
	namespace
	{
		/**
		 * AUDITED (M16-K.1): maps a generator layer identity to a dense [0,6] slot index, used by
		 * IsValid's own duplicate/coverage bookkeeping below. Returns INDEX_NONE for ConstantWhite/
		 * ConstantBlack/any unknown value -- kept private to this .cpp (never part of the public API);
		 * intentionally a second, independent switch from IsGeneratorLayer's own rather than derived
		 * from it, so each function's exhaustiveness is checked by the compiler on its own terms.
		 */
		int32 GeneratorSlotIndex(const EVertexMaskForgeScalarMaskSource Source)
		{
			switch (Source)
			{
			case EVertexMaskForgeScalarMaskSource::BoundingBox: return 0;
			case EVertexMaskForgeScalarMaskSource::AmbientOcclusion: return 1;
			case EVertexMaskForgeScalarMaskSource::Curvature: return 2;
			case EVertexMaskForgeScalarMaskSource::Noise: return 3;
			case EVertexMaskForgeScalarMaskSource::MaterialSlot: return 4;
			case EVertexMaskForgeScalarMaskSource::DirectionalNormal: return 5;
			case EVertexMaskForgeScalarMaskSource::Thickness: return 6;
			default: return INDEX_NONE;
			}
		}
	}

	TArray<EVertexMaskForgeScalarMaskSource> MakeDefault()
	{
		// AUDITED (M16-K.1): explicitly written out, one literal per line -- never derived from the
		// enum's own numeric value/declaration order/range/Max/Count/cast/sort/reflection. This is the
		// approved default sequence (M16-K.0's own audited decision), independent of
		// EVertexMaskForgeScalarMaskSource's own future declaration order.
		return TArray<EVertexMaskForgeScalarMaskSource>{
			EVertexMaskForgeScalarMaskSource::BoundingBox,
			EVertexMaskForgeScalarMaskSource::AmbientOcclusion,
			EVertexMaskForgeScalarMaskSource::Curvature,
			EVertexMaskForgeScalarMaskSource::Noise,
			EVertexMaskForgeScalarMaskSource::MaterialSlot,
			EVertexMaskForgeScalarMaskSource::DirectionalNormal,
			EVertexMaskForgeScalarMaskSource::Thickness,
		};
	}

	bool IsGeneratorLayer(const EVertexMaskForgeScalarMaskSource Source)
	{
		// AUDITED (M16-K.1): explicit allowlist, one case per valid generator -- ConstantWhite/
		// ConstantBlack (Fill overrides, not generators) and any other/unknown value (including an
		// out-of-range cast, which simply matches no case for a switch on an enum class) fall through
		// to the default.
		switch (Source)
		{
		case EVertexMaskForgeScalarMaskSource::BoundingBox:
		case EVertexMaskForgeScalarMaskSource::AmbientOcclusion:
		case EVertexMaskForgeScalarMaskSource::Curvature:
		case EVertexMaskForgeScalarMaskSource::Noise:
		case EVertexMaskForgeScalarMaskSource::MaterialSlot:
		case EVertexMaskForgeScalarMaskSource::DirectionalNormal:
		case EVertexMaskForgeScalarMaskSource::Thickness:
			return true;
		default:
			return false;
		}
	}

	bool IsValid(const TConstArrayView<EVertexMaskForgeScalarMaskSource> Order)
	{
		// AUDITED (M16-K.1): pure observation -- Order itself is never written to, reordered,
		// normalized, or repaired. Exactly 7 elements required; every element must resolve to one of
		// the 7 generator slots; no slot may be seen twice. Deliberately NOT a TSet -- a set would
		// silently collapse duplicates before this function ever saw them, hiding exactly the
		// "duplicate" and "missing" cases this contract must detect explicitly.
		if (Order.Num() != 7)
		{
			return false;
		}

		bool bSlotSeen[7] = { false, false, false, false, false, false, false };
		for (const EVertexMaskForgeScalarMaskSource Source : Order)
		{
			const int32 Slot = GeneratorSlotIndex(Source);
			if (Slot == INDEX_NONE)
			{
				// Not a generator layer identity -- ConstantWhite, ConstantBlack, or an unknown/cast-invalid value.
				return false;
			}
			if (bSlotSeen[Slot])
			{
				// Duplicate -- this generator already appeared earlier in Order.
				return false;
			}
			bSlotSeen[Slot] = true;
		}

		// AUDITED (M16-K.1): Order.Num()==7, every element resolving to a distinct slot, together
		// already guarantee (by pigeonhole) that all 7 slots were seen -- this loop restates that
		// explicitly rather than relying on the arithmetic alone, so "no missing generator" stays a
		// visible, independently-checked invariant in the code, not an implied consequence.
		for (const bool bSeen : bSlotSeen)
		{
			if (!bSeen)
			{
				return false;
			}
		}

		return true;
	}

	bool MoveUp(TArray<EVertexMaskForgeScalarMaskSource>& Order, const EVertexMaskForgeScalarMaskSource Source)
	{
		// AUDITED (M16-K.1): every rejection path below returns before any mutation is attempted --
		// Order.Swap() is the ONLY mutating statement in this function, and it is the last statement,
		// reached only once every precondition already holds.
		if (!IsValid(Order) || !IsGeneratorLayer(Source))
		{
			return false;
		}

		const int32 CurrentIndex = Order.IndexOfByKey(Source);
		if (CurrentIndex == INDEX_NONE || CurrentIndex == 0)
		{
			return false;
		}

		// AUDITED (M16-K.1): the neighbor is resolved from Order's own current array position (never
		// enum arithmetic) -- an adjacent swap, exactly one pair of positions change; every other
		// element's position and identity is untouched.
		Order.Swap(CurrentIndex, CurrentIndex - 1);
		return true;
	}

	bool MoveDown(TArray<EVertexMaskForgeScalarMaskSource>& Order, const EVertexMaskForgeScalarMaskSource Source)
	{
		if (!IsValid(Order) || !IsGeneratorLayer(Source))
		{
			return false;
		}

		const int32 CurrentIndex = Order.IndexOfByKey(Source);
		if (CurrentIndex == INDEX_NONE || CurrentIndex == Order.Num() - 1)
		{
			return false;
		}

		Order.Swap(CurrentIndex, CurrentIndex + 1);
		return true;
	}
}
