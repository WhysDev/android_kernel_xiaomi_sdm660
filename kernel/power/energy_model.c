// SPDX-License-Identifier: GPL-2.0
/*
 * Energy Model of CPUs
 *
 * Copyright (c) 2018, Arm ltd.
 * Written by: Quentin Perret, Arm ltd.
 */

#define pr_fmt(fmt) "energy_model: " fmt

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/energy_model.h>
#include <linux/sched.h>
#include <linux/slab.h>

/* Mapping of each CPU to the performance domain to which it belongs. */
static DEFINE_PER_CPU(struct em_perf_domain *, em_data);

/*
 * Mutex serializing the registrations of performance domains and letting
 * callbacks defined by drivers sleep.
 */
static DEFINE_MUTEX(em_pd_mutex);

static struct kobject *em_kobject;

/* Getters for the attributes of em_perf_domain objects */
struct em_pd_attr {
	struct attribute attr;
	ssize_t (*show)(struct em_perf_domain *pd, char *buf);
	ssize_t (*store)(struct em_perf_domain *pd, const char *buf, size_t s);
};

#define EM_ATTR_LEN 13
#define show_table_attr(_attr) \
static ssize_t show_##_attr(struct em_perf_domain *pd, char *buf) \
{ \
	ssize_t cnt = 0; \
	int i; \
	for (i = 0; i < pd->nr_cap_states; i++) { \
		if (cnt >= (ssize_t) (PAGE_SIZE / sizeof(char) \
				      - (EM_ATTR_LEN + 2))) \
			goto out; \
		cnt += scnprintf(&buf[cnt], EM_ATTR_LEN + 1, "%lu ", \
				 pd->table[i]._attr); \
	} \
out: \
	cnt += sprintf(&buf[cnt], "\n"); \
	return cnt; \
}

show_table_attr(power);
show_table_attr(frequency);
show_table_attr(cost);

static ssize_t show_cpus(struct em_perf_domain *pd, char *buf)
{
	return sprintf(buf, "%*pbl\n", cpumask_pr_args(to_cpumask(pd->cpus)));
}

#define pd_attr(_name) em_pd_##_name##_attr
#define define_pd_attr(_name) static struct em_pd_attr pd_attr(_name) = \
		__ATTR(_name, 0444, show_##_name, NULL)

define_pd_attr(power);
define_pd_attr(frequency);
define_pd_attr(cost);
define_pd_attr(cpus);

static struct attribute *em_pd_default_attrs[] = {
	&pd_attr(power).attr,
	&pd_attr(frequency).attr,
	&pd_attr(cost).attr,
	&pd_attr(cpus).attr,
	NULL
};

#define to_pd(k) container_of(k, struct em_perf_domain, kobj)
#define to_pd_attr(a) container_of(a, struct em_pd_attr, attr)

static ssize_t show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct em_perf_domain *pd = to_pd(kobj);
	struct em_pd_attr *pd_attr = to_pd_attr(attr);
	ssize_t ret;

	ret = pd_attr->show(pd, buf);

	return ret;
}

static const struct sysfs_ops em_pd_sysfs_ops = {
	.show	= show,
};

static struct kobj_type ktype_em_pd = {
	.sysfs_ops	= &em_pd_sysfs_ops,
	.default_attrs	= em_pd_default_attrs,
};

static struct em_perf_domain *em_create_pd(cpumask_t *span, int nr_states,
						struct em_data_callback *cb)
{
	unsigned long opp_eff, prev_opp_eff = ULONG_MAX;
	unsigned long power, freq, prev_freq = 0;
	int i, ret, cpu = cpumask_first(span);
	struct em_cap_state *table;
	struct em_perf_domain *pd;
	u64 fmax;

	if (!cb->active_power)
		return NULL;

	pd = kzalloc(sizeof(*pd) + cpumask_size(), GFP_KERNEL);
	if (!pd)
		return NULL;

	table = kcalloc(nr_states, sizeof(*table), GFP_KERNEL);
	if (!table)
		goto free_pd;

	/* Build the list of capacity states for this performance domain */
	for (i = 0, freq = 0; i < nr_states; i++, freq++) {
		/*
		 * active_power() is a driver callback which ceils 'freq' to
		 * lowest capacity state of 'cpu' above 'freq' and updates
		 * 'power' and 'freq' accordingly.
		 */
		ret = cb->active_power(&power, &freq, cpu);
		if (ret) {
			pr_err("pd%d: invalid cap. state: %d\n", cpu, ret);
			goto free_cs_table;
		}

		/*
		 * We expect the driver callback to increase the frequency for
		 * higher capacity states.
		 */
		if (freq <= prev_freq) {
			pr_err("pd%d: non-increasing freq: %lu\n", cpu, freq);
			goto free_cs_table;
		}

		/*
		 * The power returned by active_state() is expected to be
		 * positive, in milli-watts and to fit into 16 bits.
		 */
		if (!power || power > EM_CPU_MAX_POWER) {
			pr_err("pd%d: invalid power: %lu\n", cpu, power);
			goto free_cs_table;
		}

		table[i].power = power;
		table[i].frequency = prev_freq = freq;

		/*
		 * The hertz/watts efficiency ratio should decrease as the
		 * frequency grows on sane platforms. But this isn't always
		 * true in practice so warn the user if a higher OPP is more
		 * power efficient than a lower one.
		 */
		opp_eff = freq / power;
		if (opp_eff >= prev_opp_eff)
			pr_warn("pd%d: hertz/watts ratio non-monotonically decreasing: em_cap_state %d >= em_cap_state%d\n",
					cpu, i, i - 1);
		prev_opp_eff = opp_eff;
	}

	/* Compute the cost of each capacity_state. */
	fmax = (u64) table[nr_states - 1].frequency;
	for (i = 0; i < nr_states; i++) {
		table[i].cost = div64_u64(fmax * table[i].power,
					  table[i].frequency);
	}

	pd->table = table;
	pd->nr_cap_states = nr_states;
	cpumask_copy(to_cpumask(pd->cpus), span);

	ret = kobject_init_and_add(&pd->kobj, &ktype_em_pd, em_kobject,
				   "pd%u", cpu);
	if (ret)
		pr_err("pd%d: failed kobject_init_and_add(): %d\n", cpu, ret);

	return pd;

free_cs_table:
	kfree(table);
free_pd:
	kfree(pd);

	return NULL;
}

/**
 * em_cpu_get() - Return the performance domain for a CPU
 * @cpu : CPU to find the performance domain for
 *
 * Return: the performance domain to which 'cpu' belongs, or NULL if it doesn't
 * exist.
 */
struct em_perf_domain *em_cpu_get(int cpu)
{
	return READ_ONCE(per_cpu(em_data, cpu));
}
EXPORT_SYMBOL_GPL(em_cpu_get);

/**
 * em_register_perf_domain() - Register the Energy Model of a performance domain
 * @span	: Mask of CPUs in the performance domain
 * @nr_states	: Number of capacity states to register
 * @cb		: Callback functions providing the data of the Energy Model
 *
 * Create Energy Model tables for a performance domain using the callbacks
 * defined in cb.
 *
 * If multiple clients register the same performance domain, all but the first
 * registration will be ignored.
 *
 * Return 0 on success
 */
int em_register_perf_domain(cpumask_t *span, unsigned int nr_states,
						struct em_data_callback *cb)
{
	unsigned long cap, prev_cap = 0;
	struct em_perf_domain *pd;
	int cpu, ret = 0;

	if (!span || !nr_states || !cb)
		return -EINVAL;

	/*
	 * Use a mutex to serialize the registration of performance domains and
	 * let the driver-defined callback functions sleep.
	 */
	mutex_lock(&em_pd_mutex);

	if (!em_kobject) {
		em_kobject = kobject_create_and_add("energy_model",
						&cpu_subsys.dev_root->kobj);
		if (!em_kobject) {
			ret = -ENODEV;
			goto unlock;
		}
	}

	for_each_cpu(cpu, span) {
		/* Make sure we don't register again an existing domain. */
		if (READ_ONCE(per_cpu(em_data, cpu))) {
			ret = -EEXIST;
			goto unlock;
		}

		/*
		 * All CPUs of a domain must have the same micro-architecture
		 * since they all share the same table.
		 */
		cap = arch_scale_cpu_capacity(NULL, cpu);
		if (prev_cap && prev_cap != cap) {
			pr_err("CPUs of %*pbl must have the same capacity\n",
							cpumask_pr_args(span));
			ret = -EINVAL;
			goto unlock;
		}
		prev_cap = cap;
	}

	/* Create the performance domain and add it to the Energy Model. */
	pd = em_create_pd(span, nr_states, cb);
	if (!pd) {
		ret = -EINVAL;
		goto unlock;
	}

	for_each_cpu(cpu, span) {
		/*
		 * The per-cpu array can be read concurrently from em_cpu_get().
		 * The barrier enforces the ordering needed to make sure readers
		 * can only access well formed em_perf_domain structs.
		 */
		smp_store_release(per_cpu_ptr(&em_data, cpu), pd);
	}

	pr_debug("Created perf domain %*pbl\n", cpumask_pr_args(span));
unlock:
	mutex_unlock(&em_pd_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(em_register_perf_domain);
