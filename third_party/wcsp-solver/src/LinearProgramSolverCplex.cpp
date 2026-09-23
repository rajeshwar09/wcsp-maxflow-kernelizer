#ifdef HAVE_CPLEX

#include "LinearProgramSolverCplex.h"

#include <cstdlib>
#include <limits>
#include <string>

using cplex_detail::nnz_t;
using cplex_detail::nzcnt_t;

static int envInt(const char* name, int fallback)
{
    const char* s = std::getenv(name);
    return s ? std::atoi(s) : fallback;
}

// Turn a nonzero CPLEX status into an exception carrying CPLEX's own message,
// e.g. "Error 1016: Community Edition. Problem size limits exceeded."
void LinearProgramSolverCplex::check(int status, const std::string& what)
{
    if (status == 0)
        return;
    char buf[CPXMESSAGEBUFSIZE];
    const char* msg = CPXgeterrorstring(env, status, buf);
    throw Exception(what + " failed: " + (msg ? std::string(buf) : "CPLEX status " + std::to_string(status)));
}

void LinearProgramSolverCplex::createProblem()
{
    int status = 0;
    lp = CPXcreateprob(env, &status, "wcsp");
    if (lp == nullptr)
        check(status ? status : -1, "CPXcreateprob");
}

void LinearProgramSolverCplex::freeProblem()
{
    if (lp != nullptr)
        CPXfreeprob(env, &lp);
    lp = nullptr;
}

// swap-with-empty releases the memory, not just the contents
void LinearProgramSolverCplex::clearBuffers()
{
    std::vector<double>().swap(objCoef);
    std::vector<double>().swap(lowerBound);
    std::vector<double>().swap(upperBound);
    std::vector<char>().swap(colType);
    std::vector<nnz_t>().swap(rowBegin);
    std::vector<int>().swap(rowIndex);
    std::vector<double>().swap(rowValue);
    std::vector<double>().swap(rowRhs);
    std::vector<char>().swap(rowSense);
    hasInteger = false;
}

LinearProgramSolverCplex::LinearProgramSolverCplex()
{
    int status = 0;
    env = CPXopenCPLEX(&status);
    if (env == nullptr)
    {
        char buf[CPXMESSAGEBUFSIZE];
        CPXgeterrorstring(nullptr, status, buf);
        throw Exception(std::string("CPXopenCPLEX failed: ") + buf);
    }

    check(CPXsetintparam(env, CPXPARAM_ScreenOutput, envInt("WCSP_CPLEX_LOG", 0) ? CPX_ON : CPX_OFF), "set ScreenOutput");

    int threads = envInt("WCSP_CPLEX_THREADS", 1);
    if (threads < 0) threads = 1;
    check(CPXsetintparam(env, CPXPARAM_Threads, threads), "set Threads");

    check(CPXsetintparam(env, CPXPARAM_LPMethod, envInt("WCSP_CPLEX_METHOD", CPX_ALG_AUTOMATIC)), "set LPMethod");

    createProblem();
}

LinearProgramSolverCplex::~LinearProgramSolverCplex()
{
    freeProblem();
    if (env != nullptr)
        CPXcloseCPLEX(&env);
}

LinearProgramSolver::variable_id_t LinearProgramSolverCplex::addVariable(
    double coefficient, VarType type, double lb, double ub)
{
    objCoef.push_back(coefficient);
    lowerBound.push_back(lb);
    upperBound.push_back(ub);
    if (type == VarType::BINARY)
    {
        colType.push_back('B');
        hasInteger = true;
    }
    else
        colType.push_back('C');
    return static_cast<variable_id_t>(objCoef.size() - 1);
}

LinearProgramSolver::constraint_id_t LinearProgramSolverCplex::addConstraint(
    const std::vector<variable_id_t>& variables,
    const std::vector<double>& coefficients, double rhs,
    ConstraintType type)
{
    if (variables.empty())
        return -1;

    rowBegin.push_back(static_cast<nnz_t>(rowIndex.size()));
    for (size_t i = 0; i < variables.size(); ++ i)
    {
        rowIndex.push_back(variables[i]);
        rowValue.push_back(coefficients[i]);
    }
    rowRhs.push_back(rhs);

    switch (type)
    {
    case ConstraintType::LESS_EQUAL:    rowSense.push_back('L'); break;
    case ConstraintType::GREATER_EQUAL: rowSense.push_back('G'); break;
    case ConstraintType::EQUAL:         rowSense.push_back('E'); break;
    }
    return static_cast<constraint_id_t>(rowRhs.size() - 1);
}

void LinearProgramSolverCplex::setObjectiveType(ObjectiveType type)
{
    objSense = (type == ObjectiveType::MAX) ? CPX_MAX : CPX_MIN;
}

double LinearProgramSolverCplex::solve(std::vector<double>& assignments)
{
    assignments.clear();

    const int ncols = static_cast<int>(objCoef.size());
    const int nrows = static_cast<int>(rowRhs.size());
    if (ncols == 0)
        return 0.0;

    // If this CPLEX counts nonzeros in a 32-bit int, a model past ~2.1 billion nonzeros cannot be passed at all.
    // Fail loudly instead of letting the count wrap around.
    if (rowIndex.size() > static_cast<size_t>(std::numeric_limits<nnz_t>::max()))
        throw Exception("model has " + std::to_string(rowIndex.size()) + " nonzeros, more than this CPLEX build can index");

    check(CPXchgobjsen(env, lp, objSense), "CPXchgobjsen");

    check(CPXnewcols(env, lp, ncols, objCoef.data(), lowerBound.data(), upperBound.data(), hasInteger ? colType.data() : nullptr, nullptr), "CPXnewcols");

    if (nrows > 0)
        check(CPXaddrows(env, lp, 0, nrows, static_cast<nzcnt_t>(rowIndex.size()), rowRhs.data(), rowSense.data(), rowBegin.data(), rowIndex.data(), rowValue.data(), nullptr, nullptr), "CPXaddrows");

    // CPLEX now holds its own copy; drop ours so the two do not coexist during the solve
    const bool isMip = hasInteger;
    clearBuffers();

    if (isMip)
        check(CPXmipopt(env, lp), "CPXmipopt");
    else
        check(CPXlpopt(env, lp), "CPXlpopt");

    const int stat = CPXgetstat(env, lp);
    if (stat == CPX_STAT_ABORT_TIME_LIM || stat == CPXMIP_TIME_LIM_FEAS ||
        stat == CPXMIP_TIME_LIM_INFEAS)
        throw TimeOutException("");

    double objval = 0.0;
    check(CPXgetobjval(env, lp, &objval), "CPXgetobjval (solution status " + std::to_string(stat) + ")");

    assignments.resize(ncols);
    check(CPXgetx(env, lp, assignments.data(), 0, ncols - 1), "CPXgetx");

    return objval;
}

void LinearProgramSolverCplex::reset()
{
    freeProblem();
    clearBuffers();
    objSense = CPX_MIN;
    createProblem();
}

void LinearProgramSolverCplex::setTimeLimit(double t)
{
    check(CPXsetdblparam(env, CPXPARAM_TimeLimit, t), "set TimeLimit");
}

#endif // HAVE_CPLEX