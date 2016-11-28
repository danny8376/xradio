/*
 * Data Transmission thread implementation for XRadio drivers
 *
 * Copyright (c) 2013, XRadio
 * Author: XRadio
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <net/mac80211.h>
#include <linux/kthread.h>

#include "xradio.h"
#include "bh.h"
#include "hwio.h"
#include "wsm.h"
#include "sdio.h"

/* TODO: Verify these numbers with WSM specification. */
#define DOWNLOAD_BLOCK_SIZE_WR	(0x1000 - 4)
/* an SPI message cannot be bigger than (2"12-1)*2 bytes
 * "*2" to cvt to bytes */
#define MAX_SZ_RD_WR_BUFFERS	(DOWNLOAD_BLOCK_SIZE_WR*2)
#define PIGGYBACK_CTRL_REG	(2)
#define EFFECTIVE_BUF_SIZE	(MAX_SZ_RD_WR_BUFFERS - PIGGYBACK_CTRL_REG)

/* Suspend state privates */
enum xradio_bh_pm_state {
	XRADIO_BH_RESUMED = 0,
	XRADIO_BH_SUSPEND,
	XRADIO_BH_SUSPENDED,
	XRADIO_BH_RESUME,
};
typedef int (*xradio_wsm_handler)(struct xradio_common *hw_priv, u8 *data, size_t size);

#ifdef MCAST_FWDING
int wsm_release_buffer_to_fw(struct xradio_vif *priv, int count);
#endif
static int xradio_bh(void *arg);

int xradio_register_bh(struct xradio_common *hw_priv)
{
	int err = 0;
	struct sched_param param = { .sched_priority = 1 };

	SYS_BUG(hw_priv->bh_thread);
	atomic_set(&hw_priv->bh_rx, 0);
	atomic_set(&hw_priv->bh_tx, 0);
	atomic_set(&hw_priv->bh_term, 0);
	atomic_set(&hw_priv->bh_suspend, XRADIO_BH_RESUMED);
	hw_priv->buf_id_tx = 0;
	hw_priv->buf_id_rx = 0;
#ifdef BH_USE_SEMAPHORE
	sema_init(&hw_priv->bh_sem, 0);
	atomic_set(&hw_priv->bh_wk, 0);
#else
	init_waitqueue_head(&hw_priv->bh_wq);
#endif
	init_waitqueue_head(&hw_priv->bh_evt_wq);

	hw_priv->bh_thread = kthread_create(&xradio_bh, hw_priv, XRADIO_BH_THREAD);
	if (IS_ERR(hw_priv->bh_thread)) {
		err = PTR_ERR(hw_priv->bh_thread);
		hw_priv->bh_thread = NULL;
	} else {
		SYS_WARN(sched_setscheduler(hw_priv->bh_thread, SCHED_FIFO, &param));
#ifdef HAS_PUT_TASK_STRUCT
		get_task_struct(hw_priv->bh_thread);
#endif
		wake_up_process(hw_priv->bh_thread);
	}
	return err;
}

void xradio_unregister_bh(struct xradio_common *hw_priv)
{
	struct task_struct *thread = hw_priv->bh_thread;

	if (SYS_WARN(!thread))
		return;

	hw_priv->bh_thread = NULL;
	kthread_stop(thread);
#ifdef HAS_PUT_TASK_STRUCT
	put_task_struct(thread);
#endif
	bh_printk(XRADIO_DBG_NIY, "Unregister success.\n");
}

void xradio_irq_handler(struct xradio_common *hw_priv)
{
	if (/* SYS_WARN */(hw_priv->bh_error))
		return;
#ifdef BH_USE_SEMAPHORE
	atomic_add(1, &hw_priv->bh_rx);
	if (atomic_add_return(1, &hw_priv->bh_wk) == 1) {
		up(&hw_priv->bh_sem);
	}
#else
	if (atomic_add_return(1, &hw_priv->bh_rx) == 1) {
		wake_up(&hw_priv->bh_wq);
	}
#endif

}

void xradio_bh_wakeup(struct xradio_common *hw_priv)
{
	if (SYS_WARN(hw_priv->bh_error))
		return;
#ifdef BH_USE_SEMAPHORE
	atomic_add(1, &hw_priv->bh_tx);
	if (atomic_add_return(1, &hw_priv->bh_wk) == 1) {
		up(&hw_priv->bh_sem);
	}
#else
	if (atomic_add_return(1, &hw_priv->bh_tx) == 1) {
		wake_up(&hw_priv->bh_wq);
	}
#endif
}

int xradio_bh_suspend(struct xradio_common *hw_priv)
{
#ifdef MCAST_FWDING
	int i =0;
	struct xradio_vif *priv = NULL;
#endif

	if (hw_priv->bh_error) {
		return -EINVAL;
	}

#ifdef MCAST_FWDING
 	xradio_for_each_vif(hw_priv, priv, i) {
		if (!priv)
			continue;	
		if ( (priv->multicast_filter.enable)
			&& (priv->join_status == XRADIO_JOIN_STATUS_AP) ) {
			wsm_release_buffer_to_fw(priv,
				(hw_priv->wsm_caps.numInpChBufs - 1));
			break;
		}
	}
#endif

	atomic_set(&hw_priv->bh_suspend, XRADIO_BH_SUSPEND);
#ifdef BH_USE_SEMAPHORE
	up(&hw_priv->bh_sem);
#else
	wake_up(&hw_priv->bh_wq);
#endif
	return wait_event_timeout(hw_priv->bh_evt_wq, (hw_priv->bh_error || 
	                          XRADIO_BH_SUSPENDED == atomic_read(&hw_priv->bh_suspend)),
	                          1 * HZ)?  0 : -ETIMEDOUT;
}

int xradio_bh_resume(struct xradio_common *hw_priv)
{
#ifdef MCAST_FWDING
	int ret;
	int i =0; 
	struct xradio_vif *priv =NULL;
#endif


	if (hw_priv->bh_error) {
		return -EINVAL;
	}

	atomic_set(&hw_priv->bh_suspend, XRADIO_BH_RESUME);
#ifdef BH_USE_SEMAPHORE
	up(&hw_priv->bh_sem);
#else
	wake_up(&hw_priv->bh_wq);
#endif

#ifdef MCAST_FWDING
	ret = wait_event_timeout(hw_priv->bh_evt_wq, (hw_priv->bh_error ||
	                         XRADIO_BH_RESUMED == atomic_read(&hw_priv->bh_suspend))
	                         ,1 * HZ)? 0 : -ETIMEDOUT;

	xradio_for_each_vif(hw_priv, priv, i) {
		if (!priv)
			continue;
		if ((priv->join_status == XRADIO_JOIN_STATUS_AP) && 
			  (priv->multicast_filter.enable)) {
			u8 count = 0;
			SYS_WARN(wsm_request_buffer_request(priv, &count));
			bh_printk(XRADIO_DBG_NIY, "Reclaim Buff %d \n",count);
			break;
		}
	}

	return ret;
#else
	return wait_event_timeout(hw_priv->bh_evt_wq,hw_priv->bh_error ||
		(XRADIO_BH_RESUMED == atomic_read(&hw_priv->bh_suspend)),
		1 * HZ) ? 0 : -ETIMEDOUT;
#endif

}

static inline void wsm_alloc_tx_buffer(struct xradio_common *hw_priv)
{
	++hw_priv->hw_bufs_used;
}

int wsm_release_tx_buffer(struct xradio_common *hw_priv, int count)
{
	int ret = 0;
	int hw_bufs_used = hw_priv->hw_bufs_used;

	hw_priv->hw_bufs_used -= count;
	if (SYS_WARN(hw_priv->hw_bufs_used < 0)) {
		/* Tx data patch stops when all but one hw buffers are used.
		   So, re-start tx path in case we find hw_bufs_used equals
		   numInputChBufs - 1.
		*/
		bh_printk(XRADIO_DBG_ERROR,"%s, hw_bufs_used=%d, count=%d.\n",
		          __func__, hw_priv->hw_bufs_used, count);
		ret = -1;
	} else if (hw_bufs_used >= (hw_priv->wsm_caps.numInpChBufs - 1))
		ret = 1;
	if (!hw_priv->hw_bufs_used)
		wake_up(&hw_priv->bh_evt_wq);
	return ret;
}

int wsm_release_vif_tx_buffer(struct xradio_common *hw_priv, int if_id, int count)
{
	int ret = 0;

	hw_priv->hw_bufs_used_vif[if_id] -= count;
	if (!hw_priv->hw_bufs_used_vif[if_id])
		wake_up(&hw_priv->bh_evt_wq);

	if (SYS_WARN(hw_priv->hw_bufs_used_vif[if_id] < 0))
		ret = -1;
	return ret;
}
#ifdef MCAST_FWDING
int wsm_release_buffer_to_fw(struct xradio_vif *priv, int count)
{
	int i;
	u8 flags;
	struct wsm_buf *buf;
	size_t buf_len;
	struct wsm_hdr *wsm;
	struct xradio_common *hw_priv = priv->hw_priv;

	if (priv->join_status != XRADIO_JOIN_STATUS_AP) {
		return 0;
	}
	bh_printk(XRADIO_DBG_NIY, "Rel buffer to FW %d, %d\n", count, hw_priv->hw_bufs_used);

	for (i = 0; i < count; i++) {
		if ((hw_priv->hw_bufs_used + 1) < hw_priv->wsm_caps.numInpChBufs) {
			flags = i ? 0: 0x1;

			wsm_alloc_tx_buffer(hw_priv);
			buf = &hw_priv->wsm_release_buf[i];
			buf_len = buf->data - buf->begin;

			/* Add sequence number */
			wsm = (struct wsm_hdr *)buf->begin;
			SYS_BUG(buf_len < sizeof(*wsm));

			wsm->id &= __cpu_to_le32(~WSM_TX_SEQ(WSM_TX_SEQ_MAX));
			wsm->id |= cpu_to_le32(WSM_TX_SEQ(hw_priv->wsm_tx_seq));

			bh_printk(XRADIO_DBG_NIY, "REL %d\n", hw_priv->wsm_tx_seq);
			if (SYS_WARN(xradio_data_write(hw_priv, buf->begin, buf_len))) {
				break;
			}
			hw_priv->buf_released = 1;
			hw_priv->wsm_tx_seq = (hw_priv->wsm_tx_seq + 1) & WSM_TX_SEQ_MAX;
		} else
			break;
	}

	if (i == count) {
		return 0;
	}

	/* Should not be here */
	bh_printk(XRADIO_DBG_ERROR,"Error, Less HW buf %d,%d.\n", 
	          hw_priv->hw_bufs_used, hw_priv->wsm_caps.numInpChBufs);
	SYS_WARN(1);
	return -1;
}
#endif

/* reserve a packet for the case dev_alloc_skb failed in bh.*/
int xradio_init_resv_skb(struct xradio_common *hw_priv)
{
	int len = (SDIO_BLOCK_SIZE<<2) + WSM_TX_EXTRA_HEADROOM + \
	           8 + 12;	/* TKIP IV + ICV and MIC */

	hw_priv->skb_reserved = dev_alloc_skb(len);
	if (hw_priv->skb_reserved) {
		hw_priv->skb_resv_len = len;
	} else {
		bh_printk(XRADIO_DBG_WARN,"%s xr_alloc_skb failed(%d)\n",
		          __func__, len);
	}
	return 0;
}

void xradio_deinit_resv_skb(struct xradio_common *hw_priv)
{
	if (hw_priv->skb_reserved) {
		dev_kfree_skb(hw_priv->skb_reserved);
		hw_priv->skb_reserved = NULL;
		hw_priv->skb_resv_len = 0;
	}
}

int xradio_realloc_resv_skb(struct xradio_common *hw_priv,
							struct sk_buff *skb)
{
	if (!hw_priv->skb_reserved && hw_priv->skb_resv_len) {
		hw_priv->skb_reserved = dev_alloc_skb(hw_priv->skb_resv_len);
		if (!hw_priv->skb_reserved) {
			hw_priv->skb_reserved = skb;
			bh_printk(XRADIO_DBG_WARN, "%s xr_alloc_skb failed(%d)\n",
			          __FUNCTION__, hw_priv->skb_resv_len);
			return -1;
		}
	}
	return 0; /* realloc sbk success, deliver to upper.*/
}

static inline struct sk_buff *xradio_get_resv_skb(struct xradio_common *hw_priv,
												  size_t len)
{	struct sk_buff *skb = NULL;
	if (hw_priv->skb_reserved && len <= hw_priv->skb_resv_len) {
		skb = hw_priv->skb_reserved;
		hw_priv->skb_reserved = NULL;
	}
	return skb;
}

static inline int xradio_put_resv_skb(struct xradio_common *hw_priv,
									  struct sk_buff *skb)
{
	if (!hw_priv->skb_reserved && hw_priv->skb_resv_len) {
		hw_priv->skb_reserved = skb;
		return 0;
	}
	return 1; /* sbk not put to reserve*/
}

static struct sk_buff *xradio_get_skb(struct xradio_common *hw_priv, size_t len)
{
	struct sk_buff *skb = NULL;
	size_t alloc_len = (len > SDIO_BLOCK_SIZE) ? len : SDIO_BLOCK_SIZE;

	/* TKIP IV + TKIP ICV and MIC - Piggyback.*/
	alloc_len += WSM_TX_EXTRA_HEADROOM + 8 + 12- 2;
	if (len > SDIO_BLOCK_SIZE || !hw_priv->skb_cache) {
		skb = dev_alloc_skb(alloc_len);
		/* In AP mode RXed SKB can be looped back as a broadcast.
		 * Here we reserve enough space for headers. */
		if (skb) {
			skb_reserve(skb, WSM_TX_EXTRA_HEADROOM + 8 /* TKIP IV */
					    - WSM_RX_EXTRA_HEADROOM);
		} else {
			skb = xradio_get_resv_skb(hw_priv, alloc_len);
			if (skb) {
				bh_printk(XRADIO_DBG_WARN,"%s get skb_reserved(%d)!\n",
				          __FUNCTION__, alloc_len);
				skb_reserve(skb, WSM_TX_EXTRA_HEADROOM + 8 /* TKIP IV */
						    - WSM_RX_EXTRA_HEADROOM);
			} else {
				bh_printk(XRADIO_DBG_ERROR,"%s xr_alloc_skb failed(%d)!\n",
				          __FUNCTION__, alloc_len);
			}
		}
	} else {
		skb = hw_priv->skb_cache;
		hw_priv->skb_cache = NULL;
	}
	return skb;
}

static void xradio_put_skb(struct xradio_common *hw_priv, struct sk_buff *skb)
{
	if (hw_priv->skb_cache)
		dev_kfree_skb(skb);
	else
		hw_priv->skb_cache = skb;
}

static int xradio_bh_read_ctrl_reg(struct xradio_common *hw_priv,
					  u16 *ctrl_reg)
{
	int ret;
	ret = xradio_reg_read_16(hw_priv, HIF_CONTROL_REG_ID, ctrl_reg);
	if (ret) {
		ret = xradio_reg_read_16(hw_priv, HIF_CONTROL_REG_ID, ctrl_reg);
		if (ret) {
			hw_priv->bh_error = 1;
			bh_printk(XRADIO_DBG_ERROR, "Failed to read control register.\n");
		}
	}

	return ret;
}

static int xradio_device_wakeup(struct xradio_common *hw_priv)
{
	u16 ctrl_reg;
	int ret, i=0;

	/* To force the device to be always-on, the host sets WLAN_UP to 1 */
	ret = xradio_reg_write_16(hw_priv, HIF_CONTROL_REG_ID, HIF_CTRL_WUP_BIT);
	if (SYS_WARN(ret))
		return ret;

	ret = xradio_bh_read_ctrl_reg(hw_priv, &ctrl_reg);
	if (SYS_WARN(ret))
		return ret;

	/* If the device returns WLAN_RDY as 1, the device is active and will
	 * remain active. */
	while (!(ctrl_reg & HIF_CTRL_RDY_BIT) && i < 500) {
		ret = xradio_bh_read_ctrl_reg(hw_priv, &ctrl_reg);
		msleep(1);
		i++;
	}
	if (unlikely(i >= 500)) {
		bh_printk(XRADIO_DBG_ERROR, "Device cannot wakeup.\n");
		return -1;
	} else if (unlikely(i >= 50))
		bh_printk(XRADIO_DBG_WARN, "Device wakeup time=%dms.\n", i);
	bh_printk(XRADIO_DBG_NIY, "Device awake, t=%dms.\n", i);
	return 1;
}

/* Must be called from BH thraed. */
void xradio_enable_powersave(struct xradio_vif *priv,
			     bool enable)
{
	priv->powersave_enabled = enable;
	bh_printk(XRADIO_DBG_NIY, "Powerave is %s.\n", enable ? "enabled" : "disabled");
}

static void xradio_bh_rx_dump(u8 *data, size_t len){
	u16 msgid, ifid;
	u16 *p = (u16 *)data;
	msgid = (*(p + 1)) & 0xC3F;
	ifid  = (*(p + 1)) >> 6;
	ifid &= 0xF;
	bh_printk(XRADIO_DBG_ALWY, "[DUMP] msgid 0x%.4X ifid %d len %d\n",
	          msgid, ifid, *p);
	print_hex_dump_bytes("<-- ", DUMP_PREFIX_NONE,
	                     data, min(len, (size_t) 64));
}

static void xradio_bh_tx_dump(u8 *data, size_t len){
	u16 msgid, ifid;
	u16 *p = (u16 *)data;
	msgid = (*(p + 1)) & 0x3F;
	ifid  = (*(p + 1)) >> 6;
	ifid &= 0xF;
	if (msgid == 0x0006) {
		bh_printk(XRADIO_DBG_ALWY, "[DUMP] >>> msgid 0x%.4X ifid %d"
		          "len %d MIB 0x%.4X\n", msgid, ifid,*p, *(p + 2));
	} else {
		bh_printk(XRADIO_DBG_ALWY, "[DUMP] >>> msgid 0x%.4X ifid %d "
		          "len %d\n", msgid, ifid, *p);
	}
	print_hex_dump_bytes("--> ", DUMP_PREFIX_NONE, data,
	                     min(len, (size_t) 64));
}

static int xradio_bh(void *arg)
{
	struct xradio_common *hw_priv = arg;
	struct sk_buff *skb_rx = NULL;
	size_t read_len = 0;
	int rx = 0, tx = 0, term, suspend;
	struct wsm_hdr *wsm;
	size_t wsm_len;
	int wsm_id;
	u8 wsm_seq;
	int rx_resync = 1;
	u16 ctrl_reg = 0;
	int tx_allowed;
	int pending_tx = 0;
	int tx_burst;
	int rx_burst = 0;
	long status;
	u32 dummy;
	int vif_selected;

	for (;;) {
		/* Check if devices can sleep, and set time to wait for interrupt. */
		if (!hw_priv->hw_bufs_used && !pending_tx && 
		    hw_priv->powersave_enabled && !hw_priv->device_can_sleep &&
		    !atomic_read(&hw_priv->recent_scan) &&
		    atomic_read(&hw_priv->bh_rx) == 0   &&
		    atomic_read(&hw_priv->bh_tx) == 0) {
			bh_printk(XRADIO_DBG_MSG, "Device idle, can sleep.\n");
			SYS_WARN(xradio_reg_write_16(hw_priv, HIF_CONTROL_REG_ID, 0));
			hw_priv->device_can_sleep = true;
			status = HZ/8;    //125ms
		} else if (hw_priv->hw_bufs_used) {
			/* don't wait too long if some frames to confirm 
			 * and miss interrupt.*/
			status = HZ/20;   //50ms.
		} else {
			status = HZ/8;    //125ms
		}

		/* Dummy Read for SDIO retry mechanism*/
		if (atomic_read(&hw_priv->bh_rx) == 0 && 
		    atomic_read(&hw_priv->bh_tx) == 0) {
			xradio_reg_read(hw_priv, HIF_CONFIG_REG_ID, &dummy, sizeof(dummy));
		}
		/* If a packet has already been txed to the device then read the 
		 * control register for a probable interrupt miss before going
		 * further to wait for interrupt; if the read length is non-zero
		 * then it means there is some data to be received */
		if (hw_priv->hw_bufs_used) {
			xradio_bh_read_ctrl_reg(hw_priv, &ctrl_reg);
			if(ctrl_reg & HIF_CTRL_NEXT_LEN_MASK) {
				DBG_BH_FIX_RX_ADD;
				rx = 1;
				goto data_proc;
			}
		}

		/* Wait for Events in HZ/8 */
#ifdef BH_USE_SEMAPHORE
		rx = atomic_xchg(&hw_priv->bh_rx, 0);
		tx = atomic_xchg(&hw_priv->bh_tx, 0);
		suspend = pending_tx ? 0 : atomic_read(&hw_priv->bh_suspend);
		term    = kthread_should_stop();
		if (!(rx || tx || term || suspend || hw_priv->bh_error)) {
			atomic_set(&hw_priv->bh_wk, 0);
			status = (long)(down_timeout(&hw_priv->bh_sem, status) != -ETIME);
			//if (status && !atomic_read(&hw_priv->bh_rx) && !atomic_read(&hw_priv->bh_tx))
			//	bh_printk(XRADIO_DBG_ALWY, "bh+\n");
		}
#else
		status = wait_event_interruptible_timeout(hw_priv->bh_wq, ({
		         rx = atomic_xchg(&hw_priv->bh_rx, 0);
		         tx = atomic_xchg(&hw_priv->bh_tx, 0);
		         term = kthread_should_stop();
		         suspend = pending_tx ? 0 : atomic_read(&hw_priv->bh_suspend);
		         (rx || tx || term || suspend || hw_priv->bh_error);}),
		         status);
#endif

		/* 0--bh is going to be shut down */
		if(term) {
			bh_printk(XRADIO_DBG_MSG, "xradio_bh exit!\n");
			break;
		}
		/* 1--An fatal error occurs */
		if (status < 0 || hw_priv->bh_error) {
			bh_printk(XRADIO_DBG_ERROR, "bh_error=%d, status=%ld\n", 
			          hw_priv->bh_error, status);
			hw_priv->bh_error = __LINE__;
			break;
		}

		/* 2--Wait for interrupt time out */
		if (!status) {
			/* Check if miss interrupt. */
			xradio_bh_read_ctrl_reg(hw_priv, &ctrl_reg);
			if(ctrl_reg & HIF_CTRL_NEXT_LEN_MASK) {
				bh_printk(XRADIO_DBG_WARN, "miss interrupt!\n" );
				DBG_BH_MISS_ADD;
				rx = 1;
				goto data_proc;
			}

			/* There are some frames to be confirmed. */
			if (hw_priv->hw_bufs_used) {
				long timeout = 0;
				bool pending = 0;
				bh_printk(XRADIO_DBG_NIY, "Need confirm:%d!\n", hw_priv->hw_bufs_used);
				/* Check if frame transmission is timed out. */
				pending = xradio_query_txpkt_timeout(hw_priv, XRWL_ALL_IFS, 
				                                     hw_priv->pending_frame_id, &timeout);
				/* There are some frames confirm time out. */
				if (pending && timeout < 0) {
					bh_printk(XRADIO_DBG_ERROR, "query_txpkt_timeout:%ld!\n", timeout);
					hw_priv->bh_error = __LINE__;
					break;
				}
				rx = 1; /* Go to check rx again. */
			} else if (!pending_tx){
				if (hw_priv->powersave_enabled && !hw_priv->device_can_sleep && !atomic_read(&hw_priv->recent_scan)) {
					/* Device is idle, we can go to sleep. */
					bh_printk(XRADIO_DBG_MSG, "Device idle(timeout), can sleep.\n");
					SYS_WARN(xradio_reg_write_16(hw_priv, HIF_CONTROL_REG_ID, 0));
					hw_priv->device_can_sleep = true;
				}
				continue;
			}

		/* 3--Host suspend request. */
		} else if (suspend) {
			bh_printk(XRADIO_DBG_NIY, "Host suspend request.\n");
			/* Check powersave setting again. */
			if (hw_priv->powersave_enabled) {
				bh_printk(XRADIO_DBG_MSG,
					 "Device idle(host suspend), can sleep.\n");
				SYS_WARN(xradio_reg_write_16(hw_priv, HIF_CONTROL_REG_ID, 0));
				hw_priv->device_can_sleep = true;
			}

			/* bh thread go to suspend. */
			atomic_set(&hw_priv->bh_suspend, XRADIO_BH_SUSPENDED);
			wake_up(&hw_priv->bh_evt_wq);
#ifdef BH_USE_SEMAPHORE
			do {
				status = down_interruptible(&hw_priv->bh_sem);
			} while (!status && XRADIO_BH_RESUME != atomic_read(&hw_priv->bh_suspend));
			if (XRADIO_BH_RESUME != atomic_read(&hw_priv->bh_suspend))
				status = -1;
			else 
				status = 0;
#else
			status = wait_event_interruptible(hw_priv->bh_wq,
			         XRADIO_BH_RESUME == atomic_read(&hw_priv->bh_suspend));
#endif
			if (status < 0) {
				bh_printk(XRADIO_DBG_ERROR,"ERR: Failed to wait for resume: %ld.\n", status);
				hw_priv->bh_error = __LINE__;
				break;
			}
			bh_printk(XRADIO_DBG_NIY, "Host resume.\n");
			atomic_set(&hw_priv->bh_suspend, XRADIO_BH_RESUMED);
			wake_up(&hw_priv->bh_evt_wq);
			atomic_add(1, &hw_priv->bh_rx);
			continue;
		}
		/* query stuck frames in firmware. */
		if (atomic_xchg(&hw_priv->query_cnt, 0)){
			if(schedule_work(&hw_priv->query_work) <= 0)
				atomic_add(1, &hw_priv->query_cnt);
		}

		/* 4--Rx & Tx process. */
data_proc:
		tx += pending_tx;
		pending_tx = 0;

		if (rx) {
			size_t alloc_len;
			u8 *data;
			/* Check ctrl_reg again. */
			if(!(ctrl_reg & HIF_CTRL_NEXT_LEN_MASK))
				if (SYS_WARN(xradio_bh_read_ctrl_reg(hw_priv, &ctrl_reg))) {
					hw_priv->bh_error = __LINE__;
					break;
				}
rx:
			read_len = (ctrl_reg & HIF_CTRL_NEXT_LEN_MASK)<<1; //read_len=ctrl_reg*2.
			if (!read_len) {
				rx_burst = 0;
				goto tx;
			}
			if (SYS_WARN((read_len < sizeof(struct wsm_hdr)) ||
					(read_len > EFFECTIVE_BUF_SIZE))) {
				bh_printk(XRADIO_DBG_ERROR, "ERR: Invalid read len: %d", read_len);
				hw_priv->bh_error = __LINE__;
				break;
			}

			/* Add SIZE of PIGGYBACK reg (CONTROL Reg)
			 * to the NEXT Message length + 2 Bytes for SKB */
			read_len = read_len + 2;
			
#if defined(CONFIG_XRADIO_NON_POWER_OF_TWO_BLOCKSIZES)
			alloc_len = sdio_align_len(hw_priv, read_len);
#else
			/* Platform's SDIO workaround */
			alloc_len = read_len & ~(SDIO_BLOCK_SIZE - 1);
			if (read_len & (SDIO_BLOCK_SIZE - 1))
				alloc_len += SDIO_BLOCK_SIZE;
#endif /* CONFIG_XRADIO_NON_POWER_OF_TWO_BLOCKSIZES */
			/* Check if not exceeding XRADIO capabilities */
			if (WARN_ON_ONCE(alloc_len > EFFECTIVE_BUF_SIZE)) {
				bh_printk(XRADIO_DBG_MSG, "ERR: Read aligned len: %d\n", alloc_len);
			}

			/* Get skb buffer. */
			skb_rx = xradio_get_skb(hw_priv, alloc_len);
			if (SYS_WARN(!skb_rx)) {
				bh_printk(XRADIO_DBG_ERROR, "ERR: xradio_get_skb failed.\n");
				hw_priv->bh_error = __LINE__;
				break;
			}
			skb_trim(skb_rx, 0);
			skb_put(skb_rx, read_len);
			data = skb_rx->data;
			if (SYS_WARN(!data)) {
				bh_printk(XRADIO_DBG_ERROR, "ERR: skb data is NULL.\n");
				hw_priv->bh_error = __LINE__;
				break;
			}

			/* Read data from device. */
			if (SYS_WARN(xradio_data_read(hw_priv, data, alloc_len))) {
				hw_priv->bh_error = __LINE__;
				break;
			}
			DBG_BH_RX_TOTAL_ADD;

			/* Piggyback */
			ctrl_reg = __le16_to_cpu(((__le16 *)data)[(alloc_len >> 1) - 1]);

			/* check wsm length. */
			wsm = (struct wsm_hdr *)data;
			wsm_len = __le32_to_cpu(wsm->len);

			if (SYS_WARN(wsm_len > read_len)) {
				bh_printk(XRADIO_DBG_ERROR, "wsm_len=%d.\n", wsm_len);
				hw_priv->bh_error = __LINE__;
				break;
			}

			/* dump rx data. */
			xradio_bh_rx_dump(data, wsm_len);

			/* extract wsm id and seq. */
			wsm_id  = __le32_to_cpu(wsm->id) & 0xFFF;
			wsm_seq = (__le32_to_cpu(wsm->id) >> 13) & 7;
			skb_trim(skb_rx, wsm_len);

			/* process exceptions. */
			if(wsm_id == 0){
				printk("wtf?\n");
				continue;
			}
			else if (unlikely(wsm_id == 0x0800)) {
				bh_printk(XRADIO_DBG_ERROR, "firmware exception!\n");
				wsm_handle_exception(hw_priv, &data[sizeof(*wsm)], wsm_len-sizeof(*wsm));
				hw_priv->bh_error = __LINE__;
				break;
			} else if (unlikely(!rx_resync)) {
				if (SYS_WARN(wsm_seq != hw_priv->wsm_rx_seq)) {
					bh_printk(XRADIO_DBG_ERROR, "wsm_seq=%d.\n", wsm_seq);
					hw_priv->bh_error = __LINE__;
					break;
				}
			}
			hw_priv->wsm_rx_seq = (wsm_seq + 1) & 7;
			rx_resync = 0;
#if defined(DGB_XRADIO_HWT)
			rx_resync = 1;  //0 -> 1, HWT test, should not check this.
#endif

			/* Process tx frames confirm. */
			if (wsm_id & 0x0400) {
				int rc = wsm_release_tx_buffer(hw_priv, 1);
				if (SYS_WARN(rc < 0)) {
					bh_printk(XRADIO_DBG_ERROR, "tx buffer < 0.\n");
					hw_priv->bh_error = __LINE__;
					break;
				} else if (rc > 0)
					tx = 1;
			}

			/* WSM processing frames. */
			if (SYS_WARN(wsm_handle_rx(hw_priv, wsm_id, wsm, &skb_rx))) {
				bh_printk(XRADIO_DBG_ERROR, "wsm_handle_rx failed.\n");
				hw_priv->bh_error = __LINE__;
				break;
			}

			/* Reclaim the SKB buffer */
			if (skb_rx) {
				if(xradio_put_resv_skb(hw_priv, skb_rx))
				xradio_put_skb(hw_priv, skb_rx);
				skb_rx = NULL;
			}
			read_len = 0;

			/* Check if rx burst */
			if (rx_burst) {
				xradio_debug_rx_burst(hw_priv);
				--rx_burst;
				goto rx;
			}
		}

tx:
		SYS_BUG(hw_priv->hw_bufs_used > hw_priv->wsm_caps.numInpChBufs);
		tx_burst = hw_priv->wsm_caps.numInpChBufs - hw_priv->hw_bufs_used;
		tx_allowed = tx_burst > 0;
		if (tx && tx_allowed) {
			int ret;
			u8 *data;
			size_t tx_len;

			/* Wake up the devices */
			if (hw_priv->device_can_sleep) {
				ret = xradio_device_wakeup(hw_priv);
				if (SYS_WARN(ret < 0)) {
					hw_priv->bh_error = __LINE__;
					break;
				} else if (ret) {
					hw_priv->device_can_sleep = false;
				} else {  /* Wait for "awake" interrupt */
					pending_tx = tx;
					continue;
				}
			}
			/* Increase Tx buffer*/
			wsm_alloc_tx_buffer(hw_priv);

#if defined(DGB_XRADIO_HWT)
			//hardware test.
			ret = get_hwt_hif_tx(hw_priv, &data, &tx_len, &tx_burst, &vif_selected);
			if (ret <= 0)
#endif //DGB_XRADIO_HWT
				/* Get data to send and send it. */
				ret = wsm_get_tx(hw_priv, &data, &tx_len, &tx_burst, &vif_selected);
			if (ret <= 0) {
				wsm_release_tx_buffer(hw_priv, 1);
				if (SYS_WARN(ret < 0)) {
					bh_printk(XRADIO_DBG_ERROR, "wsm_get_tx=%d.\n", ret);
					hw_priv->bh_error = __LINE__;
					break;
				}
			} else {
				wsm = (struct wsm_hdr *)data;
				SYS_BUG(tx_len < sizeof(*wsm));
				SYS_BUG(__le32_to_cpu(wsm->len) != tx_len);

				/* Continue to send next data if have any. */
				atomic_add(1, &hw_priv->bh_tx);

				/* Align tx length and check it. */
#if defined(CONFIG_XRADIO_NON_POWER_OF_TWO_BLOCKSIZES)
				if (tx_len <= 8)
					tx_len = 16;
				tx_len = sdio_align_len(hw_priv, tx_len);
#else /* CONFIG_XRADIO_NON_POWER_OF_TWO_BLOCKSIZES */
				/* HACK!!! Platform limitation.
				* It is also supported by upper layer:
				* there is always enough space at the end of the buffer. */
				if (tx_len & (SDIO_BLOCK_SIZE - 1)) {
					tx_len &= ~(SDIO_BLOCK_SIZE - 1);
					tx_len += SDIO_BLOCK_SIZE;
				}
#endif /* CONFIG_XRADIO_NON_POWER_OF_TWO_BLOCKSIZES */
				/* Check if not exceeding XRADIO capabilities */
				if (tx_len > EFFECTIVE_BUF_SIZE) {
					bh_printk(XRADIO_DBG_WARN, "Write aligned len: %d\n", tx_len);
				}

				/* Make sequence number. */
				wsm->id &= __cpu_to_le32(~WSM_TX_SEQ(WSM_TX_SEQ_MAX));
				wsm->id |= cpu_to_le32(WSM_TX_SEQ(hw_priv->wsm_tx_seq));

				/* Send the data to devices. */
				if (SYS_WARN(xradio_data_write(hw_priv, data, tx_len))) {
					wsm_release_tx_buffer(hw_priv, 1);
					bh_printk(XRADIO_DBG_ERROR, "xradio_data_write failed\n");
					hw_priv->bh_error = __LINE__;
					break;
				}
				DBG_BH_TX_TOTAL_ADD;

				xradio_bh_tx_dump(data, tx_len);

				/* Process after data have sent. */
				if (vif_selected != -1) {
					hw_priv->hw_bufs_used_vif[vif_selected]++;
				}
				wsm_txed(hw_priv, data);
				hw_priv->wsm_tx_seq = (hw_priv->wsm_tx_seq + 1) & WSM_TX_SEQ_MAX;

				/* Check for burst. */
				if (tx_burst > 1) {
					xradio_debug_tx_burst(hw_priv);
					++rx_burst;
					goto tx;
				}
			}
		} else {
			pending_tx = tx;  //if not allow to tx, pending it.
		}

		/* Check if there are frames to be read. */
		if (ctrl_reg & HIF_CTRL_NEXT_LEN_MASK) {
			DBG_BH_NEXT_RX_ADD;
			goto rx;
		} else {
			xradio_bh_read_ctrl_reg(hw_priv, &ctrl_reg);
			if(ctrl_reg & HIF_CTRL_NEXT_LEN_MASK) {
				DBG_BH_FIX_RX_ADD;
				goto rx;
			}
		}
	}  /* for (;;)*/

	/* Reclaim the SKB buffer when exit. */
	if (skb_rx) {
		if(xradio_put_resv_skb(hw_priv, skb_rx))
		xradio_put_skb(hw_priv, skb_rx);
		skb_rx = NULL;
	}

	/* If BH Error, handle it. */
	if (!term) {
		bh_printk(XRADIO_DBG_ERROR, "Fatal error, exitting code=%d.\n", 
		          hw_priv->bh_error);

#ifdef HW_ERROR_WIFI_RESET
		/* notify upper layer to restart wifi. 
		 * don't do it in debug version. */
		wsm_upper_restart(hw_priv);
#endif
		/* TODO: schedule_work(recovery) */
#ifndef HAS_PUT_TASK_STRUCT
		/* The only reason of having this stupid code here is
		 * that __put_task_struct is not exported by kernel. */
		for (;;) {
#ifdef BH_USE_SEMAPHORE
			status = down_interruptible(&hw_priv->bh_sem);
			term = kthread_should_stop();
#else
			int status = wait_event_interruptible(hw_priv->bh_wq, ({
			             term = kthread_should_stop();
			             (term);}));
#endif
			if (status || term)
				break;
		}
#endif
	}
	atomic_add(1, &hw_priv->bh_term);  //debug info, show bh status.
	return 0;
}
