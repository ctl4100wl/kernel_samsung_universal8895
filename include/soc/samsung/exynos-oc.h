/*
 * Exynos8895 overclock / undervolt control
 *
 * The DVFS levels above the stock ceiling already exist in the ECT DVFS
 * block and in the ACPM frequency/voltage map; the stock ceiling is only
 * a fence applied from the ECT ASV level_en[] bitmap (see
 * vclk_get_asv_info()).  This interface lifts that fence and provides the
 * kernel-side clamps for userspace voltage control.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.
 */
#ifndef __EXYNOS_OC_H__
#define __EXYNOS_OC_H__

#include <linux/types.h>

#ifdef CONFIG_ARM_EXYNOS_ACME
int exynos_cpufreq_refresh_limits(unsigned int cal_id);
#else
static inline int exynos_cpufreq_refresh_limits(unsigned int cal_id)
{
	return 0;
}
#endif

/* PMIC buck step of the CPU/G3D rails on this platform */
#define EXYNOS_OC_VOLT_STEP_UV		6250

/* Hard, kernel enforced rail limits. Userspace can never escape these. */
#define EXYNOS_OC_BIG_MIN_UV		450000
#define EXYNOS_OC_BIG_MAX_UV		1250000
#define EXYNOS_OC_LITTLE_MIN_UV		600000
#define EXYNOS_OC_LITTLE_MAX_UV		1100000
#define EXYNOS_OC_G3D_MIN_UV		600000
#define EXYNOS_OC_G3D_MAX_UV		1300000

/*
 * Minimum voltage increment applied per DVFS level above the stock
 * ceiling when the ASV table does not already provide a bigger step.
 * Keeps freshly unlocked OPPs from inheriting the voltage of the OPP
 * below them.
 */
#define EXYNOS_OC_MIN_VOLT_STEP_UV	25000

#if defined(CONFIG_EXYNOS_OC)
/*
 * Lift the ASV fence on the ACPM DVFS domains. Must run after
 * vclk_initialize() and before any cpufreq/devfreq/GPU driver reads a
 * rate table, i.e. from cal_if_init().
 */
extern void exynos_oc_unlock_dvfs_domains(void);

/*
 * Ceiling this domain shipped with, in kHz, as read from ECT ASV before
 * the fence was lifted. Returns 0 for domains that are not overclocked.
 */
extern unsigned int exynos_oc_get_stock_max_freq(unsigned int cal_id);

/*
 * Sticky ceiling in kHz that cpufreq must not scale past, regardless of
 * what userspace has written to scaling_max_freq or cpufreq_max_limit.
 * Starts at the stock ceiling every boot and only moves when
 * /sys/kernel/exynos_oc/<domain>/max_freq is written. Returns 0 for
 * domains with no ceiling of ours, meaning "do not clamp".
 */
extern unsigned int exynos_oc_get_ceiling(unsigned int cal_id);

/* Rounds towards the nearest regulator step, sign preserving. */
extern int exynos_oc_round_volt(int uv);

/*
 * Rail limits for an ACPM DVFS domain. Returns false when the domain has
 * no configured range, in which case its voltages are left untouched.
 */
extern bool exynos_oc_volt_limits(unsigned int cal_id, int *min_uv, int *max_uv);
#else
static inline void exynos_oc_unlock_dvfs_domains(void) { }

static inline unsigned int exynos_oc_get_stock_max_freq(unsigned int cal_id)
{
	return 0;
}

static inline unsigned int exynos_oc_get_ceiling(unsigned int cal_id)
{
	return 0;
}

static inline int exynos_oc_round_volt(int uv)
{
	return uv;
}

static inline bool exynos_oc_volt_limits(unsigned int cal_id,
					 int *min_uv, int *max_uv)
{
	return false;
}
#endif
#endif /* __EXYNOS_OC_H__ */
