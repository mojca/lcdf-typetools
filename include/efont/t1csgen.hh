#ifndef LIBEFONT_T1CSGEN_HH
#define LIBEFONT_T1CSGEN_HH
#include "t1interp.hh"
#include "straccum.hh"

class Type1CharstringGen { public:

    Type1CharstringGen(int precision = 5);

    void clear();
    char *data() const			{ return _ncs.data(); }
    int length() const			{ return _ncs.length(); }

    void gen_number(double, int = 0);
    void gen_command(int);
    void gen_stack(CharstringInterp &, int for_cmd);

    Type1Charstring *output();
    void output(Type1Charstring &);

  private:
  
    StringAccum _ncs;
    int _precision;
    double _f_precision;
    
    double _true_x;
    double _true_y;
    double _false_x;
    double _false_y;
  
};

#endif
