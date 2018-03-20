/*
 * NTV2 Xilinx DMA interface
 *
 * Copyright 2018 AJA Video Systems Inc. All rights reserved.
 *
 * This program is free software; you may redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define NTV2_REG_CONST
#include "ntv2_xlxdma.h"
#undef NTV2_REG_CONST
#include "ntv2_register.h"
#include "ntv2_xlxreg.h"


#define NTV2_XLXDMA_MAX_TRANSFER_SIZE		(64 * 1024 * 1024)
#define NTV2_XLXDMA_MAX_SEGMENT_SIZE		(15*4096)

#define NTV2_XLXDMA_TRANSFER_TIMEOUT		(100000)
#define NTV2_XLXDMA_STATISTIC_INTERVAL		(5000000)

#define NTV2_XLXDMA_MAX_FRAME_SIZE			(2048 * 1080 * 4 * 6)
#define NTV2_XLXDMA_MAX_PAGES				(NTV2_XLXDMA_MAX_FRAME_SIZE / PAGE_SIZE)

static void ntv2_xlxdma_task(unsigned long data);
static int ntv2_xlxdma_dodma(struct ntv2_xlxdma_task *ntv2_task);
static void ntv2_xlxdma_dpc(unsigned long data);
static void ntv2_xlxdma_timeout(unsigned long data);
static void ntv2_xlxdma_cleanup(struct ntv2_xlxdma *ntv2_xlx);
static void ntv2_xlxdma_stop(struct ntv2_xlxdma *ntv2_xlx);


struct ntv2_xlxdma *ntv2_xlxdma_open(struct ntv2_object *ntv2_obj,
									 const char *name, int index)
{
	struct ntv2_xlxdma *ntv2_xlx;

	if (ntv2_obj == NULL) 
		return NULL;

	ntv2_xlx = kzalloc(sizeof(struct ntv2_xlxdma), GFP_KERNEL);
	if (ntv2_xlx == NULL) {
		NTV2_MSG_ERROR("%s: ntv2_xlxdma instance memory allocation failed\n", ntv2_obj->name);
		return NULL;
	}

	ntv2_xlx->index = index;
	snprintf(ntv2_xlx->name, NTV2_STRING_SIZE, "%s-%s%d", ntv2_obj->name, name, index);
	INIT_LIST_HEAD(&ntv2_xlx->list);
	ntv2_xlx->ntv2_dev = ntv2_obj->ntv2_dev;

	spin_lock_init(&ntv2_xlx->state_lock);

	/* dodma task */
	tasklet_init(&ntv2_xlx->engine_task,
				 ntv2_xlxdma_task,
				 (unsigned long)ntv2_xlx);

	/* interrupt dpc */
	tasklet_init(&ntv2_xlx->engine_dpc,
				 ntv2_xlxdma_dpc,
				 (unsigned long)ntv2_xlx);

	/* timeout timer */
	setup_timer(&ntv2_xlx->engine_timer,
				ntv2_xlxdma_timeout,
				(unsigned long)ntv2_xlx);

	return ntv2_xlx;
}

void ntv2_xlxdma_close(struct ntv2_xlxdma *ntv2_xlx)
{
	if (ntv2_xlx == NULL)
		return;

	/* stop the queue */
	ntv2_xlxdma_disable(ntv2_xlx);

	/* stop the dma engine and cleanup */
	ntv2_xlxdma_stop(ntv2_xlx);
	ntv2_xlxdma_cleanup(ntv2_xlx);

	/* stop the tasks */
	tasklet_kill(&ntv2_xlx->engine_dpc);
	tasklet_kill(&ntv2_xlx->engine_task);

	/* free the descriptor memory */
	if (ntv2_xlx->descriptor != NULL) {
		pci_free_consistent(ntv2_xlx->ntv2_dev->pci_dev,
							ntv2_xlx->descriptor_memsize,
							ntv2_xlx->descriptor,
							ntv2_xlx->dma_descriptor);
	}		

	kfree(ntv2_xlx);
}

int ntv2_xlxdma_configure(struct ntv2_xlxdma *ntv2_xlx, struct ntv2_register *xlx_reg)
{
	u32 max_channels;
	u32 value;
	u32 subsystem;
	u32 target;
	enum ntv2_transfer_mode	mode;
	u32	engine;
	u32	mask;
	u32 i;

	if ((ntv2_xlx == NULL) || (xlx_reg == NULL))
		return -EPERM;

	NTV2_MSG_DMA_INFO("%s: configure xlx dma engine\n", ntv2_xlx->name);

	/* check num engines */
	max_channels = NTV2_REG_COUNT(ntv2_xlxdma_reg_chn_identifier);
	if (ntv2_xlx->index >= max_channels) {
		NTV2_MSG_DMA_ERROR("%s: *error* dma engine index %d out of range\n",
						   ntv2_xlx->name, ntv2_xlx->index);
		return -EPERM;
	}
	ntv2_xlx->xlx_reg = xlx_reg;

	/* find channels */
	for (i = 0; i < max_channels; i++)
	{
		value = ntv2_reg_read(xlx_reg, ntv2_xlxdma_reg_chn_s2c_identifier, i);
		subsystem = NTV2_FLD_GET(ntv2_xlxdma_fld_chn_subsystem_id, value);
		target = NTV2_FLD_GET(ntv2_xlxdma_fld_chn_target, value);
		
		if (subsystem != ntv2_xlxdma_con_subsystem_id)
			continue;

		if (target == ntv2_xlxdma_con_target_channel_s2c) {
			mode = ntv2_transfer_mode_s2c;
			engine = ntv2_xlx->s2c_channels;
			mask = ((u32)0x1) << ntv2_xlx->s2c_channels;
			ntv2_xlx->s2c_channels++;
		}
		else if (target == ntv2_xlxdma_con_target_channel_c2s) {
			mode = ntv2_transfer_mode_c2s;
			engine = ntv2_xlx->c2s_channels;
			mask = ((u32)0x1) << (ntv2_xlx->s2c_channels + ntv2_xlx->c2s_channels);
			ntv2_xlx->c2s_channels++;
		}
		else {
			continue;
		}
		
		ntv2_reg_write(xlx_reg, ntv2_xlxdma_reg_chn_control, i, 0x0);

		if (i == ntv2_xlx->index) {
			ntv2_xlx->mode = mode;
			ntv2_xlx->engine_number = engine;
			ntv2_xlx->interrupt_mask = mask;
		}
	}

	if (mode == ntv2_transfer_mode_unknown) {
		NTV2_MSG_DMA_ERROR("%s: *error* dma engine index %d not present\n",
						   ntv2_xlx->name, ntv2_xlx->index);
		return -EPERM;
	}

	/* configure engine constants */
	value = ntv2_reg_read(xlx_reg, ntv2_xlxdma_reg_chn_alignments, i);
	ntv2_xlx->card_address_size = ((u64)0x1) << NTV2_FLD_GET(ntv2_xlxdma_fld_chn_address_bits, value);
	ntv2_xlx->max_transfer_size = NTV2_XLXDMA_MAX_TRANSFER_SIZE;
	ntv2_xlx->max_pages = NTV2_XLXDMA_MAX_PAGES;
	ntv2_xlx->max_descriptors = NTV2_XLXDMA_MAX_PAGES * 2;

	/* allocate descriptor memory */
	ntv2_xlx->descriptor_memsize = ntv2_xlx->max_descriptors * sizeof(struct ntv2_xlxdma_descriptor);
	ntv2_xlx->descriptor = pci_alloc_consistent(ntv2_xlx->ntv2_dev->pci_dev,
												ntv2_xlx->descriptor_memsize,
												&ntv2_xlx->dma_descriptor);
	if (ntv2_xlx->descriptor == NULL) {
		NTV2_MSG_DMA_ERROR("%s: *error* descriptor memory allocation failed\n", ntv2_xlx->name);
		return -ENOMEM;
	}

	NTV2_MSG_DMA_INFO("%s: configure card address size 0x%08x  max transfer size 0x%08x\n",
					  ntv2_xlx->name,
					  ntv2_xlx->card_address_size,
					  ntv2_xlx->max_transfer_size);

	/* ready for dma transfer */
	NTV2_MSG_DMA_STREAM("%s: xlx dma engine state: idle\n", ntv2_xlx->name);
	ntv2_xlx->engine_state = ntv2_xlxdma_state_idle;
	ntv2_xlx->dma_state = ntv2_task_state_disable;

	return 0;
}

int ntv2_xlxdma_enable(struct ntv2_xlxdma *ntv2_xlx)
{
	unsigned long flags;
	int result;
	int i;

	if (ntv2_xlx == NULL)
		return -EPERM;

	if (ntv2_xlx->dma_state == ntv2_task_state_enable)
		return 0;

	NTV2_MSG_DMA_STATE("%s: xlx dma task enable\n", ntv2_xlx->name);

	/* initialize task lists */
	spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
	INIT_LIST_HEAD(&ntv2_xlx->dmatask_ready_list);
	INIT_LIST_HEAD(&ntv2_xlx->dmatask_done_list);
	for (i = 0; i < NTV2_XLXDMA_MAX_TASKS; i++) {
		ntv2_xlx->dmatask_array[i].index = i;
		INIT_LIST_HEAD(&ntv2_xlx->dmatask_array[i].list);
		ntv2_xlx->dmatask_array[i].ntv2_xlx = ntv2_xlx;
		list_add_tail(&ntv2_xlx->dmatask_array[i].list, &ntv2_xlx->dmatask_done_list);
	}
	ntv2_xlx->stat_transfer_count = 0;
	ntv2_xlx->stat_transfer_bytes = 0;
	ntv2_xlx->stat_transfer_time = 0;
	ntv2_xlx->stat_descriptor_count = 0;
	ntv2_xlx->stat_last_display_time = ntv2_system_time();
	ntv2_xlx->soft_transfer_time = 0;
	ntv2_xlx->soft_dma_time = 0;
	ntv2_xlx->dma_state = ntv2_task_state_enable;
	spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);

	/* schedule the engine task */
	tasklet_schedule(&ntv2_xlx->engine_task);

	/* wait for engine task start */
	result = ntv2_wait((int*)&ntv2_xlx->task_state,
					   (int)ntv2_task_state_enable,
					   NTV2_XLXDMA_TRANSFER_TIMEOUT);
	if (result != 0) {
		NTV2_MSG_DMA_ERROR("%s: *error* timeout waiting for engine task start\n",
						   ntv2_xlx->name);
		return result;
	}

	return 0;
}

int ntv2_xlxdma_disable(struct ntv2_xlxdma *ntv2_xlx)
{
	unsigned long flags;
	int result;

	if (ntv2_xlx == NULL)
		return -EPERM;

	if (ntv2_xlx->dma_state == ntv2_task_state_disable)
		return 0;

	NTV2_MSG_DMA_STATE("%s: xlx dma task disable\n", ntv2_xlx->name);

	spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
	ntv2_xlx->dma_state = ntv2_task_state_disable;
	spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);

	/* schedule the dma task */
	tasklet_schedule(&ntv2_xlx->engine_task);

	/* wait for task stop */
	result = ntv2_wait((int*)&ntv2_xlx->task_state,
					   (int)ntv2_task_state_disable,
					   NTV2_XLXDMA_TRANSFER_TIMEOUT);
	if (result != 0) {
		NTV2_MSG_DMA_ERROR("%s: *error* timeout waiting for engine task stop\n", ntv2_xlx->name);
		return -ETIME;
	}

	/* shutdown dma engine */
	ntv2_xlxdma_abort(ntv2_xlx);

	/* wait for dma engine idle */
	result = ntv2_wait((int*)&ntv2_xlx->engine_state,
					   (int)ntv2_xlxdma_state_idle,
					   NTV2_XLXDMA_TRANSFER_TIMEOUT);
	if (result != 0) {
		NTV2_MSG_DMA_ERROR("%s: *error* timeout waiting for dma engine idle\n", ntv2_xlx->name);
		return -ETIME;
	}

	return 0;
}

int ntv2_xlxdma_transfer(struct ntv2_xlxdma *ntv2_xlx,
						 struct ntv2_transfer *ntv2_trn)
{
	struct ntv2_xlxdma_task *task = NULL;
	unsigned long flags;
	int task_index = 9999;

	if ((ntv2_trn == NULL) ||
		(ntv2_trn->sg_list == NULL) ||
		(ntv2_trn->sg_pages == 0) ||
		(ntv2_trn->card_size[0] == 0))
		return -EINVAL;

	/* get the next task */
	spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
	if ((ntv2_xlx->dma_state == ntv2_task_state_enable) &&
		(!list_empty(&ntv2_xlx->dmatask_done_list))) {
		task = list_first_entry(&ntv2_xlx->dmatask_done_list, struct ntv2_xlxdma_task, list);
	
		task->mode = ntv2_trn->mode;
		task->sg_list = ntv2_trn->sg_list;
		task->sg_pages = ntv2_trn->sg_pages;
		task->sg_offset = ntv2_trn->sg_offset;
		task->card_address[0] = ntv2_trn->card_address[0];
		task->card_address[1] = ntv2_trn->card_address[1];
		task->card_size[0] = ntv2_trn->card_size[0];
		task->card_size[1] = ntv2_trn->card_size[1];
		task->callback_func = ntv2_trn->callback_func;
		task->callback_data = ntv2_trn->callback_data;
		task->dma_start = false;
		task->dma_done = false;
		task->dma_result = 0;

		task_index = task->index;
	
		list_del_init(&task->list);
		list_add_tail(&task->list, &ntv2_xlx->dmatask_ready_list);
	}
	spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);

	if (task != NULL) {
		NTV2_MSG_DMA_STREAM("%s: dma task queue %d  addr0 0x%08x  size0 %d  addr1 0x%08x  size1 %d\n",
							ntv2_xlx->name, task_index,
							ntv2_trn->card_address[0], ntv2_trn->card_size[0],
							ntv2_trn->card_address[1], ntv2_trn->card_size[1]);
	} else {
		NTV2_MSG_DMA_ERROR("%s: *error* dma transfer could not be queued\n",
						   ntv2_xlx->name);
		return -EAGAIN;
	}
	
	/* schedule the engine task */
	tasklet_schedule(&ntv2_xlx->engine_task);

	return 0;
}

static void ntv2_xlxdma_task(unsigned long data)
{
	struct ntv2_xlxdma *ntv2_xlx = (struct ntv2_xlxdma *)data;
	struct ntv2_xlxdma_task *task;
	unsigned long flags;
	int task_index;
	int result;
	int i;

	if (ntv2_xlx == NULL)
		return;

	spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
	ntv2_xlx->task_state = ntv2_xlx->dma_state;
	spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);

	if (ntv2_xlx->task_state != ntv2_task_state_enable)
		return;

	for(i = 0; i < NTV2_XLXDMA_MAX_TASKS; i++) {
		task = NULL;
		task_index = 999;

		spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
		if (!list_empty(&ntv2_xlx->dmatask_ready_list)) {
			task = list_first_entry(&ntv2_xlx->dmatask_ready_list, struct ntv2_xlxdma_task, list);
			task_index = task->index;
		}
		spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);

		if (task == NULL)
			return;

		/* task done */
		if (task->dma_done) {
			NTV2_MSG_DMA_STREAM("%s: dma task done %d\n",
								ntv2_xlx->name, task_index);

			if (task->callback_func != NULL)
				(*task->callback_func)(task->callback_data, task->dma_result);

			spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
			list_del_init(&task->list);
			list_add_tail(&task->list, &ntv2_xlx->dmatask_done_list);
			spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);
			continue;
		}
	
		/* task already started */
		if (task->dma_start)
			return;

		result = ntv2_xlxdma_dodma(task);
		if (result != 0) {
			if (task->callback_func != NULL)
				(*task->callback_func)(task->callback_data, result);

			spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
			list_del_init(&task->list);
			list_add_tail(&task->list, &ntv2_xlx->dmatask_done_list);
			spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);
			continue;
		}
		return;
	}

	NTV2_MSG_DMA_ERROR("%s: *error* dma task process reached max frames\n",
					   ntv2_xlx->name);
}

static int ntv2_xlxdma_dodma(struct ntv2_xlxdma_task *ntv2_task)
{
	struct ntv2_xlxdma *ntv2_xlx;
	struct scatterlist *sgentry;
	struct ntv2_xlxdma_descriptor *desc;
	enum ntv2_xlxdma_state	state;
	unsigned long flags;
	u32		control;
	u64		card_address;
	u64		system_address;
	u64		desc_next;
	u32		desc_count;
	u32		total_size;
	u32		data_size;
	u32		byte_count;
	int		result;
	int		i;

	if ((ntv2_task == NULL) ||
		(ntv2_task->ntv2_xlx == NULL) ||
		(ntv2_task->sg_list == NULL))
		return -EPERM;

	ntv2_xlx = ntv2_task->ntv2_xlx;

	if (ntv2_task->mode != ntv2_xlx->mode) {
		NTV2_MSG_DMA_ERROR("%s: *error* transfer mode %d does not match engine mode %d\n",
						   ntv2_xlx->name, ntv2_task->mode, ntv2_xlx->mode);
		ntv2_xlx->error_count++;
		return -EINVAL;
	}

	spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
	state = ntv2_xlx->engine_state;
	if (ntv2_xlx->engine_state == ntv2_xlxdma_state_idle) {
		ntv2_xlx->engine_state = ntv2_xlxdma_state_start;
	}
	spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);

	if (state != ntv2_xlxdma_state_idle)
		return -EBUSY;

	NTV2_MSG_DMA_STREAM("%s: xlx dma engine state: start\n", ntv2_xlx->name);

	/* record the transfer start time */
	ntv2_xlx->soft_transfer_time_start = ntv2_system_time();

	/* count transfers */
	ntv2_xlx->transfer_start_count++;

	/* get dma parameters */
	ntv2_xlx->dma_task = ntv2_task;

	NTV2_MSG_DMA_STREAM("%s: xlx dma transfer card addr[0] 0x%08x  size[0] %d addr[1] 0x%08x  size[1] %d\n",
						ntv2_xlx->name,
						ntv2_task->card_address[0],
						ntv2_task->card_size[0],
						ntv2_task->card_address[1],
						ntv2_task->card_size[1]);

	if (ntv2_task->card_size[0] == 0) {
		NTV2_MSG_DMA_ERROR("%s: *error* transfer size is zero\n", ntv2_xlx->name);
		ntv2_xlxdma_cleanup(ntv2_xlx);
		ntv2_xlx->error_count++;
		result = -EINVAL;
		goto error_idle;
	}

	if ((ntv2_task->sg_list == NULL) ||
		(ntv2_task->sg_pages == 0)) {
		NTV2_MSG_DMA_ERROR("%s: *error* no scatter list\n", ntv2_xlx->name);
		ntv2_xlxdma_cleanup(ntv2_xlx);
		ntv2_xlx->error_count++;
		result = -EINVAL;
		goto error_idle;
	}

	if (ntv2_task->sg_pages >= ntv2_xlx->max_descriptors) {
		NTV2_MSG_DMA_ERROR("%s: *error* too many descriptor entries %d > %d\n",
						   ntv2_xlx->name,
						   ntv2_task->sg_pages,
						   ntv2_xlx->max_descriptors);
		ntv2_xlxdma_cleanup(ntv2_xlx);
		ntv2_xlx->error_count++;
		result = -EINVAL;
		goto error_idle;
	}

	/* read dma engine control/status register */
	control = ntv2_reg_read(ntv2_xlx->xlx_reg,
							ntv2_xlxdma_reg_engine_control_status, ntv2_xlx->index);

	/* make sure that engine is not running */
	if (NTV2_FLD_GET(ntv2_xlxdma_fld_chain_running, control) != 0)
	{
		NTV2_MSG_DMA_ERROR("%s: *warn* dma running before start  control/status 0x%08x\n",
						   ntv2_xlx->name, control);
		ntv2_xlxdma_stop(ntv2_xlx);
		control = ntv2_reg_read(ntv2_xlx->xlx_reg,
								ntv2_xlxdma_reg_engine_control_status, ntv2_xlx->index);
		if (NTV2_FLD_GET(ntv2_xlxdma_fld_chain_running, control) != 0)
		{
			NTV2_MSG_DMA_ERROR("%s: *error* dma running before start  control/status 0x%08x\n",
							   ntv2_xlx->name, control);
			ntv2_xlxdma_cleanup(ntv2_xlx);
			ntv2_xlx->error_count++;
			result = -EAGAIN;
			goto error_idle;
		}
	}

	/* initialize descriptor generation */
	sgentry = ntv2_task->sg_list;
	card_address = ntv2_task->card_address[0];
	desc = ntv2_xlx->descriptor;
	desc_next = ntv2_xlx->dma_descriptor + sizeof(struct ntv2_xlxdma_descriptor);
	desc_count = 0;
	data_size = 0;
	total_size = ntv2_task->card_size[0] + ntv2_task->card_size[1];
	ntv2_xlx->descriptor_count = 0;
	ntv2_xlx->descriptor_bytes = 0;

	for (i = 0; i < ntv2_task->sg_pages; i++) {
		system_address = sg_dma_address(sgentry);
		byte_count = sg_dma_len(sgentry);

		/* limit transfer to total size */
		if ((data_size + byte_count) > total_size)
			byte_count = total_size - data_size;

		/* handle split transfer */
		if ((ntv2_task->card_size[1] != 0) &&
			(data_size < ntv2_task->card_size[0]) &&
			((data_size + byte_count) >= ntv2_task->card_size[0])) {
			/* write descriptor for first fragment */
			desc->control			= 0;
			desc->byte_count		= ntv2_task->card_size[0] - data_size;
			desc->system_address	= system_address;
			desc->card_address		= card_address;
			desc->next_address		= desc_next;
			/* setup for next fragment */
			system_address += desc->byte_count;
			card_address = ntv2_task->card_address[1];
			byte_count -= desc->byte_count;
			data_size += desc->byte_count;
			if (data_size >= total_size)
				break;
			/* setup for next descriptor */
			desc++;
			desc_next += sizeof(struct ntv2_xlxdma_descriptor);
			desc_count++;
			if (desc_count >= ntv2_xlx->max_descriptors)
			break;
		}

		if (byte_count != 0) {
			/* write the descriptor */
			desc->control			= 0;
			desc->byte_count		= byte_count;
			desc->system_address	= system_address;
			desc->card_address		= card_address;
			desc->next_address		= desc_next;

			/* log some descriptors */
			if (i < 5) {
				NTV2_MSG_DMA_DESCRIPTOR("%s: con %08x cnt %08x sys %08x:%08x crd %08x:%08x nxt %08x:%08x\n",
										ntv2_xlx->name,
										desc->control,
										desc->byte_count,
										NTV2_U64_HIGH(desc->system_address),
										NTV2_U64_LOW(desc->system_address),
										NTV2_U64_HIGH(desc->card_address),
										NTV2_U64_LOW(desc->card_address),
										NTV2_U64_HIGH(desc->next_address),
										NTV2_U64_LOW(desc->next_address));
			}
			/* update card address and size */
			card_address += byte_count;
			data_size += byte_count;
			if (data_size >= total_size)
				break;
			/* setup for next descriptor */
			desc++;
			desc_next += sizeof(struct ntv2_xlxdma_descriptor);
			desc_count++;
			if (desc_count >= ntv2_xlx->max_descriptors)
				break;
		}

		sgentry = sg_next(sgentry);
	}

	if (data_size >= total_size) {
		/* last descriptor generates interrupt */
		desc->control = (NTV2_FLD_MASK(ntv2_xlxdma_fld_control_irq_on_completion) |
						 NTV2_FLD_MASK(ntv2_xlxdma_fld_control_irq_on_short_err) |
						 NTV2_FLD_MASK(ntv2_xlxdma_fld_control_irq_on_short_sw) |
						 NTV2_FLD_MASK(ntv2_xlxdma_fld_control_irq_on_short_hw));
		desc->next_address = 0;
		ntv2_xlx->descriptor_count = desc_count;
		ntv2_xlx->descriptor_bytes = data_size;
	} else {
		NTV2_MSG_DMA_ERROR("%s: *error* descriptor generation not complete\n",
						   ntv2_xlx->name);
		ntv2_xlxdma_cleanup(ntv2_xlx);
		ntv2_xlx->error_count++;
		result = -EINVAL;
		goto error_idle;
	}

	NTV2_MSG_DMA_STREAM("%s: xlx dma engine state: transfer\n", ntv2_xlx->name);
	spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
	ntv2_xlx->engine_state = ntv2_xlxdma_state_transfer;
	spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);

	/* write dma engine descriptor start */
	ntv2_reg_write(ntv2_xlx->xlx_reg,
				   ntv2_xlxdma_reg_chain_start_address_low, ntv2_xlx->index,
				   NTV2_U64_LOW(ntv2_xlx->dma_descriptor));
	ntv2_reg_write(ntv2_xlx->xlx_reg,
				   ntv2_xlxdma_reg_chain_start_address_high, ntv2_xlx->index,
				   NTV2_U64_HIGH(ntv2_xlx->dma_descriptor));

	/* record the dma start */
	ntv2_task->dma_start = true;
	ntv2_xlx->soft_dma_time_start = ntv2_system_time();

	/* start dma engine */
	control = (NTV2_FLD_MASK(ntv2_xlxdma_fld_interrupt_enable) | 
			   NTV2_FLD_MASK(ntv2_xlxdma_fld_interrupt_active) |
			   NTV2_FLD_MASK(ntv2_xlxdma_fld_chain_start) | 
			   NTV2_FLD_MASK(ntv2_xlxdma_fld_chain_complete));

	ntv2_reg_write(ntv2_xlx->xlx_reg,
				   ntv2_xlxdma_reg_engine_control_status, ntv2_xlx->index,
				   control);

	/* start the dma timeout timer */
	mod_timer(&ntv2_xlx->engine_timer, jiffies +
			  usecs_to_jiffies(NTV2_XLXDMA_TRANSFER_TIMEOUT));

	return 0;

error_idle:
	spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
	ntv2_xlx->engine_state = ntv2_xlxdma_state_idle;
	spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);

	return result;
}

int ntv2_xlxdma_interrupt(struct ntv2_xlxdma *ntv2_xlx)
{
	u32	control;

	if ((ntv2_xlx == NULL) || (ntv2_xlx->xlx_reg == NULL))
		return IRQ_NONE;

	/* read control/status register */
	control = ntv2_reg_read(ntv2_xlx->xlx_reg,
							ntv2_xlxdma_reg_engine_control_status, ntv2_xlx->index);

	/* check for interrupt active */
	if ((NTV2_FLD_GET(ntv2_xlxdma_fld_interrupt_enable, control)) &&
		(NTV2_FLD_GET(ntv2_xlxdma_fld_interrupt_active, control)))
	{
		/* clear the interrupt */
		ntv2_reg_write(ntv2_xlx->xlx_reg,
					   ntv2_xlxdma_reg_engine_control_status, ntv2_xlx->index,
					   NTV2_FLD_MASK(ntv2_xlxdma_fld_interrupt_active));

		/* save control/status for dpc */
		ntv2_xlx->dpc_control_status = control;

		/* count the interrupts */
		ntv2_xlx->interrupt_count++;

		/* schedule the dpc */
		tasklet_schedule(&ntv2_xlx->engine_dpc);

		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static void ntv2_xlxdma_dpc(unsigned long data)
{
	struct ntv2_xlxdma *ntv2_xlx = (struct ntv2_xlxdma *)data;
	enum ntv2_xlxdma_state	state;
	unsigned long flags;
	u32		val_hardware_time;
	u32		val_byte_count;
	s64		stat_time;
	int		result = 0;

	if ((ntv2_xlx == NULL) || (ntv2_xlx->xlx_reg == NULL))
		return;

	spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
	state = ntv2_xlx->engine_state;
	if (ntv2_xlx->engine_state == ntv2_xlxdma_state_transfer) {
		ntv2_xlx->engine_state = ntv2_xlxdma_state_done;
	}
	spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);

	if (state != ntv2_xlxdma_state_transfer) {
		NTV2_MSG_DMA_ERROR("%s: *error* xlx dma dpc in bad state %d\n",
						   ntv2_xlx->name, state);
		ntv2_xlx->error_count++;
		return;
	}

	NTV2_MSG_DMA_STREAM("%s: xlx dma engine state: done\n", ntv2_xlx->name);

	/* count dpc */
	ntv2_xlx->dpc_count++;

	/* stop the engine timer */
	del_timer_sync(&ntv2_xlx->engine_timer);

	/* get the hardware time (nanoseconds) and bytes transferred */
	val_hardware_time = ntv2_reg_read(ntv2_xlx->xlx_reg,
									  ntv2_xlxdma_reg_hardware_time, ntv2_xlx->index);
	val_byte_count = ntv2_reg_read(ntv2_xlx->xlx_reg,
								   ntv2_xlxdma_reg_chain_complete_byte_count, ntv2_xlx->index);

	/* transfer complete */
	ntv2_xlx->transfer_complete_count++;

	// check the reason for the interrupt
	if (NTV2_FLD_GET(ntv2_xlxdma_fld_chain_complete, ntv2_xlx->dpc_control_status) != 0)
	{
		ntv2_xlx->stat_transfer_count++;
		ntv2_xlx->stat_transfer_bytes += val_byte_count;
		ntv2_xlx->stat_transfer_time += val_hardware_time;
		ntv2_xlx->stat_descriptor_count += ntv2_xlx->descriptor_count;

		stat_time = ntv2_system_time();
		ntv2_xlx->soft_transfer_time += stat_time - ntv2_xlx->soft_transfer_time_start;
		ntv2_xlx->soft_dma_time += stat_time - ntv2_xlx->soft_dma_time_start;

		if (stat_time > (ntv2_xlx->stat_last_display_time + NTV2_XLXDMA_STATISTIC_INTERVAL))
		{
			s64 stat_transfer_kbytes = ntv2_xlx->stat_transfer_bytes / 1000;
			s64 stat_transfer_time_us = ntv2_xlx->stat_transfer_time / 1000;

			if (ntv2_xlx->stat_transfer_count == 0) {
				ntv2_xlx->stat_transfer_count = 1;
			}
			if (stat_transfer_time_us == 0) {
				stat_transfer_time_us = 1;
			}

			if (NTV2_DEBUG_ACTIVE(NTV2_DEBUG_INFO)) {
				NTV2_MSG_DMA_STATISTICS("%s: dma dir %3s  eng %1d  cnt  %6d  size %6d (kB)  desc %6d\n",
										ntv2_xlx->name,
										(ntv2_xlx->mode == ntv2_transfer_mode_s2c)?"S2C":"C2S",
										ntv2_xlx->engine_number,
										(u32)(ntv2_xlx->stat_transfer_count),
										(u32)(stat_transfer_kbytes / ntv2_xlx->stat_transfer_count ),
										(u32)(ntv2_xlx->stat_descriptor_count / ntv2_xlx->stat_transfer_count));

				NTV2_MSG_DMA_STATISTICS("%s: dma dir %3s  eng %1d  strn %6d  sdma %6d  hdma %6d (us)  perf %6d (MB/s)\n",
										ntv2_xlx->name,
										(ntv2_xlx->mode == ntv2_transfer_mode_s2c)?"S2C":"C2S",
										ntv2_xlx->engine_number,
										(u32)(ntv2_xlx->soft_transfer_time / ntv2_xlx->stat_transfer_count),
										(u32)(ntv2_xlx->soft_dma_time / ntv2_xlx->stat_transfer_count),
										(u32)(stat_transfer_time_us / ntv2_xlx->stat_transfer_count ),
										(u32)(stat_transfer_kbytes*1000 / stat_transfer_time_us));
			} else {
				NTV2_MSG_DMA_STATISTICS("%s: dma dir %3s  eng %1d  cnt %6d  size %6d (kB)  time %6d (us)  perf %6d (MB/s)\n",
										ntv2_xlx->name,
										(ntv2_xlx->mode == ntv2_transfer_mode_s2c)?"S2C":"C2S",
										ntv2_xlx->engine_number,
										(u32)(ntv2_xlx->stat_transfer_count),
										(u32)(stat_transfer_kbytes / ntv2_xlx->stat_transfer_count ),
										(u32)(ntv2_xlx->soft_transfer_time / ntv2_xlx->stat_transfer_count),
										(u32)(stat_transfer_kbytes*1000 / stat_transfer_time_us));
			}

			ntv2_xlx->stat_transfer_count = 0;
			ntv2_xlx->stat_transfer_bytes = 0;
			ntv2_xlx->stat_transfer_time = 0;
			ntv2_xlx->stat_descriptor_count = 0;
			ntv2_xlx->stat_last_display_time = stat_time;
			ntv2_xlx->soft_transfer_time = 0;
			ntv2_xlx->soft_dma_time = 0;
		}
	}
	else
	{
		NTV2_MSG_DMA_ERROR("%s: *error* dma error control/status 0x%08x\n",
						   ntv2_xlx->name, ntv2_xlx->dpc_control_status);
		ntv2_xlxdma_stop(ntv2_xlx);
		ntv2_xlx->error_count++;
		result = -EIO;
	}

	/* report task completion status */
	if (ntv2_xlx->dma_task != NULL) {
		ntv2_xlx->dma_task->dma_done = true;
		ntv2_xlx->dma_task->dma_result = result;
	}

	/* release dma resources */
	ntv2_xlxdma_cleanup(ntv2_xlx);

	NTV2_MSG_DMA_STREAM("%s: xlx dma engine state: idle\n", ntv2_xlx->name);
	spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
	ntv2_xlx->engine_state = ntv2_xlxdma_state_idle;
	spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);

	/* schedule the engine task */
	tasklet_schedule(&ntv2_xlx->engine_task);
}

static void ntv2_xlxdma_timeout(unsigned long data)
{
	struct ntv2_xlxdma		*ntv2_xlx = (struct ntv2_xlxdma *)data;
	enum ntv2_xlxdma_state	state;
	unsigned long			flags;
	u32						control;

	if (ntv2_xlx == NULL)
		return;

	spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
	state = ntv2_xlx->engine_state;
	if (ntv2_xlx->engine_state == ntv2_xlxdma_state_transfer) {
		ntv2_xlx->engine_state = ntv2_xlxdma_state_timeout;
	}
	spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);

	if (state != ntv2_xlxdma_state_transfer) {
		NTV2_MSG_DMA_ERROR("%s: *error* xlx dma timeout in bad state %d\n",
						   ntv2_xlx->name, state);
		ntv2_xlx->error_count++;
		return;
	}

	/* read control/status register */
	control = ntv2_reg_read(ntv2_xlx->xlx_reg,
							ntv2_xlxdma_reg_engine_control_status, ntv2_xlx->index);

	NTV2_MSG_DMA_ERROR("%s: *error* dma engine state: timeout  control/status 0x%08x\n",
					   ntv2_xlx->name, control);

	/* stop transfer */
	ntv2_xlxdma_stop(ntv2_xlx);

	/* report task completion status */
	if (ntv2_xlx->dma_task != NULL) {
		ntv2_xlx->dma_task->dma_done = true;
		ntv2_xlx->dma_task->dma_result = -ETIME;
	}

	/* release dma resources */
	ntv2_xlxdma_cleanup(ntv2_xlx);

	NTV2_MSG_DMA_STREAM("%s: xlx dma engine state: idle\n", ntv2_xlx->name);
	spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
	ntv2_xlx->engine_state = ntv2_xlxdma_state_idle;
	spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);

	/* schedule the engine task */
	tasklet_schedule(&ntv2_xlx->engine_task);
}

void ntv2_xlxdma_abort(struct ntv2_xlxdma *ntv2_xlx)
{
	enum ntv2_xlxdma_state	state;
	unsigned long 			flags;

	if (ntv2_xlx == NULL)
		return;

	spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
	state = ntv2_xlx->engine_state;
	if (ntv2_xlx->engine_state == ntv2_xlxdma_state_transfer) {
		ntv2_xlx->engine_state = ntv2_xlxdma_state_abort;
	}
	spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);

	if (state != ntv2_xlxdma_state_transfer)
		return;

	NTV2_MSG_DMA_STREAM("%s: xlx dma engine state: abort\n", ntv2_xlx->name);

	/* stop transfer */
	ntv2_xlxdma_stop(ntv2_xlx);

	/* report task completion status */
	if (ntv2_xlx->dma_task != NULL) {
		ntv2_xlx->dma_task->dma_done = true;
		ntv2_xlx->dma_task->dma_result = -ECANCELED;
	}

	/* release dma resources */
	ntv2_xlxdma_cleanup(ntv2_xlx);

	NTV2_MSG_DMA_STREAM("%s: xlx dma engine state: idle\n", ntv2_xlx->name);
	spin_lock_irqsave(&ntv2_xlx->state_lock, flags);
	ntv2_xlx->engine_state = ntv2_xlxdma_state_idle;
	spin_unlock_irqrestore(&ntv2_xlx->state_lock, flags);

	/* schedule the engine task */
	tasklet_schedule(&ntv2_xlx->engine_task);
}

static void ntv2_xlxdma_cleanup(struct ntv2_xlxdma *ntv2_xlx)
{
	if (ntv2_xlx == NULL)
		return;

	ntv2_xlx->dma_task = NULL;
	ntv2_xlx->dpc_control_status = 0;
	ntv2_xlx->descriptor_bytes = 0;
	ntv2_xlx->descriptor_count = 0;
}

static void ntv2_xlxdma_stop(struct ntv2_xlxdma *ntv2_xlx)
{
	if ((ntv2_xlx == NULL) || (ntv2_xlx->xlx_reg == NULL))
		return;

	/* stop the engine timer */
	del_timer(&ntv2_xlx->engine_timer);

	/* disable the interrupt */
	ntv2_reg_write(ntv2_xlx->xlx_reg,
				   ntv2_xlxdma_reg_engine_control_status, ntv2_xlx->index,
				   NTV2_FLD_MASK(ntv2_xlxdma_fld_interrupt_active));
	
	/* write the reset bit */
	ntv2_reg_write(ntv2_xlx->xlx_reg,
				   ntv2_xlxdma_reg_engine_control_status, ntv2_xlx->index,
				   NTV2_FLD_MASK(ntv2_xlxdma_fld_status_dma_reset_request));

	/* clear the interrupt */
	ntv2_reg_write(ntv2_xlx->xlx_reg,
				   ntv2_xlxdma_reg_engine_control_status, ntv2_xlx->index,
				   NTV2_FLD_MASK(ntv2_xlxdma_fld_interrupt_active));
}

void ntv2_xlxdma_interrupt_enable(struct ntv2_register *xlx_reg)
{
	if (xlx_reg == NULL)
		return;

	/* enable xlx and user interrupts */
	ntv2_reg_write(xlx_reg,
				   ntv2_xlxdma_reg_common_control_status, 0,
				   NTV2_FLD_MASK(ntv2_xlxdma_fld_dma_interrupt_enable) |
				   NTV2_FLD_MASK(ntv2_xlxdma_fld_user_interrupt_enable));
}

void ntv2_xlxdma_interrupt_disable(struct ntv2_register *xlx_reg)
{
	int num;
	int i;
	u32 val;
	
	if (xlx_reg == NULL)
		return;

	/* disable xlx and user interrupts */
	ntv2_reg_write(xlx_reg,
				   ntv2_xlxdma_reg_common_control_status, 0,
				   0);

	/* disable xlx dma interrupts */
	num = NTV2_REG_COUNT(ntv2_xlxdma_reg_capabilities);
	for (i = 0; i < num; i++) {
		val = ntv2_reg_read(xlx_reg, ntv2_xlxdma_reg_capabilities, i);
		if ((val & NTV2_FLD_MASK(ntv2_xlxdma_fld_present)) != 0) {
			ntv2_reg_write(xlx_reg, ntv2_xlxdma_reg_engine_control_status, i, 0);
		}
	}
}
