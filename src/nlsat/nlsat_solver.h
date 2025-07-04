/*++
Copyright (c) 2012 Microsoft Corporation

Module Name:

    nlsat_solver.h

Abstract:

    Nonlinear arithmetic satisfiability procedure.  The procedure is
    complete for nonlinear real arithmetic, but it also has limited
    support for integers.

Author:

    Leonardo de Moura (leonardo) 2012-01-02.

Revision History:

--*/
#pragma once

#include "nlsat/nlsat_types.h"
#include "util/params.h"
#include "util/statistics.h"
#include "util/rlimit.h"
#include "util/dependency.h"

namespace nlsat {

    class evaluator;
    class explain;

    class display_assumption_proc {
    public:
        virtual ~display_assumption_proc() = default;
        virtual std::ostream& operator()(std::ostream& out, assumption a) const = 0;
    };

    struct bound_constraint {
        var x;
        polynomial_ref A, B;
        bool is_strict;
        clause* c;
        bound_constraint(var x, polynomial_ref& A, polynomial_ref& B, bool is_strict, clause* c) :
            x(x), A(A), B(B), is_strict(is_strict), c(c) {}
    };

    class solver {
        struct imp;
        struct ctx;
        imp * m_imp;
        ctx * m_ctx;
        solver(ctx& c);
    public:
        solver(reslimit& rlim, params_ref const & p, bool incremental);
        ~solver();

        /**
           \brief Return reference to rational manager.
        */
        unsynch_mpq_manager & qm();
        
        /**
           \brief Return reference to algebraic number manager.
        */
        anum_manager & am();

        /**
           \brief Return a reference to the polynomial manager used by the solver.
        */
        pmanager & pm();

        void set_display_var(display_var_proc const & proc);

        void set_display_assumption(display_assumption_proc const& proc);

        // -----------------------
        //
        // Variable, Atoms, Clauses & Assumption creation
        //
        // -----------------------

        /**
           \brief Create a fresh boolean variable that is not associated with any 
                  nonlinear arithmetic atom.
        */
        bool_var mk_bool_var();
   
        literal  mk_true();

        /**
           \brief Create a real/integer variable.
        */
        var mk_var(bool is_int);
        
        /**
           \brief Create an atom of the form: p=0, p<0, p>0
           Where p = ps[0]^e[0]*...*ps[sz-1]^e[sz-1]

           e[i] = 1 if is_even[i] is false
           e[i] = 2 if is_even[i] is true

           \pre sz > 0
        */
        bool_var mk_ineq_atom(atom::kind k, unsigned sz, poly * const * ps, bool const * is_even);

        /**
           \brief Create a literal for the p=0, p<0, p>0.
           Where p = ps[0]^e[0]*...*ps[sz-1]^e[sz-1]  for sz > 0
                 p = 1                                for sz = 0

           e[i] = 1 if is_even[i] is false
           e[i] = 2 if is_even[i] is true
        */
        literal mk_ineq_literal(atom::kind k, unsigned sz, poly * const * ps, bool const * is_even, bool simplify = false);

        /**
           \brief Create an atom of the form: x=root[i](p), x<root[i](p), x>root[i](p)
        */
        bool_var mk_root_atom(atom::kind k, var x, unsigned i, poly * p);

        void inc_ref(bool_var b);
        void inc_ref(literal l) { inc_ref(l.var()); }
        void dec_ref(bool_var b);
        void dec_ref(literal l) { dec_ref(l.var()); }
        void inc_ref(assumption a);
        void dec_ref(assumption a);
        
        
        /**
           \brief Create a new clause.
        */
        void mk_clause(unsigned num_lits, literal * lits, assumption a = nullptr);

        // -----------------------
        //
        // Basic
        //
        // -----------------------

        /**
           \brief Return the number of Boolean variables.
        */
        unsigned num_bool_vars() const;
        
        /**
           \brief Get atom associated with Boolean variable. Return 0 if there is none.
        */
        atom * bool_var2atom(bool_var b);

        /**
           \brief extract free variables from literal.
         */
        void vars(literal l, var_vector& vs);

        /**
           \brief provide access to atoms. Used by explain.
        */
        atom_vector const& get_atoms();

        /**
           \brief Access var -> asserted equality.
        */

        atom_vector const& get_var2eq();        

        /**
           \brief expose evaluator.
        */
        
        evaluator& get_evaluator();

        /**
           \brief Access explanation module.
         */
        explain& get_explain();

        /**
           \brief Access assignments to variables.
         */
        void get_rvalues(assignment& as);
        void set_rvalues(assignment const& as);

        void get_bvalues(svector<bool_var> const& bvars, svector<lbool>& vs);
        void set_bvalues(svector<lbool> const& vs);

        /**
        * \brief Access functions for simplify module.
        */
        void del_clause(clause* c);
        clause* mk_clause(unsigned n, literal const* lits, bool learned, internal_assumption a);
        bool has_root_atom(clause const& c) const;
        assumption join(assumption a, assumption b);

        void inc_simplify();
        void add_bound(bound_constraint const& c);

        /**
           \brief reorder variables. 
         */
        void reorder(unsigned sz, var const* permutation);
        void restore_order();

        /**
           \brief Return number of integer/real variables
        */
        unsigned num_vars() const;

        bool is_int(var x) const;

        // -----------------------
        //
        // Search
        //
        // -----------------------
        lbool check();

        lbool check(literal_vector& assumptions);

        // -----------------------
        //
        // Model
        //
        // -----------------------
        anum const & value(var x) const;
        lbool bvalue(bool_var b) const;
        bool is_interpreted(bool_var b) const;

        lbool value(literal l) const;

        // -----------------------
        //
        // Core
        //
        // -----------------------

        void get_core(vector<assumption, false>& deps);

        // -----------------------
        //
        // Misc
        //
        // -----------------------
        void updt_params(params_ref const & p);
        static void collect_param_descrs(param_descrs & d);

        void reset();
        void collect_statistics(statistics & st);
        void reset_statistics();
        void display_status(std::ostream & out) const;

        // -----------------------
        //
        // Pretty printing
        //
        // -----------------------
   
        /**
           \brief Display solver's state.
        */
        std::ostream& display(std::ostream & out) const;

        /**
           \brief Display literal
        */
        std::ostream& display(std::ostream & out, literal l) const;

        std::ostream& display(std::ostream & out, unsigned n, literal const* ls) const;

        std::ostream& display(std::ostream& out, clause const& c) const;

        std::ostream& display(std::ostream & out, literal_vector const& ls) const;

        std::ostream& display(std::ostream & out, atom const& a) const;

        std::ostream& display_smt2(std::ostream & out, literal l) const;

        std::ostream& display_smt2(std::ostream & out, unsigned n, literal const* ls) const;

        std::ostream& display_smt2(std::ostream & out, literal_vector const& ls) const;



        std::ostream& display_smt2(std::ostream & out) const;

        /**
           \brief Display variable
        */
        std::ostream& display(std::ostream & out, var x) const;
        
        display_var_proc const & display_proc() const;

        std::ostream& display_assignment(std::ostream& out) const;

        std::ostream& display_var(std::ostream& out, unsigned j) const;
        
    };

};

