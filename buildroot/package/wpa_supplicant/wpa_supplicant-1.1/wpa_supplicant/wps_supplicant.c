/*
 * wpa_supplicant / WPS integration
 * Copyright (c) 2008-2010, Jouni Malinen <j@w1.fi>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Alternatively, this software may be distributed under the terms of BSD
 * license.
 *
 * See README and COPYING for more details.
 */

#include "includes.h"

#include "common.h"
#include "eloop.h"
#include "uuid.h"
#include "crypto/dh_group5.h"
#include "common/ieee802_11_defs.h"
#include "common/ieee802_11_common.h"
#include "common/wpa_common.h"
#include "common/wpa_ctrl.h"
#include "eap_common/eap_wsc_common.h"
#include "eap_peer/eap.h"
#include "eapol_supp/eapol_supp_sm.h"
#include "rsn_supp/wpa.h"
#include "config.h"
#include "wpa_supplicant_i.h"
#include "driver_i.h"
#include "notify.h"
#include "blacklist.h"
#include "bss.h"
#include "scan.h"
#include "ap.h"
#include "p2p/p2p.h"
#include "p2p_supplicant.h"
#include "wps_supplicant.h"


#ifndef WPS_PIN_SCAN_IGNORE_SEL_REG
#define WPS_PIN_SCAN_IGNORE_SEL_REG 3
#endif /* WPS_PIN_SCAN_IGNORE_SEL_REG */

static void wpas_wps_timeout(void *eloop_ctx, void *timeout_ctx);
static void wpas_clear_wps(struct wpa_supplicant *wpa_s);


int wpas_wps_eapol_cb(struct wpa_supplicant *wpa_s)
{
	int reassoc_cur_value;

	if (!wpa_s->wps_success &&
	    wpa_s->current_ssid &&
	    eap_is_wps_pin_enrollee(&wpa_s->current_ssid->eap)) {
		const u8 *bssid = wpa_s->bssid;
		if (is_zero_ether_addr(bssid))
			bssid = wpa_s->pending_bssid;

		wpa_printf(MSG_DEBUG, "WPS: PIN registration with " MACSTR
			   " did not succeed - continue trying to find "
			   "suitable AP", MAC2STR(bssid));
		wpa_blacklist_add(wpa_s, bssid);

		wpa_supplicant_deauthenticate(wpa_s,
					      WLAN_REASON_DEAUTH_LEAVING);
		wpa_s->reassociate = 1;
		wpa_supplicant_req_scan(wpa_s,
					wpa_s->blacklist_cleared ? 5 : 0, 0);
		wpa_s->blacklist_cleared = 0;
		return 1;
	}

	eloop_cancel_timeout(wpas_wps_timeout, wpa_s, ELOOP_ALL_CTX);
	if (wpa_s->key_mgmt == WPA_KEY_MGMT_WPS && !wpa_s->wps_success)
		wpa_msg(wpa_s, MSG_INFO, WPS_EVENT_FAIL);

	if (wpa_s->key_mgmt == WPA_KEY_MGMT_WPS && wpa_s->current_ssid &&
	    !(wpa_s->current_ssid->key_mgmt & WPA_KEY_MGMT_WPS)) {
		int disabled = wpa_s->current_ssid->disabled;
		unsigned int freq = wpa_s->assoc_freq;
		wpa_printf(MSG_DEBUG, "WPS: Network configuration replaced - "
			   "try to associate with the received credential "
			   "(freq=%u)", freq);
		wpa_supplicant_deauthenticate(wpa_s,
					      WLAN_REASON_DEAUTH_LEAVING);
		if (disabled) {
			wpa_printf(MSG_DEBUG, "WPS: Current network is "
				   "disabled - wait for user to enable");
			return 1;
		}
		wpa_s->after_wps = 5;
		wpa_s->wps_freq = freq;
		wpa_s->reassociate = 1;

		wpa_drv_set_fast_reassoc(wpa_s, 1, &reassoc_cur_value);

		if (reassoc_cur_value == 0) {
			wpa_s->wps_fast_reassoc = WPS_REASSOC_WPS_ENABLED;
		} else {
			wpa_s->wps_fast_reassoc = WPS_REASSOC_DEFAULT;
		}

		/*
		 * Fast reassociaton will be reset into orginal value when
		 * associating with AP.In case any failure before association,
		 * it will be reset the fast reassocation values within 2 secs
		 * in wpa_supplicant_stop_fast_reassoc().
		 */

		eloop_cancel_timeout(wpa_supplicant_stop_fast_reassoc, wpa_s, NULL);
		eloop_register_timeout(2, 0, wpa_supplicant_stop_fast_reassoc, wpa_s, NULL);

		/*
		 * Add small delay (100ms) before scanning to avoid race condition
		 * with deauthentication above (ie let the deauthentication/disassociation
		 * finish properly before scanning again)
		 */
		wpa_supplicant_req_scan(wpa_s, 0, 100000);
		return 1;
	}

	if (wpa_s->key_mgmt == WPA_KEY_MGMT_WPS && wpa_s->current_ssid) {
		wpa_printf(MSG_DEBUG, "WPS: Registration completed - waiting "
			   "for external credential processing");
		wpas_clear_wps(wpa_s);
		wpa_supplicant_deauthenticate(wpa_s,
					      WLAN_REASON_DEAUTH_LEAVING);
		return 1;
	}

	return 0;
}


static void wpas_wps_security_workaround(struct wpa_supplicant *wpa_s,
					 struct wpa_ssid *ssid,
					 const struct wps_credential *cred)
{
	struct wpa_driver_capa capa;
	struct wpa_bss *bss;
	const u8 *ie;
	struct wpa_ie_data adv;
	int wpa2 = 0, ccmp = 0;

	/*
	 * Many existing WPS APs do not know how to negotiate WPA2 or CCMP in
	 * case they are configured for mixed mode operation (WPA+WPA2 and
	 * TKIP+CCMP). Try to use scan results to figure out whether the AP
	 * actually supports stronger security and select that if the client
	 * has support for it, too.
	 */

	if (wpa_drv_get_capa(wpa_s, &capa))
		return; /* Unknown what driver supports */

	if (ssid->ssid == NULL)
		return;
	bss = wpa_bss_get(wpa_s, cred->mac_addr, ssid->ssid, ssid->ssid_len);
	if (bss == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: The AP was not found from BSS "
			   "table - use credential as-is");
		return;
	}

	wpa_printf(MSG_DEBUG, "WPS: AP found from BSS table");

	ie = wpa_bss_get_ie(bss, WLAN_EID_RSN);
	if (ie && wpa_parse_wpa_ie(ie, 2 + ie[1], &adv) == 0) {
		wpa2 = 1;
		if (adv.pairwise_cipher & WPA_CIPHER_CCMP)
			ccmp = 1;
	} else {
		ie = wpa_bss_get_vendor_ie(bss, WPA_IE_VENDOR_TYPE);
		if (ie && wpa_parse_wpa_ie(ie, 2 + ie[1], &adv) == 0 &&
		    adv.pairwise_cipher & WPA_CIPHER_CCMP)
			ccmp = 1;
	}

	if (ie == NULL && (ssid->proto & WPA_PROTO_WPA) &&
	    (ssid->pairwise_cipher & WPA_CIPHER_TKIP)) {
		/*
		 * TODO: This could be the initial AP configuration and the
		 * Beacon contents could change shortly. Should request a new
		 * scan and delay addition of the network until the updated
		 * scan results are available.
		 */
		wpa_printf(MSG_DEBUG, "WPS: The AP did not yet advertise WPA "
			   "support - use credential as-is");
		return;
	}

	if (ccmp && !(ssid->pairwise_cipher & WPA_CIPHER_CCMP) &&
	    (ssid->pairwise_cipher & WPA_CIPHER_TKIP) &&
	    (capa.key_mgmt & WPA_DRIVER_CAPA_KEY_MGMT_WPA2_PSK)) {
		wpa_printf(MSG_DEBUG, "WPS: Add CCMP into the credential "
			   "based on scan results");
		if (wpa_s->conf->ap_scan == 1)
			ssid->pairwise_cipher |= WPA_CIPHER_CCMP;
		else
			ssid->pairwise_cipher = WPA_CIPHER_CCMP;
	}

	if (wpa2 && !(ssid->proto & WPA_PROTO_RSN) &&
	    (ssid->proto & WPA_PROTO_WPA) &&
	    (capa.enc & WPA_DRIVER_CAPA_ENC_CCMP)) {
		wpa_printf(MSG_DEBUG, "WPS: Add WPA2 into the credential "
			   "based on scan results");
		if (wpa_s->conf->ap_scan == 1)
			ssid->proto |= WPA_PROTO_RSN;
		else
			ssid->proto = WPA_PROTO_RSN;
	}
}

static int wpa_supplicant_record_wps_ssid(struct wpa_supplicant *wpa_s, char *ssid, int ssid_len)
{
	int ret = 0;
	char *pp_confname;
	int conf_len;
	FILE *stream;
	char *pos;
	int fnew = 0;
	char buf[64];
	char ssid_buf[33];

	os_memset(ssid_buf, 0, sizeof(ssid_buf));
	os_memcpy(ssid_buf, ssid, ssid_len);
	wpa_printf(MSG_DEBUG, "ssid_buf is %s and ssid is %s", ssid_buf, ssid);

	conf_len = strlen(wpa_s->confname);

	pp_confname = (char *)os_malloc(conf_len + 4);
	if (!pp_confname) {
		return -1;
	}

	memset(pp_confname, 0, (conf_len + 4));
	os_strncpy(pp_confname, wpa_s->confname, (conf_len + 1));
	strcat(pp_confname, ".pp");
	wpa_printf(MSG_DEBUG, "pp_confname is %s", pp_confname);

	stream = fopen(pp_confname, "r");
	if (!stream) {
		wpa_printf(MSG_DEBUG, "%s not exist, create new one", pp_confname);
		fnew = 1;
	}

	if (!fnew) {

		while(fgets(buf, sizeof(buf), stream)) {
			buf[sizeof(buf) - 1] = '\0';
			pos = buf;
			while (*pos != '\0') {
				if (*pos == '\n') {
					*pos = '\0';
					break;
				}
				pos++;
			}
			wpa_printf(MSG_DEBUG, "buf is %s, ssid is %s", buf, ssid_buf);
			if (!strncmp(buf, ssid_buf, 32)) {
				wpa_printf(MSG_DEBUG, "%s already exists in %s", ssid_buf, pp_confname);
				fclose(stream);
				os_free(pp_confname);
				return ret;
			}
		}
		fclose(stream);
	}

	wpa_printf(MSG_DEBUG, "try to add the %s to the %s \n", ssid_buf, pp_confname);
	stream = fopen(pp_confname, "a+");
	if (stream) {
		fprintf(stream, "%s\n", ssid_buf);
		ret = 0;
		fclose(stream);
	} else {
		ret = -1;
	}
	os_free(pp_confname);

	return ret;

}

static int wpa_supplicant_wps_cred(void *ctx,
				   const struct wps_credential *cred)
{
	struct wpa_supplicant *wpa_s = ctx;
	struct wpa_ssid *ssid = wpa_s->current_ssid;
	u8 key_idx = 0;
	u16 auth_type;
#ifdef CONFIG_WPS_REG_DISABLE_OPEN
	int registrar = 0;
#endif /* CONFIG_WPS_REG_DISABLE_OPEN */
	struct wpa_driver_capa capa;

	if ((wpa_s->conf->wps_cred_processing == 1 ||
	     wpa_s->conf->wps_cred_processing == 2) && cred->cred_attr) {
		size_t blen = cred->cred_attr_len * 2 + 1;
		char *buf = os_malloc(blen);
		if (buf) {
			wpa_snprintf_hex(buf, blen,
					 cred->cred_attr, cred->cred_attr_len);
			wpa_msg(wpa_s, MSG_INFO, "%s%s",
				WPS_EVENT_CRED_RECEIVED, buf);
			os_free(buf);
		}

		wpas_notify_wps_credential(wpa_s, cred);
	} else
		wpa_msg(wpa_s, MSG_INFO, WPS_EVENT_CRED_RECEIVED);

	wpa_hexdump_key(MSG_DEBUG, "WPS: Received Credential attribute",
			cred->cred_attr, cred->cred_attr_len);

	if (wpa_s->conf->wps_cred_processing == 1)
		return 0;

	wpa_hexdump_ascii(MSG_DEBUG, "WPS: SSID", cred->ssid, cred->ssid_len);
	wpa_printf(MSG_DEBUG, "WPS: Authentication Type 0x%x",
		   cred->auth_type);
	wpa_printf(MSG_DEBUG, "WPS: Encryption Type 0x%x", cred->encr_type);
	wpa_printf(MSG_DEBUG, "WPS: Network Key Index %d", cred->key_idx);
	wpa_hexdump_key(MSG_DEBUG, "WPS: Network Key",
			cred->key, cred->key_len);
	wpa_printf(MSG_DEBUG, "WPS: MAC Address " MACSTR,
		   MAC2STR(cred->mac_addr));

	auth_type = cred->auth_type;
	if (auth_type == (WPS_AUTH_WPAPSK | WPS_AUTH_WPA2PSK)) {
		wpa_printf(MSG_DEBUG, "WPS: Workaround - convert mixed-mode "
			   "auth_type into WPA2PSK");
		auth_type = WPS_AUTH_WPA2PSK;
	}

	if (auth_type != WPS_AUTH_OPEN &&
	    auth_type != WPS_AUTH_SHARED &&
	    auth_type != WPS_AUTH_WPAPSK &&
	    auth_type != WPS_AUTH_WPA2PSK) {
		wpa_printf(MSG_DEBUG, "WPS: Ignored credentials for "
			   "unsupported authentication type 0x%x",
			   auth_type);
		return 0;
	}

	if (ssid && (ssid->key_mgmt & WPA_KEY_MGMT_WPS)) {
		wpa_printf(MSG_DEBUG, "WPS: Replace WPS network block based "
			   "on the received credential");
#ifdef CONFIG_WPS_REG_DISABLE_OPEN
		if (ssid->eap.identity &&
		    ssid->eap.identity_len == WSC_ID_REGISTRAR_LEN &&
		    os_memcmp(ssid->eap.identity, WSC_ID_REGISTRAR,
			      WSC_ID_REGISTRAR_LEN) == 0)
			registrar = 1;
#endif /* CONFIG_WPS_REG_DISABLE_OPEN */
		os_free(ssid->eap.identity);
		ssid->eap.identity = NULL;
		ssid->eap.identity_len = 0;
		os_free(ssid->eap.phase1);
		ssid->eap.phase1 = NULL;
		os_free(ssid->eap.eap_methods);
		ssid->eap.eap_methods = NULL;
		if (!ssid->p2p_group)
			ssid->temporary = 0;
		ssid->bssid_set = 0;
	} else {
		wpa_printf(MSG_DEBUG, "WPS: Create a new network based on the "
			   "received credential");
		ssid = wpa_config_add_network(wpa_s->conf);
		if (ssid == NULL)
			return -1;
		wpas_notify_network_added(wpa_s, ssid);
	}

	wpa_config_set_network_defaults(ssid);

	os_free(ssid->ssid);
	ssid->ssid = os_malloc(cred->ssid_len);
	if (ssid->ssid) {
		os_memcpy(ssid->ssid, cred->ssid, cred->ssid_len);
		ssid->ssid_len = cred->ssid_len;
	}

	switch (cred->encr_type) {
	case WPS_ENCR_NONE:
		break;
	case WPS_ENCR_WEP:
		if (cred->key_len <= 0)
			break;
		if (cred->key_len != 5 && cred->key_len != 13 &&
		    cred->key_len != 10 && cred->key_len != 26) {
			wpa_printf(MSG_ERROR, "WPS: Invalid WEP Key length "
				   "%lu", (unsigned long) cred->key_len);
			return -1;
		}
		if (cred->key_idx > NUM_WEP_KEYS) {
			wpa_printf(MSG_ERROR, "WPS: Invalid WEP Key index %d",
				   cred->key_idx);
			return -1;
		}
		if (cred->key_idx)
			key_idx = cred->key_idx - 1;
		if (cred->key_len == 10 || cred->key_len == 26) {
			if (hexstr2bin((char *) cred->key,
				       ssid->wep_key[key_idx],
				       cred->key_len / 2) < 0) {
				wpa_printf(MSG_ERROR, "WPS: Invalid WEP Key "
					   "%d", key_idx);
				return -1;
			}
			ssid->wep_key_len[key_idx] = cred->key_len / 2;
		} else {
			os_memcpy(ssid->wep_key[key_idx], cred->key,
				  cred->key_len);
			ssid->wep_key_len[key_idx] = cred->key_len;
		}
		ssid->wep_tx_keyidx = key_idx;
		break;
	case WPS_ENCR_TKIP:
		ssid->pairwise_cipher = WPA_CIPHER_TKIP;
		break;
	case WPS_ENCR_AES:
		ssid->pairwise_cipher = WPA_CIPHER_CCMP;
		break;
	}

	switch (auth_type) {
	case WPS_AUTH_OPEN:
		ssid->auth_alg = WPA_AUTH_ALG_OPEN;
		ssid->key_mgmt = WPA_KEY_MGMT_NONE;
		ssid->proto = 0;
#ifdef CONFIG_WPS_REG_DISABLE_OPEN
		if (registrar) {
			wpa_msg(wpa_s, MSG_INFO, WPS_EVENT_OPEN_NETWORK
				"id=%d - Credentials for an open "
				"network disabled by default - use "
				"'select_network %d' to enable",
				ssid->id, ssid->id);
			ssid->disabled = 1;
		}
#endif /* CONFIG_WPS_REG_DISABLE_OPEN */
		break;
	case WPS_AUTH_SHARED:
		ssid->auth_alg = WPA_AUTH_ALG_SHARED;
		ssid->key_mgmt = WPA_KEY_MGMT_NONE;
		ssid->proto = 0;
		break;
	case WPS_AUTH_WPAPSK:
		ssid->auth_alg = WPA_AUTH_ALG_OPEN;
		ssid->key_mgmt = WPA_KEY_MGMT_PSK;
		ssid->proto = WPA_PROTO_WPA;
		break;
	case WPS_AUTH_WPA:
		ssid->auth_alg = WPA_AUTH_ALG_OPEN;
		ssid->key_mgmt = WPA_KEY_MGMT_IEEE8021X;
		ssid->proto = WPA_PROTO_WPA;
		break;
	case WPS_AUTH_WPA2:
		ssid->auth_alg = WPA_AUTH_ALG_OPEN;
		ssid->key_mgmt = WPA_KEY_MGMT_IEEE8021X;
		ssid->proto = WPA_PROTO_RSN;
		break;
	case WPS_AUTH_WPA2PSK:
		ssid->auth_alg = WPA_AUTH_ALG_OPEN;
		ssid->key_mgmt = WPA_KEY_MGMT_PSK;
		ssid->proto = WPA_PROTO_RSN;
		break;
	}

	if (ssid->key_mgmt == WPA_KEY_MGMT_PSK) {
		if (cred->key_len == 2 * PMK_LEN) {
			if (hexstr2bin((const char *) cred->key, ssid->psk,
				       PMK_LEN)) {
				wpa_printf(MSG_ERROR, "WPS: Invalid Network "
					   "Key");
				return -1;
			}
			ssid->psk_set = 1;
			ssid->export_keys = 1;
		} else if (cred->key_len >= 8 && cred->key_len < 2 * PMK_LEN) {
			os_free(ssid->passphrase);
			ssid->passphrase = os_malloc(cred->key_len + 1);
			if (ssid->passphrase == NULL)
				return -1;
			os_memcpy(ssid->passphrase, cred->key, cred->key_len);
			ssid->passphrase[cred->key_len] = '\0';
			ssid->from_wps = 1;
			wpa_config_update_psk(ssid);
			ssid->export_keys = 1;
		} else {
			wpa_printf(MSG_ERROR, "WPS: Invalid Network Key "
				   "length %lu",
				   (unsigned long) cred->key_len);
			return -1;
		}
	}

	wpas_wps_security_workaround(wpa_s, ssid, cred);

	ssid->flags |= NETWORK_ACCESS_THRU_WPS;

	if (wpa_drv_get_capa(wpa_s, &capa))
	      capa.mfp_options = NO_MGMT_FRAME_PROTECTION;
	ssid->ieee80211w = capa.mfp_options;
#ifndef CONFIG_NO_CONFIG_WRITE
	if (wpa_s->conf->update_config &&
	    wpa_config_write(wpa_s->confname, wpa_s->conf)) {
		wpa_printf(MSG_DEBUG, "WPS: Failed to update configuration");
		return -1;
	}

	if (wpa_s->non_wps_pp_enable && wpa_s->conf->update_config &&
		(wpa_supplicant_record_wps_ssid(wpa_s, ssid->ssid, ssid->ssid_len) < 0)) {
		wpa_printf(MSG_DEBUG, "WPS: Failed to update the pp configuration");
		return -1;
	}
#endif /* CONFIG_NO_CONFIG_WRITE */

	return 0;
}

static const char *wps_state_to_str(int message)
{
	const char *msg_to_str[] = {
		"Unknown",
		"WPS_Beacon",
		"WPS_ProbeRequest",
		"WPS_ProbeResponse",
		"WPS_M1",
		"WPS_M2",
		"WPS_M2D",
		"WPS_M3",
		"WPS_M4",
		"WPS_M5",
		"WPS_M6",
		"WPS_M7",
		"WPS_M8",
		"WPS_WSC_ACK",
		"WPS_WSC_NACK",
		"WPS_WSC_DONE"
	};
	if ((message < 1) || (message > 15)) {
		return msg_to_str[0];
	}
	return msg_to_str[message];
}

static const char *wps_m2d_to_str(int message)
{
	static const char *msg_to_str[] = {
		"WPS_CFG_NO_ERROR",
		"WPS_CFG_OOB_IFACE_READ_ERROR",
		"WPS_CFG_DECRYPTION_CRC_FAILURE",
		"WPS_CFG_24_CHAN_NOT_SUPPORTED",
		"WPS_CFG_50_CHAN_NOT_SUPPORTED",
		"WPS_CFG_SIGNAL_TOO_WEAK",
		"WPS_CFG_NETWORK_AUTH_FAILURE",
		"WPS_CFG_NETWORK_ASSOC_FAILURE",
		"WPS_CFG_NO_DHCP_RESPONSE",
		"WPS_CFG_FAILED_DHCP_CONFIG",
		"WPS_CFG_IP_ADDR_CONFLICT",
		"WPS_CFG_NO_CONN_TO_REGISTRAR",
		"WPS_CFG_MULTIPLE_PBC_DETECTED",
		"WPS_CFG_ROGUE_SUSPECTED",
		"WPS_CFG_DEVICE_BUSY",
		"WPS_CFG_SETUP_LOCKED",
		"WPS_CFG_MSG_TIMEOUT",
		"WPS_CFG_REG_SESS_TIMEOUT",
		"WPS_CFG_DEV_PASSWORD_AUTH_FAILURE"
	};
	if ((message < 0) || (message > 18))
	{
		return "Unknown";
	}
	return msg_to_str[message];
}

static const char *wps_ext_state_to_str( enum wps_external_state external_state )
{
	static const struct {
		enum wps_external_state wps_external_state;
		char *state_as_str;
	} state_to_str[] = {
		{ WPS_EXT_STATE_INI,		"WPS_INITIAL" },
		{ WPS_EXT_STATE_PROCESS_START,	"WPS_START" },
		{ WPS_EXT_STATE_PROCESS_SUCCESS, "WPS_SUCCESS" },
		{ WPS_EXT_STATE_MSG_EX_ERROR,	"WPS_ERROR" },
		{ WPS_EXT_STATE_MSG_EX_OVERLAP,	"WPS_OVERLAP" },
		{ WPS_EXT_STATE_TIMEOUT_ERROR,	"WPS_TIMEOUT" },
		{ WPS_EXT_STATE_UNKNOWN,	 NULL }
	};

	int		 iter;
	const char	*retaddr = NULL;

	for (iter = 0; state_to_str[ iter ].wps_external_state != WPS_EXT_STATE_UNKNOWN && retaddr == NULL; iter++) {
		if (external_state == state_to_str[ iter ].wps_external_state) {
			retaddr = state_to_str[ iter ].state_as_str;
		}
	}

	return retaddr;
}

static void wpa_supplicant_send_wlan_msg(struct wpa_supplicant *wpa_s, const char *msg)
{
	/* FIXME: borrowed function for passing message to the driver. */
	wpa_drv_mlme_setprotection(wpa_s, wpa_s->own_addr, MLME_SETPROTECTION_PROTECT_TYPE_NONE,
			MLME_SETPROTECTION_KEY_TYPE_PAIRWISE, msg);
}

#ifdef CONFIG_P2P
static void wpas_wps_pbc_overlap_cb(void *eloop_ctx, void *timeout_ctx)
{
	struct wpa_supplicant *wpa_s = eloop_ctx;
	wpas_p2p_notif_pbc_overlap(wpa_s);
}
#endif /* CONFIG_P2P */

static void wpa_supplicant_wps_event_m2d(struct wpa_supplicant *wpa_s,
					 struct wps_event_m2d *m2d)
{
	char msg[MAX_WLAN_MSG_LEN];
	const char *msg_format = "WPS m2d [%s] %d";

	wpa_msg(wpa_s, MSG_INFO, WPS_EVENT_M2D
		"dev_password_id=%d config_error=%d",
		m2d->dev_password_id, m2d->config_error);
	os_memset(msg, 0, sizeof(msg));
	os_snprintf(msg, sizeof(msg)-1, msg_format,
		wps_m2d_to_str(m2d->config_error), m2d->dev_password_id);
	wpa_supplicant_send_wlan_msg(wpa_s, msg);

	wpas_notify_wps_event_m2d(wpa_s, m2d);

	if (wpa_s->wps != NULL) {
		wpa_s->wps->wps_external_state = WPS_EXT_STATE_MSG_EX_ERROR;
	}

#ifdef CONFIG_P2P
	if (wpa_s->parent && wpa_s->parent != wpa_s) {
		wpa_msg(wpa_s->parent, MSG_INFO, WPS_EVENT_M2D
			"dev_password_id=%d config_error=%d",
			m2d->dev_password_id, m2d->config_error);
	}
	if (m2d->config_error == WPS_CFG_MULTIPLE_PBC_DETECTED) {
		/*
		 * Notify P2P from eloop timeout to avoid issues with the
		 * interface getting removed while processing a message.
		 */
		eloop_register_timeout(0, 0, wpas_wps_pbc_overlap_cb, wpa_s,
				       NULL);
	}
#endif /* CONFIG_P2P */
}


static const char * wps_event_fail_reason[NUM_WPS_EI_VALUES] = {
	"No Error", /* WPS_EI_NO_ERROR */
	"TKIP Only Prohibited", /* WPS_EI_SECURITY_TKIP_ONLY_PROHIBITED */
	"WEP Prohibited" /* WPS_EI_SECURITY_WEP_PROHIBITED */
};

static void wpa_supplicant_wps_event_fail(struct wpa_supplicant *wpa_s,
					  struct wps_event_fail *fail)
{
	char msg[MAX_WLAN_MSG_LEN];
	const char *msg_format = "WPS failed [%s - %d - %s]";
	struct wpa_ssid *ssid = NULL;

	if (fail->error_indication > 0 &&
	    fail->error_indication < NUM_WPS_EI_VALUES) {
		wpa_msg(wpa_s, MSG_INFO,
			WPS_EVENT_FAIL "msg=%d config_error=%d reason=%d (%s)",
			fail->msg, fail->config_error, fail->error_indication,
			wps_event_fail_reason[fail->error_indication]);
		if (wpa_s->parent && wpa_s->parent != wpa_s)
			wpa_msg(wpa_s->parent, MSG_INFO, WPS_EVENT_FAIL
				"msg=%d config_error=%d reason=%d (%s)",
				fail->msg, fail->config_error,
				fail->error_indication,
				wps_event_fail_reason[fail->error_indication]);
	} else {
		wpa_msg(wpa_s, MSG_INFO,
			WPS_EVENT_FAIL "msg=%d config_error=%d",
			fail->msg, fail->config_error);
		if (wpa_s->parent && wpa_s->parent != wpa_s)
			wpa_msg(wpa_s->parent, MSG_INFO, WPS_EVENT_FAIL
				"msg=%d config_error=%d",
				fail->msg, fail->config_error);
	}

	os_memset(msg, 0, sizeof(msg));
	os_snprintf(msg, sizeof(msg)-1, msg_format, wps_state_to_str(fail->msg), fail->config_error, wps_m2d_to_str(fail->config_error));
	wpa_supplicant_send_wlan_msg(wpa_s, msg);

	wpas_clear_wps(wpa_s);
	wpas_notify_wps_event_fail(wpa_s, fail);
#ifdef CONFIG_P2P
	wpas_p2p_wps_failed(wpa_s, fail);
#endif /* CONFIG_P2P */

	/* Only re-enable networks that were enabled before the WPS session */
	ssid = wpa_s->conf->ssid;
	while (ssid) {
		ssid->disabled &= ~WPS_SSID_TEMP_DISABLE;
		ssid = ssid->next;
	}
	wpa_s->disconnected = 0;
	wpa_s->reassociate = 1;

	if (wpa_s->wps != NULL) {
		wpa_s->wps->wps_external_state = WPS_EXT_STATE_MSG_EX_ERROR;
	}

	wpa_s->scan_req = 2;
	wpa_supplicant_req_scan(wpa_s, 0, 0);
}


static void wpa_supplicant_wps_event_success(struct wpa_supplicant *wpa_s)
{
	char msg[MAX_WLAN_MSG_LEN];
	const char *msg_format = "WPS success";

	wpa_msg(wpa_s, MSG_INFO, WPS_EVENT_SUCCESS);

	os_memset(msg, 0, sizeof(msg));
	os_snprintf(msg, sizeof(msg)-1, msg_format);
	wpa_supplicant_send_wlan_msg(wpa_s, msg);

	wpa_s->wps_success = 1;
	wpas_notify_wps_event_success(wpa_s);
#ifdef CONFIG_P2P
	wpas_p2p_wps_success(wpa_s, wpa_s->bssid, 0);
#endif /* CONFIG_P2P */

	if (wpa_s->wps != NULL) {
		wpa_s->wps->wps_external_state = WPS_EXT_STATE_PROCESS_SUCCESS;
	}
}

/*
 * Keep event handler for PBC start separate from event handler for PIN start,
 * even though currently both have the same effect.
 */

static void wpa_supplicant_wps_pbc_start(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->wps != NULL) {
		wpa_s->wps->wps_external_state = WPS_EXT_STATE_PROCESS_START;
	}
}

static void wpa_supplicant_wps_pin_start(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->wps != NULL) {
		wpa_s->wps->wps_external_state = WPS_EXT_STATE_PROCESS_START;
	}
}

static void wpa_supplicant_wps_pbc_overlap(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->wps != NULL) {
		wpa_s->wps->wps_external_state = WPS_EXT_STATE_MSG_EX_OVERLAP;
	}
}

static void wpa_supplicant_wps_timeout(struct wpa_supplicant *wpa_s)
{
	char msg[MAX_WLAN_MSG_LEN];
	const char *msg_format = "WPS timeout";
	wpa_msg(wpa_s, MSG_INFO, WPS_EVENT_FAIL "timeout");

	memset(msg, 0, sizeof(msg));
	os_snprintf(msg, sizeof(msg)-1, msg_format);
	wpa_supplicant_send_wlan_msg(wpa_s, msg);

	if (wpa_s->wps != NULL) {
		wpa_s->wps->wps_external_state = WPS_EXT_STATE_TIMEOUT_ERROR;
	}
}

static void wpa_supplicant_wps_cancel(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->wps != NULL) {
		wpa_s->wps->wps_external_state = WPS_EXT_STATE_INI;
	}
}

static void wpa_supplicant_wps_event_er_ap_add(struct wpa_supplicant *wpa_s,
					       struct wps_event_er_ap *ap)
{
	char uuid_str[100];
	char dev_type[WPS_DEV_TYPE_BUFSIZE];

	uuid_bin2str(ap->uuid, uuid_str, sizeof(uuid_str));
	if (ap->pri_dev_type)
		wps_dev_type_bin2str(ap->pri_dev_type, dev_type,
				     sizeof(dev_type));
	else
		dev_type[0] = '\0';

	wpa_msg(wpa_s, MSG_INFO, WPS_EVENT_ER_AP_ADD "%s " MACSTR
		" pri_dev_type=%s wps_state=%d |%s|%s|%s|%s|%s|%s|",
		uuid_str, MAC2STR(ap->mac_addr), dev_type, ap->wps_state,
		ap->friendly_name ? ap->friendly_name : "",
		ap->manufacturer ? ap->manufacturer : "",
		ap->model_description ? ap->model_description : "",
		ap->model_name ? ap->model_name : "",
		ap->manufacturer_url ? ap->manufacturer_url : "",
		ap->model_url ? ap->model_url : "");
}


static void wpa_supplicant_wps_event_er_ap_remove(struct wpa_supplicant *wpa_s,
						  struct wps_event_er_ap *ap)
{
	char uuid_str[100];
	uuid_bin2str(ap->uuid, uuid_str, sizeof(uuid_str));
	wpa_msg(wpa_s, MSG_INFO, WPS_EVENT_ER_AP_REMOVE "%s", uuid_str);
}


static void wpa_supplicant_wps_event_er_enrollee_add(
	struct wpa_supplicant *wpa_s, struct wps_event_er_enrollee *enrollee)
{
	char uuid_str[100];
	char dev_type[WPS_DEV_TYPE_BUFSIZE];

	uuid_bin2str(enrollee->uuid, uuid_str, sizeof(uuid_str));
	if (enrollee->pri_dev_type)
		wps_dev_type_bin2str(enrollee->pri_dev_type, dev_type,
				     sizeof(dev_type));
	else
		dev_type[0] = '\0';

	wpa_msg(wpa_s, MSG_INFO, WPS_EVENT_ER_ENROLLEE_ADD "%s " MACSTR
		" M1=%d config_methods=0x%x dev_passwd_id=%d pri_dev_type=%s "
		"|%s|%s|%s|%s|%s|",
		uuid_str, MAC2STR(enrollee->mac_addr), enrollee->m1_received,
		enrollee->config_methods, enrollee->dev_passwd_id, dev_type,
		enrollee->dev_name ? enrollee->dev_name : "",
		enrollee->manufacturer ? enrollee->manufacturer : "",
		enrollee->model_name ? enrollee->model_name : "",
		enrollee->model_number ? enrollee->model_number : "",
		enrollee->serial_number ? enrollee->serial_number : "");
}


static void wpa_supplicant_wps_event_er_enrollee_remove(
	struct wpa_supplicant *wpa_s, struct wps_event_er_enrollee *enrollee)
{
	char uuid_str[100];
	uuid_bin2str(enrollee->uuid, uuid_str, sizeof(uuid_str));
	wpa_msg(wpa_s, MSG_INFO, WPS_EVENT_ER_ENROLLEE_REMOVE "%s " MACSTR,
		uuid_str, MAC2STR(enrollee->mac_addr));
}


static void wpa_supplicant_wps_event_er_ap_settings(
	struct wpa_supplicant *wpa_s,
	struct wps_event_er_ap_settings *ap_settings)
{
	char uuid_str[100];
	char key_str[65];
	const struct wps_credential *cred = ap_settings->cred;

	key_str[0] = '\0';
	if (cred->auth_type & (WPS_AUTH_WPAPSK | WPS_AUTH_WPA2PSK)) {
		if (cred->key_len >= 8 && cred->key_len <= 64) {
			os_memcpy(key_str, cred->key, cred->key_len);
			key_str[cred->key_len] = '\0';
		}
	}

	uuid_bin2str(ap_settings->uuid, uuid_str, sizeof(uuid_str));
	/* Use wpa_msg_ctrl to avoid showing the key in debug log */
	wpa_msg_ctrl(wpa_s, MSG_INFO, WPS_EVENT_ER_AP_SETTINGS
		     "uuid=%s ssid=%s auth_type=0x%04x encr_type=0x%04x "
		     "key=%s",
		     uuid_str, wpa_ssid_txt(cred->ssid, cred->ssid_len),
		     cred->auth_type, cred->encr_type, key_str);
}


static void wpa_supplicant_wps_event_er_set_sel_reg(
	struct wpa_supplicant *wpa_s,
	struct wps_event_er_set_selected_registrar *ev)
{
	char uuid_str[100];

	uuid_bin2str(ev->uuid, uuid_str, sizeof(uuid_str));
	switch (ev->state) {
	case WPS_ER_SET_SEL_REG_START:
		wpa_msg(wpa_s, MSG_DEBUG, WPS_EVENT_ER_SET_SEL_REG
			"uuid=%s state=START sel_reg=%d dev_passwd_id=%u "
			"sel_reg_config_methods=0x%x",
			uuid_str, ev->sel_reg, ev->dev_passwd_id,
			ev->sel_reg_config_methods);
		break;
	case WPS_ER_SET_SEL_REG_DONE:
		wpa_msg(wpa_s, MSG_DEBUG, WPS_EVENT_ER_SET_SEL_REG
			"uuid=%s state=DONE", uuid_str);
		break;
	case WPS_ER_SET_SEL_REG_FAILED:
		wpa_msg(wpa_s, MSG_INFO, WPS_EVENT_ER_SET_SEL_REG
			"uuid=%s state=FAILED", uuid_str);
		break;
	}
}


static void wpa_supplicant_wps_event(void *ctx, enum wps_event event,
				     union wps_event_data *data)
{
	struct wpa_supplicant *wpa_s = ctx;
/*
 * Caution: wps_event_data * may be NULL for selected events:
 *     WPS_EV_PBC_START
 *     WPS_EV_PIN_START
 *     WPS_EV_TIMEOUT
 */
	switch (event) {
	case WPS_EV_M2D:
		wpa_supplicant_wps_event_m2d(wpa_s, &data->m2d);
		break;
	case WPS_EV_FAIL:
		wpa_supplicant_wps_event_fail(wpa_s, &data->fail);
		break;
	case WPS_EV_SUCCESS:
		wpa_supplicant_wps_event_success(wpa_s);
		break;
	case WPS_EV_PWD_AUTH_FAIL:
#ifdef CONFIG_AP
		if (wpa_s->ap_iface && data->pwd_auth_fail.enrollee)
			wpa_supplicant_ap_pwd_auth_fail(wpa_s);
#endif /* CONFIG_AP */
		break;
	case WPS_EV_PBC_OVERLAP:
		wpa_supplicant_wps_pbc_overlap(wpa_s);
		break;
	case WPS_EV_PBC_TIMEOUT:
		wpa_supplicant_wps_timeout(wpa_s);
		break;
	case WPS_EV_PBC_START:
		wpa_supplicant_wps_pbc_start(wpa_s);
		break;
	case WPS_EV_PIN_START:
		wpa_supplicant_wps_pin_start(wpa_s);
		break;
	case WPS_EV_TIMEOUT:
		wpa_supplicant_wps_timeout(wpa_s);
		break;
	case WPS_EV_ER_AP_ADD:
		wpa_supplicant_wps_event_er_ap_add(wpa_s, &data->ap);
		break;
	case WPS_EV_ER_AP_REMOVE:
		wpa_supplicant_wps_event_er_ap_remove(wpa_s, &data->ap);
		break;
	case WPS_EV_ER_ENROLLEE_ADD:
		wpa_supplicant_wps_event_er_enrollee_add(wpa_s,
							 &data->enrollee);
		break;
	case WPS_EV_ER_ENROLLEE_REMOVE:
		wpa_supplicant_wps_event_er_enrollee_remove(wpa_s,
							    &data->enrollee);
		break;
	case WPS_EV_ER_AP_SETTINGS:
		wpa_supplicant_wps_event_er_ap_settings(wpa_s,
							&data->ap_settings);
		break;
	case WPS_EV_ER_SET_SELECTED_REGISTRAR:
		wpa_supplicant_wps_event_er_set_sel_reg(wpa_s,
							&data->set_sel_reg);
		break;
	case WPS_EV_AP_PIN_SUCCESS:
		break;
	}
}


enum wps_request_type wpas_wps_get_req_type(struct wpa_ssid *ssid)
{
	if (eap_is_wps_pbc_enrollee(&ssid->eap) ||
	    eap_is_wps_pin_enrollee(&ssid->eap))
		return WPS_REQ_ENROLLEE;
	else
		return WPS_REQ_REGISTRAR;
}


static void wpas_clear_wps(struct wpa_supplicant *wpa_s)
{
	int id;
	struct wpa_ssid *ssid, *remove_ssid = NULL, *prev_current;

	prev_current = wpa_s->current_ssid;

	eloop_cancel_timeout(wpas_wps_timeout, wpa_s, ELOOP_ALL_CTX);

	/* Remove any existing WPS network from configuration */
	ssid = wpa_s->conf->ssid;
	while (ssid) {
		if (ssid->key_mgmt & WPA_KEY_MGMT_WPS) {
			if (ssid == wpa_s->current_ssid) {
				wpa_s->current_ssid = NULL;
				if (ssid != NULL)
					wpas_notify_network_changed(wpa_s);
			}
			id = ssid->id;
			remove_ssid = ssid;
		} else
			id = -1;
		ssid = ssid->next;
		if (id >= 0) {
			if (prev_current == remove_ssid) {
				wpa_sm_set_config(wpa_s->wpa, NULL);
				eapol_sm_notify_config(wpa_s->eapol, NULL,
						       NULL);
			}
			wpas_notify_network_removed(wpa_s, remove_ssid);
			wpa_config_remove_network(wpa_s->conf, id);
		}
	}
}


static void wpas_wps_timeout(void *eloop_ctx, void *timeout_ctx)
{
	int wps_operation = (int)timeout_ctx;
	char *msg = NULL;
	struct wpa_supplicant *wpa_s = eloop_ctx;
	struct wpa_ssid *ssid = NULL;

	wpa_msg(wpa_s, MSG_INFO, WPS_EVENT_TIMEOUT "Requested operation timed "
		"out");
	wpas_clear_wps(wpa_s);

	switch(wps_operation) {
	case 1:
		msg = "WPS PBC timeout";
		break;
	case 2:
		msg = "WPS PIN timeout";
		break;
	case 3:
	default:
		msg = "WPS timeout";
	}

	if (msg) {
		wpa_supplicant_send_wlan_msg(wpa_s, msg);
	}

	if (wpa_s != NULL && wpa_s->wps != NULL && wpa_s->wps->event_cb != NULL) {
		wpa_s->wps->event_cb(wpa_s->wps->cb_ctx, WPS_EV_TIMEOUT, NULL);
	}

	/* Only re-enable networks that were enabled before the WPS session */
	ssid = wpa_s->conf->ssid;
	while (ssid) {
		if (ssid->disabled) {
			ssid->disabled &= ~WPS_SSID_TEMP_DISABLE;
		}
		ssid = ssid->next;
	}
	wpa_s->disconnected = 0;
	wpa_s->reassociate = 1;
	wpa_s->scan_req = 2;
	wpa_supplicant_req_scan(wpa_s, 0, 0);
}


static struct wpa_ssid * wpas_wps_add_network(struct wpa_supplicant *wpa_s,
					      int registrar, const u8 *bssid)
{
	struct wpa_ssid *ssid;

	ssid = wpa_config_add_network(wpa_s->conf);
	if (ssid == NULL)
		return NULL;
	wpas_notify_network_added(wpa_s, ssid);
	wpa_config_set_network_defaults(ssid);
	ssid->temporary = 1;
	if (wpa_config_set(ssid, "key_mgmt", "WPS", 0) < 0 ||
	    wpa_config_set(ssid, "eap", "WSC", 0) < 0 ||
	    wpa_config_set(ssid, "identity", registrar ?
			   "\"" WSC_ID_REGISTRAR "\"" :
			   "\"" WSC_ID_ENROLLEE "\"", 0) < 0) {
		wpas_notify_network_removed(wpa_s, ssid);
		wpa_config_remove_network(wpa_s->conf, ssid->id);
		return NULL;
	}

	if (bssid) {
#ifndef CONFIG_P2P
		struct wpa_bss *bss;
		int count = 0;
#endif /* CONFIG_P2P */

		os_memcpy(ssid->bssid, bssid, ETH_ALEN);
		ssid->bssid_set = 1;

		/*
		 * Note: With P2P, the SSID may change at the time the WPS
		 * provisioning is started, so better not filter the AP based
		 * on the current SSID in the scan results.
		 */
#ifndef CONFIG_P2P
		dl_list_for_each(bss, &wpa_s->bss, struct wpa_bss, list) {
			if (os_memcmp(bssid, bss->bssid, ETH_ALEN) != 0)
				continue;

			os_free(ssid->ssid);
			ssid->ssid = os_malloc(bss->ssid_len);
			if (ssid->ssid == NULL)
				break;
			os_memcpy(ssid->ssid, bss->ssid, bss->ssid_len);
			ssid->ssid_len = bss->ssid_len;
			wpa_hexdump_ascii(MSG_DEBUG, "WPS: Picked SSID from "
					  "scan results",
					  ssid->ssid, ssid->ssid_len);
			count++;
		}

		if (count > 1) {
			wpa_printf(MSG_DEBUG, "WPS: More than one SSID found "
				   "for the AP; use wildcard");
			os_free(ssid->ssid);
			ssid->ssid = NULL;
			ssid->ssid_len = 0;
		}
#endif /* CONFIG_P2P */
	}

	return ssid;
}


static void wpas_wps_reassoc(struct wpa_supplicant *wpa_s,
			     struct wpa_ssid *selected)
{
	struct wpa_ssid *ssid;

	if (wpa_s->current_ssid)
		wpa_supplicant_deauthenticate(
			wpa_s, WLAN_REASON_DEAUTH_LEAVING);

	/* Mark all other networks disabled and trigger reassociation */
	ssid = wpa_s->conf->ssid;
	while (ssid) {
		int was_disabled = ssid->disabled;
		/*
		 * In case the network object corresponds to a persistent group
		 * then do not send out network disabled signal. In addition,
		 * do not change disabled status of persistent network objects
		 * from 2 to 1 should we connect to another network.
		 */
		if (was_disabled != 2) {
			if (ssid != selected) {
			/*
			 * So we can distinguish later on between networks
			 * that were enabled and those that were disabled.
			 */
			ssid->disabled |= WPS_SSID_TEMP_DISABLE;
			}

			if (was_disabled != ssid->disabled)
				wpas_notify_network_enabled_changed(wpa_s,
								    ssid);
		}
		ssid = ssid->next;
	}
	wpa_s->disconnected = 0;
	wpa_s->reassociate = 1;
	wpa_s->scan_runs = 0;
	wpa_s->wps_success = 0;
	wpa_s->blacklist_cleared = 0;

	wpa_supplicant_deauthenticate(wpa_s, WLAN_REASON_DEAUTH_LEAVING);

	wpa_supplicant_req_scan(wpa_s, 0, 0);
}


int wpas_wps_start_pbc(struct wpa_supplicant *wpa_s, const u8 *bssid,
		       int p2p_group)
{
	char msg[MAX_WLAN_MSG_LEN];
	const char *msg_format = "WPS PBC start";
	struct wpa_ssid *ssid;

	if (wpa_s->conf->wps_state == 0) {
		printf("WPS disabled in config\n");
		return -1;
	}

	wpas_clear_wps(wpa_s);
	wpa_blacklist_clear(wpa_s);
	ssid = wpas_wps_add_network(wpa_s, 0, bssid);
	if (ssid == NULL)
		return -1;
	ssid->temporary = 1;
	ssid->p2p_group = p2p_group;
#ifdef CONFIG_P2P
	if (p2p_group && wpa_s->go_params && wpa_s->go_params->ssid_len) {
		ssid->ssid = os_zalloc(wpa_s->go_params->ssid_len + 1);
		if (ssid->ssid) {
			ssid->ssid_len = wpa_s->go_params->ssid_len;
			os_memcpy(ssid->ssid, wpa_s->go_params->ssid,
				  ssid->ssid_len);
			wpa_hexdump_ascii(MSG_DEBUG, "WPS: Use specific AP "
					  "SSID", ssid->ssid, ssid->ssid_len);
		}
	}
#endif /* CONFIG_P2P */
	wpa_config_set(ssid, "phase1", "\"pbc=1\"", 0);
	if (wpa_s->wps_fragment_size)
		ssid->eap.fragment_size = wpa_s->wps_fragment_size;
	eloop_register_timeout(qtn_wps_pbc_timeout, 0, wpas_wps_timeout,
			       wpa_s, (void *)1);
	wpas_wps_reassoc(wpa_s, ssid);

	/* Message indicating start of WPS PBC */
	memset(msg, 0, sizeof(msg));
	os_snprintf(msg, sizeof(msg)-1, msg_format);
	wpa_supplicant_send_wlan_msg(wpa_s, msg);

	if (wpa_s != NULL && wpa_s->wps != NULL && wpa_s->wps->event_cb != NULL) {
		wpa_s->wps->event_cb(wpa_s->wps->cb_ctx, WPS_EV_PBC_START, NULL);
	}

	return 0;
}


int wpas_wps_start_pin(struct wpa_supplicant *wpa_s, const u8 *bssid,
		       const char *pin, int p2p_group, u16 dev_pw_id)
{
	char msg[MAX_WLAN_MSG_LEN];
	const char *msg_format = "WPS PIN start";
	struct wpa_ssid *ssid;
	char val[128];
	unsigned int rpin = 0;

	if (wpa_s->conf->wps_state == 0) {
		printf("WPS disabled in config\n");
		return -1;
	}

	wpas_clear_wps(wpa_s);
	wpa_blacklist_clear(wpa_s);
	ssid = wpas_wps_add_network(wpa_s, 0, bssid);
	if (ssid == NULL)
		return -1;
	ssid->temporary = 1;
	ssid->p2p_group = p2p_group;
#ifdef CONFIG_P2P
	if (p2p_group && wpa_s->go_params && wpa_s->go_params->ssid_len) {
		ssid->ssid = os_zalloc(wpa_s->go_params->ssid_len + 1);
		if (ssid->ssid) {
			ssid->ssid_len = wpa_s->go_params->ssid_len;
			os_memcpy(ssid->ssid, wpa_s->go_params->ssid,
				  ssid->ssid_len);
			wpa_hexdump_ascii(MSG_DEBUG, "WPS: Use specific AP "
					  "SSID", ssid->ssid, ssid->ssid_len);
		}
	}
#endif /* CONFIG_P2P */
	if (pin)
		os_snprintf(val, sizeof(val), "\"pin=%s dev_pw_id=%u\"",
			    pin, dev_pw_id);
	else {
		rpin = wps_generate_pin();
		os_snprintf(val, sizeof(val), "\"pin=%08d dev_pw_id=%u\"",
			    rpin, dev_pw_id);
	}
	wpa_config_set(ssid, "phase1", val, 0);
	if (wpa_s->wps_fragment_size)
		ssid->eap.fragment_size = wpa_s->wps_fragment_size;
	eloop_register_timeout(qtn_wps_pbc_timeout, 0, wpas_wps_timeout,
			       wpa_s, (void *)2);
	wpas_wps_reassoc(wpa_s, ssid);

	/* Message indicating start of WPS PIN */
	memset(msg, 0, sizeof(msg));
	os_snprintf(msg, sizeof(msg)-1, msg_format);
	wpa_supplicant_send_wlan_msg(wpa_s, msg);

	if (wpa_s != NULL && wpa_s->wps != NULL && wpa_s->wps->event_cb != NULL) {
		wpa_s->wps->event_cb(wpa_s->wps->cb_ctx, WPS_EV_PIN_START, NULL);
	}

	return rpin;
}


/* Cancel the wps pbc/pin requests */
int wpas_wps_cancel(struct wpa_supplicant *wpa_s)
{
	struct wpa_ssid *ssid = NULL;

	if (!wpas_wps_in_progress(wpa_s))
		return 0;

#ifdef CONFIG_AP
	if (wpa_s->ap_iface) {
		wpa_printf(MSG_DEBUG, "WPS: Cancelling in AP mode");
		return wpa_supplicant_ap_wps_cancel(wpa_s);
	}
#endif /* CONFIG_AP */

	if (wpa_s->wpa_state == WPA_SCANNING ||
	    wpa_s->wpa_state == WPA_DISCONNECTED) {
		wpa_printf(MSG_DEBUG, "WPS: Cancel operation - cancel scan");
		wpa_supplicant_cancel_scan(wpa_s);
		wpas_clear_wps(wpa_s);
	} else if (wpa_s->wpa_state >= WPA_ASSOCIATED) {
		wpa_printf(MSG_DEBUG, "WPS: Cancel operation - "
			   "deauthenticate");
		wpa_supplicant_deauthenticate(wpa_s,
					      WLAN_REASON_DEAUTH_LEAVING);
		wpas_clear_wps(wpa_s);
	}
	wpa_supplicant_wps_cancel(wpa_s);

	/* Only re-enable networks that were enabled before the WPS session */
	ssid = wpa_s->conf->ssid;
	while (ssid) {
		if (ssid->disabled) {
			ssid->disabled &= ~WPS_SSID_TEMP_DISABLE;
		}
		ssid = ssid->next;
	}
	wpa_s->disconnected = 0;
	wpa_s->reassociate = 1;
	wpa_s->scan_req = 2;
	wpa_supplicant_req_scan(wpa_s, 0, 0);

	return 0;
}


#ifdef CONFIG_WPS_OOB
int wpas_wps_start_oob(struct wpa_supplicant *wpa_s, char *device_type,
		       char *path, char *method, char *name)
{
	struct wps_context *wps = wpa_s->wps;
	struct oob_device_data *oob_dev;

	oob_dev = wps_get_oob_device(device_type);
	if (oob_dev == NULL)
		return -1;
	oob_dev->device_path = path;
	oob_dev->device_name = name;
	wps->oob_conf.oob_method = wps_get_oob_method(method);

	if (wps->oob_conf.oob_method == OOB_METHOD_DEV_PWD_E) {
		/*
		 * Use pre-configured DH keys in order to be able to write the
		 * key hash into the OOB file.
		 */
		wpabuf_free(wps->dh_pubkey);
		wpabuf_free(wps->dh_privkey);
		wps->dh_privkey = NULL;
		wps->dh_pubkey = NULL;
		dh5_free(wps->dh_ctx);
		wps->dh_ctx = dh5_init(&wps->dh_privkey, &wps->dh_pubkey);
		wps->dh_pubkey = wpabuf_zeropad(wps->dh_pubkey, 192);
		if (wps->dh_ctx == NULL || wps->dh_pubkey == NULL) {
			wpa_printf(MSG_ERROR, "WPS: Failed to initialize "
				   "Diffie-Hellman handshake");
			return -1;
		}
	}

	if (wps->oob_conf.oob_method == OOB_METHOD_CRED)
		wpas_clear_wps(wpa_s);

	if (wps_process_oob(wps, oob_dev, 0) < 0)
		return -1;

	if ((wps->oob_conf.oob_method == OOB_METHOD_DEV_PWD_E ||
	     wps->oob_conf.oob_method == OOB_METHOD_DEV_PWD_R) &&
	    wpas_wps_start_pin(wpa_s, NULL,
			       wpabuf_head(wps->oob_conf.dev_password), 0,
			       DEV_PW_DEFAULT) < 0)
			return -1;

	return 0;
}
#endif /* CONFIG_WPS_OOB */


int wpas_wps_start_reg(struct wpa_supplicant *wpa_s, const u8 *bssid,
		       const char *pin, struct wps_new_ap_settings *settings)
{
	struct wpa_ssid *ssid;
	char val[200];
	char *pos, *end;
	int res;

	if (!pin)
		return -1;

	if (wpa_s->conf->wps_state == 0) {
		printf("WPS disabled in config\n");
		return -1;
	}

	wpas_clear_wps(wpa_s);
	ssid = wpas_wps_add_network(wpa_s, 1, bssid);
	if (ssid == NULL)
		return -1;
	ssid->temporary = 1;
	pos = val;
	end = pos + sizeof(val);
	res = os_snprintf(pos, end - pos, "\"pin=%s", pin);
	if (res < 0 || res >= end - pos)
		return -1;
	pos += res;
	if (settings) {
		res = os_snprintf(pos, end - pos, " new_ssid=%s new_auth=%s "
				  "new_encr=%s new_key=%s",
				  settings->ssid_hex, settings->auth,
				  settings->encr, settings->key_hex);
		if (res < 0 || res >= end - pos)
			return -1;
		pos += res;
	}
	res = os_snprintf(pos, end - pos, "\"");
	if (res < 0 || res >= end - pos)
		return -1;
	wpa_config_set(ssid, "phase1", val, 0);
	if (wpa_s->wps_fragment_size)
		ssid->eap.fragment_size = wpa_s->wps_fragment_size;
	eloop_register_timeout(qtn_wps_pbc_timeout, 0, wpas_wps_timeout,
			       wpa_s, (void *)3);
	wpas_wps_reassoc(wpa_s, ssid);
	return 0;
}


static int wpas_wps_new_psk_cb(void *ctx, const u8 *mac_addr, const u8 *psk,
			       size_t psk_len)
{
	wpa_printf(MSG_DEBUG, "WPS: Received new WPA/WPA2-PSK from WPS for "
		   "STA " MACSTR, MAC2STR(mac_addr));
	wpa_hexdump_key(MSG_DEBUG, "Per-device PSK", psk, psk_len);

	/* TODO */

	return 0;
}


static void wpas_wps_pin_needed_cb(void *ctx, const u8 *uuid_e,
				   const struct wps_device_data *dev)
{
	char uuid[40], txt[400];
	int len;
	char devtype[WPS_DEV_TYPE_BUFSIZE];
	if (uuid_bin2str(uuid_e, uuid, sizeof(uuid)))
		return;
	wpa_printf(MSG_DEBUG, "WPS: PIN needed for UUID-E %s", uuid);
	len = os_snprintf(txt, sizeof(txt), "WPS-EVENT-PIN-NEEDED %s " MACSTR
			  " [%s|%s|%s|%s|%s|%s]",
			  uuid, MAC2STR(dev->mac_addr), dev->device_name,
			  dev->manufacturer, dev->model_name,
			  dev->model_number, dev->serial_number,
			  wps_dev_type_bin2str(dev->pri_dev_type, devtype,
					       sizeof(devtype)));
	if (len > 0 && len < (int) sizeof(txt))
		wpa_printf(MSG_INFO, "%s", txt);
}

int wpas_wps_get_ext_status(
	struct wpa_supplicant *wpa_s,
	char *status_str,
	const size_t status_len
)
{
	int		 retval = -1;
	const char	*equiv_status_str  = NULL;
	char		 numeric_value[12] = "";
	size_t		 equiv_status_len = 0;
	size_t		 numeric_value_len = 0;
	size_t		 total_status_len = 0;

	if (status_str == NULL || status_len < 1 || wpa_s == NULL || wpa_s->wps == NULL)
	  return -1;

	equiv_status_str = wps_ext_state_to_str( wpa_s->wps->wps_external_state );

	if (equiv_status_str == NULL) {
		equiv_status_str = "unknown";
	}

	os_snprintf( &numeric_value[0], sizeof( numeric_value ),
		  "%d", (int) (wpa_s->wps->wps_external_state) );
	numeric_value_len = os_strlen( &numeric_value[0] );

	equiv_status_len = os_strnlen( equiv_status_str, status_len );
	/*
	 * format is "NN (XXXXXXXX)\n", where NN is the numeric value and XXXXXXXX is
	 * the equivalent string (returned from wps_ext_state_to_str).  Total length is
	 * thus numeric_value_len + equiv_status_len + 4.
	 */
	total_status_len = equiv_status_len + numeric_value_len + 4;

	if (total_status_len < status_len) {
		os_snprintf(status_str, status_len, "%s (%s)\n",
			&numeric_value[0], equiv_status_str);

		retval = (int) total_status_len;
	}

	return retval;
}

static void wpas_wps_set_sel_reg_cb(void *ctx, int sel_reg, u16 dev_passwd_id,
				    u16 sel_reg_config_methods)
{
#ifdef CONFIG_WPS_ER
	struct wpa_supplicant *wpa_s = ctx;

	if (wpa_s->wps_er == NULL)
		return;
	wpa_printf(MSG_DEBUG, "WPS ER: SetSelectedRegistrar - sel_reg=%d "
		   "dev_password_id=%u sel_reg_config_methods=0x%x",
		   sel_reg, dev_passwd_id, sel_reg_config_methods);
	wps_er_set_sel_reg(wpa_s->wps_er, sel_reg, dev_passwd_id,
			   sel_reg_config_methods);
#endif /* CONFIG_WPS_ER */
}


static u16 wps_fix_config_methods(u16 config_methods)
{
#ifdef CONFIG_WPS2
	if ((config_methods &
	     (WPS_CONFIG_DISPLAY | WPS_CONFIG_VIRT_DISPLAY |
	      WPS_CONFIG_PHY_DISPLAY)) == WPS_CONFIG_DISPLAY) {
		wpa_printf(MSG_INFO, "WPS: Converting display to "
			   "virtual_display for WPS 2.0 compliance");
		config_methods |= WPS_CONFIG_VIRT_DISPLAY;
	}
	if ((config_methods &
	     (WPS_CONFIG_PUSHBUTTON | WPS_CONFIG_VIRT_PUSHBUTTON |
	      WPS_CONFIG_PHY_PUSHBUTTON)) == WPS_CONFIG_PUSHBUTTON) {
		wpa_printf(MSG_INFO, "WPS: Converting push_button to "
			   "virtual_push_button for WPS 2.0 compliance");
		config_methods |= WPS_CONFIG_VIRT_PUSHBUTTON;
	}
#endif /* CONFIG_WPS2 */

	return config_methods;
}


static void wpas_wps_set_uuid(struct wpa_supplicant *wpa_s,
			      struct wps_context *wps)
{
	wpa_printf(MSG_DEBUG, "WPS: Set UUID for interface %s", wpa_s->ifname);
	if (is_nil_uuid(wpa_s->conf->uuid)) {
		struct wpa_supplicant *first;
		first = wpa_s->global->ifaces;
		while (first && first->next)
			first = first->next;
		if (first && first != wpa_s) {
			if (wps != wpa_s->global->ifaces->wps)
				os_memcpy(wps->uuid,
					  wpa_s->global->ifaces->wps->uuid,
					  WPS_UUID_LEN);
			wpa_hexdump(MSG_DEBUG, "WPS: UUID from the first "
				    "interface", wps->uuid, WPS_UUID_LEN);
		} else {
			uuid_gen_mac_addr(wpa_s->own_addr, wps->uuid);
			wpa_hexdump(MSG_DEBUG, "WPS: UUID based on MAC "
				    "address", wps->uuid, WPS_UUID_LEN);
		}
	} else {
		os_memcpy(wps->uuid, wpa_s->conf->uuid, WPS_UUID_LEN);
		wpa_hexdump(MSG_DEBUG, "WPS: UUID based on configuration",
			    wps->uuid, WPS_UUID_LEN);
	}
}


int wpas_wps_init(struct wpa_supplicant *wpa_s)
{
	struct wps_context *wps;
	struct wps_registrar_config rcfg;
	struct hostapd_hw_modes *modes;
	u16 m;

	wps = os_zalloc(sizeof(*wps));
	if (wps == NULL)
		return -1;

	wps->cred_cb = wpa_supplicant_wps_cred;
	wps->event_cb = wpa_supplicant_wps_event;
	wps->cb_ctx = wpa_s;

	wps->dev.device_name = wpa_s->conf->device_name;
	wps->dev.manufacturer = wpa_s->conf->manufacturer;
	wps->dev.model_name = wpa_s->conf->model_name;
	wps->dev.model_number = wpa_s->conf->model_number;
	wps->dev.serial_number = wpa_s->conf->serial_number;
	wps->config_methods =
		wps_config_methods_str2bin(wpa_s->conf->config_methods);
	if ((wps->config_methods & (WPS_CONFIG_DISPLAY | WPS_CONFIG_LABEL)) ==
	    (WPS_CONFIG_DISPLAY | WPS_CONFIG_LABEL)) {
		wpa_printf(MSG_ERROR, "WPS: Both Label and Display config "
			   "methods are not allowed at the same time");
		os_free(wps);
		return -1;
	}
	wps->config_methods = wps_fix_config_methods(wps->config_methods);
	wps->dev.config_methods = wps->config_methods;
	os_memcpy(wps->dev.pri_dev_type, wpa_s->conf->device_type,
		  WPS_DEV_TYPE_LEN);

	wps->dev.num_sec_dev_types = wpa_s->conf->num_sec_device_types;
	os_memcpy(wps->dev.sec_dev_type, wpa_s->conf->sec_device_type,
		  WPS_DEV_TYPE_LEN * wps->dev.num_sec_dev_types);

	wps->dev.os_version = WPA_GET_BE32(wpa_s->conf->os_version);
	modes = wpa_s->hw.modes;
	if (modes) {
		for (m = 0; m < wpa_s->hw.num_modes; m++) {
			if (modes[m].mode == HOSTAPD_MODE_IEEE80211B ||
			    modes[m].mode == HOSTAPD_MODE_IEEE80211G)
				wps->dev.rf_bands |= WPS_RF_24GHZ;
			else if (modes[m].mode == HOSTAPD_MODE_IEEE80211A)
				wps->dev.rf_bands |= WPS_RF_50GHZ;
		}
	}
	if (wps->dev.rf_bands == 0) {
		/*
		 * Default to claiming support for both bands if the driver
		 * does not provide support for fetching supported bands.
		 */
		wps->dev.rf_bands = WPS_RF_24GHZ | WPS_RF_50GHZ;
	}
	os_memcpy(wps->dev.mac_addr, wpa_s->own_addr, ETH_ALEN);
	wpas_wps_set_uuid(wpa_s, wps);

	wps->auth_types = WPS_AUTH_WPA2PSK | WPS_AUTH_WPAPSK;
	wps->encr_types = WPS_ENCR_AES | WPS_ENCR_TKIP;

	os_memset(&rcfg, 0, sizeof(rcfg));
	rcfg.new_psk_cb = wpas_wps_new_psk_cb;
	rcfg.pin_needed_cb = wpas_wps_pin_needed_cb;
	rcfg.set_sel_reg_cb = wpas_wps_set_sel_reg_cb;
	rcfg.cb_ctx = wpa_s;

	wps->registrar = wps_registrar_init(wps, &rcfg);
	if (wps->registrar == NULL) {
		wpa_printf(MSG_DEBUG, "Failed to initialize WPS Registrar");
		os_free(wps);
		return -1;
	}

	wps->wps_external_state = WPS_EXT_STATE_INI;

	wpa_s->wps = wps;

	return 0;
}


void wpas_wps_deinit(struct wpa_supplicant *wpa_s)
{
	eloop_cancel_timeout(wpas_wps_timeout, wpa_s, ELOOP_ALL_CTX);

	if (wpa_s->wps == NULL)
		return;

#ifdef CONFIG_WPS_ER
	wps_er_deinit(wpa_s->wps_er, NULL, NULL);
	wpa_s->wps_er = NULL;
#endif /* CONFIG_WPS_ER */

	wps_registrar_deinit(wpa_s->wps->registrar);
	wpabuf_free(wpa_s->wps->dh_pubkey);
	wpabuf_free(wpa_s->wps->dh_privkey);
	wpabuf_free(wpa_s->wps->oob_conf.pubkey_hash);
	wpabuf_free(wpa_s->wps->oob_conf.dev_password);
	os_free(wpa_s->wps->network_key);
	os_free(wpa_s->wps);
	wpa_s->wps = NULL;
}


int wpas_wps_ssid_bss_match(struct wpa_supplicant *wpa_s,
			    struct wpa_ssid *ssid, struct wpa_scan_res *bss)
{
	struct wpabuf *wps_ie;

	if (!(ssid->key_mgmt & WPA_KEY_MGMT_WPS))
		return -1;

	wps_ie = wpa_scan_get_vendor_ie_multi(bss, WPS_IE_VENDOR_TYPE);
	if (eap_is_wps_pbc_enrollee(&ssid->eap)) {
		if (!wps_ie) {
			wpa_printf(MSG_DEBUG, "   skip - non-WPS AP");
			return 0;
		}

		if (!wps_is_selected_pbc_registrar(wps_ie)) {
			wpa_printf(MSG_DEBUG, "   skip - WPS AP "
				   "without active PBC Registrar");
			wpabuf_free(wps_ie);
			return 0;
		}

		/* TODO: overlap detection */
		wpa_printf(MSG_DEBUG, "   selected based on WPS IE "
			   "(Active PBC)");
		wpabuf_free(wps_ie);
		return 1;
	}

	if (eap_is_wps_pin_enrollee(&ssid->eap)) {
		if (!wps_ie) {
			wpa_printf(MSG_DEBUG, "   skip - non-WPS AP");
			return 0;
		}

		/*
		 * Start with WPS APs that advertise our address as an
		 * authorized MAC (v2.0) or active PIN Registrar (v1.0) and
		 * allow any WPS AP after couple of scans since some APs do not
		 * set Selected Registrar attribute properly when using
		 * external Registrar.
		 */
		if (!wps_is_addr_authorized(wps_ie, wpa_s->own_addr, 1)) {
			if (wpa_s->scan_runs < WPS_PIN_SCAN_IGNORE_SEL_REG) {
				wpa_printf(MSG_DEBUG, "   skip - WPS AP "
					   "without active PIN Registrar");
				wpabuf_free(wps_ie);
				return 0;
			}
			wpa_printf(MSG_DEBUG, "   selected based on WPS IE");
		} else {
			wpa_printf(MSG_DEBUG, "   selected based on WPS IE "
				   "(Authorized MAC or Active PIN)");
		}
		wpabuf_free(wps_ie);
		return 1;
	}

	if (wps_ie) {
		wpa_printf(MSG_DEBUG, "   selected based on WPS IE");
		wpabuf_free(wps_ie);
		return 1;
	}

	return -1;
}


int wpas_wps_ssid_wildcard_ok(struct wpa_supplicant *wpa_s,
			      struct wpa_ssid *ssid,
			      struct wpa_scan_res *bss)
{
	struct wpabuf *wps_ie = NULL;
	int ret = 0;

	if (eap_is_wps_pbc_enrollee(&ssid->eap)) {
		wps_ie = wpa_scan_get_vendor_ie_multi(bss, WPS_IE_VENDOR_TYPE);
		if (wps_ie && wps_is_selected_pbc_registrar(wps_ie)) {
			/* allow wildcard SSID for WPS PBC */
			ret = 1;
		}
	} else if (eap_is_wps_pin_enrollee(&ssid->eap)) {
		wps_ie = wpa_scan_get_vendor_ie_multi(bss, WPS_IE_VENDOR_TYPE);
		if (wps_ie &&
		    (wps_is_addr_authorized(wps_ie, wpa_s->own_addr, 1) ||
		     wpa_s->scan_runs >= WPS_PIN_SCAN_IGNORE_SEL_REG)) {
			/* allow wildcard SSID for WPS PIN */
			ret = 1;
		}
	}

	if (!ret && ssid->bssid_set &&
	    os_memcmp(ssid->bssid, bss->bssid, ETH_ALEN) == 0) {
		/* allow wildcard SSID due to hardcoded BSSID match */
		ret = 1;
	}

#ifdef CONFIG_WPS_STRICT
	if (wps_ie) {
		if (wps_validate_beacon_probe_resp(wps_ie, bss->beacon_ie_len >
						   0, bss->bssid) < 0)
			ret = 0;
		if (bss->beacon_ie_len) {
			struct wpabuf *bcn_wps;
			bcn_wps = wpa_scan_get_vendor_ie_multi_beacon(
				bss, WPS_IE_VENDOR_TYPE);
			if (bcn_wps == NULL) {
				wpa_printf(MSG_DEBUG, "WPS: Mandatory WPS IE "
					   "missing from AP Beacon");
				ret = 0;
			} else {
				if (wps_validate_beacon(wps_ie) < 0)
					ret = 0;
				wpabuf_free(bcn_wps);
			}
		}
	}
#endif /* CONFIG_WPS_STRICT */

	wpabuf_free(wps_ie);

	return ret;
}


int wpas_wps_scan_pbc_overlap(struct wpa_supplicant *wpa_s,
			      struct wpa_bss *selected, struct wpa_ssid *ssid)
{
	const u8 *sel_uuid, *uuid;
	struct wpabuf *wps_ie;
	int ret = 0;
	struct wpa_bss *bss;

	if (!eap_is_wps_pbc_enrollee(&ssid->eap))
		return 0;

	wpa_printf(MSG_DEBUG, "WPS: Check whether PBC session overlap is "
		   "present in scan results; selected BSSID " MACSTR,
		   MAC2STR(selected->bssid));

	/* Make sure that only one AP is in active PBC mode */
	wps_ie = wpa_bss_get_vendor_ie_multi(selected, WPS_IE_VENDOR_TYPE);
	if (wps_ie) {
		sel_uuid = wps_get_uuid_e(wps_ie);
		wpa_hexdump(MSG_DEBUG, "WPS: UUID of the selected BSS",
			    sel_uuid, UUID_LEN);
	} else {
		wpa_printf(MSG_DEBUG, "WPS: Selected BSS does not include "
			   "WPS IE?!");
		sel_uuid = NULL;
	}

	dl_list_for_each(bss, &wpa_s->bss, struct wpa_bss, list) {
		struct wpabuf *ie;
		if (bss == selected)
			continue;
		ie = wpa_bss_get_vendor_ie_multi(bss, WPS_IE_VENDOR_TYPE);
		if (!ie)
			continue;
		if (!wps_is_selected_pbc_registrar(ie)) {
			wpabuf_free(ie);
			continue;
		}
		wpa_printf(MSG_DEBUG, "WPS: Another BSS in active PBC mode: "
			   MACSTR, MAC2STR(bss->bssid));
		uuid = wps_get_uuid_e(ie);
		wpa_hexdump(MSG_DEBUG, "WPS: UUID of the other BSS",
			    uuid, UUID_LEN);

		/*
		 * Another WPS PBC active in the scan results. Is it a
		 * multi-radio AP (same UUID)? If so this is not an overlap.
		 */
		if (sel_uuid != NULL && uuid != NULL &&
		    os_memcmp(sel_uuid, uuid, UUID_LEN) == 0) {
			wpa_printf(MSG_INFO, "No PBC overlap - same UUID\n");
		} else if (os_memcmp(bss->bssid, selected->bssid, ETH_ALEN) != 0) {
			ret = 1; /* PBC overlap */
			wpa_msg(wpa_s, MSG_INFO, "WPS: PBC overlap detected: "
				MACSTR " and " MACSTR,
				MAC2STR(selected->bssid),
				MAC2STR(bss->bssid));
			wpabuf_free(ie);
			break;
		}

		/* TODO: verify that this is reasonable dual-band situation */

		wpabuf_free(ie);
	}

	wpabuf_free(wps_ie);

	return ret;
}


void wpas_wps_notify_scan_results(struct wpa_supplicant *wpa_s)
{
	struct wpa_bss *bss;
	unsigned int pbc = 0, auth = 0, pin = 0, wps = 0;

	if (wpa_s->disconnected || wpa_s->wpa_state >= WPA_ASSOCIATED)
		return;

	dl_list_for_each(bss, &wpa_s->bss, struct wpa_bss, list) {
		struct wpabuf *ie;
		ie = wpa_bss_get_vendor_ie_multi(bss, WPS_IE_VENDOR_TYPE);
		if (!ie)
			continue;
		if (wps_is_selected_pbc_registrar(ie))
			pbc++;
		else if (wps_is_addr_authorized(ie, wpa_s->own_addr, 0))
			auth++;
		else if (wps_is_selected_pin_registrar(ie))
			pin++;
		else
			wps++;
		wpabuf_free(ie);
	}

	if (pbc)
		wpa_msg_ctrl(wpa_s, MSG_INFO, WPS_EVENT_AP_AVAILABLE_PBC);
	else if (auth)
		wpa_msg_ctrl(wpa_s, MSG_INFO, WPS_EVENT_AP_AVAILABLE_AUTH);
	else if (pin)
		wpa_msg_ctrl(wpa_s, MSG_INFO, WPS_EVENT_AP_AVAILABLE_PIN);
	else if (wps)
		wpa_msg_ctrl(wpa_s, MSG_INFO, WPS_EVENT_AP_AVAILABLE);
}


int wpas_wps_searching(struct wpa_supplicant *wpa_s)
{
	struct wpa_ssid *ssid;

	for (ssid = wpa_s->conf->ssid; ssid; ssid = ssid->next) {
		if ((ssid->key_mgmt & WPA_KEY_MGMT_WPS) && !ssid->disabled)
			return 1;
	}

	return 0;
}


int wpas_wps_scan_result_text(const u8 *ies, size_t ies_len, char *buf,
			      char *end)
{
	struct wpabuf *wps_ie;
	int ret;

	wps_ie = ieee802_11_vendor_ie_concat(ies, ies_len, WPS_DEV_OUI_WFA);
	if (wps_ie == NULL)
		return 0;

	ret = wps_attr_text(wps_ie, buf, end);
	wpabuf_free(wps_ie);
	return ret;
}


int wpas_wps_er_start(struct wpa_supplicant *wpa_s, const char *filter)
{
#ifdef CONFIG_WPS_ER
	if (wpa_s->wps_er) {
		wps_er_refresh(wpa_s->wps_er);
		return 0;
	}
	wpa_s->wps_er = wps_er_init(wpa_s->wps, wpa_s->ifname, filter);
	if (wpa_s->wps_er == NULL)
		return -1;
	return 0;
#else /* CONFIG_WPS_ER */
	return 0;
#endif /* CONFIG_WPS_ER */
}


int wpas_wps_er_stop(struct wpa_supplicant *wpa_s)
{
#ifdef CONFIG_WPS_ER
	wps_er_deinit(wpa_s->wps_er, NULL, NULL);
	wpa_s->wps_er = NULL;
#endif /* CONFIG_WPS_ER */
	return 0;
}


#ifdef CONFIG_WPS_ER
int wpas_wps_er_add_pin(struct wpa_supplicant *wpa_s, const u8 *addr,
			const char *uuid, const char *pin)
{
	u8 u[UUID_LEN];
	int any = 0;

	if (os_strcmp(uuid, "any") == 0)
		any = 1;
	else if (uuid_str2bin(uuid, u))
		return -1;
	return wps_registrar_add_pin(wpa_s->wps->registrar, addr,
				     any ? NULL : u,
				     (const u8 *) pin, os_strlen(pin), 300);
}


int wpas_wps_er_pbc(struct wpa_supplicant *wpa_s, const char *uuid)
{
	u8 u[UUID_LEN];

	if (uuid_str2bin(uuid, u))
		return -1;
	return wps_er_pbc(wpa_s->wps_er, u);
}


int wpas_wps_er_learn(struct wpa_supplicant *wpa_s, const char *uuid,
		      const char *pin)
{
	u8 u[UUID_LEN];

	if (uuid_str2bin(uuid, u))
		return -1;
	return wps_er_learn(wpa_s->wps_er, u, (const u8 *) pin,
			    os_strlen(pin));
}


int wpas_wps_er_set_config(struct wpa_supplicant *wpa_s, const char *uuid,
			   int id)
{
	u8 u[UUID_LEN];
	struct wpa_ssid *ssid;
	struct wps_credential cred;

	if (uuid_str2bin(uuid, u))
		return -1;
	ssid = wpa_config_get_network(wpa_s->conf, id);
	if (ssid == NULL || ssid->ssid == NULL)
		return -1;

	os_memset(&cred, 0, sizeof(cred));
	if (ssid->ssid_len > 32)
		return -1;
	os_memcpy(cred.ssid, ssid->ssid, ssid->ssid_len);
	cred.ssid_len = ssid->ssid_len;
	if (ssid->key_mgmt & WPA_KEY_MGMT_PSK) {
		cred.auth_type = (ssid->proto & WPA_PROTO_RSN) ?
			WPS_AUTH_WPA2PSK : WPS_AUTH_WPAPSK;
		if (ssid->pairwise_cipher & WPA_CIPHER_CCMP)
			cred.encr_type = WPS_ENCR_AES;
		else
			cred.encr_type = WPS_ENCR_TKIP;
		if (ssid->passphrase) {
			cred.key_len = os_strlen(ssid->passphrase);
			if (cred.key_len >= 64)
				return -1;
			os_memcpy(cred.key, ssid->passphrase, cred.key_len);
		} else if (ssid->psk_set) {
			cred.key_len = 32;
			os_memcpy(cred.key, ssid->psk, 32);
		} else
			return -1;
	} else {
		cred.auth_type = WPS_AUTH_OPEN;
		cred.encr_type = WPS_ENCR_NONE;
	}
	return wps_er_set_config(wpa_s->wps_er, u, &cred);
}


int wpas_wps_er_config(struct wpa_supplicant *wpa_s, const char *uuid,
		       const char *pin, struct wps_new_ap_settings *settings)
{
	u8 u[UUID_LEN];
	struct wps_credential cred;
	size_t len;

	if (uuid_str2bin(uuid, u))
		return -1;
	if (settings->ssid_hex == NULL || settings->auth == NULL ||
	    settings->encr == NULL || settings->key_hex == NULL)
		return -1;

	os_memset(&cred, 0, sizeof(cred));
	len = os_strlen(settings->ssid_hex);
	if ((len & 1) || len > 2 * sizeof(cred.ssid) ||
	    hexstr2bin(settings->ssid_hex, cred.ssid, len / 2))
		return -1;
	cred.ssid_len = len / 2;

	len = os_strlen(settings->key_hex);
	if ((len & 1) || len > 2 * sizeof(cred.key) ||
	    hexstr2bin(settings->key_hex, cred.key, len / 2))
		return -1;
	cred.key_len = len / 2;

	if (os_strcmp(settings->auth, "OPEN") == 0)
		cred.auth_type = WPS_AUTH_OPEN;
	else if (os_strcmp(settings->auth, "WPAPSK") == 0)
		cred.auth_type = WPS_AUTH_WPAPSK;
	else if (os_strcmp(settings->auth, "WPA2PSK") == 0)
		cred.auth_type = WPS_AUTH_WPA2PSK;
	else
		return -1;

	if (os_strcmp(settings->encr, "NONE") == 0)
		cred.encr_type = WPS_ENCR_NONE;
	else if (os_strcmp(settings->encr, "WEP") == 0)
		cred.encr_type = WPS_ENCR_WEP;
	else if (os_strcmp(settings->encr, "TKIP") == 0)
		cred.encr_type = WPS_ENCR_TKIP;
	else if (os_strcmp(settings->encr, "CCMP") == 0)
		cred.encr_type = WPS_ENCR_AES;
	else
		return -1;

	return wps_er_config(wpa_s->wps_er, u, (const u8 *) pin,
			     os_strlen(pin), &cred);
}


static int callbacks_pending = 0;

static void wpas_wps_terminate_cb(void *ctx)
{
	wpa_printf(MSG_DEBUG, "WPS ER: Terminated");
	if (--callbacks_pending <= 0)
		eloop_terminate();
}
#endif /* CONFIG_WPS_ER */


int wpas_wps_terminate_pending(struct wpa_supplicant *wpa_s)
{
#ifdef CONFIG_WPS_ER
	if (wpa_s->wps_er) {
		callbacks_pending++;
		wps_er_deinit(wpa_s->wps_er, wpas_wps_terminate_cb, wpa_s);
		wpa_s->wps_er = NULL;
		return 1;
	}
#endif /* CONFIG_WPS_ER */
	return 0;
}


int wpas_wps_in_progress(struct wpa_supplicant *wpa_s)
{
	struct wpa_ssid *ssid;

	for (ssid = wpa_s->conf->ssid; ssid; ssid = ssid->next) {
		if (!ssid->disabled && ssid->key_mgmt == WPA_KEY_MGMT_WPS)
			return 1;
	}

	return 0;
}


void wpas_wps_update_config(struct wpa_supplicant *wpa_s)
{
	struct wps_context *wps = wpa_s->wps;

	if (wps == NULL)
		return;

	if (wpa_s->conf->changed_parameters & CFG_CHANGED_CONFIG_METHODS) {
		wps->config_methods = wps_config_methods_str2bin(
			wpa_s->conf->config_methods);
		if ((wps->config_methods &
		     (WPS_CONFIG_DISPLAY | WPS_CONFIG_LABEL)) ==
		    (WPS_CONFIG_DISPLAY | WPS_CONFIG_LABEL)) {
			wpa_printf(MSG_ERROR, "WPS: Both Label and Display "
				   "config methods are not allowed at the "
				   "same time");
			wps->config_methods &= ~WPS_CONFIG_LABEL;
		}
	}
	wps->config_methods = wps_fix_config_methods(wps->config_methods);

	if (wpa_s->conf->changed_parameters & CFG_CHANGED_DEVICE_TYPE)
		os_memcpy(wps->dev.pri_dev_type, wpa_s->conf->device_type,
			  WPS_DEV_TYPE_LEN);

	if (wpa_s->conf->changed_parameters & CFG_CHANGED_SEC_DEVICE_TYPE) {
		wps->dev.num_sec_dev_types = wpa_s->conf->num_sec_device_types;
		os_memcpy(wps->dev.sec_dev_type, wpa_s->conf->sec_device_type,
			  wps->dev.num_sec_dev_types * WPS_DEV_TYPE_LEN);
	}

	if (wpa_s->conf->changed_parameters & CFG_CHANGED_OS_VERSION)
		wps->dev.os_version = WPA_GET_BE32(wpa_s->conf->os_version);

	if (wpa_s->conf->changed_parameters & CFG_CHANGED_UUID)
		wpas_wps_set_uuid(wpa_s, wps);

	if (wpa_s->conf->changed_parameters &
	    (CFG_CHANGED_DEVICE_NAME | CFG_CHANGED_WPS_STRING)) {
		/* Update pointers to make sure they refer current values */
		wps->dev.device_name = wpa_s->conf->device_name;
		wps->dev.manufacturer = wpa_s->conf->manufacturer;
		wps->dev.model_name = wpa_s->conf->model_name;
		wps->dev.model_number = wpa_s->conf->model_number;
		wps->dev.serial_number = wpa_s->conf->serial_number;
	}
}
