/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#undef MOZ_CRASHREPORTER

#include "nsThread.h"

#ifndef GECKO_MEDIA_CRATE
#include "base/message_loop.h"

// Chromium's logging can sometimes leak through...
#ifdef LOG
#undef LOG
#endif
#endif // ifndef GECKO_MEDIA_CRATE

#include "mozilla/ReentrantMonitor.h"
#include "nsMemoryPressure.h"
#include "nsThreadManager.h"
#include "nsIClassInfoImpl.h"
#include "nsAutoPtr.h"
#include "nsCOMPtr.h"
#include "nsQueryObject.h"
#include "pratom.h"
#include "mozilla/Logging.h"
#include "nsIObserverService.h"
#include "mozilla/HangMonitor.h"
#include "mozilla/IOInterposer.h"
#include "mozilla/Scheduler.h"
#include "mozilla/SchedulerGroup.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Services.h"
#include "mozilla/SystemGroup.h"
#include "nsXPCOMPrivate.h"
#include "mozilla/ChaosMode.h"
#include "mozilla/Telemetry.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Unused.h"
#include "nsThreadSyncDispatch.h"
#include "InputEventStatistics.h"
#include "ThreadEventTarget.h"

#ifndef GECKO_MEDIA_CRATE
#include "mozilla/CycleCollectedJSContext.h"
#include "mozilla/ipc/MessageChannel.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "mozilla/dom/ScriptSettings.h"
#include "GeckoProfiler.h"
#endif

#ifdef MOZ_CRASHREPORTER
#include "nsServiceManagerUtils.h"
#include "nsICrashReporter.h"
#include "mozilla/dom/ContentChild.h"
#endif

#ifdef XP_LINUX
#include <sys/time.h>
#include <sys/resource.h>
#include <sched.h>
#endif

#define HAVE_UALARM _BSD_SOURCE || (_XOPEN_SOURCE >= 500 ||                 \
                      _XOPEN_SOURCE && _XOPEN_SOURCE_EXTENDED) &&           \
                      !(_POSIX_C_SOURCE >= 200809L || _XOPEN_SOURCE >= 700)

#if defined(XP_LINUX) && !defined(ANDROID) && defined(_GNU_SOURCE)
#define HAVE_SCHED_SETAFFINITY
#endif

#ifdef XP_MACOSX
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif

#ifdef MOZ_CANARY
# include <unistd.h>
# include <execinfo.h>
# include <signal.h>
# include <fcntl.h>
# include "nsXULAppAPI.h"
#endif

#if defined(NS_FUNCTION_TIMER) && defined(_MSC_VER)
#include "nsTimerImpl.h"
#include "mozilla/StackWalk.h"
#endif
#ifdef NS_FUNCTION_TIMER
#include "nsCRT.h"
#endif

#ifdef MOZ_TASK_TRACER
#include "GeckoTaskTracer.h"
#include "TracedTaskCommon.h"
using namespace mozilla::tasktracer;
#endif

using namespace mozilla;

static LazyLogModule sThreadLog("nsThread");
#ifdef LOG
#undef LOG
#endif
#define LOG(args) MOZ_LOG(sThreadLog, mozilla::LogLevel::Debug, args)

NS_DECL_CI_INTERFACE_GETTER(nsThread)

Array<char, nsThread::kRunnableNameBufSize> nsThread::sMainThreadRunnableName;

//-----------------------------------------------------------------------------
// Because we do not have our own nsIFactory, we have to implement nsIClassInfo
// somewhat manually.

class nsThreadClassInfo : public nsIClassInfo
{
public:
  NS_DECL_ISUPPORTS_INHERITED  // no mRefCnt
  NS_DECL_NSICLASSINFO

  nsThreadClassInfo()
  {
  }
};

NS_IMETHODIMP_(MozExternalRefCountType)
nsThreadClassInfo::AddRef()
{
  return 2;
}
NS_IMETHODIMP_(MozExternalRefCountType)
nsThreadClassInfo::Release()
{
  return 1;
}
NS_IMPL_QUERY_INTERFACE(nsThreadClassInfo, nsIClassInfo)

NS_IMETHODIMP
nsThreadClassInfo::GetInterfaces(uint32_t* aCount, nsIID*** aArray)
{
  return NS_CI_INTERFACE_GETTER_NAME(nsThread)(aCount, aArray);
}

NS_IMETHODIMP
nsThreadClassInfo::GetScriptableHelper(nsIXPCScriptable** aResult)
{
  *aResult = nullptr;
  return NS_OK;
}

NS_IMETHODIMP
nsThreadClassInfo::GetContractID(nsACString& aResult)
{
  aResult.SetIsVoid(true);
  return NS_OK;
}

NS_IMETHODIMP
nsThreadClassInfo::GetClassDescription(nsACString& aResult)
{
  aResult.SetIsVoid(true);
  return NS_OK;
}

NS_IMETHODIMP
nsThreadClassInfo::GetClassID(nsCID** aResult)
{
  *aResult = nullptr;
  return NS_OK;
}

NS_IMETHODIMP
nsThreadClassInfo::GetFlags(uint32_t* aResult)
{
  *aResult = THREADSAFE;
  return NS_OK;
}

NS_IMETHODIMP
nsThreadClassInfo::GetClassIDNoAlloc(nsCID* aResult)
{
  return NS_ERROR_NOT_AVAILABLE;
}

//-----------------------------------------------------------------------------

NS_IMPL_ADDREF(nsThread)
NS_IMPL_RELEASE(nsThread)
NS_INTERFACE_MAP_BEGIN(nsThread)
  NS_INTERFACE_MAP_ENTRY(nsIThread)
  NS_INTERFACE_MAP_ENTRY(nsIThreadInternal)
  NS_INTERFACE_MAP_ENTRY(nsIEventTarget)
  NS_INTERFACE_MAP_ENTRY(nsISerialEventTarget)
  NS_INTERFACE_MAP_ENTRY(nsISupportsPriority)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIThread)
  if (aIID.Equals(NS_GET_IID(nsIClassInfo))) {
    static nsThreadClassInfo sThreadClassInfo;
    foundInterface = static_cast<nsIClassInfo*>(&sThreadClassInfo);
  } else
NS_INTERFACE_MAP_END
NS_IMPL_CI_INTERFACE_GETTER(nsThread, nsIThread, nsIThreadInternal,
                            nsIEventTarget, nsISupportsPriority)

//-----------------------------------------------------------------------------

class nsThreadStartupEvent : public Runnable
{
public:
  nsThreadStartupEvent()
    : Runnable("nsThreadStartupEvent")
    , mMon("nsThreadStartupEvent.mMon")
    , mInitialized(false)
  {
  }

  // This method does not return until the thread startup object is in the
  // completion state.
  void Wait()
  {
    ReentrantMonitorAutoEnter mon(mMon);
    while (!mInitialized) {
      mon.Wait();
    }
  }

  // This method needs to be public to support older compilers (xlC_r on AIX).
  // It should be called directly as this class type is reference counted.
  virtual ~nsThreadStartupEvent() {}

private:
  NS_IMETHOD Run() override
  {
    ReentrantMonitorAutoEnter mon(mMon);
    mInitialized = true;
    mon.Notify();
    return NS_OK;
  }

  ReentrantMonitor mMon;
  bool mInitialized;
};
//-----------------------------------------------------------------------------

struct nsThreadShutdownContext
{
  nsThreadShutdownContext(NotNull<nsThread*> aTerminatingThread,
                          NotNull<nsThread*> aJoiningThread,
                          bool      aAwaitingShutdownAck)
    : mTerminatingThread(aTerminatingThread)
    , mJoiningThread(aJoiningThread)
    , mAwaitingShutdownAck(aAwaitingShutdownAck)
    , mIsMainThreadJoining(NS_IsMainThread())
  {
    MOZ_COUNT_CTOR(nsThreadShutdownContext);
  }
  ~nsThreadShutdownContext()
  {
    MOZ_COUNT_DTOR(nsThreadShutdownContext);
  }

  // NB: This will be the last reference.
  NotNull<RefPtr<nsThread>> mTerminatingThread;
  NotNull<nsThread*> MOZ_UNSAFE_REF("Thread manager is holding reference to joining thread")
    mJoiningThread;
  bool mAwaitingShutdownAck;
  bool mIsMainThreadJoining;
};

// This event is responsible for notifying nsThread::Shutdown that it is time
// to call PR_JoinThread. It implements nsICancelableRunnable so that it can
// run on a DOM Worker thread (where all events must implement
// nsICancelableRunnable.)
class nsThreadShutdownAckEvent : public CancelableRunnable
{
public:
  explicit nsThreadShutdownAckEvent(NotNull<nsThreadShutdownContext*> aCtx)
    : CancelableRunnable("nsThreadShutdownAckEvent")
    , mShutdownContext(aCtx)
  {
  }
  NS_IMETHOD Run() override
  {
    mShutdownContext->mTerminatingThread->ShutdownComplete(mShutdownContext);
    return NS_OK;
  }
  nsresult Cancel() override
  {
    return Run();
  }
private:
  virtual ~nsThreadShutdownAckEvent() { }

  NotNull<nsThreadShutdownContext*> mShutdownContext;
};

// This event is responsible for setting mShutdownContext
class nsThreadShutdownEvent : public Runnable
{
public:
  nsThreadShutdownEvent(NotNull<nsThread*> aThr,
                        NotNull<nsThreadShutdownContext*> aCtx)
    : Runnable("nsThreadShutdownEvent")
    , mThread(aThr)
    , mShutdownContext(aCtx)
  {
  }
  NS_IMETHOD Run() override
  {
    mThread->mShutdownContext = mShutdownContext;
    // MessageLoop::current()->Quit();
    return NS_OK;
  }
private:
  NotNull<RefPtr<nsThread>> mThread;
  NotNull<nsThreadShutdownContext*> mShutdownContext;
};

//-----------------------------------------------------------------------------

static void
SetThreadAffinity(unsigned int cpu)
{
#ifdef HAVE_SCHED_SETAFFINITY
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  CPU_SET(cpu, &cpus);
  sched_setaffinity(0, sizeof(cpus), &cpus);
  // Don't assert sched_setaffinity's return value because it intermittently (?)
  // fails with EINVAL on Linux x64 try runs.
#elif defined(XP_MACOSX)
  // OS X does not provide APIs to pin threads to specific processors, but you
  // can tag threads as belonging to the same "affinity set" and the OS will try
  // to run them on the same processor. To run threads on different processors,
  // tag them as belonging to different affinity sets. Tag 0, the default, means
  // "no affinity" so let's pretend each CPU has its own tag `cpu+1`.
  thread_affinity_policy_data_t policy;
  policy.affinity_tag = cpu + 1;
  MOZ_ALWAYS_TRUE(thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY,
                                    &policy.affinity_tag, 1) == KERN_SUCCESS);
#elif defined(XP_WIN)
  MOZ_ALWAYS_TRUE(SetThreadIdealProcessor(GetCurrentThread(), cpu) != (DWORD)-1);
#endif
}

static void
SetupCurrentThreadForChaosMode()
{
  if (!ChaosMode::isActive(ChaosFeature::ThreadScheduling)) {
    return;
  }

#ifdef XP_LINUX
  // PR_SetThreadPriority doesn't really work since priorities >
  // PR_PRIORITY_NORMAL can't be set by non-root users. Instead we'll just use
  // setpriority(2) to set random 'nice values'. In regular Linux this is only
  // a dynamic adjustment so it still doesn't really do what we want, but tools
  // like 'rr' can be more aggressive about honoring these values.
  // Some of these calls may fail due to trying to lower the priority
  // (e.g. something may have already called setpriority() for this thread).
  // This makes it hard to have non-main threads with higher priority than the
  // main thread, but that's hard to fix. Tools like rr can choose to honor the
  // requested values anyway.
  // Use just 4 priorities so there's a reasonable chance of any two threads
  // having equal priority.
  setpriority(PRIO_PROCESS, 0, ChaosMode::randomUint32LessThan(4));
#else
  // We should set the affinity here but NSPR doesn't provide a way to expose it.
  uint32_t priority = ChaosMode::randomUint32LessThan(PR_PRIORITY_LAST + 1);
  PR_SetThreadPriority(PR_GetCurrentThread(), PRThreadPriority(priority));
#endif

  // Force half the threads to CPU 0 so they compete for CPU
  if (ChaosMode::randomUint32LessThan(2)) {
    SetThreadAffinity(0);
  }
}

namespace {

struct ThreadInitData {
  nsThread* thread;
  const nsACString& name;
};

}

/*static*/ void
nsThread::ThreadFunc(void* aArg)
{
#ifndef GECKO_MEDIA_CRATE
  using mozilla::ipc::BackgroundChild;

  char stackTop;
#endif

  ThreadInitData* initData = static_cast<ThreadInitData*>(aArg);
  nsThread* self = initData->thread;  // strong reference

  self->mThread = PR_GetCurrentThread();
  self->mVirtualThread = GetCurrentVirtualThread();
  self->mEventTarget->SetCurrentThread();
  SetupCurrentThreadForChaosMode();

  if (!initData->name.IsEmpty()) {
    NS_SetCurrentThreadName(initData->name.BeginReading());
  }

  // Inform the ThreadManager
  nsThreadManager::get().RegisterCurrentThread(*self);

#ifndef GECKO_MEDIA_CRATE
  mozilla::IOInterposer::RegisterCurrentThread();

  // This must come after the call to nsThreadManager::RegisterCurrentThread(),
  // because that call is needed to properly set up this thread as an nsThread,
  // which profiler_register_thread() requires. See bug 1347007.
  if (!initData->name.IsEmpty()) {
    profiler_register_thread(initData->name.BeginReading(), &stackTop);
  }
#endif

  // Wait for and process startup event
  nsCOMPtr<nsIRunnable> event = self->mEvents->GetEvent(true, nullptr);
  MOZ_ASSERT(event);

  initData = nullptr; // clear before unblocking nsThread::Init

  event->Run();  // unblocks nsThread::Init
  event = nullptr;

#ifndef GECKO_MEDIA_CRATE
  {
    // Scope for MessageLoop.
    nsAutoPtr<MessageLoop> loop(
      new MessageLoop(MessageLoop::TYPE_MOZILLA_NONMAINTHREAD, self));

    // Now, process incoming events...
    loop->Run();

    BackgroundChild::CloseForCurrentThread();

    // NB: The main thread does not shut down here!  It shuts down via
    // nsThreadManager::Shutdown.

    // Do NS_ProcessPendingEvents but with special handling to set
    // mEventsAreDoomed atomically with the removal of the last event. The key
    // invariant here is that we will never permit PutEvent to succeed if the
    // event would be left in the queue after our final call to
    // NS_ProcessPendingEvents. We also have to keep processing events as long
    // as we have outstanding mRequestedShutdownContexts.
    while (true) {
      // Check and see if we're waiting on any threads.
      self->WaitForAllAsynchronousShutdowns();

      if (self->mEvents->ShutdownIfNoPendingEvents()) {
        break;
      }
      NS_ProcessPendingEvents(self);
    }
  }

  mozilla::IOInterposer::UnregisterCurrentThread();
#else
  self->mShutdownNow = false;
  while (!self->mShutdownNow) {
    NS_ProcessPendingEvents(self);
  }
#endif

  // Inform the threadmanager that this thread is going away
  nsThreadManager::get().UnregisterCurrentThread(*self);

#ifndef GECKO_MEDIA_CRATE
  profiler_unregister_thread();
  // Dispatch shutdown ACK
  NotNull<nsThreadShutdownContext*> context =
    WrapNotNull(self->mShutdownContext);
  MOZ_ASSERT(context->mTerminatingThread == self);
  event = do_QueryObject(new nsThreadShutdownAckEvent(context));
  if (context->mIsMainThreadJoining) {
    SystemGroup::Dispatch(TaskCategory::Other, event.forget());
  } else {
    context->mJoiningThread->Dispatch(event, NS_DISPATCH_NORMAL);
  }
#endif

  // Release any observer of the thread here.
  self->SetObserver(nullptr);

#ifdef MOZ_TASK_TRACER
  FreeTraceInfo();
#endif

  NS_RELEASE(self);
}

//-----------------------------------------------------------------------------

#ifdef MOZ_CRASHREPORTER
// Tell the crash reporter to save a memory report if our heuristics determine
// that an OOM failure is likely to occur soon.
// Memory usage will not be checked more than every 30 seconds or saved more
// than every 3 minutes
// If |aShouldSave == kForceReport|, a report will be saved regardless of
// whether the process is low on memory or not. However, it will still not be
// saved if a report was saved less than 3 minutes ago.
bool
nsThread::SaveMemoryReportNearOOM(ShouldSaveMemoryReport aShouldSave)
{
  // Keep an eye on memory usage (cheap, ~7ms) somewhat frequently,
  // but save memory reports (expensive, ~75ms) less frequently.
  const size_t kLowMemoryCheckSeconds = 30;
  const size_t kLowMemorySaveSeconds = 3 * 60;

  static TimeStamp nextCheck = TimeStamp::NowLoRes()
    + TimeDuration::FromSeconds(kLowMemoryCheckSeconds);
  static bool recentlySavedReport = false; // Keeps track of whether a report
                                           // was saved last time we checked

  // Are we checking again too soon?
  TimeStamp now = TimeStamp::NowLoRes();
  if ((aShouldSave == ShouldSaveMemoryReport::kMaybeReport ||
      recentlySavedReport) && now < nextCheck) {
    return false;
  }

  bool needMemoryReport = (aShouldSave == ShouldSaveMemoryReport::kForceReport);
#ifdef XP_WIN // XXX implement on other platforms as needed
  // If the report is forced there is no need to check whether it is necessary
  if (aShouldSave != ShouldSaveMemoryReport::kForceReport) {
    const size_t LOWMEM_THRESHOLD_VIRTUAL = 200 * 1024 * 1024;
    MEMORYSTATUSEX statex;
    statex.dwLength = sizeof(statex);
    if (GlobalMemoryStatusEx(&statex)) {
      if (statex.ullAvailVirtual < LOWMEM_THRESHOLD_VIRTUAL) {
        needMemoryReport = true;
      }
    }
  }
#endif

  if (needMemoryReport) {
    if (XRE_IsContentProcess()) {
      dom::ContentChild* cc = dom::ContentChild::GetSingleton();
      if (cc) {
        cc->SendNotifyLowMemory();
      }
    } else {
      nsCOMPtr<nsICrashReporter> cr =
        do_GetService("@mozilla.org/toolkit/crash-reporter;1");
      if (cr) {
        cr->SaveMemoryReport();
      }
    }
    recentlySavedReport = true;
    nextCheck = now + TimeDuration::FromSeconds(kLowMemorySaveSeconds);
  } else {
    recentlySavedReport = false;
    nextCheck = now + TimeDuration::FromSeconds(kLowMemoryCheckSeconds);
  }

  return recentlySavedReport;
}
#endif

#ifdef MOZ_CANARY
int sCanaryOutputFD = -1;
#endif

nsThread::nsThread(NotNull<SynchronizedEventQueue*> aQueue,
                   MainThreadFlag aMainThread,
                   uint32_t aStackSize)
  : mEvents(aQueue.get())
  , mEventTarget(new ThreadEventTarget(mEvents.get(), aMainThread == MAIN_THREAD))
  , mScriptObserver(nullptr)
  , mPriority(PRIORITY_NORMAL)
  , mThread(nullptr)
  , mNestedEventLoopDepth(0)
  , mStackSize(aStackSize)
  , mShutdownContext(nullptr)
  , mShutdownRequired(false)
  , mIsMainThread(aMainThread)
  , mLastUnlabeledRunnable(TimeStamp::Now())
  , mCanInvokeJS(false)
#ifdef GECKO_MEDIA_CRATE
  , mShutdownNow(false)
#endif
{
}

nsThread::~nsThread()
{
  NS_ASSERTION(mRequestedShutdownContexts.IsEmpty(),
               "shouldn't be waiting on other threads to shutdown");
#ifdef DEBUG
  // We deliberately leak these so they can be tracked by the leak checker.
  // If you're having nsThreadShutdownContext leaks, you can set:
  //   XPCOM_MEM_LOG_CLASSES=nsThreadShutdownContext
  // during a test run and that will at least tell you what thread is
  // requesting shutdown on another, which can be helpful for diagnosing
  // the leak.
  for (size_t i = 0; i < mRequestedShutdownContexts.Length(); ++i) {
    Unused << mRequestedShutdownContexts[i].forget();
  }
#endif
}

nsresult
nsThread::Init(const nsACString& aName)
{
  // spawn thread and wait until it is fully setup
  RefPtr<nsThreadStartupEvent> startup = new nsThreadStartupEvent();

  NS_ADDREF_THIS();

  mShutdownRequired = true;

  ThreadInitData initData = { this, aName };

  // ThreadFunc is responsible for setting mThread
  if (!PR_CreateThread(PR_USER_THREAD, ThreadFunc, &initData,
                       PR_PRIORITY_NORMAL, PR_GLOBAL_THREAD,
                       PR_JOINABLE_THREAD, mStackSize)) {
    NS_RELEASE_THIS();
    return NS_ERROR_OUT_OF_MEMORY;
  }

  // ThreadFunc will wait for this event to be run before it tries to access
  // mThread.  By delaying insertion of this event into the queue, we ensure
  // that mThread is set properly.
  {
    mEvents->PutEvent(do_AddRef(startup), EventPriority::Normal); // retain a reference
  }

  // Wait for thread to call ThreadManager::SetupCurrentThread, which completes
  // initialization of ThreadFunc.
  startup->Wait();
  return NS_OK;
}

nsresult
nsThread::InitCurrentThread()
{
  mThread = PR_GetCurrentThread();
  mVirtualThread = GetCurrentVirtualThread();
  SetupCurrentThreadForChaosMode();

  nsThreadManager::get().RegisterCurrentThread(*this);
  return NS_OK;
}

//-----------------------------------------------------------------------------
// nsIEventTarget

NS_IMETHODIMP
nsThread::DispatchFromScript(nsIRunnable* aEvent, uint32_t aFlags)
{
  nsCOMPtr<nsIRunnable> event(aEvent);
  return mEventTarget->Dispatch(event.forget(), aFlags);
}

NS_IMETHODIMP
nsThread::Dispatch(already_AddRefed<nsIRunnable> aEvent, uint32_t aFlags)
{
  LOG(("THRD(%p) Dispatch [%p %x]\n", this, /* XXX aEvent */nullptr, aFlags));

  return mEventTarget->Dispatch(Move(aEvent), aFlags);
}

NS_IMETHODIMP
nsThread::DelayedDispatch(already_AddRefed<nsIRunnable> aEvent, uint32_t aDelayMs)
{
  return mEventTarget->DelayedDispatch(Move(aEvent), aDelayMs);
}

NS_IMETHODIMP
nsThread::IsOnCurrentThread(bool* aResult)
{
  return mEventTarget->IsOnCurrentThread(aResult);
}

NS_IMETHODIMP_(bool)
nsThread::IsOnCurrentThreadInfallible()
{
  // Rely on mVirtualThread being correct.
  MOZ_CRASH("IsOnCurrentThreadInfallible should never be called on nsIThread");
}

//-----------------------------------------------------------------------------
// nsIThread

NS_IMETHODIMP
nsThread::GetPRThread(PRThread** aResult)
{
  *aResult = mThread;
  return NS_OK;
}

NS_IMETHODIMP
nsThread::GetCanInvokeJS(bool* aResult)
{
  *aResult = mCanInvokeJS;
  return NS_OK;
}

NS_IMETHODIMP
nsThread::SetCanInvokeJS(bool aCanInvokeJS)
{
  mCanInvokeJS = aCanInvokeJS;
  return NS_OK;
}

NS_IMETHODIMP
nsThread::AsyncShutdown()
{
  LOG(("THRD(%p) async shutdown\n", this));

  // XXX If we make this warn, then we hit that warning at xpcom shutdown while
  //     shutting down a thread in a thread pool.  That happens b/c the thread
  //     in the thread pool is already shutdown by the thread manager.
  if (!mThread) {
    return NS_OK;
  }

  return !!ShutdownInternal(/* aSync = */ false) ? NS_OK : NS_ERROR_UNEXPECTED;
}

nsThreadShutdownContext*
nsThread::ShutdownInternal(bool aSync)
{
  MOZ_ASSERT(mThread);
  MOZ_ASSERT(mThread != PR_GetCurrentThread());
  if (NS_WARN_IF(mThread == PR_GetCurrentThread())) {
    return nullptr;
  }

  // Prevent multiple calls to this method
  if (!mShutdownRequired.compareExchange(true, false)) {
    return nullptr;
  }

  NotNull<nsThread*> currentThread =
    WrapNotNull(nsThreadManager::get().GetCurrentThread());

  nsAutoPtr<nsThreadShutdownContext>& context =
    *currentThread->mRequestedShutdownContexts.AppendElement();
  context = new nsThreadShutdownContext(WrapNotNull(this), currentThread, aSync);

  // Set mShutdownContext and wake up the thread in case it is waiting for
  // events to process.
  nsCOMPtr<nsIRunnable> event =
    new nsThreadShutdownEvent(WrapNotNull(this), WrapNotNull(context.get()));
  // XXXroc What if posting the event fails due to OOM?
  mEvents->PutEvent(event.forget(), EventPriority::Normal);

  // We could still end up with other events being added after the shutdown
  // task, but that's okay because we process pending events in ThreadFunc
  // after setting mShutdownContext just before exiting.
  return context;
}

void
nsThread::ShutdownComplete(NotNull<nsThreadShutdownContext*> aContext)
{
#ifdef GECKO_MEDIA_CRATE
  if (mShutdownNow)
    return;
#endif
  MOZ_ASSERT(mThread);
  MOZ_ASSERT(aContext->mTerminatingThread == this);

  if (aContext->mAwaitingShutdownAck) {
    // We're in a synchronous shutdown, so tell whatever is up the stack that
    // we're done and unwind the stack so it can call us again.
    aContext->mAwaitingShutdownAck = false;
    return;
  }

  // Now, it should be safe to join without fear of dead-locking.

#ifdef GECKO_MEDIA_CRATE
  mShutdownNow = true;
#endif

  PR_JoinThread(mThread);
  mThread = nullptr;

  // We hold strong references to our event observers, and once the thread is
  // shut down the observers can't easily unregister themselves. Do it here
  // to avoid leaking.
  ClearObservers();

#ifdef DEBUG
  nsCOMPtr<nsIThreadObserver> obs = mEvents->GetObserver();
  MOZ_ASSERT(!obs, "Should have been cleared at shutdown!");
#endif

  // Delete aContext.
  MOZ_ALWAYS_TRUE(
    aContext->mJoiningThread->mRequestedShutdownContexts.RemoveElement(aContext));
}

void
nsThread::WaitForAllAsynchronousShutdowns()
{
  // This is the motivating example for why SpinEventLoop has the template
  // parameter we are providing here.
  SpinEventLoopUntil<ProcessFailureBehavior::IgnoreAndContinue>([&]() {
      return mRequestedShutdownContexts.IsEmpty();
    }, this);
}

NS_IMETHODIMP
nsThread::Shutdown()
{
  LOG(("THRD(%p) sync shutdown\n", this));

  // XXX If we make this warn, then we hit that warning at xpcom shutdown while
  //     shutting down a thread in a thread pool.  That happens b/c the thread
  //     in the thread pool is already shutdown by the thread manager.
  if (!mThread) {
    return NS_OK;
  }

  bool aSync = true;
#ifdef GECKO_MEDIA_CRATE
  aSync = false;
#endif
  nsThreadShutdownContext* maybeContext = ShutdownInternal(aSync);
  NS_ENSURE_TRUE(maybeContext, NS_ERROR_UNEXPECTED);
  NotNull<nsThreadShutdownContext*> context = WrapNotNull(maybeContext);

#ifndef GECKO_MEDIA_CRATE
  // Process events on the current thread until we receive a shutdown ACK.
  // Allows waiting; ensure no locks are held that would deadlock us!
  SpinEventLoopUntil([&, context]() {
      return !context->mAwaitingShutdownAck;
    }, context->mJoiningThread);
#endif
  ShutdownComplete(context);

  return NS_OK;
}

NS_IMETHODIMP
nsThread::HasPendingEvents(bool* aResult)
{
  if (NS_WARN_IF(PR_GetCurrentThread() != mThread)) {
    return NS_ERROR_NOT_SAME_THREAD;
  }

  *aResult = mEvents->HasPendingEvent();
  return NS_OK;
}

NS_IMETHODIMP
nsThread::IdleDispatch(already_AddRefed<nsIRunnable> aEvent)
{
  nsCOMPtr<nsIRunnable> event = aEvent;

  if (NS_WARN_IF(!event)) {
    return NS_ERROR_INVALID_ARG;
  }

  if (!mEvents->PutEvent(event.forget(), EventPriority::Idle)) {
    NS_WARNING("An idle event was posted to a thread that will never run it (rejected)");
    return NS_ERROR_UNEXPECTED;
  }

  return NS_OK;
}

#ifdef MOZ_CANARY
void canary_alarm_handler(int signum);

class Canary
{
  //XXX ToDo: support nested loops
public:
  Canary()
  {
    if (sCanaryOutputFD > 0 && EventLatencyIsImportant()) {
      signal(SIGALRM, canary_alarm_handler);
      ualarm(15000, 0);
    }
  }

  ~Canary()
  {
    if (sCanaryOutputFD != 0 && EventLatencyIsImportant()) {
      ualarm(0, 0);
    }
  }

  static bool EventLatencyIsImportant()
  {
    return NS_IsMainThread() && XRE_IsParentProcess();
  }
};

void canary_alarm_handler(int signum)
{
  void* array[30];
  const char msg[29] = "event took too long to run:\n";
  // use write to be safe in the signal handler
  write(sCanaryOutputFD, msg, sizeof(msg));
  backtrace_symbols_fd(array, backtrace(array, 30), sCanaryOutputFD);
}

#endif

#define NOTIFY_EVENT_OBSERVERS(observers_, func_, params_)                     \
  do {                                                                         \
    if (!observers_.IsEmpty()) {                                               \
      nsTObserverArray<nsCOMPtr<nsIThreadObserver>>::ForwardIterator           \
        iter_(observers_);                                                     \
      nsCOMPtr<nsIThreadObserver> obs_;                                        \
      while (iter_.HasMore()) {                                                \
        obs_ = iter_.GetNext();                                                \
        obs_ -> func_ params_ ;                                                \
      }                                                                        \
    }                                                                          \
  } while(0)

#ifndef RELEASE_OR_BETA
static bool
GetLabeledRunnableName(nsIRunnable* aEvent, nsACString& aName)
{
  bool labeled = false;
#ifndef GECKO_MEDIA_CRATE
  if (RefPtr<SchedulerGroup::Runnable> groupRunnable = do_QueryObject(aEvent)) {
    labeled = true;
    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(groupRunnable->GetName(aName)));
  } else if (nsCOMPtr<nsINamed> named = do_QueryInterface(aEvent)) {
    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(named->GetName(aName)));
  } else {
    aName.AssignLiteral("non-nsINamed runnable");
  }
#endif
  if (aName.IsEmpty()) {
    aName.AssignLiteral("anonymous runnable");
  }

  return labeled;
}
#endif

NS_IMETHODIMP
nsThread::ProcessNextEvent(bool aMayWait, bool* aResult)
{
  LOG(("THRD(%p) ProcessNextEvent [%u %u]\n", this, aMayWait,
       mNestedEventLoopDepth));

  if (NS_WARN_IF(PR_GetCurrentThread() != mThread)) {
    return NS_ERROR_NOT_SAME_THREAD;
  }

  // The toplevel event loop normally blocks waiting for the next event, but
  // if we're trying to shut this thread down, we must exit the event loop when
  // the event queue is empty.
  // This only applys to the toplevel event loop! Nested event loops (e.g.
  // during sync dispatch) are waiting for some state change and must be able
  // to block even if something has requested shutdown of the thread. Otherwise
  // we'll just busywait as we endlessly look for an event, fail to find one,
  // and repeat the nested event loop since its state change hasn't happened yet.
  bool reallyWait = aMayWait && (mNestedEventLoopDepth > 0 || !ShuttingDown());

#ifndef GECKO_MEDIA_CRATE
  Maybe<Scheduler::EventLoopActivation> activation;
  if (mIsMainThread == MAIN_THREAD) {
    DoMainThreadSpecificProcessing(reallyWait);
    activation.emplace();
  }
#endif

  ++mNestedEventLoopDepth;

#ifndef GECKO_MEDIA_CRATE
  // We only want to create an AutoNoJSAPI on threads that actually do DOM stuff
  // (including workers).  Those are exactly the threads that have an
  // mScriptObserver.
  Maybe<dom::AutoNoJSAPI> noJSAPI;
  bool callScriptObserver = !!mScriptObserver;
  if (callScriptObserver) {
    noJSAPI.emplace();
    mScriptObserver->BeforeProcessTask(reallyWait);
  }
#endif

  nsCOMPtr<nsIThreadObserver> obs = mEvents->GetObserverOnThread();
  if (obs) {
    obs->OnProcessNextEvent(this, reallyWait);
  }

  NOTIFY_EVENT_OBSERVERS(EventQueue()->EventObservers(), OnProcessNextEvent, (this, reallyWait));

#ifdef MOZ_CANARY
  Canary canary;
#endif
  nsresult rv = NS_OK;

  {
    // Scope for |event| to make sure that its destructor fires while
    // mNestedEventLoopDepth has been incremented, since that destructor can
    // also do work.
    EventPriority priority;
    nsCOMPtr<nsIRunnable> event = mEvents->GetEvent(reallyWait, &priority);

#ifndef GECKO_MEDIA_CRATE
    if (activation.isSome()) {
      activation.ref().SetEvent(event, priority);
    }
#endif

    *aResult = (event.get() != nullptr);

    if (event) {
      LOG(("THRD(%p) running [%p]\n", this, event.get()));

#ifndef GECKO_MEDIA_CRATE
      if (MAIN_THREAD == mIsMainThread) {
        HangMonitor::NotifyActivity();
      }
#endif

#ifndef RELEASE_OR_BETA
      Maybe<Telemetry::AutoTimer<Telemetry::MAIN_THREAD_RUNNABLE_MS>> timer;
      Maybe<Telemetry::AutoTimer<Telemetry::IDLE_RUNNABLE_BUDGET_OVERUSE_MS>> idleTimer;

      nsAutoCString name;
      if ((MAIN_THREAD == mIsMainThread) || mNextIdleDeadline) {
        bool labeled = GetLabeledRunnableName(event, name);

        if (MAIN_THREAD == mIsMainThread) {
          timer.emplace(name);

          // High-priority runnables are ignored here since they'll run right away
          // even with the cooperative scheduler.
          if (!labeled && priority == EventPriority::Normal) {
            TimeStamp now = TimeStamp::Now();
            double diff = (now - mLastUnlabeledRunnable).ToMilliseconds();
            Telemetry::Accumulate(Telemetry::TIME_BETWEEN_UNLABELED_RUNNABLES_MS, diff);
            mLastUnlabeledRunnable = now;
          }
        }

        if (mNextIdleDeadline) {
          // If we construct the AutoTimer with the deadline, then we'll
          // compute TimeStamp::Now() - mNextIdleDeadline when
          // accumulating telemetry.  If that is positive we've
          // overdrawn our idle budget, if it's negative it will go in
          // the 0 bucket of the histogram.
          idleTimer.emplace(name, mNextIdleDeadline);
        }
      }

      // If we're on the main thread, we want to record our current runnable's
      // name in a static so that BHR can record it.
      Array<char, kRunnableNameBufSize> restoreRunnableName;
      restoreRunnableName[0] = '\0';
      auto clear = MakeScopeExit([&] {
        if (MAIN_THREAD == mIsMainThread) {
          MOZ_ASSERT(NS_IsMainThread());
          sMainThreadRunnableName = restoreRunnableName;
        }
      });
      if (MAIN_THREAD == mIsMainThread) {
        MOZ_ASSERT(NS_IsMainThread());
        restoreRunnableName = sMainThreadRunnableName;

        // Copy the name into sMainThreadRunnableName's buffer, and append a
        // terminating null.
        uint32_t length = std::min((uint32_t) kRunnableNameBufSize - 1,
                                   (uint32_t) name.Length());
        memcpy(sMainThreadRunnableName.begin(), name.BeginReading(), length);
        sMainThreadRunnableName[length] = '\0';
      }
#endif
      Maybe<AutoTimeDurationHelper> timeDurationHelper;
      if (priority == EventPriority::Input) {
        timeDurationHelper.emplace();
      }
      event->Run();
    } else if (aMayWait) {
      MOZ_ASSERT(ShuttingDown(),
                 "This should only happen when shutting down");
      rv = NS_ERROR_UNEXPECTED;
    }
  }

  NOTIFY_EVENT_OBSERVERS(EventQueue()->EventObservers(), AfterProcessNextEvent, (this, *aResult));

  if (obs) {
    obs->AfterProcessNextEvent(this, *aResult);
  }

#ifndef GECKO_MEDIA_CRATE
  if (callScriptObserver) {
    if (mScriptObserver) {
      mScriptObserver->AfterProcessTask(mNestedEventLoopDepth);
    }
    noJSAPI.reset();
  }
#endif

  --mNestedEventLoopDepth;

  return rv;
}

//-----------------------------------------------------------------------------
// nsISupportsPriority

NS_IMETHODIMP
nsThread::GetPriority(int32_t* aPriority)
{
  *aPriority = mPriority;
  return NS_OK;
}

NS_IMETHODIMP
nsThread::SetPriority(int32_t aPriority)
{
  if (NS_WARN_IF(!mThread)) {
    return NS_ERROR_NOT_INITIALIZED;
  }

  // NSPR defines the following four thread priorities:
  //   PR_PRIORITY_LOW
  //   PR_PRIORITY_NORMAL
  //   PR_PRIORITY_HIGH
  //   PR_PRIORITY_URGENT
  // We map the priority values defined on nsISupportsPriority to these values.

  mPriority = aPriority;

  PRThreadPriority pri;
  if (mPriority <= PRIORITY_HIGHEST) {
    pri = PR_PRIORITY_URGENT;
  } else if (mPriority < PRIORITY_NORMAL) {
    pri = PR_PRIORITY_HIGH;
  } else if (mPriority > PRIORITY_NORMAL) {
    pri = PR_PRIORITY_LOW;
  } else {
    pri = PR_PRIORITY_NORMAL;
  }
  // If chaos mode is active, retain the randomly chosen priority
  if (!ChaosMode::isActive(ChaosFeature::ThreadScheduling)) {
    PR_SetThreadPriority(mThread, pri);
  }

  return NS_OK;
}

NS_IMETHODIMP
nsThread::AdjustPriority(int32_t aDelta)
{
  return SetPriority(mPriority + aDelta);
}

//-----------------------------------------------------------------------------
// nsIThreadInternal

NS_IMETHODIMP
nsThread::GetObserver(nsIThreadObserver** aObs)
{
  nsCOMPtr<nsIThreadObserver> obs = mEvents->GetObserver();
  obs.forget(aObs);
  return NS_OK;
}

NS_IMETHODIMP
nsThread::SetObserver(nsIThreadObserver* aObs)
{
  if (NS_WARN_IF(PR_GetCurrentThread() != mThread)) {
    return NS_ERROR_NOT_SAME_THREAD;
  }

  mEvents->SetObserver(aObs);
  return NS_OK;
}

uint32_t
nsThread::RecursionDepth() const
{
  MOZ_ASSERT(PR_GetCurrentThread() == mThread);
  return mNestedEventLoopDepth;
}

NS_IMETHODIMP
nsThread::AddObserver(nsIThreadObserver* aObserver)
{
  if (NS_WARN_IF(!aObserver)) {
    return NS_ERROR_INVALID_ARG;
  }
  if (NS_WARN_IF(PR_GetCurrentThread() != mThread)) {
    return NS_ERROR_NOT_SAME_THREAD;
  }

  EventQueue()->AddObserver(aObserver);

  return NS_OK;
}

NS_IMETHODIMP
nsThread::RemoveObserver(nsIThreadObserver* aObserver)
{
  if (NS_WARN_IF(PR_GetCurrentThread() != mThread)) {
    return NS_ERROR_NOT_SAME_THREAD;
  }

  EventQueue()->RemoveObserver(aObserver);

  return NS_OK;
}

#ifndef GECKO_MEDIA_CRATE
void
nsThread::SetScriptObserver(mozilla::CycleCollectedJSContext* aScriptObserver)
{
  if (!aScriptObserver) {
    mScriptObserver = nullptr;
    return;
  }

  MOZ_ASSERT(!mScriptObserver);
  mScriptObserver = aScriptObserver;
}
#endif

void
nsThread::DoMainThreadSpecificProcessing(bool aReallyWait)
{
  MOZ_ASSERT(mIsMainThread == MAIN_THREAD);

#ifndef GECKO_MEDIA_CRATE
  ipc::CancelCPOWs();
#endif

  if (aReallyWait) {
    HangMonitor::Suspend();
  }

  // Fire a memory pressure notification, if one is pending.
  if (!ShuttingDown()) {
    MemoryPressureState mpPending = NS_GetPendingMemoryPressure();
    if (mpPending != MemPressure_None) {
      nsCOMPtr<nsIObserverService> os = services::GetObserverService();

      if (os) {
        // Use no-forward to prevent the notifications from being transferred to
        // the children of this process.
        os->NotifyObservers(nullptr, "memory-pressure",
                            mpPending == MemPressure_New ? u"low-memory-no-forward" :
                            u"low-memory-ongoing-no-forward");
      } else {
        NS_WARNING("Can't get observer service!");
      }
    }
  }

#ifdef MOZ_CRASHREPORTER
  if (!ShuttingDown()) {
    SaveMemoryReportNearOOM(ShouldSaveMemoryReport::kMaybeReport);
  }
#endif
}

NS_IMETHODIMP
nsThread::GetEventTarget(nsIEventTarget** aEventTarget)
{
  nsCOMPtr<nsIEventTarget> target = this;
  target.forget(aEventTarget);
  return NS_OK;
}

nsIEventTarget*
nsThread::EventTarget()
{
  return this;
}

nsISerialEventTarget*
nsThread::SerialEventTarget()
{
  return this;
}
