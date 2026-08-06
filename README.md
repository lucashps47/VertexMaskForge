# Vertex Mask Forge

Vertex Mask Forge is an Editor-only Unreal Engine 5.8 plugin for procedural,
layered vertex-mask authoring on Static Meshes, built around a production
environment-art workflow rather than a one-off scripting tool.

## Why Vertex Mask Forge exists

Hand-painting vertex masks for occlusion, weathering, blend transitions, and
similar effects is slow and hard to iterate on, and a full DCC round-trip
(export, paint, reimport) breaks flow for what is often a fast, local
adjustment. Vertex Mask Forge keeps that work inside the Unreal Editor:
generate masks procedurally from real mesh data (curvature, ambient
occlusion, thickness, bounding-box position, normal direction, material
slot membership, noise), compose them in a layered, painter-like stack, and
preview the result live on the actual selected components before committing
anything.

## Current capabilities

- **Layered mask authoring.** A single ordered layer stack (`Layers`) is the
  plugin's authoring workflow. Each layer owns one generator (or a plain
  Fill value), its own parameters, Blend Mode, Opacity, and per-channel
  (Red/Green/Blue/Alpha) participation — editing one layer never affects
  another's stored state.
- **Seven procedural generators.** Material Slot, Bounding Box (Local
  Space), Ambient Occlusion, Directional Normal (Local Space), Curvature,
  Noise/Grunge, and Thickness (with an internal multi-ray fallback for
  thin/concave geometry). Each generator's parameters are edited per layer,
  live, with no separate "Generate" step.
- **Sequential, reorderable composition.** Layers fold in stack order using
  a small set of blend modes (Copy, Add, Subtract, Multiply, and related
  variants), each layer's own Opacity, and per-channel gating — so a layer
  can, for example, affect only Alpha while leaving Red/Green/Blue
  untouched.
- **Non-destructive preview.** Every edit is shown live on the real
  selected components before anything is written to an asset. Nothing is
  persisted until the artist explicitly commits the result.
- **Accept and Cancel.** Accept consolidates the current layer composition
  into the selected Static Mesh asset's own source vertex colors (a real,
  transactional, Undo/Redo-capable write); Cancel discards the session
  without touching the asset.
- **Mesh Paint Texture transfer.** After an Accept, "Send to Mesh Paint
  Texture" transfers the now-accepted vertex colors through Unreal's own
  native Mesh Paint "From Vertex" import, producing or replacing the
  selected component's Mesh Paint Texture so painting can continue
  manually in Unreal's Mesh Paint mode. This is a transient bridge: it
  never leaves a persistent override behind and never modifies the Static
  Mesh asset itself.
- **Nanite-aware workflow.** Two structurally distinct geometry domains are
  supported end-to-end — render-vertex editing for conventional meshes, and
  a corner-exact Source Topology domain (`FDynamicMesh3`) for Nanite and
  other dynamic-mesh editing paths — with Accept, preview, and every
  generator working correctly in the domain the selected asset actually
  requires.

## Editing and preview workflow

1. Select one or more eligible Static Mesh components in the viewport.
2. Open the panel via `Tools → Custom Tools → Vertex Mask Forge`.
3. Click **Edit Vertex Mask** to start a session on the current selection.
4. Add and configure layers in the **Layers** stack — assign a generator or
   a Fill value per layer, adjust Blend Mode/Opacity/channel participation,
   and reorder layers to change composition priority. Every change updates
   the live preview on the real components.
5. **Accept** to write the composed result into the Static Mesh asset's
   source vertex colors, or **Cancel** to discard the session.
6. Optionally, **Send to Mesh Paint Texture** to carry the just-accepted
   result into a native Mesh Paint Texture for further manual painting.

## Architecture principles

- **Editor-only.** No Runtime module, no gameplay-facing API; nothing in
  this plugin ships into a packaged game.
- **Native C++ and Slate.** No Blueprint, no Editor Utility Widget, no
  third-party DCC or Houdini dependency.
- **Domain-explicit.** Render Vertex and Source Topology are treated as
  distinct, non-interchangeable domains throughout the codebase — no
  implicit or lossy conversion between them.
- **Non-destructive by default.** Preview state is fully separate from
  Accepted (persisted) state until the artist explicitly commits.
- **Evidence-grounded engine integration.** Native Unreal Engine entry
  points (Mesh Paint, Dynamic Mesh, Geometry Script) are used only after
  direct inspection of the installed UE 5.8 engine source, not assumed
  from prior engine versions or documentation alone.

## Current status

Vertex Mask Forge is production-oriented and under active, incremental
development, validated continuously against the installed UE 5.8 engine
source and a large automated test suite. It is not a prototype: the layer
composition system, all seven generators, Accept/Cancel, and the Nanite
Source Topology domain are implemented, tested, and manually validated in
the Editor. Mesh Paint Texture transfer is an interim, evidence-grounded
bridge to Unreal's own native "From Vertex" import — it is not yet a full,
purpose-built Mesh Paint integration.

## Roadmap

The next planned phase is a complete audit of Unreal Engine 5.8's native
Mesh Paint architecture — Mesh Paint Texture ownership and serialization,
native texture creation and component association, resource lifecycle and
persistence, and which native entry points can safely support a direct
write — undertaken specifically to design a **Direct Bake** workflow that
composes and bakes a Forge result directly into a target Mesh Paint
texture, rather than depending primarily on the current transfer bridge.
Direct Bake is a planned future capability; it is not implemented today.

## Requirements

- Unreal Engine 5.8.
- An Editor-only plugin: it must be enabled for an Unreal Engine project's
  Editor build, and is never packaged into a game build.

## Build and setup

Vertex Mask Forge is a native C++ plugin. Add it under a project's
`Plugins` directory (or reference it from wherever it is checked out) and
regenerate project files, then build the project's Editor target
(`<ProjectName>Editor Win64 Development`) as usual for a native Unreal
Engine plugin. Once built and enabled, the tool is available from
`Tools → Custom Tools → Vertex Mask Forge`.

## Validation

The plugin carries an extensive Automation test suite (hundreds of tests)
covering generator math, layer composition, domain handling, caching and
invalidation, Accept/Cancel behavior, and the Mesh Paint Texture transfer
bridge. Panel-level Slate UI wiring (button enablement, live preview
updates, drag-and-drop reordering) has no automatable seam in this
codebase and is validated manually in the Editor instead; see
[`Docs/VertexMaskForgeArchitecture.md`](Docs/VertexMaskForgeArchitecture.md)
for the exact, checkpoint-by-checkpoint boundary between automated and
manual coverage.

## Known limitations

- Dynamic (Layers) preview and composition currently operate in the
  Source Topology domain for every generator; only Material Slot has a
  Render Vertex live-preview path.
- Some generator options (for example, World Space and Unified Bounds for
  certain generators) are intentionally not yet exposed — see
  [`Docs/VertexMaskForgeArchitecture.md`](Docs/VertexMaskForgeArchitecture.md)'s
  Known Limitations section for the complete, current list.
- Mesh Paint Texture transfer is a narrow, evidence-grounded use of one
  native Unreal Engine entry point, not a complete Mesh Paint integration;
  see the Roadmap above.
- No layer-stack serialization, presets, or reusable "Paint Layers" concept
  exists yet.

## Documentation

- [`Docs/VertexMaskForgeArchitecture.md`](Docs/VertexMaskForgeArchitecture.md)
  — the living architectural source of truth: ownership, domains, color
  lifecycle, composition, generators, integration status, and current
  limitations.
- [`Docs/VertexMaskForgeDecisionLog.md`](Docs/VertexMaskForgeDecisionLog.md)
  — accepted architectural decisions, their rationale, and open questions.

## License and contribution status

This is a private, actively developed production tool. No public license
or external contribution process is currently established.
