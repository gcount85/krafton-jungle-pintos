#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

/******* P2 syscall: 헤더 & 프로토타입 추가 - 시작 *******/
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "lib/kernel/stdio.h"
#include "userprog/process.h" // exec 헤더
#include <string.h>           // strcpy를 위한?
#include "threads/palloc.h"   // EXEC PALLOG GET PAGE 헤더
struct file* find_file_by_fd(int fd);
void remove_file_from_fdt(int fd);
int add_file_to_fdt(struct file* file);

/******* P2 syscall: 헤더 & 프로토타입 추가 - 끝 *******/

void syscall_entry(void);
void syscall_handler(struct intr_frame*);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

/***************** P2 syscall: 매크로 추가 *****************/
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDIN_FILE 1  // P2 sys call: FDT의 FD 0 매크로 선언
#define STDOUT_FILE 2 // P2 sys call: FDT의 FD 1 매크로 선언
/***************** P2 syscall: 매크로 추가 - 끝 *****************/

void syscall_init(void)
{
    write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* The interrupt service rountine should not serve any interrupts
     * until the syscall_entry swaps the userland stack to the kernel
     * mode stack. Therefore, we masked the FLAG_FL. */
    write_msr(MSR_SYSCALL_MASK, FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

    /********* P2 syscall: filesys_lock 초기화를 위한 코드 추가 *********/
    lock_init(&filesys_lock);
    /********* P2 syscall: filesys_lock 초기화를 위한 코드 추가 - 끝 *********/
}

/******* P2 syscall: TODO The main system call interface & body *******/
/* ***** 중요한 점 ******
 * 1. syscall-nr.h 에서 번호 확인
 * 2. 유저가 준 포인터 에러처리; (void *)0 같은 포인터 넘겨주는 경우 등
 * (2번의 경우 포인터를 받는 함수들만 check_address 만들어서 확인하기) */
void syscall_handler(struct intr_frame* f)
{
    // TODO: Your implementation goes here.
    // 	`%rdi`, `%rsi`, `%rdx`, `%r10`, `%r8`, and `%r9`
    switch (f->R.rax)
    {
    case SYS_HALT: /* Halt the operating system. */
        halt();
        break;
    case SYS_EXIT: /* Terminate this process. */
        exit(f->R.rdi);
        break;
    case SYS_FORK: /* Clone current process. */
        memcpy(&thread_current()->parent_if, f, sizeof(struct intr_frame));
        f->R.rax = fork(f->R.rdi);
        break;
    case SYS_EXEC: /* Switch current process. */
        f->R.rax = exec(f->R.rdi);
        if (f->R.rax == -1)
        {
            exit(-1);
        }
        break;
    case SYS_WAIT: /* Wait for a child process to die. */
        f->R.rax = wait(f->R.rdi);
        break;
    case SYS_CREATE: /* Create a file. */
        f->R.rax = create(f->R.rdi, f->R.rsi);
        break;
    case SYS_REMOVE: /* Delete a file. */
        f->R.rax = remove(f->R.rdi);
        break;
    case SYS_OPEN: /* Open a file. */
        f->R.rax = open(f->R.rdi);
        break;
    case SYS_FILESIZE: /* Obtain a file's size. */
        f->R.rax = filesize(f->R.rdi);
        break;
    case SYS_READ: /* Read from a file. */
        f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
        break;
    case SYS_WRITE: /* Write to a file. */
        f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
        break;
    case SYS_SEEK: /* Change position in a file. */
        seek(f->R.rdi, f->R.rsi);
        break;
    case SYS_TELL: /* Report current position in a file. */
        f->R.rax = tell(f->R.rdi);
        break;
    case SYS_CLOSE:
        close(f->R.rdi);
        break;
    default:
        exit(-1);
    }
    // printf("system call!\n");
}

// P2 sys call: 유저가 준 포인터가 유효한 지 확인
void check_address(void* addr)
{
    struct thread* cur = thread_current();
    if (!addr || !is_user_vaddr(addr) || pml4_get_page(cur->pml4, addr) == NULL)
    {
        exit(-1);
    }
}

/* P2 syscall: sys call body */
void halt(void)
{
    power_off();
}

void exit(int status)
{
    struct thread* cur = thread_current();
    set_exit_status(cur->parent, cur->tid, status);
    printf("%s: exit(%d)\n", cur->name, status);
    thread_exit();
}

tid_t fork(const char* thread_name)
{
    return process_fork(thread_name, &thread_current()->parent_if);
}

int exec(const char* cmd_line)
{
    /********* P2 syscall: 추가 코드 - 시작 *********/
    check_address(cmd_line);
    tid_t tid;
    char* fn_copy;
    int fn_size = strlen(cmd_line) + 1;

    fn_copy = palloc_get_page(PAL_ZERO);
    if (fn_copy == NULL)
        exit(-1);
    strlcpy(fn_copy, cmd_line, fn_size);

    if (process_exec(fn_copy) == -1)
    {
        exit(-1);
    }

    NOT_REACHED();
    return;

    /********* P2 syscall: 추가 코드 - 끝 *********/
}

int wait(tid_t pid)
{
    return process_wait(pid);
}

// P2 sys call; `initial_size`를 가진 파일을 만듦
bool create(const char* file, unsigned initial_size)
{
    check_address(file);

    lock_acquire(&filesys_lock);
    bool bool_created = filesys_create(file, initial_size);
    lock_release(&filesys_lock);
    return bool_created;
}

// P2 sys call; `file`이라는 이름을 가진 파일 삭제
bool remove(const char* file)
{
    check_address(file);

    lock_acquire(&filesys_lock);
    bool bool_removed = filesys_remove(file);
    lock_release(&filesys_lock);
    return bool_removed;
}

// P2 sys call: `file` 안의 path에 있는 file open + fd 반환
int open(const char* file)
{
    check_address(file);

    /* Try to open file */
    lock_acquire(&filesys_lock);
    struct file* open_file = filesys_open(file);
    lock_release(&filesys_lock);

    /* Return -1 when file could not be opened */
    if (open_file == NULL)
    {
        return -1;
    }

    /* Add file to File descriptor table */
    struct thread* curr = thread_current();

    /*Find an empty entry*/
    while ((curr->next_fd < OPEN_MAX) && (curr->fdt[curr->next_fd]))
    {
        curr->next_fd++;
    }

    /*If FDT is full, return -1*/
    if (curr->next_fd >= OPEN_MAX)
    {
        file_close(open_file);
        return -1;
    }

    /* Add file into FDT */
    curr->fdt[curr->next_fd] = open_file;

    int fd = curr->next_fd;

    return fd;
}

// P2 sys call: `fd`로 open 되어 있는 파일의 바이트 사이즈 반환
int filesize(int fd)
{
    struct thread* cur = thread_current();
    struct file* f = cur->fdt[fd];

    if (fd < 2 || f == 0)
    {
        return -1;
    }

    return file_length(f);
}

// P2 sys call: fd`로 open 되어 있는 파일로부터 `size` 바이트를 읽어내 `buffer`에 담음 + 실제로 읽어낸 바이트 반환
int read(int fd, void* buffer, unsigned size)
{
    check_address(buffer);
    lock_acquire(&filesys_lock);
    struct thread* cur = thread_current();

    // ERROR: bad fd
    if (fd < 0 || fd == STDOUT_FILENO || fd >= OPEN_MAX)
    {
        lock_release(&filesys_lock);
        return -1;
    }

    struct file* f = cur->fdt[fd];

    // ERROR: no file
    if (f == NULL)
    {
        lock_release(&filesys_lock);
        return -1;
    }

    // standard input (여기 다름! )
    if (f == STDIN_FILE)
    {
        lock_release(&filesys_lock);
        return input_getc();
    }
    off_t offset = file_read(f, buffer, size);
    lock_release(&filesys_lock);
    return offset; // 0 반환 → 파일의 끝을 의미
}

// P2 sys call: `buffer`에 담긴 `size` 바이트를 open file `fd`에 쓰기 + 실제로 쓰인 바이트의 숫자를 반환
int write(int fd, const void* buffer, unsigned size)
{
    check_address(buffer);

    struct thread* cur = thread_current();
    off_t byte_written;

    if (fd < 0 || fd == STDIN_FILENO || fd >= OPEN_MAX) // 에러조건문 추가
    {
        return -1;
    }

    struct file* f = cur->fdt[fd];

    if (f == NULL)
    {
        return -1;
    }

    if (f == STDOUT_FILE)
    {
        putbuf(buffer, size);
        return size;
    }

    lock_acquire(&filesys_lock);
    byte_written = file_write(f, buffer, size);
    lock_release(&filesys_lock);
    return byte_written;
}

// P2 sys call: 파일의 위치(offset)를 이동하는 함수. open file `fd`에서 읽히거나 적혀야 하는 다음 바이트를 `position` 위치로 바꿈
void seek(int fd, unsigned new_position)
{
    struct thread* cur = thread_current();

    if (fd < 2 || cur->fdt[fd] == 0)
    {
        return;
    }
    file_seek(cur->fdt[fd], new_position);
}

// P2 sys call: open file `fd`에서 읽히거나 적혀야 하는 다음 바이트의 포지션을 반환
unsigned tell(int fd)
{
    struct thread* cur = thread_current();

    if (fd < 2 || cur->fdt[fd] == 0)
    {
        return;
    }

    return file_tell(cur->fdt[fd]);
}

// 러닝 스레드의 열린 파일 모두 닫음
void close(int fd)
{
    struct thread* cur = thread_current();
    if (fd < 2 || fd >= OPEN_MAX)
    {
        return;
    }
    struct file* f = cur->fdt[fd];
    if (f == NULL)
    {
        return;
    }
    cur->fdt[fd] = NULL; // 이거 f = 0; 으로 하면 에러남! 조심!
    // lock_acquire(&filesys_lock);
    file_close(f);
    // lock_release(&filesys_lock);
}

/*Helper Functions*/
/*Add file to File Descriptor Table
  Return file descriptor on success, -1 on failure
*/
int add_file_to_fdt(struct file* file)
{
    struct thread* curr = thread_current();

    /*Find an empty entry*/
    while ((curr->next_fd < OPEN_MAX) && (curr->fdt[curr->next_fd]))
    {
        curr->next_fd++;
    }

    /*If FDT is full, return -1*/
    if (curr->next_fd >= OPEN_MAX)
    {
        return -1;
    }

    /* Add file into FDT */
    curr->fdt[curr->next_fd] = file;

    return curr->next_fd;
}

/*Search thread's fdt and return file struct pointer*/
struct file* find_file_by_fd(int fd)
{
    struct thread* curr = thread_current();

    if (fd < 0 || fd >= OPEN_MAX)
    {
        return NULL;
    }

    return curr->fdt[fd];
}

/*Remove file from file descriptor table*/
void remove_file_from_fdt(int fd)
{
    struct thread* curr = thread_current();

    if (fd < 0 || fd >= OPEN_MAX)
    {
        return;
    }

    curr->fdt[fd] = NULL;
}
