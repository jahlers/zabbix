/*
** Zabbix
** Copyright (C) 2001-2022 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "checks_simple_vmware.h"

#include "config.h"

#if defined(HAVE_LIBXML2) && defined(HAVE_LIBCURL)

#include"../vmware/vmware.h"

#define ZBX_VMWARE_DATASTORE_SIZE_TOTAL		0
#define ZBX_VMWARE_DATASTORE_SIZE_FREE		1
#define ZBX_VMWARE_DATASTORE_SIZE_PFREE		2
#define ZBX_VMWARE_DATASTORE_SIZE_UNCOMMITTED	3

#define ZBX_DATASTORE_TOTAL			""
#define ZBX_DATASTORE_COUNTER_CAPACITY		0x01
#define ZBX_DATASTORE_COUNTER_USED		0x02
#define ZBX_DATASTORE_COUNTER_PROVISIONED	0x04

#define ZBX_DATASTORE_DIRECTION_READ		0
#define ZBX_DATASTORE_DIRECTION_WRITE		1

#define ZBX_IF_DIRECTION_IN			0
#define ZBX_IF_DIRECTION_OUT			1

static int	vmware_set_powerstate_result(AGENT_RESULT *result)
{
	int	ret = SYSINFO_RET_OK;

	if (NULL != GET_STR_RESULT(result))
	{
		if (0 == strcmp(result->str, "poweredOff"))
			SET_UI64_RESULT(result, 0);
		else if (0 == strcmp(result->str, "poweredOn"))
			SET_UI64_RESULT(result, 1);
		else if (0 == strcmp(result->str, "suspended"))
			SET_UI64_RESULT(result, 2);
		else
			ret = SYSINFO_RET_FAIL;

		UNSET_STR_RESULT(result);
	}

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Purpose: return pointer to Hypervisor data from hashset with uuid          *
 *                                                                            *
 * Parameters: hvs  - [IN] the hashset with all Hypervisors                   *
 *             uuid - [IN] the uuid of Hypervisor                             *
 *                                                                            *
 * Return value: zbx_vmware_hv_t* - the operation has completed successfully  *
 *               NULL             - the operation has failed                  *
 *                                                                            *
 ******************************************************************************/
static zbx_vmware_hv_t	*hv_get(zbx_hashset_t *hvs, const char *uuid)
{
	zbx_vmware_hv_t	*hv, hv_local = {.uuid = (char *)uuid};

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() uuid:'%s'", __func__, uuid);

	hv = (zbx_vmware_hv_t *)zbx_hashset_search(hvs, &hv_local);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%p", __func__, (void *)hv);

	return hv;
}

/******************************************************************************
 *                                                                            *
 * Purpose: return pointer to Datastore data from vector with id              *
 *                                                                            *
 * Parameters: dss - [IN] the vector with all Datastores                      *
 *             id  - [IN] the id of Datastore                                 *
 *                                                                            *
 * Return value:                                                              *
 *        zbx_vmware_datastore_t* - the operation has completed successfully  *
 *        NULL                    - the operation has failed                  *
 *                                                                            *
 ******************************************************************************/
static zbx_vmware_datastore_t	*ds_get(const zbx_vector_vmware_datastore_t *dss, const char *name)
{
	int			i;
	zbx_vmware_datastore_t	ds_cmp;

	ds_cmp.name = (char *)name;

	if (FAIL == (i = zbx_vector_vmware_datastore_bsearch(dss, &ds_cmp, vmware_ds_name_compare)))
		return NULL;

	return dss->values[i];
}

/******************************************************************************
 *                                                                            *
 * Purpose: return pointer to DVSwitch data from vector with uuid             *
 *                                                                            *
 * Parameters: dvss - [IN] the vector with all DVSwitches                     *
 *             uuid - [IN] the id of dvswitch                                 *
 *                                                                            *
 * Return value:                                                              *
 *        zbx_vmware_dvswitch_t* - the operation has completed successfully   *
 *        NULL                    - the operation has failed                  *
 *                                                                            *
 ******************************************************************************/
static zbx_vmware_dvswitch_t	*dvs_get(const zbx_vector_vmware_dvswitch_t *dvss, const char *uuid)
{
	zbx_vmware_dvswitch_t	dvs_cmp;
	int			i;

	dvs_cmp.uuid = (char *)uuid;

	if (FAIL == (i = zbx_vector_vmware_dvswitch_bsearch(dvss, &dvs_cmp, vmware_dvs_uuid_compare)))
		return NULL;

	return dvss->values[i];
}

static zbx_vmware_hv_t	*service_hv_get_by_vm_uuid(zbx_vmware_service_t *service, const char *uuid)
{
	zbx_vmware_vm_t		vm_local = {.uuid = (char *)uuid};
	zbx_vmware_vm_index_t	vmi_local = {&vm_local, NULL}, *vmi;
	zbx_vmware_hv_t		*hv = NULL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() uuid:'%s'", __func__, uuid);

	if (NULL != (vmi = (zbx_vmware_vm_index_t *)zbx_hashset_search(&service->data->vms_index, &vmi_local)))
		hv = vmi->hv;
	else
		hv = NULL;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%p", __func__, (void *)hv);

	return hv;

}

static zbx_vmware_vm_t	*service_vm_get(zbx_vmware_service_t *service, const char *uuid)
{
	zbx_vmware_vm_t		vm_local = {.uuid = (char *)uuid}, *vm;
	zbx_vmware_vm_index_t	vmi_local = {&vm_local, NULL}, *vmi;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() uuid:'%s'", __func__, uuid);

	if (NULL != (vmi = (zbx_vmware_vm_index_t *)zbx_hashset_search(&service->data->vms_index, &vmi_local)))
		vm = vmi->vm;
	else
		vm = NULL;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%p", __func__, (void *)vm);

	return vm;
}

static zbx_vmware_cluster_t	*cluster_get(zbx_vector_ptr_t *clusters, const char *clusterid)
{
	int			i;
	zbx_vmware_cluster_t	*cluster;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() uuid:'%s'", __func__, clusterid);

	for (i = 0; i < clusters->values_num; i++)
	{
		cluster = (zbx_vmware_cluster_t *)clusters->values[i];

		if (0 == strcmp(cluster->id, clusterid))
			goto out;
	}

	cluster = NULL;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%p", __func__, (void *)cluster);

	return cluster;
}

static zbx_vmware_cluster_t	*cluster_get_by_name(zbx_vector_ptr_t *clusters, const char *name)
{
	int			i;
	zbx_vmware_cluster_t	*cluster;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() name:'%s'", __func__, name);

	for (i = 0; i < clusters->values_num; i++)
	{
		cluster = (zbx_vmware_cluster_t *)clusters->values[i];

		if (0 == strcmp(cluster->name, name))
			goto out;
	}

	cluster = NULL;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%p", __func__, (void *)cluster);

	return cluster;
}

/******************************************************************************
 *                                                                            *
 * Purpose: gets vmware performance counter value by its identifier           *
 *                                                                            *
 * Parameters: service   - [IN] the vmware service                            *
 *             type      - [IN] the performance entity type (HostSystem,      *
 *                              VirtualMachine, Datastore)                    *
 *             id        - [IN] the performance entity identifier             *
 *             counterid - [IN] the performance counter identifier            *
 *             instance  - [IN] the performance counter instance or "" for    *
 *                              aggregate data                                *
 *             coeff     - [IN] the coefficient to apply to the value         *
 *             unit      - [IN] the counter unit info (kilo, mega, % etc)     *
 *             result    - [OUT] the output result                            *
 *                                                                            *
 * Return value: SYSINFO_RET_OK, result has value - performance counter value *
 *                               was successfully retrieved                   *
 *               SYSINFO_RET_OK, result has no value - performance counter    *
 *                               was found without a value                    *
 *               SYSINFO_RET_FAIL - otherwise, error message is set in result *
 *                                                                            *
 * Comments: There can be situation when performance counter is configured    *
 *           to be read but the collector has not yet processed it. In this   *
 *           case return SYSINFO_RET_OK with empty result so that it is       *
 *           ignored by server rather than generating error.                  *
 *                                                                            *
 ******************************************************************************/
static int	vmware_service_get_counter_value_by_id(zbx_vmware_service_t *service, const char *type, const char *id,
		zbx_uint64_t counterid, const char *instance, unsigned int coeff, int unit, AGENT_RESULT *result)
{
	zbx_vmware_perf_entity_t	*entity;
	zbx_vmware_perf_counter_t	*perfcounter;
	zbx_str_uint64_pair_t		*perfvalue;
	int				i, ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() type:%s id:%s counterid:" ZBX_FS_UI64 " instance:%s", __func__,
			type, id, counterid, instance);

	if (NULL == (entity = zbx_vmware_service_get_perf_entity(service, type, id)))
	{
		/* the requested counter has not been queried yet */
		zabbix_log(LOG_LEVEL_DEBUG, "performance data is not yet ready, ignoring request");
		ret = SYSINFO_RET_OK;
		goto out;
	}

	if (NULL != entity->error)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, entity->error));
		goto out;
	}

	if (FAIL == (i = zbx_vector_ptr_bsearch(&entity->counters, &counterid, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Performance counter data was not found."));
		goto out;
	}

	perfcounter = (zbx_vmware_perf_counter_t *)entity->counters.values[i];

	if (0 == (perfcounter->state & ZBX_VMWARE_COUNTER_READY))
	{
		ret = SYSINFO_RET_OK;
		goto out;
	}

	if (0 == perfcounter->values.values_num)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Performance counter data is not available."));
		goto out;
	}

	for (i = 0; i < perfcounter->values.values_num; i++)
	{
		perfvalue = &perfcounter->values.values[i];

		if (0 == strcmp(perfvalue->name, instance))
			break;
	}

	if (i == perfcounter->values.values_num)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Performance counter instance was not found."));
		goto out;
	}

	/* VMware returns -1 value if the performance data for the specified period is not ready - ignore it */
	if (ZBX_MAX_UINT64 == perfvalue->value)
	{
		ret = SYSINFO_RET_OK;
		goto out;
	}

	if (0 != coeff)
	{
		SET_UI64_RESULT(result, perfvalue->value * coeff);
	}
	else
	{
		switch (unit)
		{
		case ZBX_VMWARE_UNIT_KILOBYTES:
		case ZBX_VMWARE_UNIT_KILOBYTESPERSECOND:
			SET_UI64_RESULT(result, perfvalue->value * ZBX_KIBIBYTE);
			break;
		case ZBX_VMWARE_UNIT_MEGABYTES:
		case ZBX_VMWARE_UNIT_MEGABYTESPERSECOND:
			SET_UI64_RESULT(result, perfvalue->value * ZBX_MEBIBYTE);
			break;
		case ZBX_VMWARE_UNIT_TERABYTES:
			SET_UI64_RESULT(result, perfvalue->value * ZBX_TEBIBYTE);
			break;
		case ZBX_VMWARE_UNIT_PERCENT:
			SET_DBL_RESULT(result, (double)perfvalue->value / 100.0);
			break;
		case ZBX_VMWARE_UNIT_JOULE:
		case ZBX_VMWARE_UNIT_MEGAHERTZ:
		case ZBX_VMWARE_UNIT_MICROSECOND:
		case ZBX_VMWARE_UNIT_MILLISECOND:
		case ZBX_VMWARE_UNIT_NUMBER:
		case ZBX_VMWARE_UNIT_SECOND:
		case ZBX_VMWARE_UNIT_WATT:
		case ZBX_VMWARE_UNIT_CELSIUS:
			SET_UI64_RESULT(result, perfvalue->value);
			break;
		default:
			SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Performance counter type of unitInfo is unknown. "
					"Counter id:" ZBX_FS_UI64, counterid));
			goto out;
		}
	}

	ret = SYSINFO_RET_OK;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Purpose: gets vmware performance counter value by the path                 *
 *                                                                            *
 * Parameters: service  - [IN] the vmware service                             *
 *             type     - [IN] the performance entity type (HostSystem,       *
 *                             VirtualMachine, Datastore)                     *
 *             id       - [IN] the performance entity identifier              *
 *             path     - [IN] the performance counter path                   *
 *                             (<group>/<key>[<rollup type>])                 *
 *             instance - [IN] the performance counter instance or "" for     *
 *                             aggregate data                                 *
 *             coeff    - [IN] the coefficient to apply to the value          *
 *             result   - [OUT] the output result                             *
 *                                                                            *
 * Return value: SYSINFO_RET_OK, result has value - performance counter value *
 *                               was successfully retrieved                   *
 *               SYSINFO_RET_OK, result has no value - performance counter    *
 *                               was found without a value                    *
 *               SYSINFO_RET_FAIL - otherwise, error message is set in result *
 *                                                                            *
 * Comments: There can be situation when performance counter is configured    *
 *           to be read but the collector has not yet processed it. In this   *
 *           case return SYSINFO_RET_OK with empty result so that it is       *
 *           ignored by server rather than generating error.                  *
 *                                                                            *
 ******************************************************************************/
static int	vmware_service_get_counter_value_by_path(zbx_vmware_service_t *service, const char *type,
		const char *id, const char *path, const char *instance, unsigned int coeff, AGENT_RESULT *result)
{
	zbx_uint64_t	counterid;
	int		unit;

	if (FAIL == zbx_vmware_service_get_counterid(service, path, &counterid, &unit))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Performance counter is not available."));
		return SYSINFO_RET_FAIL;
	}

	return vmware_service_get_counter_value_by_id(service, type, id, counterid, instance, coeff, unit, result);
}

static int	vmware_service_get_vm_counter(zbx_vmware_service_t *service, const char *uuid, const char *instance,
		const char *path, unsigned int coeff, AGENT_RESULT *result)
{
	zbx_vmware_vm_t	*vm;
	int		ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() uuid:%s instance:%s path:%s", __func__, uuid, instance, path);

	if (NULL == (vm = service_vm_get(service, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown virtual machine uuid."));
		goto out;
	}

	ret = vmware_service_get_counter_value_by_path(service, "VirtualMachine", vm->id, path, instance, coeff,
			result);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Purpose: gets vmware service object                                        *
 *                                                                            *
 * Parameters: url       - [IN] the vmware service URL                        *
 *             username  - [IN] the vmware service username                   *
 *             password  - [IN] the vmware service password                   *
 *             ret       - [OUT] the operation result code                    *
 *                                                                            *
 * Return value: The vmware service object or NULL if the service was not     *
 *               found, did not have data or any error occurred. In the last  *
 *               case the error message will be stored in agent result.       *
 *                                                                            *
 * Comments: There are three possible cases:                                  *
 *             1) the vmware service is not ready. This can happen when       *
 *                service was added, but not yet processed by collector.      *
 *                In this case NULL is returned and result code is set to     *
 *                SYSINFO_RET_OK.                                             *
 *             2) the vmware service update failed. This can happen if there  *
 *                was a network problem, authentication failure or any error  *
 *                that prevented from obtaining and parsing vmware data.      *
 *                In this case NULL is returned and result code is set to     *
 *                SYSINFO_RET_FAIL.                                           *
 *             3) the vmware service has been updated successfully.           *
 *                In this case the service object is returned and result code *
 *                is not set.                                                 *
 *                                                                            *
 ******************************************************************************/
static zbx_vmware_service_t	*get_vmware_service(const char *url, const char *username, const char *password,
		AGENT_RESULT *result, int *ret)
{
	zbx_vmware_service_t	*service;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() '%s'@'%s'", __func__, username, url);

	if (NULL == (service = zbx_vmware_get_service(url, username, password)))
	{
		*ret = SYSINFO_RET_OK;
		goto out;
	}

	if (0 != (service->state & ZBX_VMWARE_STATE_FAILED))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, NULL != service->data->error ? service->data->error :
				"Unknown VMware service error."));

		zabbix_log(LOG_LEVEL_DEBUG, "failed to query VMware service: %s",
				NULL != service->data->error ? service->data->error : "unknown error");

		*ret = SYSINFO_RET_FAIL;
		service = NULL;
	}
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%p", __func__, (void *)service);

	return service;
}

/******************************************************************************
 *                                                                            *
 * Purpose: retrieves data from virtual machine details                       *
 *                                                                            *
 * Parameters: request   - [IN] the original request. The first parameter is  *
 *                              vmware service URL and the second parameter   *
 *                              is virtual machine uuid.                      *
 *             username  - [IN] the vmware service user name                  *
 *             password  - [IN] the vmware service password                   *
 *             xpath     - [IN] the xpath describing data to retrieve         *
 *             result    - [OUT] the request result                           *
 *                                                                            *
 ******************************************************************************/
static int	get_vcenter_vmprop(AGENT_REQUEST *request, const char *username, const char *password,
		int propid, AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	zbx_vmware_vm_t		*vm = NULL;
	const char		*url, *uuid, *value;
	int			ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() propid:%d", __func__, propid);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (vm = service_vm_get(service, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown virtual machine uuid."));
		goto unlock;
	}

	if (NULL == (value = vm->props[propid]))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Value is not available."));
		goto unlock;
	}

	SET_STR_RESULT(result, zbx_strdup(NULL, value));

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Purpose: retrieves hypervisor property                                     *
 *                                                                            *
 * Parameters: request   - [IN] the original request. The first parameter is  *
 *                              vmware service URL and the second parameter   *
 *                              is hypervisor uuid.                           *
 *             username  - [IN] the vmware service user name                  *
 *             password  - [IN] the vmware service password                   *
 *             propid    - [IN] the property id                               *
 *             result    - [OUT] the request result                           *
 *                                                                            *
 ******************************************************************************/
static int	get_vcenter_hvprop(AGENT_REQUEST *request, const char *username, const char *password, int propid,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	const char		*uuid, *url, *value;
	zbx_vmware_hv_t		*hv;
	int			ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() propid:%d", __func__, propid);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	if (NULL == (value = hv->props[propid]))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Value is not available."));
		goto unlock;
	}

	SET_STR_RESULT(result, zbx_strdup(NULL, value));
	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_cluster_discovery(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	struct zbx_json		json_data;
	const char		*url;
	zbx_vmware_service_t	*service;
	int			i, ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (1 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	zbx_json_initarray(&json_data, ZBX_JSON_STAT_BUF_LEN);

	for (i = 0; i < service->data->clusters.values_num; i++)
	{
		zbx_vmware_cluster_t	*cluster = (zbx_vmware_cluster_t *)service->data->clusters.values[i];

		zbx_json_addobject(&json_data, NULL);
		zbx_json_addstring(&json_data, "{#CLUSTER.ID}", cluster->id, ZBX_JSON_TYPE_STRING);
		zbx_json_addstring(&json_data, "{#CLUSTER.NAME}", cluster->name, ZBX_JSON_TYPE_STRING);
		zbx_json_close(&json_data);
	}

	zbx_json_close(&json_data);

	SET_STR_RESULT(result, zbx_strdup(NULL, json_data.buffer));

	zbx_json_free(&json_data);

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_cluster_status(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*url, *name;
	zbx_vmware_service_t	*service;
	zbx_vmware_cluster_t	*cluster;
	int			ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	name = get_rparam(request, 1);

	if ('\0' == *name)
		goto out;

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (cluster = cluster_get_by_name(&service->data->clusters, name)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown cluster name."));
		goto unlock;
	}

	if (NULL == cluster->status)
		goto unlock;

	ret = SYSINFO_RET_OK;

	if (0 == strcmp(cluster->status, "gray"))
		SET_UI64_RESULT(result, 0);
	else if (0 == strcmp(cluster->status, "green"))
		SET_UI64_RESULT(result, 1);
	else if (0 == strcmp(cluster->status, "yellow"))
		SET_UI64_RESULT(result, 2);
	else if (0 == strcmp(cluster->status, "red"))
		SET_UI64_RESULT(result, 3);
	else
		ret = SYSINFO_RET_FAIL;

unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

static void	vmware_get_events(const zbx_vector_ptr_t *events, zbx_uint64_t eventlog_last_key, const DC_ITEM *item,
		zbx_vector_ptr_t *add_results)
{
	int	i;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() eventlog_last_key:" ZBX_FS_UI64, __func__, eventlog_last_key);

	/* events were retrieved in reverse chronological order */
	for (i = events->values_num - 1; i >= 0; i--)
	{
		const zbx_vmware_event_t	*event = (zbx_vmware_event_t *)events->values[i];
		AGENT_RESULT			*add_result = NULL;

		if (event->key <= eventlog_last_key)
			continue;

		add_result = (AGENT_RESULT *)zbx_malloc(add_result, sizeof(AGENT_RESULT));
		init_result(add_result);

		if (SUCCEED == set_result_type(add_result, item->value_type, event->message))
		{
			set_result_meta(add_result, event->key, 0);

			if (ITEM_VALUE_TYPE_LOG == item->value_type)
			{
				add_result->log->logeventid = event->key;
				add_result->log->timestamp = event->timestamp;
			}

			zbx_vector_ptr_append(add_results, add_result);
		}
		else
			zbx_free(add_result);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(): events:%d", __func__, add_results->values_num);
}

int	check_vcenter_eventlog(AGENT_REQUEST *request, const DC_ITEM *item, AGENT_RESULT *result,
		zbx_vector_ptr_t *add_results)
{
	const char		*url, *skip;
	unsigned char		skip_old;
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 < request->nparam || 0 == request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);

	if (NULL == (skip = get_rparam(request, 1)) || '\0' == *skip || 0 == strcmp(skip, "all"))
	{
		skip_old = 0;
	}
	else if (0 == strcmp(skip, "skip"))
	{
		skip_old = (0 == request->lastlogsize ? 1 : 0);
	}
	else
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, item->username, item->password, result, &ret)))
		goto unlock;

	if (ZBX_VMWARE_EVENT_KEY_UNINITIALIZED == service->eventlog.last_key ||
			(0 != skip_old && 0 != service->eventlog.last_key ))
	{
		/* this may happen if recreate item vmware.eventlog for the same service URL */
		service->eventlog.last_key = request->lastlogsize;
		service->eventlog.skip_old = skip_old;
	}
	else if (0 != service->eventlog.oom)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Not enough shared memory to store VMware events."));
		goto unlock;
	}
	else if (request->lastlogsize < service->eventlog.last_key && 0 != request->lastlogsize)
	{
		/* this may happen if there are multiple vmware.eventlog items for the same service URL or item has  */
		/* been polled, but values got stuck in history cache and item's lastlogsize hasn't been updated yet */
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Too old events requested."));
		goto unlock;
	}
	else if (0 < service->data->events.values_num)
	{
		vmware_get_events(&service->data->events, request->lastlogsize, item, add_results);
		service->eventlog.last_key = ((const zbx_vmware_event_t *)service->data->events.values[0])->key;
	}

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_version(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*url;
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (1 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == service->version)
		goto unlock;

	SET_STR_RESULT(result, zbx_strdup(NULL, service->version));

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_fullname(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	char			*url;
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (1 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == service->fullname)
		goto unlock;

	SET_STR_RESULT(result, zbx_strdup(NULL, service->fullname));

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_cluster_name(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*url, *uuid;
	zbx_vmware_hv_t		*hv;
	zbx_vmware_service_t	*service;
	zbx_vmware_cluster_t	*cluster = NULL;
	int			ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	if (NULL != hv->clusterid)
		cluster = cluster_get(&service->data->clusters, hv->clusterid);

	SET_STR_RESULT(result, zbx_strdup(NULL, NULL != cluster ? cluster->name : ""));

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_connectionstate(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_CONNECTIONSTATE, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_cpu_usage(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_OVERALL_CPU_USAGE, result);

	if (SYSINFO_RET_OK == ret && NULL != GET_UI64_RESULT(result))
		result->ui64 = result->ui64 * 1000000;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_cpu_usage_perf(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*url, *uuid;
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	int			ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	ret = vmware_service_get_counter_value_by_path(service, "HostSystem", hv->id, "cpu/usage[average]", "", 0,
			result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_cpu_utilization(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*url, *uuid;
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	int			ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	ret = vmware_service_get_counter_value_by_path(service, "HostSystem", hv->id, "cpu/utilization[average]", "",
			0, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_power(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*path, *url, *uuid, *max;
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	int			ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 > request->nparam || request->nparam > 3)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);
	max = get_rparam(request, 2);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	if (NULL != max && '\0' != *max)
	{
		if (0 != strcmp(max, "max"))
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter."));
			goto out;
		}

		path = "power/powerCap[average]";
	}
	else
		path = "power/power[average]";

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	ret = vmware_service_get_counter_value_by_path(service, "HostSystem", hv->id, path, "", 1, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_discovery(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	struct zbx_json		json_data;
	const char		*url, *name;
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;
	zbx_vmware_hv_t		*hv;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (1 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	zbx_json_initarray(&json_data, ZBX_JSON_STAT_BUF_LEN);

	zbx_hashset_iter_reset(&service->data->hvs, &iter);
	while (NULL != (hv = (zbx_vmware_hv_t *)zbx_hashset_iter_next(&iter)))
	{
		zbx_vmware_cluster_t	*cluster = NULL;

		if (NULL == (name = hv->props[ZBX_VMWARE_HVPROP_NAME]))
			continue;

		if (NULL != hv->clusterid)
			cluster = cluster_get(&service->data->clusters, hv->clusterid);

		zbx_json_addobject(&json_data, NULL);
		zbx_json_addstring(&json_data, "{#HV.UUID}", hv->uuid, ZBX_JSON_TYPE_STRING);
		zbx_json_addstring(&json_data, "{#HV.ID}", hv->id, ZBX_JSON_TYPE_STRING);
		zbx_json_addstring(&json_data, "{#HV.NAME}", name, ZBX_JSON_TYPE_STRING);
		zbx_json_addstring(&json_data, "{#HV.IP}", ZBX_NULL2EMPTY_STR(hv->ip), ZBX_JSON_TYPE_STRING);
		zbx_json_addstring(&json_data, "{#DATACENTER.NAME}", hv->datacenter_name, ZBX_JSON_TYPE_STRING);
		zbx_json_addstring(&json_data, "{#CLUSTER.NAME}",
				NULL != cluster ? cluster->name : "", ZBX_JSON_TYPE_STRING);
		zbx_json_addstring(&json_data, "{#PARENT.NAME}", hv->parent_name, ZBX_JSON_TYPE_STRING);
		zbx_json_addstring(&json_data, "{#PARENT.TYPE}", hv->parent_type, ZBX_JSON_TYPE_STRING);
		zbx_json_addstring(&json_data, "{#HV.NETNAME}",
				ZBX_NULL2EMPTY_STR(hv->props[ZBX_VMWARE_HVPROP_NET_NAME]), ZBX_JSON_TYPE_STRING);
		zbx_json_close(&json_data);
	}

	zbx_json_close(&json_data);

	SET_STR_RESULT(result, zbx_strdup(NULL, json_data.buffer));

	zbx_json_free(&json_data);

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_fullname(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_FULL_NAME, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_hw_cpu_num(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_HW_NUM_CPU_CORES, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_hw_cpu_freq(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_HW_CPU_MHZ, result);

	if (SYSINFO_RET_OK == ret && NULL != GET_UI64_RESULT(result))
		result->ui64 = result->ui64 * 1000000;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_hw_cpu_model(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_HW_CPU_MODEL, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_hw_cpu_threads(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_HW_NUM_CPU_THREADS, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_hw_memory(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_HW_MEMORY_SIZE, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_hw_model(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_HW_MODEL, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_hw_serialnumber(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_HW_SERIALNUMBER, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_hw_uuid(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_HW_UUID, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_hw_vendor(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_HW_VENDOR, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_memory_size_ballooned(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int			i, ret = SYSINFO_RET_FAIL;
	zbx_vmware_service_t	*service;
	const char		*uuid, *url;
	zbx_vmware_hv_t		*hv;
	zbx_uint64_t		value = 0;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	for (i = 0; i < hv->vms.values_num; i++)
	{
		zbx_uint64_t	mem;
		const char	*value_str;
		zbx_vmware_vm_t	*vm = (zbx_vmware_vm_t *)hv->vms.values[i];

		if (NULL == (value_str = vm->props[ZBX_VMWARE_VMPROP_MEMORY_SIZE_BALLOONED]))
			continue;

		if (SUCCEED != is_uint64(value_str, &mem))
			continue;

		value += mem;
	}

	value *= ZBX_MEBIBYTE;
	SET_UI64_RESULT(result, value);

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_memory_used(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_MEMORY_USED, result);

	if (SYSINFO_RET_OK == ret && NULL != GET_UI64_RESULT(result))
		result->ui64 = result->ui64 * ZBX_MEBIBYTE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_sensor_health_state(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_HEALTH_STATE, result);

	if (SYSINFO_RET_OK == ret && NULL != GET_STR_RESULT(result))
	{
		if (0 == strcmp(result->str, "gray") || 0 == strcmp(result->str, "unknown"))
			SET_UI64_RESULT(result, 0);
		else if (0 == strcmp(result->str, "green"))
			SET_UI64_RESULT(result, 1);
		else if (0 == strcmp(result->str, "yellow"))
			SET_UI64_RESULT(result, 2);
		else if (0 == strcmp(result->str, "red"))
			SET_UI64_RESULT(result, 3);
		else
			ret = SYSINFO_RET_FAIL;

		UNSET_STR_RESULT(result);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_status(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_STATUS, result);

	if (SYSINFO_RET_OK == ret && NULL != GET_STR_RESULT(result))
	{
		if (0 == strcmp(result->str, "gray") || 0 == strcmp(result->str, "unknown"))
			SET_UI64_RESULT(result, 0);
		else if (0 == strcmp(result->str, "green"))
			SET_UI64_RESULT(result, 1);
		else if (0 == strcmp(result->str, "yellow"))
			SET_UI64_RESULT(result, 2);
		else if (0 == strcmp(result->str, "red"))
			SET_UI64_RESULT(result, 3);
		else
			ret = SYSINFO_RET_FAIL;

		UNSET_STR_RESULT(result);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_maintenance(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_MAINTENANCE, result);

	if (SYSINFO_RET_OK == ret && NULL != GET_STR_RESULT(result))
	{
		if (0 == strcmp(result->str, "false"))
			SET_UI64_RESULT(result, 0);
		else
			SET_UI64_RESULT(result, 1);

		UNSET_STR_RESULT(result);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_uptime(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_UPTIME, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_version(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_VERSION, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_sensors_get(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_SENSOR, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_hw_sensors_get(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_hvprop(request, username, password, ZBX_VMWARE_HVPROP_HW_SENSOR, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_vm_num(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int			ret = SYSINFO_RET_FAIL;
	zbx_vmware_service_t	*service;
	const char		*uuid, *url;
	zbx_vmware_hv_t		*hv;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	SET_UI64_RESULT(result, hv->vms.values_num);
	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

static int	check_vcenter_hv_network_common(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result, int direction, const char *func_parent)
{
	const char		*url, *mode, *uuid, *counter_name;
	unsigned int		coeff = 0;
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	int			ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s(), func_parent:'%s'", __func__, func_parent);

	if (2 > request->nparam || request->nparam > 3)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);
	mode = get_rparam(request, 2);

	if (NULL == mode || '\0' == *mode || 0 == strcmp(mode, "bps"))
	{
		counter_name = ZBX_IF_DIRECTION_IN == direction ? "net/received[average]" : "net/transmitted[average]";
		coeff = ZBX_KIBIBYTE;
	}
	else if (0 == strcmp(mode, "packets"))
	{
		counter_name = ZBX_IF_DIRECTION_IN ==
				direction ? "net/packetsRx[summation]" : "net/packetsTx[summation]";
	}
	else if (0 == strcmp(mode, "dropped"))
	{
		counter_name = ZBX_IF_DIRECTION_IN ==
				direction ? "net/droppedRx[summation]" : "net/droppedTx[summation]";
	}
	else if (0 == strcmp(mode, "errors"))
	{
		counter_name = ZBX_IF_DIRECTION_IN == direction ? "net/errorsRx[summation]" : "net/errorsTx[summation]";
	}
	else if (0 == strcmp(mode, "broadcast"))
	{
		counter_name = ZBX_IF_DIRECTION_IN ==
				direction ? "net/broadcastRx[summation]" : "net/broadcastTx[summation]";
	}
	else
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	ret = vmware_service_get_counter_value_by_path(service, "HostSystem", hv->id, counter_name, "",
			coeff, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(), func_parent:'%s', ret: %s", __func__, func_parent,
			zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_network_in(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return	check_vcenter_hv_network_common(request, username, password, result, ZBX_IF_DIRECTION_IN, __func__);
}

int	check_vcenter_hv_network_out(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return	check_vcenter_hv_network_common(request, username, password, result, ZBX_IF_DIRECTION_OUT, __func__);
}

int	check_vcenter_hv_net_if_discovery(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int			i, ret = SYSINFO_RET_FAIL;
	const char		*url, *uuid;
	struct zbx_json		json_data;
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	zbx_vmware_pnic_t	*nic;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	zbx_json_initarray(&json_data, ZBX_JSON_STAT_BUF_LEN);

	for (i = 0; i < hv->pnics.values_num; i++)
	{
		nic = hv->pnics.values[i];

		zbx_json_addobject(&json_data, NULL);
		zbx_json_addstring(&json_data, "{#IFNAME}", nic->name, ZBX_JSON_TYPE_STRING);
		zbx_json_addstring(&json_data, "{#IFDRIVER}", ZBX_NULL2EMPTY_STR(nic->driver), ZBX_JSON_TYPE_STRING);
		zbx_json_adduint64(&json_data, "{#IFSPEED}", nic->speed);
		zbx_json_addstring(&json_data, "{#IFDUPLEX}", ZBX_DUPLEX_FULL == nic->duplex ? "full" : "half",
				ZBX_JSON_TYPE_STRING);
		zbx_json_addstring(&json_data, "{#IFMAC}", ZBX_NULL2EMPTY_STR(nic->mac), ZBX_JSON_TYPE_STRING);

		zbx_json_close(&json_data);
	}

	zbx_json_close(&json_data);

	SET_STR_RESULT(result, zbx_strdup(NULL, json_data.buffer));
	zbx_json_free(&json_data);

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(), ret: %s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_network_linkspeed(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int			i, ret = SYSINFO_RET_FAIL;
	const char		*url, *uuid, *if_name;
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	zbx_vmware_pnic_t	nic_cmp;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (3 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);
	if_name = get_rparam(request, 2);

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	nic_cmp.name = (char *)if_name;

	if (FAIL == (i = zbx_vector_vmware_pnic_bsearch(&hv->pnics, &nic_cmp, vmware_pnic_compare)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown physical network interface name"));
		goto out;
	}

	SET_UI64_RESULT(result, hv->pnics.values[i]->speed);
	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(), ret: %s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_datacenter_name(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*url, *uuid;
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	int			ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	SET_STR_RESULT(result, zbx_strdup(NULL, hv->datacenter_name));

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_datastore_discovery(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*url, *uuid;
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	struct zbx_json		json_data;
	int			i, ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	zbx_json_initarray(&json_data, ZBX_JSON_STAT_BUF_LEN);

	for (i = 0; i < hv->dsnames.values_num; i++)
	{
		zbx_vmware_dsname_t	*dsname = hv->dsnames.values[i];
		int			j, total = 0;

		for (j = 0; j < dsname->hvdisks.values_num; j++)
			total += dsname->hvdisks.values[j].multipath_total;

		zbx_json_addobject(&json_data, NULL);
		zbx_json_addstring(&json_data, "{#DATASTORE}", dsname->name, ZBX_JSON_TYPE_STRING);
		zbx_json_adduint64(&json_data, "{#MULTIPATH.COUNT}", (unsigned int)total);
		zbx_json_adduint64(&json_data, "{#MULTIPATH.PARTITION.COUNT}",
				(unsigned int)dsname->hvdisks.values_num);

		zbx_json_close(&json_data);
	}

	zbx_json_close(&json_data);

	SET_STR_RESULT(result, zbx_strdup(NULL, json_data.buffer));

	zbx_json_free(&json_data);

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

#define DATASTORE_METRIC_MODE_LATENCY		0
#define	DATASTORE_METRIC_MODE_MAX_LATENCY	1
#define DATASTORE_METRIC_MODE_RPS		2

static int	check_vcenter_hv_datastore_metrics(AGENT_REQUEST *request, const char *username, const char *password,
		int direction, AGENT_RESULT *result)
{
	const char		*url, *mode, *hv_uuid, *ds_name, *perfcounter;
	zbx_uint64_t		access_filter;
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	zbx_vmware_datastore_t	*datastore;
	zbx_vmware_dsname_t	dsnames_cmp;
	int			i, metric_mode, ret = SYSINFO_RET_FAIL;
	zbx_str_uint64_pair_t	uuid_cmp = {.value = 0};

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (3 > request->nparam || request->nparam > 4)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	hv_uuid = get_rparam(request, 1);
	ds_name = get_rparam(request, 2);
	mode = get_rparam(request, 3);

	if (NULL == mode || '\0' == *mode || (0 == strcmp(mode, "latency")))
	{
		metric_mode = DATASTORE_METRIC_MODE_LATENCY;
	}
	else if (0 == strcmp(mode, "rps"))
	{
		metric_mode = DATASTORE_METRIC_MODE_RPS;
	}
	else
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid fourth parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, hv_uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	datastore = ds_get(&service->data->datastores, ds_name);

	if (NULL == datastore)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown datastore name."));
		goto unlock;
	}

	dsnames_cmp.name = datastore->name;

	if (FAIL == zbx_vector_vmware_dsname_bsearch(&hv->dsnames, &dsnames_cmp, vmware_dsname_compare))
	{
		SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Datastore \"%s\" not found on this hypervisor.",
				datastore->name));
		goto unlock;
	}

	if (NULL == datastore->uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown datastore uuid."));
		goto unlock;
	}

	uuid_cmp.name = hv->uuid;

	if (FAIL == (i = zbx_vector_str_uint64_pair_bsearch(&datastore->hv_uuids_access, uuid_cmp,
			zbx_str_uint64_pair_name_compare)))
	{
		SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Unknown hypervisor \"%s\" for datastore \"%s\".",
				hv->props[ZBX_VMWARE_HVPROP_NAME], datastore->name));
		goto unlock;
	}

	switch (direction)
	{
		case ZBX_DATASTORE_DIRECTION_READ:
			access_filter = ZBX_VMWARE_DS_READ_FILTER;

			switch (metric_mode)
			{
				case DATASTORE_METRIC_MODE_RPS:
					perfcounter = "datastore/numberReadAveraged[average]";
					break;
				default:
					perfcounter = "datastore/totalReadLatency[average]";
			}
			break;
		case ZBX_DATASTORE_DIRECTION_WRITE:
			access_filter = ZBX_VMWARE_DS_WRITE_FILTER;

			switch (metric_mode)
			{
				case DATASTORE_METRIC_MODE_RPS:
					perfcounter = "datastore/numberWriteAveraged[average]";
					break;
				default:
					perfcounter = "datastore/totalWriteLatency[average]";
			}
			break;
		default:
			THIS_SHOULD_NEVER_HAPPEN;
			goto unlock;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "%s(): perfcounter:%s", __func__, perfcounter);

	if (access_filter != (datastore->hv_uuids_access.values[i].value & access_filter))
	{
		zbx_uint64_t	mi = datastore->hv_uuids_access.values[i].value;

		SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Datastore is not available for hypervisor: %s",
				0 == (ZBX_VMWARE_DS_MOUNTED & mi) ? "unmounted" : (
				0 == (ZBX_VMWARE_DS_ACCESSIBLE & mi) ? "inaccessible" : (
				ZBX_VMWARE_DS_READ == (ZBX_VMWARE_DS_READWRITE & mi)? "readOnly" :
				"unknown"))));
		goto unlock;
	}

	ret = vmware_service_get_counter_value_by_path(service, ZBX_VMWARE_SOAP_HV, hv->id, perfcounter,
			datastore->uuid, 1, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

static int	check_vcenter_datastore_metrics(AGENT_REQUEST *request, const char *username, const char *password,
		int direction, AGENT_RESULT *result)
{
	const char		*url, *mode, *ds_name, *perfcounter;
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	zbx_vmware_datastore_t	*datastore;
	int			i, metric_mode, ret = SYSINFO_RET_FAIL, unit, count = 0, ds_count = 0;
	zbx_uint64_t		access_filter, counterid, value = 0;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 > request->nparam || request->nparam > 3)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	ds_name = get_rparam(request, 1);
	mode = get_rparam(request, 2);

	if (NULL == mode || '\0' == *mode || (0 == strcmp(mode, "latency")))
	{
		metric_mode = DATASTORE_METRIC_MODE_LATENCY;
	}
	else if (0 == strcmp(mode, "maxlatency"))
	{
		metric_mode = DATASTORE_METRIC_MODE_MAX_LATENCY;
	}
	else if (0 == strcmp(mode, "rps"))
	{
		metric_mode = DATASTORE_METRIC_MODE_RPS;
	}
	else
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	datastore = ds_get(&service->data->datastores, ds_name);

	if (NULL == datastore)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown datastore name."));
		goto unlock;
	}

	if (NULL == datastore->uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown datastore uuid."));
		goto unlock;
	}

	switch (direction)
	{
		case ZBX_DATASTORE_DIRECTION_READ:
			access_filter = ZBX_VMWARE_DS_READ_FILTER;

			switch (metric_mode)
			{
				case DATASTORE_METRIC_MODE_RPS:
					perfcounter = "datastore/numberReadAveraged[average]";
					break;
				default:
					perfcounter = "datastore/totalReadLatency[average]";
			}
			break;
		case ZBX_DATASTORE_DIRECTION_WRITE:
			access_filter = ZBX_VMWARE_DS_WRITE_FILTER;

			switch (metric_mode)
			{
				case DATASTORE_METRIC_MODE_RPS:
					perfcounter = "datastore/numberWriteAveraged[average]";
					break;
				default:
					perfcounter = "datastore/totalWriteLatency[average]";
			}
			break;
		default:
			THIS_SHOULD_NEVER_HAPPEN;
			goto unlock;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "%s(): perfcounter:%s", __func__, perfcounter);

	if (FAIL == zbx_vmware_service_get_counterid(service, perfcounter, &counterid, &unit))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Performance counter is not available."));
		goto unlock;
	}

	for (i = 0; i < datastore->hv_uuids_access.values_num; i++)
	{
		if (access_filter != (datastore->hv_uuids_access.values[i].value & access_filter))
		{
			zbx_uint64_t	mi = datastore->hv_uuids_access.values[i].value;

			zabbix_log(LOG_LEVEL_DEBUG, "Datastore %s is not available for hypervisor %s: %s",
					datastore->name, datastore->hv_uuids_access.values[i].name,
					0 == (ZBX_VMWARE_DS_MOUNTED & mi) ? "unmounted" : (
					0 == (ZBX_VMWARE_DS_ACCESSIBLE & mi) ? "inaccessible" : (
					ZBX_VMWARE_DS_READ == (ZBX_VMWARE_DS_READWRITE & mi)? "readOnly" :
					"unknown")));
			continue;
		}

		if (NULL == (hv = hv_get(&service->data->hvs, datastore->hv_uuids_access.values[i].name)))
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
			goto unlock;
		}

		if (SYSINFO_RET_OK != (ret = vmware_service_get_counter_value_by_id(service, "HostSystem", hv->id,
				counterid, datastore->uuid, 1, unit, result)))
		{
			char	*err, *msg = *GET_MSG_RESULT(result);

			*msg = (char)tolower(*msg);
			err = zbx_dsprintf(NULL, "Counter %s for datastore %s is not available for hypervisor %s: %s",
					perfcounter, datastore->name,
					ZBX_NULL2EMPTY_STR(hv->props[ZBX_VMWARE_HVPROP_NAME]), msg);
			UNSET_MSG_RESULT(result);
			SET_MSG_RESULT(result, err);
			goto unlock;
		}

		ds_count++;

		if (0 == ISSET_VALUE(result))
			continue;

		if (DATASTORE_METRIC_MODE_MAX_LATENCY != metric_mode)
		{
			value += *GET_UI64_RESULT(result);
			count++;
		}
		else if (value < *GET_UI64_RESULT(result))
			value = *GET_UI64_RESULT(result);

		UNSET_UI64_RESULT(result);
	}

	if (0 == ds_count)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "No datastores available."));
		goto unlock;
	}

	if (DATASTORE_METRIC_MODE_MAX_LATENCY != metric_mode && 0 != count)
		value = value / count;

	SET_UI64_RESULT(result, value);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

#undef DATASTORE_METRIC_MODE_LATENCY
#undef DATASTORE_METRIC_MODE_MAX_LATENCY
#undef DATASTORE_METRIC_MODE_RPS

int	check_vcenter_hv_datastore_read(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return check_vcenter_hv_datastore_metrics(request, username, password, ZBX_DATASTORE_DIRECTION_READ, result);
}

int	check_vcenter_hv_datastore_write(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return check_vcenter_hv_datastore_metrics(request, username, password, ZBX_DATASTORE_DIRECTION_WRITE, result);
}

int	check_vcenter_datastore_read(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return check_vcenter_datastore_metrics(request, username, password, ZBX_DATASTORE_DIRECTION_READ, result);
}

int	check_vcenter_datastore_write(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return check_vcenter_datastore_metrics(request, username, password, ZBX_DATASTORE_DIRECTION_WRITE, result);
}

static int	check_vcenter_hv_datastore_size_vsphere(int mode, const zbx_vmware_datastore_t *datastore,
		AGENT_RESULT *result)
{
	switch (mode)
	{
		case ZBX_VMWARE_DATASTORE_SIZE_TOTAL:
			if (ZBX_MAX_UINT64 == datastore->capacity)
			{
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Datastore \"capacity\" is not available."));
				return SYSINFO_RET_FAIL;
			}
			SET_UI64_RESULT(result, datastore->capacity);
			break;
		case ZBX_VMWARE_DATASTORE_SIZE_FREE:
			if (ZBX_MAX_UINT64 == datastore->free_space)
			{
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Datastore \"free space\" is not available."));
				return SYSINFO_RET_FAIL;
			}
			SET_UI64_RESULT(result, datastore->free_space);
			break;
		case ZBX_VMWARE_DATASTORE_SIZE_UNCOMMITTED:
			if (ZBX_MAX_UINT64 == datastore->uncommitted)
			{
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Datastore \"uncommitted\" is not available."));
				return SYSINFO_RET_FAIL;
			}
			SET_UI64_RESULT(result, datastore->uncommitted);
			break;
		case ZBX_VMWARE_DATASTORE_SIZE_PFREE:
			if (ZBX_MAX_UINT64 == datastore->capacity)
			{
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Datastore \"capacity\" is not available."));
				return SYSINFO_RET_FAIL;
			}
			if (ZBX_MAX_UINT64 == datastore->free_space)
			{
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Datastore \"free space\" is not available."));
				return SYSINFO_RET_FAIL;
			}
			if (0 == datastore->capacity)
			{
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Datastore \"capacity\" is zero."));
				return SYSINFO_RET_FAIL;
			}
			SET_DBL_RESULT(result, (double)datastore->free_space / datastore->capacity * 100);
			break;
	}

	return SYSINFO_RET_OK;
}

static int	check_vcenter_ds_param(const char *param, int *mode)
{

	if (NULL == param || '\0' == *param || 0 == strcmp(param, "total"))
	{
		*mode = ZBX_VMWARE_DATASTORE_SIZE_TOTAL;
	}
	else if (0 == strcmp(param, "free"))
	{
		*mode = ZBX_VMWARE_DATASTORE_SIZE_FREE;
	}
	else if (0 == strcmp(param, "pfree"))
	{
		*mode = ZBX_VMWARE_DATASTORE_SIZE_PFREE;
	}
	else if (0 == strcmp(param, "uncommitted"))
	{
		*mode = ZBX_VMWARE_DATASTORE_SIZE_UNCOMMITTED;
	}
	else
		return FAIL;

	return SUCCEED;
}

static int	check_vcenter_ds_size(const char *url, const char *hv_uuid, const char *name, const int mode,
		const char *username, const char *password, AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;
	zbx_vmware_datastore_t	*datastore = NULL;
	zbx_uint64_t		disk_used, disk_provisioned, disk_capacity;
	unsigned int		flags;
	zbx_str_uint64_pair_t	uuid_cmp = {.name = (char *)hv_uuid, .value = 0};

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	datastore = ds_get(&service->data->datastores, name);

	if (NULL == datastore)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown datastore name."));
		goto unlock;
	}

	if (NULL != hv_uuid &&
			FAIL == zbx_vector_str_uint64_pair_bsearch(&datastore->hv_uuids_access, uuid_cmp,
			zbx_str_uint64_pair_name_compare))
	{
		SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Hypervisor '%s' not found on this datastore.", hv_uuid));
		goto unlock;
	}

	if (ZBX_VMWARE_TYPE_VSPHERE == service->type)
	{
		ret = check_vcenter_hv_datastore_size_vsphere(mode, datastore, result);
		goto unlock;
	}

	switch (mode)
	{
		case ZBX_VMWARE_DATASTORE_SIZE_TOTAL:
			flags = ZBX_DATASTORE_COUNTER_CAPACITY;
			break;
		case ZBX_VMWARE_DATASTORE_SIZE_FREE:
			flags = ZBX_DATASTORE_COUNTER_CAPACITY | ZBX_DATASTORE_COUNTER_USED;
			break;
		case ZBX_VMWARE_DATASTORE_SIZE_PFREE:
			flags = ZBX_DATASTORE_COUNTER_CAPACITY | ZBX_DATASTORE_COUNTER_USED;
			break;
		case ZBX_VMWARE_DATASTORE_SIZE_UNCOMMITTED:
			flags = ZBX_DATASTORE_COUNTER_PROVISIONED | ZBX_DATASTORE_COUNTER_USED;
			break;
	}

	if (0 != (flags & ZBX_DATASTORE_COUNTER_PROVISIONED))
	{
		ret = vmware_service_get_counter_value_by_path(service, "Datastore", datastore->id,
				"disk/provisioned[latest]", ZBX_DATASTORE_TOTAL, ZBX_KIBIBYTE, result);

		if (SYSINFO_RET_OK != ret || NULL == GET_UI64_RESULT(result))
			goto unlock;

		disk_provisioned = *GET_UI64_RESULT(result);
		UNSET_UI64_RESULT(result);
	}

	if (0 != (flags & ZBX_DATASTORE_COUNTER_USED))
	{
		ret = vmware_service_get_counter_value_by_path(service, "Datastore", datastore->id,
				"disk/used[latest]", ZBX_DATASTORE_TOTAL, ZBX_KIBIBYTE, result);

		if (SYSINFO_RET_OK != ret || NULL == GET_UI64_RESULT(result))
			goto unlock;

		disk_used = *GET_UI64_RESULT(result);
		UNSET_UI64_RESULT(result);
	}

	if (0 != (flags & ZBX_DATASTORE_COUNTER_CAPACITY))
	{
		ret = vmware_service_get_counter_value_by_path(service, "Datastore", datastore->id,
				"disk/capacity[latest]", ZBX_DATASTORE_TOTAL, ZBX_KIBIBYTE, result);

		if (SYSINFO_RET_OK != ret || NULL == GET_UI64_RESULT(result))
			goto unlock;

		disk_capacity = *GET_UI64_RESULT(result);
		UNSET_UI64_RESULT(result);
	}

	switch (mode)
	{
		case ZBX_VMWARE_DATASTORE_SIZE_TOTAL:
			SET_UI64_RESULT(result, disk_capacity);
			break;
		case ZBX_VMWARE_DATASTORE_SIZE_FREE:
			SET_UI64_RESULT(result, disk_capacity - disk_used);
			break;
		case ZBX_VMWARE_DATASTORE_SIZE_UNCOMMITTED:
			SET_UI64_RESULT(result, disk_provisioned - disk_used);
			break;
		case ZBX_VMWARE_DATASTORE_SIZE_PFREE:
			SET_DBL_RESULT(result, 0 != disk_capacity ?
					(double) (disk_capacity - disk_used) / disk_capacity * 100 : 0);
			break;
	}

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_datastore_size(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char	*url, *uuid, *name, *param;
	int		ret = SYSINFO_RET_FAIL, mode;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (3 > request->nparam || request->nparam > 4)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);
	name = get_rparam(request, 2);
	param = get_rparam(request, 3);

	if (SUCCEED == check_vcenter_ds_param(param, &mode))
		ret = check_vcenter_ds_size(url, uuid, name, mode, username, password, result);
	else
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid fourth parameter."));
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_cl_perfcounter(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	char			*url, *path, *clusterid;
	const char 		*instance;
	zbx_vmware_service_t	*service;
	zbx_vmware_cluster_t	*cluster;
	zbx_uint64_t		counterid;
	int			unit, ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (3 > request->nparam || request->nparam > 4)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	clusterid = get_rparam(request, 1);
	path = get_rparam(request, 2);
	instance = get_rparam(request, 3);

	if (NULL == instance)
		instance = "";

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (FAIL == zbx_vmware_service_get_counterid(service, path, &counterid, &unit))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Performance counter is not available."));
		goto unlock;
	}

	if (NULL == (cluster = cluster_get(&service->data->clusters, clusterid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid cluster id."));
		goto unlock;
	}

	/* FAIL is returned if counter already exists */
	if (SUCCEED == zbx_vmware_service_add_perf_counter(service, "ClusterComputeResource", cluster->id,
			counterid, "*"))
	{
		ret = SYSINFO_RET_OK;
		goto unlock;
	}

	/* the performance counter is already being monitored, try to get the results from statistics */
	ret = vmware_service_get_counter_value_by_id(service, "ClusterComputeResource", cluster->id, counterid,
			instance, 1, unit, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_perfcounter(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*instance, *url, *uuid, *path;
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	zbx_uint64_t		counterid;
	int			unit, ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (3 > request->nparam || request->nparam > 4)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);
	path = get_rparam(request, 2);
	instance = get_rparam(request, 3);

	if (NULL == instance)
		instance = "";

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	if (FAIL == zbx_vmware_service_get_counterid(service, path, &counterid, &unit))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Performance counter is not available."));
		goto unlock;
	}

	/* FAIL is returned if counter already exists */
	if (SUCCEED == zbx_vmware_service_add_perf_counter(service, "HostSystem", hv->id, counterid, "*"))
	{
		ret = SYSINFO_RET_OK;
		goto unlock;
	}

	/* the performance counter is already being monitored, try to get the results from statistics */
	ret = vmware_service_get_counter_value_by_id(service, "HostSystem", hv->id, counterid, instance, 1, unit,
			result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_datastore_list(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*url, *hv_uuid;
	char			*ds_list = NULL;
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	int			i, ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam )
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	hv_uuid = get_rparam(request, 1);
	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, hv_uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	for (i = 0; i < hv->dsnames.values_num; i++)
	{
		zbx_vmware_dsname_t	*dsname = hv->dsnames.values[i];

		ds_list = zbx_strdcatf(ds_list, "%s\n", dsname->name);
	}

	if (NULL != ds_list)
		ds_list[strlen(ds_list)-1] = '\0';
	else
		ds_list = zbx_strdup(NULL, "");

	SET_TEXT_RESULT(result, ds_list);

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_hv_datastore_multipath(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*url, *hv_uuid, *ds_name, *partition;
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	zbx_vmware_dsname_t	*dsname;
	int			ret = SYSINFO_RET_FAIL, i, j, multipath_count = 0;
	zbx_uint64_t		partitionid = 0;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 > request->nparam || request->nparam > 4)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	hv_uuid = get_rparam(request, 1);
	ds_name = get_rparam(request, 2);
	partition = get_rparam(request, 3);

	if ('\0' == *hv_uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	if (NULL != partition && '\0' != *partition)
	{
		if (NULL == ds_name || '\0' == *ds_name)
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid fourth parameter."));
			goto out;
		}

		partitionid = (unsigned int) atoi(partition);
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = hv_get(&service->data->hvs, hv_uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
		goto unlock;
	}

	if (NULL != ds_name && '\0' != *ds_name)
	{
		zbx_vmware_datastore_t	*datastore;
		zbx_vmware_dsname_t	dsnames_cmp;
		zbx_vmware_hvdisk_t	hvdisk_cmp;

		datastore = ds_get(&service->data->datastores, ds_name);

		if (NULL == datastore)
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown datastore name."));
			goto unlock;
		}

		dsnames_cmp.name = datastore->name;

		if (FAIL == (i = zbx_vector_vmware_dsname_bsearch(&hv->dsnames, &dsnames_cmp, vmware_dsname_compare)))
		{
			SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Datastore \"%s\" not found on this hypervisor.",
					datastore->name));
			goto unlock;
		}

		if (NULL == datastore->uuid)
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown datastore uuid."));
			goto unlock;
		}

		dsname = hv->dsnames.values[i];

		if (NULL != partition)
		{
			hvdisk_cmp.partitionid = partitionid;

			if (FAIL == (i = zbx_vector_vmware_hvdisk_bsearch(&dsname->hvdisks, hvdisk_cmp,
					ZBX_DEFAULT_UINT64_COMPARE_FUNC)))
			{
				SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Unknown partition id:" ZBX_FS_UI64,
						partitionid));
				goto unlock;
			}

			multipath_count = dsname->hvdisks.values[i].multipath_active;
		}
		else
		{
			for (j = 0; j < dsname->hvdisks.values_num; j++)
				multipath_count += dsname->hvdisks.values[j].multipath_active;
		}
	}
	else
	{
		for (i = 0; i < hv->dsnames.values_num; i++)
		{
			dsname = hv->dsnames.values[i];

			for (j = 0; j < dsname->hvdisks.values_num; j++)
				multipath_count += dsname->hvdisks.values[j].multipath_active;
		}
	}

	SET_UI64_RESULT(result, (unsigned int)multipath_count);
	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_datastore_hv_list(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*url, *ds_name, *hv_name;
	char			*hv_list = NULL;
	zbx_vmware_service_t	*service;
	int			i, ret = SYSINFO_RET_FAIL;
	zbx_vmware_datastore_t	*datastore = NULL;
	zbx_vmware_hv_t		*hv;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam )
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	ds_name = get_rparam(request, 1);
	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (datastore = ds_get(&service->data->datastores, ds_name)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown datastore name."));
		goto unlock;
	}

	for (i=0; i < datastore->hv_uuids_access.values_num; i++)
	{
		if (NULL == (hv = hv_get(&service->data->hvs, datastore->hv_uuids_access.values[i].name)))
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown hypervisor uuid."));
			zbx_free(hv_list);
			goto unlock;
		}

		if (NULL == (hv_name = hv->props[ZBX_VMWARE_HVPROP_NAME]))
			hv_name = datastore->hv_uuids_access.values[i].name;

		hv_list = zbx_strdcatf(hv_list, "%s\n", hv_name);
	}

	if (NULL != hv_list)
		hv_list[strlen(hv_list)-1] = '\0';
	else
		hv_list = zbx_strdup(NULL, "");

	SET_TEXT_RESULT(result, hv_list);

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_datastore_size(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char	*url, *name, *param;
	int		ret = SYSINFO_RET_FAIL, mode;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 > request->nparam || request->nparam > 3)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	name = get_rparam(request, 1);
	param = get_rparam(request, 2);

	if (SUCCEED == check_vcenter_ds_param(param, &mode))
		ret = check_vcenter_ds_size(url, NULL, name, mode, username, password, result);
	else
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter."));
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_datastore_discovery(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*url;
	zbx_vmware_service_t	*service;
	struct zbx_json		json_data;
	int			i, j, ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (1 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	zbx_json_initarray(&json_data, ZBX_JSON_STAT_BUF_LEN);

	for (i = 0; i < service->data->datastores.values_num; i++)
	{
		zbx_vmware_datastore_t	*datastore = service->data->datastores.values[i];
		zbx_json_addobject(&json_data, NULL);
		zbx_json_addstring(&json_data, "{#DATASTORE}", datastore->name, ZBX_JSON_TYPE_STRING);
		zbx_json_addobject(&json_data, "{#DATASTORE.EXTENT}");

		for (j = 0; j < datastore->diskextents.values_num; j++)
		{
			char			buffer[MAX_ID_LEN];
			zbx_vmware_diskextent_t	*extent = datastore->diskextents.values[j];

			zbx_snprintf(buffer, sizeof(buffer), ZBX_FS_UI64, extent->partitionid);
			zbx_json_addstring(&json_data, extent->diskname, buffer, ZBX_JSON_TYPE_INT);
		}

		zbx_json_close(&json_data);
		zbx_json_close(&json_data);
	}

	zbx_json_close(&json_data);

	SET_STR_RESULT(result, zbx_strdup(NULL, json_data.buffer));

	zbx_json_free(&json_data);

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_dvswitch_discovery(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*url;
	int			i, ret = SYSINFO_RET_FAIL;
	zbx_vmware_service_t	*service;
	struct zbx_json		json_data;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (1 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	zbx_json_initarray(&json_data, ZBX_JSON_STAT_BUF_LEN);

	for (i = 0; i < service->data->dvswitches.values_num; i++)
	{
		zbx_vmware_dvswitch_t	*dvswitch = (zbx_vmware_dvswitch_t *)service->data->dvswitches.values[i];

		zbx_json_addobject(&json_data, NULL);
		zbx_json_addstring(&json_data, "{#DVSWITCH.UUID}", dvswitch->uuid, ZBX_JSON_TYPE_STRING);
		zbx_json_addstring(&json_data, "{#DVSWITCH.NAME}", dvswitch->name, ZBX_JSON_TYPE_STRING);
		zbx_json_close(&json_data);
	}

	zbx_json_close(&json_data);

	SET_STR_RESULT(result, zbx_strdup(NULL, json_data.buffer));

	zbx_json_free(&json_data);

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

static int	dvs_param_validate(zbx_vector_custquery_param_t *query_params, unsigned int vc_version)
{
	int	i;

	for (i = 0; i < query_params->values_num; i++)
	{
		zbx_vmware_custquery_param_t	*p = &query_params->values[i];

		if (0 == strcmp("active", p->name) || 0 == strcmp("connected", p->name) ||
				0 == strcmp("inside", p->name) || 0 == strcmp("nsxPort", p->name) ||
				0 == strcmp("uplinkPort", p->name))
		{
			if (0 != strcmp("true", p->value) && 0 != strcmp("false", p->value))
				return FAIL;
		}
		else if (0 != strcmp("host", p->name) && 0 != strcmp("portgroupKey", p->name) &&
				0 != strcmp("portKey", p->name))
		{
			return FAIL;
		}

		if (0 == strcmp("host", p->name) && vc_version < 65)
			return FAIL;

		if (0 == strcmp("nsxPort", p->name) && vc_version < 70)
			return FAIL;
	}

	return SUCCEED;
}

static int	custquery_param_create(const char *key, zbx_vector_custquery_param_t *query_params)
{
	char				*left, *right, *src;
	zbx_vmware_custquery_param_t	param = {NULL, NULL};
	int				ret = SUCCEED;

	if ('\0' == *key)
		return ret;

	src = zbx_strdup(NULL, key);

	while (1)
	{
		zbx_strsplit_first(src, ',', &left, &right);

		if (NULL == left || '\0' == *left)
		{
			ret = FAIL;
			break;
		}

		zbx_strsplit_first(left, ':', &param.name, &param.value);

		if (NULL == param.name || '\0' == *param.name || NULL == param.value)
		{
			ret = FAIL;
			break;
		}

		zbx_vector_custquery_param_append(query_params, param);
		param.name = NULL;
		param.value = NULL;

		if (NULL == right || '\0' == *right)
			break;

		zbx_free(src);
		src = right;
		right = NULL;
		zbx_free(left);
	}

	zbx_free(param.name);
	zbx_free(param.value);
	zbx_free(left);
	zbx_free(right);
	zbx_free(src);

	return ret;
}

int	check_vcenter_dvswitch_fetchports_get(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char			*mode, *url, *uuid, *key, *type = ZBX_VMWARE_SOAP_DVS;
	int				ret = SYSINFO_RET_FAIL;
	zbx_vmware_service_t		*service;
	zbx_vmware_dvswitch_t		*dvs;
	zbx_vmware_cust_query_t		*custom_query;
	zbx_vector_custquery_param_t	query_params;
	zbx_vmware_custom_query_type_t	query_type = VMWARE_DVSWITCH_FETCH_DV_PORTS;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	zbx_vector_custquery_param_create(&query_params);

	if (2 > request->nparam || request->nparam > 4)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);
	key = get_rparam(request, 2);
	mode = get_rparam(request, 3);

	if (NULL == mode)
	{
		mode = "state";
	}
	else if (0 != strcmp(mode, "state") && 0 != strcmp(mode, "full"))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid fourth parameter."));
		goto out;
	}

	if (NULL == key)
		key = "";

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (dvs = dvs_get(&service->data->dvswitches, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown DVSwitch uuid."));
		goto unlock;
	}

	if (NULL == (custom_query = zbx_vmware_service_get_cust_query(service, type, dvs->id, key, query_type, mode))
			&& (SUCCEED != custquery_param_create(key, &query_params)
			|| SUCCEED != dvs_param_validate(&query_params,
			service->major_version * 10 + service->minor_version)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL,
				"Unknown format of vmware DistributedVirtualSwitchPortCriteria."));
		goto unlock;
	}

	/* FAIL is returned if custom query exists */
	if (NULL == custom_query && SUCCEED == zbx_vmware_service_add_cust_query(service, type, dvs->id, key,
			query_type, mode, &query_params))
	{
		ret = SYSINFO_RET_OK;
		goto unlock;
	}
	else if (NULL == custom_query)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown DVSwitch query."));
		goto unlock;
	}

	if (0 != (custom_query->state & ZBX_VMWARE_CQ_ERROR))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, custom_query->error));
		goto unlock;
	}

	if (0 != (custom_query->state & ZBX_VMWARE_CQ_READY))
		SET_STR_RESULT(result, zbx_strdup(NULL, custom_query->value));

	if (0 != (custom_query->state & ZBX_VMWARE_CQ_PAUSED))
		custom_query->state &= ~(unsigned char)ZBX_VMWARE_CQ_PAUSED;

	custom_query->last_pooled = time(NULL);
	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zbx_vector_custquery_param_clear_ext(&query_params, zbx_vmware_cq_param_free);
	zbx_vector_custquery_param_destroy(&query_params);
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_attribute(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t		*service;
	zbx_vmware_vm_t			*vm;
	zbx_vmware_custom_attr_t	custom_attr;
	const char			*url, *vm_uuid, *attr_name;
	int				index, ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (3 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	vm_uuid = get_rparam(request, 1);
	attr_name = get_rparam(request, 2);

	if ('\0' == *vm_uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (vm = service_vm_get(service, vm_uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown virtual machine uuid."));
		goto unlock;
	}

	custom_attr.name = (char *)attr_name;

	if (FAIL == (index = zbx_vector_vmware_custom_attr_bsearch(&vm->custom_attrs, &custom_attr,
			vmware_custom_attr_compare_name)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Custom attribute is not available."));
		goto unlock;
	}

	SET_STR_RESULT(result, zbx_strdup(NULL, vm->custom_attrs.values[index]->value));
	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_cpu_num(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_CPU_NUM, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_consolidationneeded(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_CONSOLIDATION_NEEDED, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_cluster_name(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*url, *uuid;
	zbx_vmware_service_t	*service;
	zbx_vmware_cluster_t	*cluster = NULL;
	int			ret = SYSINFO_RET_FAIL;
	zbx_vmware_hv_t		*hv;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = service_hv_get_by_vm_uuid(service, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown virtual machine uuid."));
		goto unlock;
	}
	if (NULL != hv->clusterid)
		cluster = cluster_get(&service->data->clusters, hv->clusterid);

	SET_STR_RESULT(result, zbx_strdup(NULL, NULL != cluster ? cluster->name : ""));

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_cpu_ready(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;
	const char		*url, *uuid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	ret = vmware_service_get_vm_counter(service, uuid, "", "cpu/ready[summation]", 1, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_cpu_usage(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_CPU_USAGE, result);

	if (SYSINFO_RET_OK == ret && NULL != GET_UI64_RESULT(result))
		result->ui64 = result->ui64 * 1000000;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_datacenter_name(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	const char		*url, *uuid;
	int			ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = service_hv_get_by_vm_uuid(service, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown virtual machine uuid."));
		goto unlock;
	}

	SET_STR_RESULT(result, zbx_strdup(NULL, hv->datacenter_name));
	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_discovery(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	struct zbx_json		json_data;
	const char		*url, *vm_name, *hv_name, *hv_uuid;
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	zbx_vmware_vm_t		*vm;
	zbx_hashset_iter_t	iter;
	int			i, ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (1 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	zbx_json_initarray(&json_data, ZBX_JSON_STAT_BUF_LEN);

	zbx_hashset_iter_reset(&service->data->hvs, &iter);
	while (NULL != (hv = (zbx_vmware_hv_t *)zbx_hashset_iter_next(&iter)))
	{
		zbx_vmware_cluster_t	*cluster = NULL;

		if (NULL != hv->clusterid)
			cluster = cluster_get(&service->data->clusters, hv->clusterid);

		for (i = 0; i < hv->vms.values_num; i++)
		{
			int			j;
			zbx_vmware_datastore_t	*datastore = NULL;

			vm = (zbx_vmware_vm_t *)hv->vms.values[i];

			if (NULL == (vm_name = vm->props[ZBX_VMWARE_VMPROP_NAME]))
				continue;

			if (NULL == (hv_name = hv->props[ZBX_VMWARE_HVPROP_NAME]))
				continue;

			if (NULL == (hv_uuid = hv->props[ZBX_VMWARE_HVPROP_HW_UUID]))
				continue;

			for (j = 0; NULL != vm->props[ZBX_VMWARE_VMPROP_DATASTOREID] &&
				j < service->data->datastores.values_num; j++)
			{
				if (0 != strcmp(vm->props[ZBX_VMWARE_VMPROP_DATASTOREID],
						service->data->datastores.values[j]->id))
				{
					continue;
				}

				datastore = service->data->datastores.values[j];
				break;
			}

			if (NULL == datastore)
			{
				zabbix_log(LOG_LEVEL_WARNING, "%s() Unknown datastore id:%s", __func__,
						ZBX_NULL2EMPTY_STR(vm->props[ZBX_VMWARE_VMPROP_DATASTOREID]));
				continue;
			}

			zbx_json_addobject(&json_data, NULL);
			zbx_json_addstring(&json_data, "{#VM.UUID}", vm->uuid, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&json_data, "{#VM.ID}", vm->id, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&json_data, "{#VM.NAME}", vm_name, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&json_data, "{#HV.NAME}", hv_name, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&json_data, "{#HV.UUID}", hv_uuid, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&json_data, "{#HV.ID}", hv->id, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&json_data, "{#DATACENTER.NAME}", hv->datacenter_name, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&json_data, "{#CLUSTER.NAME}",
					NULL != cluster ? cluster->name : "", ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&json_data, "{#VM.IP}",
					ZBX_NULL2EMPTY_STR(vm->props[ZBX_VMWARE_VMPROP_IPADDRESS]),
					ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&json_data, "{#VM.DNS}",
					ZBX_NULL2EMPTY_STR(vm->props[ZBX_VMWARE_VMPROP_GUESTHOSTNAME]),
					ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&json_data, "{#VM.GUESTFAMILY}",
					ZBX_NULL2EMPTY_STR(vm->props[ZBX_VMWARE_VMPROP_GUESTFAMILY]),
					ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&json_data, "{#VM.GUESTFULLNAME}",
					ZBX_NULL2EMPTY_STR(vm->props[ZBX_VMWARE_VMPROP_GUESTFULLNAME]),
					ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&json_data, "{#VM.FOLDER}",
					ZBX_NULL2EMPTY_STR(vm->props[ZBX_VMWARE_VMPROP_FOLDER]), ZBX_JSON_TYPE_STRING);
			zbx_json_adduint64(&json_data, "{#VM.SNAPSHOT.COUNT}", vm->snapshot_count);
			zbx_json_addstring(&json_data, "{#VM.TOOLS.STATUS}",
					ZBX_NULL2EMPTY_STR(vm->props[ZBX_VMWARE_VMPROP_TOOLS_RUNNING_STATUS]),
					ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&json_data, "{#VM.POWERSTATE}",
					ZBX_NULL2EMPTY_STR(vm->props[ZBX_VMWARE_VMPROP_POWER_STATE]),
					ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&json_data, "{#DATASTORE.NAME}", datastore->name, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&json_data, "{#DATASTORE.UUID}", datastore->uuid, ZBX_JSON_TYPE_STRING);

			zbx_json_addstring(&json_data, "{#VM.RPOOL.ID}",
					ZBX_NULL2EMPTY_STR(vm->props[ZBX_VMWARE_VMPROP_RESOURCEPOOL]),
					ZBX_JSON_TYPE_STRING);

			if (NULL != vm->props[ZBX_VMWARE_VMPROP_RESOURCEPOOL])
			{
				zbx_vmware_resourcepool_t	rpool_cmp;
				int				idx;

				rpool_cmp.id = vm->props[ZBX_VMWARE_VMPROP_RESOURCEPOOL];

				if (FAIL != (idx = zbx_vector_vmware_resourcepool_bsearch(&service->data->resourcepools,
						&rpool_cmp, vmware_resourcepool_compare_id)))
				{
					zbx_json_addstring(&json_data, "{#VM.RPOOL.PATH}",
							ZBX_NULL2EMPTY_STR(service->data->resourcepools.values[idx]->path),
							ZBX_JSON_TYPE_STRING);
				}
				else
					zbx_json_addstring(&json_data, "{#VM.RPOOL.PATH}", "", ZBX_JSON_TYPE_STRING);
			}
			else
				zbx_json_addstring(&json_data, "{#VM.RPOOL.PATH}", "", ZBX_JSON_TYPE_STRING);

			zbx_json_addarray(&json_data, "vm.customattribute");

			for (j = 0; j < vm->custom_attrs.values_num; j++)
			{
				zbx_json_addobject(&json_data, NULL);
				zbx_json_addstring(&json_data, "name",
						vm->custom_attrs.values[j]->name, ZBX_JSON_TYPE_STRING);
				zbx_json_addstring(&json_data, "value",
						vm->custom_attrs.values[j]->value, ZBX_JSON_TYPE_STRING);
				zbx_json_close(&json_data);
			}

			zbx_json_close(&json_data);
			zbx_json_close(&json_data);
		}
	}

	zbx_json_close(&json_data);
	SET_STR_RESULT(result, zbx_strdup(NULL, json_data.buffer));
	zbx_json_free(&json_data);

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_hv_name(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	zbx_vmware_hv_t		*hv;
	const char		*url, *uuid, *name;
	int			ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (hv = service_hv_get_by_vm_uuid(service, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown virtual machine uuid."));
		goto unlock;
	}

	if (NULL == (name = hv->props[ZBX_VMWARE_HVPROP_NAME]))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "No hypervisor name found."));
		goto unlock;
	}

	SET_STR_RESULT(result, zbx_strdup(NULL, name));
	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_memory_size(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_MEMORY_SIZE, result);

	if (SYSINFO_RET_OK == ret && NULL != GET_UI64_RESULT(result))
		result->ui64 = result->ui64 * ZBX_MEBIBYTE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_memory_size_ballooned(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_MEMORY_SIZE_BALLOONED, result);

	if (SYSINFO_RET_OK == ret && NULL != GET_UI64_RESULT(result))
		result->ui64 = result->ui64 * ZBX_MEBIBYTE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_memory_size_compressed(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_MEMORY_SIZE_COMPRESSED, result);

	if (SYSINFO_RET_OK == ret && NULL != GET_UI64_RESULT(result))
		result->ui64 = result->ui64 * ZBX_MEBIBYTE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_memory_size_swapped(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_MEMORY_SIZE_SWAPPED, result);

	if (SYSINFO_RET_OK == ret && NULL != GET_UI64_RESULT(result))
		result->ui64 = result->ui64 * ZBX_MEBIBYTE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_memory_size_usage_guest(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_MEMORY_SIZE_USAGE_GUEST, result);

	if (SYSINFO_RET_OK == ret && NULL != GET_UI64_RESULT(result))
		result->ui64 = result->ui64 * ZBX_MEBIBYTE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_memory_size_usage_host(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_MEMORY_SIZE_USAGE_HOST, result);

	if (SYSINFO_RET_OK == ret && NULL != GET_UI64_RESULT(result))
		result->ui64 = result->ui64 * ZBX_MEBIBYTE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_memory_size_private(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_MEMORY_SIZE_PRIVATE, result);

	if (SYSINFO_RET_OK == ret && NULL != GET_UI64_RESULT(result))
		result->ui64 = result->ui64 * ZBX_MEBIBYTE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_memory_size_shared(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_MEMORY_SIZE_SHARED, result);

	if (SYSINFO_RET_OK == ret && NULL != GET_UI64_RESULT(result))
		result->ui64 = result->ui64 * ZBX_MEBIBYTE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_powerstate(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_POWER_STATE, result);

	if (SYSINFO_RET_OK == ret)
		ret = vmware_set_powerstate_result(result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_snapshot_get(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_SNAPSHOT, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

typedef void	(*vmpropfunc_t)(struct zbx_json *j, zbx_vmware_dev_t *dev);

static void	check_vcenter_vm_discovery_nic_props_cb(struct zbx_json *j, zbx_vmware_dev_t *dev)
{
	zbx_json_addstring(j, "{#IFNAME}", ZBX_NULL2EMPTY_STR(dev->instance), ZBX_JSON_TYPE_STRING);
	zbx_json_addstring(j, "{#IFDESC}", ZBX_NULL2EMPTY_STR(dev->label), ZBX_JSON_TYPE_STRING);
	zbx_json_addstring(j, "{#IFMAC}", ZBX_NULL2EMPTY_STR(dev->props[ZBX_VMWARE_DEV_PROPS_IFMAC]),
			ZBX_JSON_TYPE_STRING);
	zbx_json_addstring(j, "{#IFCONNECTED}", ZBX_NULL2EMPTY_STR(dev->props[ZBX_VMWARE_DEV_PROPS_IFCONNECTED]),
			ZBX_JSON_TYPE_INT);
	zbx_json_addstring(j, "{#IFTYPE}", ZBX_NULL2EMPTY_STR(dev->props[ZBX_VMWARE_DEV_PROPS_IFTYPE]),
			ZBX_JSON_TYPE_STRING);
	zbx_json_addstring(j, "{#IFBACKINGDEVICE}", ZBX_NULL2EMPTY_STR(dev->props[ZBX_VMWARE_DEV_PROPS_IFBACKINGDEVICE]),
			ZBX_JSON_TYPE_STRING);
	zbx_json_addstring(j, "{#IFDVSWITCH.UUID}", ZBX_NULL2EMPTY_STR(
			dev->props[ZBX_VMWARE_DEV_PROPS_IFDVSWITCH_UUID]), ZBX_JSON_TYPE_STRING);
	zbx_json_addstring(j, "{#IFDVSWITCH.PORTGROUP}", ZBX_NULL2EMPTY_STR(
			dev->props[ZBX_VMWARE_DEV_PROPS_IFDVSWITCH_PORTGROUP]), ZBX_JSON_TYPE_STRING);
	zbx_json_addstring(j, "{#IFDVSWITCH.PORT}", ZBX_NULL2EMPTY_STR(
			dev->props[ZBX_VMWARE_DEV_PROPS_IFDVSWITCH_PORT]), ZBX_JSON_TYPE_STRING);
}

static void	check_vcenter_vm_discovery_disk_props_cb(struct zbx_json *j, zbx_vmware_dev_t *dev)
{
	zbx_json_addstring(j, "{#DISKNAME}", ZBX_NULL2EMPTY_STR(dev->instance), ZBX_JSON_TYPE_STRING);
	zbx_json_addstring(j, "{#DISKDESC}", ZBX_NULL2EMPTY_STR(dev->label), ZBX_JSON_TYPE_STRING);
}

static int	check_vcenter_vm_discovery_common(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result, int dev_type, const char *func_parent, vmpropfunc_t props_cb)
{
	struct zbx_json		json_data;
	zbx_vmware_service_t	*service;
	zbx_vmware_vm_t		*vm = NULL;
	zbx_vmware_dev_t	*dev;
	const char		*url, *uuid;
	int			i, ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s(), func_parent:'%s'", __func__, func_parent);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (vm = service_vm_get(service, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown virtual machine uuid."));
		goto unlock;
	}

	zbx_json_initarray(&json_data, ZBX_JSON_STAT_BUF_LEN);

	for (i = 0; i < vm->devs.values_num; i++)
	{
		dev = (zbx_vmware_dev_t *)vm->devs.values[i];

		if (dev_type != dev->type)
			continue;

		if (NULL != props_cb)
		{
			zbx_json_addobject(&json_data, NULL);
			props_cb(&json_data, dev);
			zbx_json_close(&json_data);
		}
	}

	zbx_json_close(&json_data);

	SET_STR_RESULT(result, zbx_strdup(NULL, json_data.buffer));

	zbx_json_free(&json_data);

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(), func_parent:'%s', ret: %s", __func__, func_parent,
			zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_net_if_discovery(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return check_vcenter_vm_discovery_common(request, username, password, result, ZBX_VMWARE_DEV_TYPE_NIC, __func__,
			check_vcenter_vm_discovery_nic_props_cb);
}

static int	check_vcenter_vm_common(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result, const char *mode_in, const char *countername_mode, const char *countername_def,
		const char *func_parent)
{
	zbx_vmware_service_t	*service;
	const char		*path, *url, *uuid, *instance, *mode;
	unsigned int		coeff;
	int			ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s(), func_parent:'%s'", __func__, func_parent);

	if (3 > request->nparam || request->nparam > 4)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);
	instance = get_rparam(request, 2);
	mode = get_rparam(request, 3);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	if ('\0' == *instance)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == mode || '\0' == *mode || 0 == strcmp(mode, "bps"))
	{
		path = countername_def;
		coeff = ZBX_KIBIBYTE;
	}
	else if (0 == strcmp(mode, mode_in))
	{
		path = countername_mode;
		coeff = 1;
	}
	else
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid fourth parameter."));
		goto unlock;
	}

	ret = vmware_service_get_vm_counter(service, uuid, instance, path, coeff, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(), func_parent:'%s', ret: %s", __func__, func_parent,
			zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_net_if_in(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return check_vcenter_vm_common(request, username, password, result, "pps", "net/packetsRx[summation]",
			"net/received[average]", __func__);
}

int	check_vcenter_vm_net_if_out(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return check_vcenter_vm_common(request, username, password, result, "pps", "net/packetsTx[summation]",
			"net/transmitted[average]",  __func__);
}

int	check_vcenter_vm_state(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_STATE, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_storage_committed(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_STORAGE_COMMITED, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_storage_unshared(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_STORAGE_UNSHARED, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_storage_uncommitted(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_STORAGE_UNCOMMITTED, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_tools(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	zbx_vmware_vm_t		*vm = NULL;
	int			propid, ret = SYSINFO_RET_FAIL;
	const char		*url, *uuid, *mode, *value;

	if (3 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);
	mode = get_rparam(request, 2);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	if (NULL == mode || '\0' == *mode)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter."));
		goto out;
	}
	else if (0 == strcmp(mode, "version"))
	{
		propid = ZBX_VMWARE_VMPROP_TOOLS_VERSION;
	}
	else if (0 == strcmp(mode, "status"))
	{
		propid = ZBX_VMWARE_VMPROP_TOOLS_RUNNING_STATUS;
	}
	else
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter value."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (vm = service_vm_get(service, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown virtual machine uuid."));
		goto unlock;
	}

	if (NULL == (value = vm->props[propid]))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Value is not available."));
		goto unlock;
	}

	if (ZBX_VMWARE_VMPROP_TOOLS_VERSION == propid)
		SET_UI64_RESULT(result, atoi(value));
	else
		SET_STR_RESULT(result, zbx_strdup(NULL, value));

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_uptime(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = get_vcenter_vmprop(request, username, password, ZBX_VMWARE_VMPROP_UPTIME, result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_vfs_dev_discovery(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return check_vcenter_vm_discovery_common(request, username, password, result, ZBX_VMWARE_DEV_TYPE_DISK, __func__,
			check_vcenter_vm_discovery_disk_props_cb);
}

int	check_vcenter_vm_vfs_dev_read(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return check_vcenter_vm_common(request, username, password, result, "ops",
			"virtualDisk/numberReadAveraged[average]", "virtualDisk/read[average]", __func__);
}

int	check_vcenter_vm_vfs_dev_write(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return check_vcenter_vm_common(request, username, password, result, "ops",
			"virtualDisk/numberWriteAveraged[average]", "virtualDisk/write[average]", __func__);
}

int	check_vcenter_vm_vfs_fs_discovery(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	struct zbx_json		json_data;
	zbx_vmware_service_t	*service;
	zbx_vmware_vm_t		*vm = NULL;
	const char		*url, *uuid;
	int			i, ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (vm = service_vm_get(service, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown virtual machine uuid."));
		goto unlock;
	}

	zbx_json_initarray(&json_data, ZBX_JSON_STAT_BUF_LEN);

	for (i = 0; i < vm->file_systems.values_num; i++)
	{
		zbx_vmware_fs_t	*fs = (zbx_vmware_fs_t *)vm->file_systems.values[i];

		zbx_json_addobject(&json_data, NULL);
		zbx_json_addstring(&json_data, "{#FSNAME}", fs->path, ZBX_JSON_TYPE_STRING);
		zbx_json_close(&json_data);
	}

	zbx_json_close(&json_data);

	SET_STR_RESULT(result, zbx_strdup(NULL, json_data.buffer));

	zbx_json_free(&json_data);

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_vfs_fs_size(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	zbx_vmware_vm_t		*vm;
	const char		*url, *uuid, *fsname, *mode;
	int			i, ret = SYSINFO_RET_FAIL;
	zbx_vmware_fs_t		*fs = NULL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (3 > request->nparam || request->nparam > 4)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);
	fsname = get_rparam(request, 2);
	mode = get_rparam(request, 3);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (vm = service_vm_get(service, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown virtual machine uuid."));
		goto unlock;
	}

	for (i = 0; i < vm->file_systems.values_num; i++)
	{
		fs = (zbx_vmware_fs_t *)vm->file_systems.values[i];

		if (0 == strcmp(fs->path, fsname))
			break;
	}

	if (NULL == fs)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown file system path."));
		goto unlock;
	}

	ret = SYSINFO_RET_OK;

	if (NULL == mode || '\0' == *mode || 0 == strcmp(mode, "total"))
		SET_UI64_RESULT(result, fs->capacity);
	else if (0 == strcmp(mode, "free"))
		SET_UI64_RESULT(result, fs->free_space);
	else if (0 == strcmp(mode, "used"))
		SET_UI64_RESULT(result, fs->capacity - fs->free_space);
	else if (0 == strcmp(mode, "pfree"))
		SET_DBL_RESULT(result, 0 != fs->capacity ? (double)(100.0 * fs->free_space) / fs->capacity : 0);
	else if (0 == strcmp(mode, "pused"))
		SET_DBL_RESULT(result, 100.0 - (0 != fs->capacity ? 100.0 * fs->free_space / fs->capacity : 0));
	else
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid fourth parameter."));
		ret = SYSINFO_RET_FAIL;
	}
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_perfcounter(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*instance, *url, *uuid, *path;
	zbx_vmware_service_t	*service;
	zbx_vmware_vm_t		*vm;
	zbx_uint64_t		counterid;
	int			unit, ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (3 > request->nparam || request->nparam > 4)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);
	path = get_rparam(request, 2);
	instance = get_rparam(request, 3);

	if (NULL == instance)
		instance = "";

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	if (NULL == (vm = service_vm_get(service, uuid)))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Unknown virtual machine uuid."));
		goto unlock;
	}

	if (FAIL == zbx_vmware_service_get_counterid(service, path, &counterid, &unit))
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Performance counter is not available."));
		goto unlock;
	}

	/* FAIL is returned if counter already exists */
	if (SUCCEED == zbx_vmware_service_add_perf_counter(service, "VirtualMachine", vm->id, counterid, "*"))
	{
		ret = SYSINFO_RET_OK;
		goto unlock;
	}

	/* the performance counter is already being monitored, try to get the results from statistics */
	ret = vmware_service_get_counter_value_by_id(service, "VirtualMachine", vm->id, counterid, instance, 1, unit,
			result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_dc_discovery(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	const char		*url;
	zbx_vmware_service_t	*service;
	struct zbx_json		json_data;
	int			i, ret = SYSINFO_RET_FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (1 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	zbx_json_initarray(&json_data, ZBX_JSON_STAT_BUF_LEN);

	for (i = 0; i < service->data->datacenters.values_num; i++)
	{
		zbx_vmware_datacenter_t	*datacenter = service->data->datacenters.values[i];

		zbx_json_addobject(&json_data, NULL);
		zbx_json_addstring(&json_data, "{#DATACENTER}", datacenter->name, ZBX_JSON_TYPE_STRING);
		zbx_json_addstring(&json_data, "{#DATACENTERID}", datacenter->id, ZBX_JSON_TYPE_STRING);
		zbx_json_close(&json_data);
	}

	zbx_json_close(&json_data);

	SET_STR_RESULT(result, zbx_strdup(NULL, json_data.buffer));

	zbx_json_free(&json_data);

	ret = SYSINFO_RET_OK;
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_net_if_usage(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;
	const char		*url, *uuid, *instance;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 > request->nparam || request->nparam > 3)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);
	instance = get_rparam(request, 2);

	if (NULL == instance)
		instance = "";

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	ret = vmware_service_get_vm_counter(service, uuid, instance, "net/usage[average]", ZBX_KIBIBYTE, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_guest_memory_size_swapped(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;
	const char		*url, *uuid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	ret = vmware_service_get_vm_counter(service, uuid, "", "mem/swapped[average]", ZBX_KIBIBYTE, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_memory_size_consumed(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;
	const char		*url, *uuid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	ret = vmware_service_get_vm_counter(service, uuid, "", "mem/consumed[average]", ZBX_KIBIBYTE, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_memory_usage(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;
	const char		*url, *uuid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	ret = vmware_service_get_vm_counter(service, uuid, "", "mem/usage[average]", 0, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_cpu_latency(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;
	const char		*url, *uuid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	ret = vmware_service_get_vm_counter(service, uuid, "", "cpu/latency[average]", 0, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_cpu_readiness(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;
	const char		*url, *uuid, *instance;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 > request->nparam || request->nparam > 3)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);
	instance = get_rparam(request, 2);

	if (NULL == instance)
		instance = "";

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	ret = vmware_service_get_vm_counter(service, uuid, instance, "cpu/readiness[average]", 0, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_cpu_swapwait(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;
	const char		*url, *uuid, *instance;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 > request->nparam || request->nparam > 3)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);
	instance = get_rparam(request, 2);

	if (NULL == instance)
		instance = "";

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	ret = vmware_service_get_vm_counter(service, uuid, instance, "cpu/swapwait[summation]", 0, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_cpu_usage_perf(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;
	const char		*url, *uuid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	ret = vmware_service_get_vm_counter(service, uuid, "", "cpu/usage[average]", 0, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

static int	check_vcenter_vm_storage_common(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result, const char *counter_name,  const char *func_parent)
{
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;
	const char		*url, *uuid, *instance;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s(), func_parent:'%s'", __func__, func_parent);

	if (3 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);
	instance = get_rparam(request, 2);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	if ('\0' == *instance)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid third parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	ret = vmware_service_get_vm_counter(service, uuid, instance, counter_name, 1, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(), func_parent:'%s', ret: %s", __func__, func_parent,
			zbx_sysinfo_ret_string(ret));

	return ret;
}

int	check_vcenter_vm_storage_readoio(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return check_vcenter_vm_storage_common(request, username, password, result, "virtualDisk/readOIO[latest]",
			__func__);
}

int	check_vcenter_vm_storage_writeoio(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return check_vcenter_vm_storage_common(request, username, password, result, "virtualDisk/writeOIO[latest]",
			__func__);
}

int	check_vcenter_vm_storage_totalwritelatency(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return check_vcenter_vm_storage_common(request, username, password, result,
			"virtualDisk/totalWriteLatency[average]", __func__);
}

int	check_vcenter_vm_storage_totalreadlatency(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	return check_vcenter_vm_storage_common(request, username, password, result,
			"virtualDisk/totalReadLatency[average]", __func__);
}

int	check_vcenter_vm_guest_uptime(AGENT_REQUEST *request, const char *username, const char *password,
		AGENT_RESULT *result)
{
	zbx_vmware_service_t	*service;
	int			ret = SYSINFO_RET_FAIL;
	const char		*url, *uuid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (2 != request->nparam)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid number of parameters."));
		goto out;
	}

	url = get_rparam(request, 0);
	uuid = get_rparam(request, 1);

	if ('\0' == *uuid)
	{
		SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid second parameter."));
		goto out;
	}

	zbx_vmware_lock();

	if (NULL == (service = get_vmware_service(url, username, password, result, &ret)))
		goto unlock;

	ret = vmware_service_get_vm_counter(service, uuid, "", "sys/osUptime[latest]", 1, result);
unlock:
	zbx_vmware_unlock();
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_sysinfo_ret_string(ret));

	return ret;
}

#endif	/* defined(HAVE_LIBXML2) && defined(HAVE_LIBCURL) */
