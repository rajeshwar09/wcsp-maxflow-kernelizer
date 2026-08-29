#ifndef MAXFLOW_PERTURBATION_H
#define MAXFLOW_PERTURBATION_H

//  perturbation.h -- tie-breaking weight perturbation for NT kernelization
//
//  NEED:
//    With equal vertex weights the MWVC linear program is degenerate: many
//    distinct optimal solutions share the same cost. One of them is the all-half
//    solution, which decides nothing. Max-flow lands on it; the LP simplex lands
//    on a more integral one. Measured on d_m2000: max-flow 0%, Gurobi 100%
//
//    Giving each vertex a distinct weight removes the tie, so the optimum becomes
//    unique -- and a unique optimum is a vertex of the LP polytope, i.e. one of
//    the useful solutions rather than all-half.
//
//  CORRECTNESS -- both modes guarantee that every optimum of the perturbed
//  instance is also an optimum of the original, so NT persistency transfers
//  unchanged and the final WCSP objective cannot move.
//
//    integer mode (DEFAULT)   w'(v) = M*w(v) + r(v),  r(v) in [1,K],  M > n*K
//      For covers A,B with w(A) < w(B): weights are integral so w(B) >= w(A)+1,
//      hence w'(B) - w'(A) >= M - n*K > 0. The order is preserved exactly.
//      Every value stays an integer, so MAXFLOW_EPSILON never participates and
//      the solver's integrality property is preserved.
//      Exactness in a double needs M*W < 2^53; K is reduced automatically.
//
//    real mode (EXPERIMENTAL) w'(v) = w(v) + eps(v),  eps(v) in [0, delta)
//      Total distortion is < n*delta <= 1/2 and weights are integral, so any
//      perturbed optimum is a true optimum. Bound: delta < 1/(2n).
//      This reintroduces fractional capacities: residual values of order delta
//      can approach MAXFLOW_EPSILON (1e-8) on large instances, so the mode is
//      guarded by min_ratio and is not the default.


#include <cstdint>
#include <cmath>
#include <iostream>
#include <string>

#include "src/common/types.h"

namespace maxflow {

  enum class perturb_mode { off, integer, real };

  inline const char* perturb_mode_name(perturb_mode m) {
    switch (m) {
      case perturb_mode::integer: return "int";
      case perturb_mode::real:    return "real";
      default:                    return "off";
    }
  }

  //  Parse a mode name. Returns false when the string is not recognised.
  inline bool parse_perturb_mode(const std::string& s, perturb_mode& out) {
    if (s == "off"  || s == "none") { out = perturb_mode::off;     return true; }
    if (s == "int"  || s == "integer") { out = perturb_mode::integer; return true; }
    if (s == "real" || s == "float")   { out = perturb_mode::real;    return true; }
    return false;
  }

  struct perturb_config {
    perturb_mode  mode      = perturb_mode::off;

    //  integer mode: offsets are drawn from [1, spread]; scale is the multiplier M applied to the original weights. scale <= 0 means "choose automatically"
    long          spread    = 8;
    double        scale     = 0.0;

    //  real mode: eps is drawn from [0, delta). delta <= 0 means 1/(2n)
    double        delta     = 0.0;

    //  real mode guard: refuse to perturb when delta/W falls below this, because the perturbation would then be lost in double rounding of the flow
    double        min_ratio = 1e-13;

    //  integer mode guard: largest product M*W we accept as exact in a double
    double        max_exact = 9.0e15;   //  2^53 is about 9.007e15

    std::uint64_t seed      = 1;
    bool          verbose   = true;
  };

  //  splitmix64 -- a stateless mixer. Same input, same output, everywhere
  inline std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
  }

  //  perturber
  //
  //  Built once per kernelize() call from the config, the vertex count n and the total original weight W. It resolves the automatic parameters, applies the
  //  safety guards, and then answers weight(i, w) for each vertex
  //
  //  When a guard trips the perturber deactivates itself and weight() returns the original weight, so the caller never has to branch.

  class perturber {
    public:
      perturber(const perturb_config& cfg, int n, cap_t total_weight)
        : mode_(cfg.mode), seed_(cfg.seed), n_(n), scale_(1.0), spread_(0), delta_(0.0) {

        if (mode_ == perturb_mode::off) return;

        if (n <= 0 || !(total_weight > cap_t(0))) {
          disable(cfg, "graph is empty or has zero total weight");
          return;
        }

        if (mode_ == perturb_mode::integer) {
          spread_ = cfg.spread > 0 ? cfg.spread : 8;

          //  M must exceed n*K so that no accumulation of offsets can outweigh a genuine difference of 1 in the original weights
          //  Shrink K until the product M*W is still exactly representable in a double.
          while (spread_ >= 1) {
            double m = cfg.scale > 0.0
                     ? cfg.scale
                     : static_cast<double>(n) * static_cast<double>(spread_) + 1.0;
            if (m * static_cast<double>(total_weight) <= cfg.max_exact) {
              scale_ = m;
              break;
            }
            if (cfg.scale > 0.0) { spread_ = 0; break; }  //  explicit M, cannot shrink
            spread_ /= 2;
          }

          if (spread_ < 1) {
            disable(cfg, "M*W would exceed 2^53; instance too large for integer mode");
            return;
          }

          if (cfg.verbose) {
            std::cout << "[perturb] mode=int  scale=" << static_cast<long long>(scale_)
                      << "  spread=" << spread_
                      << "  seed=" << seed_
                      << "  exact=yes\n";
          }
          return;
        }

        //  real mode
        delta_ = cfg.delta > 0.0 ? cfg.delta : 1.0 / (2.0 * static_cast<double>(n));

        double ratio = delta_ / static_cast<double>(total_weight);
        if (ratio < cfg.min_ratio) {
          disable(cfg, "delta/W below min_ratio; perturbation would vanish in rounding");
          return;
        }
        if (delta_ <= 100.0 * static_cast<double>(MAXFLOW_EPSILON)) {
          disable(cfg, "delta is within 100x of MAXFLOW_EPSILON; use --perturb int");
          return;
        }

        if (cfg.verbose) {
          std::cout << "[perturb] mode=real delta=" << delta_
                    << "  delta/W=" << ratio
                    << "  seed=" << seed_
                    << "  exact=no\n";
        }
      }

      bool   active() const { return mode_ != perturb_mode::off; }
      double scale()  const { return scale_; }
      double delta()  const { return delta_; }

      //  The capacity to use for vertex i, given its original weight.
      cap_t weight(int i, cap_t w) const {
        switch (mode_) {
          case perturb_mode::integer: {
            std::uint64_t u = draw(i);
            //  r in [1, spread]
            long r = static_cast<long>(u % static_cast<std::uint64_t>(spread_)) + 1;
            return static_cast<cap_t>(scale_ * static_cast<double>(w)
                                      + static_cast<double>(r));
          }
          case perturb_mode::real: {
            std::uint64_t u = draw(i);
            //  uniform in [0,1) from the top 53 bits
            double f = static_cast<double>(u >> 11) * (1.0 / 9007199254740992.0);
            return static_cast<cap_t>(static_cast<double>(w) + f * delta_);
          }
          default:
            return w;
        }
      }

      //  Undo the integer scaling on a flow or cut value, for reporting only
      //  Meaningless in real mode, where it returns the value unchanged
      double unscale(double v) const {
        return mode_ == perturb_mode::integer ? v / scale_ : v;
      }

    private:
      std::uint64_t draw(int i) const {
        return splitmix64(seed_ ^ splitmix64(static_cast<std::uint64_t>(i) + 0x9E3779B97F4A7C15ULL));
      }

      void disable(const perturb_config& cfg, const char* why) {
        if (cfg.verbose) {
          std::cout << "[perturb] DISABLED (" << perturb_mode_name(mode_)
                    << "): " << why << "\n";
        }
        mode_ = perturb_mode::off;
      }

      perturb_mode  mode_;
      std::uint64_t seed_;
      int           n_;
      double        scale_;
      long          spread_;
      double        delta_;
  };

} // namespace maxflow

#endif // MAXFLOW_PERTURBATION_H