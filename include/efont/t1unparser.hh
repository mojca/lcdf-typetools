#ifndef T1UNPARSER_HH
#define T1UNPARSER_HH
#include "t1interp.hh"
#include "straccum.hh"
#include "string.hh"

class CharstringUnparser : public CharstringInterp { public:

    CharstringUnparser();
    CharstringUnparser(const CharstringUnparser &);

    const String &indent() const		{ return _indent; }
    void set_indent(const String &s)		{ _indent = s; }
    void set_one_command_per_line(bool b)	{ _one_command_per_line = b; }

    void clear();

    bool number(double);
    bool type1_command(int);
    bool type2_command(int, const unsigned char *, int *);
    void char_hintmask(int, const unsigned char *, int);
    void char_cntrmask(int, const unsigned char *, int);

    String value();

    static String unparse(const Charstring *);
    static String unparse(const Charstring &);
  
  private:

    String _indent;
    bool _one_command_per_line;
    bool _start_of_line;
    StringAccum _sa;

};

#endif
