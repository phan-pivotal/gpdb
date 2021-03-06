/*-------------------------------------------------------------------------
 *
 * prepunion.c
 *	  Routines to plan set-operation queries.  The filename is a leftover
 *	  from a time when only UNIONs were implemented.
 *
 * There are two code paths in the planner for set-operation queries.
 * If a subquery consists entirely of simple UNION ALL operations, it
 * is converted into an "append relation".  Otherwise, it is handled
 * by the general code in this module (plan_set_operations and its
 * subroutines).  There is some support code here for the append-relation
 * case, but most of the heavy lifting for that is done elsewhere,
 * notably in prepjointree.c and allpaths.c.
 *
 * There is also some code here to support planning of queries that use
 * inheritance (SELECT FROM foo*).  Inheritance trees are converted into
 * append relations, and thenceforth share code with the UNION ALL case.
 *
 *
 * Portions Copyright (c) 2006-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepunion.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_inherits_fn.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"

#include "cdb/cdbllize.h"                   /* pull_up_Flow() */
#include "cdb/cdbpartition.h"
#include "cdb/cdbpath.h"
#include "cdb/cdbsetop.h"
#include "cdb/cdbvars.h"
#include "commands/tablecmds.h"


typedef struct
{
	PlannerInfo *root;
	AppendRelInfo *appinfo;
	int			sublevels_up;
} adjust_appendrel_attrs_context;

static Path *recurse_set_operations(Node *setOp, PlannerInfo *root,
					   List *colTypes, List *colCollations,
					   bool junkOK,
					   int flag, List *refnames_tlist,
					   List **pTargetList,
					   double *pNumGroups);
static Path *generate_recursion_path(SetOperationStmt *setOp,
						PlannerInfo *root,
						List *refnames_tlist,
						List **pTargetList);
static Path *generate_union_path(SetOperationStmt *op, PlannerInfo *root,
					List *refnames_tlist,
					List **pTargetList,
					double *pNumGroups);
static Path *generate_nonunion_path(SetOperationStmt *op, PlannerInfo *root,
					   List *refnames_tlist,
					   List **pTargetList,
					   double *pNumGroups);
static List *recurse_union_children(Node *setOp, PlannerInfo *root,
					   SetOperationStmt *top_union,
					   List *refnames_tlist,
					   List **tlist_list);
static Path *make_union_unique(SetOperationStmt *op, Path *path, List *tlist,
				  PlannerInfo *root);
static bool choose_hashed_setop(PlannerInfo *root, List *groupClauses,
					Path *input_path,
					double dNumGroups, double dNumOutputRows,
					const char *construct);
static List *generate_setop_tlist(List *colTypes, List *colCollations,
					 int flag,
					 Index varno,
					 bool hack_constants,
					 List *input_tlist,
					 List *refnames_tlist);
static List *generate_append_tlist(List *colTypes, List *colCollations,
					  bool flag,
					  List *input_tlists,
					  List *refnames_tlist);
static List *generate_setop_grouplist(SetOperationStmt *op, List *targetlist);
static void expand_inherited_rtentry(PlannerInfo *root, RangeTblEntry *rte,
						 Index rti);
static void make_inh_translation_list(Relation oldrelation,
						  Relation newrelation,
						  Index newvarno,
						  List **translated_vars);
static Bitmapset *translate_col_privs(const Bitmapset *parent_privs,
					List *translated_vars);
static Node *adjust_appendrel_attrs_mutator(Node *node,
							   adjust_appendrel_attrs_context *context);
static Relids adjust_relid_set(Relids relids, Index oldrelid, Index newrelid);
static List *adjust_inherited_tlist(List *tlist,
					   AppendRelInfo *context);

/*
 * plan_set_operations
 *
 *	  Plans the queries for a tree of set operations (UNION/INTERSECT/EXCEPT)
 *
 * This routine only deals with the setOperations tree of the given query.
 * Any top-level ORDER BY requested in root->parse->sortClause will be handled
 * when we return to grouping_planner; likewise for LIMIT.
 *
 * What we return is an "upperrel" RelOptInfo containing at least one Path
 * that implements the set-operation tree.  In addition, root->processed_tlist
 * receives a targetlist representing the output of the topmost setop node.
 */
RelOptInfo *
plan_set_operations(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	SetOperationStmt *topop = (SetOperationStmt *) parse->setOperations;
	Node	   *node;
	RangeTblEntry *leftmostRTE;
	Query	   *leftmostQuery;
	RelOptInfo *setop_rel;
	Path	   *path;
	List	   *top_tlist;

	Assert(topop && IsA(topop, SetOperationStmt));

	/* check for unsupported stuff */
	Assert(parse->jointree->fromlist == NIL);
	Assert(parse->jointree->quals == NULL);
	Assert(parse->groupClause == NIL);
	Assert(parse->havingQual == NULL);
	Assert(parse->windowClause == NIL);
	Assert(parse->distinctClause == NIL);

	/*
	 * We'll need to build RelOptInfos for each of the leaf subqueries, which
	 * are RTE_SUBQUERY rangetable entries in this Query.  Prepare the index
	 * arrays for that.
	 */
	setup_simple_rel_arrays(root);

	/*
	 * Find the leftmost component Query.  We need to use its column names for
	 * all generated tlists (else SELECT INTO won't work right).
	 */
	node = topop->larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTE = root->simple_rte_array[((RangeTblRef *) node)->rtindex];
	leftmostQuery = leftmostRTE->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * We return our results in the (SETOP, NULL) upperrel.  For the moment,
	 * this is also the parent rel of all Paths in the setop tree; we may well
	 * change that in future.
	 */
	setop_rel = fetch_upper_rel(root, UPPERREL_SETOP, NULL);

	/*
	 * We don't currently worry about setting setop_rel's consider_parallel
	 * flag, nor about allowing FDWs to contribute paths to it.
	 */

	/*
	 * If the topmost node is a recursive union, it needs special processing.
	 */
	if (root->hasRecursion)
	{
		path = generate_recursion_path(topop, root,
									   leftmostQuery->targetList,
									   &top_tlist);
	}
	else
	{
		/*
		 * Recurse on setOperations tree to generate paths for set ops. The
		 * final output path should have just the column types shown as the
		 * output from the top-level node, plus possibly resjunk working
		 * columns (we can rely on upper-level nodes to deal with that).
		 */
		path = recurse_set_operations((Node *) topop, root,
									  topop->colTypes, topop->colCollations,
									  true, -1,
									  leftmostQuery->targetList,
									  &top_tlist,
									  NULL);
	}

	/* Must return the built tlist into root->processed_tlist. */
	root->processed_tlist = top_tlist;

	/* Add only the final path to the SETOP upperrel. */
	add_path(setop_rel, path);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_SETOP,
									NULL, setop_rel);

	/* Select cheapest path */
	set_cheapest(setop_rel);

	return setop_rel;
}

/*
 * recurse_set_operations
 *	  Recursively handle one step in a tree of set operations
 *
 * colTypes: OID list of set-op's result column datatypes
 * colCollations: OID list of set-op's result column collations
 * junkOK: if true, child resjunk columns may be left in the result
 * flag: if >= 0, add a resjunk output column indicating value of flag
 * refnames_tlist: targetlist to take column names from
 *
 * Returns a path for the subtree, as well as these output parameters:
 * *pTargetList: receives the fully-fledged tlist for the subtree's top plan
 * *pNumGroups: if not NULL, we estimate the number of distinct groups
 *		in the result, and store it there
 *
 * The pTargetList output parameter is mostly redundant with the pathtarget
 * of the returned path, but for the moment we need it because much of the
 * logic in this file depends on flag columns being marked resjunk.  Pending
 * a redesign of how that works, this is the easy way out.
 *
 * We don't have to care about typmods here: the only allowed difference
 * between set-op input and output typmods is input is a specific typmod
 * and output is -1, and that does not require a coercion.
 */
static Path *
recurse_set_operations(Node *setOp, PlannerInfo *root,
					   List *colTypes, List *colCollations,
					   bool junkOK,
					   int flag, List *refnames_tlist,
					   List **pTargetList,
					   double *pNumGroups)
{
	/* Guard against stack overflow due to overly complex setop nests */
	check_stack_depth();

	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = root->simple_rte_array[rtr->rtindex];
		Query	   *subquery = rte->subquery;
		RelOptInfo *rel;
		PlannerInfo *subroot;
		RelOptInfo *final_rel;
		Path	   *subpath;
		Path	   *path;
		List	   *tlist;

		Assert(subquery != NULL);

		/*
		 * We need to build a RelOptInfo for each leaf subquery.  This isn't
		 * used for much here, but it carries the subroot data structures
		 * forward to setrefs.c processing.
		 */
		rel = build_simple_rel(root, rtr->rtindex, RELOPT_BASEREL);

		/* plan_params should not be in use in current query level */
		Assert(root->plan_params == NIL);

		/* Generate a subroot and Paths for the subquery */
		PlannerConfig *config = CopyPlannerConfig(root->config);
		config->honor_order_by = false;
		subroot = rel->subroot = subquery_planner(root->glob, subquery,
												  root,
												  false,
												  root->tuple_fraction,
												  config);

		/*
		 * It should not be possible for the primitive query to contain any
		 * cross-references to other primitive queries in the setop tree.
		 */
		if (root->plan_params)
			elog(ERROR, "unexpected outer reference in set operation subquery");

		/*
		 * Mark rel with estimated output rows, width, etc.  Note that we have
		 * to do this before generating outer-query paths, else
		 * cost_subqueryscan is not happy.
		 */
		set_subquery_size_estimates(root, rel);

		/*
		 * For the moment, we consider only a single Path for the subquery.
		 * This should change soon (make it look more like
		 * set_subquery_pathlist).
		 */
		final_rel = fetch_upper_rel(subroot, UPPERREL_FINAL, NULL);
		subpath = get_cheapest_fractional_path(final_rel,
											   root->tuple_fraction);

		/*
		 * Stick a SubqueryScanPath atop that.
		 *
		 * We don't bother to determine the subquery's output ordering since
		 * it won't be reflected in the set-op result anyhow; so just label
		 * the SubqueryScanPath with nil pathkeys.  (XXX that should change
		 * soon too, likely.)
		 */
		/*
		 * GPDB_96_MERGE_FIXME: can we really use the subpath's locus here unmodified?
		 * Shouldn't we convert it to use Vars pointing to the outputs of the subquery,
		 * like in subquery_pathlist()
		 */
		path = (Path *) create_subqueryscan_path(root, rel, subpath,
												 NIL, subpath->locus, NULL);

		/*
		 * Figure out the appropriate target list, and update the
		 * SubqueryScanPath with the PathTarget form of that.
		 */
		tlist = generate_setop_tlist(colTypes, colCollations,
									 flag,
									 rtr->rtindex,
									 true,
									 subroot->processed_tlist,
									 refnames_tlist);

		path = apply_projection_to_path(root, rel, path,
										create_pathtarget(root, tlist));

		/* Return the fully-fledged tlist to caller, too */
		*pTargetList = tlist;

		/*
		 * Estimate number of groups if caller wants it.  If the subquery used
		 * grouping or aggregation, its output is probably mostly unique
		 * anyway; otherwise do statistical estimation.
		 */
		if (pNumGroups)
		{
			if (subquery->groupClause || subquery->groupingSets ||
				subquery->distinctClause ||
				subroot->hasHavingQual || subquery->hasAggs)
				*pNumGroups = subpath->rows;
			else
				*pNumGroups = estimate_num_groups(subroot,
							get_tlist_exprs(subroot->processed_tlist, false),
												  subpath->rows,
												  NULL);
		}

		return (Path *) path;
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;
		Path	   *path;

		/* UNIONs are much different from INTERSECT/EXCEPT */
		if (op->op == SETOP_UNION)
			path = generate_union_path(op, root,
									   refnames_tlist,
									   pTargetList,
									   pNumGroups);
		else
			path = generate_nonunion_path(op, root,
										  refnames_tlist,
										  pTargetList,
										  pNumGroups);

		/*
		 * If necessary, add a Result node to project the caller-requested
		 * output columns.
		 *
		 * XXX you don't really want to know about this: setrefs.c will apply
		 * fix_upper_expr() to the Result node's tlist. This would fail if the
		 * Vars generated by generate_setop_tlist() were not exactly equal()
		 * to the corresponding tlist entries of the subplan. However, since
		 * the subplan was generated by generate_union_plan() or
		 * generate_nonunion_plan(), and hence its tlist was generated by
		 * generate_append_tlist(), this will work.  We just tell
		 * generate_setop_tlist() to use varno OUTER (this was changed for
         * better EXPLAIN output in CDB/MPP; varno 0 is used in PostgreSQL).
		 */
		if (flag >= 0 ||
			!tlist_same_datatypes(*pTargetList, colTypes, junkOK) ||
			!tlist_same_collations(*pTargetList, colCollations, junkOK))
		{
			*pTargetList = generate_setop_tlist(colTypes, colCollations,
												flag,
												OUTER_VAR,
												false,
												*pTargetList,
												refnames_tlist);
			path = apply_projection_to_path(root,
											path->parent,
											path,
											create_pathtarget(root,
															  *pTargetList));
		}
		return path;
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
		*pTargetList = NIL;
		return NULL;			/* keep compiler quiet */
	}
}

/*
 * Generate path for a recursive UNION node
 */
static Path *
generate_recursion_path(SetOperationStmt *setOp, PlannerInfo *root,
						List *refnames_tlist,
						List **pTargetList)
{
	RelOptInfo *result_rel = fetch_upper_rel(root, UPPERREL_SETOP, NULL);
	Path	   *path;
	Path	   *lpath;
	Path	   *rpath;
	List	   *lpath_tlist;
	List	   *rpath_tlist;
	List	   *tlist;
	List	   *groupList;
	double		dNumGroups;

	/* Parser should have rejected other cases */
	if (setOp->op != SETOP_UNION)
		elog(ERROR, "only UNION queries can be recursive");
	/* Worktable ID should be assigned */
	Assert(root->wt_param_id >= 0);

	/*
	 * Unlike a regular UNION node, process the left and right inputs
	 * separately without any intention of combining them into one Append.
	 */
	lpath = recurse_set_operations(setOp->larg, root,
								   setOp->colTypes, setOp->colCollations,
								   false, -1,
								   refnames_tlist,
								   &lpath_tlist,
								   NULL);

	/*
	 * If the non-recursive side is SegmentGeneral, force it to be executed
	 * on exactly one segment. The worktable scan we build on the recursive
	 * side will use the same locus as the non-recursive side, and if it's
	 * SegmentGeneral, the result of the join may end up having a different
	 * locus.
	 *
	 * GPDB_96_MERGE_FIXME: On master, before the merge, more complicated
	 * logic was added in commit ad6a6067d9 to make the loci on the WorkTableScan
	 * and the RecursiveUnion correct. That was largely reverted as part of the
	 * merge, and things seem to be working with this much simpler thing, but
	 * I'm not sure if the logic is 100% correct now.
	 */
	if (CdbPathLocus_IsSegmentGeneral(lpath->locus))
	{
		CdbPathLocus gather_locus;

		CdbPathLocus_MakeSingleQE(&gather_locus, lpath->locus.numsegments);
		lpath = cdbpath_create_motion_path(root, lpath, NIL, false, gather_locus);
	}

	/* The right path will want to look at the left one ... */
	root->non_recursive_path = lpath;
	rpath = recurse_set_operations(setOp->rarg, root,
								   setOp->colTypes, setOp->colCollations,
								   false, -1,
								   refnames_tlist,
								   &rpath_tlist,
								   NULL);
	root->non_recursive_path = NULL;

	/*
	 * Generate tlist for RecursiveUnion path node --- same as in Append cases
	 */
	tlist = generate_append_tlist(setOp->colTypes, setOp->colCollations, false,
								  list_make2(lpath_tlist, rpath_tlist),
								  refnames_tlist);

	*pTargetList = tlist;

	/*
	 * If UNION, identify the grouping operators
	 */
	if (setOp->all)
	{
		groupList = NIL;
		dNumGroups = 0;
	}
	else
	{
		/* Identify the grouping semantics */
		groupList = generate_setop_grouplist(setOp, tlist);

		/* We only support hashing here */
		if (!grouping_is_hashable(groupList))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not implement recursive UNION"),
					 errdetail("All column datatypes must be hashable.")));

		/*
		 * For the moment, take the number of distinct groups as equal to the
		 * total input size, ie, the worst case.
		 */
		dNumGroups = lpath->rows + rpath->rows * 10;
	}

	/*
	 * And make the plan node.
	 */
	path = (Path *) create_recursiveunion_path(root,
											   result_rel,
											   lpath,
											   rpath,
											   create_pathtarget(root, tlist),
											   groupList,
											   root->wt_param_id,
											   dNumGroups);
	path->locus = rpath->locus;

	return path;
}

/*
 * Generate path for a UNION or UNION ALL node
 */
static Path *
generate_union_path(SetOperationStmt *op, PlannerInfo *root,
					List *refnames_tlist,
					List **pTargetList,
					double *pNumGroups)
{
	RelOptInfo *result_rel = fetch_upper_rel(root, UPPERREL_SETOP, NULL);
	double		save_fraction = root->tuple_fraction;
	List	   *pathlist;
	List	   *child_tlists1;
	List	   *child_tlists2;
	List	   *tlist_list;
	List	   *tlist;
	Path	   *path;
	GpSetOpType optype = PSETOP_NONE; /* CDB */

	/*
	 * If plain UNION, tell children to fetch all tuples.
	 *
	 * Note: in UNION ALL, we pass the top-level tuple_fraction unmodified to
	 * each arm of the UNION ALL.  One could make a case for reducing the
	 * tuple fraction for later arms (discounting by the expected size of the
	 * earlier arms' results) but it seems not worth the trouble. The normal
	 * case where tuple_fraction isn't already zero is a LIMIT at top level,
	 * and passing it down as-is is usually enough to get the desired result
	 * of preferring fast-start plans.
	 */
	if (!op->all)
		root->tuple_fraction = 0.0;

	/*
	 * If any of my children are identical UNION nodes (same op, all-flag, and
	 * colTypes) then they can be merged into this node so that we generate
	 * only one Append and unique-ification for the lot.  Recurse to find such
	 * nodes and compute their children's paths.
	 */
	pathlist = list_concat(recurse_union_children(op->larg, root,
												  op, refnames_tlist,
												  &child_tlists1),
						   recurse_union_children(op->rarg, root,
												  op, refnames_tlist,
												  &child_tlists2));
	tlist_list = list_concat(child_tlists1, child_tlists2);

	/* GPDB_96_MERGE_FIXME: We should use the new pathified upper planner
	 * infrastructure for this. I think we should create multiple Paths,
	 * representing different kinds of PSETOP_* implementations, and
	 * let the "add_path()" choose the cheapest one.
	 */
	/* CDB: Decide on approach, condition argument plans to suit. */
	if ( Gp_role == GP_ROLE_DISPATCH )
	{
		optype = choose_setop_type(pathlist);
		adjust_setop_arguments(root, pathlist, tlist_list, optype);
	}
	else if (Gp_role == GP_ROLE_UTILITY ||
			 Gp_role == GP_ROLE_EXECUTE) /* MPP-2928 */
	{
		optype = PSETOP_SEQUENTIAL_QD;
	}

	/*
	 * Generate tlist for Append plan node.
	 *
	 * The tlist for an Append plan isn't important as far as the Append is
	 * concerned, but we must make it look real anyway for the benefit of the
	 * next plan level up.
	 */
	tlist = generate_append_tlist(op->colTypes, op->colCollations, false,
								  tlist_list, refnames_tlist);

	*pTargetList = tlist;

	/*
	 * Append the child results together.
	 */
	path = (Path *) create_append_path(root, result_rel, pathlist, NULL, 0);
	// GPDB_96_MERGE_FIXME: Where should this go now?
	//mark_append_locus(plan, optype); /* CDB: Mark the plan result locus. */

	/* We have to manually jam the right tlist into the path; ick */
	path->pathtarget = create_pathtarget(root, tlist);

	/*
	 * For UNION ALL, we just need the Append path.  For UNION, need to add
	 * node(s) to remove duplicates.
	 */
	if (!op->all)
	{
		if ( optype == PSETOP_PARALLEL_PARTITIONED )
		{
			/* CDB: Hash motion to collocate non-distinct tuples. */
			path = make_motion_hash_all_targets(root, path, tlist);
		}
		path = make_union_unique(op, path, tlist, root);
	}

	/*
	 * Estimate number of groups if caller wants it.  For now we just assume
	 * the output is unique --- this is certainly true for the UNION case, and
	 * we want worst-case estimates anyway.
	 */
	if (pNumGroups)
		*pNumGroups = path->rows;

	/* Undo effects of possibly forcing tuple_fraction to 0 */
	root->tuple_fraction = save_fraction;

	return path;
}

/*
 * Generate path for an INTERSECT, INTERSECT ALL, EXCEPT, or EXCEPT ALL node
 */
static Path *
generate_nonunion_path(SetOperationStmt *op, PlannerInfo *root,
					   List *refnames_tlist,
					   List **pTargetList,
					   double *pNumGroups)
{
	RelOptInfo *result_rel = fetch_upper_rel(root, UPPERREL_SETOP, NULL);
	double		save_fraction = root->tuple_fraction;
	Path	   *lpath,
			   *rpath,
			   *path;
	List	   *lpath_tlist,
			   *rpath_tlist,
			   *tlist_list,
			   *tlist,
			   *groupList,
			   *pathlist;
	double		dLeftGroups,
				dRightGroups,
				dNumGroups,
				dNumOutputRows;
	bool		use_hash;
	SetOpCmd	cmd;
	int			firstFlag;
	GpSetOpType optype = PSETOP_NONE; /* CDB */

	/*
	 * Tell children to fetch all tuples.
	 */
	root->tuple_fraction = 0.0;

	/* Recurse on children, ensuring their outputs are marked */
	lpath = recurse_set_operations(op->larg, root,
								   op->colTypes, op->colCollations,
								   false, 0,
								   refnames_tlist,
								   &lpath_tlist,
								   &dLeftGroups);
	rpath = recurse_set_operations(op->rarg, root,
								   op->colTypes, op->colCollations,
								   false, 1,
								   refnames_tlist,
								   &rpath_tlist,
								   &dRightGroups);

	/* Undo effects of forcing tuple_fraction to 0 */
	root->tuple_fraction = save_fraction;

	/*
	 * For EXCEPT, we must put the left input first.  For INTERSECT, either
	 * order should give the same results, and we prefer to put the smaller
	 * input first in order to minimize the size of the hash table in the
	 * hashing case.  "Smaller" means the one with the fewer groups.
	 */
	if (op->op == SETOP_EXCEPT || dLeftGroups <= dRightGroups)
	{
		pathlist = list_make2(lpath, rpath);
		tlist_list = list_make2(lpath_tlist, rpath_tlist);
		firstFlag = 0;
	}
	else
	{
		pathlist = list_make2(rpath, lpath);
		tlist_list = list_make2(rpath_tlist, lpath_tlist);
		firstFlag = 1;
	}

	/* GPDB_96_MERGE_FIXME: We should use the new pathified upper planner
	 * infrastructure for this. I think we should create multiple Paths,
	 * representing different kinds of PSETOP_* implementations, and
	 * let the "add_path()" choose the cheapest one.
	 */

	/* CDB: Decide on approach, condition argument plans to suit. */
	if ( Gp_role == GP_ROLE_DISPATCH )
	{
		optype = choose_setop_type(pathlist);
		adjust_setop_arguments(root, pathlist, tlist_list, optype);
	}
	else if ( Gp_role == GP_ROLE_UTILITY 
			|| Gp_role == GP_ROLE_EXECUTE ) /* MPP-2928 */
	{
		optype = PSETOP_SEQUENTIAL_QD;
	}

	if ( optype == PSETOP_PARALLEL_PARTITIONED )
	{
		/*
		 * CDB: Collocate non-distinct tuples prior to sort or hash. We must
		 * put the Redistribute nodes below the Append, otherwise we lose
		 * the order of the firstFlags.
		 */
		ListCell   *pathcell;
		ListCell   *tlistcell;
		List	   *newpathlist = NIL;

		forboth(pathcell, pathlist, tlistcell, tlist_list)
		{
			Path	   *subpath = (Path *) lfirst(pathcell);
			List	   *subtlist = (List *) lfirst(tlistcell);
#if 0
			/* GPDB_96_MERGE_FIXME */
			/*
			 * If the subplan already has a Motion at the top, peel it off
			 * first, so that we don't have a Motion on top of a Motion.
			 * That would be silly. I wish we could be smarter and not
			 * create such a Motion in the first place, but it's too late
			 * for that here.
			 */
			while (IsA(subpath, Motion))
				subpath = subpath->lefttree;
#endif
			newpathlist = lappend(newpathlist,
								  make_motion_hash_all_targets(root, subpath, subtlist));
		}
		pathlist = newpathlist;
	}

	/*
	 * Generate tlist for Append plan node.
	 *
	 * The tlist for an Append plan isn't important as far as the Append is
	 * concerned, but we must make it look real anyway for the benefit of the
	 * next plan level up.  In fact, it has to be real enough that the flag
	 * column is shown as a variable not a constant, else setrefs.c will get
	 * confused.
	 */
	tlist = generate_append_tlist(op->colTypes, op->colCollations, true,
								  tlist_list, refnames_tlist);

	*pTargetList = tlist;

	/*
	 * Append the child results together.
	 */
	path = (Path *) create_append_path(root, result_rel, pathlist, NULL, 0);
	mark_append_locus(path, optype); /* CDB: Mark the plan result locus. */

	/* We have to manually jam the right tlist into the path; ick */
	path->pathtarget = create_pathtarget(root, tlist);

	/* Identify the grouping semantics */
	groupList = generate_setop_grouplist(op, tlist);

	/* punt if nothing to group on (not worth fixing in back branches) */
	if (groupList == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /* translator: %s is UNION, INTERSECT, or EXCEPT */
				 errmsg("%s over no columns is not supported",
						(op->op == SETOP_INTERSECT) ? "INTERSECT" : "EXCEPT")));

	/*
	 * Estimate number of distinct groups that we'll need hashtable entries
	 * for; this is the size of the left-hand input for EXCEPT, or the smaller
	 * input for INTERSECT.  Also estimate the number of eventual output rows.
	 * In non-ALL cases, we estimate each group produces one output row; in
	 * ALL cases use the relevant relation size.  These are worst-case
	 * estimates, of course, but we need to be conservative.
	 */
	if (op->op == SETOP_EXCEPT)
	{
		dNumGroups = dLeftGroups;
		dNumOutputRows = op->all ? lpath->rows : dNumGroups;
	}
	else
	{
		dNumGroups = Min(dLeftGroups, dRightGroups);
		dNumOutputRows = op->all ? Min(lpath->rows, rpath->rows) : dNumGroups;
	}

	/*
	 * Decide whether to hash or sort, and add a sort node if needed.
	 */
	use_hash = choose_hashed_setop(root, groupList, path,
								   dNumGroups, dNumOutputRows,
					   (op->op == SETOP_INTERSECT) ? "INTERSECT" : "EXCEPT");

	if (!use_hash)
		path = (Path *) create_sort_path(root,
										 result_rel,
										 path,
										 make_pathkeys_for_sortclauses(root,
																   groupList,
																	   tlist),
										 -1.0);

	/*
	 * Finally, add a SetOp path node to generate the correct output.
	 */
	switch (op->op)
	{
		case SETOP_INTERSECT:
			cmd = op->all ? SETOPCMD_INTERSECT_ALL : SETOPCMD_INTERSECT;
			break;
		case SETOP_EXCEPT:
			cmd = op->all ? SETOPCMD_EXCEPT_ALL : SETOPCMD_EXCEPT;
			break;
		default:
			elog(ERROR, "unrecognized set op: %d", (int) op->op);
			cmd = SETOPCMD_INTERSECT;	/* keep compiler quiet */
			break;
	}
	path = (Path *) create_setop_path(root,
									  result_rel,
									  path,
									  cmd,
									  use_hash ? SETOP_HASHED : SETOP_SORTED,
									  groupList,
									  list_length(op->colTypes) + 1,
									  use_hash ? firstFlag : -1,
									  dNumGroups,
									  dNumOutputRows);

	if (pNumGroups)
		*pNumGroups = dNumGroups;

	return path;
}

/*
 * Pull up children of a UNION node that are identically-propertied UNIONs.
 *
 * NOTE: we can also pull a UNION ALL up into a UNION, since the distinct
 * output rows will be lost anyway.
 *
 * NOTE: currently, we ignore collations while determining if a child has
 * the same properties.  This is semantically sound only so long as all
 * collations have the same notion of equality.  It is valid from an
 * implementation standpoint because we don't care about the ordering of
 * a UNION child's result: UNION ALL results are always unordered, and
 * generate_union_path will force a fresh sort if the top level is a UNION.
 */
static List *
recurse_union_children(Node *setOp, PlannerInfo *root,
					   SetOperationStmt *top_union,
					   List *refnames_tlist,
					   List **tlist_list)
{
	List	   *result;
	List	   *child_tlist;

	if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		if (op->op == top_union->op &&
			(op->all == top_union->all || op->all) &&
			equal(op->colTypes, top_union->colTypes))
		{
			/* Same UNION, so fold children into parent's subpath list */
			List	   *child_tlists1;
			List	   *child_tlists2;

			result = list_concat(recurse_union_children(op->larg, root,
														top_union,
														refnames_tlist,
														&child_tlists1),
								 recurse_union_children(op->rarg, root,
														top_union,
														refnames_tlist,
														&child_tlists2));
			*tlist_list = list_concat(child_tlists1, child_tlists2);
			return result;
		}
	}

	/*
	 * Not same, so plan this child separately.
	 *
	 * Note we disallow any resjunk columns in child results.  This is
	 * necessary since the Append node that implements the union won't do any
	 * projection, and upper levels will get confused if some of our output
	 * tuples have junk and some don't.  This case only arises when we have an
	 * EXCEPT or INTERSECT as child, else there won't be resjunk anyway.
	 */
	result = list_make1(recurse_set_operations(setOp, root,
											   top_union->colTypes,
											   top_union->colCollations,
											   false, -1,
											   refnames_tlist,
											   &child_tlist,
											   NULL));
	*tlist_list = list_make1(child_tlist);
	return result;
}

/*
 * Add nodes to the given path tree to unique-ify the result of a UNION.
 */
static Path *
make_union_unique(SetOperationStmt *op, Path *path, List *tlist,
				  PlannerInfo *root)
{
	RelOptInfo *result_rel = fetch_upper_rel(root, UPPERREL_SETOP, NULL);
	List	   *groupList;
	double		dNumGroups;

	/* Identify the grouping semantics */
	groupList = generate_setop_grouplist(op, tlist);

	/* punt if nothing to group on (not worth fixing in back branches) */
	if (groupList == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /* translator: %s is UNION, INTERSECT, or EXCEPT */
				 errmsg("%s over no columns is not supported", "UNION")));

	/*
	 * XXX for the moment, take the number of distinct groups as equal to the
	 * total input size, ie, the worst case.  This is too conservative, but we
	 * don't want to risk having the hashtable overrun memory; also, it's not
	 * clear how to get a decent estimate of the true size.  One should note
	 * as well the propensity of novices to write UNION rather than UNION ALL
	 * even when they don't expect any duplicates...
	 */
	dNumGroups = path->rows;

	/* Decide whether to hash or sort */
	if (choose_hashed_setop(root, groupList, path,
							dNumGroups, dNumGroups,
							"UNION"))
	{
		/* Hashed aggregate plan --- no sort needed */
		path = (Path *) create_agg_path(root,
										result_rel,
										path,
										create_pathtarget(root, tlist),
										AGG_HASHED,
										AGGSPLIT_SIMPLE,
										false, /* streaming */
										groupList,
										NIL,
										NULL,
										dNumGroups,
										NULL);
	}
	else
	{
		/* Sort and Unique */
		path = (Path *) create_sort_path(root,
										 result_rel,
										 path,
										 make_pathkeys_for_sortclauses(root,
																   groupList,
																	   tlist),
										 -1.0);
		/* We have to manually jam the right tlist into the path; ick */
		path->pathtarget = create_pathtarget(root, tlist);
		path = (Path *) create_upper_unique_path(root,
												 result_rel,
												 path,
												 list_length(path->pathkeys),
												 dNumGroups);
	}

	return path;
}

/*
 * choose_hashed_setop - should we use hashing for a set operation?
 */
static bool
choose_hashed_setop(PlannerInfo *root, List *groupClauses,
					Path *input_path,
					double dNumGroups, double dNumOutputRows,
					const char *construct)
{
	int			numGroupCols = list_length(groupClauses);
	bool		can_sort;
	bool		can_hash;
	Size		hashentrysize;
	Path		hashed_p;
	Path		sorted_p;
	double		tuple_fraction;

	/* Check whether the operators support sorting or hashing */
	can_sort = grouping_is_sortable(groupClauses);
	can_hash = grouping_is_hashable(groupClauses);
	if (can_hash && can_sort)
	{
		/* we have a meaningful choice to make, continue ... */
	}
	else if (can_hash)
		return true;
	else if (can_sort)
		return false;
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/* translator: %s is UNION, INTERSECT, or EXCEPT */
				 errmsg("could not implement %s", construct),
				 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

	/* Prefer sorting when enable_hashagg is off */
	if (!enable_hashagg)
		return false;

	/*
	 * Don't do it if it doesn't look like the hashtable will fit into
	 * work_mem.
	 *
	 * GPDB: In other places where we are building a Hash Aggregate, we use
	 * calcHashAggTableSizes(), which takes into account that in GPDB, a Hash
	 * Aggregate can spill to disk. We must *not* do that here, because we
	 * might be building a Hashed SetOp, not a Hash Aggregate. A Hashed SetOp
	 * uses the upstream hash table implementation unmodified, and cannot
	 * spill.
	 * FIXME: It's a bit lame that Hashed SetOp cannot spill to disk. And it's
	 * even more lame that we don't account the spilling correctly, if we are
	 * in fact constructing a Hash Aggregate. A UNION is implemented with a
	 * Hash Aggregate, only INTERSECT and EXCEPT use Hashed SetOp.
	 */
	hashentrysize = MAXALIGN(input_path->pathtarget->width) + MAXALIGN(SizeofMinimalTupleHeader);

	if (hashentrysize * dNumGroups > work_mem * 1024L)
		return false;

	/*
	 * See if the estimated cost is no more than doing it the other way.
	 *
	 * We need to consider input_plan + hashagg versus input_plan + sort +
	 * group.  Note that the actual result plan might involve a SetOp or
	 * Unique node, not Agg or Group, but the cost estimates for Agg and Group
	 * should be close enough for our purposes here.
	 *
	 * These path variables are dummies that just hold cost fields; we don't
	 * make actual Paths for these steps.
	 */
	cost_agg(&hashed_p, root, AGG_HASHED, NULL,
			 numGroupCols, dNumGroups / planner_segment_count(NULL),
			 input_path->startup_cost, input_path->total_cost,
			 input_path->rows,
			 NULL, /* GPDB: We are using the upstream hash table implementation,
					* which does not spill. */
			 false /* hash_streaming */);

	/*
	 * Now for the sorted case.  Note that the input is *always* unsorted,
	 * since it was made by appending unrelated sub-relations together.
	 */
	sorted_p.startup_cost = input_path->startup_cost;
	sorted_p.total_cost = input_path->total_cost;
	/* XXX cost_sort doesn't actually look at pathkeys, so just pass NIL */
	cost_sort(&sorted_p, root, NIL, sorted_p.total_cost,
			  input_path->rows, input_path->pathtarget->width,
			  0.0, work_mem, -1.0);
	cost_group(&sorted_p, root, numGroupCols, dNumGroups,
			   sorted_p.startup_cost, sorted_p.total_cost,
			   input_path->rows);

	/*
	 * Now make the decision using the top-level tuple fraction.  First we
	 * have to convert an absolute count (LIMIT) into fractional form.
	 */
	tuple_fraction = root->tuple_fraction;
	if (tuple_fraction >= 1.0)
		tuple_fraction /= dNumOutputRows;

	if (compare_fractional_path_costs(&hashed_p, &sorted_p,
									  tuple_fraction) < 0)
	{
		/* Hashed is cheaper, so use it */
		return true;
	}
	return false;
}

/*
 * Generate targetlist for a set-operation plan node
 *
 * colTypes: OID list of set-op's result column datatypes
 * colCollations: OID list of set-op's result column collations
 * flag: -1 if no flag column needed, 0 or 1 to create a const flag column
 * varno: varno to use in generated Vars
 * hack_constants: true to copy up constants (see comments in code)
 * input_tlist: targetlist of this node's input node
 * refnames_tlist: targetlist to take column names from
 */
static List *
generate_setop_tlist(List *colTypes, List *colCollations,
					 int flag,
					 Index varno,
					 bool hack_constants,
					 List *input_tlist,
					 List *refnames_tlist)
{
	List	   *tlist = NIL;
	int			resno = 1;
	ListCell   *ctlc,
			   *cclc,
			   *itlc,
			   *rtlc;
	TargetEntry *tle;
	Node	   *expr;

	/* there's no forfour() so we must chase one list manually */
	rtlc = list_head(refnames_tlist);
	forthree(ctlc, colTypes, cclc, colCollations, itlc, input_tlist)
	{
		Oid			colType = lfirst_oid(ctlc);
		Oid			colColl = lfirst_oid(cclc);
		TargetEntry *inputtle = (TargetEntry *) lfirst(itlc);
		TargetEntry *reftle = (TargetEntry *) lfirst(rtlc);

		rtlc = lnext(rtlc);

		Assert(inputtle->resno == resno);
		Assert(reftle->resno == resno);
		Assert(!inputtle->resjunk);
		Assert(!reftle->resjunk);

		/*
		 * Generate columns referencing input columns and having appropriate
		 * data types and column names.  Insert datatype coercions where
		 * necessary.
		 *
		 * HACK: constants in the input's targetlist are copied up as-is
		 * rather than being referenced as subquery outputs.  This is mainly
		 * to ensure that when we try to coerce them to the output column's
		 * datatype, the right things happen for UNKNOWN constants.  But do
		 * this only at the first level of subquery-scan plans; we don't want
		 * phony constants appearing in the output tlists of upper-level
		 * nodes!
		 */
		if (hack_constants && inputtle->expr && IsA(inputtle->expr, Const))
			expr = (Node *) inputtle->expr;
		else
			expr = (Node *) makeVar(varno,
									inputtle->resno,
									exprType((Node *) inputtle->expr),
									exprTypmod((Node *) inputtle->expr),
									exprCollation((Node *) inputtle->expr),
									0);

		if (exprType(expr) != colType)
		{
			/*
			 * Note: it's not really cool to be applying coerce_to_common_type
			 * here; one notable point is that assign_expr_collations never
			 * gets run on any generated nodes.  For the moment that's not a
			 * problem because we force the correct exposed collation below.
			 * It would likely be best to make the parser generate the correct
			 * output tlist for every set-op to begin with, though.
			 */
			expr = coerce_to_common_type(NULL,	/* no UNKNOWNs here */
										 expr,
										 colType,
										 "UNION/INTERSECT/EXCEPT");
		}

		/*
		 * Ensure the tlist entry's exposed collation matches the set-op. This
		 * is necessary because plan_set_operations() reports the result
		 * ordering as a list of SortGroupClauses, which don't carry collation
		 * themselves but just refer to tlist entries.  If we don't show the
		 * right collation then planner.c might do the wrong thing in
		 * higher-level queries.
		 *
		 * Note we use RelabelType, not CollateExpr, since this expression
		 * will reach the executor without any further processing.
		 */
		if (exprCollation(expr) != colColl)
		{
			expr = (Node *) makeRelabelType((Expr *) expr,
											exprType(expr),
											exprTypmod(expr),
											colColl,
											COERCE_IMPLICIT_CAST);
		}

		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup(reftle->resname),
							  false);

		/*
		 * By convention, all non-resjunk columns in a setop tree have
		 * ressortgroupref equal to their resno.  In some cases the ref isn't
		 * needed, but this is a cleaner way than modifying the tlist later.
		 */
		tle->ressortgroupref = tle->resno;

		tlist = lappend(tlist, tle);
	}

	if (flag >= 0)
	{
		/* Add a resjunk flag column */
		/* flag value is the given constant */
		expr = (Node *) makeConst(INT4OID,
								  -1,
								  InvalidOid,
								  sizeof(int32),
								  Int32GetDatum(flag),
								  false,
								  true);
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup("flag"),
							  true);
		tlist = lappend(tlist, tle);
	}

	return tlist;
}

/*
 * Generate targetlist for a set-operation Append node
 *
 * colTypes: OID list of set-op's result column datatypes
 * colCollations: OID list of set-op's result column collations
 * flag: true to create a flag column copied up from subplans
 * input_tlists: list of tlists for sub-plans of the Append
 * refnames_tlist: targetlist to take column names from
 *
 * The entries in the Append's targetlist should always be simple Vars;
 * we just have to make sure they have the right datatypes/typmods/collations.
 * The Vars are always generated with varno OUTER (CDB/MPP change for
 * EXPLAIN; varno 0 was used in PostgreSQL).
 *
 * XXX a problem with the varno-zero approach is that set_pathtarget_cost_width
 * cannot figure out a realistic width for the tlist we make here.  But we
 * ought to refactor this code to produce a PathTarget directly, anyway.
 */
static List *
generate_append_tlist(List *colTypes, List *colCollations,
					  bool flag,
					  List *input_tlists,
					  List *refnames_tlist)
{
	List	   *tlist = NIL;
	int			resno = 1;
	ListCell   *curColType;
	ListCell   *curColCollation;
	ListCell   *ref_tl_item;
	int			colindex;
	TargetEntry *tle;
	Node	   *expr;
	ListCell   *tlistl;
	int32	   *colTypmods;

	/*
	 * First extract typmods to use.
	 *
	 * If the inputs all agree on type and typmod of a particular column, use
	 * that typmod; else use -1.
	 */
	colTypmods = (int32 *) palloc(list_length(colTypes) * sizeof(int32));

	foreach(tlistl, input_tlists)
	{
		List	   *subtlist = (List *) lfirst(tlistl);
		ListCell   *subtlistl;

		curColType = list_head(colTypes);
		colindex = 0;
		foreach(subtlistl, subtlist)
		{
			TargetEntry *subtle = (TargetEntry *) lfirst(subtlistl);

			if (subtle->resjunk)
				continue;
			Assert(curColType != NULL);
			if (exprType((Node *) subtle->expr) == lfirst_oid(curColType))
			{
				/* If first subplan, copy the typmod; else compare */
				int32		subtypmod = exprTypmod((Node *) subtle->expr);

				if (tlistl == list_head(input_tlists))
					colTypmods[colindex] = subtypmod;
				else if (subtypmod != colTypmods[colindex])
					colTypmods[colindex] = -1;
			}
			else
			{
				/* types disagree, so force typmod to -1 */
				colTypmods[colindex] = -1;
			}
			curColType = lnext(curColType);
			colindex++;
		}
		Assert(curColType == NULL);
	}

	/*
	 * Now we can build the tlist for the Append.
	 */
	colindex = 0;
	forthree(curColType, colTypes, curColCollation, colCollations,
			 ref_tl_item, refnames_tlist)
	{
		Oid			colType = lfirst_oid(curColType);
		int32		colTypmod = colTypmods[colindex++];
		Oid			colColl = lfirst_oid(curColCollation);
		TargetEntry *reftle = (TargetEntry *) lfirst(ref_tl_item);

		Assert(reftle->resno == resno);
		Assert(!reftle->resjunk);
		expr = (Node *) makeVar(OUTER_VAR,
								resno,
								colType,
								colTypmod,
								colColl,
								0);
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup(reftle->resname),
							  false);

		/*
		 * By convention, all non-resjunk columns in a setop tree have
		 * ressortgroupref equal to their resno.  In some cases the ref isn't
		 * needed, but this is a cleaner way than modifying the tlist later.
		 */
		tle->ressortgroupref = tle->resno;

		tlist = lappend(tlist, tle);
	}

	if (flag)
	{
		/* Add a resjunk flag column */
		/* flag value is shown as copied up from subplan */
		expr = (Node *) makeVar(OUTER_VAR,
								resno,
								INT4OID,
								-1,
								InvalidOid,
								0);
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup("flag"),
							  true);
		tlist = lappend(tlist, tle);
	}

	pfree(colTypmods);

	return tlist;
}

/*
 * generate_setop_grouplist
 *		Build a SortGroupClause list defining the sort/grouping properties
 *		of the setop's output columns.
 *
 * Parse analysis already determined the properties and built a suitable
 * list, except that the entries do not have sortgrouprefs set because
 * the parser output representation doesn't include a tlist for each
 * setop.  So what we need to do here is copy that list and install
 * proper sortgrouprefs into it (copying those from the targetlist).
 */
static List *
generate_setop_grouplist(SetOperationStmt *op, List *targetlist)
{
	List	   *grouplist = (List *) copyObject(op->groupClauses);
	ListCell   *lg;
	ListCell   *lt;

	lg = list_head(grouplist);
	foreach(lt, targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lt);
		SortGroupClause *sgc;

		if (tle->resjunk)
		{
			/* resjunk columns should not have sortgrouprefs */
			Assert(tle->ressortgroupref == 0);
			continue;			/* ignore resjunk columns */
		}

		/* non-resjunk columns should have sortgroupref = resno */
		Assert(tle->ressortgroupref == tle->resno);

		/* non-resjunk columns should have grouping clauses */
		Assert(lg != NULL);
		sgc = (SortGroupClause *) lfirst(lg);
		lg = lnext(lg);
		Assert(sgc->tleSortGroupRef == 0);

		sgc->tleSortGroupRef = tle->ressortgroupref;
	}
	Assert(lg == NULL);
	return grouplist;
}


/*
 * expand_inherited_tables
 *		Expand each rangetable entry that represents an inheritance set
 *		into an "append relation".  At the conclusion of this process,
 *		the "inh" flag is set in all and only those RTEs that are append
 *		relation parents.
 */
void
expand_inherited_tables(PlannerInfo *root)
{
	Index		nrtes;
	Index		rti;
	ListCell   *rl;

	/*
	 * expand_inherited_rtentry may add RTEs to parse->rtable; there is no
	 * need to scan them since they can't have inh=true.  So just scan as far
	 * as the original end of the rtable list.
	 */
	nrtes = list_length(root->parse->rtable);
	rl = list_head(root->parse->rtable);
	for (rti = 1; rti <= nrtes; rti++)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(rl);

		expand_inherited_rtentry(root, rte, rti);
		rl = lnext(rl);
	}
}

/*
 * expand_inherited_rtentry
 *		Check whether a rangetable entry represents an inheritance set.
 *		If so, add entries for all the child tables to the query's
 *		rangetable, and build AppendRelInfo nodes for all the child tables
 *		and add them to root->append_rel_list.  If not, clear the entry's
 *		"inh" flag to prevent later code from looking for AppendRelInfos.
 *
 * Note that the original RTE is considered to represent the whole
 * inheritance set.  The first of the generated RTEs is an RTE for the same
 * table, but with inh = false, to represent the parent table in its role
 * as a simple member of the inheritance set.
 *
 * A childless table is never considered to be an inheritance set; therefore
 * a parent RTE must always have at least two associated AppendRelInfos.
 */
static void
expand_inherited_rtentry(PlannerInfo *root, RangeTblEntry *rte, Index rti)
{
	Query	   *parse = root->parse;
	Oid			parentOID;
	PlanRowMark *oldrc;
	Relation	oldrelation;
	LOCKMODE	lockmode;
	List	   *inhOIDs;
	List	   *appinfos;
	ListCell   *l;
	bool		parent_is_partitioned;
	Relids		child_relids = NULL;

	/* Does RT entry allow inheritance? */
	if (!rte->inh)
		return;
	/* Ignore any already-expanded UNION ALL nodes */
	if (rte->rtekind != RTE_RELATION)
	{
		Assert(rte->rtekind == RTE_SUBQUERY);
		return;
	}
	/* Fast path for common case of childless table */
	parentOID = rte->relid;
	if (!has_subclass(parentOID))
	{
		/* Clear flag before returning */
		rte->inh = false;
		return;
	}

	parent_is_partitioned = rel_is_partitioned(parentOID);

	/*
	 * The rewriter should already have obtained an appropriate lock on each
	 * relation named in the query.  However, for each child relation we add
	 * to the query, we must obtain an appropriate lock, because this will be
	 * the first use of those relations in the parse/rewrite/plan pipeline.
	 *
	 * If the parent relation is the query's result relation, then we need
	 * RowExclusiveLock.  Otherwise, if it's accessed FOR UPDATE/SHARE, we
	 * need ExclusiveLock; otherwise AccessShareLock.  We can't just grab
	 * AccessShareLock because then the executor would be trying to upgrade
	 * the lock, leading to possible deadlocks.  (This code should match the
	 * parser and rewriter.)
	 */
	oldrc = get_plan_rowmark(root->rowMarks, rti);
	if (rti == parse->resultRelation)
		lockmode = RowExclusiveLock;
	else if (oldrc)
	{
		/*
		 * Greenplum specific behavior:
		 * The implementation of select statement with locking clause
		 * (for update | no key update | share | key share) in postgres
		 * is to hold RowShareLock on tables during parsing stage, and
		 * generate a LockRows plan node for executor to lock the tuples.
		 * It is not easy to lock tuples in Greenplum database, since
		 * tuples may be fetched through motion nodes.
		 *
		 * But when Global Deadlock Detector is enabled, and the select
		 * statement with locking clause contains only one table, we are
		 * sure that there are no motions. For such simple cases, we could
		 * make the behavior just the same as Postgres.
		 */
		lockmode = oldrc->canOptSelectLockingClause ? RowShareLock : ExclusiveLock;
	}
	else
		lockmode = AccessShareLock;

	/* Scan for all members of inheritance set, acquire needed locks */
	inhOIDs = find_all_inheritors(parentOID, lockmode, NULL);

	/*
	 * Check that there's at least one descendant, else treat as no-child
	 * case.  This could happen despite above has_subclass() check, if table
	 * once had a child but no longer does.
	 */
	if (list_length(inhOIDs) < 2)
	{
		/* Clear flag before returning */
		rte->inh = false;
		return;
	}

	/*
	 * If parent relation is selected FOR UPDATE/SHARE, we need to mark its
	 * PlanRowMark as isParent = true, and generate a new PlanRowMark for each
	 * child.
	 */
	if (oldrc)
		oldrc->isParent = true;

	/*
	 * Must open the parent relation to examine its tupdesc.  We need not lock
	 * it; we assume the rewriter already did.
	 */
	oldrelation = heap_open(parentOID, NoLock);

	/* Scan the inheritance set and expand it */
	appinfos = NIL;
	foreach(l, inhOIDs)
	{
		Oid			childOID = lfirst_oid(l);
		Relation	newrelation;
		RangeTblEntry *childrte;
		Index		childRTindex;
		AppendRelInfo *appinfo;

		/* Open rel if needed; we already have required locks */
		if (childOID != parentOID)
			newrelation = heap_open(childOID, NoLock);
		else
			newrelation = oldrelation;

		/*
		 * It is possible that the parent table has children that are temp
		 * tables of other backends.  We cannot safely access such tables
		 * (because of buffering issues), and the best thing to do seems to be
		 * to silently ignore them.
		 */
		if (childOID != parentOID && RELATION_IS_OTHER_TEMP(newrelation))
		{
			heap_close(newrelation, lockmode);
			continue;
		}

		/*
		 * show root and leaf partitions
		 */
		if (parent_is_partitioned && !rel_is_leaf_partition(childOID))
		{
			if (childOID != parentOID)
				heap_close(newrelation, lockmode);
			continue;
		}

		/*
		 * Build an RTE for the child, and attach to query's rangetable list.
		 * We copy most fields of the parent's RTE, but replace relation OID
		 * and relkind, and set inh = false.  Also, set requiredPerms to zero
		 * since all required permissions checks are done on the original RTE.
		 */
		childrte = copyObject(rte);
		childrte->relid = childOID;
		childrte->relkind = newrelation->rd_rel->relkind;
		childrte->inh = false;
		childrte->requiredPerms = 0;
		parse->rtable = lappend(parse->rtable, childrte);
		childRTindex = list_length(parse->rtable);

		child_relids = bms_add_member(child_relids, childRTindex);

		/*
		 * Build an AppendRelInfo for this parent and child.
		 */
		appinfo = makeNode(AppendRelInfo);
		appinfo->parent_relid = rti;
		appinfo->child_relid = childRTindex;
		appinfo->parent_reltype = oldrelation->rd_rel->reltype;
		appinfo->child_reltype = newrelation->rd_rel->reltype;
		make_inh_translation_list(oldrelation, newrelation, childRTindex,
								  &appinfo->translated_vars);
		appinfo->parent_reloid = parentOID;
		appinfos = lappend(appinfos, appinfo);

		/*
		 * Translate the column permissions bitmaps to the child's attnums (we
		 * have to build the translated_vars list before we can do this). But
		 * if this is the parent table, leave copyObject's result alone.
		 *
		 * Note: we need to do this even though the executor won't run any
		 * permissions checks on the child RTE.  The insertedCols/updatedCols
		 * bitmaps may be examined for trigger-firing purposes.
		 */
		if (childOID != parentOID)
		{
			childrte->selectedCols = translate_col_privs(rte->selectedCols,
												   appinfo->translated_vars);
			childrte->insertedCols = translate_col_privs(rte->insertedCols,
												   appinfo->translated_vars);
			childrte->updatedCols = translate_col_privs(rte->updatedCols,
												   appinfo->translated_vars);
		}

		/*
		 * Build a PlanRowMark if parent is marked FOR UPDATE/SHARE.
		 */
		if (oldrc)
		{
			PlanRowMark *newrc = makeNode(PlanRowMark);

			newrc->rti = childRTindex;
			newrc->prti = rti;
			newrc->rowmarkId = oldrc->rowmarkId;
			/* Reselect rowmark type, because relkind might not match parent */
			newrc->markType = select_rowmark_type(childrte, oldrc->strength);
			newrc->allMarkTypes = (1 << newrc->markType);
			newrc->strength = oldrc->strength;
			newrc->waitPolicy = oldrc->waitPolicy;
			newrc->isParent = false;

			/* Include child's rowmark type in parent's allMarkTypes */
			oldrc->allMarkTypes |= newrc->allMarkTypes;

			root->rowMarks = lappend(root->rowMarks, newrc);
		}

		/* Close child relations, but keep locks */
		if (childOID != parentOID)
			heap_close(newrelation, rel_needs_long_lock(childOID) ? NoLock: lockmode);
	}

	heap_close(oldrelation, NoLock);

	if (parent_is_partitioned)
	{
		DynamicScanInfo *dsinfo;

		dsinfo = palloc(sizeof(DynamicScanInfo));
		dsinfo->parentOid = parentOID;
		dsinfo->rtindex = rti;
		dsinfo->hasSelector = false;

		dsinfo->children = child_relids;

		dsinfo->partKeyAttnos = rel_partition_key_attrs(parentOID);

		root->dynamicScans = lappend(root->dynamicScans, dsinfo);
		dsinfo->dynamicScanId = list_length(root->dynamicScans);
	}

	/*
	 * If all the children were temp tables, pretend it's a non-inheritance
	 * situation.  The duplicate RTE we added for the parent table is
	 * harmless, so we don't bother to get rid of it.
	 */
	if (list_length(appinfos) < 1)
	{
		/* Clear flag before returning */
		rte->inh = false;
		return;
	}

	/* Otherwise, OK to add to root->append_rel_list */
	root->append_rel_list = list_concat(root->append_rel_list, appinfos);
}

/*
 * make_inh_translation_list
 *	  Build the list of translations from parent Vars to child Vars for
 *	  an inheritance child.
 *
 * For paranoia's sake, we match type/collation as well as attribute name.
 */
static void
make_inh_translation_list(Relation oldrelation, Relation newrelation,
						  Index newvarno,
						  List **translated_vars)
{
	List	   *vars = NIL;
	TupleDesc	old_tupdesc = RelationGetDescr(oldrelation);
	TupleDesc	new_tupdesc = RelationGetDescr(newrelation);
	int			oldnatts = old_tupdesc->natts;
	int			newnatts = new_tupdesc->natts;
	int			old_attno;

	for (old_attno = 0; old_attno < oldnatts; old_attno++)
	{
		Form_pg_attribute att;
		char	   *attname;
		Oid			atttypid;
		int32		atttypmod;
		Oid			attcollation;
		int			new_attno;

		att = old_tupdesc->attrs[old_attno];
		if (att->attisdropped)
		{
			/* Just put NULL into this list entry */
			vars = lappend(vars, NULL);
			continue;
		}
		attname = NameStr(att->attname);
		atttypid = att->atttypid;
		atttypmod = att->atttypmod;
		attcollation = att->attcollation;

		/*
		 * When we are generating the "translation list" for the parent table
		 * of an inheritance set, no need to search for matches.
		 */
		if (oldrelation == newrelation)
		{
			vars = lappend(vars, makeVar(newvarno,
										 (AttrNumber) (old_attno + 1),
										 atttypid,
										 atttypmod,
										 attcollation,
										 0));
			continue;
		}

		/*
		 * Otherwise we have to search for the matching column by name.
		 * There's no guarantee it'll have the same column position, because
		 * of cases like ALTER TABLE ADD COLUMN and multiple inheritance.
		 * However, in simple cases it will be the same column number, so try
		 * that before we go groveling through all the columns.
		 *
		 * Note: the test for (att = ...) != NULL cannot fail, it's just a
		 * notational device to include the assignment into the if-clause.
		 */
		if (old_attno < newnatts &&
			(att = new_tupdesc->attrs[old_attno]) != NULL &&
			!att->attisdropped &&
			strcmp(attname, NameStr(att->attname)) == 0)
			new_attno = old_attno;
		else
		{
			for (new_attno = 0; new_attno < newnatts; new_attno++)
			{
				att = new_tupdesc->attrs[new_attno];
				if (!att->attisdropped &&
					strcmp(attname, NameStr(att->attname)) == 0)
					break;
			}
			if (new_attno >= newnatts)
				elog(ERROR, "could not find inherited attribute \"%s\" of relation \"%s\"",
					 attname, RelationGetRelationName(newrelation));
		}

		/* Found it, check type and collation match */
		if (atttypid != att->atttypid || atttypmod != att->atttypmod)
			elog(ERROR, "attribute \"%s\" of relation \"%s\" does not match parent's type",
				 attname, RelationGetRelationName(newrelation));
		if (attcollation != att->attcollation)
			elog(ERROR, "attribute \"%s\" of relation \"%s\" does not match parent's collation",
				 attname, RelationGetRelationName(newrelation));

		vars = lappend(vars, makeVar(newvarno,
									 (AttrNumber) (new_attno + 1),
									 atttypid,
									 atttypmod,
									 attcollation,
									 0));
	}

	*translated_vars = vars;
}

/*
 * translate_col_privs
 *	  Translate a bitmapset representing per-column privileges from the
 *	  parent rel's attribute numbering to the child's.
 *
 * The only surprise here is that we don't translate a parent whole-row
 * reference into a child whole-row reference.  That would mean requiring
 * permissions on all child columns, which is overly strict, since the
 * query is really only going to reference the inherited columns.  Instead
 * we set the per-column bits for all inherited columns.
 */
static Bitmapset *
translate_col_privs(const Bitmapset *parent_privs,
					List *translated_vars)
{
	Bitmapset  *child_privs = NULL;
	bool		whole_row;
	int			attno;
	ListCell   *lc;

	/* System attributes have the same numbers in all tables */
	for (attno = FirstLowInvalidHeapAttributeNumber + 1; attno < 0; attno++)
	{
		if (bms_is_member(attno - FirstLowInvalidHeapAttributeNumber,
						  parent_privs))
			child_privs = bms_add_member(child_privs,
								 attno - FirstLowInvalidHeapAttributeNumber);
	}

	/* Check if parent has whole-row reference */
	whole_row = bms_is_member(InvalidAttrNumber - FirstLowInvalidHeapAttributeNumber,
							  parent_privs);

	/* And now translate the regular user attributes, using the vars list */
	attno = InvalidAttrNumber;
	foreach(lc, translated_vars)
	{
		Var		   *var = (Var *) lfirst(lc);

		attno++;
		if (var == NULL)		/* ignore dropped columns */
			continue;
		Assert(IsA(var, Var));
		if (whole_row ||
			bms_is_member(attno - FirstLowInvalidHeapAttributeNumber,
						  parent_privs))
			child_privs = bms_add_member(child_privs,
						 var->varattno - FirstLowInvalidHeapAttributeNumber);
	}

	return child_privs;
}

/*
 * adjust_appendrel_attrs
 *	  Copy the specified query or expression and translate Vars referring
 *	  to the parent rel of the specified AppendRelInfo to refer to the
 *	  child rel instead.  We also update rtindexes appearing outside Vars,
 *	  such as resultRelation and jointree relids.
 *
 * Note: this is applied after conversion of sublinks to subplans in the
 * query jointree, but there may still be sublinks in the security barrier
 * quals of RTEs, so we do need to cope with recursion into sub-queries.
 *
 * Note: this is not hugely different from what pullup_replace_vars() does;
 * maybe we should try to fold the two routines together.
 */
Node *
adjust_appendrel_attrs(PlannerInfo *root, Node *node, AppendRelInfo *appinfo)
{
	Node	   *result;
	adjust_appendrel_attrs_context context;

	context.root = root;
	context.appinfo = appinfo;
	context.sublevels_up = 0;

	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, go straight to query_tree_walker to make sure that
	 * sublevels_up doesn't get incremented prematurely.
	 */
	if (node && IsA(node, Query))
	{
		Query	   *newnode;

		newnode = query_tree_mutator((Query *) node,
									 adjust_appendrel_attrs_mutator,
									 (void *) &context,
									 QTW_IGNORE_RC_SUBQUERIES);
		if (newnode->resultRelation == appinfo->parent_relid)
		{
			newnode->resultRelation = appinfo->child_relid;
			/* Fix tlist resnos too, if it's inherited UPDATE */
			if (newnode->commandType == CMD_UPDATE)
				newnode->targetList =
					adjust_inherited_tlist(newnode->targetList,
										   appinfo);
		}
		result = (Node *) newnode;
	}
	else
		result = adjust_appendrel_attrs_mutator(node, &context);

	return result;
}

/**
 * Mutator's function is to modify nodes so that they may be applicable
 * for a child partition.
 */
static Node *
adjust_appendrel_attrs_mutator(Node *node,
							   adjust_appendrel_attrs_context *context)
{
	AppendRelInfo *appinfo = context->appinfo;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) copyObject(node);

		if (var->varlevelsup == context->sublevels_up &&
			var->varno == appinfo->parent_relid)
		{
			var->varno = appinfo->child_relid;
			var->varnoold = appinfo->child_relid;
			if (var->varattno > 0)
			{
				Node	   *newnode;

				if (var->varattno > list_length(appinfo->translated_vars))
					elog(ERROR, "attribute %d of relation \"%s\" does not exist",
						 var->varattno, get_rel_name(appinfo->parent_reloid));
				newnode = copyObject(list_nth(appinfo->translated_vars,
											  var->varattno - 1));
				if (newnode == NULL)
					elog(ERROR, "attribute %d of relation \"%s\" does not exist",
						 var->varattno, get_rel_name(appinfo->parent_reloid));
				((Var *) newnode)->varlevelsup += context->sublevels_up;
				return newnode;
			}
			else if (var->varattno == 0)
			{
				/*
				 * Whole-row Var: if we are dealing with named rowtypes, we
				 * can use a whole-row Var for the child table plus a coercion
				 * step to convert the tuple layout to the parent's rowtype.
				 * Otherwise we have to generate a RowExpr.
				 */
				if (OidIsValid(appinfo->child_reltype))
				{
					Assert(var->vartype == appinfo->parent_reltype);
					if (appinfo->parent_reltype != appinfo->child_reltype)
					{
						ConvertRowtypeExpr *r = makeNode(ConvertRowtypeExpr);

						r->arg = (Expr *) var;
						r->resulttype = appinfo->parent_reltype;
						r->convertformat = COERCE_IMPLICIT_CAST;
						r->location = -1;
						/* Make sure the Var node has the right type ID, too */
						var->vartype = appinfo->child_reltype;
						return (Node *) r;
					}
				}
				else
				{
					/*
					 * Build a RowExpr containing the translated variables.
					 *
					 * In practice var->vartype will always be RECORDOID here,
					 * so we need to come up with some suitable column names.
					 * We use the parent RTE's column names.
					 *
					 * Note: we can't get here for inheritance cases, so there
					 * is no need to worry that translated_vars might contain
					 * some dummy NULLs.
					 */
					RowExpr    *rowexpr;
					List	   *fields;
					RangeTblEntry *rte;
					ListCell   *lc;

					rte = rt_fetch(appinfo->parent_relid,
								   context->root->parse->rtable);
					fields = (List *) copyObject(appinfo->translated_vars);
					foreach(lc, fields)
					{
						Var		   *field = (Var *) lfirst(lc);

						field->varlevelsup += context->sublevels_up;
					}
					rowexpr = makeNode(RowExpr);
					rowexpr->args = fields;
					rowexpr->row_typeid = var->vartype;
					rowexpr->row_format = COERCE_IMPLICIT_CAST;
					rowexpr->colnames = copyObject(rte->eref->colnames);
					rowexpr->location = -1;

					return (Node *) rowexpr;
				}
			}
			/* system attributes don't need any other translation */
		}
		return (Node *) var;
	}
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) copyObject(node);

		if (context->sublevels_up == 0 &&
			cexpr->cvarno == appinfo->parent_relid)
			cexpr->cvarno = appinfo->child_relid;
		return (Node *) cexpr;
	}
	if (IsA(node, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) copyObject(node);

		if (context->sublevels_up == 0 &&
			rtr->rtindex == appinfo->parent_relid)
			rtr->rtindex = appinfo->child_relid;
		return (Node *) rtr;
	}
	if (IsA(node, JoinExpr))
	{
		/* Copy the JoinExpr node with correct mutation of subnodes */
		JoinExpr   *j;

		j = (JoinExpr *) expression_tree_mutator(node,
											  adjust_appendrel_attrs_mutator,
												 (void *) context);
		/* now fix JoinExpr's rtindex (probably never happens) */
		if (context->sublevels_up == 0 &&
			j->rtindex == appinfo->parent_relid)
			j->rtindex = appinfo->child_relid;
		return (Node *) j;
	}
	if (IsA(node, PlaceHolderVar))
	{
		/* Copy the PlaceHolderVar node with correct mutation of subnodes */
		PlaceHolderVar *phv;

		phv = (PlaceHolderVar *) expression_tree_mutator(node,
											  adjust_appendrel_attrs_mutator,
														 (void *) context);
		/* now fix PlaceHolderVar's relid sets */
		if (phv->phlevelsup == context->sublevels_up)
			phv->phrels = adjust_relid_set(phv->phrels,
										   appinfo->parent_relid,
										   appinfo->child_relid);
		return (Node *) phv;
	}
	/* Shouldn't need to handle planner auxiliary nodes here */
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, AppendRelInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	/*
	 * We have to process RestrictInfo nodes specially.  (Note: although
	 * set_append_rel_pathlist will hide RestrictInfos in the parent's
	 * baserestrictinfo list from us, it doesn't hide those in joininfo.)
	 */
	if (IsA(node, RestrictInfo))
	{
		RestrictInfo *oldinfo = (RestrictInfo *) node;
		RestrictInfo *newinfo = makeNode(RestrictInfo);

		/* Copy all flat-copiable fields */
		memcpy(newinfo, oldinfo, sizeof(RestrictInfo));

		/* Recursively fix the clause itself */
		newinfo->clause = (Expr *)
				adjust_appendrel_attrs_mutator((Node *) oldinfo->clause, context);

		/* and the modified version, if an OR clause */
		newinfo->orclause = (Expr *)
				adjust_appendrel_attrs_mutator((Node *) oldinfo->orclause, context);

		/* adjust relid sets too */
		newinfo->clause_relids = adjust_relid_set(oldinfo->clause_relids,
												  appinfo->parent_relid,
												  appinfo->child_relid);
		newinfo->required_relids = adjust_relid_set(oldinfo->required_relids,
													appinfo->parent_relid,
													appinfo->child_relid);
		newinfo->outer_relids = adjust_relid_set(oldinfo->outer_relids,
												 appinfo->parent_relid,
												 appinfo->child_relid);
		newinfo->nullable_relids = adjust_relid_set(oldinfo->nullable_relids,
													appinfo->parent_relid,
													appinfo->child_relid);
		newinfo->left_relids = adjust_relid_set(oldinfo->left_relids,
												appinfo->parent_relid,
												appinfo->child_relid);
		newinfo->right_relids = adjust_relid_set(oldinfo->right_relids,
												 appinfo->parent_relid,
												 appinfo->child_relid);

		/*
		 * Reset cached derivative fields, since these might need to have
		 * different values when considering the child relation.  Note we
		 * don't reset left_ec/right_ec: each child variable is implicitly
		 * equivalent to its parent, so still a member of the same EC if any.
		 */
		newinfo->eval_cost.startup = -1;
		newinfo->norm_selec = -1;
		newinfo->outer_selec = -1;
		newinfo->left_em = NULL;
		newinfo->right_em = NULL;
		newinfo->scansel_cache = NIL;
		newinfo->left_bucketsize = -1;
		newinfo->right_bucketsize = -1;

		return (Node *) newinfo;
	}

	if (IsA(node, Query))
	{
		/*
		 * Recurse into sublink subqueries. This should only be possible in
		 * security barrier quals of top-level RTEs. All other sublinks should
		 * have already been converted to subplans during expression
		 * preprocessing, but this doesn't happen for security barrier quals,
		 * since they are destined to become quals of a subquery RTE, which
		 * will be recursively planned, and so should not be preprocessed at
		 * this stage.
		 *
		 * We don't explicitly Assert() for securityQuals here simply because
		 * it's not trivial to do so.
		 */
		Query	   *newnode;

		context->sublevels_up++;
		newnode = query_tree_mutator((Query *) node,
									 adjust_appendrel_attrs_mutator,
									 (void *) context, 0);
		context->sublevels_up--;
		return (Node *) newnode;
	}

	node = expression_tree_mutator(node, adjust_appendrel_attrs_mutator,
								   (void *) context);

	/*
	 * In GPDB, if you have two SubPlans referring to the same initplan, we
	 * require two separate copies of the subplan, one for each SubPlan
	 * reference. That's because even if a plan is otherwise the same, we
	 * may want to later apply different flow to different SubPlans
	 * referring it. Any subplan that is left unused, because we created
	 * the new copy here, will be removed by remove_unused_subplans().
	 */
	if (IsA(node, SubPlan))
	{
		SubPlan *sp = (SubPlan *) node;

		if (!sp->is_initplan)
		{
			PlannerInfo *root = context->root;
			Plan *newsubplan = (Plan *) copyObject(planner_subplan_get_plan(root, sp));
			PlannerInfo *newsubroot = makeNode(PlannerInfo);

			memcpy(newsubroot, planner_subplan_get_root(root, sp), sizeof(PlannerInfo));

			/*
			 * Add the subplan and its subroot to the global lists.
			 */
			root->glob->subplans = lappend(root->glob->subplans, newsubplan);
			root->glob->subroots = lappend(root->glob->subroots, newsubroot);

			/*
			 * expression_tree_mutator made a copy of the SubPlan already, so
			 * we can modify it directly.
			 */
			sp->plan_id = list_length(root->glob->subplans);
		}
	}

	return node;
}

/*
 * Substitute newrelid for oldrelid in a Relid set
 */
static Relids
adjust_relid_set(Relids relids, Index oldrelid, Index newrelid)
{
	if (bms_is_member(oldrelid, relids))
	{
		/* Ensure we have a modifiable copy */
		relids = bms_copy(relids);
		/* Remove old, add new */
		relids = bms_del_member(relids, oldrelid);
		relids = bms_add_member(relids, newrelid);
	}
	return relids;
}

/*
 * Adjust the targetlist entries of an inherited UPDATE operation
 *
 * The expressions have already been fixed, but we have to make sure that
 * the target resnos match the child table (they may not, in the case of
 * a column that was added after-the-fact by ALTER TABLE).  In some cases
 * this can force us to re-order the tlist to preserve resno ordering.
 * (We do all this work in special cases so that preptlist.c is fast for
 * the typical case.)
 *
 * The given tlist has already been through expression_tree_mutator;
 * therefore the TargetEntry nodes are fresh copies that it's okay to
 * scribble on.
 *
 * Note that this is not needed for INSERT because INSERT isn't inheritable.
 */
static List *
adjust_inherited_tlist(List *tlist, AppendRelInfo *context)
{
	bool		changed_it = false;
	ListCell   *tl;
	List	   *new_tlist;
	bool		more;
	int			attrno;

	/* This should only happen for an inheritance case, not UNION ALL */
	Assert(OidIsValid(context->parent_reloid));

	/* Scan tlist and update resnos to match attnums of child rel */
	foreach(tl, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);
		Var		   *childvar;

		if (tle->resjunk)
			continue;			/* ignore junk items */

		/* Look up the translation of this column: it must be a Var */
		if (tle->resno <= 0 ||
			tle->resno > list_length(context->translated_vars))
			elog(ERROR, "attribute %d of relation \"%s\" does not exist",
				 tle->resno, get_rel_name(context->parent_reloid));
		childvar = (Var *) list_nth(context->translated_vars, tle->resno - 1);
		if (childvar == NULL || !IsA(childvar, Var))
			elog(ERROR, "attribute %d of relation \"%s\" does not exist",
				 tle->resno, get_rel_name(context->parent_reloid));

		if (tle->resno != childvar->varattno)
		{
			tle->resno = childvar->varattno;
			changed_it = true;
		}
	}

	/*
	 * If we changed anything, re-sort the tlist by resno, and make sure
	 * resjunk entries have resnos above the last real resno.  The sort
	 * algorithm is a bit stupid, but for such a seldom-taken path, small is
	 * probably better than fast.
	 */
	if (!changed_it)
		return tlist;

	new_tlist = NIL;
	more = true;
	for (attrno = 1; more; attrno++)
	{
		more = false;
		foreach(tl, tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tl);

			if (tle->resjunk)
				continue;		/* ignore junk items */

			if (tle->resno == attrno)
				new_tlist = lappend(new_tlist, tle);
			else if (tle->resno > attrno)
				more = true;
		}
	}

	foreach(tl, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);

		if (!tle->resjunk)
			continue;			/* here, ignore non-junk items */

		tle->resno = attrno;
		new_tlist = lappend(new_tlist, tle);
		attrno++;
	}

	return new_tlist;
}

/*
 * adjust_appendrel_attrs_multilevel
 *	  Apply Var translations from a toplevel appendrel parent down to a child.
 *
 * In some cases we need to translate expressions referencing a baserel
 * to reference an appendrel child that's multiple levels removed from it.
 */
Node *
adjust_appendrel_attrs_multilevel(PlannerInfo *root, Node *node,
								  RelOptInfo *child_rel)
{
	AppendRelInfo *appinfo = find_childrel_appendrelinfo(root, child_rel);
	RelOptInfo *parent_rel = find_base_rel(root, appinfo->parent_relid);

	/* If parent is also a child, first recurse to apply its translations */
	if (parent_rel->reloptkind == RELOPT_OTHER_MEMBER_REL)
		node = adjust_appendrel_attrs_multilevel(root, node, parent_rel);
	else
		Assert(parent_rel->reloptkind == RELOPT_BASEREL);
	/* Now translate for this child */
	return adjust_appendrel_attrs(root, node, appinfo);
}
