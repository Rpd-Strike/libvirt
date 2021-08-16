/*
 * fc_driver.c: Core driver methods for managing Firecracker guests
 *
 * Copyright (C) 2021 Amazon.com, Inc. or its affiliates.
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
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <curl/curl.h>

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <pty.h>

#include "fc_driver.h"
#include "fc_conf.h"
#include "fc_domain.h"
#include "fc_monitor.h"

#include "datatypes.h"
#include "driver.h"
#include "virlog.h"
#include "virutil.h"
#include "virfdstream.h"
#include "virfile.h"
#include "viraccessapicheck.h"

#define VIR_FROM_THIS VIR_FROM_FC

VIR_LOG_INIT("fc.fc_driver");

static virFCDriver *fc_driver;

// * "Helper" functions

// Deletes the associateed folder for the vm
static int
fcDeleteVMDir(virDomainObj *vm)
{
    const char *vm_directory = ((virFCDomainObjPrivate *)vm->privateData)->vm_dir;

    if (virFileDeleteTree(vm_directory) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not delete directory: %s"), vm_directory);
        return -1;
    }

    return 0;
}

// Creates the associated folder for the vm
//   and sets correct permissions for the folder so that users can use the files inside
static int
fcCreateVMDir(virDomainObj *vm)
{
    const char *vm_directory = ((virFCDomainObjPrivate *)vm->privateData)->vm_dir;

    if (g_mkdir_with_parents(vm_directory, 0777) < 0) {
        virReportSystemError(errno,
                             _("Cannot create vm directory: '%s'"),
                             vm_directory);
        return -1;
    }

    return 0;
}

// Deletes and creates the folder, so we have a clean start when creating a domain
static int
fcRecreateVMDir(virDomainObj *vm)
{
    if (fcDeleteVMDir(vm) < 0) {
        return -1;
    }

    if (fcCreateVMDir(vm) < 0) {
        return -1;
    }

    return 0;
}

// Function used after destroy method to cleanup eventual firecracker artifacts
// left over by the starting process of the vm and also while running the vm
static int
fcFirecrackerCleanup(virFCDomainObjPrivate *vmPrivateData)
{
    VIR_DEBUG("deleting: %s", vmPrivateData->socketpath);

    if (g_remove(vmPrivateData->socketpath) < 0) {
        return -1;
    }

    return 0;
}

static void
fcDriverLock(virFCDriver *driver)
{
    virMutexLock(&driver->lock);
}

static void
fcDriverUnlock(virFCDriver *driver)
{
    virMutexUnlock(&driver->lock);
}

// * HypervisorDriver functions
static int
fcConnectURIProbe(char **uri)
{
    if (fc_driver == NULL)
        return 0;

    *uri = g_strdup("fc:///system");
    return 1;
}

static virDrvOpenStatus
fcConnectOpen(virConnectPtr conn,
              virConnectAuthPtr auth G_GNUC_UNUSED,
              virConf *conf G_GNUC_UNUSED,
              unsigned int flags)
{
    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (fc_driver == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Firecracker state driver is not active"));
        return VIR_DRV_OPEN_ERROR;
    }

    if (virConnectOpenEnsureACL(conn) < 0) {
        return VIR_DRV_OPEN_ERROR;
    }

    conn->privateData = fc_driver;

    return VIR_DRV_OPEN_SUCCESS;
}

static int
fcConnectClose(virConnectPtr conn)
{
    conn->privateData = NULL;

    return 0;
}

static virDomainObj *
fcDomainObjFromDomain(virDomainPtr domain)
{
    virDomainObj *vm;
    virFCDriver *driver = domain->conn->privateData;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    vm = virDomainObjListFindByUUID(driver->domains, domain->uuid);
    if (!vm) {
        virUUIDFormat(domain->uuid, uuidstr);
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%s' (%s)"),
                       uuidstr, domain->name);
        return NULL;
    }

    return vm;
}

static int
fcDomainOpenConsole(virDomainPtr dom,
                    const char *dev_name G_GNUC_UNUSED,
                    virStreamPtr st,
                    unsigned int flags)
{
    virDomainObj *vm = NULL;
    virFCDomainObjPrivate *vmPrivateData = NULL;
    int ret = -1;

    VIR_DEBUG("Firecracker Domain Open Console");

    virCheckFlags(0, -1);

    if (!(vm = fcDomainObjFromDomain(dom))) {
        goto cleanup;
    }

    vmPrivateData = vm->privateData;

    if (virDomainOpenConsoleEnsureACL(dom->conn, vm->def) < 0) {
        goto cleanup;
    }

    if (virDomainObjCheckActive(vm) < 0)
        goto cleanup;

    if (vm->def->nconsoles < 1) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("Domain '%s' didn't boot with a serial console"),
                       vm->def->name);
        goto cleanup;
    }

    VIR_DEBUG("Connecting to Console Device name: %s", vmPrivateData->console_pty_path);

    if (virFDStreamOpenPTY(st, vmPrivateData->console_pty_path,
                           0, 0, O_RDWR) < 0) {
        VIR_DEBUG("virFDStreamOpenPTY FAILED");
        goto cleanup;
    }

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
fcConnectNumOfDomains(virConnectPtr conn)
{
    virFCDriver *driver = conn->privateData;
    int cnt;

    if (virConnectNumOfDomainsEnsureACL(conn) < 0)
        return -1;

    fcDriverLock(driver);
    cnt =  virDomainObjListNumOfDomains(driver->domains, true,
                                        virConnectNumOfDomainsCheckACL, conn);
    fcDriverUnlock(driver);

    return cnt;
}

static int
fcConnectListDomains(virConnectPtr conn,
                     int *ids,
                     int maxids)
{
    virFCDriver *driver = conn->privateData;
    int cnt;

    if (virConnectListDomainsEnsureACL(conn) < 0) {
        return -1;
    }

    fcDriverLock(driver);
    cnt = virDomainObjListGetActiveIDs(driver->domains, ids, maxids,
                                       virConnectListDomainsCheckACL, conn);
    fcDriverUnlock(driver);

    return cnt;
}

static int
fcConnectListAllDomains(virConnectPtr conn,
                        virDomainPtr **domains,
                        unsigned int flags)
{
    virFCDriver *driver = conn->privateData;
    int ret = -1;

    VIR_INFO("fcConnect List all Domains FC driver");

    virCheckFlags(VIR_CONNECT_LIST_DOMAINS_FILTERS_ALL, -1);

    if (virConnectListAllDomainsEnsureACL(conn) < 0) {
        return -1;
    }

    fcDriverLock(driver);
    ret = virDomainObjListExport(driver->domains, conn, domains,
                                 virConnectListAllDomainsCheckACL, flags);
    fcDriverUnlock(driver);

    return ret;
}

static int
fcDomainIsActive(virDomainPtr dom)
{
    virDomainObj *vm;
    int ret = -1;

    if (!(vm = fcDomainObjFromDomain(dom))) {
        goto cleanup;
    }

    if (virDomainIsActiveEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    ret = virDomainObjIsActive(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
fcDomainGetState(virDomainPtr domain,
                 int *state,
                 int *reason,
                 unsigned int flags)
{
    virDomainObj *vm = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = fcDomainObjFromDomain(domain))) {
        goto cleanup;
    }

    if (virDomainGetStateEnsureACL(domain->conn, vm->def) < 0) {
        goto cleanup;
    }

    if (fcUpdateState(vm) < 0) {
        virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF, VIR_DOMAIN_SHUTOFF_UNKNOWN);
    }

    *state = virDomainObjGetState(vm, reason);

    VIR_DEBUG("fcDomainGetState updated: %s", virDomainStateTypeToString(*state));

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
fcDomainGetInfo(virDomainPtr domain,
                virDomainInfoPtr info)
{
    virDomainObj *vm;
    int ret = -1;

    if (!(vm = fcDomainObjFromDomain(domain)))
        goto cleanup;

    if (virDomainGetInfoEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    info->state = virDomainObjGetState(vm, NULL);

    // TODO: cpuTime Can be implemented in a future patch
    info->cpuTime = 0;
    info->nrVirtCpu = virDomainDefGetVcpus(vm->def);

    info->maxMem = virDomainDefGetMemoryTotal(vm->def);
    info->memory = info->maxMem;

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static virDomainPtr
fcDomainDefineXMLFlags(virConnectPtr conn,
                       const char *xml,
                       unsigned int flags)
{
    virFCDriver *driver = conn->privateData;
    virDomainDef *vmdef = NULL;
    virDomainObj *vm = NULL;
    virDomainPtr dom = NULL;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_INACTIVE;

    virCheckFlags(VIR_DOMAIN_DEFINE_VALIDATE, NULL);

    if (flags & VIR_DOMAIN_DEFINE_VALIDATE) {
        parse_flags |= VIR_DOMAIN_DEF_PARSE_VALIDATE_SCHEMA;
    }

    fcDriverLock(driver);

    if ((vmdef = virDomainDefParseString(xml, driver->xmlopt,
                                         NULL, parse_flags)) == NULL) {
        goto cleanup;
    }

    if (virDomainDefineXMLFlagsEnsureACL(conn, vmdef) < 0) {
        goto cleanup;
    }

    if (!(vm = virDomainObjListAdd(driver->domains, vmdef,
                                   driver->xmlopt,
                                   0, NULL))) {
        goto cleanup;
    }

    VIR_DEBUG("def->os.type             %s", virDomainOSTypeToString(vmdef->os.type));
    VIR_DEBUG("def->os.arch             %s", virArchToString(vmdef->os.arch));
    VIR_DEBUG("def->os.machine          %s", vmdef->os.machine);
    VIR_DEBUG("def->os.init             %s", vmdef->os.init);
    VIR_DEBUG("def->os.kernel           %s", vmdef->os.kernel);
    VIR_DEBUG("def->os.initrd           %s", vmdef->os.initrd);
    VIR_DEBUG("def->os.cmdline          %s", vmdef->os.cmdline);
    VIR_DEBUG("def->os.root             %s", vmdef->os.root);

    vmdef = NULL;
    vm->persistent = 1;

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 cleanup:
    virDomainDefFree(vmdef);
    virDomainObjEndAPI(&vm);
    fcDriverUnlock(driver);
    return dom;
}

static virDomainPtr
fcDomainDefineXML(virConnectPtr conn,
                  const char *xml)
{
    return fcDomainDefineXMLFlags(conn, xml, 0);
}

static int
fcDomainCreateWithFlags(virDomainPtr dom,
                        unsigned int flags)
{
    virFCDriver *driver = dom->conn->privateData;
    virFCDomainObjPrivate *vmPrivateData = NULL;
    virDomainObj *vm;
    int ret = -1;

    virCheckFlags(0, -1);

    fcDriverLock(driver);

    if (!(vm = fcDomainObjFromDomain(dom))) {
        goto error;
    }

    if (virDomainCreateWithFlagsEnsureACL(dom->conn, vm->def) < 0) {
        goto error;
    }

    if (virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("Domain is already running"));
        goto error;
    }

    fcPopulatePrivateData(driver, vm);
    vmPrivateData = vm->privateData;

    if (fcRecreateVMDir(vm) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Couldn't create vm directory: %s"),
                        vmPrivateData->vm_dir);
        goto error;
    }

    if (fcStartVMProcess(driver, vm) < 0) {
        VIR_DEBUG("Failed starting Firecracker process");
        goto error;
    }

    if (fcConfigAndStartVM(driver, vm) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed starting the vm"));
        goto error;
    }

    virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, VIR_DOMAIN_RUNNING_BOOTED);

    ret = 0;

    goto cleanup;

 error:
    if (vm) {
        vm->def->id = -1;
        vmPrivateData = vm->privateData;
        virCommandAbort(vmPrivateData->fc_process);
        fcDeleteVMDir(vm);
    }

 cleanup:
    virDomainObjEndAPI(&vm);
    fcDriverUnlock(driver);
    return ret;
}

static int
fcDomainCreate(virDomainPtr dom)
{
    return fcDomainCreateWithFlags(dom, 0);
}

static int
fcDomainShutdownFlags(virDomainPtr domain,
                      unsigned int flags)
{
    virFCDriver *driver = domain->conn->privateData;
    virDomainObj *vm = NULL;
    virFCDomainObjPrivate *vmPrivateData = NULL;
    virDomainState vmState = VIR_DOMAIN_NOSTATE;
    int status = 0;
    int ret = -1;

    virCheckFlags(0, -1);

    fcDriverLock(driver);

    if (!(vm = fcDomainObjFromDomain(domain))) {
        goto cleanup;
    }

    if (virDomainShutdownFlagsEnsureACL(domain->conn, vm->def, flags) < 0) {
        goto cleanup;
    }

    vmState = virDomainObjGetState(vm, NULL);

    if (fcUpdateState(vm) < 0) {
        // Check with the state of vm before update state, and if it is shut off then skip this error
        //   because if vm isn't running, the firecracker process is also not running
        if (vmState != VIR_DOMAIN_SHUTOFF) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Failed to refresh the state of vm"));
            goto cleanup;
        }
    }

    if (virDomainObjGetState(vm, NULL) != VIR_DOMAIN_RUNNING) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not in running state"));
        goto cleanup;
    }

    if (fcStopVM(driver, vm, VIR_DOMAIN_SHUTOFF_SHUTDOWN) < 0) {
        goto cleanup;
    }

    if (!vm->persistent) {
        virDomainObjListRemove(driver->domains, vm);
    }

    vmPrivateData = vm->privateData;
    if (virCommandWait(vmPrivateData->fc_process, &status) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Error waiting for child firecracker process to be reaped"));
        goto cleanup;
    }

    virCommandFree(vmPrivateData->fc_process);

    // We do not look at the return value of the cleanup function
    // as it is not that important to get rid of everything
    fcFirecrackerCleanup(vmPrivateData);

    vmPrivateData->fc_process = NULL;

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    fcDriverUnlock(driver);
    return ret;
}

static int
fcDomainShutdown(virDomainPtr domain)
{
    return fcDomainShutdownFlags(domain, 0);
}

static int
fcDomainDestroyFlags(virDomainPtr domain,
                     unsigned int flags)
{
    virDomainObj *vm = NULL;
    virFCDomainObjPrivate *vmPrivateData = NULL;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_DESTROY_GRACEFUL, -1);

    if (flags & VIR_DOMAIN_DESTROY_GRACEFUL) {
        // ignoring possible error because anyway we go to cleanup after shutdown
        ret = fcDomainShutdown(domain);
        goto cleanup;
    }

    if (!(vm = fcDomainObjFromDomain(domain))) {
        goto cleanup;
    }

    if (virDomainDestroyFlagsEnsureACL(domain->conn, vm->def) < 0) {
        goto cleanup;
    }

    vmPrivateData = vm->privateData;

    if (virDomainObjGetState(vm, 0) != VIR_DOMAIN_RUNNING) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not running"));
        goto cleanup;
    }

    virCommandAbort(vmPrivateData->fc_process);

    // We do not look at the return value of the cleanup function
    // as it is not that important to get rid of everything
    fcFirecrackerCleanup(vmPrivateData);

    vm->def->id = -1;
    virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF, VIR_DOMAIN_SHUTOFF_DESTROYED);

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
fcDomainDestroy(virDomainPtr domain)
{
    return fcDomainDestroyFlags(domain, 0);
}

static int
fcDomainSuspend(virDomainPtr domain)
{
    virDomainObj *vm = NULL;
    virFCDomainObjPrivate *vmPrivateData = NULL;
    int ret = -1;

    if (!(vm = fcDomainObjFromDomain(domain))) {
        return -1;
    }

    if (virDomainSuspendEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    if (fcUpdateState(vm) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to refresh the state of vm"));
        goto cleanup;
    }

    if (virDomainObjGetState(vm, NULL) != VIR_DOMAIN_RUNNING) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not in running state"));
        goto cleanup;
    }

    vmPrivateData = vm->privateData;

    if (virFCMonitorChangeState(vmPrivateData->socketpath, "Paused") < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Firecracker API call failed to suspend VM"));
        goto cleanup;
    }

    virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, VIR_DOMAIN_PAUSED_USER);

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
fcDomainResume(virDomainPtr domain)
{
    virDomainObj *vm = NULL;
    virFCDomainObjPrivate *vmPrivateData = NULL;
    int ret = -1;

    if (!(vm = fcDomainObjFromDomain(domain))) {
        return -1;
    }

    if (virDomainResumeEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    if (fcUpdateState(vm) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to refresh the state of vm"));
        goto cleanup;
    }

    if (virDomainObjGetState(vm, NULL) != VIR_DOMAIN_PAUSED) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not in paused state"));
        goto cleanup;
    }

    vmPrivateData = vm->privateData;

    if (virFCMonitorChangeState(vmPrivateData->socketpath, "Resumed") < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Firecracker API call failed to resume VM"));
        goto cleanup;
    }

    virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, VIR_DOMAIN_RUNNING_UNPAUSED);

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
fcDomainUndefineFlags(virDomainPtr dom,
                      unsigned int flags)
{
    virFCDriver *driver = dom->conn->privateData;
    virDomainObj *vm;
    int ret = -1;

    virCheckFlags(0, -1);

    fcDriverLock(driver);

    if (!(vm = fcDomainObjFromDomain(dom))) {
        goto cleanup;
    }

    if (virDomainUndefineFlagsEnsureACL(dom->conn, vm->def) < 0) {
        goto cleanup;
    }

    if (!vm->persistent) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Cannot undefine transient domain"));
        goto cleanup;
    }

    if (virDomainObjIsActive(vm)) {
        vm->persistent = 0;
    } else {
        virDomainObjListRemove(driver->domains, vm);
    }

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    fcDriverUnlock(driver);
    return ret;
}

static int
fcDomainUndefine(virDomainPtr dom)
{
    return fcDomainUndefineFlags(dom, 0);
}

static virDomainPtr
fcDomainLookupByUUID(virConnectPtr conn,
                     const unsigned char *uuid)
{
    virFCDriver *driver = conn->privateData;
    virDomainObj *vm;
    virDomainPtr dom = NULL;

    fcDriverLock(driver);
    vm = virDomainObjListFindByUUID(driver->domains, uuid);
    fcDriverUnlock(driver);

    if (!vm) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("No domain with matching id %s"), uuid);
        goto cleanup;
    }

    if (virDomainLookupByUUIDEnsureACL(conn, vm->def) < 0)
        goto cleanup;

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 cleanup:
    virDomainObjEndAPI(&vm);
    return dom;
}

static virDomainPtr
fcDomainLookupByName(virConnectPtr conn,
                     const char *name)
{
    virFCDriver *driver = conn->privateData;
    virDomainObj *vm;
    virDomainPtr dom = NULL;

    fcDriverLock(driver);
    vm = virDomainObjListFindByName(driver->domains, name);
    fcDriverUnlock(driver);

    if (!vm) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching name '%s'"), name);
        goto cleanup;
    }

    if (virDomainLookupByNameEnsureACL(conn, vm->def) < 0) {
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 cleanup:
    virDomainObjEndAPI(&vm);
    return dom;
}

// TODO: Update versions before sending patch

static virHypervisorDriver fcHypervisorDriver = {
    .name = FC_DRIVER_EXTERNAL_NAME,
    .connectURIProbe = fcConnectURIProbe,
    .connectOpen = fcConnectOpen,                     /* 7.6.0 */
    .connectClose = fcConnectClose,                   /* 7.6.0 */
    .domainOpenConsole = fcDomainOpenConsole,         /* 7.6.0 */
    .connectNumOfDomains = fcConnectNumOfDomains,     /* 7.6.0 */
    .connectListDomains = fcConnectListDomains,       /* 7.6.0 */
    .connectListAllDomains = fcConnectListAllDomains, /* 7.6.0 */
    .domainIsActive = fcDomainIsActive,               /* 7.6.0 */
    .domainGetState = fcDomainGetState,               /* 7.6.0 */
    .domainGetInfo = fcDomainGetInfo,                 /* 7.6.0 */
    .domainDefineXML = fcDomainDefineXML,             /* 7.6.0 */
    .domainDefineXMLFlags = fcDomainDefineXMLFlags,   /* 7.6.0 */
    .domainCreate = fcDomainCreate,                   /* 7.6.0 */
    .domainCreateWithFlags = fcDomainCreateWithFlags, /* 7.6.0 */
    .domainShutdown = fcDomainShutdown,               /* 7.6.0 */
    .domainShutdownFlags = fcDomainShutdownFlags,     /* 7.6.0 */
    .domainDestroy = fcDomainDestroy,                 /* 7.6.0 */
    .domainDestroyFlags = fcDomainDestroyFlags,       /* 7.6.0 */
    .domainSuspend = fcDomainSuspend,                 /* 7.6.0 */
    .domainResume = fcDomainResume,                   /* 7.6.0 */
    .domainUndefine = fcDomainUndefine,               /* 7.6.0 */
    .domainUndefineFlags = fcDomainUndefineFlags,     /* 7.6.0 */
    .domainLookupByUUID = fcDomainLookupByUUID,       /* 7.6.0 */
    .domainLookupByName = fcDomainLookupByName,       /* 7.6.0 */
};

static virConnectDriver fcConnectDriver = {
    .localOnly = true,
    .uriSchemes = (const char *[]){"fc", NULL},
    .hypervisorDriver = &fcHypervisorDriver,
};

// * StateDriver functions
// We chose the stateful model for the driver because the development time was shorter
// Also it is a performance improvement as we do not need to save and load
// the domains before and after each RPC call

static int
fcStateCleanup(void)
{
    if (fc_driver == NULL)
        return -1;

    virObjectUnref(fc_driver->domains);
    virObjectUnref(fc_driver->xmlopt);

    virFCDriverConfigFree(fc_driver->config);
    g_free(fc_driver);

    return 0;
}

static virDrvStateInitResult
fcStateInitialize(bool privileged,
                  const char *root G_GNUC_UNUSED,
                  virStateInhibitCallback callback G_GNUC_UNUSED,
                  void *opaque G_GNUC_UNUSED)
{
    VIR_DEBUG("Driver State initialize firecracker");

    fc_driver = g_new0(virFCDriver, 1);

    if (virMutexInit(&fc_driver->lock) < 0) {
        g_free(fc_driver);
        return VIR_DRV_STATE_INIT_ERROR;
    }

    if (!(fc_driver->domains = virDomainObjListNew())) {
        goto cleanup;
    }

    if (!(fc_driver->xmlopt = fcDomainXMLConfInit())) {
        goto cleanup;
    }

    if (!(fc_driver->config = virFCDriverConfigNew(privileged))) {
        goto cleanup;
    }

    if (fcExtractVersion(fc_driver) < 0) {
        goto cleanup;
    }

    return VIR_DRV_STATE_INIT_COMPLETE;

 cleanup:
    fcStateCleanup();
    return VIR_DRV_STATE_INIT_ERROR;
}

static virStateDriver fcStateDriver = {
    .name = FC_DRIVER_EXTERNAL_NAME,
    .stateInitialize = fcStateInitialize,
    .stateCleanup = fcStateCleanup
};

int
fcRegister(void)
{
    if (virRegisterConnectDriver(&fcConnectDriver,
                                 true) < 0)
        return -1;
    if (virRegisterStateDriver(&fcStateDriver) < 0)
        return -1;

    return 0;
}
