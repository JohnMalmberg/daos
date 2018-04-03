/**
 * (C) Copyright 2015, 2016 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
#define D_LOGFAC	DD_FAC(client)

#include <daos/common.h>
#include <daos/tier.h>
#include <daos/pool.h>
#include "client_internal.h"
#include "task_internal.h"
#include "../tier/cli_internal.h"


struct xconn_arg {
	uuid_t		uuid;
	char		grp[128];
	daos_handle_t	*poh;
	daos_event_t	*evp;
};

/*Task Callbacks for cascading and dependent RPCs*/
static int cross_conn_cb(tse_task_t *task, void *data)
{
	struct xconn_arg *cb_arg = (struct xconn_arg *)data;
	int		      rc = task->dt_result;

	daos_event_complete(cb_arg->evp, rc);
	return 0;
}


static int
local_tier_conn_cb(tse_task_t *task, void *data)
{

	tse_sched_t		*sched;
	tse_task_t		*cross_conn_task;
	struct xconn_arg	*cb_arg = (struct xconn_arg *)data;
	int			rc = task->dt_result;


	/*Check for task error*/
	if (rc) {
		D_ERROR("Tier Conn task returned error:%d\n", rc);
		return rc;
	}

	/*Grab Scheduler of the task*/
	sched = tse_task2sched(task);

	rc = tse_task_create(NULL, sched, NULL, &cross_conn_task);
	if (rc != 0)
		return -DER_NOMEM;

	rc = tse_task_register_comp_cb(cross_conn_task, cross_conn_cb,
				       cb_arg, sizeof(struct xconn_arg));
	if (rc != 0) {
		D_ERROR("Failed to register completion callback: %d\n", rc);
		return rc;
	}

	rc = tse_task_schedule(cross_conn_task, false);
	if (rc != 0)
		return rc;

	rc = dc_tier_connect(cb_arg->uuid, cb_arg->grp, cross_conn_task);
	if (rc != 0) {
		D_ERROR("Error from dc_tier_connect: %d\n", rc);
		return rc;
	}

	return rc;
}


int
daos_tier_fetch_cont(daos_handle_t poh, const uuid_t cont_id,
		     daos_epoch_t fetch_ep, daos_oid_list_t *obj_list,
		     daos_event_t *ev)
{
	tse_task_t	*task;
	int		rc;

	rc = dc_task_create(NULL, NULL, ev, &task);
	if (rc != 0)
		return rc;

	/* XXX this is a hack and can't work anymore */
	dc_tier_fetch_cont(poh, cont_id, fetch_ep, obj_list, task);
	return dc_task_schedule(task, true);
}

int daos_tier_pool_connect(const uuid_t uuid, const char *grp,
		    const d_rank_list_t *svc, unsigned int flags,
		    daos_handle_t *poh, daos_pool_info_t *info,
		    daos_event_t *ev)
{
	int			rc = 0;
	tse_task_t		*local_conn_task = NULL;
	struct xconn_arg	*cb_arg;
	daos_tier_info_t	*pt;
	daos_pool_connect_t	*pc_args;


	/*Note CB arg (on task complete) is freed implicitly by scheduler
	* See daos_task_complete_callback in scheduler.c
	*/
	D_ALLOC_PTR(cb_arg);
	if (cb_arg == NULL)
		return -DER_NOMEM;

	/* Make copies of the UUID and group for our our connection work
	* triggered in the callback
	*/
	uuid_copy(cb_arg->uuid, uuid);
	strcpy(cb_arg->grp, grp);
	cb_arg->evp = ev;
	cb_arg->poh = poh;

	/*Client prep, plus a manual callback register to  add our CB arg*/
	rc = dc_task_create(dc_pool_connect, NULL, ev, &local_conn_task);
	if (rc) {
		D_ERROR("Error in client task prep: %d\n", rc);
		return rc;
	}

	rc = tse_task_register_comp_cb(local_conn_task, local_tier_conn_cb,
				       cb_arg, sizeof(struct xconn_arg));

	if (rc) {
		D_ERROR("Error registering comp cb: %d\n", rc);
		return rc;
	}

	/*Connect to local pool, if this succeeeds, its noted in the CB,
	 * which will trigger the cross connect logic
	 */
	pt = tier_lookup(grp);
	if (pt == NULL)
		D_WARN("No client context, connectivity may be limited\n");

	pc_args = dc_task_get_args(local_conn_task);
	uuid_copy((unsigned char *)pc_args->uuid, uuid);
	pc_args->grp	= grp;
	pc_args->svc	= svc;
	pc_args->flags	= flags;
	pc_args->poh	= poh;
	pc_args->info	= info;

	rc = dc_task_schedule(local_conn_task, true);
	if (rc)
		D_ERROR("Error to schedule connect task: %d\n", rc);

	return rc;
}

int
daos_tier_register_cold(const uuid_t colder_id, const char *colder_grp,
			const uuid_t tgt_uuid, char *tgt_grp, daos_event_t *ev)
{

	int rc;
	tse_task_t *trc_task;

	rc = dc_task_create(NULL, NULL, ev, &trc_task);
	if (rc)
		return rc;

	/* XXX this is a hack and can't work anymore */
	rc = dc_tier_register_cold(colder_id, colder_grp, tgt_grp, trc_task);
	if (rc)
		return rc;

	return dc_task_schedule(trc_task, true);
}

void
daos_tier_setup_client_ctx(const uuid_t colder_id, const char *colder_grp,
			   daos_handle_t *cold_poh, const uuid_t tgt_uuid,
			   const char *tgt_grp, daos_handle_t *warm_poh)
{
	daos_tier_info_t	*pt;

	/*Allocates and sets up tier context stuff*/
	tier_setup_this_tier(tgt_uuid, tgt_grp);
	tier_setup_cold_tier(colder_id, colder_grp);

	/*Assign POHs*/
	pt = tier_lookup(tgt_grp);
	if (pt != NULL && warm_poh != NULL)
		pt->ti_poh = *warm_poh;

	pt = tier_lookup(colder_grp);
	if (pt != NULL && cold_poh != NULL)
		pt->ti_poh = *cold_poh;

}

int
daos_tier_ping(uint32_t ping_val, daos_event_t *ev)
{
	tse_task_t	*task;
	int		rc;

	rc = dc_task_create(NULL, NULL, ev, &task);
	if (rc != 0)
		return rc;

	/* XXX this is a hack and can't work anymore */
	dc_tier_ping(ping_val, task);
	return dc_task_schedule(task, true);
}
