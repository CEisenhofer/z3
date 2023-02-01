/*++
Copyright (c) 2014 Microsoft Corporation

Module Name:

    fixplex.h

Abstract:

    Fixed-precision unsigned integer simplex tableau.

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-6

--*/

#pragma once

#include <limits>
#include "math/simplex/sparse_matrix.h"
#include "fixplex_mod_interval.h"
#include "util/heap.h"
#include "util/map.h"
#include "util/rational.h"
#include "util/lbool.h"
#include "util/uint_set.h"
#include "util/dependency.h"
#include "util/ref.h"
#include "util/params.h"
#include "util/union_find.h"

inline rational to_rational(uint64_t n) { return rational(n, rational::ui64()); }
inline unsigned trailing_zeros(unsigned short n) { return trailing_zeros((uint32_t)n); }
inline unsigned trailing_zeros(unsigned char n) { return trailing_zeros((uint32_t)n); }
inline unsigned numeral2hash(unsigned char const& n) { return n; }
inline unsigned numeral2hash(unsigned short const& n) { return n; }
inline unsigned numeral2hash(uint32_t const& n) { return n; }
inline unsigned numeral2hash(uint64_t const& n) { return static_cast<unsigned>(n ^ (n >> 32)); }


namespace polysat {

    typedef unsigned var_t;
    
    struct fixplex_base {
        virtual ~fixplex_base() {}
        virtual lbool make_feasible() = 0;
        virtual void add_row(var_t base, unsigned num_vars, var_t const* vars, rational const* coeffs) = 0;
        virtual void del_row(var_t base_var) = 0;
        virtual void push() = 0;
        virtual void pop(unsigned n) = 0;
        virtual std::ostream& display(std::ostream& out) const = 0;
        virtual void collect_statistics(::statistics & st) const = 0;
        virtual void set_bounds(var_t v, rational const& lo, rational const& hi, unsigned dep) = 0;        
        virtual void set_value(var_t v, rational const& val, unsigned dep) = 0;
        virtual rational get_value(var_t v) = 0;
        virtual void restore_bound() = 0;   
        virtual void add_le(var_t v, var_t w, unsigned dep) = 0;
        virtual void add_lt(var_t v, var_t w, unsigned dep) = 0;
        virtual void restore_ineq() = 0;
        virtual bool inconsistent() const = 0;
        virtual unsigned_vector const& get_unsat_core() const = 0;
        virtual void updt_params(params_ref const& p) = 0;

    };

    struct ineq {
        var_t v = UINT_MAX;
        var_t w = UINT_MAX;
        bool strict = false;
        u_dependency* dep = nullptr;
        ineq(var_t v, var_t w, u_dependency* dep, bool s) :
            v(v), w(w), strict(s), dep(dep) {}

        std::ostream& display(std::ostream& out) const {
            return out << "v" << v << (strict ? " < v" : " <= v") << w;
        }
    };

    template<typename Ext>
    class fixplex : public fixplex_base {
    public:
        typedef typename Ext::numeral numeral;
        typedef typename Ext::scoped_numeral scoped_numeral;
        typedef typename Ext::manager manager;
        typedef simplex::sparse_matrix<Ext> matrix;
        typedef typename matrix::row row;
        typedef typename matrix::row_iterator row_iterator;
        typedef typename matrix::col_iterator col_iterator;

        struct var_eq {
            var_t x, y;
            u_dependency* dep;
            var_eq(var_t x, var_t y, u_dependency* dep):
                x(x), y(y), dep(dep) {}
        };

    protected:
        struct var_lt {
            bool operator()(var_t v1, var_t v2) const { return v1 < v2; }
        };
        typedef heap<var_lt> var_heap;

        struct stats {
            unsigned m_num_pivots;
            unsigned m_num_infeasible;
            unsigned m_num_checks;
            unsigned m_num_approx;
            stats() { reset(); }
            void reset() {
                memset(this, 0, sizeof(*this));
            }
        };

        enum pivot_strategy_t {
            S_BLAND,
            S_GREATEST_ERROR,
            S_LEAST_ERROR,
            S_DEFAULT
        };

        struct var_info : public mod_interval<numeral> {
            unsigned      m_base2row:29;
            unsigned      m_is_base:1;
            numeral       m_value = 0;
            u_dependency* m_lo_dep = nullptr;
            u_dependency* m_hi_dep = nullptr;
            var_info():
                m_base2row(0),
                m_is_base(false)
            {}
            ~var_info() override {}
            var_info& operator&=(mod_interval<numeral> const& range) {
                mod_interval<numeral>::operator=(range & *this);
                return *this;
            }
            var_info& operator=(mod_interval<numeral> const& range) {
                mod_interval<numeral>::operator=(range);
                return *this;                
            }
        };

        struct row_info {
            bool    m_integral { true };
            var_t   m_base;
            numeral m_value;
            numeral m_base_coeff;            
        };

        struct stashed_bound : var_info {
            var_t m_var;
            stashed_bound(var_t v, var_info const& i):
                var_info(i),
                m_var(v)
            {}
        };

        struct fix_entry {
            var_t x = null_var;
            u_dependency* dep = nullptr;
            fix_entry(var_t x, u_dependency* dep): x(x), dep(dep) {}
            fix_entry() {}
        };

        enum trail_i {
            inc_level_i,
            set_bound_i,
            set_inconsistent_i,
            add_ineq_i,
            add_row_i,
            add_eq_i,
            fixed_val_i
        };

        static const var_t null_var = UINT_MAX;
        reslimit&                   m_limit;
        mutable manager             m;
        mutable matrix              M;
        unsigned                    m_max_iterations = UINT_MAX;
        unsigned                    m_num_non_integral = 0;
        uint_set                    m_non_integral;
        var_heap                    m_to_patch;
        vector<var_info>            m_vars;
        vector<row_info>            m_rows;

        bool                        m_bland = false ;
        unsigned                    m_blands_rule_threshold = 1000;
        unsigned                    m_num_repeated = 0;
        random_gen                  m_random;
        uint_set                    m_left_basis;
        unsigned_vector             m_unsat_core; 
        bool                        m_inconsistent = false;
        unsigned_vector             m_base_vars;
        stats                       m_stats;
        vector<stashed_bound>       m_stashed_bounds;
        u_dependency_manager        m_deps;
        svector<trail_i>            m_trail;
        svector<var_t>              m_row_trail;

        // euqality propagation
        union_find_default_ctx      m_union_find_ctx;
        union_find<>                m_union_find;
        vector<var_eq>              m_var_eqs;
        vector<numeral>             m_fixed_vals;
        map<numeral, fix_entry, typename manager::hash, typename manager::eq> m_value2fixed_var;
        uint_set                    m_eq_rows;

        // inequalities
        svector<ineq>               m_ineqs;
        uint_set                    m_ineqs_to_propagate;
        uint_set                    m_touched_vars;
        vector<unsigned_vector>     m_var2ineqs;

        // bound propagation
        uint_set                    m_bound_rows;

    public:
        fixplex(params_ref const& p, reslimit& lim):
            m_limit(lim),
            M(m),
            m_to_patch(1024),
            m_union_find(m_union_find_ctx) {
            updt_params(p);
        }

        ~fixplex() override;

        void push() override;
        void pop(unsigned n) override;
        bool inconsistent() const override { return m_inconsistent; }
        void updt_params(params_ref const& p) override;

        lbool make_feasible() override;
        void add_row(var_t base, unsigned num_vars, var_t const* vars, rational const* coeffs) override;
        std::ostream& display(std::ostream& out) const override;
        void collect_statistics(::statistics & st) const override;
        void del_row(var_t base_var) override;
        void set_bounds(var_t v, rational const& lo, rational const& hi, unsigned dep) override;
        void set_value(var_t v, rational const& val, unsigned dep) override;
        rational get_value(var_t v) override;
        void restore_bound() override;
        void add_le(var_t v, var_t w, unsigned dep) override { add_ineq(v, w, dep, false); }
        void add_lt(var_t v, var_t w, unsigned dep) override { add_ineq(v, w, dep, true); }
        virtual void restore_ineq() override;

        void set_bounds(var_t v, numeral const& lo, numeral const& hi, unsigned dep);
        void update_bounds(var_t v, numeral const& l, numeral const& h, u_dependency* dep);
        void unset_bounds(var_t v) { m_vars[v].set_free(); }


        numeral const& lo(var_t var) const { return m_vars[var].lo; }
        numeral const& hi(var_t var) const { return m_vars[var].hi; }
        numeral const& value(var_t var) const { return m_vars[var].m_value; }
        void set_max_iterations(unsigned n) { m_max_iterations = n; }
        unsigned get_num_vars() const { return m_vars.size(); }
        void reset();

        vector<var_eq> const& var_eqs() const { return m_var_eqs; }

        void add_row(var_t base, unsigned num_vars, var_t const* vars, numeral const* coeffs);
        
        unsigned_vector const& get_unsat_core() const override { return m_unsat_core; }

    private:

        svector<std::pair<unsigned, unsigned>> stack;
        uint_set on_stack;
        lbool propagate_ineqs(unsigned idx);

        std::ostream& display_row(std::ostream& out, row const& r, bool values = true) const;
        var_t get_base_var(row const& r) const { return m_rows[r.id()].m_base; }

        void update_value_core(var_t v, numeral const& delta);
        void ensure_var(var_t v);

        bool patch();
        bool propagate_ineqs();
        bool propagate_row_eqs();
        bool propagate_row_bounds();
        bool is_satisfied();

        struct backoff {
            unsigned m_tries = 0;
            unsigned m_delay = 1;
            bool should_propagate() {
                return ++m_tries >= m_delay;
            }
            void update(bool progress) {
                m_tries = 0;
                if (progress)
                    m_delay = 1;
                else
                    ++m_delay;
            }
        };

        backoff m_propagate_eqs_backoff;
        backoff m_propagate_bounds_backoff;



        var_t select_smallest_var() { return m_to_patch.empty()?null_var:m_to_patch.erase_min(); }
        lbool make_var_feasible(var_t x_i);
        bool is_infeasible_row(var_t x);
        bool is_parity_infeasible_row(var_t x);
        bool is_offset_row(row const& r, numeral& cx, var_t& x, numeral& cy, var_t & y) const;
        void lookahead_eq(row const& r1, numeral const& cx, var_t x, numeral const& cy, var_t y);
        void get_offset_eqs(row const& r);
        void fixed_var_eh(u_dependency* dep, var_t x);
        var_t find(var_t x) { return m_union_find.find(x); }
        void merge(var_t x, var_t y) { m_union_find.merge(x, y); }
        void eq_eh(var_t x, var_t y, u_dependency* dep);
        bool propagate_row(row const& r);
        bool propagate_ineq(ineq const& i);
        bool propagate_strict_bounds(ineq const& i);
        bool propagate_non_strict_bounds(ineq const& i);
        bool new_bound(row const& r, var_t x, mod_interval<numeral> const& range);
        bool new_bound(ineq const& i, var_t x, numeral const& lo, numeral const& hi, u_dependency* a = nullptr, u_dependency* b = nullptr, u_dependency* c = nullptr, u_dependency* d = nullptr);
        void conflict(ineq const& i, u_dependency* a = nullptr, u_dependency* b = nullptr, u_dependency* c = nullptr, u_dependency* d = nullptr);
        void conflict(u_dependency* a);
        void conflict(u_dependency* a, u_dependency* b, u_dependency* c = nullptr, u_dependency* d = nullptr) { conflict(m_deps.mk_join(m_deps.mk_join(a, b), m_deps.mk_join(c, d))); }
        u_dependency* row2dep(row const& r);
        void pivot(var_t x_i, var_t x_j, numeral const& b, numeral const& value);
        numeral value2delta(var_t v, numeral const& new_value) const;
        numeral value2error(var_t v, numeral const& new_value) const;
        void update_value(var_t v, numeral const& delta);
        bool can_pivot(var_t x_i, numeral const& new_value, numeral const& a_ij, var_t x_j);
        bool has_minimal_trailing_zeros(var_t y, numeral const& b);
        var_t select_pivot(var_t x_i, numeral const& new_value, numeral& out_b);
        var_t select_pivot_core(var_t x, numeral const& new_value, numeral& out_b);
        bool in_bounds(var_t v) const { return in_bounds(v, value(v)); }
        bool in_bounds(var_t v, numeral const& b) const { return in_bounds(b, m_vars[v]); }
        bool in_bounds(numeral const& val, mod_interval<numeral> const& range) const { return range.contains(val); }
        bool is_free(var_t v) const { return lo(v) == hi(v); }
        bool is_non_free(var_t v) const { return !is_free(v); }
        bool is_fixed(var_t v) const { return lo(v) + 1 == hi(v); }
        bool is_valid_variable(var_t v) const { return v < m_vars.size(); }
        bool is_base(var_t x) const { return m_vars[x].m_is_base; }
        row base2row(var_t x) const { return row(m_vars[x].m_base2row); }
        numeral const& row2value(row const& r) const { return m_rows[r.id()].m_value; }
        numeral const& row2base_coeff(row const& r) const { return m_rows[r.id()].m_base_coeff; }
        var_t row2base(row const& r) const { return m_rows[r.id()].m_base; }
        bool row_is_integral(row const& r) const { return m_rows[r.id()].m_integral; }
        void set_base_value(var_t x); 
        numeral solve_for(numeral const& row_value, numeral const& coeff);
        int get_num_non_free_dep_vars(var_t x_j, int best_so_far);
        void add_patch(var_t v);
        var_t select_var_to_fix();
        void check_blands_rule(var_t v);
        pivot_strategy_t pivot_strategy() { return m_bland ? S_BLAND : S_DEFAULT; }
        var_t select_error_var(bool least);
        void set_infeasible_base(var_t v);
        void set_infeasible_bounds(var_t v);

        u_dependency* mk_leaf(unsigned dep) { return UINT_MAX == dep ? nullptr : m_deps.mk_leaf(dep); }

        // facilities for handling inequalities
        void add_ineq(var_t v, var_t w, unsigned dep, bool strict);
        void touch_var(var_t x);


        bool is_solved(row const& r) const;
        bool is_solved(var_t v) const { SASSERT(is_base(v)); return is_solved(base2row(v)); }

        bool well_formed() const;                 
        bool well_formed_row(row const& r) const;

        void  del_row(row const& r);

        var_t select_pivot_blands(var_t x, numeral const& new_value, numeral& out_b);
        bool can_improve(var_t x, numeral const& new_value, var_t y, numeral const& b);

        bool pivot_base_vars();
        bool elim_base(var_t v);

        bool eliminate_var(
            row const& r_y,
            col_iterator const& z_col,
            unsigned tz_b,
            numeral const& old_value_y);
    };

    template<typename uint_type>
    struct generic_uint_ext {
        typedef uint_type numeral;

        struct manager {
            typedef uint_type numeral;
            struct hash {
                unsigned operator()(numeral const& n) const { 
                    return numeral2hash(n); 
                }
            };
            struct eq {
                bool operator()(numeral const& a, numeral const& b) const {
                    return a == b;
                }
            };
            numeral from_rational(rational const& n) { return static_cast<uint_type>(n.get_uint64()); }
            rational to_rational(numeral const& n) const { return ::to_rational(n); }
            void reset() {}
            void reset(numeral& n) { n = 0; }
            void del(numeral const& n) {}
            bool is_zero(numeral const& n) const { return n == 0; }
            bool is_one(numeral const& n) const { return n == 1; }
            bool is_even(numeral const& n) const { return (n & 1) == 0; }
            bool is_minus_one(numeral const& n) const { return n + 1 == 0; }
            void add(numeral const& a, numeral const& b, numeral& r) { r = a + b; }
            void sub(numeral const& a, numeral const& b, numeral& r) { r = a - b; }
            void mul(numeral const& a, numeral const& b, numeral& r) { r = a * b; }
            void set(numeral& r, numeral const& a) { r = a; }
            void neg(numeral& a) { a = 0 - a; }
            numeral inv(numeral const& a) { return 0 - a; }
            void swap(numeral& a, numeral& b) { std::swap(a, b); }
            unsigned trailing_zeros(numeral const& a) const { return ::trailing_zeros(a); }
            numeral mul_inverse(numeral const& x) {
                if (x == 0)
                    return 0;
                numeral t0 = 1, t1 = 0 - 1;
                numeral r0 = x, r1 = 0 - x;
                while (r1 != 0) {
                    numeral q = r0 / r1;
                    numeral tmp = t1;
                    t1 = t0 - q * t1;
                    t0 = tmp;
                    tmp = r1;
                    r1 = r0 - q * r1;
                    r0 = tmp;
                }
                return t0;
            }
            numeral gcd(numeral x, numeral y) {
                if (x == 0) 
                    return y;
                if (y == 0)
                    return x;
                unsigned tz = trailing_zeros(x);
                numeral shift = std::min(trailing_zeros(y), tz);
                x >>= tz;
                if (x == 1) 
                    return x << shift;
                if (y == 1) 
                    return y << shift;
                if (x == y) 
                    return x << shift;
                do {
                    tz = trailing_zeros(y);
                    y >>= tz;
                    if (x > y) 
                        swap(x, y);
                    y -= x;
                }
                while (y != 0);
                return x << shift;
            }

            std::ostream& display(std::ostream& out, numeral const& x) const { 
                return out << pp(x); 
            }
        };
        typedef _scoped_numeral<manager> scoped_numeral;

    };

    typedef generic_uint_ext<uint64_t> uint64_ext;


    template<typename Ext>
    inline std::ostream& operator<<(std::ostream& out, fixplex<Ext> const& fp) {
        return fp.display(out);
    }


    inline std::ostream& operator<<(std::ostream& out, ineq const& i) {
        return i.display(out);
    }
 


};

