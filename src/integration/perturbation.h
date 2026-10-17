// tie-breaking weight perturbation for the kernelizer
// with equal weights the MWVC LP has many optimal solutions and max-flow can land on the all-half one, which decides nothing
// small distinct changes to the weights make the optimum unique without changing which covers are optimal
// usage:
//   perturb_config cfg;
//   cfg.mode = perturb_mode::integer;
//   perturber p(cfg, n, total_weight, weights_integral);
//   cap_t w2 = p.weight(i, w);
//
// int mode (default):   w' = M*w + r,   r in [1, K],   M > n*K
//   integral weights differ by at least 1 so M - n*K > 0 keeps the order of all covers, values stay integers
// real mode (testing):  w' = w + eps,   eps in [0, delta),   delta < 1/(2n)
//   total change is below 1/2 so the order of integral covers is kept, but values become fractional
// guard 1: spread must be at least 2, a spread of 1 adds the same offset to everyone and breaks no ties
// guard 2: weights must be integral, both proofs need it, UAI weights are decimals so it is switched off there

#ifndef MAXFLOW_PERTURBATION_H
#define MAXFLOW_PERTURBATION_H

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

#include "src/common/types.h"

namespace maxflow {

  enum class perturb_mode { off, integer, real };

  inline const char* perturb_mode_name(perturb_mode m) {
    switch (m) {
      case perturb_mode::integer: return "int";
      case perturb_mode::real: return "real";
      default: return "off";
    }
  }

  // [CONFIG] text from the command line to a mode, false when the text is unknown
  inline bool parse_perturb_mode(const std::string& s, perturb_mode& out) {
    if (s == "off" || s == "none") {
      out = perturb_mode::off;
      return true;
    }
    if (s == "int" || s == "integer") {
      out = perturb_mode::integer;
      return true;
    }
    if (s == "real" || s == "float") {
      out = perturb_mode::real;
      return true;
    }
    return false;
  }

  // [CONFIG] all perturbation settings
  struct perturb_config {
    perturb_mode mode = perturb_mode::off;
    long spread = 8;               // int mode, offsets from 1 to spread
    double scale = 0.0;            // int mode, multiplier M, 0 = automatic
    double delta = 0.0;            // real mode, 0 = 1/(2n)
    double min_ratio = 1e-13;      // real mode, below this delta/W the change is lost in rounding
    double max_exact = 9.0e15;     // int mode, largest M*W that a double holds exactly (2^53)
    long min_spread = 2;           // guard 1
    std::uint64_t seed = 1;
    bool verbose = true;
  };

  // [RANDOM] splitmix64, same input always gives the same output, no state needed
  inline std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
  }

  // [PERTURB] built once per kernelize call, then weight(i, w) gives the new weight of vertex i
  // if a guard fails the perturber switches itself off and weight() returns w unchanged
  class perturber {
    public:
      perturber(const perturb_config& cfg, int n, cap_t total_weight, bool weights_integral = true)
        : mode(cfg.mode), seed(cfg.seed), scale(1.0), spread(0), delta(0.0) {
        if (mode == perturb_mode::off) {
          return;
        }
        if (n <= 0 || !(total_weight > cap_t(0))) {
          disable(cfg, "graph is empty or has zero total weight");
          return;
        }
        // [GUARD] guard 2, integral weights
        if (!weights_integral) {
          disable(cfg, "vertex weights are not integral, the safety bound does not hold");
          return;
        }

        if (mode == perturb_mode::integer) {
          // [GUARD] shrink K until M*W fits exactly in a double, but never below guard 1
          spread = cfg.spread > 0 ? cfg.spread : 8;
          while (spread >= cfg.min_spread) {
            double m = cfg.scale > 0.0 ? cfg.scale : static_cast<double>(n) * static_cast<double>(spread) + 1.0;
            if (m * static_cast<double>(total_weight) <= cfg.max_exact) {
              scale = m;
              break;
            }
            if (cfg.scale > 0.0) {
              spread = 0;
              break;
            }
            spread /= 2;
          }
          if (spread < cfg.min_spread) {
            disable(cfg, "weights too large, no spread >= 2 keeps M*W exact in a double");
            return;
          }
          if (cfg.verbose) {
            std::cout << "[perturb] mode=int scale=" << static_cast<long long>(scale) << " spread=" << spread << " seed=" << seed << "\n";
          }
          return;
        }

        // [GUARD] real mode, delta must not vanish next to W or next to MAXFLOW_EPSILON
        delta = cfg.delta > 0.0 ? cfg.delta : 1.0 / (2.0 * static_cast<double>(n));
        double ratio = delta / static_cast<double>(total_weight);
        if (ratio < cfg.min_ratio) {
          disable(cfg, "delta/W below min_ratio, perturbation would vanish in rounding");
          return;
        }
        if (delta <= 100.0 * static_cast<double>(MAXFLOW_EPSILON)) {
          disable(cfg, "delta is within 100x of MAXFLOW_EPSILON, use --perturb int");
          return;
        }
        if (cfg.verbose) {
          std::cout << "[perturb] mode=real delta=" << delta << " delta/W=" << ratio << " seed=" << seed << "\n";
        }
      }

      bool active() const {
        return mode != perturb_mode::off;
      }

      // [PERTURB] new capacity of vertex i with original weight w
      cap_t weight(int i, cap_t w) const {
        if (mode == perturb_mode::integer) {
          long r = static_cast<long>(draw(i) % static_cast<std::uint64_t>(spread)) + 1;
          return static_cast<cap_t>(scale * static_cast<double>(w) + static_cast<double>(r));
        }
        if (mode == perturb_mode::real) {
          // [RANDOM] top 53 bits give a uniform number in [0, 1)
          double f = static_cast<double>(draw(i) >> 11) * (1.0 / 9007199254740992.0);
          return static_cast<cap_t>(static_cast<double>(w) + f * delta);
        }
        return w;
      }

    private:
      perturb_mode mode;
      std::uint64_t seed;
      double scale;
      long spread;
      double delta;

      // [RANDOM] random number for vertex i under this seed
      std::uint64_t draw(int i) const {
        return splitmix64(seed ^ splitmix64(static_cast<std::uint64_t>(i) + 0x9E3779B97F4A7C15ULL));
      }

      void disable(const perturb_config& cfg, const char* why) {
        if (cfg.verbose) {
          std::cout << "[perturb] disabled (" << perturb_mode_name(mode) << "): " << why << "\n";
        }
        mode = perturb_mode::off;
      }
  };

  // [GUARD] true when w is a whole number
  inline bool is_integral(cap_t w) {
    return w == std::floor(w);
  }

} // namespace maxflow

#endif // MAXFLOW_PERTURBATION_H
