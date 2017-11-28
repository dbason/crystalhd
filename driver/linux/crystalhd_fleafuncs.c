/***************************************************************************
 * Copyright (c) 2005-2009, Broadcom Corporation.
 *
 *  Name: crystalhd_fleafuncs.c
 *
 *  Description:
 *		BCM70015 Linux driver HW layer.
 *
 *  HISTORY:
 *
 **********************************************************************
 * This file is part of the crystalhd device driver.
 *
 * This driver is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This driver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this driver.  If not, see <http://www.gnu.org/licenses/>.
 **********************************************************************/

#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
#include <linux/sched/signal.h>
#endif
#include <asm/tsc.h>
#include <asm/msr.h>
#include "crystalhd_hw.h"
#include "crystalhd_fleafuncs.h"
#include "crystalhd_lnx.h"
#include "FleaDefs.h"
#include "crystalhd_flea_ddr.h"

#define OFFSETOF(_s_, _m_) ((size_t)(unsigned long)&(((_s_ *)0)->_m_))

void crystalhd_flea_core_reset(struct crystalhd_hw *hw)
{
	unsigned int pollCnt=0,regVal=0;

	dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_core_reset]: Starting core reset\n");

	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC3_RESET_CTRL,	0x01);

	pollCnt=0;
	while (1)
	{
		pollCnt++;
		regVal=0;

		msleep_interruptible(1);

		regVal = hw->pfnReadDevRegister(hw->adp, BCHP_MISC3_RESET_CTRL);

		if(!(regVal & 0x01))
		{
			/*
			-- Bit is 0, Reset is completed. Which means that
			-- wait for sometime and then allow other accesses.
			*/
			msleep_interruptible(1);
			break;
		}

		if(pollCnt > MAX_VALID_POLL_CNT)
		{
			printk("!!FATAL ERROR!! Core Reset Failure\n");
			break;
		}
	}

	msleep_interruptible(5);

	return;
}

void crystalhd_flea_disable_interrupts(struct crystalhd_hw *hw)
{
	union FLEA_INTR_BITS_COMMON IntrMaskReg;
	/*
	-- Mask everything except the reserved bits.
	*/
	IntrMaskReg.WholeReg =0xffffffff;
	IntrMaskReg.Reserved1=0;
	IntrMaskReg.Reserved2=0;
	IntrMaskReg.Reserved3=0;
	IntrMaskReg.Reserved4=0;

	hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_INTR_MSK_SET_REG, IntrMaskReg.WholeReg);

	return;
}

void crystalhd_flea_enable_interrupts(struct crystalhd_hw *hw)
{
	union FLEA_INTR_BITS_COMMON IntrMaskReg;
	/*
	-- Clear The Mask for everything except the reserved bits.
	*/
	IntrMaskReg.WholeReg =0xffffffff;
	IntrMaskReg.Reserved1=0;
	IntrMaskReg.Reserved2=0;
	IntrMaskReg.Reserved3=0;
	IntrMaskReg.Reserved4=0;

	hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_INTR_MSK_CLR_REG, IntrMaskReg.WholeReg);

	return;
}

void crystalhd_flea_clear_interrupts(struct crystalhd_hw *hw)
{
	union FLEA_INTR_BITS_COMMON	IntrStsValue;

	IntrStsValue.WholeReg = hw->pfnReadDevRegister(hw->adp, BCHP_INTR_INTR_STATUS);

	if(IntrStsValue.WholeReg)
	{
		hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_INTR_CLR_REG, IntrStsValue.WholeReg);
		hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_EOI_CTRL, 1);
	}

	return;
}

bool crystalhd_flea_detect_ddr3(struct crystalhd_hw *hw)
{
	uint32_t regVal = 0;

	/*Set the Multiplexer to select the GPIO-6*/
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_SUN_TOP_CTRL_PIN_MUX_CTRL_0);

	/*Make sure that the bits-24:27 are reset*/
	if(regVal & 0x0f000000)
	{
		regVal = regVal & 0xf0ffffff; /*Clear bit 24-27 for selecting GPIO_06*/
		hw->pfnWriteDevRegister(hw->adp, BCHP_SUN_TOP_CTRL_PIN_MUX_CTRL_0, regVal);
	}

	regVal=0;
	/*Set the Direction of GPIO-6*/
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_GIO_IODIR_LO);

	if(!(regVal & BC_BIT(6)))
	{
		/*Set the Bit number 6 to make the GPIO6 as input*/
		regVal |= BC_BIT(6);
		hw->pfnWriteDevRegister(hw->adp, BCHP_GIO_IODIR_LO, regVal);
	}

	regVal=0;
	regVal = hw->pfnReadDevRegister(hw->adp,	BCHP_GIO_DATA_LO);

	/*If this bit is clear then have DDR-3 else we have DDR-2*/
	if(!(regVal & BC_BIT(6)))
	{
		dev_dbg(&hw->adp->pdev->dev,"DDR-3 Detected\n");
		return true;
	}
	dev_dbg(&hw->adp->pdev->dev,"DDR-2 Detected\n");
	return false;
}

void crystalhd_flea_init_dram(struct crystalhd_hw *hw)
{
	int32_t ddr2_speed_grade[2];
	uint32_t sd_0_col_size, sd_0_bank_size, sd_0_row_size;
	uint32_t sd_1_col_size, sd_1_bank_size, sd_1_row_size;
	uint32_t ddr3_mode[2];
	uint32_t regVal;
	bool bDDR3Detected=false; /*Should be filled in using the detection logic. Default to DDR2 */

	/* On all designs we are using DDR2 or DDR3 x16 and running at a max of 400Mhz */
	/* Only one bank of DDR supported. The other is a dummy */

	ddr2_speed_grade[0] = DDR2_400MHZ;
	ddr2_speed_grade[1] = DDR2_400MHZ;
	sd_0_col_size = COL_BITS_10;
	sd_0_bank_size = BANK_SIZE_8;
	sd_0_row_size = ROW_SIZE_8K; /* DDR2 */
	/*	sd_0_row_size = ROW_SIZE_16K; // DDR3 */
	sd_1_col_size = COL_BITS_10;
	sd_1_bank_size = BANK_SIZE_8;
	sd_1_row_size = ROW_SIZE_8K;
	ddr3_mode[0] = 0;
	ddr3_mode[1] = 0;

	bDDR3Detected = crystalhd_flea_detect_ddr3(hw);
	if(bDDR3Detected)
	{
		ddr3_mode[0] = 1;
		sd_0_row_size = ROW_SIZE_16K; /* DDR3 */
		sd_1_row_size = ROW_SIZE_16K; /* DDR3 */

	}

	/* Step 1. PLL Init */
	crystalhd_flea_ddr_pll_config(hw, ddr2_speed_grade, 1, 0); /* only need to configure PLLs in TM0 */

	/* Step 2. DDR CTRL Init */
	crystalhd_flea_ddr_ctrl_init(hw, 0, ddr3_mode[0], ddr2_speed_grade[0], sd_0_col_size, sd_0_bank_size, sd_0_row_size, 0);

	/* Step 3 RTS Init - Real time scheduling memory arbiter */
	crystalhd_flea_ddr_arb_rts_init(hw);

	/* NAREN turn off ODT. The REF1 and SV1 and most customer designs allow this. */
	/* IF SOMEONE COMPLAINS ABOUT MEMORY OR DATA CORRUPTION LOOK HERE FIRST */

	/*hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_LOAD_EMODE_CMD, 0x02, false); */

	/*regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_PARAMS3);
	regVal &= ~(BCHP_DDR23_CTL_REGS_0_PARAMS3_wr_odt_en_MASK);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_PARAMS3, regVal);*/

	/*regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_DRIVE_PAD_CTL);
	regVal |= BCHP_DDR23_PHY_BYTE_LANE_1_DRIVE_PAD_CTL_seltxdrv_ci_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_DRIVE_PAD_CTL, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_DRIVE_PAD_CTL);
	regVal |= BCHP_DDR23_PHY_BYTE_LANE_0_DRIVE_PAD_CTL_seltxdrv_ci_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_DRIVE_PAD_CTL, regVal);*/

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_PAD_DISABLE);
	regVal |= BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_PAD_DISABLE_clk_pad_dis_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_PAD_DISABLE, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_READ_CONTROL);
	regVal &= ~(BCHP_DDR23_PHY_BYTE_LANE_0_READ_CONTROL_dq_odt_enable_MASK);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_READ_CONTROL, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_READ_CONTROL);
	regVal &= ~(BCHP_DDR23_PHY_BYTE_LANE_1_READ_CONTROL_dq_odt_enable_MASK);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_READ_CONTROL, regVal);

	return;
}

uint32_t crystalhd_flea_reg_rd(struct crystalhd_adp *adp, uint32_t reg_off)
{
	uint32_t baseAddr = reg_off >> 16;
	void	*regAddr;

	if (!adp) {
		printk(KERN_ERR "%s: Invalid args\n", __func__);
		return 0;
	}

	if(baseAddr == 0 || baseAddr == FLEA_GISB_DIRECT_BASE) /* Direct Mapped Region */
	{
		regAddr = adp->i2o_addr + (reg_off & 0x0000FFFF);
		if(regAddr > (adp->i2o_addr + adp->pci_i2o_len)) {
			dev_err(&adp->pdev->dev, "%s: reg_off out of range: 0x%08x\n",
					__func__, reg_off);
			return 0;
		}
		return readl(regAddr);
	}
	else /* non directly mapped region */
	{
		if(adp->pci_i2o_len < 0xFFFF) {
			printk("Un-expected mapped region size\n");
			return 0;
		}
		regAddr = adp->i2o_addr + FLEA_GISB_INDIRECT_ADDRESS;
		writel(reg_off | 0x10000000, regAddr);
		regAddr = adp->i2o_addr + FLEA_GISB_INDIRECT_DATA;
		return readl(regAddr);
	}
}

void crystalhd_flea_reg_wr(struct crystalhd_adp *adp, uint32_t reg_off, uint32_t val)
{
	uint32_t baseAddr = reg_off >> 16;
	void	*regAddr;

	if (!adp) {
		printk(KERN_ERR "%s: Invalid args\n", __func__);
		return;
	}

	if(baseAddr == 0 || baseAddr == FLEA_GISB_DIRECT_BASE) /* Direct Mapped Region */
	{
		regAddr = adp->i2o_addr + (reg_off & 0x0000FFFF);
		if(regAddr > (adp->i2o_addr + adp->pci_i2o_len)) {
			dev_err(&adp->pdev->dev, "%s: reg_off out of range: 0x%08x\n",
					__func__, reg_off);
					return ;
		}
		writel(val, regAddr);
	}
	else /* non directly mapped region */
	{
		if(adp->pci_i2o_len < 0xFFFF) {
			printk("Un-expected mapped region size\n");
			return;
		}
		regAddr = adp->i2o_addr + FLEA_GISB_INDIRECT_ADDRESS;
		writel(reg_off | 0x10000000, regAddr);
		regAddr = adp->i2o_addr + FLEA_GISB_INDIRECT_DATA;
		writel(val, regAddr);
	}
}

/**
* crystalhd_flea_mem_rd - Read data from DRAM area.
* @adp: Adapter instance
* @start_off: Start offset.
* @dw_cnt: Count in dwords.
* @rd_buff: Buffer to copy the data from dram.
*
* Return:
*	Status.
*
* Dram read routine.
*/
BC_STATUS crystalhd_flea_mem_rd(struct crystalhd_hw *hw, uint32_t start_off,
								uint32_t dw_cnt, uint32_t *rd_buff)
{
	uint32_t ix = 0;
	uint32_t addr = start_off, base;

	if (!hw || !rd_buff) {
		printk(KERN_ERR "%s: Invalid arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	if( hw->FleaPowerState == FLEA_PS_LP_COMPLETE ) {
		/*printk(KERN_ERR "%s: Flea power down, cann't read memory.\n", __func__); */
		return BC_STS_BUSY;
	}

	if((start_off + dw_cnt * 4) > FLEA_TOTAL_DRAM_SIZE) {
		printk(KERN_ERR "Access beyond DRAM limit at Addr 0x%x and size 0x%x words\n", start_off, dw_cnt);
		return BC_STS_ERROR;
	}

	/* Set the base addr for the 512kb window */
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC2_DIRECT_WINDOW_CONTROL,
							(addr & BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK) | BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_ENABLE_MASK);

	for (ix = 0; ix < dw_cnt; ix++) {
		rd_buff[ix] = readl(hw->adp->mem_addr + (addr & ~BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK));
		base = addr & BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK;
		addr += 4; /* DWORD access at all times */
		if (base != (addr & BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK)) {
			/* Set the base addr for next 512kb window */
			hw->pfnWriteDevRegister(hw->adp, BCHP_MISC2_DIRECT_WINDOW_CONTROL,
										(addr & BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK) | BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_ENABLE_MASK);
		}
	}
	return BC_STS_SUCCESS;
}

/**
* crystalhd_flea_mem_wr - Write data to DRAM area.
* @adp: Adapter instance
* @start_off: Start offset.
* @dw_cnt: Count in dwords.
* @wr_buff: Data Buffer to be written.
*
* Return:
*	Status.
*
* Dram write routine.
*/
BC_STATUS crystalhd_flea_mem_wr(struct crystalhd_hw *hw, uint32_t start_off,
								uint32_t dw_cnt, uint32_t *wr_buff)
{
	uint32_t ix = 0;
	uint32_t addr = start_off, base;
	uint32_t temp;

	if (!hw || !wr_buff) {
		printk(KERN_ERR "%s: Invalid arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	if( hw->FleaPowerState == FLEA_PS_LP_COMPLETE ) {
		/*printk(KERN_ERR "%s: Flea power down, cann't write memory.\n", __func__); */
		return BC_STS_BUSY;
	}

	if((start_off + dw_cnt * 4) > FLEA_TOTAL_DRAM_SIZE) {
		printk("Access beyond DRAM limit at Addr 0x%x and size 0x%x words\n", start_off, dw_cnt);
		return BC_STS_ERROR;
	}

	/* Set the base addr for the 512kb window */
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC2_DIRECT_WINDOW_CONTROL,
							(addr & BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK) | BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_ENABLE_MASK);

	for (ix = 0; ix < dw_cnt; ix++) {
		writel(wr_buff[ix], hw->adp->mem_addr + (addr & ~BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK));
		base = addr & BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK;
		addr += 4; /* DWORD access at all times */
		if (base != (addr & BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK)) {
			/* Set the base addr for next 512kb window */
			hw->pfnWriteDevRegister(hw->adp, BCHP_MISC2_DIRECT_WINDOW_CONTROL,
									(addr & BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK) | BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_ENABLE_MASK);
		}
	}

	/*Dummy Read To Flush Memory Arbitrator*/
	crystalhd_flea_mem_rd(hw, start_off, 1, &temp);
	return BC_STS_SUCCESS;
}


static
void crystalhd_flea_runtime_power_up(struct crystalhd_hw *hw)
{
	uint32_t regVal;
	uint64_t currTick;
	uint32_t totalTick_Hi;
	uint32_t TickSpentInPD_Hi;
	uint64_t temp_64;
	long totalTick_Hi_f;
	long TickSpentInPD_Hi_f;

	/*printk("RT PU \n"); */

	/* NAREN This function restores clocks and power to the DRAM and to the core to bring the decoder back up to full operation */
	/* Start the DRAM controller clocks first */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_CLK_PM_CTRL);
	regVal &= ~(BCHP_CLK_PM_CTRL_DIS_DDR_108_CLK_MASK |	BCHP_CLK_PM_CTRL_DIS_DDR_216_CLK_MASK);
	hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_PM_CTRL, regVal);

	/* Delay to allow the DRAM clock to stabilize */
	udelay(25);

	/* Power Up PHY and start clocks on DRAM device */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL);
	regVal &= ~(BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL_pwrdn_MASK);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG);
	regVal &= ~(BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG_PWRDN_MASK);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG, regVal);

	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_CLK_PM_CTRL,
		~(BCHP_DDR23_PHY_CONTROL_REGS_CLK_PM_CTRL_DIS_DDR_CLK_MASK));

	/* Delay to allow the PLL to lock */
	udelay(25);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL);
	regVal &= ~(BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_idle_MASK );
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL);
	regVal &= ~(BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL_pwrdn_MASK);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL);
	regVal &= ~(BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL_pwrdn_MASK);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL);
	regVal &= ~(BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_idle_MASK );
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL);
	regVal &= ~(BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_idle_MASK );
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL, regVal);

	/* Start Refresh Cycles from controller */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0);
	regVal |= BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0_enable_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0, regVal);

	/* turn off self-refresh */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_PARAMS2);
	regVal &= ~(BCHP_DDR23_CTL_REGS_0_PARAMS2_clke_MASK);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_PARAMS2, regVal);

	udelay(5);

	/* Issue refresh cycle */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_REFRESH_CMD, 0x60);

	/* Enable the ARM AVD and BLINK clocks */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_CLK_PM_CTRL);

	regVal &= ~(BCHP_CLK_PM_CTRL_DIS_ARM_CLK_MASK |
				BCHP_CLK_PM_CTRL_DIS_AVD_CLK_MASK |
				BCHP_CLK_PM_CTRL_DIS_BLINK_108_CLK_MASK |
				BCHP_CLK_PM_CTRL_DIS_AVD_108_CLK_MASK |
				BCHP_CLK_PM_CTRL_DIS_BLINK_216_CLK_MASK |
				BCHP_CLK_PM_CTRL_DIS_AVD_216_CLK_MASK);

	hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_PM_CTRL, 0x03000000);

	/* Start arbiter */
	hw->pfnWriteDevRegister(hw->adp, BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL, BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL_arb_disable_Enable);

#ifdef _POWER_HANDLE_AVD_WATCHDOG_
	/* Restore Watchdog timers */
	/* Make sure the timeouts do not happen */
	/*Outer Loop Watchdog timer */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DECODE_CPUREGS_0_REG_WATCHDOG_TMR, hw->OLWatchDogTimer);

	/*//Inner Loop Watchdog timer */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DECODE_CPUREGS2_0_REG_WATCHDOG_TMR, hw->ILWatchDogTimer);

#endif

	/*printk("RT Power Up Flea Complete\n"); */

	rdtscll(currTick);

	hw->TickSpentInPD += (currTick - hw->TickStartInPD);

	temp_64 = (hw->TickSpentInPD)>>24;
	TickSpentInPD_Hi = (uint32_t)(temp_64);
	TickSpentInPD_Hi_f = (long)TickSpentInPD_Hi;

	temp_64 = (currTick - hw->TickCntDecodePU)>>24;
	totalTick_Hi = (uint32_t)(temp_64);
	totalTick_Hi_f = (long)totalTick_Hi;

	if( totalTick_Hi_f <= 0 )
	{
		temp_64 = (hw->TickSpentInPD);
		TickSpentInPD_Hi = (uint32_t)(temp_64);
		TickSpentInPD_Hi_f = (long)TickSpentInPD_Hi;

		temp_64 = (currTick - hw->TickCntDecodePU);
		totalTick_Hi = (uint32_t)(temp_64);
		totalTick_Hi_f = (long)totalTick_Hi;
	}

	if( totalTick_Hi_f <= 0 )
	{
		printk("totalTick_Hi_f <= 0, set hw->PDRatio = 60\n");
		hw->PDRatio = 60;
	}
	else
		hw->PDRatio = (TickSpentInPD_Hi_f * 100) / totalTick_Hi_f;

	/*printk("Ticks currently spent in PD: 0x%llx Total: 0x%llx Ratio %d,\n", */
	/*	hw->TickSpentInPD, (currTick - hw->TickCntDecodePU), hw->PDRatio); */

	/* NAREN check if the PD ratio is greater than 75. If so, try to increase the PauseThreshold to improve the ratio */
	/* never go higher than the default threshold */
	if((hw->PDRatio > 75) && (hw->PauseThreshold < hw->DefaultPauseThreshold))
	{
		/*printk("Current PDRatio:%u, PauseThreshold:%u, DefaultPauseThreshold:%u, incress PauseThreshold.\n", */
		/*		hw->PDRatio, hw->PauseThreshold, hw->DefaultPauseThreshold); */
		hw->PauseThreshold++;
	}
	else
	{
		/*printk("Current PDRatio:%u, PauseThreshold:%u, DefaultPauseThreshold:%u, don't incress PauseThreshold.\n", */
		/*		hw->PDRatio, hw->PauseThreshold, hw->DefaultPauseThreshold); */
	}

	return;
}

static
void crystalhd_flea_runtime_power_dn(struct crystalhd_hw *hw)
{
	uint32_t regVal;
	uint32_t pollCnt;

	/*printk("RT PD \n"); */

	hw->DrvPauseCnt++;

	/* NAREN This function stops the decoder clocks including the AVD, ARM and DRAM */
	/* It powers down the DRAM device and places the DRAM into self-refresh */

#ifdef _POWER_HANDLE_AVD_WATCHDOG_
	/* Make sure the timeouts do not happen */
	/* Because the AVD drops to a debug prompt and stops decoding if it hits any watchdogs */
	/*Outer Loop Watchdog timer */
	regVal = hw->pfnReadDevRegister(hw->adp,
		BCHP_DECODE_CPUREGS_0_REG_WATCHDOG_TMR);

	hw->OLWatchDogTimer = regVal;
	hw->pfnWriteDevRegister(hw->adp,
		BCHP_DECODE_CPUREGS_0_REG_WATCHDOG_TMR,
		0xffffffff);

	/*Inner Loop Watchdog timer */
	regVal = hw->pfnReadDevRegister(hw->adp,
		BCHP_DECODE_CPUREGS2_0_REG_WATCHDOG_TMR);

	hw->ILWatchDogTimer = regVal;
	hw->pfnWriteDevRegister(hw->adp,
		BCHP_DECODE_CPUREGS2_0_REG_WATCHDOG_TMR,
		0xffffffff);
#else
	regVal = 0;
#endif

	/* Stop memory arbiter first to freese memory access */
	hw->pfnWriteDevRegister(hw->adp, BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL, BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL_arb_disable_Disable);

	/* delay at least 15us for memory transactions to complete */
	/* udelay(15); */

	/* Wait for MEMC to become idle. Continue even if we are no since worst case this would just mean higher power consumption */
	pollCnt=0;
	while (pollCnt++ <= 400) /*200 */
	{
		regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_CTL_STATUS);

		if(regVal & BCHP_DDR23_CTL_REGS_0_CTL_STATUS_idle_MASK)
		{
			/* udelay(10); */
			break;
		}
		udelay(10);
	}

	/*If we failed Start the arbiter and return*/
	if(!(regVal & BCHP_DDR23_CTL_REGS_0_CTL_STATUS_idle_MASK))
	{
		printk("RT PD : failed Start the arbiter and return.\n");
		hw->pfnWriteDevRegister(hw->adp,
			BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL,
			BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL_arb_disable_Enable);
		return;
	}

	/* Disable the AVD, ARM and BLINK clocks*/
	/*regVal = hw->pfnReadDevRegister(hw->adp, BCHP_CLK_PM_CTRL);

	regVal |=	BCHP_CLK_PM_CTRL_DIS_ARM_CLK_MASK |
				BCHP_CLK_PM_CTRL_DIS_AVD_CLK_MASK |
				BCHP_CLK_PM_CTRL_DIS_BLINK_108_CLK_MASK |
				BCHP_CLK_PM_CTRL_DIS_AVD_108_CLK_MASK |
				BCHP_CLK_PM_CTRL_DIS_BLINK_216_CLK_MASK |
				BCHP_CLK_PM_CTRL_DIS_AVD_216_CLK_MASK;

	hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_PM_CTRL, regValE);*/

	/* turn on self-refresh */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_PARAMS2);
	regVal |= BCHP_DDR23_CTL_REGS_0_PARAMS2_clke_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_PARAMS2, regVal);

	/* Issue refresh cycle */
	hw->pfnWriteDevRegister(hw->adp,	BCHP_DDR23_CTL_REGS_0_REFRESH_CMD, 0x60);

	/* Stop Refresh Cycles from controller */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0);
	regVal &= ~(BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0_enable_MASK);
	hw->pfnWriteDevRegister(hw->adp, BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0, regVal);

	/* Check if we are in self-refresh. Continue even if we are no since worst case this would just mean higher power consumption */
	pollCnt=0;
	while(pollCnt++ < 100)
	{
		regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_CTL_STATUS);

		if(!(regVal & BCHP_DDR23_CTL_REGS_0_CTL_STATUS_clke_MASK))
			break;
	}

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_CLK_PM_CTRL);

	regVal |=	BCHP_CLK_PM_CTRL_DIS_ARM_CLK_MASK |
				BCHP_CLK_PM_CTRL_DIS_AVD_CLK_MASK |
				BCHP_CLK_PM_CTRL_DIS_BLINK_108_CLK_MASK |
				BCHP_CLK_PM_CTRL_DIS_AVD_108_CLK_MASK |
				BCHP_CLK_PM_CTRL_DIS_BLINK_216_CLK_MASK |
				BCHP_CLK_PM_CTRL_DIS_AVD_216_CLK_MASK;

	hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_PM_CTRL, regVal);

	/* Power down PHY and stop clocks on DRAM */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL);

	regVal |=	BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_idle_MASK |
				BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_dm_iddq_MASK |
				BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_dq_iddq_MASK |
				BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_read_enb_iddq_MASK |
				BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_dqs_iddq_MASK |
				BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_clk_iddq_MASK;

	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL);

	regVal |=	BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_idle_MASK |
				BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_dm_iddq_MASK |
				BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_dq_iddq_MASK |
				BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_read_enb_iddq_MASK |
				BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_dqs_iddq_MASK |
				BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_clk_iddq_MASK;

	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL);
	regVal |= BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL_pwrdn_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL);
	regVal |= BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL_pwrdn_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL);
	regVal |=	BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_idle_MASK |
				BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_ctl_iddq_MASK |
				BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_rxenb_MASK |
				BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_cke_reb_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL, regVal);

	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_CLK_PM_CTRL, BCHP_DDR23_PHY_CONTROL_REGS_CLK_PM_CTRL_DIS_DDR_CLK_MASK);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG);
	regVal |= BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG_PWRDN_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL);
	regVal |= BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL_pwrdn_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL, regVal);

	/* Finally clock off the DRAM controller */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_CLK_PM_CTRL);
	regVal |=	BCHP_CLK_PM_CTRL_DIS_DDR_108_CLK_MASK |	BCHP_CLK_PM_CTRL_DIS_DDR_216_CLK_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_PM_CTRL, regVal);

	/* udelay(20); */

	/*printk("RT Power Down Flea Complete\n"); */

	/* Measure how much time we spend in idle */
	rdtscll(hw->TickStartInPD);

	return;
}

bool crystalhd_flea_detect_fw_alive(struct crystalhd_hw *hw)
{
	uint32_t pollCnt		= 0;
	uint32_t hbCnt			= 0;
	uint32_t heartBeatReg1	= 0;
	uint32_t heartBeatReg2	= 0;
	bool	 bRetVal		= false;

	heartBeatReg1 = hw->pfnReadDevRegister(hw->adp, HEART_BEAT_REGISTER);
	while(1)
	{
		heartBeatReg2 = hw->pfnReadDevRegister(hw->adp, HEART_BEAT_REGISTER);
		if(heartBeatReg1 != heartBeatReg2) {
			hbCnt++;
			heartBeatReg1 = heartBeatReg2;
		}

		if(hbCnt >= HEART_BEAT_POLL_CNT) {
			bRetVal = true;
			break;
		}

		pollCnt++;
		if(pollCnt >= FLEA_MAX_POLL_CNT) {
			bRetVal = false;
			break;
		}

		msleep_interruptible(1);
	}

	return bRetVal;
}

void crystalhd_flea_handle_PicQSts_intr(struct crystalhd_hw *hw)
{
	uint32_t	newChBitmap=0;

	newChBitmap = hw->pfnReadDevRegister(hw->adp, RX_DMA_PIC_QSTS_MBOX);

	hw->PicQSts = newChBitmap;

	/* -- For link we were enabling the capture on format change
	   -- For Flea, we will get a PicQSts interrupt where we will
	   -- enable the capture. */

	if(hw->RxCaptureState != 1)
	{
		hw->RxCaptureState = 1;
	}
}

void crystalhd_flea_update_tx_buff_info(struct crystalhd_hw *hw)
{
	TX_INPUT_BUFFER_INFO	TxBuffInfo;
	uint32_t ReadSzInDWords=0;

	ReadSzInDWords = (sizeof(TxBuffInfo) - sizeof(TxBuffInfo.Reserved))/4;
	hw->pfnDevDRAMRead(hw, hw->TxBuffInfoAddr, ReadSzInDWords, (uint32_t*)&TxBuffInfo);

	if(TxBuffInfo.DramBuffAdd % 4)
	{
		printk("Tx Err:: DWORD UNAligned Tx Addr. Not Updating\n");
		return;
	}

	hw->TxFwInputBuffInfo.DramBuffAdd			= TxBuffInfo.DramBuffAdd;
	hw->TxFwInputBuffInfo.DramBuffSzInBytes		= TxBuffInfo.DramBuffSzInBytes;
	hw->TxFwInputBuffInfo.Flags					= TxBuffInfo.Flags;
	hw->TxFwInputBuffInfo.HostXferSzInBytes		= TxBuffInfo.HostXferSzInBytes;
	hw->TxFwInputBuffInfo.SeqNum				= TxBuffInfo.SeqNum;

	return;
}

/* was HWFleaNotifyFllChange */
void crystalhd_flea_notify_fll_change(struct crystalhd_hw *hw, bool bCleanupContext)
{
	unsigned long flags = 0;
	uint32_t freeListLen = 0;
	/*
	* When we are doing the cleanup we should update DRAM only if the
	* firmware is running. So Detect the heart beat.
	*/
	if(bCleanupContext && (!crystalhd_flea_detect_fw_alive(hw)))
		return;

	spin_lock_irqsave(&hw->lock, flags);
	freeListLen = crystalhd_dioq_count(hw->rx_freeq);
	hw->pfnDevDRAMWrite(hw, hw->FleaFLLUpdateAddr, 1, &freeListLen);
	spin_unlock_irqrestore(&hw->lock, flags);

	return;
}


static
void crystalhd_flea_init_power_state(struct crystalhd_hw *hw)
{
	hw->FleaEnablePWM = false;	/* enable by default */
	hw->FleaPowerState = FLEA_PS_NONE;
}

static
bool crystalhd_flea_set_power_state(struct crystalhd_hw *hw,
					enum FLEA_POWER_STATES	NewState)
{
	bool StChangeSuccess=false;
	uint32_t tempFLL = 0;
	uint32_t freeListLen = 0;
	BC_STATUS sts;
	struct crystalhd_rx_dma_pkt *rx_pkt = NULL;

	freeListLen = crystalhd_dioq_count(hw->rx_freeq);

	switch(NewState)
	{
		case FLEA_PS_ACTIVE:
		{
			/*Transition to Active State*/
			if(hw->FleaPowerState == FLEA_PS_LP_PENDING)
			{
				StChangeSuccess = true;
				hw->FleaPowerState = FLEA_PS_ACTIVE;
				/* Write the correct FLL to FW */
				hw->pfnDevDRAMWrite(hw,
									hw->FleaFLLUpdateAddr,
									1,
									&freeListLen);
				/* We need to check to post here because we may never get a context to post otherwise */
				if(hw->PicQSts != 0)
				{
					rx_pkt = crystalhd_dioq_fetch(hw->rx_freeq);
					if (rx_pkt)
						sts = hw->pfnPostRxSideBuff(hw, rx_pkt);
				}
				/*printk(" Success\n"); */

			}else if(hw->FleaPowerState == FLEA_PS_LP_COMPLETE){
				crystalhd_flea_runtime_power_up(hw);
				StChangeSuccess = true;
				hw->FleaPowerState = FLEA_PS_ACTIVE;
				/* Write the correct FLL to FW */
				hw->pfnDevDRAMWrite(hw,
										hw->FleaFLLUpdateAddr,
										1,
										&freeListLen);
				/* Now check if we missed processing PiQ and TXFIFO interrupts when we were in power down */
				if (hw->PwrDwnPiQIntr)
				{
					crystalhd_flea_handle_PicQSts_intr(hw);
					hw->PwrDwnPiQIntr = false;
				}
				/* We need to check to post here because we may never get a context to post otherwise */
				if(hw->PicQSts != 0)
				{
					rx_pkt = crystalhd_dioq_fetch(hw->rx_freeq);
					if (rx_pkt)
						sts = hw->pfnPostRxSideBuff(hw, rx_pkt);
				}
				if (hw->PwrDwnTxIntr)
				{
					crystalhd_flea_update_tx_buff_info(hw);
					hw->PwrDwnTxIntr = false;
				}

			}
			break;
		}

		case FLEA_PS_LP_PENDING:
		{
			if(hw->FleaPowerState != FLEA_PS_ACTIVE)
			{
				break;
			}

			/*printk(" Success\n"); */

			StChangeSuccess = true;
			/* Write 0 FLL to FW to prevent it from sending PQ*/
			hw->pfnDevDRAMWrite(hw,
									hw->FleaFLLUpdateAddr,
									1,
									&tempFLL);
			hw->FleaPowerState = FLEA_PS_LP_PENDING;
			break;
		}

		case FLEA_PS_LP_COMPLETE:
		{
			if( (hw->FleaPowerState == FLEA_PS_ACTIVE) ||
			    (hw->FleaPowerState == FLEA_PS_LP_PENDING)) {
				/* Write 0 FLL to FW to prevent it from sending PQ*/
				hw->pfnDevDRAMWrite(hw,
									hw->FleaFLLUpdateAddr,
									1,
									&tempFLL);
				crystalhd_flea_runtime_power_dn(hw);
				StChangeSuccess = true;
				hw->FleaPowerState = FLEA_PS_LP_COMPLETE;

			}
			break;
		}
		default:
			break;
	}

	return StChangeSuccess;
}

/*
* Look At Different States and List Status and decide on
* Next Logical State To Be In.
*/
static
void crystalhd_flea_set_next_power_state(struct crystalhd_hw *hw,
						enum FLEA_STATE_CH_EVENT		PowerEvt)
{
	enum FLEA_POWER_STATES NextPS;
	NextPS = hw->FleaPowerState;

	if( hw->FleaEnablePWM == false )
	{
		hw->FleaPowerState = FLEA_PS_ACTIVE;
		return;
	}

/*	printk("Trying Power State Transition from %x Because Of Event:%d \n", */
/*			hw->FleaPowerState, */
/*			PowerEvt); */

	if(PowerEvt == FLEA_EVT_STOP_DEVICE)
	{
		hw->FleaPowerState = FLEA_PS_STOPPED;
		return;
	}

	if(PowerEvt == FLEA_EVT_START_DEVICE)
	{
		hw->FleaPowerState = FLEA_PS_ACTIVE;
		return;
	}

	switch(hw->FleaPowerState)
	{
		case FLEA_PS_ACTIVE:
		{
			if(PowerEvt == FLEA_EVT_FLL_CHANGE)
			{
				/*Ready List Was Decremented. */
				/*printk("1:TxL0Sts:%x TxL1Sts:%x EmptyCnt:%x RxL0Sts:%x RxL1Sts:%x FwCmdCnt:%x\n", */
				/*	hw->TxList0Sts, */
				/*	hw->TxList1Sts, */
				/*	hw->EmptyCnt, */
				/*	hw->rx_list_sts[0], */
				/*	hw->rx_list_sts[1], */
				/*	hw->FwCmdCnt); */

				if( (hw->TxList0Sts == ListStsFree)	&&
					(hw->TxList1Sts == ListStsFree) &&
					(!hw->EmptyCnt) && /*We have Not Indicated Any Empty Fifo to Application*/
					(!hw->SingleThreadAppFIFOEmpty) && /*for single threaded apps*/
					(!(hw->rx_list_sts[0] && rx_waiting_y_intr)) &&
					(!(hw->rx_list_sts[1] && rx_waiting_y_intr)) &&
					(!hw->FwCmdCnt))
				{
					NextPS = FLEA_PS_LP_COMPLETE;
				}else{
					NextPS = FLEA_PS_LP_PENDING;
				}
			}

			break;
		}

		case FLEA_PS_LP_PENDING:
		{
			if( (PowerEvt == FLEA_EVT_FW_CMD_POST) ||
				(PowerEvt == FLEA_EVT_FLL_CHANGE))
			{
				NextPS = FLEA_PS_ACTIVE;
			}else if(PowerEvt == FLEA_EVT_CMD_COMP){

				/*printk("2:TxL0Sts:%x TxL1Sts:%x EmptyCnt:%x STAppFIFOEmpty:%x RxL0Sts:%x RxL1Sts:%x FwCmdCnt:%x\n", */
				/*	hw->TxList0Sts, */
				/*	hw->TxList1Sts, */
				/*	hw->EmptyCnt, */
				/*	hw->SingleThreadAppFIFOEmpty, */
				/*	hw->rx_list_sts[0], */
				/*	hw->rx_list_sts[1], */
				/*	hw->FwCmdCnt); */

				if( (hw->TxList0Sts == ListStsFree)	&&
					(hw->TxList1Sts == ListStsFree) &&
					(!hw->EmptyCnt) &&	/*We have Not Indicated Any Empty Fifo to Application*/
					(!hw->SingleThreadAppFIFOEmpty) && /*for single threaded apps*/
					(!(hw->rx_list_sts[0] && rx_waiting_y_intr)) &&
					(!(hw->rx_list_sts[1] && rx_waiting_y_intr)) &&
					(!hw->FwCmdCnt))
				{
					NextPS = FLEA_PS_LP_COMPLETE;
				}
			}
			break;
		}
		case FLEA_PS_LP_COMPLETE:
		{
			if( (PowerEvt == FLEA_EVT_FLL_CHANGE) ||
				(PowerEvt == FLEA_EVT_FW_CMD_POST))
			{
				NextPS = FLEA_PS_ACTIVE;
			}

			break;
		}
		default:
		{
			printk("Invalid Flea Power State %x\n",
				hw->FleaPowerState);

			break;
		}
	}

	if(hw->FleaPowerState != NextPS)
	{
		printk("%s:State Transition [FromSt:%x ToSt:%x] Because Of Event:%d \n",
			__FUNCTION__,
			hw->FleaPowerState,
			NextPS,
			PowerEvt);

		crystalhd_flea_set_power_state(hw,NextPS);
	}

	return;
}

/* was FleaSetRxPicFireAddr */
#if 0
static
void crystalhd_flea_set_rx_pic_fire_addr(struct crystalhd_hw *hw, uint32_t BorshContents)
{
	hw->FleaRxPicDelAddr = BorshContents + 1 + HOST_TO_FW_PIC_DEL_INFO_ADDR;
	hw->FleaFLLUpdateAddr = BorshContents + 1 + HOST_TO_FW_FLL_ADDR;

	return;
}
#endif

void crystalhd_flea_init_temperature_measure (struct crystalhd_hw *hw, bool bTurnOn)
{
	hw->TemperatureRegVal=0;

	if(bTurnOn) {
		hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_TEMP_MON_CTRL, 0x3);
		hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_TEMP_MON_CTRL, 0x203);
	} else {
		hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_TEMP_MON_CTRL, 0x103);
	}

	return;
}

/* was HwFleaUpdateTempInfo */
void crystalhd_flea_update_temperature(struct crystalhd_hw *hw)
{
	uint32_t	regVal = 0;

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_CLK_TEMP_MON_STATUS);
	hw->TemperatureRegVal = regVal;

	return;
}

/**
* crystalhd_flea_download_fw - Write data to DRAM area.
* @adp: Adapter instance
* @pBuffer: Buffer pointer for the FW data.
* @buffSz: data size in bytes.
*
* Return:
*	Status.
*
* Flea firmware download routine.
*/
BC_STATUS crystalhd_flea_download_fw(struct crystalhd_hw *hw, uint8_t *pBuffer, uint32_t buffSz)
{
	uint32_t pollCnt=0,regVal=0;
	uint32_t borchStachAddr=0;
	uint32_t *pCmacSig=NULL,cmacOffset=0,i=0;
	/*uint32_t BuffSz = (BuffSzInDWords * 4); */
	/*uint32_t HBCnt=0; */

	bool bRetVal = true;
	bool bSecure = true; // Default production cards. Can be false only for internal Broadcom dev cards

	dev_dbg(&hw->adp->pdev->dev, "[%s]: Sz:%d\n", __func__, buffSz);

/*
 *-- Step 1. Enable the SRCUBBING and DRAM SCRAMBLING
 *-- Step 2. Poll for SCRAM_KEY_DONE_INT.
 *-- Step 3. Write the BORCH and STARCH addresses.
 *-- Step 4. Write the firmware to DRAM.
 *-- Step 5. Write the CMAC to SCRUB->CMAC registers.
 *-- Step 6. Write the ARM run bit to 1.
 *-- Step 7. Poll for BOOT verification done interrupt.
 */

	/* First validate that we got data in the FW buffer */
	if (buffSz == 0)
		return BC_STS_ERROR;

/*-- Step 1. Enable the SRCUBBING and DRAM SCRAMBLING. */
/*   Can we set both the bits at the same time?? Security Arch Doc describes the steps */
/*   and the first step is to enable scrubbing and then scrambling. */

	if(bSecure)
	{
		dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 1. Enable scrubbing\n");

		/* Enable Scrubbing */
		regVal = hw->pfnReadDevRegister(hw->adp, BCHP_SCRUB_CTRL_SCRUB_ENABLE);
		regVal |= SCRUB_ENABLE_BIT;
		hw->pfnWriteDevRegister(hw->adp, BCHP_SCRUB_CTRL_SCRUB_ENABLE, regVal);

		/* Enable Scrambling */
		regVal |= DRAM_SCRAM_ENABLE_BIT;
		hw->pfnWriteDevRegister(hw->adp, BCHP_SCRUB_CTRL_SCRUB_ENABLE, regVal);


		//-- Step 2. Poll for SCRAM_KEY_DONE_INT.
		dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 2. Poll for SCRAM_KEY_DONE_INT\n");

		pollCnt=0;
		while(pollCnt < FLEA_MAX_POLL_CNT)
		{
			regVal = hw->pfnReadDevRegister(hw->adp, BCHP_WRAP_MISC_INTR2_PCI_STATUS);

			if(regVal & SCRAM_KEY_DONE_INT_BIT)
				break;

			pollCnt++;
			msleep_interruptible(1); /*1 Milli Sec delay*/
		}

		/* -- Will Assert when we do not see SCRAM_KEY_DONE_INTERRUPT */
		if(!(regVal & SCRAM_KEY_DONE_INT_BIT))
		{
			dev_err(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 2. Did not get scram key done interrupt.\n");
			return BC_STS_ERROR;
		}
	}

	/*Clear the interrupts by writing the register value back*/
	regVal &= 0x00FFFFFF; /*Mask off the reserved bits.[24-31] */
	hw->pfnWriteDevRegister(hw->adp, BCHP_WRAP_MISC_INTR2_PCI_CLEAR, regVal);

/*-- Step 3. Write the BORCH and STARCH addresses. */
	borchStachAddr = GetScrubEndAddr(buffSz);
	if(!bSecure)
		borchStachAddr = (buffSz - 1) & BCHP_SCRUB_CTRL_BORCH_END_ADDRESS_BORCH_END_ADDR_MASK;

	hw->pfnWriteDevRegister(hw->adp, BCHP_SCRUB_CTRL_BORCH_END_ADDRESS, borchStachAddr);
	hw->pfnWriteDevRegister(hw->adp, BCHP_SCRUB_CTRL_STARCH_END_ADDRESS, borchStachAddr);

	/*
	 *	Now the command address is
	 *	relative to firmware file size.
	 */
	/*FWIFSetFleaCmdAddr(pHWExt->pFwExt, */
	/*		borchStachAddr+1+DDRADDR_4_FWCMDS); */

	hw->fwcmdPostAddr = borchStachAddr+1+DDRADDR_4_FWCMDS;
	hw->fwcmdPostMbox = FW_CMD_POST_MBOX;
	hw->fwcmdRespMbox = FW_CMD_RES_MBOX;
	/*FleaSetRxPicFireAddr(pHWExt,borchStachAddr); */
	hw->FleaRxPicDelAddr = borchStachAddr + 1 + HOST_TO_FW_PIC_DEL_INFO_ADDR;
	hw->FleaFLLUpdateAddr = borchStachAddr + 1 + HOST_TO_FW_FLL_ADDR;

	dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 3. Write the BORCH and STARCH addresses. %x:%x, %x:%x\n",
			BCHP_SCRUB_CTRL_BORCH_END_ADDRESS,
			borchStachAddr,
			BCHP_SCRUB_CTRL_STARCH_END_ADDRESS,
			borchStachAddr );

/*-- Step 4. Write the firmware to DRAM. [Without the Signature, 32-bit access to DRAM] */

	dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 4. Write the firmware to DRAM. Sz:%d Bytes\n",
			buffSz - FLEA_FW_SIG_LEN_IN_BYTES - LENGTH_FIELD_SIZE);

	if(bSecure)
		hw->pfnDevDRAMWrite(hw, FW_DOWNLOAD_START_ADDR, (buffSz - FLEA_FW_SIG_LEN_IN_BYTES - LENGTH_FIELD_SIZE)/4, (uint32_t *)pBuffer);
	else
		hw->pfnDevDRAMWrite(hw, FW_DOWNLOAD_START_ADDR, buffSz/4, (uint32_t*)pBuffer);

/* -- Step 5. Write the signature to CMAC register. */
/*
-- This is what we need to write to CMAC registers.
==================================================================================
Register							Offset				Boot Image CMAC
														Value
==================================================================================
BCHP_SCRUB_CTRL_BI_CMAC_31_0		0x000f600c			CMAC Bits[31:0]
BCHP_SCRUB_CTRL_BI_CMAC_63_32		0x000f6010			CMAC Bits[63:32]
BCHP_SCRUB_CTRL_BI_CMAC_95_64		0x000f6014			CMAC Bits[95:64]
BCHP_SCRUB_CTRL_BI_CMAC_127_96		0x000f6018			CMAC Bits[127:96]
==================================================================================
*/

	if(bSecure)
	{
		dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 5. Write the signature to CMAC register.\n");
		cmacOffset = buffSz - FLEA_FW_SIG_LEN_IN_BYTES;
		pCmacSig = (uint32_t *) &pBuffer[cmacOffset];

		for(i=0;i < FLEA_FW_SIG_LEN_IN_DWORD;i++)
		{
			uint32_t offSet = (BCHP_SCRUB_CTRL_BI_CMAC_127_96 - (i * 4));

			hw->pfnWriteDevRegister(hw->adp, offSet, cpu_to_be32(*pCmacSig));

			pCmacSig++;
		}
	}

/*-- Step 6. Write the ARM run bit to 1. */
/*   We need a write back because we do not want to change other bits */
	dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 6. Write the ARM run bit to 1.\n");

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_ARMCR4_BRIDGE_REG_BRIDGE_CTL);
	regVal |= ARM_RUN_REQ_BIT;
	hw->pfnWriteDevRegister(hw->adp, BCHP_ARMCR4_BRIDGE_REG_BRIDGE_CTL, regVal);

	if(bSecure)
	{
		/* -- Step 7. Poll for Boot Verification done/failure interrupt.*/
		dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 7. Poll for Boot Verification done/failure interrupt.\n");
		pollCnt=0;
		while(1)
		{
			regVal = hw->pfnReadDevRegister(hw->adp, BCHP_WRAP_MISC_INTR2_PCI_STATUS);

			if(regVal & BOOT_VER_FAIL_BIT )
			{
				dev_err(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 7. Error bit occured. RetVal:%x\n", regVal);

				bRetVal = false;
				break;
			}

			if(regVal & BOOT_VER_DONE_BIT)
			{
				dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 7. Done  RetVal:%x\n", regVal);

				bRetVal = true; /*This is the only place we return TRUE from*/
				break;
			}

			pollCnt++;
			if( pollCnt >= FLEA_MAX_POLL_CNT )
			{
				dev_err(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 7. Both done and failure bits are not set.\n");
				bRetVal = false;
				break;
			}

			msleep_interruptible(5); /*5 Milli Sec delay*/
		}

		if( !bRetVal )
		{
			dev_info(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 7. Firmware image signature failure.\n");
			return BC_STS_ERROR;
		}

		/*Clear the interrupts by writing the register value back*/
		regVal &= 0x00FFFFFF; //Mask off the reserved bits.[24-31]
		hw->pfnWriteDevRegister(hw->adp, BCHP_WRAP_MISC_INTR2_PCI_CLEAR, regVal);

		msleep_interruptible(10); /*10 Milli Sec delay*/
	}
	else
		bRetVal = true;

	/*
	   -- It was seen on Dell390 systems that the firmware command was fired before the
	   -- firmware was actually ready to accept the firmware commands. The driver did
	   -- not recieve a response for the firmware commands and this was causing the DIL to timeout
	   -- ,reclaim the resources and crash. The following code looks for the heartbeat and
	   -- to make sure that we return from this function only when we get the heart beat making sure
	   -- that the firmware is running.
	   */

	bRetVal = crystalhd_flea_detect_fw_alive(hw);
	if( !bRetVal )
	{
		dev_info(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 8. Detect firmware heart beat failed.\n");
		return BC_STS_ERROR;
	}

	dev_dbg(&hw->adp->pdev->dev, "[%s]: Complete.\n", __func__);
	return BC_STS_SUCCESS;
}

bool crystalhd_flea_start_device(struct crystalhd_hw *hw)
{
	uint32_t	regVal	= 0;
	bool		bRetVal = false;

	/*
	-- Issue Core reset to bring in the default values in place
	*/
	crystalhd_flea_core_reset(hw);

	/*
	-- If the gisb arbitar register is not set to some other value
	-- and the firmware crashes, we see a NMI since the hardware did
	-- not respond to a register read at all. The PCI-E trace confirms the problem.
	-- Right now we are setting the register values to 0x7e00 and will check later
	-- what should be the correct value to program.
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_SUN_GISB_ARB_TIMER, 0xd80);

	/*
	-- Disable all interrupts
	*/
	crystalhd_flea_clear_interrupts(hw);
	crystalhd_flea_disable_interrupts(hw);

	/*
	-- Enable the option for getting the done count in
	-- Rx DMA engine.
	*/
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_MISC1_DMA_DEBUG_OPTIONS_REG);
	regVal |= 0x10;
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_DMA_DEBUG_OPTIONS_REG, regVal);

	/*
	-- Enable the TX DMA Engine once on startup.
	-- This is a new bit added.
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_TX_DMA_CTRL, 0x01);

	/*
	-- Enable the RX3 DMA Engine once on startup.
	-- This is a new bit added.
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_HIF_DMA_CTRL, 0x01);

	/*
	-- Set the Run bit for RX-Y and RX-UV DMA engines.
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_Y_RX_SW_DESC_LIST_CTRL_STS, 0x01);

	/*
	-- Make sure Early L1 is disabled - NAREN - This will not prevent the device from entering L1 under active mode
	*/
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_MISC_PERST_CLOCK_CTRL);
	regVal &= ~BCHP_MISC_PERST_CLOCK_CTRL_EARLY_L1_EXIT_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC_PERST_CLOCK_CTRL, regVal);

	crystalhd_flea_init_dram(hw);

	msleep_interruptible(5);

	/* Enable the Single Shot Transaction on PCI by disabling the */
	/* bit 29 of transaction configuration register */

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_PCIE_TL_TRANSACTION_CONFIGURATION);
	regVal &= (~(BC_BIT(29)));
	hw->pfnWriteDevRegister(hw->adp, BCHP_PCIE_TL_TRANSACTION_CONFIGURATION, regVal);

	crystalhd_flea_init_temperature_measure(hw,true);

	crystalhd_flea_init_power_state(hw);
	crystalhd_flea_set_next_power_state(hw, FLEA_EVT_START_DEVICE);

	/*
	-- Enable all interrupts
	*/
	crystalhd_flea_clear_interrupts(hw);
	crystalhd_flea_enable_interrupts(hw);

	/*
	-- This is the only time we set this pointer for Flea.
	-- Since there is no stop the pointer is not reset anytime....
	-- except for fatal errors.
	*/
	hw->rx_list_post_index = 0;
	hw->RxCaptureState = 0;

	msleep_interruptible(1);

	return bRetVal;
}


bool crystalhd_flea_stop_device(struct crystalhd_hw *hw)
{
	uint32_t regVal=0, pollCnt=0;

	/*
	-- Issue the core reset so that we
	-- make sure there is nothing running.
	*/
	crystalhd_flea_core_reset(hw);

	crystalhd_flea_init_temperature_measure(hw, false);

	/*
	-- If the gisb arbitrater register is not set to some other value
	-- and the firmware crashes, we see a NMI since the hardware did
	-- not respond to a register read at all. The PCI-E trace confirms the problem.
	-- Right now we are setting the register values to 0x7e00 and will check later
	-- what should be the correct value to program.
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_SUN_GISB_ARB_TIMER, 0xd80);

	/*
	-- Disable the TX DMA Engine once on shutdown.
	-- This is a new bit added.
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_TX_DMA_CTRL, 0x0);

	/*
	-- Disable the RX3 DMA Engine once on Stop.
	-- This is a new bit added.
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_HIF_DMA_CTRL, 0x0);

	/*
	-- Clear the RunStop Bit For RX DMA Control
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_Y_RX_SW_DESC_LIST_CTRL_STS, 0x0);

	hw->pfnWriteDevRegister(hw->adp, BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0, 0x0);

	/* * Wait for MEMC to become idle */
	pollCnt=0;
	while (1)
	{
		regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_CTL_STATUS);

		if(regVal & BCHP_DDR23_CTL_REGS_0_CTL_STATUS_idle_MASK)
			break;

		pollCnt++;
		if(pollCnt >= 100)
			break;

		msleep_interruptible(1);
	}

	/*First Disable the AVD and ARM before disabling the DRAM*/
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_CLK_PM_CTRL);

	regVal = BCHP_CLK_PM_CTRL_DIS_ARM_CLK_MASK |
			 BCHP_CLK_PM_CTRL_DIS_AVD_CLK_MASK |
			 BCHP_CLK_PM_CTRL_DIS_AVD_108_CLK_MASK |
			 BCHP_CLK_PM_CTRL_DIS_AVD_216_CLK_MASK;

	hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_PM_CTRL, regVal);

	/*
	-- Disable the interrupt after disabling the ARM and AVD.
	-- We should be able to access the registers because we still
	-- have not disabled the clock for blink block. We disable the
	-- blick 108 abd 216 clock at the end of this function.
	*/
	crystalhd_flea_clear_interrupts(hw);
	crystalhd_flea_disable_interrupts(hw);

	/*Now try disabling the DRAM.*/
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_PARAMS2);

	regVal |= BCHP_DDR23_CTL_REGS_0_PARAMS2_clke_MASK;

	/* * disable CKE */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_PARAMS2, regVal);

	/* * issue refresh command */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_REFRESH_CMD, 0x60);

	pollCnt=0;
	while(1)
	{
		regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_CTL_STATUS);

		if(!(regVal & BCHP_DDR23_CTL_REGS_0_CTL_STATUS_clke_MASK))
			break;

		pollCnt++;
		if(pollCnt >= 100)
			break;

		msleep_interruptible(1);
	}

	/* * Enable DDR clock, DM and READ_ENABLE pads power down and force into the power down */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL,
							BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_idle_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_dm_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_dq_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_read_enb_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_dqs_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_clk_iddq_MASK);

	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL,
							BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_idle_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_dm_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_dq_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_read_enb_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_dqs_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_clk_iddq_MASK);

	/* * Power down BL LDO cells */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL,
							BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL_pwrdn_MASK);

	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL,
							BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL_pwrdn_MASK);

	/* * Enable DDR control signal pad power down and force into the power down */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL,
							BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_idle_MASK |
							BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_ctl_iddq_MASK);

	/* * Disable ddr phy clock */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_CLK_PM_CTRL,
							BCHP_DDR23_PHY_CONTROL_REGS_CLK_PM_CTRL_DIS_DDR_CLK_MASK);

	/* * Disable PLL output */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG);

	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG,
							regVal & ~BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG_ENB_CLKOUT_MASK);

	/* * Power down addr_ctl LDO cells */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL,
							BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL_pwrdn_MASK);

	/* * Power down the PLL */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG);

	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG,
							regVal | BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG_PWRDN_MASK);

	/* shut down the PLL1 */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_CLK_PLL1_CTRL);

	hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_PLL1_CTRL,
							regVal | BCHP_CLK_PLL1_CTRL_POWERDOWN_MASK);

	hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_PLL0_ARM_DIV,	0xff);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_CLK_PM_CTRL);

	regVal |= BCHP_CLK_PM_CTRL_DIS_SUN_27_LOW_PWR_MASK |
			  BCHP_CLK_PM_CTRL_DIS_SUN_108_LOW_PWR_MASK |
			  BCHP_CLK_PM_CTRL_DIS_MISC_OTP_9_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_ARM_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_AVD_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_AVD_108_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_BLINK_108_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_MISC_108_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_BLINK_216_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_DDR_108_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_DDR_216_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_AVD_216_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_MISC_216_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_SUN_216_CLK_MASK;

	hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_PM_CTRL, regVal);

	crystalhd_flea_set_next_power_state(hw, FLEA_EVT_STOP_DEVICE);
	return true;
}

bool
crystalhd_flea_wake_up_hw(struct crystalhd_hw *hw)
{
	if(hw->FleaPowerState != FLEA_PS_ACTIVE)
	{
		crystalhd_flea_set_next_power_state(hw,	FLEA_EVT_FLL_CHANGE);
	}

	/* Now notify HW of the number of entries in the Free List */
	/* This starts up the channel bitmap delivery */
	crystalhd_flea_notify_fll_change(hw, false);

	hw->WakeUpDecodeDone = true;

	return true;
}

bool crystalhd_flea_check_input_full(struct crystalhd_hw *hw, uint32_t needed_sz, uint32_t *empty_sz, bool b_188_byte_pkts, uint8_t *flags)
{
	uint32_t				regVal=0;
	TX_INPUT_BUFFER_INFO	*pTxBuffInfo;
	uint32_t				FlagsAddr=0;

	*empty_sz = 0;
/*	*DramAddrOut=0; */


	/* Add condition here to wake up the HW in case some application is trying to do TX before starting RX - like FP */
	/* To prevent deadlocks. We are called here from Synchronized context so we can safely call this directly */

	if(hw->WakeUpDecodeDone != true)
	{
		/* Only wake up the HW if we are either being called from a single threaded app - like FP */
		/* or if we are not checking for the input buffer size as just a test */
		if(*flags == 0)
			crystalhd_flea_wake_up_hw(hw);
		else {
			*empty_sz = 2 * 1024 * 1024; /* FW Buffer size */
			/**DramAddrOut=0; */
			*flags=0;
			return false;
		}
	}

	/* if we have told the app that we have buffer empty then we cannot go to low power */
	if((hw->FleaPowerState != FLEA_PS_ACTIVE) && !hw->SingleThreadAppFIFOEmpty)
	{
		/**TxBuffSzOut=0; */
		/**DramAddrOut=0; */
		*empty_sz = 0;
		*flags=0;
		/*printk("PD can't Tx\n"); */
		return true; /*Indicate FULL*/
	}


	if(hw->TxFwInputBuffInfo.Flags & DFW_FLAGS_TX_ABORT)
	{
		*empty_sz=0;
		/**DramAddrOut=0; */
		*flags |= DFW_FLAGS_TX_ABORT;
		return true;
	}

	if( (hw->TxFwInputBuffInfo.DramBuffSzInBytes < needed_sz)
		||(!hw->TxFwInputBuffInfo.DramBuffAdd))
	{
		*empty_sz=0;
		/**DramAddrOut=0; */
		*flags=0;
		return true; /*Indicate FULL*/
	}

	if(hw->TxFwInputBuffInfo.DramBuffAdd % 4)
	{
		/*
		-- Indicate Full if we get a non-dowrd aligned address.
		-- This will avoid us posting the command to firmware and
		-- The TX will timeout and we will close the application properly.
		-- This avoids a illegal operation as far as the TX is concerned.
		*/
		printk("TxSDRAM-Destination Address Not DWORD Aligned:%x\n",hw->TxFwInputBuffInfo.DramBuffAdd);
		return true;
	}

	/*
	-- We got everything correctly from the firmware and hence we should be
	-- able to do the DMA. Indicate what app wants to hear.
	-- Firmware SAYS: I AM HUNGRY, GIVE ME FOOD. :)
	*/
	*empty_sz=hw->TxFwInputBuffInfo.DramBuffSzInBytes;
	/**dramAddrOut=pHWExt->TxFwInputBuffInfo.DramBuffAdd; */
/*	printk("empty size is %d\n", *empty_sz); */

	/* If we are just checking stats and are not actually going to DMA, don't increment */
	/* But we have to account for single threaded apps */
	if((*flags & 0x08) == 0x08)
	{
		/* This is a synchronized function */
		/* NAREN - In single threaded mode, if we have less than a defined size of buffer */
		/* ask the firmware to wrap around. To prevent deadlocks. */
		if(hw->TxFwInputBuffInfo.DramBuffSzInBytes < TX_WRAP_THRESHOLD)
		{
			pTxBuffInfo = (TX_INPUT_BUFFER_INFO *) (0);
			FlagsAddr = hw->TxBuffInfoAddr + ((uintptr_t) (&pTxBuffInfo->Flags));
			/* Read Modify the Flags to ask the FW to WRAP */
			hw->pfnDevDRAMRead(hw,FlagsAddr,1,&regVal);
			regVal |= DFW_FLAGS_WRAP;
			hw->pfnDevDRAMWrite(hw,FlagsAddr,1,&regVal);

			/* Indicate Busy to the application because we have to get new buffers from FW */
			*empty_sz=0;
			/* *DramAddrOut=0; */
			*flags=0;
			/* Wait for the next interrupt from the HW */
			hw->TxFwInputBuffInfo.DramBuffSzInBytes = 0;
			hw->TxFwInputBuffInfo.DramBuffAdd = 0;
			return true;
		}
		else
			hw->SingleThreadAppFIFOEmpty = true;
	}
	else if((*flags & 0x04) != 0x04)
		hw->EmptyCnt++;		/*OS_INTERLOCK_INCREMENT(&pHWExt->EmptyCnt); */

	/* Different from our Windows implementation */
	/* set bit 7 of the flags field to indicate that we have to use the destination address for TX */
	*flags |= BC_BIT(7);

	return false; /*Indicate Empty*/
}

static BC_STATUS crystalhd_flea_fw_cmd_post_proc(struct crystalhd_hw *hw, BC_FW_CMD *fw_cmd)
{
	BC_STATUS sts = BC_STS_SUCCESS;
	struct DecRspChannelStartVideo *st_rsp = NULL;
	struct C011_TS_CMD				*pGenRsp = NULL;
	struct DecRspChannelChannelOpen *pRsp  = NULL;

	pGenRsp = (struct C011_TS_CMD *) fw_cmd->rsp;

	switch (fw_cmd->cmd[0]) {
		case eCMD_C011_DEC_CHAN_STREAM_OPEN:
			hw->channelNum = pGenRsp->ulParams[2];

			dev_dbg(&hw->adp->pdev->dev, "Snooped Stream Open Cmd For ChNo:%x\n", hw->channelNum);
			break;
		case eCMD_C011_DEC_CHAN_OPEN:
			pRsp = (struct DecRspChannelChannelOpen *)pGenRsp;
			hw->channelNum = pRsp->ChannelID;

			/* used in Flea to update the Tx Buffer stats */
			hw->TxBuffInfoAddr = pRsp->transportStreamCaptureAddr;
			hw->TxFwInputBuffInfo.DramBuffAdd=0;
			hw->TxFwInputBuffInfo.DramBuffSzInBytes=0;
			hw->TxFwInputBuffInfo.Flags=0;
			hw->TxFwInputBuffInfo.HostXferSzInBytes=0;
			hw->TxFwInputBuffInfo.SeqNum=0;

			/* NAREN Init power management states here when we start the channel */
			hw->PwrDwnTxIntr = false;
			hw->PwrDwnPiQIntr = false;
			hw->EmptyCnt = 0;
			hw->SingleThreadAppFIFOEmpty = false;

			dev_dbg(&hw->adp->pdev->dev, "Snooped ChOpen Cmd For ChNo:%x TxBuffAddr:%x\n",
					hw->channelNum,
					hw->TxBuffInfoAddr);
			break;
		case eCMD_C011_DEC_CHAN_START_VIDEO:
			st_rsp = (struct DecRspChannelStartVideo *)fw_cmd->rsp;
			hw->pib_del_Q_addr = st_rsp->picInfoDeliveryQ;
			hw->pib_rel_Q_addr = st_rsp->picInfoReleaseQ;

			dev_dbg(&hw->adp->pdev->dev, "Snooping CHAN_START_VIDEO command to get the Addr of Del/Rel Queue\n");
			dev_dbg(&hw->adp->pdev->dev, "DelQAddr:%x RelQAddr:%x\n",
					hw->pib_del_Q_addr, hw->pib_rel_Q_addr);
			break;
		default:
			break;
	}
	return sts;
}

BC_STATUS crystalhd_flea_do_fw_cmd(struct crystalhd_hw *hw, BC_FW_CMD *fw_cmd)
{
	struct device *dev;
	uint32_t cnt = 0, cmd_res_addr;
	uint32_t *cmd_buff, *res_buff;
	wait_queue_head_t fw_cmd_event;
	int rc = 0;
	BC_STATUS sts;
	unsigned long flags;

	crystalhd_create_event(&fw_cmd_event);

	if (!hw || !fw_cmd) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_INV_ARG;
	}

	dev = &hw->adp->pdev->dev;

	dev_dbg(dev, "%s entered\n", __func__);

	cmd_buff = fw_cmd->cmd;
	res_buff = fw_cmd->rsp;

	if (!cmd_buff || !res_buff) {
		dev_err(dev, "Invalid Parameters for F/W Command\n");
		return BC_STS_INV_ARG;
	}

	hw->fwcmd_evt_sts = 0;
	hw->pfw_cmd_event = &fw_cmd_event;
	hw->FwCmdCnt++;

	if(hw->FleaPowerState != FLEA_PS_ACTIVE)
	{
		crystalhd_flea_set_next_power_state(hw,	FLEA_EVT_FW_CMD_POST);
	}

	spin_lock_irqsave(&hw->lock, flags);

	/*Write the command to the memory*/
	hw->pfnDevDRAMWrite(hw, hw->fwcmdPostAddr, FW_CMD_BUFF_SZ, cmd_buff);

	/*Memory Read for memory arbitrator flush*/
	hw->pfnDevDRAMRead(hw, hw->fwcmdPostAddr, 1, &cnt);

	/* Write the command address to mailbox */
	hw->pfnWriteDevRegister(hw->adp, hw->fwcmdPostMbox, hw->fwcmdPostAddr);

	spin_unlock_irqrestore(&hw->lock, flags);

	msleep_interruptible(50);

	/* FW commands should complete even if we got a signal from the upper layer */
	crystalhd_wait_on_event(&fw_cmd_event, hw->fwcmd_evt_sts,
							20000, rc, true);

	if (!rc) {
		sts = BC_STS_SUCCESS;
	} else if (rc == -EBUSY) {
		dev_err(dev, "Firmware command T/O\n");
		sts = BC_STS_TIMEOUT;
	} else if (rc == -EINTR) {
		dev_info(dev, "FwCmd Wait Signal - Can Never Happen\n");
		sts = BC_STS_IO_USER_ABORT;
	} else {
		dev_err(dev, "FwCmd IO Error.\n");
		sts = BC_STS_IO_ERROR;
	}

	if (sts != BC_STS_SUCCESS) {
		dev_err(dev, "FwCmd Failed.\n");
		return sts;
	}

	spin_lock_irqsave(&hw->lock, flags);

	/*Get the Responce Address*/
	cmd_res_addr = hw->pfnReadDevRegister(hw->adp, hw->fwcmdRespMbox);

	/*Read the Response*/
	hw->pfnDevDRAMRead(hw, cmd_res_addr, FW_CMD_BUFF_SZ, res_buff);

	spin_unlock_irqrestore(&hw->lock, flags);

	if (res_buff[2] != 0) {
		dev_err(dev, "res_buff[2] != C011_RET_SUCCESS\n");
		return BC_STS_FW_CMD_ERR;
	}

	sts = crystalhd_flea_fw_cmd_post_proc(hw, fw_cmd);
	if (sts != BC_STS_SUCCESS)
		dev_err(dev, "crystalhd_fw_cmd_post_proc Failed.\n");

	return sts;

}

void crystalhd_flea_get_dnsz(struct crystalhd_hw *hw, uint32_t list_index, uint32_t *y_dw_dnsz, uint32_t *uv_dw_dnsz)
{
	uint32_t y_dn_sz_reg, uv_dn_sz_reg;

	if (!list_index) {
		y_dn_sz_reg  = BCHP_MISC1_Y_RX_LIST0_CUR_BYTE_CNT;
		uv_dn_sz_reg = BCHP_MISC1_HIF_RX_LIST0_CUR_BYTE_CNT;
	} else {
		y_dn_sz_reg  = BCHP_MISC1_Y_RX_LIST1_CUR_BYTE_CNT;
		uv_dn_sz_reg = BCHP_MISC1_HIF_RX_LIST1_CUR_BYTE_CNT;
	}

	*y_dw_dnsz  = hw->pfnReadFPGARegister(hw->adp, y_dn_sz_reg);
	*uv_dw_dnsz = hw->pfnReadFPGARegister(hw->adp, uv_dn_sz_reg);

	return ;
}

BC_STATUS crystalhd_flea_hw_pause(struct crystalhd_hw *hw, bool state)
{
	/*printk("%s: Set flea to power down.\n", __func__); */
	crystalhd_flea_set_next_power_state(hw,	FLEA_EVT_FLL_CHANGE);
	return BC_STS_SUCCESS;
}

bool crystalhd_flea_peek_next_decoded_frame(struct crystalhd_hw *hw, uint64_t *meta_payload, uint32_t *picNumFlags, uint32_t PicWidth)
{
	unsigned long flags = 0;
	struct crystalhd_dioq *ioq;
	struct crystalhd_elem *tmp;
	struct crystalhd_rx_dma_pkt *rpkt;

	*meta_payload = 0;

	ioq = hw->rx_rdyq;
	spin_lock_irqsave(&ioq->lock, flags);

	if ((ioq->count > 0) && (ioq->head != (struct crystalhd_elem *)&ioq->head)) {
		tmp = ioq->head;
		spin_unlock_irqrestore(&ioq->lock, flags);
		rpkt = (struct crystalhd_rx_dma_pkt *)tmp->data;
		if (rpkt) {
			flea_GetPictureInfo(hw, rpkt, picNumFlags, meta_payload);
			/*printk("%s: flea_GetPictureInfo Pic#:%d\n", __func__, PicNumber); */
		}
		return true;
	}
	spin_unlock_irqrestore(&ioq->lock, flags);

	return false;

}

void crystalhd_flea_clear_rx_errs_intrs(struct crystalhd_hw *hw)
/*
-- Clears all the errors and interrupt on RX DMA engine.
*/
{
	uint32_t ulRegVal;
	union FLEA_INTR_BITS_COMMON	IntrToClear,IntrSts;

	IntrToClear.WholeReg = 0;
	IntrSts.WholeReg = 0;

	IntrSts.WholeReg = hw->pfnReadDevRegister(hw->adp, BCHP_INTR_INTR_STATUS);
	if(IntrSts.WholeReg)
	{
		ulRegVal = hw->pfnReadDevRegister(hw->adp, BCHP_MISC1_Y_RX_ERROR_STATUS);
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_Y_RX_ERROR_STATUS, ulRegVal);
		ulRegVal = hw->pfnReadDevRegister(hw->adp, BCHP_MISC1_HIF_RX_ERROR_STATUS);
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_HIF_RX_ERROR_STATUS, ulRegVal);

		IntrToClear.L0UVRxDMADone	=	IntrSts.L0UVRxDMADone;
		IntrToClear.L0UVRxDMAErr	=	IntrSts.L0UVRxDMAErr;
		IntrToClear.L0YRxDMADone	=	IntrSts.L0YRxDMADone;
		IntrToClear.L0YRxDMAErr		=	IntrSts.L0YRxDMAErr;
		IntrToClear.L1UVRxDMADone	=	IntrSts.L1UVRxDMADone;
		IntrToClear.L1UVRxDMAErr	=	IntrSts.L1UVRxDMAErr;
		IntrToClear.L1YRxDMADone	=	IntrSts.L1YRxDMADone;
		IntrToClear.L1YRxDMAErr		=	IntrSts.L1YRxDMAErr;

		hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_INTR_CLR_REG, IntrToClear.WholeReg);

		hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_EOI_CTRL, 1);
	}
	return;
}


void crystalhd_flea_stop_rx_dma_engine(struct crystalhd_hw *hw)
{
	union FLEA_INTR_BITS_COMMON	IntrStsValue;
	bool failedL0 = true, failedL1 = true;
	uint32_t pollCnt = 0;

	hw->RxCaptureState = 2;

	if((hw->rx_list_sts[0] == sts_free) && (hw->rx_list_sts[1] == sts_free)) {
		hw->RxCaptureState = 0;
		hw->RxSeqNum = 0;
		return; /* Nothing to be done */
	}

	if(hw->rx_list_sts[0] == sts_free)
		failedL0 = false;
	if(hw->rx_list_sts[1] == sts_free)
		failedL1 = false;

	while(1)
	{
		IntrStsValue.WholeReg = hw->pfnReadDevRegister(hw->adp, BCHP_INTR_INTR_STATUS);

		if(hw->rx_list_sts[0] != sts_free) {
			if( (IntrStsValue.L0YRxDMADone)  || (IntrStsValue.L0YRxDMAErr) ||
				(IntrStsValue.L0UVRxDMADone) || (IntrStsValue.L0UVRxDMAErr) )
			{
				failedL0 = false;
			}
		}
		else
			failedL0 = false;

		if(hw->rx_list_sts[1] != sts_free) {
			if( (IntrStsValue.L1YRxDMADone)  || (IntrStsValue.L1YRxDMAErr) ||
				(IntrStsValue.L1UVRxDMADone) || (IntrStsValue.L1UVRxDMAErr) )
			{
				failedL1 = false;
			}
		}
		else
			failedL1 = false;

		msleep_interruptible(10);

		if(pollCnt >= MAX_VALID_POLL_CNT)
			break;

		if((failedL0 == false) && (failedL1 == false))
			break;

		pollCnt++;
	}

	if(failedL0 || failedL1)
		printk("Failed to stop RX DMA\n");

	hw->RxCaptureState = 0;
	hw->RxSeqNum = 0;

	crystalhd_flea_clear_rx_errs_intrs(hw);
}

BC_STATUS crystalhd_flea_hw_fire_rxdma(struct crystalhd_hw *hw,
									   struct crystalhd_rx_dma_pkt *rx_pkt)
{
	struct device *dev;
	addr_64 desc_addr;
	unsigned long flags;
	PIC_DELIVERY_HOST_INFO	PicDeliInfo;
	uint32_t BuffSzInDwords;

	if (!hw || !rx_pkt) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_INV_ARG;
	}

	dev = &hw->adp->pdev->dev;

	if (hw->rx_list_post_index >= DMA_ENGINE_CNT) {
		dev_err(dev, "List Out Of bounds %x\n", hw->rx_list_post_index);
		return BC_STS_INV_ARG;
	}

	if(hw->RxCaptureState != 1) {
		dev_err(dev, "Capture not enabled\n");
		return BC_STS_BUSY;
	}

	spin_lock_irqsave(&hw->rx_lock, flags);
	if (hw->rx_list_sts[hw->rx_list_post_index]) {
		dev_dbg(dev, "HW list is busy\n");
		spin_unlock_irqrestore(&hw->rx_lock, flags);
		return BC_STS_BUSY;
	}

	if (!TEST_BIT(hw->PicQSts, hw->channelNum)) {
		/* NO pictures available for this channel */
		dev_dbg(dev, "No Picture Available for DMA\n");
		spin_unlock_irqrestore(&hw->rx_lock, flags);
		return BC_STS_BUSY;
	}

	CLEAR_BIT(hw->PicQSts, hw->channelNum);

	desc_addr.full_addr = rx_pkt->desc_mem.phy_addr;

	PicDeliInfo.ListIndex = hw->rx_list_post_index;
	PicDeliInfo.RxSeqNumber = hw->RxSeqNum;
	PicDeliInfo.HostDescMemLowAddr_Y = desc_addr.low_part;
	PicDeliInfo.HostDescMemHighAddr_Y = desc_addr.high_part;

	if (rx_pkt->uv_phy_addr) {
		/* Program the UV descriptor */
		desc_addr.full_addr = rx_pkt->uv_phy_addr;
		PicDeliInfo.HostDescMemLowAddr_UV = desc_addr.low_part;
		PicDeliInfo.HostDescMemHighAddr_UV = desc_addr.high_part;
	}

	rx_pkt->pkt_tag = hw->rx_pkt_tag_seed + hw->rx_list_post_index;
	hw->rx_list_sts[hw->rx_list_post_index] |= rx_waiting_y_intr;
	if (rx_pkt->uv_phy_addr)
		hw->rx_list_sts[hw->rx_list_post_index] |= rx_waiting_uv_intr;
	hw->rx_list_post_index = (hw->rx_list_post_index + 1) % DMA_ENGINE_CNT;

	spin_unlock_irqrestore(&hw->rx_lock, flags);

	crystalhd_dioq_add(hw->rx_actq, (void *)rx_pkt, false, rx_pkt->pkt_tag);

	BuffSzInDwords = (sizeof (PicDeliInfo) - sizeof(PicDeliInfo.Reserved))/4;

	/*
	-- Write the parameters in DRAM.
	*/
	spin_lock_irqsave(&hw->lock, flags);
	hw->pfnDevDRAMWrite(hw, hw->FleaRxPicDelAddr, BuffSzInDwords, (uint32_t*)&PicDeliInfo);
	hw->pfnWriteDevRegister(hw->adp, RX_POST_MAILBOX, hw->channelNum);
	spin_unlock_irqrestore(&hw->lock, flags);

	hw->RxSeqNum++;

	return BC_STS_SUCCESS;
}

BC_STATUS crystalhd_flea_hw_post_cap_buff(struct crystalhd_hw *hw, struct crystalhd_rx_dma_pkt *rx_pkt)
{
	BC_STATUS sts = crystalhd_flea_hw_fire_rxdma(hw, rx_pkt);

	if (sts != BC_STS_SUCCESS)
		crystalhd_dioq_add(hw->rx_freeq, (void *)rx_pkt, false, rx_pkt->pkt_tag);

	hw->pfnNotifyFLLChange(hw, false);

	return sts;
}

void crystalhd_flea_start_tx_dma_engine(struct crystalhd_hw *hw, uint8_t list_id, addr_64 desc_addr)
{
	uint32_t dma_cntrl;
	uint32_t first_desc_u_addr, first_desc_l_addr;
	TX_INPUT_BUFFER_INFO	TxBuffInfo;
	uint32_t WrAddr=0, WrSzInDWords=0;

	hw->EmptyCnt--;
	hw->SingleThreadAppFIFOEmpty = false;

	/* For FLEA, first update the HW with the DMA parameters */
	WrSzInDWords = (sizeof(TxBuffInfo.DramBuffAdd) +
					sizeof(TxBuffInfo.DramBuffSzInBytes) +
					sizeof(TxBuffInfo.HostXferSzInBytes))/4;

	/*Make the DramBuffSz as Zero skip first ULONG*/
	WrAddr = hw->TxBuffInfoAddr;
	hw->TxFwInputBuffInfo.DramBuffAdd = TxBuffInfo.DramBuffAdd = 0;
	hw->TxFwInputBuffInfo.DramBuffSzInBytes =  TxBuffInfo.DramBuffSzInBytes = 0;
	TxBuffInfo.HostXferSzInBytes = hw->TxFwInputBuffInfo.HostXferSzInBytes;

	hw->pfnDevDRAMWrite(hw,	WrAddr,	WrSzInDWords, (uint32_t *)&TxBuffInfo);

	if (list_id == 0) {
		first_desc_u_addr = BCHP_MISC1_TX_FIRST_DESC_U_ADDR_LIST0;
		first_desc_l_addr = BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST0;
	} else {
		first_desc_u_addr = BCHP_MISC1_TX_FIRST_DESC_U_ADDR_LIST1;
		first_desc_l_addr = BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST1;
	}

	dma_cntrl = hw->pfnReadFPGARegister(hw->adp, BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS);
	if (!(dma_cntrl & BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS_TX_DMA_RUN_STOP_MASK)) {
		dma_cntrl |= BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS_TX_DMA_RUN_STOP_MASK;
		hw->pfnWriteFPGARegister(hw->adp, BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS,
								 dma_cntrl);
	}

	hw->pfnWriteFPGARegister(hw->adp, first_desc_u_addr, desc_addr.high_part);

	hw->pfnWriteFPGARegister(hw->adp, first_desc_l_addr, desc_addr.low_part | 0x01);
	/* Be sure we set the valid bit ^^^^ */

	return;
}

BC_STATUS crystalhd_flea_stop_tx_dma_engine(struct crystalhd_hw *hw)
{
	struct device *dev;
	uint32_t dma_cntrl, cnt = 30;
	uint32_t l1 = 1, l2 = 1;

	dma_cntrl = hw->pfnReadFPGARegister(hw->adp, BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS);

	dev = &hw->adp->pdev->dev;

	dev_dbg(dev, "Stopping TX DMA Engine..\n");

	if (!(dma_cntrl & BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS_TX_DMA_RUN_STOP_MASK)) {
		hw->TxList0Sts = ListStsFree;
		hw->TxList1Sts = ListStsFree;
		hw->tx_list_post_index = 0;

		dev_dbg(dev, "Already Stopped\n");
		return BC_STS_SUCCESS;
	}

	crystalhd_flea_disable_interrupts(hw);

	/* Issue stop to HW */
	dma_cntrl &= ~BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS_TX_DMA_RUN_STOP_MASK;
	hw->pfnWriteFPGARegister(hw->adp, BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS, dma_cntrl);

	dev_dbg(dev, "Cleared the DMA Start bit\n");

	/* Poll for 3seconds (30 * 100ms) on both the lists..*/
	while ((l1 || l2) && cnt) {

		if (l1) {
			l1 = hw->pfnReadFPGARegister(hw->adp,
										 BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST0);
			l1 &= BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS_TX_DMA_RUN_STOP_MASK;
		}

		if (l2) {
			l2 = hw->pfnReadFPGARegister(hw->adp,
										 BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST1);
			l2 &= BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS_TX_DMA_RUN_STOP_MASK;
		}

		msleep_interruptible(100);

		cnt--;
	}

	if (!cnt) {
		dev_err(dev, "Failed to stop TX DMA.. l1 %d, l2 %d\n", l1, l2);
		crystalhd_flea_enable_interrupts(hw);
		return BC_STS_ERROR;
	}

	hw->TxList0Sts = ListStsFree;
	hw->TxList1Sts = ListStsFree;

	hw->tx_list_post_index = 0;
	dev_dbg(dev, "stopped TX DMA..\n");
	crystalhd_flea_enable_interrupts(hw);

	return BC_STS_SUCCESS;
}

static void crystalhd_flea_update_tx_done_to_fw(struct crystalhd_hw *hw)
{
	struct device *dev;
	uint32_t				regVal		= 0;
	uint32_t				seqNumAddr	= 0;
	uint32_t				seqVal		= 0;
	TX_INPUT_BUFFER_INFO	*pTxBuffInfo;

	dev = &hw->adp->pdev->dev;
	/*
	-- first update the sequence number and then update the
	-- scratch.
	*/
	pTxBuffInfo = (TX_INPUT_BUFFER_INFO *) (0);
	seqNumAddr = hw->TxBuffInfoAddr + ((uintptr_t) (&pTxBuffInfo->SeqNum));

	/*Read the seqnece number */
	hw->pfnDevDRAMRead(hw, seqNumAddr, 1, &regVal);

	seqVal = regVal;
	regVal++;

	/*Increment and Write back to same memory location. */
	hw->pfnDevDRAMWrite(hw,	seqNumAddr, 1, &regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, INDICATE_TX_DONE_REG);
	regVal++;
	hw->pfnWriteDevRegister(hw->adp, INDICATE_TX_DONE_REG, regVal);

	dev_dbg(dev, "TxUpdate[SeqNum DRAM Addr:%x] SeqNum:%x ScratchValue:%x\n",
		seqNumAddr, seqVal, regVal);

	return;
}

bool crystalhd_flea_tx_list0_handler(struct crystalhd_hw *hw, uint32_t err_sts)
{
	uint32_t err_mask, tmp;

	err_mask = MISC1_TX_DMA_ERROR_STATUS_TX_L0_DESC_TX_ABORT_ERRORS_MASK |
	MISC1_TX_DMA_ERROR_STATUS_TX_L0_DMA_DATA_TX_ABORT_ERRORS_MASK |
	MISC1_TX_DMA_ERROR_STATUS_TX_L0_FIFO_FULL_ERRORS_MASK;

	if (!(err_sts & err_mask))
		return false;

	dev_err(&hw->adp->pdev->dev, "Error on Tx-L0 %x\n", err_sts);

	tmp = err_mask;

	if (err_sts & MISC1_TX_DMA_ERROR_STATUS_TX_L0_FIFO_FULL_ERRORS_MASK)
		tmp &= ~MISC1_TX_DMA_ERROR_STATUS_TX_L0_FIFO_FULL_ERRORS_MASK;

	if (tmp) {
		/* reset list index.*/
		hw->tx_list_post_index = 0;
	}

	tmp = err_sts & err_mask;
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_TX_DMA_ERROR_STATUS, tmp);

	return true;
}

bool crystalhd_flea_tx_list1_handler(struct crystalhd_hw *hw, uint32_t err_sts)
{
	uint32_t err_mask, tmp;

	err_mask = MISC1_TX_DMA_ERROR_STATUS_TX_L1_DESC_TX_ABORT_ERRORS_MASK |
	MISC1_TX_DMA_ERROR_STATUS_TX_L1_DMA_DATA_TX_ABORT_ERRORS_MASK |
	MISC1_TX_DMA_ERROR_STATUS_TX_L1_FIFO_FULL_ERRORS_MASK;

	if (!(err_sts & err_mask))
		return false;

	dev_err(&hw->adp->pdev->dev, "Error on Tx-L1 %x\n", err_sts);

	tmp = err_mask;

	if (err_sts & MISC1_TX_DMA_ERROR_STATUS_TX_L1_FIFO_FULL_ERRORS_MASK)
		tmp &= ~MISC1_TX_DMA_ERROR_STATUS_TX_L1_FIFO_FULL_ERRORS_MASK;

	if (tmp) {
		/* reset list index.*/
		hw->tx_list_post_index = 0;
	}

	tmp = err_sts & err_mask;
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_TX_DMA_ERROR_STATUS, tmp);

	return true;
}

void crystalhd_flea_tx_isr(struct crystalhd_hw *hw, union FLEA_INTR_BITS_COMMON int_sts)
{
	uint32_t err_sts;

	if (int_sts.L0TxDMADone) {
		hw->TxList0Sts &= ~TxListWaitingForIntr;
		crystalhd_hw_tx_req_complete(hw, hw->tx_ioq_tag_seed + 0, BC_STS_SUCCESS);
	}

	if (int_sts.L1TxDMADone) {
		hw->TxList1Sts &= ~TxListWaitingForIntr;
		crystalhd_hw_tx_req_complete(hw, hw->tx_ioq_tag_seed + 1, BC_STS_SUCCESS);
	}

	if (!(int_sts.L0TxDMAErr || int_sts.L1TxDMAErr))
		/* No error mask set.. */
		return;

	/* Handle Tx errors. */
	err_sts = hw->pfnReadDevRegister(hw->adp, BCHP_MISC1_TX_DMA_ERROR_STATUS);

	if (crystalhd_flea_tx_list0_handler(hw, err_sts))
		crystalhd_hw_tx_req_complete(hw, hw->tx_ioq_tag_seed + 0, BC_STS_ERROR);

	if (crystalhd_flea_tx_list1_handler(hw, err_sts))
		crystalhd_hw_tx_req_complete(hw, hw->tx_ioq_tag_seed + 1, BC_STS_ERROR);

	hw->stats.tx_errors++;
}

bool crystalhd_flea_rx_list0_handler(struct crystalhd_hw *hw,
									 union FLEA_INTR_BITS_COMMON int_sts,
									 uint32_t y_err_sts,
									 uint32_t uv_err_sts)
{
	uint32_t tmp;
	enum list_sts tmp_lsts;

	if (!(y_err_sts & GET_Y0_ERR_MSK) && !(uv_err_sts & GET_UV0_ERR_MSK))
		return false;

	tmp_lsts = hw->rx_list_sts[0];

	/* Y0 - DMA */
	tmp = y_err_sts & GET_Y0_ERR_MSK;
	if (int_sts.L0YRxDMADone)
		hw->rx_list_sts[0] &= ~rx_waiting_y_intr;

	if (y_err_sts & MISC1_Y_RX_ERROR_STATUS_RX_L0_UNDERRUN_ERROR_MASK) {
		hw->rx_list_sts[0] &= ~rx_waiting_y_intr;
		tmp &= ~MISC1_Y_RX_ERROR_STATUS_RX_L0_UNDERRUN_ERROR_MASK;
	}

	if (y_err_sts & MISC1_Y_RX_ERROR_STATUS_RX_L0_FIFO_FULL_ERRORS_MASK) {
		/* Can never happen for Flea */
		printk("FLEA fifo full - impossible\n");
		hw->rx_list_sts[0] &= ~rx_y_mask;
		hw->rx_list_sts[0] |= rx_y_error;
		tmp &= ~MISC1_Y_RX_ERROR_STATUS_RX_L0_FIFO_FULL_ERRORS_MASK;
	}

	if (tmp) {
		hw->rx_list_sts[0] &= ~rx_y_mask;
		hw->rx_list_sts[0] |= rx_y_error;
		hw->rx_list_post_index = 0;
	}

	/* UV0 - DMA */
	tmp = uv_err_sts & GET_UV0_ERR_MSK;
	if (int_sts.L0UVRxDMADone)
		hw->rx_list_sts[0] &= ~rx_waiting_uv_intr;

	if (uv_err_sts & MISC1_UV_RX_ERROR_STATUS_RX_L0_UNDERRUN_ERROR_MASK) {
		hw->rx_list_sts[0] &= ~rx_waiting_uv_intr;
		tmp &= ~MISC1_UV_RX_ERROR_STATUS_RX_L0_UNDERRUN_ERROR_MASK;
	}

	if (uv_err_sts & MISC1_UV_RX_ERROR_STATUS_RX_L0_FIFO_FULL_ERRORS_MASK) {
		/* Can never happen for Flea */
		printk("FLEA fifo full - impossible\n");
		hw->rx_list_sts[0] &= ~rx_uv_mask;
		hw->rx_list_sts[0] |= rx_uv_error;
		tmp &= ~MISC1_UV_RX_ERROR_STATUS_RX_L0_FIFO_FULL_ERRORS_MASK;
	}

	if (tmp) {
		hw->rx_list_sts[0] &= ~rx_uv_mask;
		hw->rx_list_sts[0] |= rx_uv_error;
		hw->rx_list_post_index = 0;
	}

	if (y_err_sts & GET_Y0_ERR_MSK) {
		tmp = y_err_sts & GET_Y0_ERR_MSK;
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_Y_RX_ERROR_STATUS, tmp);
	}

	if (uv_err_sts & GET_UV0_ERR_MSK) {
		tmp = uv_err_sts & GET_UV0_ERR_MSK;
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_HIF_RX_ERROR_STATUS, tmp);
	}

	return (tmp_lsts != hw->rx_list_sts[0]);
}

bool crystalhd_flea_rx_list1_handler(struct crystalhd_hw *hw,
									 union FLEA_INTR_BITS_COMMON int_sts,
									 uint32_t y_err_sts,
									 uint32_t uv_err_sts)
{
	uint32_t tmp;
	enum list_sts tmp_lsts;

	if (!(y_err_sts & GET_Y1_ERR_MSK) && !(uv_err_sts & GET_UV1_ERR_MSK))
		return false;

	tmp_lsts = hw->rx_list_sts[1];

	/* Y1 - DMA */
	tmp = y_err_sts & GET_Y1_ERR_MSK;
	if (int_sts.L1YRxDMADone)
		hw->rx_list_sts[1] &= ~rx_waiting_y_intr;

	if (y_err_sts & MISC1_Y_RX_ERROR_STATUS_RX_L1_UNDERRUN_ERROR_MASK) {
		hw->rx_list_sts[1] &= ~rx_waiting_y_intr;
		tmp &= ~MISC1_Y_RX_ERROR_STATUS_RX_L1_UNDERRUN_ERROR_MASK;
	}

	if (y_err_sts & MISC1_Y_RX_ERROR_STATUS_RX_L1_FIFO_FULL_ERRORS_MASK) {
		/* Can never happen for Flea */
		printk("FLEA fifo full - impossible\n");
		hw->rx_list_sts[1] &= ~rx_y_mask;
		hw->rx_list_sts[1] |= rx_y_error;
		tmp &= ~MISC1_Y_RX_ERROR_STATUS_RX_L1_FIFO_FULL_ERRORS_MASK;
	}

	if (tmp) {
		hw->rx_list_sts[1] &= ~rx_y_mask;
		hw->rx_list_sts[1] |= rx_y_error;
		hw->rx_list_post_index = 0;
	}

	/* UV1 - DMA */
	tmp = uv_err_sts & GET_UV1_ERR_MSK;
	if (int_sts.L1UVRxDMADone)
		hw->rx_list_sts[1] &= ~rx_waiting_uv_intr;

	if (uv_err_sts & MISC1_UV_RX_ERROR_STATUS_RX_L1_UNDERRUN_ERROR_MASK) {
		hw->rx_list_sts[1] &= ~rx_waiting_uv_intr;
		tmp &= ~MISC1_UV_RX_ERROR_STATUS_RX_L1_UNDERRUN_ERROR_MASK;
	}

	if (uv_err_sts & MISC1_UV_RX_ERROR_STATUS_RX_L1_FIFO_FULL_ERRORS_MASK) {
		/* Can never happen for Flea */
		printk("FLEA fifo full - impossible\n");
		hw->rx_list_sts[1] &= ~rx_uv_mask;
		hw->rx_list_sts[1] |= rx_uv_error;
		tmp &= ~MISC1_UV_RX_ERROR_STATUS_RX_L1_FIFO_FULL_ERRORS_MASK;
	}

	if (tmp) {
		hw->rx_list_sts[1] &= ~rx_uv_mask;
		hw->rx_list_sts[1] |= rx_uv_error;
		hw->rx_list_post_index = 0;
	}

	if (y_err_sts & GET_Y1_ERR_MSK) {
		tmp = y_err_sts & GET_Y1_ERR_MSK;
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_Y_RX_ERROR_STATUS, tmp);
	}

	if (uv_err_sts & GET_UV1_ERR_MSK) {
		tmp = uv_err_sts & GET_UV1_ERR_MSK;
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_HIF_RX_ERROR_STATUS, tmp);
	}

	return (tmp_lsts != hw->rx_list_sts[1]);
}

void crystalhd_flea_rx_isr(struct crystalhd_hw *hw, union FLEA_INTR_BITS_COMMON intr_sts)
{
	unsigned long flags;
	uint32_t i, list_avail = 0;
	BC_STATUS comp_sts = BC_STS_NO_DATA;
	uint32_t y_err_sts, uv_err_sts, y_dn_sz = 0, uv_dn_sz = 0;
	bool ret = 0;

	if (!hw) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return;
	}

	if (!(intr_sts.L0YRxDMADone || intr_sts.L1YRxDMADone || intr_sts.L0UVRxDMADone || intr_sts.L1UVRxDMADone ||
		intr_sts.L0YRxDMAErr || intr_sts.L1YRxDMAErr || intr_sts.L0UVRxDMAErr || intr_sts.L1UVRxDMAErr))
		return;

	spin_lock_irqsave(&hw->rx_lock, flags);

	y_err_sts = hw->pfnReadDevRegister(hw->adp, BCHP_MISC1_Y_RX_ERROR_STATUS);
	uv_err_sts = hw->pfnReadDevRegister(hw->adp, BCHP_MISC1_HIF_RX_ERROR_STATUS);

	for (i = 0; i < DMA_ENGINE_CNT; i++) {
		/* Update States..*/
		if (i == 0)
			ret = crystalhd_flea_rx_list0_handler(hw, intr_sts, y_err_sts, uv_err_sts);
		else
			ret = crystalhd_flea_rx_list1_handler(hw, intr_sts, y_err_sts, uv_err_sts);
		if (ret) {
			switch (hw->rx_list_sts[i]) {
				case sts_free:
					comp_sts = BC_STS_SUCCESS;
					list_avail = 1;
					hw->stats.rx_success++;
					break;
				case rx_y_error:
				case rx_uv_error:
				case rx_sts_error:
					/* We got error on both or Y or uv. */
					hw->stats.rx_errors++;
					hw->pfnHWGetDoneSize(hw, i, &y_dn_sz, &uv_dn_sz);
					dev_info(&hw->adp->pdev->dev, "list_index:%x "
					"rx[%d] rxtot[%d] Y:%x UV:%x Int:%x YDnSz:%x "
					"UVDnSz:%x\n", i, hw->stats.rx_errors,
							 hw->stats.rx_errors + hw->stats.rx_success,
							 y_err_sts, uv_err_sts, intr_sts.WholeReg,
							 y_dn_sz, uv_dn_sz);
							 hw->rx_list_sts[i] = sts_free;
							 comp_sts = BC_STS_ERROR;
							 break;
				default:
					/* Wait for completion..*/
					comp_sts = BC_STS_NO_DATA;
					break;
			}
		}
		/* handle completion...*/
		if (comp_sts != BC_STS_NO_DATA) {
			crystalhd_rx_pkt_done(hw, i, comp_sts);
			comp_sts = BC_STS_NO_DATA;
		}
	}

	spin_unlock_irqrestore(&hw->rx_lock, flags);

	if (list_avail)
		crystalhd_hw_start_capture(hw);
}

bool crystalhd_flea_hw_interrupt_handle(struct crystalhd_adp *adp, struct crystalhd_hw *hw)
{
	union FLEA_INTR_BITS_COMMON	IntrStsValue;
	bool				bIntFound		= false;
	bool				bPostRxBuff		= false;
	bool				bSomeCmdDone	= false;
	struct crystalhd_rx_dma_pkt *rx_pkt;

	bool	rc = false;

	if (!adp || !hw->dev_started)
		return rc;

	IntrStsValue.WholeReg=0;

	IntrStsValue.WholeReg = hw->pfnReadDevRegister(hw->adp, BCHP_INTR_INTR_STATUS);

	if(!IntrStsValue.WholeReg)
		return rc;	/*Not Our interrupt*/

	/*If any of the bit is set we have a problem*/
	if(IntrStsValue.HaltIntr || IntrStsValue.PcieTgtCaAttn || IntrStsValue.PcieTgtUrAttn)
	{
		printk("Bad HW Error in CrystalHD Driver\n");
		return rc;
	}

	/* Our interrupt */
	hw->stats.num_interrupts++;
	rc = true;

	/* NAREN When In Power Down state, only interrupts possible are TXFIFO and PiQ       */
	/* Save the state of these interrupts to process them when we resume from power down */
	if(hw->FleaPowerState == FLEA_PS_LP_COMPLETE)
	{
		if(IntrStsValue.ArmMbox1Int)
		{
			hw->PwrDwnPiQIntr = true;
			bIntFound = true;
		}

		if(IntrStsValue.ArmMbox2Int)
		{
			hw->PwrDwnTxIntr = true;
			bIntFound = true;
		}

		/*Write End Of Interrupt for PCIE*/
		if(bIntFound)
		{
			hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_INTR_CLR_REG, IntrStsValue.WholeReg);
			hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_EOI_CTRL, 1);
		}
		return (bIntFound);
	}

	/*
	-- Arm Mail box Zero interrupt is
	-- BCHP_ARMCR4_BRIDGE_REG_MBOX_ARM1
	*/
	if(IntrStsValue.ArmMbox0Int)
	{
		/*HWFWCmdComplete(pHWExt,IntrBmp); */
		/*Set the Event and the status flag*/
		if (hw->pfw_cmd_event) {
			hw->fwcmd_evt_sts = 1;
			crystalhd_set_event(hw->pfw_cmd_event);
		}
		bIntFound = true;
		bSomeCmdDone = true;
		hw->FwCmdCnt--;
	}

	/* Rx interrupts */
	crystalhd_flea_rx_isr(hw, IntrStsValue);

	if( IntrStsValue.L0YRxDMADone || IntrStsValue.L1YRxDMADone || IntrStsValue.L0UVRxDMADone || IntrStsValue.L1UVRxDMADone || IntrStsValue.L0YRxDMAErr || IntrStsValue.L1YRxDMAErr )
	{
		bSomeCmdDone = true;
	}


	/* Tx interrupts*/
	crystalhd_flea_tx_isr(hw, IntrStsValue);

	/*
	-- Indicate the TX Done to Flea Firmware.
	*/
	if(IntrStsValue.L0TxDMADone || IntrStsValue.L1TxDMADone || IntrStsValue.L0TxDMAErr || IntrStsValue.L1TxDMAErr)
	{
		crystalhd_flea_update_tx_done_to_fw(hw);
		bSomeCmdDone = true;
	}
	/*
	-- We are doing this here because we processed the interrupts.
	-- We might want to change the PicQSts bitmap in any of the interrupts.
	-- This should be done before trying to post the next RX buffer.
	-- NOTE: ArmMbox1Int is BCHP_ARMCR4_BRIDGE_REG_MBOX_ARM2
	*/
	if(IntrStsValue.ArmMbox1Int)
	{
		/*pHWExt->FleaBmpIntrCnt++; */
		crystalhd_flea_update_temperature(hw);
		crystalhd_flea_handle_PicQSts_intr(hw);
		bPostRxBuff = true;
		bIntFound = true;
	}

	if(IntrStsValue.ArmMbox2Int)
	{
		crystalhd_flea_update_temperature(hw);
		crystalhd_flea_update_tx_buff_info(hw);
		bIntFound = true;
	}

	/*Write End Of Interrupt for PCIE*/
	if(rc)
	{
		hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_INTR_CLR_REG, IntrStsValue.WholeReg);
		hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_EOI_CTRL, 1);
	}

	/* Try to post RX Capture buffer from ISR context */
	if(bPostRxBuff) {
		rx_pkt = crystalhd_dioq_fetch(hw->rx_freeq);
		if (rx_pkt)
			hw->pfnPostRxSideBuff(hw, rx_pkt);
	}

	if( (hw->FleaPowerState == FLEA_PS_LP_PENDING) && (bSomeCmdDone))
	{
		/*printk("interrupt_handle: current PS:%d, bSomeCmdDone%d\n", hw->FleaPowerState,bSomeCmdDone); */
		crystalhd_flea_set_next_power_state(hw, FLEA_EVT_CMD_COMP);
	}

	/* NAREN place the device in low power mode if we have not started playing video */
	/*if((hw->FleaPowerState == FLEA_PS_ACTIVE) && (hw->WakeUpDecodeDone != true)) */
	/*{ */
	/*	if((hw->ReadyListLen == 0) && (hw->FreeListLen == 0)) */
	/*	{ */
	/*		crystalhd_flea_set_next_power_state(hw, FLEA_EVT_FLL_CHANGE); */
	/*		printk("ISR Idle\n"); */
	/*	} */
	/*} */

	return rc;
}

/* This function cannot be called from ISR context since it uses APIs that can sleep */
bool flea_GetPictureInfo(struct crystalhd_hw *hw, struct crystalhd_rx_dma_pkt * rx_pkt,
							uint32_t *PicNumber, uint64_t *PicMetaData)
{
	struct device *dev = &hw->adp->pdev->dev;
	uint32_t PicInfoLineNum = 0, offset = 0, size = 0;
	PBC_PIC_INFO_BLOCK pPicInfoLine = NULL;
	uint32_t tmpYBuffData;
	unsigned long res = 0;
	uint32_t widthField = 0;
	bool rtVal = true;

	void *tmpPicInfo = NULL;
	struct crystalhd_dio_req *dio = rx_pkt->dio_req;
	*PicNumber = 0;
	*PicMetaData = 0;

	if (!dio)
		goto getpictureinfo_err_nosem;

/*	if(down_interruptible(&hw->fetch_sem)) */
/*		goto getpictureinfo_err_nosem; */

	tmpPicInfo = kmalloc(2 * sizeof(BC_PIC_INFO_BLOCK) + 16, GFP_KERNEL); /* since copy_from_user can sleep anyway */
	if(tmpPicInfo == NULL)
		goto getpictureinfo_err;
	dio->pib_va = kmalloc(32, GFP_KERNEL); /* temp buffer of 32 bytes for the rest; */
	if(dio->pib_va == NULL)
		goto getpictureinfo_err;

	offset = (rx_pkt->dio_req->uinfo.y_done_sz * 4) - PIC_PIB_DATA_OFFSET_FROM_END;
	res = copy_from_user(dio->pib_va, (void *)(dio->uinfo.xfr_buff + offset), 4);
	if (res != 0)
		goto getpictureinfo_err;
	PicInfoLineNum = *(uint32_t*)(dio->pib_va);
	if (PicInfoLineNum > 1092) {
		dev_err(dev, "Invalid Line Number[%x], DoneSz:0x%x Bytes\n",
			(int)PicInfoLineNum, rx_pkt->dio_req->uinfo.y_done_sz * 4);
		goto getpictureinfo_err;
	}

	offset = (rx_pkt->dio_req->uinfo.y_done_sz * 4) - PIC_WIDTH_OFFSET_FROM_END;
	res = copy_from_user(dio->pib_va, (void *)(dio->uinfo.xfr_buff + offset), 4);
	if (res != 0)
		goto getpictureinfo_err;
	widthField = *(uint32_t*)(dio->pib_va);

	hw->PICWidth = widthField & 0x3FFFFFFF; /* bit 31 is FMT Change, bit 30 is EOS */
	if (hw->PICWidth > 2048) {
		dev_err(dev, "Invalid width [%d]\n", hw->PICWidth);
		goto getpictureinfo_err;
	}

	/* calc pic info line offset */
	if (dio->uinfo.b422mode) {
		size = 2 * sizeof(BC_PIC_INFO_BLOCK);
		offset = (PicInfoLineNum * hw->PICWidth * 2) + 4;
	} else {
		size = sizeof(BC_PIC_INFO_BLOCK);
		offset = (PicInfoLineNum * hw->PICWidth) + 4;
	}

	res = copy_from_user(tmpPicInfo, (void *)(dio->uinfo.xfr_buff+offset), size);
	if (res != 0)
		goto getpictureinfo_err;

	pPicInfoLine = (PBC_PIC_INFO_BLOCK)(tmpPicInfo);

	*PicMetaData = pPicInfoLine->timeStamp;

	if(widthField & PIB_EOS_DETECTED_BIT)
	{
		dev_dbg(dev, "Got EOS flag.\n");
		hw->DrvEosDetected = 1;
		*(uint32_t *)(dio->pib_va) = 0xFFFFFFFF;
		res = copy_to_user((void *)(dio->uinfo.xfr_buff), dio->pib_va, 4);
		if (res != 0)
			goto getpictureinfo_err;
	}
	else
	{
		if( hw->DrvEosDetected == 1 )
			hw->DrvCancelEosFlag = 1;

		hw->DrvEosDetected = 0;
		res = copy_from_user(dio->pib_va, (void *)(dio->uinfo.xfr_buff), 4);
		if (res != 0)
			goto getpictureinfo_err;

		tmpYBuffData = *(uint32_t *)(dio->pib_va);
		pPicInfoLine->ycom = tmpYBuffData;
		res = copy_to_user((void *)(dio->uinfo.xfr_buff+offset), tmpPicInfo, size);
		if (res != 0)
			goto getpictureinfo_err;

		*(uint32_t *)(dio->pib_va) = PicInfoLineNum;
		res = copy_to_user((void *)(dio->uinfo.xfr_buff), dio->pib_va, 4);
		if (res != 0)
			goto getpictureinfo_err;
	}

	if(widthField & PIB_FORMAT_CHANGE_BIT)
	{
		rx_pkt->flags = 0;
		rx_pkt->flags |= COMP_FLAG_PIB_VALID | COMP_FLAG_FMT_CHANGE;

		rx_pkt->pib.picture_number			= pPicInfoLine->picture_number;
		rx_pkt->pib.width					= pPicInfoLine->width;
		rx_pkt->pib.height					= pPicInfoLine->height;
		rx_pkt->pib.chroma_format			= pPicInfoLine->chroma_format;
		rx_pkt->pib.pulldown				= pPicInfoLine->pulldown;
		rx_pkt->pib.flags					= pPicInfoLine->flags;
		rx_pkt->pib.sess_num				= pPicInfoLine->sess_num;
		rx_pkt->pib.aspect_ratio			= pPicInfoLine->aspect_ratio;
		rx_pkt->pib.colour_primaries		= pPicInfoLine->colour_primaries;
		rx_pkt->pib.picture_meta_payload	= pPicInfoLine->picture_meta_payload;
		rx_pkt->pib.frame_rate				= pPicInfoLine->frame_rate;
		rx_pkt->pib.custom_aspect_ratio_width_height = pPicInfoLine->custom_aspect_ratio_width_height;
		rx_pkt->pib.n_drop				= pPicInfoLine->n_drop;
		rx_pkt->pib.ycom				= pPicInfoLine->ycom;
		hw->PICHeight = rx_pkt->pib.height;
		hw->PICWidth = rx_pkt->pib.width;
		hw->LastPicNo=0;
		hw->LastTwoPicNo=0;
		hw->PDRatio = 0; /* NAREN - reset PD ratio to start measuring for new clip */
		hw->PauseThreshold = hw->DefaultPauseThreshold;
		hw->TickSpentInPD = 0;
		rdtscll(hw->TickCntDecodePU);

		dev_dbg(dev, "[FMT CH] DoneSz:0x%x, PIB:%x %x %x %x %x %x %x %x %x %x\n",
			rx_pkt->dio_req->uinfo.y_done_sz * 4,
				 rx_pkt->pib.picture_number,
				 rx_pkt->pib.aspect_ratio,
				 rx_pkt->pib.chroma_format,
				 rx_pkt->pib.colour_primaries,
				 rx_pkt->pib.frame_rate,
				 rx_pkt->pib.height,
				 rx_pkt->pib.width,
				 rx_pkt->pib.n_drop,
				 rx_pkt->pib.pulldown,
				 rx_pkt->pib.ycom);
		rtVal = false;
	}

	if(pPicInfoLine->flags & FLEA_DECODE_ERROR_FLAG)
	{
		*PicNumber = 0;
	} else {
		/* get pic number and flags */
		if (dio->uinfo.b422mode)
			offset = (PicInfoLineNum * hw->PICWidth * 2);
		else
			offset = (PicInfoLineNum * hw->PICWidth);

		res = copy_from_user(dio->pib_va, (void *)(dio->uinfo.xfr_buff+offset), 4);
		if (res != 0)
			goto getpictureinfo_err;

		*PicNumber = *(uint32_t *)(dio->pib_va);
	}

	if(dio->pib_va)
		kfree(dio->pib_va);
	if(tmpPicInfo)
		kfree(tmpPicInfo);

/*	up(&hw->fetch_sem); */

	return rtVal;

getpictureinfo_err:
/*	up(&hw->fetch_sem); */

getpictureinfo_err_nosem:
	if(dio->pib_va)
		kfree(dio->pib_va);
	if(tmpPicInfo)
		kfree(tmpPicInfo);

	*PicNumber = 0;
	*PicMetaData = 0;

	return false;
}

uint32_t flea_GetRptDropParam(struct crystalhd_hw *hw, void* pRxDMAReq)
{
	uint32_t PicNumber = 0,result = 0;
	uint64_t PicMetaData = 0;

	if(flea_GetPictureInfo(hw, (struct crystalhd_rx_dma_pkt *)pRxDMAReq,
		&PicNumber, &PicMetaData))
		result = PicNumber;

	return result;
}

bool crystalhd_flea_notify_event(struct crystalhd_hw *hw, enum BRCM_EVENT EventCode)
{
	switch(EventCode)
	{
		case BC_EVENT_START_CAPTURE:
		{
			crystalhd_flea_wake_up_hw(hw);
			break;
		}
		default:
			break;
	}

	return true;
}
