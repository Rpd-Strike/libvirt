/*
 * fc_domain.c: Firecracker domain private state
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

#include <fcntl.h>
#include <pty.h>

#include "fc_domain.h"
#include "fc_conf.h"
#include "fc_monitor.h"

#include "virerror.h"
#include "virlog.h"
#include "virfile.h"
#include "virtime.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_FC

VIR_LOG_INIT("fc.fc_domain");

// From the defined disk devices, try to find the one that matches
// the <target dev = '...'> tag to the one given in the <root> element of the os
virDomainDiskDef *
getRootFSDiskDevice(virDomainDef *def)
{
    size_t i = 0;

    if (!def->os.root) {
        return NULL;
    }

    for (i = 0; i < def->ndisks; ++i) {
        if (STREQ(def->os.root, def->disks[i]->dst)) {
            return def->disks[i];
        }
    }

    return NULL;
}

// Callback called after parsing the xml file
// Checks for device requirements specific to firecracker vmm
static int
virFCDomainDefPostParseBasic(virDomainDef *def,
                             void *opaque G_GNUC_UNUSED)
{
    if (virXMLCheckIllegalChars("name", def->name, "\n") < 0) {
        return -1;
    }

    if (!def->emulator) {
        if (!(def->emulator = g_find_program_in_path(FC_CMD))) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("No emulator found for firecracker"));
            return -1;
        }
    }

    // * We check that we have a path to a kernel image
    if (virStringIsEmpty(def->os.kernel)) {
        virReportError(VIR_ERR_XML_INVALID_SCHEMA, "%s",
                       _("Kernel image path not existent or there are only whitespaces"));
        return -1;
    }

    // * We need to have specified the target device logical name for the rootfs
    if (virStringIsEmpty(def->os.root)) {
        virReportError(VIR_ERR_XML_DETAIL, "%s",
                       _("Missing root tag in the os description that specifies logical device name for the rootfs (or only whitespaces)"));
            return -1;
    }

    // * Devices
    if (def->nparallels > 0) {
        virReportError(VIR_ERR_XML_DETAIL, "%s",
                       _("Firecracker doesn't support parallel devices"));
        return -1;
    }
    if (def->nconsoles > 0) {
        virReportError(VIR_ERR_XML_DETAIL, "%s",
                       _("Firecracker doesn't support console devices. A serial device can be configured instead."));
        return -1;
    }
    if (def->nchannels > 0) {
        virReportError(VIR_ERR_XML_DETAIL, "%s",
                       _("Firecracker doesn't support channel devices"));
        return -1;
    }

    // * serial device, we should have none or one serial device configured properly
    if (def->nserials > 1) {
        virReportError(VIR_ERR_XML_DETAIL, "%s",
                       _("Firecracker supports maximum one serial device"));
        return -1;
    }
    if (def->nserials == 1) {
        if (def->serials[0]->deviceType != VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL) {
            virReportError(VIR_ERR_XML_DETAIL, "%s",
                           _("For character devices, Firecracker supports only serial"));
            return -1;
        }
        if (def->serials[0]->source->type != VIR_DOMAIN_CHR_TYPE_PTY) {
            virReportError(VIR_ERR_XML_DETAIL, "%s",
                           _("The type of the serial device needs to be a pseudo terminal ('pty')"));
            return -1;
        }
    }

    // * Checking if there is a correctly configured rootfs disk
    if (getRootFSDiskDevice(def) == NULL) {
        virReportError(VIR_ERR_XML_DETAIL,
                       _("There is no disk device with target '%s'"),
                       def->os.root);
        return -1;
    }

    return 0;
}

virDomainDefParserConfig
virFCDriverDomainDefParserConfig =
{
    .domainPostParseBasicCallback = virFCDomainDefPostParseBasic,
};

static void *
virFCDomainObjPrivateAlloc(void *opaque G_GNUC_UNUSED)
{
    virFCDomainObjPrivate *priv = g_new0(virFCDomainObjPrivate, 1);

    return priv;
}


static void
virFCDomainObjPrivateFree(void *data)
{
    virFCDomainObjPrivate *priv = data;

    g_free(priv->console_pty_path);
    g_free(priv->socketpath);
    g_free(priv->vm_dir);
    g_free(priv);
}

virDomainXMLPrivateDataCallbacks virFCDriverPrivateDataCallbacks = {
    .alloc = virFCDomainObjPrivateAlloc,
    .free = virFCDomainObjPrivateFree,
};

// * 'Helper' functions
// Function used when creating a vm,
void
fcPopulatePrivateData(virFCDriver *driver,
                     virDomainObj *vm)
{
    virFCDomainObjPrivate *vmPrivateData = vm->privateData;

    // full folder path to vm specific folder
    vmPrivateData->vm_dir = g_strdup_printf("%s/%s",
                                            driver->config->stateDir,
                                            vm->def->name);
    // full socket path for the vm
    vmPrivateData->socketpath = g_strdup_printf("%s/firecracker-lv.socket",
                                                vmPrivateData->vm_dir);
}

// Function used to wait until the socket for the firecracker vm gets created
static int
fcWaitUntilExists(const char* path)
{
    bool exists = false;
    int seconds = MAX_SECONDS_WAITING_UPDATE;
    virTimeBackOffVar timeout;

    exists = virFileExists(path);

    if (virTimeBackOffStart(&timeout, 1, seconds * 1000) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("virTimeBackOffStart() returned negative value"));
        return -1;
    }

    while (virTimeBackOffWait(&timeout)) {
        exists = virFileExists(path);

        if (exists) {
            break;
        }
    }

    return exists ? 0 : -1;
}

/**
 * The console argument is added to the cmdline of kernel
 *   if we have one serial device defined in the XML schema,
 *   based on the fact that Firecracker supports only one serial device
 * */
static char *
fcAddAdditionalCmdlineArgs(virDomainObj *vm)
{
    GString *cmdline = g_string_new(vm->def->os.cmdline);

    if (vm->def->nserials > 0) {
        g_string_append_printf(cmdline, " console=ttyS%d", vm->def->serials[0]->target.port);
    }

    return g_string_free(cmdline, false);
}

// * Lifecycle functions
/**
 * Update the libvirt-style status of the vm from the firecracker process
 * If the firecracker process doesn't respond (Proceess is dead, socket doesn't exist, etc)
 *   then we return -1
 * */
int
fcUpdateState(virDomainObj *vm)
{
    virFCDomainObjPrivate *vmPrivateData = vm->privateData;
    virDomainState domainState = virFCMonitorGetStatus(vmPrivateData->socketpath);

    virDomainObjSetState(vm, domainState, 0);

    if (domainState == VIR_DOMAIN_NOSTATE) {
        return -1;
    }

    VIR_DEBUG("vm '%s' updated with state: '%s'",
              vm->def->name,
              virDomainStateTypeToString(domainState));

    return 0;
}

/**
 * Tries to start the Firecracker process for a specified vm
 * If succeeds, then set domain as active by making vm->def->id equal to the pid of firecracker process
 * */
int
fcStartVMProcess(virFCDriver *driver G_GNUC_UNUSED,
                 virDomainObj *vm)
{
    int primaryFD = -1;
    int secondaryFD = -1;
    pid_t fc_pid = 0;
    virFCDomainObjPrivate *vmPrivateData = vm->privateData;
    g_autofree char *out_err_file = g_strdup_printf("%s/fc_err.log",
                                                    vmPrivateData->vm_dir);
    g_autofree char *out_std_file = g_strdup_printf("%s/fc_std.log",
                                                    vmPrivateData->vm_dir);
    int fc_errfd = -1, fc_stdfd = -1;
    virCommand *cmd = NULL;
    const bool open_serial_console = (vm->def->nserials > 0);
    int ret = -1;

    if ((fc_errfd = open(out_err_file, O_CREAT | O_WRONLY | O_APPEND, 0666)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to open() the file for stderr output: %d, (%s)"),
                       errno, g_strerror(errno));
        goto error;
    }

    // * Create PTY device for eventual communication through console
    if (open_serial_console) {
        vmPrivateData->console_pty_path = g_strnfill(MAX_PTY_NAME_LENGTH, ' ');
        if (openpty(&primaryFD, &secondaryFD, vmPrivateData->console_pty_path, NULL, NULL) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("Couldn't create PTY for console access"));
            goto error;
        }

        VIR_DEBUG("pty name created: %s", vmPrivateData->console_pty_path);
    } else {
        vmPrivateData->console_pty_path = NULL;
    }

    cmd = virCommandNew(vm->def->emulator);

    virCommandAddArgList(cmd, "--api-sock", vmPrivateData->socketpath, NULL);

    virCommandSetUmask(cmd, 0x002);

    if (open_serial_console) {
        virCommandSetOutputFD(cmd, &primaryFD);
        virCommandSetInputFD(cmd, primaryFD);
    } else {
        if ((fc_stdfd = open(out_std_file, O_CREAT | O_WRONLY | O_APPEND, 0666)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                        _("Failed to open() the file for stdout output: %d, (%s)"),
                        errno, g_strerror(errno));
            goto error;
        }
        virCommandSetOutputFD(cmd, &fc_stdfd);
    }
    virCommandSetErrorFD(cmd, &fc_errfd);

    if (virCommandRunAsync(cmd, &fc_pid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("virCommandRunAsync() returned a negative response code"));
        goto error;
    }

    // * Wait until socket created by firecracker is created
    if (fcWaitUntilExists(vmPrivateData->socketpath) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Socket file for the vm couldn't be verified to exist"));
        goto error;
    }

    // Because of the process creating the socket is considered root,
    // in order for subsequent writes and reads to the socket we need to change the permissions
    // to account for the RPC calls having other users
    if (virFileUpdatePerm(vmPrivateData->socketpath, 0000, 0666) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("cannot modify permissions for the socket"));
        VIR_DEBUG("cannot modify permissions for the socket");
    } else {
        VIR_DEBUG("Permissions modified for the socket!");
    }

    ret = 0;

    vm->def->id = fc_pid;
    vmPrivateData->fc_process = cmd;

    return ret;

 error:
    vm->def->id = -1;
    virCommandAbort(cmd);
    vmPrivateData->fc_process = NULL;

    return ret;
}

// Looks into the vm definition and calls the corresponding endpoints
// to set the pre-boot parameters properly.
// After that it starts the vm
int
fcConfigAndStartVM(virFCDriver *driver G_GNUC_UNUSED,
                   virDomainObj *vm)
{
    virFCDomainObjPrivate *vmPrivateData = vm->privateData;
    virDomainDiskDef *root_device = NULL;
    g_autofree gchar *computed_cmdline = NULL;

    // * Before starting the vm, firecracker needs to be properly configured via http requests
    computed_cmdline = fcAddAdditionalCmdlineArgs(vm);
    if (virFCMonitorSetKernel(vmPrivateData->socketpath,
                              vm->def->os.kernel,
                              computed_cmdline) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Firecracker API call failed setting kernel and cmdline"));
        return -1;
    }

    // We need a disk device that matches the 'dev' property on the
    // 'target' tag with the name given in the 'os.root' tag
    root_device = getRootFSDiskDevice(vm->def);

    if (root_device == NULL) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Did not find a disk device with target destination '%s'"), vm->def->os.root);
        return -1;
    }

    if (virFCMonitorSetDisk(vmPrivateData->socketpath, "rootfs",
                            root_device->src->path, true, false) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Firecracker API call failed setting rootfs"));
        return -1;
    }

    if (virFCMonitorStartVM(vmPrivateData->socketpath) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Firecracker API call failed starting the vm"));
        return -1;
    }

    return 0;
}

// This method sends the url request to shutdown the vm
// and sets the state to SHUTOFF for libvirt
int
fcStopVM(virFCDriver *driver G_GNUC_UNUSED,
         virDomainObj *vm,
         virDomainShutoffReason reason)
{
    virFCDomainObjPrivate *vmPrivateData = vm->privateData;

    if (virFCMonitorShutdownVM(vmPrivateData->socketpath) < 0) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("Firecracker API call failed or received an error for Shutting down the vm"));
        return -1;
    }

    virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF, reason);

    vm->def->id = -1;

    return 0;
}
