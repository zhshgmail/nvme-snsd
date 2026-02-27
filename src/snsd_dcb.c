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
#include "snsd_dcb.h"

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/dcbnl.h>
#include <linux/if_link.h>

/* Netlink message buffer size */
#define SNSD_NL_BUF_SIZE    4096

/* DSCP default priority mapping: dscp / 8 gives priority 0-7 */
#define SNSD_DSCP_TO_PRIO(dscp)     ((dscp) >> 3)

/* Trust sysfs path */
#define SNSD_TRUST_SYSFS_PATH       "/sys/class/net/%s/qos/trust"
#define SNSD_TRUST_SYSFS_MAX_LEN    32

/* DCB netlink message structure:
 * [ nlmsghdr ][ dcbmsg ][ nlattr: DCB_ATTR_IFNAME ][ nlattr: DCB_ATTR_IEEE (nested) ]
 *                                                      [ nlattr: DCB_ATTR_IEEE_PFC ]
 *                                                      [ nlattr: DCB_ATTR_IEEE_APP_TABLE (nested) ]
 *                                                        [ nlattr: DCB_ATTR_IEEE_APP ]
 */

struct snsd_nl_msg {
    char buf[SNSD_NL_BUF_SIZE];
    int len;
};

static int snsd_nl_open(void)
{
    int fd;
    struct sockaddr_nl addr;

    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) {
        SNSD_PRINT(SNSD_ERR, "Failed to open netlink socket: %s", strerror(errno));
        return -errno;
    }

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = 0;  /* let kernel assign */
    addr.nl_groups = 0;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        SNSD_PRINT(SNSD_ERR, "Failed to bind netlink socket: %s", strerror(errno));
        close(fd);
        return -errno;
    }

    return fd;
}

static void snsd_nl_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

/* Initialize a DCB netlink message */
static void snsd_dcb_msg_init(struct snsd_nl_msg *msg, int type, int cmd,
                              const char *ifname)
{
    struct nlmsghdr *nlh;
    struct dcbmsg *dcb;
    struct nlattr *nla;
    int ifname_len;

    memset(msg->buf, 0, sizeof(msg->buf));
    msg->len = 0;

    /* netlink header */
    nlh = (struct nlmsghdr *)msg->buf;
    nlh->nlmsg_type = type;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_seq = 1;
    nlh->nlmsg_pid = 0;

    /* dcbmsg header */
    dcb = (struct dcbmsg *)NLMSG_DATA(nlh);
    dcb->dcb_family = AF_UNSPEC;
    dcb->cmd = cmd;
    dcb->dcb_pad = 0;

    msg->len = NLMSG_ALIGN(NLMSG_LENGTH(sizeof(struct dcbmsg)));

    /* DCB_ATTR_IFNAME attribute */
    ifname_len = strlen(ifname) + 1;
    nla = (struct nlattr *)(msg->buf + msg->len);
    nla->nla_len = NLA_HDRLEN + ifname_len;
    nla->nla_type = DCB_ATTR_IFNAME;
    memcpy((char *)nla + NLA_HDRLEN, ifname, ifname_len);
    msg->len += NLA_ALIGN(nla->nla_len);

    /* update nlmsghdr length */
    nlh->nlmsg_len = msg->len;
}

/* Add a nested attribute start marker, returns offset for nesting */
static int snsd_dcb_nest_start(struct snsd_nl_msg *msg, int type)
{
    struct nlattr *nla;
    int offset = msg->len;

    nla = (struct nlattr *)(msg->buf + msg->len);
    nla->nla_len = NLA_HDRLEN;  /* will be updated by nest_end */
    nla->nla_type = type;
    msg->len += NLA_HDRLEN;

    return offset;
}

/* Close a nested attribute */
static void snsd_dcb_nest_end(struct snsd_nl_msg *msg, int offset)
{
    struct nlattr *nla = (struct nlattr *)(msg->buf + offset);

    nla->nla_len = msg->len - offset;

    /* update nlmsghdr length */
    ((struct nlmsghdr *)msg->buf)->nlmsg_len = msg->len;
}

/* Add a raw data attribute */
static void snsd_dcb_add_attr(struct snsd_nl_msg *msg, int type,
                              const void *data, int data_len)
{
    struct nlattr *nla;

    nla = (struct nlattr *)(msg->buf + msg->len);
    nla->nla_len = NLA_HDRLEN + data_len;
    nla->nla_type = type;
    if (data && data_len > 0)
        memcpy((char *)nla + NLA_HDRLEN, data, data_len);
    msg->len += NLA_ALIGN(nla->nla_len);

    /* update nlmsghdr length */
    ((struct nlmsghdr *)msg->buf)->nlmsg_len = msg->len;
}

/* Send netlink message and receive response.
 * Returns 0 on success, stores response in resp_buf.
 * resp_len: in/out, input is buffer size, output is received length.
 */
static int snsd_dcb_send_recv(int fd, struct snsd_nl_msg *msg,
                              char *resp_buf, int *resp_len)
{
    struct nlmsghdr *nlh;
    int ret;
    int buf_size = *resp_len;

    ret = send(fd, msg->buf, msg->len, 0);
    if (ret < 0) {
        SNSD_PRINT(SNSD_ERR, "DCB netlink send failed: %s", strerror(errno));
        return -errno;
    }

    ret = recv(fd, resp_buf, buf_size, 0);
    if (ret < 0) {
        SNSD_PRINT(SNSD_ERR, "DCB netlink recv failed: %s", strerror(errno));
        return -errno;
    }

    *resp_len = ret;

    /* Check for netlink error */
    nlh = (struct nlmsghdr *)resp_buf;
    if (nlh->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nlh);
        if (err->error != 0) {
            SNSD_PRINT(SNSD_ERR, "DCB netlink error: %s (%d)",
                       strerror(-err->error), err->error);
            return err->error;
        }
    }

    return 0;
}

/* Find a nested attribute in a netlink message payload.
 * data: pointer to start of attributes area.
 * data_len: length of attributes area.
 * type: attribute type to find.
 * Returns pointer to the nlattr, or NULL if not found.
 */
static struct nlattr *snsd_nla_find(const char *data, int data_len, int type)
{
    const char *p = data;

    while (data_len >= NLA_HDRLEN) {
        struct nlattr *nla = (struct nlattr *)p;
        int nla_aligned_len;

        if (nla->nla_len < NLA_HDRLEN || nla->nla_len > data_len)
            break;

        if (nla->nla_type == type)
            return (struct nlattr *)p;

        nla_aligned_len = NLA_ALIGN(nla->nla_len);
        p += nla_aligned_len;
        data_len -= nla_aligned_len;
    }

    return NULL;
}

/* ============ PFC get/set ============ */

int snsd_dcb_get_ieee_pfc(const char *ifname, uint8_t *pfc_en)
{
    struct snsd_nl_msg msg;
    char resp[SNSD_NL_BUF_SIZE];
    int resp_len = sizeof(resp);
    struct nlmsghdr *nlh;
    struct nlattr *ieee_attr, *pfc_attr;
    const char *attrs_start;
    int attrs_len;
    int fd;
    int ret;

    fd = snsd_nl_open();
    if (fd < 0)
        return fd;

    snsd_dcb_msg_init(&msg, RTM_GETDCB, DCB_CMD_IEEE_GET, ifname);

    ret = snsd_dcb_send_recv(fd, &msg, resp, &resp_len);
    snsd_nl_close(fd);
    if (ret != 0)
        return ret;

    /* Parse response: skip nlmsghdr + dcbmsg, then find DCB_ATTR_IEEE */
    nlh = (struct nlmsghdr *)resp;
    attrs_start = resp + NLMSG_ALIGN(NLMSG_LENGTH(sizeof(struct dcbmsg)));
    attrs_len = nlh->nlmsg_len - NLMSG_ALIGN(NLMSG_LENGTH(sizeof(struct dcbmsg)));

    ieee_attr = snsd_nla_find(attrs_start, attrs_len, DCB_ATTR_IEEE);
    if (!ieee_attr) {
        SNSD_PRINT(SNSD_ERR, "DCB_ATTR_IEEE not found in response for %s",
                   ifname);
        return -ENODATA;
    }

    /* Find DCB_ATTR_IEEE_PFC inside the IEEE nested attr */
    pfc_attr = snsd_nla_find((char *)ieee_attr + NLA_HDRLEN,
                             ieee_attr->nla_len - NLA_HDRLEN,
                             DCB_ATTR_IEEE_PFC);
    if (!pfc_attr) {
        SNSD_PRINT(SNSD_ERR, "DCB_ATTR_IEEE_PFC not found for %s", ifname);
        return -ENODATA;
    }

    /* struct ieee_pfc: pfc_cap(u8), pfc_en(u8), mbc(u8), delay(u16), ... */
    {
        const uint8_t *pfc_data = (const uint8_t *)pfc_attr + NLA_HDRLEN;
        *pfc_en = pfc_data[1]; /* pfc_en is the second byte */
    }

    return 0;
}

int snsd_dcb_set_ieee_pfc(const char *ifname, uint8_t pfc_en)
{
    struct snsd_nl_msg msg;
    char resp[SNSD_NL_BUF_SIZE];
    int resp_len = sizeof(resp);
    struct ieee_pfc pfc;
    int ieee_offset;
    int fd;
    int ret;

    fd = snsd_nl_open();
    if (fd < 0)
        return fd;

    snsd_dcb_msg_init(&msg, RTM_SETDCB, DCB_CMD_IEEE_SET, ifname);

    /* Build ieee_pfc struct */
    memset(&pfc, 0, sizeof(pfc));
    pfc.pfc_cap = SNSD_DCB_MAX_TCS;
    pfc.pfc_en = pfc_en;

    /* Add nested: DCB_ATTR_IEEE -> DCB_ATTR_IEEE_PFC */
    ieee_offset = snsd_dcb_nest_start(&msg, DCB_ATTR_IEEE);
    snsd_dcb_add_attr(&msg, DCB_ATTR_IEEE_PFC, &pfc, sizeof(pfc));
    snsd_dcb_nest_end(&msg, ieee_offset);

    ret = snsd_dcb_send_recv(fd, &msg, resp, &resp_len);
    snsd_nl_close(fd);

    if (ret == 0) {
        SNSD_PRINT(SNSD_INFO, "PFC set to 0x%02x on %s", pfc_en, ifname);
    }

    return ret;
}

/* ============ Trust mode get/set ============ */

/* Try sysfs path first */
static int snsd_trust_sysfs_get(const char *ifname, enum snsd_trust_mode *trust)
{
    char path[PATH_MAX];
    char buf[SNSD_TRUST_SYSFS_MAX_LEN];
    int fd, ret;

    snprintf(path, sizeof(path), SNSD_TRUST_SYSFS_PATH, ifname);

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;

    ret = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (ret <= 0)
        return -EIO;

    buf[ret] = '\0';
    /* Remove trailing newline */
    if (ret > 0 && buf[ret - 1] == '\n')
        buf[ret - 1] = '\0';

    if (strcmp(buf, "dscp") == 0)
        *trust = SNSD_TRUST_DSCP;
    else
        *trust = SNSD_TRUST_PCP;

    return 0;
}

static int snsd_trust_sysfs_set(const char *ifname, enum snsd_trust_mode mode)
{
    char path[PATH_MAX];
    const char *val;
    int fd, ret;

    snprintf(path, sizeof(path), SNSD_TRUST_SYSFS_PATH, ifname);

    fd = open(path, O_WRONLY);
    if (fd < 0)
        return -errno;

    val = (mode == SNSD_TRUST_DSCP) ? "dscp" : "pcp";
    ret = write(fd, val, strlen(val));
    close(fd);

    return (ret > 0) ? 0 : -EIO;
}

/* Count DSCP APP entries via netlink to determine trust mode */
static int snsd_trust_netlink_get(const char *ifname, enum snsd_trust_mode *trust)
{
    struct snsd_nl_msg msg;
    char resp[SNSD_NL_BUF_SIZE];
    int resp_len = sizeof(resp);
    struct nlmsghdr *nlh;
    struct nlattr *ieee_attr, *app_table_attr;
    const char *attrs_start;
    int attrs_len;
    int fd;
    int ret;
    int dscp_count = 0;

    fd = snsd_nl_open();
    if (fd < 0)
        return fd;

    snsd_dcb_msg_init(&msg, RTM_GETDCB, DCB_CMD_IEEE_GET, ifname);

    ret = snsd_dcb_send_recv(fd, &msg, resp, &resp_len);
    snsd_nl_close(fd);
    if (ret != 0)
        return ret;

    nlh = (struct nlmsghdr *)resp;
    attrs_start = resp + NLMSG_ALIGN(NLMSG_LENGTH(sizeof(struct dcbmsg)));
    attrs_len = nlh->nlmsg_len - NLMSG_ALIGN(NLMSG_LENGTH(sizeof(struct dcbmsg)));

    ieee_attr = snsd_nla_find(attrs_start, attrs_len, DCB_ATTR_IEEE);
    if (!ieee_attr) {
        *trust = SNSD_TRUST_PCP;
        return 0;
    }

    /* Find APP_TABLE inside IEEE nested */
    app_table_attr = snsd_nla_find((char *)ieee_attr + NLA_HDRLEN,
                                   ieee_attr->nla_len - NLA_HDRLEN,
                                   DCB_ATTR_IEEE_APP_TABLE);
    if (!app_table_attr) {
        *trust = SNSD_TRUST_PCP;
        return 0;
    }

    /* Count entries with selector = IEEE_8021QAZ_APP_SEL_DSCP (5) */
    {
        const char *p = (const char *)app_table_attr + NLA_HDRLEN;
        int remain = app_table_attr->nla_len - NLA_HDRLEN;

        while (remain >= NLA_HDRLEN) {
            struct nlattr *app_nla = (struct nlattr *)p;
            int aligned;

            if (app_nla->nla_len < NLA_HDRLEN || app_nla->nla_len > remain)
                break;

            if (app_nla->nla_len >= NLA_HDRLEN + (int)sizeof(struct dcb_app)) {
                struct dcb_app *app = (struct dcb_app *)(p + NLA_HDRLEN);
                if (app->selector == IEEE_8021QAZ_APP_SEL_DSCP)
                    dscp_count++;
            }

            aligned = NLA_ALIGN(app_nla->nla_len);
            p += aligned;
            remain -= aligned;
        }
    }

    *trust = (dscp_count > 0) ? SNSD_TRUST_DSCP : SNSD_TRUST_PCP;
    return 0;
}

/* Set trust to DSCP via netlink: add 64 default DSCP->priority APP entries */
static int snsd_trust_netlink_set_dscp(const char *ifname)
{
    int fd;
    int dscp;
    int ret;

    fd = snsd_nl_open();
    if (fd < 0)
        return fd;

    for (dscp = 0; dscp < SNSD_DCB_MAX_DSCP; dscp++) {
        struct snsd_nl_msg msg;
        char resp[SNSD_NL_BUF_SIZE];
        int resp_len = sizeof(resp);
        struct dcb_app app;
        int ieee_offset, app_table_offset;

        snsd_dcb_msg_init(&msg, RTM_SETDCB, DCB_CMD_IEEE_SET, ifname);

        app.selector = IEEE_8021QAZ_APP_SEL_DSCP;
        app.priority = SNSD_DSCP_TO_PRIO(dscp);
        app.protocol = dscp;

        ieee_offset = snsd_dcb_nest_start(&msg, DCB_ATTR_IEEE);
        app_table_offset = snsd_dcb_nest_start(&msg, DCB_ATTR_IEEE_APP_TABLE);
        snsd_dcb_add_attr(&msg, DCB_ATTR_IEEE_APP, &app, sizeof(app));
        snsd_dcb_nest_end(&msg, app_table_offset);
        snsd_dcb_nest_end(&msg, ieee_offset);

        ret = snsd_dcb_send_recv(fd, &msg, resp, &resp_len);
        if (ret != 0) {
            SNSD_PRINT(SNSD_ERR, "Failed to set DSCP APP entry %d on %s: %d",
                       dscp, ifname, ret);
            snsd_nl_close(fd);
            return ret;
        }
    }

    snsd_nl_close(fd);
    return 0;
}

/* Set trust to PCP via netlink: delete all DSCP APP entries */
static int snsd_trust_netlink_set_pcp(const char *ifname)
{
    struct snsd_nl_msg get_msg;
    char resp[SNSD_NL_BUF_SIZE];
    int resp_len = sizeof(resp);
    struct nlmsghdr *nlh;
    struct nlattr *ieee_attr, *app_table_attr;
    const char *attrs_start;
    int attrs_len;
    int fd;
    int ret;

    fd = snsd_nl_open();
    if (fd < 0)
        return fd;

    /* First get current APP entries */
    snsd_dcb_msg_init(&get_msg, RTM_GETDCB, DCB_CMD_IEEE_GET, ifname);

    ret = snsd_dcb_send_recv(fd, &get_msg, resp, &resp_len);
    if (ret != 0) {
        snsd_nl_close(fd);
        return ret;
    }

    nlh = (struct nlmsghdr *)resp;
    attrs_start = resp + NLMSG_ALIGN(NLMSG_LENGTH(sizeof(struct dcbmsg)));
    attrs_len = nlh->nlmsg_len - NLMSG_ALIGN(NLMSG_LENGTH(sizeof(struct dcbmsg)));

    ieee_attr = snsd_nla_find(attrs_start, attrs_len, DCB_ATTR_IEEE);
    if (!ieee_attr) {
        snsd_nl_close(fd);
        return 0; /* no IEEE attrs, already PCP */
    }

    app_table_attr = snsd_nla_find((char *)ieee_attr + NLA_HDRLEN,
                                   ieee_attr->nla_len - NLA_HDRLEN,
                                   DCB_ATTR_IEEE_APP_TABLE);
    if (!app_table_attr) {
        snsd_nl_close(fd);
        return 0; /* no APP entries, already PCP */
    }

    /* Delete each DSCP entry */
    {
        const char *p = (const char *)app_table_attr + NLA_HDRLEN;
        int remain = app_table_attr->nla_len - NLA_HDRLEN;

        while (remain >= NLA_HDRLEN) {
            struct nlattr *app_nla = (struct nlattr *)p;
            int aligned;

            if (app_nla->nla_len < NLA_HDRLEN || app_nla->nla_len > remain)
                break;

            if (app_nla->nla_len >= NLA_HDRLEN + (int)sizeof(struct dcb_app)) {
                struct dcb_app *app = (struct dcb_app *)(p + NLA_HDRLEN);

                if (app->selector == IEEE_8021QAZ_APP_SEL_DSCP) {
                    struct snsd_nl_msg del_msg;
                    char del_resp[SNSD_NL_BUF_SIZE];
                    int del_resp_len = sizeof(del_resp);
                    struct dcb_app del_app;
                    int ieee_off, table_off;

                    snsd_dcb_msg_init(&del_msg, RTM_SETDCB,
                                      DCB_CMD_IEEE_DEL, ifname);

                    del_app.selector = app->selector;
                    del_app.priority = app->priority;
                    del_app.protocol = app->protocol;

                    ieee_off = snsd_dcb_nest_start(&del_msg, DCB_ATTR_IEEE);
                    table_off = snsd_dcb_nest_start(&del_msg,
                                                     DCB_ATTR_IEEE_APP_TABLE);
                    snsd_dcb_add_attr(&del_msg, DCB_ATTR_IEEE_APP,
                                      &del_app, sizeof(del_app));
                    snsd_dcb_nest_end(&del_msg, table_off);
                    snsd_dcb_nest_end(&del_msg, ieee_off);

                    ret = snsd_dcb_send_recv(fd, &del_msg,
                                             del_resp, &del_resp_len);
                    if (ret != 0) {
                        SNSD_PRINT(SNSD_ERR,
                                   "Failed to del DSCP APP entry on %s: %d",
                                   ifname, ret);
                    }
                }
            }

            aligned = NLA_ALIGN(app_nla->nla_len);
            p += aligned;
            remain -= aligned;
        }
    }

    snsd_nl_close(fd);
    return 0;
}

int snsd_dcb_get_trust(const char *ifname, enum snsd_trust_mode *trust)
{
    int ret;

    /* Try sysfs first (faster, available on newer Mellanox drivers) */
    ret = snsd_trust_sysfs_get(ifname, trust);
    if (ret == 0)
        return 0;

    /* Fallback to netlink APP table inspection */
    return snsd_trust_netlink_get(ifname, trust);
}

int snsd_dcb_set_trust(const char *ifname, enum snsd_trust_mode mode)
{
    int ret;

    /* Try sysfs first */
    ret = snsd_trust_sysfs_set(ifname, mode);
    if (ret == 0) {
        SNSD_PRINT(SNSD_INFO, "Trust set to %s on %s (sysfs)",
                   (mode == SNSD_TRUST_DSCP) ? "dscp" : "pcp", ifname);
        return 0;
    }

    /* Fallback to netlink APP table manipulation */
    if (mode == SNSD_TRUST_DSCP)
        ret = snsd_trust_netlink_set_dscp(ifname);
    else
        ret = snsd_trust_netlink_set_pcp(ifname);

    if (ret == 0) {
        SNSD_PRINT(SNSD_INFO, "Trust set to %s on %s (netlink)",
                   (mode == SNSD_TRUST_DSCP) ? "dscp" : "pcp", ifname);
    }

    return ret;
}

/* ============ Egress QoS map (netlink RTM_SETLINK/RTM_GETLINK) ============ */

/* Initialize an RTM_SETLINK / RTM_GETLINK message with ifinfomsg header.
 * This is different from DCB messages which use dcbmsg header.
 */
static void snsd_rtnl_msg_init(struct snsd_nl_msg *msg, int type,
                                unsigned int ifindex)
{
    struct nlmsghdr *nlh;
    struct ifinfomsg *ifi;

    memset(msg->buf, 0, sizeof(msg->buf));
    msg->len = 0;

    nlh = (struct nlmsghdr *)msg->buf;
    nlh->nlmsg_type = type;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_seq = 1;
    nlh->nlmsg_pid = 0;

    ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = ifindex;

    msg->len = NLMSG_ALIGN(NLMSG_LENGTH(sizeof(struct ifinfomsg)));
    nlh->nlmsg_len = msg->len;
}

int snsd_dcb_set_egress_qos_map(const char *vlan_ifname,
                                const struct snsd_egress_map *maps, int count)
{
    struct snsd_nl_msg msg;
    char resp_buf[SNSD_NL_BUF_SIZE];
    int resp_len = sizeof(resp_buf);
    unsigned int ifindex;
    int linkinfo_off, data_off, egress_off;
    int fd;
    int ret;
    int i;

    ifindex = if_nametoindex(vlan_ifname);
    if (ifindex == 0) {
        SNSD_PRINT(SNSD_ERR, "Interface %s not found: %s",
                   vlan_ifname, strerror(errno));
        return -ENODEV;
    }

    fd = snsd_nl_open();
    if (fd < 0)
        return fd;

    /* Build RTM_SETLINK message:
     * [nlmsghdr][ifinfomsg]
     *   [IFLA_LINKINFO (nested)]
     *     [IFLA_INFO_KIND = "vlan"]
     *     [IFLA_INFO_DATA (nested)]
     *       [IFLA_VLAN_EGRESS_QOS (nested)]
     *         [IFLA_VLAN_QOS_MAPPING {from, to}] * N
     */
    /* RTM_NEWLINK (not RTM_SETLINK) is needed to modify VLAN attributes.
     * In the kernel, only rtnl_newlink() processes IFLA_LINKINFO;
     * rtnl_setlink() / do_setlink() silently ignores it.
     */
    snsd_rtnl_msg_init(&msg, RTM_NEWLINK, ifindex);

    linkinfo_off = snsd_dcb_nest_start(&msg, IFLA_LINKINFO);
    snsd_dcb_add_attr(&msg, IFLA_INFO_KIND, "vlan", 5); /* "vlan\0" */

    data_off = snsd_dcb_nest_start(&msg, IFLA_INFO_DATA);
    egress_off = snsd_dcb_nest_start(&msg, IFLA_VLAN_EGRESS_QOS);

    for (i = 0; i < count; i++) {
        struct ifla_vlan_qos_mapping qos;

        qos.from = (__u32)maps[i].from;
        qos.to = (__u32)maps[i].to;
        snsd_dcb_add_attr(&msg, IFLA_VLAN_QOS_MAPPING,
                          &qos, sizeof(qos));
    }

    snsd_dcb_nest_end(&msg, egress_off);
    snsd_dcb_nest_end(&msg, data_off);
    snsd_dcb_nest_end(&msg, linkinfo_off);

    ret = snsd_dcb_send_recv(fd, &msg, resp_buf, &resp_len);
    snsd_nl_close(fd);

    if (ret == 0) {
        SNSD_PRINT(SNSD_INFO,
                   "Egress-qos-map set on %s: %d mapping(s) via netlink",
                   vlan_ifname, count);
    }

    return ret;
}

int snsd_dcb_get_egress_qos_map(const char *vlan_ifname,
                                struct snsd_egress_map *maps, int *count)
{
    struct snsd_nl_msg msg;
    char resp_buf[SNSD_NL_BUF_SIZE];
    int resp_len = sizeof(resp_buf);
    unsigned int ifindex;
    int fd;
    int ret;
    int found = 0;
    struct nlmsghdr *nlh;
    struct nlattr *linkinfo, *info_data, *egress_qos, *nla;
    const char *attr_data;
    int attr_len;

    ifindex = if_nametoindex(vlan_ifname);
    if (ifindex == 0) {
        SNSD_PRINT(SNSD_DBG, "Interface %s not found: %s",
                   vlan_ifname, strerror(errno));
        *count = 0;
        return -ENODEV;
    }

    fd = snsd_nl_open();
    if (fd < 0)
        return fd;

    /* RTM_GETLINK to retrieve VLAN link info */
    snsd_rtnl_msg_init(&msg, RTM_GETLINK, ifindex);

    /* Request IFLA_LINKINFO in the filter mask */
    ((struct nlmsghdr *)msg.buf)->nlmsg_flags |= NLM_F_REQUEST;
    /* Add IFLA_EXT_MASK to get VLAN details */
    {
        __u32 ext_mask = 1; /* RTEXT_FILTER_VF */
        snsd_dcb_add_attr(&msg, IFLA_EXT_MASK,
                          &ext_mask, sizeof(ext_mask));
    }

    ret = snsd_dcb_send_recv(fd, &msg, resp_buf, &resp_len);
    snsd_nl_close(fd);

    if (ret != 0) {
        *count = 0;
        return ret;
    }

    /* Parse response: find IFLA_LINKINFO > IFLA_INFO_DATA > IFLA_VLAN_EGRESS_QOS */
    nlh = (struct nlmsghdr *)resp_buf;
    attr_data = (const char *)NLMSG_DATA(nlh) + NLMSG_ALIGN(sizeof(struct ifinfomsg));
    attr_len = nlh->nlmsg_len - NLMSG_ALIGN(NLMSG_LENGTH(sizeof(struct ifinfomsg)));

    linkinfo = snsd_nla_find(attr_data, attr_len, IFLA_LINKINFO);
    if (!linkinfo) {
        SNSD_PRINT(SNSD_DBG, "No IFLA_LINKINFO for %s", vlan_ifname);
        *count = 0;
        return 0;
    }

    /* Navigate into IFLA_LINKINFO nested attrs */
    attr_data = (const char *)linkinfo + NLA_HDRLEN;
    attr_len = linkinfo->nla_len - NLA_HDRLEN;

    info_data = snsd_nla_find(attr_data, attr_len, IFLA_INFO_DATA);
    if (!info_data) {
        SNSD_PRINT(SNSD_DBG, "No IFLA_INFO_DATA for %s", vlan_ifname);
        *count = 0;
        return 0;
    }

    /* Navigate into IFLA_INFO_DATA nested attrs */
    attr_data = (const char *)info_data + NLA_HDRLEN;
    attr_len = info_data->nla_len - NLA_HDRLEN;

    egress_qos = snsd_nla_find(attr_data, attr_len, IFLA_VLAN_EGRESS_QOS);
    if (!egress_qos) {
        SNSD_PRINT(SNSD_DBG, "No IFLA_VLAN_EGRESS_QOS for %s", vlan_ifname);
        *count = 0;
        return 0;
    }

    /* Parse IFLA_VLAN_QOS_MAPPING entries */
    attr_data = (const char *)egress_qos + NLA_HDRLEN;
    attr_len = egress_qos->nla_len - NLA_HDRLEN;

    nla = (struct nlattr *)attr_data;
    while (attr_len >= NLA_HDRLEN && found < SNSD_DCB_MAX_EGRESS_MAP) {
        if (nla->nla_len < NLA_HDRLEN || nla->nla_len > attr_len)
            break;

        if (nla->nla_type == IFLA_VLAN_QOS_MAPPING &&
            nla->nla_len >= NLA_HDRLEN + (int)sizeof(struct ifla_vlan_qos_mapping)) {
            struct ifla_vlan_qos_mapping *qos;

            qos = (struct ifla_vlan_qos_mapping *)((char *)nla + NLA_HDRLEN);
            maps[found].from = (int)qos->from;
            maps[found].to = (int)qos->to;
            found++;
        }

        attr_len -= NLA_ALIGN(nla->nla_len);
        nla = (struct nlattr *)((char *)nla + NLA_ALIGN(nla->nla_len));
    }

    *count = found;
    return 0;
}
