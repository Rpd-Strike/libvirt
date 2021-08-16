/*
 * fc_domain.h: Firecracker domain private state
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

#pragma once

#include "virconftypes.h"
#include "vircommand.h"

#include "fc_conf.h"

extern virDomainDefParserConfig virFCDriverDomainDefParserConfig;
extern virDomainXMLPrivateDataCallbacks virFCDriverPrivateDataCallbacks;

typedef struct _virFCDomainObjPrivate virFCDomainObjPrivate;

struct _virFCDomainObjPrivate {
    char *console_pty_path;
    char *vm_dir;
    char *socketpath;

    // Holds the virCommand structure used to start the child Firecracker
    // process so we can clean up at shutdown / destroy
    virCommand *fc_process;
};

virDomainDiskDef *
getRootFSDiskDevice(virDomainDef *vm);

void
fcPopulatePrivateData(virFCDriver *driver,
                      virDomainObj *vm);

int
fcUpdateState(virDomainObj *vm);

int
fcStartVMProcess(virFCDriver *driver,
                 virDomainObj *vm);

int
fcConfigAndStartVM(virFCDriver *driver,
                   virDomainObj *vm);

int
fcStopVM(virFCDriver *driver,
         virDomainObj *vm,
         virDomainShutoffReason reason);
