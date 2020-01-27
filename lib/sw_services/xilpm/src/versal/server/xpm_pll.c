/******************************************************************************
*
* Copyright (C) 2018-2020 Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
*
*
*
******************************************************************************/

#include "xpm_pll.h"

#define CLK_PARENTS_PAYLOAD_LEN		12U

/* Period of time needed to lock the PLL (TODO: measure actual latency) */
#define PM_PLL_LOCKING_TIME		1U

static struct XPm_PllTopology PllTopologies[] =
{
	{ TOPOLOGY_GENERIC_PLL, PLLPARAMS, RESET_SHIFT, BYPASS_SHIFT,
	  GEN_LOCK_SHIFT, GEN_STABLE_SHIFT, GEN_REG3_OFFSET },
	{ TOPOLOGY_NOC_PLL, PLLPARAMS, RESET_SHIFT, BYPASS_SHIFT,
	  NPLL_LOCK_SHIFT, NPLL_STABLE_SHIFT, NPLL_REG3_OFFSET },
};

XStatus XPmClockPll_AddNode(u32 Id, u32 ControlReg, u8 TopologyType,
			    u16 *Offsets, u32 PowerDomainId, u8 ClkFlags)
{
	XStatus Status = XST_FAILURE;
	XPm_PllClockNode *PllClkPtr;

	if (NULL != XPmClock_GetById(Id)) {
		Status = XPM_PM_INVALID_NODE;
		goto done;
	}
	if (TopologyType!=TOPOLOGY_GENERIC_PLL && TopologyType!=TOPOLOGY_NOC_PLL) {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	PllClkPtr = XPm_AllocBytes(sizeof(XPm_PllClockNode));
	if (PllClkPtr == NULL) {
		Status = XST_BUFFER_TOO_SMALL;
		goto done;
	}
	XPmNode_Init(&PllClkPtr->ClkNode.Node, Id, (u8)PM_PLL_STATE_SUSPENDED, 0);
	PllClkPtr->ClkNode.Node.BaseAddress = ControlReg;
	PllClkPtr->ClkNode.ClkHandles = NULL;
	PllClkPtr->ClkNode.UseCount = 0;
	PllClkPtr->ClkNode.NumParents = 1;
	PllClkPtr->ClkNode.Flags = ClkFlags;
	PllClkPtr->Topology = &PllTopologies[TopologyType-TOPOLOGY_GENERIC_PLL];
	PllClkPtr->StatusReg = ControlReg + Offsets[0];
	PllClkPtr->ConfigReg = ControlReg + Offsets[1];
	PllClkPtr->FracConfigReg = ControlReg + Offsets[2];

	Status = XPmClock_SetById(Id, (XPm_ClockNode *)PllClkPtr);
	if (XST_SUCCESS != Status) {
		goto done;
	}

	if (((u32)XPM_NODECLASS_POWER != NODECLASS(PowerDomainId)) ||
	    ((u32)XPM_NODESUBCL_POWER_DOMAIN != NODESUBCLASS(PowerDomainId))) {
		PllClkPtr->ClkNode.PwrDomain = NULL;
		goto done;
	}

	PllClkPtr->ClkNode.PwrDomain = XPmPower_GetById(PowerDomainId);
	if (NULL == PllClkPtr->ClkNode.PwrDomain) {
		Status = XST_DEVICE_NOT_FOUND;
		goto done;
	}

done:
	return Status;
}

XStatus XPmClockPll_AddParent(u32 Id, u32 *Parents, u8 NumParents)
{
	XStatus Status = XST_FAILURE;
	XPm_PllClockNode *PllPtr = (XPm_PllClockNode *)XPmClock_GetById(Id);

	if (PllPtr == NULL) {
		Status = XST_INVALID_PARAM;
		goto done;
	}
	if (PllPtr->ClkNode.NumParents == 1U && NumParents != 1U) {
		Status = XST_INVALID_PARAM;
		goto done;
	} else {
		PllPtr->ClkNode.ParentIdx = (u16)(NODEINDEX(Parents[0]));
		Status = XST_SUCCESS;
	}

done:
	return Status;
}

XStatus XPmClockPll_SetMode(XPm_PllClockNode *Pll, u32 Mode)
{
	XStatus Status = XST_FAILURE;
	u32 Val = 0;

	if ((u32)PM_PLL_MODE_FRACTIONAL == Mode) {
		/* Check if fractional value has been set */
		XPmClockPll_GetParam(Pll, (u32)PM_PLL_PARAM_ID_DATA, &Val);
		if (0U == Val) {
			Status = XST_FAILURE;
			goto done;
		}
	}

	Status = XPmClockPll_Reset(Pll, PLL_RESET_ASSERT);
	if (XST_SUCCESS != Status) {
		goto done;
	}

	if ((u32)PM_PLL_MODE_RESET == Mode) {
		Status = XST_SUCCESS;
		goto done;
	} else if ((u32)PM_PLL_MODE_FRACTIONAL == Mode) {
		/* Enable fractional mode */
		XPm_RMW32(Pll->FracConfigReg, PLL_FRAC_CFG_ENABLED_MASK,
			  PLL_FRAC_CFG_ENABLED_MASK);
	} else if ((u32)PM_PLL_MODE_INTEGER == Mode) {
		/* Disable fractional mode */
		XPm_RMW32(Pll->FracConfigReg, PLL_FRAC_CFG_ENABLED_MASK, 0);
	} else {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	Status = XPmClockPll_Reset(Pll, PLL_RESET_RELEASE);

done:
	if (XST_SUCCESS == Status) {
		Pll->PllMode = (u8)Mode;
	}
	return Status;
}

XStatus XPmClockPll_GetMode(XPm_PllClockNode *Pll, u32 *Mode)
{
	u32 Val;
	XStatus Status = XST_FAILURE;
	XPm_Power *PowerDomain = Pll->ClkNode.PwrDomain;

	if ((u8)XPM_POWER_STATE_ON != PowerDomain->Node.State) {
		Status = XST_NO_ACCESS;
		goto done;
	}

	Val = XPm_Read32(Pll->ClkNode.Node.BaseAddress);
	if (0U != (Val & BIT(Pll->Topology->ResetShift))) {
		*Mode = (u32)PM_PLL_MODE_RESET;
	} else {
		Val = XPm_Read32(Pll->FracConfigReg);
		if (0U != (Val & PLL_FRAC_CFG_ENABLED_MASK)) {
			*Mode = (u32)PM_PLL_MODE_FRACTIONAL;
		} else {
			*Mode = (u32)PM_PLL_MODE_INTEGER;
		}
	}

	Pll->PllMode = (u8)(*Mode);

	Status = XST_SUCCESS;

done:
	return Status;
}

static void XPm_PllSaveContext(XPm_PllClockNode* Pll)
{
	/* Save register setting */
	Pll->Context.Ctrl = XPm_Read32(Pll->ClkNode.Node.BaseAddress);
	Pll->Context.Cfg = XPm_Read32(Pll->ConfigReg);
	Pll->Context.Frac = XPm_Read32(Pll->FracConfigReg);
	Pll->Context.Flag |= PM_PLL_CONTEXT_SAVED;
}

static void XPm_PllRestoreContext(XPm_PllClockNode* Pll)
{
	XPm_Write32(Pll->ClkNode.Node.BaseAddress, Pll->Context.Ctrl);
	XPm_Write32(Pll->ConfigReg, Pll->Context.Cfg);
	XPm_Write32(Pll->FracConfigReg, Pll->Context.Frac);
	Pll->Context.Flag &= (u8)(~PM_PLL_CONTEXT_SAVED);
}

XStatus XPmClockPll_Suspend(XPm_PllClockNode *Pll)
{
	XStatus Status = XST_FAILURE;
	XPm_Power *PowerDomain = Pll->ClkNode.PwrDomain;

	XPm_PllSaveContext(Pll);

	/* If PLL is not already in reset, bypass it and put in reset/pwrdn */
	if (PM_PLL_STATE_RESET != Pll->ClkNode.Node.State) {
		Status = XPmClockPll_Reset(Pll, PLL_RESET_ASSERT);
		if (XST_SUCCESS != Status) {
			goto done;
		}
	}

	if (NULL != PowerDomain) {
		Status = PowerDomain->HandleEvent(&PowerDomain->Node,
						  XPM_POWER_EVENT_PWR_DOWN);
		if (XST_SUCCESS != Status) {
			goto done;
		}
	}

	Pll->ClkNode.Node.State = PM_PLL_STATE_SUSPENDED;

	Status = XST_SUCCESS;

done:
	return Status;
}

XStatus XPmClockPll_Resume(XPm_PllClockNode *Pll)
{
	XStatus Status = XST_FAILURE;
	XPm_Power *PowerDomain = Pll->ClkNode.PwrDomain;

	if (NULL != PowerDomain) {
		Status = PowerDomain->HandleEvent(&PowerDomain->Node,
						  XPM_POWER_EVENT_PWR_UP);
		if (XST_SUCCESS != Status) {
			goto done;
		}
	}

	if (Pll->Context.Flag & PM_PLL_CONTEXT_SAVED) {
		XPm_PllRestoreContext(Pll);
	}

	/* By saved configuration PLL is in reset, leave it as is */
	if (Pll->Context.Ctrl & BIT(Pll->Topology->ResetShift)) {
		Pll->ClkNode.Node.State = PM_PLL_STATE_RESET;
		Status = XST_SUCCESS;
	} else {
		Status = XPmClockPll_Reset(Pll, PLL_RESET_RELEASE);
	}

done:
	return Status;
}

XStatus XPmClockPll_Request(u32 PllId)
{
	XStatus Status = XST_FAILURE;

	XPm_PllClockNode *Pll = (XPm_PllClockNode *)XPmClock_GetById(PllId);
	if (Pll == NULL) {
		Status = XST_INVALID_PARAM;
		goto done;
	}
	Pll->ClkNode.UseCount++;

	/* If the PLL is suspended it needs to be resumed first */
	if (Pll->ClkNode.Node.State == PM_PLL_STATE_SUSPENDED) {
		Status = XPmClockPll_Resume(Pll);
	}
	else if (Pll->ClkNode.Node.State == PM_PLL_STATE_RESET) {
		Status = XPmClockPll_Reset(Pll, PLL_RESET_RELEASE);
	} else {
		Status = XST_SUCCESS;
	}

done:
	return Status;
}

XStatus XPmClockPll_Release(u32 PllId)
{
	XStatus Status = XST_FAILURE;
	XPm_PllClockNode *Pll = (XPm_PllClockNode *)XPmClock_GetById(PllId);
	if (Pll == NULL) {
		Status = XST_INVALID_PARAM;
		goto done;
	}
	Pll->ClkNode.UseCount--;

	if (Pll->ClkNode.UseCount == 0U) {
		Status = XPmClockPll_Suspend(Pll);
		goto done;
	}

	Status = XST_SUCCESS;

done:
	return Status;
}


XStatus XPmClockPll_Reset(XPm_PllClockNode *Pll, uint8_t Flags)
{
	XStatus Status = XST_FAILURE;
	u32 ControlReg = Pll->ClkNode.Node.BaseAddress;

	if (Flags & PLL_RESET_ASSERT) {
		/* Bypass PLL before putting it into the reset */
		XPm_RMW32(ControlReg, BIT32(Pll->Topology->BypassShift),
			   BIT32(Pll->Topology->BypassShift));

		/* Power down PLL (= reset PLL) */
		XPm_RMW32(ControlReg, BIT32(Pll->Topology->ResetShift),
			   BIT32(Pll->Topology->ResetShift));
		Pll->ClkNode.Node.State = PM_PLL_STATE_RESET;

		if ((PLATFORM_VERSION_SILICON == Platform) &&
		    (PLATFORM_VERSION_SILICON_ES1 != PlatformVersion)) {
			/*
			 * The value of the CRX.XPLL_REG3.CP_RES_H must be set
			 * to 0x1 while the PLL is in reset for ES2 and forward
			 */
			u32 Reg;
			if ((u32)XPM_NODEIDX_CLK_PMC_PLL ==
			   NODEINDEX(Pll->ClkNode.Node.Id)) {
			   Reg = ((ControlReg & (0xFFFFFF00)) +
				  PPLL_REG3_OFFSET);
			} else {
			   Reg = ((ControlReg & (0xFFFFFF00)) +
				  (Pll->Topology->PllReg3Offset));
			}
			XPm_RMW32(Reg, BITNMASK(PLL_REG3_CP_RES_H_SHIFT,
				  PLL_REG3_CP_RES_H_WIDTH),
				  0x1U << PLL_REG3_CP_RES_H_SHIFT);
		}
	}
	if (Flags & PLL_RESET_RELEASE) {
		/* Deassert the reset */
		XPm_RMW32(ControlReg, BIT32(Pll->Topology->ResetShift),
			   ~BIT32(Pll->Topology->ResetShift));
		/* Poll status register for the lock */
		Status = XPm_PollForMask(Pll->StatusReg, BIT32(Pll->Topology->LockShift),
						  PLL_LOCK_TIMEOUT);
		/* Deassert bypass if the PLL locked */
		if (XST_SUCCESS == Status) {
			XPm_RMW32(ControlReg, BIT32(Pll->Topology->BypassShift),
					~BIT32(Pll->Topology->BypassShift));
			Pll->ClkNode.Node.State = PM_PLL_STATE_LOCKED;
		} else {
			goto done;
		}
	}

	Status = XST_SUCCESS;

done:
	return Status;
}

XStatus XPmClockPll_SetParam(XPm_PllClockNode *Pll, u32 Param, u32 Value)
{
	XStatus Status = XST_FAILURE;
	XPm_PllParam *PtrParam;
	u32 Mask, ParamValue, Reg = 0;

	if (Param >= (u32)PM_PLL_PARAM_MAX) {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	PtrParam = &Pll->Topology->ConfigParams[Param];
	if (Value > BITMASK(PtrParam->Width)) {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	/* Allow config change only if PLL is in reset mode */
	if (Pll->ClkNode.Node.State != PM_PLL_STATE_RESET) {
		/*
		 * TODO - revisit it to allow subsystem CDO
		 * re-parsing which re-sets PLL parameters.
		 */
		PmInfo("Warning: Setting PLL parameter while not in reset state.\r\n");
		Status = XST_SUCCESS;
		goto done;
	}

	Mask = BITNMASK(PtrParam->Shift,PtrParam->Width);
	ParamValue = Value << PtrParam->Shift;

	switch (Param) {
	case (u32)PM_PLL_PARAM_ID_DIV2:
	case (u32)PM_PLL_PARAM_ID_FBDIV:
		Reg = Pll->ClkNode.Node.BaseAddress;
		Status = XST_SUCCESS;
		break;
	case (u32)PM_PLL_PARAM_ID_DATA:
		Reg = Pll->FracConfigReg;
		Status = XST_SUCCESS;
		break;
	case (u32)PM_PLL_PARAM_ID_PRE_SRC:
	case (u32)PM_PLL_PARAM_ID_POST_SRC:
	case (u32)PM_PLL_PARAM_ID_LOCK_DLY:
	case (u32)PM_PLL_PARAM_ID_LOCK_CNT:
	case (u32)PM_PLL_PARAM_ID_LFHF:
	case (u32)PM_PLL_PARAM_ID_CP:
	case (u32)PM_PLL_PARAM_ID_RES:
		Reg = Pll->ConfigReg;
		Status = XST_SUCCESS;
		break;
	default:
		Status = XST_FAILURE;
		break;
	}

	if (XST_SUCCESS == Status) {
		XPm_RMW32(Reg, Mask, ParamValue);
	}

done:
	return Status;
}

XStatus XPmClockPll_GetParam(XPm_PllClockNode *Pll, u32 Param, u32 *Val)
{
	XStatus Status = XST_FAILURE;
	XPm_PllParam *PtrParam;
	u32 Shift, Mask, Reg = 0;
	XPm_Power *PowerDomain = Pll->ClkNode.PwrDomain;

	if ((u32)XPM_POWER_STATE_ON != PowerDomain->Node.State) {
		Status = XST_NO_ACCESS;
		goto done;
	}

	if (Param >= (u32)PM_PLL_PARAM_MAX) {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	PtrParam = &Pll->Topology->ConfigParams[Param];
	Mask = BITNMASK(PtrParam->Shift, PtrParam->Width);
	Shift = PtrParam->Shift;

	switch (Param) {
	case (u32)PM_PLL_PARAM_ID_DIV2:
	case (u32)PM_PLL_PARAM_ID_FBDIV:
		Reg = Pll->ClkNode.Node.BaseAddress;
		Status = XST_SUCCESS;
		break;
	case (u32)PM_PLL_PARAM_ID_DATA:
		Reg = Pll->FracConfigReg;
		Status = XST_SUCCESS;
		break;
	case (u32)PM_PLL_PARAM_ID_PRE_SRC:
	case (u32)PM_PLL_PARAM_ID_POST_SRC:
	case (u32)PM_PLL_PARAM_ID_LOCK_DLY:
	case (u32)PM_PLL_PARAM_ID_LOCK_CNT:
	case (u32)PM_PLL_PARAM_ID_LFHF:
	case (u32)PM_PLL_PARAM_ID_CP:
	case (u32)PM_PLL_PARAM_ID_RES:
		Reg = Pll->ConfigReg;
		Status = XST_SUCCESS;
		break;
	default:
		Status = XST_FAILURE;
		break;
	}

	if (XST_SUCCESS == Status) {
		*Val = (XPm_Read32(Reg) & Mask) >> Shift;
	}

done:
	return Status;
}

int XPmClockPll_QueryMuxSources(u32 Id, u32 Index, u32 *Resp)
{
	int Status = XST_FAILURE;
	XPm_PllClockNode *PllPtr = (XPm_PllClockNode *)XPmClock_GetById(Id);

	if (PllPtr == NULL) {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	memset(Resp, 0, CLK_PARENTS_PAYLOAD_LEN);

	if (Index != 0U) {
		Status = XST_INVALID_PARAM;
		goto done;
	}
	Resp[0] = PllPtr->ClkNode.ParentIdx;
	Resp[1] = 0xFFFFFFFF;

	Status = XST_SUCCESS;

done:
	return Status;
}

int XPmClockPll_GetWakeupLatency(const u32 Id, u32 *Latency)
{
	int Status = XST_SUCCESS;
	XPm_PllClockNode *Pll = (XPm_PllClockNode *)XPmClock_GetById(Id);
	u32 Lat = 0;

	*Latency = 0;
	if (NULL == Pll) {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	if (PM_PLL_STATE_LOCKED == Pll->ClkNode.Node.State) {
		goto done;
	}

	*Latency += PM_PLL_LOCKING_TIME;

	Status = XPmPower_GetWakeupLatency(Pll->ClkNode.PwrDomain->Node.Id,
					   &Lat);
	if (XST_SUCCESS == Status) {
		*Latency += Lat;
	}

done:
	return Status;
}
