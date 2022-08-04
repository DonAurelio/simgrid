/* Copyright (c) 2016-2022. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/mc/explo/DFSExplorer.hpp"
#include "src/mc/VisitedState.hpp"
#include "src/mc/mc_config.hpp"
#include "src/mc/mc_exit.hpp"
#include "src/mc/mc_private.hpp"
#include "src/mc/mc_record.hpp"
#include "src/mc/transition/Transition.hpp"

#include "src/xbt/mmalloc/mmprivate.h"
#include "xbt/log.h"
#include "xbt/sysdep.h"

#include <cassert>
#include <cstdio>

#include <memory>
#include <string>
#include <vector>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(mc_dfs, mc, "DFS exploration algorithm of the model-checker");

namespace simgrid::mc {

xbt::signal<void()> DFSExplorer::on_exploration_start_signal;
xbt::signal<void()> DFSExplorer::on_backtracking_signal;

xbt::signal<void(State*)> DFSExplorer::on_state_creation_signal;

xbt::signal<void(State*)> DFSExplorer::on_restore_system_state_signal;
xbt::signal<void()> DFSExplorer::on_restore_initial_state_signal;
xbt::signal<void(Transition*)> DFSExplorer::on_transition_replay_signal;
xbt::signal<void(Transition*)> DFSExplorer::on_transition_execute_signal;

xbt::signal<void()> DFSExplorer::on_log_state_signal;

void DFSExplorer::check_non_termination(const State* current_state)
{
  for (auto state = stack_.rbegin(); state != stack_.rend(); ++state)
    if (Api::get().snapshot_equal((*state)->get_system_state(), current_state->get_system_state())) {
      XBT_INFO("Non-progressive cycle: state %ld -> state %ld", (*state)->get_num(), current_state->get_num());
      XBT_INFO("******************************************");
      XBT_INFO("*** NON-PROGRESSIVE CYCLE DETECTED ***");
      XBT_INFO("******************************************");
      XBT_INFO("Counter-example execution trace:");
      for (auto const& s : get_textual_trace())
        XBT_INFO("  %s", s.c_str());
      XBT_INFO("Path = %s", get_record_trace().to_string().c_str());
      log_state();

      throw TerminationError();
    }
}

RecordTrace DFSExplorer::get_record_trace() // override
{
  RecordTrace res;
  for (auto const& state : stack_)
    res.push_back(state->get_transition());
  return res;
}

std::vector<std::string> DFSExplorer::get_textual_trace() // override
{
  std::vector<std::string> trace;
  for (auto const& state : stack_) {
    const auto* t = state->get_transition();
    trace.push_back(xbt::string_printf("%ld: %s", t->aid_, t->to_string().c_str()));
  }
  return trace;
}

void DFSExplorer::log_state() // override
{
  on_log_state_signal();
  XBT_INFO("DFS exploration ended. %ld unique states visited; %ld backtracks (%lu transition replays, %lu states "
           "visited overall)",
           State::get_expanded_states(), backtrack_count_, mc_model_checker->get_visited_states(),
           Transition::get_replayed_transitions());
}

void DFSExplorer::run()
{
  on_exploration_start_signal();
  /* This function runs the DFS algorithm the state space.
   * We do so iteratively instead of recursively, dealing with the call stack manually.
   * This allows one to explore the call stack at will. */

  while (not stack_.empty()) {
    /* Get current state */
    State* state = stack_.back().get();

    XBT_DEBUG("**************************************************");
    XBT_DEBUG("Exploration depth=%zu (state:%ld; %zu interleaves)", stack_.size(), state->get_num(),
              state->count_todo());

    mc_model_checker->inc_visited_states();

    // Backtrack if we reached the maximum depth
    if (stack_.size() > (std::size_t)_sg_mc_max_depth) {
      if (reductionMode_ == ReductionMode::dpor) {
        XBT_ERROR("/!\\ Max depth of %d reached! THIS WILL PROBABLY BREAK the dpor reduction /!\\",
                  _sg_mc_max_depth.get());
        XBT_ERROR("/!\\ If bad things happen, disable dpor with --cfg=model-check/reduction:none /!\\");
      } else
        XBT_WARN("/!\\ Max depth reached ! /!\\ ");
      this->backtrack();
      continue;
    }

    // Backtrack if we are revisiting a state we saw previously
    if (visited_state_ != nullptr) {
      XBT_DEBUG("State already visited (equal to state %ld), exploration stopped on this path.",
                visited_state_->original_num == -1 ? visited_state_->num : visited_state_->original_num);

      visited_state_ = nullptr;
      this->backtrack();
      continue;
    }

    // Search for the next transition
    int next = state->next_transition();

    if (next < 0) { // If there is no more transition in the current state, backtrack.
      XBT_DEBUG("There remains %d actors, but none to interleave (depth %zu).", state->get_actor_count(),
                stack_.size() + 1);

      if (state->get_actor_count() == 0) {
        mc_model_checker->finalize_app();
        XBT_VERB("Execution came to an end at %s (state: %ld, depth: %zu)", get_record_trace().to_string().c_str(),
                 state->get_num(), stack_.size());
      }
      this->backtrack();
      continue;
    }

    /* Actually answer the request: let's execute the selected request (MCed does one step) */
    state->execute_next(next);
    on_transition_execute_signal(state->get_transition());

    // If there are processes to interleave and the maximum depth has not been
    // reached then perform one step of the exploration algorithm.
    XBT_VERB("Execute %ld: %.60s (stack depth: %zu, state: %ld, %zu interleaves)", state->get_transition()->aid_,
             state->get_transition()->to_string().c_str(), stack_.size(), state->get_num(), state->count_todo());

    /* Create the new expanded state (copy the state of MCed into our MCer data) */
    auto next_state = std::make_unique<State>(get_remote_app());
    on_state_creation_signal(next_state.get());

    if (_sg_mc_termination)
      this->check_non_termination(next_state.get());

    /* Check whether we already explored next_state in the past (but only if interested in state-equality reduction) */
    if (_sg_mc_max_visited_states > 0)
      visited_state_ = visited_states_.addVisitedState(next_state->get_num(), next_state.get(), true);

    /* If this is a new state (or if we don't care about state-equality reduction) */
    if (visited_state_ == nullptr) {
      /* Get an enabled process and insert it in the interleave set of the next state */
      for (auto const& [aid, _] : next_state->get_actors_list()) {
        if (next_state->is_actor_enabled(aid)) {
          next_state->mark_todo(aid);
          if (reductionMode_ == ReductionMode::dpor)
            break; // With DPOR, we take the first enabled transition
        }
      }

      if (dot_output != nullptr)
        std::fprintf(dot_output, "\"%ld\" -> \"%ld\" [%s];\n", state->get_num(), next_state->get_num(),
                     state->get_transition()->dot_string().c_str());

    } else if (dot_output != nullptr)
      std::fprintf(dot_output, "\"%ld\" -> \"%ld\" [%s];\n", state->get_num(),
                   visited_state_->original_num == -1 ? visited_state_->num : visited_state_->original_num,
                   state->get_transition()->dot_string().c_str());

    stack_.push_back(std::move(next_state));
  }

  log_state();
}

void DFSExplorer::backtrack()
{
  backtrack_count_++;
  XBT_VERB("Backtracking from %s", get_record_trace().to_string().c_str());
  on_backtracking_signal();
  stack_.pop_back();

  get_remote_app().check_deadlock();

  /* Traverse the stack backwards until a state with a non empty interleave set is found, deleting all the states that
   *  have it empty in the way. For each deleted state, check if the request that has generated it (from its
   *  predecessor state), depends on any other previous request executed before it. If it does then add it to the
   *  interleave set of the state that executed that previous request. */

  while (not stack_.empty()) {
    std::unique_ptr<State> state = std::move(stack_.back());
    stack_.pop_back();
    if (reductionMode_ == ReductionMode::dpor) {
      aid_t issuer_id = state->get_transition()->aid_;
      for (auto i = stack_.rbegin(); i != stack_.rend(); ++i) {
        State* prev_state = i->get();
        if (state->get_transition()->aid_ == prev_state->get_transition()->aid_) {
          XBT_DEBUG("Simcall >>%s<< and >>%s<< with same issuer %ld", state->get_transition()->to_string().c_str(),
                    prev_state->get_transition()->to_string().c_str(), issuer_id);
          break;
        } else if (prev_state->get_transition()->depends(state->get_transition())) {
          XBT_VERB("Dependent Transitions:");
          XBT_VERB("  %s (state=%ld)", prev_state->get_transition()->to_string().c_str(), prev_state->get_num());
          XBT_VERB("  %s (state=%ld)", state->get_transition()->to_string().c_str(), state->get_num());

          if (not prev_state->is_done(issuer_id))
            prev_state->mark_todo(issuer_id);
          else
            XBT_DEBUG("Actor %ld is in done set", issuer_id);
          break;
        } else {
          XBT_VERB("INDEPENDENT Transitions:");
          XBT_VERB("  %s (state=%ld)", prev_state->get_transition()->to_string().c_str(), prev_state->get_num());
          XBT_VERB("  %s (state=%ld)", state->get_transition()->to_string().c_str(), state->get_num());
        }
      }
    }

    if (state->count_todo() && stack_.size() < (std::size_t)_sg_mc_max_depth) {
      /* We found a back-tracking point, let's loop */
      XBT_DEBUG("Back-tracking to state %ld at depth %zu", state->get_num(), stack_.size() + 1);
      stack_.push_back(
          std::move(state)); // Put it back on the stack from which it was removed earlier in this while loop
      this->restore_state();
      XBT_DEBUG("Back-tracking to state %ld at depth %zu done", stack_.back()->get_num(), stack_.size());
      break;
    } else {
      XBT_DEBUG("Delete state %ld at depth %zu", state->get_num(), stack_.size() + 1);
    }
  }
}

void DFSExplorer::restore_state()
{
  /* If asked to rollback on a state that has a snapshot, restore it */
  State* last_state = stack_.back().get();
  if (const auto* system_state = last_state->get_system_state()) {
    Api::get().restore_state(system_state);
    on_restore_system_state_signal(last_state);
    return;
  }

  /* if no snapshot, we need to restore the initial state and replay the transitions */
  get_remote_app().restore_initial_state();
  on_restore_initial_state_signal();

  /* Traverse the stack from the state at position start and re-execute the transitions */
  for (std::unique_ptr<State> const& state : stack_) {
    if (state == stack_.back()) /* If we are arrived on the target state, don't replay the outgoing transition */
      break;
    state->get_transition()->replay();
    on_transition_replay_signal(state->get_transition());
    /* Update statistics */
    mc_model_checker->inc_visited_states();
  }
}

DFSExplorer::DFSExplorer(RemoteApp& remote_app) : Exploration(remote_app)
{
  reductionMode_ = reduction_mode;
  if (_sg_mc_termination)
    reductionMode_ = ReductionMode::none;
  else if (reductionMode_ == ReductionMode::unset)
    reductionMode_ = ReductionMode::dpor;

  if (_sg_mc_termination)
    XBT_INFO("Check non progressive cycles");
  else
    XBT_INFO("Start a DFS exploration. Reduction is: %s.",
             (reductionMode_ == ReductionMode::none ? "none"
                                                    : (reductionMode_ == ReductionMode::dpor ? "dpor" : "unknown")));

  XBT_DEBUG("Starting the DFS exploration");

  auto initial_state = std::make_unique<State>(get_remote_app());

  XBT_DEBUG("**************************************************");

  /* Get an enabled actor and insert it in the interleave set of the initial state */
  XBT_DEBUG("Initial state. %d actors to consider", initial_state->get_actor_count());
  for (auto const& [aid, _] : initial_state->get_actors_list()) {
    if (initial_state->is_actor_enabled(aid)) {
      initial_state->mark_todo(aid);
      if (reductionMode_ == ReductionMode::dpor) {
        XBT_DEBUG("Actor %ld is TODO, DPOR is ON so let's go for this one.", aid);
        break;
      }
      XBT_DEBUG("Actor %ld is TODO", aid);
    }
  }

  stack_.push_back(std::move(initial_state));
}

Exploration* create_dfs_exploration(RemoteApp& remote_app)
{
  return new DFSExplorer(remote_app);
}

} // namespace simgrid::mc
