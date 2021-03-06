#include <stdint.h>
#include <stddef.h>
#include <protos/stivale.h>
#include <lib/elf.h>
#include <lib/blib.h>
#include <lib/acpi.h>
#include <lib/e820.h>
#include <drivers/vbe.h>

struct stivale_header {
    uint64_t stack;
    uint16_t video_mode;  // 0 = default at boot (CGA text mode). 1 = graphical VESA
    uint16_t framebuffer_width;
    uint16_t framebuffer_height;
    uint16_t framebuffer_bpp;
} __attribute__((packed));

struct stivale_module {
    uint64_t begin;
    uint64_t end;
    char     string[128];
} __attribute__((packed));

struct stivale_struct {
    uint64_t cmdline;
    uint64_t memory_map_addr;
    uint64_t memory_map_entries;
    uint64_t framebuffer_addr;
    uint16_t framebuffer_pitch;
    uint16_t framebuffer_width;
    uint16_t framebuffer_height;
    uint16_t framebuffer_bpp;
    uint64_t rsdp;
    uint64_t module_count;
    struct stivale_module modules[];
} __attribute__((packed));

struct stivale_struct stivale_struct = {0};

void stivale_load(struct echfs_file_handle *fd) {
    uint64_t entry_point;

    struct stivale_header stivale_hdr;
    int ret = elf_load_section(fd, &stivale_hdr, ".stivalehdr", sizeof(struct stivale_header));
    switch (ret) {
        case 1:
            print("stivale: File is not a valid ELF.\n");
            for (;;);
        case 2:
            print("stivale: Section .stivalehdr not found.\n");
            for (;;);
        case 3:
            print("stivale: Section .stivalehdr exceeds the size of the struct.\n");
            for (;;);
        default:
            break;
    }

    print("stivale: Requested stack at %X\n", stivale_hdr.stack);
    print("stivale: Video mode: %u\n", stivale_hdr.video_mode);

    elf_load(fd, &entry_point);

    stivale_struct.memory_map_entries = (uint64_t)init_e820();
    stivale_struct.memory_map_addr    = (uint64_t)(size_t)e820_map;

    stivale_struct.rsdp = (uint64_t)(size_t)get_rsdp();
    print("stivale: RSDP at %X\n", stivale_struct.rsdp);

    stivale_struct.framebuffer_width  = stivale_hdr.framebuffer_width;
    stivale_struct.framebuffer_height = stivale_hdr.framebuffer_height;
    stivale_struct.framebuffer_bpp    = stivale_hdr.framebuffer_bpp;

    if (stivale_hdr.video_mode == 1) {
        init_vbe(&stivale_struct.framebuffer_addr,
                 &stivale_struct.framebuffer_pitch,
                 &stivale_struct.framebuffer_width,
                 &stivale_struct.framebuffer_height,
                 &stivale_struct.framebuffer_bpp);
    }

    struct pagemap {
        uint64_t pml4[512];
        uint64_t pml3_lo[512];
        uint64_t pml3_hi[512];
        uint64_t pml2_0gb[512];
        uint64_t pml2_1gb[512];
        uint64_t pml2_2gb[512];
        uint64_t pml2_3gb[512];
    };
    struct pagemap *pagemap = balloc_aligned(sizeof(struct pagemap), 0x1000);

    // first, zero out the pagemap
    for (uint64_t *p = (uint64_t *)pagemap; p < &pagemap->pml3_hi[512]; p++)
        *p = 0;

    pagemap->pml4[511]    = (uint64_t)(size_t)pagemap->pml3_hi  | 0x03;
    pagemap->pml4[256]    = (uint64_t)(size_t)pagemap->pml3_lo  | 0x03;
    pagemap->pml4[0]      = (uint64_t)(size_t)pagemap->pml3_lo  | 0x03;
    pagemap->pml3_hi[510] = (uint64_t)(size_t)pagemap->pml2_0gb | 0x03;
    pagemap->pml3_hi[511] = (uint64_t)(size_t)pagemap->pml2_1gb | 0x03;
    pagemap->pml3_lo[0]   = (uint64_t)(size_t)pagemap->pml2_0gb | 0x03;
    pagemap->pml3_lo[1]   = (uint64_t)(size_t)pagemap->pml2_1gb | 0x03;
    pagemap->pml3_lo[2]   = (uint64_t)(size_t)pagemap->pml2_2gb | 0x03;
    pagemap->pml3_lo[3]   = (uint64_t)(size_t)pagemap->pml2_3gb | 0x03;

    // populate the page directories
    for (size_t i = 0; i < 512 * 4; i++)
        (&pagemap->pml2_0gb[0])[i] = (i * 0x200000) | 0x03 | (1 << 7);

    asm volatile (
        "cli\n\t"
        "mov cr3, eax\n\t"
        "mov eax, cr4\n\t"
        "or eax, 1 << 5\n\t"
        "mov cr4, eax\n\t"
        "mov ecx, 0xc0000080\n\t"
        "rdmsr\n\t"
        "or eax, 1 << 8\n\t"
        "wrmsr\n\t"
        "mov eax, cr0\n\t"
        "or eax, 1 << 31\n\t"
        "mov cr0, eax\n\t"
        "jmp 0x28:1f\n\t"
        "1: .code64\n\t"
        "mov ax, 0x30\n\t"
        "mov ds, ax\n\t"
        "mov es, ax\n\t"
        "mov fs, ax\n\t"
        "mov gs, ax\n\t"
        "mov ss, ax\n\t"
        "mov rsp, [rsi]\n\t"
        "jmp [rbx]\n\t"
        ".code32\n\t"
        :
        : "a" (pagemap), "b" (&entry_point),
          "D" (&stivale_struct), "S" (&stivale_hdr.stack)
    );
}
