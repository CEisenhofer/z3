/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    polysat conflict

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-6

Notes:

 TODO: try a final core reduction step or other core minimization

 TODO: revert(pvar v) is too weak. 
       It should apply saturation rules currently only available for propagated values.

 TODO: dependency tracking for constraints evaluating to false should be minimized.

--*/

#include "math/polysat/conflict.h"
#include "math/polysat/solver.h"
#include "math/polysat/log.h"
#include "math/polysat/log_helper.h"
#include "math/polysat/explain.h"
#include "math/polysat/solve_explain.h"
#include "math/polysat/forbidden_intervals.h"
#include "math/polysat/saturation.h"
#include "math/polysat/variable_elimination.h"
#include <algorithm>

namespace polysat {

    conflict::conflict(solver& s):s(s) {
        ex_engines.push_back(alloc(ex_polynomial_superposition, s));
        ex_engines.push_back(alloc(solve_explain, s));
        ve_engines.push_back(alloc(ve_reduction));
        inf_engines.push_back(alloc(inf_saturate, s));
    }

    conflict::~conflict() {}

    constraint_manager& conflict::cm() const { return s.m_constraints; }

    std::ostream& conflict::display(std::ostream& out) const {
        char const* sep = "";
        for (auto c : *this) 
            out << sep << c->bvar2string() << " " << c, sep = " ; ";
        if (!m_vars.empty())
            out << " vars";
        for (auto v : m_vars)
            out << " v" << v;
        if (!m_bail_vars.empty())
            out << " bail vars";
        for (auto v : m_bail_vars)
            out << " v" << v;
        return out;
    }

    void conflict::reset() {
        for (auto c : *this)
            unset_mark(c);
        m_constraints.reset();
        m_literals.reset();
        m_vars.reset();
        m_bail_vars.reset();
        m_conflict_var = null_var;
        m_bailout = false;
        SASSERT(empty());        
    }

    /**
    * The constraint is false under the current assignment of variables.
    * The core is then the conjuction of this constraint and assigned variables.
    */
    void conflict::set(signed_constraint c) {
        LOG("Conflict: " << c << " " << c.bvalue(s));
        SASSERT(empty());
        if (c.bvalue(s) == l_false) {
            auto* cl = s.m_bvars.reason(c.blit().var());
            if (cl)
                set(*cl);
            else
                insert(c);
        }
        else {
            SASSERT(c.is_currently_false(s));
            // TBD: fails with test_subst       SASSERT(c.bvalue(s) == l_true);
            insert_vars(c);
            insert(c);
        }
        SASSERT(!empty());
    }

    /**
    * The variable v cannot be assigned.
    * The conflict is the set of justifications accumulated for the viable values for v.
    * These constraints are (in the current form) not added to the core, but passed directly 
    * to the forbidden interval module.
    * A consistent approach could be to add these constraints to the core and then also include the
    * variable assignments.
    */
    void conflict::set(pvar v) {
        LOG("Conflict: v" << v);
        SASSERT(empty());
        m_conflict_var = v;
        SASSERT(!empty());
    }

    /**
     * The clause is conflicting in the current search state.
     */
    void conflict::set(clause const& cl) {
        if (!empty())
            return;
        LOG("Conflict: " << cl);
        SASSERT(empty());
        for (auto lit : cl) {
            auto c = s.lit2cnstr(lit);
            SASSERT(c.bvalue(s) == l_false);
            insert(~c);            
        }
        SASSERT(!empty());
    }

    /**
     * Insert constraint into conflict state
     * Skip trivial constraints
     *  - e.g., constant ones such as "4 > 1"... only true ones 
     *   should appear, otherwise the lemma would be a tautology
     */
    void conflict::insert(signed_constraint c) {
        if (c.is_always_true())
            return;        
        if (c->is_marked())
            return;
        LOG("inserting: " << c);
        SASSERT(!c->vars().empty());
        set_mark(c);
        if (c->has_bvar())
            insert_literal(c.blit());
        else
            m_constraints.push_back(c);
    }

    void conflict::propagate(signed_constraint c) {
        cm().ensure_bvar(c.get());
        switch (c.bvalue(s)) {
        case l_undef:
            s.assign_eval(c.blit());
            break;
        case l_true:
            break;
        case l_false:            
            break;
        }
        insert(c);
    }

    void conflict::insert_vars(signed_constraint c) {
        for (pvar v : c->vars()) 
            if (s.is_assigned(v)) 
                m_vars.insert(v);  
    }

    /**
     * Premises can either be justified by a Clause or by a value assignment.
     * Premises that are justified by value assignments are not assigned (the bvalue is l_undef)
     * The justification for those premises are based on the free assigned variables.
     *
     * NOTE: maybe we should skip intermediate steps and just collect the leaf premises for c?
     * Ensure that c is assigned and justified
     */
    void conflict::insert(signed_constraint c, vector<signed_constraint> const& premises) {
        keep(c);

        clause_builder c_lemma(s);
        for (auto premise : premises) {
            LOG_H3("premise: " << premise);
            keep(premise);
            SASSERT(premise->has_bvar());
            SASSERT(premise.bvalue(s) != l_false);         
            c_lemma.push(~premise.blit());
        }
        c_lemma.push(c.blit());
        clause_ref lemma = c_lemma.build();
        SASSERT(lemma);
        cm().store(lemma.get(), s, false);
        if (c.bvalue(s) == l_undef)
            s.assign_propagate(c.blit(), *lemma);
    }

    void conflict::remove(signed_constraint c) {
        SASSERT(!c->has_bvar() || std::count(m_constraints.begin(), m_constraints.end(), c) == 0);
        unset_mark(c);       
        if (c->has_bvar())             
            remove_literal(c.blit());        
        else
            m_constraints.erase(c);
    }

    void conflict::replace(signed_constraint c_old, signed_constraint c_new, vector<signed_constraint> const& c_new_premises) {
        remove(c_old);
        insert(c_new, c_new_premises);
    }


    bool conflict::contains(signed_constraint c) {
        if (c->has_bvar())
            return m_literals.contains(c.blit().index());
        else
            return m_constraints.contains(c);
    }

    void conflict::set_bailout() {
        SASSERT(!is_bailout());
        m_bailout = true;
        s.m_stats.m_num_bailouts++;
    }

    void conflict::resolve(sat::literal lit, clause const& cl) {
        // Note: core: x, y, z; corresponds to clause ~x \/ ~y \/ ~z
        //       clause: x \/ u \/ v
        //       resolvent: ~y \/ ~z \/ u \/ v; as core: y, z, ~u, ~v

        SASSERT(lit != sat::null_literal);
        SASSERT(~lit != sat::null_literal);
        SASSERT(std::all_of(m_constraints.begin(), m_constraints.end(), [](signed_constraint const& c){ return !c->has_bvar(); }));
        SASSERT(contains_literal(lit));
        SASSERT(std::count(cl.begin(), cl.end(), lit) > 0);
        SASSERT(!contains_literal(~lit));
        SASSERT(std::count(cl.begin(), cl.end(), ~lit) == 0);
        
        remove_literal(lit);        
        unset_mark(s.lit2cnstr(lit));         
        for (sat::literal lit2 : cl)
            if (lit2 != lit)
                insert(s.lit2cnstr(~lit2));
    }

    void conflict::resolve_with_assignment(sat::literal lit, unsigned lvl) {
        // The reason for lit is conceptually:
        //    x1 = v1 /\ ... /\ xn = vn ==> lit

        SASSERT(lit != sat::null_literal);
        SASSERT(~lit != sat::null_literal);
        SASSERT(std::all_of(m_constraints.begin(), m_constraints.end(), [](signed_constraint const& c){ return !c->has_bvar(); }));
        SASSERT(contains_literal(lit));
        SASSERT(!contains_literal(~lit));

        signed_constraint c = s.lit2cnstr(lit);
        bool has_decision = false;
        for (pvar v : c->vars()) 
            if (s.is_assigned(v) && s.m_justification[v].is_decision()) 
                m_bail_vars.insert(v), has_decision = true;

        if (!has_decision) {
            remove(c);
            for (pvar v : c->vars()) 
                if (s.is_assigned(v)) 
                    m_vars.insert(v);            
        }
    }

    /** 
     * If the constraint c is a temporary constraint derived by core saturation, 
     * insert it (and recursively, its premises) into \Gamma 
     */
    void conflict::keep(signed_constraint c) {
        if (c->has_bvar())
            return;
        LOG_H3("keeping: " << c);
        remove(c);
        cm().ensure_bvar(c.get());
        insert(c);        
    }

    clause_builder conflict::build_lemma() {
        // SASSERT(std::all_of(m_vars.begin(), m_vars.end(), [&](pvar v) { return s.is_assigned(v); }));
        SASSERT(std::all_of(m_constraints.begin(), m_constraints.end(), [](signed_constraint const& c) { return !c->has_bvar(); }));

        LOG_H3("Build lemma from core");
        LOG("core: " << *this);
        clause_builder lemma(s);

        while (!m_constraints.empty()) 
            keep(m_constraints.back());

        for (auto c : *this)
            minimize_vars(c);

        for (auto c : *this)
            lemma.push(~c);

        for (unsigned v : m_vars) {
            auto eq = s.eq(s.var(v), s.get_value(v));
            cm().ensure_bvar(eq.get());
            if (eq.bvalue(s) == l_undef) 
                s.assign_eval(eq.blit());            
            lemma.push(~eq);
        }        
        s.decay_activity();

        return lemma;
    }

    void conflict::minimize_vars(signed_constraint c) {
        if (m_vars.empty())
            return;
        if (!c.is_currently_false(s))
            return;
        
        assignment_t a;
        for (auto v : m_vars)
            a.push_back(std::make_pair(v, s.get_value(v)));
        for (unsigned i = 0; i < a.size(); ++i) {
            std::pair<pvar, rational> save = a[i];
            std::pair<pvar, rational> last = a.back();
            a[i] = last;
            a.pop_back();
            if (c.is_currently_false(s, a)) 
                --i;
            else {
                a.push_back(last);
                a[i] = save;
            }
        }
        if (a.size() == m_vars.num_elems())
            return;
        m_vars.reset();
        for (auto const& [v, val] : a)
            m_vars.insert(v);
        LOG("reduced " << m_vars);
    }


    bool conflict::resolve_value(pvar v) {
        // NOTE:
        // In the "standard" case where "v = val" is on the stack:
        // forbidden interval projection is performed at top level

        SASSERT(v != conflict_var());

        auto const& j = s.m_justification[v];

        if (j.is_decision() && m_bail_vars.contains(v))
            return false;
        
        s.inc_activity(v);    
        m_vars.remove(v);

        if (is_bailout())
            goto bailout;
        
        if (j.is_propagation()) 
            for (auto const& c : s.m_viable.get_constraints(v)) 
                propagate(c);

        LOG("try-explain v" << v);
        if (try_explain(v))
            return true;

        // No value resolution method was successful => fall back to saturation and variable elimination
        while (s.inc()) {
            LOG("try-eliminate v" << v);
            // TODO: as a last resort, substitute v by m_value[v]?
            if (try_eliminate(v))
                return true;
            if (!try_saturate(v))
                break;
        }
        LOG("bailout v" << v);
        set_bailout();
    bailout:
        if (s.is_assigned(v) && j.is_decision())
            m_vars.insert(v);
        return false;
    }

    bool conflict::try_eliminate(pvar v) {    
        LOG("try v" << v << " contains " << m_vars.contains(v));
        if (m_vars.contains(v))
            return false;
        bool has_v = false;
        for (auto c : *this)
            has_v |= c->contains_var(v);
        if (!has_v)
            return true;
        for (auto* engine : ve_engines)
            if (engine->perform(s, v, *this)) 
                return true;            
        return false;
    }

    bool conflict::try_saturate(pvar v) {
        for (auto* engine : inf_engines)
            if (engine->perform(v, *this))
                return true;
        return false;
    }

    bool conflict::try_explain(pvar v) {
        for (auto* engine : ex_engines)
            if (engine->try_explain(v, *this))
                return true;
        return false;
    }

    void conflict::set_mark(signed_constraint c) {
        if (c->is_marked())
            return;
        c->set_mark();
        if (c->has_bvar())
            set_bmark(c->bvar());    
    }

    /**
     * unset marking on the constraint, but keep variable dependencies.
     */
    void conflict::unset_mark(signed_constraint c) {
        if (!c->is_marked())
            return;
        c->unset_mark();
        if (c->has_bvar())
            unset_bmark(c->bvar());
    }

    void conflict::set_bmark(sat::bool_var b) {
        if (b >= m_bvar2mark.size())
            m_bvar2mark.resize(b + 1);
        SASSERT(!m_bvar2mark[b]);
        m_bvar2mark[b] = true;
    }

    void conflict::unset_bmark(sat::bool_var b) {
        SASSERT(m_bvar2mark[b]);
        m_bvar2mark[b] = false;
    }

    bool conflict::is_bmarked(sat::bool_var b) const {
        return m_bvar2mark.get(b, false);
    }

    bool conflict::contains_literal(sat::literal lit) const {
        return m_literals.contains(lit.to_uint());
    }

    void conflict::insert_literal(sat::literal lit) {
        m_literals.insert(lit.to_uint());
    }

    void conflict::remove_literal(sat::literal lit) {
        m_literals.remove(lit.to_uint());
    }

}
