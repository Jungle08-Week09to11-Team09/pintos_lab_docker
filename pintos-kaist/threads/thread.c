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


// test

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b // 스택 오버플로 감지용 매직 값

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210 // 기본 스레드용 매직 값 (수정 금지)

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list; // 실행 준비된 스레드 리스트
static struct list sleep_list; // [*]1-1. 잠든 스레드 리스트

/* Idle thread. */
static struct thread *idle_thread; // 아이들 스레드

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread; // 초기 스레드 (init.c의 main 함수 실행)

/* Lock used by allocate_tid(). */
static struct lock tid_lock; // tid 할당 시 사용되는 락

/* Thread destruction requests */
static struct list destruction_req; // 스레드 파괴 요청 리스트

/* Statistics. */
static long long idle_ticks; /* # of timer ticks spent idle. */			 // 아이들 상태에서 소비된 타이머 틱 수
static long long kernel_ticks; /* # of timer ticks in kernel threads. */ // 커널 스레드에서 소비된 타이머 틱 수
static long long user_ticks; /* # of timer ticks in user programs. */	 // 사용자 프로그램에서 소비된 타이머 틱 수

/* Scheduling. */
#define TIME_SLICE 4 /* # of timer ticks to give each thread. */	   // 각 스레드에 할당할 타이머 틱 수
static unsigned thread_ticks; /* # of timer ticks since last yield. */ // 마지막 스레드 양보 이후의 타이머 틱 수

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;
; // MLFQS 스케줄러 사용 여부 (커널 명령줄 옵션으로 제어)

static void kernel_thread(thread_func *, void *aux); // 커널 스레드 실행 함수

static void idle(void *aux UNUSED);										  // 아이들 스레드 함수
static struct thread *next_thread_to_run(void);							  // 다음 실행할 스레드 선택 함수
static void init_thread(struct thread *, const char *name, int priority); // 스레드 초기화 함수
static void do_schedule(int status);									  // 스케줄링 수행 (스레드 상태 변경 포함) 함수
static void schedule(void);												  // 스케줄링 함수
static tid_t allocate_tid(void);										  // 새 스레드 ID 할당 함수

//[*]1-1.
int64_t min_wakeup_tick = INT64_MAX; // global tick 초기화
static bool compare_wakeup_tick(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
void thread_sleep(int64_t ticks);
void thread_wakeup(void);

//[*]1-2-1. [*]1-2-2.
bool thread_compare_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
void thread_preemption(void);

// [*]1-2-3.
void thread_refresh_priority(void);
void donate_priority(void);
bool thread_compare_donate_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
void remove_with_lock(struct lock *lock);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC) // 주어진 포인터가 유효한 스레드 구조체를 가리키는지 확인

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *)(pg_round_down(rrsp()))) // 현재 실행 중인 스레드 반환

// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff}; // thread_start를 위한 전역 디스크립터 테이블 (임시)

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
	ASSERT(intr_get_level() == INTR_OFF); // 인터럽트 비활성화 상태 확인

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		// 임시 GDT 디스크립터
		.size = sizeof(gdt) - 1, // 크기 설정
		.address = (uint64_t)gdt // 주소 설정
	};
	lgdt(&gdt_ds); // 임시 GDT 로드

	/* Init the globla thread context */
	lock_init(&tid_lock);		 // tid 락 초기화
	list_init(&ready_list);		 // 준비 큐 초기화
	list_init(&sleep_list);		 // [*]1-1. 수면 큐 초기화
	list_init(&destruction_req); // 스레드 파괴 요청 리스트 초기화

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread();				  // 현재 실행 중인 스레드를 초기 스레드로 설정
	init_thread(initial_thread, "main", PRI_DEFAULT); // 초기 스레드 초기화
	initial_thread->status = THREAD_RUNNING;		  // 초기 스레드 상태를 실행 중으로 설정
	initial_thread->tid = allocate_tid();			  // 초기 스레드에 tid 할당
	initial_thread->wakeup_tick = 0;				  // [*]1-1. local tick 초기화
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void)
{
	/* Create the idle thread. */
	struct semaphore idle_started; // 아이들 스레드 시작 동기화용 세마포어
	sema_init(&idle_started, 0);   // 세마포어 초기화 (초기 값 0)
	thread_create("idle", PRI_MIN, idle, &idle_started);
	; // 아이들 스레드 생성

	/* Start preemptive thread scheduling. */
	intr_enable(); // 인터럽트 활성화 (선점형 스케줄링 시작)

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down(&idle_started); // 아이들 스레드가 idle_thread 초기화 완료될 때까지 대기
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void thread_tick(void)
{
	struct thread *t = thread_current(); // 현재 실행 중인 스레드 획득

	/* Update statistics. */
	if (t == idle_thread) // 현재 스레드가 아이들 스레드면
		idle_ticks++;	  // 아이들 틱 증가
#ifdef USERPROG
	else if (t->pml4 != NULL) // 현재 스레드가 사용자 프로그램 스레드면
		user_ticks++;		  // 사용자 틱 증가
#endif
	else				// 현재 스레드가 커널 스레드면
		kernel_ticks++; // 커널 틱 증가

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE) // 할당된 시간 슬라이스를 초과하면
		intr_yield_on_return();		  // 인터럽트 반환 시 스레드 양보 요청
}

/* Prints thread statistics. */
void thread_print_stats(void)
{
	printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
		   idle_ticks, kernel_ticks, user_ticks); // 스레드 통계 출력
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
tid_t thread_create(const char *name, int priority,
					thread_func *function, void *aux)
{
	struct thread *t; // 새로운 스레드 구조체 포인터
	tid_t tid;		  // 새로운 스레드 ID

	ASSERT(function != NULL); // 함수 포인터가 NULL이 아닌지 확인

	/* Allocate thread. */
	t = palloc_get_page(PAL_ZERO); // 스레드 구조체를 위한 페이지 할당 (0으로 초기화)
	if (t == NULL)				   // 할당 실패 시
		return TID_ERROR;		   // 오류 반환

	/* Initialize thread. */
	init_thread(t, name, priority); // 스레드 기본 정보 초기화
	tid = t->tid = allocate_tid();	// 스레드 ID 할당 및 저장
	// [*]2-o 자식 스레드 만들어주면서 부모 자신을 부모필드에 직접 추가.

	t->parent = thread_current();
	// [*]2-o 부모의 child list에 생성되는 스레드 t의 elem 넣기.
	list_push_back(&t->parent->child_list,&t->child_elem);

	// [*]2-B. fd table 관련 수정(이중 포인터로 바꿔뒀으니 동적 할당 받아줘야 함)
	t->fd_table = palloc_get_multiple(PAL_ZERO, FDT_PAGES); 
	if (t->fd_table == NULL) {
    palloc_free_page(t);
    return TID_ERROR;
	}
	t->next_fd = 2; // 0: stdin, 1: stdout
	
	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t)kernel_thread; // 실행될 함수 주소 설정
	t->tf.R.rdi = (uint64_t)function;	  // 첫 번째 인자 설정
	t->tf.R.rsi = (uint64_t)aux;		  // 두 번째 인자 설정
	t->tf.ds = SEL_KDSEG;				  // 데이터 세그먼트 설정
	t->tf.es = SEL_KDSEG;				  // 엑스트라 세그먼트 설정
	t->tf.ss = SEL_KDSEG;				  // 스택 세그먼트 설정
	t->tf.cs = SEL_KCSEG;				  // 코드 세그먼트 설정
	t->tf.eflags = FLAG_IF;				  // 인터럽트 플래그 설정 (인터럽트 활성화)

	/* Add to run queue. */
	thread_unblock(t); // 스레드를 준비 큐에 추가 (실행 가능 상태로 변경)

	/* compare the priorities of the currently running
	thread and the newly inserted one. Yield the CPU if the
	newly arriving thread has higher priority*/
	// [*]1-2-1. 새로 만들어진 애의 priority가 현재 실행 중인 애 것보다 크면 실행 중인 애 나가리
	// if (thread_current()->priority < t->priority)
	// 	thread_yield();
	thread_preemption();
	return tid; // 새 스레드 ID 반환
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void)
{
	ASSERT(!intr_context());				   // 인터럽트 컨텍스트가 아닌지 확인
	ASSERT(intr_get_level() == INTR_OFF);	   // 인터럽트 비활성화 상태 확인
	thread_current()->status = THREAD_BLOCKED; // 현재 스레드 상태를 BLOCKED (대기)로 변경
	schedule();								   // 스케줄러 호출하여 다른 스레드 실행
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock(struct thread *t)
{
	enum intr_level old_level; // 이전 인터럽트 상태 저장

	ASSERT(is_thread(t)); // 주어진 스레드가 유효한 스레드인지 확인

	old_level = intr_disable();			 // 인터럽트 비활성화
	ASSERT(t->status == THREAD_BLOCKED); // 스레드가 BLOCKED (대기) 상태인지 확인

	// list_push_back (&ready_list, &t->elem); // 스레드를 준비 큐의 뒤에 추가
	list_insert_ordered(&ready_list, &t->elem, thread_compare_priority, NULL); // [*]1-2-1. 스레드를 우선순위 맞춰 준비 큐에 추가

	t->status = THREAD_READY;  // 스레드 상태를 READY (실행 준비)로 변경
	intr_set_level(old_level); // 인터럽트 이전 상태로 복원
}

/* Returns the name of the running thread. */
const char *
thread_name(void)
{
	return thread_current()->name; // 현재 실행 중인 스레드의 이름 반환
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current(void)
{
	struct thread *t = running_thread(); // 현재 실행 중인 스레드 획득

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT(is_thread(t));				 // t가 유효한 스레드인지 확인
	ASSERT(t->status == THREAD_RUNNING); // 스레드 상태가 RUNNING (실행 중)인지 확인

	return t; // 현재 실행 중인 스레드 반환
}

/* Returns the running thread's tid. */
tid_t thread_tid(void)
{
	return thread_current()->tid; // 현재 실행 중인 스레드의 tid 반환
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void)
{
	ASSERT(!intr_context()); // 인터럽트 컨텍스트가 아닌지 확인

#ifdef USERPROG
	process_exit(); // 사용자 프로그램 관련 정리 작업
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable();			   // 인터럽트 비활성화
	do_schedule(THREAD_DYING); // 현재 스레드 상태를 DYING (종료 중)으로 설정하고 스케줄러 호출
	NOT_REACHED();			   // 이 함수는 반환하지 않음
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void)
{
	struct thread *curr = thread_current(); // 현재 실행 중인 스레드 획득
	enum intr_level old_level;				// 이전 인터럽트 상태 저장

	ASSERT(!intr_context()); // 인터럽트 컨텍스트가 아닌지 확인

	old_level = intr_disable(); // 인터럽트 비활성화
	if (curr != idle_thread)	// 현재 스레드가 아이들 스레드가 아니면
		// list_push_back (&ready_list, &curr->elem); // 현재 스레드를 준비 큐의 뒤에 추가
		list_insert_ordered(&ready_list, &curr->elem, thread_compare_priority, NULL); // [*]1-2-1. 스레드를 우선순위 맞춰 준비 큐에 추가

	do_schedule(THREAD_READY); // 현재 스레드 상태를 READY (실행 준비)로 설정하고 스케줄러 호출
	intr_set_level(old_level); // 인터럽트 이전 상태로 복원
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority)
{
	// [*]1-2-2.현재 스레드의 우선순위를 new_priority로 설정
	// [*]1-2-3. 스레드 우선순위가 바뀔때(기부 받는 것 말고)는 init_priority가 바뀌도록 설정
	thread_current()->init_priority = new_priority;
	thread_refresh_priority(); // [*]1-2-3. donation 받은 priority와 새로 설정된 priority 비교
	thread_preemption();	   // [*]1-2-2. 현재 실행 중인 애와 레디 큐 헤드 비교 후 실행
}

/* Returns the current thread's priority. */
int thread_get_priority(void)
{
	return thread_current()->priority; // 현재 스레드의 우선순위 반환
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED)
{
	/* TODO: Your implementation goes here */ // TODO: 스레드의 nice 값 설정 (구현 필요)
}

/* Returns the current thread's nice value. */
int thread_get_nice(void)
{
	/* TODO: Your implementation goes here */ // TODO: 스레드의 nice 값 반환 (구현 필요)
	return 0;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void)
{
	/* TODO: Your implementation goes here */ // TODO: 시스템 부하 평균 값의 100배 반환 (구현 필요)
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void)
{
	/* TODO: Your implementation goes here */ // TODO: 현재 스레드의 최근 CPU 사용량 값의 100배 반환 (구현 필요)
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
static void
idle(void *idle_started_ UNUSED)
{
	struct semaphore *idle_started = idle_started_; // thread_start에서 전달된 세마포어

	idle_thread = thread_current(); // 현재 스레드를 아이들 스레드로 설정
	sema_up(idle_started);			// thread_start를 깨워 초기화 완료를 알림

	for (;;)
	{
		/* Let someone else run. */
		intr_disable(); // 인터럽트 비활성화
		thread_block(); // 현재 스레드를 블록 상태로 전환 (다른 스레드 실행 기회 부여)

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
		asm volatile("sti; hlt" : : : "memory"); // 인터럽트 활성화 후 CPU를 대기 상태로 전환 (전력 소모 감소)
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread(thread_func *function, void *aux)
{
	ASSERT(function != NULL); // 실행할 함수 포인터가 NULL이 아닌지 확인

	intr_enable(); /* The scheduler runs with interrupts off. */ // 스케줄러는 인터럽트 비활성화 상태에서 실행됨 (여기서는 활성화)
	function(aux); /* Execute the thread function. */			 // 스레드 함수 실행
	thread_exit(); /* If function() returns, kill the thread. */ // 함수가 반환되면 스레드 종료
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread(struct thread *t, const char *name, int priority)
{
	ASSERT(t != NULL);									// 스레드 구조체 포인터가 NULL이 아닌지 확인
	ASSERT(PRI_MIN <= priority && priority <= PRI_MAX); // 우선순위가 유효 범위 내에 있는지 확인
	ASSERT(name != NULL);								// 스레드 이름이 NULL이 아닌지 확인

	memset(t, 0, sizeof *t);						   // 스레드 구조체 내용을 0으로 초기화
	t->status = THREAD_BLOCKED;						   // 초기 상태를 BLOCKED (대기)로 설정
	strlcpy(t->name, name, sizeof t->name);			   // 스레드 이름 복사
	t->tf.rsp = (uint64_t)t + PGSIZE - sizeof(void *); // 스레드 스택 포인터 설정 (페이지 끝에서부터 아래로)
	t->priority = priority;							   // 스레드 우선순위 설정
	t->magic = THREAD_MAGIC;						   // 스택 오버플로 감지용 매직 값 설정

	// [*]1-2-3. 구조체 수정한거 초기화
	t->init_priority = priority;
	t->wait_on_lock = NULL;
	list_init(&t->donations);

	// [*]2-B. 구조체 수정한거 초기화
	list_init(&t->child_list);
	sema_init(&t->exit_sema, 0);
	sema_init(&t->free_sema, 0);
	sema_init(&t->fork_sema, 0);
	t->running = NULL; // [*]2-B. 추가

	// [*]2-B. exit_state 초기 설정은 0으로 변경(정상 종료)
	t->exit_status = 0;
	t->parent = NULL;
	t->fd_table = NULL; // fd_table 초기화
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run(void)
{
	if (list_empty(&ready_list))											 // 준비 큐가 비어있으면
		return idle_thread;													 // 아이들 스레드 반환
	else																	 // 준비 큐에 스레드가 있으면
		return list_entry(list_pop_front(&ready_list), struct thread, elem); // 준비 큐의 앞에서 스레드를 꺼내 반환
}

/* Use iretq to launch the thread */
void do_iret(struct intr_frame *tf)
{
	__asm __volatile(
		"movq %0, %%rsp\n"		// 인터럽트 프레임 주소를 스택 포인터로 설정
		"movq 0(%%rsp),%%r15\n" // 레지스터 r15 ~ rax복원
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
		"addq $120,%%rsp\n"				   // 스택 포인터 조정 (레지스터 값 복원 후)
		"movw 8(%%rsp),%%ds\n"			   // 데이터 세그먼트 복원
		"movw (%%rsp),%%es\n"			   // 엑스트라 세그먼트 복원
		"addq $32, %%rsp\n"				   // 스택 포인터 조정 (세그먼트 값 복원 후)
		"iretq"							   // 인터럽트 반환 (컨텍스트 전환)
		: : "g"((uint64_t)tf) : "memory"); // 입력: 인터럽트 프레임 주소, 메모리 수정
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch(struct thread *th)
{
	uint64_t tf_cur = (uint64_t)&running_thread()->tf; // 현재 실행 중인 스레드의 인터럽트 프레임 주소
	uint64_t tf = (uint64_t)&th->tf;				   // 새로 실행할 스레드의 인터럽트 프레임 주소
	ASSERT(intr_get_level() == INTR_OFF);			   // 인터럽트 비활성화 상태 확인

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
		"movw %%cs, 8(%%rax)\n"	 // cs
		"pushfq\n"
		"popq %%rbx\n"
		"mov %%rbx, 16(%%rax)\n" // eflags
		"mov %%rsp, 24(%%rax)\n" // rsp
		"movw %%ss, 32(%%rax)\n"
		"mov %%rcx, %%rdi\n"
		"call do_iret\n"
		"out_iret:\n"
		: : "g"(tf_cur), "g"(tf) : "memory"); // 현재 스레드의 레지스터 값을 저장하고 새 스레드의 컨텍스트로 전환
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status)
{
	ASSERT(intr_get_level() == INTR_OFF);				// 인터럽트 비활성화 상태 확인
	ASSERT(thread_current()->status == THREAD_RUNNING); // 인터럽트 비활성화 상태 확인
	while (!list_empty(&destruction_req))
	{																		   // 파괴 요청 리스트가 비어있지 않으면
		struct thread *victim =												   // 파괴할 스레드 획득
			list_entry(list_pop_front(&destruction_req), struct thread, elem); // 리스트 앞에서 스레드 구조체 추출
		palloc_free_page(victim);											   // 해당 스레드의 페이지 메모리 해제
	}
	thread_current()->status = status; // 현재 스레드 상태를 주어진 상태로 변경
	schedule();						   // 스케줄러 호출
}

static void
schedule(void)
{
	struct thread *curr = running_thread();		// 현재 실행 중인 스레드 획득
	struct thread *next = next_thread_to_run(); // 다음 실행할 스레드 획득

	ASSERT(intr_get_level() == INTR_OFF);	// 인터럽트 비활성화 상태 확인
	ASSERT(curr->status != THREAD_RUNNING); // 현재 스레드 상태가 RUNNING이 아닌지 확인
	ASSERT(is_thread(next));				// 다음 스레드가 유효한 스레드인지 확인
	/* Mark us as running. */
	next->status = THREAD_RUNNING; // 다음 스레드 상태를 RUNNING으로 설정

	/* Start new time slice. */
	thread_ticks = 0; // 타이머 틱 초기화

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate(next); // 다음 스레드의 주소 공간 활성화 (사용자 프로그램인 경우)
#endif

	if (curr != next)
	{ // 현재 스레드와 다음 스레드가 다르면 (스레드 전환 필요)
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread)
		{												   // 종료 중인 스레드가 있고 초기 스레드가 아니면
			ASSERT(curr != next);						   // 현재 스레드와 다음 스레드가 같지 않은지 확인
			list_push_back(&destruction_req, &curr->elem); // 파괴 요청 리스트에 현재 스레드 추가 (나중에 해제)
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch(next); // 다음 스레드 실행
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid(void)
{
	static tid_t next_tid = 1; // 다음 할당할 스레드 ID (정적 변수로 유지)
	tid_t tid;				   // 할당할 스레드 ID

	lock_acquire(&tid_lock); // tid 락 획득 (tid 할당은 임계 영역)
	tid = next_tid++;		 // 다음 tid 할당 및 next_tid 증가
	lock_release(&tid_lock); // tid 락 해제

	return tid; // 할당된 tid 반환
}

// [*]1-1. thread를 재움 (thread_yield 함수 참고)
void thread_sleep(int64_t ticks)
{

	/* if the current thread is not idle thread,
   change the state of the caller thread to BLOCKED,
   store the local tick to wake up,
   update the global tick if necessary,
   and call schedule() */
	/* When you manipulate thread list, disable interrupt! */

	struct thread *curr = thread_current(); // 현재 실행 중인 스레드 획득
	enum intr_level old_level;				// 이전 인터럽트 상태 저장

	ASSERT(!intr_context());	// 인터럽트 컨텍스트가 아닌지 확인
	old_level = intr_disable(); // 인터럽트 비활성화

	if (curr != idle_thread)
	{																			  // 현재 스레드가 아이들 스레드가 아니면
		curr->status = THREAD_BLOCKED;											  // 스레드 상태를 BLOCKED로 변경
		curr->wakeup_tick = ticks;												  // 스레드가 깨어날 틱 설정
		list_insert_ordered(&sleep_list, &curr->elem, compare_wakeup_tick, NULL); // sleep_list에 스레드를 정렬된 순서로 삽입

		if (min_wakeup_tick > ticks)
			min_wakeup_tick = ticks; // 글로벌 틱 업데이트
	}

	schedule();				   // 다른 스레드로 CPU 양도
	intr_set_level(old_level); // 인터럽트 이전 상태로 복원
}

// [*]1-1. thread를 깨움
void thread_wakeup(void)
{
	enum intr_level old_level;
	old_level = intr_disable();					   // 인터럽트 비활성화
	struct list_elem *e = list_begin(&sleep_list); // sleep_list의 첫 번째 요소부터 순회
	while (e != list_end(&sleep_list))
	{
		struct thread *t = list_entry(e, struct thread, elem); // 현재 요소를 스레드 구조체로 변환
		if (timer_ticks() >= t->wakeup_tick)
		{																			   // 현재 틱이 스레드가 깨어나야 할 시간보다 크거나 같으면
			e = list_remove(e);														   // sleep_list에서 스레드 제거
			t->status = THREAD_READY;												   // 스레드 상태를 READY로 변경
																					   // list_push_back (&ready_list, &t->elem); // ready_list에 스레드 추가
			list_insert_ordered(&ready_list, &t->elem, thread_compare_priority, NULL); // [*]1-2-1. 스레드를 우선순위 맞춰 준비 큐에 추가
		}
		else
		{
			break; // 순서대로 넣었으니까 로컬 틱이 아직 깰 시간이 아닌 애 만나면 빠져나오면 됨
		}
	}
	// min_wakeup_tick 업데이트
	if (!list_empty(&sleep_list))
	{
		struct thread *front = list_entry(list_front(&sleep_list), struct thread, elem);
		min_wakeup_tick = front->wakeup_tick;
	}
	else
	{
		min_wakeup_tick = INT64_MAX; // sleep_list가 비었을 경우
	}
	intr_set_level(old_level); // 인터럽트 이전 상태로 복원
}

// [*]1-1. thread를 재울 때 수면 큐에 넣을 위치 찾으려고 호출 (오름차순)
static bool
compare_wakeup_tick(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
	const struct thread *t_a = list_entry(a, struct thread, elem); // 첫 번째 스레드
	const struct thread *t_b = list_entry(b, struct thread, elem); // 두 번째 스레드

	return t_a->wakeup_tick < t_b->wakeup_tick; // wakeup_tick이 작은 스레드가 먼저 오도록 정렬
												// list_insert_ordered돌 때 삽입하려는 요소(a)가 검사 중인 요소(b)보다 작으면 true 반환, 그 위치에 insert 하게 함
}

// [*]1-2-1. priority 비교 (내림차순)
bool thread_compare_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
	const struct thread *t_a = list_entry(a, struct thread, elem); // 첫 번째 스레드
	const struct thread *t_b = list_entry(b, struct thread, elem); // 두 번째 스레드

	return t_a->priority > t_b->priority;
	// 삽입하려는 요소(a)가 검사 중인 요소(b)보다 크면 true 반환, 그 위치에 insert 하게 함
}

// [*]1-2-1. 실행 중인 애랑 지금 ready_list의 헤드에 있는 애랑 우선 순위 비교해서 실행
void thread_preemption(void)
{
	struct thread *now_running = thread_current();
	struct list_elem *e = list_begin(&ready_list); // 여기서 list_front 쓰면 리스트 비어있을 때 못 얻어 오는 경우 생겨서 fail
	struct thread *ready_head = list_entry(e, struct thread, elem);

	if (!list_empty(&ready_list) && (thread_current() != idle_thread) && (now_running->priority < ready_head->priority))
	{
		thread_yield();
	}
}

// [*]1-2-3. nested donation 처리
// 내가 기다리는 락을 가진 스레드에게 기부, 그 스레드도 다른 락을 기다리고 있다면 계속 전파
void donate_priority(void)
{
	int depth;
	struct thread *cur = thread_current();

	for (depth = 0; depth < 8; depth++)
	{
		if (!cur->wait_on_lock)
			break;
		struct thread *holder = cur->wait_on_lock->holder;
		holder->priority = cur->priority;
		cur = holder;
	}
}
// [*]1-2-3. 현재 스레드의 우선순위를 원래대로 복구하고, 남아 있는 기부 우선순위 중 가장 높은 것으로 보정
void thread_refresh_priority(void)
{
	struct thread *cur = thread_current();
	cur->priority = cur->init_priority;

	if (!list_empty(&cur->donations))
	{
		list_sort(&cur->donations, thread_compare_donate_priority, NULL);
		struct thread *front = list_entry(list_front(&cur->donations), struct thread, d_elem);
		if (front->priority > cur->priority)
			cur->priority = front->priority;
	}
}

// [*]1-2-3. 우선순위로 정렬된 donation 리스트에서 insert를 위해 사용
bool thread_compare_donate_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
	const struct thread *t_a = list_entry(a, struct thread, d_elem);
	const struct thread *t_b = list_entry(b, struct thread, d_elem);
	return t_a->priority > t_b->priority;
}

// [*]1-2-3. 현재 스레드가 기부받은 priority 중
// 이번에 해제한 lock을 기다리던 애들로부터 받은 것만 donations 리스트에서 제거
void remove_with_lock(struct lock *lock)
{
	struct list_elem *e;
	struct thread *cur = thread_current();

	for (e = list_begin(&cur->donations); e != list_end(&cur->donations); e = list_next(e))
	{
		struct thread *t = list_entry(e, struct thread, d_elem);
		if (t->wait_on_lock == lock)
			list_remove(&t->d_elem);
	}
}
