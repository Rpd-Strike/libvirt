/*
 * fc_monitor.c: Manage Firecracker interactions
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

#include "fc_conf.h"
#include "fc_monitor.h"

#include "virjson.h"
#include "virlog.h"
#include "virerror.h"

#define URL_CONFIG_PREBOOT "machine-config"
#define URL_CONFIG_KERNEL "boot-source"
#define URL_CONFIG_DISK "drives"
#define URL_CONFIG_NETWORK "network-interfaces"
#define URL_ACTIONS "actions"
#define URL_VM "vm"

#define VIR_FROM_THIS VIR_FROM_FC

VIR_LOG_INIT("fc.fc_monitor");

// Currently firecracker API returns 200 or 204 for success codes
static bool
isSuccessCode(long response_code)
{
    return response_code == 200 || response_code == 204;
}

// Function that parses a given httpResponse from the request that asks for the state of the vm
// and returns the corresponding libvirt virDomainState enum
static virDomainState
fcInstanceInfoToDomainState(const char *httpResponse)
{
    virJSONValue_autoptr jsonObj = virJSONValueFromString(httpResponse);
    const char *state = NULL;

    if (jsonObj == NULL) {
        virReportError(VIR_ERR_PARSE_FAILED, "%s",
                       _("Failed to parse http response as json"));
        return VIR_DOMAIN_NOSTATE;
    }

    if ((state = virJSONValueObjectGetString(jsonObj, "state")) == NULL) {
        virReportError(VIR_ERR_PARSE_FAILED, "%s",
                       _("Failed to parse key-value pair in json for finding the vm state"));
        return VIR_DOMAIN_NOSTATE;
    }

    if (STREQ(state, "Running")) {
        return VIR_DOMAIN_RUNNING;
    }

    if (STREQ(state, "Paused")) {
        return VIR_DOMAIN_PAUSED;
    }

    if (STREQ(state, "Not started")) {
        return VIR_DOMAIN_SHUTOFF;
    }

    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("Could not find the state of the vm"));

    return VIR_DOMAIN_NOSTATE;
}

static long
virFCMonitorCurlExec(CURL *handle)
{
    CURLcode err_code;
    long response_code = 0;

    err_code = curl_easy_perform(handle);

    if (err_code != CURLE_OK) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("curl_easy_perform() returned an error: %s (%d)"),
                       curl_easy_strerror(err_code), err_code);
        return -1;
    }

    err_code = curl_easy_getinfo(handle,
                                 CURLINFO_RESPONSE_CODE,
                                 &response_code);

    if (err_code != CURLE_OK) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("curl_easy_getinfo(CURLINFO_RESPONSE_CODE) returned an "
                         "error: %s (%d)"), curl_easy_strerror(err_code),
                       err_code);
        return -1;
    }

    if (response_code < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("curl_easy_getinfo(CURLINFO_RESPONSE_CODE) returned a "
                         "negative response code"));
        return -1;
    }

    VIR_DEBUG("Response code: %ld", response_code);

    return response_code;
}

static int
virFCJsonActionExec(const char *unix_path,
                    const char *url_endpoint,
                    const char *http_action,
                    virJSONValue_autoptr jsonObj)
{
    CURL *handle = curl_easy_init();
    struct curl_slist *headers = NULL;
    g_autofree char *url = g_strdup_printf("%s/%s", URL_ROOT, url_endpoint);
    g_autofree char *jsonObjString = virJSONValueToString(jsonObj, false);
    long response_code = 0;

    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");

    VIR_DEBUG("JSON string: %s", jsonObjString);
    VIR_DEBUG("socket path in request: %s", unix_path);
    VIR_DEBUG("Request URL: %s", url);

    curl_easy_setopt(handle, CURLOPT_UNIX_SOCKET_PATH, unix_path);
    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, http_action);
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, jsonObjString);

    response_code = virFCMonitorCurlExec(handle);

    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);

    return response_code;
}

int virFCMonitorSetConfig(const char *socketpath,
                          bool hyper_threading,
                          virDomainDef *vmdef)
{
    virJSONValue_autoptr jsonObj = virJSONValueNewObject();
    long response_code = 0;
    int ret = -1;
    int mem_mb = virDomainDefGetMemoryInitial(vmdef) / 1024;
    int maxvcpus = 0;

    VIR_DEBUG("Memory in mb: %d", mem_mb);

    /* Currently firecracker doesn't support cpu hot plugging,
     * so we always use the maximum amount of vcpus */
    maxvcpus = virDomainDefGetVcpusMax(vmdef);

    virJSONValueObjectAppendBoolean(jsonObj, "ht_enabled", hyper_threading);
    virJSONValueObjectAppendNumberInt(jsonObj, "mem_size_mib", mem_mb);
    virJSONValueObjectAppendNumberInt(jsonObj, "vcpu_count", maxvcpus);

    response_code = virFCJsonActionExec(socketpath, URL_CONFIG_PREBOOT, "PUT", jsonObj);

    if (isSuccessCode(response_code))
        ret = 0;

    return ret;
}

int
virFCMonitorSetKernel(const char *socketpath,
                      char *kernel_path,
                      char *kernel_cmdline)
{
    virJSONValue_autoptr jsonObj = virJSONValueNewObject();
    long response_code = 0;
    int ret = -1;

    virJSONValueObjectAppendString(jsonObj, "kernel_image_path", kernel_path);
    virJSONValueObjectAppendString(jsonObj, "boot_args", kernel_cmdline);

    response_code = virFCJsonActionExec(socketpath, URL_CONFIG_KERNEL, "PUT", jsonObj);

    if (isSuccessCode(response_code))
        ret = 0;

    return ret;
}

int
virFCMonitorSetDisk(const char *socketpath,
                    const char *drive_id,
                    char* disk_path_host,
                    const bool is_root_device,
                    const bool is_read_only)
{
    virJSONValue_autoptr jsonObj = virJSONValueNewObject();
    g_autofree char *url = g_strdup_printf("/%s/%s", URL_CONFIG_DISK, drive_id);
    long response_code = 0;
    int ret = -1;

    virJSONValueObjectAppendString(jsonObj, "drive_id", drive_id);
    virJSONValueObjectAppendString(jsonObj, "path_on_host", disk_path_host);
    virJSONValueObjectAppendBoolean(jsonObj, "is_root_device", is_root_device);
    virJSONValueObjectAppendBoolean(jsonObj, "is_read_only", is_read_only);

    response_code = virFCJsonActionExec(socketpath, url, "PUT", jsonObj);

    if (isSuccessCode(response_code))
        ret = 0;

    return ret;
}

int
virFCMonitorStartVM(const char* socketpath)
{
    virJSONValue_autoptr jsonObj = virJSONValueNewObject();
    long response_code = 0;
    int ret = -1;

    virJSONValueObjectAppendString(jsonObj, "action_type", "InstanceStart");

    response_code = virFCJsonActionExec(socketpath, URL_ACTIONS, "PUT", jsonObj);

    if (isSuccessCode(response_code))
        ret = 0;

    return ret;
}

int
virFCMonitorShutdownVM(const char *socketpath)
{
    virJSONValue_autoptr jsonObj = virJSONValueNewObject();
    long response_code = 0;
    int ret = -1;

    virJSONValueObjectAppendString(jsonObj, "action_type", "SendCtrlAltDel");

    response_code = virFCJsonActionExec(socketpath, URL_ACTIONS, "PUT", jsonObj);

    if (isSuccessCode(response_code))
        ret = 0;

    return ret;
}

int
virFCMonitorChangeState(const char *socketpath,
                        const char *state)
{
    virJSONValue_autoptr jsonObj = virJSONValueNewObject();
    long response_code = 0;
    int ret = -1;

    if (STRNEQ(state, "Paused") &&
        STRNEQ(state, "Resumed")) {

        virReportError(VIR_ERR_INVALID_ARG,
                       _("Domain can not transition into invalid state '%s'"),
                       state);
        goto cleanup;
    }

    virJSONValueObjectAppendString(jsonObj, "state", state);

    response_code = virFCJsonActionExec(socketpath, URL_VM, "PATCH", jsonObj);

    if (isSuccessCode(response_code))
        ret = 0;

 cleanup:
    return ret;
}

int
virFCMonitorSetNetwork(const char *socketpath,
                              const char *iface_id,
                              const char *guest_mac,
                              const char *host_dev_name,
                              const bool allow_mmds_requests)
{
    virJSONValue_autoptr jsonObj = virJSONValueNewObject();
    g_autofree char *url = g_strdup_printf("/%s/%s", URL_CONFIG_NETWORK, iface_id);
    int response_code = 0;
    int ret = -1;

    virJSONValueObjectAppendBoolean(jsonObj, "allow_mmds_requests", allow_mmds_requests);
    virJSONValueObjectAppendString(jsonObj, "guest_mac", guest_mac);
    virJSONValueObjectAppendString(jsonObj, "host_dev_name", host_dev_name);
    virJSONValueObjectAppendString(jsonObj, "iface_id", iface_id);

    response_code = virFCJsonActionExec(socketpath, url, "PUT", jsonObj);

    if (isSuccessCode(response_code))
        ret = 0;

    return ret;
}

// we need a function to pass to curl library so we can parse the httpResponse
static size_t
writedataCurlCallback(const char *in,
                      size_t size,
                      size_t num,
                      virBuffer *buffer)
{
    size_t realsize = size * num;

    VIR_DEBUG("inside writedatacurlcallback: %s", in);
    VIR_DEBUG("Size, num: %lu, %lu", size, num);

    virBufferAdd(buffer, in, realsize);

    return realsize;
}

virDomainState
virFCMonitorGetStatus(const char *socketpath)
{
    CURL *handle = curl_easy_init();
    struct curl_slist *headers = NULL;
    g_autofree char *url = NULL;
    g_auto(virBuffer) recvBuffer = VIR_BUFFER_INITIALIZER;
    g_autofree const char* httpResponse = NULL;
    long response_code = 0;
    virDomainState ret = VIR_DOMAIN_NOSTATE;

    url = g_strdup_printf("%s/", URL_ROOT);

    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(handle, CURLOPT_UNIX_SOCKET_PATH, socketpath);
    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writedataCurlCallback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &recvBuffer);

    response_code = virFCMonitorCurlExec(handle);

    VIR_DEBUG("Before current content recvbuffer");

    if (!isSuccessCode(response_code)) {
        goto cleanup;
    }

    httpResponse = virBufferContentAndReset(&recvBuffer);

    VIR_DEBUG("Get status curl request response: %s", httpResponse);

    ret = fcInstanceInfoToDomainState(httpResponse);

 cleanup:
    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);

    return ret;
}
