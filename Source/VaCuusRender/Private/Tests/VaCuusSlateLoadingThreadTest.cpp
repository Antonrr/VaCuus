// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "SVaCuusWidget.h"
#include "VaCuus.h"
#include "VaCuusEngine.h"
#include "VaCuusSlateElement.h"
#include "VaCuusStats.h"
#include "VaCuusSubsystem.h"
#include "VaCuusTestDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"
#include "VaCuusViewStatus.h"

#include "Engine/GameInstance.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Thread.h"
#include "Input/HittestGrid.h"
#include "Layout/Geometry.h"
#include "Misc/ScopeExit.h"
#include "Rendering/DrawElements.h"
#include "RenderingThread.h"
#include "Types/PaintArgs.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SWindow.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A WIDGET PAINTED BY THE SLATE LOADING THREAD MUST SURVIVE IT, AND KEEP COMPOSITING.
 *
 * The configuration this pins is the engine's stock loading screen: a widget handed to the
 * movie player as FLoadingScreenAttributes::WidgetLoadingScreen (or to PreLoadScreen) is painted
 * by a dedicated thread while the game thread is inside LoadMap, and SWidget::Paint ticks it on
 * the way down (SWidget.cpp:1505-1511). SVaCuusWidget::Tick used to run in full there and died
 * on the first game-thread assertion it met; SVaCuusWidget::Tick's comment carries the engine
 * path and the rest of the argument.
 *
 * THE THREAD IS NOT A MOCK OF THE MOVIE PLAYER, it is the two facts the engine's own loading
 * thread establishes before it paints, reproduced in the same order: it claims
 * GSlateLoadingThreadId (MoviePlayerThreading.cpp:195) and tags itself ESlateThread
 * (MoviePlayerThreading.cpp:208), and it releases the id when it leaves (:220). Those two are
 * what IsInSlateThread() and IsInGameThread() read (ThreadingBase.cpp:190-207, :227-242), and
 * the test asserts both answers before it trusts the rest. The movie player itself cannot run
 * here: an editor has no game-mode movie player to hand a widget to.
 *
 * WHAT IS ASSERTED, and why each one:
 *  - the loading-thread paint returns and still emits the view's custom element -- the composite
 *    is what a loading screen is for;
 *  - it queues no resize, although it paints at a different size -- the queue behind Resize()
 *    has one producer (VaCuusUIQueues.h:328-330);
 *  - it leaves the OnPaint scope's last sample alone -- a game-thread budget row with one
 *    writing thread (VaCuusStats.cpp:82, :202-203);
 *  - the next GAME-thread paint of the same geometry sends that size and samples OnPaint -- the
 *    skip latched nothing and is specific to the thread.
 *
 * RESTORE-THE-BUG: delete the IsInGameThread() early return from SVaCuusWidget::Tick. The
 * loading-thread paint then dies inside the test at `check(IsInGameThread())` in
 * UVaCuusView::Resize (VaCuusView.cpp:328) -- a fatal assertion, so the run stops there rather
 * than reporting a failed assertion. Delete the one in SVaCuusWidget::OnPaint instead and the
 * run completes with exactly the OnPaint-sample assertion failing.
 */
namespace VaCuusSlateLoadingThreadTest
{
/**
 * A probe that only has to report the size the UI thread applied. The base stores it in
 * SetViewSize, on the UI thread; the base's thread hand-off rule makes reading it after
 * WaitForFrameCount() sound.
 */
class FProbeHost final : public FVaCuusTestDocumentHost
{
public:
	FProbeHost()
		: FVaCuusTestDocumentHost(TEXT("vacuus_loading_thread_view"), "vacuus://loading_thread.rml", Rml::FocusFlag::Document)
	{
	}

	virtual void SetVisible(bool /*bVisible*/) override
	{
		check(FVaCuusUIThread::IsInUIThread());
	}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());
		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	FIntPoint GetAppliedViewSize() const { return ViewSize; }
};

/** One UI frame at a time; the wake event coalesces, so N triggers are not N frames. */
static bool RunFrames(FVaCuusUIThread& UIThread, int32 NumFrames)
{
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		const uint64 Before = UIThread.GetFrameCount();
		UIThread.Trigger();
		if (!UIThread.WaitForFrameCount(Before + 1, 5.0))
		{
			return false;
		}
	}

	return true;
}

static const TCHAR* GDocument = TEXT(R"(<rml>
<head>
<style>
body { display: block; width: 100%; height: 100%; }
</style>
</head>
<body/>
</rml>)");

/** What the loading thread saw, read by the test thread only after Join(). */
struct FLoadingPaintResult
{
	bool bClaimedThreadId = false;
	bool bWasSlateThread = false;
	bool bWasGameThread = true;
	int32 NumCustomElements = -1;
};

static int32 CountCustomElements(const FSlateWindowElementList& Elements)
{
	return Elements.GetUncachedDrawElements().Get<(uint8)EElementType::ET_Custom>().Num();
}
} // namespace VaCuusSlateLoadingThreadTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusSlateLoadingThreadTest, "VaCuus.Threading.SlateLoadingThread",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusSlateLoadingThreadTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusSlateLoadingThreadTest;

	// SWidget::Paint asks the Slate renderer for its scene index, so a renderer is required, not
	// only an application.
	if (!FSlateApplication::IsInitialized() || FSlateApplication::Get().GetRenderer() == nullptr ||
		!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: needs an FSlateApplication with a renderer to paint against and a worker thread to drive"));
		return true;
	}

	if (!TestTrue(TEXT("No Slate loading thread is running before the test"), GSlateLoadingThreadId == 0))
	{
		return false;
	}

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	// The loading screen paints at a size the view has not been told about, which is what makes
	// a skipped resize observable.
	const FIntPoint InitialSize(400, 300);
	const FIntPoint PaintedSize(640, 480);
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();

	TUniquePtr<FProbeHost> OwnedHost = MakeUnique<FProbeHost>();
	FProbeHost* Host = OwnedHost.Get();

	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MoveTemp(OwnedHost), InitialSize, Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GDocument, /*LoadSerial=*/1);

	// Wired by hand, as in VaCuus.Input.SlateRouting.
	TStrongObjectPtr<UGameInstance> GameInstance(NewObject<UGameInstance>());
	TStrongObjectPtr<UVaCuusSubsystem> Subsystem(NewObject<UVaCuusSubsystem>(GameInstance.Get()));
	TStrongObjectPtr<UVaCuusView> View(NewObject<UVaCuusView>(Subsystem.Get()));
	View->InitializeView(Subsystem.Get(), ViewId, Status, InitialSize);

	const TSharedRef<FVaCuusSlateElement> Element = MakeShared<FVaCuusSlateElement>();
	const TSharedRef<SVaCuusWidget> Widget = SNew(SVaCuusWidget, View.Get(), Element);

	if (!TestTrue(TEXT("UI frames ran"), RunFrames(*UIThread, 2)))
	{
		return false;
	}
	if (!TestTrue(TEXT("Document loaded"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1 &&
				Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return false;
	}
	if (!TestEqual(TEXT("The view starts at its initial size"), Host->GetAppliedViewSize().ToString(), InitialSize.ToString()))
	{
		return false;
	}

	// Never shown: the element lists only need a window to belong to.
	const TSharedRef<SWindow> Window = SNew(SWindow);
	const FGeometry Geometry = FGeometry::MakeRoot(FVector2f(PaintedSize.X, PaintedSize.Y), FSlateLayoutTransform());
	const FSlateRect CullingRect(0.f, 0.f, PaintedSize.X, PaintedSize.Y);

	// A value no real sample can take, so "untouched" is a plain equality. Written from this
	// thread, the one that owns the slot. With PerfLog off (the default) AddSample stores the
	// slot and nothing else (VaCuusStats.cpp:226, :237-240).
	const double UntouchedSampleMs = -1.0;
	FVaCuusPerfLog::AddSample(FVaCuusPerfLog::OnPaint, UntouchedSampleMs);

	FLoadingPaintResult Result;
	FSlateWindowElementList LoadingElements(Window);
	{
		FThread LoadingThread(TEXT("VaCuusTestSlateLoadingThread"),
			[&Result, &LoadingElements, Widget, Geometry, CullingRect]()
			{
				const int32 ThreadId = static_cast<int32>(FPlatformTLS::GetCurrentThreadId());
				if (FPlatformAtomics::InterlockedCompareExchange(reinterpret_cast<int32*>(&GSlateLoadingThreadId),
						ThreadId, 0) != 0)
				{
					return;
				}
				Result.bClaimedThreadId = true;

				{
					FTaskTagScope Scope(ETaskTag::ESlateThread);
					Result.bWasSlateThread = IsInSlateThread();
					Result.bWasGameThread = IsInGameThread();

					FHittestGrid HittestGrid;
					const FPaintArgs PaintArgs(nullptr, HittestGrid, FVector2D::ZeroVector, FPlatformTime::Seconds(), 1.f / 60.f);
					Widget->Paint(PaintArgs, Geometry, CullingRect, LoadingElements, 0, FWidgetStyle(), true);
					Result.NumCustomElements = CountCustomElements(LoadingElements);
				}

				FPlatformAtomics::InterlockedCompareExchange(reinterpret_cast<int32*>(&GSlateLoadingThreadId), 0,
					ThreadId);
			});
		LoadingThread.Join();
	}

	if (!TestTrue(TEXT("The paint ran on a thread the engine reports as the Slate loading thread, not the game thread"),
			Result.bClaimedThreadId && Result.bWasSlateThread && !Result.bWasGameThread))
	{
		return false;
	}

	// The render command the loading-thread paint enqueued, run before the view is driven again.
	FlushRenderingCommands();

	TestEqual(TEXT("The loading thread still emits the view's custom element"), Result.NumCustomElements, 1);
	TestEqual(TEXT("The loading thread leaves the game-thread OnPaint sample alone"),
		FVaCuusPerfLog::GetLastSampleMs(FVaCuusPerfLog::OnPaint), UntouchedSampleMs);

	if (!TestTrue(TEXT("UI frames ran after the loading-thread paint"), RunFrames(*UIThread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("The loading thread's tick queued no resize"), Host->GetAppliedViewSize().ToString(), InitialSize.ToString());

	// The same paint on the game thread.
	FSlateWindowElementList GameElements(Window);
	FHittestGrid GameHittestGrid;
	const FPaintArgs GamePaintArgs(nullptr, GameHittestGrid, FVector2D::ZeroVector, FPlatformTime::Seconds(), 1.f / 60.f);
	Widget->Paint(GamePaintArgs, Geometry, CullingRect, GameElements, 0, FWidgetStyle(), true);
	FlushRenderingCommands();

	TestEqual(TEXT("The game thread emits the element too"), CountCustomElements(GameElements), 1);
	TestNotEqual(TEXT("The game thread samples OnPaint"), FVaCuusPerfLog::GetLastSampleMs(FVaCuusPerfLog::OnPaint),
		UntouchedSampleMs);

	if (!TestTrue(TEXT("UI frames ran after the game-thread paint"), RunFrames(*UIThread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("The next game-thread tick sends the size the loading thread skipped"),
		Host->GetAppliedViewSize().ToString(), PaintedSize.ToString());

	// Torn down as VaCuus.Input.SlateRouting does it: the widget lets go of the view first.
	Widget->DetachView();
	UIThread->EnqueueRemoveView(ViewId);
	TestTrue(TEXT("UI frames survive the removal"), RunFrames(*UIThread, 2));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
