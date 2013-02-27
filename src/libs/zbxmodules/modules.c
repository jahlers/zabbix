/*
** Zabbix
** Copyright (C) 2000-2011 Zabbix SIA
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

#include "common.h"
#include "module.h"
#include "zbxmodules.h"

#include "log.h"
#include "sysinfo.h"

#define ZBX_MODULE_FUNC_INIT		"zbx_module_init"
#define ZBX_MODULE_FUNC_API_VERSION	"zbx_module_api_version"
#define ZBX_MODULE_FUNC_ITEM_LIST	"zbx_module_item_list"
#define ZBX_MODULE_FUNC_ITEM_PROCESS	"zbx_module_item_process"
#define ZBX_MODULE_FUNC_ITEM_TIMEOUT	"zbx_module_item_timeout"
#define ZBX_MODULE_FUNC_UNINIT		"zbx_module_uninit"

static void	**modules = NULL;

/******************************************************************************
 *                                                                            *
 * Function: register_module                                                  *
 *                                                                            *
 * Purpose: Add module to the list of loaded modules (dynamic libraries).     *
 *          It skips a module if it is already registered.                    *
 *                                                                            *
 * Parameters: module - library handler                                       *
 *                                                                            *
 ******************************************************************************/
static void	register_module(void *module)
{
	const char	*__function_name = "register_module";

	int		i = 0;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (NULL == modules)
	{
		modules = zbx_malloc(modules, sizeof(void *));
		modules[0] = NULL;
	}

	while (NULL != modules[i])
	{
		if (module == modules[i])	/* a module is already registered */
			goto out;
		i++;
	}

	modules = zbx_realloc(modules, (i + 2) * sizeof(void *));
	modules[i] = module;
	modules[i + 1] = NULL;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: load_modules                                                     *
 *                                                                            *
 * Purpose: load loadable modules (dynamic libraries)                         *
 *          It skips a module in case of any errors                           *
 *                                                                            *
 * Parameters: path - directory where modules are located                     *
 *             modules - list of module names                                 *
 *             timeout - timeout in seconds for processing of items by module *
 *                                                                            *
 * Return value: 0 - success                                                  *
 *               -1 - loading of modules failed                               *
 *                                                                            *
 ******************************************************************************/
int	load_modules(const char *path, char **modules, int timeout)
{
	const char	*__function_name = "load_modules";

	char		**module;
	ZBX_METRIC	*metrics;
	void		*lib;
	char		filename[MAX_STRING_LEN];
	int		(*func_init)(), (*func_version)();
	ZBX_METRIC	*(*func_list)();
	void		(*func_timeout)();
	int		i, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	for (module = modules; NULL != *module; module++)
	{
		zbx_snprintf(filename, sizeof(filename), "%s/%s", path, *module);

		zabbix_log(LOG_LEVEL_DEBUG, "loading module \"%s\"", filename);
		if (NULL == (lib = dlopen(filename, RTLD_NOW)))
		{
			zabbix_log(LOG_LEVEL_WARNING, "cannot load module \"%s\": %s", filename, dlerror());
			goto fail;
		}

		*(void **)(&func_version) = dlsym(lib, ZBX_MODULE_FUNC_API_VERSION);
		if (NULL == func_version)
		{
			zabbix_log(LOG_LEVEL_WARNING, "cannot find \"" ZBX_MODULE_FUNC_API_VERSION "()\""
					" function in module \"%s\": %s", *module, dlerror());
			dlclose(lib);
			goto fail;
		}

		if (ZBX_MODULE_API_VERSION_ONE != (i = func_version()))
		{
			zabbix_log(LOG_LEVEL_WARNING, "unsupported module \"%s\" version: %d", *module, i);
			dlclose(lib);
			goto fail;
		}

		*(void **)(&func_init) = dlsym(lib, ZBX_MODULE_FUNC_INIT);
		if (NULL == func_init)
		{
			zabbix_log(LOG_LEVEL_WARNING, "cannot find \"" ZBX_MODULE_FUNC_INIT "()\""
					" function in module \"%s\": %s", *module, dlerror());
			dlclose(lib);
			goto fail;
		}

		if (ZBX_MODULE_OK != func_init())
		{
			zabbix_log(LOG_LEVEL_WARNING, "cannot initialize module \"%s\"", *module);
			dlclose(lib);
			goto fail;
		}

		/* the function is optional, zabbix will load the module ieven if it is missing */
		*(void **)(&func_timeout) = dlsym(lib, ZBX_MODULE_FUNC_ITEM_TIMEOUT);
		if (NULL == func_timeout)
		{
			zabbix_log(LOG_LEVEL_DEBUG, "cannot find \"" ZBX_MODULE_FUNC_ITEM_TIMEOUT "()\""
					" function in module \"%s\": %s", *module, dlerror());
		}
		else
			func_timeout(timeout);

		*(void **)(&func_list) = dlsym(lib, ZBX_MODULE_FUNC_ITEM_LIST);
		if (NULL == func_list)
		{
			zabbix_log(LOG_LEVEL_WARNING, "cannot find \"" ZBX_MODULE_FUNC_ITEM_LIST "()\""
					" function in module \"%s\": %s", *module, dlerror());
			dlclose(lib);
			continue;
		}

		metrics = func_list();
		for (i = 0; NULL != metrics[i].key; i++)
		{
			/* accept only CF_HAVEPARAMS flag from module items */
			metrics[i].flags &= CF_HAVEPARAMS;
			/* the flag means that the items comes from a loadable module */
			metrics[i].flags |= CF_MODULE;
			add_metric(&metrics[i]);
		}

		register_module(lib);
	}

	ret = SUCCEED;
fail:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: unload_modules                                                   *
 *                                                                            *
 * Purpose: Unload already loaded loadable modules (dynamic libraries).       *
 *          It is called on process shutdown.                                 *
 *                                                                            *
 ******************************************************************************/
void	unload_modules()
{
	const char	*__function_name = "unload_modules";

	int		(*func_uninit)();
	void		**module;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	/* there is no registered modules */
	if (NULL == modules)
		return;

	for (module = modules; NULL != *module; module++)
	{
		*(void **)(&func_uninit) = dlsym(*module, ZBX_MODULE_FUNC_UNINIT);
		if (NULL == func_uninit)
		{
			zabbix_log(LOG_LEVEL_DEBUG, "cannot find \"" ZBX_MODULE_FUNC_UNINIT "()\" function: %s",
					dlerror());
			dlclose(*module);
			continue;
		}

		if (ZBX_MODULE_OK != func_uninit())
			zabbix_log(LOG_LEVEL_WARNING, "uninitialization failed");

		dlclose(*module);
	}

	zbx_free(modules);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}
