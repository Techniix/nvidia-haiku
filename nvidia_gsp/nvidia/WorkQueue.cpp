#include "WorkQueue.h"

#include <KernelExport.h>
#include <util/AutoLock.h>

#include "nv-include.h"
extern "C" {
#include <os-interface.h>
}

#include "Driver.h"


NvHaikuWorkQueue::NvHaikuWorkQueue()
{
	fPendingCondition.Init(this, "nvidia work pending");
	fCompletedCondition.Init(this, "nvidia work completed");
}


NvHaikuWorkQueue::~NvHaikuWorkQueue()
{
	Shutdown();
}


status_t NvHaikuWorkQueue::Init(const char *name, int32 priority)
{
	fThread = spawn_kernel_thread(&ThreadEntry, name, priority, this);
	if (fThread < B_OK) {
		dprintf("nvidia_gsp: can't spawn work queue thread: %s\n", strerror(fThread));
		return fThread;
	}

	{
		InterruptsSpinLocker locker(&fLock);
		fRunning = true;
	}
	resume_thread(fThread);
	return B_OK;
}


void NvHaikuWorkQueue::Shutdown()
{
	{
		InterruptsSpinLocker locker(&fLock);
		if (!fRunning)
			return;
		fRunning = false;
		fQuit = true;
		fPendingCondition.NotifyAll();
	}

	// The thread drains the remaining items before it exits.
	status_t result;
	wait_for_thread(fThread, &result);
	fThread = -1;
}


status_t NvHaikuWorkQueue::Enqueue(void *data)
{
	// os_alloc_mem picks an interrupt-safe pool when interrupts are disabled.
	Item *item = nullptr;
	if (os_alloc_mem((void**)&item, sizeof(Item)) != NV_OK || item == nullptr)
		return B_NO_MEMORY;

	new(item) Item();
	item->data = data;

	InterruptsSpinLocker locker(&fLock);
	if (!fRunning) {
		locker.Unlock();
		item->~Item();
		os_free_mem(item);
		return B_NOT_INITIALIZED;
	}

	fItems.Add(item);
	fQueuedCount++;
	fPendingCondition.NotifyAll();
	return B_OK;
}


void NvHaikuWorkQueue::WaitCompleted(uint64 ticket)
{
	InterruptsSpinLocker locker(&fLock);
	while (fCompletedCount < ticket) {
		ConditionVariableEntry entry;
		fCompletedCondition.Add(&entry);
		locker.Unlock();
		entry.Wait();
		locker.Lock();
	}
}


status_t NvHaikuWorkQueue::Flush(bool isUnload)
{
	if (!are_interrupts_enabled()) {
		// Same rule as Linux (NV_MAY_SLEEP): flushing blocks.
		dprintf("nvidia_gsp: work queue flush from atomic context refused\n");
		return B_NOT_ALLOWED;
	}
	if (find_thread(NULL) == fThread) {
		// A work item flushing its own queue would wait for itself.
		dprintf("nvidia_gsp: work queue flush from the work queue thread refused\n");
		return B_NOT_ALLOWED;
	}

	fUnloadFlushOngoing = isUnload;

	// Two passes, like nv_kthread_q_flush(): items may queue further items
	// while the first pass runs.
	for (int pass = 0; pass < 2; pass++) {
		uint64 ticket;
		{
			InterruptsSpinLocker locker(&fLock);
			if (!fRunning && fItems.IsEmpty())
				break;
			ticket = fQueuedCount;
		}
		WaitCompleted(ticket);
	}

	fUnloadFlushOngoing = false;
	return B_OK;
}


status_t NvHaikuWorkQueue::ThreadEntry(void *arg)
{
	static_cast<NvHaikuWorkQueue*>(arg)->Run();
	return B_OK;
}


void NvHaikuWorkQueue::Run()
{
	InterruptsSpinLocker locker(&fLock);
	for (;;) {
		while (fItems.IsEmpty() && !fQuit) {
			ConditionVariableEntry entry;
			fPendingCondition.Add(&entry);
			locker.Unlock();
			entry.Wait();
			locker.Lock();
		}
		if (fItems.IsEmpty())
			break; // fQuit set and fully drained

		Item *item = fItems.RemoveHead();
		locker.Unlock();

		void *data = item->data;
		item->~Item();
		os_free_mem(item);

		rm_execute_work_item(nullptr, data);

		// We may block here; top up the pool used by interrupt-context
		// allocations so it doesn't drain under sustained interrupt load.
		NvHaikuDriver::Instance().IntrSafePool().Maintain();

		locker.Lock();
		fCompletedCount++;
		fCompletedCondition.NotifyAll();
	}
}
