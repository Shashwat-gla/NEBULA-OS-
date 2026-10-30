#include <stdint.h>

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct gdt_entry gdt[3];
static struct gdt_ptr   gp;

extern void gdt_flush(uint32_t);

static void gdt_set_gate(int n, uint32_t base, uint32_t limit,
                         uint8_t access, uint8_t gran)
{
    gdt[n].base_low    = base & 0xFFFF;
    gdt[n].base_mid    = (base >> 16) & 0xFF;
    gdt[n].base_high   = (base >> 24) & 0xFF;
    gdt[n].limit_low   = limit & 0xFFFF;
    gdt[n].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[n].access      = access;
}

void gdt_init(void)
{
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint32_t)&gdt;

    gdt_set_gate(0, 0, 0,          0x00, 0x00);  /* null   */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);  /* code   */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);  /* data   */

    gdt_flush((uint32_t)&gp);
}
