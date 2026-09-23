/** \file LinearProgramSolverCplex.h
 *
 * A LinearProgramSolver backed by IBM ILOG CPLEX (C callable library).
 * Drop-in alternative to LinearProgramSolverGurobi: KernelizerLinearProgramming and MWVCSolverLinearProgramming accept it unchanged.
 *
 * The model is buffered in plain arrays while it is being described, then handed to CPLEX in two bulk calls inside solve(). All of that is inside kernelize(),
 * so model construction is timed exactly as it is for Gurobi.
 *
 * Environment variables (mirroring WCSP_GUROBI_*):
 *   WCSP_CPLEX_THREADS  1 (default) | N | 0 = let CPLEX use every core
 *   WCSP_CPLEX_METHOD   0 auto (default) | 1 primal | 2 dual | 3 network | 4 barrier | 5 sifting | 6 concurrent
 *   WCSP_CPLEX_LOG      0 (default) | 1 = print CPLEX's own log
 */

#ifdef HAVE_CPLEX

#ifndef LINEARPROGRAMSOLVERCPLEX_H_
#define LINEARPROGRAMSOLVERCPLEX_H_

#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include <ilcplex/cplex.h>

#include "LinearProgramSolver.h"

namespace cplex_detail
{
    // The integer type CPLEX uses for "position in the nonzero list" differs between CPLEX versions
    // (plain int in some, a 64-bit type in others). Instead of guessing a name, read it straight off the declaration of CPXaddrows in the installed cplex.h:
    //   argument 4 = nzcnt   (how many nonzeros in total)
    //   argument 7 = rmatbeg (where each row starts in the nonzero list)
    template <class R, class... A> std::tuple<A...> argsOf(R (*)(A...));
    using addrows_args = decltype(argsOf(&CPXaddrows));
    using nzcnt_t = std::tuple_element_t<4, addrows_args>;
    using nnz_t   = std::remove_cv_t<std::remove_pointer_t<std::tuple_element_t<7, addrows_args>>>;
}

class LinearProgramSolverCplex : public LinearProgramSolver
{
private:
    CPXENVptr env = nullptr;
    CPXLPptr  lp  = nullptr;

    // columns (variables)
    std::vector<double> objCoef;
    std::vector<double> lowerBound;
    std::vector<double> upperBound;
    std::vector<char>   colType;
    bool hasInteger = false;

    // rows (constraints), compressed sparse row form
    std::vector<cplex_detail::nnz_t> rowBegin;
    std::vector<int>    rowIndex;
    std::vector<double> rowValue;
    std::vector<double> rowRhs;
    std::vector<char>   rowSense;

    int objSense = CPX_MIN;

    void check(int status, const std::string& what);
    void createProblem();
    void freeProblem();
    void clearBuffers();

public:
    LinearProgramSolverCplex();
    virtual ~LinearProgramSolverCplex();

    virtual variable_id_t addVariable(double coefficient, VarType type = VarType::BINARY, double lb = 0.0, double ub = 1.0);
    virtual constraint_id_t addConstraint(const std::vector<variable_id_t>& variables, const std::vector<double>& coefficients, double rhs = 0.0, ConstraintType type = ConstraintType::LESS_EQUAL);
    virtual void setObjectiveType(ObjectiveType type);
    virtual double solve(std::vector<double>& assignments);
    virtual void reset();
    virtual void setTimeLimit(double t);
};

#endif /* LINEARPROGRAMSOLVERCPLEX_H_ */

#endif // HAVE_CPLEX