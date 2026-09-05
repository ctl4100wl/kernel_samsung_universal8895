#!/usr/bin/env python3
"""Run the actual OC/ACME C functions against mocked policy/clock services."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(os.environ.get("EXYNOS_OC_SOURCE", Path(__file__).resolve().parents[4]))

def function(path, name):
    source = (root / path).read_text()
    match = re.search(r'^[\w \t*\n]+\b' + name + r'\([^;]*?\)\s*\{.*?^\}', source, re.M | re.S)
    if not match:
        raise RuntimeError('Cannot extract ' + name)
    return match.group(0)

preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define pr_debug(...) ((void)0)
#define pr_err(...) ((void)0)
#define BUG_ON(x) assert(!(x))
#define mutex_lock(x) ((void)0)
#define mutex_unlock(x) ((void)0)
#define CPUFREQ_ENTRY_INVALID (~0U)
#define CPUFREQ_TABLE_END (~1U)
#define CPUFREQ_RELATION_L 0
#define CPUFREQ_RELATION_H 1
#define CPUFREQ_RELATION_C 2
#define FVMAP_MAX_LEVEL 32
#define cpufreq_for_each_valid_entry(p,t) \
 for ((p)=(t); (p)->frequency!=CPUFREQ_TABLE_END; ++(p)) \
 if ((p)->frequency!=CPUFREQ_ENTRY_INVALID)
struct cpufreq_frequency_table { unsigned int driver_data, frequency; };
struct cpufreq_policy {
 unsigned int cpu, min, max;
 struct { unsigned int min_freq, max_freq; } cpuinfo;
};
struct exynos_cpufreq_domain {
 unsigned int cal_id, id, old, min_freq;
 bool enabled;
 struct cpufreq_frequency_table *freq_table;
};
struct exynos_oc_domain { unsigned int cal_id, ceiling, oc_max_freq; };
struct attribute { int unused; };
struct kobject { int unused; };
struct kobj_attribute { struct attribute attr; };
static struct exynos_oc_domain oc;
static struct exynos_cpufreq_domain domain;
static unsigned int applied, refresh_calls;
static int refresh_error;
static struct exynos_cpufreq_domain *find_domain(unsigned int cpu) { return &domain; }
static unsigned int exynos_oc_get_ceiling(unsigned int id) { return oc.ceiling; }
static unsigned int get_freq(struct exynos_cpufreq_domain *d) { return d->old; }
static unsigned int apply_pm_qos(struct exynos_cpufreq_domain *d,
 struct cpufreq_policy *p, unsigned int f) { return f; }
static unsigned int index_to_freq(struct cpufreq_frequency_table *t, unsigned int i) { return t[i].frequency; }
static int scale(struct exynos_cpufreq_domain *d, struct cpufreq_policy *p,
 unsigned int f) { applied=f; return 0; }
static struct exynos_oc_domain *oc_domain_of_attr(struct attribute *a,
 const char **what) { return &oc; }
static int kstrtouint(const char *s, int base, unsigned int *v) {
 char *end; unsigned long n=strtoul(s,&end,base);
 if (end==s || *end || n>~0U) return -EINVAL;
 *v=n; return 0;
}
static unsigned int cal_dfs_get_min_freq(unsigned int id) { return domain.min_freq; }
static int cal_dfs_get_lv_num(unsigned int id) {
 int n=0; while (domain.freq_table[n].frequency!=CPUFREQ_TABLE_END) ++n;
 return n;
}
static int cal_dfs_get_rate_table(unsigned int id, unsigned long *rates) {
 int n=cal_dfs_get_lv_num(id), i;
 for (i=0;i<n;++i) rates[i]=domain.freq_table[i].frequency;
 return n;
}
static int exynos_cpufreq_refresh_limits(unsigned int id) { ++refresh_calls; return refresh_error; }
'''
code = preamble
code += function('include/linux/cpufreq.h', 'cpufreq_verify_within_limits')
code += function('include/linux/cpufreq.h', 'cpufreq_verify_within_cpu_limits')
for name in ['cpufreq_frequency_table_verify', 'cpufreq_frequency_table_target']:
    code += function('drivers/cpufreq/freq_table.c', name)
for name in ['exynos_cpufreq_verify', '__exynos_cpufreq_target']:
    code += function('drivers/cpufreq/exynos-acme.c', name)
for name in ['oc_snap_to_level', 'oc_max_freq_store']:
    code += function('drivers/soc/samsung/cal-if/exynos-oc.c', name)
code += r'''
int main(void) {
 struct cpufreq_frequency_table big[]={{0,2652000},{1,2574000},{2,2496000},{3,2314000},{4,741000},{5,CPUFREQ_TABLE_END}};
 struct cpufreq_frequency_table little[]={{0,2002000},{1,1898000},{2,1794000},{3,1690000},{4,455000},{5,CPUFREQ_TABLE_END}};
 struct cpufreq_policy p={.min=2652000,.max=2652000,.cpuinfo={741000,2652000}};
 struct kobj_attribute attr={0};
 domain.enabled=true; domain.min_freq=741000; domain.freq_table=big;
 oc.ceiling=2314000; oc.oc_max_freq=2652000;
 assert(exynos_cpufreq_verify(&p)==0);
 assert(p.min==2314000 && p.max==2314000);
 /* A stale policy floor and upward table relation must not escape the cap. */
 p.min=p.max=2652000; domain.old=741000; applied=0;
 assert(__exynos_cpufreq_target(&p,2652000,CPUFREQ_RELATION_L)==0);
 assert(applied==2314000);
 p.min=741000; oc.ceiling=2600000; domain.old=741000;
 assert(__exynos_cpufreq_target(&p,2652000,CPUFREQ_RELATION_L)==0);
 assert(applied==2574000);
 /* Exact OC steps remain reachable. */
 oc.ceiling=2652000; domain.old=741000;
 assert(__exynos_cpufreq_target(&p,2652000,CPUFREQ_RELATION_L)==0);
 assert(applied==2652000);
 /* Sysfs rounds down, refreshes even on a repeated write, and rolls back errors. */
 assert(oc_max_freq_store(NULL,&attr,"2600000",7)==7);
 assert(oc.ceiling==2574000 && refresh_calls==1);
 assert(oc_max_freq_store(NULL,&attr,"2600000",7)==7);
 assert(refresh_calls==2);
 refresh_error=-EIO;
 assert(oc_max_freq_store(NULL,&attr,"2314000",7)==-EIO);
 assert(oc.ceiling==2574000 && refresh_calls==4);
 refresh_error=0;
 assert(oc_max_freq_store(NULL,&attr,"2808000",7)==-ERANGE);
 assert(oc_max_freq_store(NULL,&attr,"0",1)==-ERANGE);
 assert(oc.ceiling==2574000 && refresh_calls==4);
 domain.freq_table=little; domain.min_freq=455000; domain.old=455000;
 oc.ceiling=2002000; oc.oc_max_freq=2002000;
 p.min=455000; p.max=2002000;
 assert(__exynos_cpufreq_target(&p,2002000,CPUFREQ_RELATION_L)==0);
 assert(applied==2002000);
 assert(oc_max_freq_store(NULL,&attr,"1999000",7)==7);
 assert(oc.ceiling==1898000);
 puts("PASS: policy refresh, rollback, floor conflicts, rounding, and exact CPU OC steps");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='exynos-oc-test-') as tmp:
    source = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    source.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=gnu99', '-Werror=implicit-function-declaration',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
