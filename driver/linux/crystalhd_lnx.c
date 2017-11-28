/***************************************************************************
  BCM70010 Linux driver
  Copyright (c) 2005-2009, Broadcom Corporation.

  This driver is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, version 2 of the License.

  This driver is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this driver.  If not, see <http://www.gnu.org/licenses/>.
***************************************************************************/

#include <linux/version.h>
#include <linux/compat.h>

#include "crystalhd_lnx.h"

static struct class *crystalhd_class;

static struct crystalhd_adp *g_adp_info;

extern int bc_get_userhandle_count(struct crystalhd_cmd *ctx);

struct device *chddev(void)
{
	return &g_adp_info->pdev->dev;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
loff_t noop_llseek(struct file *file, loff_t offset, int origin)
{
	return file->f_pos;
}
#endif

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 18)
static irqreturn_t chd_dec_isr(int irq, void *arg)
#else
static irqreturn_t chd_dec_isr(int irq, void *arg, struct pt_regs *r)
#endif
{
	struct crystalhd_adp *adp = (struct crystalhd_adp *) arg;
	int rc = 0;
	if (adp)
		rc = crystalhd_cmd_interrupt(&adp->cmds);

	return IRQ_RETVAL(rc);
}

static int chd_dec_enable_int(struct crystalhd_adp *adp)
{
	int rc = 0;

	if (!adp || !adp->pdev) {
		printk(KERN_ERR "%s: Invalid arg\n", __func__);
		return -EINVAL;
	}

	rc = pci_enable_msi(adp->pdev);
	if(rc != 0)
		dev_err(&adp->pdev->dev, "MSI request failed..\n");
	else
		adp->msi = 1;

	rc = request_irq(adp->pdev->irq, chd_dec_isr, IRQF_SHARED,
			 adp->name, (void *)adp);

	if (rc != 0) {
		dev_err(&adp->pdev->dev, "Interrupt request failed..\n");
		if(adp->msi) {
			pci_disable_msi(adp->pdev);
			adp->msi = 0;
		}
	}

	return rc;
}

static int chd_dec_disable_int(struct crystalhd_adp *adp)
{
	if (!adp || !adp->pdev) {
		printk(KERN_ERR "%s: Invalid arg\n", __func__);
		return -EINVAL;
	}

	free_irq(adp->pdev->irq, adp);

	if (adp->msi) {
		pci_disable_msi(adp->pdev);
		adp->msi = 0;
	}

	return 0;
}

crystalhd_ioctl_data *chd_dec_alloc_iodata(struct crystalhd_adp *adp, bool isr)
{
	unsigned long flags = 0;
	crystalhd_ioctl_data *temp;

	if (!adp)
		return NULL;

	spin_lock_irqsave(&adp->lock, flags);

	temp = adp->idata_free_head;
	if (temp) {
		adp->idata_free_head = adp->idata_free_head->next;
		memset(temp, 0, sizeof(*temp));
	}

	spin_unlock_irqrestore(&adp->lock, flags);
	return temp;
}

void chd_dec_free_iodata(struct crystalhd_adp *adp, crystalhd_ioctl_data *iodata,
			 bool isr)
{
	unsigned long flags = 0;

	if (!adp || !iodata)
		return;

	spin_lock_irqsave(&adp->lock, flags);
	iodata->next = adp->idata_free_head;
	adp->idata_free_head = iodata;
	spin_unlock_irqrestore(&adp->lock, flags);
}

static inline int crystalhd_user_data(void __user *ud, void *dr, int size, int set)
{
	int rc;

	if (!ud || !dr) {
		dev_err(chddev(), "%s: Invalid arg\n", __func__);
		return -EINVAL;
	}

	if (set)
	        rc = copy_to_user(ud, dr, size);
	else
		rc = copy_from_user(dr, ud, size);

	if (rc) {
		dev_err(chddev(), "Invalid args for command\n");
		rc = -EFAULT;
	}

	return rc;
}

static int chd_dec_fetch_cdata(struct crystalhd_adp *adp, crystalhd_ioctl_data *io,
			       uint32_t m_sz, void __user *ua)
{
	void __user *ua_off;
	int rc = 0;

	if (!adp || !io || !ua || !m_sz) {
		dev_err(chddev(), "Invalid Arg!!\n");
		return -EINVAL;
	}

	io->add_cdata = vmalloc(m_sz);
	if (!io->add_cdata) {
		dev_err(chddev(), "kalloc fail for sz:%x\n", m_sz);
		return -ENOMEM;
	}

	io->add_cdata_sz = m_sz;
	ua_off = (char __user *)ua + sizeof(io->udata);
	rc = crystalhd_user_data(ua_off, io->add_cdata, io->add_cdata_sz, 0);
	if (rc) {
		dev_err(chddev(), "failed to pull add_cdata sz:%x "
			"ua_off:%p\n", io->add_cdata_sz,
			ua_off);
		kfree(io->add_cdata);
		io->add_cdata = NULL;
		return -ENODATA;
	}

	return rc;
}

static int chd_dec_release_cdata(struct crystalhd_adp *adp,
				 crystalhd_ioctl_data *io, void __user *ua)
{
	void __user *ua_off;
	int rc;

	if (!adp || !io || !ua) {
		dev_err(chddev(), "Invalid Arg!!\n");
		return -EINVAL;
	}

	if (io->cmd != BCM_IOC_FW_DOWNLOAD) {
	        ua_off = (char __user *)ua + sizeof(io->udata);
		rc = crystalhd_user_data(ua_off, io->add_cdata,
					io->add_cdata_sz, 1);
		if (rc) {
			dev_err(chddev(), "failed to push add_cdata sz:%x "
				"ua_off:%p\n", io->add_cdata_sz,
				ua_off);
			return -ENODATA;
		}
	}

	if (io->add_cdata) {
		vfree(io->add_cdata);
		io->add_cdata = NULL;
	}

	return 0;
}

static int chd_dec_proc_user_data(struct crystalhd_adp *adp,
				  crystalhd_ioctl_data *io,
				  void __user *ua, int set)
{
	int rc;
	uint32_t m_sz = 0;

	if (!adp || !io || !ua) {
		dev_err(chddev(), "Invalid Arg!!\n");
		return -EINVAL;
	}

	rc = crystalhd_user_data(ua, &io->udata, sizeof(io->udata), set);
	if (rc) {
		dev_err(chddev(), "failed to %s iodata\n",
			(set ? "set" : "get"));
		return rc;
	}

	switch (io->cmd) {
	case BCM_IOC_MEM_RD:
	case BCM_IOC_MEM_WR:
	case BCM_IOC_FW_DOWNLOAD:
		m_sz = io->udata.u.devMem.NumDwords * 4;
		if (set)
			rc = chd_dec_release_cdata(adp, io, ua);
		else
			rc = chd_dec_fetch_cdata(adp, io, m_sz, ua);
		break;
	default:
		break;
	}

	return rc;
}

static int chd_dec_api_cmd(struct crystalhd_adp *adp, void __user *ua,
			   uint32_t uid, uint32_t cmd, crystalhd_cmd_proc func)
{
	int rc;
	crystalhd_ioctl_data *temp;
	BC_STATUS sts = BC_STS_SUCCESS;

	temp = chd_dec_alloc_iodata(adp, 0);
	if (!temp) {
		dev_err(chddev(), "Failed to get iodata..\n");
		return -EINVAL;
	}

	temp->u_id = uid;
	temp->cmd  = cmd;

	rc = chd_dec_proc_user_data(adp, temp, ua, 0);
	if (!rc) {
		if(func == NULL)
			sts = BC_STS_PWR_MGMT; /* Can only happen when we are in suspend state */
		else
			sts = func(&adp->cmds, temp);
		if (sts == BC_STS_PENDING)
			sts = BC_STS_NOT_IMPL;
		temp->udata.RetSts = sts;
		rc = chd_dec_proc_user_data(adp, temp, ua, 1);
	}

	if (temp) {
		chd_dec_free_iodata(adp, temp, 0);
		temp = NULL;
	}

	return rc;
}

/* API interfaces */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
static int chd_dec_ioctl(struct inode *in, struct file *fd,
			 unsigned int cmd, unsigned long ua)
#else
static long chd_dec_ioctl(struct file *fd,
			  unsigned int cmd, unsigned long ua)
#endif
{
	struct crystalhd_adp *adp = chd_get_adp();
	struct device *dev = &adp->pdev->dev;
	crystalhd_cmd_proc cproc;
	struct crystalhd_user *uc;

	dev_dbg(dev, "Entering %s\n", __func__);

	if (!adp || !fd) {
		dev_err(chddev(), "Invalid adp\n");
		return -EINVAL;
	}

	uc = fd->private_data;
	if (!uc) {
		dev_err(chddev(), "Failed to get uc\n");
		return -ENODATA;
	}

	cproc = crystalhd_get_cmd_proc(&adp->cmds, cmd, uc);
	if (!cproc && !(adp->cmds.state & BC_LINK_SUSPEND)) {
		dev_err(chddev(), "Unhandled command: %d\n", cmd);
		return -EINVAL;
	}

	return chd_dec_api_cmd(adp, (void __user *)ua, uc->uid, cmd, cproc);
}

#ifdef CONFIG_COMPAT
/*
 * LERDA Patrick - Add 32 bits layer compatibility for 64 bits processor mode. Enums in headers are modified for better compatibility with kernel source code. Some general minor fixes added too.
 *                 This works fine while using ia32 mode on a x86-64 kernel, and I hope with the new x32 abi too.
 */

/* -*/
struct compat_PPB_MPEG {
   uint32_t		to_be_defined;
   uint32_t		valid;
   uint32_t		display_horizontal_size;
   uint32_t		display_vertical_size;
   uint32_t		offset_count;
   int32_t		horizontal_offset[3];
   int32_t		vertical_offset[3];
   int32_t		userDataSize;
   compat_uptr_t	userData;
} __attribute__((packed));

struct compat_PPB_VC1 {
   uint32_t		to_be_defined;
   uint32_t		valid;
   uint32_t		display_horizontal_size;
   uint32_t		display_vertical_size;
   uint32_t		num_panscan_windows;
   int32_t		ps_horiz_offset[4];
   int32_t		ps_vert_offset[4];
   int32_t		ps_width[4];
   int32_t		ps_height[4];
   int32_t		userDataSize;
   compat_uptr_t	userData;
} __attribute__((packed));

struct compat_PPB_H264 {
   uint32_t	valid;
   int32_t		poc_top;	/* POC for Top Field/Frame */
   int32_t		poc_bottom;	/* POC for Bottom Field    */
   uint32_t		idr_pic_id;
   uint32_t		pan_scan_count;
   int32_t		pan_scan_left[3];
   int32_t		pan_scan_right[3];
   int32_t		pan_scan_top[3];
   int32_t		pan_scan_bottom[3];
   uint32_t		ct_type_count;
   uint32_t		ct_type[3];
   int32_t		sps_crop_left;
   int32_t		sps_crop_right;
   int32_t		sps_crop_top;
   int32_t		sps_crop_bottom;
   uint32_t		chroma_top;
   uint32_t		chroma_bottom;
   uint32_t		user_data_size;
   compat_uptr_t	user_data;
   compat_uptr_t	pfgt;
} __attribute__((packed));

struct compat_PPB {
   uint32_t	picture_number;	/* Ordinal display number */
   uint32_t	video_buffer;	/* Video (picbuf) number */
   uint32_t	video_address;	/* Address of picbuf Y */
   uint32_t	video_address_uv; /* Address of picbuf UV */
   uint32_t	video_stripe;	/* Picbuf stripe */
   uint32_t	video_width;	/* Picbuf width */
   uint32_t	video_height;	/* Picbuf height */
   uint32_t	channel_id;	/* Decoder channel ID */
   uint32_t	status;		/* reserved */
   uint32_t	width;		/* pixels */
   uint32_t	height;		/* pixels */
   uint32_t	chroma_format;	/* see above */
   uint32_t	pulldown;	/* see above */
   uint32_t	flags;		/* see above */
   uint32_t	pts;		/* 32 LSBs of PTS */
   uint32_t	protocol;	/* protocolXXX (above) */
   uint32_t	frame_rate;	/* see above */
   uint32_t	matrix_coeff;	/* see above */
   uint32_t	aspect_ratio;	/* see above */
   uint32_t	colour_primaries; /* see above */
   uint32_t	transfer_char;	/* see above */
   uint32_t	pcr_offset;	/* 45kHz if PCR type; else 27MHz */
   uint32_t	n_drop;		/* Number of pictures to be dropped */
   uint32_t	custom_aspect_ratio_width_height;/* upper 16-bits is Y and lower 16-bits is X */
   uint32_t	picture_tag;	/* Indexing tag from BUD packets */
   uint32_t	picture_done_payload;
   uint32_t	picture_meta_payload;
   uint32_t	reserved[1];

   union {
      struct compat_PPB_H264	h264;
      struct compat_PPB_MPEG	mpeg;
      struct compat_PPB_VC1	 vc1;
   } other;
} __attribute__((packed));


struct compat_C011_PIB {
  uint32_t     bFormatChange;
  uint32_t     resolution;
  uint32_t     channelId;
  uint32_t     ppbPtr;
  int32_t      ptsStcOffset;
  uint32_t     zeroPanscanValid;
  uint32_t     dramOutBufAddr;
  uint32_t     yComponent;
  struct compat_PPB ppb;
} __attribute__((packed));
/* -*/

struct compat_BC_CMD_REG_ACC {
	uint32_t		Offset;
	uint32_t		Value;
} __attribute__((packed));

struct compat_BC_CMD_DEV_MEM {
	uint32_t		StartOff;
	uint32_t		NumDwords;
	uint32_t		Rsrd;
} __attribute__((packed));

struct compat_BC_FW_CMD {
	uint32_t		cmd[BC_MAX_FW_CMD_BUFF_SZ];
	uint32_t		rsp[BC_MAX_FW_CMD_BUFF_SZ];
	uint32_t		flags;
	uint32_t		add_data;
} __attribute__((packed));

struct compat_BC_HW_TYPE {
	uint16_t		PciDevId;
	uint16_t		PciVenId;
	uint8_t			HwRev;
	uint8_t			Align[3];
} __attribute__((packed));

struct compat_BC_PCI_CFG {
	uint32_t		Size;
	uint32_t		Offset;
	uint8_t			pci_cfg_space[PCI_CFG_SIZE];
} __attribute__((packed));

struct compat_BC_VERSION_INFO {
	uint8_t			DriverMajor;
	uint8_t			DriverMinor;
	uint16_t		DriverRevision;
} __attribute__((packed));

struct compat_BC_START_RX_CAP {
	uint32_t		Rsrd;
	uint32_t		StartDeliveryThsh;
	uint32_t		PauseThsh;
	uint32_t		ResumeThsh;
} __attribute__((packed));

struct compat_BC_FLUSH_RX_CAP {
	uint32_t		Rsrd;
	uint32_t		bDiscardOnly;
} __attribute__((packed));

struct compat_BC_DTS_STATS {
	uint8_t			drvRLL;
	uint8_t			drvFLL;
	uint8_t			eosDetected;
	uint8_t			pwr_state_change; /* 0 is Default (running/stopped), 1 is going to suspend, 2 is going to resume */

	/* Stats from App */
	uint32_t		opFrameDropped;
	uint32_t		opFrameCaptured;
	uint32_t		ipSampleCnt;
	uint64_t		ipTotalSize;
	uint32_t		reptdFrames;
	uint32_t		pauseCount;
	uint32_t		pibMisses;
	uint32_t		discCounter;

	/* Stats from Driver */
	uint32_t		TxFifoBsyCnt;
	uint32_t		intCount;
	uint32_t		DrvIgnIntrCnt;
	uint32_t		DrvTotalFrmDropped;
	uint32_t		DrvTotalHWErrs;
	uint32_t		DrvTotalPIBFlushCnt;
	uint32_t		DrvTotalFrmCaptured;
	uint32_t		DrvPIBMisses;
	uint32_t		DrvPauseTime;
	uint32_t		DrvRepeatedFrms;
	/*
	 * BIT-31 MEANS READ Next PIB Info.
	 * Width will be in bit 0-16.
	 */
	uint64_t		DrvNextMDataPLD;
	uint32_t		DrvcpbEmptySize;

	float			Temperature;
	uint32_t		TempFromDriver;
	uint32_t		picNumFlags;
	uint32_t		res1[7];
} __attribute__((packed));

struct compat_BC_PROC_INPUT {
	compat_uptr_t		pDmaBuff;
	uint32_t		BuffSz;
	uint8_t			Mapped;
	uint8_t			Encrypted;
	uint8_t			Rsrd[2];
	uint32_t		DramOffset;	/* For debug use only */
} __attribute__((packed));

struct compat_BC_DEC_YUV_BUFFS {
	uint32_t		b422Mode;
	compat_uptr_t		YuvBuff;
	uint32_t		YuvBuffSz;
	uint32_t		UVbuffOffset;
	uint32_t		YBuffDoneSz;
	uint32_t		UVBuffDoneSz;
	uint32_t		RefCnt;
} __attribute__((packed));

struct compat_BC_DEC_OUT_BUFF{
	struct compat_BC_DEC_YUV_BUFFS	OutPutBuffs;
        struct compat_C011_PIB PibInfo;
	uint32_t		Flags;
	uint32_t		BadFrCnt;
} __attribute__((packed));

struct compat_BC_NOTIFY_MODE {
	uint32_t		Mode;
	uint32_t		Rsvr[3];
} __attribute__((packed));

/* - */

struct compat_BC_IOCTL_DATA {
	BC_STATUS		RetSts;
	uint32_t		IoctlDataSz;
	uint32_t		Timeout;
	union {
		struct compat_BC_CMD_REG_ACC		regAcc;
		struct compat_BC_CMD_DEV_MEM		devMem;
		struct compat_BC_FW_CMD		        fwCmd;
		struct compat_BC_HW_TYPE		hwType;
		struct compat_BC_PCI_CFG		pciCfg;
		struct compat_BC_VERSION_INFO		VerInfo;
		struct compat_BC_PROC_INPUT		ProcInput;
	        struct compat_BC_DEC_YUV_BUFFS	        RxBuffs;
		struct compat_BC_DEC_OUT_BUFF		DecOutData;
		struct compat_BC_START_RX_CAP		RxCap;
		struct compat_BC_FLUSH_RX_CAP		FlushRxCap;
		struct compat_BC_DTS_STATS		drvStat;
		struct compat_BC_NOTIFY_MODE		NotifyMode;
	} u;
	compat_uptr_t	next;
} __attribute__((packed));


#define COMPAT_BCM_IOC_GET_VERSION             _IOWR(BC_IOC_BASE, DRV_CMD_VERSION, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_GET_HWTYPE              _IOWR(BC_IOC_BASE, DRV_CMD_GET_HWTYPE, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_REG_RD                  _IOWR(BC_IOC_BASE, DRV_CMD_REG_RD, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_REG_WR                  _IOWR(BC_IOC_BASE, DRV_CMD_REG_WR, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_MEM_RD                  _IOWR(BC_IOC_BASE, DRV_CMD_MEM_RD, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_MEM_WR                  _IOWR(BC_IOC_BASE, DRV_CMD_MEM_WR, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_FPGA_RD                 _IOWR(BC_IOC_BASE, DRV_CMD_FPGA_RD, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_FPGA_WR                 _IOWR(BC_IOC_BASE, DRV_CMD_FPGA_WR, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_RD_PCI_CFG              _IOWR(BC_IOC_BASE, DRV_CMD_RD_PCI_CFG, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_WR_PCI_CFG              _IOWR(BC_IOC_BASE, DRV_CMD_WR_PCI_CFG, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_PROC_INPUT              _IOWR(BC_IOC_BASE, DRV_CMD_PROC_INPUT, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_ADD_RXBUFFS             _IOWR(BC_IOC_BASE, DRV_CMD_ADD_RXBUFFS, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_FETCH_RXBUFF            _IOWR(BC_IOC_BASE, DRV_CMD_FETCH_RXBUFF, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_FW_CMD                  _IOWR(BC_IOC_BASE, DRV_ISSUE_FW_CMD, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_START_RX_CAP            _IOWR(BC_IOC_BASE, DRV_CMD_START_RX_CAP, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_FLUSH_RX_CAP            _IOWR(BC_IOC_BASE, DRV_CMD_FLUSH_RX_CAP, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_GET_DRV_STAT            _IOWR(BC_IOC_BASE, DRV_CMD_GET_DRV_STAT, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_RST_DRV_STAT            _IOWR(BC_IOC_BASE, DRV_CMD_RST_DRV_STAT, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_NOTIFY_MODE             _IOWR(BC_IOC_BASE, DRV_CMD_NOTIFY_MODE, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_FW_DOWNLOAD             _IOWR(BC_IOC_BASE, DRV_CMD_FW_DOWNLOAD, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_RELEASE                 _IOWR(BC_IOC_BASE, DRV_CMD_RELEASE, struct compat_BC_IOCTL_DATA)
#define COMPAT_BCM_IOC_END                     BC_IOC_VOID


static int compat_chd_dec_fetch_cdata(struct crystalhd_adp *adp, crystalhd_ioctl_data *io, uint32_t m_sz, void __user *ua)
{
	void __user *ua_off;
	int rc = 0;

	if (!adp || !io || !ua || !m_sz) {
		dev_err(chddev(), "Invalid Arg!!\n");
		return -EINVAL;
	}

	io->add_cdata = vmalloc(m_sz);
	if (!io->add_cdata) {
		dev_err(chddev(), "kalloc fail for sz:%x\n", m_sz);
		return -ENOMEM;
	}

	io->add_cdata_sz = m_sz;
	ua_off = (char __user *)ua + sizeof(struct compat_BC_IOCTL_DATA);
	rc = crystalhd_user_data(ua_off, io->add_cdata, io->add_cdata_sz, 0);
	if (rc) {
		dev_err(chddev(), "failed to pull add_cdata sz:%x "
			"ua_off:%p\n", io->add_cdata_sz,
			ua_off);
		kfree(io->add_cdata);
		io->add_cdata = NULL;
		return -ENODATA;
	}

	return rc;
}

static int compat_chd_dec_release_cdata(struct crystalhd_adp *adp,
				 crystalhd_ioctl_data *io, void __user *ua)
{
	void __user *ua_off;
	int rc;

	if (!adp || !io || !ua) {
		dev_err(chddev(), "Invalid Arg!!\n");
		return -EINVAL;
	}

	if (io->cmd != BCM_IOC_FW_DOWNLOAD) {
	        ua_off = (char __user *)ua + sizeof(struct compat_BC_IOCTL_DATA);
		rc = crystalhd_user_data(ua_off, io->add_cdata,
					io->add_cdata_sz, 1);
		if (rc) {
			dev_err(chddev(), "failed to push add_cdata sz:%x "
				"ua_off:%p\n", io->add_cdata_sz,
				ua_off);
			return -ENODATA;
		}
	}

	if (io->add_cdata) {
		vfree(io->add_cdata);
		io->add_cdata = NULL;
	}

	return 0;
}

static int compat_chd_dec_proc_user_data(struct crystalhd_adp *adp,
				  crystalhd_ioctl_data *io,
				  void __user *ua, int set)
{
	int rc;
	uint32_t m_sz = 0;
	struct compat_BC_IOCTL_DATA w;

	if (!adp || !io || !ua) {
		dev_err(chddev(), "Invalid Arg!!\n");
		return -EINVAL;
	}

	if (set) {
	  memcpy(&w, &io->udata, offsetof(struct compat_BC_IOCTL_DATA, u));
	  memcpy(&w.u, &io->udata.u, offsetof(struct compat_BC_IOCTL_DATA, next)-offsetof(struct compat_BC_IOCTL_DATA, u));

	  w.next = ptr_to_compat(io->udata.next);
	  if (io->udata.next) printk(KERN_ERR "warning: compat_chd_dec_proc_user_data %p not null line %d\n", io->udata.next, __LINE__);

	  switch (io->cmd) {
	  case BCM_IOC_PROC_INPUT:
	    w.u.ProcInput.pDmaBuff = ptr_to_compat(io->udata.u.ProcInput.pDmaBuff);
	    w.u.ProcInput.BuffSz = io->udata.u.ProcInput.BuffSz;
	    w.u.ProcInput.Mapped = io->udata.u.ProcInput.Mapped;
	    w.u.ProcInput.Encrypted = io->udata.u.ProcInput.Encrypted;
	    w.u.ProcInput.Rsrd[0] = io->udata.u.ProcInput.Rsrd[0];
	    w.u.ProcInput.Rsrd[1] = io->udata.u.ProcInput.Rsrd[1];
	    w.u.ProcInput.DramOffset = io->udata.u.ProcInput.DramOffset;
	    break;
	  case BCM_IOC_ADD_RXBUFFS:
	    w.u.RxBuffs.b422Mode = io->udata.u.RxBuffs.b422Mode;
	    w.u.RxBuffs.YuvBuff = ptr_to_compat(io->udata.u.RxBuffs.YuvBuff);
	    w.u.RxBuffs.YuvBuffSz = io->udata.u.RxBuffs.YuvBuffSz;
	    w.u.RxBuffs.UVbuffOffset = io->udata.u.RxBuffs.UVbuffOffset;
	    w.u.RxBuffs.YBuffDoneSz = io->udata.u.RxBuffs.YBuffDoneSz;
	    w.u.RxBuffs.UVBuffDoneSz = io->udata.u.RxBuffs.UVBuffDoneSz;
	    w.u.RxBuffs.RefCnt = io->udata.u.RxBuffs.RefCnt;
	    break;
	  case BCM_IOC_FETCH_RXBUFF:
	    w.u.DecOutData.OutPutBuffs.b422Mode = io->udata.u.DecOutData.OutPutBuffs.b422Mode;
	    w.u.DecOutData.OutPutBuffs.YuvBuff = ptr_to_compat(io->udata.u.DecOutData.OutPutBuffs.YuvBuff);
	    w.u.DecOutData.OutPutBuffs.YuvBuffSz = io->udata.u.DecOutData.OutPutBuffs.YuvBuffSz;
	    w.u.DecOutData.OutPutBuffs.UVbuffOffset = io->udata.u.DecOutData.OutPutBuffs.UVbuffOffset;
	    w.u.DecOutData.OutPutBuffs.YBuffDoneSz = io->udata.u.DecOutData.OutPutBuffs.YBuffDoneSz;
	    w.u.DecOutData.OutPutBuffs.UVBuffDoneSz = io->udata.u.DecOutData.OutPutBuffs.UVBuffDoneSz;
	    w.u.DecOutData.OutPutBuffs.RefCnt = io->udata.u.DecOutData.OutPutBuffs.RefCnt;
	    w.u.DecOutData.PibInfo.bFormatChange = io->udata.u.DecOutData.PibInfo.bFormatChange;
	    w.u.DecOutData.PibInfo.resolution = io->udata.u.DecOutData.PibInfo.resolution;
	    w.u.DecOutData.PibInfo.channelId = io->udata.u.DecOutData.PibInfo.channelId;
	    w.u.DecOutData.PibInfo.ppbPtr = io->udata.u.DecOutData.PibInfo.ppbPtr;
	    w.u.DecOutData.PibInfo.ptsStcOffset = io->udata.u.DecOutData.PibInfo.ptsStcOffset;
	    w.u.DecOutData.PibInfo.zeroPanscanValid = io->udata.u.DecOutData.PibInfo.zeroPanscanValid;
	    w.u.DecOutData.PibInfo.dramOutBufAddr = io->udata.u.DecOutData.PibInfo.dramOutBufAddr;
	    w.u.DecOutData.PibInfo.yComponent = io->udata.u.DecOutData.PibInfo.yComponent;
	    memcpy(&w.u.DecOutData.PibInfo.ppb, &io->udata.u.DecOutData.PibInfo.ppb, sizeof(w.u.DecOutData.PibInfo.ppb));;
	    w.u.DecOutData.Flags = io->udata.u.DecOutData.Flags;
	    w.u.DecOutData.BadFrCnt = io->udata.u.DecOutData.BadFrCnt;
	    break;
	  case BCM_IOC_GET_DRV_STAT: /*sizeof(drvStat): x32:132, x64:136 */
	    w.u.drvStat.drvRLL              = io->udata.u.drvStat.drvRLL;
	    w.u.drvStat.drvFLL              = io->udata.u.drvStat.drvFLL;
	    w.u.drvStat.eosDetected         = io->udata.u.drvStat.eosDetected;
	    w.u.drvStat.pwr_state_change    = io->udata.u.drvStat.pwr_state_change;
	    w.u.drvStat.opFrameDropped      = io->udata.u.drvStat.opFrameDropped;
	    w.u.drvStat.opFrameCaptured     = io->udata.u.drvStat.opFrameCaptured;
	    w.u.drvStat.ipSampleCnt         = io->udata.u.drvStat.ipSampleCnt;
	    w.u.drvStat.ipTotalSize         = io->udata.u.drvStat.ipTotalSize;
	    w.u.drvStat.reptdFrames         = io->udata.u.drvStat.reptdFrames;
	    w.u.drvStat.pauseCount          = io->udata.u.drvStat.pauseCount;
	    w.u.drvStat.pibMisses           = io->udata.u.drvStat.pibMisses;
	    w.u.drvStat.discCounter         = io->udata.u.drvStat.discCounter;
	    w.u.drvStat.TxFifoBsyCnt        = io->udata.u.drvStat.TxFifoBsyCnt;
	    w.u.drvStat.intCount            = io->udata.u.drvStat.intCount;
	    w.u.drvStat.DrvIgnIntrCnt       = io->udata.u.drvStat.DrvIgnIntrCnt;
	    w.u.drvStat.DrvTotalFrmDropped  = io->udata.u.drvStat.DrvTotalFrmDropped;
	    w.u.drvStat.DrvTotalHWErrs      = io->udata.u.drvStat.DrvTotalHWErrs;
	    w.u.drvStat.DrvTotalPIBFlushCnt = io->udata.u.drvStat.DrvTotalPIBFlushCnt;
	    w.u.drvStat.DrvTotalFrmCaptured = io->udata.u.drvStat.DrvTotalFrmCaptured;
	    w.u.drvStat.DrvPIBMisses        = io->udata.u.drvStat.DrvPIBMisses;
	    w.u.drvStat.DrvPauseTime        = io->udata.u.drvStat.DrvPauseTime;
	    w.u.drvStat.DrvRepeatedFrms     = io->udata.u.drvStat.DrvRepeatedFrms;
	    w.u.drvStat.DrvNextMDataPLD     = io->udata.u.drvStat.DrvNextMDataPLD;
	    w.u.drvStat.DrvcpbEmptySize     = io->udata.u.drvStat.DrvcpbEmptySize;
	    w.u.drvStat.Temperature         = io->udata.u.drvStat.Temperature;
	    w.u.drvStat.TempFromDriver      = io->udata.u.drvStat.TempFromDriver;
	    w.u.drvStat.picNumFlags         = io->udata.u.drvStat.picNumFlags;
	    w.u.drvStat.res1[0]             = io->udata.u.drvStat.res1[0];
	    w.u.drvStat.res1[1]             = io->udata.u.drvStat.res1[1];
	    w.u.drvStat.res1[2]             = io->udata.u.drvStat.res1[2];
	    w.u.drvStat.res1[3]             = io->udata.u.drvStat.res1[3];
	    w.u.drvStat.res1[4]             = io->udata.u.drvStat.res1[4];
	    w.u.drvStat.res1[5]             = io->udata.u.drvStat.res1[5];
	    w.u.drvStat.res1[6]             = io->udata.u.drvStat.res1[6];
	    break;
	  case BCM_IOC_MEM_RD:
	  case BCM_IOC_MEM_WR:
	  case BCM_IOC_FW_DOWNLOAD:
            w.u.devMem.StartOff = io->udata.u.devMem.StartOff;
            w.u.devMem.NumDwords = io->udata.u.devMem.NumDwords;
            w.u.devMem.Rsrd = io->udata.u.devMem.Rsrd;
	    break;
	  }
	} else {
	  memset(&w, 0, sizeof(w));
	}

	rc = crystalhd_user_data(ua, &w, sizeof(w), set);

	if (!set) {
	  memcpy(&io->udata, &w, offsetof(struct compat_BC_IOCTL_DATA, u));
	  memcpy(&io->udata.u, &w.u, offsetof(struct compat_BC_IOCTL_DATA, next) - offsetof(struct compat_BC_IOCTL_DATA, u));
	  io->udata.next = compat_ptr(w.next);

	  if (io->udata.next) printk(KERN_ERR "warning: compat_chd_dec_proc_user_data %p not null line %d\n", io->udata.next, __LINE__);

	  switch (io->cmd) {
	  case BCM_IOC_PROC_INPUT:
	    io->udata.u.ProcInput.pDmaBuff = compat_ptr(w.u.ProcInput.pDmaBuff);
	    io->udata.u.ProcInput.BuffSz = w.u.ProcInput.BuffSz;
	    io->udata.u.ProcInput.Mapped = w.u.ProcInput.Mapped;
	    io->udata.u.ProcInput.Encrypted = w.u.ProcInput.Encrypted;
	    io->udata.u.ProcInput.Rsrd[0] = w.u.ProcInput.Rsrd[0];
	    io->udata.u.ProcInput.Rsrd[1] = w.u.ProcInput.Rsrd[1];
	    io->udata.u.ProcInput.DramOffset = w.u.ProcInput.DramOffset;
	    break;
	  case BCM_IOC_ADD_RXBUFFS:
	    io->udata.u.RxBuffs.b422Mode = w.u.RxBuffs.b422Mode;
	    io->udata.u.RxBuffs.YuvBuff = compat_ptr(w.u.RxBuffs.YuvBuff);
	    io->udata.u.RxBuffs.YuvBuffSz = w.u.RxBuffs.YuvBuffSz;
	    io->udata.u.RxBuffs.UVbuffOffset = w.u.RxBuffs.UVbuffOffset;
	    io->udata.u.RxBuffs.YBuffDoneSz = w.u.RxBuffs.YBuffDoneSz;
	    io->udata.u.RxBuffs.UVBuffDoneSz = w.u.RxBuffs.UVBuffDoneSz;
	    io->udata.u.RxBuffs.RefCnt = w.u.RxBuffs.RefCnt;
	    break;
	  case BCM_IOC_FETCH_RXBUFF:
	    io->udata.u.DecOutData.OutPutBuffs.b422Mode = w.u.DecOutData.OutPutBuffs.b422Mode;
	    io->udata.u.DecOutData.OutPutBuffs.YuvBuff = compat_ptr(w.u.DecOutData.OutPutBuffs.YuvBuff);
	    io->udata.u.DecOutData.OutPutBuffs.YuvBuffSz = w.u.DecOutData.OutPutBuffs.YuvBuffSz;
	    io->udata.u.DecOutData.OutPutBuffs.UVbuffOffset = w.u.DecOutData.OutPutBuffs.UVbuffOffset;
	    io->udata.u.DecOutData.OutPutBuffs.YBuffDoneSz = w.u.DecOutData.OutPutBuffs.YBuffDoneSz;
	    io->udata.u.DecOutData.OutPutBuffs.UVBuffDoneSz = w.u.DecOutData.OutPutBuffs.UVBuffDoneSz;
	    io->udata.u.DecOutData.OutPutBuffs.RefCnt = w.u.DecOutData.OutPutBuffs.RefCnt;
	    io->udata.u.DecOutData.PibInfo.bFormatChange = w.u.DecOutData.PibInfo.bFormatChange;
	    io->udata.u.DecOutData.PibInfo.resolution = w.u.DecOutData.PibInfo.resolution;
	    io->udata.u.DecOutData.PibInfo.channelId = w.u.DecOutData.PibInfo.channelId;
	    io->udata.u.DecOutData.PibInfo.ppbPtr = w.u.DecOutData.PibInfo.ppbPtr;
	    io->udata.u.DecOutData.PibInfo.ptsStcOffset = w.u.DecOutData.PibInfo.ptsStcOffset;
	    io->udata.u.DecOutData.PibInfo.zeroPanscanValid = w.u.DecOutData.PibInfo.zeroPanscanValid;
	    io->udata.u.DecOutData.PibInfo.dramOutBufAddr = w.u.DecOutData.PibInfo.dramOutBufAddr;
	    io->udata.u.DecOutData.PibInfo.yComponent = w.u.DecOutData.PibInfo.yComponent;
	    memcpy(&io->udata.u.DecOutData.PibInfo.ppb, &w.u.DecOutData.PibInfo.ppb, sizeof(w.u.DecOutData.PibInfo.ppb));;
	    io->udata.u.DecOutData.Flags = w.u.DecOutData.Flags;
	    io->udata.u.DecOutData.BadFrCnt = w.u.DecOutData.BadFrCnt;
	    break;
	  case BCM_IOC_GET_DRV_STAT:
	    io->udata.u.drvStat.drvRLL              = w.u.drvStat.drvRLL;
	    io->udata.u.drvStat.drvFLL              = w.u.drvStat.drvFLL;
	    io->udata.u.drvStat.eosDetected         = w.u.drvStat.eosDetected;
	    io->udata.u.drvStat.pwr_state_change    = w.u.drvStat.pwr_state_change;
	    io->udata.u.drvStat.opFrameDropped      = w.u.drvStat.opFrameDropped;
	    io->udata.u.drvStat.opFrameCaptured     = w.u.drvStat.opFrameCaptured;
	    io->udata.u.drvStat.ipSampleCnt         = w.u.drvStat.ipSampleCnt;
	    io->udata.u.drvStat.ipTotalSize         = w.u.drvStat.ipTotalSize;
	    io->udata.u.drvStat.reptdFrames         = w.u.drvStat.reptdFrames;
	    io->udata.u.drvStat.pauseCount          = w.u.drvStat.pauseCount;
	    io->udata.u.drvStat.pibMisses           = w.u.drvStat.pibMisses;
	    io->udata.u.drvStat.discCounter         = w.u.drvStat.discCounter;
	    io->udata.u.drvStat.TxFifoBsyCnt        = w.u.drvStat.TxFifoBsyCnt;
	    io->udata.u.drvStat.intCount            = w.u.drvStat.intCount;
	    io->udata.u.drvStat.DrvIgnIntrCnt       = w.u.drvStat.DrvIgnIntrCnt;
	    io->udata.u.drvStat.DrvTotalFrmDropped  = w.u.drvStat.DrvTotalFrmDropped;
	    io->udata.u.drvStat.DrvTotalHWErrs      = w.u.drvStat.DrvTotalHWErrs;
	    io->udata.u.drvStat.DrvTotalPIBFlushCnt = w.u.drvStat.DrvTotalPIBFlushCnt;
	    io->udata.u.drvStat.DrvTotalFrmCaptured = w.u.drvStat.DrvTotalFrmCaptured;
	    io->udata.u.drvStat.DrvPIBMisses        = w.u.drvStat.DrvPIBMisses;
	    io->udata.u.drvStat.DrvPauseTime        = w.u.drvStat.DrvPauseTime;
	    io->udata.u.drvStat.DrvRepeatedFrms     = w.u.drvStat.DrvRepeatedFrms;
	    io->udata.u.drvStat.DrvNextMDataPLD     = w.u.drvStat.DrvNextMDataPLD;
	    io->udata.u.drvStat.DrvcpbEmptySize     = w.u.drvStat.DrvcpbEmptySize;
	    io->udata.u.drvStat.Temperature         = w.u.drvStat.Temperature;
	    io->udata.u.drvStat.TempFromDriver      = w.u.drvStat.TempFromDriver;
	    io->udata.u.drvStat.picNumFlags         = w.u.drvStat.picNumFlags;
	    io->udata.u.drvStat.res1[0]             = w.u.drvStat.res1[0];
	    io->udata.u.drvStat.res1[1]             = w.u.drvStat.res1[1];
	    io->udata.u.drvStat.res1[2]             = w.u.drvStat.res1[2];
	    io->udata.u.drvStat.res1[3]             = w.u.drvStat.res1[3];
	    io->udata.u.drvStat.res1[4]             = w.u.drvStat.res1[4];
	    io->udata.u.drvStat.res1[5]             = w.u.drvStat.res1[5];
	    io->udata.u.drvStat.res1[6]             = w.u.drvStat.res1[6];
	    break;
	  case BCM_IOC_MEM_RD:
	  case BCM_IOC_MEM_WR:
	  case BCM_IOC_FW_DOWNLOAD:
            io->udata.u.devMem.StartOff             = w.u.devMem.StartOff;
            io->udata.u.devMem.NumDwords            = w.u.devMem.NumDwords;
            io->udata.u.devMem.Rsrd                 = w.u.devMem.Rsrd;
	    break;
	  }
	}

	if (rc) {
		dev_err(chddev(), "failed to %s iodata\n",
			(set ? "set" : "get"));
		return rc;
	}

	switch (io->cmd) {
	case BCM_IOC_MEM_RD:
	case BCM_IOC_MEM_WR:
	case BCM_IOC_FW_DOWNLOAD:
		m_sz = io->udata.u.devMem.NumDwords * 4;
		if (set)
			rc = compat_chd_dec_release_cdata(adp, io, ua);
		else
			rc = compat_chd_dec_fetch_cdata(adp, io, m_sz, ua);
		break;
	default:
		break;
	}

	return rc;
}

static int compat_chd_dec_api_cmd(struct crystalhd_adp *adp, void __user *ua,
			   uint32_t uid, uint32_t cmd, crystalhd_cmd_proc func)
{
	int rc;
	crystalhd_ioctl_data *temp;
	BC_STATUS sts = BC_STS_SUCCESS;

	temp = chd_dec_alloc_iodata(adp, 0);
	if (!temp) {
		dev_err(chddev(), "Failed to get iodata..\n");
		return -EINVAL;
	}

	temp->u_id = uid;
	temp->cmd  = cmd;

	rc = compat_chd_dec_proc_user_data(adp, temp, ua, 0);
	if (!rc) {
		if(func == NULL)
			sts = BC_STS_PWR_MGMT; /* Can only happen when we are in suspend state */
		else
			sts = func(&adp->cmds, temp);
		if (sts == BC_STS_PENDING)
			sts = BC_STS_NOT_IMPL;
		temp->udata.RetSts = sts;
		rc = compat_chd_dec_proc_user_data(adp, temp, ua, 1);
	}

	if (temp) {
		chd_dec_free_iodata(adp, temp, 0);
		temp = NULL;
	}

	return rc;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
static int compat_ioctl(struct inode *in, struct file *fd, unsigned int cmd32, unsigned long ua)
#else /*LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)*/
static long compat_ioctl(struct file *fd, unsigned int cmd32, unsigned long ua)
#endif /*LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)*/
{
	struct crystalhd_adp *adp = chd_get_adp();
	//struct device *dev = &adp->pdev->dev;
	crystalhd_cmd_proc cproc;
	struct crystalhd_user *uc;
	uint32_t cmd;

	//dev_dbg(dev, "Entering %s\n", __func__);

	if (!adp || !fd) {
		dev_err(chddev(), "Invalid adp\n");
		return -EINVAL;
	}

	uc = fd->private_data;
	if (!uc) {
		dev_err(chddev(), "Failed to get uc\n");
		return -ENODATA;
	}

	switch (cmd32) {
	        case COMPAT_BCM_IOC_GET_VERSION: cmd = BCM_IOC_GET_VERSION; break;
	        case COMPAT_BCM_IOC_GET_HWTYPE: cmd = BCM_IOC_GET_HWTYPE; break;
           	case COMPAT_BCM_IOC_REG_RD: cmd = BCM_IOC_REG_RD; break;
	        case COMPAT_BCM_IOC_REG_WR: cmd = BCM_IOC_REG_WR; break;
	        case COMPAT_BCM_IOC_MEM_RD: cmd = BCM_IOC_MEM_RD; break;
	        case COMPAT_BCM_IOC_MEM_WR: cmd = BCM_IOC_MEM_WR; break;
	        case COMPAT_BCM_IOC_FPGA_RD: cmd = BCM_IOC_FPGA_RD; break;
	        case COMPAT_BCM_IOC_FPGA_WR: cmd = BCM_IOC_FPGA_WR; break;
	        case COMPAT_BCM_IOC_RD_PCI_CFG: cmd = BCM_IOC_RD_PCI_CFG; break;
	        case COMPAT_BCM_IOC_WR_PCI_CFG: cmd = BCM_IOC_WR_PCI_CFG; break;
	        case COMPAT_BCM_IOC_PROC_INPUT: cmd = BCM_IOC_PROC_INPUT; break;
	        case COMPAT_BCM_IOC_ADD_RXBUFFS: cmd = BCM_IOC_ADD_RXBUFFS; break;
	        case COMPAT_BCM_IOC_FETCH_RXBUFF: cmd = BCM_IOC_FETCH_RXBUFF; break;
	        case COMPAT_BCM_IOC_FW_CMD: cmd = BCM_IOC_FW_CMD; break;
	        case COMPAT_BCM_IOC_START_RX_CAP: cmd = BCM_IOC_START_RX_CAP; break;
	        case COMPAT_BCM_IOC_FLUSH_RX_CAP: cmd = BCM_IOC_FLUSH_RX_CAP; break;
	        case COMPAT_BCM_IOC_GET_DRV_STAT: cmd = BCM_IOC_GET_DRV_STAT; break;
	        case COMPAT_BCM_IOC_RST_DRV_STAT: cmd = BCM_IOC_RST_DRV_STAT; break;
 	        case COMPAT_BCM_IOC_NOTIFY_MODE: cmd = BCM_IOC_NOTIFY_MODE; break;
	        case COMPAT_BCM_IOC_FW_DOWNLOAD: cmd = BCM_IOC_FW_DOWNLOAD; break;
	        case COMPAT_BCM_IOC_RELEASE: cmd = BCM_IOC_RELEASE; break;
	        default: cmd = BCM_IOC_END; break;
	}

	cproc = crystalhd_get_cmd_proc(&adp->cmds, cmd, uc);
	if (!cproc && !(adp->cmds.state & BC_LINK_SUSPEND)) {
		dev_err(chddev(), "Unhandled command: %d\n", cmd);
		return -EINVAL;
	}

	return compat_chd_dec_api_cmd(adp, compat_ptr(ua), uc->uid, cmd, cproc);
}
#endif /*CONFIG_COMPAT*/

static int chd_dec_open(struct inode *in, struct file *fd)
{
	struct crystalhd_adp *adp = chd_get_adp();
	struct device *dev = &adp->pdev->dev;
	int rc = 0;
	BC_STATUS sts = BC_STS_SUCCESS;
	struct crystalhd_user *uc = NULL;

	dev_dbg(dev, "Entering %s\n", __func__);
	if (!adp) {
		dev_err(dev, "Invalid adp\n");
		return -EINVAL;
	}

	if (adp->cfg_users >= BC_LINK_MAX_OPENS) {
		dev_info(dev, "Already in use.%d\n", adp->cfg_users);
		return -EBUSY;
	}

	sts = crystalhd_user_open(&adp->cmds, &uc);
	if (sts != BC_STS_SUCCESS) {
		dev_err(dev, "cmd_user_open - %d\n", sts);
		rc = -EBUSY;
	}
	else {
		adp->cfg_users++;
		fd->private_data = uc;
	}

	return rc;
}

static int chd_dec_close(struct inode *in, struct file *fd)
{
	struct crystalhd_adp *adp = chd_get_adp();
	struct device *dev = &adp->pdev->dev;
	struct crystalhd_cmd *ctx = &adp->cmds;
	struct crystalhd_user *uc;
	uint32_t mode;

	dev_dbg(dev, "Entering %s\n", __func__);
	if (!adp) {
		dev_err(dev, "Invalid adp\n");
		return -EINVAL;
	}

	uc = fd->private_data;
	if (!uc) {
		dev_err(dev, "Failed to get uc\n");
		return -ENODATA;
	}

	/* Check and close only if we have not flush/closed before */
	/* This is needed because release is not guarenteed to be called immediately on close,
	 * if duplicate file handles exist due to fork etc. This causes problems with close and re-open
	 of the device immediately */

	if(uc->in_use) {
		mode = uc->mode;

		ctx->user[uc->uid].mode = DTS_MODE_INV;
		ctx->user[uc->uid].in_use = 0;

		dev_info(chddev(), "Closing user[%x] handle with mode %x\n", uc->uid, mode);

		if (((mode & 0xFF) == DTS_DIAG_MODE) ||
			((mode & 0xFF) == DTS_PLAYBACK_MODE) ||
			((bc_get_userhandle_count(ctx) == 0) && (ctx->hw_ctx != NULL))) {
			ctx->cin_wait_exit = 1;
			ctx->pwr_state_change = BC_HW_RUNNING;
			/* Stop the HW Capture just in case flush did not get called before stop */
			/* And only if we had actually started it */
			if(ctx->hw_ctx->rx_freeq != NULL) {
				crystalhd_hw_stop_capture(ctx->hw_ctx, true);
				crystalhd_hw_free_dma_rings(ctx->hw_ctx);
			}
			if(ctx->adp->fill_byte_pool)
				crystalhd_destroy_dio_pool(ctx->adp);
			if(ctx->adp->elem_pool_head)
				crystalhd_delete_elem_pool(ctx->adp);
			ctx->state = BC_LINK_INVALID;
			crystalhd_hw_close(ctx->hw_ctx, ctx->adp);
			kfree(ctx->hw_ctx);
			ctx->hw_ctx = NULL;
		}

		uc->in_use = 0;

		if(adp->cfg_users > 0)
			adp->cfg_users--;
	}

	return 0;
}

static const struct file_operations chd_dec_fops = {
	.owner		= THIS_MODULE,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
	.ioctl		= chd_dec_ioctl,
#else
	.unlocked_ioctl	= chd_dec_ioctl,
#endif
	.open		= chd_dec_open,
	.release	= chd_dec_close,
	.llseek		= noop_llseek,
#ifdef CONFIG_COMPAT
        .compat_ioctl   = compat_ioctl,
#endif /*CONFIG_COMPAT*/
};

static int __init chd_dec_init_chdev(struct crystalhd_adp *adp)
{
	struct device *xdev = &adp->pdev->dev;
	struct device *dev;
	crystalhd_ioctl_data *temp;
	int rc = -ENODEV, i = 0;

	if (!adp)
		goto fail;

	adp->chd_dec_major = register_chrdev(0, CRYSTALHD_API_NAME,
					     &chd_dec_fops);
	if (adp->chd_dec_major < 0) {
		dev_err(xdev, "Failed to create config dev\n");
		rc = adp->chd_dec_major;
		goto fail;
	}

	/* register crystalhd class */
	crystalhd_class = class_create(THIS_MODULE, "crystalhd");
	if (IS_ERR(crystalhd_class)) {
		dev_err(xdev, "failed to create class\n");
		goto fail;
	}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 25)
	dev = device_create(crystalhd_class, NULL, MKDEV(adp->chd_dec_major, 0),
			    NULL, "crystalhd");
#else
	dev = device_create(crystalhd_class, NULL, MKDEV(adp->chd_dec_major, 0),
			    "crystalhd");
#endif
	if (IS_ERR(dev)) {
		dev_err(xdev, "failed to create device\n");
		goto device_create_fail;
	}

/*	rc = crystalhd_create_elem_pool(adp, BC_LINK_ELEM_POOL_SZ); */
/*	if (rc) { */
/*		dev_err(xdev, "failed to create device\n"); */
/*		goto elem_pool_fail; */
/*	} */

	/* Allocate general purpose ioctl pool. */
	for (i = 0; i < CHD_IODATA_POOL_SZ; i++) {
		temp = kzalloc(sizeof(crystalhd_ioctl_data), GFP_KERNEL);
		if (!temp) {
			dev_err(xdev, "ioctl data pool kzalloc failed\n");
			rc = -ENOMEM;
			goto kzalloc_fail;
		}
		/* Add to global pool.. */
		chd_dec_free_iodata(adp, temp, 0);
	}

	return 0;

kzalloc_fail:
	/*crystalhd_delete_elem_pool(adp); */
/*elem_pool_fail: */
	device_destroy(crystalhd_class, MKDEV(adp->chd_dec_major, 0));
device_create_fail:
	class_destroy(crystalhd_class);
fail:
	return rc;
}

static void chd_dec_release_chdev(struct crystalhd_adp *adp)
{
	crystalhd_ioctl_data *temp = NULL;
	if (!adp)
		return;

	if (adp->chd_dec_major > 0) {
		/* unregister crystalhd class */
		device_destroy(crystalhd_class, MKDEV(adp->chd_dec_major, 0));
		unregister_chrdev(adp->chd_dec_major, CRYSTALHD_API_NAME);
		dev_info(chddev(), "released api device - %d\n",
		       adp->chd_dec_major);
		class_destroy(crystalhd_class);
	}
	adp->chd_dec_major = 0;

	/* Clear iodata pool.. */
	do {
		temp = chd_dec_alloc_iodata(adp, 0);
		kfree(temp);
	} while (temp);

	/*crystalhd_delete_elem_pool(adp); */
}

static int __init chd_pci_reserve_mem(struct crystalhd_adp *pinfo)
{
	struct device *dev = &pinfo->pdev->dev;
	int rc;

	uint32_t bar0		= pci_resource_start(pinfo->pdev, 0);
	uint32_t i2o_len	= pci_resource_len(pinfo->pdev, 0);

	uint32_t bar2		= pci_resource_start(pinfo->pdev, 2);
	uint32_t mem_len	= pci_resource_len(pinfo->pdev, 2);

	dev_dbg(dev, "bar0:0x%x-0x%08x  bar2:0x%x-0x%08x\n",
	        bar0, i2o_len, bar2, mem_len);

	/* bar-0 */
	pinfo->pci_i2o_start = bar0;
	pinfo->pci_i2o_len   = i2o_len;

	/* bar-2 */
	pinfo->pci_mem_start = bar2;
	pinfo->pci_mem_len   = mem_len;

	/* pdev */
	rc = pci_request_regions(pinfo->pdev, pinfo->name);
	if (rc < 0) {
		printk(KERN_ERR "Region request failed: %d\n", rc);
		return rc;
	}
	
	pinfo->i2o_addr = pci_ioremap_bar(pinfo->pdev, 0);
	if (!pinfo->i2o_addr) {
		printk(KERN_ERR "Failed to remap i2o region...\n");
		return -ENOMEM;
	}
	
	pinfo->mem_addr = pci_ioremap_bar(pinfo->pdev, 2);
	if (!pinfo->mem_addr) {
		printk(KERN_ERR "Failed to remap mem region...\n");
		return -ENOMEM;
	}

	dev_dbg(dev, "i2o_addr:0x%08lx   Mapped addr:0x%08lx  \n",
	        (unsigned long)pinfo->i2o_addr, (unsigned long)pinfo->mem_addr);

	return 0;
}

static void chd_pci_release_mem(struct crystalhd_adp *pinfo)
{
	if (!pinfo)
		return;

	if (pinfo->mem_addr)
		iounmap(pinfo->mem_addr);

	if (pinfo->i2o_addr)
		iounmap(pinfo->i2o_addr);

	pci_release_regions(pinfo->pdev);
}


static void __exit chd_dec_pci_remove(struct pci_dev *pdev)
{
	struct crystalhd_adp *pinfo;
	BC_STATUS sts = BC_STS_SUCCESS;

	dev_dbg(chddev(), "Entering %s\n", __func__);

	pinfo = (struct crystalhd_adp *) pci_get_drvdata(pdev);
	if (!pinfo) {
		dev_err(chddev(), "could not get adp\n");
		return;
	}

	sts = crystalhd_delete_cmd_context(&pinfo->cmds);
	if (sts != BC_STS_SUCCESS)
		dev_err(chddev(), "cmd delete :%d\n", sts);

	chd_dec_release_chdev(pinfo);

	chd_dec_disable_int(pinfo);

	chd_pci_release_mem(pinfo);
	pci_disable_device(pinfo->pdev);

	kfree(pinfo);
	g_adp_info = NULL;
}

static int __init chd_dec_pci_probe(struct pci_dev *pdev,
			     const struct pci_device_id *entry)
{
	struct device *dev = &pdev->dev;
	struct crystalhd_adp *pinfo;
	int rc;
	BC_STATUS sts = BC_STS_SUCCESS;

	dev_info(dev, "Starting Device:0x%04x\n", pdev->device);

	pinfo = kzalloc(sizeof(struct crystalhd_adp), GFP_KERNEL);
	if (!pinfo) {
		dev_err(dev, "%s: Failed to allocate memory\n", __func__);
		rc = -ENOMEM;
		goto out;
	}

	pinfo->pdev = pdev;

	rc = pci_enable_device(pdev);
	if (rc) {
		dev_err(dev, "%s: Failed to enable PCI device\n", __func__);
		goto free_priv;
	}

	snprintf(pinfo->name, sizeof(pinfo->name), "crystalhd_pci_e:%d:%d:%d",
		 pdev->bus->number, PCI_SLOT(pdev->devfn),
		 PCI_FUNC(pdev->devfn));

	rc = chd_pci_reserve_mem(pinfo);
	if (rc) {
		dev_err(dev, "%s: Failed to set up memory regions.\n",
			__func__);
		goto disable_device;
	}

	pinfo->present	= 1;
	pinfo->drv_data = entry->driver_data;

	/* Setup adapter level lock.. */
	spin_lock_init(&pinfo->lock);

	/* setup api stuff.. */
	rc = chd_dec_init_chdev(pinfo);
	if (rc)
		goto release_mem;

	rc = chd_dec_enable_int(pinfo);
	if (rc) {
		dev_err(dev, "%s: _enable_int err:%d\n", __func__, rc);
		goto cleanup_chdev;
	}

	/* Set dma mask... */
	if (!pci_set_dma_mask(pdev, DMA_BIT_MASK(64))) {
		pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64));
		pinfo->dmabits = 64;
	} else if (!pci_set_dma_mask(pdev, DMA_BIT_MASK(32))) {
		pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
		pinfo->dmabits = 32;
	} else {
		dev_err(dev, "%s: Unabled to setup DMA %d\n", __func__, rc);
		rc = -ENODEV;
		goto cleanup_int;
	}

	sts = crystalhd_setup_cmd_context(&pinfo->cmds, pinfo);
	if (sts != BC_STS_SUCCESS) {
		dev_err(dev, "%s: cmd setup :%d\n", __func__, sts);
		rc = -ENODEV;
		goto cleanup_int;
	}

	pci_set_master(pdev);

	pci_set_drvdata(pdev, pinfo);

	g_adp_info = pinfo;

out:
	return rc;
cleanup_int:
	chd_dec_disable_int(pinfo);
cleanup_chdev:
	chd_dec_release_chdev(pinfo);
release_mem:
	chd_pci_release_mem(pinfo);
disable_device:
	pci_disable_device(pdev);
free_priv:
	kfree(pdev);
	goto out;
}

int chd_dec_pci_suspend(struct pci_dev *pdev, pm_message_t state)
{
	struct crystalhd_adp *adp;
	struct device *dev = &pdev->dev;
	crystalhd_ioctl_data *temp;
	BC_STATUS sts = BC_STS_SUCCESS;

	adp = (struct crystalhd_adp *)pci_get_drvdata(pdev);
	if (!adp) {
		dev_err(dev, "%s: could not get adp\n", __func__);
		return -ENODEV;
	}

	temp = chd_dec_alloc_iodata(adp, false);
	if (!temp) {
		dev_err(dev, "could not get ioctl data\n");
		return -ENODEV;
	}

	sts = crystalhd_suspend(&adp->cmds, temp);
	if (sts != BC_STS_SUCCESS) {
		dev_err(dev, "Crystal HD Suspend %d\n", sts);
		chd_dec_free_iodata(adp, temp, false);
		return -ENODEV;
	}

	chd_dec_free_iodata(adp, temp, false);
	chd_dec_disable_int(adp);
	pci_save_state(pdev);

	/* Disable IO/bus master/irq router */
	pci_disable_device(pdev);
	pci_set_power_state(pdev, pci_choose_state(pdev, state));
	return 0;
}

int chd_dec_pci_resume(struct pci_dev *pdev)
{
	struct crystalhd_adp *adp;
	struct device *dev = &pdev->dev;
	BC_STATUS sts = BC_STS_SUCCESS;
	int rc;

	adp = (struct crystalhd_adp *)pci_get_drvdata(pdev);
	if (!adp) {
		dev_err(dev, "%s: could not get adp\n", __func__);
		return -ENODEV;
	}

	pci_set_power_state(pdev, PCI_D0);
	pci_restore_state(pdev);

	/* device's irq possibly is changed, driver should take care */
	if (pci_enable_device(pdev)) {
		dev_err(dev, "Failed to enable PCI device\n");
		return 1;
	}

	pci_set_master(pdev);

	rc = chd_dec_enable_int(adp);
	if (rc) {
		dev_err(dev, "_enable_int err:%d\n", rc);
		pci_disable_device(pdev);
		return -ENODEV;
	}

	sts = crystalhd_resume(&adp->cmds);
	if (sts != BC_STS_SUCCESS) {
		dev_err(dev, "Crystal HD Resume %d\n", sts);
		pci_disable_device(pdev);
		return -ENODEV;
	}

	return 0;
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 24)
static const struct pci_device_id chd_dec_pci_id_table[] = {
	{ PCI_VDEVICE(BROADCOM, 0x1612), 8 },
	{ PCI_VDEVICE(BROADCOM, 0x1615), 8 },
	{ 0, },
};
#else
static const struct pci_device_id chd_dec_pci_id_table[] = {
/*	vendor, device, subvendor, subdevice, class, classmask, driver_data */
	{ 0x14e4, 0x1612, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 8 },
	{ 0x14e4, 0x1615, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 8 },
	{ 0, },
};
#endif
MODULE_DEVICE_TABLE(pci, chd_dec_pci_id_table);

static struct pci_driver bc_chd_driver __refdata = {
	.name     = "crystalhd",
	.probe    = chd_dec_pci_probe,
	.remove   = __exit_p(chd_dec_pci_remove),
	.id_table = chd_dec_pci_id_table,
	.suspend  = chd_dec_pci_suspend,
	.resume   = chd_dec_pci_resume
};

struct crystalhd_adp *chd_get_adp(void)
{
	return g_adp_info;
}

static int __init chd_dec_module_init(void)
{
	int rc;

	printk(KERN_DEBUG "Loading crystalhd v%d.%d.%d\n",
	       crystalhd_kmod_major, crystalhd_kmod_minor, crystalhd_kmod_rev);

	rc = pci_register_driver(&bc_chd_driver);

	if (rc < 0)
		printk(KERN_ERR "%s: Could not find any devices. err:%d\n",
		       __func__, rc);

	return rc;
}
module_init(chd_dec_module_init);

static void __exit chd_dec_module_cleanup(void)
{
	printk(KERN_DEBUG "Unloading crystalhd %d.%d.%d\n",
	       crystalhd_kmod_major, crystalhd_kmod_minor, crystalhd_kmod_rev);

	pci_unregister_driver(&bc_chd_driver);
}
module_exit(chd_dec_module_cleanup);

MODULE_AUTHOR("Naren Sankar <nsankar@broadcom.com>");
MODULE_AUTHOR("Prasad Bolisetty <prasadb@broadcom.com>");
MODULE_DESCRIPTION(CRYSTAL_HD_NAME);
MODULE_LICENSE("GPL");
MODULE_ALIAS("crystalhd");
