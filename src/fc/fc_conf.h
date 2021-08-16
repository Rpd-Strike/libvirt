/*
 * fc_conf.h: Firecracker configuration management
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

#include "virdomainobjlist.h"

#define FC_DRIVER_EXTERNAL_NAME "Firecracker"
#define FC_CMD "firecracker"

// https://man7.org/linux/man-pages/man3/openpty.3.html#BUGS
// if the bugs section of this man page changes, maybe we can update this macro
#define MAX_PTY_NAME_LENGTH 256

#define MAX_SECONDS_WAITING_UPDATE 10

#define MIN_FIRECRACKER_VERSION (0 * 1000 * 1000  +  25 * 1000  +  0)

typedef struct _virFCDriverConfig virFCDriverConfig;

typedef struct _virFCDriver virFCDriver;

struct _virFCDriverConfig {
    char *stateDir;
};

struct _virFCDriver
{
    /* Require lock to get a reference on the object,
     * lockless access thereafter */
    virMutex lock;

    virCaps *caps;

    virDomainXMLOption *xmlopt;

    virDomainObjList *domains;

    virFCDriverConfig *config;

    unsigned long version;
};

virDomainXMLOption *
fcDomainXMLConfInit(void);

virFCDriverConfig *
virFCDriverConfigNew(bool privileged);

void
virFCDriverConfigFree(virFCDriverConfig *config);

int
fcExtractVersion(virFCDriver *driver);
