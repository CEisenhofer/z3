/*++
  Copyright (c) 2017 Microsoft Corporation

  Author:
   Lev Nachmanson (levnach)
   Nikolaj Bjorner (nbjorner)
  --*/
#pragma once

namespace nla {
    class core;
    class monotone : common {    
    public:
        monotone(core *core);
        void monotonicity_lemma();
        
    private:
        void monotonicity_lemma(monic const& m);
        void monotonicity_lemma_gt(const monic& m);    
        void monotonicity_lemma_lt(const monic& m);
    };
}
