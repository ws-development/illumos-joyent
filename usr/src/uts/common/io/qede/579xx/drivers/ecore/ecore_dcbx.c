/*
* CDDL HEADER START
*
* The contents of this file are subject to the terms of the
* Common Development and Distribution License, v.1,  (the "License").
* You may not use this file except in compliance with the License.
*
* You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
* or http://opensource.org/licenses/CDDL-1.0.
* See the License for the specific language governing permissions
* and limitations under the License.
*
* When distributing Covered Code, include this CDDL HEADER in each
* file and include the License file at usr/src/OPENSOLARIS.LICENSE.
* If applicable, add the following below this CDDL HEADER, with the
* fields enclosed by brackets "[]" replaced with your own identifying
* information: Portions Copyright [yyyy] [name of copyright owner]
*
* CDDL HEADER END
*/

/*
* Copyright 2014-2017 Cavium, Inc. 
* The contents of this file are subject to the terms of the Common Development 
* and Distribution License, v.1,  (the "License").

* You may not use this file except in compliance with the License.

* You can obtain a copy of the License at available 
* at http://opensource.org/licenses/CDDL-1.0

* See the License for the specific language governing permissions and 
* limitations under the License.
*/

#include "bcm_osal.h"
#include "ecore.h"
#include "ecore_sp_commands.h"
#include "ecore_dcbx.h"
#include "ecore_cxt.h"
#include "ecore_gtt_reg_addr.h"
#include "ecore_iro.h"
#ifdef CONFIG_ECORE_ROCE
#include "ecore_roce.h"
#endif
#include "ecore_iov_api.h"

#define ECORE_DCBX_MAX_MIB_READ_TRY	(100)
#define ECORE_ETH_TYPE_DEFAULT		(0)
#define ECORE_ETH_TYPE_ROCE		(0x8915)
#define ECORE_UDP_PORT_TYPE_ROCE_V2	(0x12B7)
#define ECORE_ETH_TYPE_FCOE		(0x8906)
#define ECORE_TCP_PORT_ISCSI		(0xCBC)

#define ECORE_DCBX_INVALID_PRIORITY	0xFF

/* Get Traffic Class from priority traffic class table, 4 bits represent
 * the traffic class corresponding to the priority.
 */
#define ECORE_DCBX_PRIO2TC(prio_tc_tbl, prio) \
		((u32)(prio_tc_tbl >> ((7 - prio) * 4)) & 0x7)

static bool ecore_dcbx_app_ethtype(u32 app_info_bitmap)
{
	return !!(ECORE_MFW_GET_FIELD(app_info_bitmap, DCBX_APP_SF) ==
		  DCBX_APP_SF_ETHTYPE);
}

static bool ecore_dcbx_ieee_app_ethtype(u32 app_info_bitmap)
{
	u8 mfw_val = ECORE_MFW_GET_FIELD(app_info_bitmap, DCBX_APP_SF_IEEE);

	/* Old MFW */
	if (mfw_val == DCBX_APP_SF_IEEE_RESERVED)
		return ecore_dcbx_app_ethtype(app_info_bitmap);

	return !!(mfw_val == DCBX_APP_SF_IEEE_ETHTYPE);
}

static bool ecore_dcbx_app_port(u32 app_info_bitmap)
{
	return !!(ECORE_MFW_GET_FIELD(app_info_bitmap, DCBX_APP_SF) ==
		  DCBX_APP_SF_PORT);
}

static bool ecore_dcbx_ieee_app_port(u32 app_info_bitmap, u8 type)
{
	u8 mfw_val = ECORE_MFW_GET_FIELD(app_info_bitmap, DCBX_APP_SF_IEEE);

	/* Old MFW */
	if (mfw_val == DCBX_APP_SF_IEEE_RESERVED)
		return ecore_dcbx_app_port(app_info_bitmap);

	return !!(mfw_val == type || mfw_val == DCBX_APP_SF_IEEE_TCP_UDP_PORT);
}

static bool ecore_dcbx_default_tlv(u32 app_info_bitmap, u16 proto_id, bool ieee)
{
	bool ethtype;

	if (ieee)
		ethtype = ecore_dcbx_ieee_app_ethtype(app_info_bitmap);
	else
		ethtype = ecore_dcbx_app_ethtype(app_info_bitmap);

	return !!(ethtype && (proto_id == ECORE_ETH_TYPE_DEFAULT));
}

static bool ecore_dcbx_iscsi_tlv(u32 app_info_bitmap, u16 proto_id, bool ieee)
{
	bool port;

	if (ieee)
		port = ecore_dcbx_ieee_app_port(app_info_bitmap,
						DCBX_APP_SF_IEEE_TCP_PORT);
	else
		port = ecore_dcbx_app_port(app_info_bitmap);

	return !!(port && (proto_id == ECORE_TCP_PORT_ISCSI));
}

static bool ecore_dcbx_fcoe_tlv(u32 app_info_bitmap, u16 proto_id, bool ieee)
{
	bool ethtype;

	if (ieee)
		ethtype = ecore_dcbx_ieee_app_ethtype(app_info_bitmap);
	else
		ethtype = ecore_dcbx_app_ethtype(app_info_bitmap);

	return !!(ethtype && (proto_id == ECORE_ETH_TYPE_FCOE));
}

static bool ecore_dcbx_roce_tlv(u32 app_info_bitmap, u16 proto_id, bool ieee)
{
	bool ethtype;

	if (ieee)
		ethtype = ecore_dcbx_ieee_app_ethtype(app_info_bitmap);
	else
		ethtype = ecore_dcbx_app_ethtype(app_info_bitmap);

	return !!(ethtype && (proto_id == ECORE_ETH_TYPE_ROCE));
}

static bool ecore_dcbx_roce_v2_tlv(u32 app_info_bitmap, u16 proto_id, bool ieee)
{
	bool port;

	if (ieee)
		port = ecore_dcbx_ieee_app_port(app_info_bitmap,
						DCBX_APP_SF_IEEE_UDP_PORT);
	else
		port = ecore_dcbx_app_port(app_info_bitmap);

	return !!(port && (proto_id == ECORE_UDP_PORT_TYPE_ROCE_V2));
}

static bool ecore_dcbx_iwarp_tlv(struct ecore_hwfn *p_hwfn, u32 app_info_bitmap,
				 u16 proto_id, bool ieee)
{
	bool port;

	if (!p_hwfn->p_dcbx_info->iwarp_port)
		return false;

	if (ieee)
		port = ecore_dcbx_ieee_app_port(app_info_bitmap,
						DCBX_APP_SF_IEEE_TCP_PORT);
	else
		port = ecore_dcbx_app_port(app_info_bitmap);

	return !!(port && (proto_id == p_hwfn->p_dcbx_info->iwarp_port));
}

static void
ecore_dcbx_dp_protocol(struct ecore_hwfn *p_hwfn,
		       struct ecore_dcbx_results *p_data)
{
	enum dcbx_protocol_type id;
	int i;

	DP_VERBOSE(p_hwfn, ECORE_MSG_DCB, "DCBX negotiated: %d\n",
		   p_data->dcbx_enabled);

	for (i = 0; i < OSAL_ARRAY_SIZE(ecore_dcbx_app_update); i++) {
		id = ecore_dcbx_app_update[i].id;

		DP_VERBOSE(p_hwfn, ECORE_MSG_DCB,
			   "%s info: update %d, enable %d, prio %d, tc %d, num_active_tc %d dscp_enable = %d dscp_val = %d\n",
			   ecore_dcbx_app_update[i].name, p_data->arr[id].update,
			   p_data->arr[id].enable, p_data->arr[id].priority,
			   p_data->arr[id].tc, p_hwfn->hw_info.num_active_tc,
			   p_data->arr[id].dscp_enable,
			   p_data->arr[id].dscp_val);
	}
}

static void
ecore_dcbx_set_params(struct ecore_dcbx_results *p_data,
		      struct ecore_hwfn *p_hwfn,
		      bool enable, u8 prio, u8 tc,
		      enum dcbx_protocol_type type,
		      enum ecore_pci_personality personality)
{
	struct ecore_dcbx_dscp_params *dscp = &p_hwfn->p_dcbx_info->get.dscp;

	/* PF update ramrod data */
	p_data->arr[type].enable = enable;
	p_data->arr[type].priority = prio;
	p_data->arr[type].tc = tc;
	p_data->arr[type].dscp_enable = dscp->enabled;
	if (p_data->arr[type].dscp_enable) {
		u8 i;

		for (i = 0; i < ECORE_DCBX_DSCP_SIZE; i++)
			if (prio == dscp->dscp_pri_map[i]) {
				p_data->arr[type].dscp_val = i;
				break;
			}
	}

	if (enable && p_data->arr[type].dscp_enable)
		p_data->arr[type].update = UPDATE_DCB_DSCP;
	else if (enable)
		p_data->arr[type].update = UPDATE_DCB;
	else
		p_data->arr[type].update = DONT_UPDATE_DCB_DSCP;

	/* QM reconf data */
	if (p_hwfn->hw_info.personality == personality)
		p_hwfn->hw_info.offload_tc = tc;
}

/* Update app protocol data and hw_info fields with the TLV info */
static void
ecore_dcbx_update_app_info(struct ecore_dcbx_results *p_data,
			   struct ecore_hwfn *p_hwfn,
			   bool enable, u8 prio, u8 tc,
			   enum dcbx_protocol_type type)
{
	enum ecore_pci_personality personality;
	enum dcbx_protocol_type id;
	char *name;
	int i;

	for (i = 0; i < OSAL_ARRAY_SIZE(ecore_dcbx_app_update); i++) {
		id = ecore_dcbx_app_update[i].id;

		if (type != id)
			continue;

		personality = ecore_dcbx_app_update[i].personality;
		name = ecore_dcbx_app_update[i].name;

		ecore_dcbx_set_params(p_data, p_hwfn, enable,
				      prio, tc, type, personality);
	}
}

static enum _ecore_status_t
ecore_dcbx_get_app_priority(u8 pri_bitmap, u8 *priority)
{
	u32 pri_mask, pri = ECORE_MAX_PFC_PRIORITIES;
	u32 index = ECORE_MAX_PFC_PRIORITIES - 1;
	enum _ecore_status_t rc = ECORE_SUCCESS;

	/* Bitmap 1 corresponds to priority 0, return priority 0 */
	if (pri_bitmap == 1) {
		*priority = 0;
		return rc;
	}

	/* Choose the highest priority */
	while ((ECORE_MAX_PFC_PRIORITIES == pri) && index) {
		pri_mask = 1 << index;
		if (pri_bitmap & pri_mask)
			pri = index;
		index--;
	}

	if (pri < ECORE_MAX_PFC_PRIORITIES)
		*priority = (u8)pri;
	else
		rc = ECORE_INVAL;

	return rc;
}

static bool
ecore_dcbx_get_app_protocol_type(struct ecore_hwfn *p_hwfn,
				 u32 app_prio_bitmap, u16 id,
				 enum dcbx_protocol_type *type, bool ieee)
{
	if (ecore_dcbx_fcoe_tlv(app_prio_bitmap, id, ieee)) {
		*type = DCBX_PROTOCOL_FCOE;
	} else if (ecore_dcbx_roce_tlv(app_prio_bitmap, id, ieee)) {
		*type = DCBX_PROTOCOL_ROCE;
	} else if (ecore_dcbx_iscsi_tlv(app_prio_bitmap, id, ieee)) {
		*type = DCBX_PROTOCOL_ISCSI;
	} else if (ecore_dcbx_default_tlv(app_prio_bitmap, id, ieee)) {
		*type = DCBX_PROTOCOL_ETH;
	} else if (ecore_dcbx_roce_v2_tlv(app_prio_bitmap, id, ieee)) {
		*type = DCBX_PROTOCOL_ROCE_V2;
	} else if (ecore_dcbx_iwarp_tlv(p_hwfn, app_prio_bitmap, id, ieee)) {
		*type = DCBX_PROTOCOL_IWARP;
	} else {
		*type = DCBX_MAX_PROTOCOL_TYPE;
		DP_ERR(p_hwfn,
		       "No action required, App TLV id = 0x%x app_prio_bitmap = 0x%x\n",
		       id, app_prio_bitmap);
		return false;
	}

	return true;
}

/*  Parse app TLV's to update TC information in hw_info structure for
 * reconfiguring QM. Get protocol specific data for PF update ramrod command.
 */
static enum _ecore_status_t
ecore_dcbx_process_tlv(struct ecore_hwfn *p_hwfn,
		       struct ecore_dcbx_results *p_data,
		       struct dcbx_app_priority_entry *p_tbl, u32 pri_tc_tbl,
		       int count, u8 dcbx_version)
{
	enum dcbx_protocol_type type;
	u8 tc, priority_map;
	bool enable, ieee;
	u16 protocol_id;
	u8 priority;
	enum _ecore_status_t rc = ECORE_SUCCESS;
	int i;

	DP_VERBOSE(p_hwfn, ECORE_MSG_DCB,
		   "Num APP entries = %d pri_tc_tbl = 0x%x dcbx_version = %u\n",
		   count, pri_tc_tbl, dcbx_version);

	ieee = (dcbx_version == DCBX_CONFIG_VERSION_IEEE);
	/* Parse APP TLV */
	for (i = 0; i < count; i++) {
		protocol_id = ECORE_MFW_GET_FIELD(p_tbl[i].entry,
						  DCBX_APP_PROTOCOL_ID);
		priority_map = ECORE_MFW_GET_FIELD(p_tbl[i].entry,
						   DCBX_APP_PRI_MAP);
		DP_VERBOSE(p_hwfn, ECORE_MSG_DCB, "Id = 0x%x pri_map = %u\n",
			   protocol_id, priority_map);
		rc = ecore_dcbx_get_app_priority(priority_map, &priority);
		if (rc == ECORE_INVAL) {
			DP_ERR(p_hwfn, "Invalid priority\n");
			return ECORE_INVAL;
		}

		tc = ECORE_DCBX_PRIO2TC(pri_tc_tbl, priority);
		if (ecore_dcbx_get_app_protocol_type(p_hwfn, p_tbl[i].entry,
						     protocol_id, &type,
						     ieee)) {
			/* ETH always have the enable bit reset, as it gets
			 * vlan information per packet. For other protocols,
			 * should be set according to the dcbx_enabled
			 * indication, but we only got here if there was an
			 * app tlv for the protocol, so dcbx must be enabled.
			 */
			enable = !(type == DCBX_PROTOCOL_ETH);

			ecore_dcbx_update_app_info(p_data, p_hwfn, enable,
						   priority, tc, type);
		}
	}

	/* Update ramrod protocol data and hw_info fields
	 * with default info when corresponding APP TLV's are not detected.
	 * The enabled field has a different logic for ethernet as only for
	 * ethernet dcb should disabled by default, as the information arrives
	 * from the OS (unless an explicit app tlv was present).
	 */
	tc = p_data->arr[DCBX_PROTOCOL_ETH].tc;
	priority = p_data->arr[DCBX_PROTOCOL_ETH].priority;
	for (type = 0; type < DCBX_MAX_PROTOCOL_TYPE; type++) {
		if (p_data->arr[type].update)
			continue;

		enable = (type == DCBX_PROTOCOL_ETH) ? false : !!dcbx_version;
		ecore_dcbx_update_app_info(p_data, p_hwfn, enable,
					   priority, tc, type);
	}

	return ECORE_SUCCESS;
}

/* Parse app TLV's to update TC information in hw_info structure for
 * reconfiguring QM. Get protocol specific data for PF update ramrod command.
 */
static enum _ecore_status_t
ecore_dcbx_process_mib_info(struct ecore_hwfn *p_hwfn)
{
	struct dcbx_app_priority_feature *p_app;
	enum _ecore_status_t rc = ECORE_SUCCESS;
	struct ecore_dcbx_results data = { 0 };
	struct dcbx_app_priority_entry *p_tbl;
	struct dcbx_ets_feature *p_ets;
	struct ecore_hw_info *p_info;
	u32 pri_tc_tbl, flags;
	u8 dcbx_version;
	int num_entries;

	flags = p_hwfn->p_dcbx_info->operational.flags;
	dcbx_version = ECORE_MFW_GET_FIELD(flags, DCBX_CONFIG_VERSION);

	p_app = &p_hwfn->p_dcbx_info->operational.features.app;
	p_tbl = p_app->app_pri_tbl;

	p_ets = &p_hwfn->p_dcbx_info->operational.features.ets;
	pri_tc_tbl = p_ets->pri_tc_tbl[0];

	p_info = &p_hwfn->hw_info;
	num_entries = ECORE_MFW_GET_FIELD(p_app->flags, DCBX_APP_NUM_ENTRIES);

	rc = ecore_dcbx_process_tlv(p_hwfn, &data, p_tbl, pri_tc_tbl,
				    num_entries, dcbx_version);
	if (rc != ECORE_SUCCESS)
		return rc;

	p_info->num_active_tc = ECORE_MFW_GET_FIELD(p_ets->flags, DCBX_ETS_MAX_TCS);
	p_hwfn->qm_info.ooo_tc = ECORE_MFW_GET_FIELD(p_ets->flags, DCBX_OOO_TC);
	data.pf_id = p_hwfn->rel_pf_id;
	data.dcbx_enabled = !!dcbx_version;

	ecore_dcbx_dp_protocol(p_hwfn, &data);

	OSAL_MEMCPY(&p_hwfn->p_dcbx_info->results, &data,
		    sizeof(struct ecore_dcbx_results));

	return ECORE_SUCCESS;
}

static enum _ecore_status_t
ecore_dcbx_copy_mib(struct ecore_hwfn *p_hwfn,
		    struct ecore_ptt *p_ptt,
		    struct ecore_dcbx_mib_meta_data *p_data,
		    enum ecore_mib_read_type type)
{
	enum _ecore_status_t rc = ECORE_SUCCESS;
	u32 prefix_seq_num, suffix_seq_num;
	int read_count = 0;

	/* The data is considered to be valid only if both sequence numbers are
	 * the same.
	 */
	do {
		if (type == ECORE_DCBX_REMOTE_LLDP_MIB) {
			ecore_memcpy_from(p_hwfn, p_ptt, p_data->lldp_remote,
					  p_data->addr, p_data->size);
			prefix_seq_num = p_data->lldp_remote->prefix_seq_num;
			suffix_seq_num = p_data->lldp_remote->suffix_seq_num;
		} else {
			ecore_memcpy_from(p_hwfn, p_ptt, p_data->mib,
					  p_data->addr, p_data->size);
			prefix_seq_num = p_data->mib->prefix_seq_num;
			suffix_seq_num = p_data->mib->suffix_seq_num;
		}
		read_count++;

		DP_VERBOSE(p_hwfn, ECORE_MSG_DCB,
			   "mib type = %d, try count = %d prefix seq num  = %d suffix seq num = %d\n",
			   type, read_count, prefix_seq_num, suffix_seq_num);
	} while ((prefix_seq_num != suffix_seq_num) &&
		 (read_count < ECORE_DCBX_MAX_MIB_READ_TRY));

	if (read_count >= ECORE_DCBX_MAX_MIB_READ_TRY) {
		DP_ERR(p_hwfn,
		       "MIB read err, mib type = %d, try count = %d prefix seq num = %d suffix seq num = %d\n",
		       type, read_count, prefix_seq_num, suffix_seq_num);
		rc = ECORE_IO;
	}

	return rc;
}

static void
ecore_dcbx_get_priority_info(struct ecore_hwfn *p_hwfn,
			     struct ecore_dcbx_app_prio *p_prio,
			     struct ecore_dcbx_results *p_results)
{
	u8 val;

	p_prio->roce = ECORE_DCBX_INVALID_PRIORITY;
	p_prio->roce_v2 = ECORE_DCBX_INVALID_PRIORITY;
	p_prio->iscsi = ECORE_DCBX_INVALID_PRIORITY;
	p_prio->fcoe = ECORE_DCBX_INVALID_PRIORITY;

	if (p_results->arr[DCBX_PROTOCOL_ROCE].update &&
	    p_results->arr[DCBX_PROTOCOL_ROCE].enable)
		p_prio->roce = p_results->arr[DCBX_PROTOCOL_ROCE].priority;

	if (p_results->arr[DCBX_PROTOCOL_ROCE_V2].update &&
	    p_results->arr[DCBX_PROTOCOL_ROCE_V2].enable) {
		val = p_results->arr[DCBX_PROTOCOL_ROCE_V2].priority;
		p_prio->roce_v2 = val;
	}

	if (p_results->arr[DCBX_PROTOCOL_ISCSI].update &&
	    p_results->arr[DCBX_PROTOCOL_ISCSI].enable)
		p_prio->iscsi = p_results->arr[DCBX_PROTOCOL_ISCSI].priority;

	if (p_results->arr[DCBX_PROTOCOL_FCOE].update &&
	    p_results->arr[DCBX_PROTOCOL_FCOE].enable)
		p_prio->fcoe = p_results->arr[DCBX_PROTOCOL_FCOE].priority;

	if (p_results->arr[DCBX_PROTOCOL_ETH].update &&
	    p_results->arr[DCBX_PROTOCOL_ETH].enable)
		p_prio->eth = p_results->arr[DCBX_PROTOCOL_ETH].priority;

	DP_VERBOSE(p_hwfn, ECORE_MSG_DCB,
		   "Priorities: iscsi %d, roce %d, roce v2 %d, fcoe %d, eth %d\n",
		   p_prio->iscsi, p_prio->roce, p_prio->roce_v2, p_prio->fcoe,
		   p_prio->eth);
}

static void
ecore_dcbx_get_app_data(struct ecore_hwfn *p_hwfn,
			struct dcbx_app_priority_feature *p_app,
			struct dcbx_app_priority_entry *p_tbl,
			struct ecore_dcbx_params *p_params, bool ieee)
{
	struct ecore_app_entry *entry;
	u8 pri_map;
	int i;

	p_params->app_willing = ECORE_MFW_GET_FIELD(p_app->flags,
						    DCBX_APP_WILLING);
	p_params->app_valid = ECORE_MFW_GET_FIELD(p_app->flags,
						  DCBX_APP_ENABLED);
	p_params->app_error = ECORE_MFW_GET_FIELD(p_app->flags, DCBX_APP_ERROR);
	p_params->num_app_entries = ECORE_MFW_GET_FIELD(p_app->flags,
							DCBX_APP_NUM_ENTRIES);
	for (i = 0; i < DCBX_MAX_APP_PROTOCOL; i++) {
		entry = &p_params->app_entry[i];
		if (ieee) {
			u8 sf_ieee;
			u32 val;

			sf_ieee = ECORE_MFW_GET_FIELD(p_tbl[i].entry,
						      DCBX_APP_SF_IEEE);
			switch (sf_ieee) {
			case DCBX_APP_SF_IEEE_RESERVED:
				/* Old MFW */
				val = ECORE_MFW_GET_FIELD(p_tbl[i].entry,
							    DCBX_APP_SF);
				entry->sf_ieee = val ?
					ECORE_DCBX_SF_IEEE_TCP_UDP_PORT :
					ECORE_DCBX_SF_IEEE_ETHTYPE;
				break;
			case DCBX_APP_SF_IEEE_ETHTYPE:
				entry->sf_ieee = ECORE_DCBX_SF_IEEE_ETHTYPE;
				break;
			case DCBX_APP_SF_IEEE_TCP_PORT:
				entry->sf_ieee = ECORE_DCBX_SF_IEEE_TCP_PORT;
				break;
			case DCBX_APP_SF_IEEE_UDP_PORT:
				entry->sf_ieee = ECORE_DCBX_SF_IEEE_UDP_PORT;
				break;
			case DCBX_APP_SF_IEEE_TCP_UDP_PORT:
				entry->sf_ieee = ECORE_DCBX_SF_IEEE_TCP_UDP_PORT;
				break;
			}
		} else {
			entry->ethtype = !(ECORE_MFW_GET_FIELD(p_tbl[i].entry,
							       DCBX_APP_SF));
		}

		pri_map = ECORE_MFW_GET_FIELD(p_tbl[i].entry, DCBX_APP_PRI_MAP);
		ecore_dcbx_get_app_priority(pri_map, &entry->prio);
		entry->proto_id = ECORE_MFW_GET_FIELD(p_tbl[i].entry,
						      DCBX_APP_PROTOCOL_ID);
		ecore_dcbx_get_app_protocol_type(p_hwfn, p_tbl[i].entry,
						 entry->proto_id,
						 &entry->proto_type, ieee);
	}

	DP_VERBOSE(p_hwfn, ECORE_MSG_DCB,
		   "APP params: willing %d, valid %d error = %d\n",
		   p_params->app_willing, p_params->app_valid,
		   p_params->app_error);
}

static void
ecore_dcbx_get_pfc_data(struct ecore_hwfn *p_hwfn,
			u32 pfc, struct ecore_dcbx_params *p_params)
{
	u8 pfc_map;

	p_params->pfc.willing = ECORE_MFW_GET_FIELD(pfc, DCBX_PFC_WILLING);
	p_params->pfc.max_tc = ECORE_MFW_GET_FIELD(pfc, DCBX_PFC_CAPS);
	p_params->pfc.enabled = ECORE_MFW_GET_FIELD(pfc, DCBX_PFC_ENABLED);
	pfc_map = ECORE_MFW_GET_FIELD(pfc, DCBX_PFC_PRI_EN_BITMAP);
	p_params->pfc.prio[0] = !!(pfc_map & DCBX_PFC_PRI_EN_BITMAP_PRI_0);
	p_params->pfc.prio[1] = !!(pfc_map & DCBX_PFC_PRI_EN_BITMAP_PRI_1);
	p_params->pfc.prio[2] = !!(pfc_map & DCBX_PFC_PRI_EN_BITMAP_PRI_2);
	p_params->pfc.prio[3] = !!(pfc_map & DCBX_PFC_PRI_EN_BITMAP_PRI_3);
	p_params->pfc.prio[4] = !!(pfc_map & DCBX_PFC_PRI_EN_BITMAP_PRI_4);
	p_params->pfc.prio[5] = !!(pfc_map & DCBX_PFC_PRI_EN_BITMAP_PRI_5);
	p_params->pfc.prio[6] = !!(pfc_map & DCBX_PFC_PRI_EN_BITMAP_PRI_6);
	p_params->pfc.prio[7] = !!(pfc_map & DCBX_PFC_PRI_EN_BITMAP_PRI_7);

	DP_VERBOSE(p_hwfn, ECORE_MSG_DCB,
		   "PFC params: willing %d, pfc_bitmap %u max_tc = %u enabled = %d\n",
		   p_params->pfc.willing, pfc_map, p_params->pfc.max_tc,
		   p_params->pfc.enabled);
}

static void
ecore_dcbx_get_ets_data(struct ecore_hwfn *p_hwfn,
			struct dcbx_ets_feature *p_ets,
			struct ecore_dcbx_params *p_params)
{
	u32 bw_map[2], tsa_map[2], pri_map;
	int i;

	p_params->ets_willing = ECORE_MFW_GET_FIELD(p_ets->flags,
						    DCBX_ETS_WILLING);
	p_params->ets_enabled = ECORE_MFW_GET_FIELD(p_ets->flags,
						    DCBX_ETS_ENABLED);
	p_params->ets_cbs = ECORE_MFW_GET_FIELD(p_ets->flags, DCBX_ETS_CBS);
	p_params->max_ets_tc = ECORE_MFW_GET_FIELD(p_ets->flags,
						   DCBX_ETS_MAX_TCS);
	DP_VERBOSE(p_hwfn, ECORE_MSG_DCB,
		   "ETS params: willing %d, enabled = %d ets_cbs %d pri_tc_tbl_0 %x max_ets_tc %d\n",
		   p_params->ets_willing, p_params->ets_enabled,
		   p_params->ets_cbs, p_ets->pri_tc_tbl[0],
		   p_params->max_ets_tc);
	if (p_params->ets_enabled && !p_params->max_ets_tc)
	{
		p_params->max_ets_tc = ECORE_MAX_PFC_PRIORITIES;
		DP_VERBOSE(p_hwfn, ECORE_MSG_DCB,
			   "ETS params: max_ets_tc is forced to %d\n",
		   p_params->max_ets_tc);
	}
	/* 8 bit tsa and bw data corresponding to each of the 8 TC's are
	 * encoded in a type u32 array of size 2.
	 */
	bw_map[0] = OSAL_BE32_TO_CPU(p_ets->tc_bw_tbl[0]);
	bw_map[1] = OSAL_BE32_TO_CPU(p_ets->tc_bw_tbl[1]);
	tsa_map[0] = OSAL_BE32_TO_CPU(p_ets->tc_tsa_tbl[0]);
	tsa_map[1] = OSAL_BE32_TO_CPU(p_ets->tc_tsa_tbl[1]);
	pri_map = p_ets->pri_tc_tbl[0];
	for (i = 0; i < ECORE_MAX_PFC_PRIORITIES; i++) {
		p_params->ets_tc_bw_tbl[i] = ((u8 *)bw_map)[i];
		p_params->ets_tc_tsa_tbl[i] = ((u8 *)tsa_map)[i];
		p_params->ets_pri_tc_tbl[i] = ECORE_DCBX_PRIO2TC(pri_map, i);
		DP_VERBOSE(p_hwfn, ECORE_MSG_DCB,
			   "elem %d  bw_tbl %x tsa_tbl %x\n",
			   i, p_params->ets_tc_bw_tbl[i],
			   p_params->ets_tc_tsa_tbl[i]);
	}
}

static void
ecore_dcbx_get_common_params(struct ecore_hwfn *p_hwfn,
			     struct dcbx_app_priority_feature *p_app,
			     struct dcbx_app_priority_entry *p_tbl,
			     struct dcbx_ets_feature *p_ets,
			     u32 pfc, struct ecore_dcbx_params *p_params,
			     bool ieee)
{
	ecore_dcbx_get_app_data(p_hwfn, p_app, p_tbl, p_params, ieee);
	ecore_dcbx_get_ets_data(p_hwfn, p_ets, p_params);
	ecore_dcbx_get_pfc_data(p_hwfn, pfc, p_params);
}

static void
ecore_dcbx_get_local_params(struct ecore_hwfn *p_hwfn,
			    struct ecore_ptt *p_ptt,
			    struct ecore_dcbx_get *params)
{
	struct dcbx_features *p_feat;

	p_feat = &p_hwfn->p_dcbx_info->local_admin.features;
	ecore_dcbx_get_common_params(p_hwfn, &p_feat->app,
				     p_feat->app.app_pri_tbl, &p_feat->ets,
				     p_feat->pfc, &params->local.params, false);
	params->local.valid = true;
}

static void
ecore_dcbx_get_remote_params(struct ecore_hwfn *p_hwfn,
			     struct ecore_ptt *p_ptt,
			     struct ecore_dcbx_get *params)
{
	struct dcbx_features *p_feat;

	p_feat = &p_hwfn->p_dcbx_info->remote.features;
	ecore_dcbx_get_common_params(p_hwfn, &p_feat->app,
				     p_feat->app.app_pri_tbl, &p_feat->ets,
				     p_feat->pfc, &params->remote.params,
				     false);
	params->remote.valid = true;
}

static enum _ecore_status_t
ecore_dcbx_get_operational_params(struct ecore_hwfn *p_hwfn,
				  struct ecore_ptt *p_ptt,
				  struct ecore_dcbx_get *params)
{
	struct ecore_dcbx_operational_params *p_operational;
	struct ecore_dcbx_results *p_results;
	struct dcbx_features *p_feat;
	bool enabled, err;
	u32 flags;
	bool val;

	flags = p_hwfn->p_dcbx_info->operational.flags;

	/* If DCBx version is non zero, then negotiation
	 * was successfuly performed
	 */
	p_operational = &params->operational;
	enabled = !!(ECORE_MFW_GET_FIELD(flags, DCBX_CONFIG_VERSION) !=
		     DCBX_CONFIG_VERSION_DISABLED);
	if (!enabled) {
		p_operational->enabled = enabled;
		p_operational->valid = false;
		DP_VERBOSE(p_hwfn, ECORE_MSG_DCB, "Dcbx is disabled\n");
		return ECORE_INVAL;
	}

	p_feat = &p_hwfn->p_dcbx_info->operational.features;
	p_results = &p_hwfn->p_dcbx_info->results;

	val = !!(ECORE_MFW_GET_FIELD(flags, DCBX_CONFIG_VERSION) ==
		 DCBX_CONFIG_VERSION_IEEE);
	p_operational->ieee = val;

	val = !!(ECORE_MFW_GET_FIELD(flags, DCBX_CONFIG_VERSION) ==
		 DCBX_CONFIG_VERSION_CEE);
	p_operational->cee = val;

	val = !!(ECORE_MFW_GET_FIELD(flags, DCBX_CONFIG_VERSION) ==
		 DCBX_CONFIG_VERSION_STATIC);
	p_operational->local = val;

	DP_VERBOSE(p_hwfn, ECORE_MSG_DCB,
		   "Version support: ieee %d, cee %d, static %d\n",
		   p_operational->ieee, p_operational->cee,
		   p_operational->local);

	ecore_dcbx_get_common_params(p_hwfn, &p_feat->app,
				     p_feat->app.app_pri_tbl, &p_feat->ets,
				     p_feat->pfc, &params->operational.params,
				     p_operational->ieee);
	ecore_dcbx_get_priority_info(p_hwfn, &p_operational->app_prio,
				     p_results);
	err = ECORE_MFW_GET_FIELD(p_feat->app.flags, DCBX_APP_ERROR);
	p_operational->err = err;
	p_operational->enabled = enabled;
	p_operational->valid = true;

	return ECORE_SUCCESS;
}

static void
ecore_dcbx_get_dscp_params(struct ecore_hwfn *p_hwfn,
			   struct ecore_ptt *p_ptt,
			   struct ecore_dcbx_get *params)
{
	struct ecore_dcbx_dscp_params *p_dscp;
	struct dcb_dscp_map *p_dscp_map;
	int i, j, entry;
	u32 pri_map;

	p_dscp = &params->dscp;
	p_dscp_map = &p_hwfn->p_dcbx_info->dscp_map;
	p_dscp->enabled = ECORE_MFW_GET_FIELD(p_dscp_map->flags,
					      DCB_DSCP_ENABLE);
	/* MFW encodes 64 dscp entries into 8 element array of u32 entries,
	 * where each entry holds the 4bit priority map for 8 dscp entries.
	 */
	for (i = 0, entry = 0; i < ECORE_DCBX_DSCP_SIZE / 8; i++) {
		pri_map = OSAL_BE32_TO_CPU(p_dscp_map->dscp_pri_map[i]);
		DP_VERBOSE(p_hwfn, ECORE_MSG_DCB, "elem %d pri_map 0x%x\n",
			   entry, pri_map);
		for (j = 0; j < ECORE_DCBX_DSCP_SIZE / 8; j++, entry++)
			p_dscp->dscp_pri_map[entry] = (u32)(pri_map >>
							   (j * 4)) & 0xf;
	}
}

static void
ecore_dcbx_get_local_lldp_params(struct ecore_hwfn *p_hwfn,
				 struct ecore_ptt *p_ptt,
				 struct ecore_dcbx_get *params)
{
	struct lldp_config_params_s *p_local;

	p_local = &p_hwfn->p_dcbx_info->lldp_local[LLDP_NEAREST_BRIDGE];

	OSAL_MEMCPY(params->lldp_local.local_chassis_id,
		    p_local->local_chassis_id,
		    OSAL_ARRAY_SIZE(p_local->local_chassis_id));
	OSAL_MEMCPY(params->lldp_local.local_port_id, p_local->local_port_id,
		    OSAL_ARRAY_SIZE(p_local->local_port_id));
}

static void
ecore_dcbx_get_remote_lldp_params(struct ecore_hwfn *p_hwfn,
				  struct ecore_ptt *p_ptt,
				  struct ecore_dcbx_get *params)
{
	struct lldp_status_params_s *p_remote;

	p_remote = &p_hwfn->p_dcbx_info->lldp_remote[LLDP_NEAREST_BRIDGE];

	OSAL_MEMCPY(params->lldp_remote.peer_chassis_id,
		    p_remote->peer_chassis_id,
		    OSAL_ARRAY_SIZE(p_remote->peer_chassis_id));
	OSAL_MEMCPY(params->lldp_remote.peer_port_id, p_remote->peer_port_id,
		    OSAL_ARRAY_SIZE(p_remote->peer_port_id));
}

static enum _ecore_status_t
ecore_dcbx_get_params(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt,
		      struct ecore_dcbx_get *p_params,
		      enum ecore_mib_read_type type)
{
	enum _ecore_status_t rc = ECORE_SUCCESS;

	switch (type) {
	case ECORE_DCBX_REMOTE_MIB:
		ecore_dcbx_get_remote_params(p_hwfn, p_ptt, p_params);
		break;
	case ECORE_DCBX_LOCAL_MIB:
		ecore_dcbx_get_local_params(p_hwfn, p_ptt, p_params);
		break;
	case ECORE_DCBX_OPERATIONAL_MIB:
		ecore_dcbx_get_operational_params(p_hwfn, p_ptt, p_params);
		break;
	case ECORE_DCBX_REMOTE_LLDP_MIB:
		ecore_dcbx_get_remote_lldp_params(p_hwfn, p_ptt, p_params);
		break;
	case ECORE_DCBX_LOCAL_LLDP_MIB:
		ecore_dcbx_get_local_lldp_params(p_hwfn, p_ptt, p_params);
		break;
	default:
		DP_ERR(p_hwfn, "MIB read err, unknown mib type %d\n", type);
		return ECORE_INVAL;
	}

	return rc;
}

static enum _ecore_status_t
ecore_dcbx_read_local_lldp_mib(struct ecore_hwfn *p_hwfn,
			       struct ecore_ptt *p_ptt)
{
	struct ecore_dcbx_mib_meta_data data;
	enum _ecore_status_t rc = ECORE_SUCCESS;

	OSAL_MEM_ZERO(&data, sizeof(data));
	data.addr = p_hwfn->mcp_info->port_addr + offsetof(struct public_port,
							   lldp_config_params);
	data.lldp_local = p_hwfn->p_dcbx_info->lldp_local;
	data.size = sizeof(struct lldp_config_params_s);
	ecore_memcpy_from(p_hwfn, p_ptt, data.lldp_local, data.addr, data.size);

	return rc;
}

static enum _ecore_status_t
ecore_dcbx_read_remote_lldp_mib(struct ecore_hwfn *p_hwfn,
				struct ecore_ptt *p_ptt,
				enum ecore_mib_read_type type)
{
	struct ecore_dcbx_mib_meta_data data;
	enum _ecore_status_t rc = ECORE_SUCCESS;

	OSAL_MEM_ZERO(&data, sizeof(data));
	data.addr = p_hwfn->mcp_info->port_addr + offsetof(struct public_port,
							   lldp_status_params);
	data.lldp_remote = p_hwfn->p_dcbx_info->lldp_remote;
	data.size = sizeof(struct lldp_status_params_s);
	rc = ecore_dcbx_copy_mib(p_hwfn, p_ptt, &data, type);

	return rc;
}

static enum _ecore_status_t
ecore_dcbx_read_operational_mib(struct ecore_hwfn *p_hwfn,
				struct ecore_ptt *p_ptt,
				enum ecore_mib_read_type type)
{
	struct ecore_dcbx_mib_meta_data data;
	enum _ecore_status_t rc = ECORE_SUCCESS;

	OSAL_MEM_ZERO(&data, sizeof(data));
	data.addr = p_hwfn->mcp_info->port_addr +
		    offsetof(struct public_port, operational_dcbx_mib);
	data.mib = &p_hwfn->p_dcbx_info->operational;
	data.size = sizeof(struct dcbx_mib);
	rc = ecore_dcbx_copy_mib(p_hwfn, p_ptt, &data, type);

	return rc;
}

static enum _ecore_status_t
ecore_dcbx_read_remote_mib(struct ecore_hwfn *p_hwfn,
			   struct ecore_ptt *p_ptt,
			   enum ecore_mib_read_type type)
{
	struct ecore_dcbx_mib_meta_data data;
	enum _ecore_status_t rc = ECORE_SUCCESS;

	OSAL_MEM_ZERO(&data, sizeof(data));
	data.addr = p_hwfn->mcp_info->port_addr +
		    offsetof(struct public_port, remote_dcbx_mib);
	data.mib = &p_hwfn->p_dcbx_info->remote;
	data.size = sizeof(struct dcbx_mib);
	rc = ecore_dcbx_copy_mib(p_hwfn, p_ptt, &data, type);

	return rc;
}

static enum _ecore_status_t
ecore_dcbx_read_local_mib(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt)
{
	struct ecore_dcbx_mib_meta_data data;
	enum _ecore_status_t rc = ECORE_SUCCESS;

	OSAL_MEM_ZERO(&data, sizeof(data));
	data.addr = p_hwfn->mcp_info->port_addr +
			offsetof(struct public_port, local_admin_dcbx_mib);
	data.local_admin = &p_hwfn->p_dcbx_info->local_admin;
	data.size = sizeof(struct dcbx_local_params);
	ecore_memcpy_from(p_hwfn, p_ptt, data.local_admin,
			  data.addr, data.size);

	return rc;
}

static void
ecore_dcbx_read_dscp_mib(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt)
{
	struct ecore_dcbx_mib_meta_data data;

	data.addr = p_hwfn->mcp_info->port_addr +
			offsetof(struct public_port, dcb_dscp_map);
	data.dscp_map = &p_hwfn->p_dcbx_info->dscp_map;
	data.size = sizeof(struct dcb_dscp_map);
	ecore_memcpy_from(p_hwfn, p_ptt, data.dscp_map, data.addr, data.size);
}

static enum _ecore_status_t ecore_dcbx_read_mib(struct ecore_hwfn *p_hwfn,
						struct ecore_ptt *p_ptt,
						enum ecore_mib_read_type type)
{
	enum _ecore_status_t rc = ECORE_INVAL;

	switch (type) {
	case ECORE_DCBX_OPERATIONAL_MIB:
		ecore_dcbx_read_dscp_mib(p_hwfn, p_ptt);
		rc = ecore_dcbx_read_operational_mib(p_hwfn, p_ptt, type);
		break;
	case ECORE_DCBX_REMOTE_MIB:
		rc = ecore_dcbx_read_remote_mib(p_hwfn, p_ptt, type);
		break;
	case ECORE_DCBX_LOCAL_MIB:
		rc = ecore_dcbx_read_local_mib(p_hwfn, p_ptt);
		break;
	case ECORE_DCBX_REMOTE_LLDP_MIB:
		rc = ecore_dcbx_read_remote_lldp_mib(p_hwfn, p_ptt, type);
		break;
	case ECORE_DCBX_LOCAL_LLDP_MIB:
		rc = ecore_dcbx_read_local_lldp_mib(p_hwfn, p_ptt);
		break;
	default:
		DP_ERR(p_hwfn, "MIB read err, unknown mib type %d\n", type);
	}

	return rc;
}

/*
 * Read updated MIB.
 * Reconfigure QM and invoke PF update ramrod command if operational MIB
 * change is detected.
 */
enum _ecore_status_t
ecore_dcbx_mib_update_event(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt,
			    enum ecore_mib_read_type type)
{
	enum _ecore_status_t rc = ECORE_SUCCESS;

	rc = ecore_dcbx_read_mib(p_hwfn, p_ptt, type);
	if (rc)
		return rc;

	if (type == ECORE_DCBX_OPERATIONAL_MIB) {
		ecore_dcbx_get_dscp_params(p_hwfn, p_ptt,
					   &p_hwfn->p_dcbx_info->get);

		rc = ecore_dcbx_process_mib_info(p_hwfn);
		if (!rc) {
			bool enabled;

			/* reconfigure tcs of QM queues according
			 * to negotiation results
			 */
			ecore_qm_reconf(p_hwfn, p_ptt);

			/* update storm FW with negotiation results */
			ecore_sp_pf_update_dcbx(p_hwfn);

			/* set eagle enigne 1 flow control workaround
			 * according to negotiation results
			 */
			enabled = p_hwfn->p_dcbx_info->results.dcbx_enabled;

#ifdef CONFIG_ECORE_ROCE
			/* for roce PFs, we may want to enable/disable DPM
			 * when DCBx change occurs
			 */
			if (ECORE_IS_ROCE_PERSONALITY(p_hwfn))
				ecore_roce_dpm_dcbx(p_hwfn, p_ptt);
#endif
		}
	}

	ecore_dcbx_get_params(p_hwfn, p_ptt, &p_hwfn->p_dcbx_info->get, type);

	if (type == ECORE_DCBX_OPERATIONAL_MIB) {
		struct ecore_dcbx_results *p_data;
		u16 val;

		/* Update the DSCP to TC mapping bit if required */
		if (p_hwfn->p_dcbx_info->dscp_nig_update) {
			ecore_wr(p_hwfn, p_ptt, NIG_REG_DSCP_TO_TC_MAP_ENABLE,
				 0x1);
			p_hwfn->p_dcbx_info->dscp_nig_update = false;
		}

		/* Configure in NIG which protocols support EDPM and should
		 * honor PFC.
		 */
		p_data = &p_hwfn->p_dcbx_info->results;
		val = (0x1 << p_data->arr[DCBX_PROTOCOL_ROCE].tc) |
			(0x1 << p_data->arr[DCBX_PROTOCOL_ROCE_V2].tc);
		val <<= NIG_REG_TX_EDPM_CTRL_TX_EDPM_TC_EN_SHIFT;
		val |= NIG_REG_TX_EDPM_CTRL_TX_EDPM_EN;
		ecore_wr(p_hwfn, p_ptt, NIG_REG_TX_EDPM_CTRL, val);
	}

	OSAL_DCBX_AEN(p_hwfn, type);

	return rc;
}

enum _ecore_status_t ecore_dcbx_info_alloc(struct ecore_hwfn *p_hwfn)
{
#ifndef __EXTRACT__LINUX__
	OSAL_BUILD_BUG_ON(ECORE_LLDP_CHASSIS_ID_STAT_LEN !=
			  LLDP_CHASSIS_ID_STAT_LEN);
	OSAL_BUILD_BUG_ON(ECORE_LLDP_PORT_ID_STAT_LEN !=
			  LLDP_PORT_ID_STAT_LEN);
	OSAL_BUILD_BUG_ON(ECORE_DCBX_MAX_APP_PROTOCOL !=
			  DCBX_MAX_APP_PROTOCOL);
#endif

	p_hwfn->p_dcbx_info = OSAL_ZALLOC(p_hwfn->p_dev, GFP_KERNEL,
					  sizeof(*p_hwfn->p_dcbx_info));
	if (!p_hwfn->p_dcbx_info) {
		DP_NOTICE(p_hwfn, true,
			  "Failed to allocate `struct ecore_dcbx_info'");
		return ECORE_NOMEM;
	}

	p_hwfn->p_dcbx_info->iwarp_port =
		p_hwfn->pf_params.rdma_pf_params.iwarp_port;

	return ECORE_SUCCESS;
}

void ecore_dcbx_info_free(struct ecore_hwfn *p_hwfn)
{
	OSAL_FREE(p_hwfn->p_dev, p_hwfn->p_dcbx_info);
	p_hwfn->p_dcbx_info = OSAL_NULL;
}

static void ecore_dcbx_update_protocol_data(struct protocol_dcb_data *p_data,
					    struct ecore_dcbx_results *p_src,
					    enum dcbx_protocol_type type)
{
	p_data->dcb_enable_flag = p_src->arr[type].enable;
	p_data->dcb_priority = p_src->arr[type].priority;
	p_data->dcb_tc = p_src->arr[type].tc;
	p_data->dscp_enable_flag = p_src->arr[type].dscp_enable;
	p_data->dscp_val = p_src->arr[type].dscp_val;
}

/* Set pf update ramrod command params */
void ecore_dcbx_set_pf_update_params(struct ecore_dcbx_results *p_src,
				     struct pf_update_ramrod_data *p_dest)
{
	struct protocol_dcb_data *p_dcb_data;
	u8 update_flag;

	p_dest->pf_id = p_src->pf_id;

	update_flag = p_src->arr[DCBX_PROTOCOL_FCOE].update;
	p_dest->update_fcoe_dcb_data_mode = update_flag;

	update_flag = p_src->arr[DCBX_PROTOCOL_ROCE].update;
	p_dest->update_roce_dcb_data_mode = update_flag;

	update_flag = p_src->arr[DCBX_PROTOCOL_ROCE_V2].update;
	p_dest->update_rroce_dcb_data_mode = update_flag;

	update_flag = p_src->arr[DCBX_PROTOCOL_ISCSI].update;
	p_dest->update_iscsi_dcb_data_mode = update_flag;
	update_flag = p_src->arr[DCBX_PROTOCOL_ETH].update;
	p_dest->update_eth_dcb_data_mode = update_flag;
	update_flag = p_src->arr[DCBX_PROTOCOL_IWARP].update;
	p_dest->update_iwarp_dcb_data_mode = update_flag;

	p_dcb_data = &p_dest->fcoe_dcb_data;
	ecore_dcbx_update_protocol_data(p_dcb_data, p_src, DCBX_PROTOCOL_FCOE);
	p_dcb_data = &p_dest->roce_dcb_data;
	ecore_dcbx_update_protocol_data(p_dcb_data, p_src, DCBX_PROTOCOL_ROCE);
	p_dcb_data = &p_dest->rroce_dcb_data;
	ecore_dcbx_update_protocol_data(p_dcb_data, p_src,
					DCBX_PROTOCOL_ROCE_V2);
	p_dcb_data = &p_dest->iscsi_dcb_data;
	ecore_dcbx_update_protocol_data(p_dcb_data, p_src, DCBX_PROTOCOL_ISCSI);
	p_dcb_data = &p_dest->eth_dcb_data;
	ecore_dcbx_update_protocol_data(p_dcb_data, p_src, DCBX_PROTOCOL_ETH);
	p_dcb_data = &p_dest->iwarp_dcb_data;
	ecore_dcbx_update_protocol_data(p_dcb_data, p_src, DCBX_PROTOCOL_IWARP);
}

enum _ecore_status_t ecore_dcbx_query_params(struct ecore_hwfn *p_hwfn,
					     struct ecore_dcbx_get *p_get,
					     enum ecore_mib_read_type type)
{
	struct ecore_ptt *p_ptt;
	enum _ecore_status_t rc;

	if (IS_VF(p_hwfn->p_dev))
		return ECORE_INVAL;

	p_ptt = ecore_ptt_acquire(p_hwfn);
	if (!p_ptt) {
		rc = ECORE_TIMEOUT;
		DP_ERR(p_hwfn, "rc = %d\n", rc);
		return rc;
	}

	rc = ecore_dcbx_read_mib(p_hwfn, p_ptt, type);
	if (rc != ECORE_SUCCESS)
		goto out;

	rc = ecore_dcbx_get_params(p_hwfn, p_ptt, p_get, type);

out:
	ecore_ptt_release(p_hwfn, p_ptt);
	return rc;
}

static void
ecore_dcbx_set_pfc_data(struct ecore_hwfn *p_hwfn,
			u32 *pfc, struct ecore_dcbx_params *p_params)
{
	u8 pfc_map = 0;
	int i;

	*pfc &= ~DCBX_PFC_ERROR_MASK;

	if (p_params->pfc.willing)
		*pfc |= DCBX_PFC_WILLING_MASK;
	else
		*pfc &= ~DCBX_PFC_WILLING_MASK;

	if (p_params->pfc.enabled)
		*pfc |= DCBX_PFC_ENABLED_MASK;
	else
		*pfc &= ~DCBX_PFC_ENABLED_MASK;

	*pfc &= ~DCBX_PFC_CAPS_MASK;
	*pfc |= (u32)p_params->pfc.max_tc << DCBX_PFC_CAPS_SHIFT;

	for (i = 0; i < ECORE_MAX_PFC_PRIORITIES; i++)
		if (p_params->pfc.prio[i])
			pfc_map |= (1 << i);
	*pfc &= ~DCBX_PFC_PRI_EN_BITMAP_MASK;
	*pfc |= (pfc_map << DCBX_PFC_PRI_EN_BITMAP_SHIFT);

	DP_VERBOSE(p_hwfn, ECORE_MSG_DCB, "pfc = 0x%x\n", *pfc);
}

static void
ecore_dcbx_set_ets_data(struct ecore_hwfn *p_hwfn,
			struct dcbx_ets_feature *p_ets,
			struct ecore_dcbx_params *p_params)
{
	u8 *bw_map, *tsa_map;
	u32 val;
	int i;

	if (p_params->ets_willing)
		p_ets->flags |= DCBX_ETS_WILLING_MASK;
	else
		p_ets->flags &= ~DCBX_ETS_WILLING_MASK;

	if (p_params->ets_cbs)
		p_ets->flags |= DCBX_ETS_CBS_MASK;
	else
		p_ets->flags &= ~DCBX_ETS_CBS_MASK;

	if (p_params->ets_enabled)
		p_ets->flags |= DCBX_ETS_ENABLED_MASK;
	else
		p_ets->flags &= ~DCBX_ETS_ENABLED_MASK;

	p_ets->flags &= ~DCBX_ETS_MAX_TCS_MASK;
	p_ets->flags |= (u32)p_params->max_ets_tc << DCBX_ETS_MAX_TCS_SHIFT;

	bw_map = (u8 *)&p_ets->tc_bw_tbl[0];
	tsa_map = (u8 *)&p_ets->tc_tsa_tbl[0];
	p_ets->pri_tc_tbl[0] = 0;
	for (i = 0; i < ECORE_MAX_PFC_PRIORITIES; i++) {
		bw_map[i] = p_params->ets_tc_bw_tbl[i];
		tsa_map[i] = p_params->ets_tc_tsa_tbl[i];
		/* Copy the priority value to the corresponding 4 bits in the
		 * traffic class table.
		 */
		val = (((u32)p_params->ets_pri_tc_tbl[i]) << ((7 - i) * 4));
		p_ets->pri_tc_tbl[0] |= val;
	}
	for (i = 0; i < 2; i++) {
		p_ets->tc_bw_tbl[i] = OSAL_CPU_TO_BE32(p_ets->tc_bw_tbl[i]);
		p_ets->tc_tsa_tbl[i] = OSAL_CPU_TO_BE32(p_ets->tc_tsa_tbl[i]);
	}

	DP_VERBOSE(p_hwfn, ECORE_MSG_DCB,
		   "flags = 0x%x pri_tc = 0x%x tc_bwl[] = {0x%x, 0x%x} tc_tsa = {0x%x, 0x%x}\n",
		   p_ets->flags, p_ets->pri_tc_tbl[0], p_ets->tc_bw_tbl[0],
		   p_ets->tc_bw_tbl[1], p_ets->tc_tsa_tbl[0],
		   p_ets->tc_tsa_tbl[1]);
}

static void
ecore_dcbx_set_app_data(struct ecore_hwfn *p_hwfn,
			struct dcbx_app_priority_feature *p_app,
			struct ecore_dcbx_params *p_params, bool ieee)
{
	u32 *entry;
	int i;

	if (p_params->app_willing)
		p_app->flags |= DCBX_APP_WILLING_MASK;
	else
		p_app->flags &= ~DCBX_APP_WILLING_MASK;

	if (p_params->app_valid)
		p_app->flags |= DCBX_APP_ENABLED_MASK;
	else
		p_app->flags &= ~DCBX_APP_ENABLED_MASK;

	p_app->flags &= ~DCBX_APP_NUM_ENTRIES_MASK;
	p_app->flags |= (u32)p_params->num_app_entries <<
					DCBX_APP_NUM_ENTRIES_SHIFT;

	for (i = 0; i < DCBX_MAX_APP_PROTOCOL; i++) {
		entry = &p_app->app_pri_tbl[i].entry;
		*entry = 0;
		if (ieee) {
			*entry &= ~(DCBX_APP_SF_IEEE_MASK | DCBX_APP_SF_MASK);
			switch (p_params->app_entry[i].sf_ieee) {
			case ECORE_DCBX_SF_IEEE_ETHTYPE:
				*entry  |= ((u32)DCBX_APP_SF_IEEE_ETHTYPE <<
					    DCBX_APP_SF_IEEE_SHIFT);
				*entry  |= ((u32)DCBX_APP_SF_ETHTYPE <<
					    DCBX_APP_SF_SHIFT);
				break;
			case ECORE_DCBX_SF_IEEE_TCP_PORT:
				*entry  |= ((u32)DCBX_APP_SF_IEEE_TCP_PORT <<
					    DCBX_APP_SF_IEEE_SHIFT);
				*entry  |= ((u32)DCBX_APP_SF_PORT <<
					    DCBX_APP_SF_SHIFT);
				break;
			case ECORE_DCBX_SF_IEEE_UDP_PORT:
				*entry  |= ((u32)DCBX_APP_SF_IEEE_UDP_PORT <<
					    DCBX_APP_SF_IEEE_SHIFT);
				*entry  |= ((u32)DCBX_APP_SF_PORT <<
					    DCBX_APP_SF_SHIFT);
				break;
			case ECORE_DCBX_SF_IEEE_TCP_UDP_PORT:
				*entry  |= (u32)DCBX_APP_SF_IEEE_TCP_UDP_PORT <<
					    DCBX_APP_SF_IEEE_SHIFT;
				*entry  |= ((u32)DCBX_APP_SF_PORT <<
					    DCBX_APP_SF_SHIFT);
				break;
			}
		} else {
			*entry &= ~DCBX_APP_SF_MASK;
			if (p_params->app_entry[i].ethtype)
				*entry  |= ((u32)DCBX_APP_SF_ETHTYPE <<
					    DCBX_APP_SF_SHIFT);
			else
				*entry  |= ((u32)DCBX_APP_SF_PORT <<
					    DCBX_APP_SF_SHIFT);
		}
		*entry &= ~DCBX_APP_PROTOCOL_ID_MASK;
		*entry |= ((u32)p_params->app_entry[i].proto_id <<
				DCBX_APP_PROTOCOL_ID_SHIFT);
		*entry &= ~DCBX_APP_PRI_MAP_MASK;
		*entry |= ((u32)(p_params->app_entry[i].prio) <<
				DCBX_APP_PRI_MAP_SHIFT);
	}

	DP_VERBOSE(p_hwfn, ECORE_MSG_DCB, "flags = 0x%x\n", p_app->flags);
}

static enum _ecore_status_t
ecore_dcbx_set_local_params(struct ecore_hwfn *p_hwfn,
			    struct dcbx_local_params *local_admin,
			    struct ecore_dcbx_set *params)
{
	bool ieee = false;

	local_admin->flags = 0;
	OSAL_MEMCPY(&local_admin->features,
		    &p_hwfn->p_dcbx_info->operational.features,
		    sizeof(local_admin->features));

	if (params->enabled) {
		local_admin->config = params->ver_num;
		ieee = !!(params->ver_num & DCBX_CONFIG_VERSION_IEEE);
	} else
		local_admin->config = DCBX_CONFIG_VERSION_DISABLED;

	if (params->override_flags & ECORE_DCBX_OVERRIDE_PFC_CFG)
		ecore_dcbx_set_pfc_data(p_hwfn, &local_admin->features.pfc,
					&params->config.params);

	if (params->override_flags & ECORE_DCBX_OVERRIDE_ETS_CFG)
		ecore_dcbx_set_ets_data(p_hwfn, &local_admin->features.ets,
					&params->config.params);

	if (params->override_flags & ECORE_DCBX_OVERRIDE_APP_CFG)
		ecore_dcbx_set_app_data(p_hwfn, &local_admin->features.app,
					&params->config.params, ieee);

	return ECORE_SUCCESS;
}

static enum _ecore_status_t
ecore_dcbx_set_dscp_params(struct ecore_hwfn *p_hwfn,
			   struct dcb_dscp_map *p_dscp_map,
			   struct ecore_dcbx_set *p_params)
{
	int entry, i, j;
	u32 val;

	OSAL_MEMCPY(p_dscp_map, &p_hwfn->p_dcbx_info->dscp_map,
		    sizeof(*p_dscp_map));

	p_dscp_map->flags &= ~DCB_DSCP_ENABLE_MASK;
	if (p_params->dscp.enabled)
		p_dscp_map->flags |= DCB_DSCP_ENABLE_MASK;

	for (i = 0, entry = 0; i < 8; i++) {
		val = 0;
		for (j = 0; j < 8; j++, entry++)
			val |= (((u32)p_params->dscp.dscp_pri_map[entry]) <<
				(j * 4));

		p_dscp_map->dscp_pri_map[i] = OSAL_CPU_TO_BE32(val);
	}

	p_hwfn->p_dcbx_info->dscp_nig_update = true;

	DP_VERBOSE(p_hwfn, ECORE_MSG_DCB, "flags = 0x%x\n", p_dscp_map->flags);

	return ECORE_SUCCESS;
}

enum _ecore_status_t ecore_dcbx_config_params(struct ecore_hwfn *p_hwfn,
					      struct ecore_ptt *p_ptt,
					      struct ecore_dcbx_set *params,
					      bool hw_commit)
{
	struct dcbx_local_params local_admin;
	struct ecore_dcbx_mib_meta_data data;
	struct dcb_dscp_map dscp_map;
	u32 resp = 0, param = 0;
	enum _ecore_status_t rc = ECORE_SUCCESS;

	if (!hw_commit) {
		OSAL_MEMCPY(&p_hwfn->p_dcbx_info->set, params,
			    sizeof(p_hwfn->p_dcbx_info->set));
		return ECORE_SUCCESS;
	}

	/* clear set-parmas cache */
	OSAL_MEMSET(&p_hwfn->p_dcbx_info->set, 0,
		    sizeof(struct ecore_dcbx_set));

	OSAL_MEMSET(&local_admin, 0, sizeof(local_admin));
	ecore_dcbx_set_local_params(p_hwfn, &local_admin, params);

	data.addr = p_hwfn->mcp_info->port_addr +
			offsetof(struct public_port, local_admin_dcbx_mib);
	data.local_admin = &local_admin;
	data.size = sizeof(struct dcbx_local_params);
	ecore_memcpy_to(p_hwfn, p_ptt, data.addr, data.local_admin, data.size);

	if (params->override_flags & ECORE_DCBX_OVERRIDE_DSCP_CFG) {
		OSAL_MEMSET(&dscp_map, 0, sizeof(dscp_map));
		ecore_dcbx_set_dscp_params(p_hwfn, &dscp_map, params);

		data.addr = p_hwfn->mcp_info->port_addr +
				offsetof(struct public_port, dcb_dscp_map);
		data.dscp_map = &dscp_map;
		data.size = sizeof(struct dcb_dscp_map);
		ecore_memcpy_to(p_hwfn, p_ptt, data.addr, data.dscp_map,
				data.size);
	}

	rc = ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_SET_DCBX,
			   1 << DRV_MB_PARAM_LLDP_SEND_SHIFT, &resp, &param);
	if (rc != ECORE_SUCCESS) {
		DP_NOTICE(p_hwfn, false,
			  "Failed to send DCBX update request\n");
		return rc;
	}

	return rc;
}

enum _ecore_status_t ecore_dcbx_get_config_params(struct ecore_hwfn *p_hwfn,
						  struct ecore_dcbx_set *params)
{
	struct ecore_dcbx_get *dcbx_info;
	int rc;

	if (p_hwfn->p_dcbx_info->set.config.valid) {
		OSAL_MEMCPY(params, &p_hwfn->p_dcbx_info->set,
			    sizeof(struct ecore_dcbx_set));
		return ECORE_SUCCESS;
	}

	dcbx_info = OSAL_ALLOC(p_hwfn->p_dev, GFP_KERNEL,
			       sizeof(*dcbx_info));
	if (!dcbx_info) {
		DP_ERR(p_hwfn, "Failed to allocate struct ecore_dcbx_info\n");
		return ECORE_NOMEM;
	}

	OSAL_MEMSET(dcbx_info, 0, sizeof(*dcbx_info));
	rc = ecore_dcbx_query_params(p_hwfn, dcbx_info,
				     ECORE_DCBX_OPERATIONAL_MIB);
	if (rc) {
		OSAL_FREE(p_hwfn->p_dev, dcbx_info);
		return rc;
	}
	p_hwfn->p_dcbx_info->set.override_flags = 0;

	p_hwfn->p_dcbx_info->set.ver_num = DCBX_CONFIG_VERSION_DISABLED;
	if (dcbx_info->operational.cee)
		p_hwfn->p_dcbx_info->set.ver_num |= DCBX_CONFIG_VERSION_CEE;
	if (dcbx_info->operational.ieee)
		p_hwfn->p_dcbx_info->set.ver_num |= DCBX_CONFIG_VERSION_IEEE;
	if (dcbx_info->operational.local)
		p_hwfn->p_dcbx_info->set.ver_num |= DCBX_CONFIG_VERSION_STATIC;

	p_hwfn->p_dcbx_info->set.enabled = dcbx_info->operational.enabled;
	OSAL_MEMCPY(&p_hwfn->p_dcbx_info->set.config.params,
		    &dcbx_info->operational.params,
		    sizeof(struct ecore_dcbx_admin_params));
	p_hwfn->p_dcbx_info->set.config.valid = true;

	OSAL_MEMCPY(params, &p_hwfn->p_dcbx_info->set,
		    sizeof(struct ecore_dcbx_set));

	OSAL_FREE(p_hwfn->p_dev, dcbx_info);

	return ECORE_SUCCESS;
}
