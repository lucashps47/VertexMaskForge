#pragma once

#include "Containers/Array.h"
#include "CoreMinimal.h"
#include "VertexMaskForgeMaskTypes.h"

/**
 * Generic, stateless Preview Mode display-color reduction (M3 extraction). Operates ONLY on an
 * already-composed WorkingColors buffer -- it never generates or reads a mask, never knows about
 * generators, caches, PreviewMesh/UObject ownership, render vertices vs. triangle corners, or
 * Nanite/Source-Topology domains. It does not decide WHEN Preview Mode display reduction should be
 * applied (that decision -- e.g. that it is applied to the render-vertex domain only, in this
 * checkpoint -- remains entirely the caller's responsibility; see
 * VertexMaskForgePanel::ApplyPreviewToEntry in SVertexMaskForgePanel.cpp).
 */
namespace VertexMaskForgeDisplayColorDerivation
{
	/**
	 * Derives the render-order color buffer actually pushed to a preview component's override colors
	 * for the LIVE PREVIEW ONLY, by reducing WorkingColors' RAW composited RGBA through the given
	 * Preview Mode -- e.g. "Red Channel" reduces to a Red-only grayscale. DISPLAY-ONLY: never mutates
	 * WorkingColors, and the caller must never use this result for persistence (Accept always persists
	 * the real, un-reduced WorkingColors verbatim, regardless of which Preview Mode is selected).
	 */
	TArray<FColor> DeriveDisplayColors(const TArray<FColor>& WorkingColors, EVertexMaskForgePreviewMode PreviewMode);
}
