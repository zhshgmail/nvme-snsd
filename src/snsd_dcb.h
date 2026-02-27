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
#ifndef _SNSD_DCB_H
#define _SNSD_DCB_H

#include "snsd.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cpluscplus */

/* IEEE 802.1Qaz constants */
#define SNSD_DCB_MAX_TCS            8
#define SNSD_DCB_MAX_DSCP           64
#define SNSD_DCB_MAX_EGRESS_MAP     8

/* Trust mode */
enum snsd_trust_mode {
    SNSD_TRUST_PCP = 0,
    SNSD_TRUST_DSCP = 1,
    SNSD_TRUST_BUTT
};

/* Egress QoS mapping: skprio -> VLAN UP */
struct snsd_egress_map {
    int from;   /* sk_priority */
    int to;     /* VLAN User Priority (UP) */
};

/* Get IEEE PFC enable bitmask from a physical interface.
 * Returns 0 on success, negative errno on failure.
 * pfc_en: output bitmask where bit i = PFC enabled on priority i.
 */
int snsd_dcb_get_ieee_pfc(const char *ifname, uint8_t *pfc_en);

/* Set IEEE PFC enable bitmask on a physical interface.
 * pfc_en: bitmask where bit i = enable PFC on priority i.
 */
int snsd_dcb_set_ieee_pfc(const char *ifname, uint8_t pfc_en);

/* Get current trust mode on a physical interface.
 * Returns 0 on success, negative errno on failure.
 * trust: output SNSD_TRUST_PCP or SNSD_TRUST_DSCP.
 */
int snsd_dcb_get_trust(const char *ifname, enum snsd_trust_mode *trust);

/* Set trust mode on a physical interface.
 * mode: SNSD_TRUST_PCP or SNSD_TRUST_DSCP.
 */
int snsd_dcb_set_trust(const char *ifname, enum snsd_trust_mode mode);

/* Set VLAN egress-qos-map on a VLAN interface.
 * maps: array of skprio->UP mappings.
 * count: number of mappings.
 */
int snsd_dcb_set_egress_qos_map(const char *vlan_ifname,
                                const struct snsd_egress_map *maps, int count);

/* Get VLAN egress-qos-map from a VLAN interface.
 * maps: output array (at least SNSD_DCB_MAX_EGRESS_MAP entries).
 * count: output number of mappings found.
 */
int snsd_dcb_get_egress_qos_map(const char *vlan_ifname,
                                struct snsd_egress_map *maps, int *count);

#ifdef __cplusplus
}
#endif  /* __cpluscplus */
#endif /* _SNSD_DCB_H */
