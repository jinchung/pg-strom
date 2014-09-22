/*
 * gpuscan.c
 *
 * Sequential scan accelerated by GPU processors
 * ----
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This software is an extension of PostgreSQL; You can use, copy,
 * modify or distribute it under the terms of 'LICENSE' included
 * within this package.
 */
#include "postgres.h"
#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "catalog/pg_namespace.h"
#include "commands/explain.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/execnodes.h"
#include "nodes/makefuncs.h"
#include "nodes/plannodes.h"
#include "nodes/relation.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/spccache.h"
#include "utils/tqual.h"
#include "pg_strom.h"
#include "opencl_gpuscan.h"

static add_scan_path_hook_type	add_scan_path_next;
static CustomPathMethods		gpuscan_path_methods;
static CustomPlanMethods		gpuscan_plan_methods;
static bool						enable_gpuscan;

typedef struct {
	CustomPath	cpath;
	List	   *dev_quals;		/* RestrictInfo run on device */
	List	   *host_quals;		/* RestrictInfo run on host */
	Bitmapset  *dev_attnums;	/* attnums referenced in device */
	Bitmapset  *host_attnums;	/* attnums referenced in host */
	bool		syscol_referenced; /* true, if system column is referenced */
} GpuScanPath;

typedef struct {
	CustomPlan	cplan;
	Index		scanrelid;		/* index of the range table */
	const char *kern_source;	/* source of opencl kernel */
	int			extra_flags;	/* extra libraries to be included */
	List	   *used_params;	/* list of Const/Param in use */
	List	   *used_vars;		/* list of Var in use */
	List	   *dev_clauses;	/* clauses to be run on device */
	Bitmapset  *dev_attnums;	/* attnums referenced in device */
	Bitmapset  *host_attnums;	/* attnums referenced in host */
	bool		syscol_referenced; /* true, if system column is referenced */
} GpuScanPlan;

typedef struct {
	CustomPlanState		cps;
	Relation			scan_rel;
	TupleTableSlot	   *scan_slot;
	HeapTupleData		scan_tuple;
	BlockNumber			curr_blknum;
	BlockNumber			last_blknum;
	double				ntup_per_page;
	List			   *dev_quals;
	bool				syscol_referenced;

	pgstrom_queue	   *mqueue;
	Datum				dprog_key;
	kern_parambuf	   *kparams;
	List			   *bulk_attmap;

	pgstrom_gpuscan	   *curr_chunk;
	uint32				curr_index;
	int					num_running;
	dlist_head			ready_chunks;

	pgstrom_perfmon		pfm;	/* sum of performance counter */
} GpuScanState;

/* bitmask of all system columns; for convenience */
#define BITMASK_ALL_SYSCOLS							\
	((1 << (InvalidAttrNumber -						\
			FirstLowInvalidHeapAttributeNumber)) |	\
	 (1 << (SelfItemPointerAttributeNumber -		\
			FirstLowInvalidHeapAttributeNumber)) |	\
	 (1 << (ObjectIdAttributeNumber -				\
			FirstLowInvalidHeapAttributeNumber)) |	\
	 (1 << (MinTransactionIdAttributeNumber -		\
			FirstLowInvalidHeapAttributeNumber)) |	\
	 (1 << (MinCommandIdAttributeNumber -			\
			FirstLowInvalidHeapAttributeNumber)) |	\
	 (1 << (MaxTransactionIdAttributeNumber -		\
			FirstLowInvalidHeapAttributeNumber)) |	\
	 (1 << (MaxCommandIdAttributeNumber -			\
			FirstLowInvalidHeapAttributeNumber)) |	\
	 (1 << (TableOidAttributeNumber -				\
			FirstLowInvalidHeapAttributeNumber)))

/* static functions */
static void clserv_process_gpuscan(pgstrom_message *msg);
static void clserv_put_gpuscan(pgstrom_message *msg);

/*
 * cost_gpuscan
 *
 * cost estimation for GpuScan
 */
static void
cost_gpuscan(GpuScanPath *gpu_path, PlannerInfo *root,
			 RelOptInfo *baserel, ParamPathInfo *param_info,
			 List *dev_quals, List *host_quals)
{
	Path	   *path = &gpu_path->cpath.path;
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	double		spc_seq_page_cost;
	QualCost	dev_cost;
	QualCost	host_cost;
	Cost		gpu_per_tuple;
	Cost		cpu_per_tuple;
	Selectivity	dev_sel;

	/* Should only be applied to base relations */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	/* Mark the path with the correct row estimate */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/* fetch estimated page cost for tablespace containing table */
	get_tablespace_page_costs(baserel->reltablespace,
							  NULL,
							  &spc_seq_page_cost);
	/*
	 * disk costs
	 * XXX - needs to adjust after columner cache in case of bare heapscan,
	 * or partial heapscan if targetlist references out of cached columns
	 */
    run_cost += spc_seq_page_cost * baserel->pages;

	/* GPU costs */
	cost_qual_eval(&dev_cost, dev_quals, root);
	dev_sel = clauselist_selectivity(root, dev_quals, 0, JOIN_INNER, NULL);

	/*
	 * XXX - very rough estimation towards GPU startup and device calculation
	 *       to be adjusted according to device info
	 *
	 * TODO: startup cost takes NITEMS_PER_CHUNK * width to be carried, but
	 * only first chunk because data transfer is concurrently done, if NOT
	 * integrated GPU
	 * TODO: per_tuple calculation cost shall be divided by parallelism of
	 * average opencl spec.
	 */
	dev_cost.startup += 10000;
	dev_cost.per_tuple /= 100;

	/* CPU costs */
	cost_qual_eval(&host_cost, host_quals, root);
	if (param_info)
	{
		QualCost	param_cost;

		/* Include costs of pushed-down clauses */
		cost_qual_eval(&param_cost, param_info->ppi_clauses, root);
		host_cost.startup += param_cost.startup;
		host_cost.per_tuple += param_cost.per_tuple;
	}

	/* total path cost */
	startup_cost += dev_cost.startup + host_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + host_cost.per_tuple;
	gpu_per_tuple = cpu_tuple_cost / 100 + dev_cost.per_tuple;
	run_cost += (gpu_per_tuple * baserel->tuples +
				 cpu_per_tuple * dev_sel * baserel->tuples);

    gpu_path->cpath.path.startup_cost = startup_cost;
    gpu_path->cpath.path.total_cost = startup_cost + run_cost;
}

static void
gpuscan_add_scan_path(PlannerInfo *root,
					  RelOptInfo *baserel,
					  RangeTblEntry *rte)
{
	GpuScanPath	   *pathnode;
	List		   *dev_quals = NIL;
	List		   *host_quals = NIL;
	Bitmapset	   *dev_attnums = NULL;
	Bitmapset	   *host_attnums = NULL;
	bool			syscol_referenced = false;
	ListCell	   *cell;
	codegen_context	context;

	/* call the secondary hook */
	if (add_scan_path_next)
		add_scan_path_next(root, baserel, rte);

	/* nothing to do, if either PG-Strom or GpuScan is not enabled */
	if (!pgstrom_enabled || !enable_gpuscan)
		return;

	/* only base relation we can handle */
	if (baserel->rtekind != RTE_RELATION || baserel->relid == 0)
		return;

	/* system catalog is not supported */
	if (get_rel_namespace(rte->relid) == PG_CATALOG_NAMESPACE)
		return;

	/* check whether the qualifier can run on GPU device, or not */
	memset(&context, 0, sizeof(codegen_context));
	foreach (cell, baserel->baserestrictinfo)
	{
		RestrictInfo   *rinfo = lfirst(cell);

		if (pgstrom_codegen_available_expression(rinfo->clause))
		{
			pull_varattnos((Node *)rinfo->clause,
						   baserel->relid,
						   &dev_attnums);
			dev_quals = lappend(dev_quals, rinfo);
		}
		else
		{
			pull_varattnos((Node *)rinfo->clause,
						   baserel->relid,
						   &host_attnums);
			host_quals = lappend(host_quals, rinfo);
		}
	}
	/* also, picks up Var nodes in the target list */
	pull_varattnos((Node *)baserel->reltargetlist,
				   baserel->relid,
				   &host_attnums);
	/*
	 * remove system column references from the host_attnums/dev_attnums
	 * bitmap, because it takes special case handling.
	 * In case of dev_attnums, it should never reference system columns
	 * because it does not have supported data type.
	 */
	Assert(!dev_attnums || (dev_attnums->words[0] & BITMASK_ALL_SYSCOLS) == 0);
	if (host_attnums && (host_attnums->words[0] &
						 BITMASK_ALL_SYSCOLS) != 0)
	{
		host_attnums->words[0] &= ~BITMASK_ALL_SYSCOLS;
		syscol_referenced = true;
	}

	/*
	 * FIXME: needs to pay attention for projection cost.
	 * It may make sense to use build_physical_tlist, if host_attnums
	 * are much wider than dev_attnums.
	 * Anyway, it needs investigation of the actual behavior.
	 */

	/* XXX - check whether columnar cache may be available */

	/*
	 * Construction of a custom-plan node.
	 */
	pathnode = palloc0(sizeof(GpuScanPath));
	pathnode->cpath.path.type = T_CustomPath;
	pathnode->cpath.path.pathtype = T_CustomPlan;
	pathnode->cpath.path.parent = baserel;
	pathnode->cpath.path.param_info
		= get_baserel_parampathinfo(root, baserel, baserel->lateral_relids);
	pathnode->cpath.path.pathkeys = NIL;	/* gpuscan has unsorted result */
	pathnode->cpath.methods = &gpuscan_path_methods;

	cost_gpuscan(pathnode, root, baserel,
				 pathnode->cpath.path.param_info,
				 dev_quals, host_quals);

	pathnode->dev_quals = dev_quals;
	pathnode->host_quals = host_quals;
	pathnode->dev_attnums = dev_attnums;
	pathnode->host_attnums = host_attnums;
	pathnode->syscol_referenced = syscol_referenced;

	add_path(baserel, &pathnode->cpath.path);
}

/*
 * OpenCL code generation that can run on GPU/MIC device
 */
static char *
gpuscan_codegen_quals(PlannerInfo *root, List *dev_quals,
					  codegen_context *context)
{
	StringInfoData	str;
	StringInfoData	decl;
	char		   *expr_code;

	memset(context, 0, sizeof(codegen_context));
	if (dev_quals == NIL)
		return NULL;

#if 0
	/*
	 * A dummy constant
	 * KPARAM_0 - a template of kern_data_store to be loaded.
	 * KPARAM_1 - a template of kern_toastbuf to be loaded.
	 * Just a placeholder here. Set it up later.
	 */
	context->used_params = list_make2(makeConst(BYTEAOID,
												-1,
												InvalidOid,
												-1,
												PointerGetDatum(NULL),
												true,
												false),
									  makeConst(BYTEAOID,
												-1,
												InvalidOid,
												-1,
												PointerGetDatum(NULL),
												true,
												false));
	context->type_defs = list_make1(pgstrom_devtype_lookup(BYTEAOID));
#endif
	/* OK, let's walk on the device expression tree */
	expr_code = pgstrom_codegen_expression((Node *)dev_quals, context);
	Assert(expr_code != NULL);

	initStringInfo(&decl);
	initStringInfo(&str);

	/*
	 * make declarations of var and param references
	 */
	appendStringInfo(&str, "%s\n", pgstrom_codegen_type_declarations(context));
	appendStringInfo(&str, "%s\n", pgstrom_codegen_func_declarations(context));
	appendStringInfo(&decl, "%s%s\n",
					 pgstrom_codegen_param_declarations(context,
														context->param_refs),
					 pgstrom_codegen_var_declarations(context));

	/* qualifier definition with row-store */
	appendStringInfo(
		&str,
		"static pg_bool_t\n"
		"gpuscan_qual_eval(__private cl_int *errcode,\n"
		"                  __global kern_parambuf *kparams,\n"
		"                  __global kern_data_store *kds,\n"
		"                  __global kern_toastbuf *ktoast,\n"
		"                  size_t kds_index)\n"
		"{\n"
		"%s"
		"  if (get_global_id(0) == 0)\n"
		"  {\n"
		"    printf(\"%%d %%f\\n\", KPARAM_0.isnull, KPARAM_0.value);\n"
		"    printf(\"%%d %%f\\n\", KPARAM_1.isnull, KPARAM_1.value);\n"
		"    printf(\"%%d %%f\\n\", KPARAM_2.isnull, KPARAM_2.value);\n"
		"    printf(\"%%d %%f\\n\", KPARAM_3.isnull, KPARAM_3.value);\n"
		"  }\n"
		"  return %s;\n"
		"}\n", decl.data, expr_code);
	return str.data;
}

static CustomPlan *
gpuscan_create_plan(PlannerInfo *root, CustomPath *best_path)
{
	RelOptInfo	   *rel = best_path->path.parent;
	GpuScanPath	   *gpath = (GpuScanPath *)best_path;
	GpuScanPlan	   *gscan;
	List		   *tlist;
	List		   *host_clauses;
	List		   *dev_clauses;
	char		   *kern_source;
	codegen_context	context;

	/*
	 * See the comments in create_scan_plan(). We may be able to omit
	 * projection of the table tuples, if possible.
	 */
	if (use_physical_tlist(root, rel))
	{
		tlist = build_physical_tlist(root, rel);
		if (tlist == NIL)
			tlist = build_path_tlist(root, &best_path->path);
	}
	else
		tlist = build_path_tlist(root, &best_path->path);

	/* it should be a base relation */
	Assert(rel->relid > 0);
	Assert(rel->rtekind == RTE_RELATION);

	/* Sort clauses into best execution order */
	host_clauses = order_qual_clauses(root, gpath->host_quals);
	dev_clauses = order_qual_clauses(root, gpath->dev_quals);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	host_clauses = extract_actual_clauses(host_clauses, false);
	dev_clauses = extract_actual_clauses(dev_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->path.param_info)
	{
		host_clauses = (List *)
			replace_nestloop_params(root, (Node *) host_clauses);
		dev_clauses = (List *)
			replace_nestloop_params(root, (Node *) dev_clauses);
	}

	/*
	 * Construct OpenCL kernel code - A kernel code contains two forms of
	 * entrypoints; for row-store and column-store. OpenCL intermediator
	 * invoked proper kernel function according to the class of data store.
	 * Once a kernel function for row-store is called, it translates the
	 * data format into column-store, then kicks jobs for row-evaluation.
	 * This design is optimized to process column-oriented data format on
	 * the relation cache.
	 */
	kern_source = gpuscan_codegen_quals(root, dev_clauses, &context);

	/*
	 * Construction of GpuScanPlan node; on top of CustomPlan node
	 */
	gscan = palloc0(sizeof(GpuScanPlan));
	gscan->cplan.plan.type = T_CustomPlan;
	gscan->cplan.plan.targetlist = tlist;
	gscan->cplan.plan.qual = host_clauses;
	gscan->cplan.plan.lefttree = NULL;
	gscan->cplan.plan.righttree = NULL;
	gscan->cplan.methods = &gpuscan_plan_methods;

	gscan->scanrelid = rel->relid;
	gscan->kern_source = kern_source;
	gscan->extra_flags = context.extra_flags | DEVKERNEL_NEEDS_GPUSCAN;
	gscan->used_params = context.used_params;
	gscan->used_vars = context.used_vars;
	gscan->dev_clauses = dev_clauses;
	gscan->dev_attnums = gpath->dev_attnums;
	gscan->host_attnums = gpath->host_attnums;
	gscan->syscol_referenced = gpath->syscol_referenced;

	return &gscan->cplan;
}

static void
gpuscan_textout_path(StringInfo str, Node *node)
{
	GpuScanPath	   *pathnode = (GpuScanPath *) node;
	char		   *temp;

	/* dev_quals */
	temp = nodeToString(pathnode->dev_quals);
	appendStringInfo(str, " :dev_quals %s", temp);
	pfree(temp);

	/* host_quals */
	temp = nodeToString(pathnode->host_quals);
	appendStringInfo(str, " :host_quals %s", temp);
	pfree(temp);

	/* dev_attnums */
	appendStringInfo(str, " :dev_attnums ");
	_outBitmapset(str, pathnode->dev_attnums);

	/* host_attnums */
	appendStringInfo(str, " :host_attnums ");
	_outBitmapset(str, pathnode->host_attnums);

	/* syscol_referenced */
	appendStringInfo(str, " :syscol_referenced %s",
					 pathnode->syscol_referenced ? "true" : "false");
}

static void
gpuscan_set_plan_ref(PlannerInfo *root,
					 CustomPlan *custom_plan,
					 int rtoffset)
{
	GpuScanPlan	   *gscan = (GpuScanPlan *)custom_plan;

	gscan->scanrelid += rtoffset;
	gscan->cplan.plan.targetlist = (List *)
		fix_scan_expr(root, (Node *)gscan->cplan.plan.targetlist, rtoffset);
	gscan->cplan.plan.qual = (List *)
		fix_scan_expr(root, (Node *)gscan->cplan.plan.qual, rtoffset);
	gscan->used_vars = (List *)
		fix_scan_expr(root, (Node *)gscan->used_vars, rtoffset);
	gscan->dev_clauses = (List *)
		fix_scan_expr(root, (Node *)gscan->dev_clauses, rtoffset);
}

static void
gpuscan_finalize_plan(PlannerInfo *root,
					  CustomPlan *custom_plan,
					  Bitmapset **paramids,
					  Bitmapset **valid_params,
					  Bitmapset **scan_params)
{
	*paramids = bms_add_members(*paramids, *scan_params);
}

/*
 * gpuscan_support_multi_exec
 *
 * It gives a hint whether the supplied plan-state support bulk-exec mode,
 * or not. If it is GpuScan provided by PG-Strom, it allows bulk-exec in
 * case when ...
 * - no scan projection
 * - no hybrid-scan mode (cache-only scan, or regular heap-scan)
 */
bool
gpuscan_support_multi_exec(const CustomPlanState *cps)
{
	if (cps->ps.ps_ProjInfo != NULL)
		return false;
	return true;
}

static  CustomPlanState *
gpuscan_begin(CustomPlan *node, EState *estate, int eflags)
{
	GpuScanPlan	   *gsplan = (GpuScanPlan *) node;
	Index			scanrelid = gsplan->scanrelid;
	GpuScanState   *gss;
	TupleDesc		tupdesc;
	List		   *tlist;
	//List		   *used_params = NIL;
	Form_pg_class	rel_form;

	/* gpuscan should not have inner/outer plan now */
	Assert(outerPlan(node) == NULL);
    Assert(innerPlan(node) == NULL);

	/*
	 * create a state structure
	 */
	gss = palloc0(sizeof(GpuScanState));
	gss->cps.ps.type = T_CustomPlanState;
	gss->cps.ps.plan = (Plan *) node;
	gss->cps.ps.state = estate;
	gss->cps.methods = &gpuscan_plan_methods;

	/*
	 * create expression context
	 */
	ExecAssignExprContext(estate, &gss->cps.ps);

	/*
	 * initialize child expressions
	 */
	gss->cps.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist, &gss->cps.ps);
	gss->cps.ps.qual = (List *)
		ExecInitExpr((Expr *) node->plan.qual, &gss->cps.ps);
	gss->dev_quals = (List *)
		ExecInitExpr((Expr *) gsplan->dev_clauses, &gss->cps.ps);

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &gss->cps.ps);
	gss->scan_slot = ExecAllocTableSlot(&estate->es_tupleTable);

	/*
	 * initialize scan relation
	 */
	gss->scan_rel = ExecOpenScanRelation(estate, scanrelid, eflags);
	tupdesc = RelationGetDescr(gss->scan_rel);
	ExecSetSlotDescriptor(gss->scan_slot, tupdesc);
	gss->scan_tuple.t_tableOid = RelationGetRelid(gss->scan_rel);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&gss->cps.ps);
	if (tlist_matches_tupdesc(&gss->cps.ps,
							  node->plan.targetlist,
							  scanrelid,
							  tupdesc))
		gss->cps.ps.ps_ProjInfo = NULL;
	else
		ExecAssignProjectionInfo(&gss->cps.ps, tupdesc);

	/* also, check availability of simple projection */
	tlist = gss->cps.ps.plan->targetlist;
	gss->bulk_attmap = pgstrom_make_bulk_attmap(tlist, scanrelid);
	gss->syscol_referenced = gsplan->syscol_referenced;

	/*
	 * OK, let's initialize stuff for block scan
	 */
	gss->curr_blknum = 0;
	gss->last_blknum = RelationGetNumberOfBlocks(gss->scan_rel);
	rel_form = RelationGetForm(gss->scan_rel);
	gss->ntup_per_page = rel_form->reltuples / (float)rel_form->relpages;

	/*
	 * Setting up kernel program, if needed
	 */
	if (gsplan->kern_source)
	{
		gss->dprog_key = pgstrom_get_devprog_key(gsplan->kern_source,
												 gsplan->extra_flags);
		pgstrom_track_object((StromObject *)gss->dprog_key, 0);

		/* also, message queue */
		gss->mqueue = pgstrom_create_queue();
		pgstrom_track_object(&gss->mqueue->sobj, 0);
	}
#if 0
	/*
	 * construction of kds_head and ktoast_head template, if it can be
	 * executed in the kernel space.
	 */
	if (gsplan->kern_source)
	{
		Const	   *kparam_0;
		Const	   *kparam_1;
		bytea	   *kds_head;
		bytea	   *ktoast_head;

		used_params = copyObject(gsplan->used_params);
		Assert(list_length(used_params) >= 2);
		kparam_0 = (Const *)linitial(used_params);
		Assert(IsA(kparam_0, Const) &&
			   kparam_0->consttype == BYTEAOID &&
			   kparam_0->constisnull);
		kds_head = kparam_make_kds_head(tupdesc, gsplan->dev_attnums, 0);
		kparam_0->constvalue = PointerGetDatum(kds_head);
		kparam_0->constisnull = false;

		kparam_1 = (Const *)lsecond(used_params);
		Assert(IsA(kparam_1, Const) &&
			   kparam_1->consttype == BYTEAOID &&
			   kparam_1->constisnull);
		ktoast_head = kparam_make_ktoast_head(tupdesc, 0);
		kparam_1->constvalue = PointerGetDatum(ktoast_head);
		kparam_1->constisnull = false;
	}
#endif
	gss->kparams = pgstrom_create_kern_parambuf(gsplan->used_params,
												gss->cps.ps.ps_ExprContext);
	/* rest of run-time parameters */
	gss->curr_chunk = NULL;
	gss->curr_index = 0;
	gss->num_running = 0;
	dlist_init(&gss->ready_chunks);

	/* Is perfmon needed? */
	gss->pfm.enabled = pgstrom_perfmon_enabled;

	return &gss->cps;
}

static pgstrom_gpuscan *
pgstrom_create_gpuscan(GpuScanState *gss, pgstrom_data_store *pds)
{
	pgstrom_gpuscan *gpuscan;
	kern_data_store	*kds = pds->kds;
	kern_resultbuf	*kresults;
	cl_uint		nitems = kds->nitems;
	Size		length;

	length = (STROMALIGN(offsetof(pgstrom_gpuscan, kern.kparams)) +
			  STROMALIGN(gss->kparams->length) +
			  STROMALIGN(offsetof(kern_resultbuf, results[nitems])));
	gpuscan = pgstrom_shmem_alloc(length);
	if (!gpuscan)
		elog(ERROR, "out of shared memory");

	/* fields of pgstrom_gpuscan */
	pgstrom_init_message(&gpuscan->msg,
						 StromTag_GpuScan,
						 gss->dprog_key != 0 ? gss->mqueue : NULL,
						 clserv_process_gpuscan,
						 clserv_put_gpuscan,
						 gss->pfm.enabled);
	if (gss->dprog_key != 0)
		gpuscan->dprog_key = pgstrom_retain_devprog_key(gss->dprog_key);
	else
		gpuscan->dprog_key = 0;
	gpuscan->pds = pds;

	/* copy kern_parambuf */
	Assert(gss->kparams->length == STROMALIGN(gss->kparams->length));
	memcpy(KERN_GPUSCAN_PARAMBUF(&gpuscan->kern),
		   gss->kparams,
		   gss->kparams->length);

	/* setting up resultbuf */
	kresults = KERN_GPUSCAN_RESULTBUF(&gpuscan->kern);
	memset(kresults, 0, sizeof(kern_resultbuf));
	kresults->nrels = 1;
	kresults->nrooms = nitems;

	/* If GpuScan does not involve kernel execution, we treat this
	 * chunk as all-visible. */
	if (gss->dprog_key == 0)
	{
		kresults->all_visible = true;
		kresults->nitems = nitems;
	}
	return gpuscan;
}

static pgstrom_gpuscan *
pgstrom_load_gpuscan(GpuScanState *gss)
{
	pgstrom_gpuscan	   *gpuscan = NULL;
	Relation			rel = gss->scan_rel;
	TupleDesc			tupdesc = RelationGetDescr(rel);
	Snapshot			snapshot = gss->cps.ps.state->es_snapshot;
	Size				length;
	pgstrom_data_store *pds;
	struct timeval tv1, tv2;

	/* no more blocks to read */
	if (gss->curr_blknum > gss->last_blknum)
		return NULL;

	if (gss->pfm.enabled)
		gettimeofday(&tv1, NULL);

	length = (pgstrom_chunk_size << 20);
	pds = pgstrom_create_data_store_row(tupdesc, length, gss->ntup_per_page);
	PG_TRY();
	{
		while (gss->curr_blknum < gss->last_blknum &&
			   pgstrom_data_store_insert_block(pds, rel,
											   gss->curr_blknum,
											   snapshot, true))
			gss->curr_blknum++;

		if (pds->kds->nitems > 0)
			gpuscan = pgstrom_create_gpuscan(gss, pds);
	}
	PG_CATCH();
    {
		pgstrom_put_data_store(pds);
        PG_RE_THROW();
    }
    PG_END_TRY();
	/* track local object */
	if (gpuscan)
		pgstrom_track_object(&gpuscan->msg.sobj, 0);
	/* update perfmon statistics */
	if (gss->pfm.enabled)
	{
		gettimeofday(&tv2, NULL);
		gss->pfm.time_outer_load += timeval_diff(&tv1, &tv2);
	}
	return gpuscan;
}

static TupleTableSlot *
gpuscan_next_tuple(GpuScanState *gss)
{
	pgstrom_gpuscan	   *gpuscan = gss->curr_chunk;
	kern_resultbuf	   *kresults = KERN_GPUSCAN_RESULTBUF(&gpuscan->kern);
	TupleTableSlot	   *slot = NULL;
	cl_int				i_result;
	bool				do_recheck = false;
	struct timeval		tv1, tv2;

	if (gss->curr_index == 0)
	{
		int i;

		elog(INFO, "kresults {nrels=%u nrooms=%u nitems=%u errcode=%d has_recheks=%d}", kresults->nrels, kresults->nrooms, kresults->nitems, kresults->errcode, kresults->has_rechecks);
		for (i=0; i < kresults->nitems; i++)
			elog(INFO, "kresults[%d] = %d", i, kresults->results[i]);
		
	}

	if (!gpuscan)
		return false;

	if (gss->pfm.enabled)
		gettimeofday(&tv1, NULL);

   	while (gss->curr_index < kresults->nitems)
	{
		pgstrom_data_store *pds = gpuscan->pds;

		if (kresults->all_visible)
			i_result = ++gss->curr_index;
		else
		{
			i_result = kresults->results[gss->curr_index++];
			if (i_result < 0)
			{
				i_result = -i_result;
				do_recheck = true;
			}
		}
		Assert(i_result > 0 && i_result <= kresults->nitems);

		if (!pgstrom_fetch_data_store(gss->scan_slot,
									  pds, i_result - 1,
									  &gss->scan_tuple))
			elog(ERROR, "failed to fetch a record from pds: %d", i_result);

		if (do_recheck)
		{
			ExprContext *econtext = gss->cps.ps.ps_ExprContext;

			Assert(gss->dev_quals != NULL);
			econtext->ecxt_scantuple = gss->scan_slot;
			if (!ExecQual(gss->dev_quals, econtext, false))
				continue;
		}
		slot = gss->scan_slot;
		break;
	}
	if (gss->pfm.enabled)
	{
		gettimeofday(&tv2, NULL);
		gss->pfm.time_materialize += timeval_diff(&tv1, &tv2);
	}
	return slot;
}

static TupleTableSlot *
gpuscan_fetch_tuple(CustomPlanState *node)
{
	GpuScanState   *gss = (GpuScanState *) node;
	TupleTableSlot *slot = gss->scan_slot;

	ExecClearTuple(slot);

	while (!gss->curr_chunk || !(slot = gpuscan_next_tuple(gss)))
	{
		pgstrom_message	   *msg;
		pgstrom_gpuscan	   *gpuscan;

		/*
		 * Release the current gpuscan chunk being already scanned
		 */
		if (gss->curr_chunk)
		{
			pgstrom_message	   *msg = &gss->curr_chunk->msg;

			if (msg->pfm.enabled)
				pgstrom_perfmon_add(&gss->pfm, &msg->pfm);
			Assert(msg->refcnt == 1);
			pgstrom_untrack_object(&msg->sobj);
			pgstrom_put_message(msg);
			gss->curr_chunk = NULL;
			gss->curr_index = 0;
		}

		/*
		 * In case when no device code will be executed, we have no
		 * message queue for asynchronous execution because of senseless.
		 * We handle the job synchronously, but may make sense if tcache
		 * reduces disk accesses.
		 */
		if (gss->dprog_key == 0)
		{
			gss->curr_chunk = pgstrom_load_gpuscan(gss);
			gss->curr_index = 0;
			if (gss->curr_chunk)
				continue;
			break;
		}
		Assert(gss->mqueue != NULL);

		/*
		 * Dequeue the current gpuscan chunks being already processed
		 */
		while ((msg = pgstrom_try_dequeue_message(gss->mqueue)) != NULL)
		{
			Assert(gss->num_running > 0);
			gss->num_running--;
			dlist_push_tail(&gss->ready_chunks, &msg->chain);
		}

		/*
		 * Try to keep number of gpuscan chunks being asynchronously executed
		 * larger than minimum multiplicity, unless it does not exceed
		 * maximum one and OpenCL server does not return a new response.
		 */
		while (gss->num_running <= pgstrom_max_async_chunks)
		{
			pgstrom_gpuscan	*gpuscan = pgstrom_load_gpuscan(gss);

			if (!gpuscan)
				break;	/* scan reached end of the relation */

			if (!pgstrom_enqueue_message(&gpuscan->msg))
			{
				pgstrom_put_message(&gpuscan->msg);
				elog(ERROR, "failed to enqueue pgstrom_gpuscan message");
			}
			gss->num_running++;

			if (gss->num_running > pgstrom_min_async_chunks &&
				(msg = pgstrom_try_dequeue_message(gss->mqueue)) != NULL)
			{
				gss->num_running--;
				dlist_push_tail(&gss->ready_chunks, &msg->chain);
				break;
			}
		}

		/*
		 * Wait for server's response if no available chunks were replied.
		 */
		if (dlist_is_empty(&gss->ready_chunks))
		{
			/* OK, no more chunks to be scanned */
			if (gss->num_running == 0)
				break;

			msg = pgstrom_dequeue_message(gss->mqueue);
			if (!msg)
				elog(ERROR, "message queue wait timeout");
			gss->num_running--;
			dlist_push_tail(&gss->ready_chunks, &msg->chain);
		}

		/*
		 * Picks up next available chunks if any
		 */
		Assert(!dlist_is_empty(&gss->ready_chunks));
		gpuscan = dlist_container(pgstrom_gpuscan, msg.chain,
								  dlist_pop_head_node(&gss->ready_chunks));
		Assert(StromTagIs(gpuscan, GpuScan));

		/*
		 * Raise an error, if an error was reported.
		 *
		 * XXX: Row-level recheck really makes sense?
		 */
		if (gpuscan->msg.errcode != StromError_Success)
		{
			if (gpuscan->msg.errcode == CL_BUILD_PROGRAM_FAILURE)
			{
				const char *buildlog
					= pgstrom_get_devprog_errmsg(gpuscan->dprog_key);
				const char *kern_source
					= ((GpuScanPlan *)gss->cps.ps.plan)->kern_source;

				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("PG-Strom: OpenCL execution error (%s)\n%s",
								pgstrom_strerror(gpuscan->msg.errcode),
								kern_source),
						 errdetail("%s", buildlog)));
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("PG-Strom: OpenCL execution error (%s)",
								pgstrom_strerror(gpuscan->msg.errcode))));
			}
		}
		elog(INFO, "curr_chunk = %p", gpuscan);
		gss->curr_chunk = gpuscan;
		gss->curr_index = 0;
	}
	return slot;
}

static TupleTableSlot *
gpuscan_exec(CustomPlanState *node)
{
	/* overall logic were copied from ExecScan */
	ExprContext	   *econtext = node->ps.ps_ExprContext;
	List		   *qual = node->ps.qual;
	ProjectionInfo *projInfo = node->ps.ps_ProjInfo;
	ExprDoneCond	isDone;
	TupleTableSlot *resultSlot;

	/*
	 * If we have neither a qual to check nor a projection to do, just skip
	 * all the overhead and return the raw scan tuple.
	 */
	if (!qual && !projInfo)
	{
		ResetExprContext(econtext);
		return gpuscan_fetch_tuple(node);
	}

	/*
	 * Check to see if we're still projecting out tuples from a previous scan
	 * tuple (because there is a function-returning-set in the projection
	 * expressions).  If so, try to project another one.
	 */
	if (node->ps.ps_TupFromTlist)
	{
		Assert(projInfo);		/* can't get here if not projecting */
		resultSlot = ExecProject(projInfo, &isDone);
		if (isDone == ExprMultipleResult)
			return resultSlot;
		/* Done with that source tuple... */
		node->ps.ps_TupFromTlist = false;
	}

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.  Note this can't happen
	 * until we're done projecting out tuples from a scan tuple.
	 */
	ResetExprContext(econtext);

	/*
	 * get a tuple from the access method.  Loop until we obtain a tuple that
	 * passes the qualification.
	 */
	for (;;)
	{
		TupleTableSlot *slot;

		CHECK_FOR_INTERRUPTS();

		slot = gpuscan_fetch_tuple(node);

		/*
		 * if the slot returned by the accessMtd contains NULL, then it means
		 * there is nothing more to scan so we just return an empty slot,
		 * being careful to use the projection result slot so it has correct
		 * tupleDesc.
		 */
		if (TupIsNull(slot))
		{
			if (projInfo)
				return ExecClearTuple(projInfo->pi_slot);
			else
				return slot;
		}

		/*
		 * place the current tuple into the expr context
		 */
		econtext->ecxt_scantuple = slot;

		/*
		 * check that the current tuple satisfies the qual-clause
		 *
		 * check for non-nil qual here to avoid a function call to ExecQual()
		 * when the qual is nil ... saves only a few cycles, but they add up
		 * ...
		 */
		if (!qual || ExecQual(qual, econtext, false))
		{
			/*
			 * Found a satisfactory scan tuple.
			 */
			if (projInfo)
			{
				/*
				 * Form a projection tuple, store it in the result tuple slot
				 * and return it --- unless we find we can project no tuples
				 * from this scan tuple, in which case continue scan.
				 */
				resultSlot = ExecProject(projInfo, &isDone);
				if (isDone != ExprEndResult)
				{
					node->ps.ps_TupFromTlist = (isDone == ExprMultipleResult);
					return resultSlot;
				}
			}
			else
			{
				/*
				 * Here, we aren't projecting, so just return scan tuple.
				 */
				return slot;
			}
		}
		else
			InstrCountFiltered1(node, 1);

		/*
		 * Tuple fails qual, so free per-tuple memory and try again.
		 */
		ResetExprContext(econtext);
	}
}

#if 0

static Node *
gpuscan_exec_multi(CustomPlanState *node)
{
	GpuScanState	   *gss = (GpuScanState *) node;
	Snapshot			snapshot = node->ps.state->es_snapshot;
	List			   *host_qual = node->ps.qual;
	pgstrom_gpuscan	   *gpuscan;
	kern_resultbuf	   *kresults;
	pgstrom_bulkslot   *bulk;
	pgstrom_message	   *msg;
	cl_uint				i, j, nitems;
	cl_int			   *rindex;

	/* must provide our own instrumentation support */
	if (node->ps.instrument)
		InstrStartNode(node->ps.instrument);

	/* bulk data exchange should not have projection */
	Assert(gss->bulk_attmap != NULL);
	/* also, hybrid scan is nonsense */
	Assert(!gss->hybrid_scan);

retry:
	while (true)
	{
		gpuscan = pgstrom_load_gpuscan(gss);
		if (!gpuscan)
			break;

		/*
		 * In case we have no device executable stuff, here is no chance
		 * to run something asynchronous, so we simply returns this row-
		 * or column-store as-is.
		 */
		if (gss->dprog_key == 0)
			break;

		/*
		 * To keep number of asynchronous row/column stores to be processed,
		 *  we try to enqueue at least one stores per fetch.
		 */
		if (!pgstrom_enqueue_message(&gpuscan->msg))
		{
			pgstrom_put_message(&gpuscan->msg);
			elog(ERROR, "failed to enqueue pgstrom_gpuscan message");
		}
		gss->num_running++;
		gpuscan = NULL;

		/* check if any chunk being already processed */ 
		msg = pgstrom_try_dequeue_message(gss->mqueue);
		if (!msg)
		{
			if (gss->num_running < pgstrom_max_async_chunks)
				continue;
			else
				break;
		}
		gss->num_running--;
		Assert(StromTagIs(msg, GpuScan));
		gpuscan = (pgstrom_gpuscan *) msg;
		break;
	}

	/*
	 * Try synchronous dequeue, if nothing returned yet.
	 */
	if (!gpuscan)
	{
		/* if no asynchronous jobs in progress, it's end of scan */
		if (gss->num_running == 0)
		{
			/* must provide our own instrumentation support */
			if (node->ps.instrument)
				InstrStopNode(node->ps.instrument, 0.0);
			return NULL;
		}
		msg = pgstrom_dequeue_message(gss->mqueue);
		if (!msg)
			elog(ERROR, "message queue wait timeout");
		gss->num_running--;
		Assert(StromTagIs(msg, GpuScan));
		gpuscan = (pgstrom_gpuscan *) msg;
	}

	/*
	 * Raise an error, if chunk-level error was reported.
	 */
	if (gpuscan->msg.errcode != StromError_Success)
	{
		if (gpuscan->msg.errcode == CL_BUILD_PROGRAM_FAILURE)
		{
			const char *buildlog
				= pgstrom_get_devprog_errmsg(gpuscan->dprog_key);
			const char *kern_source
				= ((GpuScanPlan *)gss->cps.ps.plan)->kern_source;

			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("PG-Strom: OpenCL execution error (%s)\n%s",
							pgstrom_strerror(gpuscan->msg.errcode),
							kern_source),
					 errdetail("%s", buildlog)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("PG-Strom: OpenCL execution error (%s)",
							pgstrom_strerror(gpuscan->msg.errcode))));
		}
	}
	kresults = KERN_GPUSCAN_RESULTBUF(&gpuscan->kern);

	/*
	 * Make a new bulk-slot according to the result
	 */
	bulk = palloc0(offsetof(pgstrom_bulkslot, rindex[kresults->nitems]));
	bulk->rcstore = pgstrom_get_rcstore(gpuscan->rc_store);
	bulk->nvalids = 0;	/* to be set later */
	pgstrom_track_object(bulk->rcstore, 0);

	/*
	 * GpuScan needs to have the host side checks below:
	 * - MVCC visibility checks.
	 * - Rechecks of device qualifier, if result is negative
	 * - Host side qualifier checks, if any.
	 */
	if (kresults->all_visible)
	{
		nitems = pgstrom_nitems_rcstore(gpuscan->rc_store);
		rindex = NULL;
	}
	else
	{
		nitems = kresults->nitems;
		rindex = kresults->results;
	}
	Assert(nitems <= kresults->nrooms);

	for (i=0, j=0; i < nitems; i++)
	{
		cl_uint			row_index = (!rindex ? i + 1 : rindex[i]);
		bool			do_recheck;
		TupleTableSlot *slot;

		if (row_index < 0)
		{
			row_index = -row_index - 1;
			do_recheck = true;
		}
		else
		{
			row_index--;
			do_recheck = false;
		}

		if (host_qual || do_recheck)
			slot = gss->scan_slot;
		else
			slot = NULL;

		/*
		 * MVCC visibility checks
		 */
		if (!gpuscan_check_tuple_visibility(gss, bulk->rcstore, row_index,
											snapshot, slot))
			continue;

		/*
		 * Device qualifier rechecks, if needed
		 */
		if (do_recheck)
		{
			ExprContext *econtext = gss->cps.ps.ps_ExprContext;

			Assert(gss->dev_quals != NULL);
			econtext->ecxt_scantuple = gss->scan_slot;
			if (!ExecQual(gss->dev_quals, econtext, false))
				continue;
		}

		/*
		 * Optional, host qualifier checks
		 */
		if (host_qual)
		{
			ExprContext *econtext = gss->cps.ps.ps_ExprContext;

			econtext->ecxt_scantuple = gss->scan_slot;
			if (!ExecQual(host_qual, econtext, false))
				continue;
		}
		bulk->rindex[j++] = row_index;
	}
	bulk->nvalids = j;
	/* no longer gpuscan is referenced any more, linked rcstore is not
	 * actually released because its refcnt is incremented above. */
	pgstrom_untrack_object(&gpuscan->msg.sobj);
	pgstrom_put_message(&gpuscan->msg);

	if (bulk->nvalids == 0)
	{
		pgstrom_put_rcstore(bulk->rcstore);
		pfree(bulk);
		goto retry;
	}
	/* must provide our own instrumentation support */
	if (node->ps.instrument)
		InstrStopNode(node->ps.instrument, (double) bulk->nvalids);
	return (Node *) bulk;
}
#endif

static void
gpuscan_end(CustomPlanState *node)
{
	GpuScanState	   *gss = (GpuScanState *)node;
	pgstrom_gpuscan	   *gpuscan;

	if (gss->curr_chunk)
	{
		gpuscan = gss->curr_chunk;

		pgstrom_untrack_object(&gpuscan->msg.sobj);
		pgstrom_put_message(&gpuscan->msg);
		gss->curr_chunk = NULL;
	}

	while (gss->num_running > 0)
	{
		gpuscan = (pgstrom_gpuscan *)pgstrom_dequeue_message(gss->mqueue);
		if (!gpuscan)
			elog(ERROR, "message queue wait timeout");
		pgstrom_untrack_object(&gpuscan->msg.sobj);
		pgstrom_put_message(&gpuscan->msg);
		gss->num_running--;
	}

	if (gss->dprog_key)
	{
		pgstrom_untrack_object((StromObject *)gss->dprog_key);
		pgstrom_put_devprog_key(gss->dprog_key);
	}

	if (gss->mqueue)
	{
		pgstrom_untrack_object(&gss->mqueue->sobj);
		pgstrom_close_queue(gss->mqueue);
	}

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&gss->cps.ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(gss->cps.ps.ps_ResultTupleSlot);
	ExecClearTuple(gss->scan_slot);

	/*
	 * close heap scan and relation
	 */
	heap_close(gss->scan_rel, NoLock);
}

static void
gpuscan_rescan(CustomPlanState *node)
{
	GpuScanState	   *gss = (GpuScanState *) node;
	pgstrom_message	   *msg;
	pgstrom_gpuscan	   *gpuscan;
	dlist_mutable_iter	iter;

	/*
	 * If asynchronous requests are still running, we need to synchronize
	 * their completion first.
	 *
	 * TODO: if we track all the pgstrom_gpuscan objects being enqueued
	 * locally, all we need to do should be just decrements reference
	 * counter. It may be a future enhancement.
	 */
	gpuscan = gss->curr_chunk;
	if (gpuscan)
	{
		pgstrom_untrack_object(&gpuscan->msg.sobj);
		pgstrom_put_message(&gpuscan->msg);
	}

	while (gss->num_running > 0)
	{
		msg = pgstrom_dequeue_message(gss->mqueue);
		if (!msg)
			elog(ERROR, "message queue wait timeout");
		gss->num_running--;
		dlist_push_tail(&gss->ready_chunks, &msg->chain);
	}

	dlist_foreach_modify (iter, &gss->ready_chunks)
	{
		gpuscan = dlist_container(pgstrom_gpuscan, msg.chain, iter.cur);
		Assert(StromTagIs(gpuscan, GpuScan));

		dlist_delete(&gpuscan->msg.chain);
		if (gpuscan->msg.errcode != StromError_Success)
		{
			if (gpuscan->msg.errcode == CL_BUILD_PROGRAM_FAILURE)
			{
				const char *buildlog
					= pgstrom_get_devprog_errmsg(gpuscan->dprog_key);
				const char *kern_source
					= ((GpuScanPlan *)gss->cps.ps.plan)->kern_source;

				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("PG-Strom: OpenCL execution error (%s)\n%s",
								pgstrom_strerror(gpuscan->msg.errcode),
								kern_source),
						 errdetail("%s", buildlog)));
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("PG-Strom: OpenCL execution error (%s)",
								pgstrom_strerror(gpuscan->msg.errcode))));
            }
		}
		pgstrom_untrack_object(&gpuscan->msg.sobj);
		pgstrom_put_message(&gpuscan->msg);
	}

	/*
	 * OK, asynchronous jobs were cleared. revert scan state to the head.
	 */
	gss->curr_chunk = NULL;
	gss->curr_index = 0;
	gss->curr_blknum = 0;
}

static void
gpuscan_explain_rel(CustomPlanState *node, ExplainState *es)
{
	GpuScanState   *gss = (GpuScanState *)node;
	GpuScanPlan	   *gsplan = (GpuScanPlan *)gss->cps.ps.plan;
	Relation		rel = gss->scan_rel;
	RangeTblEntry  *rte;
	char		   *refname;
	char		   *objectname;
	char		   *namespace = NULL;

	rte = rt_fetch(gsplan->scanrelid, es->rtable);
	refname = (char *) list_nth(es->rtable_names,
								gsplan->scanrelid - 1);
	if (refname == NULL)
		refname = rte->eref->aliasname;

	Assert(rte->rtekind == RTE_RELATION);
	objectname = RelationGetRelationName(rel);
	if (es->verbose)
		namespace = get_namespace_name(RelationGetNamespace(rel));

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		appendStringInfoString(es->str, " on");
		if (namespace != NULL)
			appendStringInfo(es->str, " %s.%s",
							 quote_identifier(namespace),
							 quote_identifier(objectname));
		else
			appendStringInfo(es->str, " %s",
							 quote_identifier(objectname));
	}
	else
	{
		ExplainPropertyText("Relation Name", objectname, es);
		if (namespace != NULL)
			ExplainPropertyText("Schema", namespace, es);
		ExplainPropertyText("Alias", refname, es);
	}
}

static void
gpuscan_explain(CustomPlanState *node, List *ancestors, ExplainState *es)
{
	GpuScanState   *gss = (GpuScanState *) node;
	GpuScanPlan	   *gsplan = (GpuScanPlan *) gss->cps.ps.plan;
	Relation		rel = gss->scan_rel;
	Bitmapset	   *tempset;
	Oid				relid = RelationGetRelid(rel);
	AttrNumber		anum;
	bool			is_first;
	StringInfoData	str;

	initStringInfo(&str);
	tempset = bms_copy(gsplan->host_attnums);
	is_first = true;
	while ((anum = bms_first_member(tempset)) >= 0)
	{
		anum += FirstLowInvalidHeapAttributeNumber;

		appendStringInfo(&str, "%s%s",
						 is_first ? "" : ", ",
						 get_relid_attribute_name(relid, anum));
		is_first = false;
	}
	bms_free(tempset);
	ExplainPropertyText("Host References", str.data, es);

	resetStringInfo(&str);
	tempset = bms_copy(gsplan->dev_attnums);
	is_first = true;
	while ((anum = bms_first_member(tempset)) >= 0)
	{
		anum += FirstLowInvalidHeapAttributeNumber;

		appendStringInfo(&str, "%s%s",
						 is_first ? "" : ", ",
						 get_relid_attribute_name(relid, anum));
		is_first = false;
	}
	bms_free(tempset);
	ExplainPropertyText("Device References", str.data, es);

	if (gsplan->cplan.plan.qual != NIL)
	{
		show_scan_qual(gsplan->cplan.plan.qual,
					   "Filter", &gss->cps.ps, ancestors, es);
		show_instrumentation_count("Rows Removed by Filter",
								   1, &gss->cps.ps, es);
	}
	if (gsplan->dev_clauses != NIL)
	{
		show_scan_qual(gsplan->dev_clauses,
					   "Device Filter", &gss->cps.ps, ancestors, es);
		show_instrumentation_count("Rows Removed by Device Fileter",
								   2, &gss->cps.ps, es);
	}
	show_device_kernel(gss->dprog_key, es);

	if (es->analyze && gss->pfm.enabled)
		pgstrom_perfmon_explain(&gss->pfm, es);
}

static Bitmapset *
gpuscan_get_relids(CustomPlanState *node)
{
	GpuScanPlan	   *gsp = (GpuScanPlan *)node->ps.plan;

	return bms_make_singleton(gsp->scanrelid);
}

static void
gpuscan_textout_plan(StringInfo str, const CustomPlan *node)
{
	GpuScanPlan	   *plannode = (GpuScanPlan *)node;
	char		   *temp;

	appendStringInfo(str, " :scanrelid %u", plannode->scanrelid);

	appendStringInfo(str, " :kern_source ");
	_outToken(str, plannode->kern_source);

	appendStringInfo(str, " :extra_flags %u", plannode->scanrelid);

	temp = nodeToString(plannode->used_params);
	appendStringInfo(str, " :used_params %s", temp);
	pfree(temp);

	temp = nodeToString(plannode->used_vars);
	appendStringInfo(str, " :used_vars %s", temp);
	pfree(temp);

	temp = nodeToString(plannode->dev_clauses);
	appendStringInfo(str, " :dev_clauses %s", temp);
	pfree(temp);

	appendStringInfo(str, " :dev_attnums ");
	_outBitmapset(str, plannode->dev_attnums);

	appendStringInfo(str, " :host_attnums ");
	_outBitmapset(str, plannode->host_attnums);

	appendStringInfo(str, " :syscol_referenced %s",
					 plannode->syscol_referenced ? "true" : "false");
}

static CustomPlan *
gpuscan_copy_plan(const CustomPlan *from)
{
	GpuScanPlan	   *oldnode = (GpuScanPlan *)from;
	GpuScanPlan	   *newnode = palloc(sizeof(GpuScanPlan));

	CopyCustomPlanCommon((Node *)from, (Node *)newnode);
	newnode->scanrelid = oldnode->scanrelid;
	newnode->kern_source = (oldnode->kern_source
							? pstrdup(oldnode->kern_source)
							: NULL);
	newnode->extra_flags = oldnode->extra_flags;
	newnode->used_params = copyObject(oldnode->used_params);
	newnode->used_vars = copyObject(oldnode->used_vars);
	newnode->dev_clauses = copyObject(oldnode->dev_clauses);
	newnode->dev_attnums = bms_copy(oldnode->dev_attnums);
	newnode->host_attnums = bms_copy(oldnode->host_attnums);
	newnode->syscol_referenced = oldnode->syscol_referenced;

	return &newnode->cplan;
}

void
pgstrom_init_gpuscan(void)
{
	/* enable_gpuscan */
	DefineCustomBoolVariable("enable_gpuscan",
							 "Enables the use of GPU accelerated full-scan",
							 NULL,
							 &enable_gpuscan,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/* setup path methods */
	gpuscan_path_methods.CustomName			= "GpuScan";
	gpuscan_path_methods.CreateCustomPlan	= gpuscan_create_plan;
	gpuscan_path_methods.TextOutCustomPath	= gpuscan_textout_path;

	/* setup plan methods */
	gpuscan_plan_methods.CustomName			= "GpuScan";
	gpuscan_plan_methods.SetCustomPlanRef	= gpuscan_set_plan_ref;
	gpuscan_plan_methods.SupportBackwardScan= NULL;
	gpuscan_plan_methods.FinalizeCustomPlan	= gpuscan_finalize_plan;
	gpuscan_plan_methods.BeginCustomPlan	= gpuscan_begin;
	gpuscan_plan_methods.ExecCustomPlan		= gpuscan_exec;
	//gpuscan_plan_methods.MultiExecCustomPlan= gpuscan_exec_multi;
	gpuscan_plan_methods.EndCustomPlan		= gpuscan_end;
	gpuscan_plan_methods.ReScanCustomPlan	= gpuscan_rescan;
	gpuscan_plan_methods.ExplainCustomPlanTargetRel	= gpuscan_explain_rel;
	gpuscan_plan_methods.ExplainCustomPlan	= gpuscan_explain;
	gpuscan_plan_methods.GetRelidsCustomPlan= gpuscan_get_relids;
	gpuscan_plan_methods.GetSpecialCustomVar= NULL;
	gpuscan_plan_methods.TextOutCustomPlan	= gpuscan_textout_plan;
	gpuscan_plan_methods.CopyCustomPlan		= gpuscan_copy_plan;

	/* hook registration */
	add_scan_path_next = add_scan_path_hook;
	add_scan_path_hook = gpuscan_add_scan_path;
}

/*
 * GpuScan message handler
 * ------------------------------------------------------------
 * Note that below routines are executed in the context of OpenCL
 * intermediation server, thus, usual PostgreSQL internal APIs are
 * not available, and need to pay attention that routines work in
 * another process's address space.
 */
typedef struct
{
	pgstrom_message	*msg;
	cl_program		program;
	cl_kernel		kernel;
	cl_mem			m_gpuscan;
	cl_mem			m_dstore;
	cl_mem			m_ktoast;
	cl_uint			ev_index;
	cl_event		events[20];
} clstate_gpuscan;

static void
clserv_respond_gpuscan(cl_event event, cl_int ev_status, void *private)
{
	clstate_gpuscan	   *clgss = private;
	pgstrom_gpuscan	   *gpuscan = (pgstrom_gpuscan *)clgss->msg;
	kern_resultbuf	   *kresults = KERN_GPUSCAN_RESULTBUF(&gpuscan->kern);
	cl_int				status;
	cl_int				rc;

	/* put error code */
	if (ev_status != CL_COMPLETE)
	{
		clserv_log("unexpected CL_EVENT_COMMAND_EXECUTION_STATUS: %d",
				   ev_status);
		gpuscan->msg.errcode = StromError_OpenCLInternal;
	}
	else
	{
		gpuscan->msg.errcode = kresults->errcode;
	}

	/*
	 * collect performance statistics
	 */
	if (gpuscan->msg.pfm.enabled)
	{
		cl_ulong    dma_send_begin;
		cl_ulong    dma_send_end;
		cl_ulong    kern_exec_begin;
		cl_ulong    kern_exec_end;
		cl_ulong    dma_recv_begin;
		cl_ulong    dma_recv_end;
		cl_ulong	temp;
		cl_int		i, n;

		n = clgss->ev_index - 2;
		dma_send_begin = (cl_ulong)(~0);
		dma_send_end = (cl_ulong) 0;
		for (i=0; i < n; i++)
		{
			rc = clGetEventProfilingInfo(clgss->events[i],
										 CL_PROFILING_COMMAND_START,
										 sizeof(cl_ulong),
										 &temp,
										 NULL);
			if (rc != CL_SUCCESS)
				goto skip_perfmon;
			if (i==0 || dma_send_begin > temp)
				dma_send_begin = temp;

			rc = clGetEventProfilingInfo(clgss->events[i],
										 CL_PROFILING_COMMAND_END,
										 sizeof(cl_ulong),
										 &temp,
										 NULL);
			if (rc != CL_SUCCESS)
				goto skip_perfmon;
			if (i==0 || dma_send_end < temp)
				dma_send_end = temp;
		}

		rc = clGetEventProfilingInfo(clgss->events[clgss->ev_index - 2],
									 CL_PROFILING_COMMAND_START,
									 sizeof(cl_ulong),
									 &kern_exec_begin,
									 NULL);
		if (rc != CL_SUCCESS)
			goto skip_perfmon;

		rc = clGetEventProfilingInfo(clgss->events[clgss->ev_index - 2],
									 CL_PROFILING_COMMAND_END,
									 sizeof(cl_ulong),
									 &kern_exec_end,
									 NULL);
		if (rc != CL_SUCCESS)
			goto skip_perfmon;

		rc = clGetEventProfilingInfo(clgss->events[clgss->ev_index - 1],
									 CL_PROFILING_COMMAND_START,
									 sizeof(cl_ulong),
									 &dma_recv_begin,
									 NULL);
		if (rc != CL_SUCCESS)
			goto skip_perfmon;

		rc = clGetEventProfilingInfo(clgss->events[clgss->ev_index - 1],
									 CL_PROFILING_COMMAND_END,
									 sizeof(cl_ulong),
									 &dma_recv_end,
									 NULL);
		if (rc != CL_SUCCESS)
			goto skip_perfmon;

		gpuscan->msg.pfm.time_dma_send
			+= (dma_send_end - dma_send_begin) / 1000;
		gpuscan->msg.pfm.time_kern_exec
			+= (kern_exec_end - kern_exec_begin) / 1000;
		gpuscan->msg.pfm.time_dma_recv
			+= (dma_recv_end - dma_recv_begin) / 1000;
	skip_perfmon:
		if (rc != CL_SUCCESS)
		{
			clserv_log("failed on clGetEventProfilingInfo (%s)",
					   opencl_strerror(rc));
			gpuscan->msg.pfm.enabled = false;
		}
	}

	rc = clGetEventInfo(clgss->events[clgss->ev_index - 2],
						CL_EVENT_COMMAND_EXECUTION_STATUS,
						sizeof(cl_int),
						&status,
						NULL);
	Assert(rc == CL_SUCCESS);

	/* release opencl objects */
	while (clgss->ev_index > 0)
		clReleaseEvent(clgss->events[--clgss->ev_index]);
	if (clgss->m_ktoast)
		clReleaseMemObject(clgss->m_ktoast);
	clReleaseMemObject(clgss->m_dstore);
	clReleaseMemObject(clgss->m_gpuscan);
	clReleaseKernel(clgss->kernel);
	clReleaseProgram(clgss->program);
	free(clgss);

	/* respond to the backend side */
	pgstrom_reply_message(&gpuscan->msg);
}

/*
 * clserv_process_gpuscan
 *
 * entrypoint of kernel gpuscan implementation
 */
static void
clserv_process_gpuscan(pgstrom_message *msg)
{
	pgstrom_gpuscan	   *gpuscan = (pgstrom_gpuscan *) msg;
	pgstrom_perfmon	   *pfm = &gpuscan->msg.pfm;
	pgstrom_data_store *pds = gpuscan->pds;
	kern_data_store	   *kds = pds->kds;
	clstate_gpuscan	   *clgss = NULL;
	cl_program			program;
	cl_command_queue	kcmdq;
	kern_resultbuf	   *kresults = KERN_GPUSCAN_RESULTBUF(&gpuscan->kern);
	int					dindex;
	cl_int				rc;
	size_t				length;
	size_t				offset;
	size_t				gwork_sz;
	size_t				lwork_sz;

	/* sanity checks */
	Assert(StromTagIs(gpuscan, GpuScan));
	Assert(!kds->is_column);
	Assert(kresults->nrels == 1);
	if (kds->nitems == 0)
	{
		msg->errcode = StromError_BadRequestMessage;
		pgstrom_reply_message(msg);
		return;
	}

	/*
	 * First of all, it looks up a program object to be run on
	 * the supplied row-store. We may have three cases.
	 * 1) NULL; it means the required program is under asynchronous
	 *    build, and the message is kept on its internal structure
	 *    to be enqueued again. In this case, we have nothing to do
	 *    any more on the invocation.
	 * 2) BAD_OPENCL_PROGRAM; it means previous compile was failed
	 *    and unavailable to run this program anyway. So, we need
	 *    to reply StromError_ProgramCompile error to inform the
	 *    backend this program.
	 * 3) valid cl_program object; it is an ideal result. pre-compiled
	 *    program object was on the program cache, and cl_program
	 *    object is ready to use.
	 */
	program = clserv_lookup_device_program(gpuscan->dprog_key,
										   &gpuscan->msg);
	if (!program)
		return;		/* message is in waitq, retry it! */
	if (program == BAD_OPENCL_PROGRAM)
	{
		rc = CL_BUILD_PROGRAM_FAILURE;
		goto error;
	}

	/* debug output */
	pgstrom_dump_data_store(pds);

	/*
	 * create a state object
	 */
	clgss = calloc(1, offsetof(clstate_gpuscan, events[20 + kds->nblocks]));
	if (!clgss)
	{
		clReleaseProgram(program);
		rc = CL_OUT_OF_HOST_MEMORY;
		goto error;
	}
	clgss->msg = msg;
	clgss->program = program;

	/*
	 * lookup kernel function for gpuscan
	 */
	clgss->kernel = clCreateKernel(clgss->program, "gpuscan_qual", &rc);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clCreateBuffer: %s", opencl_strerror(rc));
		goto error;
	}

	/*
	 * choose a device to execute this kernel, and compute an optimal
	 * workgroup-size of this kernel
	 */
	dindex = pgstrom_opencl_device_schedule(&gpuscan->msg);
	kcmdq = opencl_cmdq[dindex];	
	if (!clserv_compute_workgroup_size(&gwork_sz, &lwork_sz,
									   clgss->kernel, dindex,
									   false,	/* smaller WG-sz is better */
									   kds->nitems, sizeof(cl_uint)))
		goto error;

	/* allocation of device memory for kern_gpuscan argument */
	clgss->m_gpuscan = clCreateBuffer(opencl_context,
									  CL_MEM_READ_WRITE,
									  KERN_GPUSCAN_LENGTH(&gpuscan->kern),
									  NULL,
									  &rc);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clCreateBuffer: %s", opencl_strerror(rc));
		goto error;
	}

	/* allocation of device memory for kern_data_store argument */
	clgss->m_dstore = clCreateBuffer(opencl_context,
									 CL_MEM_READ_WRITE,
									 KERN_DATA_STORE_LENGTH(kds),
									 NULL,
									 &rc);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clCreateBuffer: %s", opencl_strerror(rc));
		goto error;
	}

	/*
	 * allocation of device memory for kern_toastbuf, but never required
	 */
	clgss->m_ktoast = NULL;

	/*
	 * OK, all the device memory and kernel objects acquired.
	 * Let's prepare kernel invocation.
	 *
	 * The kernel call is:
	 * pg_bool_t
	 * gpuscan_qual_eval(__global kern_gpuscan *kgpuscan,
	 *                   __global kern_data_store *kds,
	 *                   __global kern_toastbuf *ktoast,
	 *                   __local void *local_workbuf)
	 */
	rc = clSetKernelArg(clgss->kernel,
						0,		/* kern_gpuscan */
						sizeof(cl_mem),
						&clgss->m_gpuscan);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	rc = clSetKernelArg(clgss->kernel,
						1,		/* kern_data_store */
						sizeof(cl_mem),
						&clgss->m_dstore);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	rc = clSetKernelArg(clgss->kernel,
						2,		/* kern_toastbuf; always NULL */
						sizeof(cl_mem),
						&clgss->m_ktoast);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	rc = clSetKernelArg(clgss->kernel,
						3,		/* local_workmem */
						sizeof(cl_uint) * lwork_sz,
						NULL);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	/*
     * OK, enqueue DMA transfer, kernel execution, then DMA writeback.
     *
	 * (1) gpuscan (incl. kparams) - DMA send
	 * (2) kds_head - DMA send
	 * (3) kds body (either row or column store) - DMA send
	 * (4) execution of kernel function
	 * (5) write back vrelation - DMA recv
	 */

	/* kern_gpuscan */
	offset = KERN_GPUSCAN_DMASEND_OFFSET(&gpuscan->kern);
	length = KERN_GPUSCAN_DMASEND_LENGTH(&gpuscan->kern);
	rc = clEnqueueWriteBuffer(kcmdq,
							  clgss->m_gpuscan,
							  CL_FALSE,
							  offset,
							  length,
							  &gpuscan->kern,
							  0,
							  NULL,
							  &clgss->events[clgss->ev_index]);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clEnqueueWriteBuffer: %s", opencl_strerror(rc));
		goto error;
	}
	clgss->ev_index++;
	pfm->bytes_dma_send += length;
	pfm->num_dma_send++;

	/* kern_data_store, via common routine */
	rc = clserv_dmasend_data_store(pds,
								   kcmdq,
								   clgss->m_dstore,
								   clgss->m_ktoast,
								   0,
								   NULL,
								   &clgss->ev_index,
								   clgss->events,
								   pfm);
	if (rc != CL_SUCCESS)
		goto error;

	/* execution of kernel function */
	rc = clEnqueueNDRangeKernel(kcmdq,
								clgss->kernel,
								1,
								NULL,
								&gwork_sz,
								&lwork_sz,
								clgss->ev_index,
								&clgss->events[0],
								&clgss->events[clgss->ev_index]);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clEnqueueNDRangeKernel: %s",
				   opencl_strerror(rc));
		goto error;
	}
	clgss->ev_index++;

	/* write back result vrelation */
	offset = KERN_GPUSCAN_DMARECV_OFFSET(&gpuscan->kern);
	length = KERN_GPUSCAN_DMARECV_LENGTH(&gpuscan->kern);
	rc = clEnqueueReadBuffer(kcmdq,
							 clgss->m_gpuscan,
							 CL_FALSE,
							 offset,
							 length,
							 kresults,
							 1,
							 &clgss->events[clgss->ev_index - 1],
							 &clgss->events[clgss->ev_index]);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clEnqueueReadBuffer: %s",
				   opencl_strerror(rc));
		goto error;
	}
	clgss->ev_index++;
	pfm->bytes_dma_recv += length;
	pfm->num_dma_recv++;

	/*
	 * Last, registers a callback routine that replies the message
	 * to the backend
	 */
	rc = clSetEventCallback(clgss->events[clgss->ev_index - 1],
							CL_COMPLETE,
							clserv_respond_gpuscan,
							clgss);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetEventCallback: %s", opencl_strerror(rc));
		goto error;
	}
	return;

error:
	if (clgss)
	{
		if (clgss->ev_index > 0)
			clWaitForEvents(clgss->ev_index, clgss->events);
		if (clgss->m_ktoast)
			clReleaseMemObject(clgss->m_ktoast);
		if (clgss->m_dstore)
			clReleaseMemObject(clgss->m_dstore);
		if (clgss->m_gpuscan)
			clReleaseMemObject(clgss->m_gpuscan);
		if (clgss->kernel)
			clReleaseKernel(clgss->kernel);
		if (clgss->program)
			clReleaseProgram(clgss->program);
		free(clgss);
	}
	gpuscan->msg.errcode = rc;
	pgstrom_reply_message(&gpuscan->msg);
}

/*
 * clserv_put_gpuscan
 *
 * Callback handler when reference counter of pgstrom_gpuscan object
 * reached to zero, due to pgstrom_put_message.
 * It also unlinks associated device program and release row-store.
 * Also note that this routine can be called under the OpenCL server
 * context.
 */
static void
clserv_put_gpuscan(pgstrom_message *msg)
{
	pgstrom_gpuscan	   *gpuscan = (pgstrom_gpuscan *)msg;

	/* unlink message queue, if any */
	if (msg->respq)
		pgstrom_put_queue(msg->respq);
	/* unlink device program, if any */
	if (gpuscan->dprog_key != 0)
		pgstrom_put_devprog_key(gpuscan->dprog_key);
	/* release row-/column-store, if any */
	if (gpuscan->pds)
		pgstrom_put_data_store(gpuscan->pds);
	pgstrom_shmem_free(gpuscan);
}
