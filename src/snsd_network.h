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
#ifndef _SNSD_NETWORK_H
#define _SNSD_NETWORK_H

#include "snsd.h"
#include "snsd_dcb.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cpluscplus */

#define SNSD_SECTION_NETWORK_NAME       "NETWORK"
#define SNSD_QOS_CHECK_INTERVAL_DEFAULT 30  /* seconds */
#define SNSD_QOS_CHECK_INTERVAL_MIN     5
#define SNSD_QOS_CHECK_INTERVAL_MAX     3600

/* Per-interface QoS configuration from [NETWORK] section */
struct snsd_network_cfg {
    char ifname[IFNAMSIZ];                  /* configured interface, e.g. "ens0.10" */
    char phy_ifname[IFNAMSIZ];              /* physical device, e.g. "ens0" */
    bool is_vlan;                           /* true if ifname is a VLAN interface */
    uint8_t pfc_en;                         /* PFC enable bitmask (bit per priority) */
    enum snsd_trust_mode trust;             /* pcp or dscp */
    int egress_count;                       /* number of egress mappings */
    struct snsd_egress_map egress[SNSD_DCB_MAX_EGRESS_MAP];
    struct list_head list;
};

/* Initialize network QoS module: parse [NETWORK] config section.
 * Returns 0 on success, negative errno on failure.
 */
int snsd_network_init(void);

/* Apply all configured QoS settings.
 * Called once after init and on drift detection.
 */
int snsd_network_apply(void);

/* Check if QoS configuration has drifted and re-apply if needed.
 * Logs diagnostic messages for any mismatch.
 */
void snsd_network_check(void);

/* Check if it is time for a periodic QoS check based on poll_count.
 * interval_sec: check interval from base_cfg.qos_check_interval
 */
static inline bool snsd_network_need_check(unsigned int last_check,
                                           unsigned int now,
                                           int interval_sec)
{
    unsigned int interval;
    unsigned int threshold;

    if (interval_sec <= 0)
        return false;

    threshold = (unsigned int)interval_sec * (1000000 / POLL_INTERVAL_TIME);
    interval = (now >= last_check) ? (now - last_check) :
               ((unsigned int)0xffffffff - last_check + now + 1);

    return interval >= threshold;
}

/* Free all network QoS config resources. */
void snsd_network_exit(void);

#ifdef __cplusplus
}
#endif  /* __cpluscplus */
#endif /* _SNSD_NETWORK_H */
