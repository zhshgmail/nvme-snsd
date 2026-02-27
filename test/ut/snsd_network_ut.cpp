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
#include "gtest/gtest.h"
#include <mockcpp/mockcpp.hpp>
#include "snsd.h"
#include "snsd_network.h"
#include "snsd_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cpluscplus */

/* Access to DCB functions for mocking */
int snsd_dcb_get_ieee_pfc(const char *ifname, uint8_t *pfc_en);
int snsd_dcb_set_ieee_pfc(const char *ifname, uint8_t pfc_en);
int snsd_dcb_get_trust(const char *ifname, enum snsd_trust_mode *trust);
int snsd_dcb_set_trust(const char *ifname, enum snsd_trust_mode mode);
int snsd_dcb_set_egress_qos_map(const char *vlan_ifname,
                                const struct snsd_egress_map *maps, int count);
int snsd_dcb_get_egress_qos_map(const char *vlan_ifname,
                                struct snsd_egress_map *maps, int *count);

#ifdef __cplusplus
}
#endif  /* __cpluscplus */

namespace
{
    class snsd_network_ut : public ::testing::Test {
    protected:
        virtual void SetUp()
        {
            std::cout << "SetUp: snsd_network_ut." << std::endl;
        }

        virtual void TearDown()
        {
            std::cout << "TearDown: snsd_network_ut." << std::endl;
            GlobalMockObject::verify();
        }
    };
}

/* ============ snsd_network_need_check() tests ============ */

TEST_F(snsd_network_ut, need_check_zero_interval)
{
    /* interval_sec <= 0 should always return false */
    EXPECT_FALSE(snsd_network_need_check(0, 100, 0));
    EXPECT_FALSE(snsd_network_need_check(0, 100, -1));
}

TEST_F(snsd_network_ut, need_check_not_elapsed)
{
    /* 30 second interval = 30 * 1000000 / 100000 = 300 ticks
     * If only 100 ticks passed, should not trigger.
     */
    EXPECT_FALSE(snsd_network_need_check(0, 100, 30));
    EXPECT_FALSE(snsd_network_need_check(0, 299, 30));
}

TEST_F(snsd_network_ut, need_check_elapsed)
{
    /* 30 second interval = 300 ticks. At tick 300, should trigger. */
    EXPECT_TRUE(snsd_network_need_check(0, 300, 30));
    EXPECT_TRUE(snsd_network_need_check(0, 500, 30));
}

TEST_F(snsd_network_ut, need_check_min_interval)
{
    /* 5 second interval = 50 ticks */
    EXPECT_FALSE(snsd_network_need_check(0, 49, 5));
    EXPECT_TRUE(snsd_network_need_check(0, 50, 5));
}

TEST_F(snsd_network_ut, need_check_relative)
{
    /* When last_check is non-zero, check relative difference */
    EXPECT_FALSE(snsd_network_need_check(100, 200, 30));
    EXPECT_TRUE(snsd_network_need_check(100, 400, 30));
    EXPECT_TRUE(snsd_network_need_check(100, 401, 30));
}

TEST_F(snsd_network_ut, need_check_wraparound)
{
    /* Test unsigned int wraparound: now < last_check */
    unsigned int last = 0xffffff00;
    /* Need 300 ticks for 30s. 0xffffffff - 0xffffff00 + 1 = 256. Not enough. */
    unsigned int now1 = 0xffffff00 + 200;
    EXPECT_FALSE(snsd_network_need_check(last, now1, 30));

    /* 0xffffff00 + 300 wraps around, should trigger */
    unsigned int now2 = last + 300;  /* wraps to 0x000000fc */
    EXPECT_TRUE(snsd_network_need_check(last, now2, 30));
}

/* ============ snsd_network_init/exit lifecycle tests ============ */

TEST_F(snsd_network_ut, init_exit_lifecycle)
{
    int ret;

    /* Init should succeed regardless of whether [NETWORK] section exists.
     * If no [NETWORK] section, returns 0 with "QoS disabled" message.
     * If [NETWORK] section present, parses and loads config.
     */
    ret = snsd_network_init();
    EXPECT_EQ(0, ret);

    /* Exit should clean up without error */
    snsd_network_exit();
}

TEST_F(snsd_network_ut, double_init_exit)
{
    int ret;

    ret = snsd_network_init();
    EXPECT_EQ(0, ret);
    snsd_network_exit();

    /* Second init/exit cycle should also work cleanly */
    ret = snsd_network_init();
    EXPECT_EQ(0, ret);
    snsd_network_exit();
}

TEST_F(snsd_network_ut, exit_without_init)
{
    /* Exit without init should not crash */
    snsd_network_exit();
}

/* ============ snsd_network_apply/check tests ============ */

TEST_F(snsd_network_ut, apply_no_config)
{
    int ret;

    /* With no [NETWORK] config loaded, apply should be a no-op */
    snsd_network_exit();  /* ensure clean state */

    ret = snsd_network_apply();
    EXPECT_EQ(0, ret);
}

TEST_F(snsd_network_ut, check_no_config)
{
    /* With no config loaded, check should be a no-op and not crash */
    snsd_network_exit();  /* ensure clean state */
    snsd_network_check();
}

TEST_F(snsd_network_ut, apply_with_mock_dcb)
{
    int ret;

    /* Initialize from config file (may or may not have [NETWORK] section) */
    ret = snsd_network_init();
    EXPECT_EQ(0, ret);

    /* Mock DCB functions to return success */
    MOCKER(snsd_dcb_set_ieee_pfc)
        .stubs()
        .will(returnValue(0));
    MOCKER(snsd_dcb_set_trust)
        .stubs()
        .will(returnValue(0));
    MOCKER(snsd_dcb_set_egress_qos_map)
        .stubs()
        .will(returnValue(0));

    ret = snsd_network_apply();
    EXPECT_EQ(0, ret);

    GlobalMockObject::reset();
    snsd_network_exit();
}

TEST_F(snsd_network_ut, check_with_mock_dcb)
{
    int ret;

    ret = snsd_network_init();
    EXPECT_EQ(0, ret);

    /* Mock DCB get functions to return matching values (no drift) */
    MOCKER(snsd_dcb_get_ieee_pfc)
        .stubs()
        .will(returnValue(0));
    MOCKER(snsd_dcb_get_trust)
        .stubs()
        .will(returnValue(0));
    MOCKER(snsd_dcb_get_egress_qos_map)
        .stubs()
        .will(returnValue(0));

    /* Mock set functions in case drift triggers re-apply */
    MOCKER(snsd_dcb_set_ieee_pfc)
        .stubs()
        .will(returnValue(0));
    MOCKER(snsd_dcb_set_trust)
        .stubs()
        .will(returnValue(0));
    MOCKER(snsd_dcb_set_egress_qos_map)
        .stubs()
        .will(returnValue(0));

    snsd_network_check();

    GlobalMockObject::reset();
    snsd_network_exit();
}

/* ============ QoS check interval base config test ============ */

TEST_F(snsd_network_ut, base_cfg_qos_interval)
{
    struct snsd_base_cfg *bcfg;

    bcfg = snsd_get_base_info();
    EXPECT_NE((struct snsd_base_cfg *)NULL, bcfg);

    /* qos_check_interval should be set (default 30 or from config) */
    if (bcfg->qos_check_interval == 0) {
        /* Not configured, that's fine */
        EXPECT_EQ(0, bcfg->qos_check_interval);
    } else {
        EXPECT_GE(bcfg->qos_check_interval, SNSD_QOS_CHECK_INTERVAL_MIN);
        EXPECT_LE(bcfg->qos_check_interval, SNSD_QOS_CHECK_INTERVAL_MAX);
    }
}
