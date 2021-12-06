/* arch-specific syscall definitions */
struct stat {
    /* 0 - 3 */
    u64 st_dev;
    u64 st_ino;
    u32 st_mode;
    u32 st_nlink;
    u32 st_uid;
    u32 st_gid;

    /* 4 - 7 */
    u64 st_rdev;
    u64 pad1;
    s64 st_size;
    s32 st_blksize;
    s32 pad2;

    /* 8 - 11 */
    s64 st_blocks;
    s64 st_atime;
    u64 st_atime_nsec;
    s64 st_mtime;

    /* 12 - 15 */
    u64 st_mtime_nsec;
    s64 st_ctime;
    u64 st_ctime_nsec;
    s32 unused[2];
} __attribute__((packed));

#define O_DIRECTORY     00040000
#define O_NOFOLLOW      00100000
#define O_DIRECT        00200000
#define O_LARGEFILE     00400000

struct epoll_event {
    u32 events;                 /* Epoll events */
    u64 data;
};

/* kernel stuff */

#define SYSCALL_FRAME_ARG0       FRAME_X0
#define SYSCALL_FRAME_ARG1       FRAME_X1
#define SYSCALL_FRAME_ARG2       FRAME_X2
#define SYSCALL_FRAME_ARG3       FRAME_X3
#define SYSCALL_FRAME_ARG4       FRAME_X4
#define SYSCALL_FRAME_ARG5       FRAME_X5
#define SYSCALL_FRAME_RETVAL1    FRAME_X0
#define SYSCALL_FRAME_RETVAL2    FRAME_X1
#define SYSCALL_FRAME_SP         FRAME_SP
#define SYSCALL_FRAME_SP_TOP     FRAMTE_STACK_TOP
#define SYSCALL_FRAME_PC         FRAME_ELR

#define MINSIGSTKSZ 5120

struct sigcontext {
    u64 fault_address;
    u64 regs[31];
    u64 sp;
    u64 pc;
    u64 pstate;
    u8 reserved[4096] __attribute__((__aligned__(16)));
};

struct _aarch64_ctx {
    u32 magic;
    u32 size;
};

#define FPSIMD_MAGIC 0x46508001

struct fpsimd_context {
    struct _aarch64_ctx head;
    u32 fpsr;
    u32 fpcr;
    u128 vregs[32];
};

struct ucontext {
    unsigned long uc_flags;
    struct ucontext * uc_link;
    stack_t uc_stack;
    sigset_t uc_sigmask;
    u8 pad[1024 / 8 - sizeof(sigset_t)];
    struct sigcontext uc_mcontext;
};

struct rt_sigframe {
    struct siginfo info;
    struct ucontext uc;
};

static inline pageflags pageflags_from_vmflags(u64 vmflags)
{
    pageflags flags = pageflags_user(pageflags_memory());
    if (vmflags & VMAP_FLAG_EXEC)
        flags = pageflags_exec(flags);
    if (vmflags & VMAP_FLAG_WRITABLE)
        flags = pageflags_writable(flags);
    return flags;
}

static inline void set_tls(context_frame f, u64 tls)
{
    f[FRAME_TPIDR_EL0] = tls;
    f[FRAME_TXCTX_FLAGS] |= FRAME_TXCTX_TPIDR_EL0_SAVED;
}

static inline void syscall_restart_arch_setup(context_frame f)
{
    f[FRAME_SAVED_X0] = f[FRAME_X0];
}

static inline void syscall_restart_arch_fixup(context_frame f)
{
    /* rewind to syscall */
    f[FRAME_X0] = f[FRAME_SAVED_X0];
    f[FRAME_ELR] -= 4; /* rewind to syscall; no thumb mode support */
}

void frame_save_fpsimd(context_frame f);
void frame_restore_fpsimd(context_frame f);

static inline void thread_frame_save_fpsimd(context_frame f)
{
    if ((f[FRAME_TXCTX_FLAGS] & FRAME_TXCTX_FPSIMD_SAVED) == 0) {
        f[FRAME_TXCTX_FLAGS] |= FRAME_TXCTX_FPSIMD_SAVED;
        frame_save_fpsimd(f);
    }
}

static inline void thread_frame_restore_fpsimd(context_frame f)
{
    if (f[FRAME_TXCTX_FLAGS] & FRAME_TXCTX_FPSIMD_SAVED) {
        f[FRAME_TXCTX_FLAGS] &= ~FRAME_TXCTX_FPSIMD_SAVED;
        frame_restore_fpsimd(f);
    }
}

static inline void thread_frame_save_tls(context_frame f)
{
    if ((f[FRAME_TXCTX_FLAGS] & FRAME_TXCTX_TPIDR_EL0_SAVED) == 0) {
        f[FRAME_TXCTX_FLAGS] |= FRAME_TXCTX_TPIDR_EL0_SAVED;
        f[FRAME_TPIDR_EL0] = read_psr(TPIDR_EL0);
    }
}

static inline void thread_frame_restore_tls(context_frame f)
{
    if (f[FRAME_TXCTX_FLAGS] & FRAME_TXCTX_TPIDR_EL0_SAVED) {
        f[FRAME_TXCTX_FLAGS] &= ~FRAME_TXCTX_TPIDR_EL0_SAVED;
        write_psr(TPIDR_EL0, f[FRAME_TPIDR_EL0]);
    }
}
