// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusEngine.h"
#include "VaCuusRecordingRenderInterface.h"

#include "Misc/ScopeExit.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Backport of upstream RmlUi's own doctest `Element.TransformStateAfterVisibilityChange`
 * (mikke89/RmlUi commit 6df503c, 2026-08-16, "Fix transform not being applied after element
 * turns visible, see #982"). Ported here rather than into the vendored doctest suite, because
 * this tree never vendors RmlUi's Tests/ directory at all (VENDORED_TAG.txt, "What is here, and
 * why exactly this") -- a VaCuus automation test through a real Rml::Context is the equivalent
 * proof available in this repo, and it exercises the same vendored Element.cpp this patch
 * touches rather than a separate doctest binary.
 *
 * WHY THE ASSERTION READS THE RECORDED TRANSFORM instead of calling Element::GetTransformState()
 * directly, unlike upstream's own test: TransformState is declared in the private
 * Source/Core/TransformState.h (not Include/), with no RMLUICORE_API export on the class
 * (TransformState.h:8) -- it is compiled into the VaCuusRml module, and this test lives in
 * VaCuusRender, a different module/DLL. FVaCuusRecordingRenderInterface (this module) observes
 * the same fact from the public RenderInterface seam instead: RmlUi only calls
 * RenderInterface::SetTransform when the active matrix actually changes
 * (RenderManager::SetTransform, RenderManager.cpp:144-154), and ElementUtilities::ApplyTransform
 * passes nullptr whenever the element has no TransformState (ElementUtilities.cpp:375-379) -- so
 * an element stuck without one forces an explicit SetTransform(identity) recording, which the
 * recorder captures without ever touching TransformState.
 *
 * WHY THE ASSERTIONS COMPARE AGAINST pane_reference's RECORDED TRANSFORM rather than a hand-built
 * scale(2) matrix: `Element::UpdateTransformState` folds `transform-origin` into the resolved
 * matrix as `Translate(origin) * transform * Translate(-origin)` (Element.cpp:2994-2995), and the
 * default origin is the element's own box centre (Element.cpp:2980). For this 100x100 `#window`
 * the recorded matrix is therefore `[2 0 0 0][0 2 0 0][0 0 1 0][-50 -50 0 1]` (UE row-vector
 * layout, translation in row 3), not a pure diagonal scale. Comparing the target pane against the
 * always-visible reference pane in the same frame keeps that origin math out of the test: with the
 * patch both panes draw under #window's one resolved matrix, without it the target reads identity
 * while the reference does not.
 *
 * THE MECHANISM UNDER TEST, read from this vendored tree (RmlUi 0ae381e, VENDORED_TAG.txt:4-6):
 * - `#window` has its own `transform: scale(2)` and is therefore a local stacking context; its
 *   `transform_state` resolves on the very first render, before `#pane_target` ever exists in
 *   its `stacking_context`.
 * - `#pane_target` starts `display: none`. A visibility flip only calls
 *   `parent->DirtyStackingContext()` when the element's effective visibility actually changes
 *   (Element.cpp:1853-1868); `AddToStackingContext` (which feeds BuildLocalStackingContext)
 *   skips invisible elements, so the pane is never a stacking_context member while hidden and
 *   is therefore never reached by the propagation loop at Element.cpp:3037-3041 either.
 * - `Element::UpdateTransformState` returns immediately when neither dirty flag is set
 *   (Element.cpp:2897-2898), so an element that was never dirtied keeps `transform_state`
 *   exactly as it was (null, for a pane that has never rendered before) even after `display`
 *   flips back to `block` -- UNLESS something dirties it at that point.
 * - Pre-patch, `Element::BuildLocalStackingContext` (Element.cpp:2362-2373, this vendored
 *   revision) rebuilds `#window`'s `stacking_context` to admit the now-visible pane but dirties
 *   nobody. `#window` itself is not dirty in this frame either (its own transform did not
 *   change), so `UpdateTransformState`'s own change-propagation loop (Element.cpp:3037-3041)
 *   never runs for it and the gap is never closed. Patch #7 (VENDORED_TAG.txt) closes exactly
 *   this: it adds a loop to `BuildLocalStackingContext` that calls
 *   `child->DirtyTransformState(false, true)` for every member whenever the container already
 *   has a `transform_state` -- `#window`'s case here.
 */
namespace VaCuusTransformVisibilityTest
{
static const FIntPoint GViewSize(400, 400);

/** One recorded frame: Update (styles/layout, where a display/visibility change dirties the
 *  parent's stacking context) then Render (issues the RenderInterface calls this test inspects).
 *  Mirrors VaCuusDecoratorTest.cpp's RecordContextFrame. */
TUniquePtr<FVaCuusCommandBuffer> RecordContextFrame(FVaCuusRecordingRenderInterface& Recorder, Rml::Context* Context)
{
	Recorder.BeginFrame(GViewSize);
	Context->Update();
	Context->Render();
	return Recorder.EndFrameAndPublish();
}

/** The transform active at the moment Buffer.Commands[DrawIndex] was recorded.
 *  RenderManager::SetTransform only emits a command when the active matrix actually changes
 *  (RenderManager.cpp:144-154, opened above), so the matrix in force at any draw is whichever
 *  SetTransform command most recently preceded it in the stream -- identity if none did. */
static FMatrix44f ActiveTransformBefore(const FVaCuusCommandBuffer& Buffer, int32 DrawIndex)
{
	FMatrix44f Current = FMatrix44f::Identity;
	for (int32 Index = 0; Index < DrawIndex; ++Index)
	{
		if (Buffer.Commands[Index].Type == EVaCuusCommandType::SetTransform)
		{
			Current = Buffer.Commands[Index].Transform;
		}
	}
	return Current;
}

/** Index of the DrawGeometry command whose Translation matches ExpectedTranslation, or
 *  INDEX_NONE. Translation is the element's own absolute layout offset, unaffected by any
 *  transform (the background renders at GetAbsoluteOffset(BoxArea::Border),
 *  ElementBackgroundBorder.cpp:41-42), so it identifies which element a draw belongs to
 *  independently of the bug under test. */
static int32 FindDrawAt(const FVaCuusCommandBuffer& Buffer, const FVector2f& ExpectedTranslation)
{
	for (int32 Index = 0; Index < Buffer.Commands.Num(); ++Index)
	{
		const FVaCuusCommand& Command = Buffer.Commands[Index];
		if (Command.Type == EVaCuusCommandType::DrawGeometry && Command.Translation.Equals(ExpectedTranslation, 0.01f))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}
} // namespace VaCuusTransformVisibilityTest

/**
 * THE RESTORE-THE-BUG CASE. `#pane_target` becomes visible AFTER `#window`'s `scale(2)` has
 * already been resolved by an earlier frame -- exactly the ordering upstream's fix targets
 * (6df503c, #982). A VaCuus document hits this same ordering through a `data-if` (inline
 * `display: none`) or a `data-for` clone created after the ancestor's matrix resolved; this test
 * reproduces it with a plain RmlUi property change so it stays independent of VaCuus's own
 * data-binding layer.
 *
 * Without Patch #7 this fails at the final assertion: `#pane_target` draws at its correct,
 * unscaled layout position (Translation never depended on the transform) but under an identity
 * matrix, because `BuildLocalStackingContext` re-admits it to `#window`'s stacking context
 * without dirtying it (Element.cpp:2362-2373 pre-patch) and nothing else does either (see the
 * file-level comment). With the patch it draws under #window's resolved transform, matching the
 * always-visible reference pane in the same frame.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTransformAfterVisibilityChangeTest, "VaCuus.Render.Transform.AfterVisibilityChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTransformAfterVisibilityChangeTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusTransformVisibilityTest;

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	if (!TestTrue(TEXT("Initialized"), Engine.Initialize()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Engine.Shutdown();
	};

	FVaCuusRecordingRenderInterface Recorder;
	const Rml::String ContextName("vacuus_transform_visibility_test");
	Rml::Context* Context = Rml::CreateContext(ContextName, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(ContextName);
	};

	// #window: a local stacking context (transform: scale(2)) whose matrix resolves on the very
	// first render, well before #pane_target ever joins its stacking_context. #pane_reference
	// stays visible throughout -- the positive control proving the harness itself applies the
	// parent's transform correctly. #pane_target starts display:none and is flipped to block only
	// after frame 1, so it joins the stacking context AFTER the scale was already resolved.
	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("#window{display:block;position:absolute;left:0;top:0;width:100px;height:100px;transform:scale(2);}")
		TEXT(".pane{display:block;position:absolute;width:20px;height:20px;background-color:#ff00ff;}")
		TEXT("#pane_reference{left:10px;top:10px;}")
		TEXT("#pane_target{left:50px;top:10px;display:none;}")
		TEXT("</style></head><body><div id=\"window\">")
		TEXT("<div class=\"pane\" id=\"pane_reference\"/><div class=\"pane\" id=\"pane_target\"/>")
		TEXT("</div></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://transform_visibility.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	const FVector2f ReferenceTranslation(10.f, 10.f);
	const FVector2f TargetTranslation(50.f, 10.f);

	// Frame 1: pane_target is hidden and must not draw at all; pane_reference proves a real,
	// non-identity transform is active, so the second frame's assertion is measuring the bug and
	// not a broken rig.
	const TUniquePtr<FVaCuusCommandBuffer> First = RecordContextFrame(Recorder, Context);
	if (!TestNotNull(TEXT("Frame 1 publishes"), First.Get()))
	{
		return false;
	}
	const int32 ReferenceDrawFrame1 = FindDrawAt(*First, ReferenceTranslation);
	if (TestTrue(TEXT("pane_reference draws in frame 1"), ReferenceDrawFrame1 != INDEX_NONE))
	{
		TestFalse(TEXT("pane_reference draws under a non-identity transform (positive control)"),
			ActiveTransformBefore(*First, ReferenceDrawFrame1).Equals(FMatrix44f::Identity, 1e-4f));
	}
	TestEqual(TEXT("pane_target does not draw while display:none"), FindDrawAt(*First, TargetTranslation), INDEX_NONE);

	// The visibility flip: this is the ordering the upstream fix targets. #window's scale was
	// already resolved in frame 1; #pane_target joins its stacking context only now.
	Rml::Element* Target = Document->GetElementById("pane_target");
	if (!TestNotNull(TEXT("pane_target element"), Target))
	{
		return false;
	}
	Target->SetProperty("display", "block");

	const TUniquePtr<FVaCuusCommandBuffer> Second = RecordContextFrame(Recorder, Context);
	if (!TestNotNull(TEXT("Frame 2 (after the visibility flip) publishes"), Second.Get()))
	{
		return false;
	}

	const int32 TargetDrawFrame2 = FindDrawAt(*Second, TargetTranslation);
	if (!TestTrue(TEXT("pane_target draws in frame 2, at its correct unscaled layout offset"), TargetDrawFrame2 != INDEX_NONE))
	{
		return false;
	}

	// pane_reference draws again every frame, and #window's matrix is recorded again with it:
	// Context::Render() ends with ResetState() (Context.cpp:239), which returns the active transform
	// to identity (RenderManager.cpp:187-190, RenderManager.h:37), so the next frame cannot dedupe it
	// away. Its frame-2 transform is the ground truth pane_target is compared against, instead of a
	// hand-computed matrix (see the file-level comment).
	const int32 ReferenceDrawFrame2 = FindDrawAt(*Second, ReferenceTranslation);
	if (!TestTrue(TEXT("pane_reference draws in frame 2"), ReferenceDrawFrame2 != INDEX_NONE))
	{
		return false;
	}

	// THE ASSERTION. Without Patch #7 pane_target has no TransformState, so ApplyTransform passes
	// nullptr (ElementUtilities.cpp:375-379). #window's matrix is still active from pane_reference's
	// draw, which comes first in DOM order, so RenderManager::SetTransform sees a change and records
	// an explicit identity (RenderManager.cpp:144-154). Translation is correct in both cases (see
	// FindDrawAt), so only the matrix tells the two states apart.
	const FMatrix44f TargetTransform = ActiveTransformBefore(*Second, TargetDrawFrame2);
	const FMatrix44f ReferenceTransform = ActiveTransformBefore(*Second, ReferenceDrawFrame2);
	TestTrue(TEXT("pane_target's transform in frame 2 matches pane_reference's (both under #window's resolved transform)"),
		TargetTransform.Equals(ReferenceTransform, 1e-4f));
	TestFalse(TEXT("pane_target draws under a non-identity transform, not identity"), TargetTransform.Equals(FMatrix44f::Identity, 1e-4f));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
