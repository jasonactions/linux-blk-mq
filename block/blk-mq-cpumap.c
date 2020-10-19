#include <linux/kernel.h>
#include <linux/threads.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/cpu.h>

#include <linux/blk-mq.h>
#include "blk.h"
#include "blk-mq.h"

static void show_map(unsigned int *map, unsigned int nr)
{
	int i;

	pr_info("blk-mq: CPU -> queue map\n");
	for_each_online_cpu(i)
		pr_info("  CPU%2u -> Queue %u\n", i, map[i]);
}

static int cpu_to_queue_index(unsigned int nr_cpus, unsigned int nr_queues,
			      const int cpu)
{
	return cpu / ((nr_cpus + nr_queues - 1) / nr_queues);
}

static int get_first_sibling(unsigned int cpu)
{
	unsigned int ret;

	ret = cpumask_first(topology_thread_cpumask(cpu));
	if (ret < nr_cpu_ids)
		return ret;

	return cpu;
}

/*
 * @map: 数组用于记录cpu软队列被映射到哪个硬队列中, 数组个数与cpu个数相同
 * @nr_queues: 硬队列个数
 */
int blk_mq_update_queue_map(unsigned int *map, unsigned int nr_queues)
{
	unsigned int i, nr_cpus, nr_uniq_cpus, queue, first_sibling;
	/* struct cpumask { unsigned long bits[1];} *cpus */
	cpumask_var_t cpus;

	if (!alloc_cpumask_var(&cpus, GFP_ATOMIC))
		return 1;

	cpumask_clear(cpus);
	nr_cpus = nr_uniq_cpus = 0;
	for_each_online_cpu(i) {
		nr_cpus++;
		first_sibling = get_first_sibling(i);
		if (!cpumask_test_cpu(first_sibling, cpus))
			nr_uniq_cpus++;
		cpumask_set_cpu(i, cpus);
	}

	queue = 0;
	for_each_possible_cpu(i) {
		if (!cpu_online(i)) {
			map[i] = 0;
			continue;
		}

		/*
		 * Easy case - we have equal or more hardware queues. Or
		 * there are no thread siblings to take into account. Do
		 * 1:1 if enough, or sequential mapping if less.
		 */
		if (nr_queues >= nr_cpus || nr_cpus == nr_uniq_cpus) {
			map[i] = cpu_to_queue_index(nr_cpus, nr_queues, queue);
			queue++;
			continue;
		}

		/*
		 * Less then nr_cpus queues, and we have some number of
		 * threads per cores. Map sibling threads to the same
		 * queue.
		 */
		first_sibling = get_first_sibling(i);
		if (first_sibling == i) {
			map[i] = cpu_to_queue_index(nr_uniq_cpus, nr_queues,
							queue);
			queue++;
		} else
			map[i] = map[first_sibling];
	}

	show_map(map, nr_cpus);
	free_cpumask_var(cpus);
	return 0;
}

/*建立硬队列与软队列的映射关系,返回的map中保存了软队列映射的硬队列号*/
unsigned int *blk_mq_make_queue_map(struct blk_mq_reg *reg)
{
	unsigned int *map;

	/* If cpus are offline, map them to first hctx */
	map = kzalloc_node(sizeof(*map) * num_possible_cpus(), GFP_KERNEL,
				reg->numa_node);
	if (!map)
		return NULL;

	/*更新软队列所映射的硬队列编号，保存在map数组里,如map[0]表示软队列0映射的硬队列号*/
	if (!blk_mq_update_queue_map(map, reg->nr_hw_queues))
		return map;

	kfree(map);
	return NULL;
}
