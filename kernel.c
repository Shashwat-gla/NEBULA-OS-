#include <stdint.h>

/* ---------- VGA text mode ---------- */
static volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
static int row = 0, col = 0;

void print(const char *s)
{
    while (*s) {
        if (*s == '\n') { row++; col = 0; }
        else { vga[row * 80 + col] = (uint16_t)(0x0F00 | (uint8_t)*s); col++; }
        s++;
    }
}

static void clear(void)
{
    for (int i = 0; i < 80 * 25; i++) vga[i] = 0x0F20;
    row = col = 0;
}

/* ---------- ISR C handler ---------- */
typedef struct {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} registers_t;

static const char *names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound Range", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Overrun", "Invalid TSS",
    "Segment Not Present", "Stack Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 FPU Error", "Alignment Check",
    "Machine Check", "SIMD FP Exception", "Virtualization",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Security", "Reserved"
};

void isr_handler(registers_t *r)
{
    print("\nEXCEPTION: ");
    if (r->int_no < 32) print(names[r->int_no]);
    print("\nSystem halted.");
    for (;;) __asm__ volatile ("cli; hlt");
}

extern void gdt_init(void);
extern void idt_init(void);

void kernel_main(void)
{
    clear();
    print("NEBULA OS booting...\n");
    gdt_init();
    print("GDT loaded\n");
    idt_init();
    print("IDT loaded\n");

#ifdef TEST_DIV0
    volatile int a = 5, b = 0;
    volatile int c = a / b;
    (void)c;
#endif
#ifdef TEST_GPF
    __asm__ volatile ("mov $0x28, %%ax; mov %%ax, %%ds" ::: "ax");
#endif
#ifdef TEST_INT3
    __asm__ volatile ("int3");
#endif
#ifdef TEST_UD2
    __asm__ volatile ("ud2");
#endif
    print("Kernel running OK\n");
}
