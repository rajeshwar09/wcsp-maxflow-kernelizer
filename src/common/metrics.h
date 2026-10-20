// one global store for the numbers a run produces (times, counts, memory) every value is printed at the end as "[m] name : value" so the scripts can pick up new values without any change
// usage:
//   double t = now_sec();
//   (the work to time)
//   t = mark("time.kern.collect", t);
//   metrics::get().keep_max("mem.gpu_alloc_mb", mb);
//   metrics::get().print(std::cout);

#ifndef MAXFLOW_METRICS_H
#define MAXFLOW_METRICS_H

#include <chrono>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace maxflow {

  // [METRICS] list of (name, value) pairs, printed in the order they were first added
  class metrics {
    public:
      static metrics& get() {
        static metrics m;
        return m;
      }

      // [METRICS] add to the old value, used for times and counts summed over rounds
      void add(const std::string& name, double v) {
        double* p = find(name);
        if (p == nullptr) {
          items.emplace_back(name, v);
        } else {
          *p += v;
        }
      }

      // [METRICS] replace the old value
      void set(const std::string& name, double v) {
        double* p = find(name);
        if (p == nullptr) {
          items.emplace_back(name, v);
        } else {
          *p = v;
        }
      }

      // [METRICS] keep the larger value, used for memory peaks over rounds
      void keep_max(const std::string& name, double v) {
        double* p = find(name);
        if (p == nullptr) {
          items.emplace_back(name, v);
        } else if (v > *p) {
          *p = v;
        }
      }

      // [METRICS] current value of a name, 0 when it was never set
      double value(const std::string& name) {
        double* p = find(name);
        return p == nullptr ? 0.0 : *p;
      }

      // [METRICS] print every value, one per line
      void print(std::ostream& os) const {
        for (const auto& it : items) {
          os << "[m] " << it.first << " : " << it.second << "\n";
        }
      }

    private:
      metrics() = default;

      // [METRICS] linear search is fine, there are only a few dozen names
      double* find(const std::string& name) {
        for (auto& it : items) {
          if (it.first == name) {
            return &it.second;
          }
        }
        return nullptr;
      }

      std::vector<std::pair<std::string, double>> items;
  };

  // [TIMER] wall clock in seconds
  inline double now_sec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  // [TIMER] add the time since t0 to a name and return the current time, so phases can be chained
  inline double mark(const std::string& name, double t0) {
    double t1 = now_sec();
    metrics::get().add(name, t1 - t0);
    return t1;
  }

} // namespace maxflow

#endif // MAXFLOW_METRICS_H
