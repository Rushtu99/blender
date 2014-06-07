/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2013 Blender Foundation.
 * All rights reserved.
 *
 * Original Author: Joshua Leung
 * Contributor(s): None Yet
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 * Evaluation engine entrypoints for Depsgraph Engine
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

extern "C" {
#include "BLI_blenlib.h"
#include "BLI_string.h"
#include "BLI_threads.h"
#include "BLI_utildefines.h"

#include "DNA_anim_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_action.h"
#include "BKE_animsys.h"
#include "BKE_constraint.h"
#include "BKE_DerivedMesh.h"
#include "BKE_main.h"
#include "BKE_object.h"
#include "BKE_scene.h"
#include "BKE_global.h" /* XXX only for debug value, remove eventually */

#include "DEG_depsgraph.h"

#include "RNA_access.h"
#include "RNA_types.h"
} /* extern "C" */

#include "atomic_ops.h"

#include "depsgraph.h"
#include "depsnode.h"
#include "depsnode_component.h"
#include "depsnode_operation.h"
#include "depsgraph_types.h"
#include "depsgraph_eval.h"
#include "depsgraph_queue.h"
#include "depsgraph_intern.h"
#include "depsgraph_debug.h"

eDEG_EvalMode DEG_get_eval_mode(void)
{
	switch (G.debug_value) {
		case DEG_EVAL_MODE_NEW: return DEG_EVAL_MODE_NEW;
		case DEG_EVAL_MODE_SIM: return DEG_EVAL_MODE_SIM;
		default: return DEG_EVAL_MODE_OLD;
	}
}

void DEG_set_eval_mode(eDEG_EvalMode mode)
{
	switch (mode) {
		case DEG_EVAL_MODE_NEW: G.debug_value = DEG_EVAL_MODE_NEW;
		case DEG_EVAL_MODE_SIM: G.debug_value = DEG_EVAL_MODE_SIM;
		default: G.debug_value = DEG_EVAL_MODE_OLD;
	}
}

/* *************************************************** */
/* Multi-Threaded Evaluation Internals */

static SpinLock threaded_update_lock;

/* Initialise threading lock - called during application startup */
void DEG_threaded_init(void)
{
	DepsgraphTaskScheduler::init();
	BLI_spin_init(&threaded_update_lock);
}

/* Free threading lock - called during application shutdown */
void DEG_threaded_exit(void)
{
	DepsgraphDebug::stats_free();
	
	DepsgraphTaskScheduler::exit();
	BLI_spin_end(&threaded_update_lock);
}


/* *************************************************** */
/* Evaluation Entrypoints */

static void calculate_pending_parents(Depsgraph *graph)
{
	for (Depsgraph::OperationNodes::const_iterator it_op = graph->operations.begin(); it_op != graph->operations.end(); ++it_op) {
		OperationDepsNode *node = *it_op;
		
		node->num_links_pending = 0;
		node->scheduled = false;
		
		/* count number of inputs that need updates */
		if (node->flag & DEPSOP_FLAG_NEEDS_UPDATE) {
			for (OperationDepsNode::Relations::const_iterator it_rel = node->inlinks.begin(); it_rel != node->inlinks.end(); ++it_rel) {
				DepsRelation *rel = *it_rel;
				if (rel->from->flag & DEPSOP_FLAG_NEEDS_UPDATE)
					++node->num_links_pending;
			}
		}
	}
}

static void calculate_eval_priority(OperationDepsNode *node)
{
	if (node->done)
		return;
	node->done = 1;
	
	if (node->flag & DEPSOP_FLAG_NEEDS_UPDATE) {
		/* XXX standard cost of a node, could be estimated somewhat later on */
		const float cost = 1.0f;
		/* NOOP nodes have no cost */
		node->eval_priority = node->is_noop() ? cost : 0.0f;
		
		for (OperationDepsNode::Relations::const_iterator it = node->outlinks.begin(); it != node->outlinks.end(); ++it) {
			DepsRelation *rel = *it;
			calculate_eval_priority(rel->to);
			
			node->eval_priority += rel->to->eval_priority;
		}
	}
	else
		node->eval_priority = 0.0f;
}

static void schedule_graph(DepsgraphTaskPool &pool, Depsgraph *graph, eEvaluationContextType context_type)
{
	BLI_spin_lock(&threaded_update_lock);
	for (Depsgraph::OperationNodes::const_iterator it = graph->operations.begin(); it != graph->operations.end(); ++it) {
		OperationDepsNode *node = *it;
		
		if ((node->flag & DEPSOP_FLAG_NEEDS_UPDATE) && node->num_links_pending == 0) {
			pool.push(graph, node, context_type);
			node->scheduled = true;
		}
	}
	BLI_spin_unlock(&threaded_update_lock);
}

void deg_schedule_children(DepsgraphTaskPool &pool, Depsgraph *graph, eEvaluationContextType context_type, OperationDepsNode *node)
{
	for (OperationDepsNode::Relations::const_iterator it = node->outlinks.begin(); it != node->outlinks.end(); ++it) {
		DepsRelation *rel = *it;
		OperationDepsNode *child = rel->to;
		
		if (child->flag & DEPSOP_FLAG_NEEDS_UPDATE) {
			BLI_assert(child->num_links_pending > 0);
			atomic_sub_uint32(&child->num_links_pending, 1);
			
			if (child->num_links_pending == 0) {
				BLI_spin_lock(&threaded_update_lock);
				bool need_schedule = !child->scheduled;
				child->scheduled = true;
				BLI_spin_unlock(&threaded_update_lock);
				
				if (need_schedule)
					pool.push(graph, child, context_type);
			}
		}
	}
}


/* Evaluate all nodes tagged for updating 
 * ! This is usually done as part of main loop, but may also be 
 *   called from frame-change update
 */
void DEG_evaluate_on_refresh(Depsgraph *graph, eEvaluationContextType context_type)
{
	/* generate base evaluation context, upon which all the others are derived... */
	// TODO: this needs both main and scene access...
	
	/* XXX could use a separate pool for each eval context */
	static DepsgraphTaskPool task_pool = DepsgraphTaskPool();
	
	/* recursively push updates out to all nodes dependent on this, 
	 * until all affected are tagged and/or scheduled up for eval
	 */
	DEG_graph_flush_updates(graph);
	
	calculate_pending_parents(graph);
	
	/* clear tags */
	for (Depsgraph::OperationNodes::const_iterator it = graph->operations.begin(); it != graph->operations.end(); ++it) {
		OperationDepsNode *node = *it;
		node->done = 0;
	}
	/* calculate priority for operation nodes */
	for (Depsgraph::OperationNodes::const_iterator it = graph->operations.begin(); it != graph->operations.end(); ++it) {
		OperationDepsNode *node = *it;
		calculate_eval_priority(node);
	}
	
	DepsgraphDebug::eval_begin(context_type);
	
	schedule_graph(task_pool, graph, context_type);
	
	task_pool.wait();
	
	DepsgraphDebug::eval_end(context_type);
	
	/* clear any uncleared tags - just in case */
	DEG_graph_clear_tags(graph);
}

/* Frame-change happened for root scene that graph belongs to */
void DEG_evaluate_on_framechange(Depsgraph *graph, eEvaluationContextType context_type, double ctime)
{
	TimeSourceDepsNode *tsrc;
	
	/* update time on primary timesource */
	tsrc = (TimeSourceDepsNode *)graph->find_node(NULL, "", DEPSNODE_TYPE_TIMESOURCE, "");
	tsrc->cfra = ctime;
	
#if 0 /* XXX TODO */
	graph->tag_update(tsrc);
#endif
	
	/* perform recalculation updates */
	DEG_evaluate_on_refresh(graph, context_type);
}

/* *************************************************** */
