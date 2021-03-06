// -*- mode: C++ -*-
//
// Copyright (C) 2013-2019 Red Hat, Inc.
//
// This file is part of the GNU Application Binary Interface Generic
// Analysis and Instrumentation Library (libabigail).  This library is
// free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License as published by the
// Free Software Foundation; either version 3, or (at your option) any
// later version.

// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Lesser Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this program; see the file COPYING-LGPLV3.  If
// not, see <http://www.gnu.org/licenses/>.

/// @file

#ifndef __ABG_CORPUS_H__
#define __ABG_CORPUS_H__

#include <abg-ir.h>

namespace abigail
{

namespace ir
{

/// This is the abstraction of a set of translation units (themselves
/// seen as bundles of unitary abi artefacts like types and decls)
/// bundled together as a corpus.  A corpus is thus the Application
/// binary interface of a program, a library or just a set of modules
/// put together.
class corpus
{
public:
  struct priv;
  /// Convenience typedef for shared_ptr of corpus::priv
  typedef shared_ptr<priv> priv_sptr;

  /// A convenience typedef for std::vector<string>.
  typedef vector<string> strings_type;

  /// Convenience typedef for std::vector<abigail::ir::function_decl*>
  typedef vector<function_decl*> functions;

  ///Convenience typedef for std::vector<abigail::ir::var_decl*>
  typedef vector<var_decl*> variables;

  class exported_decls_builder;

  /// Convenience typedef for shared_ptr<exported_decls_builder>.
  typedef shared_ptr<exported_decls_builder> exported_decls_builder_sptr;

  /// This abstracts where the corpus comes from.  That is, either it
  /// has been read from the native xml format, from DWARF or built
  /// artificially using the library's API.
  enum origin
  {
    ARTIFICIAL_ORIGIN = 0,
    NATIVE_XML_ORIGIN,
    DWARF_ORIGIN
  };

private:
  corpus();

  void record_canonical_type(const type_base_sptr&) const;
  type_base_sptr lookup_canonical_type(const string& qualified_name) const;

public:
  shared_ptr<priv> priv_;

  corpus(ir::environment*, const string& path= "");

  const environment*
  get_environment() const;

  environment*
  get_environment();

  void
  set_environment(environment*) const;

  void
  add(const translation_unit_sptr);

  const translation_units&
  get_translation_units() const;

  const translation_unit_sptr
  find_translation_unit(const string &path) const;

  void
  drop_translation_units();

  type_maps&
  get_types();

  const type_maps&
  get_types() const;

  type_maps&
  get_type_per_loc_map();

  const type_maps&
  get_type_per_loc_map() const;

  origin
  get_origin() const;

  void
  set_origin(origin);

  string&
  get_path() const;

  void
  set_path(const string&);

  const vector<string>&
  get_needed() const;

  void
  set_needed(const vector<string>&);

  const string&
  get_soname();

  void
  set_soname(const string&);

  const string&
  get_architecture_name() const;

  void
  set_architecture_name(const string&);

  virtual bool
  is_empty() const;

  bool
  operator==(const corpus&) const;

  void
  set_fun_symbol_map(string_elf_symbols_map_sptr);

  void
  set_undefined_fun_symbol_map(string_elf_symbols_map_sptr);

  void
  set_var_symbol_map(string_elf_symbols_map_sptr);

  void
  set_undefined_var_symbol_map(string_elf_symbols_map_sptr);

  const string_elf_symbols_map_sptr
  get_fun_symbol_map_sptr() const;

  const string_elf_symbols_map_type&
  get_fun_symbol_map() const;

  const string_elf_symbols_map_sptr
  get_undefined_fun_symbol_map_sptr() const;

  const string_elf_symbols_map_type&
  get_undefined_fun_symbol_map() const;

  virtual const elf_symbols&
  get_sorted_fun_symbols() const;

  const elf_symbols&
  get_sorted_undefined_fun_symbols() const;

  const string_elf_symbols_map_sptr
  get_var_symbol_map_sptr() const;

  const string_elf_symbols_map_type&
  get_var_symbol_map() const;

  const string_elf_symbols_map_sptr
  get_undefined_var_symbol_map_sptr() const;

  const string_elf_symbols_map_type&
  get_undefined_var_symbol_map() const;

  virtual const elf_symbols&
  get_sorted_var_symbols() const;

  const elf_symbols&
  get_sorted_undefined_var_symbols() const;

  const elf_symbol_sptr
  lookup_function_symbol(const string& n) const;

  const elf_symbol_sptr
  lookup_function_symbol(const string& symbol_name,
			 const elf_symbol::version& version) const;

  const elf_symbol_sptr
  lookup_function_symbol(const elf_symbol& symbol) const;

  const elf_symbol_sptr
  lookup_variable_symbol(const string& n) const;

  const elf_symbol_sptr
  lookup_variable_symbol(const string& symbol_name,
			 const elf_symbol::version& version) const;

  const elf_symbol_sptr
  lookup_variable_symbol(const elf_symbol& symbol) const;

  virtual const functions&
  get_functions() const;

  const vector<function_decl*>*
  lookup_functions(const string& id) const;

  void
  sort_functions();

  virtual const variables&
  get_variables() const;

  void
  sort_variables();

  virtual const elf_symbols&
  get_unreferenced_function_symbols() const;

  virtual const elf_symbols&
  get_unreferenced_variable_symbols() const;

  vector<string>&
  get_regex_patterns_of_fns_to_suppress();

  const vector<string>&
  get_regex_patterns_of_fns_to_suppress() const;

  vector<string>&
  get_regex_patterns_of_vars_to_suppress();

  const vector<string>&
  get_regex_patterns_of_vars_to_suppress() const;

  vector<string>&
  get_regex_patterns_of_fns_to_keep();

  const vector<string>&
  get_regex_patterns_of_fns_to_keep() const;

  vector<string>&
  get_sym_ids_of_fns_to_keep();

  const vector<string>&
  get_sym_ids_of_fns_to_keep() const;

  vector<string>&
  get_regex_patterns_of_vars_to_keep();

  const vector<string>&
  get_regex_patterns_of_vars_to_keep() const;

  vector<string>&
  get_sym_ids_of_vars_to_keep();

  const vector<string>&
  get_sym_ids_of_vars_to_keep() const;

  void
  maybe_drop_some_exported_decls();

  exported_decls_builder_sptr
  get_exported_decls_builder() const;

  friend class type_base;
};// end class corpus.

/// Abstracts the building of the set of exported variables and
/// functions.
///
/// Given a function or variable, this type can decide if it belongs
/// to the list of exported functions and variables based on all the
/// parameters needed.
class corpus::exported_decls_builder
{
public:
  class priv;

  /// Convenience typedef for shared_ptr<priv>
  typedef shared_ptr<priv> priv_sptr;

  friend class corpus;

private:
  priv_sptr priv_;

  // Forbid default construction.
  exported_decls_builder();

public:

  exported_decls_builder(functions& fns,
			 variables& vars,
			 strings_type& fns_suppress_regexps,
			 strings_type& vars_suppress_regexps,
			 strings_type& fns_keep_regexps,
			 strings_type& vars_keep_regexps,
			 strings_type& sym_id_of_fns_to_keep,
			 strings_type& sym_id_of_vars_to_keep);


  const functions&
  exported_functions() const;

  functions&
  exported_functions();

  const variables&
  exported_variables() const;

  variables&
  exported_variables();

  void
  maybe_add_fn_to_exported_fns(function_decl*);

  void
  maybe_add_var_to_exported_vars(var_decl*);
}; //corpus::exported_decls_builder

/// Abstraction of a group of corpora.
///
/// A corpus group is a union of corpora.  It provides a unified view
/// of a set of corpora.  It lets you get the set of functions,
/// variables and symbols that are defined and exported by a set of
/// corpora.
class corpus_group : public corpus
{
  struct priv;
  typedef shared_ptr<priv> priv_sptr;

  priv_sptr priv_;

  // Forbid copy
  corpus_group(const corpus_group&);

public:
  typedef vector<corpus_sptr> corpora_type;

  corpus_group(ir::environment*, const string&);

  virtual ~corpus_group();

  void add_corpus(const corpus_sptr&);

  const corpora_type&
  get_corpora() const;

  virtual bool
  is_empty() const;

  virtual const corpus::functions&
  get_functions() const;

  virtual const corpus::variables&
  get_variables() const;

  const string_elf_symbols_map_type&
  get_var_symbol_map() const;

  const string_elf_symbols_map_type&
  get_fun_symbol_map() const;

  virtual const elf_symbols&
  get_sorted_fun_symbols() const;

  virtual const elf_symbols&
  get_sorted_var_symbols() const;

  virtual const elf_symbols&
  get_unreferenced_function_symbols() const;

  virtual const elf_symbols&
  get_unreferenced_variable_symbols() const;

  bool
  operator==(const corpus_group&) const;
}; // end class corpus_group

}// end namespace ir
}//end namespace abigail
#endif //__ABG_CORPUS_H__
