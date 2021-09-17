/*
 * Copyright 2004-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <sys/param.h>

#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/xml_internal.h>

#include <glib.h>

#include <crm/pengine/status.h>
#include <pacemaker-internal.h>
#include "libpacemaker_private.h"

enum remote_connection_state {
    remote_state_unknown = 0,
    remote_state_alive = 1,
    remote_state_resting = 2,
    remote_state_failed = 3,
    remote_state_stopped = 4
};

static const char *
state2text(enum remote_connection_state state)
{
    switch (state) {
        case remote_state_unknown:
            return "unknown";
        case remote_state_alive:
            return "alive";
        case remote_state_resting:
            return "resting";
        case remote_state_failed:
            return "failed";
        case remote_state_stopped:
            return "stopped";
    }

    return "impossible";
}

/* We always use pe_order_preserve with these convenience functions to exempt
 * internally generated constraints from the prohibition of user constraints
 * involving remote connection resources.
 *
 * The start ordering additionally uses pe_order_runnable_left so that the
 * specified action is not runnable if the start is not runnable.
 */

static inline void
order_start_then_action(pe_resource_t *lh_rsc, pe_action_t *rh_action,
                        enum pe_ordering extra, pe_working_set_t *data_set)
{
    if ((lh_rsc != NULL) && (rh_action != NULL) && (data_set != NULL)) {
        pcmk__new_ordering(lh_rsc, start_key(lh_rsc), NULL,
                           rh_action->rsc, NULL, rh_action,
                           pe_order_preserve|pe_order_runnable_left|extra,
                           data_set);
    }
}

static inline void
order_action_then_stop(pe_action_t *lh_action, pe_resource_t *rh_rsc,
                       enum pe_ordering extra, pe_working_set_t *data_set)
{
    if ((lh_action != NULL) && (rh_rsc != NULL) && (data_set != NULL)) {
        pcmk__new_ordering(lh_action->rsc, NULL, lh_action,
                           rh_rsc, stop_key(rh_rsc), NULL,
                           pe_order_preserve|extra, data_set);
    }
}

static enum remote_connection_state
get_remote_node_state(pe_node_t *node)
{
    pe_resource_t *remote_rsc = NULL;
    pe_node_t *cluster_node = NULL;

    CRM_ASSERT(node != NULL);

    remote_rsc = node->details->remote_rsc;
    CRM_ASSERT(remote_rsc != NULL);

    cluster_node = pe__current_node(remote_rsc);

    /* If the cluster node the remote connection resource resides on
     * is unclean or went offline, we can't process any operations
     * on that remote node until after it starts elsewhere.
     */
    if ((remote_rsc->next_role == RSC_ROLE_STOPPED)
        || (remote_rsc->allocated_to == NULL)) {

        // The connection resource is not going to run anywhere

        if ((cluster_node != NULL) && cluster_node->details->unclean) {
            /* The remote connection is failed because its resource is on a
             * failed node and can't be recovered elsewhere, so we must fence.
             */
            return remote_state_failed;
        }

        if (!pcmk_is_set(remote_rsc->flags, pe_rsc_failed)) {
            /* Connection resource is cleanly stopped */
            return remote_state_stopped;
        }

        /* Connection resource is failed */

        if ((remote_rsc->next_role == RSC_ROLE_STOPPED)
            && remote_rsc->remote_reconnect_ms
            && node->details->remote_was_fenced
            && !pe__shutdown_requested(node)) {

            /* We won't know whether the connection is recoverable until the
             * reconnect interval expires and we reattempt connection.
             */
            return remote_state_unknown;
        }

        /* The remote connection is in a failed state. If there are any
         * resources known to be active on it (stop) or in an unknown state
         * (probe), we must assume the worst and fence it.
         */
        return remote_state_failed;

    } else if (cluster_node == NULL) {
        /* Connection is recoverable but not currently running anywhere, so see
         * if we can recover it first
         */
        return remote_state_unknown;

    } else if (cluster_node->details->unclean
               || !(cluster_node->details->online)) {
        // Connection is running on a dead node, see if we can recover it first
        return remote_state_resting;

    } else if (pcmk__list_of_multiple(remote_rsc->running_on)
               && (remote_rsc->partial_migration_source != NULL)
               && (remote_rsc->partial_migration_target != NULL)) {
        /* We're in the middle of migrating a connection resource, so wait until
         * after the migration completes before performing any actions.
         */
        return remote_state_resting;

    }
    return remote_state_alive;
}

static int
is_recurring_action(pe_action_t *action)
{
    guint interval_ms;

    if (pcmk__guint_from_hash(action->meta,
                              XML_LRM_ATTR_INTERVAL_MS, 0,
                              &interval_ms) != pcmk_rc_ok) {
        return 0;
    }
    return (interval_ms > 0);
}

/*!
 * \internal
 * \brief Order actions on remote node relative to actions for the connection
 */
static void
apply_remote_ordering(pe_action_t *action, pe_working_set_t *data_set)
{
    pe_resource_t *remote_rsc = NULL;
    enum action_tasks task = text2task(action->task);
    enum remote_connection_state state = get_remote_node_state(action->node);

    enum pe_ordering order_opts = pe_order_none;

    if (action->rsc == NULL) {
        return;
    }

    CRM_ASSERT(pe__is_guest_or_remote_node(action->node));

    remote_rsc = action->node->details->remote_rsc;
    CRM_ASSERT(remote_rsc != NULL);

    crm_trace("Order %s action %s relative to %s%s (state: %s)",
              action->task, action->uuid,
              pcmk_is_set(remote_rsc->flags, pe_rsc_failed)? "failed " : "",
              remote_rsc->id, state2text(state));

    if (pcmk__strcase_any_of(action->task, CRMD_ACTION_MIGRATE,
                             CRMD_ACTION_MIGRATED, NULL)) {
        /* Migration ops map to "no_action", but we need to apply the same
         * ordering as for stop or demote (see get_router_node()).
         */
        task = stop_rsc;
    }

    switch (task) {
        case start_rsc:
        case action_promote:
            order_opts = pe_order_none;

            if (state == remote_state_failed) {
                /* Force recovery, by making this action required */
                pe__set_order_flags(order_opts, pe_order_implies_then);
            }

            /* Ensure connection is up before running this action */
            order_start_then_action(remote_rsc, action, order_opts, data_set);
            break;

        case stop_rsc:
            if (state == remote_state_alive) {
                order_action_then_stop(action, remote_rsc,
                                       pe_order_implies_first, data_set);

            } else if (state == remote_state_failed) {
                /* The resource is active on the node, but since we don't have a
                 * valid connection, the only way to stop the resource is by
                 * fencing the node. There is no need to order the stop relative
                 * to the remote connection, since the stop will become implied
                 * by the fencing.
                 */
                pe_fence_node(data_set, action->node,
                              "resources are active but connection is unrecoverable",
                              FALSE);

            } else if (remote_rsc->next_role == RSC_ROLE_STOPPED) {
                /* State must be remote_state_unknown or remote_state_stopped.
                 * Since the connection is not coming back up in this
                 * transition, stop this resource first.
                 */
                order_action_then_stop(action, remote_rsc,
                                       pe_order_implies_first, data_set);

            } else {
                /* The connection is going to be started somewhere else, so
                 * stop this resource after that completes.
                 */
                order_start_then_action(remote_rsc, action, pe_order_none,
                                        data_set);
            }
            break;

        case action_demote:
            /* Only order this demote relative to the connection start if the
             * connection isn't being torn down. Otherwise, the demote would be
             * blocked because the connection start would not be allowed.
             */
            if ((state == remote_state_resting)
                || (state == remote_state_unknown)) {

                order_start_then_action(remote_rsc, action, pe_order_none,
                                        data_set);
            } /* Otherwise we can rely on the stop ordering */
            break;

        default:
            /* Wait for the connection resource to be up */
            if (is_recurring_action(action)) {
                /* In case we ever get the recovery logic wrong, force
                 * recurring monitors to be restarted, even if just
                 * the connection was re-established
                 */
                order_start_then_action(remote_rsc, action,
                                        pe_order_implies_then, data_set);

            } else {
                pe_node_t *cluster_node = pe__current_node(remote_rsc);

                if ((task == monitor_rsc) && (state == remote_state_failed)) {
                    /* We would only be here if we do not know the state of the
                     * resource on the remote node. Since we have no way to find
                     * out, it is necessary to fence the node.
                     */
                    pe_fence_node(data_set, action->node,
                                  "resources are in unknown state "
                                  "and connection is unrecoverable", FALSE);
                }

                if ((cluster_node != NULL) && (state == remote_state_stopped)) {
                    /* The connection is currently up, but is going down
                     * permanently. Make sure we check services are actually
                     * stopped _before_ we let the connection get closed.
                     */
                    order_action_then_stop(action, remote_rsc,
                                           pe_order_runnable_left, data_set);

                } else {
                    order_start_then_action(remote_rsc, action, pe_order_none,
                                            data_set);
                }
            }
            break;
    }
}

static void
apply_container_ordering(pe_action_t *action, pe_working_set_t *data_set)
{
    /* VMs are also classified as containers for these purposes... in
     * that they both involve a 'thing' running on a real or remote
     * cluster node.
     *
     * This allows us to be smarter about the type and extent of
     * recovery actions required in various scenarios
     */
    pe_resource_t *remote_rsc = NULL;
    pe_resource_t *container = NULL;
    enum action_tasks task = text2task(action->task);

    CRM_ASSERT(action->rsc != NULL);
    CRM_ASSERT(action->node != NULL);
    CRM_ASSERT(pe__is_guest_or_remote_node(action->node));

    remote_rsc = action->node->details->remote_rsc;
    CRM_ASSERT(remote_rsc != NULL);

    container = remote_rsc->container;
    CRM_ASSERT(container != NULL);

    if (pcmk_is_set(container->flags, pe_rsc_failed)) {
        pe_fence_node(data_set, action->node, "container failed", FALSE);
    }

    crm_trace("Order %s action %s relative to %s%s for %s%s",
              action->task, action->uuid,
              pcmk_is_set(remote_rsc->flags, pe_rsc_failed)? "failed " : "",
              remote_rsc->id,
              pcmk_is_set(container->flags, pe_rsc_failed)? "failed " : "",
              container->id);

    if (pcmk__strcase_any_of(action->task, CRMD_ACTION_MIGRATE,
                             CRMD_ACTION_MIGRATED, NULL)) {
        /* Migration ops map to "no_action", but we need to apply the same
         * ordering as for stop or demote (see get_router_node()).
         */
        task = stop_rsc;
    }

    switch (task) {
        case start_rsc:
        case action_promote:
            // Force resource recovery if the container is recovered
            order_start_then_action(container, action, pe_order_implies_then,
                                    data_set);

            // Wait for the connection resource to be up, too
            order_start_then_action(remote_rsc, action, pe_order_none,
                                    data_set);
            break;

        case stop_rsc:
        case action_demote:
            if (pcmk_is_set(container->flags, pe_rsc_failed)) {
                /* When the container representing a guest node fails, any stop
                 * or demote actions for resources running on the guest node
                 * are implied by the container stopping. This is similar to
                 * how fencing operations work for cluster nodes and remote
                 * nodes.
                 */
            } else {
                /* Ensure the operation happens before the connection is brought
                 * down.
                 *
                 * If we really wanted to, we could order these after the
                 * connection start, IFF the container's current role was
                 * stopped (otherwise we re-introduce an ordering loop when the
                 * connection is restarting).
                 */
                order_action_then_stop(action, remote_rsc, pe_order_none,
                                       data_set);
            }
            break;

        default:
            /* Wait for the connection resource to be up */
            if (is_recurring_action(action)) {
                /* In case we ever get the recovery logic wrong, force
                 * recurring monitors to be restarted, even if just
                 * the connection was re-established
                 */
                if(task != no_action) {
                    order_start_then_action(remote_rsc, action,
                                            pe_order_implies_then, data_set);
                }
            } else {
                order_start_then_action(remote_rsc, action, pe_order_none,
                                        data_set);
            }
            break;
    }
}

/*!
 * \internal
 * \brief Order all relevant actions relative to remote connection actions
 *
 * \param[in] data_set  Cluster working set
 */
void
pcmk__order_remote_connection_actions(pe_working_set_t *data_set)
{
    if (!pcmk_is_set(data_set->flags, pe_flag_have_remote_nodes)) {
        return;
    }

    crm_trace("Creating remote connection orderings");

    for (GList *gIter = data_set->actions; gIter != NULL; gIter = gIter->next) {
        pe_action_t *action = (pe_action_t *) gIter->data;
        pe_resource_t *remote = NULL;

        // We are only interested in resource actions
        if (action->rsc == NULL) {
            continue;
        }

        /* Special case: If we are clearing the failcount of an actual
         * remote connection resource, then make sure this happens before
         * any start of the resource in this transition.
         */
        if (action->rsc->is_remote_node &&
            pcmk__str_eq(action->task, CRM_OP_CLEAR_FAILCOUNT, pcmk__str_casei)) {

            pcmk__new_ordering(action->rsc, NULL, action, action->rsc,
                               pcmk__op_key(action->rsc->id, RSC_START, 0),
                               NULL, pe_order_optional, data_set);

            continue;
        }

        // We are only interested in actions allocated to a node
        if (action->node == NULL) {
            continue;
        }

        if (!pe__is_guest_or_remote_node(action->node)) {
            continue;
        }

        /* We are only interested in real actions.
         *
         * @TODO This is probably wrong; pseudo-actions might be converted to
         * real actions and vice versa later in update_actions() at the end of
         * pcmk__apply_orderings().
         */
        if (pcmk_is_set(action->flags, pe_action_pseudo)) {
            continue;
        }

        remote = action->node->details->remote_rsc;
        if (remote == NULL) {
            // Orphaned
            continue;
        }

        /* Another special case: if a resource is moving to a Pacemaker Remote
         * node, order the stop on the original node after any start of the
         * remote connection. This ensures that if the connection fails to
         * start, we leave the resource running on the original node.
         */
        if (pcmk__str_eq(action->task, RSC_START, pcmk__str_casei)) {
            for (GList *item = action->rsc->actions; item != NULL;
                 item = item->next) {
                pe_action_t *rsc_action = item->data;

                if ((rsc_action->node->details != action->node->details)
                    && pcmk__str_eq(rsc_action->task, RSC_STOP, pcmk__str_casei)) {
                    pcmk__new_ordering(remote, start_key(remote), NULL,
                                       action->rsc, NULL, rsc_action,
                                       pe_order_optional, data_set);
                }
            }
        }

        /* The action occurs across a remote connection, so create
         * ordering constraints that guarantee the action occurs while the node
         * is active (after start, before stop ... things like that).
         *
         * This is somewhat brittle in that we need to make sure the results of
         * this ordering are compatible with the result of get_router_node().
         * It would probably be better to add XML_LRM_ATTR_ROUTER_NODE as part
         * of this logic rather than action2xml().
         */
        if (remote->container) {
            crm_trace("Container ordering for %s", action->uuid);
            apply_container_ordering(action, data_set);

        } else {
            crm_trace("Remote ordering for %s", action->uuid);
            apply_remote_ordering(action, data_set);
        }
    }
}

/*!
 * \internal
 * \brief Check whether a node is a failed remote node
 *
 * \param[in] node  Node to check
 *
 * \return true if \p node is a failed remote node, false otherwise
 */
bool
pcmk__is_failed_remote_node(pe_node_t *node)
{
    return pe__is_remote_node(node) && (node->details->remote_rsc != NULL)
           && (get_remote_node_state(node) == remote_state_failed);
}

/*!
 * \internal
 * \brief Check whether a given resource corresponds to a given node as guest
 *
 * \param[in] rsc   Resource to check
 * \param[in] node  Node to check
 *
 * \return true if \p node is a guest node and \p rsc is its containing
 *         resource, otherwise false
 */
bool
pcmk__rsc_corresponds_to_guest(pe_resource_t *rsc, pe_node_t *node)
{
    return (rsc != NULL) && (rsc->fillers != NULL) && (node != NULL)
            && (node->details->remote_rsc != NULL)
            && (node->details->remote_rsc->container == rsc);
}
