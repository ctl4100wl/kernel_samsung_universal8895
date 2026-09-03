/*
 * Exynos8895 overclock and voltage control
 *
 * The DVFS levels above the stock ceiling are not something this driver
 * invents: they are already present in the ECT DVFS level list and in the
 * ACPM frequency/voltage map, complete with the voltages Samsung
 * characterised for them. What hides them is the ECT ASV level_en[]
 * bitmap, which vclk_get_asv_info() turns into vclk->max_freq, and which
 * init_table() in exynos-acme.c then uses to mark every higher level
 * CPUFREQ_ENTRY_INVALID.
 *
 * So overclocking here means moving that one ceiling, and nothing else.
 * No rate is synthesised, no PLL word is rewritten, and the DVFS
 * governors keep scaling through the whole table as before.
 *
 * The machine still comes up at the stock ceiling on every boot: a PM QoS
 * max request pinned at the stock maximum holds the CPU clusters down
 * until userspace deliberately raises it through sysfs. Nothing persists
 * across a reboot, so an unstable setting is always one power cycle away
 * from being undone.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.
 */

#define pr_fmt(fmt) "exynos-oc: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/pm_qos.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include <soc/samsung/cal-if.h>
#include <soc/samsung/exynos-oc.h>
#include <dt-bindings/clock/exynos8895.h>

#include "fvmap.h"

/*
 * Note the naming inversion inherited from the SoC: CPUCL0 is the big
 * (Exynos M2) cluster on cpu4-7, CPUCL1 is the LITTLE (Cortex-A53)
 * cluster on cpu0-3. exynos8895.dtsi wires them up the same way.
 */
struct exynos_oc_domain {
	const char *name;
	unsigned int cal_id;

	/* Ceiling this build is willing to expose, in kHz. */
	unsigned int oc_max_freq;

	/* Ceiling read back from ECT ASV before the fence was lifted. */
	unsigned int stock_max_freq;

	/* Highest rate the ECT DVFS level list actually holds. */
	unsigned int hw_max_freq;

	/*
	 * Sticky ceiling the cpufreq driver enforces on every transition.
	 * Starts at the stock ceiling so a fresh boot behaves exactly like
	 * a stock kernel, and only /sys/kernel/exynos_oc/<domain>/max_freq
	 * moves it. Unlike policy->max this survives anything the power HAL
	 * or a kernel manager writes to scaling_max_freq, and unlike a PM
	 * QoS request it costs one comparison instead of a list walk on the
	 * transition path.
	 */
	unsigned int ceiling;

	int volt_min_uv;
	int volt_max_uv;

	/*
	 * PM QoS max class for the cluster, or 0 for domains whose ceiling
	 * is owned by another driver (the GPU ceiling lives in the Mali
	 * platform code, which already exports /sys/kernel/gpu/gpu_max_clock).
	 */
	int pm_qos_max_class;

};

static struct exynos_oc_domain oc_domains[] = {
	{
		.name		= "little",
		.cal_id		= ACPM_DVFS_CPUCL1,
		.oc_max_freq	= CONFIG_EXYNOS_OC_LITTLE_MAX_KHZ,
		.volt_min_uv	= EXYNOS_OC_LITTLE_MIN_UV,
		.volt_max_uv	= EXYNOS_OC_LITTLE_MAX_UV,
		.pm_qos_max_class = PM_QOS_CLUSTER0_FREQ_MAX,
	},
	{
		.name		= "big",
		.cal_id		= ACPM_DVFS_CPUCL0,
		.oc_max_freq	= CONFIG_EXYNOS_OC_BIG_MAX_KHZ,
		.volt_min_uv	= EXYNOS_OC_BIG_MIN_UV,
		.volt_max_uv	= EXYNOS_OC_BIG_MAX_UV,
		.pm_qos_max_class = PM_QOS_CLUSTER1_FREQ_MAX,
	},
	{
		.name		= "gpu",
		.cal_id		= ACPM_DVFS_G3D,
		.oc_max_freq	= CONFIG_EXYNOS_OC_G3D_MAX_KHZ,
		.volt_min_uv	= EXYNOS_OC_G3D_MIN_UV,
		.volt_max_uv	= EXYNOS_OC_G3D_MAX_UV,
	},
};

static struct exynos_oc_domain *oc_find_domain(unsigned int cal_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(oc_domains); i++)
		if (oc_domains[i].cal_id == cal_id)
			return &oc_domains[i];

	return NULL;
}

int exynos_oc_round_volt(int uv)
{
	int step = EXYNOS_OC_VOLT_STEP_UV;

	if (uv >= 0)
		return ((uv + step / 2) / step) * step;

	return -(((-uv + step / 2) / step) * step);
}

bool exynos_oc_volt_limits(unsigned int cal_id, int *min_uv, int *max_uv)
{
	struct exynos_oc_domain *dom = oc_find_domain(cal_id);

	if (!dom)
		return false;

	*min_uv = dom->volt_min_uv;
	*max_uv = dom->volt_max_uv;

	return true;
}

unsigned int exynos_oc_get_stock_max_freq(unsigned int cal_id)
{
	struct exynos_oc_domain *dom = oc_find_domain(cal_id);

	return dom ? dom->stock_max_freq : 0;
}

unsigned int exynos_oc_get_ceiling(unsigned int cal_id)
{
	struct exynos_oc_domain *dom = oc_find_domain(cal_id);

	return dom ? dom->ceiling : 0;
}

/*
 * Snap a requested ceiling down to a rate that really is in the level
 * list. Anything else would leave cal_dfs_get_max_freq() reporting a rate
 * that no table row carries, which the Mali level lookup in particular
 * cannot resolve.
 */
static unsigned int oc_snap_to_level(unsigned int cal_id, unsigned int target)
{
	unsigned long rates[FVMAP_MAX_LEVEL];
	unsigned int best = 0;
	int num, i;

	/* cal_dfs_get_rate_table() fills one entry per level, unbounded. */
	num = cal_dfs_get_lv_num(cal_id);
	if (num <= 0 || num > FVMAP_MAX_LEVEL)
		return 0;

	if (cal_dfs_get_rate_table(cal_id, rates) != num)
		return 0;

	for (i = 0; i < num; i++)
		if (rates[i] <= target && rates[i] > best)
			best = rates[i];

	return best;
}

void exynos_oc_unlock_dvfs_domains(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(oc_domains); i++) {
		struct exynos_oc_domain *dom = &oc_domains[i];
		unsigned int target;
		int ret;

		dom->stock_max_freq = cal_dfs_get_max_freq(dom->cal_id);
		dom->hw_max_freq = cal_dfs_get_hw_max_freq(dom->cal_id);
		dom->ceiling = dom->stock_max_freq;

		if (!dom->stock_max_freq || !dom->hw_max_freq) {
			pr_warn("%s: no DVFS level list, left alone\n",
				dom->name);
			continue;
		}

		/*
		 * vclk_get_asv_info() stores -1 in an unsigned field when the
		 * ASV lookup fails, so a ceiling above the level list means
		 * "unknown" rather than "very fast".
		 */
		if (dom->stock_max_freq > dom->hw_max_freq) {
			dom->stock_max_freq = dom->hw_max_freq;
			dom->ceiling = dom->stock_max_freq;
		}

		/*
		 * Never go past what the level list holds, and never go
		 * backwards: a firmware whose stock ceiling is already above
		 * the configured target keeps its own ceiling.
		 */
		target = oc_snap_to_level(dom->cal_id,
					  min(dom->oc_max_freq,
					      dom->hw_max_freq));
		if (!target) {
			pr_warn("%s: could not read the level list, left alone\n",
				dom->name);
			dom->oc_max_freq = dom->stock_max_freq;
			continue;
		}

		if (target <= dom->stock_max_freq) {
			pr_info("%s: stock %u kHz already at or above target %u kHz\n",
				dom->name, dom->stock_max_freq, target);
			dom->oc_max_freq = dom->stock_max_freq;
			continue;
		}

		ret = cal_dfs_set_max_freq(dom->cal_id, target);
		if (ret) {
			pr_err("%s: could not raise ceiling to %u kHz (%d)\n",
			       dom->name, target, ret);
			dom->oc_max_freq = dom->stock_max_freq;
			continue;
		}

		dom->oc_max_freq = target;
		pr_info("%s: ceiling %u -> %u kHz (level list tops out at %u kHz)\n",
			dom->name, dom->stock_max_freq, target,
			dom->hw_max_freq);
	}
}

/*********************************************************************
 *                              SYSFS                                *
 *********************************************************************/
static struct exynos_oc_domain *oc_domain_of_attr(struct attribute *attr,
						  const char **suffix)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(oc_domains); i++) {
		size_t len = strlen(oc_domains[i].name);

		if (strncmp(attr->name, oc_domains[i].name, len))
			continue;
		if (attr->name[len] != '_')
			continue;

		*suffix = attr->name + len + 1;
		return &oc_domains[i];
	}

	return NULL;
}

static ssize_t oc_freq_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	struct exynos_oc_domain *dom;
	const char *what;
	ssize_t len = 0;
	int i, num;

	dom = oc_domain_of_attr(&attr->attr, &what);
	if (!dom)
		return -EINVAL;

	if (!strcmp(what, "stock_max_freq"))
		return scnprintf(buf, PAGE_SIZE, "%u\n", dom->stock_max_freq);

	if (!strcmp(what, "oc_max_freq"))
		return scnprintf(buf, PAGE_SIZE, "%u\n", dom->oc_max_freq);

	if (!strcmp(what, "max_freq"))
		return scnprintf(buf, PAGE_SIZE, "%u\n",
				 dom->ceiling ? dom->ceiling : dom->oc_max_freq);

	/* available_freqs */
	num = fvmap_get_lv_num(dom->cal_id);
	for (i = num - 1; i >= 0; i--) {
		unsigned int rate, asv_uv, cur_uv;

		if (fvmap_get_level(dom->cal_id, i, &rate, &asv_uv, &cur_uv))
			continue;
		if (rate > dom->oc_max_freq)
			continue;
		len += scnprintf(buf + len, PAGE_SIZE - len, "%u ", rate);
	}
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");

	return len;
}

static ssize_t oc_max_freq_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	struct exynos_oc_domain *dom;
	const char *what;
	unsigned int freq;

	dom = oc_domain_of_attr(&attr->attr, &what);
	if (!dom)
		return -EINVAL;

	if (!dom->ceiling)
		return -EOPNOTSUPP;

	if (kstrtouint(buf, 10, &freq))
		return -EINVAL;

	/*
	 * Anything between the stock ceiling and the ceiling this build
	 * exposes. The cpufreq core still snaps the request down to a real
	 * table entry, so a value that falls between two levels simply
	 * lands on the level below it.
	 */
	if (freq < cal_dfs_get_min_freq(dom->cal_id) || freq > dom->oc_max_freq)
		return -ERANGE;

	dom->ceiling = freq;

	return count;
}

static ssize_t oc_volt_table_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	struct exynos_oc_domain *dom;
	const char *what;
	ssize_t len = 0;
	int i, num;

	dom = oc_domain_of_attr(&attr->attr, &what);
	if (!dom)
		return -EINVAL;

	num = fvmap_get_lv_num(dom->cal_id);
	for (i = 0; i < num; i++) {
		unsigned int rate, asv_uv, cur_uv;

		if (fvmap_get_level(dom->cal_id, i, &rate, &asv_uv, &cur_uv))
			continue;
		len += scnprintf(buf + len, PAGE_SIZE - len, "%u %u %u\n",
				 rate, cur_uv, asv_uv);
	}

	return len;
}

static ssize_t oc_volt_table_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	struct exynos_oc_domain *dom;
	const char *what;
	unsigned int rate;
	int uv, ret;

	dom = oc_domain_of_attr(&attr->attr, &what);
	if (!dom)
		return -EINVAL;

	if (sscanf(buf, "%u %d", &rate, &uv) != 2)
		return -EINVAL;

	ret = fvmap_set_level_volt(dom->cal_id, rate, uv);
	if (ret)
		return ret;

	return count;
}

static ssize_t oc_volt_offset_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	struct exynos_oc_domain *dom;
	const char *what;

	dom = oc_domain_of_attr(&attr->attr, &what);
	if (!dom)
		return -EINVAL;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 fvmap_get_volt_offset(dom->cal_id));
}

static ssize_t oc_volt_offset_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	struct exynos_oc_domain *dom;
	const char *what;
	int uv, ret;

	dom = oc_domain_of_attr(&attr->attr, &what);
	if (!dom)
		return -EINVAL;

	if (kstrtoint(buf, 10, &uv))
		return -EINVAL;

	ret = fvmap_set_volt_offset(dom->cal_id, uv);
	if (ret)
		return ret;

	return count;
}

static ssize_t oc_volt_limits_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	struct exynos_oc_domain *dom;
	const char *what;

	dom = oc_domain_of_attr(&attr->attr, &what);
	if (!dom)
		return -EINVAL;

	return scnprintf(buf, PAGE_SIZE, "%d %d %d\n", dom->volt_min_uv,
			 dom->volt_max_uv, EXYNOS_OC_VOLT_STEP_UV);
}

static ssize_t oc_volt_reset_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	struct exynos_oc_domain *dom;
	const char *what;
	int ret;

	dom = oc_domain_of_attr(&attr->attr, &what);
	if (!dom)
		return -EINVAL;

	ret = fvmap_reset_volt(dom->cal_id);
	if (ret)
		return ret;

	return count;
}

static ssize_t volt_step_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", EXYNOS_OC_VOLT_STEP_UV);
}

#define OC_ATTR_RO(_name, _show)					\
	static struct kobj_attribute oc_attr_##_name =			\
		__ATTR(_name, 0444, _show, NULL)
#define OC_ATTR_RW(_name, _show, _store)				\
	static struct kobj_attribute oc_attr_##_name =			\
		__ATTR(_name, 0644, _show, _store)
#define OC_ATTR_WO(_name, _store)					\
	static struct kobj_attribute oc_attr_##_name =			\
		__ATTR(_name, 0200, NULL, _store)

#define OC_DOMAIN_ATTRS(_d)						\
	OC_ATTR_RO(_d##_stock_max_freq, oc_freq_show);			\
	OC_ATTR_RO(_d##_oc_max_freq, oc_freq_show);			\
	OC_ATTR_RO(_d##_available_freqs, oc_freq_show);			\
	OC_ATTR_RW(_d##_max_freq, oc_freq_show, oc_max_freq_store);	\
	OC_ATTR_RW(_d##_volt_table, oc_volt_table_show, oc_volt_table_store); \
	OC_ATTR_RW(_d##_volt_offset, oc_volt_offset_show, oc_volt_offset_store); \
	OC_ATTR_RO(_d##_volt_limits, oc_volt_limits_show);		\
	OC_ATTR_WO(_d##_volt_reset, oc_volt_reset_store)

#define OC_DOMAIN_ATTR_LIST(_d)						\
	&oc_attr_##_d##_stock_max_freq.attr,				\
	&oc_attr_##_d##_oc_max_freq.attr,				\
	&oc_attr_##_d##_available_freqs.attr,				\
	&oc_attr_##_d##_max_freq.attr,					\
	&oc_attr_##_d##_volt_table.attr,				\
	&oc_attr_##_d##_volt_offset.attr,				\
	&oc_attr_##_d##_volt_limits.attr,				\
	&oc_attr_##_d##_volt_reset.attr

OC_DOMAIN_ATTRS(little);
OC_DOMAIN_ATTRS(big);
OC_DOMAIN_ATTRS(gpu);
OC_ATTR_RO(volt_step, volt_step_show);

static struct attribute *exynos_oc_attrs[] = {
	OC_DOMAIN_ATTR_LIST(little),
	OC_DOMAIN_ATTR_LIST(big),
	OC_DOMAIN_ATTR_LIST(gpu),
	&oc_attr_volt_step.attr,
	NULL,
};

static struct attribute_group exynos_oc_attr_group = {
	.attrs = exynos_oc_attrs,
};

static struct kobject *exynos_oc_kobj;


static int __init exynos_oc_sysfs_init(void)
{
	int ret;

	exynos_oc_kobj = kobject_create_and_add("exynos_oc", kernel_kobj);
	if (!exynos_oc_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(exynos_oc_kobj, &exynos_oc_attr_group);
	if (ret) {
		kobject_put(exynos_oc_kobj);
		exynos_oc_kobj = NULL;
		return ret;
	}

	return 0;
}
late_initcall(exynos_oc_sysfs_init);
