#ifndef MAXFLOW_STAGE_TIMER_H
#define MAXFLOW_STAGE_TIMER_H

#include <chrono>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace maxflow {

  //  A tiny global accumulator for named stage timings
  //
  //  e2e_solve already times the top-level pipeline stages (parse, toPolynomial, addPolynomial, simplify, getGraph copy, kernelize, solve)
  //
  //  Any component can register time against a name here. e2e_solve prints
  //  everything collected at the end, one line per stage:
  //
  //      [time] <name> : <seconds> s
  //
  //  kernelize() is called once per round, so a stage registered inside it
  //  accumulates across rounds automatically. The collection harness parses those
  //  lines by name, so a stage added later -- by the worklist solver, say -- shows
  //  up in the raw output with no harness change at all.

  class stage_timer {
    public:
      static stage_timer& instance() {
        static stage_timer t;
        return t;
      }

      //  Add seconds to a named stage, creating it on first use.
      void add(const std::string& name, double seconds) {
        for (auto& e : entries_) {
          if (e.first == name) {
            e.second += seconds;
            return;
          }
        }
        entries_.emplace_back(name, seconds);
      }

      void dump(std::ostream& os) const {
        for (const auto& e : entries_) {
          os << "[time] " << e.first << " : " << e.second << " s\n";
        }
      }

      bool empty() const {
        return entries_.empty();
      }

      void reset() {
        entries_.clear();
      }

    private:
      stage_timer() = default;
      std::vector<std::pair<std::string, double> > entries_;
  };

  //  Convenience: read the clock in the same units the accumulator expects
  inline double stage_now() {
    return std::chrono::duration<double>(
             std::chrono::high_resolution_clock::now().time_since_epoch()).count();
  }

  //  Record the interval [t0, now) against a stage name and return the new "now", so consecutive phases chain without an extra variable per boundary:
  //
  //      double t = stage_now();
  //      ... phase 1 ...
  //      t = stage_mark("kern.collect", t);
  //      ... phase 2 ...
  //      t = stage_mark("kern.build_flownet", t);
  inline double stage_mark(const std::string& name, double t0) {
    double t1 = stage_now();
    stage_timer::instance().add(name, t1 - t0);
    return t1;
  }

} // namespace maxflow

#endif // MAXFLOW_STAGE_TIMER_H