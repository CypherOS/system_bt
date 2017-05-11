/******************************************************************************
 *
 *  Copyright (C) 2004-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This file contains functions for managing the SCO connection used in AG.
 *
 ******************************************************************************/

#include <stddef.h>

#include "bt_common.h"
#include "bta_ag_api.h"
#include "bta_ag_co.h"
#include "bta_ag_int.h"
#include "bta_api.h"
#if (BTM_SCO_HCI_INCLUDED == TRUE)
#include "bta_dm_co.h"
#endif
#include "btm_api.h"
#include "device/include/controller.h"
#include "device/include/esco_parameters.h"
#include "osi/include/osi.h"
#include "utl.h"

#ifndef BTA_AG_SCO_DEBUG
#define BTA_AG_SCO_DEBUG FALSE
#endif

/* Codec negotiation timeout */
#ifndef BTA_AG_CODEC_NEGOTIATION_TIMEOUT_MS
#define BTA_AG_CODEC_NEGOTIATION_TIMEOUT_MS (3 * 1000) /* 3 seconds */
#endif

extern fixed_queue_t* btu_bta_alarm_queue;

#if (BTA_AG_SCO_DEBUG == TRUE)
static char* bta_ag_sco_evt_str(uint8_t event);
static char* bta_ag_sco_state_str(uint8_t state);
#endif

#define BTA_AG_NO_EDR_ESCO                                       \
  (ESCO_PKT_TYPES_MASK_NO_2_EV3 | ESCO_PKT_TYPES_MASK_NO_3_EV3 | \
   ESCO_PKT_TYPES_MASK_NO_2_EV5 | ESCO_PKT_TYPES_MASK_NO_3_EV5)

/* sco events */
enum {
  BTA_AG_SCO_LISTEN_E,       /* listen request */
  BTA_AG_SCO_OPEN_E,         /* open request */
  BTA_AG_SCO_OPEN_PENDING_E, /* Pending operations in open request */
  BTA_AG_SCO_XFER_E,         /* transfer request */
#if (BTM_WBS_INCLUDED == TRUE)
  BTA_AG_SCO_CN_DONE_E, /* codec negotiation done */
  BTA_AG_SCO_REOPEN_E,  /* Retry with other codec when failed */
#endif
  BTA_AG_SCO_CLOSE_E,      /* close request */
  BTA_AG_SCO_SHUTDOWN_E,   /* shutdown request */
  BTA_AG_SCO_CONN_OPEN_E,  /* sco open */
  BTA_AG_SCO_CONN_CLOSE_E, /* sco closed */
  BTA_AG_SCO_CI_DATA_E     /* SCO data ready */
};

/*******************************************************************************
 *
 * Function         bta_ag_sco_conn_cback
 *
 * Description      BTM SCO connection callback.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_ag_sco_conn_cback(uint16_t sco_idx) {
  uint16_t handle;
  tBTA_AG_SCB* p_scb;

  /* match callback to scb; first check current sco scb */
  if (bta_ag_cb.sco.p_curr_scb != NULL && bta_ag_cb.sco.p_curr_scb->in_use) {
    handle = bta_ag_scb_to_idx(bta_ag_cb.sco.p_curr_scb);
  }
  /* then check for scb connected to this peer */
  else {
    /* Check if SLC is up */
    handle = bta_ag_idx_by_bdaddr(BTM_ReadScoBdAddr(sco_idx));
    p_scb = bta_ag_scb_by_idx(handle);
    if (p_scb && !p_scb->svc_conn) handle = 0;
  }

  if (handle != 0) {
    BT_HDR* p_buf = (BT_HDR*)osi_malloc(sizeof(BT_HDR));
    p_buf->event = BTA_AG_SCO_OPEN_EVT;
    p_buf->layer_specific = handle;
    bta_sys_sendmsg(p_buf);
  } else {
    /* no match found; disconnect sco, init sco variables */
    bta_ag_cb.sco.p_curr_scb = NULL;
    bta_ag_cb.sco.state = BTA_AG_SCO_SHUTDOWN_ST;
    BTM_RemoveSco(sco_idx);
  }
}

/*******************************************************************************
 *
 * Function         bta_ag_sco_disc_cback
 *
 * Description      BTM SCO disconnection callback.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_ag_sco_disc_cback(uint16_t sco_idx) {
  uint16_t handle = 0;

  APPL_TRACE_DEBUG(
      "bta_ag_sco_disc_cback(): sco_idx: 0x%x  p_cur_scb: 0x%08x  sco.state: "
      "%d",
      sco_idx, bta_ag_cb.sco.p_curr_scb, bta_ag_cb.sco.state);

  APPL_TRACE_DEBUG(
      "bta_ag_sco_disc_cback(): scb[0] addr: 0x%08x  in_use: %u  sco_idx: 0x%x "
      " sco state: %u",
      &bta_ag_cb.scb[0], bta_ag_cb.scb[0].in_use, bta_ag_cb.scb[0].sco_idx,
      bta_ag_cb.scb[0].state);
  APPL_TRACE_DEBUG(
      "bta_ag_sco_disc_cback(): scb[1] addr: 0x%08x  in_use: %u  sco_idx: 0x%x "
      " sco state: %u",
      &bta_ag_cb.scb[1], bta_ag_cb.scb[1].in_use, bta_ag_cb.scb[1].sco_idx,
      bta_ag_cb.scb[1].state);

  /* match callback to scb */
  if (bta_ag_cb.sco.p_curr_scb != NULL && bta_ag_cb.sco.p_curr_scb->in_use) {
    /* We only care about callbacks for the active SCO */
    if (bta_ag_cb.sco.p_curr_scb->sco_idx != sco_idx) {
      if (bta_ag_cb.sco.p_curr_scb->sco_idx != 0xFFFF) return;
    }
    handle = bta_ag_scb_to_idx(bta_ag_cb.sco.p_curr_scb);
  }

  if (handle != 0) {
#if (BTM_SCO_HCI_INCLUDED == TRUE)

    tBTM_STATUS status =
        BTM_ConfigScoPath(ESCO_DATA_PATH_PCM, NULL, NULL, true);
    APPL_TRACE_DEBUG("%s: sco close config status = %d", __func__, status);
    /* SCO clean up here */
    bta_dm_sco_co_close();
#endif

#if (BTM_WBS_INCLUDED == TRUE)
    /* Restore settings */
    if (bta_ag_cb.sco.p_curr_scb->inuse_codec == BTA_AG_CODEC_MSBC) {
      /* Bypass vendor specific and voice settings if enhanced eSCO supported */
      if (!(controller_get_interface()
                ->supports_enhanced_setup_synchronous_connection())) {
        BTM_WriteVoiceSettings(BTM_VOICE_SETTING_CVSD);
      }

      /* If SCO open was initiated by AG and failed for mSBC T2, try mSBC T1
       * 'Safe setting' first. If T1 also fails, try CVSD */
      if (bta_ag_sco_is_opening(bta_ag_cb.sco.p_curr_scb)) {
        bta_ag_cb.sco.p_curr_scb->state = BTA_AG_SCO_CODEC_ST;
        if (bta_ag_cb.sco.p_curr_scb->codec_msbc_settings ==
            BTA_AG_SCO_MSBC_SETTINGS_T2) {
          APPL_TRACE_DEBUG("%s: Fallback to mSBC T1 settings", __func__);
          bta_ag_cb.sco.p_curr_scb->codec_msbc_settings =
              BTA_AG_SCO_MSBC_SETTINGS_T1;
        } else {
          APPL_TRACE_DEBUG("%s: Fallback to CVSD", __func__);
          bta_ag_cb.sco.p_curr_scb->codec_fallback = true;
        }
      }
    }

    bta_ag_cb.sco.p_curr_scb->inuse_codec = BTA_AG_CODEC_NONE;
#endif

    BT_HDR* p_buf = (BT_HDR*)osi_malloc(sizeof(BT_HDR));
    p_buf->event = BTA_AG_SCO_CLOSE_EVT;
    p_buf->layer_specific = handle;
    bta_sys_sendmsg(p_buf);
  } else {
    /* no match found */
    APPL_TRACE_DEBUG("no scb for ag_sco_disc_cback");

    /* sco could be closed after scb dealloc'ed */
    if (bta_ag_cb.sco.p_curr_scb != NULL) {
      bta_ag_cb.sco.p_curr_scb->sco_idx = BTM_INVALID_SCO_INDEX;
      bta_ag_cb.sco.p_curr_scb = NULL;
      bta_ag_cb.sco.state = BTA_AG_SCO_SHUTDOWN_ST;
    }
  }
}
#if (BTM_SCO_HCI_INCLUDED == TRUE)
/*******************************************************************************
 *
 * Function         bta_ag_sco_read_cback
 *
 * Description      Callback function is the callback function for incoming
 *                  SCO data over HCI.
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_ag_sco_read_cback(uint16_t sco_inx, BT_HDR* p_data,
                                  tBTM_SCO_DATA_FLAG status) {
  if (status != BTM_SCO_DATA_CORRECT) {
    APPL_TRACE_DEBUG("bta_ag_sco_read_cback: status(%d)", status);
  }

  /* Callout function must free the data. */
  bta_dm_sco_co_in_data(p_data, status);
}
#endif
/*******************************************************************************
 *
 * Function         bta_ag_remove_sco
 *
 * Description      Removes the specified SCO from the system.
 *                  If only_active is true, then SCO is only removed if
 *                  connected
 *
 * Returns          bool   - true if Sco removal was started
 *
 ******************************************************************************/
static bool bta_ag_remove_sco(tBTA_AG_SCB* p_scb, bool only_active) {
  bool removed_started = false;
  tBTM_STATUS status;

  if (p_scb->sco_idx != BTM_INVALID_SCO_INDEX) {
    if (!only_active || p_scb->sco_idx == bta_ag_cb.sco.cur_idx) {
      status = BTM_RemoveSco(p_scb->sco_idx);

      APPL_TRACE_DEBUG("%s: inx 0x%04x, status:0x%x", __func__, p_scb->sco_idx,
                       status);

      if (status == BTM_CMD_STARTED) {
        /* Sco is connected; set current control block */
        bta_ag_cb.sco.p_curr_scb = p_scb;

        removed_started = true;
      }
      /* If no connection reset the sco handle */
      else if ((status == BTM_SUCCESS) || (status == BTM_UNKNOWN_ADDR)) {
        p_scb->sco_idx = BTM_INVALID_SCO_INDEX;
      }
    }
  }
  return removed_started;
}

/*******************************************************************************
 *
 * Function         bta_ag_esco_connreq_cback
 *
 * Description      BTM eSCO connection requests and eSCO change requests
 *                  Only the connection requests are processed by BTA.
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_ag_esco_connreq_cback(tBTM_ESCO_EVT event,
                                      tBTM_ESCO_EVT_DATA* p_data) {
  tBTA_AG_SCB* p_scb;
  uint16_t handle;
  uint16_t sco_inx = p_data->conn_evt.sco_inx;

  /* Only process connection requests */
  if (event == BTM_ESCO_CONN_REQ_EVT) {
    if ((handle = bta_ag_idx_by_bdaddr(BTM_ReadScoBdAddr(sco_inx))) != 0 &&
        ((p_scb = bta_ag_scb_by_idx(handle)) != NULL) && p_scb->svc_conn) {
      p_scb->sco_idx = sco_inx;

      /* If no other SCO active, allow this one */
      if (!bta_ag_cb.sco.p_curr_scb) {
        APPL_TRACE_EVENT("%s: Accept Conn Request (sco_inx 0x%04x)", __func__,
                         sco_inx);
        bta_ag_sco_conn_rsp(p_scb, &p_data->conn_evt);

        bta_ag_cb.sco.state = BTA_AG_SCO_OPENING_ST;
        bta_ag_cb.sco.p_curr_scb = p_scb;
        bta_ag_cb.sco.cur_idx = p_scb->sco_idx;
      } else /* Begin a transfer: Close current SCO before responding */
      {
        APPL_TRACE_DEBUG("bta_ag_esco_connreq_cback: Begin XFER");
        bta_ag_cb.sco.p_xfer_scb = p_scb;
        bta_ag_cb.sco.conn_data = p_data->conn_evt;
        bta_ag_cb.sco.state = BTA_AG_SCO_OPEN_XFER_ST;

        if (!bta_ag_remove_sco(bta_ag_cb.sco.p_curr_scb, true)) {
          APPL_TRACE_ERROR(
              "%s: Nothing to remove,so accept Conn Request(sco_inx 0x%04x)",
              __func__, sco_inx);
          bta_ag_cb.sco.p_xfer_scb = NULL;
          bta_ag_cb.sco.state = BTA_AG_SCO_LISTEN_ST;

          bta_ag_sco_conn_rsp(p_scb, &p_data->conn_evt);
        }
      }
    }
    /* If error occurred send reject response immediately */
    else {
      APPL_TRACE_WARNING(
          "no scb for bta_ag_esco_connreq_cback or no resources");
      BTM_EScoConnRsp(p_data->conn_evt.sco_inx, HCI_ERR_HOST_REJECT_RESOURCES,
                      (enh_esco_params_t*)NULL);
    }
  }
  /* Received a change in the esco link */
  else if (event == BTM_ESCO_CHG_EVT) {
    APPL_TRACE_EVENT(
        "%s: eSCO change event (inx %d): rtrans %d, "
        "rxlen %d, txlen %d, txint %d",
        __func__, p_data->chg_evt.sco_inx, p_data->chg_evt.retrans_window,
        p_data->chg_evt.rx_pkt_len, p_data->chg_evt.tx_pkt_len,
        p_data->chg_evt.tx_interval);
  }
}

/*******************************************************************************
 *
 * Function         bta_ag_cback_sco
 *
 * Description      Call application callback function with SCO event.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_ag_cback_sco(tBTA_AG_SCB* p_scb, uint8_t event) {
  tBTA_AG_HDR sco;

  sco.handle = bta_ag_scb_to_idx(p_scb);
  sco.app_id = p_scb->app_id;

  /* call close cback */
  (*bta_ag_cb.p_cback)(event, (tBTA_AG*)&sco);
}

/*******************************************************************************
 *
 * Function         bta_ag_create_sco
 *
 * Description
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_ag_create_sco(tBTA_AG_SCB* p_scb, bool is_orig) {
  uint8_t* p_bd_addr = NULL;
  enh_esco_params_t params;
  tBTA_AG_PEER_CODEC esco_codec = BTA_AG_CODEC_CVSD;

  /* Make sure this sco handle is not already in use */
  if (p_scb->sco_idx != BTM_INVALID_SCO_INDEX) {
    APPL_TRACE_WARNING("bta_ag_create_sco: Index 0x%04x Already In Use!",
                       p_scb->sco_idx);
    return;
  }

  APPL_TRACE_DEBUG("%s: Using enhanced sco %d)", __func__,
                   controller_get_interface()
                       ->supports_enhanced_setup_synchronous_connection());

#if (BTM_WBS_INCLUDED == TRUE)
  if ((p_scb->sco_codec == BTA_AG_CODEC_MSBC) && !p_scb->codec_fallback &&
      !p_scb->retry_with_sco_only)
    esco_codec = BTA_AG_CODEC_MSBC;

  if (p_scb->codec_fallback) {
    p_scb->codec_fallback = false;

    /* Force AG to send +BCS for the next audio connection. */
    p_scb->codec_updated = true;
  }

  esco_codec_t codec_index = ESCO_CODEC_CVSD;
  /* If WBS included, use CVSD by default, index is 0 for CVSD by initialization
   */
  /* If eSCO codec is mSBC, index is T2 or T1 */
  if (esco_codec == BTA_AG_CODEC_MSBC) {
    if (p_scb->codec_msbc_settings == BTA_AG_SCO_MSBC_SETTINGS_T2) {
      codec_index = ESCO_CODEC_MSBC_T2;
    } else {
      codec_index = ESCO_CODEC_MSBC_T1;
    }
  }

  params = esco_parameters_for_codec(codec_index);
  if (esco_codec == BTM_SCO_CODEC_CVSD) /* For CVSD */
  {
    /* Use the applicable packet types (3-EV3 is not allowed according to
     * errata 2363) */
    params.packet_types =
        p_bta_ag_cfg->sco_pkt_types | ESCO_PKT_TYPES_MASK_NO_3_EV3;

    if ((!(p_scb->features & BTA_AG_FEAT_ESCO)) ||
        (!(p_scb->peer_features & BTA_AG_PEER_FEAT_ESCO))) {
      params.max_latency_ms = 10;
      params.retransmission_effort = ESCO_RETRANSMISSION_POWER;
    }
  }
#else
  /* When WBS is not included, use CVSD by default */
  params = esco_parameters_for_codec(ESCO_CODEC_CVSD);

  /* Use the applicable packet types (3-EV3 is not allowed according to errata
   * 2363) */
  params.packet_types =
      p_bta_ag_cfg->sco_pkt_types | ESCO_PKT_TYPES_MASK_NO_3_EV3;
  if ((!(p_scb->features & BTA_AG_FEAT_ESCO)) ||
      (!(p_scb->peer_features & BTA_AG_PEER_FEAT_ESCO))) {
    params.max_latency_ms = 10;
    params.retransmission_effort = ESCO_RETRANSMISSION_POWER;
  }

#endif

  /* if initiating set current scb and peer bd addr */
  if (is_orig) {
    bta_ag_cb.sco.is_local = true;
    /* Attempt to use eSCO if remote host supports HFP >= 1.5 */
    /* Need to find out from SIG if HSP can use eSCO; for now use SCO */
    if (p_scb->conn_service == BTA_AG_HFP &&
        p_scb->peer_version >= HFP_VERSION_1_5 && !p_scb->retry_with_sco_only) {
      BTM_SetEScoMode(&params);
      /* If ESCO or EDR ESCO, retry with SCO only in case of failure */
      if ((params.packet_types & BTM_ESCO_LINK_ONLY_MASK) ||
          !((params.packet_types &
             ~(BTM_ESCO_LINK_ONLY_MASK | BTM_SCO_LINK_ONLY_MASK)) ^
            BTA_AG_NO_EDR_ESCO)) {
#if (BTM_WBS_INCLUDED == TRUE)
        if (esco_codec != BTA_AG_CODEC_MSBC) {
          p_scb->retry_with_sco_only = true;
          APPL_TRACE_API("Setting retry_with_sco_only to TRUE");
        } else /* Do not use SCO when using mSBC */
        {
          p_scb->retry_with_sco_only = false;
          APPL_TRACE_API("Setting retry_with_sco_only to FALSE");
        }
#else
        p_scb->retry_with_sco_only = true;
        APPL_TRACE_API("Setting retry_with_sco_only to TRUE");
#endif
      }
    } else {
      if (p_scb->retry_with_sco_only) APPL_TRACE_API("retrying with SCO only");
      p_scb->retry_with_sco_only = false;

      BTM_SetEScoMode(&params);
    }

    bta_ag_cb.sco.p_curr_scb = p_scb;

    /* save the current codec as sco_codec can be updated while SCO is open. */
    p_scb->inuse_codec = esco_codec;

    /* tell sys to stop av if any */
    bta_sys_sco_use(BTA_ID_AG, p_scb->app_id, p_scb->peer_addr);

#if (BTM_WBS_INCLUDED == TRUE)
    /* Allow any platform specific pre-SCO set up to take place After the
       pre-SCO Vendor Specific commands are sent,bta_ag_ci_audio_open_continue
       call-in needs to be called by the app to continue with SCO connection
       creation */
    bta_ag_co_audio_state(bta_ag_scb_to_idx(p_scb), p_scb->app_id,
                          SCO_STATE_SETUP, esco_codec);
#else
    bta_ag_co_audio_state(bta_ag_scb_to_idx(p_scb), p_scb->app_id,
                          SCO_STATE_SETUP);
#endif
  } else {
    p_scb->retry_with_sco_only = false;
    p_bd_addr = p_scb->peer_addr;

    tBTM_STATUS status =
        BTM_CreateSco(p_bd_addr, is_orig, params.packet_types, &p_scb->sco_idx,
                      bta_ag_sco_conn_cback, bta_ag_sco_disc_cback);
    if (status == BTM_CMD_STARTED)
      BTM_RegForEScoEvts(p_scb->sco_idx, bta_ag_esco_connreq_cback);

    APPL_TRACE_API(
        "ag create sco: orig %d, inx 0x%04x, status 0x%x, pkt types 0x%04x",
        is_orig, p_scb->sco_idx, status, params.packet_types);
  }
}

/*******************************************************************************
 *
 * Function         bta_ag_create_pending_sco
 *
 * Description      This Function is called after the pre-SCO vendor setup is
 *                  done for the BTA to continue and send the HCI Commands for
 *                  creating/accepting SCO connection with peer based on the
 *                  is_local parameter.
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_ag_create_pending_sco(tBTA_AG_SCB* p_scb, bool is_local) {
  tBTA_AG_PEER_CODEC esco_codec = p_scb->inuse_codec;
  enh_esco_params_t params;
  bta_ag_cb.sco.p_curr_scb = p_scb;
  bta_ag_cb.sco.cur_idx = p_scb->sco_idx;

  /* Local device requested SCO connection to peer */
  if (is_local) {
    if (bta_ag_cb.sco.set_audio_status == BTA_AG_SUCCESS) {
      if (esco_codec == BTA_AG_CODEC_MSBC) {
#if (BTM_WBS_INCLUDED == TRUE)
        if (p_scb->codec_msbc_settings == BTA_AG_SCO_MSBC_SETTINGS_T2) {
          params = esco_parameters_for_codec(ESCO_CODEC_MSBC_T2);
        } else
#endif
          params = esco_parameters_for_codec(ESCO_CODEC_MSBC_T1);
      } else {
        params = esco_parameters_for_codec(ESCO_CODEC_CVSD);
        if ((!(p_scb->features & BTA_AG_FEAT_ESCO)) ||
            (!(p_scb->peer_features & BTA_AG_PEER_FEAT_ESCO))) {
          params.max_latency_ms = 10;
          params.retransmission_effort = ESCO_RETRANSMISSION_POWER;
        }
      }

#if (BTM_WBS_INCLUDED == TRUE)
      /* Bypass vendor specific and voice settings if enhanced eSCO supported */
      if (!(controller_get_interface()
                ->supports_enhanced_setup_synchronous_connection())) {
        if (esco_codec == BTA_AG_CODEC_MSBC)
          BTM_WriteVoiceSettings(BTM_VOICE_SETTING_TRANS);
        else
          BTM_WriteVoiceSettings(BTM_VOICE_SETTING_CVSD);
      }
#endif

#if (BTM_SCO_HCI_INCLUDED == TRUE)
      /* initialize SCO setup, no voice setting for AG, data rate <==> sample
       * rate */
      BTM_ConfigScoPath(params.input_data_path, bta_ag_sco_read_cback, NULL,
                        TRUE);
#endif

      tBTM_STATUS status = BTM_CreateSco(
          p_scb->peer_addr, TRUE, params.packet_types, &p_scb->sco_idx,
          bta_ag_sco_conn_cback, bta_ag_sco_disc_cback);
      if (status == BTM_CMD_STARTED) {
        /* Initiating the connection, set the current sco handle */
        bta_ag_cb.sco.cur_idx = p_scb->sco_idx;
      }
    } else {
      /* Pre-SCO Vendor setup failed Go back to Listening state */
      bta_ag_cb.sco.state = BTA_AG_SCO_LISTEN_ST;
      bta_ag_create_sco(p_scb, false);
    }
  } else {
    params = esco_parameters_for_codec(ESCO_CODEC_CVSD);
    if ((!(p_scb->features & BTA_AG_FEAT_ESCO)) ||
        (!(p_scb->peer_features & BTA_AG_PEER_FEAT_ESCO)))

    {
      params.max_latency_ms = 10;
      params.retransmission_effort = ESCO_RETRANSMISSION_POWER;
    }

    if (bta_ag_cb.sco.set_audio_status == BTA_AG_SUCCESS)
      BTM_EScoConnRsp(p_scb->sco_idx, HCI_SUCCESS, &params);
    else
      BTM_EScoConnRsp(p_scb->sco_idx, HCI_ERR_HOST_REJECT_RESOURCES, &params);
  }
}

#if (BTM_WBS_INCLUDED == TRUE)
/*******************************************************************************
 *
 * Function         bta_ag_attempt_msbc_safe_settings
 *
 * Description    Checks if ESCO connection needs to be attempted using mSBC
 *                T1(safe) settings
 *
 *
 * Returns          true if T1 settings has to be used, false otherwise
 *
 ******************************************************************************/
bool bta_ag_attempt_msbc_safe_settings(tBTA_AG_SCB* p_scb) {
  if (p_scb->svc_conn && p_scb->sco_codec == BTM_SCO_CODEC_MSBC &&
      p_scb->codec_msbc_settings == BTA_AG_SCO_MSBC_SETTINGS_T1)
    return true;
  else
    return false;
}

/*******************************************************************************
 *
 * Function         bta_ag_codec_negotiation_timer_cback
 *
 * Description
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_ag_codec_negotiation_timer_cback(void* data) {
  tBTA_AG_SCB* p_scb = (tBTA_AG_SCB*)data;

  /* Announce that codec negotiation failed. */
  bta_ag_sco_codec_nego(p_scb, false);

  /* call app callback */
  bta_ag_cback_sco(p_scb, BTA_AG_AUDIO_CLOSE_EVT);
}

/*******************************************************************************
 *
 * Function         bta_ag_codec_negotiate
 *
 * Description      Initiate codec negotiation by sending AT command.
 *                  If not necessary, skip negotiation.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_codec_negotiate(tBTA_AG_SCB* p_scb) {
  bta_ag_cb.sco.p_curr_scb = p_scb;

  if ((p_scb->codec_updated || p_scb->codec_fallback) &&
      (p_scb->peer_features & BTA_AG_PEER_FEAT_CODEC)) {
    /* Change the power mode to Active until sco open is completed. */
    bta_sys_busy(BTA_ID_AG, p_scb->app_id, p_scb->peer_addr);

    /* Send +BCS to the peer */
    bta_ag_send_bcs(p_scb, NULL);

    /* Start timer to handle timeout */
    alarm_set_on_queue(
        p_scb->codec_negotiation_timer, BTA_AG_CODEC_NEGOTIATION_TIMEOUT_MS,
        bta_ag_codec_negotiation_timer_cback, p_scb, btu_bta_alarm_queue);
  } else {
    /* use same codec type as previous SCO connection, skip codec negotiation */
    APPL_TRACE_DEBUG(
        "use same codec type as previous SCO connection,skip codec "
        "negotiation");
    bta_ag_sco_codec_nego(p_scb, true);
  }
}
#endif /* (BTM_WBS_INCLUDED == TRUE) */

/*******************************************************************************
 *
 * Function         bta_ag_sco_event
 *
 * Description
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_ag_sco_event(tBTA_AG_SCB* p_scb, uint8_t event) {
  tBTA_AG_SCO_CB* p_sco = &bta_ag_cb.sco;
#if (BTM_WBS_INCLUDED == TRUE)
  tBTA_AG_SCB* p_cn_scb = NULL; /* For codec negotiation */
#endif
#if (BTM_SCO_HCI_INCLUDED == TRUE)
  BT_HDR* p_buf;
#endif

#if BTA_AG_SCO_DEBUG == TRUE
  uint8_t in_state = p_sco->state;

  if (event != BTA_AG_SCO_CI_DATA_E) {
    APPL_TRACE_EVENT(
        "%s: BTA ag sco evt (hdl 0x%04x): State %d (%s), Event %d (%s)",
        __func__, p_scb->sco_idx, p_sco->state,
        bta_ag_sco_state_str(p_sco->state), event, bta_ag_sco_evt_str(event));
  }
#else

  if (event != BTA_AG_SCO_CI_DATA_E) {
    APPL_TRACE_EVENT("%s: BTA ag sco evt (hdl 0x%04x): State %d, Event %d",
                     __func__, p_scb->sco_idx, p_sco->state, event);
  }
#endif

#if (BTM_SCO_HCI_INCLUDED == TRUE)
  if (event == BTA_AG_SCO_CI_DATA_E) {
    while (true) {
      bta_dm_sco_co_out_data(&p_buf);
      if (p_buf) {
        if (p_sco->state == BTA_AG_SCO_OPEN_ST)
          BTM_WriteScoData(p_sco->p_curr_scb->sco_idx, p_buf);
        else
          osi_free(p_buf);
      } else
        break;
    }

    return;
  }
#endif

  switch (p_sco->state) {
    case BTA_AG_SCO_SHUTDOWN_ST:
      switch (event) {
        case BTA_AG_SCO_LISTEN_E:
          /* create sco listen connection */
          bta_ag_create_sco(p_scb, false);
          p_sco->state = BTA_AG_SCO_LISTEN_ST;
          break;

        default:
          APPL_TRACE_WARNING("%s: BTA_AG_SCO_SHUTDOWN_ST: Ignoring event %d",
                             __func__, event);
          break;
      }
      break;

    case BTA_AG_SCO_LISTEN_ST:
      switch (event) {
        case BTA_AG_SCO_LISTEN_E:
          /* create sco listen connection (Additional channel) */
          bta_ag_create_sco(p_scb, false);
          break;

        case BTA_AG_SCO_OPEN_E:
          /* remove listening connection */
          bta_ag_remove_sco(p_scb, false);

#if (BTM_WBS_INCLUDED == TRUE)
          /* start codec negotiation */
          p_sco->state = BTA_AG_SCO_CODEC_ST;
          p_cn_scb = p_scb;
#else
          /* create sco connection to peer */
          bta_ag_create_sco(p_scb, true);
          p_sco->state = BTA_AG_SCO_OPENING_ST;
#endif
          break;

        case BTA_AG_SCO_SHUTDOWN_E:
          /* remove listening connection */
          bta_ag_remove_sco(p_scb, false);

          if (p_scb == p_sco->p_curr_scb) p_sco->p_curr_scb = NULL;

          /* If last SCO instance then finish shutting down */
          if (!bta_ag_other_scb_open(p_scb)) {
            p_sco->state = BTA_AG_SCO_SHUTDOWN_ST;
          }
          break;

        case BTA_AG_SCO_CLOSE_E:
          /* remove listening connection */
          /* Ignore the event. Keep listening SCO for the active SLC
           */
          APPL_TRACE_WARNING("%s: BTA_AG_SCO_LISTEN_ST: Ignoring event %d",
                             __func__, event);
          break;

        case BTA_AG_SCO_CONN_CLOSE_E:
          /* sco failed; create sco listen connection */
          bta_ag_create_sco(p_scb, false);
          p_sco->state = BTA_AG_SCO_LISTEN_ST;
          break;

        default:
          APPL_TRACE_WARNING("%s: BTA_AG_SCO_LISTEN_ST: Ignoring event %d",
                             __func__, event);
          break;
      }
      break;

#if (BTM_WBS_INCLUDED == TRUE)
    case BTA_AG_SCO_CODEC_ST:
      switch (event) {
        case BTA_AG_SCO_LISTEN_E:
          /* create sco listen connection (Additional channel) */
          bta_ag_create_sco(p_scb, false);
          break;

        case BTA_AG_SCO_CN_DONE_E:
          /* create sco connection to peer */
          bta_ag_create_sco(p_scb, true);
          p_sco->state = BTA_AG_SCO_OPENING_ST;
          break;

        case BTA_AG_SCO_XFER_E:
          /* save xfer scb */
          p_sco->p_xfer_scb = p_scb;
          p_sco->state = BTA_AG_SCO_CLOSE_XFER_ST;
          break;

        case BTA_AG_SCO_SHUTDOWN_E:
          /* remove listening connection */
          bta_ag_remove_sco(p_scb, false);

          if (p_scb == p_sco->p_curr_scb) p_sco->p_curr_scb = NULL;

          /* If last SCO instance then finish shutting down */
          if (!bta_ag_other_scb_open(p_scb)) {
            p_sco->state = BTA_AG_SCO_SHUTDOWN_ST;
          }
          break;

        case BTA_AG_SCO_CLOSE_E:
          /* sco open is not started yet. just go back to listening */
          p_sco->state = BTA_AG_SCO_LISTEN_ST;
          break;

        case BTA_AG_SCO_CONN_CLOSE_E:
          /* sco failed; create sco listen connection */
          bta_ag_create_sco(p_scb, false);
          p_sco->state = BTA_AG_SCO_LISTEN_ST;
          break;

        default:
          APPL_TRACE_WARNING("%s: BTA_AG_SCO_CODEC_ST: Ignoring event %d",
                             __func__, event);
          break;
      }
      break;
#endif

    case BTA_AG_SCO_OPENING_ST:
      switch (event) {
        case BTA_AG_SCO_LISTEN_E:
          /* second headset has now joined */
          /* create sco listen connection (Additional channel) */
          if (p_scb != p_sco->p_curr_scb) {
            bta_ag_create_sco(p_scb, false);
          }
          break;

#if (BTM_WBS_INCLUDED == TRUE)
        case BTA_AG_SCO_REOPEN_E:
          /* start codec negotiation */
          p_sco->state = BTA_AG_SCO_CODEC_ST;
          p_cn_scb = p_scb;
          break;
#endif

        case BTA_AG_SCO_OPEN_PENDING_E:
          /* Send pending commands to create SCO connection to peer */
          bta_ag_create_pending_sco(p_scb, p_sco->is_local);
          break;

        case BTA_AG_SCO_XFER_E:
          /* save xfer scb */
          p_sco->p_xfer_scb = p_scb;
          p_sco->state = BTA_AG_SCO_CLOSE_XFER_ST;
          break;

        case BTA_AG_SCO_CLOSE_E:
          p_sco->state = BTA_AG_SCO_OPEN_CL_ST;
          break;

        case BTA_AG_SCO_SHUTDOWN_E:
          /* If not opening scb, just close it */
          if (p_scb != p_sco->p_curr_scb) {
            /* remove listening connection */
            bta_ag_remove_sco(p_scb, false);
          } else
            p_sco->state = BTA_AG_SCO_SHUTTING_ST;

          break;

        case BTA_AG_SCO_CONN_OPEN_E:
          p_sco->state = BTA_AG_SCO_OPEN_ST;
          break;

        case BTA_AG_SCO_CONN_CLOSE_E:
          /* sco failed; create sco listen connection */
          bta_ag_create_sco(p_scb, false);
          p_sco->state = BTA_AG_SCO_LISTEN_ST;
          break;

        default:
          APPL_TRACE_WARNING("%s: BTA_AG_SCO_OPENING_ST: Ignoring event %d",
                             __func__, event);
          break;
      }
      break;

    case BTA_AG_SCO_OPEN_CL_ST:
      switch (event) {
        case BTA_AG_SCO_XFER_E:
          /* save xfer scb */
          p_sco->p_xfer_scb = p_scb;

          p_sco->state = BTA_AG_SCO_CLOSE_XFER_ST;
          break;

        case BTA_AG_SCO_OPEN_E:
          p_sco->state = BTA_AG_SCO_OPENING_ST;
          break;

        case BTA_AG_SCO_SHUTDOWN_E:
          /* If not opening scb, just close it */
          if (p_scb != p_sco->p_curr_scb) {
            /* remove listening connection */
            bta_ag_remove_sco(p_scb, false);
          } else
            p_sco->state = BTA_AG_SCO_SHUTTING_ST;

          break;

        case BTA_AG_SCO_CONN_OPEN_E:
          /* close sco connection */
          bta_ag_remove_sco(p_scb, true);

          p_sco->state = BTA_AG_SCO_CLOSING_ST;
          break;

        case BTA_AG_SCO_CONN_CLOSE_E:
          /* sco failed; create sco listen connection */

          p_sco->state = BTA_AG_SCO_LISTEN_ST;
          break;

        default:
          APPL_TRACE_WARNING("%s: BTA_AG_SCO_OPEN_CL_ST: Ignoring event %d",
                             __func__, event);
          break;
      }
      break;

    case BTA_AG_SCO_OPEN_XFER_ST:
      switch (event) {
        case BTA_AG_SCO_CLOSE_E:
          /* close sco connection */
          bta_ag_remove_sco(p_scb, true);

          p_sco->state = BTA_AG_SCO_CLOSING_ST;
          break;

        case BTA_AG_SCO_SHUTDOWN_E:
          /* remove all connection */
          bta_ag_remove_sco(p_scb, false);
          p_sco->state = BTA_AG_SCO_SHUTTING_ST;

          break;

        case BTA_AG_SCO_CONN_CLOSE_E:
          /* closed sco; place in listen mode and
             accept the transferred connection */
          bta_ag_create_sco(p_scb, false); /* Back into listen mode */

          /* Accept sco connection with xfer scb */
          bta_ag_sco_conn_rsp(p_sco->p_xfer_scb, &p_sco->conn_data);
          p_sco->state = BTA_AG_SCO_OPENING_ST;
          p_sco->p_curr_scb = p_sco->p_xfer_scb;
          p_sco->cur_idx = p_sco->p_xfer_scb->sco_idx;
          p_sco->p_xfer_scb = NULL;
          break;

        default:
          APPL_TRACE_WARNING("%s: BTA_AG_SCO_OPEN_XFER_ST: Ignoring event %d",
                             __func__, event);
          break;
      }
      break;

    case BTA_AG_SCO_OPEN_ST:
      switch (event) {
        case BTA_AG_SCO_LISTEN_E:
          /* second headset has now joined */
          /* create sco listen connection (Additional channel) */
          if (p_scb != p_sco->p_curr_scb) {
            bta_ag_create_sco(p_scb, false);
          }
          break;

        case BTA_AG_SCO_XFER_E:
          /* close current sco connection */
          bta_ag_remove_sco(p_sco->p_curr_scb, true);

          /* save xfer scb */
          p_sco->p_xfer_scb = p_scb;

          p_sco->state = BTA_AG_SCO_CLOSE_XFER_ST;
          break;

        case BTA_AG_SCO_CLOSE_E:
          /* close sco connection if active */
          if (bta_ag_remove_sco(p_scb, true)) {
            p_sco->state = BTA_AG_SCO_CLOSING_ST;
          }
          break;

        case BTA_AG_SCO_SHUTDOWN_E:
          /* remove all listening connections */
          bta_ag_remove_sco(p_scb, false);

          /* If SCO was active on this scb, close it */
          if (p_scb == p_sco->p_curr_scb) {
            p_sco->state = BTA_AG_SCO_SHUTTING_ST;
          }
          break;

        case BTA_AG_SCO_CONN_CLOSE_E:
          /* peer closed sco; create sco listen connection */
          bta_ag_create_sco(p_scb, false);
          p_sco->state = BTA_AG_SCO_LISTEN_ST;
          break;

        default:
          APPL_TRACE_WARNING("%s: BTA_AG_SCO_OPEN_ST: Ignoring event %d",
                             __func__, event);
          break;
      }
      break;

    case BTA_AG_SCO_CLOSING_ST:
      switch (event) {
        case BTA_AG_SCO_LISTEN_E:
          /* create sco listen connection (Additional channel) */
          if (p_scb != p_sco->p_curr_scb) {
            bta_ag_create_sco(p_scb, false);
          }
          break;

        case BTA_AG_SCO_OPEN_E:
          p_sco->state = BTA_AG_SCO_CLOSE_OP_ST;
          break;

        case BTA_AG_SCO_XFER_E:
          /* save xfer scb */
          p_sco->p_xfer_scb = p_scb;

          p_sco->state = BTA_AG_SCO_CLOSE_XFER_ST;
          break;

        case BTA_AG_SCO_SHUTDOWN_E:
          /* If not closing scb, just close it */
          if (p_scb != p_sco->p_curr_scb) {
            /* remove listening connection */
            bta_ag_remove_sco(p_scb, false);
          } else
            p_sco->state = BTA_AG_SCO_SHUTTING_ST;

          break;

        case BTA_AG_SCO_CONN_CLOSE_E:
          /* peer closed sco; create sco listen connection */
          bta_ag_create_sco(p_scb, false);

          p_sco->state = BTA_AG_SCO_LISTEN_ST;
          break;

        default:
          APPL_TRACE_WARNING("%s: BTA_AG_SCO_CLOSING_ST: Ignoring event %d",
                             __func__, event);
          break;
      }
      break;

    case BTA_AG_SCO_CLOSE_OP_ST:
      switch (event) {
        case BTA_AG_SCO_CLOSE_E:
          p_sco->state = BTA_AG_SCO_CLOSING_ST;
          break;

        case BTA_AG_SCO_SHUTDOWN_E:
          p_sco->state = BTA_AG_SCO_SHUTTING_ST;
          break;

        case BTA_AG_SCO_CONN_CLOSE_E:
#if (BTM_WBS_INCLUDED == TRUE)
          /* start codec negotiation */
          p_sco->state = BTA_AG_SCO_CODEC_ST;
          p_cn_scb = p_scb;
#else
          /* open sco connection */
          bta_ag_create_sco(p_scb, true);
          p_sco->state = BTA_AG_SCO_OPENING_ST;
#endif
          break;

        case BTA_AG_SCO_LISTEN_E:
          /* create sco listen connection (Additional channel) */
          if (p_scb != p_sco->p_curr_scb) {
            bta_ag_create_sco(p_scb, false);
          }
          break;

        default:
          APPL_TRACE_WARNING("%s: BTA_AG_SCO_CLOSE_OP_ST: Ignoring event %d",
                             __func__, event);
          break;
      }
      break;

    case BTA_AG_SCO_CLOSE_XFER_ST:
      switch (event) {
        case BTA_AG_SCO_CONN_OPEN_E:
          /* close sco connection so headset can be transferred
             Probably entered this state from "opening state" */
          bta_ag_remove_sco(p_scb, true);
          break;

        case BTA_AG_SCO_CLOSE_E:
          /* clear xfer scb */
          p_sco->p_xfer_scb = NULL;

          p_sco->state = BTA_AG_SCO_CLOSING_ST;
          break;

        case BTA_AG_SCO_SHUTDOWN_E:
          /* clear xfer scb */
          p_sco->p_xfer_scb = NULL;

          p_sco->state = BTA_AG_SCO_SHUTTING_ST;
          break;

        case BTA_AG_SCO_CONN_CLOSE_E:
          /* closed sco; place old sco in listen mode,
             take current sco out of listen, and
             create originating sco for current */
          bta_ag_create_sco(p_scb, false);
          bta_ag_remove_sco(p_sco->p_xfer_scb, false);

#if (BTM_WBS_INCLUDED == TRUE)
          /* start codec negotiation */
          p_sco->state = BTA_AG_SCO_CODEC_ST;
          p_cn_scb = p_sco->p_xfer_scb;
          p_sco->p_xfer_scb = NULL;
#else
          /* create sco connection to peer */
          bta_ag_create_sco(p_sco->p_xfer_scb, true);
          p_sco->p_xfer_scb = NULL;
          p_sco->state = BTA_AG_SCO_OPENING_ST;
#endif
          break;

        default:
          APPL_TRACE_WARNING("%s: BTA_AG_SCO_CLOSE_XFER_ST: Ignoring event %d",
                             __func__, event);
          break;
      }
      break;

    case BTA_AG_SCO_SHUTTING_ST:
      switch (event) {
        case BTA_AG_SCO_CONN_OPEN_E:
          /* close sco connection; wait for conn close event */
          bta_ag_remove_sco(p_scb, true);
          break;

        case BTA_AG_SCO_CONN_CLOSE_E:
          /* If last SCO instance then finish shutting down */
          if (!bta_ag_other_scb_open(p_scb)) {
            p_sco->state = BTA_AG_SCO_SHUTDOWN_ST;
          } else /* Other instance is still listening */
          {
            p_sco->state = BTA_AG_SCO_LISTEN_ST;
          }

          /* If SCO closed for other HS which is not being disconnected,
             then create listen sco connection for it as scb still open */
          if (bta_ag_scb_open(p_scb)) {
            bta_ag_create_sco(p_scb, false);
            p_sco->state = BTA_AG_SCO_LISTEN_ST;
          }

          if (p_scb == p_sco->p_curr_scb) {
            p_sco->p_curr_scb->sco_idx = BTM_INVALID_SCO_INDEX;
            p_sco->p_curr_scb = NULL;
          }
          break;

        case BTA_AG_SCO_LISTEN_E:
          /* create sco listen connection (Additional channel) */
          if (p_scb != p_sco->p_curr_scb) {
            bta_ag_create_sco(p_scb, false);
          }
          break;

        case BTA_AG_SCO_SHUTDOWN_E:
          if (!bta_ag_other_scb_open(p_scb)) {
            p_sco->state = BTA_AG_SCO_SHUTDOWN_ST;
          } else /* Other instance is still listening */
          {
            p_sco->state = BTA_AG_SCO_LISTEN_ST;
          }

          if (p_scb == p_sco->p_curr_scb) {
            p_sco->p_curr_scb->sco_idx = BTM_INVALID_SCO_INDEX;
            p_sco->p_curr_scb = NULL;
          }
          break;

        default:
          APPL_TRACE_WARNING("%s: BTA_AG_SCO_SHUTTING_ST: Ignoring event %d",
                             __func__, event);
          break;
      }
      break;

    default:
      break;
  }
#if (BTA_AG_SCO_DEBUG == TRUE)
  if (p_sco->state != in_state) {
    APPL_TRACE_EVENT("BTA AG SCO State Change: [%s] -> [%s] after Event [%s]",
                     bta_ag_sco_state_str(in_state),
                     bta_ag_sco_state_str(p_sco->state),
                     bta_ag_sco_evt_str(event));
  }
#endif

#if (BTM_WBS_INCLUDED == TRUE)
  if (p_cn_scb) {
    bta_ag_codec_negotiate(p_cn_scb);
  }
#endif
}

/*******************************************************************************
 *
 * Function         bta_ag_sco_is_open
 *
 * Description      Check if sco is open for this scb.
 *
 *
 * Returns          true if sco open for this scb, false otherwise.
 *
 ******************************************************************************/
bool bta_ag_sco_is_open(tBTA_AG_SCB* p_scb) {
  return ((bta_ag_cb.sco.state == BTA_AG_SCO_OPEN_ST) &&
          (bta_ag_cb.sco.p_curr_scb == p_scb));
}

/*******************************************************************************
 *
 * Function         bta_ag_sco_is_opening
 *
 * Description      Check if sco is in Opening state.
 *
 *
 * Returns          true if sco is in Opening state for this scb, false
 *                  otherwise.
 *
 ******************************************************************************/
bool bta_ag_sco_is_opening(tBTA_AG_SCB* p_scb) {
  return ((bta_ag_cb.sco.state == BTA_AG_SCO_OPENING_ST) &&
          (bta_ag_cb.sco.p_curr_scb == p_scb));
}

/*******************************************************************************
 *
 * Function         bta_ag_sco_listen
 *
 * Description
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_sco_listen(tBTA_AG_SCB* p_scb, UNUSED_ATTR tBTA_AG_DATA* p_data) {
  bta_ag_sco_event(p_scb, BTA_AG_SCO_LISTEN_E);
}

/*******************************************************************************
 *
 * Function         bta_ag_sco_open
 *
 * Description
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_sco_open(tBTA_AG_SCB* p_scb, UNUSED_ATTR tBTA_AG_DATA* p_data) {
  uint8_t event;

  /* if another scb using sco, this is a transfer */
  if (bta_ag_cb.sco.p_curr_scb != NULL && bta_ag_cb.sco.p_curr_scb != p_scb) {
    event = BTA_AG_SCO_XFER_E;
  }
  /* else it is an open */
  else {
    event = BTA_AG_SCO_OPEN_E;
  }

  bta_ag_sco_event(p_scb, event);
}

/*******************************************************************************
 *
 * Function         bta_ag_ci_sco_open_continue
 *
 * Description      This is called by the API_AUDIO_OPEN_CONTINUE_EVT from the
 *                  BTA AG state machine and is used to send the pending HCI
 *                  commands for SCO Connection after the pre-SCO setup is done
 *
 *
 * Returns          void
 *
 ******************************************************************************/

void bta_ag_ci_sco_open_continue(tBTA_AG_SCB* p_scb, tBTA_AG_DATA* p_data) {
  uint8_t p_status = p_data->api_result.result;
  bta_ag_cb.sco.set_audio_status = p_status;

  APPL_TRACE_DEBUG("%s: Handle %d", __func__, p_status);
  bta_ag_sco_event(p_scb, BTA_AG_SCO_OPEN_PENDING_E);
}

/*******************************************************************************
 *
 * Function         bta_ag_sco_close
 *
 * Description
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_sco_close(tBTA_AG_SCB* p_scb, UNUSED_ATTR tBTA_AG_DATA* p_data) {
/* if scb is in use */
#if (BTM_WBS_INCLUDED == TRUE)
  /* sco_idx is not allocated in SCO_CODEC_ST, still need to move to listen
   * state. */
  if ((p_scb->sco_idx != BTM_INVALID_SCO_INDEX) ||
      (bta_ag_cb.sco.state == BTA_AG_SCO_CODEC_ST))
#else
  if (p_scb->sco_idx != BTM_INVALID_SCO_INDEX)
#endif
  {
    APPL_TRACE_DEBUG("bta_ag_sco_close: sco_inx = %d", p_scb->sco_idx);
    bta_ag_sco_event(p_scb, BTA_AG_SCO_CLOSE_E);
  }
}

#if (BTM_WBS_INCLUDED == TRUE)

/*******************************************************************************
 *
 * Function         bta_ag_sco_codec_nego
 *
 * Description
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_sco_codec_nego(tBTA_AG_SCB* p_scb, bool result) {
  if (result == true) {
    /* Subsequent sco connection will skip codec negotiation */
    p_scb->codec_updated = false;

    bta_ag_sco_event(p_scb, BTA_AG_SCO_CN_DONE_E);
  } else /* codec negotiation failed */
    bta_ag_sco_event(p_scb, BTA_AG_SCO_CLOSE_E);
}
#endif

/*******************************************************************************
 *
 * Function         bta_ag_sco_shutdown
 *
 * Description
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_sco_shutdown(tBTA_AG_SCB* p_scb, UNUSED_ATTR tBTA_AG_DATA* p_data) {
  bta_ag_sco_event(p_scb, BTA_AG_SCO_SHUTDOWN_E);
}

/*******************************************************************************
 *
 * Function         bta_ag_sco_conn_open
 *
 * Description
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_sco_conn_open(tBTA_AG_SCB* p_scb,
                          UNUSED_ATTR tBTA_AG_DATA* p_data) {
  bta_ag_sco_event(p_scb, BTA_AG_SCO_CONN_OPEN_E);

  bta_sys_sco_open(BTA_ID_AG, p_scb->app_id, p_scb->peer_addr);

#if (BTM_WBS_INCLUDED == TRUE)
  bta_ag_co_audio_state(bta_ag_scb_to_idx(p_scb), p_scb->app_id, SCO_STATE_ON,
                        p_scb->inuse_codec);
#else
  bta_ag_co_audio_state(bta_ag_scb_to_idx(p_scb), p_scb->app_id, SCO_STATE_ON);
#endif

#if (BTM_SCO_HCI_INCLUDED == TRUE)
  /* open SCO codec if SCO is routed through transport */
  bta_dm_sco_co_open(bta_ag_scb_to_idx(p_scb), BTA_SCO_OUT_PKT_SIZE,
                     BTA_AG_CI_SCO_DATA_EVT);
#endif

  /* call app callback */
  bta_ag_cback_sco(p_scb, BTA_AG_AUDIO_OPEN_EVT);

  p_scb->retry_with_sco_only = false;
#if (BTM_WBS_INCLUDED == TRUE)
  /* reset to mSBC T2 settings as the preferred */
  p_scb->codec_msbc_settings = BTA_AG_SCO_MSBC_SETTINGS_T2;
#endif
}

/*******************************************************************************
 *
 * Function         bta_ag_sco_conn_close
 *
 * Description
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_sco_conn_close(tBTA_AG_SCB* p_scb,
                           UNUSED_ATTR tBTA_AG_DATA* p_data) {
  uint16_t handle = bta_ag_scb_to_idx(p_scb);

  /* clear current scb */
  bta_ag_cb.sco.p_curr_scb = NULL;
  p_scb->sco_idx = BTM_INVALID_SCO_INDEX;

#if (BTM_WBS_INCLUDED == TRUE)
  /* codec_fallback is set when AG is initiator and connection failed for mSBC.
   */
  /* OR if codec is msbc and T2 settings failed, then retry Safe T1 settings */
  if ((p_scb->codec_fallback && p_scb->svc_conn) ||
      bta_ag_attempt_msbc_safe_settings(p_scb)) {
    bta_ag_sco_event(p_scb, BTA_AG_SCO_REOPEN_E);
  } else if (p_scb->retry_with_sco_only && p_scb->svc_conn) {
    /* retry_with_sco_only is set when AG is initiator and connection failed for
     * eSCO */
    bta_ag_create_sco(p_scb, true);
  }
#else
  /* retry_with_sco_only, will be set only when AG is initiator
  ** and AG is first trying to establish an eSCO connection */
  if (p_scb->retry_with_sco_only && p_scb->svc_conn) {
    bta_ag_create_sco(p_scb, true);
  }
#endif
  else {
    sco_state_t sco_state =
        bta_ag_cb.sco.p_xfer_scb ? SCO_STATE_OFF_TRANSFER : SCO_STATE_OFF;
#if (BTM_WBS_INCLUDED == TRUE)
    /* Indicate if the closing of audio is because of transfer */
    bta_ag_co_audio_state(handle, p_scb->app_id, sco_state, p_scb->inuse_codec);
#else
    /* Indicate if the closing of audio is because of transfer */
    bta_ag_co_audio_state(handle, p_scb->app_id, sco_state);
#endif
    bta_ag_sco_event(p_scb, BTA_AG_SCO_CONN_CLOSE_E);

    bta_sys_sco_close(BTA_ID_AG, p_scb->app_id, p_scb->peer_addr);

    /* if av got suspended by this call, let it resume. */
    /* In case call stays alive regardless of sco, av should not be affected. */
    if (((p_scb->call_ind == BTA_AG_CALL_INACTIVE) &&
         (p_scb->callsetup_ind == BTA_AG_CALLSETUP_NONE)) ||
        (p_scb->post_sco == BTA_AG_POST_SCO_CALL_END)) {
      bta_sys_sco_unuse(BTA_ID_AG, p_scb->app_id, p_scb->peer_addr);
    }

    /* call app callback */
    bta_ag_cback_sco(p_scb, BTA_AG_AUDIO_CLOSE_EVT);
#if (BTM_WBS_INCLUDED == TRUE)
    p_scb->codec_msbc_settings = BTA_AG_SCO_MSBC_SETTINGS_T2;
#endif
  }
  p_scb->retry_with_sco_only = false;
}

/*******************************************************************************
 *
 * Function         bta_ag_sco_conn_rsp
 *
 * Description      Process the SCO connection request
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_sco_conn_rsp(tBTA_AG_SCB* p_scb,
                         tBTM_ESCO_CONN_REQ_EVT_DATA* p_data) {
  bta_ag_cb.sco.is_local = false;

  APPL_TRACE_DEBUG("%s: using enhanced sco %d)", __func__,
                   controller_get_interface()
                       ->supports_enhanced_setup_synchronous_connection());

  if (bta_ag_cb.sco.state == BTA_AG_SCO_LISTEN_ST ||
      bta_ag_cb.sco.state == BTA_AG_SCO_CLOSE_XFER_ST ||
      bta_ag_cb.sco.state == BTA_AG_SCO_OPEN_XFER_ST) {
    /* tell sys to stop av if any */
    bta_sys_sco_use(BTA_ID_AG, p_scb->app_id, p_scb->peer_addr);
#if (BTM_WBS_INCLUDED == FALSE)
    /* Allow any platform specific pre-SCO set up to take place After the
       pre-SCO Vendor Specific commands are sent, bta_ag_ci_audio_open_continue
       call-in needs to be called by the app to continue with SCO accept/reject
       HCI commands*/
    bta_ag_co_audio_state(bta_ag_scb_to_idx(p_scb), p_scb->app_id,
                          SCO_STATE_SETUP);
#else
    /* When HS initiated SCO, it cannot be WBS. */
    /* Allow any platform specific pre-SCO set up to take place */
    bta_ag_co_audio_state(bta_ag_scb_to_idx(p_scb), p_scb->app_id,
                          SCO_STATE_SETUP, BTA_AG_CODEC_CVSD);
#endif

#if (BTM_SCO_HCI_INCLUDED == TRUE)
    /* Configure the transport being used */
    BTM_ConfigScoPath(resp.input_data_path, bta_ag_sco_read_cback, NULL, TRUE);
#endif
  }

#if (BTM_WBS_INCLUDED == TRUE)
  /* If SCO open was initiated from HS, it must be CVSD */
  p_scb->inuse_codec = BTA_AG_CODEC_NONE;
#endif
}

/*******************************************************************************
 *
 * Function         bta_ag_ci_sco_data
 *
 * Description      Process the SCO data ready callin event
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_ci_sco_data(UNUSED_ATTR tBTA_AG_SCB* p_scb,
                        UNUSED_ATTR tBTA_AG_DATA* p_data) {
#if (BTM_SCO_HCI_INCLUDED == TRUE)
  bta_ag_sco_event(p_scb, BTA_AG_SCO_CI_DATA_E);
#endif
}

/*******************************************************************************
 *  Debugging functions
 ******************************************************************************/

#if (BTA_AG_SCO_DEBUG == TRUE)
static char* bta_ag_sco_evt_str(uint8_t event) {
  switch (event) {
    case BTA_AG_SCO_LISTEN_E:
      return "Listen Request";
    case BTA_AG_SCO_OPEN_E:
      return "Open Request";
    case BTA_AG_SCO_OPEN_PENDING_E:
      return "Open pending request";
    case BTA_AG_SCO_XFER_E:
      return "Transfer Request";
#if (BTM_WBS_INCLUDED == TRUE)
    case BTA_AG_SCO_CN_DONE_E:
      return "Codec Negotiation Done";
    case BTA_AG_SCO_REOPEN_E:
      return "Reopen Request";
#endif
    case BTA_AG_SCO_CLOSE_E:
      return "Close Request";
    case BTA_AG_SCO_SHUTDOWN_E:
      return "Shutdown Request";
    case BTA_AG_SCO_CONN_OPEN_E:
      return "Opened";
    case BTA_AG_SCO_CONN_CLOSE_E:
      return "Closed";
    case BTA_AG_SCO_CI_DATA_E:
      return "Sco Data";
    default:
      return "Unknown SCO Event";
  }
}

static char* bta_ag_sco_state_str(uint8_t state) {
  switch (state) {
    case BTA_AG_SCO_SHUTDOWN_ST:
      return "Shutdown";
    case BTA_AG_SCO_LISTEN_ST:
      return "Listening";
#if (BTM_WBS_INCLUDED == TRUE)
    case BTA_AG_SCO_CODEC_ST:
      return "Codec Negotiation";
#endif
    case BTA_AG_SCO_OPENING_ST:
      return "Opening";
    case BTA_AG_SCO_OPEN_CL_ST:
      return "Open while closing";
    case BTA_AG_SCO_OPEN_XFER_ST:
      return "Opening while Transferring";
    case BTA_AG_SCO_OPEN_ST:
      return "Open";
    case BTA_AG_SCO_CLOSING_ST:
      return "Closing";
    case BTA_AG_SCO_CLOSE_OP_ST:
      return "Close while Opening";
    case BTA_AG_SCO_CLOSE_XFER_ST:
      return "Close while Transferring";
    case BTA_AG_SCO_SHUTTING_ST:
      return "Shutting Down";
    default:
      return "Unknown SCO State";
  }
}

#endif /* (BTA_AG_SCO_DEBUG) */
