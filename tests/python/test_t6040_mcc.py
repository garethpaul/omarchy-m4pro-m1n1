"""Exercise the firmware-cache initializer with rejected layouts and status reads."""
from pathlib import Path
import subprocess


def test_firmware_cache_init(tmp_path):
    source = (Path(__file__).resolve().parents[2] / "src/mcc.c").read_text()
    helper = source[source.index("static int mcc_init_firmware_cache("):source.index("\nint mcc_init(void)")]
    harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
typedef uint64_t u64;
typedef uint32_t u32;
#define T6040 0x6040
#define MAX_MCC_INSTANCES 16
#define T8103_PLANE_STRIDE 0x40000
static void *adt;
static u32 chip_id = T6040, board_id = 6;
static int mcc_count;
static bool mcc_initialized;
static struct {u64 plane_base,plane_stride; int plane_count,dcs_count; void *tz; bool has_cache_control;} mcc_regs[16];
static u64 count=4, channels=4, planes=4, stride=0x40000, aperture=0x200000;
static u32 reg_idx=12;
static bool is_t6041=true;
static int bad_status=-1, reads, bad_reg=-1;
static u64 base_offset;
static int adt_path_offset(void *a, const char *p) {return 2;}
static bool adt_is_compatible(void *a,int n,const char *p) {return is_t6041;}
static void *adt_getprop(void *a,int n,const char *p,void *length) {return &count;}
static int getprop(void *a,int n,const char *p,u32 *v) {*v=reg_idx;return 0;}
#define ADT_GETPROP(a,n,p,v) getprop(a,n,p,v)
static int mcc_get_count(int n,const char *p,u64 *v) {
 if (!strcmp(p,"amcc-count")) *v=count;
 else if (!strcmp(p,"dcs-count-per-amcc")) *v=channels;
 else if (!strcmp(p,"plane-count")) *v=planes;
 else if (!strcmp(p,"plane-stride")) *v=stride;
 else return -1;
 return 0;
}
static int adt_get_reg(void *a,int *path,const char *p,int idx,u64 *base,u64 *size) {
 if (idx==bad_reg) return -1;
 *base=0x220000000ULL+(u64)(idx-reg_idx)*0x2000000+base_offset;
 *size=aperture;return 0;
}
static u32 plane_read32(int i,int plane,u64 offset) {
 assert(offset==0x2804); assert(i>=0 && i<4);assert(plane>=0 && plane<4);
 assert(mcc_regs[i].plane_base==0x220000000ULL+(u64)i*0x2000000);
 assert(mcc_regs[i].plane_stride==0x40000);
 return reads++==bad_status ? 0 : 0x0c000c00;
}
''' + helper + r'''
static void reset(void) {mcc_initialized=false;reads=0;bad_status=-1;bad_reg=-1;base_offset=0;}
static void check_t6040_layout(int *path) {
 reset();assert(mcc_init_firmware_cache(1,path)==0);assert(mcc_initialized && reads==16);
 for(int i=0;i<4;i++) assert(!mcc_regs[i].has_cache_control && !mcc_regs[i].tz);
 for(int bad=0;bad<16;bad++) {
  reset();bad_status=bad;assert(mcc_init_firmware_cache(1,path)<0);
  assert(!mcc_initialized && reads==bad+1);
 }
 reset();reg_idx=11;assert(mcc_init_firmware_cache(1,path)<0 && !reads);reg_idx=12;
 reset();chip_id=0x6041;assert(mcc_init_firmware_cache(1,path)<0 && !reads);chip_id=T6040;
 reset();count=0;assert(mcc_init_firmware_cache(1,path)<0 && !reads);count=4;
 reset();count=17;assert(mcc_init_firmware_cache(1,path)<0 && !reads);count=4;
 reset();channels=3;assert(mcc_init_firmware_cache(1,path)<0 && !reads);channels=4;
 reset();planes=5;assert(mcc_init_firmware_cache(1,path)<0 && !reads);planes=4;
 reset();stride=0x80000;assert(mcc_init_firmware_cache(1,path)<0 && !reads);stride=0x40000;
 reset();aperture=4;assert(mcc_init_firmware_cache(1,path)<0 && !reads);aperture=0x200000;
 reset();base_offset=0x1000;assert(mcc_init_firmware_cache(1,path)<0 && !reads);
 reset();bad_reg=15;assert(mcc_init_firmware_cache(1,path)<0 && !reads);
}
int main(void) {
 int path[8]={0};
 board_id=4;check_t6040_layout(path);
 board_id=6;check_t6040_layout(path);
 reset();board_id=5;assert(mcc_init_firmware_cache(1,path)<0 && !reads);
 reset();board_id=7;assert(mcc_init_firmware_cache(1,path)<0 && !reads);
 reset();is_t6041=false;chip_id=0x8140;reg_idx=0;
 assert(mcc_init_firmware_cache(1,path)==0 && mcc_initialized && !reads);
 return 0;
}
'''
    cfile = tmp_path / "mcc.c"
    binary = tmp_path / "mcc-test"
    cfile.write_text(harness)
    subprocess.run(["cc", "-std=c11", "-Wall", str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, capture_output=True)
