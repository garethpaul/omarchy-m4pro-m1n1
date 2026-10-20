/* SPDX-License-Identifier: MIT */
/* Explicit experimental J614s single-CPU native handoff, opt-in build only. */
#include "j614s_native.h"
#include "adt.h"
#include "dapf.h"
#include "heapblock.h"
#include "memory.h"
#include "smp.h"
#include "spmi.h"
#include "utils.h"
#include "wdt.h"
#include "xnuboot.h"
#include "libfdt/libfdt.h"
#include "tinf/tinf.h"

#define ALIGN_2M(x) (((x) + 0x1fffffUL) & ~0x1fffffUL)
#define RAM_SIZE (4UL << 30)
#define INITRD_OFF (32UL << 20)
#define FDT_OFF (40UL << 20)

/* [DEBUG-J614S-NATIVE] A direct diagnostic, independent of fb_init(). */
void j614s_native_note(const char *text)
{
    extern u8 _binary_build_font_bin_start[];
    const struct boot_video *v = &cur_boot_args.video;
    if (v->base != 1124735713280UL || v->stride != 12096 || (v->depth & 0xff) != 30)
        return;
    size_t len = strlen(text);
    if (len > 24)
        return;
    for (unsigned y = 0; y < 64; y++) {
        for (unsigned x = 0; x < 768; x++) {
            unsigned col = x / 32;
            unsigned char ch = col < len ? text[col] : ' ';
            unsigned off = (ch - 0x20) * 8 * 16 + (y / 4) * 8 + (x / 4) % 8;
            u32 color = _binary_build_font_bin_start[off] > 127 ? 0x3fffffff : 0;
            write32(v->base + (y + 96) * v->stride + (x + 32) * 4, color);
        }
        if (mmu_active())
            dc_cvac_range((void *)(v->base + (y + 96) * v->stride + 32 * 4), 768 * 4);
    }
    sysop("dsb sy");
}

/* [DEBUG-J614S-NATIVE] Exact framebuffer lies outside both MCC carveouts. */
void j614s_native_mark(unsigned color)
{
    if (cur_boot_args.video.base != 1124735713280UL ||
        cur_boot_args.video.stride != 12096)
        return;
    for (unsigned y = 0; y < 64; y++)
        for (unsigned x = 0; x < 128; x++)
            write32(cur_boot_args.video.base + y * 12096 + x * 4, color);
    if (mmu_active())
        for (unsigned y = 0; y < 64; y++)
            dc_cvac_range((void *)(cur_boot_args.video.base + y * 12096), 128 * 4);
    sysop("dsb sy");
}

struct native_header {
    char magic[16];
    u32 version, image_size, initrd_size, fdt_size;
    u32 image_crc, initrd_crc, fdt_crc, reserved;
} PACKED;

/* Retain every ADT carveout except the one describing the entire RAM envelope. */
static int find_ram(u64 *start, u64 ram_end)
{
    const char *paths[] = {"/chosen/memory-map", "/chosen/carveout-memory-map"};
    bool changed;
    do {
        changed = false;
        for (unsigned i = 0; i < ARRAY_SIZE(paths); i++) {
            int node = adt_path_offset(adt, paths[i]);
            if (node < 0)
                return -1;
            ADT_FOREACH_PROPERTY(adt, node, prop) {
                if ((prop->size & 0x7fffffff) != 16)
                    continue;
                u64 range[2];
                memcpy(range, prop->value, sizeof(range));
                u64 lo = range[0], size = range[1];
                if (lo == ~0UL && size == ~0UL)
                    continue;
                if (size > ~0UL - lo)
                    return -1;
                if (i == 1 && !strcmp(prop->name, "region-id-24") &&
                    lo == cur_boot_args.phys_base && size == cur_boot_args.mem_size)
                    continue;
                if (!size || lo >= ram_end || lo + size <= cur_boot_args.phys_base)
                    continue;
                if (*start > ram_end || RAM_SIZE > ram_end - *start)
                    return -1;
                if (lo < *start + RAM_SIZE && lo + size > *start) {
                    if (lo + size > ~0UL - 0x1fffff)
                        return -1;
                    *start = ALIGN_2M(lo + size);
                    changed = true;
                }
            }
        }
    } while (changed);
    return *start <= ram_end && RAM_SIZE <= ram_end - *start ? 0 : -1;
}

static int set_pair(void *dt, int node, const char *name, u64 base, u64 size)
{
    fdt64_t data[] = {cpu_to_fdt64(base), cpu_to_fdt64(size)};
    return node < 0 ? -1 : fdt_setprop_inplace(dt, node, name, data, sizeof(data));
}

/* Exact J614s MTP firmware layout; obtain the external address from live ADT. */
static int mtp_external_span(const struct adt_segment_ranges *seg, u32 len,
                             u64 rt_base, u64 rt_size, u64 ram_start, u64 ram_end,
                             u64 *base, u64 *size)
{
    if (len != 3 * sizeof(*seg) ||
        seg[0].phys != 0x514c00000UL || seg[0].iova != 0x1000000 ||
        seg[0].remap != seg[0].phys || seg[0].size != 0x5e000 || seg[0].unk != 3 ||
        seg[1].phys != 0x514c5e000UL || seg[1].iova != 0x105e000 ||
        seg[1].remap != seg[1].phys || seg[1].size != 0x6c000 || seg[1].unk != 2 ||
        seg[2].iova != 0x10ca000 || seg[2].remap != seg[2].phys ||
        seg[2].size != 0x3000 || seg[2].unk != 0xa || (seg[2].phys & 0x3fff) ||
        rt_size > ~0UL - rt_base || rt_size < 0x4000 ||
        seg[2].phys < rt_base || seg[2].phys > rt_base + rt_size - 0x4000 ||
        (seg[2].phys < ram_end && seg[2].phys + 0x4000 > ram_start))
        return -1;
    *base = seg[2].phys;
    *size = 0x4000;
    return 0;
}

static int prepare_mtp_memory(void *dt, int node, u64 start, u64 end)
{
    u32 len;
    int nub = adt_path_offset(adt, "/arm-io/mtp/iop-mtp-nub");
    int mmap = adt_path_offset(adt, "/chosen/memory-map");
    if (nub < 0 || mmap < 0)
        return -1;
    const struct adt_segment_ranges *seg = adt_getprop(adt, nub, "segment-ranges", &len);
    u64 rt[2], base, size;
    if (!seg || ADT_GETPROP_ARRAY(adt, mmap, "RTBuddySeg", rt) < 0 ||
        mtp_external_span(seg, len, rt[0], rt[1], start, end, &base, &size) ||
        base < cur_boot_args.phys_base ||
        base + size > cur_boot_args.phys_base + cur_boot_args.mem_size)
        return -1;
    /* Never let Linux replace translations while the coprocessor owns DMA. */
    if (read32(0x514800008UL) != 0x2a2a0202 ||
        read32(0x514600044UL) != 0 || read32(0x514800c00UL) != 0 ||
        read32(0x514801000UL) != 1 || read32(0x514801400UL) != 0 ||
        (read32(0x514800200UL) & 1) || read32(0x514800100UL) != 0 ||
        !(read32(0x514608114UL) & BIT(17)))
        return -1;
    int mtp = fdt_path_offset(dt, "/mtp@514600000");
    int hid = fdt_path_offset(dt, "/fifo@514b14000/input@8000");
    if (mtp < 0 || hid < 0 || !fdt_get_phandle(dt, mtp) || !fdt_get_phandle(dt, hid) ||
        fdt_node_check_compatible(dt, node, "apple,asc-mem") ||
        !fdt_getprop(dt, node, "no-map", NULL))
        return -1;
    fdt32_t maps[10] = {
        cpu_to_fdt32(fdt_get_phandle(dt, mtp)), cpu_to_fdt32(base >> 32),
        cpu_to_fdt32(base), 0, cpu_to_fdt32(size),
        cpu_to_fdt32(fdt_get_phandle(dt, hid)), cpu_to_fdt32(base >> 32),
        cpu_to_fdt32(base), 0, cpu_to_fdt32(size),
    };
    if (set_pair(dt, node, "reg", base, size) ||
        fdt_setprop_inplace(dt, node, "iommu-addresses", maps, sizeof(maps)))
        return -1;
    /* This device is stopped; initialize only its ADT-declared DMA protection. */
    return dapf_init("/arm-io/dart-mtp", 1);
}

static int clear_health_counter(void)
{
    int path[8];
    int n = adt_path_offset_trace(adt, "/arm-io/nub-spmi0", path);
    u32 gen;
    u64 base, size;
    if (n < 0 || ADT_GETPROP(adt, n, "gen", &gen) < 0 || gen != 3 ||
        adt_get_reg(adt, path, "reg", 0, &base, &size) ||
        base != 0x509014000UL || size != 0x4000)
        return -1;
    bool found = false;
    int child;
    ADT_FOREACH_CHILD(adt, n) {
        child = n;
        u32 primary, scratch, reg[6];
        const u32 expected_reg[] = {14, 3, 0, 4, 0, 0};
        if (ADT_GETPROP(adt, child, "is-primary", &primary) < 0 || primary != 1)
            continue;
        if (found || !adt_is_compatible(adt, child, "pmu,spmi") ||
            ADT_GETPROP(adt, child, "info-leg_scrpad", &scratch) < 0 || scratch != 0xf800 ||
            ADT_GETPROP_ARRAY(adt, child, "reg", reg) < 0 ||
            memcmp(reg, expected_reg, sizeof(reg)))
            return -1;
        found = true;
    }
    if (!found || read32(base) != 0x1000100 || (read32(base + 0x80) & BIT(27)))
        return -1;
    spmi_dev_t *dev = spmi_init("/arm-io/nub-spmi0");
    if (!dev)
        return -1;
    u8 value, zero = 0;
    int ret = spmi_ext_read_long(dev, 14, 0xf802, &value, 1);
    if (!ret && value)
        ret = spmi_ext_write_long(dev, 14, 0xf802, &zero, 1);
    if (!ret)
        ret = spmi_ext_read_long(dev, 14, 0xf802, &value, 1);
    if (!ret && (value || read32(base) != 0x1000100 || (read32(base + 0x80) & BIT(27))))
        ret = -1;
    spmi_shutdown(dev);
    return ret;
}

int j614s_native_run(void)
{
    j614s_native_note("PAYLOAD_START");
    struct native_header hdr;
    memcpy(&hdr, _payload_start, sizeof(hdr));
    if (memcmp(hdr.magic, "J614S_NATIVE_V1\0", 16) || hdr.version != 1 || hdr.reserved)
        return -1;
    if (chip_id != T6040 || board_id != 4 || boot_cpu_idx != 4 ||
        (mrs(MPIDR_EL1) & 0xffffff) != 0x10100 ||
        !adt_is_compatible(adt, 0, "J614sAP"))
        return -1;
    const struct boot_video *v = &cur_boot_args.video;
    if (v->base != 1124735713280UL || v->stride != 12096 || v->width != 3024 ||
        v->height != 1964 || (v->depth & 0xff) != 30 || !v->display)
        return -1;
    if (hdr.image_size < 64 || hdr.image_size >= INITRD_OFF || !hdr.initrd_size ||
        hdr.initrd_size >= (4UL << 20) || hdr.fdt_size < 40 || hdr.fdt_size > 0x10000)
        return -1;
    u8 *src_image = (u8 *)_payload_start + sizeof(hdr);
    u8 *src_initrd = src_image + hdr.image_size;
    u8 *src_fdt = src_initrd + hdr.initrd_size;
    if ((u64)src_fdt + hdr.fdt_size > cur_boot_args.top_of_kernel_data ||
        memcmp(src_image + 56, "ARMd", 4) ||
        tinf_crc32(src_image, hdr.image_size) != hdr.image_crc ||
        tinf_crc32(src_initrd, hdr.initrd_size) != hdr.initrd_crc ||
        tinf_crc32(src_fdt, hdr.fdt_size) != hdr.fdt_crc || fdt_check_header(src_fdt) ||
        fdt_totalsize(src_fdt) != hdr.fdt_size ||
        fdt_node_check_compatible(src_fdt, 0, "apple,j614s"))
        return -1;
    u64 image_header[3];
    j614s_native_note("CRC_OK");
    memcpy(image_header, src_image, sizeof(image_header));
    if (image_header[1] || image_header[2] < hdr.image_size || image_header[2] >= INITRD_OFF)
        return -1;
    /* All malloc/SPMI work precedes the irreversible non-freeing RAM allocation. */
    if (wdt_arm_seconds(60) || clear_health_counter())
        return -1;
    j614s_native_note("PMU_OK");
    u64 old_heap = heapblock_high_water(), start = ALIGN_2M(old_heap);
    u64 ram_end = cur_boot_args.phys_base + cur_boot_args.mem_size;
    if (ram_end < cur_boot_args.phys_base || old_heap < cur_boot_args.phys_base ||
        find_ram(&start, ram_end))
        return -1;
    u64 end = start + RAM_SIZE;
    if (start < v->base + v->stride * v->height && end > v->base)
        return -1;
    if (heapblock_alloc(end - old_heap) != (void *)old_heap)
        return -1;
    j614s_native_note("RAM_OWNED");
    void *image = (void *)start, *initrd = (void *)(start + INITRD_OFF);
    void *dt = (void *)(start + FDT_OFF);
    memset(image, 0, image_header[2]);
    memcpy(image, src_image, hdr.image_size);
    memcpy(initrd, src_initrd, hdr.initrd_size);
    memcpy(dt, src_fdt, hdr.fdt_size);
    j614s_native_note("COPY_DONE");
    int memory = fdt_node_offset_by_prop_value(dt, -1, "device_type", "memory", 7);
    int chosen = fdt_path_offset(dt, "/chosen");
    int reserved = fdt_path_offset(dt, "/reserved-memory");
    int child;
    if (set_pair(dt, memory, "reg", start, RAM_SIZE) || chosen < 0 || reserved < 0 ||
        fdt_setprop_inplace_u64(dt, chosen, "linux,initrd-start", (u64)initrd) ||
        fdt_setprop_inplace_u64(dt, chosen, "linux,initrd-end", (u64)initrd + hdr.initrd_size))
        return -1;
    unsigned patched = 0;
    fdt_for_each_subnode(child, dt, reserved) {
        const char *name = fdt_get_name(dt, child, NULL);
        if (!strncmp(name, "ramoops@", 8)) {
            if (set_pair(dt, child, "reg", end - (1UL << 20), 1UL << 20))
                return -1;
            memset((void *)(end - (1UL << 20)), 0, 1UL << 20);
            patched |= 1;
        } else if (!strncmp(name, "retention-control@", 18)) {
            if (set_pair(dt, child, "reg", end - (2UL << 20), 0x4000))
                return -1;
            patched |= 2;
        } else if (!strcmp(name, "mtp-firmware@0")) {
            if ((patched & 4) || prepare_mtp_memory(dt, child, start, end))
                return -1;
            patched |= 4;
        } else {
            return -1;
        }
    }
    if (patched != 3 && patched != 7)
        return -1;
    j614s_native_note("FDT_DONE");
    dc_civac_range(image, image_header[2]);
    ic_ivau_range(image, image_header[2]);
    dc_civac_range(initrd, hdr.initrd_size);
    dc_civac_range(dt, hdr.fdt_size);
    smp_set_wfe_mode(true);
    next_stage.entry = (generic_func *)image;
    next_stage.args[0] = (u64)dt;
    for (unsigned i = 1; i < ARRAY_SIZE(next_stage.args); i++)
        next_stage.args[i] = 0;
    next_stage.restore_logo = true;
    j614s_native_note("HANDOFF_READY");
    printf("J614s native payload qualified: 4 GiB at %p, one CPU\n", image);
    j614s_native_note("PAYLOAD_RETURN");
    return 0;
}
