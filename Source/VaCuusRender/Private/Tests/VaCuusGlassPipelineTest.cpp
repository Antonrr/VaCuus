// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusEngine.h"
#include "VaCuusGlassDistiller.h"
#include "VaCuusRecordingRenderInterface.h"

#include "Misc/ScopeExit.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Variant.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * M5 Task 3: the glass distiller and its coordinate mapping (spec §2(a)) — what a
 * published buffer's backdrop signature becomes on the Slate element, that the list is
 * replaced WHOLESALE per publish, that sigma resolves across buffers and retires on
 * release, and that the PIE-shaped mapping (nonzero DestRect.Min) is exact.
 *
 * The distiller is deliberately thread-agnostic data-in/data-out (see its header), so
 * these tests drive it directly with recorder-produced buffers — through a real
 * Rml::Context where the property under test is RmlUi's call sequence, and through
 * direct recorder drives where it is the distiller's own bookkeeping (the idle-gate
 * file's stated split, applied one stage downstream).
 */
namespace VaCuusGlassPipelineTest
{
static const FIntPoint GViewSize(800, 600);

TUniquePtr<FVaCuusCommandBuffer> RecordContextFrame(FVaCuusRecordingRenderInterface& Recorder, Rml::Context* Context)
{
	Recorder.BeginFrame(GViewSize);
	Context->Update();
	Context->Render();
	return Recorder.EndFrameAndPublish();
}

/** Records one frame and, when the gate lets it publish, distills it. Returns whether it published. */
bool RecordAndDistill(FVaCuusRecordingRenderInterface& Recorder, Rml::Context* Context, FVaCuusGlassDistiller& Distiller)
{
	if (const TUniquePtr<FVaCuusCommandBuffer> Buffer = RecordContextFrame(Recorder, Context))
	{
		Distiller.Distill(*Buffer);
		return true;
	}
	return false;
}

/** The axis-aligned bounding box of every vertex position + Translation, rounded to pixels. */
FIntRect ComputeVertexBoundingBox(const FVaCuusGeometryData& Geometry, const FVector2f& Translation)
{
	FVector2f Min(TNumericLimits<float>::Max(), TNumericLimits<float>::Max());
	FVector2f Max(TNumericLimits<float>::Lowest(), TNumericLimits<float>::Lowest());
	for (const FVaCuusVertex& Vertex : Geometry.Vertices)
	{
		const FVector2f Position = Vertex.Position + Translation;
		Min.X = FMath::Min(Min.X, Position.X);
		Min.Y = FMath::Min(Min.Y, Position.Y);
		Max.X = FMath::Max(Max.X, Position.X);
		Max.Y = FMath::Max(Max.Y, Position.Y);
	}
	return FIntRect(FMath::RoundToInt(Min.X), FMath::RoundToInt(Min.Y), FMath::RoundToInt(Max.X), FMath::RoundToInt(Max.Y));
}

/** Every edge of Actual within one pixel of the same edge of Expected. */
bool BoxWithinOnePixel(const FIntRect& Actual, const FIntRect& Expected)
{
	return FMath::Abs(Actual.Min.X - Expected.Min.X) <= 1 && FMath::Abs(Actual.Min.Y - Expected.Min.Y) <= 1 &&
		FMath::Abs(Actual.Max.X - Expected.Max.X) <= 1 && FMath::Abs(Actual.Max.Y - Expected.Max.Y) <= 1;
}
} // namespace VaCuusGlassPipelineTest

/**
 * The reference panel end to end: the Task 2 recorded sequence (BackdropSequence's
 * numbers) distilled into exactly one entry carrying the grab scissor as SampleRegion,
 * the border-box scissor as DrawRegion, sigma verbatim, and an owned copy of the
 * clip-mask geometry at the border-box translation.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassDistillSequenceTest, "VaCuus.Render.Glass.DistillBackdropSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassDistillSequenceTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassPipelineTest;

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
	const Rml::String ContextName("vacuus_glass_distill_test");
	Rml::Context* Context = Rml::CreateContext(ContextName, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(ContextName);
	};

	// The Task 2 reference panel: border box (40,40)-(240,160), blur(12px) -> sigma 12,
	// ink overflow 3*max(12,1) = 36px -> grab scissor (4,4)-(276,196).
	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("#panel{display:block;position:absolute;left:40px;top:40px;width:200px;height:120px;")
		TEXT("border-radius:16px;background-color:#30405080;backdrop-filter:blur(12px);}")
		TEXT("</style></head><body><div id=\"panel\"/></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://glass_distill.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	FVaCuusGlassDistiller Distiller;
	if (!TestTrue(TEXT("The glass frame publishes and distills"), RecordAndDistill(Recorder, Context, Distiller)))
	{
		return false;
	}

	if (!TestEqual(TEXT("Exactly one glass entry"), Distiller.GetEntries().Num(), 1))
	{
		return false;
	}
	const FVaCuusGlassEntry& Entry = Distiller.GetEntries()[0];

	TestTrue(TEXT("SampleRegion is the grab scissor: border box + 3-sigma ink overflow (4,4)-(276,196)"),
		Entry.SampleRegion == FIntRect(4, 4, 276, 196));
	TestTrue(TEXT("DrawRegion is the border box (40,40)-(240,160)"), Entry.DrawRegion == FIntRect(40, 40, 240, 160));
	TestEqual(TEXT("Sigma is FilterBlur's resolved value verbatim"), Entry.Sigma, 12.0f);
	TestTrue(TEXT("The list stores the buffer's ViewSize (the mapping's denominator)"), Distiller.GetViewSize() == GViewSize);

	if (TestTrue(TEXT("The rounded panel carries its clip-mask geometry"), Entry.MaskGeometry.IsValid()))
	{
		TestTrue(TEXT("...a non-empty owned copy"),
			Entry.MaskGeometry->Vertices.Num() > 0 && Entry.MaskGeometry->Indices.Num() > 0);
		TestTrue(TEXT("...at the border-box translation (40,40)"), Entry.MaskTranslation == FVector2f(40.0f, 40.0f));
	}

	return true;
}

/**
 * A glass panel under a transformed ancestor — the usual game-HUD shape of a panel root
 * scaled by transform: scale() around a screen corner. Before the fix the mask carried
 * only the panel's UNSCALED border box, because Distill() never tracked SetTransform, so
 * the mask covered only the part of the scaled panel nearest the transform origin. After
 * the fix the mask vertices land at the scaled box, same as SampleRegion/DrawRegion
 * always did.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassDistillTransformedMaskTest, "VaCuus.Render.Glass.DistillTransformedMask",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassDistillTransformedMaskTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassPipelineTest;

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
	const Rml::String ContextName("vacuus_glass_transform_test");
	Rml::Context* Context = Rml::CreateContext(ContextName, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(ContextName);
	};

	// #panel is the Task 2 reference panel again: border box (40,40)-(240,160),
	// border-radius:12px, blur(12px). #parent sits at the document origin and scales it
	// 1.5x around its own top-left corner (the HUD's own transform-origin choice), so the
	// scale's centre IS the document origin and every corner just multiplies by 1.5:
	//   scaled box = (40*1.5, 40*1.5) .. ((40+200)*1.5, (40+120)*1.5) = (60,60)-(360,240)
	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("#parent{display:block;position:absolute;left:0px;top:0px;width:400px;height:300px;}")
		TEXT("#parent.scaled{transform:scale(1.5);transform-origin:left top;}")
		TEXT("#panel{display:block;position:absolute;left:40px;top:40px;width:200px;height:120px;")
		TEXT("border-radius:12px;background-color:#30405080;backdrop-filter:blur(12px);}")
		TEXT("</style></head><body><div id=\"parent\" class=\"scaled\"><div id=\"panel\"/></div></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://glass_transform.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	Rml::Element* Parent = Document->GetElementById("parent");
	if (!TestNotNull(TEXT("Parent element"), Parent))
	{
		return false;
	}

	// Settle, same as the removal test: a fresh rounded panel publishes more than once
	// (the clip-geometry settle), and every published buffer is distilled as the element
	// would distill it.
	FVaCuusGlassDistiller Distiller;
	for (int32 Settle = 0; Settle < 4 && RecordAndDistill(Recorder, Context, Distiller); ++Settle)
	{
	}

	if (TestEqual(TEXT("Exactly one glass entry under the transformed ancestor"), Distiller.GetEntries().Num(), 1))
	{
		const FVaCuusGlassEntry& Entry = Distiller.GetEntries()[0];
		if (TestTrue(TEXT("The transformed panel still carries its clip-mask geometry"), Entry.MaskGeometry.IsValid()))
		{
			const FIntRect ExpectedBox(60, 60, 360, 240);
			const FIntRect ActualBox = ComputeVertexBoundingBox(*Entry.MaskGeometry, Entry.MaskTranslation);
			TestTrue(FString::Printf(TEXT("Mask vertices land at the SCALED border box %s (got %s)"), *ExpectedBox.ToString(), *ActualBox.ToString()),
				BoxWithinOnePixel(ActualBox, ExpectedBox));
			TestTrue(TEXT("DrawRegion contains the scaled mask box"), Entry.DrawRegion.Contains(ActualBox));
		}
	}

	// THE REGRESSION GUARD: the same panel with #parent's transform removed must still
	// give the UNSCALED box — the identity path (shared ref + MaskTranslation, today's
	// behaviour) stays untouched by this fix.
	Parent->SetClass("scaled", false);
	for (int32 Settle = 0; Settle < 4 && RecordAndDistill(Recorder, Context, Distiller); ++Settle)
	{
	}

	if (TestEqual(TEXT("Exactly one glass entry once the ancestor transform is gone"), Distiller.GetEntries().Num(), 1))
	{
		const FVaCuusGlassEntry& Entry = Distiller.GetEntries()[0];
		if (TestTrue(TEXT("The untransformed panel still carries its clip-mask geometry"), Entry.MaskGeometry.IsValid()))
		{
			const FIntRect ExpectedBox(40, 40, 240, 160);
			const FIntRect ActualBox = ComputeVertexBoundingBox(*Entry.MaskGeometry, Entry.MaskTranslation);
			TestTrue(FString::Printf(TEXT("Mask vertices land at the UNSCALED border box %s (got %s)"), *ExpectedBox.ToString(), *ActualBox.ToString()),
				BoxWithinOnePixel(ActualBox, ExpectedBox));
		}
	}

	return true;
}

/**
 * THE REMOVAL TEST (spec §7, the list-replacement invariant made observable): show glass
 * -> the list has one entry; remove the panel (display:none via class swap) -> the next
 * published buffer distills to an EMPTY list; same for document unload.
 *
 * Restore-the-bug: give Distill() an early-out on glass-free buffers (return before the
 * Entries.Reset() when the buffer records no CompositeLayers) — "the list is empty after
 * removal" fails with the stale entry still in the list, observed via the list-size
 * observable. Verified by doing exactly that; both outcomes in the Task 3 report.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassRemovalTest, "VaCuus.Render.Glass.Removal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassRemovalTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassPipelineTest;

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
	const Rml::String ContextName("vacuus_glass_removal_test");
	Rml::Context* Context = Rml::CreateContext(ContextName, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(ContextName);
	};

	static const TCHAR* Source =
		TEXT("<rml><head><style>")
		TEXT("body{display:block;width:100%;height:100%;}")
		TEXT("#panel{display:block;position:absolute;left:40px;top:40px;width:200px;height:120px;")
		TEXT("border-radius:16px;background-color:#30405080;}")
		// #panel.hidden, not bare .hidden: the #panel rule's display:block is ID-specific
		// (specificity 1-0-0) and would beat a lone class selector — the panel would
		// never hide and the "removal" frame would be withheld as genuinely unchanged.
		// Caught by exactly that happening on the first run.
		TEXT(".glass{backdrop-filter:blur(12px);}")
		TEXT("#panel.hidden{display:none;}")
		TEXT("</style></head><body><div id=\"panel\" class=\"glass\"/></body></rml>");

	Rml::ElementDocument* Document =
		Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(Source)), "vacuus://glass_removal.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();

	Rml::Element* Panel = Document->GetElementById("panel");
	if (!TestNotNull(TEXT("Panel element"), Panel))
	{
		return false;
	}

	// Settle (a fresh rounded panel publishes exactly twice — the clip-geometry settle,
	// VaCuus.Render.Glass.SigmaChangePublishes) with every published buffer distilled,
	// as the element would.
	FVaCuusGlassDistiller Distiller;
	for (int32 Settle = 0; Settle < 4 && RecordAndDistill(Recorder, Context, Distiller); ++Settle)
	{
	}
	if (!TestEqual(TEXT("The shown glass panel distills to one entry"), Distiller.GetEntries().Num(), 1))
	{
		return false;
	}

	// THE REMOVAL: display:none via class swap. The frame's command list changes, so the
	// gate publishes it — and that glass-free buffer must WHOLESALE-replace the list.
	Panel->SetClassNames("glass hidden");
	if (!TestTrue(TEXT("The removal frame publishes"), RecordAndDistill(Recorder, Context, Distiller)))
	{
		return false;
	}
	TestEqual(TEXT("After display:none the glass list is EMPTY (wholesale replacement)"), Distiller.GetEntries().Num(), 0);

	// Re-show, so the unload leg below starts from a live entry again — this also
	// exercises the mask geometry being recompiled after its release (fresh handle,
	// fresh NewGeometry copy).
	Panel->SetClassNames("glass");
	for (int32 Settle = 0; Settle < 4 && RecordAndDistill(Recorder, Context, Distiller); ++Settle)
	{
	}
	if (!TestEqual(TEXT("Re-shown glass distills to one entry again"), Distiller.GetEntries().Num(), 1))
	{
		return false;
	}

	// THE UNLOAD: closing the document releases its resources; the next recorded frame
	// carries that traffic, publishes, and distills to nothing.
	Document->Close();
	if (!TestTrue(TEXT("The unload frame publishes"), RecordAndDistill(Recorder, Context, Distiller)))
	{
		return false;
	}
	TestEqual(TEXT("After document unload the glass list is EMPTY"), Distiller.GetEntries().Num(), 0);

	return true;
}

/**
 * Cross-buffer sigma resolution and retirement, plus the wholesale invariant across
 * publishes and the square (no-mask) entry shape — direct recorder drive, because the
 * property is the distiller's own bookkeeping and pre-minted handles isolate it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassCrossBufferSigmaTest, "VaCuus.Render.Glass.CrossBufferSigma",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassCrossBufferSigmaTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassPipelineTest;

	FVaCuusRecordingRenderInterface Recorder;
	FVaCuusGlassDistiller Distiller;

	const auto RecordGlassComposite = [&Recorder](Rml::CompiledFilterHandle Filter)
	{
		const Rml::LayerHandle Layer = Recorder.PushLayer();
		Recorder.CompositeLayers(Rml::LayerHandle(0), Layer, Rml::BlendMode::Blend,
			Rml::Span<const Rml::CompiledFilterHandle>(&Filter, 1));
		Recorder.CompositeLayers(Layer, Rml::LayerHandle(0), Rml::BlendMode::Blend, {});
		Recorder.PopLayer();
	};

	// Frame 1: compile + composite in one buffer, no scissor -> the scissorless default.
	Recorder.BeginFrame(GViewSize);
	const Rml::CompiledFilterHandle Blur12 =
		Recorder.CompileFilter("blur", Rml::Dictionary{{"sigma", Rml::Variant(12.0f)}});
	TestTrue(TEXT("Blur compiled"), Blur12 != 0);
	RecordGlassComposite(Blur12);
	{
		const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish();
		if (!TestNotNull(TEXT("Frame 1 publishes"), Buffer.Get()))
		{
			return false;
		}
		Distiller.Distill(*Buffer);
	}
	if (!TestEqual(TEXT("Frame 1: one entry"), Distiller.GetEntries().Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("...sigma 12 (same-buffer resolution)"), Distiller.GetEntries()[0].Sigma, 12.0f);
	TestTrue(TEXT("...scissorless grab samples the full view"),
		Distiller.GetEntries()[0].SampleRegion == FIntRect(0, 0, GViewSize.X, GViewSize.Y));
	TestFalse(TEXT("...no clip mask means a square entry"), Distiller.GetEntries()[0].MaskGeometry.IsValid());

	// Frame 2: the SAME handle under a scissor — no filter traffic in this buffer, so the
	// cross-buffer map is the only place sigma can come from.
	Recorder.BeginFrame(GViewSize);
	Recorder.SetScissorRegion(Rml::Rectanglei::FromCorners({10, 10}, {200, 200}));
	RecordGlassComposite(Blur12);
	{
		const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish();
		if (!TestNotNull(TEXT("Frame 2 publishes (the scissor changed)"), Buffer.Get()))
		{
			return false;
		}
		TestEqual(TEXT("Frame 2 carries no filter traffic"), Buffer->NewFilters.Num(), 0);
		Distiller.Distill(*Buffer);
	}
	if (!TestEqual(TEXT("Frame 2: still exactly one entry (wholesale replacement, not accumulation)"),
			Distiller.GetEntries().Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("...sigma 12 resolved CROSS-BUFFER"), Distiller.GetEntries()[0].Sigma, 12.0f);
	TestTrue(TEXT("...at the new frame's scissor"), Distiller.GetEntries()[0].SampleRegion == FIntRect(10, 10, 200, 200));

	// Frame 3: release + reference in the SAME buffer — the parse must still resolve
	// (retirement is deferred past the parse, the replayer's own release rule).
	Recorder.BeginFrame(GViewSize);
	Recorder.ReleaseFilter(Blur12);
	Recorder.SetScissorRegion(Rml::Rectanglei::FromCorners({20, 20}, {180, 180}));
	RecordGlassComposite(Blur12);
	{
		const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish();
		if (!TestNotNull(TEXT("Frame 3 publishes (release traffic)"), Buffer.Get()))
		{
			return false;
		}
		Distiller.Distill(*Buffer);
	}
	TestEqual(TEXT("Frame 3: same-buffer release still resolves for this buffer's composite"),
		Distiller.GetEntries().Num(), 1);

	// Frame 4: the handle is retired; a composite that still names it resolves nothing,
	// produces no entry, and says so once.
	AddExpectedMessagePlain(TEXT("no recorded sigma"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Recorder.BeginFrame(GViewSize);
	Recorder.SetScissorRegion(Rml::Rectanglei::FromCorners({30, 30}, {160, 160}));
	RecordGlassComposite(Blur12);
	{
		const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish();
		if (!TestNotNull(TEXT("Frame 4 publishes (the scissor changed)"), Buffer.Get()))
		{
			return false;
		}
		Distiller.Distill(*Buffer);
	}
	TestEqual(TEXT("Frame 4: a retired sigma produces NO glass entry"), Distiller.GetEntries().Num(), 0);

	return true;
}

/**
 * A filterless layer round-trip (the refused-non-blur shape: the sequence runs, the
 * lists arrive empty — CompiledFilterShader.cpp:6-12) produces no glass entry.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassNoBlurNoEntryTest, "VaCuus.Render.Glass.NoBlurNoEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassNoBlurNoEntryTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusGlassPipelineTest;

	FVaCuusRecordingRenderInterface Recorder;
	FVaCuusGlassDistiller Distiller;

	Recorder.BeginFrame(GViewSize);
	const Rml::LayerHandle Layer = Recorder.PushLayer();
	Recorder.CompositeLayers(Rml::LayerHandle(0), Layer, Rml::BlendMode::Blend, {});
	Recorder.CompositeLayers(Layer, Rml::LayerHandle(0), Rml::BlendMode::Blend, {});
	Recorder.PopLayer();

	const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish();
	if (!TestNotNull(TEXT("The frame publishes"), Buffer.Get()))
	{
		return false;
	}
	Distiller.Distill(*Buffer);

	TestEqual(TEXT("A composite with no blur produces no glass entry"), Distiller.GetEntries().Num(), 0);
	return true;
}

/**
 * THE PIE-SHAPED MAPPING (spec §2(a), §12.1): view-space data drawn at window
 * coordinates works fullscreen and breaks in PIE, where DestRect.Min != 0. Pure math on
 * the mapping the element applies every engine frame — offset = DestRect.Min +
 * ElementsOffset, scale = DestRect.Size / ViewSize on regions AND sigma, SceneViewRect
 * clamp.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusGlassMappingTest, "VaCuus.Render.Glass.MappingPIEShaped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusGlassMappingTest::RunTest(const FString& Parameters)
{
	// PIE-shaped: the view lives at (320,180) inside a 1920x1080 elements texture, with a
	// nonzero elements offset on top, at 1:1 scale.
	{
		const FVaCuusGlassMapping Mapping = VaCuusMakeGlassMapping(
			/*DestRect=*/FIntRect(320, 180, 1280, 720), /*ElementsOffset=*/FVector2f(8.0f, 4.0f),
			/*ViewSize=*/FIntPoint(960, 540), /*SceneViewRect=*/FIntRect(320, 180, 1280, 720),
			/*OutputExtent=*/FIntPoint(1920, 1080));

		TestTrue(TEXT("Scale is 1:1 when DestRect.Size == ViewSize"), Mapping.Scale == FVector2f(1.0f, 1.0f));
		TestTrue(TEXT("Offset is DestRect.Min + ElementsOffset"), Mapping.Offset == FVector2f(328.0f, 184.0f));
		TestTrue(TEXT("A view-space rect lands at the offset window position"),
			Mapping.MapRect(FIntRect(4, 4, 276, 196)) == FIntRect(332, 188, 604, 380));
		// (-50,-50) maps to (278,134) — outside the scene view — so the clamp bites at
		// SceneViewRect.Min (320,180), NOT at the mapping offset (328,184).
		TestTrue(TEXT("A rect poking off the view clamps to SceneViewRect (the PIE clamp)"),
			Mapping.MapRect(FIntRect(-50, -50, 100, 100)) == FIntRect(320, 180, 428, 284));
		TestTrue(TEXT("Sigma is unscaled at 1:1"), Mapping.MapSigma(12.0f) == FVector2f(12.0f, 12.0f));
	}

	// Scaled: the composite stretches a half-size view — regions AND sigma scale with it.
	{
		const FVaCuusGlassMapping Mapping = VaCuusMakeGlassMapping(
			FIntRect(0, 0, 480, 270), FVector2f::ZeroVector, FIntPoint(960, 540), FIntRect(0, 0, 480, 270),
			FIntPoint(1920, 1080));

		TestTrue(TEXT("Half-size DestRect halves the scale"), Mapping.Scale == FVector2f(0.5f, 0.5f));
		TestTrue(TEXT("Regions scale through"), Mapping.MapRect(FIntRect(40, 40, 240, 160)) == FIntRect(20, 20, 120, 80));
		TestTrue(TEXT("Sigma scales with the same factor"), Mapping.MapSigma(12.0f) == FVector2f(6.0f, 6.0f));
	}

	// Degenerate SceneViewRect: the clamp falls back to the output extent, never wider.
	{
		const FVaCuusGlassMapping Mapping = VaCuusMakeGlassMapping(
			FIntRect(0, 0, 960, 540), FVector2f::ZeroVector, FIntPoint(960, 540), FIntRect(0, 0, 0, 0),
			FIntPoint(960, 540));
		TestTrue(TEXT("An empty SceneViewRect clamps to the output extent"),
			Mapping.MapRect(FIntRect(-10, -10, 2000, 2000)) == FIntRect(0, 0, 960, 540));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
