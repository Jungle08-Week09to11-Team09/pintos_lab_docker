#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/synch.h" // [*]2-B. include 추가
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif

/* States in a thread's life cycle. */
enum thread_status
{
	THREAD_RUNNING, /* Running thread. */
	THREAD_READY,	/* Not running but ready to run. */
	THREAD_BLOCKED, /* Waiting for an event to trigger. */
	THREAD_DYING	/* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) - 1) /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0	   /* Lowest priority. */
#define PRI_DEFAULT 31 /* Default priority. */
#define PRI_MAX 63	   /* Highest priority. */
#define OPEN_LIMIT 126 // 최대 동시 오픈가능한 파일 수


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
	tid_t tid;				   /* Thread identifier. */
	enum thread_status status; /* Thread state. */
	char name[16];			   /* Name (for debugging purposes). */
	int priority;			   /* Priority. */

	/* Shared between thread.c and synch.c. */
	struct list_elem elem; /* List element. */

	// [*]2-B. 구조체 변경
	struct file *fd_table[OPEN_LIMIT];  // 오픈한 파일을 가리키는 배열
	struct file *running;
 	//int next_fd; // 다음 오픈시 부여될 파일디스크립터
	

#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4; /* Page map level 4 */
	// [*]2-O
	// 커널에서 페이지 테이블 접근을 하기위한 포인터


#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf; /* Information for switching */
	unsigned magic;		  /* Detects stack overflow. */

	int64_t wakeup_tick; //[*]1-1. local tick 부여

	int init_priority;		   //[*]1-2-3. 초창기 중요도
	struct lock *wait_on_lock; //[*]1-2-3. release되기를 기다리고 있는 lock
	struct list donations;	   //[*]1-2-3. 중요도 양도한 애 리스트
	struct list_elem d_elem;   //[*]1-2-3. 중요도 양도한 애 관리(prev, next)

	// [*]2-B,O 자식의 종료신호를 기다리는 필드
	struct list child_list;				// 자식 프로세스 정보 리스트
	struct list_elem child_elem;
	struct thread *parent;				// 나를 만든 부모 스레드
	int exit_status;					// exit(int status)에서 설정, 부모가 wait()에서 자식의 종료 코드를 수거할 수 있게 해주는 변수
	// 복제가 잘 될때까지 부모가 기다리기 위한 세마포어
	struct semaphore fork_sema;
	// 자식의 종료상태를 기다리는 세마포어
	struct semaphore exit_sema;
	// 자식의 메모리 수거 상태를 기다리는 세마포어
	struct semaphore free_sema;
};



int64_t min_wakeup_tick; //[*]1-1. global tick 선언

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(struct thread *);

struct thread *thread_current(void);
tid_t thread_tid(void);
const char *thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

void do_iret(struct intr_frame *tf);

void thread_sleep(int64_t ticks);																	  //[*]1-1.
void thread_wakeup(void);																			  //[*]1-1.
bool thread_compare_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED); //[*]1-2-1.
void thread_preemption(void);																		  //[*]1-2-1.

// [*]1-2-3.
void thread_refresh_priority(void);
void donate_priority(void);
bool thread_compare_donate_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
void remove_with_lock(struct lock *lock);

#endif /* threads/thread.h */
