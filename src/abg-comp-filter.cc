// -*- Mode: C++ -*-
//
// Copyright (C) 2013-2018 Red Hat, Inc.
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
//
// Author: Dodji Seketeli

/// @file
///
/// This file contains definitions of diff objects filtering
/// facilities.

#include "abg-internal.h"
// <headers defining libabigail's API go under here>
ABG_BEGIN_EXPORT_DECLARATIONS

#include "abg-comp-filter.h"

ABG_END_EXPORT_DECLARATIONS
// </headers defining libabigail's API>

namespace abigail
{
namespace comparison
{
namespace filtering
{

using std::tr1::dynamic_pointer_cast;

/// Walk the diff sub-trees of a a @ref corpus_diff and apply a filter
/// to the nodes visted.  The filter categorizes each node, assigning
/// it into one or several categories.
///
/// @param filter the filter to apply to the diff nodes
///
/// @param d the corpus diff to apply the filter to.
void
apply_filter(filter_base& filter, corpus_diff_sptr d)
{
  bool s = d->context()->visiting_a_node_twice_is_forbidden();
  d->context()->forbid_visiting_a_node_twice(false);
  d->traverse(filter);
  d->context()->forbid_visiting_a_node_twice(s);
}

/// Walk a diff sub-tree and apply a filter to the nodes visted.  The
/// filter categorizes each node, assigning it into one or several
/// categories.
///
/// Note that this function makes sure to avoid visiting a node (or
/// any other node equivalent to it) more than once.  This helps avoid
/// infinite loops for diff trees that involve type changes that
/// reference themselves.
///
/// @param filter the filter to apply to the nodes of the sub-tree.
///
/// @param d the diff sub-tree to walk and apply the filter to.
void
apply_filter(filter_base& filter, diff_sptr d)
{
  bool s = d->context()->visiting_a_node_twice_is_forbidden();
  d->context()->forbid_visiting_a_node_twice(true);
  d->context()->forget_visited_diffs();
  d->traverse(filter);
  d->context()->forbid_visiting_a_node_twice(s);
}

/// Walk a diff sub-tree and apply a filter to the nodes visted.  The
/// filter categorizes each node, assigning it into one or several
/// categories.
///
/// Note that this function makes sure to avoid visiting a node (or
/// any other node equivalent to it) more than once.  This helps avoid
/// infinite loops for diff trees that involve type changes that
/// reference themselves.
///
/// @param filter the filter to apply to the nodes of the sub-tree.
///
/// @param d the diff sub-tree to walk and apply the filter to.
void
apply_filter(filter_base_sptr filter, diff_sptr d)
{apply_filter(*filter, d);}

/// Test if there is a class that is declaration-only among the two
/// classes in parameter.
///
/// @param class1 the first class to consider.
///
/// @param class2 the second class to consider.
///
/// @return true if either classes are declaration-only, false
/// otherwise.
static bool
there_is_a_decl_only_class(const class_decl_sptr& class1,
			   const class_decl_sptr& class2)
{
  if ((class1 && class1->get_is_declaration_only())
      || (class2 && class2->get_is_declaration_only()))
    return true;
  return false;
}

/// Test if the diff involves a declaration-only class.
///
/// @param diff the class diff to consider.
///
/// @return true iff the diff involves a declaration-only class.
static bool
diff_involves_decl_only_class(const class_diff* diff)
{
  if (diff && there_is_a_decl_only_class(diff->first_class_decl(),
					 diff->second_class_decl()))
    return true;
  return false;
}

/// Tests if the size of a given type changed.
///
/// @param f the first version of the type to consider.
///
/// @param s the second version of the type to consider.
///
/// @return true if the type size changed, false otherwise.
static bool
type_size_changed(const type_base_sptr f, const type_base_sptr s)
{
  if (!f || !s
      || f->get_size_in_bits() == 0
      || s->get_size_in_bits() == 0
      || there_is_a_decl_only_class(is_compatible_with_class_type(f),
				    is_compatible_with_class_type(s)))
    return false;

  return f->get_size_in_bits() != s->get_size_in_bits();
}

/// Tests if the size of a given type changed.
///
/// @param f the declaration of the first version of the type to
/// consider.
///
/// @param s the declaration of the second version of the type to
/// consider.
///
/// @return true if the type size changed, false otherwise.
static bool
type_size_changed(const decl_base_sptr f, const decl_base_sptr s)
{return type_size_changed(is_type(f), is_type(s));}

/// Test if a given type diff node carries a type size change.
///
/// @param diff the diff tree node to test.
///
/// @return true if @p diff carries a type size change.
static bool
has_type_size_change(const diff* diff)
{
  if (!diff)
    return false;

  type_base_sptr f = is_type(diff->first_subject()),
    s = is_type(diff->second_subject());

  if (!f || !s)
    return false;

  return type_size_changed(f, s);
}
/// Tests if the access specifiers for a member declaration changed.
///
/// @param f the declaration for the first version of the member
/// declaration to consider.
///
/// @param s the declaration for the second version of the member
/// delcaration to consider.
///
/// @return true iff the access specifier changed.
static bool
access_changed(const decl_base_sptr& f, const decl_base_sptr& s)
{
  if (!is_member_decl(f)
      || !is_member_decl(s))
    return false;

  access_specifier fa = get_member_access_specifier(f),
    sa = get_member_access_specifier(s);

  if (sa != fa)
    return true;

  return false;
}

/// Test if there was a function name change, but there there was no
/// change in name of the underlying symbol.  IOW, if the name of a
/// function changed, but the symbol of the new function is equal to
/// the symbol of the old one, or is equal to an alians of the symbol
/// of the old function.
///
/// @param f the first function to consider.
///
/// @param s the second function to consider.
///
/// @return true if the test is positive, false otherwise.
static bool
function_name_changed_but_not_symbol(const function_decl_sptr& f,
				     const function_decl_sptr& s)
{
  if (!f || !s)
    return false;
  string fn = f->get_qualified_name(),
    sn = s->get_qualified_name();

  if (fn != sn)
    {
      elf_symbol_sptr fs = f->get_symbol(), ss = s->get_symbol();
      if (fs == ss)
	return true;
      for (elf_symbol_sptr s = fs->get_next_alias();
	   s && !s->is_main_symbol();
	   s = s->get_next_alias())
	if (*s == *ss)
	  return true;
    }
  return false;
}

/// Test if the current diff tree node carries a function name change,
/// in which there there was no change in the name of the underlying
/// symbol.  IOW, if the name of a function changed, but the symbol of
/// the new function is equal to the symbol of the old one, or is
/// equal to an alians of the symbol of the old function.
///
/// @param diff the diff tree node to consider.
///
/// @return true if the test is positive, false otherwise.
static bool
function_name_changed_but_not_symbol(const diff* diff)
{
  if (const function_decl_diff* d =
      dynamic_cast<const function_decl_diff*>(diff))
    return function_name_changed_but_not_symbol(d->first_function_decl(),
						d->second_function_decl());
  return false;
}

/// Tests if the offset of a given data member changed.
///
/// @param f the declaration for the first version of the data member to
/// consider.
///
/// @param s the declaration for the second version of the data member
/// to consider.
///
/// @return true iff the offset of the data member changed.
static bool
data_member_offset_changed(decl_base_sptr f, decl_base_sptr s)
{
  if (!is_member_decl(f)
      || !is_member_decl(s))
    return false;

  var_decl_sptr v0 = dynamic_pointer_cast<var_decl>(f),
    v1 = dynamic_pointer_cast<var_decl>(s);
  if (!v0 || !v1)
    return false;

  if (get_data_member_offset(v0) != get_data_member_offset(v1))
    return true;

  return false;
}

/// Test if the size of a non-static data member changed accross two
/// versions.
///
/// @param f the first version of the non-static data member.
///
/// @param s the second version of the non-static data member.
static bool
non_static_data_member_type_size_changed(const decl_base_sptr& f,
					 const decl_base_sptr& s)
{
  if (!is_member_decl(f)
      || !is_member_decl(s))
    return false;

  var_decl_sptr fv = dynamic_pointer_cast<var_decl>(f),
    sv = dynamic_pointer_cast<var_decl>(s);
  if (!fv
      || !sv
      || get_member_is_static(fv)
      || get_member_is_static(sv))
    return false;

  return type_size_changed(fv->get_type(), sv->get_type());
}

/// Test if the size of a static data member changed accross two
/// versions.
///
/// @param f the first version of the static data member.
///
/// @param s the second version of the static data member.
static bool
static_data_member_type_size_changed(const decl_base_sptr& f,
				     const decl_base_sptr& s)
{
  if (!is_member_decl(f)
      || !is_member_decl(s))
    return false;

  var_decl_sptr fv = dynamic_pointer_cast<var_decl>(f),
    sv = dynamic_pointer_cast<var_decl>(s);
  if (!fv
      || !sv
      || !get_member_is_static(fv)
      || !get_member_is_static(sv))
    return false;

  return type_size_changed(fv->get_type(), sv->get_type());
}

/// Test if two types are different but compatible.
///
/// @param d1 the declaration of the first type to consider.
///
/// @param d2 the declaration of the second type to consider.
///
/// @return true if d1 and d2 are different but compatible.
static bool
is_compatible_change(const decl_base_sptr& d1, const decl_base_sptr& d2)
{
  if ((d1 && d2)
      && (d1 != d2)
      && types_are_compatible(d1, d2))
    return true;
  return false;
}

/// Test if two decls have different names.
///
/// @param d1 the first declaration to consider.
///
/// @param d2 the second declaration to consider.
///
/// @return true if d1 and d2 have different names.
static bool
decl_name_changed(const type_or_decl_base* a1, const type_or_decl_base *a2)
{
  string d1_name, d2_name;

  const decl_base *d1 = dynamic_cast<const decl_base*>(a1);
  if (d1 == 0)
    return false;

  const decl_base *d2 = dynamic_cast<const decl_base*>(a2);
  if (d2 == 0)
    return false;

  if (d1)
    d1_name = d1->get_qualified_name();
  if (d2)
    d2_name = d2->get_qualified_name();

  return d1_name != d2_name;
}

/// Test if two decls have different names.
///
/// @param d1 the first declaration to consider.
///
/// @param d2 the second declaration to consider.
///
/// @return true if d1 and d2 have different names.
static bool
decl_name_changed(const type_or_decl_base_sptr& d1,
		  const type_or_decl_base_sptr& d2)
{return decl_name_changed(d1.get(), d2.get());}

/// Test if a diff nodes carries a changes in which two decls have
/// different names.
///
/// @param d the diff node to consider.
///
/// @return true iff d carries a changes in which two decls have
/// different names.
static bool
decl_name_changed(const diff *d)
{return decl_name_changed(d->first_subject(), d->second_subject());}

/// Test if two decls represents a harmless name change.
///
/// For now, a harmless name change is considered only for a typedef,
/// enum or a data member.
///
/// @param f the first decl to consider in the comparison.
///
/// @param s the second decl to consider in the comparison.
///
/// @return true iff decl @p s represents a harmless change over @p f.
bool
has_harmless_name_change(const decl_base_sptr& f, const decl_base_sptr& s)
{
  // So, a harmless name change is either ...
  return (decl_name_changed(f, s)
	  && (// ... a typedef name change, without having the
	      // underlying type changed ...
	      (is_typedef(f)
	       && is_typedef(s)
	       && (is_typedef(f)->get_underlying_type()
		   == is_typedef(s)->get_underlying_type()))
	      // .. or a data member name change, without having the
	      // its type changed ...
	      || (is_data_member(f)
		  && is_data_member(s)
		  && (is_var_decl(f)->get_type()
		      == is_var_decl(s)->get_type()))
	      // .. an enum name change without having any other part
	      // of the enum to change.
	      || (is_enum_type(f)
		  && is_enum_type(s)
		  && !enum_has_non_name_change(*is_enum_type(f),
					       *is_enum_type(s),
					       0))));
}

/// Test if a class_diff node has non-static members added or
/// removed.
///
/// @param diff the diff node to consider.
///
/// @return true iff the class_diff node has non-static members added
/// or removed.
static bool
non_static_data_member_added_or_removed(const class_diff* diff)
{
  if (diff && !diff_involves_decl_only_class(diff))
    {
      for (string_decl_base_sptr_map::const_iterator i =
	     diff->inserted_data_members().begin();
	   i != diff->inserted_data_members().end();
	   ++i)
	if (!get_member_is_static(i->second))
	  return true;

      for (string_decl_base_sptr_map::const_iterator i =
	     diff->deleted_data_members().begin();
	   i != diff->deleted_data_members().end();
	   ++i)
	if (!get_member_is_static(i->second))
	  return true;
    }

  return false;
}

/// Test if a class_diff node has members added or removed.
///
/// @param diff the diff node to consider.
///
/// @return true iff the class_diff node has members added or removed.
static bool
non_static_data_member_added_or_removed(const diff* diff)
{
  return non_static_data_member_added_or_removed
    (dynamic_cast<const class_diff*>(diff));
}

/// Test if a class_diff node has static members added or removed.
///
/// @param diff the diff node to consider.
///
/// @return true iff the class_diff node has static members added
/// or removed.
static bool
static_data_member_added_or_removed(const class_diff* diff)
{
  if (diff && !diff_involves_decl_only_class(diff))
    {
      for (string_decl_base_sptr_map::const_iterator i =
	     diff->inserted_data_members().begin();
	   i != diff->inserted_data_members().end();
	   ++i)
	if (get_member_is_static(i->second))
	  return true;

      for (string_decl_base_sptr_map::const_iterator i =
	     diff->deleted_data_members().begin();
	   i != diff->deleted_data_members().end();
	   ++i)
	if (get_member_is_static(i->second))
	  return true;
    }

  return false;
}

/// Test if a class_diff node has a harmless "One Definition Rule"
/// violation that will cause a diagnostic rule.
///
/// The conditions this function looks for are:
///
///  1/ The two subject of the diff must be canonically different
///
///  2/ The two subjects of the diff must be structurally equal
///
///  3/ The canonical types of the subjects of the diff must be
///  structurally different.
///
/// These conditions makes the diff node appears as it carries changes
/// (because of a ODR glitch present in the binary), but the glitch
/// has no effect on the structural equality of the subjects of the
/// diff.  If we do not detect these conditions, we'd end up with a
/// diagnostic glitch where the reporter thinks there is an ABI change
/// (because of the canonical difference), but then it fails to give
/// any detail about it, because there is no structural change.
///
/// @param diff the diff node to consider.
///
/// @return true iff the the diff node has a harmless "One Definition
/// Rule" violation that cause an empty false positive.
static bool
class_diff_has_harmless_odr_violation_change(const diff* dif)
{
  class_diff* d =  dynamic_cast<class_diff*>(const_cast<diff*>(dif));
  if (!d || !d->has_changes())
    return false;

  class_decl_sptr first = d->first_class_decl();
  class_decl_sptr second = d->second_class_decl();

  if (equals(*first, *second, 0))
    {
      class_decl_sptr fc = is_class_type(first->get_canonical_type());
      class_decl_sptr sc = is_class_type(second->get_canonical_type());

      if (!equals(*fc, *sc, 0))
	return true;
    }

  return false;
}

/// Test if a class_diff node has static members added or
/// removed.
///
/// @param diff the diff node to consider.
///
/// @return true iff the class_diff node has static members added
/// or removed.
static bool
static_data_member_added_or_removed(const diff* diff)
{
  return static_data_member_added_or_removed
    (dynamic_cast<const class_diff*>(diff));
}

/// Test if the class_diff node has a change involving virtual member
/// functions.
///
/// That means whether there is an added, removed or changed virtual
/// member function.
///
/// @param diff the class_diff node to consider.
///
/// @return true iff the class_diff node contains changes involving
/// virtual member functions.
static bool
has_virtual_mem_fn_change(const class_diff* diff)
{
  if (!diff || diff_involves_decl_only_class(diff))
    return false;

  for (string_member_function_sptr_map::const_iterator i =
	 diff->deleted_member_fns().begin();
       i != diff->deleted_member_fns().end();
       ++i)
    {
      if (get_member_function_is_virtual(i->second))
	{
	  // Do not consider a virtual function that got deleted from
	  // an offset and re-inserted at the same offset as a
	  // "virtual member function change".
	  string_member_function_sptr_map::const_iterator j =
	    diff->inserted_member_fns().find(i->first);
	  if (j != diff->inserted_member_fns().end()
	      && (get_member_function_vtable_offset(i->second)
		  == get_member_function_vtable_offset(j->second)))
	    continue;

	  return true;
	}
    }

  for (string_member_function_sptr_map::const_iterator i =
	 diff->inserted_member_fns().begin();
       i != diff->inserted_member_fns().end();
       ++i)
    {
      if (get_member_function_is_virtual(i->second))
	{
	  // Do not consider a virtual function that got deleted from
	  // an offset and re-inserted at the same offset as a
	  // "virtual member function change".
	  string_member_function_sptr_map::const_iterator j =
	    diff->deleted_member_fns().find(i->first);
	  if (j != diff->deleted_member_fns().end()
	      && (get_member_function_vtable_offset(i->second)
		  == get_member_function_vtable_offset(j->second)))
	    continue;

	  return true;
	}
    }

  for (function_decl_diff_sptrs_type::const_iterator i =
	 diff->changed_member_fns().begin();
       i != diff->changed_member_fns().end();
       ++i)
    if (get_member_function_is_virtual((*i)->first_function_decl())
	|| get_member_function_is_virtual((*i)->second_function_decl()))
      {
	if (get_member_function_vtable_offset((*i)->first_function_decl())
	    == get_member_function_vtable_offset((*i)->second_function_decl()))
	  continue;

	return true;
      }

  return false;
}

/// Test if the function_decl_diff node has a change involving virtual
/// member functions.
///
/// That means whether there is an added, removed or changed virtual
/// member function.
///
/// @param diff the function_decl_diff node to consider.
///
/// @return true iff the function_decl_diff node contains changes
/// involving virtual member functions.
bool
has_virtual_mem_fn_change(const function_decl_diff* diff)
{
  if (!diff)
    return false;

  function_decl_sptr ff = diff->first_function_decl(),
    sf = diff->second_function_decl();

  if (!is_member_function(ff)
      || !is_member_function(sf))
    return false;

  bool ff_is_virtual = get_member_function_is_virtual(ff),
    sf_is_virtual = get_member_function_is_virtual(sf);

  if (ff_is_virtual != sf_is_virtual)
    return true;

  size_t ff_vtable_offset = get_member_function_vtable_offset(ff),
    sf_vtable_offset = get_member_function_vtable_offset(sf);

  if (ff_vtable_offset != sf_vtable_offset)
    return true;

  return false;
}

/// Test if the class_diff node has a change involving virtual member
/// functions.
///
/// That means whether there is an added, removed or changed virtual
/// member function.
///
/// @param diff the class_diff node to consider.
///
/// @return true iff the class_diff node contains changes involving
/// virtual member functions.
static bool
has_virtual_mem_fn_change(const diff* diff)
{
  return (has_virtual_mem_fn_change(dynamic_cast<const class_diff*>(diff))
	  || has_virtual_mem_fn_change(dynamic_cast<const function_decl_diff*>(diff)));
}

/// Test if the class_diff has changes to non virtual member
/// functions.
///
///@param diff the class_diff nod e to consider.
///
/// @retrurn iff the class_diff node has changes to non virtual member
/// functions.
static bool
has_non_virtual_mem_fn_change(const class_diff* diff)
{
  if (!diff || diff_involves_decl_only_class(diff))
    return false;

  for (string_member_function_sptr_map::const_iterator i =
	 diff->deleted_member_fns().begin();
       i != diff->deleted_member_fns().end();
       ++i)
    if (!get_member_function_is_virtual(i->second))
      return true;

  for (string_member_function_sptr_map::const_iterator i =
	 diff->inserted_member_fns().begin();
       i != diff->inserted_member_fns().end();
       ++i)
    if (!get_member_function_is_virtual(i->second))
      return true;

  for (function_decl_diff_sptrs_type::const_iterator i =
	 diff->changed_member_fns().begin();
       i != diff->changed_member_fns().end();
       ++i)
    if(!get_member_function_is_virtual((*i)->first_function_decl())
       && !get_member_function_is_virtual((*i)->second_function_decl()))
      return true;

  return false;
}

/// Test if the class_diff has changes to non virtual member
/// functions.
///
///@param diff the class_diff nod e to consider.
///
/// @retrurn iff the class_diff node has changes to non virtual member
/// functions.
static bool
has_non_virtual_mem_fn_change(const diff* diff)
{return has_non_virtual_mem_fn_change(dynamic_cast<const class_diff*>(diff));}

/// Test if a class_diff carries base classes adding or removals.
///
/// @param diff the class_diff to consider.
///
/// @return true iff @p diff carries base classes adding or removals.
static bool
base_classes_added_or_removed(const class_diff* diff)
{
  if (!diff)
    return false;
  return diff->deleted_bases().size() || diff->inserted_bases().size();
}

/// Test if a class_diff carries base classes adding or removals.
///
/// @param diff the class_diff to consider.
///
/// @return true iff @p diff carries base classes adding or removals.
static bool
base_classes_added_or_removed(const diff* diff)
{return base_classes_added_or_removed(dynamic_cast<const class_diff*>(diff));}

/// Test if two @ref class_or_union_sptr are different just by the
/// fact that one is decl-only and the other one is defined.
///
/// @param first the first class or union to consider.
///
/// @param second the second class or union to consider.
///
/// @return true iff the two arguments are different just by the fact
/// that one is decl-only and the other one is defined.
bool
has_class_decl_only_def_change(const class_or_union_sptr& first,
			       const class_or_union_sptr& second)
{
  if (!first || !second)
    return false;

  class_or_union_sptr f =
    look_through_decl_only_class(first);
  class_or_union_sptr s =
    look_through_decl_only_class(second);

  if (f->get_qualified_name() != s->get_qualified_name())
    return false;

  return (f->get_is_declaration_only() != s->get_is_declaration_only());
}

/// Test if a class_or_union_diff carries a change in which the two
/// classes are different by the fact that one is a decl-only and the
/// other one is defined.
///
/// @param diff the diff node to consider.
////
//// @return true if the class_or_union_diff carries a change in which
/// the two classes are different by the fact that one is a decl-only
/// and the other one is defined.
bool
has_class_decl_only_def_change(const diff *diff)
{
  const class_or_union_diff *d = dynamic_cast<const class_or_union_diff*>(diff);
  if (!d)
    return false;

  class_or_union_sptr f =
    look_through_decl_only_class(d->first_class_or_union());
  class_or_union_sptr s =
    look_through_decl_only_class(d->second_class_or_union());

  return has_class_decl_only_def_change(f, s);
}

/// Test if a diff node carries a basic type name change.
///
/// @param d the diff node to consider.
///
/// @return true iff the diff node carries a basic type name change.
bool
has_basic_type_name_change(const diff *d)
{
  if (const type_decl_diff *dif = is_diff_of_basic_type(d))
    if (decl_name_changed(dif))
      return true;

  return false;
}

/// Test if a diff node carries a class or union type name change.
///
/// @param d the diff node to consider.
///
/// @return true iff the diff node carries a class or union type name
/// change.
bool
has_class_or_union_type_name_change(const diff *d)
{
  if (const class_or_union_diff *dif = is_diff_of_class_or_union_type(d))
    if (decl_name_changed(dif))
      return true;

  return false;
}

/// Test if a diff node carries a basic or class type name change.
///
/// @param d the diff node to consider.
///
/// @return true iff the diff node carries a basic or class type name
/// change.
bool
has_basic_or_class_type_name_change(const diff *d)
{
  return (has_basic_type_name_change(d)
	  || has_class_or_union_type_name_change(d));
}

/// Test if a diff node carries a distinct type change or a
/// pointer/reference/typedef to distinct type change.
///
/// Note that a distinct type change is a change where the two
/// subjects of the change are not of the same kind, e.g, a basic type
/// that got changed into a qualified type.
///
/// @param d the diff node to consider.
///
/// @return true iff @p d is mostly a distinct diff.
bool
is_mostly_distinct_diff(const diff *d)
{
  if (is_distinct_diff(d))
    return true;

  // Let's consider that 'd' is a type diff ...
  type_diff_base* td = const_cast<type_diff_base*>(is_type_diff(d));
  if (!td)
    {
      // ... or a function parameter diff.  In which case, let's get
      // its child type diff.
      fn_parm_diff *pd = const_cast<fn_parm_diff*>(is_fn_parm_diff(d));
      if (pd)
	td = const_cast<type_diff_base*>(is_type_diff(pd->type_diff().get()));
      if (!td)
	return false;
    }

  // At this point, if we are not looking at a type diff we must have
  // bailed out already.
  assert(td);

  type_base_sptr first = is_type(td->first_subject());
  type_base_sptr second = is_type(td->second_subject());

  first = peel_typedef_pointer_or_reference_type(first,
						 /*peel_qualified_type=*/false);
  second = peel_typedef_pointer_or_reference_type(second,
						  /*peel_qual_type=*/false);
  assert(first && second);

  return distinct_diff::entities_are_of_distinct_kinds(first, second);
}

/// Test if a diff node carries a non-anonymous data member to
/// anonymous data member change, or vice-versa.
///
/// @param d the diff node to consider.
///
/// @return true iff @p d carries a non-anonymous to anonymous data
/// member change, or vice-versa.
bool
has_anonymous_data_member_change(const diff *d)
{
  if (is_anonymous_data_member(d->first_subject())
      || is_anonymous_data_member(d->second_subject()))
    return true;
  return false;
}

/// Test if a diff node carries a non-anonymous data member to
/// anonymous data member change, or vice-versa.
///
/// @param d the diff node to consider.
///
/// @return true iff @p d carries a non-anonymous to anonymous data
/// member change, or vice-versa.
bool
has_anonymous_data_member_change(const diff_sptr &d)
{return has_anonymous_data_member_change(d.get());}

/// Test if an enum_diff carries an enumerator insertion.
///
/// @param diff the enum_diff to consider.
///
/// @return true iff @p diff carries an enumerator insertion.
static bool
has_enumerator_insertion(const diff* diff)
{
  if (const enum_diff* d = dynamic_cast<const enum_diff*>(diff))
    return !d->inserted_enumerators().empty();
  return false;
}

/// Test if an enum_diff carries an enumerator removal.
///
/// @param diff the enum_diff to consider.
///
/// @return true iff @p diff carries an enumerator removal or change.
static bool
has_enumerator_removal_or_change(const diff* diff)
{
  if (const enum_diff* d = dynamic_cast<const enum_diff*>(diff))
    return (!d->deleted_enumerators().empty()
	    || !d->changed_enumerators().empty());
  return false;
}

/// Test if an enum_diff carries a harmful change.
///
/// @param diff the enum_diff to consider.
///
/// @return true iff @p diff carries a harmful change.
static bool
has_harmful_enum_change(const diff* diff)
{
  if (const enum_diff* d = dynamic_cast<const enum_diff*>(diff))
    return (has_enumerator_removal_or_change(d)
	    || has_type_size_change(d));
  return false;
}

/// Test if an @ref fn_parm_diff node has a top cv qualifier change on
/// the type of the function parameter.
///
/// @param diff the diff node to consider.  It should be a @ref
/// fn_parm_diff, otherwise the function returns 'false' directly.
///
/// @return true iff @p diff is a @ref fn_parm_diff node that has a
/// top cv qualifier change on the type of the function parameter.
static bool
has_fn_parm_type_top_cv_qual_change(const diff* diff)
{
  // is diff a "function parameter diff node?
  const fn_parm_diff* parm_diff = is_fn_parm_diff(diff);

  if (!parm_diff || !parm_diff->has_changes())
    // diff either carries no change or is not a function parameter
    // diff node.  So bail out.
    return false;

  function_decl::parameter_sptr first_parm = parm_diff->first_parameter();
  function_decl::parameter_sptr second_parm = parm_diff->second_parameter();

  type_base_sptr first_parm_type = first_parm->get_type();
  type_base_sptr second_parm_type = second_parm->get_type();

  if (!is_qualified_type(first_parm_type)
      && !is_qualified_type(second_parm_type))
    // None of the parameter types is qualified.
    return false;

  qualified_type_def::CV cv_quals_1 = qualified_type_def::CV_NONE;
  qualified_type_def::CV cv_quals_2 = qualified_type_def::CV_NONE;
  type_base_sptr peeled_type_1 = first_parm_type;
  type_base_sptr peeled_type_2 = second_parm_type;

  if (qualified_type_def_sptr qtype1 = is_qualified_type(first_parm_type))
    {
      cv_quals_1 = qtype1->get_cv_quals();
      peeled_type_1 = peel_qualified_type(qtype1);
    }

  if (qualified_type_def_sptr qtype2 = is_qualified_type(second_parm_type))
    {
      cv_quals_2 = qtype2->get_cv_quals();
      peeled_type_2 = peel_qualified_type(qtype2);
    }

  if (peeled_type_1
      && peeled_type_2
      && get_type_name(peeled_type_1) == get_type_name(peeled_type_2)
      && cv_quals_1 != cv_quals_2)
    // The top-level CV qualifiers of the function type are different
    // and the un-qualified variant (peeled) of said function types
    // are equal.  This means the only change the function types have
    // are about top-level CV qualifiers.
    return true;

  return false;
}

/// Test if an @ref fn_parm_diff node has a cv qualifier change on the
/// type of the function parameter. That is, we are looking for
/// changes like 'const char*' to 'char*'.
///
/// @param diff the diff node to consider.  It should be a @ref
/// fn_parm_diff, otherwise the function returns 'false' directly.
///
/// @return true iff @p diff is a @ref fn_parm_diff node that has a
/// cv qualifier change on the type of the function parameter.
static bool
has_fn_parm_type_cv_qual_change(const diff* dif)
{
  // is diff a "function parameter diff node?
  const fn_parm_diff* parm_diff = is_fn_parm_diff(dif);

  if (!parm_diff || !parm_diff->has_changes())
    // diff either carries no change or is not a function parameter
    // diff node.  So bail out.
    return false;

  const diff *type_dif = parm_diff->type_diff().get();
  if (is_pointer_diff(type_dif))
    type_dif = peel_pointer_diff(type_dif);
  else if (is_reference_diff(type_dif))
    type_dif = peel_reference_diff(type_dif);

  const type_base *f = 0;
  const type_base *s = 0;
  if (const distinct_diff *d = is_distinct_diff(type_dif))
    {
      if (is_qualified_type(d->first()) != is_qualified_type(d->second()))
	{
	  f = is_type(d->first()).get();
	  s = is_type(d->second()).get();
	}
      else
	return false;
    }
  else if (const qualified_type_diff *d = is_qualified_type_diff(type_dif))
    {
      f = is_type(d->first_qualified_type()).get();
      s = is_type(d->second_qualified_type()).get();
    }
  else
    return false;

  f = peel_qualified_type(f);
  s = peel_qualified_type(s);

  return *f == *s;
}

/// Detect if the changes carried by a given diff node are deemed
/// harmless and do categorize the diff node accordingly.
///
/// @param d the diff node being visited.
///
/// @param pre this is true iff the node is being visited *before* the
/// children nodes of @p d.
///
/// @return true iff the traversal shall keep going after the
/// completion of this function.
static bool
categorize_harmless_diff_node(diff *d, bool pre)
{
  if (!d->has_changes())
    return true;

  if (pre)
    {
      diff_category category = NO_CHANGE_CATEGORY;

      decl_base_sptr f = is_decl(d->first_subject()),
	s = is_decl(d->second_subject());

      if (has_class_decl_only_def_change(d))
	category |= CLASS_DECL_ONLY_DEF_CHANGE_CATEGORY;

      if (access_changed(f, s))
	category |= ACCESS_CHANGE_CATEGORY;

      if (is_compatible_change(f, s))
	category |= COMPATIBLE_TYPE_CHANGE_CATEGORY;

      if (has_harmless_name_change(f, s)
	  || class_diff_has_harmless_odr_violation_change(d))
	category |= HARMLESS_DECL_NAME_CHANGE_CATEGORY;

      if (has_non_virtual_mem_fn_change(d))
	category |= NON_VIRT_MEM_FUN_CHANGE_CATEGORY;

      if (static_data_member_added_or_removed(d)
	  || static_data_member_type_size_changed(f, s))
	category |= STATIC_DATA_MEMBER_CHANGE_CATEGORY;

      if (has_enumerator_insertion(d)
	  && !has_harmful_enum_change(d))
	category |= HARMLESS_ENUM_CHANGE_CATEGORY;

      if (function_name_changed_but_not_symbol(d))
	category |= HARMLESS_SYMBOL_ALIAS_CHANGE_CATEORY;

      if (has_fn_parm_type_top_cv_qual_change(d))
	category |= FN_PARM_TYPE_TOP_CV_CHANGE_CATEGORY;

      if (has_fn_parm_type_cv_qual_change(d))
	category |= FN_PARM_TYPE_CV_CHANGE_CATEGORY;

      if (category)
	{
	  d->add_to_local_and_inherited_categories(category);
	  // Also update the category of the canonical node.
	  if (diff * canonical = d->get_canonical_diff())
	    canonical->add_to_local_and_inherited_categories(category);
	}
    }

  return true;
}

/// Detect if the changes carried by a given diff node are deemed
/// harmful and do categorize the diff node accordingly.
///
/// @param d the diff node being visited.
///
/// @param pre this is true iff the node is being visited *before* the
/// children nodes of @p d.
///
/// @return true iff the traversal shall keep going after the
/// completion of this function.
static bool
categorize_harmful_diff_node(diff *d, bool pre)
{
  if (!d->has_changes())
    return true;

  if (pre)
    {
      diff_category category = NO_CHANGE_CATEGORY;
      decl_base_sptr f = is_decl(d->first_subject()),
	s = is_decl(d->second_subject());

      // Detect size or offset changes as well as data member addition
      // or removal.
      //
      // TODO: be more specific -- not all size changes are harmful.
      if (!has_class_decl_only_def_change(d)
	  && (type_size_changed(f, s)
	      || data_member_offset_changed(f, s)
	      || non_static_data_member_type_size_changed(f, s)
	      || non_static_data_member_added_or_removed(d)
	      || base_classes_added_or_removed(d)
	      || has_harmful_enum_change(d)))
	category |= SIZE_OR_OFFSET_CHANGE_CATEGORY;

      if (has_virtual_mem_fn_change(d))
	category |= VIRTUAL_MEMBER_CHANGE_CATEGORY;

      if (category)
	{
	  d->add_to_local_and_inherited_categories(category);
	  // Update the category of the canonical diff node too.
	  if (diff * canonical = d->get_canonical_diff())
	    canonical->add_to_local_and_inherited_categories(category);
	}
    }

  return true;
}

/// The visiting code of the harmless_harmful_filter.
///
/// @param d the diff node being visited.
///
/// @param pre this is true iff the node is being visited *before* the
/// children nodes of @p d.
///
/// @return true iff the traversal shall keep going after the
/// completion of this function.
bool
harmless_harmful_filter::visit(diff* d, bool pre)
{
  return (categorize_harmless_diff_node(d, pre)
	  && categorize_harmful_diff_node(d, pre));
}

/// Part of the visiting code of the harmless_harmful_filter.
///
/// This function is called after the visiting of a given diff node.
/// Note that when this function is called, the visiting might not
/// have taken place *if* the node (or an equivalent node) has already
/// been visited.
///
/// @param d the diff node that has either been visited or skipped
/// (because it has already been visited during this traversing).
void
harmless_harmful_filter::visit_end(diff* d)
{
  if (d->context()->diff_has_been_visited(d))
    {
      // This node or one of its equivalent node has already been
      // visited.  That means at this moment,
      // harmless_harmful_filter::visit() has *not* been called prior
      // to this harmless_harmful_filter::visit_end() is called.  In
      // other words, only harmless_harmful_filter::visit_begin() and
      // harmless_harmful_filter::visit_end() are called.
      //
      // So let's update the category of this diff node from its
      // canonical node.
      if (diff* c = d->get_canonical_diff())
	d->add_to_local_and_inherited_categories(c->get_local_category());
    }
}
} // end namespace filtering
} // end namespace comparison
} // end namespace abigail
