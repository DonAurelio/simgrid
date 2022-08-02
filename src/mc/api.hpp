/* Copyright (c) 2020-2022. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SIMGRID_MC_API_HPP
#define SIMGRID_MC_API_HPP

#include <memory>
#include <vector>

#include "simgrid/forward.h"
#include "src/mc/api/RemoteApp.hpp"
#include "src/mc/api/State.hpp"
#include "src/mc/mc_forward.hpp"
#include "src/mc/mc_record.hpp"
#include "xbt/automaton.hpp"
#include "xbt/base.h"

namespace simgrid::mc {

XBT_DECLARE_ENUM_CLASS(ExplorationAlgorithm, Safety, UDPOR, Liveness, CommDeterminism);

/*
** This class aimes to implement FACADE APIs for simgrid. The FACADE layer sits between the CheckerSide
** (Unfolding_Checker, DPOR, ...) layer and the
** AppSide layer. The goal is to drill down into the entagled details in the CheckerSide layer and break down the
** detailes in a way that the CheckerSide eventually
** be capable to acquire the required information through the FACADE layer rather than the direct access to the AppSide.
*/

class Api {
private:
  Api() = default;

  struct DerefAndCompareByActorsCountAndUsedHeap {
    template <class X, class Y> bool operator()(X const& a, Y const& b) const
    {
      return std::make_pair(a->actor_count_, a->heap_bytes_used) < std::make_pair(b->actor_count_, b->heap_bytes_used);
    }
  };

  std::unique_ptr<simgrid::mc::RemoteApp> remote_app_;

public:
  // No copy:
  Api(Api const&) = delete;
  void operator=(Api const&) = delete;

  static Api& get()
  {
    static Api api;
    return api;
  }

  simgrid::mc::Exploration* initialize(char** argv, const std::unordered_map<std::string, std::string>& env,
                                       simgrid::mc::ExplorationAlgorithm algo);

  // ACTOR APIs
  unsigned long get_maxpid() const;

  // REMOTE APIs
  std::size_t get_remote_heap_bytes() const;

  // MODEL CHECKER APIs
  void mc_inc_visited_states() const;
  unsigned long mc_get_visited_states() const;
  XBT_ATTRIB_NORETURN void mc_exit(int status) const;

  // STATE APIs
  void restore_state(const Snapshot* system_state) const;

  // SNAPSHOT APIs
  bool snapshot_equal(const Snapshot* s1, const Snapshot* s2) const;
  simgrid::mc::Snapshot* take_snapshot(long num_state) const;

  // SESSION APIs
  simgrid::mc::RemoteApp const& get_remote_app() const { return *remote_app_; }
  void s_close();

  // AUTOMATION APIs
  void automaton_load(const char* file) const;
  std::vector<int> automaton_propositional_symbol_evaluate() const;
  std::vector<xbt_automaton_state_t> get_automaton_state() const;
  int compare_automaton_exp_label(const xbt_automaton_exp_label* l) const;
  void set_property_automaton(xbt_automaton_state_t const& automaton_state) const;
  inline DerefAndCompareByActorsCountAndUsedHeap compare_pair() const
  {
    return DerefAndCompareByActorsCountAndUsedHeap();
  }
  xbt_automaton_exp_label_t get_automaton_transition_label(xbt_dynar_t const& dynar, int index) const;
  xbt_automaton_state_t get_automaton_transition_dst(xbt_dynar_t const& dynar, int index) const;
};

} // namespace simgrid::mc

#endif
