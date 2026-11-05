#pragma once

#include <OS.h>
#include <lock.h>
#include <condition_variable.h>
#include <util/DoublyLinkedList.h>


// Executes NVIDIA RM work items (os_queue_work_item) on a dedicated kernel
// thread, in FIFO order.
//
// RM work items acquire RM API/GPU locks and may block, so they must not run
// on Haiku's shared DPC queues: those also carry this driver's interrupt
// bottom half (rm_isr_bh), which could then wait behind a work item that is
// itself waiting for a lock. This mirrors the Linux driver, which runs work
// items on its own nv_kthread_q.
//
// Enqueue() is safe to call with interrupts disabled (from timer hooks,
// DPCs or the interrupt handler). Flush() must be called from a context that
// may block.
class NvHaikuWorkQueue {
public:
	NvHaikuWorkQueue();
	~NvHaikuWorkQueue();

	status_t Init(const char *name, int32 priority);
	// Runs all pending items, then stops the thread. Further Enqueue() calls
	// fail with B_NOT_INITIALIZED.
	void Shutdown();

	status_t Enqueue(void *data);
	status_t Flush(bool isUnload);
	bool IsUnloadFlushOngoing() const {return fUnloadFlushOngoing;}

private:
	struct Item: DoublyLinkedListLinkImpl<Item> {
		void *data;
	};
	typedef DoublyLinkedList<Item> ItemList;

	static status_t ThreadEntry(void *arg);
	void Run();
	void WaitCompleted(uint64 ticket);

private:
	spinlock fLock = B_SPINLOCK_INITIALIZER;
	ItemList fItems;
	ConditionVariable fPendingCondition;
	ConditionVariable fCompletedCondition;
	thread_id fThread = -1;
	bool fQuit = false;
	bool fRunning = false;
	volatile bool fUnloadFlushOngoing = false;
	uint64 fQueuedCount = 0;
	uint64 fCompletedCount = 0;
};
