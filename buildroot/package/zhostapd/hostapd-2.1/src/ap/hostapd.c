/*
 * hostapd / Initialization and configuration
 * Copyright (c) 2002-2014, Jouni Malinen <j@w1.fi>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "utils/includes.h"

#include "utils/common.h"
#include "utils/eloop.h"
#include "common/ieee802_11_defs.h"
#include "common/wpa_ctrl.h"
#include "radius/radius_client.h"
#include "radius/radius_das.h"
#include "hostapd.h"
#include "authsrv.h"
#include "sta_info.h"
#include "accounting.h"
#include "ap_list.h"
#include "beacon.h"
#include "iapp.h"
#include "ieee802_1x.h"
#include "ieee802_11_auth.h"
#include "vlan_init.h"
#include "wpa_auth.h"
#include "wps_hostapd.h"
#include "hw_features.h"
#include "wpa_auth_glue.h"
#include "ap_drv_ops.h"
#include "ap_config.h"
#include "p2p_hostapd.h"
#include "gas_serv.h"
#include "dfs.h"
#include "qtn_hapd/qtn_hapd_scs.h"
#include "qtn_hapd/qtn_hapd_pp.h"

static int hostapd_flush_old_stations(struct hostapd_data *hapd, u16 reason);
static int hostapd_setup_encryption(char *iface, struct hostapd_data *hapd);
static int hostapd_broadcast_wep_clear(struct hostapd_data *hapd);
static int setup_interface2(struct hostapd_iface *iface);
static void channel_list_update_timeout(void *eloop_ctx, void *timeout_ctx);
static void hostapd_bss_deinit(struct hostapd_data *hapd);
static int hostapd_setup_bss(struct hostapd_data *hapd, int first);
static int hostapd_remove_bss(struct hostapd_iface *iface, unsigned int idx);


int hostapd_for_each_interface(struct hapd_interfaces *interfaces,
			       int (*cb)(struct hostapd_iface *iface,
					 void *ctx), void *ctx)
{
	size_t i;
	int ret;

	for (i = 0; i < interfaces->count; i++) {
		ret = cb(interfaces->iface[i], ctx);
		if (ret)
			return ret;
	}

	return 0;
}

static int
hostapd_sta_remove_sm(struct hostapd_data *hapd,
		      struct sta_info *sta, void *ctx)
{
	wpa_auth_sta_deinit(sta->wpa_sm);
	sta->wpa_sm = NULL;
	return 0;
}

static int hostapd_probe_req_event(void *ctx, const u8 *addr, const u8 *da,
				   const u8 *bssid,
				   const u8 *ie, size_t ie_len,
				   int ssi_signal)
{
	struct hostapd_data *hapd = (struct hostapd_data *) ctx;
	struct ieee802_11_elems elems;
	char ssid[HOSTAPD_MAX_SSID_LEN + 1];
	ParseRes parse_result;

	parse_result = ieee802_11_parse_elems(ie, ie_len, &elems, 0);

	if (parse_result == ParseFailed) {
		wpa_printf(MSG_DEBUG, "Could not parse ProbeReq from "
			   MACSTR, MAC2STR(addr));
		return parse_result;
	}

	if (elems.ssid) {
		ieee802_11_print_ssid(ssid, elems.ssid, elems.ssid_len);
	} else {
		ssid[0] = 0;
	}

	/* this will be sent to ctrl iface */
	wpa_msg(hapd->msg_ctx, MSG_INFO, "PROBE-REQ "MACSTR" %s%s%s",
		MAC2STR(addr), ssid[0] ? "'" : "(", ssid[0] ? ssid : "broadcast",
		ssid[0] ? "'" : ")");
	return parse_result;
}
static void hostapd_reload_bss(struct hostapd_data *hapd)
{
	struct hostapd_ssid *ssid;

#ifndef CONFIG_NO_RADIUS
	radius_client_reconfig(hapd->radius, hapd->conf->radius);
#endif /* CONFIG_NO_RADIUS */

	if (hapd->conf->wmm_enabled < 0)
		hapd->conf->wmm_enabled = hapd->iconf->ieee80211n;

	ssid = &hapd->conf->ssid;
	if (!ssid->wpa_psk_set && ssid->wpa_psk && !ssid->wpa_psk->next &&
	    ssid->wpa_passphrase_set && ssid->wpa_passphrase) {
		/*
		 * Force PSK to be derived again since SSID or passphrase may
		 * have changed.
		 */
		os_free(ssid->wpa_psk);
		ssid->wpa_psk = NULL;
	}
	if (hostapd_setup_wpa_psk(hapd->conf)) {
		wpa_printf(MSG_ERROR, "Failed to re-configure WPA PSK "
			   "after reloading configuration");
	}

	if (hapd->conf->ieee802_1x || hapd->conf->wpa)
		hostapd_set_drv_ieee8021x(hapd, hapd->conf->iface, 1);
	else
		hostapd_set_drv_ieee8021x(hapd, hapd->conf->iface, 0);

	ieee802_1x_eap_auth_update(hapd);

	if ((hapd->conf->wpa || hapd->conf->osen) && hapd->wpa_auth == NULL) {
		hostapd_setup_wpa(hapd);
		if (hapd->wpa_auth)
			wpa_init_keys(hapd->wpa_auth);
	} else if (hapd->conf->wpa) {
		const u8 *wpa_ie;
		size_t wpa_ie_len;
		hostapd_reconfig_wpa(hapd);
		wpa_ie = wpa_auth_get_wpa_ie(hapd->wpa_auth, &wpa_ie_len);
		if (hostapd_set_generic_elem(hapd, wpa_ie, wpa_ie_len))
			wpa_printf(MSG_ERROR, "Failed to configure WPA IE for "
				   "the kernel driver.");
	} else if (hapd->wpa_auth) {
		/* Disabling security */
		ap_for_each_sta(hapd, hostapd_sta_remove_sm, NULL);
		wpa_deinit(hapd->wpa_auth);
		hapd->wpa_auth = NULL;
		hostapd_set_privacy(hapd, 0);
		hostapd_broadcast_wep_clear(hapd);
		hostapd_setup_encryption(hapd->conf->iface, hapd);
		hostapd_set_generic_elem(hapd, (u8 *) "", 0);
	}

	/* Enable or disable SSID broadcast in beacons */
	if (hostapd_set_broadcast_ssid(hapd,
			!!hapd->conf->ignore_broadcast_ssid)) {
		wpa_printf(MSG_WARNING, "Could not modify broadcast SSID flag");
	}

	ieee802_11_set_beacon(hapd);
	hostapd_deinit_wps(hapd);
	if (hostapd_init_wps(hapd, hapd->conf)) {
		wpa_printf(MSG_ERROR, "Could not reconfigure WPS");
	}
	hostapd_init_wps_complete(hapd);

	if (hapd->conf->ssid.ssid_set &&
	    hostapd_set_ssid(hapd, hapd->conf->ssid.ssid,
			     hapd->conf->ssid.ssid_len)) {
		wpa_printf(MSG_ERROR, "Could not set SSID for kernel driver");
		/* try to continue */
	}

	if (hapd->conf->set_assoc_limit_required &&
	    hostapd_set_bss_assoc_limit(hapd, hapd->conf->max_num_sta)) {
		wpa_printf(MSG_ERROR, "Could not set max_num_sta for kernel driver");
	}

	qtn_hapd_pp2_setup(hapd);

	hapd->pbc_detect_enhance = hapd->conf->pbc_detect_enhance;

	wpa_printf(MSG_DEBUG, "Reconfigured interface %s", hapd->conf->iface);
}


static void hostapd_clear_old(struct hostapd_iface *iface)
{
	size_t j;

	/*
	 * Deauthenticate all stations since the new configuration may not
	 * allow them to use the BSS anymore.
	 */
	for (j = 0; j < iface->num_bss; j++) {
		hostapd_flush_old_stations(iface->bss[j],
					   WLAN_REASON_PREV_AUTH_NOT_VALID);
		hostapd_broadcast_wep_clear(iface->bss[j]);

#ifndef CONFIG_NO_RADIUS
		/* TODO: update dynamic data based on changed configuration
		 * items (e.g., open/close sockets, etc.) */
		radius_client_flush(iface->bss[j]->radius, 0);
#endif /* CONFIG_NO_RADIUS */
	}
}


/* Find the index of a BSS by name on a given parent iface */
static int hostapd_find_bss(struct hostapd_iface *iface, const char *ifname)
{
	int i;

	for (i = 0; i < iface->num_bss; i++) {
		struct hostapd_data *hapd = iface->bss[i];
		if (!hapd)
			return -1;

		if (strncmp(ifname, hapd->conf->iface, IFNAMSIZ) == 0)
			return i;
	}

	/* Not found */
	return -1;
}


static int hostapd_find_bss_config(struct hostapd_config *conf, const char *ifname)
{
	size_t j;

	if (conf == NULL)
		return -1;

	for (j = 0; j < conf->num_bss; j++) {
		if (strncmp(conf->bss[j]->iface, ifname, IFNAMSIZ) == 0)
			return j;
	}

	return -1;
}

int hostapd_add_bss(struct hostapd_iface *iface, const char *bss_name)
{
	struct hostapd_config *newconf, *curconf;
	int cfg_idx, bss_idx;
	struct hostapd_data *hapd;
	int ret;

	if (iface->interfaces == NULL || iface->interfaces->config_read_cb == NULL)
		return -1;

	newconf = iface->interfaces->config_read_cb(iface->config_fname);
	if (newconf == NULL)
		return -1;

	cfg_idx = hostapd_find_bss_config(newconf, bss_name);
	bss_idx = hostapd_find_bss(iface, bss_name);

	if ((cfg_idx >= 0) && (bss_idx < 0)) {
		struct hostapd_bss_config **bss_cfg_array;

		if (iface->num_bss >= MAX_BSSID) {
			wpa_printf(MSG_ERROR, "exceed allowed BSS number (%d)", MAX_BSSID);
			ret = -1;
			goto ab_out;
		}

		curconf = iface->conf;
		bss_cfg_array = os_realloc_array(curconf->bss,
				curconf->num_bss + 1,
				sizeof(struct hostapd_bss_config *));
		if (bss_cfg_array == NULL) {
			wpa_printf(MSG_ERROR, "bss config array realloc fail");
			ret = -1;
			goto ab_out;
		}
		curconf->bss = bss_cfg_array;
		curconf->bss[curconf->num_bss++] = newconf->bss[cfg_idx];
		curconf->last_bss = newconf->bss[cfg_idx];
		newconf->bss[cfg_idx] = NULL;

		hapd = hostapd_alloc_bss_data(iface, curconf, curconf->last_bss);
		iface->bss[iface->num_bss++] = hapd;
		hapd->msg_ctx = hapd;
		hostapd_setup_bss(hapd, 0);
	} else {
		wpa_printf(MSG_ERROR, "Invalid bss %s\n", bss_name);
		ret = -1;
		goto ab_out;
	}

	if (hostapd_driver_commit(hapd) < 0) {
		wpa_printf(MSG_ERROR, "%s: Failed to commit driver "
			   "configuration", __func__);
		ret = -1;
		goto ab_out;
	}

	ret = 0;

ab_out:
	hostapd_config_free(newconf);
	return ret;
}


int hostapd_del_bss(struct hostapd_iface *iface, const char *bss_name)
{
	int bss_idx;
	int i;

	bss_idx = hostapd_find_bss(iface, bss_name);

	if (bss_idx < 0) {
		wpa_printf(MSG_ERROR, "BSS %s not exist", bss_name);
		return -1;
	} else if (iface->bss[bss_idx]->primary_interface) {
		wpa_printf(MSG_ERROR, "Could not remove primary interface");
		return -1;
	}

	/* restore bss for HW Push button to primary interface */
	if (iface->bss[bss_idx] == iface->default_pbc_bss) {
		for (i = 0; i < iface->num_bss; i++) {
			if (iface->bss[i]->primary_interface) {
				iface->default_pbc_bss = iface->bss[i];
				break;
			}
		}
	}

	return hostapd_remove_bss(iface, bss_idx);
}

static int hostapd_is_if_up(int skfd, const char *ifname)
{
	struct ifreq	ifr;
	int retval = -1;

	strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	retval = ioctl(skfd, SIOCGIFFLAGS, &ifr);

	if (retval >= 0)
	{
		int	interface_up_flags = (IFF_UP | IFF_RUNNING);
		int	result_flags = ifr.ifr_flags & (interface_up_flags);

		if (result_flags == interface_up_flags)
			return 0;
		else
			return -1;
	} else {
		return retval;
	}
}

int hostapd_update_bss(struct hostapd_iface *iface, const char *bss_name)
{
	struct hostapd_config *newconf, *curconf;
	int bss_idx, cfg_idx, cfg_idx_old;
	struct hostapd_data *hapd;
	int ret;
	int if_up = -1;

	if (iface->interfaces == NULL || iface->interfaces->config_read_cb == NULL)
		return -1;

	newconf = iface->interfaces->config_read_cb(iface->config_fname);
	if (newconf == NULL)
		return -1;

	curconf = iface->conf;
	bss_idx = hostapd_find_bss(iface, bss_name);
	cfg_idx = hostapd_find_bss_config(newconf, bss_name);
	cfg_idx_old = hostapd_find_bss_config(curconf, bss_name);

	if ((bss_idx >= 0) && (cfg_idx >= 0)) {
		struct hostapd_bss_config *old_bss_cfg;
		const char *current_wps_ap_pin = NULL;
		char retained_wps_ap_pin[WPS_AP_PIN_LEN + 1];

		hapd = iface->bss[bss_idx];
		hostapd_flush_old_stations(hapd,
					   WLAN_REASON_PREV_AUTH_NOT_VALID);
		hostapd_broadcast_wep_clear(hapd);
		vlan_deinit(hapd);

#ifndef CONFIG_NO_RADIUS
		radius_client_flush(hapd->radius, 0);
#endif /* CONFIG_NO_RADIUS */

		old_bss_cfg = curconf->bss[cfg_idx_old];
		curconf->bss[cfg_idx_old] = newconf->bss[cfg_idx];
		hapd->conf = newconf->bss[cfg_idx];
		newconf->bss[cfg_idx] = NULL;

		current_wps_ap_pin = hostapd_wps_ap_pin_get(hapd);
		if (current_wps_ap_pin) {
			os_memcpy(retained_wps_ap_pin, current_wps_ap_pin,
					WPS_AP_PIN_LEN);
			retained_wps_ap_pin[WPS_AP_PIN_LEN] = 0;
		}

		hostapd_reload_bss(hapd);

		if (current_wps_ap_pin && !hapd->conf->ap_pin) {
			hostapd_wps_ap_pin_set(hapd, (const char *) &retained_wps_ap_pin,
					WPS_AP_PIN_DEFAULT_TIMEOUT);
		}

		hostapd_config_free_bss(old_bss_cfg);

		if_up = hostapd_is_if_up(hapd->ctrl_sock, bss_name);
	} else {
		wpa_printf(MSG_ERROR, "Invalid BSS %s", bss_name);
		ret = -1;
		goto ub_out;
	}

	if (if_up >= 0) {
		if (hostapd_driver_commit(hapd) < 0) {
			wpa_printf(MSG_ERROR, "%s: Failed to commit driver "
				   "configuration", __func__);
			ret = -1;
			goto ub_out;
		}
	}

	ret = 0;

ub_out:
	hostapd_config_free(newconf);
	return ret;
}


/*
 * The configuration can contain interfaces in any order. The strategy on
 * configuration reload is to iterate through each item in the new config and
 * compare with the actual running BSSes, not just the current configuration.
 * If a BSS has been removed, the array of pointers to BSSes needs to be
 * "shuffled down" to prevent leaving gaps. But the interfaces can still be in
 * any order, i.e. ifname is not related to their BSS index.
 */

/* Check new configuration against currently configured BSSes. Returns 1 if a
 * BSS was removed, 0 if there were none / no more to remove */
static int hostapd_check_for_removed_bss(struct hostapd_iface *iface,
		struct hostapd_config *newconf)
{
	int i, j;

	for (i = 0; i < iface->num_bss; i++) {
		int active = 0;
		for (j = 0; j < newconf->num_bss; j++) {
			if (strncmp(iface->bss[i]->conf->iface,
					newconf->bss[j]->iface,
					IFNAMSIZ) == 0) {
				active = 1;
				break;
			}
		}
		if (!active) {
			struct hostapd_data *bss_hapd = iface->bss[i];
			wpa_printf(MSG_DEBUG, "Interface %s removed",
					bss_hapd->conf->iface);
			hostapd_bss_deinit(bss_hapd);
			os_free(bss_hapd);
			iface->num_bss--;
			/* Shuffle pointers down */
			for (j = i; j < MAX_BSSID - 1; j++)
				iface->bss[j] = iface->bss[j + 1];
			return 1;
		}
	}

	/* Nothing else left to remove */
	return 0;
}


int hostapd_reload_config(struct hostapd_iface *iface)
{
	struct hostapd_data *hapd = iface->bss[0];
	struct hostapd_config *newconf, *oldconf;
	size_t i,j;

	if (iface->config_fname == NULL) {
		/* Only in-memory config in use - assume it has been updated */
		hostapd_clear_old(iface);
		for (j = 0; j < iface->num_bss; j++)
			hostapd_reload_bss(iface->bss[j]);
		return 0;
	}

	if (iface->interfaces == NULL ||
	    iface->interfaces->config_read_cb == NULL)
		return -1;
	newconf = iface->interfaces->config_read_cb(iface->config_fname);
	if (newconf == NULL)
		return -1;

	hostapd_clear_old(iface);

	oldconf = hapd->iconf;
	iface->conf = newconf;

	/* Remove old BSSes first */
	while (hostapd_check_for_removed_bss(iface, newconf));

	/* Reconfigure existing and add new interfaces */
	for (i = 0; i < newconf->num_bss; i++) {
		struct hostapd_data *hapd_bss;
		int idx = hostapd_find_bss(iface, newconf->bss[i]->iface);
		int if_up = 0;
		if (idx >= 0) {
			const char *current_wps_ap_pin = NULL;
			char retained_wps_ap_pin[WPS_AP_PIN_LEN + 1];

			hapd_bss = iface->bss[idx];

			/* Check if WPS AP Pin is set. If so, retain it. */
			current_wps_ap_pin = hostapd_wps_ap_pin_get(hapd_bss);
			if (current_wps_ap_pin) {
				os_memcpy(retained_wps_ap_pin, current_wps_ap_pin,
						WPS_AP_PIN_LEN);
				retained_wps_ap_pin[WPS_AP_PIN_LEN] = 0;
			}

			hapd_bss->iconf = newconf;
			hapd_bss->conf = newconf->bss[i];
			/* TODO: compare iface->bss[idx].conf with
			 * newconf->bss[i] and reload iff different */

			hostapd_reload_bss(hapd_bss);

			if (current_wps_ap_pin && !hapd_bss->conf->ap_pin) {
				hostapd_wps_ap_pin_set(hapd_bss, (const char *) &retained_wps_ap_pin,
						WPS_AP_PIN_DEFAULT_TIMEOUT);
			}
			if_up = hostapd_is_if_up(hapd->ctrl_sock, iface->config_fname);
		} else {
			/* Create new BSS */
			hapd_bss = hostapd_alloc_bss_data(iface, newconf,
					newconf->bss[i]);
			iface->bss[i] = hapd_bss;
			hapd_bss->msg_ctx = hapd_bss;
			hostapd_setup_bss(iface->bss[i], 0);
			if_up = 0;
		}

		if (if_up >= 0) {
			if (hostapd_driver_commit(hapd_bss) < 0) {
				wpa_printf(MSG_ERROR, "%s: Failed to commit driver "
					   "configuration", __func__);
				hostapd_config_free(oldconf);
				return -1;
			}
		}
	}
	iface->num_bss = newconf->num_bss;

	hostapd_config_free(oldconf);

	return 0;
}


static void hostapd_broadcast_key_clear_iface(struct hostapd_data *hapd,
					      char *ifname)
{
	int i;

	for (i = 0; i < NUM_WEP_KEYS; i++) {
		if (hostapd_drv_set_key(ifname, hapd, WPA_ALG_NONE, NULL, i,
					0, NULL, 0, NULL, 0)) {
			wpa_printf(MSG_DEBUG, "Failed to clear default "
				   "encryption keys (ifname=%s keyidx=%d)",
				   ifname, i);
		}
	}
#ifdef CONFIG_IEEE80211W
	if (hapd->conf->ieee80211w) {
		for (i = NUM_WEP_KEYS; i < NUM_WEP_KEYS + 2; i++) {
			if (hostapd_drv_set_key(ifname, hapd, WPA_ALG_NONE,
						NULL, i, 0, NULL,
						0, NULL, 0)) {
				wpa_printf(MSG_DEBUG, "Failed to clear "
					   "default mgmt encryption keys "
					   "(ifname=%s keyidx=%d)", ifname, i);
			}
		}
	}
#endif /* CONFIG_IEEE80211W */
}


static int hostapd_broadcast_wep_clear(struct hostapd_data *hapd)
{
	hostapd_broadcast_key_clear_iface(hapd, hapd->conf->iface);
	return 0;
}


static int hostapd_broadcast_wep_set(struct hostapd_data *hapd)
{
	int errors = 0, idx;
	struct hostapd_ssid *ssid = &hapd->conf->ssid;

	idx = ssid->wep.idx;
	if (ssid->wep.default_len &&
	    hostapd_drv_set_key(hapd->conf->iface,
				hapd, WPA_ALG_WEP, broadcast_ether_addr, idx,
				1, NULL, 0, ssid->wep.key[idx],
				ssid->wep.len[idx])) {
		wpa_printf(MSG_WARNING, "Could not set WEP encryption.");
		errors++;
	}

	return errors;
}


static void hostapd_free_hapd_data(struct hostapd_data *hapd)
{
	if (!hapd->started) {
		wpa_printf(MSG_ERROR, "%s: Interface %s wasn't started",
			   __func__, hapd->conf->iface);
		return;
	}
	hapd->started = 0;

	wpa_printf(MSG_DEBUG, "%s(%s)", __func__, hapd->conf->iface);
        hostapd_scs_deinit(hapd->scs);
        hapd->scs = NULL;
	iapp_deinit(hapd->iapp);
	hapd->iapp = NULL;
	accounting_deinit(hapd);
	hostapd_deinit_wpa(hapd);
	vlan_deinit(hapd);
	hostapd_acl_deinit(hapd);
#ifndef CONFIG_NO_RADIUS
	radius_client_deinit(hapd->radius);
	hapd->radius = NULL;
	radius_das_deinit(hapd->radius_das);
	hapd->radius_das = NULL;
#endif /* CONFIG_NO_RADIUS */

	hostapd_deinit_wps(hapd);

	authsrv_deinit(hapd);

	if (hapd->interface_added &&
	    hostapd_if_remove(hapd, WPA_IF_AP_BSS, hapd->conf->iface)) {
		wpa_printf(MSG_WARNING, "Failed to remove BSS interface %s",
			   hapd->conf->iface);
	}

	os_free(hapd->probereq_cb);
	hapd->probereq_cb = NULL;

#ifdef CONFIG_P2P
	wpabuf_free(hapd->p2p_beacon_ie);
	hapd->p2p_beacon_ie = NULL;
	wpabuf_free(hapd->p2p_probe_resp_ie);
	hapd->p2p_probe_resp_ie = NULL;
#endif /* CONFIG_P2P */

	wpabuf_free(hapd->time_adv);

#ifdef CONFIG_INTERWORKING
	gas_serv_deinit(hapd);
#endif /* CONFIG_INTERWORKING */

#ifdef CONFIG_SQLITE
	os_free(hapd->tmp_eap_user.identity);
	os_free(hapd->tmp_eap_user.password);
#endif /* CONFIG_SQLITE */
}


/**
 * hostapd_cleanup - Per-BSS cleanup (deinitialization)
 * @hapd: Pointer to BSS data
 *
 * This function is used to free all per-BSS data structures and resources.
 * Most of the modules that are initialized in hostapd_setup_bss() are
 * deinitialized here.
 */
static void hostapd_cleanup(struct hostapd_data *hapd)
{
	wpa_printf(MSG_DEBUG, "%s(hapd=%p (%s))", __func__, hapd,
		   hapd->conf->iface);
	if (hapd->iface->interfaces &&
	    hapd->iface->interfaces->ctrl_iface_deinit)
		hapd->iface->interfaces->ctrl_iface_deinit(hapd);
	hostapd_free_hapd_data(hapd);
}


static void hostapd_cleanup_iface_partial(struct hostapd_iface *iface)
{
	wpa_printf(MSG_DEBUG, "%s(%p)", __func__, iface);
	hostapd_free_hw_features(iface->hw_features, iface->num_hw_features);
	iface->hw_features = NULL;
	os_free(iface->current_rates);
	iface->current_rates = NULL;
	os_free(iface->basic_rates);
	iface->basic_rates = NULL;
	ap_list_deinit(iface);
}


/**
 * hostapd_cleanup_iface - Complete per-interface cleanup
 * @iface: Pointer to interface data
 *
 * This function is called after per-BSS data structures are deinitialized
 * with hostapd_cleanup().
 */
static void hostapd_cleanup_iface(struct hostapd_iface *iface)
{
	wpa_printf(MSG_DEBUG, "%s(%p)", __func__, iface);
	eloop_cancel_timeout(channel_list_update_timeout, iface, NULL);

	hostapd_cleanup_iface_partial(iface);
	hostapd_config_free(iface->conf);
	iface->conf = NULL;

	os_free(iface->config_fname);
	os_free(iface->bss);
	wpa_printf(MSG_DEBUG, "%s: free iface=%p", __func__, iface);
	os_free(iface);
}


static void hostapd_clear_wep(struct hostapd_data *hapd)
{
	if (hapd->drv_priv) {
		hostapd_set_privacy(hapd, 0);
		hostapd_broadcast_wep_clear(hapd);
	}
}


static int hostapd_setup_encryption(char *iface, struct hostapd_data *hapd)
{
	int i;

	hostapd_broadcast_wep_set(hapd);

	if (hapd->conf->ssid.wep.default_len) {
		hostapd_set_privacy(hapd, 1);
		return 0;
	}

	/*
	 * When IEEE 802.1X is not enabled, the driver may need to know how to
	 * set authentication algorithms for static WEP.
	 */
	hostapd_drv_set_authmode(hapd, hapd->conf->auth_algs);

	for (i = 0; i < 4; i++) {
		if (hapd->conf->ssid.wep.key[i] &&
		    hostapd_drv_set_key(iface, hapd, WPA_ALG_WEP, NULL, i,
					i == hapd->conf->ssid.wep.idx, NULL, 0,
					hapd->conf->ssid.wep.key[i],
					hapd->conf->ssid.wep.len[i])) {
			wpa_printf(MSG_WARNING, "Could not set WEP "
				   "encryption.");
			return -1;
		}
		if (hapd->conf->ssid.wep.key[i] &&
		    i == hapd->conf->ssid.wep.idx)
			hostapd_set_privacy(hapd, 1);
	}

	return 0;
}


static int hostapd_flush_old_stations(struct hostapd_data *hapd, u16 reason)
{
	int ret = 0;
	u8 addr[ETH_ALEN];

	if (hostapd_drv_none(hapd) || hapd->drv_priv == NULL)
		return 0;

	wpa_dbg(hapd->msg_ctx, MSG_DEBUG, "Flushing old station entries");
	if (hostapd_flush(hapd)) {
		wpa_msg(hapd->msg_ctx, MSG_WARNING, "Could not connect to "
			"kernel driver");
		ret = -1;
	}
	wpa_dbg(hapd->msg_ctx, MSG_DEBUG, "Deauthenticate all stations");
	os_memset(addr, 0xff, ETH_ALEN);
	hostapd_drv_sta_deauth(hapd, addr, reason);
	hostapd_free_stas(hapd);

	return ret;
}


/**
 * hostapd_validate_bssid_configuration - Validate BSSID configuration
 * @iface: Pointer to interface data
 * Returns: 0 on success, -1 on failure
 *
 * This function is used to validate that the configured BSSIDs are valid.
 */
static int hostapd_validate_bssid_configuration(struct hostapd_iface *iface)
{
#ifdef CONFIG_BSSID_VALIDATION
	u8 mask[ETH_ALEN] = { 0 };
	struct hostapd_data *hapd = iface->bss[0];
	unsigned int i = iface->conf->num_bss, bits = 0, j;
	int auto_addr = 0;

	if (hostapd_drv_none(hapd))
		return 0;

	/* Generate BSSID mask that is large enough to cover the BSSIDs. */

	/* Determine the bits necessary to cover the number of BSSIDs. */
	for (i--; i; i >>= 1)
		bits++;

	/* Determine the bits necessary to any configured BSSIDs,
	   if they are higher than the number of BSSIDs. */
	for (j = 0; j < iface->conf->num_bss; j++) {
		if (hostapd_mac_comp_empty(iface->conf->bss[j]->bssid) == 0) {
			if (j)
				auto_addr++;
			continue;
		}

		for (i = 0; i < ETH_ALEN; i++) {
			mask[i] |=
				iface->conf->bss[j]->bssid[i] ^
				hapd->own_addr[i];
		}
	}

	if (!auto_addr)
		goto skip_mask_ext;

	for (i = 0; i < ETH_ALEN && mask[i] == 0; i++)
		;
	j = 0;
	if (i < ETH_ALEN) {
		j = (5 - i) * 8;

		while (mask[i] != 0) {
			mask[i] >>= 1;
			j++;
		}
	}

	if (bits < j)
		bits = j;

	if (bits > 40) {
		wpa_printf(MSG_ERROR, "Too many bits in the BSSID mask (%u)",
			   bits);
		return -1;
	}

	os_memset(mask, 0xff, ETH_ALEN);
	j = bits / 8;
	for (i = 5; i > 5 - j; i--)
		mask[i] = 0;
	j = bits % 8;
	while (j--)
		mask[i] <<= 1;

skip_mask_ext:
	wpa_printf(MSG_DEBUG, "BSS count %lu, BSSID mask " MACSTR " (%d bits)",
		   (unsigned long) iface->conf->num_bss, MAC2STR(mask), bits);

	if (!auto_addr)
		return 0;

	for (i = 0; i < ETH_ALEN; i++) {
		if ((hapd->own_addr[i] & mask[i]) != hapd->own_addr[i]) {
			wpa_printf(MSG_ERROR, "Invalid BSSID mask " MACSTR
				   " for start address " MACSTR ".",
				   MAC2STR(mask), MAC2STR(hapd->own_addr));
			wpa_printf(MSG_ERROR, "Start address must be the "
				   "first address in the block (i.e., addr "
				   "AND mask == addr).");
			return -1;
		}
	}

#endif
	return 0;
}


static int mac_in_conf(struct hostapd_config *conf, const void *a)
{
	size_t i;

	for (i = 0; i < conf->num_bss; i++) {
		if (hostapd_mac_comp(conf->bss[i]->bssid, a) == 0) {
			return 1;
		}
	}

	return 0;
}


#ifndef CONFIG_NO_RADIUS

static int hostapd_das_nas_mismatch(struct hostapd_data *hapd,
				    struct radius_das_attrs *attr)
{
	/* TODO */
	return 0;
}


static struct sta_info * hostapd_das_find_sta(struct hostapd_data *hapd,
					      struct radius_das_attrs *attr)
{
	struct sta_info *sta = NULL;
	char buf[128];

	if (attr->sta_addr)
		sta = ap_get_sta(hapd, attr->sta_addr);

	if (sta == NULL && attr->acct_session_id &&
	    attr->acct_session_id_len == 17) {
		for (sta = hapd->sta_list; sta; sta = sta->next) {
			os_snprintf(buf, sizeof(buf), "%08X-%08X",
				    sta->acct_session_id_hi,
				    sta->acct_session_id_lo);
			if (os_memcmp(attr->acct_session_id, buf, 17) == 0)
				break;
		}
	}

	if (sta == NULL && attr->cui) {
		for (sta = hapd->sta_list; sta; sta = sta->next) {
			struct wpabuf *cui;
			cui = ieee802_1x_get_radius_cui(sta->eapol_sm);
			if (cui && wpabuf_len(cui) == attr->cui_len &&
			    os_memcmp(wpabuf_head(cui), attr->cui,
				      attr->cui_len) == 0)
				break;
		}
	}

	if (sta == NULL && attr->user_name) {
		for (sta = hapd->sta_list; sta; sta = sta->next) {
			u8 *identity;
			size_t identity_len;
			identity = ieee802_1x_get_identity(sta->eapol_sm,
							   &identity_len);
			if (identity &&
			    identity_len == attr->user_name_len &&
			    os_memcmp(identity, attr->user_name, identity_len)
			    == 0)
				break;
		}
	}

	return sta;
}


static enum radius_das_res
hostapd_das_disconnect(void *ctx, struct radius_das_attrs *attr)
{
	struct hostapd_data *hapd = ctx;
	struct sta_info *sta;

	if (hostapd_das_nas_mismatch(hapd, attr))
		return RADIUS_DAS_NAS_MISMATCH;

	sta = hostapd_das_find_sta(hapd, attr);
	if (sta == NULL)
		return RADIUS_DAS_SESSION_NOT_FOUND;

	hostapd_drv_sta_deauth(hapd, sta->addr,
			       WLAN_REASON_PREV_AUTH_NOT_VALID);
	ap_sta_deauthenticate(hapd, sta, WLAN_REASON_PREV_AUTH_NOT_VALID);

	return RADIUS_DAS_SUCCESS;
}

#endif /* CONFIG_NO_RADIUS */


/**
 * hostapd_setup_bss - Per-BSS setup (initialization)
 * @hapd: Pointer to BSS data
 * @first: Whether this BSS is the first BSS of an interface; -1 = not first,
 *	but interface may exist
 *
 * This function is used to initialize all per-BSS data structures and
 * resources. This gets called in a loop for each BSS when an interface is
 * initialized. Most of the modules that are initialized here will be
 * deinitialized in hostapd_cleanup().
 */
static int hostapd_setup_bss(struct hostapd_data *hapd, int first)
{
	struct hostapd_bss_config *conf = hapd->conf;
	u8 ssid[HOSTAPD_MAX_SSID_LEN + 1];
	int ssid_len, set_ssid;
	char force_ifname[IFNAMSIZ];
	u8 if_addr[ETH_ALEN] = {0};

	wpa_printf(MSG_DEBUG, "%s(hapd=%p (%s), first=%d)",
		   __func__, hapd, hapd->conf->iface, first);

	if (hapd->started) {
		wpa_printf(MSG_ERROR, "%s: Interface %s was already started",
			   __func__, hapd->conf->iface);
		return -1;
	}
	hapd->started = 1;

	if (!first || first == -1) {
		if (hostapd_mac_comp_empty(hapd->conf->bssid) == 0) {
			/* Don't generate one available BSSID, driver will take care of it. */
			os_memcpy(hapd->own_addr, hapd->conf->bssid, ETH_ALEN);
		} else {
			/* Allocate the configured BSSID. */
			os_memcpy(hapd->own_addr, hapd->conf->bssid, ETH_ALEN);

			if (hostapd_mac_comp(hapd->own_addr,
					     hapd->iface->bss[0]->own_addr) ==
			    0) {
				wpa_printf(MSG_ERROR, "BSS '%s' may not have "
					   "BSSID set to the MAC address of "
					   "the radio", hapd->conf->iface);
				return -1;
			}
		}

		hapd->interface_added = 1;
		if (hostapd_if_add(hapd->iface->bss[0], WPA_IF_AP_BSS,
				   hapd->conf->iface, hapd->own_addr, hapd,
				   &hapd->drv_priv, force_ifname, if_addr,
				   hapd->conf->bridge[0] ? hapd->conf->bridge :
				   NULL, first == -1)) {
			wpa_printf(MSG_ERROR, "Failed to add BSS (BSSID="
				   MACSTR ")", MAC2STR(hapd->own_addr));
			hapd->interface_added = 0;
			return -1;
		}

		if (hostapd_mac_comp_empty(if_addr) != 0) {
			/* Driver returned the if address */
			os_memcpy(hapd->own_addr, if_addr, ETH_ALEN);
		}
	}

	if (conf->wmm_enabled < 0)
		conf->wmm_enabled = hapd->iconf->ieee80211n;

	hostapd_flush_old_stations(hapd, WLAN_REASON_PREV_AUTH_NOT_VALID);
	hostapd_set_privacy(hapd, 0);

	hostapd_broadcast_wep_clear(hapd);
	if (hostapd_setup_encryption(hapd->conf->iface, hapd))
		return -1;

	/*
	 * Fetch the SSID from the system and use it or,
	 * if one was specified in the config file, verify they
	 * match.
	 */
	ssid_len = hostapd_get_ssid(hapd, ssid, sizeof(ssid));
	if (ssid_len < 0) {
		wpa_printf(MSG_ERROR, "Could not read SSID from system");
		return -1;
	}
	if (conf->ssid.ssid_set) {
		/*
		 * If SSID is specified in the config file and it differs
		 * from what is being used then force installation of the
		 * new SSID.
		 */
		set_ssid = (conf->ssid.ssid_len != (size_t) ssid_len ||
			    os_memcmp(conf->ssid.ssid, ssid, ssid_len) != 0);
	} else {
		/*
		 * No SSID in the config file; just use the one we got
		 * from the system.
		 */
		set_ssid = 0;
		conf->ssid.ssid_len = ssid_len;
		os_memcpy(conf->ssid.ssid, ssid, conf->ssid.ssid_len);
	}

	if (!hostapd_drv_none(hapd)) {
		wpa_printf(MSG_ERROR, "Using interface %s with hwaddr " MACSTR
			   " and ssid \"%s\"",
			   hapd->conf->iface, MAC2STR(hapd->own_addr),
			   wpa_ssid_txt(hapd->conf->ssid.ssid,
					hapd->conf->ssid.ssid_len));
	}

	if (hostapd_setup_wpa_psk(conf)) {
		wpa_printf(MSG_ERROR, "WPA-PSK setup failed.");
		return -1;
	}

	/* Set SSID for the kernel driver (to be used in beacon and probe
	 * response frames) */
	if (set_ssid && hostapd_set_ssid(hapd, conf->ssid.ssid,
					 conf->ssid.ssid_len)) {
		wpa_printf(MSG_ERROR, "Could not set SSID for kernel driver");
		return -1;
	}

        if (conf->set_assoc_limit_required &&
                        hostapd_set_bss_assoc_limit(hapd, conf->max_num_sta)) {
                wpa_printf(MSG_ERROR, "Could not set max_num_sta for kernel driver");
                return -1;
        }

	if (wpa_debug_level == MSG_MSGDUMP)
		conf->radius->msg_dumps = 1;
#ifndef CONFIG_NO_RADIUS
	hapd->radius = radius_client_init(hapd, conf->radius);
	if (hapd->radius == NULL) {
		wpa_printf(MSG_ERROR, "RADIUS client initialization failed.");
		return -1;
	}

	if (hapd->conf->radius_das_port) {
		struct radius_das_conf das_conf;
		os_memset(&das_conf, 0, sizeof(das_conf));
		das_conf.port = hapd->conf->radius_das_port;
		das_conf.shared_secret = hapd->conf->radius_das_shared_secret;
		das_conf.shared_secret_len =
			hapd->conf->radius_das_shared_secret_len;
		das_conf.client_addr = &hapd->conf->radius_das_client_addr;
		das_conf.time_window = hapd->conf->radius_das_time_window;
		das_conf.require_event_timestamp =
			hapd->conf->radius_das_require_event_timestamp;
		das_conf.ctx = hapd;
		das_conf.disconnect = hostapd_das_disconnect;
		hapd->radius_das = radius_das_init(&das_conf);
		if (hapd->radius_das == NULL) {
			wpa_printf(MSG_ERROR, "RADIUS DAS initialization "
				   "failed.");
			return -1;
		}
	}
#endif /* CONFIG_NO_RADIUS */

	if (hostapd_acl_init(hapd)) {
		wpa_printf(MSG_ERROR, "ACL initialization failed.");
		return -1;
	}

	if (conf->wps_lockdown == WPS_LOCKDOWN_AUTO) {
		hapd->auto_ld.force_ap_setup_locked = conf->ap_setup_locked;
		hapd->auto_ld.max_fail_retry = conf->auto_ld_max_retry;
		hapd->auto_ld.fail_count = 0;
	} else if (conf->wps_lockdown == WPS_LOCKDOWN_DEFAULT) {
		hapd->ap_pin_failures = 0;
		hapd->ap_pin_failures_consecutive = 0;
	}
	hapd->current_wps_lockdown = conf->wps_lockdown;

	if (hostapd_init_wps(hapd, conf))
		return -1;

	if (authsrv_init(hapd) < 0)
		return -1;

	if (ieee802_1x_init(hapd)) {
		wpa_printf(MSG_ERROR, "IEEE 802.1X initialization failed.");
		return -1;
	}

	if ((hapd->conf->wpa || hapd->conf->osen) && hostapd_setup_wpa(hapd))
		return -1;

	if (accounting_init(hapd)) {
		wpa_printf(MSG_ERROR, "Accounting initialization failed.");
		return -1;
	}

	if (hapd->conf->ieee802_11f &&
	    (hapd->iapp = iapp_init(hapd, hapd->conf->iapp_iface)) == NULL) {
		wpa_printf(MSG_ERROR, "IEEE 802.11F (IAPP) initialization "
			   "failed.");
		return -1;
	}

        if ((hapd->scs = hostapd_scs_init(hapd, hapd->conf->iface)) == NULL) {
                wpa_printf(MSG_ERROR, "SCS initialization "
                           "failed.");
                return -1;
        }

#ifdef CONFIG_INTERWORKING
	if (gas_serv_init(hapd)) {
		wpa_printf(MSG_ERROR, "GAS server initialization failed");
		return -1;
	}

	if (conf->qos_map_set_len &&
	    hostapd_drv_set_qos_map(hapd, conf->qos_map_set,
				    conf->qos_map_set_len)) {
		wpa_printf(MSG_ERROR, "Failed to initialize QoS Map");
		return -1;
	}
#endif /* CONFIG_INTERWORKING */

	if (!hostapd_drv_none(hapd) && vlan_init(hapd)) {
		wpa_printf(MSG_ERROR, "VLAN initialization failed.");
		return -1;
	}

	if (!hapd->conf->start_disabled && ieee802_11_set_beacon(hapd) < 0)
		return -1;

	/* Enable or disable SSID broadcast in beacons */
	if (hostapd_set_broadcast_ssid(hapd,
			!!hapd->conf->ignore_broadcast_ssid)) {
		wpa_printf(MSG_WARNING, "Could not modify broadcast SSID flag");
	}

	if (hapd->wpa_auth && wpa_init_keys(hapd->wpa_auth) < 0)
		return -1;

	if (hapd->driver && hapd->driver->set_operstate)
		hapd->driver->set_operstate(hapd->drv_priv, 1);

	if (qtn_hapd_pp2_setup(hapd) < 0)
		return -1;

	/* by default
	 * 1. pbc detect enhancement is enabled unless it's disabled in conf.
	 * 2. first EAPOL-Start package should be 0.5 second after association.
	 * 3. when firt EAPOL-Start appear earlier than expect, delay the handling function for 0.8 second.
	 */

	hapd->pbc_detect_enhance = conf->pbc_detect_enhance;
	hapd->pbc_detect_interval = 500000;
	hapd->eapol_resp_delay_s = 0;
	hapd->eapol_resp_delay_us = 800000;

	/* to be informed of probes */
	hostapd_register_probereq_cb(hapd, hostapd_probe_req_event, hapd);

	return 0;
}


static void hostapd_tx_queue_params(struct hostapd_iface *iface)
{
	struct hostapd_data *hapd = iface->bss[0];
	int i;
	struct hostapd_tx_queue_params *p;

	for (i = 0; i < NUM_TX_QUEUES; i++) {
		p = &iface->conf->tx_queue[i];

		if (hostapd_set_tx_queue_params(hapd, i, p->aifs, p->cwmin,
						p->cwmax, p->burst)) {
			wpa_printf(MSG_DEBUG, "Failed to set TX queue "
				   "parameters for queue %d.", i);
			/* Continue anyway */
		}
	}
}


static int hostapd_set_acl_list(struct hostapd_data *hapd,
				struct mac_acl_entry *mac_acl,
				int n_entries, u8 accept_acl)
{
	struct hostapd_acl_params *acl_params;
	int i, err;

	acl_params = os_zalloc(sizeof(*acl_params) +
			       (n_entries * sizeof(acl_params->mac_acl[0])));
	if (!acl_params)
		return -ENOMEM;

	for (i = 0; i < n_entries; i++)
		os_memcpy(acl_params->mac_acl[i].addr, mac_acl[i].addr,
			  ETH_ALEN);

	acl_params->acl_policy = accept_acl;
	acl_params->num_mac_acl = n_entries;

	err = hostapd_drv_set_acl(hapd, acl_params);

	os_free(acl_params);

	return err;
}


static void hostapd_set_acl(struct hostapd_data *hapd)
{
	struct hostapd_config *conf = hapd->iconf;
	int err;
	u8 accept_acl;

	if (hapd->iface->drv_max_acl_mac_addrs == 0)
		return;
	if (!(conf->bss[0]->num_accept_mac || conf->bss[0]->num_deny_mac))
		return;

	if (conf->bss[0]->macaddr_acl == DENY_UNLESS_ACCEPTED) {
		if (conf->bss[0]->num_accept_mac) {
			accept_acl = 1;
			err = hostapd_set_acl_list(hapd,
						   conf->bss[0]->accept_mac,
						   conf->bss[0]->num_accept_mac,
						   accept_acl);
			if (err) {
				wpa_printf(MSG_DEBUG, "Failed to set accept acl");
				return;
			}
		} else {
			wpa_printf(MSG_DEBUG, "Mismatch between ACL Policy & Accept/deny lists file");
		}
	} else if (conf->bss[0]->macaddr_acl == ACCEPT_UNLESS_DENIED) {
		if (conf->bss[0]->num_deny_mac) {
			accept_acl = 0;
			err = hostapd_set_acl_list(hapd, conf->bss[0]->deny_mac,
						   conf->bss[0]->num_deny_mac,
						   accept_acl);
			if (err) {
				wpa_printf(MSG_DEBUG, "Failed to set deny acl");
				return;
			}
		} else {
			wpa_printf(MSG_DEBUG, "Mismatch between ACL Policy & Accept/deny lists file");
		}
	}
}


static int start_ctrl_iface_bss(struct hostapd_data *hapd)
{
	if (!hapd->iface->interfaces ||
	    !hapd->iface->interfaces->ctrl_iface_init)
		return 0;

	if (hapd->iface->interfaces->ctrl_iface_init(hapd)) {
		wpa_printf(MSG_ERROR,
			   "Failed to setup control interface for %s",
			   hapd->conf->iface);
		return -1;
	}

	return 0;
}


static int start_ctrl_iface(struct hostapd_iface *iface)
{
	size_t i;

	if (!iface->interfaces || !iface->interfaces->ctrl_iface_init)
		return 0;

	for (i = 0; i < iface->num_bss; i++) {
		struct hostapd_data *hapd = iface->bss[i];
		if (iface->interfaces->ctrl_iface_init(hapd)) {
			wpa_printf(MSG_ERROR,
				   "Failed to setup control interface for %s",
				   hapd->conf->iface);
			return -1;
		}
	}

	return 0;
}


static void channel_list_update_timeout(void *eloop_ctx, void *timeout_ctx)
{
	struct hostapd_iface *iface = eloop_ctx;

	if (!iface->wait_channel_update) {
		wpa_printf(MSG_INFO, "Channel list update timeout, but interface was not waiting for it");
		return;
	}

	/*
	 * It is possible that the existing channel list is acceptable, so try
	 * to proceed.
	 */
	wpa_printf(MSG_DEBUG, "Channel list update timeout - try to continue anyway");
	setup_interface2(iface);
}


void hostapd_channel_list_updated(struct hostapd_iface *iface, int initiator)
{
	if (!iface->wait_channel_update || initiator != REGDOM_SET_BY_USER)
		return;

	wpa_printf(MSG_DEBUG, "Channel list updated - continue setup");
	eloop_cancel_timeout(channel_list_update_timeout, iface, NULL);
	setup_interface2(iface);
}


static int setup_interface(struct hostapd_iface *iface)
{
	struct hostapd_data *hapd = iface->bss[0];
	size_t i;

	/* Mark first bss as primary */
	hapd->primary_interface = 1;
	iface->default_pbc_bss = hapd;

	if (!iface->phy[0]) {
		const char *phy = hostapd_drv_get_radio_name(hapd);
		if (phy) {
			wpa_printf(MSG_DEBUG, "phy: %s", phy);
			os_strlcpy(iface->phy, phy, sizeof(iface->phy));
		}
	}

	/*
	 * Make sure that all BSSes get configured with a pointer to the same
	 * driver interface.
	 */
	for (i = 1; i < iface->num_bss; i++) {
		iface->bss[i]->driver = hapd->driver;
		iface->bss[i]->drv_priv = hapd->drv_priv;
	}

	if (hostapd_validate_bssid_configuration(iface))
		return -1;

	/*
	 * Initialize control interfaces early to allow external monitoring of
	 * channel setup operations that may take considerable amount of time
	 * especially for DFS cases.
	 */
	if (start_ctrl_iface(iface))
		return -1;

	if (hapd->iconf->country[0] && hapd->iconf->country[1]) {
		char country[4], previous_country[4];

		hostapd_set_state(iface, HAPD_IFACE_COUNTRY_UPDATE);
		if (hostapd_get_country(hapd, previous_country) < 0)
			previous_country[0] = '\0';

		os_memcpy(country, hapd->iconf->country, 3);
		country[3] = '\0';
		if (hostapd_set_country(hapd, country) < 0) {
			wpa_printf(MSG_ERROR, "Failed to set country code");
			return -1;
		}

		wpa_printf(MSG_DEBUG, "Previous country code %s, new country code %s",
			   previous_country, country);

		if (os_strncmp(previous_country, country, 2) != 0) {
			wpa_printf(MSG_DEBUG, "Continue interface setup after channel list update");
			iface->wait_channel_update = 1;
			eloop_register_timeout(5, 0,
					       channel_list_update_timeout,
					       iface, NULL);
			return 0;
		}
	}

	return setup_interface2(iface);
}


static int setup_interface2(struct hostapd_iface *iface)
{
	iface->wait_channel_update = 0;

	if (hostapd_get_hw_features(iface)) {
		/* Not all drivers support this yet, so continue without hw
		 * feature data. */
	} else {
		int ret = hostapd_select_hw_mode(iface);
		if (ret < 0) {
			wpa_printf(MSG_ERROR, "Could not select hw_mode and "
				   "channel. (%d)", ret);
			return -1;
		}
		if (ret == 1) {
			wpa_printf(MSG_DEBUG, "Interface initialization will be completed in a callback (ACS)");
			return 0;
		}
		ret = hostapd_check_ht_capab(iface);
		if (ret < 0)
			return -1;
		if (ret == 1) {
			wpa_printf(MSG_DEBUG, "Interface initialization will "
				   "be completed in a callback");
			return 0;
		}

		if (iface->conf->ieee80211h)
			wpa_printf(MSG_DEBUG, "DFS support is enabled");
	}
	return hostapd_setup_interface_complete(iface, 0);
}


/**
 * hostapd_setup_interface_complete - Complete interface setup
 *
 * This function is called when previous steps in the interface setup has been
 * completed. This can also start operations, e.g., DFS, that will require
 * additional processing before interface is ready to be enabled. Such
 * operations will call this function from eloop callbacks when finished.
 */
int hostapd_setup_interface_complete(struct hostapd_iface *iface, int err)
{
	struct hostapd_data *hapd = iface->bss[0];
	size_t j;
	u8 *prev_addr;

	if (err) {
		wpa_printf(MSG_ERROR, "Interface initialization failed");
		hostapd_set_state(iface, HAPD_IFACE_DISABLED);
		if (iface->interfaces && iface->interfaces->terminate_on_error)
			eloop_terminate();
		return -1;
	}

	wpa_printf(MSG_DEBUG, "Completing interface initialization");
	if (iface->conf->channel) {
#ifdef NEED_AP_MLME
		int res;
#endif /* NEED_AP_MLME */

		iface->freq = hostapd_hw_get_freq(hapd, iface->conf->channel);
		wpa_printf(MSG_DEBUG, "Mode: %s  Channel: %d  "
			   "Frequency: %d MHz",
			   hostapd_hw_mode_txt(iface->conf->hw_mode),
			   iface->conf->channel, iface->freq);

#ifdef NEED_AP_MLME
		/* Check DFS */
		res = hostapd_handle_dfs(iface);
		if (res <= 0)
			return res;
#endif /* NEED_AP_MLME */

		if (hostapd_set_freq(hapd, hapd->iconf->hw_mode, iface->freq,
				     hapd->iconf->channel,
				     hapd->iconf->ieee80211n,
				     hapd->iconf->ieee80211ac,
				     hapd->iconf->secondary_channel,
				     hapd->iconf->vht_oper_chwidth,
				     hapd->iconf->vht_oper_centr_freq_seg0_idx,
				     hapd->iconf->vht_oper_centr_freq_seg1_idx)) {
			wpa_printf(MSG_ERROR, "Could not set channel for "
				   "kernel driver");
			return -1;
		}
	}

	if (iface->current_mode) {
		if (hostapd_prepare_rates(iface, iface->current_mode)) {
			wpa_printf(MSG_ERROR, "Failed to prepare rates "
				   "table.");
			hostapd_logger(hapd, NULL, HOSTAPD_MODULE_IEEE80211,
				       HOSTAPD_LEVEL_WARNING,
				       "Failed to prepare rates table.");
			return -1;
		}
	}

	if (hapd->iconf->rts_threshold > -1 &&
	    hostapd_set_rts(hapd, hapd->iconf->rts_threshold)) {
		wpa_printf(MSG_ERROR, "Could not set RTS threshold for "
			   "kernel driver");
		return -1;
	}

	if (hapd->iconf->fragm_threshold > -1 &&
	    hostapd_set_frag(hapd, hapd->iconf->fragm_threshold)) {
		wpa_printf(MSG_ERROR, "Could not set fragmentation threshold "
			   "for kernel driver");
		return -1;
	}

        if (hapd->iconf->total_assoc_limit >= 0 &&
                        hapd->iconf->total_assoc_limit <= MAX_STA_COUNT) {
                if (hostapd_set_total_assoc_limit(hapd, hapd->iconf->total_assoc_limit)) {
                        wpa_printf(MSG_ERROR, "Could not set total_assoc_limit "
                                   "for kernel driver");
                        return -1;
                }
        }

	prev_addr = hapd->own_addr;

	for (j = 0; j < iface->num_bss; j++) {
		hapd = iface->bss[j];
		if (j)
			os_memcpy(hapd->own_addr, prev_addr, ETH_ALEN);
		if (hostapd_setup_bss(hapd, j == 0))
			return -1;
		if (hostapd_mac_comp_empty(hapd->conf->bssid) == 0)
			prev_addr = hapd->own_addr;
	}
	hapd = iface->bss[0];

	hostapd_tx_queue_params(iface);

	ap_list_init(iface);

	hostapd_set_acl(hapd);

	for (j = 0; j < iface->num_bss; j++) {
		hapd = iface->bss[j];
		if (hostapd_driver_commit(hapd) < 0) {
			wpa_printf(MSG_ERROR, "%s: Failed to commit driver "
				   "configuration", __func__);
			return -1;
		}
	}

	/*
	 * WPS UPnP module can be initialized only when the "upnp_iface" is up.
	 * If "interface" and "upnp_iface" are the same (e.g., non-bridge
	 * mode), the interface is up only after driver_commit, so initialize
	 * WPS after driver_commit.
	 */
	for (j = 0; j < iface->num_bss; j++) {
		if (hostapd_init_wps_complete(iface->bss[j]))
			return -1;
	}

	hostapd_set_state(iface, HAPD_IFACE_ENABLED);
	wpa_msg(iface->bss[0]->msg_ctx, MSG_INFO, AP_EVENT_ENABLED);
	if (hapd->setup_complete_cb)
		hapd->setup_complete_cb(hapd->setup_complete_cb_ctx);

	wpa_printf(MSG_DEBUG, "%s: Setup of interface done.",
		   iface->bss[0]->conf->iface);
	if (iface->interfaces && iface->interfaces->terminate_on_error > 0)
		iface->interfaces->terminate_on_error--;

	return 0;
}


/**
 * hostapd_setup_interface - Setup of an interface
 * @iface: Pointer to interface data.
 * Returns: 0 on success, -1 on failure
 *
 * Initializes the driver interface, validates the configuration,
 * and sets driver parameters based on the configuration.
 * Flushes old stations, sets the channel, encryption,
 * beacons, and WDS links based on the configuration.
 *
 * If interface setup requires more time, e.g., to perform HT co-ex scans, ACS,
 * or DFS operations, this function returns 0 before such operations have been
 * completed. The pending operations are registered into eloop and will be
 * completed from eloop callbacks. Those callbacks end up calling
 * hostapd_setup_interface_complete() once setup has been completed.
 */
int hostapd_setup_interface(struct hostapd_iface *iface)
{
	int ret;

	ret = setup_interface(iface);
	if (ret) {
		wpa_printf(MSG_ERROR, "%s: Unable to setup interface.",
			   iface->bss[0]->conf->iface);
		return -1;
	}

	return 0;
}


/**
 * hostapd_alloc_bss_data - Allocate and initialize per-BSS data
 * @hapd_iface: Pointer to interface data
 * @conf: Pointer to per-interface configuration
 * @bss: Pointer to per-BSS configuration for this BSS
 * Returns: Pointer to allocated BSS data
 *
 * This function is used to allocate per-BSS data structure. This data will be
 * freed after hostapd_cleanup() is called for it during interface
 * deinitialization.
 */
struct hostapd_data *
hostapd_alloc_bss_data(struct hostapd_iface *hapd_iface,
		       struct hostapd_config *conf,
		       struct hostapd_bss_config *bss)
{
	struct hostapd_data *hapd;

	hapd = os_zalloc(sizeof(*hapd));
	if (hapd == NULL)
		return NULL;

	hapd->new_assoc_sta_cb = hostapd_new_assoc_sta;
	hapd->iconf = conf;
	hapd->conf = bss;
	hapd->iface = hapd_iface;
	hapd->driver = hapd->iconf->driver;
	hapd->ctrl_sock = -1;

	return hapd;
}


static void hostapd_bss_deinit(struct hostapd_data *hapd)
{
	wpa_printf(MSG_DEBUG, "%s: deinit bss %s", __func__,
		   hapd->conf->iface);
	hostapd_free_stas(hapd);
	hostapd_flush_old_stations(hapd, WLAN_REASON_DEAUTH_LEAVING);
	hostapd_clear_wep(hapd);
	hostapd_cleanup(hapd);
}


void hostapd_interface_deinit(struct hostapd_iface *iface)
{
	int j;

	wpa_printf(MSG_DEBUG, "%s(%p)", __func__, iface);
	if (iface == NULL)
		return;

	eloop_cancel_timeout(channel_list_update_timeout, iface, NULL);
	iface->wait_channel_update = 0;

	for (j = iface->num_bss - 1; j >= 0; j--)
		hostapd_bss_deinit(iface->bss[j]);
}


void hostapd_interface_free(struct hostapd_iface *iface)
{
	size_t j;
	wpa_printf(MSG_DEBUG, "%s(%p)", __func__, iface);
	for (j = 0; j < iface->num_bss; j++) {
		wpa_printf(MSG_DEBUG, "%s: free hapd %p",
			   __func__, iface->bss[j]);
		os_free(iface->bss[j]);
	}
	hostapd_cleanup_iface(iface);
}


/**
 * hostapd_init - Allocate and initialize per-interface data
 * @config_file: Path to the configuration file
 * Returns: Pointer to the allocated interface data or %NULL on failure
 *
 * This function is used to allocate main data structures for per-interface
 * data. The allocated data buffer will be freed by calling
 * hostapd_cleanup_iface().
 */
struct hostapd_iface * hostapd_init(struct hapd_interfaces *interfaces,
				    const char *config_file)
{
	struct hostapd_iface *hapd_iface = NULL;
	struct hostapd_config *conf = NULL;
	struct hostapd_data *hapd;
	size_t i;

	hapd_iface = os_zalloc(sizeof(*hapd_iface));
	if (hapd_iface == NULL)
		goto fail;

	hapd_iface->config_fname = os_strdup(config_file);
	if (hapd_iface->config_fname == NULL)
		goto fail;

	conf = interfaces->config_read_cb(hapd_iface->config_fname);
	if (conf == NULL)
		goto fail;
	hapd_iface->conf = conf;

	hapd_iface->num_bss = conf->num_bss;
	hapd_iface->bss = os_calloc(MAX_BSSID,
				    sizeof(struct hostapd_data *));
	if (hapd_iface->bss == NULL)
		goto fail;

	for (i = 0; i < conf->num_bss; i++) {
		hapd = hapd_iface->bss[i] =
			hostapd_alloc_bss_data(hapd_iface, conf,
					       conf->bss[i]);
		if (hapd == NULL)
			goto fail;
		hapd->msg_ctx = hapd;
	}

        hapd_iface->scs_brcm_sock = -1;
        hapd_iface->scs_ioctl_sock = -1;

	return hapd_iface;

fail:
	wpa_printf(MSG_ERROR, "Failed to set up interface with %s",
		   config_file);
	if (conf)
		hostapd_config_free(conf);
	if (hapd_iface) {
		os_free(hapd_iface->config_fname);
		os_free(hapd_iface->bss);
		wpa_printf(MSG_DEBUG, "%s: free iface %p",
			   __func__, hapd_iface);
		os_free(hapd_iface);
	}
	return NULL;
}


static int ifname_in_use(struct hapd_interfaces *interfaces, const char *ifname)
{
	size_t i, j;

	for (i = 0; i < interfaces->count; i++) {
		struct hostapd_iface *iface = interfaces->iface[i];
		for (j = 0; j < iface->num_bss; j++) {
			struct hostapd_data *hapd = iface->bss[j];
			if (os_strcmp(ifname, hapd->conf->iface) == 0)
				return 1;
		}
	}

	return 0;
}


/**
 * hostapd_interface_init_bss - Read configuration file and init BSS data
 *
 * This function is used to parse configuration file for a BSS. This BSS is
 * added to an existing interface sharing the same radio (if any) or a new
 * interface is created if this is the first interface on a radio. This
 * allocate memory for the BSS. No actual driver operations are started.
 *
 * This is similar to hostapd_interface_init(), but for a case where the
 * configuration is used to add a single BSS instead of all BSSes for a radio.
 */
struct hostapd_iface *
hostapd_interface_init_bss(struct hapd_interfaces *interfaces, const char *phy,
			   const char *config_fname, int debug)
{
	struct hostapd_iface *new_iface = NULL, *iface = NULL;
	struct hostapd_data *hapd;
	int k;
	size_t i, bss_idx;

	if (!phy || !*phy)
		return NULL;

	for (i = 0; i < interfaces->count; i++) {
		if (os_strcmp(interfaces->iface[i]->phy, phy) == 0) {
			iface = interfaces->iface[i];
			break;
		}
	}

	wpa_printf(MSG_INFO, "Configuration file: %s (phy %s)%s",
		   config_fname, phy, iface ? "" : " --> new PHY");
	if (iface) {
		struct hostapd_config *conf;
		struct hostapd_bss_config **tmp_conf;
		struct hostapd_data **tmp_bss;
		struct hostapd_bss_config *bss;
		const char *ifname;

		/* Add new BSS to existing iface */
		conf = interfaces->config_read_cb(config_fname);
		if (conf == NULL)
			return NULL;
		if (conf->num_bss > 1) {
			wpa_printf(MSG_ERROR, "Multiple BSSes specified in BSS-config");
			hostapd_config_free(conf);
			return NULL;
		}

		ifname = conf->bss[0]->iface;
		if (ifname[0] != '\0' && ifname_in_use(interfaces, ifname)) {
			wpa_printf(MSG_ERROR,
				   "Interface name %s already in use", ifname);
			hostapd_config_free(conf);
			return NULL;
		}

		tmp_conf = os_realloc_array(
			iface->conf->bss, iface->conf->num_bss + 1,
			sizeof(struct hostapd_bss_config *));
		tmp_bss = os_realloc_array(iface->bss, iface->num_bss + 1,
					   sizeof(struct hostapd_data *));
		if (tmp_bss)
			iface->bss = tmp_bss;
		if (tmp_conf) {
			iface->conf->bss = tmp_conf;
			iface->conf->last_bss = tmp_conf[0];
		}
		if (tmp_bss == NULL || tmp_conf == NULL) {
			hostapd_config_free(conf);
			return NULL;
		}
		bss = iface->conf->bss[iface->conf->num_bss] = conf->bss[0];
		iface->conf->num_bss++;

		hapd = hostapd_alloc_bss_data(iface, iface->conf, bss);
		if (hapd == NULL) {
			iface->conf->num_bss--;
			hostapd_config_free(conf);
			return NULL;
		}
		iface->conf->last_bss = bss;
		iface->bss[iface->num_bss] = hapd;
		hapd->msg_ctx = hapd;

		bss_idx = iface->num_bss++;
		conf->num_bss--;
		conf->bss[0] = NULL;
		hostapd_config_free(conf);
	} else {
		/* Add a new iface with the first BSS */
		new_iface = iface = hostapd_init(interfaces, config_fname);
		if (!iface)
			return NULL;
		os_strlcpy(iface->phy, phy, sizeof(iface->phy));
		iface->interfaces = interfaces;
		bss_idx = 0;
	}

	for (k = 0; k < debug; k++) {
		if (iface->bss[bss_idx]->conf->logger_stdout_level > 0)
			iface->bss[bss_idx]->conf->logger_stdout_level--;
	}

	if (iface->conf->bss[bss_idx]->iface[0] == '\0' &&
	    !hostapd_drv_none(iface->bss[bss_idx])) {
		wpa_printf(MSG_ERROR, "Interface name not specified in %s",
			   config_fname);
		if (new_iface)
			hostapd_interface_deinit_free(new_iface);
		return NULL;
	}

	return iface;
}


void hostapd_interface_deinit_free(struct hostapd_iface *iface)
{
	const struct wpa_driver_ops *driver;
	void *drv_priv;

	wpa_printf(MSG_DEBUG, "%s(%p)", __func__, iface);
	if (iface == NULL)
		return;
	wpa_printf(MSG_DEBUG, "%s: num_bss=%u conf->num_bss=%u",
		   __func__, (unsigned int) iface->num_bss,
		   (unsigned int) iface->conf->num_bss);
	driver = iface->bss[0]->driver;
	drv_priv = iface->bss[0]->drv_priv;
	hostapd_interface_deinit(iface);
	wpa_printf(MSG_DEBUG, "%s: driver=%p drv_priv=%p -> hapd_deinit",
		   __func__, driver, drv_priv);
	if (driver && driver->hapd_deinit && drv_priv)
		driver->hapd_deinit(drv_priv);
	hostapd_interface_free(iface);
}


int hostapd_enable_iface(struct hostapd_iface *hapd_iface)
{
	if (hapd_iface->bss[0]->drv_priv != NULL) {
		wpa_printf(MSG_ERROR, "Interface %s already enabled",
			   hapd_iface->conf->bss[0]->iface);
		return -1;
	}

	wpa_printf(MSG_DEBUG, "Enable interface %s",
		   hapd_iface->conf->bss[0]->iface);

	if (hostapd_config_check(hapd_iface->conf, 1) < 0) {
		wpa_printf(MSG_INFO, "Invalid configuration - cannot enable");
		return -1;
	}

	if (hapd_iface->interfaces == NULL ||
	    hapd_iface->interfaces->driver_init == NULL ||
	    hapd_iface->interfaces->driver_init(hapd_iface))
		return -1;

	if (hostapd_setup_interface(hapd_iface)) {
		const struct wpa_driver_ops *driver;
		void *drv_priv;

		driver = hapd_iface->bss[0]->driver;
		drv_priv = hapd_iface->bss[0]->drv_priv;
		wpa_printf(MSG_DEBUG, "%s: driver=%p drv_priv=%p -> hapd_deinit",
			   __func__, driver, drv_priv);
		if (driver && driver->hapd_deinit && drv_priv) {
			driver->hapd_deinit(drv_priv);
			hapd_iface->bss[0]->drv_priv = NULL;
		}
		return -1;
	}

	return 0;
}


int hostapd_reload_iface(struct hostapd_iface *hapd_iface)
{
	size_t j;

	wpa_printf(MSG_DEBUG, "Reload interface %s",
		   hapd_iface->conf->bss[0]->iface);
	for (j = 0; j < hapd_iface->num_bss; j++)
		hostapd_set_security_params(hapd_iface->conf->bss[j]);
	if (hostapd_config_check(hapd_iface->conf, 1) < 0) {
		wpa_printf(MSG_ERROR, "Updated configuration is invalid");
		return -1;
	}
	hostapd_clear_old(hapd_iface);
	for (j = 0; j < hapd_iface->num_bss; j++)
		hostapd_reload_bss(hapd_iface->bss[j]);

	return 0;
}


int hostapd_disable_iface(struct hostapd_iface *hapd_iface)
{
	size_t j;
	const struct wpa_driver_ops *driver;
	void *drv_priv;

	if (hapd_iface == NULL)
		return -1;
	wpa_msg(hapd_iface->bss[0]->msg_ctx, MSG_INFO, AP_EVENT_DISABLED);
	driver = hapd_iface->bss[0]->driver;
	drv_priv = hapd_iface->bss[0]->drv_priv;

	/* whatever hostapd_interface_deinit does */
	for (j = 0; j < hapd_iface->num_bss; j++) {
		struct hostapd_data *hapd = hapd_iface->bss[j];
		hostapd_free_stas(hapd);
		hostapd_flush_old_stations(hapd, WLAN_REASON_DEAUTH_LEAVING);
		hostapd_clear_wep(hapd);
		hostapd_free_hapd_data(hapd);
	}

	wpa_printf(MSG_DEBUG, "%s: driver=%p drv_priv=%p -> hapd_deinit",
		   __func__, driver, drv_priv);
	if (driver && driver->hapd_deinit && drv_priv) {
		driver->hapd_deinit(drv_priv);
		hapd_iface->bss[0]->drv_priv = NULL;
	}

	/* From hostapd_cleanup_iface: These were initialized in
	 * hostapd_setup_interface and hostapd_setup_interface_complete
	 */
	hostapd_cleanup_iface_partial(hapd_iface);

	wpa_printf(MSG_DEBUG, "Interface %s disabled",
		   hapd_iface->bss[0]->conf->iface);
	hostapd_set_state(hapd_iface, HAPD_IFACE_DISABLED);
	return 0;
}


static struct hostapd_iface *
hostapd_iface_alloc(struct hapd_interfaces *interfaces)
{
	struct hostapd_iface **iface, *hapd_iface;

	iface = os_realloc_array(interfaces->iface, interfaces->count + 1,
				 sizeof(struct hostapd_iface *));
	if (iface == NULL)
		return NULL;
	interfaces->iface = iface;
	hapd_iface = interfaces->iface[interfaces->count] =
		os_zalloc(sizeof(*hapd_iface));
	if (hapd_iface == NULL) {
		wpa_printf(MSG_ERROR, "%s: Failed to allocate memory for "
			   "the interface", __func__);
		return NULL;
	}
	interfaces->count++;
	hapd_iface->interfaces = interfaces;

	return hapd_iface;
}


static struct hostapd_config *
hostapd_config_alloc(struct hapd_interfaces *interfaces, const char *ifname,
		     const char *ctrl_iface)
{
	struct hostapd_bss_config *bss;
	struct hostapd_config *conf;

	/* Allocates memory for bss and conf */
	conf = hostapd_config_defaults();
	if (conf == NULL) {
		 wpa_printf(MSG_ERROR, "%s: Failed to allocate memory for "
				"configuration", __func__);
		return NULL;
	}

	conf->driver = wpa_drivers[0];
	if (conf->driver == NULL) {
		wpa_printf(MSG_ERROR, "No driver wrappers registered!");
		hostapd_config_free(conf);
		return NULL;
	}

	bss = conf->last_bss = conf->bss[0];

	os_strlcpy(bss->iface, ifname, sizeof(bss->iface));
	bss->ctrl_interface = os_strdup(ctrl_iface);
	if (bss->ctrl_interface == NULL) {
		hostapd_config_free(conf);
		return NULL;
	}

	/* Reading configuration file skipped, will be done in SET!
	 * From reading the configuration till the end has to be done in
	 * SET
	 */
	return conf;
}


static struct hostapd_iface * hostapd_data_alloc(
	struct hapd_interfaces *interfaces, struct hostapd_config *conf)
{
	size_t i;
	struct hostapd_iface *hapd_iface =
		interfaces->iface[interfaces->count - 1];
	struct hostapd_data *hapd;

	hapd_iface->conf = conf;
	hapd_iface->num_bss = conf->num_bss;

	hapd_iface->bss = os_zalloc(conf->num_bss *
				    sizeof(struct hostapd_data *));
	if (hapd_iface->bss == NULL)
		return NULL;

	for (i = 0; i < conf->num_bss; i++) {
		hapd = hapd_iface->bss[i] =
			hostapd_alloc_bss_data(hapd_iface, conf, conf->bss[i]);
		if (hapd == NULL)
			return NULL;
		hapd->msg_ctx = hapd;
	}

	hapd_iface->interfaces = interfaces;

	return hapd_iface;
}


int hostapd_add_iface(struct hapd_interfaces *interfaces, char *buf)
{
	struct hostapd_config *conf = NULL;
	struct hostapd_iface *hapd_iface = NULL, *new_iface = NULL;
	struct hostapd_data *hapd;
	char *ptr;
	size_t i, j;
	const char *conf_file = NULL, *phy_name = NULL;

	if (os_strncmp(buf, "bss_config=", 11) == 0) {
		char *pos;
		phy_name = buf + 11;
		pos = os_strchr(phy_name, ':');
		if (!pos)
			return -1;
		*pos++ = '\0';
		conf_file = pos;
		if (!os_strlen(conf_file))
			return -1;

		hapd_iface = hostapd_interface_init_bss(interfaces, phy_name,
							conf_file, 0);
		if (!hapd_iface)
			return -1;
		for (j = 0; j < interfaces->count; j++) {
			if (interfaces->iface[j] == hapd_iface)
				break;
		}
		if (j == interfaces->count) {
			struct hostapd_iface **tmp;
			tmp = os_realloc_array(interfaces->iface,
					       interfaces->count + 1,
					       sizeof(struct hostapd_iface *));
			if (!tmp) {
				hostapd_interface_deinit_free(hapd_iface);
				return -1;
			}
			interfaces->iface = tmp;
			interfaces->iface[interfaces->count++] = hapd_iface;
			new_iface = hapd_iface;
		}

		if (new_iface) {
			if (interfaces->driver_init(hapd_iface) ||
			    hostapd_setup_interface(hapd_iface)) {
				interfaces->count--;
				goto fail;
			}
		} else {
			/* Assign new BSS with bss[0]'s driver info */
			hapd = hapd_iface->bss[hapd_iface->num_bss - 1];
			hapd->driver = hapd_iface->bss[0]->driver;
			hapd->drv_priv = hapd_iface->bss[0]->drv_priv;
			os_memcpy(hapd->own_addr, hapd_iface->bss[0]->own_addr,
				  ETH_ALEN);

			if (start_ctrl_iface_bss(hapd) < 0 ||
			    (hapd_iface->state == HAPD_IFACE_ENABLED &&
			     hostapd_setup_bss(hapd, -1))) {
				hapd_iface->conf->num_bss--;
				hapd_iface->num_bss--;
				wpa_printf(MSG_DEBUG, "%s: free hapd %p %s",
					   __func__, hapd, hapd->conf->iface);
				os_free(hapd);
				return -1;
			}
		}
		return 0;
	}

	ptr = os_strchr(buf, ' ');
	if (ptr == NULL)
		return -1;
	*ptr++ = '\0';

	if (os_strncmp(ptr, "config=", 7) == 0)
		conf_file = ptr + 7;

	for (i = 0; i < interfaces->count; i++) {
		if (!os_strcmp(interfaces->iface[i]->conf->bss[0]->iface,
			       buf)) {
			wpa_printf(MSG_INFO, "Cannot add interface - it "
				   "already exists");
			return -1;
		}
	}

	hapd_iface = hostapd_iface_alloc(interfaces);
	if (hapd_iface == NULL) {
		wpa_printf(MSG_ERROR, "%s: Failed to allocate memory "
			   "for interface", __func__);
		goto fail;
	}

	if (conf_file && interfaces->config_read_cb) {
		conf = interfaces->config_read_cb(conf_file);
		if (conf && conf->bss)
			os_strlcpy(conf->bss[0]->iface, buf,
				   sizeof(conf->bss[0]->iface));
	} else
		conf = hostapd_config_alloc(interfaces, buf, ptr);
	if (conf == NULL || conf->bss == NULL) {
		wpa_printf(MSG_ERROR, "%s: Failed to allocate memory "
			   "for configuration", __func__);
		goto fail;
	}

	hapd_iface = hostapd_data_alloc(interfaces, conf);
	if (hapd_iface == NULL) {
		wpa_printf(MSG_ERROR, "%s: Failed to allocate memory "
			   "for hostapd", __func__);
		goto fail;
	}

	if (start_ctrl_iface(hapd_iface) < 0)
		goto fail;

	wpa_printf(MSG_INFO, "Add interface '%s'", conf->bss[0]->iface);

	return 0;

fail:
	if (conf)
		hostapd_config_free(conf);
	if (hapd_iface) {
		if (hapd_iface->bss) {
			for (i = 0; i < hapd_iface->num_bss; i++) {
				hapd = hapd_iface->bss[i];
				if (hapd && hapd_iface->interfaces &&
				    hapd_iface->interfaces->ctrl_iface_deinit)
					hapd_iface->interfaces->
						ctrl_iface_deinit(hapd);
				wpa_printf(MSG_DEBUG, "%s: free hapd %p (%s)",
					   __func__, hapd_iface->bss[i],
					hapd_iface->bss[i]->conf->iface);
				os_free(hapd_iface->bss[i]);
			}
			os_free(hapd_iface->bss);
		}
		wpa_printf(MSG_DEBUG, "%s: free iface %p",
			   __func__, hapd_iface);
		os_free(hapd_iface);
	}
	return -1;
}


static int hostapd_remove_bss(struct hostapd_iface *iface, unsigned int idx)
{
	size_t i;

	wpa_printf(MSG_INFO, "Remove BSS '%s'", iface->conf->bss[idx]->iface);

	/* Remove hostapd_data only if it has already been initialized */
	if (idx < iface->num_bss) {
		struct hostapd_data *hapd = iface->bss[idx];

		hostapd_bss_deinit(hapd);
		wpa_printf(MSG_DEBUG, "%s: free hapd %p (%s)",
			   __func__, hapd, hapd->conf->iface);
		hostapd_config_free_bss(hapd->conf);
		os_free(hapd);

		iface->num_bss--;

		for (i = idx; i < iface->num_bss; i++)
			iface->bss[i] = iface->bss[i + 1];
	} else {
		hostapd_config_free_bss(iface->conf->bss[idx]);
		iface->conf->bss[idx] = NULL;
	}

	iface->conf->num_bss--;
	for (i = idx; i < iface->conf->num_bss; i++)
		iface->conf->bss[i] = iface->conf->bss[i + 1];

	return 0;
}


int hostapd_remove_iface(struct hapd_interfaces *interfaces, char *buf)
{
	struct hostapd_iface *hapd_iface;
	size_t i, j, k = 0;

	for (i = 0; i < interfaces->count; i++) {
		hapd_iface = interfaces->iface[i];
		if (hapd_iface == NULL)
			return -1;
		if (!os_strcmp(hapd_iface->conf->bss[0]->iface, buf)) {
			wpa_printf(MSG_INFO, "Remove interface '%s'", buf);
			hostapd_interface_deinit_free(hapd_iface);
			k = i;
			while (k < (interfaces->count - 1)) {
				interfaces->iface[k] =
					interfaces->iface[k + 1];
				k++;
			}
			interfaces->count--;
			return 0;
		}

		for (j = 0; j < hapd_iface->conf->num_bss; j++) {
			if (!os_strcmp(hapd_iface->conf->bss[j]->iface, buf))
				return hostapd_remove_bss(hapd_iface, j);
		}
	}
	return -1;
}


/**
 * hostapd_new_assoc_sta - Notify that a new station associated with the AP
 * @hapd: Pointer to BSS data
 * @sta: Pointer to the associated STA data
 * @reassoc: 1 to indicate this was a re-association; 0 = first association
 *
 * This function will be called whenever a station associates with the AP. It
 * can be called from ieee802_11.c for drivers that export MLME to hostapd and
 * from drv_callbacks.c based on driver events for drivers that take care of
 * management frames (IEEE 802.11 authentication and association) internally.
 */
void hostapd_new_assoc_sta(struct hostapd_data *hapd, struct sta_info *sta,
			   int reassoc)
{
	if (hapd->tkip_countermeasures) {
		hostapd_drv_sta_deauth(hapd, sta->addr,
				       WLAN_REASON_MICHAEL_MIC_FAILURE);
		return;
	}

	hostapd_prune_associations(hapd, sta->addr);

	/* IEEE 802.11F (IAPP) */
	if (hapd->conf->ieee802_11f)
		iapp_new_station(hapd->iapp, sta);

#ifdef CONFIG_P2P
	if (sta->p2p_ie == NULL && !sta->no_p2p_set) {
		sta->no_p2p_set = 1;
		hapd->num_sta_no_p2p++;
		if (hapd->num_sta_no_p2p == 1)
			hostapd_p2p_non_p2p_sta_connected(hapd);
	}
#endif /* CONFIG_P2P */

	/* Start accounting here, if IEEE 802.1X and WPA are not used.
	 * IEEE 802.1X/WPA code will start accounting after the station has
	 * been authorized. */
	if (!hapd->conf->ieee802_1x && !hapd->conf->wpa && !hapd->conf->osen) {
		ap_sta_set_authorized(hapd, sta, 1);
		os_get_reltime(&sta->connected_time);
		accounting_sta_start(hapd, sta);
	}

	/* Start IEEE 802.1X authentication process for new stations */
	ieee802_1x_new_station(hapd, sta);
	if (reassoc) {
		if (sta->auth_alg != WLAN_AUTH_FT &&
		    !(sta->flags & (WLAN_STA_WPS | WLAN_STA_MAYBE_WPS)))
			wpa_auth_sm_event(sta->wpa_sm, WPA_REAUTH);
		else if (sta->auth_alg != WLAN_AUTH_FT &&
			 (sta->flags & (WLAN_STA_WPS | WLAN_STA_MAYBE_WPS)))
			wpa_auth_sm_event(sta->wpa_sm, WPA_REAUTH_EAPOL);
	} else
		wpa_auth_sta_associated(hapd->wpa_auth, sta->wpa_sm);

	if (!(hapd->iface->drv_flags & WPA_DRIVER_FLAGS_INACTIVITY_TIMER)) {
		wpa_printf(MSG_DEBUG, "%s: reschedule ap_handle_timer timeout "
			   "for " MACSTR " (%d seconds - ap_max_inactivity)",
			   __func__, MAC2STR(sta->addr),
			   hapd->conf->ap_max_inactivity);
		eloop_cancel_timeout(ap_handle_timer, hapd, sta);
		eloop_register_timeout(hapd->conf->ap_max_inactivity, 0,
				       ap_handle_timer, hapd, sta);
	}
}


const char * hostapd_state_text(enum hostapd_iface_state s)
{
	switch (s) {
	case HAPD_IFACE_UNINITIALIZED:
		return "UNINITIALIZED";
	case HAPD_IFACE_DISABLED:
		return "DISABLED";
	case HAPD_IFACE_COUNTRY_UPDATE:
		return "COUNTRY_UPDATE";
	case HAPD_IFACE_ACS:
		return "ACS";
	case HAPD_IFACE_HT_SCAN:
		return "HT_SCAN";
	case HAPD_IFACE_DFS:
		return "DFS";
	case HAPD_IFACE_ENABLED:
		return "ENABLED";
	}

	return "UNKNOWN";
}


void hostapd_set_state(struct hostapd_iface *iface, enum hostapd_iface_state s)
{
	wpa_printf(MSG_INFO, "%s: interface state %s->%s",
		   iface->conf->bss[0]->iface, hostapd_state_text(iface->state),
		   hostapd_state_text(s));
	iface->state = s;
}


#ifdef NEED_AP_MLME

static void free_beacon_data(struct beacon_data *beacon)
{
	os_free(beacon->head);
	beacon->head = NULL;
	os_free(beacon->tail);
	beacon->tail = NULL;
	os_free(beacon->probe_resp);
	beacon->probe_resp = NULL;
	os_free(beacon->beacon_ies);
	beacon->beacon_ies = NULL;
	os_free(beacon->proberesp_ies);
	beacon->proberesp_ies = NULL;
	os_free(beacon->assocresp_ies);
	beacon->assocresp_ies = NULL;
}


static int hostapd_build_beacon_data(struct hostapd_iface *iface,
				     struct beacon_data *beacon)
{
	struct wpabuf *beacon_extra, *proberesp_extra, *assocresp_extra;
	struct wpa_driver_ap_params params;
	int ret;
	struct hostapd_data *hapd = iface->bss[0];

	os_memset(beacon, 0, sizeof(*beacon));
	ret = ieee802_11_build_ap_params(hapd, &params);
	if (ret < 0)
		return ret;

	ret = hostapd_build_ap_extra_ies(hapd, &beacon_extra,
					 &proberesp_extra,
					 &assocresp_extra);
	if (ret)
		goto free_ap_params;

	ret = -1;
	beacon->head = os_malloc(params.head_len);
	if (!beacon->head)
		goto free_ap_extra_ies;

	os_memcpy(beacon->head, params.head, params.head_len);
	beacon->head_len = params.head_len;

	beacon->tail = os_malloc(params.tail_len);
	if (!beacon->tail)
		goto free_beacon;

	os_memcpy(beacon->tail, params.tail, params.tail_len);
	beacon->tail_len = params.tail_len;

	if (params.proberesp != NULL) {
		beacon->probe_resp = os_malloc(params.proberesp_len);
		if (!beacon->probe_resp)
			goto free_beacon;

		os_memcpy(beacon->probe_resp, params.proberesp,
			  params.proberesp_len);
		beacon->probe_resp_len = params.proberesp_len;
	}

	/* copy the extra ies */
	if (beacon_extra) {
		beacon->beacon_ies = os_malloc(wpabuf_len(beacon_extra));
		if (!beacon->beacon_ies)
			goto free_beacon;

		os_memcpy(beacon->beacon_ies,
			  beacon_extra->buf, wpabuf_len(beacon_extra));
		beacon->beacon_ies_len = wpabuf_len(beacon_extra);
	}

	if (proberesp_extra) {
		beacon->proberesp_ies =
			os_malloc(wpabuf_len(proberesp_extra));
		if (!beacon->proberesp_ies)
			goto free_beacon;

		os_memcpy(beacon->proberesp_ies, proberesp_extra->buf,
			  wpabuf_len(proberesp_extra));
		beacon->proberesp_ies_len = wpabuf_len(proberesp_extra);
	}

	if (assocresp_extra) {
		beacon->assocresp_ies =
			os_malloc(wpabuf_len(assocresp_extra));
		if (!beacon->assocresp_ies)
			goto free_beacon;

		os_memcpy(beacon->assocresp_ies, assocresp_extra->buf,
			  wpabuf_len(assocresp_extra));
		beacon->assocresp_ies_len = wpabuf_len(assocresp_extra);
	}

	ret = 0;
free_beacon:
	/* if the function fails, the caller should not free beacon data */
	if (ret)
		free_beacon_data(beacon);

free_ap_extra_ies:
	hostapd_free_ap_extra_ies(hapd, beacon_extra, proberesp_extra,
				  assocresp_extra);
free_ap_params:
	ieee802_11_free_ap_params(&params);
	return ret;
}


/*
 * TODO: This flow currently supports only changing frequency within the
 * same hw_mode. Any other changes to MAC parameters or provided settings (even
 * width) are not supported.
 */
static int hostapd_change_config_freq(struct hostapd_data *hapd,
				      struct hostapd_config *conf,
				      struct hostapd_freq_params *params,
				      struct hostapd_freq_params *old_params)
{
	int channel;

	if (!params->channel) {
		/* check if the new channel is supported by hw */
		channel = hostapd_hw_get_channel(hapd, params->freq);
		if (!channel)
			return -1;
	} else {
		channel = params->channel;
	}

	/* if a pointer to old_params is provided we save previous state */
	if (old_params) {
		old_params->channel = conf->channel;
		old_params->ht_enabled = conf->ieee80211n;
		old_params->sec_channel_offset = conf->secondary_channel;
	}

	conf->channel = channel;
	conf->ieee80211n = params->ht_enabled;
	conf->secondary_channel = params->sec_channel_offset;

	/* TODO: maybe call here hostapd_config_check here? */

	return 0;
}


static int hostapd_fill_csa_settings(struct hostapd_iface *iface,
				     struct csa_settings *settings)
{
	struct hostapd_freq_params old_freq;
	int ret;

	os_memset(&old_freq, 0, sizeof(old_freq));
	if (!iface || !iface->freq || iface->csa_in_progress)
		return -1;

	ret = hostapd_change_config_freq(iface->bss[0], iface->conf,
					 &settings->freq_params,
					 &old_freq);
	if (ret)
		return ret;

	ret = hostapd_build_beacon_data(iface, &settings->beacon_after);

	/* change back the configuration */
	hostapd_change_config_freq(iface->bss[0], iface->conf,
				   &old_freq, NULL);

	if (ret)
		return ret;

	/* set channel switch parameters for csa ie */
	iface->cs_freq_params = settings->freq_params;
	iface->cs_count = settings->cs_count;
	iface->cs_block_tx = settings->block_tx;

	ret = hostapd_build_beacon_data(iface, &settings->beacon_csa);
	if (ret) {
		free_beacon_data(&settings->beacon_after);
		return ret;
	}

	settings->counter_offset_beacon = iface->cs_c_off_beacon;
	settings->counter_offset_presp = iface->cs_c_off_proberesp;

	return 0;
}


void hostapd_cleanup_cs_params(struct hostapd_data *hapd)
{
	os_memset(&hapd->iface->cs_freq_params, 0,
		  sizeof(hapd->iface->cs_freq_params));
	hapd->iface->cs_count = 0;
	hapd->iface->cs_block_tx = 0;
	hapd->iface->cs_c_off_beacon = 0;
	hapd->iface->cs_c_off_proberesp = 0;
	hapd->iface->csa_in_progress = 0;
}


int hostapd_switch_channel(struct hostapd_data *hapd,
			   struct csa_settings *settings)
{
	int ret;
	ret = hostapd_fill_csa_settings(hapd->iface, settings);
	if (ret)
		return ret;

	ret = hostapd_drv_switch_channel(hapd, settings);
	free_beacon_data(&settings->beacon_csa);
	free_beacon_data(&settings->beacon_after);

	if (ret) {
		/* if we failed, clean cs parameters */
		hostapd_cleanup_cs_params(hapd);
		return ret;
	}

	hapd->iface->csa_in_progress = 1;
	return 0;
}

#endif /* NEED_AP_MLME */
