#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include <filesys/filesys.h>
#include <filesys/file.h>
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
typedef int pid_t;
#include "threads/palloc.h"
#include <string.h>
#include "userprog/process.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *f);
void sys_halt (void);
void sys_exit (int status);
int sys_write(int fd, const void *buffer, unsigned size);
int sys_exec (const char *cmd_line);
int sys_open(const char *file);
void sys_close(int fd);
bool sys_create (const char *file, unsigned initial_size);
bool sys_remove (const char *file);
int sys_filesize (int fd);
int sys_read (int fd, void *buffer, unsigned size);
void sys_seek (int fd, unsigned position);
unsigned sys_tell (int fd);
void check_address(void *addr);
pid_t sys_fork(const char *thread_name, struct intr_frame *fff);
static struct file *find_file_by_fd(int fd);
int sys_wait(pid_t pid);
void check_buffer(void *buffer, unsigned size);

/*
이 파일에서 프로세스 생성과 실행을 관리한다
유저프로세스가 커널 기능에 접근하고 싶을 때, 시스템 콜을 호출하여 커널에게 요청한다

현재는 메세지만 출력하고 프로세스를 종료하는 기능만 있다

시스템 콜에 필요한 나머지 기능을 여기에 구현해야한다
*/

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081			/* Segment selector msr */
#define MSR_LSTAR 0xc0000082		/* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */


void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			  FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
  lock_init(&filesys_lock); // [*]2-K: 락 초기화
}

/* The main system call interface */
// [*]2-K : 기본 함수에 시스템콜 넘버에 따른 분기 추가
void
syscall_handler (struct intr_frame *f UNUSED) {
  // TODO: Your implementation goes here.

  /* rax = 시스템 콜 넘버 */
  int syscall_n = f->R.rax; /* 시스템 콜 넘버 */
  switch (syscall_n)
  {
  case SYS_HALT:
    sys_halt();
    break;
  case SYS_EXIT:
    sys_exit(f->R.rdi);
    break;
  case SYS_WRITE:
    f->R.rax = sys_write(f->R.rdi, f->R.rsi, f->R.rdx);
    break;
  case SYS_EXEC:
    if (sys_exec(f->R.rdi) == -1) {
      sys_exit(-1);
      }
    break;
  case SYS_FORK:
    f->R.rax = sys_fork(f->R.rdi, f);
    break;
  case SYS_OPEN:
    f->R.rax = sys_open(f->R.rdi);
    break;
  case SYS_CLOSE:
    sys_close(f->R.rdi);
    break;
  case SYS_CREATE:
    f->R.rax = sys_create(f->R.rdi, f->R.rsi);
    break;
  case SYS_READ:
    f->R.rax = sys_read(f->R.rdi, f->R.rsi, f->R.rdx);
    break;
  case SYS_REMOVE:
    f->R.rax = sys_remove(f->R.rdi);
    break;
  case SYS_SEEK:
    sys_seek(f->R.rdi, f->R.rsi);
    break;
  case SYS_TELL:
    f->R.rax = sys_tell(f->R.rdi);
    break;
  case SYS_FILESIZE:
    f->R.rax = sys_filesize(f->R.rdi);
    break;
  case SYS_WAIT:
    f->R.rax = sys_wait(f->R.rdi);
    break;
  default:
    thread_exit ();
    break;
  }
}

// [*]2-K : 커널 halt는 프로그램 종료
void 
sys_halt(void) {
  power_off();
}

// [*]2-K : 커널 exit은 상태값을 받아서 출력 후 종료
void 
sys_exit(int status) {
  struct thread *cur = thread_current();
  
  // 정상적으로 종료됐으면 status는 0을 받는다.
  cur->exit_status = status;
  printf("%s: exit(%d)\n", cur->name, status);
  thread_exit();  // process_exit() → schedule() → _cleanup
}

// // [*]2-K : 커널 write
int
sys_write(int fd, const void *buffer, unsigned size) {
  // check_address(buffer);

  check_buffer(buffer, size);
  struct file *file = find_file_by_fd(fd);
  int bytes_written = 0;
  // 파일이 없거나, 표준입력인 경우 -1 리턴
  if (file == NULL && fd == 0)
    return -1;
  // 표준출력인 경우 콘솔에 출력
  if (fd == 1)
  {
    putbuf(buffer, size);
    bytes_written = size;
  } else {
    lock_acquire(&filesys_lock);
    bytes_written = file_write(file, buffer, size);
    lock_release(&filesys_lock);
    return bytes_written;
  }  
}

// [*]2-K 커널 fork
pid_t sys_fork(const char *thread_name, struct intr_frame *fff){
  check_address(thread_name);
  return process_fork(thread_name, fff);
}

// [*]2-K 커널 exec
int
sys_exec(const char *cmd_line) {
  // 유저 영역에서 커널 영역 침범하지 않았는지 확인

  check_address(cmd_line);
  // 커널 영역에 명령어 복사를 위한 공간 확보
  int cmd_line_size = strlen(cmd_line) + 1;
  char *cm_copy = palloc_get_page(PAL_ZERO);  // 커널 메모리 확보
  if (cm_copy == NULL)
  { 
    sys_exit(-1);
  }
  strlcpy(cm_copy, cmd_line, cmd_line_size);  // 안전하게 복사해둠
  
  // file 실행이 실패했다면 -1을 리턴한다.
  if (process_exec(cm_copy) == -1)
  {   
      return -1;
  }
  // 정상적으로 process_exec 실행되면 아래 부분은 실행되지 않음.
  NOT_REACHED();
  return 0;
}

// [*]2-K 커널 open
int 
sys_open(const char *file)
{

  check_address(file);
  lock_acquire(&filesys_lock);
  struct file *open_file = filesys_open(file);

  if (open_file == NULL)
  {
    lock_release(&filesys_lock);
    return -1;
  }
  // fd_table에 file추가
  int fd = add_file_to_fdt(open_file);

  // fd_table 가득 찼을경우
  if (fd == -1)
  {
    file_close(open_file);
    lock_release(&filesys_lock);
    return -1; // 즉시 반환
  }
  lock_release(&filesys_lock);
  return fd;
}

// [*]2-K 커널 close, 이미 open된 파일 닫음
void 
sys_close(int fd){
  struct thread *cur = thread_current ();
  if (fd < 2 || fd >= OPEN_LIMIT || cur->fd_table[fd] == NULL)
    return;
  cur->fd_table[fd] = NULL;
  
  lock_acquire(&filesys_lock);
  file_close(cur->fd_table[fd]);
    lock_release(&filesys_lock);
}


int sys_wait(pid_t pid){
  return process_wait(pid);
}

// [*]2-K 커널 create
bool
sys_create (const char *file, unsigned initial_size) {
    
  check_address(file);
  lock_acquire(&filesys_lock);
  
  // if (filesys_create(file, initial_size)) {
  //     lock_release(&filesys_lock);
  //     return true;
  // } else {
  //     lock_release(&filesys_lock);
  //     return false;
  // }
  bool success = filesys_create(file, initial_size);
  lock_release(&filesys_lock);

  return success;
}

// [*]2-K 커널 read, 사용자 입력일 때
int
sys_read(int fd, void *buffer, unsigned size)
{
  // check_address(buffer);

  check_buffer(buffer, size);
  
  // 읽은 바이트 수 저장할 변수
  int read_byte = 0;
  // 버퍼를 바이트 단위로 접근하기 위한 포인터
  uint8_t *read_buffer = buffer;

  // 표준입력일 경우 데이터를 읽는다
  if (fd == 0)
  {
    char key;
    for (read_byte = 0; read_byte < size; read_byte++)
    {
      // input_getc 함수로 입력을 가져오고, buffer에 저장한다
      key = input_getc();
      *read_buffer++ = key;

      
      // 널 문자를 만나면 종료한다.
      // if (key == '\0'){
      //   break;
      }
    }
  
  // 표준출력일 경우 -1을 리턴
  else if (fd == 1){
      return -1;
  }
  // 2이상, 즉 파일일 경우 파일을 읽어온다.
  else {
    struct file *file = find_file_by_fd(fd);
    if (file == NULL){
        return -1;
    }
    lock_acquire(&filesys_lock);
    read_byte = file_read(file, buffer, size);
    lock_release(&filesys_lock);
  }

  // 읽어온 바이트 수 리턴
  return read_byte;
}

// [*]2-K 커널 remove, 디스크에서 파일 지움
bool
sys_remove (const char *file) {

  check_address(file);

  lock_acquire(&filesys_lock);
  bool success = filesys_remove(file);
  lock_release(&filesys_lock);

  return success;
}

// [*]2-K 커널 seek, fdt에서 file 위치 찾기
void 
sys_seek (int fd, unsigned position){
  struct file *file = find_file_by_fd(fd);

  // [*]2-o **여기 주석하니까 syn-write 통과
  //check_address(file);
  if (fd < 2 || fd >= OPEN_LIMIT || file == NULL) return;

  lock_acquire(&filesys_lock);
  file_seek(file, position);
  lock_release(&filesys_lock);
}

// [*]2-K 커널 tell, 시작 위치 변경
unsigned sys_tell (int fd){
  struct file *file = find_file_by_fd(fd);

  check_address(file);
  if (fd < 2 || fd >= OPEN_LIMIT || file == NULL) return;

  return file_tell(file);
}

// [*]2-K 커널 filesize, fd 파일 길이 반환
int 
sys_filesize (int fd){
  struct file *file = find_file_by_fd(fd);

  if (fd < 2 || fd >= OPEN_LIMIT || file == NULL) return -1;

  lock_acquire(&filesys_lock);
  int length = file_length(file);
  lock_release(&filesys_lock);

  return length;
}

// [*]2-K 유저 영역에서 커널 영역 침범하지 않았는지 확인
void 
check_address(void *addr) {
  struct thread *t = thread_current();

  if (!is_user_vaddr(addr) || addr == NULL || pml4_get_page(t->pml4, addr) == NULL)
  {
      sys_exit(-1);
  }
}

// [*]2-B. 버퍼 전체범위 검사
void check_buffer(void *buffer, unsigned size) {
    uint8_t *start = buffer;
    uint8_t *end = start + size;
    for (; start < end; start += PGSIZE) {
        check_address(start);
    }
}

// [*]2-K: 파일을 현재 프로세스의 fdt에 추가
int
add_file_to_fdt (struct file *file)
{
  struct thread *cur = thread_current ();
    for (int fd = 3; fd < OPEN_LIMIT; fd++) {
      if (cur->fd_table[fd] == NULL) {
          cur->fd_table[fd] = file;
          return fd;
      }
  }
  return -1;
}

// [*]2-K: fd 값을 넣으면 해당 file을 반환하는 함수
static struct file *find_file_by_fd(int fd)
{
    struct thread *cur = thread_current();
    if (fd < 0 || fd >= OPEN_LIMIT)
        return NULL;

    return cur->fd_table[fd];
}
