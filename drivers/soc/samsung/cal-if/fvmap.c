#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <soc/samsung/cal-if.h>
#include <soc/samsung/exynos-oc.h>

#include "fvmap.h"
#include "cmucal.h"
#include "vclk.h"
#include "ra.h"
#include "acpm_dvfs.h"

#define FVMAP_SIZE		(SZ_8K)

void __iomem *fvmap_base;
void __iomem *sram_fvmap_base;

int init_margin_table[10];

int set_mif_volt;
int set_int_volt;
int set_cpucl0_volt;
int set_cpucl1_volt;
int set_g3d_volt;
int set_intcam_volt;
int set_cam_volt;
int set_disp_volt;
int set_g3dm_volt;
int set_cp_volt;

/*
 * Shadow of the rate/voltage map that ACPM keeps in its SRAM.
 *
 * base_volt[] is the per-OPP voltage userspace asked for (seeded from the
 * ASV table at boot), offset_uv is a domain wide trim on top of it. The
 * value actually written to SRAM is always
 *
 *	clamp(round(base_volt[i] + offset_uv))
 *
 * so every path into the hardware goes through the same clamp and no
 * userspace request can push a rail outside its configured range.
 */
struct fvmap_domain {
	int num_of_lv;
	bool valid;
	int offset_uv;
	unsigned int rate[FVMAP_MAX_LEVEL];
	unsigned int asv_volt[FVMAP_MAX_LEVEL];
	int base_volt[FVMAP_MAX_LEVEL];
};

static struct fvmap_domain fvmap_domains[FVMAP_MAX_DOMAIN];
static DEFINE_MUTEX(fvmap_lock);

static int __init get_mif_volt(char *str)
{
	get_option(&str, &set_mif_volt);
	init_margin_table[0] = set_mif_volt;
	return 0;
}
early_param("mif", get_mif_volt);

static int __init get_int_volt(char *str)
{
	get_option(&str, &set_int_volt);
	init_margin_table[1] = set_int_volt;
	return 0;
}
early_param("int", get_int_volt);

static int __init get_cpucl0_volt(char *str)
{
	get_option(&str, &set_cpucl0_volt);
	init_margin_table[2] = set_cpucl0_volt;
	return 0;
}
early_param("big", get_cpucl0_volt);

static int __init get_cpucl1_volt(char *str)
{
	get_option(&str, &set_cpucl1_volt);
	init_margin_table[3] = set_cpucl1_volt;
	return 0;
}
early_param("lit", get_cpucl1_volt);

static int __init get_g3d_volt(char *str)
{
	get_option(&str, &set_g3d_volt);
	init_margin_table[4] = set_g3d_volt;
	return 0;
}
early_param("g3d", get_g3d_volt);

static int __init get_intcam_volt(char *str)
{
	get_option(&str, &set_intcam_volt);
	init_margin_table[5] = set_intcam_volt;
	return 0;
}
early_param("intcam", get_intcam_volt);

static int __init get_cam_volt(char *str)
{
	get_option(&str, &set_cam_volt);
	init_margin_table[6] = set_cam_volt;
	return 0;
}
early_param("cam", get_cam_volt);

static int __init get_disp_volt(char *str)
{
	get_option(&str, &set_disp_volt);
	init_margin_table[7] = set_disp_volt;
	return 0;
}
early_param("disp", get_disp_volt);

static int __init get_g3dm_volt(char *str)
{
	get_option(&str, &set_g3dm_volt);
	init_margin_table[8] = set_g3dm_volt;
	return 0;
}
early_param("g3dm", get_g3dm_volt);

static int __init get_cp_volt(char *str)
{
	get_option(&str, &set_cp_volt);
	init_margin_table[9] = set_cp_volt;
	return 0;
}
early_param("cp", get_cp_volt);

static struct fvmap_domain *fvmap_get_domain(unsigned int id)
{
	int idx = GET_IDX(id);

	if (idx < 0 || idx >= FVMAP_MAX_DOMAIN)
		return NULL;

	if (!fvmap_domains[idx].valid)
		return NULL;

	return &fvmap_domains[idx];
}

static unsigned int fvmap_clamp_volt(unsigned int id, int uv)
{
	int min_uv, max_uv;

	if (!exynos_oc_volt_limits(id, &min_uv, &max_uv))
		return uv < 0 ? 0 : uv;

	uv = exynos_oc_round_volt(uv);

	if (uv < min_uv)
		uv = min_uv;
	if (uv > max_uv)
		uv = max_uv;

	return uv;
}

/*
 * Push base_volt[] + offset_uv into both the SRAM map that ACPM reads and
 * the kernel side copy that cal_dfs_get_asv_table() serves.
 */
static void fvmap_commit_locked(unsigned int id, struct fvmap_domain *dom)
{
	struct fvmap_header *sram_header = sram_fvmap_base;
	struct fvmap_header *map_header = fvmap_base;
	struct rate_volt_header *sram_table, *map_table;
	int idx = GET_IDX(id);
	int i;

	if (!sram_fvmap_base || !fvmap_base)
		return;

	sram_table = sram_fvmap_base + sram_header[idx].o_ratevolt;
	map_table = fvmap_base + map_header[idx].o_ratevolt;

	for (i = 0; i < dom->num_of_lv; i++) {
		unsigned int volt;

		volt = fvmap_clamp_volt(id,
					dom->base_volt[i] + dom->offset_uv);
		sram_table->table[i].volt = volt;
		map_table->table[i].volt = volt;
	}
}

int fvmap_get_lv_num(unsigned int id)
{
	struct fvmap_domain *dom;
	int num;

	mutex_lock(&fvmap_lock);
	dom = fvmap_get_domain(id);
	num = dom ? dom->num_of_lv : 0;
	mutex_unlock(&fvmap_lock);

	return num;
}

/*
 * Snapshot of one domain: the rate of each level, the voltage it booted
 * with and the voltage in effect now.
 */
int fvmap_get_level(unsigned int id, int index, unsigned int *rate,
		    unsigned int *asv_uv, unsigned int *cur_uv)
{
	struct fvmap_domain *dom;
	int ret = -EINVAL;

	mutex_lock(&fvmap_lock);
	dom = fvmap_get_domain(id);
	if (dom && index >= 0 && index < dom->num_of_lv) {
		*rate = dom->rate[index];
		*asv_uv = dom->asv_volt[index];
		*cur_uv = fvmap_clamp_volt(id,
				dom->base_volt[index] + dom->offset_uv);
		ret = 0;
	}
	mutex_unlock(&fvmap_lock);

	return ret;
}

/*
 * Set the voltage of a single OPP, addressed by its rate in kHz. Requests
 * that fall outside the rail limits are rejected rather than silently
 * clamped, so a userspace mistake is visible as a write error.
 */
int fvmap_set_level_volt(unsigned int id, unsigned int rate, int uv)
{
	struct fvmap_domain *dom;
	int min_uv, max_uv;
	int i, ret = -EINVAL;

	if (!exynos_oc_volt_limits(id, &min_uv, &max_uv))
		return -EOPNOTSUPP;

	uv = exynos_oc_round_volt(uv);
	if (uv < min_uv || uv > max_uv)
		return -ERANGE;

	mutex_lock(&fvmap_lock);
	dom = fvmap_get_domain(id);
	if (!dom)
		goto out;

	for (i = 0; i < dom->num_of_lv; i++) {
		if (dom->rate[i] != rate)
			continue;

		/*
		 * base_volt[] holds the request; the offset is folded back in
		 * on commit so the two controls stay independent.
		 */
		dom->base_volt[i] = uv - dom->offset_uv;
		fvmap_commit_locked(id, dom);
		ret = 0;
		break;
	}
out:
	mutex_unlock(&fvmap_lock);

	return ret;
}

/*
 * Domain wide trim. Rejected unless every level stays inside the rail
 * limits with the offset applied, so an offset never silently flattens
 * the top or bottom of the table.
 */
int fvmap_set_volt_offset(unsigned int id, int uv)
{
	struct fvmap_domain *dom;
	int min_uv, max_uv;
	int i, ret = -EINVAL;

	if (!exynos_oc_volt_limits(id, &min_uv, &max_uv))
		return -EOPNOTSUPP;

	uv = exynos_oc_round_volt(uv);

	mutex_lock(&fvmap_lock);
	dom = fvmap_get_domain(id);
	if (!dom)
		goto out;

	for (i = 0; i < dom->num_of_lv; i++) {
		int target = dom->base_volt[i] + uv;

		if (target < min_uv || target > max_uv) {
			ret = -ERANGE;
			goto out;
		}
	}

	dom->offset_uv = uv;
	fvmap_commit_locked(id, dom);
	ret = 0;
out:
	mutex_unlock(&fvmap_lock);

	return ret;
}

int fvmap_get_volt_offset(unsigned int id)
{
	struct fvmap_domain *dom;
	int offset = 0;

	mutex_lock(&fvmap_lock);
	dom = fvmap_get_domain(id);
	if (dom)
		offset = dom->offset_uv;
	mutex_unlock(&fvmap_lock);

	return offset;
}

/* Drop every userspace adjustment and go back to the booted ASV table. */
int fvmap_reset_volt(unsigned int id)
{
	struct fvmap_domain *dom;
	int i, ret = -EINVAL;

	mutex_lock(&fvmap_lock);
	dom = fvmap_get_domain(id);
	if (dom) {
		for (i = 0; i < dom->num_of_lv; i++)
			dom->base_volt[i] = dom->asv_volt[i];
		dom->offset_uv = 0;
		fvmap_commit_locked(id, dom);
		ret = 0;
	}
	mutex_unlock(&fvmap_lock);

	return ret;
}

/* Kept for the old debug callers: applies a trim to the whole domain. */
int fvmap_set_raw_voltage_table(unsigned int id, int uV)
{
	return fvmap_set_volt_offset(id, uV);
}

int fvmap_get_voltage_table(unsigned int id, unsigned int *table)
{
	struct fvmap_header *fvmap_header = fvmap_base;
	struct rate_volt_header *fv_table;
	int idx, i;
	int num_of_lv;

	if (!IS_ACPM_VCLK(id))
		return 0;

	idx = GET_IDX(id);

	fvmap_header = fvmap_base;
	fv_table = fvmap_base + fvmap_header[idx].o_ratevolt;
	num_of_lv = fvmap_header[idx].num_of_lv;

	for (i = 0; i < num_of_lv; i++)
		table[i] = fv_table->table[i].volt;

	return num_of_lv;

}

int fvmap_get_raw_voltage_table(unsigned int id)
{
	struct fvmap_header *fvmap_header;
	struct rate_volt_header *fv_table;
	int idx, i;
	int num_of_lv;
	unsigned int table[20];

	idx = GET_IDX(id);

	fvmap_header = sram_fvmap_base;
	fv_table = sram_fvmap_base + fvmap_header[idx].o_ratevolt;
	num_of_lv = fvmap_header[idx].num_of_lv;

	for (i = 0; i < num_of_lv; i++)
		table[i] = fv_table->table[i].volt;

	for (i = 0; i < num_of_lv; i++)
		printk("dvfs id : %d  %d Khz : %d uv\n", ACPM_VCLK_TYPE | id, fv_table->table[i].rate, table[i]);

	return 0;
}

/*
 * Seed the shadow for one domain and give the levels above the stock
 * ceiling a sane starting voltage.
 *
 * Levels are stored highest first. Walking upwards from the bottom, every
 * unlocked level is held at least EXYNOS_OC_MIN_VOLT_STEP_UV above the one
 * below it, so a freshly exposed OPP never inherits the voltage of a
 * slower OPP. Where the ASV table already characterises the level its own
 * value wins, since that is the value Samsung measured for this die.
 */
static void fvmap_seed_domain(unsigned int id, struct rate_volt_header *table,
			      int num_of_lv)
{
	struct fvmap_domain *dom;
	unsigned int stock_max;
	int idx = GET_IDX(id);
	int i;

	if (idx < 0 || idx >= FVMAP_MAX_DOMAIN)
		return;

	dom = &fvmap_domains[idx];
	dom->num_of_lv = min(num_of_lv, FVMAP_MAX_LEVEL);
	dom->offset_uv = 0;
	dom->valid = true;

	stock_max = exynos_oc_get_stock_max_freq(id);

	for (i = 0; i < dom->num_of_lv; i++) {
		dom->rate[i] = table->table[i].rate;
		dom->asv_volt[i] = table->table[i].volt;
	}

	if (stock_max) {
		for (i = dom->num_of_lv - 2; i >= 0; i--) {
			unsigned int floor;

			if (dom->rate[i] <= stock_max)
				continue;

			floor = dom->asv_volt[i + 1] +
					EXYNOS_OC_MIN_VOLT_STEP_UV;
			if (dom->asv_volt[i] < floor)
				dom->asv_volt[i] = floor;
		}
	}

	for (i = 0; i < dom->num_of_lv; i++) {
		dom->asv_volt[i] = fvmap_clamp_volt(id, dom->asv_volt[i]);
		dom->base_volt[i] = dom->asv_volt[i];
	}
}

static void fvmap_copy_from_sram(void __iomem *map_base, void __iomem *sram_base)
{
	volatile struct fvmap_header *fvmap_header, *header;
	struct rate_volt_header *old, *new;
	struct clocks *clks;
	struct pll_header *plls;
	struct vclk *vclk;
	struct cmucal_clk *clk_node;
	unsigned int paddr_offset, fvaddr_offset;
	int size;
	int i, j;

	fvmap_header = map_base;
	header = sram_base;

	size = cmucal_get_list_size(ACPM_VCLK_TYPE);

	for (i = 0; i < size; i++) {
		/* load fvmap info */
		fvmap_header[i].dvfs_type = header[i].dvfs_type;
		fvmap_header[i].num_of_lv = header[i].num_of_lv;
		fvmap_header[i].num_of_members = header[i].num_of_members;
		fvmap_header[i].num_of_pll = header[i].num_of_pll;
		fvmap_header[i].num_of_mux = header[i].num_of_mux;
		fvmap_header[i].num_of_div = header[i].num_of_div;
		fvmap_header[i].gearratio = header[i].gearratio;
		fvmap_header[i].init_lv = header[i].init_lv;
		fvmap_header[i].num_of_gate = header[i].num_of_gate;
		fvmap_header[i].reserved[0] = header[i].reserved[0];
		fvmap_header[i].reserved[1] = header[i].reserved[1];
		fvmap_header[i].block_addr[0] = header[i].block_addr[0];
		fvmap_header[i].block_addr[1] = header[i].block_addr[1];
		fvmap_header[i].block_addr[2] = header[i].block_addr[2];
		fvmap_header[i].o_members = header[i].o_members;
		fvmap_header[i].o_ratevolt = header[i].o_ratevolt;
		fvmap_header[i].o_tables = header[i].o_tables;

		vclk = cmucal_get_node(ACPM_VCLK_TYPE | i);
		if (vclk == NULL)
			continue;
		pr_info("dvfs_type : %s - id : %x\n",
			vclk->name, fvmap_header[i].dvfs_type);
		pr_info("  num_of_lv      : %d\n", fvmap_header[i].num_of_lv);
		pr_info("  num_of_members : %d\n", fvmap_header[i].num_of_members);

		old = sram_base + fvmap_header[i].o_ratevolt;
		new = map_base + fvmap_header[i].o_ratevolt;
		if (init_margin_table[i])
			cal_dfs_set_volt_margin(i | ACPM_VCLK_TYPE,
						init_margin_table[i]);

		fvmap_seed_domain(vclk->id, old, fvmap_header[i].num_of_lv);

		for (j = 0; j < fvmap_header[i].num_of_lv; j++) {
			unsigned int volt;

			if (i < FVMAP_MAX_DOMAIN && j < FVMAP_MAX_LEVEL &&
			    fvmap_domains[i].valid)
				volt = fvmap_domains[i].base_volt[j];
			else
				volt = fvmap_clamp_volt(vclk->id,
							old->table[j].volt);

			new->table[j].rate = old->table[j].rate;
			new->table[j].volt = volt;
			old->table[j].volt = volt;
			pr_info("  lv : [%7d], volt = %d uV\n",
				new->table[j].rate, new->table[j].volt);
		}

		for (j = 0; j < fvmap_header[i].num_of_pll; j++) {
			clks = sram_base + fvmap_header[i].o_members;
			plls = sram_base + clks->addr[j];
			clk_node = cmucal_get_node(vclk->list[j]);
			if (clk_node == NULL)
				continue;
			paddr_offset = clk_node->paddr & 0xFFFF;
			fvaddr_offset = plls->addr & 0xFFFF;
			if (paddr_offset == fvaddr_offset)
				continue;

			clk_node->paddr += fvaddr_offset - paddr_offset;
			clk_node->pll_con0 += fvaddr_offset - paddr_offset;
			if (clk_node->pll_con1)
				clk_node->pll_con1 += fvaddr_offset - paddr_offset;
		}
	}
}

int fvmap_init(void __iomem *sram_base)
{
	void __iomem *map_base;

	map_base = kzalloc(FVMAP_SIZE, GFP_KERNEL);

	fvmap_base = map_base;
	sram_fvmap_base = sram_base;
	pr_info("%s:fvmap initialize %pK\n", __func__, sram_base);
	fvmap_copy_from_sram(map_base, sram_base);

	if (IS_ENABLED(CONFIG_VDD_AUTO_CAL))
		exynos_acpm_vdd_auto_calibration(1);

	return 0;
}
