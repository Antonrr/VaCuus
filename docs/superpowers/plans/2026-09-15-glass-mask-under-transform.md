# Glass clip mask under a transform: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace PR #3's vertex baking with a folded draw matrix, stop rebuilding the glass
mask buffers on every publish, and make a transformed ancestor's square clip reach the glass
entry the way an untransformed one already does.

**Architecture:** The clip element's transform travels on the entry as a matrix instead of
being pushed into the vertices. A pure `VaCuusMakeGlassMaskMatrix` composes translation,
transform, view-to-output mapping and the pixel-to-clip ortho; the slate element and the
tests both call it. Because the mask geometry is then a stable shared pointer again, the
per-entry vertex/index buffers can be keyed on it and reused across publishes. Separately,
every `Intersect` mask folds its transformed bounds into `DrawRegion`.

**Tech stack:** UE 5.8.1, C++, `IMPLEMENT_SIMPLE_AUTOMATION_TEST`, vendored RmlUi 6.x.

**Where the work happens:** edit in `/w/Unreal/VcHost/Plugins/VaCuus` (the build clone; the
editor loads it), branch off `pr-3`. `/w/Unreal/VaCuus` gets the docs and the final diff.

**Status:** executed 2026-09-15. Task 4's observable and Task 5's test document both changed
during execution; the spec records what was decided and why.

**Spec:** `docs/superpowers/specs/2026-09-15-glass-mask-under-transform-design.md`
**Bead:** `VaCuus-k8m`

---

## Shared commands

Build (no editor may be running; check `pgrep -a UnrealEditor` and kill by PID):

```bash
cd /w/Unreal/UnrealEngine && ./Engine/Build/BatchFiles/Linux/Build.sh \
  VcHostEditor Linux Development -project=/w/Unreal/VcHost/VcHost.uproject
```

Run the glass suite (note the COMMA separators and the trailing comma):

```bash
rm -f /w/Unreal/VcHost/Saved/Logs/VcHost.log
setsid nohup /w/Unreal/UnrealEngine/Engine/Binaries/Linux/UnrealEditor-Cmd \
  /w/Unreal/VcHost/VcHost.uproject -unattended -nullrhi -nosplash \
  -ExecCmds="Automation RunTests VaCuus.Render.Glass, Quit," > /tmp/glass.log 2>&1 & disown
until grep -q "Sending StopTestSession" /w/Unreal/VcHost/Saved/Logs/VcHost.log; do sleep 2; done
grep "Test Completed" /w/Unreal/VcHost/Saved/Logs/VcHost.log | sed 's/.*Result={\([^}]*\)}.*Path={\([^}]*\)}.*/\1 \2/'
```

Then kill the editor by PID. It does not exit on `Quit` in an automation run.

---

## Task 1: `VaCuusMakeGlassMaskMatrix`, the composition as one testable unit

**Files:**
- Modify: `Source/VaCuusRender/Private/VaCuusGlassDistiller.h` (entry struct, new declaration)
- Modify: `Source/VaCuusRender/Private/VaCuusGlassDistiller.cpp:299` (new definition at the end)
- Test: `Source/VaCuusRender/Private/Tests/VaCuusGlassPipelineTest.cpp`

- [ ] **Step 1: Add `MaskTransform` to the entry and restore `MaskTranslation`'s single meaning**

In `VaCuusGlassDistiller.h`, replace the two-shape doc comment on `MaskGeometry` that PR #3
introduced with the single shape, and add the matrix after `MaskTranslation`:

```cpp
	/**
	 * The rounded mask: a COPY of the clip-mask geometry (the recorded
	 * RenderToClipMask(Set) between the two composites), shared with the distiller's
	 * cross-buffer map, in the clip element's OWN untransformed space. Null = square
	 * corners (scissor-only clipping) — the element draws a plain quad over DrawRegion
	 * instead.
	 *
	 * THE LIST OWNS ITS COPY (via this shared ref): the buffer the vertices arrived in is
	 * recycled after replay, and the map entry may be retired by a later buffer's
	 * ReleasedGeometry while this list still draws — the ref keeps the payload alive until
	 * the next wholesale replacement drops it.
	 */
	TSharedPtr<const FVaCuusGeometryData> MaskGeometry;

	/** RenderToClipMask's Translation: the mask's border-box offset in view space. */
	FVector2f MaskTranslation = FVector2f::ZeroVector;

	/**
	 * The clip element's OWN transform at the mask draw — RenderManager::ApplyClipMask
	 * calls SetTransform with it before each RenderToClipMask and restores the caller's
	 * afterwards (ThirdParty/RmlUi/Source/Core/RenderManager.cpp:164-175). Identity for
	 * the overwhelmingly common untransformed panel. NOT applied to the vertices: it is
	 * folded into the draw matrix by VaCuusMakeGlassMaskMatrix, which is what lets the
	 * geometry stay a shared ref that the buffer cache can key on.
	 */
	FMatrix44f MaskTransform = FMatrix44f::Identity;
```

- [ ] **Step 2: Declare the composition beside the mapping factory**

At the end of `VaCuusGlassDistiller.h`, under the existing `VaCuusMakeGlassMapping`
declaration:

```cpp
/**
 * The mask's draw matrix: view-space mask vertices -> clip space, in ROW-VECTOR order
 *
 *   translate(MaskTranslation) * MaskTransform * (mapping scale, offset) * pixel-to-clip
 *
 * the same composition the replayer's stencil pass draws the mask with
 * (VaCuusReplayRenderer.cpp:1525), with the perspective divide left to the GPU. Sound
 * because everything after MaskTransform is affine with fourth column (0,0,0,1), so w
 * survives to the divide, and because MakePixelToClipMatrix pins clip z at 0.5*w and gives
 * z no path into x or y (VaCuusReplayRenderer.cpp:214-224) — a 3D-rotated panel cannot
 * drift out of the clip planes.
 */
FMatrix44f VaCuusMakeGlassMaskMatrix(const FVaCuusGlassEntry& Entry, const FVaCuusGlassMapping& Mapping, FIntPoint OutputExtent);
```

- [ ] **Step 3: Write the failing test**

In `VaCuusGlassPipelineTest.cpp`, beside `FVaCuusGlassMappingTest`. It drives the pure
function only, no engine, no document:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassMaskMatrixTest, "VaCuus.Render.Glass.MaskMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassMaskMatrixTest::RunTest(const FString& Parameters)
{
	// Where a view-space mask corner lands in OUTPUT PIXELS, by undoing the ortho the
	// matrix ends with: clip.xy / clip.w back through MakePixelToClipMatrix's inverse.
	const FIntPoint OutputExtent(1920, 1080);
	auto ToOutputPixels = [&OutputExtent](const FMatrix44f& M, const FVector2f& ViewPosition)
	{
		const FVector4f Clip = M.TransformFVector4(FVector4f(ViewPosition.X, ViewPosition.Y, 0.0f, 1.0f));
		const float InvW = (Clip.W != 0.0f) ? 1.0f / Clip.W : 1.0f;
		return FVector2f((Clip.X * InvW + 1.0f) * 0.5f * float(OutputExtent.X),
			(1.0f - Clip.Y * InvW) * 0.5f * float(OutputExtent.Y));
	};

	// A 1:1 mapping at the origin isolates the mask matrix from the DestRect mapping.
	const FVaCuusGlassMapping Identity = VaCuusMakeGlassMapping(
		FIntRect(0, 0, 1920, 1080), FVector2f::ZeroVector, FIntPoint(1920, 1080), FIntRect(0, 0, 1920, 1080), OutputExtent);

	{
		// No transform: translation alone moves the corner, exactly as today.
		FVaCuusGlassEntry Entry;
		Entry.MaskTranslation = FVector2f(40.0f, 40.0f);
		const FVector2f Landed = ToOutputPixels(VaCuusMakeGlassMaskMatrix(Entry, Identity, OutputExtent), FVector2f(200.0f, 120.0f));
		TestTrue(TEXT("Untransformed: vertex + translation, unchanged"), Landed.Equals(FVector2f(240.0f, 160.0f), 0.05f));
	}

	{
		// scale(1.5) about the document origin: (200+40, 120+40) * 1.5 = (360, 240).
		FVaCuusGlassEntry Entry;
		Entry.MaskTranslation = FVector2f(40.0f, 40.0f);
		Entry.MaskTransform = FScaleMatrix44f(FVector3f(1.5f, 1.5f, 1.0f));
		const FVector2f Landed = ToOutputPixels(VaCuusMakeGlassMaskMatrix(Entry, Identity, OutputExtent), FVector2f(200.0f, 120.0f));
		TestTrue(TEXT("Scaled: the transform applies AFTER the translation"), Landed.Equals(FVector2f(360.0f, 240.0f), 0.05f));
	}

	{
		// THE ORDER GUARD: transform then mapping, never the other way round. A PIE-shaped
		// mapping at half size with a nonzero origin -> (360,240)*0.5 + (100,50).
		const FVaCuusGlassMapping Pie = VaCuusMakeGlassMapping(
			FIntRect(100, 50, 1060, 590), FVector2f::ZeroVector, FIntPoint(1920, 1080), FIntRect(0, 0, 1920, 1080), OutputExtent);
		FVaCuusGlassEntry Entry;
		Entry.MaskTranslation = FVector2f(40.0f, 40.0f);
		Entry.MaskTransform = FScaleMatrix44f(FVector3f(1.5f, 1.5f, 1.0f));
		const FVector2f Landed = ToOutputPixels(VaCuusMakeGlassMaskMatrix(Entry, Pie, OutputExtent), FVector2f(200.0f, 120.0f));
		TestTrue(TEXT("PIE-shaped: mask transform first, DestRect mapping second"), Landed.Equals(FVector2f(280.0f, 170.0f), 0.05f));
	}

	return true;
}
```

- [ ] **Step 4: Run it and watch it fail to compile**

Build. Expected: `VaCuusMakeGlassMaskMatrix` undefined. That is the failure for this step;
a link error is the test failing.

- [ ] **Step 5: Implement**

At the end of `VaCuusGlassDistiller.cpp`, after `VaCuusMakeGlassMapping`. It needs
`MakePixelToClipMatrix`, which lives in the `VaCuusReplay` namespace in
`VaCuusReplayRenderer.h`:

```cpp
FMatrix44f VaCuusMakeGlassMaskMatrix(const FVaCuusGlassEntry& Entry, const FVaCuusGlassMapping& Mapping, FIntPoint OutputExtent)
{
	// Row-vector composition, left to right = application order.
	FMatrix44f MaskToView = FMatrix44f::Identity;
	MaskToView.M[3][0] = Entry.MaskTranslation.X;
	MaskToView.M[3][1] = Entry.MaskTranslation.Y;

	FMatrix44f ViewToOutput = FMatrix44f::Identity;
	ViewToOutput.M[0][0] = Mapping.Scale.X;
	ViewToOutput.M[1][1] = Mapping.Scale.Y;
	ViewToOutput.M[3][0] = Mapping.Offset.X;
	ViewToOutput.M[3][1] = Mapping.Offset.Y;

	return MaskToView * Entry.MaskTransform * ViewToOutput * VaCuusReplay::MakePixelToClipMatrix(OutputExtent);
}
```

- [ ] **Step 6: Run the test, expect PASS**

Run the glass suite. `VaCuus.Render.Glass.MaskMatrix` must be Success.

- [ ] **Step 7: Restore-the-bug**

Swap the last line to `MaskToView * ViewToOutput * Entry.MaskTransform * MakePixelToClipMatrix(...)`,
rebuild, and confirm the PIE-shaped case fails while the untransformed one still passes. That
is the order guard doing its job. Restore.

---

## Task 2: Carry the matrix instead of baking the vertices

**Files:**
- Modify: `Source/VaCuusRender/Private/VaCuusGlassDistiller.cpp:22-56` (delete the helper),
  `:186-230` (the Set-mask block)
- Modify: `Source/VaCuusRender/Private/VaCuusSlateElement.cpp:540-552`
- Test: `Source/VaCuusRender/Private/Tests/VaCuusGlassPipelineTest.cpp` (rewrite
  `DistillTransformedMask`)

- [ ] **Step 1: Rewrite the transformed-mask test against the matrix**

Replace the two mask-box assertion blocks in `FVaCuusGlassDistillTransformedMaskTest`. The
helper `ComputeVertexBoundingBox` stays but gains the entry's matrix, so the test measures
what reaches the shader rather than what sits in the vertex buffer:

```cpp
/** The axis-aligned bounding box of every vertex pushed through the entry's own mask matrix, in view pixels. */
FIntRect ComputeMaskBoundingBox(const FVaCuusGlassEntry& Entry, FIntPoint ViewSize)
{
	// A 1:1 mapping keeps the answer in view pixels, so the expected boxes below stay the
	// CSS numbers a reader can check by hand.
	const FVaCuusGlassMapping Identity = VaCuusMakeGlassMapping(
		FIntRect(0, 0, ViewSize.X, ViewSize.Y), FVector2f::ZeroVector, ViewSize, FIntRect(0, 0, ViewSize.X, ViewSize.Y), ViewSize);
	const FMatrix44f Mask = VaCuusMakeGlassMaskMatrix(Entry, Identity, ViewSize);

	FVector2f Min(TNumericLimits<float>::Max(), TNumericLimits<float>::Max());
	FVector2f Max(TNumericLimits<float>::Lowest(), TNumericLimits<float>::Lowest());
	for (const FVaCuusVertex& Vertex : Entry.MaskGeometry->Vertices)
	{
		const FVector4f Clip = Mask.TransformFVector4(FVector4f(Vertex.Position.X, Vertex.Position.Y, 0.0f, 1.0f));
		const float InvW = (Clip.W != 0.0f) ? 1.0f / Clip.W : 1.0f;
		const FVector2f Pixels((Clip.X * InvW + 1.0f) * 0.5f * float(ViewSize.X), (1.0f - Clip.Y * InvW) * 0.5f * float(ViewSize.Y));
		Min.X = FMath::Min(Min.X, Pixels.X);
		Min.Y = FMath::Min(Min.Y, Pixels.Y);
		Max.X = FMath::Max(Max.X, Pixels.X);
		Max.Y = FMath::Max(Max.Y, Pixels.Y);
	}
	return FIntRect(FMath::RoundToInt(Min.X), FMath::RoundToInt(Min.Y), FMath::RoundToInt(Max.X), FMath::RoundToInt(Max.Y));
}
```

Both call sites become `ComputeMaskBoundingBox(Entry, GViewSize)`, and the old
`ComputeVertexBoundingBox` is deleted. The expected boxes do not change: `(60,60)-(360,240)`
scaled, `(40,40)-(240,160)` unscaled.

Add one assertion the old test could not make, right after the scaled box:

```cpp
			TestTrue(TEXT("The geometry itself is NOT baked: it stays the shared untransformed copy"),
				Entry.MaskTransform != FMatrix44f::Identity);
```

- [ ] **Step 2: Run, watch it fail**

Build and run. Expected: `DistillTransformedMask` fails, because `MaskTransform` is still
identity on every entry while the vertices carry the scale, so the box comes out at
`(90,90)-(540,360)` — the scale applied twice.

- [ ] **Step 3: Delete the baking helper**

Remove `TransformMaskGeometry` and its comment block from the `VaCuusGlassPrivate` namespace
in `VaCuusGlassDistiller.cpp`. Leave `FMaskDraw::Transform` in place.

- [ ] **Step 4: Collapse the Set-mask branch**

Replace the whole `if (ActiveMasks[0].Transform == FMatrix44f::Identity) { ... } else { ... }`
with the single shape:

```cpp
							if (const TSharedPtr<const FVaCuusGeometryData>* Found = MaskGeometry.Find(ActiveMasks[0].Geometry))
							{
								Entry.MaskGeometry = *Found;
								Entry.MaskTranslation = ActiveMasks[0].Translation;

								// The clip element's own transform rides along as a matrix
								// rather than being pushed into the vertices: the geometry
								// stays the map's shared, untransformed copy, which is what
								// the element's buffer cache keys on.
								Entry.MaskTransform = ActiveMasks[0].Transform;
							}
```

- [ ] **Step 5: Fold the matrix into the slate element's draw**

In `VaCuusSlateElement.cpp`, replace the hand-built `Affine` (the four `M[...]` assignments
and their comment) with the shared composition:

```cpp
			// Row-vector composition: mask translation (view px) -> the clip element's own
			// transform -> mapping scale+offset (output px) -> clip. One shared function so
			// the shader and VaCuus.Render.Glass.MaskMatrix cannot drift apart.
			FVaCuusUIShaderParameters VSParameters;
			VSParameters.Projection = VaCuusMakeGlassMaskMatrix(Entry, Mapping, OutputExtent);
```

Delete the now-unused `PixelToClip` local if nothing else in the loop uses it; check first
with `grep -n PixelToClip Source/VaCuusRender/Private/VaCuusSlateElement.cpp`.

- [ ] **Step 6: Run the whole suite, expect all green**

`VaCuus.Render.Glass` must be green, `DistillTransformedMask` and `MaskMatrix` included.

- [ ] **Step 7: Restore-the-bug**

Comment out the `Entry.MaskTransform = ActiveMasks[0].Transform;` line, rebuild, confirm
`DistillTransformedMask` fails on the scaled box with the old `(40,40)-(240,160)`, and that
the untransformed half still passes. Restore.

---

## Task 3: Rotation and self-transform coverage

**Files:**
- Test: `Source/VaCuusRender/Private/Tests/VaCuusGlassPipelineTest.cpp`

The fix is not scale-specific: `GetClippingRegion` pushes a clip mask for the glass element
itself whenever it is transformed, because `has_clipping_content` is true for the forced self
clip (`ThirdParty/RmlUi/Source/Core/ElementUtilities.cpp:150-163`). Two cases the current
test does not reach.

- [ ] **Step 1: Write both tests**

Same skeleton as `DistillTransformedMask` — engine up, context, document, four settle
iterations, `ON_SCOPE_EXIT` teardown — with these documents and assertions:

```cpp
// Rotation: #panel at (100,100), 200x100, rotated 90 degrees about its own centre (200,150).
// The border box turns into (150,50)-(250,250): width and height swap about the centre.
	TEXT("#panel{display:block;position:absolute;left:100px;top:100px;width:200px;height:100px;")
	TEXT("border-radius:12px;background-color:#30405080;backdrop-filter:blur(12px);")
	TEXT("transform:rotate(90deg);}")
```

expects `FIntRect(150, 50, 250, 250)`, and

```cpp
// The transform on the PANEL itself rather than an ancestor: same (40,40) 200x120 panel,
// scale(1.5) about its own top-left corner -> (40,40)-(340,220).
	TEXT("#panel{display:block;position:absolute;left:40px;top:40px;width:200px;height:120px;")
	TEXT("border-radius:12px;background-color:#30405080;backdrop-filter:blur(12px);")
	TEXT("transform:scale(1.5);transform-origin:left top;}")
```

expects `FIntRect(40, 40, 340, 220)`. Both reuse `ComputeMaskBoundingBox` and
`BoxWithinOnePixel`, and both assert `Entry.DrawRegion.Contains(ActualBox)`.

- [ ] **Step 2: Run, expect PASS**

They pass immediately — Task 2 already made them work. That is the point: they are coverage,
not a driver.

- [ ] **Step 3: Restore-the-bug**

Comment out `Entry.MaskTransform = ActiveMasks[0].Transform;` again and confirm BOTH new
tests fail, each on its own box. Restore.

---

## Task 4: Reuse the mask buffers across publishes

**Files:**
- Modify: `Source/VaCuusRender/Private/VaCuusSlateElement.h:118-129`
- Modify: `Source/VaCuusRender/Private/VaCuusSlateElement.cpp:328-377`
- Test: `Source/VaCuusRender/Private/Tests/VaCuusGlassPipelineTest.cpp`

- [ ] **Step 1: Give the cache an observable**

The reuse cannot be asserted without a counter — the RHI buffers are not reachable from a
test, and an invariant with no observable rots. Add to `FVaCuusSlateElement`, beside
`GlassDrawsGeneration` in the header:

```cpp
	/**
	 * Mask vertex/index buffers created since construction. The buffer-reuse observable:
	 * a publish that changes no mask geometry must not move it. Debug-and-test only, never
	 * a control input.
	 */
	uint64 GlassDrawUploads = 0;
```

- [ ] **Step 2: Key each draw on its source**

In the header, `FGlassDraw` gains the two identity fields:

```cpp
	/** One glass entry's uploaded geometry: the mask copy, or a generated DrawRegion quad. */
	struct FGlassDraw
	{
		FBufferRHIRef VB;
		FBufferRHIRef IB;
		int32 NumVertices = 0;
		int32 NumIndices = 0;

		/**
		 * What these buffers were built FROM, so an unchanged entry reuses them. The shared
		 * ref, not a raw pointer: holding it is what makes address equality mean identity,
		 * since the payload cannot be freed and its address reused while this ref is alive.
		 * SourceQuad carries the square-corner case, whose quad is generated from DrawRegion.
		 */
		TSharedPtr<const FVaCuusGeometryData> SourceGeometry;
		FIntRect SourceQuad;
	};
```

- [ ] **Step 3: Write the failing test**

A document with a rounded glass panel and a second element that changes every frame, so the
publishes keep coming while the mask does not move. It needs the element, not just the
distiller, so it exercises `RefreshGlassDrawResources` through its real call path.

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassDrawBuffersReusedTest, "VaCuus.Render.Glass.DrawBuffersReused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
```

Assertions, in order: after the first settle, `GlassDrawUploads` is nonzero; after a publish
caused by changing the *other* element's text, `GlassDrawUploads` has not moved; after
changing the panel's `border-radius`, it has.

If driving the slate element from a test turns out to need a live RHI that `-nullrhi` cannot
give, move the counter and the cache decision into a small pure helper
(`VaCuusGlassDrawNeedsUpload(const FGlassDraw&, const FVaCuusGlassEntry&)`) and test that
instead, with the element calling it. Decide this at implementation time, not before: the
answer is one build away and guessing it here would be a placeholder.

- [ ] **Step 4: Implement the reuse**

`RefreshGlassDrawResources` keeps the generation gate, then rebuilds `GlassDraws` by moving
matching slots out of the old array instead of creating buffers:

```cpp
	TArray<FGlassDraw> Previous = MoveTemp(GlassDraws);
	GlassDraws.Reset();

	for (const FVaCuusGlassEntry& Entry : GlassDistiller.GetEntries())
	{
		const FIntRect WantedQuad = Entry.MaskGeometry.IsValid() ? FIntRect() : Entry.DrawRegion;

		// Linear scan, and deliberately so: a document has a handful of glass panels, and a
		// map keyed on a shared ref would cost more to maintain than it saves.
		const int32 Reusable = Previous.IndexOfByPredicate([&Entry, &WantedQuad](const FGlassDraw& Candidate)
			{ return Candidate.VB.IsValid() && Candidate.SourceGeometry == Entry.MaskGeometry && Candidate.SourceQuad == WantedQuad; });
		if (Reusable != INDEX_NONE)
		{
			GlassDraws.Add(MoveTemp(Previous[Reusable]));
			Previous.RemoveAt(Reusable);
			continue;
		}

		// ... the existing quad-generation and buffer-creation body, plus:
		//   Draw.SourceGeometry = Entry.MaskGeometry;
		//   Draw.SourceQuad = WantedQuad;
		//   ++GlassDrawUploads;
	}
```

Keep the existing empty-slot rule: an entry whose geometry is degenerate still adds a default
`FGlassDraw` so `GlassDraws` stays index-parallel to the entries.

- [ ] **Step 5: Run, expect PASS, then restore-the-bug**

Force `Reusable = INDEX_NONE`, confirm the "did not move" assertion fails, restore.

---

## Task 5: Fold Intersect masks into DrawRegion

**Files:**
- Modify: `Source/VaCuusRender/Private/VaCuusGlassDistiller.h` (entry field)
- Modify: `Source/VaCuusRender/Private/VaCuusGlassDistiller.cpp` (the Set-mask block's
  surrounding scope)
- Test: `Source/VaCuusRender/Private/Tests/VaCuusGlassPipelineTest.cpp`

- [ ] **Step 1: Add the observable to the entry**

```cpp
	/**
	 * Intersect clip masks applied as BOUNDS ONLY — their transformed axis-aligned box was
	 * intersected into DrawRegion, their shape was not drawn. Zero for the ordinary panel.
	 *
	 * Why this number exists: without a transform an ancestor's clip reaches the entry
	 * through the scissor, so DrawRegion already carries it; with one, GetClippingRegion
	 * sets disable_scissor_clipping and expresses that ancestor ONLY as an Intersect mask
	 * (ThirdParty/RmlUi/Source/Core/ElementUtilities.cpp:162-178). Folding the bounds
	 * restores parity. What parity does not restore is an ancestor's corner ROUNDING, and
	 * this counter is how that remaining gap is asserted instead of merely believed.
	 */
	int32 BoundsOnlyMasks = 0;
```

- [ ] **Step 2: Write the failing test**

`#outer` is 200x150 at (50,50), `overflow:hidden`, scaled 1.5 about the document origin, so
its clip box lands at `(75,75)-(375,300)`. `#panel` inside it is 400x400 — bigger than the
container, so `has_clipping_content` is true and RmlUi really does push the Intersect mask.

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassTransformedAncestorClipTest, "VaCuus.Render.Glass.TransformedAncestorClip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
```

Assert: `Entry.BoundsOnlyMasks == 1`, and `Entry.DrawRegion.Max.X <= 376` — the glass no
longer paints past the container's right edge. Then remove the `scaled` class and assert the
untransformed control still clips the same way, through the scissor, with
`BoundsOnlyMasks == 0`.

- [ ] **Step 3: Run, watch it fail**

Expected: `BoundsOnlyMasks` is 0 and `DrawRegion` runs to the panel's own projected right
edge, well past 376.

- [ ] **Step 4: Implement**

In the write-back block, after the existing Set-mask handling, walk the rest of the list.
Compute each mask's transformed bounds with the same composition the Set mask draws with:

```cpp
						// Every OTHER mask in the list is an Intersect (ElementUtilities.cpp:163-169
						// makes the first Set and the ancestors Intersect). Its exact shape is not
						// drawn — one Set mask remains the drawn geometry — but its BOUNDS belong in
						// DrawRegion, and without this a transformed clipping ancestor reaches the
						// entry through nothing at all: GetClippingRegion disables scissor clipping
						// for a transformed element (ElementUtilities.cpp:174-178). Strictly
						// conservative: the clip is the intersection of the shapes, so every drawn
						// pixel lies inside every mask's bounds.
						for (int32 MaskIndex = 1; MaskIndex < ActiveMasks.Num(); ++MaskIndex)
						{
							const FMaskDraw& Mask = ActiveMasks[MaskIndex];
							if (const TSharedPtr<const FVaCuusGeometryData>* Found = MaskGeometry.Find(Mask.Geometry))
							{
								FIntRect Bounds;
								if (VaCuusGlassPrivate::ComputeMaskBounds(**Found, Mask.Translation, Mask.Transform, Bounds))
								{
									Entry.DrawRegion.Clip(Bounds);
									++Entry.BoundsOnlyMasks;
								}
							}
						}
```

and the helper in `VaCuusGlassPrivate`, which is also the one place the perspective divide
has to be done on the CPU, because a rect has no `w`:

```cpp
/** The transformed axis-aligned bounds of a mask, in view pixels. False for empty geometry. */
bool ComputeMaskBounds(const FVaCuusGeometryData& Geometry, const FVector2f& Translation, const FMatrix44f& Transform, FIntRect& OutBounds)
{
	if (Geometry.Vertices.Num() == 0)
	{
		return false;
	}

	FVector2f Min(TNumericLimits<float>::Max(), TNumericLimits<float>::Max());
	FVector2f Max(TNumericLimits<float>::Lowest(), TNumericLimits<float>::Lowest());
	for (const FVaCuusVertex& Vertex : Geometry.Vertices)
	{
		const FVector4f Homogeneous = Transform.TransformFVector4(
			FVector4f(Vertex.Position.X + Translation.X, Vertex.Position.Y + Translation.Y, 0.0f, 1.0f));
		const float InvW = (Homogeneous.W != 0.0f) ? (1.0f / Homogeneous.W) : 1.0f;
		const FVector2f Position(Homogeneous.X * InvW, Homogeneous.Y * InvW);
		Min.X = FMath::Min(Min.X, Position.X);
		Min.Y = FMath::Min(Min.Y, Position.Y);
		Max.X = FMath::Max(Max.X, Position.X);
		Max.Y = FMath::Max(Max.Y, Position.Y);
	}

	// Outward rounding: a bound that clips DrawRegion must never eat a pixel the mask covers.
	OutBounds = FIntRect(FMath::FloorToInt(Min.X), FMath::FloorToInt(Min.Y), FMath::CeilToInt(Max.X), FMath::CeilToInt(Max.Y));
	return true;
}
```

- [ ] **Step 5: Correct the comment this task falsifies**

The Set-mask comment still ends with *"while ancestor square clipping still lands via
DrawRegion's scissor"*. Replace that clause:

```cpp
						// The rounded mask: the Set draw of the active list (first —
						// ElementUtilities.cpp:163-169 makes the first Set and ancestors
						// Intersect). v1 draws the Set mask only; ancestor ROUNDED clipping
						// over a glass panel is out of scope (spec §11 — root-level
						// elements). Ancestor SQUARE clipping lands in DrawRegion: through
						// the scissor when the ancestor is untransformed, and through the
						// Intersect fold below when it is not — the scissor carries nothing
						// for a transformed ancestor (ElementUtilities.cpp:174-178).
```

- [ ] **Step 6: Run, expect PASS, then restore-the-bug**

Delete the `Entry.DrawRegion.Clip(Bounds);` line, confirm the right-edge assertion fails while
the counter assertion still passes — two independent failures would mean the test is testing
one thing twice. Restore.

---

## Task 6: Land it

- [ ] **Step 1: Full glass suite plus the neighbours**

Run `Automation RunTests VaCuus.Render` and confirm nothing outside Glass moved.

- [ ] **Step 2: Monolithic game target**

```bash
cd /w/Unreal/UnrealEngine && ./Engine/Build/BatchFiles/Linux/Build.sh \
  VcHost Linux Development -project=/w/Unreal/VcHost/VcHost.uproject
```

This leg has never been run for this code. It is the one in the spec's testing section.

- [ ] **Step 3: Citation gate**

```bash
cd /w/Unreal/VaCuus && python3 Tools/citation_check.py
```

Every `file:line` added by this plan must resolve. Expect `CLEAN`.

- [ ] **Step 4: Move the diff to the git tree and stop**

Bring the clone's diff into `/w/Unreal/VaCuus` on a branch off `pr-3`, leave it uncommitted,
and report. The conservative agent profile in `CLAUDE.md` forbids committing or pushing
without a current instruction to do so, and PR #3 belongs to its author — the landing shape
(amend the PR, stack on it, or open a second one) is the owner's call, not the plan's.

- [ ] **Step 5: Close the bead**

`bd close VaCuus-k8m` once the owner has accepted the diff, with the follow-up for rounded
transformed ancestors filed separately if it is still wanted.
