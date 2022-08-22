#include <lunaix/ds/waitq.h>
#include <lunaix/process.h>
#include <lunaix/sched.h>
#include <lunaix/spike.h>

void
pwait(waitq_t* queue)
{
    waitq_t* current_wq = &__current->waitqueue;
    assert(llist_empty(&current_wq->waiters));

    llist_append(&queue->waiters, &current_wq->waiters);

    // FIXME centralize the state change.
    __current->state = PS_BLOCKED;
    sched_yieldk();
}

void
pwake_one(waitq_t* queue)
{
    if (llist_empty(&queue->waiters)) {
        return;
    }

    waitq_t* wq = list_entry(queue->waiters.next, waitq_t, waiters);
    struct proc_info* proc = container_of(wq, struct proc_info, waitqueue);

    assert(proc->state == PS_BLOCKED);
    proc->state = PS_READY;
    llist_delete(&wq->waiters);
}

void
pwake_all(waitq_t* queue)
{
    if (llist_empty(&queue->waiters)) {
        return;
    }

    struct proc_info* proc;
    waitq_t *pos, *n;
    llist_for_each(pos, n, &queue->waiters, waiters)
    {
        proc = container_of(pos, struct proc_info, waitqueue);

        proc->state = PS_READY;
        llist_delete(&pos->waiters);
    }
}