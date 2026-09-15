# The glass clip mask under a transform

**Date:** 2026-09-15
**Status:** implemented and verified 2026-09-15 (UE 5.8.1, Linux). Two decisions were taken
during implementation rather than here, and are marked **DECIDED IN CODE** below. Builds on PR #3 (`ab646ce`, branch
`fix/glass-mask-under-transform`), which found the bug and fixed it correctly by a different
means. This spec replaces the fix's *mechanism*, keeps its *finding*, and closes two adjacent
holes the finding exposed.
**Beads:** `VaCuus-k8m`

## What PR #3 established, and what it left

`backdrop-filter` glass on an element under a transformed ancestor blurred only part of the
panel. The mechanism is settled and was verified against the sources, line by line, during
review:

- `RenderManager::ApplyClipMask` calls `SetTransform` with each clip element's own transform
  before its `RenderToClipMask`, then restores the caller's
  (`ThirdParty/RmlUi/Source/Core/RenderManager.cpp:164-175`).
- A child of a transformed ancestor carries the accumulated transform, because
  `Element::UpdateTransformState` folds the parent's in
  (`ThirdParty/RmlUi/Source/Core/Element.cpp:3003-3018`).
- `FVaCuusGlassDistiller::Distill` never handled `SetTransform`, so the entry kept the clip
  element's *untransformed* geometry, while `SampleRegion` and `DrawRegion` came from
  scissors that `ElementUtilities::GetBoundingBox` already projects
  (`ThirdParty/RmlUi/Source/Core/ElementUtilities.cpp:291-300`). Mask and draw region
  disagreed.

Restore-the-bug, reproduced independently during review at `ab646ce` with the fix's branch
forced to the identity path:

| expected mask box | observed before the fix |
| --- | --- |
| `(60,60)-(360,240)` | `(40,40)-(240,160)` |

PR #3 fixes this by **baking**: under a non-identity transform it allocates an owned copy of
the mask geometry, pushes every vertex through `(position + Translation) * Transform`, and
zeroes `MaskTranslation`. That is correct. It is also the more expensive of the two available
shapes, it gives `MaskTranslation` two different meanings, and it makes one adjacent
optimisation permanently unreachable.

## Non-goals, decided rather than deferred

- **Per-pixel ancestor clipping with rounded corners.** A rounded, clipping ancestor over a
  glass panel stays out of scope (M5 spec §11, root-level elements). Closing it means a
  stencil pass of its own inside the glass section, several passes per entry, for a case the
  product does not have. What this spec *does* fix is the part that is free: the ancestor's
  bounds. See §3.
- **Clipping the blur's read region by an ancestor.** `SampleRegion` stays as recorded.
  RmlUi does not clip the backdrop read either: `ApplyScissorRegionForBackdrop` overwrites
  the scissor with the bounding box plus ink overflow rather than intersecting it
  (`ThirdParty/RmlUi/Source/Core/ElementEffects.cpp:236-244`).
- **A general multi-mask glass draw.** One `Set` mask remains the drawn shape. Everything
  else is folded into bounds.

## 1. Fold the transform into the draw matrix

**The change.** `FVaCuusGlassEntry` gains `FMatrix44f MaskTransform = FMatrix44f::Identity`.
`MaskGeometry` goes back to always being the shared reference from the cross-buffer map, and
`MaskTranslation` goes back to always meaning the recorded translation. The branch in
`Distill` and the vertex-baking helper both disappear.

The slate element composes, in row-vector order:

```
MaskToView(MaskTranslation) * MaskTransform * ViewToOutput(Mapping) * PixelToClip
```

**Why this is the same pixels, not an approximation.** The composition the replayer's stencil
pass draws with is `Translate * CurrentTransform * Projection`
(`VaCuusRender/Private/VaCuusReplayRenderer.cpp:1525`), with the GPU's divide by `w` after
the projection. Folding reproduces it exactly, on two facts about the matrices that follow
`MaskTransform`:

- `ViewToOutput` and `PixelToClip` are affine with fourth column `(0,0,0,1)`, so `w` passes
  through untouched and the divide commutes with them. For a row vector `u` and such a
  matrix `A`, `(u * A) / u.w == (u / u.w) * A`, because `A`'s translation row is scaled by
  `u.w` in exactly the way the divide undoes.
- The `z` that a 3D transform produces never reaches the output. `MakePixelToClipMatrix`
  sets `M[2][2] = 0` and `M[3][2] = 0.5`, pinning clip `z` to `0.5 * w`
  (`VaCuusRender/Private/VaCuusReplayRenderer.cpp:214-224`), and its `M[2][0]`/`M[2][1]` are
  zero, so `z` contributes to neither `x` nor `y`. A 3D-rotated panel cannot drift outside
  the near/far planes.

Baking gets the same answer for an affine transform and an *approximation* for a projective
one, because it does the divide in view space with `z` already discarded. Folding lets the
GPU do it. So folding is at least as correct everywhere and strictly more correct under CSS
`perspective()`.

**What it costs.** Two extra 4x4 multiplies per entry per engine frame, against one
allocation plus a full vertex pass per entry per *publish* today. The entry grows by 64
bytes.

**Where the math lives.** Not inline in the draw loop. A pure function beside
`VaCuusMakeGlassMapping` in `VaCuusGlassDistiller.h`:

```cpp
FMatrix44f VaCuusMakeGlassMaskMatrix(const FVaCuusGlassEntry&, const FVaCuusGlassMapping&, FIntPoint OutputExtent);
```

`VaCuusMakeGlassMapping` already established this shape and already has its own unit test
(`VaCuus.Render.Glass.MappingPIEShaped`). Following it means the mask composition becomes
data-in/data-out, and the test asserts the matrix that actually reaches the shader instead of
re-deriving the implementation's arithmetic on the vertices.

## 2. Stop rebuilding the mask buffers on every publish

`ListGeneration` moves in every `Distill` (`VaCuusRender/Private/VaCuusGlassDistiller.cpp:36`),
`RefreshGlassDrawResources` gates on it (`VaCuusRender/Private/VaCuusSlateElement.cpp:330`),
and it runs from `SetPendingBuffer_RenderThread`
(`VaCuusRender/Private/VaCuusSlateElement.cpp:46`). So **every published buffer recreates the
vertex and index buffer of every glass entry**, whatever caused the publish. A clock ticking
in the corner of the HUD rebuilds the glass panel's mask.

After §1 the mask geometry is a stable shared pointer for as long as the handle lives, so the
cache becomes possible at all. With baking it never could be: the geometry is a fresh
allocation by construction on every publish.

**The key is the `TSharedPtr` the entry already holds.** Comparing raw addresses would be an
ABA bug the day a released payload's allocation is reused; holding the reference in the cache
makes the address meaningful, because it cannot be reused while the reference is alive. The
square-corner entries key on their `DrawRegion` instead, since their quad is generated from
it.

`FGlassDraw` gains the source reference and the source rect, `RefreshGlassDrawResources`
reuses a matching slot and rebuilds only the rest. `GlassDraws` stays index-parallel to the
entries, which the empty-slot rule already depends on.

**DECIDED IN CODE: where the observable lives.** The plan reached for an upload counter on
the element, which would have needed a test able to drive `RefreshGlassDrawResources` — and
that needs an RHI command list and friendship with a private member, neither of which this
suite has any precedent for. The decision that can actually be wrong is *whether these
buffers still fit this entry*, so that is what became a free function,
`VaCuusGlassDrawMatchesEntry`, with `VaCuus.Render.Glass.DrawBufferReuse` driving it on
plain data. The element is then one `IndexOfByPredicate` away from the answer. A counter no
test reads would have been the same rot this spec complains about in §3, pointing the other
way.

## 3. Fold Intersect masks into DrawRegion

**The invariant to restore: a transform must not change what is supported.**

Without a transform, an ancestor's square clipping reaches the entry through the scissor, and
`DrawRegion` carries it. With a transform it does not. `GetClippingRegion` sets
`disable_scissor_clipping` for a transformed clipping element and expresses that element's
clip *only* as an `Intersect` clip mask
(`ThirdParty/RmlUi/Source/Core/ElementUtilities.cpp:162-178`) — and `Distill` uses
`ActiveMasks[0]` and drops the rest. So glass inside a transformed, `overflow: hidden`
container whose content overflows paints past the container.

The comment at the drop site says the opposite today: *"ancestor square clipping still lands
via DrawRegion's scissor"*. Under a transform that sentence is false. It is exactly the class
of comment CLAUDE.md warns about, and it is load-bearing enough that it hid this hole.

**The fix, in one step.** Each `Intersect` mask contributes its transformed axis-aligned
bounds to `DrawRegion` by intersection. This is strictly conservative and cannot remove a
pixel RmlUi would have drawn: the clip is the intersection of the mask shapes, so every drawn
pixel lies inside every mask's bounds. For a scaled ancestor without rotation it is exact.

**The observable.** What remains lost is an ancestor's corner rounding, and today that loss is
invisible, untestable and therefore rotting. The entry gains a count of Intersect masks
applied as bounds only. It is a fact, not a heuristic — the distiller cannot tell a rounded
clip box from a square one by its geometry, and does not have to.

## Testing

Every item is a restore-the-bug: break it, watch the named test fail, restore, report both.

All five shipped, all five seen to fail and then pass. `VaCuus.Render.Glass` went from 11
tests to 16; the whole of `VaCuus.Render` is 76 and green.

| test | asserts | what breaking it produced |
| --- | --- | --- |
| `Glass.MaskMatrix` | translation, then transform, then the DestRect mapping | order swapped: the PIE case alone failed, `(330,195)` for `(280,170)` |
| `Glass.DistillTransformedMask` | a panel under a scaled ancestor masks the scaled box | `MaskTransform` dropped: `(40,40)-(240,160)` for `(60,60)-(360,240)` |
| `Glass.DistillRotatedMask` | a quarter-turned panel masks the turned box | same break: `(100,100)-(300,200)` for `(150,50)-(250,250)` |
| `Glass.DistillSelfTransformedMask` | the transform on the panel itself takes the same path | same break: `(40,40)-(240,160)` for `(40,40)-(340,220)` |
| `Glass.DrawBufferReuse` | a moving transform re-uploads nothing | rounded entries keyed on `DrawRegion` too: only the animation assertion failed |
| `Glass.TransformedAncestorClip` | a transformed clipping ancestor shrinks `DrawRegion` to its bounds | fold dropped: `(50,49)-(650,600)` for `(50,50)-(350,275)` |

**DECIDED IN CODE: the ancestor test's document.** The first version put the glass panel at
`position: absolute` inside the container, and RmlUi pushed no Intersect mask at all —
`has_clipping_content` was false, because an absolutely positioned child does not make the
container's content overflow (`ThirdParty/RmlUi/Source/Core/ElementUtilities.cpp:148-150`).
That is not the gap: with no clipping content RmlUi does not clip either, so the entry was
already right. The panel had to go back into normal flow for the container to have something
to clip and for the mask to exist to be folded.

`VaCuus.Render.Glass` must stay green as a whole, and the monolithic game target must build:
this touches `VaCuusRender`, which ships in it, and the editor leg alone has never proved
that.
