#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"

/*************** P2 sys call: 헤더 파일 추가 - 시작 ***************/
#include "threads/synch.h" // thread 구조체의 세마포어 필드를 위함

/*************** P2 sys call: 헤더 파일 추가 - 끝 ***************/

#ifdef VM
#include "vm/vm.h"
#endif

/* States in a thread's life cycle. */
enum thread_status
{
    THREAD_RUNNING, /* Running thread. */
    THREAD_READY,   /* Not running but ready to run. */
    THREAD_BLOCKED, /* Waiting for an event to trigger. */
    THREAD_DYING    /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t)-1) /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0      /* Lowest priority. */
#define PRI_DEFAULT 31 /* Default priority. */
#define PRI_MAX 63     /* Highest priority. */

/*************** P2 sys call: 매크로 추가 ***************/
#define FDT_PAGES 3
#define OPEN_MAX (FDT_PAGES * (1 << 9)) // fdt의 최대 fd 값

/*************** P2 sys call: 매크로 추가 - 끝 ***************/

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */
struct thread
{
    /* Owned by thread.c. */
    tid_t tid;                 /* Thread identifier. */
    enum thread_status status; /* Thread state. */
    char name[16];             /* Name (for debugging purposes). */
    int priority;              /* Priority. */

    /****************** P1: 추가한 필드 *************************/
    int64_t wakeup_tick;                     // P1 alarm: tick till wake up
    int origin_priority;                     // P1 donation: 원래의 우선순위 값
    struct lock* lock_address;               // P1 donation: 락 주소
    struct list multiple_donation;           // P1 donation
    struct list_elem multiple_donation_elem; // P1 donation
    /****************** P1: 추가한 필드 *************************/

    /****************** P2: 추가한 필드 *************************/
    // struct thread *parent_process;	// Pointer to parent process
    struct semaphore sema_for_wait; // `wait()`을 위한 세마포어 (구조체 불러오기 어떻게?)
    struct semaphore sema_for_fork; // `fork()`을 위한 세마포어 (구조체 불러오기 어떻게?)
    struct file** fdt;              // fdt를 가리키는 포인터 (구조체 불러오기 어떻게?)
    struct intr_frame parent_if;    // +++ 부모의 tf 값 (fork)
    struct file* running_f;         // +++ 실행 중인 파일
    int next_fd;
    struct thread* parent;
    struct list child_info_list;
    /****************** P2: 추가한 필드 - 끝 *************************/

    /* Shared between thread.c and synch.c. */
    struct list_elem elem; /* List element. */

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint64_t* pml4; /* Page map level 4 */
#endif
#ifdef VM
    /* Table for whole virtual memory owned by thread. */
    struct supplemental_page_table spt;
#endif

    /* Owned by thread.c. */
    struct intr_frame tf; /* Information for switching */
    unsigned magic;       /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void* aux);
tid_t thread_create(const char* name, int priority, thread_func*, void*);

void thread_block(void);
void thread_unblock(struct thread*);

struct thread* thread_current(void);
tid_t thread_tid(void);
const char* thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

void do_iret(struct intr_frame* tf);

/****************** P1 priority: 프로토타입 추가 *************************/
void thread_sleep(int64_t ticks);
void thread_wakeup(int64_t ticks);
void update_next_tick_to_awake(int64_t ticks);
int64_t get_next_tick_to_awake(void);
bool cmp_priority(const struct list_elem* a, const struct list_elem* b, void* aux);
void test_max_priority(void);
/****************** P1 priority: 프로토타입 추가 - 끝 *************************/

/*************** P2 sys call: 프로토타입 추가 - 시작 ***************/
struct child_info_t
{
    tid_t tid;
    int exit_status;
    struct semaphore sema_for_wait;
    struct list_elem elem;
};

void add_child(struct thread* parent, tid_t child_tid);
void set_exit_status(struct thread* parent, tid_t child_tid, int exit_status);
int get_exit_status(struct thread* parent, tid_t child_tid);
struct child_info_t* get_child_info(struct thread* parent, tid_t child_tid);
void delete_child(struct thread* parent, tid_t child_tid);

/*************** P2 sys call: 프로토타입 추가 - 끝 ***************/

#endif /* threads/thread.h */