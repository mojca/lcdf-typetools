#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include "t1font.hh"
#include "t1item.hh"
#include "t1rw.hh"
#include "t1mm.hh"
#include "error.hh"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static PermString::Initializer initializer;
static PermString lenIV_str = "lenIV";
static PermString FontInfo_str = "FontInfo";

Type1Font::Type1Font(Type1Reader &reader)
  : _cached_defs(false), _glyph_map(-1), _encoding(0),
    _cached_mmspace(0), _mmspace(0),
    _synthetic_item(0)
{
  _dict = new HashMap<PermString, Type1Definition *>[dLast];
  for (int i = 0; i < dLast; i++) {
    _index[i] = -1;
    _dict_deltas[i] = 0;
    _dict[i].set_default_value((Type1Definition *)0);
  }
  
  Dict cur_dict = dFont;
  int eexec_state = 0;
  bool have_subrs = false;
  bool have_charstrings = false;
  int lenIV = 4;
  Type1SubrGroupItem *cur_group = 0;
  int cur_group_count = 0;
  
  StringAccum accum;
  while (reader.next_line(accum)) {
    
    // check for NULL STRING
    int x_length = accum.length();
    if (!x_length) continue;
    accum.push(0);		// ensure we don't run off the string
    char *x = accum.data();
    
    // check for CHARSTRINGS
    if (reader.was_charstring()) {
      Type1Subr *fcs = Type1Subr::make(x, x_length, reader.charstring_start(),
				       reader.charstring_length(), lenIV);
      
      if (fcs->is_subr()) {
	if (fcs->subrno() >= _subrs.size())
	  _subrs.resize(fcs->subrno() + 30, (Type1Subr *)0);
	_subrs[fcs->subrno()] = fcs;
	if (!have_subrs && _items.size()) {
	  if (Type1CopyItem *copy = _items.back()->cast_copy()) {
	    cur_group = new Type1SubrGroupItem
	      (this, true, copy->take_value(), copy->length());
	    cur_group_count = 0;
	    _items.back() = cur_group;
	    delete copy;
	  }
	  have_subrs = true;
	}
	
      } else {
	int num = _glyphs.size();
	_glyphs.push_back(fcs);
	_glyph_map.insert(fcs->name(), num);
	if (!have_charstrings && _items.size()) {
	  if (Type1CopyItem *copy = _items.back()->cast_copy()) {
	    cur_group = new Type1SubrGroupItem
	      (this, false, copy->take_value(), copy->length());
	    cur_group_count = 0;
	    _items.back() = cur_group;
	    delete copy;
	  }
	  have_charstrings = true;
	}
      }
      
      accum.clear();
      continue;
    }
    
    // check for COMMENTS
    if (x[0] == '%') {
      _items.push_back(new Type1CopyItem(accum.take(), x_length));
      continue;
    }
    
    // check for CHARSTRING START
    // 5/29/1999: beware of charstring start-like things that don't have
    // `readstring' in them!
    if (!_charstring_definer
	&& strstr(x, "string currentfile") != 0
	&& strstr(x, "readstring") != 0) {
      char *sb = x;
      while (*sb && *sb != '/') sb++;
      char *se = sb + 1;
      while (*sb && *se && *se != ' ' && *se != '{') se++;
      if (*sb && *se) {
	_charstring_definer = permprintf(" %*s ", se - sb - 1, sb + 1);
	reader.set_charstring_definer(_charstring_definer);
	_items.push_back(new Type1CopyItem(accum.take(), x_length));
	continue;
      }
    }
    
    // check for ENCODING
    if (!_encoding && strncmp(x, "/Encoding ", 10) == 0) {
      read_encoding(reader, x + 10);
      accum.clear();
      continue;
    }
    
    // check for a DEFINITION
    if (x[0] == '/') {
     definition_succeed:
      Type1Definition *fdi = Type1Definition::make(accum, &reader);
      if (!fdi) goto definition_fail;
      
      if (fdi->name() == lenIV_str)
	fdi->value_int(lenIV);
      
      _dict[cur_dict].insert(fdi->name(), fdi);
      if (_index[cur_dict] < 0) _index[cur_dict] = _items.size();
      _items.push_back(fdi);
      accum.clear();
      continue;
    } else if (x[0] == ' ') {
      char *y;
      for (y = x; y[0] == ' '; y++) ;
      if (y[0] == '/') goto definition_succeed;
    }
    
   definition_fail:
    
    // check for ZEROS special case
    if (eexec_state == 2) {
      // In eexec_state 2 (right after turning off eexec), the opening part
      // of the string will have some 0 bytes followed by '0's.
      // Change the 0 bytes into textual '0's.
      int zeros = 0;
      while (x[zeros] == 0 && x_length > 0)
	zeros++, x_length--;
      char *zeros_str = new char[zeros * 2 + x_length];
      memset(zeros_str, '0', zeros * 2 + x_length);
      _items.push_back(new Type1CopyItem(zeros_str, zeros * 2 + x_length));
      eexec_state = 3;
      accum.clear();
      continue;
    }

    // check for MODIFIED FONT
    if (eexec_state == 1 && strstr(x, "FontDirectory") != 0) {
      accum.pop();
      if (read_synthetic_font(reader, x, accum)) {
	accum.clear();
	continue;
      }
      accum.push(0);
    }

    // check for END-OF-CHARSTRING-GROUP TEXT
    if (cur_group) {
      if (cur_group_count == 0
	  || ((strstr(x, "end") != 0 || strstr(x, "put") != 0)
	      && strchr(x, '/') == 0)) {
	//fprintf(stderr, "++ %s\n", x);
	cur_group->add_end_text(x);
	cur_group_count++;
	accum.clear();
	continue;
      }
      cur_group = 0;
    }
    
    // add COPY ITEM
    x = accum.take();
    _items.push_back(new Type1CopyItem(x, x_length));

    if (eexec_state == 0 && strncmp(x, "currentfile eexec", 17) == 0) {
      // allow arbitrary whitespace after "currentfile eexec".
      // note: strlen("currentfile eexec") == 17
      while (isspace(x[17])) x++;
      if (!x[17]) {
	reader.switch_eexec(true);
	_items.push_back(new Type1EexecItem(true));
	eexec_state = 1;
      }
    } else if (eexec_state == 1 && strstr(x, "currentfile closefile") != 0) {
      reader.switch_eexec(false);
      _items.push_back(new Type1EexecItem(false));
      eexec_state = 2;
    } else if (strstr(x, "begin") != 0) {
      // 30.Sep.2002: NuevaMM's BlendFontInfo dict starts with a simple
      // "/FontInfo ... begin" inside a "/Blend ... begin".
      Dict was_dict = cur_dict;
      if (strstr(x, "/Private") != 0)
	cur_dict = dPrivate;
      else if (strstr(x, "/FontInfo") != 0)
	cur_dict = dFontInfo;
      else
	cur_dict = dFont;
      if (strstr(x, "/Blend") != 0)
	cur_dict = (Dict)(cur_dict + dBlend);
      else if (was_dict == dBlend && cur_dict == dFontInfo)
	cur_dict = (Dict)(cur_dict + dBlend);
    } else if (cur_dict == dFontInfo && strstr(x, "end") != 0)
      cur_dict = dFont;
  }

  // set dictionary deltas
  for (int i = dFI; i < dLast; i++)
    _dict_deltas[i] = get_dict_size(i) - _dict[i].size();
  // borrow glyphs and glyph map from _synthetic_item
  if (!_glyphs.size() && _synthetic_item) {
    _glyphs = _synthetic_item->included_font()->_glyphs;
    _glyph_map = _synthetic_item->included_font()->_glyph_map;
  }
}

Type1Font::~Type1Font()
{
  delete[] _dict;
  for (int i = 0; i < _items.size(); i++)
    delete _items[i];
  delete _mmspace;
  for (int i = 0; i < _subrs.size(); i++)
    delete _subrs[i];
  if (!_synthetic_item)
    for (int i = 0; i < _glyphs.size(); i++)
      delete _glyphs[i];
}

bool
Type1Font::ok() const
{
  return font_name() && _glyphs.size() > 0;
}


void
Type1Font::read_encoding(Type1Reader &reader, const char *first_line)
{
  while (*first_line == ' ') first_line++;
  if (strncmp(first_line, "StandardEncoding", 16) == 0) {
    _encoding = Type1Encoding::standard_encoding();
    _items.push_back(_encoding);
    return;
  }
  
  _encoding = new Type1Encoding;
  _items.push_back(_encoding);
  
  bool got_any = false;
  StringAccum accum;
  while (reader.next_line(accum)) {
    
    // check for NULL STRING
    if (!accum.length()) continue;
    accum.push(0);		// ensure we don't run off the string
    char *pos = accum.data();
    
    // skip to first `dup' token
    if (!got_any) {
      pos = strstr(pos, "dup");
      if (!pos) {
	accum.clear();
	continue;
      }
    }
    
    // parse as many `dup INDEX */CHARNAME put' as there are in the line
    while (1) {
      // skip spaces, look for `dup '
      while (isspace(pos[0])) pos++;
      if (pos[0] != 'd' || pos[1] != 'u' || pos[2] != 'p' || !isspace(pos[3]))
	break;
      
      // look for `INDEX */'
      char *scan;
      int char_value = strtol(pos + 4, &scan, 10);
      while (scan[0] == ' ') scan++;
      if (char_value < 0 || char_value >= 256 || scan[0] != '/')
	break;
      
      // look for `CHARNAME put'
      scan++;
      char *name_pos = scan;
      while (scan[0] != ' ' && scan[0]) scan++;
      char *name_end = scan;
      while (scan[0] == ' ') scan++;
      if (scan[0] != 'p' || scan[1] != 'u' || scan[2] != 't')
	break;
      
      _encoding->put(char_value, PermString(name_pos, name_end - name_pos));
      got_any = true;
      pos = scan + 3;
    }
    
    // add COPY ITEM if necessary for leftovers we didn't parse
    if (got_any && *pos) {
      int len = strlen(pos);
      char *copy = new char[len + 1];
      strcpy(copy, pos);
      _items.push_back(new Type1CopyItem(copy, len));
    }
    
    // check for end of encoding section
    if (strstr(pos, "readonly") != 0 || strstr(pos, "def") != 0)
      return;
    
    accum.clear();
  }
}

static bool
read_synthetic_string(Type1Reader &reader, StringAccum &wrong_accum,
		      const char *format, int *value)
{
  StringAccum accum;
  if (!reader.next_line(accum))
    return false;
  wrong_accum << accum;
  accum.push(0);		// ensure we don't run off the string
  int n = 0;
  if (value)
    sscanf(accum.data(), format, value, &n);
  else
    sscanf(accum.data(), format, &n);
  return (n != 0 && (isspace(accum[n]) || accum[n] == '\0'));
}

bool
Type1Font::read_synthetic_font(Type1Reader &reader, const char *first_line,
			       StringAccum &wrong_accum)
{
  // read font name
  PermString font_name;
  {
    char *x = new char[strlen(first_line) + 1];
    int n = 0;
    sscanf(first_line, "FontDirectory /%[^] \t\r\n[{}/] known {%n", x, &n);
    if (n && (isspace(first_line[n]) || first_line[n] == 0))
      font_name = x;
    delete[] x;
    if (!font_name)
      return false;
  }

  // check UniqueID
  int unique_id;
  {
    StringAccum accum;
    if (!reader.next_line(accum))
      return false;
    wrong_accum << accum;
    accum.push(0);		// ensure we don't run off the string
    const char *y = accum.data();
    if (*y != '/' || strncmp(y + 1, font_name.cc(), font_name.length()) != 0)
      return false;
    int n = 0;
    sscanf(y + font_name.length() + 1, " findfont%n", &n);
    y = strstr(y, "/UniqueID get ");
    if (n == 0 || y == 0)
      return false;
    n = 0;
    sscanf(y + 14, "%d%n", &unique_id, &n);
    if (n == 0)
      return false;
  }

  // check lines that say how much text
  int multiplier;
  if (!read_synthetic_string(reader, wrong_accum, "save userdict /fbufstr %d string put%n", &multiplier))
    return false;

  int multiplicand;
  if (!read_synthetic_string(reader, wrong_accum, "%d {currentfile fbufstr readstring { pop } { clear currentfile%n", &multiplicand))
    return false;

  if (!read_synthetic_string(reader, wrong_accum, "closefile /fontdownload /unexpectedEOF /.error cvx exec } ifelse } repeat%n", 0))
    return false;

  int extra;
  if (!read_synthetic_string(reader, wrong_accum, "currentfile %d string readstring { pop } { clear currentfile%n", &extra))
    return false;

  if (!read_synthetic_string(reader, wrong_accum, "closefile /fontdownload /unexpectedEOF /.error cvx exec } ifelse%n", 0))
    return false;

  if (!read_synthetic_string(reader, wrong_accum, "restore } if } if%n", 0))
    return false;

  Type1SubsetReader subreader(&reader, multiplier*multiplicand + extra);
  Type1Font *synthetic = new Type1Font(subreader);
  if (!synthetic->ok())
    delete synthetic;
  else {
    _synthetic_item = new Type1IncludedFont(synthetic, unique_id);
    _items.push_back(_synthetic_item);
  }
  return true;
}


void
Type1Font::undo_synthetic()
{
  // A synthetic font doesn't share arbitrary code with its base font; it
  // shares just the CharStrings dictionary, according to Adobe Type 1 Font
  // Format. We take advantage of this.
  
  if (!_synthetic_item)
    return;
  
  int mod_ii;
  for (mod_ii = nitems() - 1; mod_ii >= 0; mod_ii--)
    if (_items[mod_ii] == _synthetic_item)
      break;
  if (mod_ii < 0)
    return;

  // remove synthetic item and the reference to the included font
  _items[mod_ii] = new Type1NullItem;
  if (Type1CopyItem *copy = _items[mod_ii+1]->cast_copy())
    if (strstr(copy->value(), "findfont") != 0) {
      delete copy;
      _items[mod_ii+1] = new Type1NullItem;
    }

  Type1Font *f = _synthetic_item->included_font();
  // its glyphs are already stored in our _glyphs array

  // copy SubrGroupItem from `f' into `this'
  Type1SubrGroupItem *oth_subrs = 0, *oth_glyphs = 0;
  for (int i = 0; i < f->nitems(); i++)
    if (Type1SubrGroupItem *subr_grp = f->_items[i]->cast_subr_group()) {
      if (subr_grp->is_subrs())
	oth_subrs = subr_grp;
      else
	oth_glyphs = subr_grp;
    }

  assert(oth_glyphs);
  
  for (int i = nitems() - 1; i >= 0; i--)
    if (Type1SubrGroupItem *subr_grp = _items[i]->cast_subr_group()) {
      assert(subr_grp->is_subrs());
      if (oth_subrs)
	subr_grp->set_end_text(oth_subrs->end_text());
      shift_indices(i + 1, 1);
      Type1SubrGroupItem *nsubr = new Type1SubrGroupItem(*oth_glyphs, this);
      _items[i + 1] = nsubr;
      break;
    }

  // delete included font
  f->_glyphs.clear();		// don't delete glyphs; we've stolen them
  delete _synthetic_item;
  _synthetic_item = 0;
}


Type1Charstring *
Type1Font::subr(int e) const
{
  if (e >= 0 && e < _subrs.size() && _subrs[e])
    return &_subrs[e]->t1cs();
  else
    return 0;
}


bool
Type1Font::set_subr(int e, const Type1Charstring &t1cs)
{
  if (e < 0) return false;
  if (e >= _subrs.size())
    _subrs.resize(e + 30, (Type1Subr *)0);
  
  Type1Subr *pattern_subr = _subrs[e];
  if (!pattern_subr) {
    for (int i = 0; i < _subrs.size() && !pattern_subr; i++)
      pattern_subr = _subrs[e];
  }
  if (!pattern_subr)
    return false;
  
  delete _subrs[e];
  _subrs[e] = Type1Subr::make_subr(e, pattern_subr->definer(), t1cs);
  return true;
}

bool
Type1Font::remove_subr(int e)
{
  if (e < 0 || e >= _subrs.size()) return false;
  delete _subrs[e];
  _subrs[e] = 0;
  return true;
}


Type1Charstring *
Type1Font::glyph(PermString name) const
{
  int i = _glyph_map[name];
  if (i >= 0)
    return &_glyphs[i]->t1cs();
  else
    return 0;
}

void
Type1Font::shift_indices(int move_index, int delta)
{
  if (delta > 0) {
    _items.resize(_items.size() + delta, (Type1Item *)0);
    memmove(&_items[move_index + delta], &_items[move_index],
	    sizeof(Type1Item *) * (_items.size() - move_index - delta));
    
    for (int i = dFont; i < dLast; i++)
      if (_index[i] > move_index)
	_index[i] += delta;
    
  } else {
    memmove(&_items[move_index], &_items[move_index - delta],
	    sizeof(Type1Item *) * (_items.size() - (move_index - delta)));
    _items.resize(_items.size() + delta);
    
    for (int i = dFont; i < dLast; i++)
      if (_index[i] >= move_index) {
	if (_index[i] < move_index - delta)
	  _index[i] = move_index;
	else
	  _index[i] += delta;
      }
  }
}

Type1Definition *
Type1Font::ensure(Dict dict, PermString name)
{
  assert(_index[dict] >= 0);
  Type1Definition *def = _dict[dict][name];
  if (!def) {
    def = new Type1Definition(name, 0, "def");
    int move_index = _index[dict];
    shift_indices(move_index, 1);
    _items[move_index] = def;
  }
  return def;
}

void
Type1Font::add_header_comment(const char *comment)
{
  int i;
  for (i = 0; i < _items.size(); i++) {
    Type1CopyItem *copy = _items[i]->cast_copy();
    if (!copy || copy->value()[0] != '%') break;
  }
  shift_indices(i, 1);
  
  int len = strlen(comment);
  char *v = new char[len];
  memcpy(v, comment, len);
  _items[i] = new Type1CopyItem(v, len);
}


Type1Item *
Type1Font::dict_size_item(int d) const
{
  switch (d) {

   case dFI: case dP: case dB:
    if (_index[d] > 0)
      return _items[_index[d] - 1];
    break;

   case dBFI:
    if (Type1Item *t1i = b_dict("FontInfo"))
      return t1i;
    else if (_index[dBFI] > 0)
      return _items[_index[dBFI] - 1];
    break;

   case dBP:
    if (Type1Item *t1i = b_dict("Private"))
      return t1i;
    else if (_index[dBP] > 0)
      return _items[_index[dBP] - 1];
    break;

  }
  return 0;
}

int
Type1Font::get_dict_size(int d) const
{
  Type1Item *item = dict_size_item(d);
  if (!item)
    /* nada */;
  else if (Type1Definition *t1d = item->cast_definition()) {
    int num;
    if (strstr(t1d->definer().cc(), "dict") && t1d->value_int(num))
      return num;
  } else if (Type1CopyItem *copy = item->cast_copy()) {
    char *value = copy->value();
    char *d = strstr(value, " dict");
    if (d && d > value && isdigit(d[-1])) {
      char *c = d - 1;
      while (c > value && isdigit(c[-1]))
	c--;
      return strtol(c, 0, 10);
    }
  }
  return -1;
}

void
Type1Font::set_dict_size(int d, int size)
{
  Type1Item *item = dict_size_item(d);
  if (!item)
    return;
  if (Type1Definition *t1d = item->cast_definition()) {
    int num;
    if (strstr(t1d->definer().cc(), "dict") && t1d->value_int(num))
      t1d->set_int(size);
  } else if (Type1CopyItem *copy = item->cast_copy()) {
    char *value = copy->value();
    char *d = strstr(value, " dict");
    if (d && d > value && isdigit(d[-1])) {
      char *c = d - 1;
      while (c > value && isdigit(c[-1]))
	c--;
      StringAccum accum;
      accum.push(value, c - value);
      accum << size;
      accum.push(d, copy->length() - (d - value));
      int accum_length = accum.length();
      copy->set_value(accum.take(), accum_length);
    }
  }
}

void
Type1Font::write(Type1Writer &w)
{
  Type1Definition *lenIV_def = p_dict("lenIV");
  int lenIV = 4;
  if (lenIV_def)
    lenIV_def->value_int(lenIV);
  w.set_charstring_start(_charstring_definer);
  w.set_lenIV(lenIV);
  
  // change dict sizes
  for (int i = dFI; i < dLast; i++)
    set_dict_size(i, _dict[i].size() + _dict_deltas[i]);
  // XXX what if dict had nothing, but now has something?
  
  for (int i = 0; i < _items.size(); i++)
    _items[i]->gen(w);
  
  w.flush();
}

void
Type1Font::cache_defs() const
{
  Type1Definition *t1d;
  
  t1d = dict("FontName");
  if (t1d) t1d->value_name(_font_name);
  
  _cached_defs = true;
}

Type1MMSpace *
Type1Font::create_mmspace(ErrorHandler *errh) const
{
  if (_cached_mmspace)
    return _mmspace;
  _cached_mmspace = 1;
  
  Type1Definition *t1d;
  
  Vector< Vector<double> > master_positions;
  t1d = fi_dict("BlendDesignPositions");
  if (!t1d || !t1d->value_numvec_vec(master_positions))
    return 0;
  
  int nmasters = master_positions.size();
  if (nmasters <= 0) {
    errh->error("bad BlendDesignPositions");
    return 0;
  }
  int naxes = master_positions[0].size();
  _mmspace = new Type1MMSpace(font_name(), naxes, nmasters);
  _mmspace->set_master_positions(master_positions);
  
  Vector< Vector<double> > normalize_in, normalize_out;
  t1d = fi_dict("BlendDesignMap");
  if (t1d && t1d->value_normalize(normalize_in, normalize_out))
    _mmspace->set_normalize(normalize_in, normalize_out);
  
  Vector<PermString> axis_types;
  t1d = fi_dict("BlendAxisTypes");
  if (t1d && t1d->value_namevec(axis_types) && axis_types.size() == naxes)
    for (int a = 0; a < axis_types.size(); a++)
      _mmspace->set_axis_type(a, axis_types[a]);
  
  int ndv, cdv;
  t1d = p_dict("NDV");
  if (t1d && t1d->value_int(ndv))
    _mmspace->set_ndv(subr(ndv), false);
  t1d = p_dict("CDV");
  if (t1d && t1d->value_int(cdv))
    _mmspace->set_cdv(subr(cdv), false);
  
  Vector<double> design_vector;
  t1d = dict("DesignVector");
  if (t1d && t1d->value_numvec(design_vector))
    _mmspace->set_design_vector(design_vector);
  
  Vector<double> weight_vector;
  t1d = dict("WeightVector");
  if (t1d && t1d->value_numvec(weight_vector))
    _mmspace->set_weight_vector(weight_vector);
  
  if (!_mmspace->check(errh)) {
    delete _mmspace;
    _mmspace = 0;
  }
  return _mmspace;
}
