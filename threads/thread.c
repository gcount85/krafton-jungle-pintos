#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/*************** P1 priority: 프로토타입 & 매크로 추가 - 시작 ***************/
static struct list sleep_list;     // 블락된 쓰레드들을 위한 쓰레드
static int64_t next_tick_to_awake; // 슬립 리스트의 쓰레드가 가진 가장 작은 틱
static bool cmp_wakeup_tick(const struct list_elem* a, const struct list_elem* b, void* aux);
#define MIN(a, b) (((a) < (b)) ? (a) : (b)) // +++

/*************** P1 priority: 프로토타입 & 매크로 추가 - 끝 ***************/

/* Idle thread. */
static struct thread* idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread* initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4          /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread(thread_func*, void* aux);

static void idle(void* aux UNUSED);
static struct thread* next_thread_to_run(void);
static void init_thread(struct thread*, const char* name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread*)(pg_round_down(rrsp())))

// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff};

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init(void)
{
    ASSERT(intr_get_level() == INTR_OFF);

    /* Reload the temporal gdt for the kernel
     * This gdt does not include the user context.
     * The kernel will rebuild the gdt with user context, in gdt_init (). */
    struct desc_ptr gdt_ds = {.size = sizeof(gdt) - 1, .address = (uint64_t)gdt};
    lgdt(&gdt_ds);

    /* Init the global thread context */
    lock_init(&tid_lock);
    list_init(&ready_list);
    list_init(&sleep_list); // P1 alarm: 슬립리스트 초기화
    /****************** P1 alarm: 추가 *************************/
    next_tick_to_awake = INT64_MAX; // +++
    /****************** P1 alarm: 추가 - 끝 *************************/

    list_init(&destruction_req);

    /* Set up a thread structure for the running thread. */
    initial_thread = running_thread();
    init_thread(initial_thread, "main", PRI_DEFAULT);
    initial_thread->status = THREAD_RUNNING;
    initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void)
{
    /* Create the idle thread. */
    struct semaphore idle_started;
    sema_init(&idle_started, 0);
    thread_create("idle", PRI_MIN, idle, &idle_started);

    /* Start preemptive thread scheduling. */
    intr_enable();

    /* Wait for the idle thread to initialize idle_thread. */
    sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context.

   */
void thread_tick(void)
{
    struct thread* t = thread_current();

    /* Update statistics. */
    if (t == idle_thread)
        idle_ticks++;
#ifdef USERPROG
    else if (t->pml4 != NULL)
        user_ticks++;
#endif
    else
        kernel_ticks++;

    /* Enforce preemption. */
    if (++thread_ticks >= TIME_SLICE)
        intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void)
{
    printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n", idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(const char* name, int priority, thread_func* function, void* aux)
{
    struct thread* t;
    tid_t tid;

    ASSERT(function != NULL);

    /* Allocate thread. */
    t = palloc_get_page(PAL_ZERO);
    if (t == NULL)
        return TID_ERROR;

    /* Initialize thread. */
    init_thread(t, name, priority);
    tid = t->tid = allocate_tid();

    /********** P2 sys call: 초기화 코드 - 시작 **********/
    // fdt 초기화
    t->fdt = palloc_get_multiple(PAL_ZERO, FDT_PAGES); // zeroed 4KB 페이지 할당
    if (t->fdt == NULL)
        return TID_ERROR;
    t->fdt[0] = 1;
    t->fdt[1] = 2;
    t->next_fd = 2; // +++
    t->parent = thread_current();
    /********** P2 sys call: 초기화 코드 - 끝 **********/

    /* Call the kernel_thread if it scheduled.
     * Note) rdi is 1st argument, and rsi is 2nd argument. */
    t->tf.rip = (uintptr_t)kernel_thread;
    t->tf.R.rdi = (uint64_t)function;
    t->tf.R.rsi = (uint64_t)aux;
    t->tf.ds = SEL_KDSEG;
    t->tf.es = SEL_KDSEG;
    t->tf.ss = SEL_KDSEG;
    t->tf.cs = SEL_KCSEG;
    t->tf.eflags = FLAG_IF;

    add_child(thread_current(), tid);
    /* Add to run queue. */
    thread_unblock(t);

    /********** P1 priority: 추가 코드 - 시작 **********/
    test_max_priority(); // P1 priority: 방금 만든 쓰레드와 러닝 스레드 비교 위해

    /********** P1 priority: 추가 코드 - 끝 **********/

    return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void)
{
    ASSERT(!intr_context());
    ASSERT(intr_get_level() == INTR_OFF);
    thread_current()->status = THREAD_BLOCKED;
    schedule();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
// 쓰레드 상태 전환; 블락 → ready 상태 + ready 리스트에 삽입
void thread_unblock(struct thread* t)
{
    enum intr_level old_level;

    ASSERT(is_thread(t));

    old_level = intr_disable();
    ASSERT(t->status == THREAD_BLOCKED);

    /********** P1 priority: 추가 코드 - 시작 **********/
    list_insert_ordered(&ready_list, &t->elem, cmp_priority, NULL); // P1 priority: 우선 순위 정렬에 맞춰 리스트에 삽입

    /********** P1 priority: 추가 코드 - 끝 **********/

    t->status = THREAD_READY;
    intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char* thread_name(void)
{
    return thread_current()->name;
}

/* Returns the running thread 포인터.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread* thread_current(void)
{
    struct thread* t = running_thread();

    /* Make sure T is really a thread.
       If either of these assertions fire, then your thread may
       have overflowed its stack.  Each thread has less than 4 kB
       of stack, so a few big automatic arrays or moderate
       recursion can cause stack overflow. */
    ASSERT(is_thread(t));
    ASSERT(t->status == THREAD_RUNNING);

    return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void)
{
    return thread_current()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void)
{
    ASSERT(!intr_context());

#ifdef USERPROG
    process_exit();
#endif

    /* Just set our status to dying and schedule another process.
       We will be destroyed during the call to schedule_tail(). */
    intr_disable();
    do_schedule(THREAD_DYING);
    NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim.
   */
// 쓰레드 상태 전환; 실행 → ready 상태 + ready 리스트에 삽입
void thread_yield(void)
{
    struct thread* curr = thread_current();
    enum intr_level old_level;

    ASSERT(!intr_context());

    old_level = intr_disable(); // 인터럽트 off
    if (curr != idle_thread)
    {
        /********** P1 priority: 추가 코드 - 시작 **********/
        list_insert_ordered(&ready_list, &curr->elem, cmp_priority, NULL); // P1 priority: 우선순위 정렬에 맞춰 리스트에 삽입

        /********** P1 priority: 추가 코드 - 끝 **********/
    }
    do_schedule(THREAD_READY); // 러닝 스레드가 레디 상태로 바뀌고, 레디 리스트 삽입
    intr_set_level(old_level); // 인터럽트 다시 받겠다
}

/* Sets the current thread's priority to NEW_PRIORITY. */

/* Returns the current thread's priority. */
int thread_get_priority(void)
{
    return thread_current()->priority;
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED)
{
    /* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int thread_get_nice(void)
{
    /* TODO: Your implementation goes here */
    return 0;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void)
{
    /* TODO: Your implementation goes here */
    return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void)
{
    /* TODO: Your implementation goes here */
    return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void idle(void* idle_started_ UNUSED)
{
    struct semaphore* idle_started = idle_started_;

    idle_thread = thread_current();
    sema_up(idle_started);

    for (;;)
    {
        /* Let someone else run. */
        intr_disable();
        thread_block();

        /* Re-enable interrupts and wait for the next one.

           The `sti' instruction disables interrupts until the
           completion of the next instruction, so these two
           instructions are executed atomically.  This atomicity is
           important; otherwise, an interrupt could be handled
           between re-enabling interrupts and waiting for the next
           one to occur, wasting as much as one clock tick worth of
           time.

           See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
           7.11.1 "HLT Instruction". */
        asm volatile("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void kernel_thread(thread_func* function, void* aux)
{
    ASSERT(function != NULL);

    intr_enable(); /* The scheduler runs with interrupts off. */
    function(aux); /* Execute the thread function. */
    thread_exit(); /* If function() returns, kill the thread. */
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void init_thread(struct thread* t, const char* name, int priority)
{
    ASSERT(t != NULL);
    ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
    ASSERT(name != NULL);

    memset(t, 0, sizeof *t);
    t->status = THREAD_BLOCKED; // 쓰레드 초기화: block 상태

    strlcpy(t->name, name, sizeof t->name);
    t->tf.rsp = (uint64_t)t + PGSIZE - sizeof(void*);
    t->priority = priority;
    t->magic = THREAD_MAGIC;

    /***************** P1 donation: 초기화 추가*****************/
    t->origin_priority = priority;
    t->lock_address = NULL;
    list_init(&t->multiple_donation);
    /***************** P1 donation: 초기화 추가 - 끝 *****************/

    /***************** P2 sys call: 초기화 추가*************************/
    sema_init(&t->sema_for_wait, 0); // `wait()`을 위한 세마포어 초기화
    sema_init(&t->sema_for_fork, 0); // `fork()`을 위한 세마포어 초기화
    t->running_f = NULL;             // +++
    list_init(&t->child_info_list);
    /***************** P2 sys call: 초기화 추가 - 끝*****************/
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread* next_thread_to_run(void)
{
    if (list_empty(&ready_list))
        return idle_thread;
    else
        return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void do_iret(struct intr_frame* tf)
{
    __asm __volatile("movq %0, %%rsp\n"
                     "movq 0(%%rsp),%%r15\n"
                     "movq 8(%%rsp),%%r14\n"
                     "movq 16(%%rsp),%%r13\n"
                     "movq 24(%%rsp),%%r12\n"
                     "movq 32(%%rsp),%%r11\n"
                     "movq 40(%%rsp),%%r10\n"
                     "movq 48(%%rsp),%%r9\n"
                     "movq 56(%%rsp),%%r8\n"
                     "movq 64(%%rsp),%%rsi\n"
                     "movq 72(%%rsp),%%rdi\n"
                     "movq 80(%%rsp),%%rbp\n"
                     "movq 88(%%rsp),%%rdx\n"
                     "movq 96(%%rsp),%%rcx\n"
                     "movq 104(%%rsp),%%rbx\n"
                     "movq 112(%%rsp),%%rax\n"
                     "addq $120,%%rsp\n"
                     "movw 8(%%rsp),%%ds\n"
                     "movw (%%rsp),%%es\n"
                     "addq $32, %%rsp\n"
                     "iretq"
                     :
                     : "g"((uint64_t)tf)
                     : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void thread_launch(struct thread* th)
{
    uint64_t tf_cur = (uint64_t)&running_thread()->tf;
    uint64_t tf = (uint64_t)&th->tf;
    ASSERT(intr_get_level() == INTR_OFF);

    /* The main switching logic.
     * We first restore the whole execution context into the intr_frame
     * and then switching to the next thread by calling do_iret.
     * Note that, we SHOULD NOT use any stack from here
     * until switching is done. */
    __asm __volatile(
        /* Store registers that will be used. */
        "push %%rax\n"
        "push %%rbx\n"
        "push %%rcx\n"
        /* Fetch input once */
        "movq %0, %%rax\n"
        "movq %1, %%rcx\n"
        "movq %%r15, 0(%%rax)\n"
        "movq %%r14, 8(%%rax)\n"
        "movq %%r13, 16(%%rax)\n"
        "movq %%r12, 24(%%rax)\n"
        "movq %%r11, 32(%%rax)\n"
        "movq %%r10, 40(%%rax)\n"
        "movq %%r9, 48(%%rax)\n"
        "movq %%r8, 56(%%rax)\n"
        "movq %%rsi, 64(%%rax)\n"
        "movq %%rdi, 72(%%rax)\n"
        "movq %%rbp, 80(%%rax)\n"
        "movq %%rdx, 88(%%rax)\n"
        "pop %%rbx\n" // Saved rcx
        "movq %%rbx, 96(%%rax)\n"
        "pop %%rbx\n" // Saved rbx
        "movq %%rbx, 104(%%rax)\n"
        "pop %%rbx\n" // Saved rax
        "movq %%rbx, 112(%%rax)\n"
        "addq $120, %%rax\n"
        "movw %%es, (%%rax)\n"
        "movw %%ds, 8(%%rax)\n"
        "addq $32, %%rax\n"
        "call __next\n" // read the current rip.
        "__next:\n"
        "pop %%rbx\n"
        "addq $(out_iret -  __next), %%rbx\n"
        "movq %%rbx, 0(%%rax)\n" // rip
        "movw %%cs, 8(%%rax)\n"  // cs
        "pushfq\n"
        "popq %%rbx\n"
        "mov %%rbx, 16(%%rax)\n" // eflags
        "mov %%rsp, 24(%%rax)\n" // rsp
        "movw %%ss, 32(%%rax)\n"
        "mov %%rcx, %%rdi\n"
        "call do_iret\n"
        "out_iret:\n"
        :
        : "g"(tf_cur), "g"(tf)
        : "memory");
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void do_schedule(int status)
{
    ASSERT(intr_get_level() == INTR_OFF);
    ASSERT(thread_current()->status == THREAD_RUNNING);
    while (!list_empty(&destruction_req))
    {
        struct thread* victim = list_entry(list_pop_front(&destruction_req), struct thread, elem);
        palloc_free_page(victim);
    }
    thread_current()->status = status;
    schedule(); // 문맥 전환
}

// 다음 스케줄링 실행
static void schedule(void)
{
    struct thread* curr = running_thread();
    struct thread* next = next_thread_to_run();

    ASSERT(intr_get_level() == INTR_OFF);
    ASSERT(curr->status != THREAD_RUNNING);
    ASSERT(is_thread(next));
    /* Mark us as running. */
    next->status = THREAD_RUNNING;

    /* Start new time slice. */
    thread_ticks = 0;

#ifdef USERPROG
    /* Activate the new address space. */
    process_activate(next);
#endif

    if (curr != next)
    {
        /* If the thread we switched from is dying, destroy its struct
           thread. This must happen late so that thread_exit() doesn't
           pull out the rug under itself.
           We just queuing the page free reqeust here because the page is
           currently used bye the stack.
           The real destruction logic will be called at the beginning of the
           schedule(). */
        if (curr && curr->status == THREAD_DYING && curr != initial_thread)
        {
            ASSERT(curr != next);
            list_push_back(&destruction_req, &curr->elem);
        }

        /* Before switching the thread, we first save the information
         * of current running. */
        thread_launch(next);
    }
}

/* Returns a tid to use for a new thread. */
static tid_t allocate_tid(void)
{
    static tid_t next_tid = 1;
    tid_t tid;

    lock_acquire(&tid_lock);
    tid = next_tid++;
    lock_release(&tid_lock);

    return tid;
}

/*************** P1 priority: 추가한 함수 - 시작 ***************/
// P1 alarm: 정렬을 위한 리스트 원소의 wakeup_tick값을 비교하기
static bool cmp_wakeup_tick(const struct list_elem* a, const struct list_elem* b, void* aux)
{
    int64_t a_wakeup_t = list_entry(a, struct thread, elem)->wakeup_tick;
    int64_t b_wakeup_t = list_entry(b, struct thread, elem)->wakeup_tick;
    return (a_wakeup_t < b_wakeup_t);
}

// P1 alarm: 주어진 ticks를 global tick으로 저장하기
void update_next_tick_to_awake(int64_t ticks)
{
    // next_tick_to_awake = ticks;
    next_tick_to_awake = MIN(next_tick_to_awake, ticks);
}

// P1 alarm: global tick 반환하기
int64_t get_next_tick_to_awake(void)
{
    return next_tick_to_awake;
}

// P1 alarm: 호출자 스레드의 상태를 블락으로 바꿈, 그리고 슬립 큐로 넣음
// 참고: https://poalim.tistory.com/28
void thread_sleep(int64_t ticks)
{
    struct thread* cur;
    enum intr_level old_level;

    old_level = intr_disable(); // 인터럽트 off → 지금 스레드 고정
    cur = thread_current();     // 현재 스레드

    ASSERT(cur != idle_thread);

    cur->wakeup_tick = ticks; // 일어날 시간; local tick

    // P1 alarm: 쓰레드가 정렬된 순서로 리스트 안에 위치하도록
    list_insert_ordered(&sleep_list, &cur->elem, cmp_wakeup_tick, NULL);

    // ###IMPR### P1: 최소 wakeup_tick 찾기
    struct list_elem* e = list_begin(&sleep_list);
    struct thread* t = list_entry(e, struct thread, elem);
    update_next_tick_to_awake(t->wakeup_tick);

    thread_block(); // block 상태로 변경

    intr_set_level(old_level); // 인터럽트 on

    /* if the current thread is not idle thread,
    change the state of the caller thread to BLOCKED,
    store the local tick to wake up,
    update the global tick if necessary,
    and call schedule() */
    /* When you manipulate thread list, disable interrupt! */
}

// P1 alarm: 일어날 쓰레드를 깨워서 슬립큐에서 제거하고 레디큐에 넣기
// 참고: https://poalim.tistory.com/28
void thread_wakeup(int64_t ticks)
{

    // // -- 내 코드 --------------------------------------
    // struct list_elem *e = list_begin(&sleep_list);

    // while (e != list_end(&sleep_list)) // 슬립 리스트의 마지막 원소에 도달할때까지
    // {
    // 	struct thread *t = list_entry(e, struct thread, elem);

    // 	if (t->wakeup_tick <= ticks) // 스레드가 일어날 시간이 되었는지 확인
    // 	{
    // 		e = list_remove(e); // sleep list 에서 제거
    // 		thread_unblock(t);	// 스레드 unblock
    // 	}
    // 	else // 깨울 시간이 되지 않은 쓰레드 발견되면 최소 틱으로 저장하고 break
    // 	// 괄호 때문에 에러 났었음
    // 	{
    // 		update_next_tick_to_awake(t->wakeup_tick);
    // 		break;
    // 	}
    // }
    // // -- 내 코드 --------------------------------------

    // next_tick_to_awake = INT64_MAX;
    struct list_elem* target; //= list_begin (&sleep_list);

    // while (target != list_end (&sleep_list)) {
    for (target = list_begin(&sleep_list); target != list_end(&sleep_list);)
    {
        struct thread* thread_pt = list_entry(target, struct thread,
            elem); /*list_entry() : Converts pointer to list element LIST_ELEM into a pointer to the structure that LIST_ELEM is embedded inside.*/
        /*if tick is greater than / or same as wakeup_tick,
          removes a target thread out of sleep_list
          and changes its state to THREAD_READY*/
        if (thread_pt->wakeup_tick <= ticks)
        {
            target = list_remove(target); /*list_remove(): Removes element out of list and returns next element pointer*/
            thread_unblock(thread_pt);
        }
        /*If not, just do nothing and set pointer to the next sleeping thread.
          Also, there could be a change in a sleep_list(through the process above), update next_tick_to_awake*/
        else
        {
            update_next_tick_to_awake(thread_pt->wakeup_tick);
            target = list_next(target);
        }
    }
}

// P1 priority: 우선순위 비교 (내림차순 정렬을 위한)
// 우선순위가 같은 경우도 추가함 (return 2)
bool cmp_priority(const struct list_elem* a, const struct list_elem* b, void* aux)
{
    int a_priority = list_entry(a, struct thread, elem)->priority;
    int b_priority = list_entry(b, struct thread, elem)->priority;

    if (a_priority > b_priority) // 부등호 "=" 넣었다가 fifo 실패 (선입선출!!!)
        return true;
    else
        return false;
}

// P1 Priority: 러닝 스레드와 레디 리스트 첫 스레드의 우선순위와 비교 → 스케줄링
void test_max_priority(void)
{
    /* =========================== 수정코드 시작 - OK ===================================== */
    // 슬립 리스트가 비어 있지 않고,
    // 현재의 스레드의 우선순위 < 슬립 리스트의 첫번째 원소의 우선순위
    // 인터럽트 처리 중이 아닐 때 (!intr_context())
    // 위의 조건을 모두 만족할 때 thread_yield();

    // 수정한 거
    // ==cmp priority를 쓸건지, 직접 비교할건지? == -> 직접 비교로 바꿔서 에러 해결함(아래4번)
    // 1. cur 을 thread_current로 바로 받음 (중간에 현재 쓰레드 바뀔까봐 -> 상관 없었다)★
    // 2. 리스트 엠티 확인
    // 3. 레디리스트의 첫째를 변수로 저장 안하고 바로 받음 (중간에 레디 리스트 첫째가 바뀔까봐 -> 상관 없었따)★
    // 4. cmp_priority 함수 사용 안함 (함수로 넘겨주면서 바뀌거나, 등호 때문인듯)★★★ 이놈이 원인
    // 5. begin -> front로 바꿔 써봄 (차이 없었다)

    struct thread* cur = thread_current();
    struct list_elem* ready_begin = list_begin(&ready_list);

    if (list_empty(&ready_list))
        return;

    if (cur->priority >= list_entry(ready_begin, struct thread, elem)->priority)
        return;

    if (intr_context()) // P2 에러 방지를 위해 추가
        return;

    thread_yield();
    /* =========================== 수정코드 끝 ===================================== */
}

// P1 Priority: 도네이션을 고려하여 우선순위 지정
void thread_set_priority(int new_priority)
{
    thread_current()->origin_priority = new_priority;
    refresh_priority();
    test_max_priority();
}

void add_child(struct thread* parent, tid_t child_tid)
{
    struct child_info_t* info = (struct child_info_t*)malloc(sizeof(struct child_info_t));
    info->tid = child_tid;
    sema_init(&info->sema_for_wait, 0);
    list_push_back(&parent->child_info_list, &info->elem);
}

void set_exit_status(struct thread* parent, tid_t child_tid, int exit_status)
{
    struct list_elem* e;
    for (e = list_begin(&parent->child_info_list); e != list_end(&parent->child_info_list); e = list_next(e))
    {
        struct child_info_t* info = list_entry(e, struct child_info_t, elem);
        if (info->tid == child_tid)
        {
            info->exit_status = exit_status;
        }
    }
}

int get_exit_status(struct thread* parent, tid_t child_tid)
{
    struct list_elem* e;
    for (e = list_begin(&parent->child_info_list); e != list_end(&parent->child_info_list); e = list_next(e))
    {
        struct child_info_t* info = list_entry(e, struct child_info_t, elem);
        if (info->tid == child_tid)
        {
            return info->exit_status;
        }
    }

    return 0;
}

struct child_info_t* get_child_info(struct thread* parent, tid_t child_tid)
{
    struct list_elem* e;
    for (e = list_begin(&parent->child_info_list); e != list_end(&parent->child_info_list); e = list_next(e))
    {
        struct child_info_t* info = list_entry(e, struct child_info_t, elem);
        if (info->tid == child_tid)
        {
            return info;
        }
    }

    return NULL;
}

void delete_child(struct thread* parent, tid_t child_tid)
{
    struct list_elem* e;
    for (e = list_begin(&parent->child_info_list); e != list_end(&parent->child_info_list); e = list_next(e))
    {
        struct child_info_t* info = list_entry(e, struct child_info_t, elem);
        if (info->tid == child_tid)
        {
            list_remove(e);
        }
    }
}