/*
 * Copyright (C) 2021, Amazon, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library;  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "testutils.h"

#include "virlog.h"

#ifdef WITH_FC

#include "vircommand.h"

# define VIR_FROM_THIS VIR_FROM_NONE

VIR_LOG_INIT("tests.firecrackerdrivertest");

static int
downloadFile(const char *download_link,
             const char *save_file_path)
{
    virCommand *cmd = virCommandNew("curl");

    virCommandAddArg(cmd, "-fsSL");
    virCommandAddArg(cmd, "-o");
    virCommandAddArg(cmd, save_file_path);
    virCommandAddArg(cmd, download_link);

    if (virCommandRun(cmd, NULL) < 0) {
        return -1;
    }

    virCommandFree(cmd);

    return 0;
}

static int
downloadKernelAndRootFS(void)
{
    const char *arch = virArchToString(virArchFromHost());
    g_autofree const char *kernel_url = g_strdup_printf(
        "https://s3.amazonaws.com/spec.ccfc.min/img/quickstart_guide/%s/kernels/vmlinux.bin",
        arch);
    g_autofree const char *rootfs_url = g_strdup_printf(
        "https://s3.amazonaws.com/spec.ccfc.min/img/quickstart_guide/%s/rootfs/bionic.rootfs.ext4",
        arch);

    g_autofree const char *download_kernel_path = g_strdup_printf(
        "%s/firecrackerdriverdata/hello-vmlinux-test.bin",
        abs_srcdir);
    g_autofree const char *download_rootfs_path = g_strdup_printf(
        "%s/firecrackerdriverdata/hello-rootfs-test.ext4",
        abs_srcdir);

    if (downloadFile(kernel_url, download_kernel_path) < 0) {
        VIR_WARN("Error downloading test kernel from link: %s",
                 kernel_url);
        return -1;
    }

    if (downloadFile(rootfs_url, download_rootfs_path) < 0) {
        VIR_WARN("Error downloading test rootfs from link: %s",
                 rootfs_url);
        return -1;
    }

    return 0;
}

static int
testDriverConnection(const void *opaque G_GNUC_UNUSED)
{
    int ret = 0;

    virConnectPtr conn = virConnectOpen("fc:///system");

    if (conn == NULL)
        ret = -1;

    if (virConnectClose(conn) < 0)
        ret = -1;

    return ret;
}

static virConnectPtr test_conn;

static int
testInitiateConnection(const void *opaque G_GNUC_UNUSED)
{
    test_conn = virConnectOpen("fc:///system");

    if (test_conn == NULL)
        return -1;

    return 0;
}

static int
testDefineDomain(const void *opaque G_GNUC_UNUSED)
{
    g_autofree const char *path = g_strdup_printf("%s/firecrackerdriverdata/%s.xml",
                                      abs_srcdir,
                                      "test_domain");

    g_autofree char *xml = NULL;
    if (virFileReadAll(path, 10 * 1024 * 1024, &xml) < 0) {
        return -1;
    }

    VIR_DEBUG("xml read: %s", xml);

    if (virDomainDefineXML(test_conn, xml) == NULL) {
        return -1;
    }

    return 0;
}

static int
testCreateDomain(const void *opaque G_GNUC_UNUSED)
{
    virDomainPtr dom = NULL;
    int state = VIR_DOMAIN_NOSTATE;
    int max_retries = 100;

    if ((dom = virDomainLookupByName(test_conn, "firecracker_domain")) == NULL) {
        return -1;
    }

    if (virDomainCreate(dom) < 0) {
        return -1;
    }

    // Usually firecracker is set into running state even if the vm isn't
    // yet ready to receive other commands like 'SendCtrlAltDel'
    g_usleep(2500 * 1000);

    do {
        if (virDomainGetState(dom, &state, NULL, 0) < 0) {
            VIR_DEBUG("Error getting state");
            return -1;
        }

        if (max_retries <= 0)
            return -1;

        --max_retries;
    }
    while (state != VIR_DOMAIN_RUNNING);

    return 0;
}

static int
testShutdownDomain(const void *opaque G_GNUC_UNUSED)
{
    virDomainPtr dom = NULL;
    int state = VIR_DOMAIN_NOSTATE;
    int max_retries = 500;

    if ((dom = virDomainLookupByName(test_conn, "firecracker_domain")) == NULL) {
        return -1;
    }

    if (virDomainShutdown(dom) < 0) {
        return -1;
    }

    do {
        g_usleep(50 * 1000);

        if (virDomainGetState(dom, &state, NULL, 0) < 0) {
            VIR_DEBUG("Error getting state");
            return -1;
        }

        if (max_retries <= 0)
            return -1;

        --max_retries;
    }
    while (state != VIR_DOMAIN_SHUTOFF);

    return 0;
}

static int
testUndefineDomain(const void *opaque G_GNUC_UNUSED)
{
    virDomainPtr dom = NULL;

    if ((dom = virDomainLookupByName(test_conn, "firecracker_domain")) == NULL) {
        return -1;
    }

    if (virDomainUndefine(dom) < 0) {
        return -1;
    }

    return 0;
}

static int
testCloseConnection(const void *opaque G_GNUC_UNUSED)
{
    if (virConnectClose(test_conn) < 0) {
        return -1;
    }

    return 0;
}

# define DO_SUBTEST(TESTFUNC, arg) \
    do { \
        fprintf(stderr, "\n" #TESTFUNC "   ... "); \
        if (TESTFUNC(opaque) < 0) { \
            fprintf(stderr, "FAIL\n"); \
            return -1; \
        } else { \
            fprintf(stderr, "OK\n"); \
        } \
    } while (0)

static int
testLifecycle(const void *opaque G_GNUC_UNUSED)
{
    DO_SUBTEST(testInitiateConnection, opaque);
    DO_SUBTEST(testDefineDomain, opaque);
    DO_SUBTEST(testCreateDomain, opaque);
    DO_SUBTEST(testShutdownDomain, opaque);
    DO_SUBTEST(testUndefineDomain, opaque);
    DO_SUBTEST(testCloseConnection, opaque);

    return 0;
}

static int
testDefineShutdown(const void *opaque G_GNUC_UNUSED)
{
    DO_SUBTEST(testInitiateConnection, opaque);
    DO_SUBTEST(testDefineDomain, opaque);

    if (testShutdownDomain(opaque) == 0) {
        fprintf(stderr, "Should not be able to shutdown a domain that was not started \n");
        return -1;
    }

    DO_SUBTEST(testUndefineDomain, opaque);
    DO_SUBTEST(testCloseConnection, opaque);

    return 0;
}

# define DO_TEST(TESTNAME, NAME) \
    if (virTestRun(TESTNAME, \
                   NAME, NULL) < 0) \
        ret = -1

static int
mymain(void)
{
    int ret = 0;

    DO_TEST("Open and close driver connection ", testDriverConnection);

    if (downloadKernelAndRootFS() < 0) {
        fprintf(stderr, "Could not properly download kernel and rootfs\n");
        return -1;
    }
    DO_TEST("Lifecycle", testLifecycle);

    DO_TEST("Negative test: Shutdown after define -> SHUT_OFF state", testDefineShutdown);

    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIR_TEST_MAIN(mymain)

#else

int
main(void)
{
    return EXIT_AM_SKIP;
}

#endif /* WITH_FC */
