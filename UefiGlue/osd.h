#ifndef _OSD_H_
#define _OSD_H_

#ifdef _MSC_VER
#define INLINE static __inline
#else
#define INLINE static __inline__
#endif

#define MAX_INPUTS 8
#define MAX_INPUTS_CFG 8

typedef struct {
  unsigned char padtype;
} t_input_cfg;

#define MAXROMSIZE 10485760

#define MD_ROM    0
#define SMS_ROM   1
#define GG_ROM    "gg_bios.bin"
#define SG_ROM    3
#define CD_ROM    4
#define SCD_ROM   5
#define AR_ROM    "ar_bios.bin"
#define SK_ROM    "sk.bin"
#define SK_UPMEM  "sk2chip.bin"
#define MAX_ROMS  9

#define GG_BIOS   "gg_bios.bin"
#define CD_BIOS_US "us_scd1_10x.bin"
#define CD_BIOS_EU "eu_mcd2_100.bin"
#define CD_BIOS_JP "jp_mcd1_100.bin"
#define MS_BIOS_US "us_bios.bin"
#define MS_BIOS_EU "eu_bios.bin"
#define MS_BIOS_JP "jp_bios.bin"

typedef struct
{
  unsigned char system;
  unsigned char region_detect;
  unsigned char vdp_mode;
  unsigned char master_clock;
  unsigned char force_dtack;
  unsigned char addr_error;
  unsigned char bios;
  unsigned char lock_on;
  unsigned char overscan;
  unsigned char gg_extra;
  unsigned char ntsc;
  unsigned char lcd;
  unsigned char render;
  int psg_preamp;
  int fm_preamp;
  unsigned char hq_fm;
  unsigned char psgBoostNoise;
  unsigned char filter;
  unsigned int lp_range;
  unsigned int low_freq;
  unsigned int high_freq;
  unsigned int lg_boost;
  unsigned char dac_bits;
  unsigned char gun_cursor[2];
  unsigned char invert_mouse;
  unsigned char ym2413;
  unsigned char add_on;
  unsigned char ym2612;
  unsigned char hq_psg;
  unsigned char ym3438;
  unsigned char opll;
  int cdda_volume;
  int pcm_volume;
  int cd_latency;
  unsigned char mono;
  unsigned char enhanced_vscroll;
  unsigned int enhanced_vscroll_limit;
  int lg;
  int mg;
  int hg;
  t_input_cfg input[MAX_INPUTS_CFG];
} t_config;

extern t_config config;

void osd_input_update(void);

#endif /* _OSD_H_ */
