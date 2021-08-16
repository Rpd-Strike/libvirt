/*
 * fc_monitor.h: Header file for managing Firecracker interactions
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

#define URL_ROOT "http://localhost"

int
virFCMonitorSetKernel(const char *socketpath,
                      char *kernel_path,
                      char *kernel_cmdline);

int
virFCMonitorSetDisk(const char *socketpath,
                    const char *drive_id,
                    char* disk_path_host,
                    const bool is_root_device,
                    const bool is_read_only);

int
virFCMonitorStartVM(const char *socketpath);

int
virFCMonitorShutdownVM(const char *socketpath);

int virFCMonitorChangeState(const char *socketpath,
                            const char *state);

virDomainState
virFCMonitorGetStatus(const char *socketpath);
