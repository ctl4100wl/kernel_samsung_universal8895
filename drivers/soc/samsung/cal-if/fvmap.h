#ifndef __FVMAP_H__
#define __FVMAP_H__

/* ACPM DVFS domains on this SoC, and the deepest level list any of them has */
#define FVMAP_MAX_DOMAIN	10
#define FVMAP_MAX_LEVEL		24

/* FV(Frequency Voltage MAP) */
struct fvmap_header {
	unsigned char dvfs_type;
	unsigned char num_of_lv;
	unsigned char num_of_members;
	unsigned char num_of_pll;
	unsigned char num_of_mux;
	unsigned char num_of_div;
	unsigned short gearratio;
	unsigned char init_lv;
	unsigned char num_of_gate;
	unsigned char reserved[2];
	unsigned short block_addr[3];
	unsigned short o_members;
	unsigned short o_ratevolt;
	unsigned short o_tables;
};

struct clocks {
	unsigned short addr[0];
};

struct pll_header {
	unsigned int addr;
	unsigned short o_lock;
	unsigned short level;
	unsigned int pms[0];
};

struct rate_volt {
	unsigned int rate;
	unsigned int volt;
};

struct rate_volt_header {
	struct rate_volt table[0];
};

struct dvfs_table {
	unsigned char val[0];
};

#ifdef CONFIG_ACPM_DVFS
extern int fvmap_init(void __iomem *sram_base);
extern int fvmap_get_voltage_table(unsigned int id, unsigned int *table);
extern int fvmap_get_lv_num(unsigned int id);
extern int fvmap_get_level(unsigned int id, int index, unsigned int *rate,
			   unsigned int *asv_uv, unsigned int *cur_uv);
extern int fvmap_set_level_volt(unsigned int id, unsigned int rate, int uv);
extern int fvmap_set_volt_offset(unsigned int id, int uv);
extern int fvmap_get_volt_offset(unsigned int id);
extern int fvmap_reset_volt(unsigned int id);
extern int fvmap_set_raw_voltage_table(unsigned int id, int uV);
#else
static inline int fvmap_init(phys_addr_t phys_addr)
{
	return 0;
}

static inline int fvmap_get_voltage_table(unsigned int id, unsigned int *table)
{
	return 0;
}

static inline int fvmap_get_lv_num(unsigned int id)
{
	return 0;
}

static inline int fvmap_get_level(unsigned int id, int index,
				  unsigned int *rate, unsigned int *asv_uv,
				  unsigned int *cur_uv)
{
	return -EINVAL;
}

static inline int fvmap_set_level_volt(unsigned int id, unsigned int rate,
				       int uv)
{
	return -EINVAL;
}

static inline int fvmap_set_volt_offset(unsigned int id, int uv)
{
	return -EINVAL;
}

static inline int fvmap_get_volt_offset(unsigned int id)
{
	return 0;
}

static inline int fvmap_reset_volt(unsigned int id)
{
	return -EINVAL;
}

static inline int fvmap_set_raw_voltage_table(unsigned int id, int uV)
{
	return -EINVAL;
}
#endif
#endif
