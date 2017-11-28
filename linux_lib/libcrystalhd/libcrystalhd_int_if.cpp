/********************************************************************
 * Copyright(c) 2006-2009 Broadcom Corporation.
 *
 *  Name: libcrystalhd_int_if.cpp
 *
 *  Description: Driver Internal Interfaces
 *
 *  AU
 *
 *  HISTORY:
 *
 ********************************************************************
 *
 * This file is part of libcrystalhd.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 *
 *******************************************************************/

#include "7411d.h"
#include "bc_defines.h"
#include "bc_decoder_regs.h"
#include "libcrystalhd_priv.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_fwcmds.h"

#include <emmintrin.h>

#define SV_MAX_LINE_SZ 128
#define PCI_GLOBAL_CONTROL MISC2_GLOBAL_CTRL
#define PCI_INT_STS_REG MISC2_INTERNAL_STATUS

// FLEA
#define BCHP_MISC2_GLOBAL_CTRL 0x00502100 /* Global Control Register */
#define BCHP_CLK_TEMP_MON_CTRL 0x00070040 /* Temperature monitor control. */
#define BCHP_CLK_TEMP_MON_STATUS 0x00070044 /* Temperature monitor status. */


//===================================Externs ===========================================
DRVIFLIB_INT_API BC_STATUS
DtsGetHwType(
    HANDLE  hDevice,
    uint32_t     *DeviceID,
    uint32_t     *VendorID,
    uint32_t     *HWRev
    )
{
	BC_HW_TYPE *pHWInfo;
	BC_IOCTL_DATA *pIocData = NULL;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;

	DTS_GET_CTX(hDevice,Ctx);

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	pHWInfo = 	&pIocData->u.hwType;

	pHWInfo->PciDevId = 0xffff;
	pHWInfo->PciVenId =  0xffff;
	pHWInfo->HwRev = 0xff;

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_GET_HWTYPE,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsGetHwType: Ioctl failed: %d\n",sts);
		return sts;
	}

	*DeviceID	= 	pHWInfo->PciDevId;
	*VendorID	=	pHWInfo->PciVenId;
	*HWRev		=	pHWInfo->HwRev;

	// Set these early
	Ctx->DevId 	= 	pHWInfo->PciDevId;
	Ctx->hwRevId = 	pHWInfo->HwRev;
	Ctx->VendorId = pHWInfo->PciVenId;

	DtsRelIoctlData(Ctx,pIocData);

	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API VOID
DtsHwReset(
    HANDLE hDevice
    )
{

	return;
}

DRVIFLIB_INT_API BC_STATUS
DtsSoftReset(
    HANDLE hDevice
    )
{

	uint32_t Val = 0;
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	if(Ctx->DevId == BC_PCI_DEVID_LINK || Ctx->DevId == BC_PCI_DEVID_DOZER)
	{
		DtsDevRegisterWr( hDevice, DecHt_HostSwReset, 0x00000001);	// Assert c011 soft reset
		bc_sleep_ms(50);
		DtsDevRegisterWr( hDevice, DecHt_HostSwReset, 0x00000000  );	// Release c011 soft reset

		/* Disable Stuffing.. */
		DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&Val);
		Val |= BC_BIT(8);
		DtsFPGARegisterWr(hDevice,PCI_GLOBAL_CONTROL,Val);

		//DtsSetCoreClock(hDevice, 0);
	}
	else if(Ctx->DevId == BC_PCI_DEVID_FLEA)
	{
		// For Link, this is used to bring up 7412 and running.
		// Since the 70012 was running in low power mode, but the 7412 was not.
		// In flea there is no need for this. In general most of the chip will be in idle mode,
		// and should not be needed to reset in order to start up.
		// In Flea, we can never do full chip reset, because that will reset PCIe as well and
		// probably cause a BSOD. Individual blocks will have to be reset, either by asserting true resets
		// (but not from the host) or by re-initializing -like the ARM for example.
	}

	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API BC_STATUS
DtsSetLinkIn422Mode(HANDLE hDevice)
{
	uint32_t					Val = 0;
	DTS_LIB_CONTEXT		*Ctx;
	uint32_t					ModeSelect;

	DTS_GET_CTX(hDevice,Ctx);
	ModeSelect = Ctx->b422Mode;

	DebugLog_Trace(LDIL_DBG,"Setting Color Mode to %u\n", Ctx->b422Mode);
	/*
	 * EN_WRITE_ALL BIT -Bit 20
	 * This bit dictates that weather the data will be xferred in
	 * 1 -  UYVY Mode.
	 * 0 - YUY2 Mode.
 	 */
	DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&Val);

	if (ModeSelect == OUTPUT_MODE420) {
        Val &= 0xffeeffff;
	} else {
		Val |= BC_BIT(16);
		if(ModeSelect == OUTPUT_MODE422_UYVY) {
			Val |= BC_BIT(20);
		} else {
			Val &= ~BC_BIT(20);
		}
	}

	DtsFPGARegisterWr(hDevice,PCI_GLOBAL_CONTROL,Val);
	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API BC_STATUS
DtsSetFleaIn422Mode(HANDLE hDevice)
{
	uint32_t			Val = 0;
	DTS_LIB_CONTEXT		*Ctx;
	uint32_t			ModeSelect;

	DTS_GET_CTX(hDevice,Ctx);
	ModeSelect = Ctx->b422Mode;

	// Flea HW only support UYVY/YUY2
	if( ModeSelect != OUTPUT_MODE422_UYVY && ModeSelect != OUTPUT_MODE422_YUY2 )
		return BC_STS_INV_ARG;

	DtsDevRegisterRead(hDevice,BCHP_MISC2_GLOBAL_CTRL,&Val);

	Val &= 0x0000007c;
	if( ModeSelect == OUTPUT_MODE422_YUY2 )
	{
		Val |= BC_BIT(1);  // bit_1  0-> UYVY, 1-> YUY2
	}

	DtsDevRegisterWr(hDevice,BCHP_MISC2_GLOBAL_CTRL,Val);
	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API BC_STATUS
DtsGetConfig(
    HANDLE hDevice,
	BC_DTS_CFG *cfg
    )
{
	DTS_LIB_CONTEXT		*Ctx;

	DTS_GET_CTX(hDevice,Ctx);

	if(!cfg){
		return BC_STS_INV_ARG;
	}
	*cfg = Ctx->CfgFlags;

	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API BC_STATUS
DtsSetConfig(
    HANDLE hDevice,
	BC_DTS_CFG *cfg
    )
{
	DTS_LIB_CONTEXT		*Ctx;

	DTS_GET_CTX(hDevice,Ctx);

	if(!cfg){
		return BC_STS_INV_ARG;
	}

	Ctx->CfgFlags = *cfg;

	return BC_STS_SUCCESS;
}

BC_STATUS
DtsSetCoreClock(
    HANDLE hDevice,
	uint32_t freq
    )
{
//	uint32_t Val=0,clkRate=0, cnt;
	DTS_LIB_CONTEXT		*Ctx;
//	uint32_t DevID,VendorID,Revision;

	uint32_t reg;
	uint32_t n, i;
	uint32_t vco_mg;
	uint32_t refresh_reg;

	DTS_GET_CTX(hDevice,Ctx);
	if(Ctx->DevId != BC_PCI_DEVID_LINK)
	{
		//DebugLog_Trace(LDIL_DBG,"DtsSetCoreClock is not supported in this device\n");
		return BC_STS_ERROR;
	}

#if 0
	if(BC_STS_SUCCESS != DtsGetHwType(hDevice,&DevID,&VendorID,&Revision))	{
		DebugLog_Trace(LDIL_DBG,"Get Hardware Type Failed\n");
		return BC_STS_INV_ARG;
	}

	if(DevID == BC_PCI_DEVID_LINK) {
		// Don't set the core clock
		return BC_STS_SUCCESS;
	}

	if(freq){
		DebugLog_Trace(LDIL_DBG,"DtsSetCoreClock: Custom pll settings not implemented yet.\n");
		return BC_STS_NOT_IMPL;
	}
	if(Ctx->CfgFlags & BC_DEC_VCLK_74MHZ){
		clkRate = 0x000230f0;
	}else if(Ctx->CfgFlags & BC_DEC_VCLK_77MHZ){
		clkRate = 0x000230f2;
	}else{
		return BC_STS_INV_ARG;
	}
#endif
	if(freq == 0)
		return BC_STS_SUCCESS;

	n = freq/5;

	//if (n == Ctx->prev_n)
	//	return BC_STS_CLK_NOCHG;

	if ((n * 27) < 560)
		vco_mg = 0;
	else if ((n * 27) < 900)
		vco_mg = 1;
	else if ((n * 27) < 1030)
		vco_mg = 2;
	else
		vco_mg = 3;

	DtsDevRegisterRead(hDevice,DecHt_PllACtl, &reg);

	reg &= 0xFFFFCFC0;
	reg |= n;
	reg |= vco_mg << 12;

	refresh_reg = (7 * freq / 16);
	DtsDevRegisterWr(hDevice,SDRAM_REF_PARAM,((1 << 12) | refresh_reg));

	DtsDevRegisterWr(hDevice, DecHt_PllACtl, reg);
	DebugLog_Trace(LDIL_DBG,"Clock set to %d\n", freq);
	i = 0;

	while (i < 10) {
		DtsDevRegisterRead(hDevice,DecHt_PllACtl, &reg);

		if (reg & 0x00020000) {
			//Ctx->prev_n = n;
			return BC_STS_SUCCESS;
		}
		else {
			bc_sleep_ms(10);
		}
		i++;
	}

	return BC_STS_ERROR;
}

DRVIFLIB_INT_API BC_STATUS
DtsSetTSMode(
	HANDLE hDevice,
	uint32_t	resv1
	)
{
	uint32_t RegVal = 0;
	DTS_LIB_CONTEXT		*Ctx;
	BOOL TsMode = TRUE;

	DTS_GET_CTX(hDevice,Ctx);

	if(Ctx->DevId != BC_PCI_DEVID_LINK && Ctx->DevId != BC_PCI_DEVID_DOZER)
	{
		DebugLog_Trace(LDIL_DBG,"DtsSetTSMode is not supported in this device\n");
		return BC_STS_ERROR;
	}

	if(Ctx->FixFlags & DTS_LOAD_FILE_PLAY_FW)
		TsMode = FALSE;

	if(TsMode){
		DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&RegVal);
		RegVal &= 0xFFFFFFFE;		//Reset Bit 0
		DtsFPGARegisterWr(hDevice,PCI_GLOBAL_CONTROL,RegVal);
	}else{
		// Set the FPGA up in non TS mode
		DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&RegVal);
		RegVal |= 0x01;		//Set Bit 0
		DtsFPGARegisterWr(hDevice,PCI_GLOBAL_CONTROL,RegVal);
	}

	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API BC_STATUS
DtsSetProgressive(
	HANDLE hDevice,
	uint32_t resv1
	)
{
	uint32_t			RegVal;
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);
	if(Ctx->DevId != BC_PCI_DEVID_LINK && Ctx->DevId != BC_PCI_DEVID_DOZER)
	{
		return BC_STS_SUCCESS;
	}

	// Set the FPGA up in Progressive mode - i.e. 1 vsync/frame
	DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&RegVal);
	RegVal |= 0x10;		//Set Bit 4
	DtsFPGARegisterWr(hDevice,PCI_GLOBAL_CONTROL,RegVal);

	return BC_STS_SUCCESS;
}

BC_STATUS
DtsRstVidClkDLL(
				HANDLE hDevice)
{
	uint32_t RegVal,Cnt=100;

	DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&RegVal);
	RegVal |= 0x08;		//Set Bit 3 the Reset Bit
	DtsFPGARegisterWr(hDevice,PCI_GLOBAL_CONTROL,RegVal);

	//
	// Wait for the bit to go low [Unlock]
	//
	while(Cnt)
	{
		DtsFPGARegisterRead(hDevice,PCI_INT_STS_REG,&RegVal);
		if(!(RegVal | 0x04) )
		{
			break;

		}else{
			bc_sleep_ms(100);
			Cnt--;
		}
	}
	bc_sleep_ms(100);
	DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&RegVal);
	RegVal &= 0xfffffff7;	//reset bit 3
	DtsFPGARegisterWr(hDevice,PCI_GLOBAL_CONTROL,RegVal);
	RegVal=0;
	Cnt =100;
	while(Cnt)
	{
		DtsFPGARegisterRead(hDevice,PCI_INT_STS_REG,&RegVal);
		if(RegVal & 0x04)
		{
			//
			// This means that the video clock is locked.
			//
			return BC_STS_SUCCESS;
		}else{
			bc_sleep_ms(100);
			Cnt--;
		}
	}

	DebugLog_Trace(LDIL_DBG,"DtsSetVideoClock: DLL did not lock.\n");
	return BC_STS_ERROR;
}

DRVIFLIB_INT_API BC_STATUS
DtsSetVideoClock(
    HANDLE hDevice,
	uint32_t freq
    )
{
	uint32_t Val=0;
	uint32_t clkRate = 0;
	DTS_LIB_CONTEXT *Ctx;
	uint32_t DevID,VendorID,Revision;

	DTS_GET_CTX(hDevice,Ctx);

	if(freq){
		DebugLog_Trace(LDIL_DBG,"DtsSetVideoClock: Custom pll settings not implemented yet.\n");
		return BC_STS_NOT_IMPL;
	}

	if(BC_STS_SUCCESS != DtsGetHwType(hDevice,&DevID,&VendorID,&Revision)) {
		DebugLog_Trace(LDIL_DBG,"Get Hardware Type Failed\n");
		return BC_STS_INV_ARG;
	}
	if(DevID == BC_PCI_DEVID_LINK || DevID == BC_PCI_DEVID_FLEA) {
		// Don't set the video clock
		return BC_STS_SUCCESS;
	}

	if(Ctx->CfgFlags & BC_DEC_VCLK_74MHZ){
		// Program PLL-E to 75 MHZ (n = 44, m = 10, vco_rng = 1)
		clkRate = 0x000012AC;
	}else if(Ctx->CfgFlags & BC_DEC_VCLK_77MHZ){
		// Program PLL-E to 77 MHZ (n = ??, m = ??, vco_rng = ??)
		clkRate = 0x000012B0;
	}else{
		return BC_STS_INV_ARG;
	}


	DtsDevRegisterWr( hDevice, DecHt_PllDCtl, 0x00010000);	// Bypass PLL-D

	bc_sleep_ms(50);

	DtsDevRegisterRead( hDevice, DecHt_PllDCtl, &Val);

	if(Val != 0x00030000){
		DebugLog_Trace(LDIL_DBG,"DtsSetVideoClock: Failed to change PLL_D_CTL\n");
		//FIX_ME
		//return BC_STS_NO_ACCESS;
	}

	DtsDevRegisterWr( hDevice, DecHt_PllECtl, clkRate);

	bc_sleep_ms(50);

	DtsDevRegisterRead( hDevice, DecHt_PllECtl, &Val);

	if(Val != (clkRate | 0x00020000) ){
		DebugLog_Trace(LDIL_DBG,"DtsSetVideoClock: Failed to change PLL_E_CTL\n");
		//FIX_ME
		//return BC_STS_NO_ACCESS;
	}

	//if(BC_STS_SUCCESS !=  DtsRstVidClkDLL(hDevice))
	//{
	//	DebugLog_Trace(LDIL_DBG,"DtsSetVideoClock: Vid Clk DLL Failed to Lock\n");
	//	return BC_STS_ERROR;
	//}

	return BC_STS_SUCCESS;
}
DRVIFLIB_INT_API BOOL
DtsIsVideoClockSet(HANDLE hDevice)
{
	uint32_t RegVal=0;
	DTS_LIB_CONTEXT *Ctx = NULL;
	uint32_t DevID,VendorID,Revision;

	DTS_GET_CTX(hDevice,Ctx);

	if(BC_STS_SUCCESS != DtsGetHwType(hDevice,&DevID,&VendorID,&Revision)) {
		DebugLog_Trace(LDIL_DBG,"Get Hardware Type Failed\n");
		return FALSE;
	}
	if(DevID == BC_PCI_DEVID_LINK || DevID == BC_PCI_DEVID_FLEA) {
		// Don't set the video clock
		return FALSE;
	}

	if((Ctx->RegCfg.DbgOptions & BC_BIT(1)) && (Ctx->OpMode == DTS_PLAYBACK_MODE))
		return FALSE;

	DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&RegVal);

	if(RegVal & BC_BIT(0))
		return TRUE;

	DtsFPGARegisterRead(hDevice,PCI_INT_STS_REG,&RegVal);

	return (RegVal & BC_BIT(2));

}

DRVIFLIB_INT_API BC_STATUS
DtsGetPciConfigSpace(
    HANDLE 	hDevice,
    uint8_t          *info
    )
{
	BC_IOCTL_DATA *pIocData = NULL;
	BC_PCI_CFG *pciInfo;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;


	DTS_GET_CTX(hDevice,Ctx);

	if(!info)
	{
		DebugLog_Trace(LDIL_DBG,"DtsGetPciConfigSpace: Invlid Arguments\n");
		return BC_STS_ERROR;
	}

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	pciInfo = (BC_PCI_CFG *)&pIocData->u.pciCfg;

	pciInfo->Size = PCI_CFG_SIZE;
	pciInfo->Offset = 0;
	memset(info,0,	PCI_CFG_SIZE);

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_RD_PCI_CFG,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsGetPciConfigSpace: Ioctl failed: %d\n",sts);
		return sts;
	}

	memcpy(info,pciInfo->pci_cfg_space,PCI_CFG_SIZE);

	DtsRelIoctlData(Ctx,pIocData);

	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API BC_STATUS
DtsDevRegisterRead(
    HANDLE		hDevice,
    uint32_t			offset,
    uint32_t			*Value
    )
{
	BC_IOCTL_DATA *pIocData = NULL;
	BC_CMD_REG_ACC	*reg_acc_read;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;


	DTS_GET_CTX(hDevice,Ctx);

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;


	reg_acc_read = (BC_CMD_REG_ACC *) &pIocData->u.regAcc;

	//
	// Prepare the command here.
	//
	reg_acc_read->Offset	= offset;
	reg_acc_read->Value		= 0;

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_REG_RD,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsDevRegisterRead: Ioctl failed: %d\n",sts);
		return sts;
	}

	*Value = reg_acc_read->Value;

	DtsRelIoctlData(Ctx,pIocData);

	return BC_STS_SUCCESS;
}


DRVIFLIB_INT_API BC_STATUS
DtsDevRegisterWr(
    HANDLE	hDevice,
    uint32_t		offset,
    uint32_t		Value
    )
{
	BC_CMD_REG_ACC	*reg_acc_wr;
	BC_IOCTL_DATA *pIocData = NULL;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;


	DTS_GET_CTX(hDevice, Ctx);

	if (!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	reg_acc_wr = (BC_CMD_REG_ACC *) &pIocData->u.regAcc;

	// Prepare the command here.
	reg_acc_wr->Offset		= offset;
	reg_acc_wr->Value		= Value;

	sts = DtsDrvCmd(Ctx, BCM_IOC_REG_WR, 0, pIocData, FALSE);

	if (sts != BC_STS_SUCCESS)
		DebugLog_Trace(LDIL_DBG,"DtsDevRegisterWr: Ioctl failed: %d\n", sts);

	DtsRelIoctlData(Ctx,pIocData);

	return sts;
}

DRVIFLIB_INT_API BC_STATUS
DtsFPGARegisterRead(
    HANDLE		hDevice,
    uint32_t			offset,
    uint32_t			*Value
    )
{
	BC_IOCTL_DATA *pIocData = NULL;
	BC_CMD_REG_ACC	*reg_acc_read;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;


	DTS_GET_CTX(hDevice,Ctx);

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;


	reg_acc_read = (BC_CMD_REG_ACC *) &pIocData->u.regAcc;

	//
	// Prepare the command here.
	//
	reg_acc_read->Offset	= offset;
	reg_acc_read->Value		= 0;

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_FPGA_RD,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsFPGARegisterRead: Ioctl failed: %d\n",sts);
		return sts;
	}

	*Value = reg_acc_read->Value;

	DtsRelIoctlData(Ctx,pIocData);

	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API BC_STATUS
DtsFPGARegisterWr(
    HANDLE	hDevice,
    uint32_t		offset,
    uint32_t		Value
    )
{
	BC_CMD_REG_ACC	*reg_acc_wr;
	BC_IOCTL_DATA *pIocData = NULL;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;


	DTS_GET_CTX(hDevice,Ctx);

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	reg_acc_wr = (BC_CMD_REG_ACC *) &pIocData->u.regAcc;

	//
	// Prepare the command here.
	//
	reg_acc_wr->Offset		= offset;
	reg_acc_wr->Value		= Value;

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_FPGA_WR,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsFPGARegisterWr: Ioctl failed: %d\n",sts);
		return sts;
	}

	DtsRelIoctlData(Ctx,pIocData);

	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API BC_STATUS
DtsDevMemRd(
    HANDLE	hDevice,
    uint32_t		*Buffer,
    uint32_t		BuffSz,
    uint32_t		Offset
    )
{
	uint8_t					*pXferBuff;
	uint32_t					size_in_dword;
	BC_IOCTL_DATA		*pIoctlData;
	BC_CMD_DEV_MEM		*pMemAccessRd;
	uint32_t					BytesReturned,AllocSz;

	if(!hDevice)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemRd: Invalid Handle\n");
		return BC_STS_INV_ARG;
	}

	if(!Buffer)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemRd: Null Buffer\n");
		return BC_STS_INV_ARG;
	}

	if(BuffSz % 4)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemRd: Buff Size is not a multiple of DWORD\n");
		return BC_STS_ERROR;
	}


	AllocSz = sizeof(BC_IOCTL_DATA) + (BuffSz);

	pIoctlData = (BC_IOCTL_DATA *) malloc(AllocSz);

	if(!pIoctlData)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemRd: Memory Allocation Failed\n");
		return BC_STS_ERROR;
	}

	pXferBuff = ( ((PUCHAR)pIoctlData) + sizeof(BC_IOCTL_DATA));

	pMemAccessRd =  &pIoctlData->u.devMem;
	size_in_dword = BuffSz / 4;
	pIoctlData->RetSts = BC_STS_ERROR;
	pIoctlData->IoctlDataSz = sizeof(BC_IOCTL_DATA);
	pMemAccessRd->StartOff = Offset;
	memset(pXferBuff,'a',BuffSz);
	/* The size is passed in Bytes*/
	pMemAccessRd->NumDwords = size_in_dword;
	if(!DtsDrvIoctl(hDevice,
					BCM_IOC_MEM_RD,
					pIoctlData,
					AllocSz,
					pIoctlData,
					AllocSz,
					(LPDWORD)&BytesReturned,
					0))
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemRd: DeviceIoControl Failed\n");
		return BC_STS_ERROR;
	}

	if(BC_STS_ERROR == pIoctlData->RetSts)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemRd: IOCTL Cmd Failed By Driver\n");
		return pIoctlData->RetSts;
	}

	memcpy(Buffer,pXferBuff,BuffSz);

	if(pIoctlData)
		free(pIoctlData);

	return BC_STS_SUCCESS;
}



DRVIFLIB_INT_API BC_STATUS
DtsDevMemWr(
    HANDLE	hDevice,
    uint32_t		*Buffer,
    uint32_t		BuffSz,
    uint32_t		Offset
    )
{
	uint8_t					*pXferBuff;
	uint32_t					size_in_dword;
	BC_IOCTL_DATA		*pIoctlData;
	BC_CMD_DEV_MEM		*pMemAccessRd;
	uint32_t					BytesReturned,AllocSz;

	//pIoctlData = (BC_IOCTL_DATA *)malloc(sizeof(BC_IOCTL_DATA));


	if(!hDevice)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemWr: Invalid Handle\n");
		return BC_STS_INV_ARG;
	}

	if(!Buffer)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemWr: Null Buffer\n");
		return BC_STS_INV_ARG;
	}

	if(BuffSz % 4)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemWr: Buff Size is not a multiple of DWORD\n");
		return BC_STS_ERROR;
	}


	AllocSz = sizeof(BC_IOCTL_DATA) + (BuffSz);

	pIoctlData = (BC_IOCTL_DATA *) malloc(AllocSz);

	if(!pIoctlData)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemWr: Memory Allocation Failed\n");
		return BC_STS_ERROR;
	}

	pXferBuff = ( ((PUCHAR)pIoctlData) + sizeof(BC_IOCTL_DATA));

	pMemAccessRd =  &pIoctlData->u.devMem;
	size_in_dword = BuffSz / 4;
	pIoctlData->RetSts = BC_STS_ERROR;
	pIoctlData->IoctlDataSz = sizeof(BC_IOCTL_DATA);
	pMemAccessRd->StartOff = Offset;
	memcpy(pXferBuff,Buffer,BuffSz);
	/* The size is passed in Bytes*/
	pMemAccessRd->NumDwords = size_in_dword;
	if(!DtsDrvIoctl(hDevice,
					BCM_IOC_MEM_WR,
					pIoctlData,
					AllocSz,
					pIoctlData,
					AllocSz,
					(LPDWORD)&BytesReturned,
					0))
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemWr: DeviceIoControl Failed\n");
		return BC_STS_ERROR;
	}

	if(BC_STS_ERROR == pIoctlData->RetSts)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemWr: IOCTL Cmd Failed By Driver\n");
		return pIoctlData->RetSts;
	}

	if(pIoctlData)
		free(pIoctlData);

	return BC_STS_SUCCESS;

}

DRVIFLIB_INT_API BC_STATUS
DtsTxDmaText( HANDLE  hDevice ,
				 uint8_t *pUserData,
				 uint32_t ulSizeInBytes,
				 uint32_t *dramOff,
				 uint8_t Encrypted)
{
	BC_STATUS status = BC_STS_SUCCESS;
	uint32_t		ulDmaSz;
	uint8_t		*pDmaBuff;

	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_IOCTL_DATA		*pIocData = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	if( (!pUserData) || (!ulSizeInBytes) || !dramOff)
	{
		return BC_STS_INV_ARG;
	}

	pDmaBuff = pUserData;
	ulDmaSz = ulSizeInBytes;

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	pIocData->RetSts = BC_STS_ERROR;
	pIocData->IoctlDataSz = sizeof(BC_IOCTL_DATA);
	pIocData->u.ProcInput.DramOffset =0;
	pIocData->u.ProcInput.pDmaBuff = pDmaBuff;
	pIocData->u.ProcInput.BuffSz = ulDmaSz;
	pIocData->u.ProcInput.Mapped = FALSE;
	pIocData->u.ProcInput.Encrypted = Encrypted;
	if(Ctx->VidParams.VideoAlgo == BC_VID_ALGO_VC1MP)
		pIocData->u.ProcInput.Encrypted|=0x2;


	status = DtsDrvCmd(Ctx,BCM_IOC_PROC_INPUT,1,pIocData,FALSE);

	*dramOff = pIocData->u.ProcInput.DramOffset;

	if( BC_STS_SUCCESS != status && BC_STS_IO_USER_ABORT != status)
	{
		DebugLog_Trace(LDIL_DBG,"DtsTxDmaText: DeviceIoControl Failed with Sts %d\n", status);
	}

	DtsRelIoctlData(Ctx,pIocData);

	DumpInputSampleToFile(pUserData,ulSizeInBytes);

	return status;
}

DRVIFLIB_INT_API BC_STATUS
DtsCancelProcOutput(
    HANDLE  hDevice,
	PVOID	Context)
{
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	return DtsCancelFetchOutInt(Ctx);
}


//------------------------------------------------------------------------
// Name: DtsChkYUVSizes
// Description: Check Src/Dst buffer sizes with configured resolution.
//
// Vin:  Strtucture received from HW
// Vout: Structure got from App (Where data need to be copied)
//
//------------------------------------------------------------------------
DRVIFLIB_INT_API BC_STATUS DtsChkYUVSizes(
	DTS_LIB_CONTEXT *Ctx,
	BC_DTS_PROC_OUT *Vout,
	BC_DTS_PROC_OUT *Vin)
{
	if (!Vout || !Vout->Ybuff || !Vin || !Vin->Ybuff){
		return BC_STS_INV_ARG;
	}
	if((!Ctx->b422Mode) && (!Vout->UVbuff || !Vin->UVbuff)){
		return BC_STS_INV_ARG;
	}

	/* Pass on size info irrespective of the status (for debug) */
	Vout->YBuffDoneSz = Vin->YBuffDoneSz;
	Vout->UVBuffDoneSz = Vin->UVBuffDoneSz;

	/* Does Driver qualifies this condition before setting _PIB_VALID flag??..*/
	if( !( Vin->YBuffDoneSz) || ((!Ctx->b422Mode) && (!Vin->UVBuffDoneSz)) ){
		DebugLog_Trace(LDIL_DBG,"DtsChkYUVSizes: Incomplete Transfer\n");
		return BC_STS_IO_XFR_ERROR;
	}
/*
	Let the upper layer take care of this
	if(!(Vin->PoutFlags & BC_POUT_FLAGS_PIB_VALID)){
		DebugLog_Trace(LDIL_DBG,"DtsChkYUVSizes: PIB not Valid\n");
		return BC_STS_IO_XFR_ERROR;
	}*/

	return BC_STS_SUCCESS;
}

/***/

DRVIFLIB_INT_API BC_STATUS
DtsGetDrvStat(
    HANDLE		hDevice,
	BC_DTS_STATS *pDrvStat
    )
{
	BC_IOCTL_DATA *pIocData = NULL;
	BC_DTS_STATS *pIntDrvStat;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;
//	float		fTemperature = 0;

	DTS_GET_CTX(hDevice,Ctx);

	if(!pDrvStat)
	{
		DebugLog_Trace(LDIL_DBG,"DtsGetDrvStat: Invlid Arguments\n");
		return BC_STS_ERROR;
	}

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	if(Ctx->SingleThreadedAppMode)
		pIocData->u.drvStat.DrvNextMDataPLD = pDrvStat->DrvNextMDataPLD;

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_GET_DRV_STAT,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsGetDriveStats: Ioctl failed: %d\n",sts);
		return sts;
	}

	/* DIL counters */
	pIntDrvStat = DtsGetgStats ( );
	//memcpy_s(pDrvStat, 128, pIntDrvStat, 128);
	memcpy(pDrvStat, pIntDrvStat, 128);

	/* Driver counters */
	pIntDrvStat = (BC_DTS_STATS *)&pIocData->u.drvStat;
	pDrvStat->drvRLL = pIntDrvStat->drvRLL;
	pDrvStat->drvFLL = pIntDrvStat->drvFLL;
	pDrvStat->intCount = pIntDrvStat->intCount;
	pDrvStat->pauseCount = pIntDrvStat->pauseCount;
	pDrvStat->DrvIgnIntrCnt = pIntDrvStat->DrvIgnIntrCnt;
	pDrvStat->DrvTotalFrmDropped = pIntDrvStat->DrvTotalFrmDropped;
	pDrvStat->DrvTotalHWErrs = pIntDrvStat->DrvTotalHWErrs;
	pDrvStat->DrvTotalPIBFlushCnt = pIntDrvStat->DrvTotalPIBFlushCnt;
	pDrvStat->DrvTotalFrmCaptured = pIntDrvStat->DrvTotalFrmCaptured;
	pDrvStat->DrvPIBMisses = pIntDrvStat->DrvPIBMisses;
	pDrvStat->DrvPauseTime = pIntDrvStat->DrvPauseTime;
	pDrvStat->DrvRepeatedFrms = pIntDrvStat->DrvRepeatedFrms;
	pDrvStat->TxFifoBsyCnt = pIntDrvStat->TxFifoBsyCnt;
	pDrvStat->pwr_state_change = pIntDrvStat->pwr_state_change;
	pDrvStat->DrvNextMDataPLD = pIntDrvStat->DrvNextMDataPLD;
	pDrvStat->DrvcpbEmptySize = pIntDrvStat->DrvcpbEmptySize;
	pDrvStat->eosDetected = pIntDrvStat->eosDetected;
	pDrvStat->picNumFlags = pIntDrvStat->picNumFlags;


//

	DtsRelIoctlData(Ctx,pIocData);

	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API BC_STATUS
DtsSetTemperatureMeasure(
    HANDLE			hDevice,
	BOOL			bTurnOn
    )
{
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;
	uint32_t	Val = 0;

	DTS_GET_CTX(hDevice,Ctx);
	if( Ctx->DevId != BC_PCI_DEVID_FLEA )
	{
		DebugLog_Trace(LDIL_DBG,"DtsSetTemperatureMeasure Only support for Flea.\n");
		return BC_STS_SUCCESS;
	}

	if( bTurnOn )
	{
		Val = 0x3; //
		sts = DtsDevRegisterWr(hDevice,BCHP_CLK_TEMP_MON_CTRL,Val);
		bc_sleep_ms(10);

		Val = 0x203; //
		sts = DtsDevRegisterWr(hDevice,BCHP_CLK_TEMP_MON_CTRL,Val);
		bc_sleep_ms(10);
	}
	else
	{
		Val = 0x103; //
		sts = DtsDevRegisterWr(hDevice,BCHP_CLK_TEMP_MON_CTRL,Val);
		bc_sleep_ms(10);
	}
	return sts;
}

DRVIFLIB_INT_API BC_STATUS
DtsGetCoreTemperature(
    HANDLE			hDevice,
	float			*pTemperature
    )
{
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_ERROR;
	uint32_t	Val = 0;
	*pTemperature = 0;

	DTS_GET_CTX(hDevice,Ctx);
	if( Ctx->DevId != BC_PCI_DEVID_FLEA )
	{
		DebugLog_Trace(LDIL_DBG,"DtsSetTemperatureMeasure Only support for Flea.\n");
		return BC_STS_SUCCESS;
	}

	sts = DtsDevRegisterRead(hDevice,BCHP_CLK_TEMP_MON_STATUS,&Val);
	if( sts != BC_STS_SUCCESS )
		return sts;
	Val = Val & 0x0000ffff;

	*pTemperature = 267.2 - 0.7 * (float)Val;

	return sts;
}


DRVIFLIB_INT_API BC_STATUS
DtsRstDrvStat(
    HANDLE		hDevice
    )
{
	BC_IOCTL_DATA *pIocData = NULL;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;

	DTS_GET_CTX(hDevice,Ctx);

	/* CHECK WHETHER NULL pIocData CAN BE PASSED */
	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	/* Driver related counters */
	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_RST_DRV_STAT,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsRstDrvStats: Ioctl failed: %d\n",sts);
		return sts;
	}

	/* DIL related counters */
	DtsRstStats( );

	DtsRelIoctlData(Ctx,pIocData);

	return BC_STS_SUCCESS;
}

/**/
/* Get firmware files */
DRVIFLIB_INT_API BC_STATUS
DtsGetFWFiles(
	HANDLE hDevice,
	char *StreamFName,
	char *VDecOuter,
	char *VDecInner
	)
{
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;

	DTS_GET_CTX(hDevice,Ctx);

	sts = DtsGetFirmwareFiles(Ctx);
	if(sts == BC_STS_SUCCESS){
		strncpy(StreamFName,  Ctx->StreamFile, MAX_PATH);
		strncpy(VDecOuter,  Ctx->VidOuter, MAX_PATH);
		strncpy(VDecInner, Ctx->VidInner, MAX_PATH );
	}else{
		return sts;
	}

	return sts;
}
/**/

DRVIFLIB_INT_API BC_STATUS
DtsDownloadFWBin(HANDLE	hDevice, uint8_t *binBuff, uint32_t buffsize, uint32_t dramOffset)
{
	BC_STATUS rstatus = BC_STS_SUCCESS;

	/* Write bootloader vector table section */
	rstatus = DtsDevMemWr(hDevice,(uint32_t *)binBuff,buffsize,dramOffset);
	if (BC_STS_SUCCESS != rstatus)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDownloadFWBin: Fw Download Failed\n");
	}

	return rstatus;
}

BC_STATUS
DtsCopyRawDataToOutBuff(DTS_LIB_CONTEXT	*Ctx,
						BC_DTS_PROC_OUT *Vout,
						BC_DTS_PROC_OUT *Vin)
{
	uint32_t	y,lDestStride=0;
	uint8_t	*pSrc = NULL, *pDest=NULL;
	uint32_t	dstWidthInPixels, dstHeightInPixels;
	uint32_t srcWidthInPixels = 0;
	BC_STATUS	Sts = BC_STS_SUCCESS;

	if ( (Sts = DtsChkYUVSizes(Ctx,Vout,Vin)) != BC_STS_SUCCESS)
		return Sts;

	if(Vout->PoutFlags & BC_POUT_FLAGS_STRIDE)
		lDestStride = Vout->StrideSz;

	if(Vout->PoutFlags & BC_POUT_FLAGS_SIZE) {
		// Use DShow provided size for now
		dstWidthInPixels = Vout->PicInfo.width;
		if(!Ctx->VidParams.Progressive)
			dstHeightInPixels = Vout->PicInfo.height/2;
		else
			dstHeightInPixels = Vout->PicInfo.height;
		/* Check for Valid data based on the filter information */
/* interlaced frames currently don't get delivered from the library if this check is in place */
#if 0
		if(Vout->YBuffDoneSz < (dstWidthInPixels * dstHeightInPixels / 2)) {
			DebugLog_Trace(LDIL_DBG,"DtsCopy422: XFER ERROR dnsz %u, w %u, h %u\n", Vout->YBuffDoneSz, dstWidthInPixels, dstHeightInPixels);
			return BC_STS_IO_XFR_ERROR;
		}
#endif
		srcWidthInPixels = Ctx->HWOutPicWidth;
	} else {
		dstWidthInPixels = Vin->PicInfo.width;
		dstHeightInPixels = Vin->PicInfo.height;
	}

	lDestStride = lDestStride*2;
	dstWidthInPixels = dstWidthInPixels*2;
	srcWidthInPixels = srcWidthInPixels*2;
	// Do a strided copy only if the stride is non-zero
	if( (lDestStride != 0)|| (srcWidthInPixels != dstWidthInPixels) ) {
		// Y plane
		pDest = Vout->Ybuff;
		pSrc = Vin->Ybuff;
		for (y = 0; y < dstHeightInPixels; y++){
			memcpy(pDest,pSrc,dstWidthInPixels);
			//memcpy_fast(pDest,pSrc,dstWidthInPixels*2);
			pDest += dstWidthInPixels + lDestStride;
			pSrc += (srcWidthInPixels);
		}
	} else {
		// Y Plane
		memcpy(Vout->Ybuff, Vin->Ybuff, dstHeightInPixels * dstWidthInPixels);
		//memcpy_fast(Vout->Ybuff, Vin->Ybuff, dstHeightInPixels * dstWidthInPixels * 2);
	}

	return BC_STS_SUCCESS;
}
/***/
//FIX_ME:: This routine assumes, Y & UV buffs are contiguous..
BC_STATUS DtsCopyNV12ToYV12(DTS_LIB_CONTEXT	*Ctx, BC_DTS_PROC_OUT *Vout, BC_DTS_PROC_OUT *Vin)
{

	uint8_t	*buff=NULL;
	uint8_t	*yv12buff = NULL;
	uint32_t uvbase=0;
	BC_STATUS	Sts = BC_STS_SUCCESS;
	uint32_t	x,y,lDestStrideY=0, lDestStrideUV=0;
	uint8_t	*pSrc = NULL, *pDest=NULL;
	uint32_t	dstWidthInPixels, dstHeightInPixels;
	uint32_t srcWidthInPixels;


	if ( (Sts = DtsChkYUVSizes(Ctx,Vout,Vin)) != BC_STS_SUCCESS)
		return Sts;

	if(Vout->PoutFlags & BC_POUT_FLAGS_SIZE)// needs to be optimized.
	{
		if(Vout->PoutFlags & BC_POUT_FLAGS_STRIDE)
			lDestStrideUV = (lDestStrideY = Vout->StrideSz)/2;
		if(Vout->PoutFlags & BC_POUT_FLAGS_STRIDE_UV)
			lDestStrideUV = Vout->StrideSzUV;

		// Use DShow provided size for now
		dstWidthInPixels = Vout->PicInfo.width;
		if(!Ctx->VidParams.Progressive)
			dstHeightInPixels = Vout->PicInfo.height/2;
		else
			dstHeightInPixels = Vout->PicInfo.height;

		/* Check for Valid data based on the filter information */
		if((Vout->YBuffDoneSz < (dstWidthInPixels * dstHeightInPixels / 4)) ||
			(Vout->UVBuffDoneSz < (dstWidthInPixels * dstHeightInPixels/2 / 4)))
		{
			DebugLog_Trace(LDIL_DBG,"DtsCopyYV12: XFER ERROR\n");
			return BC_STS_IO_XFR_ERROR;
		}
		srcWidthInPixels = Ctx->HWOutPicWidth;

		//copy luma
		pDest = Vout->Ybuff;
		pSrc = Vin->Ybuff;
		for (y = 0; y < dstHeightInPixels; y++)
		{
			memcpy(pDest,pSrc,dstWidthInPixels);
			pDest += dstWidthInPixels + lDestStrideY;
			pSrc += srcWidthInPixels;
		}
		//copy chroma
		pDest = Vout->UVbuff;
		pSrc = Vin->UVbuff;
		uvbase = (dstWidthInPixels + lDestStrideY) * dstHeightInPixels/4 ;//(Vin->UVBuffDoneSz * 4/2);
		for (y = 0; y < dstHeightInPixels/2; y++)
		{
			for(x = 0; x < dstWidthInPixels; x += 2)
			{
				pDest[x/2] = pSrc[x+1];
				pDest[uvbase + x/2] = pSrc[x];
			}
			pDest += dstWidthInPixels / 2 + lDestStrideUV;
			pSrc += srcWidthInPixels;
		}
	}
	else
	{
		/* Y-Buff loop */
		buff = Vin->Ybuff;
		yv12buff = Vout->Ybuff;
		for(uint32_t i = 0; i < Vin->YBuffDoneSz*4; i += 2) {
			yv12buff[i] = buff[i];
			yv12buff[i+1] = buff[i+1];
		}

		/* UV-Buff loop */
		buff = Vin->UVbuff;
		yv12buff = Vout->UVbuff;
		uvbase = (Vin->UVBuffDoneSz * 4/2);
		for(uint32_t i = 0; i < Vin->UVBuffDoneSz*4; i += 2) {
			yv12buff[i/2] = buff[i+1];
			yv12buff[uvbase + (i/2)] = buff[i];
		}
	}

	return BC_STS_SUCCESS;
}


BC_STATUS DtsCopyNV12(DTS_LIB_CONTEXT *Ctx, BC_DTS_PROC_OUT *Vout, BC_DTS_PROC_OUT *Vin)
{
	uint32_t y,lDestStrideY=0,lDestStrideUV=0;
	uint8_t	*pSrc = NULL, *pDest=NULL;
	uint32_t dstWidthInPixels, dstHeightInPixels;
	uint32_t srcWidthInPixels=0;

	BC_STATUS	Sts = BC_STS_SUCCESS;

	if ( (Sts = DtsChkYUVSizes(Ctx,Vout,Vin)) != BC_STS_SUCCESS)
		return Sts;

	if(Vout->PoutFlags & BC_POUT_FLAGS_STRIDE)
		lDestStrideUV = lDestStrideY = Vout->StrideSz;
	if(Vout->PoutFlags & BC_POUT_FLAGS_STRIDE_UV)
		lDestStrideUV = Vout->StrideSzUV;

	if(Vout->PoutFlags & BC_POUT_FLAGS_SIZE) {
		// Use DShow provided size for now
		dstWidthInPixels = Vout->PicInfo.width;
		if(!Ctx->VidParams.Progressive)
			dstHeightInPixels = Vout->PicInfo.height/2;
		else
			dstHeightInPixels = Vout->PicInfo.height;
		/* Check for Valid data based on the filter information */
		if((Vout->YBuffDoneSz < (dstWidthInPixels * dstHeightInPixels / 4)) ||
			(Vout->UVBuffDoneSz < (dstWidthInPixels * dstHeightInPixels/2 / 4)))
			return BC_STS_IO_XFR_ERROR;
		srcWidthInPixels = Ctx->HWOutPicWidth;
	} else {
		dstWidthInPixels = Vin->PicInfo.width;
		dstHeightInPixels = Vin->PicInfo.height;
	}

	// NV12 is planar: Y plane, followed by packed U-V plane.

	// Do a strided copy only if the stride is non-zero
	if((lDestStrideY != 0) || (lDestStrideUV != 0) || (srcWidthInPixels != dstWidthInPixels)) {
		// Y plane
		pDest = Vout->Ybuff;
		pSrc = Vin->Ybuff;
		for (y = 0; y < dstHeightInPixels; y++){
			memcpy(pDest,pSrc,dstWidthInPixels);
			pDest += dstWidthInPixels + lDestStrideY;
			pSrc += srcWidthInPixels;
		}
	// U-V plane
		pDest = Vout->UVbuff;
		pSrc = Vin->UVbuff;
		for (y = 0; y < dstHeightInPixels/2; y++){
			memcpy(pDest,pSrc,dstWidthInPixels);
			pDest += dstWidthInPixels + lDestStrideUV;
			pSrc += srcWidthInPixels;
		}
	} else {
		// Y Plane
		memcpy(Vout->Ybuff, Vin->Ybuff, dstHeightInPixels * dstWidthInPixels);
		// UV Plane
		memcpy(Vout->UVbuff, Vin->UVbuff, dstHeightInPixels/2 * dstWidthInPixels);

	}

	return BC_STS_SUCCESS;
}

// TODO: add sse2 detection
static bool gSSE2 = true; // most of the platforms will have it anyway:
// 64 bits: no test necessary
// mac: no test necessary
// linux/windows: we might have to do the test.

static void fast_memcpy(uint8_t *dst, const uint8_t *src, uint32_t count)
{
	// tested
	if (gSSE2)
	{
		if (((((uintptr_t) dst) & 0xf) == 0) && ((((uintptr_t) src) & 0xf) == 0))
		{
			while (count >= (16*4))
			{
				_mm_stream_si128((__m128i *) (dst+ 0*16),  _mm_load_si128((__m128i *) (src+ 0*16)));
				_mm_stream_si128((__m128i *) (dst+ 1*16),  _mm_load_si128((__m128i *) (src+ 1*16)));
				_mm_stream_si128((__m128i *) (dst+ 2*16),  _mm_load_si128((__m128i *) (src+ 2*16)));
				_mm_stream_si128((__m128i *) (dst+ 3*16),  _mm_load_si128((__m128i *) (src+ 3*16)));
				count -= 16*4;
				src += 16*4;
				dst += 16*4;
			}
		}
		else
		{
			while (count >= (16*4))
			{
				_mm_storeu_si128((__m128i *) (dst+ 0*16),  _mm_loadu_si128((__m128i *) (src+ 0*16)));
				_mm_storeu_si128((__m128i *) (dst+ 1*16),  _mm_loadu_si128((__m128i *) (src+ 1*16)));
				_mm_storeu_si128((__m128i *) (dst+ 2*16),  _mm_loadu_si128((__m128i *) (src+ 2*16)));
				_mm_storeu_si128((__m128i *) (dst+ 3*16),  _mm_loadu_si128((__m128i *) (src+ 3*16)));
				count -= 16*4;
				src += 16*4;
				dst += 16*4;
			}
		}
	}

	while (count --)
		*dst++ = *src++;
}

#if 0
// this is not good.
// if we have 3 buffers, we cannot assume V is after U
static BC_STATUS DtsCopy422ToYV12(uint8_t *dstY, uint8_t *dstUV, const uint8_t *srcY, uint32_t srcWidth, uint32_t dstWidth, uint32_t height, uint32_t strideY, uint32_t strideUV)
{ // copy YUY2 to YV12
	// TODO
	// NOTE: if we want to support this porperly, we will need to add a Vbuffer pointer
	// if we have 3 destination buffers, there's no guarantee that V buffer is derivable from UV pointer.
	return BC_STS_INV_ARG;
}
#endif /*0*/

// this is just a memcpy
static BC_STATUS DtsCopy422ToYUY2(uint8_t *dstY, uint8_t *dstUV, const uint8_t *srcY, uint32_t srcWidth, uint32_t dstWidth, uint32_t height, uint32_t strideY, uint32_t strideUV)
{ // copy YUY2 to YUY2
	uint32_t y;

	// TODO: test this
	strideY += dstWidth*2;

	for (y = 0; y < height; y++)
	{
		fast_memcpy(dstY, srcY, srcWidth*2);
		srcY += srcWidth*2;
		dstY += strideY;
	}
	return BC_STS_SUCCESS;
}

// almost a memcpy, we just need to shuffle YUV's around
static BC_STATUS DtsCopy422ToUYVY(uint8_t *dstY, uint8_t *dstUV, const uint8_t *srcY, uint32_t srcWidth, uint32_t dstWidth, uint32_t height, uint32_t strideY, uint32_t strideUV)
{
	// TODO, test this
	uint32_t x = 0, __y;

	strideY += dstWidth*2;

	for (__y = 0; __y < height; __y++)
	{
		if (gSSE2)
		{
			if (((((uintptr_t) dstY) & 0xf) == 0) && ((((uintptr_t) srcY) & 0xf) == 0))
			{
				while (x < srcWidth-7)
				{
					__m128i v = _mm_load_si128((__m128i *)(srcY+x*2));
					__m128i v1 = _mm_srli_epi16(v, 8);
					__m128i v2 = _mm_slli_epi16(v, 8);
					_mm_stream_si128((__m128i *)(dstY+x*2), _mm_or_si128(v1, v2));
					x += 8;
				}
			}
			else
			{
				while (x < srcWidth-7)
				{
					__m128i v = _mm_loadu_si128((__m128i *)(srcY+x*2));
					__m128i v1 = _mm_srli_epi16(v, 8);
					__m128i v2 = _mm_slli_epi16(v, 8);
					_mm_storeu_si128((__m128i *)(dstY+x*2), _mm_or_si128(v1, v2));
					x += 8;
				}
			}
		}

		while (x < srcWidth-1)
		{
			dstY[x*2+0] = srcY[x+1];
			dstY[x*2+1] = srcY[x+0];
			dstY[x*2+2] = srcY[x+3];
			dstY[x*2+3] = srcY[x+2];
			x += 2;
		}

		srcY += srcWidth*2;
		dstY += strideY;
	}
	return BC_STS_SUCCESS;
}

// convert to NV12
static BC_STATUS DtsCopy422ToNV12(uint8_t *dstY, uint8_t *dstUV, const uint8_t *srcY, uint32_t srcWidth, uint32_t dstWidth, uint32_t height, uint32_t strideY, uint32_t strideUV)
{
	// tested
	uint32_t x, __y;

	strideY += dstWidth;
	strideUV += dstWidth;

	static __m128i mask = _mm_set_epi16(0x00ff, 0x00ff, 0x00ff, 0x00ff, 0x00ff, 0x00ff, 0x00ff, 0x00ff);

	for (__y = 0; __y < height; __y += 2)
	{
		x = 0;

		// first line: Y and UV extraction

		if (gSSE2)
		{
			if (((((uintptr_t) dstY) & 0xf) == 0) && ((((uintptr_t) srcY) & 0xf) == 0) && ((((uintptr_t) dstUV) & 0xf) == 0))
			{
				while (x < srcWidth-15)
				{
					__m128i s1 = _mm_load_si128((__m128i *) (srcY+x*2+ 0)); // load 8 pixels
					__m128i s2 = _mm_load_si128((__m128i *) (srcY+x*2+16)); // load 8 more

					__m128i y1 = _mm_and_si128(s1, mask); // mask out uvs
					__m128i y2 = _mm_and_si128(s2, mask); // mask out uvs
					__m128i y = _mm_packus_epi16 (y1, y2); // get the y together
					_mm_stream_si128((__m128i *) (dstY+x), y); // store 16 Y

					s1 = _mm_srli_epi16(s1, 8); // get rid of Y
					s2 = _mm_srli_epi16(s2, 8); // get rid of Y
					__m128i uv = _mm_packus_epi16 (s1, s2); // get the uv together
					_mm_stream_si128((__m128i *) (dstUV+x), uv); // store 8 UV pairs

					x += 16;
				}
			}
			else
			{
				while (x < srcWidth-15)
				{
					__m128i s1 = _mm_loadu_si128((__m128i *) (srcY+x*2+ 0)); // load 8 pixels
					__m128i s2 = _mm_loadu_si128((__m128i *) (srcY+x*2+16)); // load 8 more

					__m128i y1 = _mm_and_si128(s1, mask); // mask out uvs
					__m128i y2 = _mm_and_si128(s2, mask); // mask out uvs
					__m128i y = _mm_packus_epi16 (y1, y2); // get the y together
					_mm_storeu_si128((__m128i *) (dstY+x), y); // store 16 Y

					s1 = _mm_srli_epi16(s1, 8); // get rid of Y
					s2 = _mm_srli_epi16(s2, 8); // get rid of Y
					__m128i uv = _mm_packus_epi16 (s1, s2); // get the uv together
					_mm_storeu_si128((__m128i *) (dstUV+x), uv); // store 8 UV pairs

					x += 16;
				}
			}
		}


		while (x < srcWidth-1)
		{
			dstY [x+0] = srcY[x*2+0]; // Y
			dstUV[x+0] = srcY[x*2+1]; // U
			dstY [x+1] = srcY[x*2+2]; // Y
			dstUV[x+1] = srcY[x*2+3]; // V
			x += 2;
		}

		srcY += srcWidth*2;
		dstY += strideY;
		dstUV += strideUV;

		// second line: just Y
		x = 0;
		if (gSSE2)
		{
			if (((((uintptr_t) dstY) & 0xf) == 0) && ((((uintptr_t) srcY) & 0xf) == 0))
			{
				while (x < srcWidth-15)
				{
					__m128i s1 = _mm_load_si128((__m128i *) (srcY+x*2+ 0)); // load 8 pixels
					__m128i s2 = _mm_load_si128((__m128i *) (srcY+x*2+16)); // load 8 more

					__m128i y1 = _mm_and_si128(s1, mask); // mask out uvs
					__m128i y2 = _mm_and_si128(s2, mask); // mask out uvs
					__m128i y = _mm_packus_epi16 (y1, y2); // get the y
					_mm_stream_si128((__m128i *) (dstY+x), y); // store 16 Y

					x += 16;
				}
			}
			else
			{
				while (x < srcWidth-15)
				{
					__m128i s1 = _mm_loadu_si128((__m128i *) (srcY+x*2+ 0)); // load 8 pixels
					__m128i s2 = _mm_loadu_si128((__m128i *) (srcY+x*2+16)); // load 8 more

					__m128i y1 = _mm_and_si128(s1, mask); // mask out uvs
					__m128i y2 = _mm_and_si128(s2, mask); // mask out uvs
					__m128i y = _mm_packus_epi16 (y1, y2); // get the y
					_mm_storeu_si128((__m128i *) (dstY+x), y); // store 16 Y

					x += 16;
				}
			}
		}

		while (x < srcWidth-1)
		{
			dstY [x+0] = srcY[x*2+0]; // Y
			dstY [x+1] = srcY[x*2+2]; // Y
			x += 2;
		}

		srcY += srcWidth*2;
		dstY += strideY;
	}
	return BC_STS_SUCCESS;
}


#if 0
// this is not good.
// if we have 3 textures, we cannot assume V is after U
static BC_STATUS DtsCopy420ToYV12(uint8_t *dstY, uint8_t *dstUV, const uint8_t *srcY, const uint8_t *srcUV, uint32_t srcWidth, uint32_t dstWidth, uint32_t height, uint32_t strideY, uint32_t strideUV)
{
	// TODO
	// NOTE: if we want to support this porperly, we will need to add a Vbuffer pointer
	return BC_STS_INV_ARG;
}
#endif /*0*/

static BC_STATUS DtsCopy420ToYUY2(uint8_t *dstY, uint8_t *dstUV, const uint8_t *srcY, const uint8_t *srcUV, uint32_t srcWidth, uint32_t dstWidth, uint32_t height, uint32_t strideY, uint32_t strideUV)
{
	// TODO, test this
	uint32_t x, __y;

	strideY += dstWidth*2;

	__y = 0;
	while (__y < height-2)
	{
		// first line
		x = 0;

		if (gSSE2)
		{
			if (((((uintptr_t) dstY) & 0xf) == 0) && ((((uintptr_t) srcY) & 0xf) == 0))
			{
				while (x < srcWidth-15)
				{
					__m128i y = _mm_load_si128((__m128i *) (srcY+x)); // load 16 Y pixels
					__m128i uv = _mm_load_si128((__m128i *) (srcUV+x)); // load 8 UV
					_mm_stream_si128((__m128i *) (dstY+x*2+ 0), _mm_unpacklo_epi8(y, uv)); // store 8 pixels
					_mm_stream_si128((__m128i *) (dstY+x*2+16), _mm_unpackhi_epi8(y, uv)); // store 8 pixels

					x += 16;
				}
			}
			else
			{
				while (x < srcWidth-15)
				{
					__m128i y = _mm_loadu_si128((__m128i *) (srcY+x)); // load 16 Y pixels
					__m128i uv = _mm_loadu_si128((__m128i *) (srcUV+x)); // load 8 UV
					_mm_storeu_si128((__m128i *) (dstY+x*2+ 0), _mm_unpacklo_epi8(y, uv)); // store 8 pixels
					_mm_storeu_si128((__m128i *) (dstY+x*2+16), _mm_unpackhi_epi8(y, uv)); // store 8 pixels

					x += 16;
				}
			}
		}

		while (x < srcWidth-1)
		{
			dstY[x*2+0] = srcY [x+0];
			dstY[x*2+1] = srcUV[x+0];
			dstY[x*2+2] = srcY [x+1];
			dstY[x*2+3] = srcUV[x+1];

			x += 2;
		}

		srcY += srcWidth;
		dstY += strideY;

		// second line

		x = 0;

		if (gSSE2)
		{
			if (((((uintptr_t) dstY) & 0xf) == 0) && ((((uintptr_t) srcY) & 0xf) == 0))
			{
				while (x < srcWidth-15)
				{
					__m128i y = _mm_load_si128((__m128i *) (srcY+x)); // load 16 Y pixels
					__m128i uv1 = _mm_load_si128((__m128i *) (srcUV+x)); // load 8 UV
					__m128i uv2 = _mm_load_si128((__m128i *) (srcUV+x+srcWidth)); // load 8 UV
					__m128i uv = _mm_avg_epu8(uv1, uv2);
					_mm_stream_si128((__m128i *) (dstY+x*2+ 0), _mm_unpacklo_epi8(y, uv)); // store 8 pixels
					_mm_stream_si128((__m128i *) (dstY+x*2+16), _mm_unpackhi_epi8(y, uv)); // store 8 pixels

					x += 16;
				}
			}
			else
			{
				while (x < srcWidth-15)
				{
					__m128i y = _mm_loadu_si128((__m128i *) (srcY+x)); // load 16 Y pixels
					__m128i uv1 = _mm_loadu_si128((__m128i *) (srcUV+x)); // load 8 UV
					__m128i uv2 = _mm_loadu_si128((__m128i *) (srcUV+x+srcWidth)); // load 8 UV
					__m128i uv = _mm_avg_epu8(uv1, uv2);
					_mm_storeu_si128((__m128i *) (dstY+x*2+ 0), _mm_unpacklo_epi8(y, uv)); // store 8 pixels
					_mm_storeu_si128((__m128i *) (dstY+x*2+16), _mm_unpackhi_epi8(y, uv)); // store 8 pixels

					x += 16;
				}
			}
		}

		while (x < srcWidth-1)
		{
			dstY[x*2+0] = srcY [x+0];
			dstY[x*2+1] = (srcUV[x+0] + srcUV[x+0+srcWidth])/2;
			dstY[x*2+2] = srcY [x+1];
			dstY[x*2+3] = (srcUV[x+1] + srcUV[x+1+srcWidth])/2;

			x += 2;
		}

		srcY += srcWidth;
		srcUV += srcWidth;
		dstY += strideY;

		__y += 2;
	}

	// last 2 lines
	while (__y < height)
	{
		x = 0;

		if (gSSE2)
		{
			if (((((uintptr_t) dstY) & 0xf) == 0) && ((((uintptr_t) srcY) & 0xf) == 0))
			{
				while (x < srcWidth-15)
				{
					__m128i y = _mm_load_si128((__m128i *) (srcY+x)); // load 16 Y pixels
					__m128i uv = _mm_load_si128((__m128i *) (srcUV+x)); // load 8 UV
					_mm_stream_si128((__m128i *) (dstY+x*2+ 0), _mm_unpacklo_epi8(y, uv)); // store 8 pixels
					_mm_stream_si128((__m128i *) (dstY+x*2+16), _mm_unpackhi_epi8(y, uv)); // store 8 pixels

					x += 16;
				}
			}
			else
			{
				while (x < srcWidth-15)
				{
					__m128i y = _mm_loadu_si128((__m128i *) (srcY+x)); // load 16 Y pixels
					__m128i uv = _mm_loadu_si128((__m128i *) (srcUV+x)); // load 8 UV
					_mm_storeu_si128((__m128i *) (dstY+x*2+ 0), _mm_unpacklo_epi8(y, uv)); // store 8 pixels
					_mm_storeu_si128((__m128i *) (dstY+x*2+16), _mm_unpackhi_epi8(y, uv)); // store 8 pixels

					x += 16;
				}
			}
		}

		while (x < srcWidth-1)
		{
			dstY[x*2+0] = srcY [x+0];
			dstY[x*2+1] = srcUV[x+0];
			dstY[x*2+2] = srcY [x+1];
			dstY[x*2+3] = srcUV[x+1];

			x += 2;
		}

		srcY += srcWidth;
		dstY += strideY;

		__y++;
	}

	return BC_STS_SUCCESS;
}

static BC_STATUS DtsCopy420ToUYVY(uint8_t *dstY, uint8_t *dstUV, const uint8_t *srcY, const uint8_t *srcUV, uint32_t srcWidth, uint32_t dstWidth, uint32_t height, uint32_t strideY, uint32_t strideUV)
{
	// TODO, test this
	uint32_t x, __y;

	strideY += dstWidth*2;

	__y = 0;
	while (__y < height-2)
	{
		// first line
		x = 0;

		if (gSSE2)
		{
			if (((((uintptr_t) dstY) & 0xf) == 0) && ((((uintptr_t) srcY) & 0xf) == 0))
			{
				while (x < srcWidth-15)
				{
					__m128i y = _mm_load_si128((__m128i *) (srcY+x)); // load 16 Y pixels
					__m128i uv = _mm_load_si128((__m128i *) (srcUV+x)); // load 8 UV
					_mm_stream_si128((__m128i *) (dstY+x*2+ 0), _mm_unpacklo_epi8(uv, y)); // store 8 pixels
					_mm_stream_si128((__m128i *) (dstY+x*2+16), _mm_unpackhi_epi8(uv, y)); // store 8 pixels

					x += 16;
				}
			}
			else
			{
				while (x < srcWidth-15)
				{
					__m128i y = _mm_loadu_si128((__m128i *) (srcY+x)); // load 16 Y pixels
					__m128i uv = _mm_loadu_si128((__m128i *) (srcUV+x)); // load 8 UV
					_mm_storeu_si128((__m128i *) (dstY+x*2+ 0), _mm_unpacklo_epi8(uv, y)); // store 8 pixels
					_mm_storeu_si128((__m128i *) (dstY+x*2+16), _mm_unpackhi_epi8(uv, y)); // store 8 pixels

					x += 16;
				}
			}
		}

		while (x < srcWidth-1)
		{
			dstY[x*2+1] = srcY [x+0];
			dstY[x*2+0] = srcUV[x+0];
			dstY[x*2+3] = srcY [x+1];
			dstY[x*2+2] = srcUV[x+1];

			x += 2;
		}

		srcY += srcWidth;
		dstY += strideY;

		// second line

		x = 0;

		if (gSSE2)
		{
			if (((((uintptr_t) dstY) & 0xf) == 0) && ((((uintptr_t) srcY) & 0xf) == 0))
			{
				while (x < srcWidth-15)
				{
					__m128i y = _mm_load_si128((__m128i *) (srcY+x)); // load 16 Y pixels
					__m128i uv1 = _mm_load_si128((__m128i *) (srcUV+x)); // load 8 UV
					__m128i uv2 = _mm_load_si128((__m128i *) (srcUV+x+srcWidth)); // load 8 UV
					__m128i uv = _mm_avg_epu8(uv1, uv2);
					_mm_stream_si128((__m128i *) (dstY+x*2+ 0), _mm_unpacklo_epi8(uv, y)); // store 8 pixels
					_mm_stream_si128((__m128i *) (dstY+x*2+16), _mm_unpackhi_epi8(uv, y)); // store 8 pixels

					x += 16;
				}
			}
			else
			{
				while (x < srcWidth-15)
				{
					__m128i y = _mm_loadu_si128((__m128i *) (srcY+x)); // load 16 Y pixels
					__m128i uv1 = _mm_loadu_si128((__m128i *) (srcUV+x)); // load 8 UV
					__m128i uv2 = _mm_loadu_si128((__m128i *) (srcUV+x+srcWidth)); // load 8 UV
					__m128i uv = _mm_avg_epu8(uv1, uv2);
					_mm_storeu_si128((__m128i *) (dstY+x*2+ 0), _mm_unpacklo_epi8(uv, y)); // store 8 pixels
					_mm_storeu_si128((__m128i *) (dstY+x*2+16), _mm_unpackhi_epi8(uv, y)); // store 8 pixels

					x += 16;
				}
			}
		}

		while (x < srcWidth-1)
		{
			dstY[x*2+1] = srcY [x+0];
			dstY[x*2+0] = (srcUV[x+0] + srcUV[x+0+srcWidth])/2;
			dstY[x*2+3] = srcY [x+1];
			dstY[x*2+2] = (srcUV[x+1] + srcUV[x+1+srcWidth])/2;

			x += 2;
		}

		srcY += srcWidth;
		srcUV += srcWidth;
		dstY += strideY;
	}

	// last 2 lines
	while (__y < height)
	{
		x = 0;

		if (gSSE2)
		{
			if (((((uintptr_t) dstY) & 0xf) == 0) && ((((uintptr_t) srcY) & 0xf) == 0))
			{
				while (x < srcWidth-15)
				{
					__m128i y = _mm_load_si128((__m128i *) (srcY+x)); // load 16 Y pixels
					__m128i uv = _mm_load_si128((__m128i *) (srcUV+x)); // load 8 UV
					_mm_stream_si128((__m128i *) (dstY+x*2+ 0), _mm_unpacklo_epi8(uv, y)); // store 8 pixels
					_mm_stream_si128((__m128i *) (dstY+x*2+16), _mm_unpackhi_epi8(uv, y)); // store 8 pixels

					x += 16;
				}
			}
			else
			{
				while (x < srcWidth-15)
				{
					__m128i y = _mm_loadu_si128((__m128i *) (srcY+x)); // load 16 Y pixels
					__m128i uv = _mm_loadu_si128((__m128i *) (srcUV+x)); // load 8 UV
					_mm_storeu_si128((__m128i *) (dstY+x*2+ 0), _mm_unpacklo_epi8(uv, y)); // store 8 pixels
					_mm_storeu_si128((__m128i *) (dstY+x*2+16), _mm_unpackhi_epi8(uv, y)); // store 8 pixels

					x += 16;
				}
			}
		}

		while (x < srcWidth-1)
		{
			dstY[x*2+1] = srcY [x+0];
			dstY[x*2+0] = srcUV[x+0];
			dstY[x*2+3] = srcY [x+1];
			dstY[x*2+2] = srcUV[x+1];

			x += 2;
		}

		srcY += srcWidth;
		dstY += strideY;

		__y++;
	}

	return BC_STS_SUCCESS;
}

static BC_STATUS DtsCopy420ToNV12(uint8_t *dstY, uint8_t *dstUV, const uint8_t *srcY, const uint8_t *srcUV, uint32_t srcWidth, uint32_t dstWidth, uint32_t height, uint32_t strideY, uint32_t strideUV)
{ // tested
	uint32_t __y;

	strideY += dstWidth;
	strideUV += dstWidth;

	// first copy Y
	for (__y = 0; __y < height; __y++)
	{
		fast_memcpy(dstY, srcY, srcWidth);
		dstY += strideY;
		srcY += srcWidth;
	}

	// now copy uvs
	height /= 2;
	for (__y = 0; __y < height; __y++)
	{
		fast_memcpy(dstUV, srcUV, srcWidth);
		srcUV += srcWidth;
		dstUV += strideUV;

	}
	return BC_STS_SUCCESS;
}


// copy 422/420 ( device format to format specified in Vout)
BC_STATUS DtsCopyFormat(DTS_LIB_CONTEXT	*Ctx, BC_DTS_PROC_OUT *Vout, BC_DTS_PROC_OUT *Vin)
{
	uint32_t lDestStrideY=0, lDestStrideUV=0;
	uint32_t dstHeightInPixels;

	BC_STATUS	Sts = BC_STS_SUCCESS;

	if ( (Sts = DtsChkYUVSizes(Ctx,Vout,Vin)) != BC_STS_SUCCESS)
		return Sts;

	if(Vout->PoutFlags & BC_POUT_FLAGS_STRIDE)
		lDestStrideUV = lDestStrideY = Vout->StrideSz;
	if(Vout->PoutFlags & BC_POUT_FLAGS_STRIDE_UV)
		lDestStrideUV = Vout->StrideSzUV;

	if(Vout->PoutFlags & BC_POUT_FLAGS_SIZE) {
		// Use application provided size for now
		if(!Ctx->VidParams.Progressive)
			dstHeightInPixels = Vout->PicInfo.height/2;
		else
			dstHeightInPixels = Vout->PicInfo.height;
		/* Check for Valid data based on the application information */
		// we cannot do that any more.size may vary, we have to suppose them
		// ok
	//	if((Vout->YBuffDoneSz < (dstWidthInPixels * dstHeightInPixels / 4)) ||
	//		(Vout->UVBuffDoneSz < (dstWidthInPixels * dstHeightInPixels/2 / 4)))
	//		return BC_STS_IO_XFR_ERROR;
	} else {
		dstHeightInPixels = Vin->PicInfo.height;
	}

	// check that we can do the copy properly
	if (Ctx->HWOutPicWidth > Vin->PicInfo.width)
		return BC_STS_IO_XFR_ERROR;

	//DebugLog_Trace(LDIL_DBG,"Copying from %d to %d\n", Ctx->b422Mode, Vout->b422Mode);

	if (Ctx->b422Mode) {
		// input is 422 (YUY2)
		switch (Vout->b422Mode) {
			case OUTPUT_MODE422_YUY2:
				Sts = DtsCopy422ToYUY2(
					Vout->Ybuff, Vout->UVbuff, Vin->Ybuff,
					Ctx->HWOutPicWidth, Vin->PicInfo.width,  dstHeightInPixels, lDestStrideY, lDestStrideUV
				);
				break;
			case OUTPUT_MODE422_UYVY:
				Sts = DtsCopy422ToUYVY(
					Vout->Ybuff, Vout->UVbuff, Vin->Ybuff,
					Ctx->HWOutPicWidth, Vin->PicInfo.width, dstHeightInPixels, lDestStrideY, lDestStrideUV
				);
				break;
			case OUTPUT_MODE420_NV12:
				Sts = DtsCopy422ToNV12(
					Vout->Ybuff, Vout->UVbuff, Vin->Ybuff,
					Ctx->HWOutPicWidth, Vin->PicInfo.width, dstHeightInPixels, lDestStrideY, lDestStrideUV
				);
				break;
			default:
				Sts = BC_STS_INV_ARG;
				break;
		}
	}else{
		// input is 420 (NV12)
		switch (Vout->b422Mode) {
			case OUTPUT_MODE422_YUY2:
				Sts = DtsCopy420ToYUY2(
					Vout->Ybuff, Vout->UVbuff, Vin->Ybuff, Vin->UVbuff,
					Ctx->HWOutPicWidth, Vin->PicInfo.width, dstHeightInPixels, lDestStrideY, lDestStrideUV
				);
				break;
			case OUTPUT_MODE422_UYVY:
				Sts = DtsCopy420ToUYVY(
					Vout->Ybuff, Vout->UVbuff, Vin->Ybuff, Vin->UVbuff,
					Ctx->HWOutPicWidth, Vin->PicInfo.width, dstHeightInPixels, lDestStrideY, lDestStrideUV
				);
				break;
			case OUTPUT_MODE420_NV12:
				Sts = DtsCopy420ToNV12(
					Vout->Ybuff, Vout->UVbuff, Vin->Ybuff, Vin->UVbuff,
					Ctx->HWOutPicWidth, Vin->PicInfo.width, dstHeightInPixels, lDestStrideY, lDestStrideUV
				);
				break;
			default:
				Sts = BC_STS_INV_ARG;
				break;
		}
	}

	return Sts;
}




DRVIFLIB_INT_API BC_STATUS
DtsPushFwBinToLink(
	HANDLE hDevice,
    uint32_t *Buffer,
    uint32_t BuffSz)
{
	uint8_t			*pXferBuff;
	BC_IOCTL_DATA	*pIoctlData;
	BC_CMD_DEV_MEM	*pMemAccess;
	uint32_t				BytesReturned, AllocSz;

	if (!hDevice) {
		DebugLog_Trace(LDIL_DBG,"DtsPushFwBinToLink: Invalid Handle\n");
		return BC_STS_INV_ARG;
	}

	if (!Buffer) {
		DebugLog_Trace(LDIL_DBG,"DtsPushFwBinToLink: Null Buffer\n");
		return BC_STS_INV_ARG;
	}

	if (BuffSz % 4) {
		DebugLog_Trace(LDIL_DBG,"DtsPushFwBinToLink: Buff Size is not a multiple of DWORD\n");
		return BC_STS_ERROR;
	}

	AllocSz = sizeof(BC_IOCTL_DATA) + (BuffSz);
	pIoctlData = (BC_IOCTL_DATA *) malloc(AllocSz);
	if(!pIoctlData) {
		DebugLog_Trace(LDIL_DBG,"DtsPushFwBinToLink: Memory Allocation Failed\n");
		return BC_STS_ERROR;
	}
	memset(pIoctlData, 0, AllocSz);
	pXferBuff = ((PUCHAR)pIoctlData) + sizeof(BC_IOCTL_DATA);
	pMemAccess = &pIoctlData->u.devMem;
	pIoctlData->RetSts = BC_STS_ERROR;
	pIoctlData->IoctlDataSz = sizeof(BC_IOCTL_DATA);
	pMemAccess->StartOff = 0;
	pMemAccess->NumDwords = BuffSz/4;
	memcpy(pXferBuff, Buffer, BuffSz);

	if (!DtsDrvIoctl(hDevice, BCM_IOC_FW_DOWNLOAD, pIoctlData, AllocSz, pIoctlData, AllocSz, (LPDWORD)&BytesReturned, 0)) {
		DebugLog_Trace(LDIL_DBG,"DtsPushFwBinToLink: DeviceIoControl Failed\n");
		return BC_STS_ERROR;
	}

	if (BC_STS_ERROR == pIoctlData->RetSts) {
		DebugLog_Trace(LDIL_DBG,"DtsPushFwBinToLink: IOCTL Cmd Failed By Driver\n");
		return pIoctlData->RetSts;
	}

	if(pIoctlData) {
		free(pIoctlData);
	}

	return BC_STS_SUCCESS;
}

/*====================== Debug Routines ========================================*/
void DumpDataToFile(FILE *fp, char *header, uint32_t off, uint8_t *buff, uint32_t dwcount)
{
	uint32_t i, k=1;

#ifndef  _LIB_EN_FWDUMP_
	// Skip FW Download dumping..
	return ;
#endif

	if(!fp)
		return;

	if(header){
		fprintf(fp,"%s\n",header);
	}

	for(i = 0; i < dwcount; i++){
		if (k == 1)
			fprintf(fp, "0x%08X : ", off);

		fprintf(fp," 0x%08X ", *((uint32_t *)buff));

		buff	+= sizeof(uint32_t);
		off		+= sizeof(uint32_t);
		k++;
		if ((i == dwcount - 1) || (k > 4)){
			fprintf(fp,"\n");
			k = 1;
		}
	}
	//fprintf(fp,"\n");

	fflush(fp);
}

void DumpInputSampleToFile(uint8_t *buff, uint32_t buffsize)
{
#ifndef  _LIB_EN_INDUMP_
	return ;
#endif

	static FILE	*pOutputFile=NULL;

	if(!buff || !buffsize){
		if(pOutputFile){
			fclose(pOutputFile);
			pOutputFile = NULL;
		}
		return;
	}

	if(!pOutputFile){
		if(!(pOutputFile = fopen("hdfile_dump.ts","wb")) )
			return;
	}

	fwrite(buff, sizeof(uint8_t), buffsize, pOutputFile);

	fflush(pOutputFile);
}

