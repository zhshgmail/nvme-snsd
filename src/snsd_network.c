/*
 * BSD 3-Clause License
 *
 * Copyright (c) [2020], [Huawei Technologies Co., Ltd.]
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "snsd_network.h"
#include "snsd_cfg.h"

/* Configuration file path (same as snsd_cfg.c) */
#define SNSD_NETWORK_CFG_FILE   SNSD_CONFIG_FILE_PATH
#define SNSD_NETWORK_LINE_MAX   1024
#define SNSD_NETWORK_FIELD_MAX  256

/* Global list of network QoS configs */
static LIST_HEAD(network_cfg_list);
static int network_cfg_count;

/* ============ Config file parsing ============ */

/* Skip leading whitespace */
static char *skip_space(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

/* Trim trailing whitespace in-place */
static void trim_trailing(char *s)
{
    int len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
}

/* Extract physical device name from interface name.
 * "ens0.10" -> "ens0", is_vlan=true
 * "ens0"    -> "ens0", is_vlan=false
 */
static void snsd_network_parse_ifname(struct snsd_network_cfg *cfg)
{
    char *dot;

    strncpy(cfg->phy_ifname, cfg->ifname, IFNAMSIZ - 1);
    cfg->phy_ifname[IFNAMSIZ - 1] = '\0';

    dot = strchr(cfg->phy_ifname, '.');
    if (dot) {
        *dot = '\0';
        cfg->is_vlan = true;
    } else {
        cfg->is_vlan = false;
    }
}

/* Parse PFC field: "0,0,0,1,0,0,0,0" -> pfc_en bitmask */
static int snsd_network_parse_pfc(const char *val, uint8_t *pfc_en)
{
    int prio;
    const char *p = val;
    uint8_t mask = 0;

    for (prio = 0; prio < SNSD_DCB_MAX_TCS; prio++) {
        int v;

        while (*p && isspace((unsigned char)*p))
            p++;

        if (*p == '0' || *p == '1') {
            v = *p - '0';
            if (v)
                mask |= (1 << prio);
            p++;
        } else {
            SNSD_PRINT(SNSD_ERR, "Invalid PFC value at priority %d: '%c'",
                       prio, *p);
            return -EINVAL;
        }

        if (prio < SNSD_DCB_MAX_TCS - 1) {
            while (*p && isspace((unsigned char)*p))
                p++;
            if (*p != ',') {
                SNSD_PRINT(SNSD_ERR,
                           "PFC values must be comma-separated, got '%c'", *p);
                return -EINVAL;
            }
            p++; /* skip comma */
        }
    }

    *pfc_en = mask;
    return 0;
}

/* Parse trust field: "dscp" or "pcp" */
static int snsd_network_parse_trust(const char *val,
                                    enum snsd_trust_mode *trust)
{
    if (strcmp(val, "dscp") == 0) {
        *trust = SNSD_TRUST_DSCP;
        return 0;
    }
    if (strcmp(val, "pcp") == 0) {
        *trust = SNSD_TRUST_PCP;
        return 0;
    }

    SNSD_PRINT(SNSD_ERR, "Invalid trust value: '%s' (expected dscp or pcp)",
               val);
    return -EINVAL;
}

/* Parse egress field: "0:3" or "0:3,1:3,2:3" */
static int snsd_network_parse_egress(const char *val,
                                     struct snsd_egress_map *maps,
                                     int *count)
{
    const char *p = val;
    int n = 0;

    while (*p && n < SNSD_DCB_MAX_EGRESS_MAP) {
        int from, to;

        while (*p && isspace((unsigned char)*p))
            p++;

        if (sscanf(p, "%d:%d", &from, &to) != 2) {
            SNSD_PRINT(SNSD_ERR, "Invalid egress mapping at: '%s'", p);
            return -EINVAL;
        }

        if (from < 0 || from > 7 || to < 0 || to > 7) {
            SNSD_PRINT(SNSD_ERR,
                       "Egress mapping out of range: %d:%d (must be 0-7)",
                       from, to);
            return -EINVAL;
        }

        maps[n].from = from;
        maps[n].to = to;
        n++;

        /* Skip to next comma or end */
        while (*p && *p != ',')
            p++;
        if (*p == ',')
            p++;
    }

    *count = n;
    return 0;
}

/* Parse a single key=value pair from a pipe-delimited field.
 * field: e.g., "--ifname = ens0.10" (already whitespace-normalized)
 */
static int snsd_network_parse_field(const char *field,
                                    struct snsd_network_cfg *cfg)
{
    char key[SNSD_NETWORK_FIELD_MAX];
    char val[SNSD_NETWORK_FIELD_MAX];
    char *eq;
    char *p;

    /* Copy to working buffer */
    strncpy(key, field, sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';

    /* Find '=' separator */
    eq = strchr(key, '=');
    if (!eq) {
        SNSD_PRINT(SNSD_ERR, "No '=' found in field: '%s'", field);
        return -EINVAL;
    }

    *eq = '\0';
    trim_trailing(key);
    strncpy(val, skip_space(eq + 1), sizeof(val) - 1);
    val[sizeof(val) - 1] = '\0';
    trim_trailing(val);

    /* Strip leading "--" from key if present */
    p = key;
    p = skip_space(p);
    if (strncmp(p, "--", 2) == 0)
        p += 2;

    if (strcmp(p, "ifname") == 0) {
        strncpy(cfg->ifname, val, IFNAMSIZ - 1);
        cfg->ifname[IFNAMSIZ - 1] = '\0';
        return 0;
    }

    if (strcmp(p, "pfc") == 0)
        return snsd_network_parse_pfc(val, &cfg->pfc_en);

    if (strcmp(p, "trust") == 0)
        return snsd_network_parse_trust(val, &cfg->trust);

    if (strcmp(p, "egress") == 0)
        return snsd_network_parse_egress(val, cfg->egress, &cfg->egress_count);

    SNSD_PRINT(SNSD_ERR, "Unknown NETWORK config key: '%s'", p);
    return -EINVAL;
}

/* Parse one configuration line (pipe-delimited fields) */
static int snsd_network_parse_line(char *line)
{
    struct snsd_network_cfg *cfg;
    char *field;
    char *save_ptr = NULL;
    int ret;

    cfg = (struct snsd_network_cfg *)malloc(sizeof(struct snsd_network_cfg));
    if (!cfg) {
        SNSD_PRINT(SNSD_ERR, "Failed to alloc network config.");
        return -ENOMEM;
    }
    memset(cfg, 0, sizeof(*cfg));

    /* Split line by '|' */
    field = strtok_r(line, "|", &save_ptr);
    while (field) {
        ret = snsd_network_parse_field(field, cfg);
        if (ret != 0) {
            free(cfg);
            return ret;
        }
        field = strtok_r(NULL, "|", &save_ptr);
    }

    /* Validate: ifname is required */
    if (cfg->ifname[0] == '\0') {
        SNSD_PRINT(SNSD_ERR, "NETWORK config: --ifname is required.");
        free(cfg);
        return -EINVAL;
    }

    /* Validate: if trust=pcp, must be VLAN device */
    snsd_network_parse_ifname(cfg);
    if (cfg->trust == SNSD_TRUST_PCP && !cfg->is_vlan) {
        SNSD_PRINT(SNSD_ERR,
                   "NETWORK config: trust=pcp requires VLAN interface, "
                   "got '%s'.", cfg->ifname);
        free(cfg);
        return -EINVAL;
    }

    /* Validate: egress only makes sense on VLAN */
    if (cfg->egress_count > 0 && !cfg->is_vlan) {
        SNSD_PRINT(SNSD_ERR,
                   "NETWORK config: --egress requires VLAN interface, "
                   "got '%s'.", cfg->ifname);
        free(cfg);
        return -EINVAL;
    }

    list_add_tail(&cfg->list, &network_cfg_list);
    network_cfg_count++;

    SNSD_PRINT(SNSD_INFO,
               "NETWORK config loaded: ifname=%s phy=%s pfc=0x%02x trust=%s "
               "egress_count=%d",
               cfg->ifname, cfg->phy_ifname, cfg->pfc_en,
               (cfg->trust == SNSD_TRUST_DSCP) ? "dscp" : "pcp",
               cfg->egress_count);

    return 0;
}

/* Read and parse the [NETWORK] section from config file.
 * We do a simple line-by-line parse: read the [NETWORK] section,
 * then parse each non-empty, non-comment line.
 */
static int snsd_network_parse_section(void)
{
    FILE *fp;
    char line[SNSD_NETWORK_LINE_MAX];
    bool in_section = false;
    int ret = 0;

    fp = fopen(SNSD_NETWORK_CFG_FILE, "r");
    if (!fp) {
        SNSD_PRINT(SNSD_INFO, "Config file %s not found, skip NETWORK.",
                   SNSD_NETWORK_CFG_FILE);
        return 0;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *p = skip_space(line);

        /* Remove trailing newline/whitespace */
        trim_trailing(p);

        /* Skip empty lines and comments */
        if (*p == '\0' || *p == ';')
            continue;

        /* Section header detection */
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) {
                *end = '\0';
                in_section = (strcmp(p + 1,
                                    SNSD_SECTION_NETWORK_NAME) == 0);
            }
            continue;
        }

        if (!in_section)
            continue;

        /* Parse this config line */
        ret = snsd_network_parse_line(p);
        if (ret != 0) {
            SNSD_PRINT(SNSD_ERR, "Failed to parse NETWORK line: '%s'", p);
            break;
        }
    }

    fclose(fp);
    return ret;
}

/* ============ QoS apply and check ============ */

static int snsd_network_apply_one(struct snsd_network_cfg *cfg)
{
    int ret;

    /* 1. Set PFC on physical port */
    ret = snsd_dcb_set_ieee_pfc(cfg->phy_ifname, cfg->pfc_en);
    if (ret != 0) {
        SNSD_PRINT(SNSD_ERR, "Failed to set PFC on %s: %d",
                   cfg->phy_ifname, ret);
        return ret;
    }

    /* 2. Set trust mode on physical port */
    ret = snsd_dcb_set_trust(cfg->phy_ifname, cfg->trust);
    if (ret != 0) {
        SNSD_PRINT(SNSD_ERR, "Failed to set trust on %s: %d",
                   cfg->phy_ifname, ret);
        return ret;
    }

    /* 3. Set egress-qos-map on VLAN interface (if applicable) */
    if (cfg->is_vlan && cfg->egress_count > 0) {
        ret = snsd_dcb_set_egress_qos_map(cfg->ifname,
                                           cfg->egress, cfg->egress_count);
        if (ret != 0) {
            SNSD_PRINT(SNSD_ERR, "Failed to set egress-qos-map on %s: %d",
                       cfg->ifname, ret);
            return ret;
        }
    }

    return 0;
}

static void snsd_network_check_one(struct snsd_network_cfg *cfg)
{
    uint8_t cur_pfc = 0;
    enum snsd_trust_mode cur_trust = SNSD_TRUST_PCP;
    struct snsd_egress_map cur_egress[SNSD_DCB_MAX_EGRESS_MAP];
    int cur_egress_count = 0;
    bool drift = false;
    int ret;
    int i;

    /* Check PFC */
    ret = snsd_dcb_get_ieee_pfc(cfg->phy_ifname, &cur_pfc);
    if (ret != 0) {
        SNSD_PRINT(SNSD_ERR, "QoS check: failed to read PFC on %s: %d",
                   cfg->phy_ifname, ret);
        drift = true;
    } else if (cur_pfc != cfg->pfc_en) {
        SNSD_PRINT(SNSD_ERR,
                   "QoS drift on %s: PFC expected 0x%02x, got 0x%02x",
                   cfg->phy_ifname, cfg->pfc_en, cur_pfc);
        drift = true;
    }

    /* Check trust */
    ret = snsd_dcb_get_trust(cfg->phy_ifname, &cur_trust);
    if (ret != 0) {
        SNSD_PRINT(SNSD_ERR, "QoS check: failed to read trust on %s: %d",
                   cfg->phy_ifname, ret);
        drift = true;
    } else if (cur_trust != cfg->trust) {
        SNSD_PRINT(SNSD_ERR,
                   "QoS drift on %s: trust expected %s, got %s",
                   cfg->phy_ifname,
                   (cfg->trust == SNSD_TRUST_DSCP) ? "dscp" : "pcp",
                   (cur_trust == SNSD_TRUST_DSCP) ? "dscp" : "pcp");
        drift = true;
    }

    /* Check egress-qos-map */
    if (cfg->is_vlan && cfg->egress_count > 0) {
        memset(cur_egress, 0, sizeof(cur_egress));
        ret = snsd_dcb_get_egress_qos_map(cfg->ifname,
                                           cur_egress, &cur_egress_count);
        if (ret != 0) {
            SNSD_PRINT(SNSD_ERR,
                       "QoS check: failed to read egress map on %s: %d",
                       cfg->ifname, ret);
            drift = true;
        } else {
            /* Compare each configured mapping */
            for (i = 0; i < cfg->egress_count; i++) {
                bool found = false;
                int j;
                for (j = 0; j < cur_egress_count; j++) {
                    if (cur_egress[j].from == cfg->egress[i].from &&
                        cur_egress[j].to == cfg->egress[i].to) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    SNSD_PRINT(SNSD_ERR,
                               "QoS drift on %s: egress mapping %d:%d missing",
                               cfg->ifname, cfg->egress[i].from,
                               cfg->egress[i].to);
                    drift = true;
                }
            }
        }
    }

    if (drift) {
        SNSD_PRINT(SNSD_INFO, "Re-applying QoS config for %s", cfg->ifname);
        snsd_network_apply_one(cfg);
    } else {
        SNSD_PRINT(SNSD_DBG, "QoS config OK for %s", cfg->ifname);
    }
}

/* ============ Public API ============ */

int snsd_network_init(void)
{
    int ret;

    INIT_LIST_HEAD(&network_cfg_list);
    network_cfg_count = 0;

    ret = snsd_network_parse_section();
    if (ret != 0) {
        SNSD_PRINT(SNSD_ERR, "Failed to parse [NETWORK] section.");
        snsd_network_exit();
        return ret;
    }

    if (network_cfg_count == 0) {
        SNSD_PRINT(SNSD_INFO, "No [NETWORK] config found, QoS disabled.");
        return 0;
    }

    SNSD_PRINT(SNSD_INFO, "Network QoS init: %d interface(s) configured.",
               network_cfg_count);
    return 0;
}

int snsd_network_apply(void)
{
    struct list_head *pos;
    struct snsd_network_cfg *cfg;
    int fail_count = 0;

    if (network_cfg_count == 0)
        return 0;

    list_for_each(pos, &network_cfg_list) {
        cfg = list_entry(pos, struct snsd_network_cfg, list);
        if (snsd_network_apply_one(cfg) != 0)
            fail_count++;
    }

    if (fail_count > 0) {
        SNSD_PRINT(SNSD_ERR, "QoS apply: %d interface(s) failed.", fail_count);
        return -EIO;
    }

    SNSD_PRINT(SNSD_INFO, "QoS applied to %d interface(s).", network_cfg_count);
    return 0;
}

void snsd_network_check(void)
{
    struct list_head *pos;
    struct snsd_network_cfg *cfg;

    if (network_cfg_count == 0)
        return;

    SNSD_PRINT(SNSD_DBG, "Periodic QoS check running...");

    list_for_each(pos, &network_cfg_list) {
        cfg = list_entry(pos, struct snsd_network_cfg, list);
        snsd_network_check_one(cfg);
    }
}

void snsd_network_exit(void)
{
    struct snsd_network_cfg *cfg;
    struct list_head *pos, *tmp;

    list_for_each_safe(pos, tmp, &network_cfg_list) {
        cfg = list_entry(pos, struct snsd_network_cfg, list);
        list_del(pos);
        free(cfg);
    }

    network_cfg_count = 0;
    SNSD_PRINT(SNSD_INFO, "Network QoS module exited.");
}
