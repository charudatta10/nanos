#if !(defined(KERNEL) || defined(BOOT))
#error must be in kernel or bootloader build
#endif

#define MBR_ADDRESS 0x7c00

/*
  bit  47    - kern / user, extended for canonical
  bit  46    - set if directly-mapped (tag area must be zero)
  bits 45:38 - tag
 */

#define KERNEL_BASE             0xffffffff80000000ull
#define KERNEL_LIMIT            0xfffffffffffff000ull // XXX ? klibs
#define KMEM_LIMIT              0xffffbfff00000000ull
#define PAGES_BASE              0xffffffffc0000000ull
#define LINEAR_BACKED_LIMIT     0xffffffff00000000ull
#define LINEAR_BACKED_BASE      0xffffc00000000000ull
#define LINEAR_BACKED_PHYSLIMIT 0x00003fff00000000ull
#define LINEAR_BACKED_PAGELOG   30
#define HUGE_PAGESIZE           0x100000000ull

#define KERNEL_BASE_PHYS    0x00200000ul
#define STACK_ALIGNMENT     16

#define VIRTUAL_ADDRESS_BITS 48

#define KERNEL_CODE_SELECTOR 0x08
#define USER_CODE32_SELECTOR 0x18

#define TSC_DEADLINE_MSR 0x6e0

#define EFER_MSR         0xc0000080
#define EFER_SCE         0x0001
#define EFER_LME         0x0100
#define EFER_LMA         0x0400
#define EFER_NXE         0x0800
#define EFER_SVME        0x1000
#define EFER_LMSLE       0x2000
#define EFER_FFXSR       0x4000
#define EFER_TCE         0x8000
#define STAR_MSR         0xc0000081
#define LSTAR_MSR        0xc0000082
#define SFMASK_MSR       0xc0000084

#define FS_MSR           0xc0000100
#define GS_MSR           0xc0000101
#define KERNEL_GS_MSR    0xc0000102
#define TSC_AUX_MSR      0xc0000103

#define C0_MP   0x00000002
#define C0_EM   0x00000004
#define C0_WP   0x00010000

#define CR4_PAE         (1 << 5)
#define CR4_PGE         (1 << 7)
#define CR4_OSFXSR      (1 << 9)
#define CR4_OSXMMEXCPT  (1 << 10)
#define CR4_OSXSAVE     (1 << 18)

#define EFLAG_CARRY                     0
#define EFLAG_FIXED                     1
#define EFLAG_PARITY                    2
#define EFLAG_AUX_CARRY                 4
#define EFLAG_ZERO                      6
#define EFLAG_SIGN                      7
#define EFLAG_TRAP                      8
#define EFLAG_INTERRUPT                 9
#define EFLAG_DIRECTION                 10
#define EFLAG_OVERFLOW                  11
#define EFLAG_IOPL                      12
#define EFLAG_NESTED_TASK               14
#define EFLAG_RESUME                    16
#define EFLAG_VIRTUAL_MODE              17
#define EFLAG_ALIGN_CHECK               18
#define EFLAG_VIRTUAL_INTERRUPT         19
#define EFLAG_VIRTUAL_INTERRUPT_PENDING 20
#define EFLAG_CPUID_DETECT              21

#define SAFE_EFLAGS \
    (U64_FROM_BIT(EFLAG_ALIGN_CHECK) | \
     U64_FROM_BIT(EFLAG_OVERFLOW) | \
     U64_FROM_BIT(EFLAG_DIRECTION) | \
     U64_FROM_BIT(EFLAG_TRAP) | \
     U64_FROM_BIT(EFLAG_SIGN) | \
     U64_FROM_BIT(EFLAG_ZERO) | \
     U64_FROM_BIT(EFLAG_AUX_CARRY) | \
     U64_FROM_BIT(EFLAG_PARITY) | \
     U64_FROM_BIT(EFLAG_CARRY) | \
     U64_FROM_BIT(EFLAG_RESUME))

#define TSS_SIZE 0x68

struct cpuinfo_machine;

/* AP boot page */
extern void * AP_BOOT_PAGE;
#define AP_BOOT_START u64_from_pointer(&AP_BOOT_PAGE)
#define AP_BOOT_END (AP_BOOT_START + PAGESIZE)

/* interrupt control */
static inline void enable_interrupts()
{
    asm volatile("sti");
}

static inline void disable_interrupts()
{
    asm volatile("cli");
}

static inline u32 read_eflags(void)
{
    u32 out;
    asm volatile("pushfd; popl %0" : "=r"(out) :: "memory");
    return out;
}

static inline u64 read_flags(void)
{
    u64 out;
    asm volatile("pushfq; popq %0" : "=r"(out) :: "memory");
    return out;
}

static inline u64 irq_disable_save(void)
{
    u64 flags = read_flags();
    disable_interrupts();
    return flags;
}

static inline u64 irq_enable_save(void)
{
    u64 flags = read_flags();
    enable_interrupts();
    return flags;
}

static inline void irq_restore(u64 flags)
{
    asm volatile("push %0; popf" :: "g"(flags) : "memory", "cc");
}

static inline void wait_for_interrupt(void)
{
    asm volatile("sti; hlt" ::: "memory");
}

void triple_fault(void) __attribute__((noreturn));
void start_cpu(int index);
void allocate_apboot(heap stackheap, void (*ap_entry)());
void deallocate_apboot(heap stackheap);
void install_idt(void);
void init_cpu_features();

#define IST_EXCEPTION 1
#define IST_INTERRUPT 2

void set_ist(struct cpuinfo_machine *cpu, int i, u64 sp);
void install_gdt64_and_tss(void *tss_desc, void *tss, void *gdt, void *gdt_pointer);

#ifdef KERNEL
/* locking constructs */
#include <lock.h>
#endif

/* device mmio region access */
static inline u32 mmio_read_32(u64 addr)
{
    u32 val;
    asm volatile("movl %1, %0" : "=r"(val) :
                 "m"(*(volatile u32 *)pointer_from_u64(addr)));
    return val;
}

static inline u64 mmio_read_64(u64 addr)
{
    u64 val;
    asm volatile("movq %1, %0" : "=r"(val) :
                 "m"(*(volatile u64 *)pointer_from_u64(addr)));
    return val;
}

static inline void mmio_write_32(u64 addr, u32 val)
{
    asm volatile("movl %0, %1" :: "r"(val),
                 "m"(*(volatile u64 *)pointer_from_u64(addr)));
}

static inline void mmio_write_64(u64 addr, u64 val)
{
    asm volatile("movq %0, %1" :: "r"(val),
                 "m"(*(volatile u64 *)pointer_from_u64(addr)));
}

/* special register access */
extern u64 read_msr(u64);
extern void write_msr(u64, u64);
extern u64 read_xmsr(u64);
extern void write_xmsr(u64, u64);

#define mov_to_cr(__x, __y) asm volatile("mov %0,%%"__x : : "a"(__y) : "memory");
#define mov_from_cr(__x, __y) asm volatile("mov %%"__x", %0" : "=a"(__y) : : "memory");

static inline void cpuid(u32 fn, u32 ecx, u32 * v)
{
    asm volatile("cpuid" : "=a" (v[0]), "=b" (v[1]), "=c" (v[2]), "=d" (v[3]) : "0" (fn), "2" (ecx));
}

static inline void xsetbv(u32 ecx, u32 eax, u32 edx)
{
    asm volatile("xsetbv" : : "a" (eax), "d" (edx), "c" (ecx));
}

static inline void xgetbv(u32 ecx, u32 *eax, u32 *edx)
{
    asm volatile("xgetbv" : "=a" (*eax), "=d" (*edx) : "c" (ecx));
}

/* syscall entry */

static inline void set_syscall_handler(void *syscall_entry)
{
    write_msr(LSTAR_MSR, u64_from_pointer(syscall_entry));
    u32 selectors = ((USER_CODE32_SELECTOR | 0x3) << 16) | KERNEL_CODE_SELECTOR;
    write_msr(STAR_MSR, (u64)selectors << 32);
    write_msr(SFMASK_MSR, U64_FROM_BIT(EFLAG_INTERRUPT) | U64_FROM_BIT(EFLAG_TRAP));
    write_msr(EFER_MSR, read_msr(EFER_MSR) | EFER_SCE);
}

extern void syscall_enter(void);

static inline void init_syscall_handler()
{
    set_syscall_handler(syscall_enter);
}

static inline void set_page_write_protect(boolean enable)
{
    word cr0;
    mov_from_cr("cr0", cr0);
    cr0 = enable ? (cr0 | C0_WP) : (cr0 & ~C0_WP);
    mov_to_cr("cr0", cr0);
}

typedef struct {
    u8 data[8];
} seg_desc_t;

struct cpuinfo_machine {
    /*** Fields accessed by low-level entry points. ***/
    /* Don't move these without updating gs-relative accesses in crt0.s ***/

    /* For accessing cpuinfo via %gs:0; must be first */
    void *self;

    /* This points to the currently-running context and bottom of associated frame. +8 */
    context current_context;

    /* Next kernel context to install +16 */
    context kernel_context;

    /* Next syscall context to install +24 */
    context syscall_context;

    /* One temporary for syscall enter to use so that we don't need to touch the user stack. +32 */
    u64 tmp;

#ifdef CONFIG_FTRACE
    /* Used by mcount to determine if to enter ftrace code. +40 */
    u64 ftrace_disable_cnt;
#endif

    /*** End of fields touched by kernel entries ***/

    /* Stack for exceptions (which may occur in interrupt handlers) */
    void *exception_stack;

    /* Stack for interrupts */
    void *int_stack;

    struct gdt {
        seg_desc_t null;
        seg_desc_t code;
        seg_desc_t data;
        seg_desc_t user_code;
        seg_desc_t user_data;
        seg_desc_t user_code_64;
        u8 tss_desc[0x10];
    } gdt;

    struct gdt_pointer {
        u16 limit;
        u64 base;
    } __attribute__((packed)) gdt_pointer;
    u64 tss[TSS_SIZE / sizeof(u64)];

    /* Monotonic clock timestamp when the lapic timer is supposed to fire; used to re-arm the timer
     * when it fires too early (based on what the monotonic clock source says). */
    timestamp lapic_timer_expiry;
};

typedef struct cpuinfo *cpuinfo;

static inline cpuinfo current_cpu(void)
{
    u64 addr;
    asm volatile("movq %%gs:0, %0":"=r"(addr));
    return (cpuinfo)pointer_from_u64(addr);
}

extern u64 extended_frame_size;

static inline boolean frame_is_full(context_frame f)
{
    return f[FRAME_FULL];
}

static inline void *frame_extended(context_frame f)
{
    return pointer_from_u64(f[FRAME_EXTENDED]);
}

static inline void frame_enable_interrupts(context_frame f)
{
    f[FRAME_EFLAGS] |= U64_FROM_BIT(EFLAG_INTERRUPT);
}

static inline void frame_disable_interrupts(context_frame f)
{
    f[FRAME_EFLAGS] &= ~U64_FROM_BIT(EFLAG_INTERRUPT);
}

extern void xsave(context_frame f);
extern void clone_frame_pstate(context_frame dest, context_frame src);

static inline boolean is_protection_fault(context_frame f)
{
    return (f[FRAME_ERROR_CODE] & FRAME_ERROR_PF_P) != 0;
}

static inline boolean is_usermode_fault(context_frame f)
{
    return (f[FRAME_ERROR_CODE] & FRAME_ERROR_PF_US) != 0;
}

static inline boolean is_instruction_fault(context_frame f)
{
    return (f[FRAME_ERROR_CODE] & FRAME_ERROR_PF_ID) != 0;
}

static inline boolean is_data_fault(context_frame f)
{
    return !is_instruction_fault(f);
}

static inline boolean is_write_fault(context_frame f)
{
    return (f[FRAME_ERROR_CODE] & FRAME_ERROR_PF_RW) != 0;
}

/* page table integrity check? open to interpretation for other archs... */
static inline boolean is_pte_error(context_frame f)
{
    /* XXX check sdm before merging - seems suspicious */
    return (is_protection_fault(f) && (f[FRAME_ERROR_CODE] & FRAME_ERROR_PF_RSV));
}

static inline u64 frame_return_address(context_frame f)
{
    return f[FRAME_RIP];
}

static inline u64 fault_address(context_frame f)
{
    return f[FRAME_CR2];
}

static inline boolean is_page_fault(context_frame f)
{
    return f[FRAME_VECTOR] == 14; // XXX defined somewhere?
}

static inline boolean is_div_by_zero(context_frame f)
{
    return f[FRAME_VECTOR] == 0; // XXX defined somewhere?
}

static inline void *frame_get_stack(context_frame f)
{
    return pointer_from_u64(f[FRAME_RSP]);
}

static inline void frame_set_stack(context_frame f, u64 sp)
{
    f[FRAME_RSP] = sp;
}

static inline void *frame_get_stack_top(context_frame f)
{
    return pointer_from_u64(f[FRAME_STACK_TOP]);
}

static inline void frame_set_stack_top(context_frame f, void *st)
{
    f[FRAME_STACK_TOP] = u64_from_pointer(st);
}

static inline void frame_reset_stack(context_frame f)
{
    f[FRAME_RSP] = f[FRAME_STACK_TOP];
}

#ifdef KERNEL
static inline void install_runloop_trampoline(context c)
{
    /* make instance of inline for trampoline use */
    *(u64*)frame_get_stack_top(c->frame) = u64_from_pointer(runloop_target);
}
#endif

#define switch_stack(__s, __target) {                           \
        asm volatile("mov %0, %%rdx": :"r"(__s):"%rdx");        \
        asm volatile("mov %0, %%rax": :"r"(__target));          \
        asm volatile("mov %%rdx, %%rsp"::);                     \
        asm volatile("jmp *%%rax"::);                           \
    }

#define switch_stack_1(__s, __target, __a0) {                   \
        asm volatile("mov %0, %%rdx": :"r"(__s):"%rdx");        \
        asm volatile("mov %0, %%rax": :"r"(__target));          \
        asm volatile("mov %0, %%rdi": :"r"(__a0));              \
        asm volatile("mov %%rdx, %%rsp"::);                     \
        asm volatile("jmp *%%rax"::);                           \
    }

#define switch_stack_2(__s, __target, __a0, __a1) {             \
        asm volatile("mov %0, %%rdx": :"r"(__s):"%rdx");        \
        asm volatile("mov %0, %%rax": :"r"(__target));          \
        asm volatile("mov %0, %%rdi": :"r"(__a0));              \
        asm volatile("mov %0, %%rsi": :"r"(__a1));              \
        asm volatile("mov %%rdx, %%rsp"::);                     \
        asm volatile("jmp *%%rax"::);                           \
    }

/* for vdso */
#define do_syscall(sysnr, rdi, rsi) ({\
    sysreturn rv;\
    asm("syscall"\
        : "=a" (rv)\
        : "0" (sysnr), "D" (rdi), "S"(rsi)\
        : "memory"\
    );\
    rv;\
})

/* clocksource */
static inline u64
_rdtscp(void)
{
    u32 a, d;
    asm volatile("rdtscp" : "=a" (a), "=d" (d) :: "%rcx");
    return (((u64)a) | (((u64)d) << 32));
}

static inline u64
_rdtsc(void)
{
    u32 a, d;
    asm volatile("rdtsc" : "=a" (a), "=d" (d));
    return (((u64)a) | (((u64)d) << 32));
}

static inline u64
rdtsc(void)
{
    if (platform_has_precise_clocksource())
        return _rdtscp();
    return _rdtsc();
}

static inline u64
rdtsc_ordered(void)
{
    if (platform_has_precise_clocksource())
        return _rdtscp();

    /* Now both AMD and Intel has lfence  */
    __asm __volatile("lfence" : : : "memory");
    return _rdtsc();
}

static inline u64
rdtsc_precise(void)
{
    if (platform_has_precise_clocksource())
        return _rdtscp();

    asm volatile("cpuid" ::: "%rax", "%rbx", "%rcx", "%rdx"); /* serialize execution */
    return _rdtsc();
}

/* IPI */
static inline void machine_halt(void)
{
    __asm__("cli; hlt");
}

void send_ipi(u64 cpu, u8 vector);

u64 allocate_interrupt(void);
void deallocate_interrupt(u64 irq);
#define allocate_ipi_interrupt allocate_interrupt
#define deallocate_ipi_interrupt deallocate_interrupt
#define allocate_mmio_interrupt allocate_interrupt
#define deallocate_mmio_interrupt deallocate_interrupt
