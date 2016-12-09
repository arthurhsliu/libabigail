// -*- Mode: C++ -*-
//
// Copyright (C) 2013-2016 Red Hat, Inc.
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
/// This file contains the definitions of the entry points to
/// de-serialize an instance of @ref abigail::corpus from a file in
/// elf format, containing dwarf information.

#include "config.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <libgen.h>
#include <assert.h>
#include <limits.h>
#include <cstring>
#include <cmath>
#include <elfutils/libdwfl.h>
#include <dwarf.h>
#include <algorithm>
#include <iostream>
#include <tr1/unordered_map>
#include <stack>
#include <deque>
#include <list>
#include <ostream>
#include <sstream>

#include "abg-suppression-priv.h"

#include "abg-internal.h"
// <headers defining libabigail's API go under here>
ABG_BEGIN_EXPORT_DECLARATIONS

#include "abg-dwarf-reader.h"
#include "abg-sptr-utils.h"
#include "abg-tools-utils.h"

ABG_END_EXPORT_DECLARATIONS
// </headers defining libabigail's API>

using std::string;

namespace abigail
{

using std::cerr;

/// The namespace for the DWARF reader.
namespace dwarf_reader
{

using std::tr1::dynamic_pointer_cast;
using std::tr1::static_pointer_cast;
using std::tr1::unordered_map;
using std::stack;
using std::deque;
using std::list;

/// Where a DIE comes from. For instance, a DIE can come from the main
/// debug info section, the alternate debug info section or from the
/// type unit section.
enum die_source
{
  NO_DEBUG_INFO_DIE_SOURCE,
  PRIMARY_DEBUG_INFO_DIE_SOURCE,
  ALT_DEBUG_INFO_DIE_SOURCE,
  TYPE_UNIT_DIE_SOURCE,
  NUMBER_OF_DIE_SOURCES,	// This one must always be the latest
				// enumerator
};

/// Prefix increment operator for @ref die_source.
///
/// @param source the die_source to increment.
/// @return the incremented source.
static die_source&
operator++(die_source& source)
{
  source = static_cast<die_source>(source + 1);
  return source;
}

/// A functor used by @ref dwfl_sptr.
struct dwfl_deleter
{
  void
  operator()(Dwfl* dwfl)
  {dwfl_end(dwfl);}
};//end struct dwfl_deleter

/// A convenience typedef for a shared pointer to a Dwfl.
typedef shared_ptr<Dwfl> dwfl_sptr;

/// Convenience typedef for a map which key is the offset of a dwarf
/// die, (given by dwarf_dieoffset()) and which value is the
/// corresponding decl_base.
typedef unordered_map<Dwarf_Off, decl_base_sptr> die_decl_map_type;

/// Convenience typedef for a map which key is the offset of a dwarf
/// die and which value is the corresponding type_base.
typedef unordered_map<Dwarf_Off, type_base_sptr> die_type_map_type;

/// Convenience typedef for a map which key is the offset of a dwarf
/// die, (given by dwarf_dieoffset()) and which value is the
/// corresponding class_decl.
typedef unordered_map<Dwarf_Off, class_decl_sptr> die_class_map_type;

/// Convenience typedef for a map which key is the offset of a dwarf
/// die, (given by dwarf_dieoffset()) and which value is the
/// corresponding class_or_union_sptr.
typedef unordered_map<Dwarf_Off, class_or_union_sptr> die_class_or_union_map_type;

/// Convenience typedef for a map which key the offset of a dwarf die
/// and which value is the corresponding function_decl.
typedef unordered_map<Dwarf_Off, function_decl_sptr> die_function_decl_map_type;

/// Convenience typedef for a map which key is the offset of a dwarf
/// die and which value is the corresponding function_type.
typedef unordered_map<Dwarf_Off, function_type_sptr> die_function_type_map_type;

/// Convenience typedef for a map which key is the offset of a
/// DW_TAG_compile_unit and the key is the corresponding @ref
/// translation_unit_sptr.
typedef unordered_map<Dwarf_Off, translation_unit_sptr> die_tu_map_type;

/// Convenience typedef for a map which key is an elf address and
/// which value is an elf_symbol_sptr.
typedef unordered_map<GElf_Addr, elf_symbol_sptr> addr_elf_symbol_sptr_map_type;

/// Convenience typedef for a shared pointer to an
/// addr_elf_symbol_sptr_map_type.
typedef shared_ptr<addr_elf_symbol_sptr_map_type> addr_elf_symbol_sptr_map_sptr;

/// Convenience typedef for a stack containing the scopes up to the
/// current point in the abigail Internal Representation (aka IR) tree
/// that is being built.
typedef stack<scope_decl*> scope_stack_type;

/// Convenience typedef for a map which key is a dwarf offset.  The
/// value is also a dwarf offset.
typedef unordered_map<Dwarf_Off, Dwarf_Off> offset_offset_map;

/// Convenience typedef for a map which key is a string and which
/// value is a vector of smart pointer to a class.
typedef unordered_map<string, classes_type> string_classes_map;

/// The abstraction of the place where a partial unit has been
/// imported.  This is what the DW_TAG_imported_unit DIE expresses.
///
/// This type thus contains:
///	- the offset to which the partial unit is imported
///	- the offset of the imported partial unit.
///	- the offset of the imported partial unit.
struct imported_unit_point
{
  Dwarf_Off	offset_of_import;
  // The boolean below is true iff the imported unit comes from the
  // alternate debug info file.
  die_source	imported_unit_die_source;
  Dwarf_Off	imported_unit_die_off;
  Dwarf_Off	imported_unit_cu_off;
  Dwarf_Off	imported_unit_child_off;

  /// Default constructor for @ref the type imported_unit_point.
  imported_unit_point ()
    : offset_of_import(),
      imported_unit_die_source(PRIMARY_DEBUG_INFO_DIE_SOURCE),
      imported_unit_die_off(),
      imported_unit_cu_off(),
      imported_unit_child_off()
  {}

  /// Constructor of @ref the type imported_unit_point.
  ///
  /// @param import_off the offset of the point at which the unit has
  /// been imported.
  imported_unit_point (Dwarf_Off import_off)
    : offset_of_import(import_off),
      imported_unit_die_source(PRIMARY_DEBUG_INFO_DIE_SOURCE),
      imported_unit_die_off(),
      imported_unit_cu_off(),
      imported_unit_child_off()
  {}

  /// Constructor of @ref the type imported_unit_point.
  ///
  /// @param import_off the offset of the point at which the unit has
  /// been imported.
  ///
  /// @param from where the imported DIE comes from.
  ///
  /// @param imported_die the die of the unit that has been imported.
  imported_unit_point (Dwarf_Off	import_off,
		       const Dwarf_Die& imported_die,
		       die_source from)
    : offset_of_import(import_off),
      imported_unit_die_source(from),
      imported_unit_die_off(dwarf_dieoffset
			    (const_cast<Dwarf_Die*>(&imported_die))),
      imported_unit_cu_off(),
      imported_unit_child_off()
  {
    Dwarf_Die imported_unit_child;

    dwarf_child(const_cast<Dwarf_Die*>(&imported_die),
		&imported_unit_child);
    imported_unit_child_off =
      dwarf_dieoffset(const_cast<Dwarf_Die*>(&imported_unit_child));

    Dwarf_Die cu_die_memory;
    Dwarf_Die *cu_die;

    cu_die = dwarf_diecu(const_cast<Dwarf_Die*>(&imported_unit_child),
			 &cu_die_memory, 0, 0);
    imported_unit_cu_off = dwarf_dieoffset(cu_die);
  }
}; // struct imported_unit_point

/// "Less than" operator for instances of @ref imported_unit_point
/// type.
///
/// @param the left hand side operand of the "Less than" operator.
///
/// @param the right hand side operand of the "Less than" operator.
///
/// @return true iff @p l is less than @p r.
static bool
operator<(const imported_unit_point& l, const imported_unit_point& r)
{return l.offset_of_import < r.offset_of_import;}

/// Convenience typedef for a vector of @ref imported_unit_point.
typedef vector<imported_unit_point> imported_unit_points_type;

/// Convenience typedef for a vector of @ref imported_unit_point.
typedef unordered_map<Dwarf_Off, imported_unit_points_type>
tu_die_imported_unit_points_map_type;

static bool
find_symbol_table_section(Elf* elf_handle, Elf_Scn*& section);

static bool
get_symbol_versionning_sections(Elf*		elf_handle,
				Elf_Scn*&	versym_section,
				Elf_Scn*&	verdef_section,
				Elf_Scn*&	verneed_section);

static bool
eval_last_constant_dwarf_sub_expr(Dwarf_Op*	expr,
				  size_t	expr_len,
				  ssize_t&	value,
				  bool&	is_tls_address);
static bool
die_address_attribute(Dwarf_Die* die, unsigned attr_name, Dwarf_Addr& result);

static bool
die_location_address(Dwarf_Die*	die,
		     Dwarf_Addr&	address,
		     bool&		is_tls_address);

static bool
die_die_attribute(Dwarf_Die* die,
		  unsigned attr_name,
		  Dwarf_Die& result,
		  bool look_thru_abstract_origin = true);

static void
maybe_canonicalize_type(Dwarf_Off	die_offset,
			die_source	source,
			read_context&	ctxt);

static int
get_default_array_lower_bound(translation_unit::language l);

static bool
find_lower_bound_in_imported_unit_points(const imported_unit_points_type&,
					 Dwarf_Off,
					 imported_unit_points_type::const_iterator&);

/// Convert an elf symbol type (given by the ELF{32,64}_ST_TYPE
/// macros) into an elf_symbol::type value.
///
/// Note that this function aborts when given an unexpected value.
///
/// @param the symbol type value to convert.
///
/// @return the converted value.
static elf_symbol::type
stt_to_elf_symbol_type(unsigned char stt)
{
  elf_symbol::type t = elf_symbol::NOTYPE_TYPE;

  switch (stt)
    {
    case STT_NOTYPE:
      t = elf_symbol::NOTYPE_TYPE;
      break;
    case STT_OBJECT:
      t = elf_symbol::OBJECT_TYPE;
      break;
    case STT_FUNC:
      t = elf_symbol::FUNC_TYPE;
      break;
    case STT_SECTION:
      t = elf_symbol::SECTION_TYPE;
      break;
    case STT_FILE:
      t = elf_symbol::FILE_TYPE;
      break;
    case STT_COMMON:
      t = elf_symbol::COMMON_TYPE;
      break;
    case STT_TLS:
      t = elf_symbol::TLS_TYPE;
      break;
    case STT_GNU_IFUNC:
      t = elf_symbol::GNU_IFUNC_TYPE;
      break;
    default:
      // An unknown value that probably ought to be supported?  Let's
      // abort right here rather than yielding garbage.
      ABG_ASSERT_NOT_REACHED;
    }

  return t;
}

/// Convert an elf symbol binding (given by the ELF{32,64}_ST_BIND
/// macros) into an elf_symbol::binding value.
///
/// Note that this function aborts when given an unexpected value.
///
/// @param the symbol binding value to convert.
///
/// @return the converted value.
static elf_symbol::binding
stb_to_elf_symbol_binding(unsigned char stb)
{
  elf_symbol::binding b = elf_symbol::GLOBAL_BINDING;

  switch (stb)
    {
    case STB_LOCAL:
      b = elf_symbol::LOCAL_BINDING;
      break;
    case STB_GLOBAL:
      b = elf_symbol::GLOBAL_BINDING;
      break;
    case STB_WEAK:
      b = elf_symbol::WEAK_BINDING;
      break;
    case STB_GNU_UNIQUE:
      b = elf_symbol::GNU_UNIQUE_BINDING;
      break;
    default:
      ABG_ASSERT_NOT_REACHED;
    }

  return b;

}

/// Convert the value of the e_machine field of GElf_Ehdr into a
/// string.  This is to get a string representing the architecture of
/// the elf file at hand.
///
/// @param e_machine the value of GElf_Ehdr::e_machine.
///
/// @return the string representation of GElf_Ehdr::e_machine.
static string
e_machine_to_string(GElf_Half e_machine)
{
  string result;
  switch (e_machine)
    {
    case EM_NONE:
      result = "elf-no-arch";
      break;
    case EM_M32:
      result = "elf-att-we-32100";
      break;
    case EM_SPARC:
      result = "elf-sun-sparc";
      break;
    case EM_386:
      result = "elf-intel-80386";
      break;
    case EM_68K:
      result = "elf-motorola-68k";
      break;
    case EM_88K:
      result = "elf-motorola-88k";
      break;
    case EM_860:
      result = "elf-intel-80860";
      break;
    case EM_MIPS:
      result = "elf-mips-r3000-be";
      break;
    case EM_S370:
      result = "elf-ibm-s370";
      break;
    case EM_MIPS_RS3_LE:
      result = "elf-mips-r3000-le";
      break;
    case EM_PARISC:
      result = "elf-hp-parisc";
      break;
    case EM_VPP500:
      result = "elf-fujitsu-vpp500";
      break;
    case EM_SPARC32PLUS:
      result = "elf-sun-sparc-v8plus";
      break;
    case EM_960:
      result = "elf-intel-80960";
      break;
    case EM_PPC:
      result = "elf-powerpc";
      break;
    case EM_PPC64:
      result = "elf-powerpc-64";
      break;
    case EM_S390:
      result = "elf-ibm-s390";
      break;
    case EM_V800:
      result = "elf-nec-v800";
      break;
    case EM_FR20:
      result = "elf-fujitsu-fr20";
      break;
    case EM_RH32:
      result = "elf-trw-rh32";
      break;
    case EM_RCE:
      result = "elf-motorola-rce";
      break;
    case EM_ARM:
      result = "elf-arm";
      break;
    case EM_FAKE_ALPHA:
      result = "elf-digital-alpha";
      break;
    case EM_SH:
      result = "elf-hitachi-sh";
      break;
    case EM_SPARCV9:
      result = "elf-sun-sparc-v9-64";
      break;
    case EM_TRICORE:
      result = "elf-siemens-tricore";
      break;
    case EM_ARC:
      result = "elf-argonaut-risc-core";
      break;
    case EM_H8_300:
      result = "elf-hitachi-h8-300";
      break;
    case EM_H8_300H:
      result = "elf-hitachi-h8-300h";
      break;
    case EM_H8S:
      result = "elf-hitachi-h8s";
      break;
    case EM_H8_500:
      result = "elf-hitachi-h8-500";
      break;
    case EM_IA_64:
      result = "elf-intel-ia-64";
      break;
    case EM_MIPS_X:
      result = "elf-stanford-mips-x";
      break;
    case EM_COLDFIRE:
      result = "elf-motorola-coldfire";
      break;
    case EM_68HC12:
      result = "elf-motorola-68hc12";
      break;
    case EM_MMA:
      result = "elf-fujitsu-mma";
      break;
    case EM_PCP:
      result = "elf-siemens-pcp";
      break;
    case EM_NCPU:
      result = "elf-sony-ncpu";
      break;
    case EM_NDR1:
      result = "elf-denso-ndr1";
      break;
    case EM_STARCORE:
      result = "elf-motorola-starcore";
      break;
    case EM_ME16:
      result = "elf-toyota-me16";
      break;
    case EM_ST100:
      result = "elf-stm-st100";
      break;
    case EM_TINYJ:
      result = "elf-alc-tinyj";
      break;
    case EM_X86_64:
      result = "elf-amd-x86_64";
      break;
    case EM_PDSP:
      result = "elf-sony-pdsp";
      break;
    case EM_FX66:
      result = "elf-siemens-fx66";
      break;
    case EM_ST9PLUS:
      result = "elf-stm-st9+";
      break;
    case EM_ST7:
      result = "elf-stm-st7";
      break;
    case EM_68HC16:
      result = "elf-motorola-68hc16";
      break;
    case EM_68HC11:
      result = "elf-motorola-68hc11";
      break;
    case EM_68HC08:
      result = "elf-motorola-68hc08";
      break;
    case EM_68HC05:
      result = "elf-motorola-68hc05";
      break;
    case EM_SVX:
      result = "elf-sg-svx";
      break;
    case EM_ST19:
      result = "elf-stm-st19";
      break;
    case EM_VAX:
      result = "elf-digital-vax";
      break;
    case EM_CRIS:
      result = "elf-axis-cris";
      break;
    case EM_JAVELIN:
      result = "elf-infineon-javelin";
      break;
    case EM_FIREPATH:
      result = "elf-firepath";
      break;
    case EM_ZSP:
      result = "elf-lsi-zsp";
      break;
    case EM_MMIX:
      result = "elf-don-knuth-mmix";
      break;
    case EM_HUANY:
      result = "elf-harvard-huany";
      break;
    case EM_PRISM:
      result = "elf-sitera-prism";
      break;
    case EM_AVR:
      result = "elf-atmel-avr";
      break;
    case EM_FR30:
      result = "elf-fujistu-fr30";
      break;
    case EM_D10V:
      result = "elf-mitsubishi-d10v";
      break;
    case EM_D30V:
      result = "elf-mitsubishi-d30v";
      break;
    case EM_V850:
      result = "elf-nec-v850";
      break;
    case EM_M32R:
      result = "elf-mitsubishi-m32r";
      break;
    case EM_MN10300:
      result = "elf-matsushita-mn10300";
      break;
    case EM_MN10200:
      result = "elf-matsushita-mn10200";
      break;
    case EM_PJ:
      result = "elf-picojava";
      break;
    case EM_OPENRISC:
      result = "elf-openrisc-32";
      break;
    case EM_ARC_A5:
      result = "elf-arc-a5";
      break;
    case EM_XTENSA:
      result = "elf-tensilica-xtensa";
      break;

#ifdef HAVE_EM_AARCH64_MACRO
    case EM_AARCH64:
      result = "elf-arm-aarch64";
      break;
#endif

#ifdef HAVE_EM_TILEPRO_MACRO
    case EM_TILEPRO:
      result = "elf-tilera-tilepro";
      break;
#endif

#ifdef HAVE_EM_TILEGX_MACRO
    case EM_TILEGX:
      result = "elf-tilera-tilegx";
      break;
#endif

    case EM_NUM:
      result = "elf-last-arch-number";
      break;
    case EM_ALPHA:
      result = "elf-non-official-alpha";
      break;
    default:
      {
	std::ostringstream o;
	o << "elf-unknown-arch-value-" << e_machine;
	result = o.str();
      }
      break;
  }
    return result;
}

/// The kind of ELF hash table found by the function
/// find_hash_table_section_index.
enum hash_table_kind
{
  NO_HASH_TABLE_KIND = 0,
  SYSV_HASH_TABLE_KIND,
  GNU_HASH_TABLE_KIND
};

/// Get the offset offset of the hash table section.
///
/// @param elf_handle the elf handle to use.
///
/// @param ht_section_offset this is set to the the resulting offset
/// of the hash table section.  This is set iff the function returns true.
///
/// @param symtab_section_offset the offset of the section of the
/// symbol table the hash table refers to.
static hash_table_kind
find_hash_table_section_index(Elf*	elf_handle,
			      size_t&	ht_section_index,
			      size_t&	symtab_section_index)
{
  if (!elf_handle)
    return NO_HASH_TABLE_KIND;

  GElf_Shdr header_mem, *section_header;
  bool found_sysv_ht = false, found_gnu_ht = false;
  for (Elf_Scn* section = elf_nextscn(elf_handle, 0);
       section != 0;
       section = elf_nextscn(elf_handle, section))
    {
      section_header= gelf_getshdr(section, &header_mem);
      if (section_header->sh_type != SHT_HASH
	  && section_header->sh_type != SHT_GNU_HASH)
	continue;

      ht_section_index = elf_ndxscn(section);
      symtab_section_index = section_header->sh_link;

      if (section_header->sh_type == SHT_HASH)
	found_sysv_ht = true;
      else if (section_header->sh_type == SHT_GNU_HASH)
	found_gnu_ht = true;
    }

  if (found_gnu_ht)
    return GNU_HASH_TABLE_KIND;
  else if (found_sysv_ht)
    return SYSV_HASH_TABLE_KIND;
  else
    return NO_HASH_TABLE_KIND;
}

/// Find the symbol table.
///
/// If we are looking at a relocatable or executable file, this
/// function will return the .symtab symbol table (of type
/// SHT_SYMTAB).  But if we are looking at a DSO it returns the
/// .dynsym symbol table (of type SHT_DYNSYM).
///
/// @param elf_handle the elf handle to consider.
///
/// @param symtab the symbol table found.
///
/// @return true iff the symbol table is found.
static bool
find_symbol_table_section(Elf* elf_handle, Elf_Scn*& symtab)
{
  Elf_Scn* section = 0, *dynsym = 0, *sym_tab = 0;
  while ((section = elf_nextscn(elf_handle, section)) != 0)
    {
      GElf_Shdr header_mem, *header;
      header = gelf_getshdr(section, &header_mem);
      if (header->sh_type == SHT_DYNSYM)
	dynsym = section;
      else if (header->sh_type == SHT_SYMTAB)
	sym_tab = section;
    }

  if (dynsym || sym_tab)
    {
      GElf_Ehdr eh_mem;
      GElf_Ehdr* elf_header = gelf_getehdr(elf_handle, &eh_mem);
      if (elf_header->e_type == ET_REL
	  || elf_header->e_type == ET_EXEC)
	symtab = sym_tab ? sym_tab : dynsym;
      else
	symtab = dynsym ? dynsym : sym_tab;
      return true;
    }
  return false;
}

/// Find the index (in the section headers table) of the symbol table
/// section.
///
/// If we are looking at a relocatable or executable file, this
/// function will return the index for the .symtab symbol table (of
/// type SHT_SYMTAB).  But if we are looking at a DSO it returns the
/// index for the .dynsym symbol table (of type SHT_DYNSYM).
///
/// @param elf_handle the elf handle to use.
///
/// @param symtab_index the index of the symbol_table, that was found.
///
/// @return true iff the symbol table section index was found.
static bool
find_symbol_table_section_index(Elf* elf_handle,
				size_t& symtab_index)
{
  Elf_Scn* section = 0;
  if (!find_symbol_table_section(elf_handle, section))
    return false;

  symtab_index = elf_ndxscn(section);
  return true;
}

/// Find and return a section by its name and its type.
///
/// @param elf_handle the elf handle to use.
///
/// @param name the name of the section.
///
/// @param section_type the type of the section.  This is the
/// Elf32_Shdr::sh_type (or Elf64_Shdr::sh_type) data member.
/// Examples of values of this parameter are SHT_PROGBITS or SHT_NOBITS.
///
/// @return the section found, nor nil if none was found.
static Elf_Scn*
find_section(Elf* elf_handle, const string& name, Elf64_Word section_type)
{
  GElf_Ehdr ehmem, *elf_header;
  elf_header = gelf_getehdr(elf_handle, &ehmem);

  Elf_Scn* section = 0;
  while ((section = elf_nextscn(elf_handle, section)) != 0)
    {
      GElf_Shdr header_mem, *header;
      header = gelf_getshdr(section, &header_mem);
      if (header->sh_type != section_type)
	continue;

      const char* section_name =
	elf_strptr(elf_handle, elf_header->e_shstrndx, header->sh_name);
      if (section_name && name == section_name)
	return section;
    }

  return 0;
}

/// Find and return the .text section.
///
/// @param elf_handle the elf handle to use.
///
/// @return the .text section found.
static Elf_Scn*
find_text_section(Elf* elf_handle)
{return find_section(elf_handle, ".text", SHT_PROGBITS);}

/// Find and return the .bss section.
///
/// @param elf_handle.
///
/// @return the .bss section found.
static Elf_Scn*
find_bss_section(Elf* elf_handle)
{return find_section(elf_handle, ".bss", SHT_NOBITS);}

/// Find and return the .rodata section.
///
/// @param elf_handle.
///
/// @return the .rodata section found.
static Elf_Scn*
find_rodata_section(Elf* elf_handle)
{return find_section(elf_handle, ".rodata", SHT_PROGBITS);}

/// Find and return the .data section.
///
/// @param elf_handle the elf handle to use.
///
/// @return the .data section found.
static Elf_Scn*
find_data_section(Elf* elf_handle)
{return find_section(elf_handle, ".data", SHT_PROGBITS);}

/// Find and return the .data1 section.
///
/// @param elf_handle the elf handle to use.
///
/// @return the .data1 section found.
static Elf_Scn*
find_data1_section(Elf* elf_handle)
{return find_section(elf_handle, ".data1", SHT_PROGBITS);}

/// Get the address at which a given binary is loaded in memory⋅
///
/// @param elf_handle the elf handle for the binary to consider.
///
/// @param load_address the address where the binary is loaded.  This
/// is set by the function iff it returns true.
///
/// @return true if the function could get the binary load address
/// and assign @p load_address to it.
static bool
get_binary_load_address(Elf *elf_handle,
			GElf_Addr &load_address)
{
  GElf_Ehdr eh_mem;
  GElf_Ehdr *elf_header = gelf_getehdr(elf_handle, &eh_mem);
  size_t num_segments = elf_header->e_phnum;

  for (unsigned i = 0; i < num_segments; ++i)
    {
      GElf_Phdr ph_mem;
      GElf_Phdr *program_header =gelf_getphdr(elf_handle, i, &ph_mem);
      if (program_header->p_type == PT_LOAD
	  && program_header->p_offset == 0)
	{
	  // This program header represent the segment containing the
	  // first byte of this binary.  We want to return the address
	  // at which the segment is loaded in memory.
	  load_address = program_header->p_vaddr;
	  return true;
	}
    }
  return false;
}

/// Find the file name of the alternate debug info file, as well as
/// its build ID.
///
/// @param elf_module the elf module to consider.
///
/// @param out parameter.  Is set to the file name of the alternate
/// debug info file, iff this function returns true.
///
/// @param out parameter.  Is set to the build ID of the alternate
/// debug info file.
///
/// @return true iff the location of the alternate debug info file was
/// found.
static bool
find_alt_debug_info_location(Dwfl_Module *elf_module,
			     string &alt_file_name,
			     string &build_id)
{
  GElf_Addr bias = 0;
  Dwarf *dwarf = dwfl_module_getdwarf(elf_module, &bias);
  Elf *elf = dwarf_getelf(dwarf);
  GElf_Ehdr ehmem, *elf_header;
  elf_header = gelf_getehdr(elf, &ehmem);

  Elf_Scn* section = 0;
  while ((section = elf_nextscn(elf, section)) != 0)
    {
      GElf_Shdr header_mem, *header;
      header = gelf_getshdr(section, &header_mem);
      if (header->sh_type != SHT_PROGBITS)
	continue;

      const char *section_name = elf_strptr(elf,
					    elf_header->e_shstrndx,
					    header->sh_name);

      char *alt_name = 0;
      char *buildid = 0;
      size_t buildid_len = 0;
      if (section_name != 0
	  && strcmp(section_name, ".gnu_debugaltlink") == 0)
	{
	  Elf_Data *data = elf_getdata(section, 0);
	  if (data != 0 && data->d_size != 0)
	    {
	      alt_name = (char*) data->d_buf;
	      char *end_of_alt_name =
		(char *) memchr(alt_name, '\0', data->d_size);
	      buildid_len = data->d_size - (end_of_alt_name - alt_name + 1);
	      if (buildid_len == 0)
		return false;
	      buildid = end_of_alt_name + 1;
	    }
	}
      else
	continue;

      if (buildid == 0 || alt_name == 0)
	return false;

      build_id = buildid;
      alt_file_name = alt_name;
      return true;
    }

  return false;
}

/// Return the alternate debug info associated to a given main debug
/// info file.
///
/// @param elf_module the elf module to consider.
///
/// @param alt_file_name output parameter.  This is set to the file
/// path of the alternate debug info file associated to @p elf_module.
/// This is set iff the function returns a non-null result.
///
/// @param alt_fd the file descriptor used to access the alternate
/// debug info.  If this parameter is set by the function, then the
/// caller needs to fclose it, otherwise the file descriptor is going
/// to be leaked.  Note however that on recent versions of elfutils
/// where libdw.h contains the function dwarf_getalt(), this parameter
/// is set to 0, so it doesn't need to be fclosed.
///
/// Note that the alternate debug info file is a DWARF extension as of
/// DWARF 4 ans is decribed at
/// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1.
///
/// @return the alternate debuginfo, or null.  If @p alt_fd is
/// non-zero, then the caller of this function needs to call
/// dwarf_end() on the returned alternate debuginfo pointer,
/// otherwise, it's going to be leaked.
static Dwarf*
find_alt_debug_info(Dwfl_Module *elf_module,
		    string& alt_file_name,
		    int& alt_fd)
{
  if (elf_module == 0)
    return 0;

  Dwarf* result = 0;
  string build_id;
  find_alt_debug_info_location(elf_module, alt_file_name, build_id);

#ifdef LIBDW_HAS_DWARF_GETALT
  // We are on recent versions of elfutils where the function
  // dwarf_getalt exists, so let's use it.
  Dwarf_Addr bias = 0;
  Dwarf* dwarf = dwfl_module_getdwarf(elf_module, &bias);
  result = dwarf_getalt(dwarf);
  alt_fd = 0;
#else
  // We are on an old version of elfutils where the function
  // dwarf_getalt doesn't exist yet, so let's open code its
  // functionality
  char *alt_name = 0;
  const char *file_name = 0;
  void **user_data = 0;
  Dwarf_Addr low_addr = 0;
  char *alt_file = 0;

  file_name = dwfl_module_info(elf_module, &user_data,
			       &low_addr, 0, 0, 0, 0, 0);

  alt_fd = dwfl_standard_find_debuginfo(elf_module, user_data,
					file_name, low_addr,
					alt_name, file_name,
					0, &alt_file);

  result = dwarf_begin(alt_fd, DWARF_C_READ);
#endif

  return result;
}

/// Compare a symbol name against another name, possibly demangling
/// the symbol_name before performing the comparison.
///
/// @param symbol_name the symbol_name to take in account.
///
/// @param name the second name to take in account.
///
/// @param demangle if true, demangle @p symbol_name and compare the
/// result of the demangling with @p name.
///
/// @return true iff symbol_name equals name.
static bool
compare_symbol_name(const string& symbol_name,
		    const string& name,
		    bool demangle)
{
  if (demangle)
    {
      string m = demangle_cplus_mangled_name(symbol_name);
      return m == name;
    }
  return symbol_name == name;
}

/// Return the SHT_GNU_versym, SHT_GNU_verdef and SHT_GNU_verneed
/// sections that are involved in symbol versionning.
///
/// @param elf_handle the elf handle to use.
///
/// @param versym_section the SHT_GNU_versym section found.  If the
/// section wasn't found, this is set to nil.
///
/// @param verdef_section the SHT_GNU_verdef section found.  If the
/// section wasn't found, this is set to nil.
///
/// @param verneed_section the SHT_GNU_verneed section found.  If the
/// section wasn't found, this is set to nil.
///
/// @return true iff at least one of the the sections where found.
static bool
get_symbol_versionning_sections(Elf*		elf_handle,
				Elf_Scn*&	versym_section,
				Elf_Scn*&	verdef_section,
				Elf_Scn*&	verneed_section)
{
  Elf_Scn* section = NULL;
  GElf_Shdr mem;
  Elf_Scn* versym = NULL, *verdef = NULL, *verneed = NULL;

  while ((section = elf_nextscn(elf_handle, section)) != NULL)
    {
      GElf_Shdr* h = gelf_getshdr(section, &mem);
      if (h->sh_type == SHT_GNU_versym)
	versym = section;
      else if (h->sh_type == SHT_GNU_verdef)
	verdef = section;
      else if (h->sh_type == SHT_GNU_verneed)
	verneed = section;
    }

  if (versym || verdef || verneed)
    {
      // At least one the versionning sections was found.  Return it.
      versym_section = versym;
      verdef_section = verdef;
      verneed_section = verneed;
      return true;
    }

  return false;
}

/// Get the version definition (from the SHT_GNU_verdef section) of a
/// given symbol represented by a pointer to GElf_Versym.
///
/// @param elf_hande the elf handle to use.
///
/// @param versym the symbol to get the version definition for.
///
/// @param verdef_section the SHT_GNU_verdef section.
///
/// @param version the resulting version definition.  This is set iff
/// the function returns true.
///
/// @return true upon successful completion, false otherwise.
static bool
get_version_definition_for_versym(Elf*			 elf_handle,
				  GElf_Versym*		 versym,
				  Elf_Scn*		 verdef_section,
				  elf_symbol::version&	 version)
{
  Elf_Data* verdef_data = elf_getdata(verdef_section, NULL);
  GElf_Verdef verdef_mem;
  GElf_Verdef* verdef = gelf_getverdef(verdef_data, 0, &verdef_mem);
  size_t vd_offset = 0;

  for (;; vd_offset += verdef->vd_next)
    {
      for (;verdef != 0;)
	{
	  if (verdef->vd_ndx == (*versym & 0x7fff))
	    // Found the version of the symbol.
	    break;
	  vd_offset += verdef->vd_next;
	  verdef = (verdef->vd_next == 0
		    ? 0
		    : gelf_getverdef(verdef_data, vd_offset, &verdef_mem));
	}

      if (verdef != 0)
	{
	  GElf_Verdaux verdaux_mem;
	  GElf_Verdaux *verdaux = gelf_getverdaux(verdef_data,
						  vd_offset + verdef->vd_aux,
						  &verdaux_mem);
	  GElf_Shdr header_mem;
	  GElf_Shdr* verdef_section_header = gelf_getshdr(verdef_section,
							  &header_mem);
	  size_t verdef_stridx = verdef_section_header->sh_link;
	  version.str(elf_strptr(elf_handle, verdef_stridx, verdaux->vda_name));
	  if (*versym & 0x8000)
	    version.is_default(false);
	  else
	    version.is_default(true);
	  return true;
	}
      if (!verdef || verdef->vd_next == 0)
	break;
    }
  return false;
}

/// Get the version needed (from the SHT_GNU_verneed section) to
/// resolve an undefined symbol represented by a pointer to
/// GElf_Versym.
///
/// @param elf_hande the elf handle to use.
///
/// @param versym the symbol to get the version definition for.
///
/// @param verneed_section the SHT_GNU_verneed section.
///
/// @param version the resulting version definition.  This is set iff
/// the function returns true.
///
/// @return true upon successful completion, false otherwise.
static bool
get_version_needed_for_versym(Elf*			elf_handle,
			      GElf_Versym*		versym,
			      Elf_Scn*			verneed_section,
			      elf_symbol::version&	version)
{
  if (versym == 0 || elf_handle == 0 || verneed_section == 0)
    return false;

  size_t vn_offset = 0;
  Elf_Data* verneed_data = elf_getdata(verneed_section, NULL);
  GElf_Verneed verneed_mem;
  GElf_Verneed* verneed = gelf_getverneed(verneed_data, 0, &verneed_mem);

  for (;verneed; vn_offset += verneed->vn_next)
    {
      size_t vna_offset = vn_offset;
      GElf_Vernaux vernaux_mem;
      GElf_Vernaux *vernaux = gelf_getvernaux(verneed_data,
					      vn_offset + verneed->vn_aux,
					      &vernaux_mem);
      for (;vernaux != 0 && verneed;)
	{
	  if (vernaux->vna_other == *versym)
	    // Found the version of the symbol.
	    break;
	  vna_offset += verneed->vn_next;
	  verneed = (verneed->vn_next == 0
		    ? 0
		    : gelf_getverneed(verneed_data, vna_offset, &verneed_mem));
	}

      if (verneed != 0 && vernaux != 0 && vernaux->vna_other == *versym)
	{
	  GElf_Shdr header_mem;
	  GElf_Shdr* verneed_section_header = gelf_getshdr(verneed_section,
							  &header_mem);
	  size_t verneed_stridx = verneed_section_header->sh_link;
	  version.str(elf_strptr(elf_handle,
				 verneed_stridx,
				 vernaux->vna_name));
	  if (*versym & 0x8000)
	    version.is_default(false);
	  else
	    version.is_default(true);
	  return true;
	}

      if (!verneed || verneed->vn_next == 0)
	break;
    }
  return false;
}

/// Return the version for a symbol that is at a given index in its
/// SHT_SYMTAB section.
///
/// @param elf_handle the elf handle to use.
///
/// @param symbol_index the index of the symbol to consider.
///
/// @param get_def_version if this is true, it means that that we want
/// the version for a defined symbol; in that case, the version is
/// looked for in a section of type SHT_GNU_verdef.  Otherwise, if
/// this parameter is false, this means that we want the version for
/// an undefined symbol; in that case, the version is the needed one
/// for the symbol to be resolved; so the version is looked fo in a
/// section of type SHT_GNU_verneed.
///
/// @param version the version found for symbol at @p symbol_index.
///
/// @return true iff a version was found for symbol at index @p
/// symbol_index.
static bool
get_version_for_symbol(Elf*			elf_handle,
		       size_t			symbol_index,
		       bool			get_def_version,
		       elf_symbol::version&	version)
{
  Elf_Scn *versym_section = NULL,
    *verdef_section = NULL,
    *verneed_section = NULL;

  if (!get_symbol_versionning_sections(elf_handle,
				       versym_section,
				       verdef_section,
				       verneed_section))
    return false;

  GElf_Versym versym_mem;
  Elf_Data* versym_data = (versym_section)
    ? elf_getdata(versym_section, NULL)
    : NULL;
  GElf_Versym* versym = (versym_data)
    ? gelf_getversym(versym_data, symbol_index, &versym_mem)
    : NULL;

  if (versym == 0 || *versym <= 1)
    // I got these value from the code of readelf.c in elfutils.
    // Apparently, if the symbol version entry has these values, the
    // symbol must be discarded. This is not documented in the
    // official specification.
    return false;

  if (get_def_version)
    {
      if (*versym == 0x8001)
	// I got this value from the code of readelf.c in elfutils
	// too.  It's not really documented in the official
	// specification.
	return false;

      if (verdef_section
	  && get_version_definition_for_versym(elf_handle, versym,
					       verdef_section, version))
	return true;
    }
  else
    {
      if (verneed_section
	  && get_version_needed_for_versym(elf_handle, versym,
					   verneed_section, version))
	return true;
    }

  return false;
}

/// Lookup a symbol using the SysV ELF hash table.
///
/// Note that this function hasn't been tested.  So it hasn't been
/// debugged yet.  IOW, it is not known to work.  Or rather, it's
/// almost like it's surely doesn't work ;-)
///
/// Use it at your own risks.  :-)
///
///@parm env the environment we are operating from.
///
/// @param elf_handle the elf_handle to use.
///
/// @param sym_name the symbol name to look for.
///
/// @param ht_index the index (in the section headers table) of the
/// hash table section to use.
///
/// @param sym_tab_index the index (in the section headers table) of
/// the symbol table to use.
///
/// @param demangle if true, demangle @p sym_name before comparing it
/// to names from the symbol table.
///
/// @param syms_found a vector of symbols found with the name @p
/// sym_name.  table.
static bool
lookup_symbol_from_sysv_hash_tab(const environment*		env,
				 Elf*				elf_handle,
				 const string&			sym_name,
				 size_t			ht_index,
				 size_t			sym_tab_index,
				 bool				demangle,
				 vector<elf_symbol_sptr>&	syms_found)
{
  Elf_Scn* sym_tab_section = elf_getscn(elf_handle, sym_tab_index);
  assert(sym_tab_section);

  Elf_Data* sym_tab_data = elf_getdata(sym_tab_section, 0);
  assert(sym_tab_data);

  GElf_Shdr sheader_mem;
  GElf_Shdr* sym_tab_section_header = gelf_getshdr(sym_tab_section,
						   &sheader_mem);
  Elf_Scn* hash_section = elf_getscn(elf_handle, ht_index);
  assert(hash_section);

  // Poke at the different parts of the hash table and get them ready
  // to be used.
  unsigned long hash = elf_hash(sym_name.c_str());
  Elf_Data* ht_section_data = elf_getdata(hash_section, 0);
  Elf32_Word* ht_data = reinterpret_cast<Elf32_Word*>(ht_section_data->d_buf);
  size_t nb_buckets = ht_data[0];
  size_t nb_chains = ht_data[1];

  if (nb_buckets == 0)
    // An empty hash table.  Not sure if that is possible, but it
    // would mean an empty table of exported symbols.
    return false;

  //size_t nb_chains = ht_data[1];
  Elf32_Word* ht_buckets = &ht_data[2];
  Elf32_Word* ht_chains = &ht_buckets[nb_buckets];

  // Now do the real work.
  size_t bucket = hash % nb_buckets;
  size_t symbol_index = ht_buckets[bucket];

  GElf_Sym symbol;
  const char* sym_name_str;
  size_t sym_size;
  elf_symbol::type sym_type;
  elf_symbol::binding sym_binding;
  bool found = false;

  do
    {
      assert(gelf_getsym(sym_tab_data, symbol_index, &symbol));
      sym_name_str = elf_strptr(elf_handle,
				sym_tab_section_header->sh_link,
				symbol.st_name);
      if (sym_name_str
	  && compare_symbol_name(sym_name_str, sym_name, demangle))
	{
	  sym_type = stt_to_elf_symbol_type(GELF_ST_TYPE(symbol.st_info));
	  sym_binding = stb_to_elf_symbol_binding(GELF_ST_BIND(symbol.st_info));
	  sym_size = symbol.st_size;
	  elf_symbol::version ver;
	  if (get_version_for_symbol(elf_handle, symbol_index,
				     /*get_def_version=*/true, ver))
	    assert(!ver.str().empty());
	  elf_symbol_sptr symbol_found =
	    elf_symbol::create(env,
			       symbol_index,
			       sym_size,
			       sym_name_str,
			       sym_type,
			       sym_binding,
			       symbol.st_shndx != SHN_UNDEF,
			       symbol.st_shndx == SHN_COMMON,
			       ver);
	  syms_found.push_back(symbol_found);
	  found = true;
	}
      symbol_index = ht_chains[symbol_index];
    } while (symbol_index != STN_UNDEF || symbol_index >= nb_chains);

  return found;
}

/// Get the size of the elf class, in bytes.
///
/// @param elf_handle the elf handle to use.
///
/// @return the size computed.
static char
get_elf_class_size_in_bytes(Elf* elf_handle)
{
  char result = 0;
  GElf_Ehdr hdr;

  assert(gelf_getehdr(elf_handle, &hdr));
  int c = hdr.e_ident[EI_CLASS];

  switch (c)
    {
    case ELFCLASS32:
      result = 4;
      break;
    case ELFCLASS64:
      result = 8;
      break;
    default:
      ABG_ASSERT_NOT_REACHED;
    }

  return result;
}

/// Get a given word of a bloom filter, referred to by the index of
/// the word.  The word size depends on the current elf class and this
/// function abstracts that nicely.
///
/// @param elf_handle the elf handle to use.
///
/// @param bloom_filter the bloom filter to consider.
///
/// @param index the index of the bloom filter to return.
static GElf_Word
bloom_word_at(Elf*		elf_handle,
	      Elf32_Word*	bloom_filter,
	      size_t		index)
{
  GElf_Word result = 0;
  GElf_Ehdr h;
  assert(gelf_getehdr(elf_handle, &h));
  int c;
  c = h.e_ident[EI_CLASS];

  switch(c)
    {
    case ELFCLASS32:
      result = bloom_filter[index];
      break ;
    case ELFCLASS64:
      {
	GElf_Word* f= reinterpret_cast<GElf_Word*>(bloom_filter);
	result = f[index];
      }
      break;
    default:
      abort();
    }

  return result;
}

/// The abstraction of the gnu elf hash table.
///
/// The members of this struct are explained at
///   - https://sourceware.org/ml/binutils/2006-10/msg00377.html
///   - https://blogs.oracle.com/ali/entry/gnu_hash_elf_sections.
struct gnu_ht
{
  size_t nb_buckets;
  Elf32_Word* buckets;
  Elf32_Word* chain;
  size_t first_sym_index;
  size_t bf_nwords;
  size_t bf_size;
  Elf32_Word* bloom_filter;
  size_t shift;
  size_t sym_count;
  Elf_Scn* sym_tab_section;
  GElf_Shdr sym_tab_section_header;

  gnu_ht()
    : nb_buckets(0),
      buckets(0),
      chain(0),
      first_sym_index(0),
      bf_nwords(0),
      bf_size(0),
      bloom_filter(0),
      shift(0),
      sym_count(0),
      sym_tab_section(0)
  {}
}; // end struct gnu_ht

/// Setup the members of the gnu hash table.
///
/// @param elf_handle a handle on the elf file to use.
///
/// @param ht_index the index  (into the elf section headers table) of
/// the hash table section to use.
///
/// @param sym_tab_index the index (into the elf section headers
/// table) of the symbol table the gnu hash table is about.
///
/// @param ht the resulting hash table.
///
/// @return true iff the hash table @ ht could be setup.
static bool
setup_gnu_ht(Elf* elf_handle,
	     size_t ht_index,
	     size_t sym_tab_index,
	     gnu_ht& ht)
{
  ht.sym_tab_section = elf_getscn(elf_handle, sym_tab_index);
  assert(ht.sym_tab_section);
  assert(gelf_getshdr(ht.sym_tab_section, &ht.sym_tab_section_header));
  ht.sym_count =
    ht.sym_tab_section_header.sh_size / ht.sym_tab_section_header.sh_entsize;
  Elf_Scn* hash_section = elf_getscn(elf_handle, ht_index);
  assert(hash_section);

  // Poke at the different parts of the hash table and get them ready
  // to be used.
  Elf_Data* ht_section_data = elf_getdata(hash_section, 0);
  Elf32_Word* ht_data = reinterpret_cast<Elf32_Word*>(ht_section_data->d_buf);

  ht.nb_buckets = ht_data[0];
  if (ht.nb_buckets == 0)
    // An empty hash table.  Not sure if that is possible, but it
    // would mean an empty table of exported symbols.
    return false;
  ht.first_sym_index = ht_data[1];
  // The number of words used by the bloom filter.  A size of a word
  // is ELFCLASS.
  ht.bf_nwords = ht_data[2];
  // The shift used by the bloom filter code.
  ht.shift = ht_data[3];
  // The data of the bloom filter proper.
  ht.bloom_filter = &ht_data[4];
  // The size of the bloom filter in 4 bytes word.  This is going to
  // be used to index the 'bloom_filter' above, which is of type
  // Elf32_Word*; thus we need that bf_size be expressed in 4 bytes
  // words.
  ht.bf_size = (get_elf_class_size_in_bytes(elf_handle) / 4) * ht.bf_nwords;
  // The buckets of the hash table.
  ht.buckets = ht.bloom_filter + ht.bf_size;
  // The chain of the hash table.
  ht.chain = ht.buckets + ht.nb_buckets;

  return true;
}

/// Look into the symbol tables of the underlying elf file and find
/// the symbol we are being asked.
///
/// This function uses the GNU hash table for the symbol lookup.
///
/// The reference of for the implementation of this function can be
/// found at:
///   - https://sourceware.org/ml/binutils/2006-10/msg00377.html
///   - https://blogs.oracle.com/ali/entry/gnu_hash_elf_sections.
///
/// @param elf_handle the elf handle to use.
///
/// @param sym_name the name of the symbol to look for.
///
/// @param ht_index the index of the hash table header to use.
///
/// @param sym_tab_index the index of the symbol table header to use
/// with this hash table.
///
/// @param demangle if true, demangle @p sym_name.
///
/// @param syms_found the vector of symbols found with the name @p
/// sym_name.
///
/// @return true if a symbol was actually found.
static bool
lookup_symbol_from_gnu_hash_tab(const environment*		env,
				Elf*				elf_handle,
				const string&			sym_name,
				size_t				ht_index,
				size_t				sym_tab_index,
				bool				demangle,
				vector<elf_symbol_sptr>&	syms_found)
{
  gnu_ht ht;
  if (!setup_gnu_ht(elf_handle, ht_index, sym_tab_index, ht))
    return false;

  // Now do the real work.

  // Compute bloom hashes (GNU hash and second bloom specific hashes).
  size_t h1 = elf_gnu_hash(sym_name.c_str());
  size_t h2 = h1 >> ht.shift;
  // The size of one of the words used in the bloom
  // filter, in bits.
  int c = get_elf_class_size_in_bytes(elf_handle) * 8;
  int n =  (h1 / c) % ht.bf_nwords;
  unsigned char bitmask = (1 << (h1 % c)) | (1 << (h2 % c));

  // Test if the symbol is *NOT* present in this ELF file.
  if ((bloom_word_at(elf_handle, ht.bloom_filter, n) & bitmask) != bitmask)
    return false;

  size_t i = ht.buckets[h1 % ht.nb_buckets];
  if (i == STN_UNDEF)
    return false;

  Elf32_Word stop_word, *stop_wordp;
  elf_symbol::version ver;
  GElf_Sym symbol;
  const char* sym_name_str;
  bool found = false;

  elf_symbol::type sym_type;
  elf_symbol::binding sym_binding;

  // Let's walk the hash table and record the versions of all the
  // symbols which name equal sym_name.
  for (i = ht.buckets[h1 % ht.nb_buckets],
	 stop_wordp = &ht.chain[i - ht.first_sym_index];
       i != STN_UNDEF
	 && (stop_wordp
	     < ht.chain + (ht.sym_count - ht.first_sym_index));
       ++i, ++stop_wordp)
    {
      stop_word = *stop_wordp;
      if ((stop_word & ~ 1)!= (h1 & ~1))
	// A given bucket can reference several hashes.  Here we
	// stumbled accross a hash value different from the one we are
	// looking for.  Let's keep walking.
	continue;

      assert(gelf_getsym(elf_getdata(ht.sym_tab_section, 0),
			 i, &symbol));
      sym_name_str = elf_strptr(elf_handle,
				ht.sym_tab_section_header.sh_link,
				symbol.st_name);
      if (sym_name_str
	  && compare_symbol_name(sym_name_str, sym_name, demangle))
	{
	  // So we found a symbol (in the symbol table) that equals
	  // sym_name.  Now lets try to get its version and record it.
	  sym_type = stt_to_elf_symbol_type(GELF_ST_TYPE(symbol.st_info));
	  sym_binding = stb_to_elf_symbol_binding(GELF_ST_BIND(symbol.st_info));

	  if (get_version_for_symbol(elf_handle, i,
				     /*get_def_version=*/true,
				     ver))
	    assert(!ver.str().empty());

	  elf_symbol_sptr symbol_found =
	    elf_symbol::create(env, i, symbol.st_size, sym_name_str,
			       sym_type, sym_binding,
			       symbol.st_shndx != SHN_UNDEF,
			       symbol.st_shndx == SHN_COMMON,
			       ver);
	  syms_found.push_back(symbol_found);
	  found = true;
	}

      if (stop_word & 1)
	// The last bit of the stop_word is 1.  That means we need to
	// stop here.  We reached the end of the chain of values
	// referenced by the hask bucket.
	break;
    }
  return found;
}

/// Look into the symbol tables of the underlying elf file and find
/// the symbol we are being asked.
///
/// This function uses the elf hash table (be it the GNU hash table or
/// the sysv hash table) for the symbol lookup.
///
/// @param env the environment we are operating from.
///
/// @param elf_handle the elf handle to use.
///
/// @param ht_kind the kind of hash table to use.  This is returned by
/// the function function find_hash_table_section_index.
///
/// @param ht_index the index (in the section headers table) of the
/// hash table section to use.
///
/// @param sym_tab_index the index (in section headers table) of the
/// symbol table index to use with this hash table.
///
/// @param symbol_name the name of the symbol to look for.
///
/// @param demangle if true, demangle @p sym_name.
///
/// @param syms_found the symbols that were actually found with the
/// name @p symbol_name.
///
/// @return true iff the function found the symbol from the elf hash
/// table.
static bool
lookup_symbol_from_elf_hash_tab(const environment*		env,
				Elf*				elf_handle,
				hash_table_kind		ht_kind,
				size_t				ht_index,
				size_t				symtab_index,
				const string&			symbol_name,
				bool				demangle,
				vector<elf_symbol_sptr>&	syms_found)
{
  if (elf_handle == 0 || symbol_name.empty())
    return false;

  if (ht_kind == NO_HASH_TABLE_KIND)
    return false;

  if (ht_kind == SYSV_HASH_TABLE_KIND)
    return lookup_symbol_from_sysv_hash_tab(env,
					    elf_handle, symbol_name,
					    ht_index,
					    symtab_index,
					    demangle,
					    syms_found);
  else if (ht_kind == GNU_HASH_TABLE_KIND)
    return lookup_symbol_from_gnu_hash_tab(env,
					   elf_handle, symbol_name,
					   ht_index,
					   symtab_index,
					   demangle,
					   syms_found);
  return false;
}

/// Lookup a symbol from the symbol table directly.
///
///
/// @param env the environment we are operating from.
///
/// @param elf_handle the elf handle to use.
///
/// @param sym_name the name of the symbol to look up.
///
/// @param sym_tab_index the index (in the section headers table) of
/// the symbol table section.
///
/// @param demangle if true, demangle the names found in the symbol
/// table before comparing them with @p sym_name.
///
/// @param sym_name_found the actual name of the symbol found.
///
/// @param sym_type the type of the symbol found.
///
/// @param sym_binding the binding of the symbol found.
///
/// @param sym_versions the versions of the symbol found.
///
/// @return true iff the symbol was found.
static bool
lookup_symbol_from_symtab(const environment*		env,
			  Elf*				elf_handle,
			  const string&		sym_name,
			  size_t			sym_tab_index,
			  bool				demangle,
			  vector<elf_symbol_sptr>&	syms_found)
{
  // TODO: read all of the symbol table, store it in memory in a data
  // structure that associates each symbol with its versions and in
  // which lookups of a given symbol is fast.
  Elf_Scn* sym_tab_section = elf_getscn(elf_handle, sym_tab_index);
  assert(sym_tab_section);

  GElf_Shdr header_mem;
  GElf_Shdr * sym_tab_header = gelf_getshdr(sym_tab_section,
					    &header_mem);

  size_t symcount = sym_tab_header->sh_size / sym_tab_header->sh_entsize;
  Elf_Data* symtab = elf_getdata(sym_tab_section, NULL);
  GElf_Sym* sym;
  char* name_str = 0;
  elf_symbol::version ver;
  bool found = false;

  for (size_t i = 0; i < symcount; ++i)
    {
      GElf_Sym sym_mem;
      sym = gelf_getsym(symtab, i, &sym_mem);
      name_str = elf_strptr(elf_handle,
			    sym_tab_header->sh_link,
			    sym->st_name);

      if (name_str && compare_symbol_name(name_str, sym_name, demangle))
	{
	  elf_symbol::type sym_type =
	    stt_to_elf_symbol_type(GELF_ST_TYPE(sym->st_info));
	  elf_symbol::binding sym_binding =
	    stb_to_elf_symbol_binding(GELF_ST_BIND(sym->st_info));
	  bool sym_is_defined = sym->st_shndx != SHN_UNDEF;
	  bool sym_is_common = sym->st_shndx == SHN_COMMON;
	  if (get_version_for_symbol(elf_handle, i,
				     /*get_def_version=*/sym_is_defined,
				     ver))
	    assert(!ver.str().empty());
	  elf_symbol_sptr symbol_found =
	    elf_symbol::create(env, i, sym->st_size,
			       name_str, sym_type,
			       sym_binding, sym_is_defined,
			       sym_is_common, ver);
	  syms_found.push_back(symbol_found);
	  found = true;
	}
    }

  if (found)
    return true;

  return false;
}

/// Look into the symbol tables of the underlying elf file and see
/// if we find a given symbol.
///
/// @param env the environment we are operating from.
///
/// @param symbol_name the name of the symbol to look for.
///
/// @param demangle if true, try to demangle the symbol name found in
/// the symbol table before comparing it to @p symbol_name.
///
/// @param syms_found the list of symbols found, with the name @p
/// symbol_name.
///
/// @param sym_type this is set to the type of the symbol found.  This
/// shall b a standard elf.h value for symbol types, that is SHT_OBJECT,
/// STT_FUNC, STT_IFUNC, etc ...
///
/// Note that this parameter is set iff the function returns true.
///
/// @param sym_binding this is set to the binding of the symbol found.
/// This is a standard elf.h value of the symbol binding kind, that
/// is, STB_LOCAL, STB_GLOBAL, or STB_WEAK.
///
/// @param symbol_versions the versions of the symbol @p symbol_name,
/// if it was found.
///
/// @return true iff a symbol with the name @p symbol_name was found.
static bool
lookup_symbol_from_elf(const environment*		env,
		       Elf*				elf_handle,
		       const string&			symbol_name,
		       bool				demangle,
		       vector<elf_symbol_sptr>&	syms_found)
{
  size_t hash_table_index = 0, symbol_table_index = 0;
  hash_table_kind ht_kind = NO_HASH_TABLE_KIND;

  if (!demangle)
    ht_kind = find_hash_table_section_index(elf_handle,
					    hash_table_index,
					    symbol_table_index);

  if (ht_kind == NO_HASH_TABLE_KIND)
    {
      if (!find_symbol_table_section_index(elf_handle, symbol_table_index))
	return false;

      return lookup_symbol_from_symtab(env,
				       elf_handle,
				       symbol_name,
				       symbol_table_index,
				       demangle,
				       syms_found);
    }

  return lookup_symbol_from_elf_hash_tab(env,
					 elf_handle,
					 ht_kind,
					 hash_table_index,
					 symbol_table_index,
					 symbol_name,
					 demangle,
					 syms_found);
}

/// Look into the symbol tables of the underlying elf file and see if
/// we find a given public (global or weak) symbol of function type.
///
/// @param env the environment we are operating from.
///
/// @param elf_handle the elf handle to use for the query.
///
/// @param symbol_name the function symbol to look for.
///
/// @param func_syms the vector of public functions symbols found, if
/// any.
///
/// @return true iff the symbol was found.
static bool
lookup_public_function_symbol_from_elf(const environment*		env,
				       Elf*				elf_handle,
				       const string&			symbol_name,
				       vector<elf_symbol_sptr>&	func_syms)
{
  vector<elf_symbol_sptr> syms_found;
  bool found = false;

  if (lookup_symbol_from_elf(env, elf_handle, symbol_name,
			     /*demangle=*/false, syms_found))
    {
      for (vector<elf_symbol_sptr>::const_iterator i = syms_found.begin();
	   i != syms_found.end();
	   ++i)
	{
	  elf_symbol::type type = (*i)->get_type();
	  elf_symbol::binding binding = (*i)->get_binding();

	  if ((type == elf_symbol::FUNC_TYPE
	       || type == elf_symbol::GNU_IFUNC_TYPE
	       || type == elf_symbol::COMMON_TYPE)
	      && (binding == elf_symbol::GLOBAL_BINDING
		  || binding == elf_symbol::WEAK_BINDING))
	    {
	      func_syms.push_back(*i);
	      found = true;
	    }
      }
    }

  return found;
}

/// Look into the symbol tables of the underlying elf file and see if
/// we find a given public (global or weak) symbol of variable type.
///
/// @param env the environment we are operating from.
///
/// @param elf the elf handle to use for the query.
///
/// @param symname the variable symbol to look for.
///
/// @param var_syms the vector of public variable symbols found, if any.
///
/// @return true iff symbol @p symname was found.
static bool
lookup_public_variable_symbol_from_elf(const environment*		env,
				       Elf*				elf,
				       const string&			symname,
				       vector<elf_symbol_sptr>&	var_syms)
{
  vector<elf_symbol_sptr> syms_found;
  bool found = false;

  if (lookup_symbol_from_elf(env, elf, symname, /*demangle=*/false, syms_found))
    {
      for (vector<elf_symbol_sptr>::const_iterator i = syms_found.begin();
	   i != syms_found.end();
	   ++i)
	if ((*i)->is_variable()
	    && ((*i)->get_binding() == elf_symbol::GLOBAL_BINDING
		|| (*i)->get_binding() == elf_symbol::WEAK_BINDING))
	    {
	      var_syms.push_back(*i);
	      found = true;
	    }
    }

  return found;
}

/// Get data tag information of an ELF file by looking up into its
/// dynamic segment
///
/// @param elf the elf handle to use for the query.
///
/// @param dt_tag data tag to look for in dynamic segment
/// @param dt_tag_data vector of found information for a given @p data_tag
///
/// @return true iff data tag @p data_tag was found

bool
lookup_data_tag_from_dynamic_segment(Elf*                       elf,
                                     Elf64_Sxword               data_tag,
                                     vector<string>&            dt_tag_data)
{
  size_t num_prog_headers = 0;
  bool found = false;
  if (elf_getphdrnum(elf, &num_prog_headers) < 0)
    return found;

  // Cycle through each program header.
  for (size_t i = 0; i < num_prog_headers; ++i)
    {
      GElf_Phdr phdr_mem;
      GElf_Phdr *phdr = gelf_getphdr(elf, i, &phdr_mem);
      if (phdr == NULL || phdr->p_type != PT_DYNAMIC)
        continue;

      // Poke at the dynamic segment like a section, so that we can
      // get its section header information; also we'd like to read
      // the data of the segment by using elf_getdata() but that
      // function needs a Elf_Scn data structure to act on.
      // Elfutils doesn't really have any particular function to
      // access segment data, other than the functions used to
      // access section data.
      Elf_Scn *dynamic_section = gelf_offscn(elf, phdr->p_offset);
      GElf_Shdr  shdr_mem;
      GElf_Shdr *dynamic_section_header = gelf_getshdr(dynamic_section,
                                                        &shdr_mem);
      if (dynamic_section_header == NULL
          || dynamic_section_header->sh_type != SHT_DYNAMIC)
        continue;

      // Get data of the dynamic segment (seen as a section).
      Elf_Data *data = elf_getdata(dynamic_section, NULL);
      if (data == NULL)
        continue;

      // Get the index of the section headers string table.
      size_t string_table_index = 0;
      assert (elf_getshdrstrndx(elf, &string_table_index) >= 0);

      size_t dynamic_section_header_entry_size = gelf_fsize(elf,
                                                            ELF_T_DYN, 1,
                                                            EV_CURRENT);

      GElf_Shdr link_mem;
      GElf_Shdr *link =
        gelf_getshdr(elf_getscn(elf,
                                dynamic_section_header->sh_link),
                      &link_mem);
      assert(link != NULL);

      size_t num_dynamic_section_entries =
        dynamic_section_header->sh_size / dynamic_section_header_entry_size;

      // Now walk through all the DT_* data tags that are in the
      // segment/section
      for (size_t j = 0; j < num_dynamic_section_entries; ++j)
        {
          GElf_Dyn dynamic_section_mem;
          GElf_Dyn *dynamic_section = gelf_getdyn(data,
                                                  j,
                                                  &dynamic_section_mem);
          if (dynamic_section->d_tag == data_tag)
            {
              dt_tag_data.push_back(elf_strptr(elf,
                                               dynamic_section_header->sh_link,
                                              dynamic_section->d_un.d_val));
              found = true;
            }
        }
    }
  return found;
}

/// Convert the type of ELF file into @ref elf_type.
///
/// @param elf the elf handle to use for the query.
///
/// @return the @ref elf_type for a given elf type.
static elf_type
elf_file_type(Elf* elf)
{
  GElf_Ehdr ehdr_mem;
  GElf_Ehdr *header = gelf_getehdr (elf, &ehdr_mem);
  vector<string> dt_debug_data;

  switch (header->e_type)
    {
    case ET_DYN:
      if (lookup_data_tag_from_dynamic_segment(elf, DT_DEBUG, dt_debug_data))
	return ELF_TYPE_PI_EXEC;
      else
	return ELF_TYPE_DSO;
    case ET_EXEC:
      return ELF_TYPE_EXEC;
    case ET_REL:
      return ELF_TYPE_RELOCATABLE;
    default:
      return ELF_TYPE_UNKNOWN;
    }
}

/// The context used to build ABI corpus from debug info in DWARF
/// format.
///
/// This context is to be created by create_read_context().  It's then
/// passed to all the routines that read specific dwarf bits as they
/// get some important data from it.
class read_context
{
  environment*			env_;
  suppr::suppressions_type	supprs_;
  unsigned short		dwarf_version_;
  Dwfl_Callbacks		offline_callbacks_;
  dwfl_sptr			handle_;
  Dwarf*			dwarf_;
  // The alternate debug info.  Alternate debug info sections are a
  // DWARF extension as of DWARF4 and are described at
  // http://www.dwarfstd.org/ShowIssue.php?issue=120604.1.  Below are
  // the file desctor used to access the alternate debug info
  // sections, and the representation of the DWARF debug info.  Both
  // need to be freed after we are done using them, with fclose and
  // dwarf_end.
  int				alt_fd_;
  Dwarf*			alt_dwarf_;
  string			alt_debug_info_path_;
  // The address range of the offline elf file we are looking at.
  Dwfl_Module*			elf_module_;
  mutable Elf*			elf_handle_;
  const string			elf_path_;
  Dwarf_Die*			cur_tu_die_;
  // This is a map that associates a decl to the DIE that represents
  // it.  This is for DIEs that come from the main debug info file we
  // are looking at.
  die_decl_map_type		die_decl_map_;
  // This is a similar map as die_decl_map_, but it's for DIEs that
  // com from the alternate debug info file.  Alternate debug info is
  // described by the DWARF extension (as of DWARF4) described at
  // http://www.dwarfstd.org/ShowIssue.php?issue=120604.1.
  die_decl_map_type		alternate_die_decl_map_;
  die_decl_map_type		type_unit_die_decl_map_;
  // This is a map that associates DIE offsets to their types.  This
  // is for DIEs that represent types.  Note that it's for DIEs
  // defined in the main debug info section.
  die_type_map_type		die_type_map_;
  // This is a map that associates DIE offsets to their types.  This
  // is for DIEs that represent types.  Note that it's for DIEs
  // defined in the alternate debug info section.
  die_type_map_type		alternate_die_type_map_;
  die_type_map_type		type_unit_die_type_map_;
  die_class_or_union_map_type	die_wip_classes_map_;
  die_class_or_union_map_type	alternate_die_wip_classes_map_;
  die_class_or_union_map_type	type_unit_die_wip_classes_map_;
  die_function_type_map_type	die_wip_function_types_map_;
  die_function_type_map_type	alternate_die_wip_function_types_map_;
  die_function_type_map_type	type_unit_die_wip_function_types_map_;
  die_function_decl_map_type	die_function_with_no_symbol_map_;
  vector<Dwarf_Off>		types_to_canonicalize_;
  vector<Dwarf_Off>		alt_types_to_canonicalize_;
  vector<Dwarf_Off>		type_unit_types_to_canonicalize_;
  string_classes_map		decl_only_classes_map_;
  die_tu_map_type		die_tu_map_;
  corpus_sptr			cur_corpus_;
  translation_unit_sptr	cur_tu_;
  scope_stack_type		scope_stack_;
  offset_offset_map		primary_die_parent_map_;
  // A map that associates each tu die to a vector of unit import
  // points, in the main debug info
  tu_die_imported_unit_points_map_type tu_die_imported_unit_points_map_;
  // A map that associates each tu die to a vector of unit import
  // points, in the alternate debug info
  tu_die_imported_unit_points_map_type alt_tu_die_imported_unit_points_map_;
  tu_die_imported_unit_points_map_type type_units_tu_die_imported_unit_points_map_;
  // A DIE -> parent map for DIEs coming from the alternate debug info
  // file.
  offset_offset_map		alternate_die_parent_map_;
  offset_offset_map		type_section_die_parent_map_;
  list<var_decl_sptr>		var_decls_to_add_;
  Elf_Scn*			symtab_section_;
  // The "Official procedure descriptor section, aka .opd", used in
  // ppc64 elf v1 binaries.  This section contains the procedure
  // descriptors on that platform.
  Elf_Scn*			opd_section_;
  bool				symbol_versionning_sections_loaded_;
  bool				symbol_versionning_sections_found_;
  Elf_Scn*			versym_section_;
  Elf_Scn*			verdef_section_;
  Elf_Scn*			verneed_section_;
  addr_elf_symbol_sptr_map_sptr fun_addr_sym_map_;
  // On PPC64, the function entry point address is different from the
  // GElf_Sym::st_value value, which is the address of the descriptor
  // of the function.  The map below thus associates the address of
  // the entry point to the function symbol.  If we are not on ppc64,
  // then this map ought to be empty.  Only the fun_addr_sym_map_ is
  // used in that case.  On ppc64, though, both maps are used.
  addr_elf_symbol_sptr_map_sptr fun_entry_addr_sym_map_;
  string_elf_symbols_map_sptr	fun_syms_;
  addr_elf_symbol_sptr_map_sptr var_addr_sym_map_;
  string_elf_symbols_map_sptr	var_syms_;
  string_elf_symbols_map_sptr	undefined_fun_syms_;
  string_elf_symbols_map_sptr	undefined_var_syms_;
  vector<string>		dt_needed_;
  string			dt_soname_;
  string			elf_architecture_;
  corpus::exported_decls_builder* exported_decls_builder_;
  bool				load_all_types_;
  bool				show_stats_;
  bool				do_log_;

  read_context();

public:
  read_context(const string& elf_path)
    : env_(),
      dwarf_version_(),
      handle_(),
      dwarf_(),
      alt_fd_(),
      alt_dwarf_(),
      elf_module_(),
      elf_handle_(),
      elf_path_(elf_path),
      cur_tu_die_(),
      symtab_section_(),
      opd_section_(),
      symbol_versionning_sections_loaded_(),
      symbol_versionning_sections_found_(),
      versym_section_(),
      verdef_section_(),
      verneed_section_(),
      exported_decls_builder_(),
      load_all_types_(),
      show_stats_(),
      do_log_()
  {
    memset(&offline_callbacks_, 0, sizeof(offline_callbacks_));
  }

  /// Detructor of the @ref read_context type.
  ~read_context()
  {
    if (alt_fd_)
      {
	close(alt_fd_);
	if (alt_dwarf_)
	  dwarf_end(alt_dwarf_);
      }
  }

  /// Clear the data that is relevant only for the current translation
  /// unit being read.  The rest of the data is relevant for the
  /// entire ABI corpus.
  void
  clear_per_translation_unit_data()
  {
    while (!scope_stack().empty())
      scope_stack().pop();
    var_decls_to_re_add_to_tree().clear();
  }

  /// Clear the data that is relevant for the current corpus being
  /// read.
  void
  clear_per_corpus_data()
  {
    die_decl_map().clear();
    alternate_die_decl_map().clear();
    clear_die_type_maps();
    clear_types_to_canonicalize();
  }

  /// Getter for the current environment.
  ///
  /// @return the current environment.
  const ir::environment*
  env() const
  {return env_;}

  /// Getter for the current environment.
  ///
  /// @return the current environment.
  ir::environment*
  env()
  {return env_;}

  /// Setter for the current environment.
  ///
  /// @param env the new current environment.
  void
  env(ir::environment* env)
  {env_ = env;}

  /// Getter of the suppression specifications to be used during
  /// ELF/DWARF parsing.
  ///
  /// @return the suppression specifications.
  const suppr::suppressions_type&
  get_suppressions() const
  {return supprs_;}

  /// Getter of the suppression specifications to be used during
  /// ELF/DWARF parsing.
  ///
  /// @return the suppression specifications.
  suppr::suppressions_type&
  get_suppressions()
  {return supprs_;}

  /// Getter for the callbacks of the Dwarf Front End library of
  /// elfutils that is used by this reader to read dwarf.
  ///
  /// @return the callbacks.
  const Dwfl_Callbacks*
  offline_callbacks() const
  {return &offline_callbacks_;}

  /// Getter for the callbacks of the Dwarf Front End library of
  /// elfutils that is used by this reader to read dwarf.
  /// @returnthe callbacks
  Dwfl_Callbacks*
  offline_callbacks()
  {return &offline_callbacks_;}

  /// Constructor for a default Dwfl handle that knows how to load debug
  /// info from a library or executable elf file.
  ///
  /// @param debug_info_root_path a pointer to the root path under which
  /// to look for the debug info of the elf files that are later handled
  /// by the Dwfl.  This for cases where the debug info is split into a
  /// different file from the binary we want to inspect.  On Red Hat
  /// compatible systems, this root path is usually /usr/lib/debug by
  /// default.  If this argument is set to NULL, then "./debug" and
  /// /usr/lib/debug will be searched for sub-directories containing the
  /// debug info file.  Note that for now, elfutils wants this path to
  /// be absolute otherwise things just don't work and the debug info is
  /// not found.
  ///
  /// @return the constructed Dwfl handle.
  void
  create_default_dwfl(char** debug_info_root_path)
  {
    offline_callbacks()->find_debuginfo = dwfl_standard_find_debuginfo;
    offline_callbacks()->section_address = dwfl_offline_section_address;
    offline_callbacks()->debuginfo_path = debug_info_root_path;
    handle_.reset(dwfl_begin(offline_callbacks()),
		  dwfl_deleter());
  }

  unsigned short
  dwarf_version() const
  {return dwarf_version_;}

  void
  dwarf_version(unsigned short v)
  {dwarf_version_ = v;}

  /// Getter for a smart pointer to a handle on the dwarf front end
  /// library that we use to read dwarf.
  ///
  /// @return the dwfl handle.
  dwfl_sptr
  dwfl_handle() const
  {return handle_;}

  /// Setter for a smart pointer to a handle on the dwarf front end
  /// library that we use to read dwarf.
  ///
  /// @param h the new dwfl handle.
  void
  dwfl_handle(dwfl_sptr& h)
  {handle_ = h;}

  Dwfl_Module*
  elf_module() const
  {return elf_module_;}

  /// Return the ELF descriptor for the binary we are analizing.
  ///
  /// @return a pointer to the Elf descriptor representing the binary
  /// we are analizing.
  Elf*
  elf_handle() const
  {
    if (elf_handle_ == 0)
      {
	if (elf_module())
	  {
	    GElf_Addr bias = 0;
	    elf_handle_ = dwfl_module_getelf(elf_module(), &bias);
	  }
      }
    return elf_handle_;
  }

  /// Return the ELF descriptor used for DWARF access.
  ///
  /// This can be the same as read_context::elf_handle() above, if the
  /// DWARF info is in the same ELF file as the one of the binary we
  /// are analizing.  It is different if e.g, the debug info is split
  /// from the ELF file we are analizing.
  ///
  /// @return a pointer to the ELF descriptor used to access debug
  /// info.
  Elf*
  dwarf_elf_handle() const
  {return dwarf_getelf(dwarf());}

  /// Test if the debug information is in a separate ELF file wrt the
  /// main ELF file of the program (application or shared library) we
  /// are analizing.
  ///
  /// @return true if the debug information is in a separate ELF file
  /// compared to the main ELF file of the program (application or
  /// shared library) that we are looking at.
  bool
  dwarf_is_splitted() const
  {return dwarf_elf_handle() != elf_handle();}

  /// Load the debug info associated with an elf file that is at a
  /// given path.
  ///
  /// @return a pointer to the DWARF debug info pointer upon
  /// successful debug info loading, NULL otherwise.
  Dwarf*
  load_debug_info()
  {
    if (!dwfl_handle())
      return 0;

    if (dwarf_)
      return dwarf_;

    elf_module_ =
      dwfl_report_offline(dwfl_handle().get(),
			  basename(const_cast<char*>(elf_path().c_str())),
			  elf_path().c_str(),
			  -1);
    dwfl_report_end(dwfl_handle().get(), 0, 0);

    Dwarf_Addr bias = 0;
    dwarf_ = dwfl_module_getdwarf(elf_module_, &bias);
    if (!alt_dwarf_)
      alt_dwarf_ = find_alt_debug_info(elf_module_,
				       alt_debug_info_path_,
				       alt_fd_);

    return dwarf_;
  }

  /// Return the main debug info we are looking at.
  ///
  /// @return the main debug info.
  Dwarf*
  dwarf() const
  {return dwarf_;}

  /// Return the alternate debug info we are looking at.
  ///
  /// Note that "alternate debug info sections" is a GNU extension as
  /// of DWARF4 and is described at
  /// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1
  ///
  /// @return the alternate debug info.
  Dwarf*
  alt_dwarf() const
  {return alt_dwarf_;}

  /// Return the path to the alternate debug info as contained in the
  /// .gnu_debugaltlink section of the main elf file.
  ///
  /// Note that "alternate debug info sections" is a GNU extension as
  /// of DWARF4 and is described at
  /// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1
  ///
  /// @return the path to the alternate debug info file, or an empty
  /// path if no alternate debug info file is associated.
  const string&
  alt_debug_info_path() const
  {return alt_debug_info_path_;}

  const string&
  elf_path() const
  {return elf_path_;}

  const Dwarf_Die*
  cur_tu_die() const
  {return cur_tu_die_;}

  void
  cur_tu_die(Dwarf_Die* cur_tu_die)
  {cur_tu_die_ = cur_tu_die;}

  /// Return the map that associates a decl to the DIE that represents
  /// it.  This if for DIEs that come from the main debug info file we
  /// are looking at.
  ///
  /// @return the die -> decl map of the main debug info file.
  const die_decl_map_type&
  die_decl_map() const
  {return die_decl_map_;}

  /// Get the source of the DIE.
  ///
  /// The function returns an enumerator value saying if the DIE comes
  /// from the .debug_info section of the primary debug info file, the
  /// .debug_info section of the alternate debug info file, or the
  /// .debug_types section.
  ///
  /// @param die the DIE to get the source of.
  ///
  /// @param source out parameter.  The function sets this parameter
  /// to the source of the DIE @p iff it returns true.
  ///
  /// @return true iff the source of the DIE could be determined and
  /// returned.
  bool
  get_die_source(Dwarf_Die *die, die_source &source) const
  {
    assert(die);
    return get_die_source(*die, source);
  }

  /// Get the source of the DIE.
  ///
  /// The function returns an enumerator value saying if the DIE comes
  /// from the .debug_info section of the primary debug info file, the
  /// .debug_info section of the alternate debug info file, or the
  /// .debug_types section.
  ///
  /// @param die the DIE to get the source of.
  ///
  /// @param source out parameter.  The function sets this parameter
  /// to the source of the DIE @p iff it returns true.
  ///
  /// @return true iff the source of the DIE could be determined and
  /// returned.
  bool
  get_die_source(Dwarf_Die &die, die_source &source) const
  {
    Dwarf_Die cu_die;
    Dwarf_Die cu_kind;
    uint8_t address_size = 0, offset_size = 0;
    if (!dwarf_diecu(&die, &cu_die, &address_size, &offset_size))
      return false;

    Dwarf_Half version = 0;
    Dwarf_Off abbrev_offset = 0;
    uint64_t type_signature = 0;
    Dwarf_Off type_offset = 0;
    if (!dwarf_cu_die(cu_die.cu, &cu_kind,
		      &version, &abbrev_offset,
		      &address_size, &offset_size,
		      &type_signature, &type_offset))
      return false;

    int tag = dwarf_tag(&cu_kind);

    if (tag == DW_TAG_compile_unit
	|| tag == DW_TAG_partial_unit)
      {
	Dwarf *die_dwarf = dwarf_cu_getdwarf(cu_die.cu);
	if (dwarf() == die_dwarf)
	  source = PRIMARY_DEBUG_INFO_DIE_SOURCE;
	else if (alt_dwarf() == die_dwarf)
	  source = ALT_DEBUG_INFO_DIE_SOURCE;
	else
	  ABG_ASSERT_NOT_REACHED;
      }
    else if (tag == DW_TAG_type_unit)
      source = TYPE_UNIT_DIE_SOURCE;
    else
      return false;

    return true;
  }

  /// Return the map that associates a decl to the DIE that represents
  /// it.  This if for DIEs that come from the main debug info file we
  /// are looking at.
  ///
  /// @return the die -> decl map of the main debug info file.
  die_decl_map_type&
  die_decl_map()
  {return die_decl_map_;}

  /// Return the map that associates a decl to the DIE that represents
  /// it.  This if for DIEs that come from the alternate debug info file.
  ///
  /// Note that "alternate debug info sections" is a GNU extension as
  /// of DWARF4 and is described at
  /// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1
  ///
  /// @return the die -> decl map of the alternate debug info file.
  const die_decl_map_type&
  alternate_die_decl_map() const
  {return alternate_die_decl_map_;}

  /// Return the map that associates a decl to the DIE that represents
  /// it.  This if for DIEs that come from the alternate debug info file.
  ///
  /// Note that "alternate debug info sections" is a GNU extension as
  /// of DWARF4 and is described at
  /// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1
  ///
  /// @return the die -> decl map of the alternate debug info file.
  die_decl_map_type&
  alternate_die_decl_map()
  {return alternate_die_decl_map_;}

private:
  /// Add an entry to the die->decl map for DIEs coming from the main
  /// (or primary) debug info file.
  ///
  /// @param die_offset the DIE offset of the DIE we are interested in.
  ///
  /// @param decl the decl we are interested in.
  void
  associate_die_to_decl_primary(size_t die_offset,
				decl_base_sptr decl)
  {die_decl_map()[die_offset] = decl;}

  /// Add an entry to the die->decl map for DIEs coming from the
  /// alternate debug info file.
  ///
  /// Note that "alternate debug info sections" is a GNU extension as
  /// of DWARF4 and is described at
  /// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1
  ///
  /// @param die_offset the DIE offset of the DIE we are interested in.
  ///
  /// @param decl the decl we are interested in.
  void
  associate_die_to_decl_alternate(size_t die_offset,
				  decl_base_sptr decl)
  {alternate_die_decl_map()[die_offset] = decl;}

  /// Add an entry to the die->decl map for DIEs coming from the
  /// type unit section.
  ///
  /// @param die_offset the DIE offset of the DIE we are interested in.
  ///
  /// @param decl the decl we are interested in.
  void
  associate_die_to_decl_from_type_unit(size_t die_offset, decl_base_sptr decl)
  {type_unit_die_decl_map_[die_offset] = decl;}

public:

  /// Add an entry to the relevant die->decl map.
  ///
  /// @param die_offset the offset of the DIE to add the the map.
  ///
  /// @param source where the DIE comes from.
  ///
  /// Note that "alternate debug info sections" is a GNU extension as
  /// of DWARF4 and is described at
  /// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1
  ///
  /// @param decl the decl to consider.
  void
  associate_die_to_decl(size_t die_offset,
			die_source source,
			decl_base_sptr decl)
  {
    switch(source)
      {
      case PRIMARY_DEBUG_INFO_DIE_SOURCE:
	associate_die_to_decl_primary(die_offset, decl);
	break;
      case ALT_DEBUG_INFO_DIE_SOURCE:
	associate_die_to_decl_alternate(die_offset, decl);
	break;
      case TYPE_UNIT_DIE_SOURCE:
	associate_die_to_decl_from_type_unit(die_offset, decl);
	break;
      case NO_DEBUG_INFO_DIE_SOURCE:
      case NUMBER_OF_DIE_SOURCES:
	ABG_ASSERT_NOT_REACHED;
      }
  }

public:
  /// Lookup the decl for a given DIE.  This works on DIEs that come
  /// from the main debug info sections.
  ///
  /// @param die_offset the offset of the DIE to consider.
  ///
  /// @return the resulting decl, or null if no decl is associated to
  /// the DIE represented by @p die_offset.
  decl_base_sptr
  lookup_decl_from_die_offset_primary(size_t die_offset)
  {
    die_decl_map_type::const_iterator it =
      die_decl_map().find(die_offset);
    if (it == die_decl_map().end())
      return decl_base_sptr();
    return it->second;
  }

  /// Lookup the decl for a given DIE.  This works on DIEs that come
  /// from the alternate debug info sections.
  ///
  /// Note that "alternate debug info sections" is a GNU extension as
  /// of DWARF4 and is described at
  /// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1
  ///
  /// @param die_offset the offset of the DIE to consider.
  ///
  /// @return the resulting decl, or null if no decl is associated to
  /// the DIE represented by @p die_offset.
  decl_base_sptr
  lookup_decl_from_die_offset_alternate(size_t die_offset)
  {
    die_decl_map_type::const_iterator it =
      alternate_die_decl_map().find(die_offset);
    if (it == alternate_die_decl_map().end())
      return decl_base_sptr();
    return it->second;
  }

  /// Lookup the decl for a given DIE.  This works on DIEs that come
  /// from the type unit section.
  ///
  /// @param die_offset the offset of the DIE to consider.
  ///
  /// @return the resulting decl, or null if no decl is associated to
  /// the DIE represented by @p die_offset.
  decl_base_sptr
  lookup_decl_from_type_unit_die_offset(size_t die_offset)
  {
    die_decl_map_type::const_iterator it =
      type_unit_die_decl_map_.find(die_offset);
    if (it == type_unit_die_decl_map_.end())
      return decl_base_sptr();
    return it->second;
  }

  /// Lookup the decl for a given DIE.
  ///
  /// @param die_offset the offset of the DIE to consider.
  ///
  /// @param source where the DIE represented by @p die_offset comes
  /// from.
  ///
  /// Note that "alternate debug info sections" is a GNU extension as
  /// of DWARF4 and is described at
  /// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1
  ///
  /// @return the resulting decl, or null if no decl is associated to
  /// the DIE represented by @p die_offset.
  decl_base_sptr
  lookup_decl_from_die_offset(size_t die_offset, die_source source)
  {
    decl_base_sptr result;

    switch(source)
      {
      case PRIMARY_DEBUG_INFO_DIE_SOURCE:
	result = lookup_decl_from_die_offset_primary(die_offset);
	break;
      case ALT_DEBUG_INFO_DIE_SOURCE:
	result = lookup_decl_from_die_offset_alternate(die_offset);
	break;
      case TYPE_UNIT_DIE_SOURCE:
	result = lookup_decl_from_type_unit_die_offset(die_offset);
	break;
      case NO_DEBUG_INFO_DIE_SOURCE:
      case NUMBER_OF_DIE_SOURCES:
	ABG_ASSERT_NOT_REACHED;
      }

    return result;
  }

  /// Return the map that associates DIEs to the type they represent.
  ///
  /// @param source where the DIE comes from.
  ///
  /// @return return the map that associated DIEs to the type they represent.
  die_type_map_type&
  die_type_map(die_source source)
  {
    switch(source)
      {
      case PRIMARY_DEBUG_INFO_DIE_SOURCE:
	break;
      case ALT_DEBUG_INFO_DIE_SOURCE:
	return alternate_die_type_map_;
      case TYPE_UNIT_DIE_SOURCE:
	return type_unit_die_type_map_;
      case NO_DEBUG_INFO_DIE_SOURCE:
      case NUMBER_OF_DIE_SOURCES:
	// We should not reach this point!
	ABG_ASSERT_NOT_REACHED;
      }
    return die_type_map_;
  }

  /// Clear the type maps that associate a die to a type.
  void
  clear_die_type_maps()
  {
    die_type_map_.clear();
    alternate_die_type_map_.clear();
    type_unit_die_type_map_.clear();
  }

  /// Return the map that associates DIEs to the type they represent.
  ///
  /// @param source where the DIE comes from.
  ///
  /// @return the map that associated DIEs to the type they represent.
  const die_type_map_type&
  die_type_map(die_source source) const
  {return const_cast<read_context*>(this)->die_type_map(source);}

  /// Associated a DIE (representing a type) at a given offset to the
  /// type that it represents.
  ///
  /// @param die_offset the offset of the DIE to consider.
  ///
  /// @param source where the DIE comes from.
  ///
  /// @param type the type to associate the DIE to.
  void
  associate_die_to_type(size_t		die_offset,
			die_source	source,
			type_base_sptr	type)
  {
    if (!type)
      return;

    die_type_map_type& m = die_type_map(source);
    m[die_offset] = type;
  }

  /// Lookup the type associated to a given DIE.
  ///
  /// Note that the DIE must have been associated to type by a
  /// previous invocation of the function
  /// read_context::associate_die_to_type().
  ///
  /// @param die_offset the offset of the DIE to consider.
  ///
  /// @param source where the DIE comes from.
  ///
  /// @return the type associated to the DIE of offset @p die_offset,
  /// or NULL if no type is associated to the DIE.
  type_base_sptr
  lookup_type_from_die_offset(size_t die_offset, die_source source) const
  {
    type_base_sptr result;
    {
      const die_type_map_type& m = die_type_map(source);
      die_type_map_type::const_iterator i = m.find(die_offset);

      if (i != m.end())
	result = i->second;
    }

    if (!result)
      {
	// Maybe we are looking for a class type being constructed?
	const die_class_or_union_map_type& m = die_wip_classes_map(source);
	die_class_or_union_map_type::const_iterator i = m.find(die_offset);

	if (i != m.end())
	  result = i->second;
      }

    if (!result)
      {
	// Maybe we are looking for a function type being constructed?
	const die_function_type_map_type& m =
	  die_wip_function_types_map(source);
	die_function_type_map_type::const_iterator i = m.find(die_offset);

	if (i != m.end())
	  result = i->second;
      }

    return result;
  }

  /// Getter of a map that associates a die that represents a
  /// class/struct with the declaration of the class, while the class
  /// is being constructed.
  ///
  /// @param source where the DIE is from.
  ///
  /// @return the map that associates a DIE to the class that is being
  /// built.
  const die_class_or_union_map_type&
  die_wip_classes_map(die_source source) const
  {return const_cast<read_context*>(this)->die_wip_classes_map(source);}

  /// Getter of a map that associates a die that represents a
  /// class/struct with the declaration of the class, while the class
  /// is being constructed.
  ///
  /// @param source where the DIE comes from.
  ///
  /// @return the map that associates a DIE to the class that is being
  /// built.
  die_class_or_union_map_type&
  die_wip_classes_map(die_source source)
  {
    switch(source)
      {
      case PRIMARY_DEBUG_INFO_DIE_SOURCE:
	break;
      case ALT_DEBUG_INFO_DIE_SOURCE:
	return alternate_die_wip_classes_map_;
      case TYPE_UNIT_DIE_SOURCE:
	return type_unit_die_wip_classes_map_;
      case NO_DEBUG_INFO_DIE_SOURCE:
      case NUMBER_OF_DIE_SOURCES:
	ABG_ASSERT_NOT_REACHED;
      }
    return die_wip_classes_map_;
  }

  /// Getter for a map that associates a die (that represents a
  /// function type) whith a function type, while the function type is
  /// being constructed (WIP == work in progress).
  ///
  /// @param source where the DIE comes from.n
  ///
  /// @return the map of wip function types.
  const die_function_type_map_type&
  die_wip_function_types_map(die_source source) const
  {return const_cast<read_context*>(this)->die_wip_function_types_map(source);}

  /// Getter for a map that associates a die (that represents a
  /// function type) whith a function type, while the function type is
  /// being constructed (WIP == work in progress).
  ///
  /// @param source where DIEs of the map come from.
  ///
  /// @return the map of wip function types.
  die_function_type_map_type&
  die_wip_function_types_map(die_source source)
  {
    switch(source)
      {
      case PRIMARY_DEBUG_INFO_DIE_SOURCE:
	break;
      case ALT_DEBUG_INFO_DIE_SOURCE:
	return alternate_die_wip_function_types_map_;
      case TYPE_UNIT_DIE_SOURCE:
	return type_unit_die_wip_function_types_map_;
      case NO_DEBUG_INFO_DIE_SOURCE:
      case NUMBER_OF_DIE_SOURCES:
	ABG_ASSERT_NOT_REACHED;
      }
    return die_wip_function_types_map_;
  }

  /// Getter for a map that associates a die with a function decl
  /// which has a linkage name but no elf symbol yet.
  ///
  /// This is to fixup function decls with linkage names, but with no
  /// link to their underlying elf symbol.  There are some DIEs like
  /// that in DWARF sometimes, especially when the compiler optimizes
  /// stuff aggressively.
  die_function_decl_map_type&
  die_function_decl_with_no_symbol_map()
  {return die_function_with_no_symbol_map_;}

  /// Return true iff a given offset is for the DIE of a class that is
  /// being built, but that is not fully built yet.  WIP == "work in
  /// progress".
  ///
  /// @param offset the DIE offset to consider.
  ///
  /// @param source where the DIE of the map come from.
  ///
  /// @return true iff @p offset is the offset of the DIE of a class
  /// that is being currently built.
  bool
  is_wip_class_die_offset(Dwarf_Off offset, die_source source) const
  {
    die_class_or_union_map_type::const_iterator i =
      die_wip_classes_map(source).find(offset);
    return (i != die_wip_classes_map(source).end());
  }

  /// Return true iff a given offset is for the DIE of a function type
  /// that is being built at the moment, but is not fully built yet.
  /// WIP == work in progress.
  ///
  /// @param offset DIE offset to consider.
  ///
  /// @param source where the DIE comes from.
  ///
  /// @return true iff @p offset is the offset of the DIE of a
  /// function type that is being currently built.
  bool
  is_wip_function_type_die_offset(Dwarf_Off offset, die_source source) const
  {
    die_function_type_map_type::const_iterator i =
      die_wip_function_types_map(source).find(offset);
    return (i != die_wip_function_types_map(source).end());
  }

  /// Getter for the map of declaration-only classes that are to be
  /// resolved to their definition classes by the end of the corpus
  /// loading.
  ///
  /// @return a map of string -> vector of classes where the key is
  /// the fully qualified name of the class and the value is the
  /// vector of declaration-only class.
  const string_classes_map&
  declaration_only_classes() const
  {return decl_only_classes_map_;}

  /// Getter for the map of declaration-only classes that are to be
  /// resolved to their definition classes by the end of the corpus
  /// loading.
  ///
  /// @return a map of string -> vector of classes where the key is
  /// the fully qualified name of the class and the value is the
  /// vector of declaration-only class.
  string_classes_map&
  declaration_only_classes()
  {return decl_only_classes_map_;}

  /// If a given class is a declaration-only class then stash it on
  /// the side so that at the end of the corpus reading we can resolve
  /// it to its definition.
  ///
  /// @param klass the class to consider.
  void
  maybe_schedule_declaration_only_class_for_resolution(class_decl_sptr& klass)
  {
    if (klass->get_is_declaration_only()
	&& klass->get_definition_of_declaration() == 0)
      {
	string qn = klass->get_qualified_name();
	string_classes_map::iterator record =
	  declaration_only_classes().find(qn);
	if (record == declaration_only_classes().end())
	  declaration_only_classes()[qn].push_back(klass);
	else
	  record->second.push_back(klass);
      }
  }

  /// Test if a given declaration-only class has been scheduled for
  /// resolution to a defined class.
  ///
  /// @param klass the class to consider for the test.
  ///
  /// @return true iff @p klass is a declaration-only class and if
  /// it's been scheduled for resolution to a defined class.
  bool
  is_decl_only_class_scheduled_for_resolution(class_decl_sptr& klass)
  {
    if (klass->get_is_declaration_only())
      return (declaration_only_classes().find(klass->get_qualified_name())
	      != declaration_only_classes().end());

    return false;
  }

  /// Walk the declaration-only classes that have been found during
  /// the building of the corpus and resolve them to their definitions.
  void
  resolve_declaration_only_classes()
  {
    vector<string> resolved_classes;

    for (string_classes_map::iterator i =
	   declaration_only_classes().begin();
	 i != declaration_only_classes().end();
	 ++i)
      {
	bool to_resolve = false;
	for (classes_type::iterator j = i->second.begin();
		 j != i->second.end();
		 ++j)
	  if ((*j)->get_is_declaration_only()
	      && ((*j)->get_definition_of_declaration() == 0))
	    to_resolve = true;

	if (!to_resolve)
	  {
	    resolved_classes.push_back(i->first);
	    continue;
	  }

	if (decl_base_sptr type_decl =
	    lookup_class_type_in_corpus(i->first,
					*current_corpus()))
	  {
	    class_decl_sptr klass = is_class_type(type_decl);
	    assert(klass);
	    if (klass->get_is_declaration_only())
	      klass = is_class_type(klass->get_definition_of_declaration());
	    assert(!klass->get_is_declaration_only());
	    for (classes_type::iterator j = i->second.begin();
		 j != i->second.end();
		 ++j)
	      {
		if ((*j)->get_is_declaration_only()
		    && ((*j)->get_definition_of_declaration() == 0))
		  (*j)->set_definition_of_declaration(klass);
	      }
	    resolved_classes.push_back(i->first);
	  }
      }

    size_t num_decl_only_classes = declaration_only_classes().size(),
      num_resolved = resolved_classes.size();
    if (show_stats())
      cerr << "resolved " << num_resolved
	   << " class declarations out of "
	   << num_decl_only_classes
	   << "\n";

    for (vector<string>::const_iterator i = resolved_classes.begin();
	 i != resolved_classes.end();
	 ++i)
      declaration_only_classes().erase(*i);

    for (string_classes_map::iterator i = declaration_only_classes().begin();
	 i != declaration_only_classes().end();
	 ++i)
      {
	if (show_stats())
	  {
	    if (i == declaration_only_classes().begin())
	      cerr << "Here are the "
		   << num_decl_only_classes - num_resolved
		   << " unresolved class declarations:\n";
	    else
	      cerr << "    " << i->first << "\n";
	  }
      }
  }

  /// Some functions described by DWARF may have their linkage name
  /// set, but no link to their actual underlying elf symbol.  When
  /// these are virtual member functions, comparing the enclosing type
  /// against another one which has its underlying symbol properly set
  /// might lead to spurious type changes.
  ///
  /// If the corpus contains a symbol with the same name as the
  /// linkage name of the function, then set up the link between the
  /// function and its underlying symbol.
  ///
  /// Note that for the moment, only virtual member functions are
  /// fixed up like this.  This is because they really are the only
  /// fuctions of functions that can affect types (in spurious ways).
  void
  fixup_functions_with_no_symbols()
  {
    corpus_sptr corp = current_corpus();
    if (!corp)
      return;

    die_function_decl_map_type &fns_with_no_symbol =
      die_function_decl_with_no_symbol_map();

    if (do_log())
	cerr << fns_with_no_symbol.size()
	     << " functions to fixup, potentially\n";

    for (die_function_decl_map_type::iterator i = fns_with_no_symbol.begin();
	 i != fns_with_no_symbol.end();
	 ++i)
      if (elf_symbol_sptr sym =
	  corp->lookup_function_symbol(i->second->get_linkage_name()))
	{
	  assert(is_member_function(i->second));
	  assert(get_member_function_is_virtual(i->second));
	  i->second->set_symbol(sym);
	  if (do_log())
	    cerr << "fixed up '"
		 << i->second->get_pretty_representation()
		 << "' with symbol '"
		 << sym->get_id_string()
		 << "'\n";
	}

    fns_with_no_symbol.clear();
  }

  /// Return a reference to the vector containing the offsets of the
  /// types that need late canonicalizing.
  ///
  /// @param source whe DIEs referred to by the offsets contained in
  /// the vector to return are from.
  vector<Dwarf_Off>&
  types_to_canonicalize(die_source source)
  {
    switch(source)
      {
      case PRIMARY_DEBUG_INFO_DIE_SOURCE:
	break;
      case ALT_DEBUG_INFO_DIE_SOURCE:
	return alt_types_to_canonicalize_;
      case TYPE_UNIT_DIE_SOURCE:
	return type_unit_types_to_canonicalize_;
      case NO_DEBUG_INFO_DIE_SOURCE:
      case NUMBER_OF_DIE_SOURCES:
	ABG_ASSERT_NOT_REACHED;
      }
    return types_to_canonicalize_;
  }

  /// Return a reference to the vector containing the offsets of the
  /// types that need late canonicalizing.
  ///
  /// @param source where the DIEs referred to by the offset in the
  /// returned vector are from.
  const vector<Dwarf_Off>&
  types_to_canonicalize(die_source source) const
  {return const_cast<read_context*>(this)->types_to_canonicalize(source);}

  /// Clear the containers holding types to canonicalize.
  void
  clear_types_to_canonicalize()
  {
    types_to_canonicalize_.clear();
    alt_types_to_canonicalize_.clear();
    type_unit_types_to_canonicalize_.clear();
  }

  /// Put the offset of a DIE representing a type on a side vector so
  /// that when the reading of the debug info of the current
  /// translation unit is done, we can get back to the type DIE and
  /// from there, to the type it's associated to, and then
  /// canonicalize it.  This what we call late canonicalization.
  ///
  /// @param o the offset of the type DIE to schedule for late type
  /// canonicalization.
  ///
  /// @param source where the DIE referred to by @p o is from.
  void
  schedule_type_for_late_canonicalization(Dwarf_Off o, die_source source)
  {
    // First, some sanity check: ensure that the offset 'o' is for a
    // type DIE that we know about.
    type_base_sptr t = lookup_type_from_die_offset(o, source);
    assert(t);

    // Then really do the scheduling.
    types_to_canonicalize(source).push_back(o);
  }

  /// Canonicalize types which DIE offsets are stored in vectors on
  /// the side.  This is a sub-routine of
  /// read_context::perform_late_type_canonicalizing().
  ///
  /// @param source where the DIE of the types to canonicalize are
  /// from.
  void
  canonicalize_types_scheduled(die_source source)
  {
    if (do_log())
      {
	cerr << "going to canonicalize types";
	corpus_sptr c = current_corpus();
	if (c)
	  cerr << " of corpus " << current_corpus()->get_path();
	cerr << " (DIEs source: " << source << ")\n";
      }

    if (!types_to_canonicalize(source).empty())
      {
	size_t total = types_to_canonicalize(source).size();
	if (do_log())
	  cerr << total << " types to canonicalize\n";
	for (size_t i = 0; i < total; ++i)
	  {
	    Dwarf_Off element = types_to_canonicalize(source)[i];
	    type_base_sptr t =
	      lookup_type_from_die_offset(element, source);
	    assert(t);
	    if (do_log())
	      {
		cerr << "canonicalizing type "
		     << get_pretty_representation(t, false)
		     << " [" << i + 1 << "/" << total << "]";
		if (corpus_sptr c = current_corpus())
		  cerr << "@" << c->get_path();
		cerr << " ...";
	      }
	    canonicalize(t);
	    if (do_log())
	      cerr << " DONE\n";
	  }
      }
    if (do_log())
      cerr << "finished canonicalizing types.  (source: "
	   << source << ")\n";
  }

  /// Compute the number of canonicalized and missed types in the late
  /// canonicalization phase.
  ///
  /// @param source where the DIEs of the canonicalized types are
  /// from.
  ///
  /// @param canonicalized the number of types that got canonicalized
  /// is added to the value already present in this parameter.
  ///
  /// @param missed the number of types scheduled for late
  /// canonicalization and which couldn't be canonicalized (for a
  /// reason) is added to the value already present in this parameter.
  void
  add_late_canonicalized_types_stats(die_source	source,
				     size_t&		canonicalized,
				     size_t&		missed) const
  {
    for (vector<Dwarf_Off>::const_iterator i =
	   types_to_canonicalize(source).begin();
	 i != types_to_canonicalize(source).end();
	 ++i)
      {
        type_base_sptr t = lookup_type_from_die_offset(*i, source);
	if (t->get_canonical_type())
	  ++canonicalized;
	else
	  ++missed;
      }
  }

  /// Compute the number of canonicalized and missed types in the late
  /// canonicalization phase.
  ///
  /// @param canonicalized the number of types that got canonicalized
  /// is added to the value already present in this parameter.
  ///
  /// @param missed the number of types scheduled for late
  /// canonicalization and which couldn't be canonicalized (for a
  /// reason) is added to the value already present in this parameter.
  void
  add_late_canonicalized_types_stats(size_t& canonicalized,
				     size_t& missed) const
  {
    for (die_source source = PRIMARY_DEBUG_INFO_DIE_SOURCE;
	 source < NUMBER_OF_DIE_SOURCES;
	 ++source)
      add_late_canonicalized_types_stats(source, canonicalized, missed);
  }

  // Look at the types that need to be canonicalized after the
  // translation unit has been constructed and canonicalize them.
  void
  perform_late_type_canonicalizing()
  {
    for (die_source source = PRIMARY_DEBUG_INFO_DIE_SOURCE;
	 source < NUMBER_OF_DIE_SOURCES;
	 ++source)
      canonicalize_types_scheduled(source);

    if (show_stats())
      {
	size_t num_canonicalized = 0, num_missed = 0, total = 0;
	add_late_canonicalized_types_stats(num_canonicalized,
					   num_missed);
	total = num_canonicalized + num_missed;
	cerr << "binary: "
	     << elf_path()
	     << "\n";
	cerr << "    # late canonicalized types: "
	     << num_canonicalized
	     << " (" << num_canonicalized * 100 / total << "%)\n"
	     << "    # missed canonicalization opportunities: "
	     << num_missed
	     << " (" << num_missed * 100 / total << "%)\n";
      }

  }

  const die_tu_map_type&
  die_tu_map() const
  {return die_tu_map_;}

  die_tu_map_type&
  die_tu_map()
  {return die_tu_map_;}

  /// Getter for the map that associates a translation unit DIE to the
  /// vector of imported unit points that it contains.
  ///
  /// @param source where the DIEs are from.
  ///
  /// @return the map.
  const tu_die_imported_unit_points_map_type&
  tu_die_imported_unit_points_map(die_source source) const
  {return const_cast<read_context*>(this)->tu_die_imported_unit_points_map(source);}

  /// Getter for the map that associates a translation unit DIE to the
  /// vector of imported unit points that it contains.
  ///
  /// @param source where the DIEs are from.
  ///
  /// @return the map.
  tu_die_imported_unit_points_map_type&
  tu_die_imported_unit_points_map(die_source source)
  {
    switch(source)
      {
      case PRIMARY_DEBUG_INFO_DIE_SOURCE:
	break;
      case ALT_DEBUG_INFO_DIE_SOURCE:
	return alt_tu_die_imported_unit_points_map_;
      case TYPE_UNIT_DIE_SOURCE:
	return type_units_tu_die_imported_unit_points_map_;
      case NO_DEBUG_INFO_DIE_SOURCE:
      case NUMBER_OF_DIE_SOURCES:
	// We cannot reach this point.
	ABG_ASSERT_NOT_REACHED;
      }
    return tu_die_imported_unit_points_map_;
  }

  const corpus_sptr
  current_corpus() const
  {return cur_corpus_;}

  corpus_sptr
  current_corpus()
  {return cur_corpus_;}

  void
  current_corpus(corpus_sptr c)
  {
    if (c)
      cur_corpus_ = c;
  }

  void
  reset_current_corpus()
  {cur_corpus_.reset();}

  /// Get the map that associates each DIE to its parent DIE.  This is
  /// for DIEs coming from the main debug info sections.
  ///
  /// @param source where the DIEs in the map come from.
  ///
  /// @return the DIE -> parent map.
  const offset_offset_map&
  die_parent_map(die_source source) const
  {return const_cast<read_context*>(this)->die_parent_map(source);}

  /// Get the map that associates each DIE to its parent DIE.  This is
  /// for DIEs coming from the main debug info sections.
  ///
  /// @param source where the DIEs in the map come from.
  ///
  /// @return the DIE -> parent map.
  offset_offset_map&
  die_parent_map(die_source source)
  {
    switch(source)
      {
      case PRIMARY_DEBUG_INFO_DIE_SOURCE:
	break;
      case ALT_DEBUG_INFO_DIE_SOURCE:
	return alternate_die_parent_map_;
      case TYPE_UNIT_DIE_SOURCE:
	return type_section_die_parent_map();
      case NO_DEBUG_INFO_DIE_SOURCE:
      case NUMBER_OF_DIE_SOURCES:
	ABG_ASSERT_NOT_REACHED;
      }
    return primary_die_parent_map_;
  }

  const offset_offset_map&
  type_section_die_parent_map() const
  {return type_section_die_parent_map_;}

  offset_offset_map&
  type_section_die_parent_map()
  {return type_section_die_parent_map_;}

  /// Getter of the current translation unit.
  ///
  /// @return the current translation unit being constructed.
  const translation_unit_sptr
  cur_transl_unit() const
  {return cur_tu_;}

  /// Getter of the current translation unit.
  ///
  /// @return the current translation unit being constructed.
  translation_unit_sptr&
  cur_transl_unit()
  {return cur_tu_;}

  /// Setter of the current translation unit.
  ///
  /// @param tu the current translation unit being constructed.
  void
  cur_transl_unit(translation_unit_sptr tu)
  {
    if (tu)
      cur_tu_ = tu;
  }

  const scope_stack_type&
  scope_stack() const
  {return scope_stack_;}

  scope_stack_type&
  scope_stack()
  {return scope_stack_;}

  scope_decl*
  current_scope()
  {
    if (scope_stack().empty())
      {
	if (cur_transl_unit())
	  scope_stack().push(cur_transl_unit()->get_global_scope().get());
      }
    return scope_stack().top();
  }

  list<var_decl_sptr>&
  var_decls_to_re_add_to_tree()
  {return var_decls_to_add_;}

  /// Return the type of the current elf file.
  ///
  /// @return the type of the current elf file.
  elf_type
  get_elf_file_type()
  {
    return elf_file_type(elf_handle());
  }

  /// The section containing the symbol table from the current ELF
  /// file.
  ///
  /// Note that after it's first invocation, this function caches the
  /// symbol table that it found.  Subsequent invocations just return
  /// the cached symbol table section.
  ///
  /// @return the symbol table section if found
  Elf_Scn*
  find_symbol_table_section()
  {
    if (!symtab_section_)
      dwarf_reader::find_symbol_table_section(elf_handle(), symtab_section_);
    return symtab_section_;
  }

  /// Return the "Official Procedure descriptors section."  This
  /// section is named .opd, and is usually present only on PPC64
  /// ELFv1 binaries.
  ///
  /// @return the .opd section, if found.  Return nil otherwise.
  Elf_Scn*
  find_opd_section()
  {
    if (!opd_section_)
      opd_section_= find_section(elf_handle(), ".opd", SHT_PROGBITS);
    return opd_section_;
  }

  /// Return the SHT_GNU_versym, SHT_GNU_verdef and SHT_GNU_verneed
  /// sections that are involved in symbol versionning.
  ///
  /// @param versym_section the SHT_GNU_versym section found.
  ///
  /// @param verdef_section the SHT_GNU_verdef section found.
  ///
  /// @param verneed_section the SHT_GNU_verneed section found.
  ///
  /// @return true iff the sections where found.
  bool
  get_symbol_versionning_sections(Elf_Scn*&	versym_section,
				  Elf_Scn*&	verdef_section,
				  Elf_Scn*&	verneed_section)
  {
    if (!symbol_versionning_sections_loaded_)
      {
	symbol_versionning_sections_found_ =
	  dwarf_reader::get_symbol_versionning_sections(elf_handle(),
							versym_section_,
							verdef_section_,
							verneed_section_);
	symbol_versionning_sections_loaded_ = true;
      }

    versym_section = versym_section_;
    verdef_section = verdef_section_;
    verneed_section = verneed_section_;
    return symbol_versionning_sections_found_;
  }

  /// Return the version for a symbol that is at a given index in its
  /// SHT_SYMTAB section.
  ///
  /// The first invocation of this function caches the results and
  /// subsequent invocations just return the cached results.
  ///
  /// @param symbol_index the index of the symbol to consider.
  ///
  /// @param get_def_version if this is true, it means that that we want
  /// the version for a defined symbol; in that case, the version is
  /// looked for in a section of type SHT_GNU_verdef.  Otherwise, if
  /// this parameter is false, this means that we want the version for
  /// an undefined symbol; in that case, the version is the needed one
  /// for the symbol to be resolved; so the version is looked fo in a
  /// section of type SHT_GNU_verneed.
  ///
  /// @param version the version found for symbol at @p symbol_index.
  ///
  /// @return true iff a version was found for symbol at index @p
  /// symbol_index.
  bool
  get_version_for_symbol(size_t		symbol_index,
			 bool			get_def_version,
			 elf_symbol::version&	version)
  {
    Elf_Scn *versym_section = NULL,
      *verdef_section = NULL,
      *verneed_section = NULL;

    if (!get_symbol_versionning_sections(versym_section,
					 verdef_section,
					 verneed_section))
      return false;

    GElf_Versym versym_mem;
    Elf_Data* versym_data = (versym_section)
      ? elf_getdata(versym_section, NULL)
      : NULL;
    GElf_Versym* versym = (versym_data)
      ? gelf_getversym(versym_data, symbol_index, &versym_mem)
      : NULL;

    if (versym == 0 || *versym <= 1)
      // I got these value from the code of readelf.c in elfutils.
      // Apparently, if the symbol version entry has these values, the
      // symbol must be discarded. This is not documented in the
      // official specification.
      return false;

    if (get_def_version)
      {
	if (*versym == 0x8001)
	  // I got this value from the code of readelf.c in elfutils
	  // too.  It's not really documented in the official
	  // specification.
	  return false;

	if (verdef_section
	    && get_version_definition_for_versym(elf_handle(), versym,
						 verdef_section, version))
	  return true;
      }
    else
      {
	if (verneed_section
	    && get_version_needed_for_versym(elf_handle(), versym,
					     verneed_section, version))
	  return true;
      }

    return false;
  }

  /// Look into the symbol tables of the underlying elf file and see
  /// if we find a given symbol.
  ///
  /// @param symbol_name the name of the symbol to look for.
  ///
  /// @param demangle if true, demangle the symbols found in the symbol
  /// tables.
  ///
  /// @param syms the vector of symbols with the name @p symbol_name
  /// that were found.
  ///
  /// @return true iff the symbol was found.
  bool
  lookup_symbol_from_elf(const string&			symbol_name,
			 bool				demangle,
			 vector<elf_symbol_sptr>&	syms) const
  {
    return dwarf_reader::lookup_symbol_from_elf(env(),
						elf_handle(),
						symbol_name,
						demangle,
						syms);
  }

  /// Given the index of a symbol into the symbol table of an ELF
  /// file, look the symbol up, build an instace of @ref elf_symbol
  /// and return it.
  ///
  /// @param symbol_index the index of the symbol into the symbol
  /// table of the current elf file.
  ///
  /// @param symbol the resulting instance of @ref elf_symbol, iff the
  /// function returns true.
  ///
  /// @return the elf symbol found or nil if none was found.
  elf_symbol_sptr
  lookup_elf_symbol_from_index(size_t symbol_index)
  {
    Elf_Scn* symtab_section = find_symbol_table_section();
    if (!symtab_section)
      return elf_symbol_sptr();

    GElf_Shdr header_mem;
    GElf_Shdr* symtab_sheader = gelf_getshdr(symtab_section,
					     &header_mem);

    Elf_Data* symtab = elf_getdata(symtab_section, 0);
    assert(symtab);

    GElf_Sym* s, smem;
    s = gelf_getsym(symtab, symbol_index, &smem);
    if (!s)
      return elf_symbol_sptr();

    bool sym_is_defined = s->st_shndx != SHN_UNDEF;
    bool sym_is_common = s->st_shndx == SHN_COMMON; // this occurs in
						    // relocatable
						    // files.
    const char* name_str = elf_strptr(elf_handle(),
				      symtab_sheader->sh_link,
				      s->st_name);
    if (name_str == 0)
      name_str = "";

    elf_symbol::version v;
    get_version_for_symbol(symbol_index,
			   sym_is_defined,
			   v);

    elf_symbol_sptr sym =
      elf_symbol::create(env(), symbol_index, s->st_size, name_str,
			 stt_to_elf_symbol_type(GELF_ST_TYPE(s->st_info)),
			 stb_to_elf_symbol_binding(GELF_ST_BIND(s->st_info)),
			 sym_is_defined, sym_is_common, v);
    return sym;
  }

  /// Lookup the address of the function entry point that corresponds
  /// to the address of a given function descriptor.
  ///
  /// On PPC64, a function pointer is the address of a function
  /// descriptor.  Function descriptors are located in the .opd
  /// section.  Each function descriptor is a triplet of three
  /// addresses, each one on 64 bits.  Among those three address only
  /// the first one is of any interest to us: the address of the entry
  /// point of the function.
  ///
  /// This function returns the address of the entry point of the
  /// function whose descriptor's address is given.
  ///
  /// http://refspecs.linuxfoundation.org/ELF/ppc64/PPC-elf64abi.html#FUNC-DES
  ///
  /// https://www.ibm.com/developerworks/community/blogs/5894415f-be62-4bc0-81c5-3956e82276f3/entry/deeply_understand_64_bit_powerpc_elf_abi_function_descriptors?lang=en
  ///
  /// @param fn_desc_address the address of the function descriptor to
  /// consider.
  ///
  /// @return the address of the entry point of the function whose
  /// descriptor has the address @p fn_desc_address.  If there is no
  /// .opd section (e.g because we are not on ppc64) or more generally
  /// if the function descriptor could not be found then this function
  /// just returns the address of the fuction descriptor.
  GElf_Addr
  lookup_ppc64_elf_fn_entry_pointer_address(GElf_Addr fn_desc_address)
  {
    if (!elf_handle())
      return fn_desc_address;

    if (!elf_architecture_is_ppc64())
      return fn_desc_address;

    bool is_big_endian = elf_architecture_is_big_endian();

    Elf_Scn *opd_section = find_opd_section();
    if (!opd_section)
      return fn_desc_address;

    GElf_Shdr header_mem;
    // The section header of the .opd section.
    GElf_Shdr *opd_sheader = gelf_getshdr(opd_section, &header_mem);

    // The offset of the function descriptor entry, in the .opd
    // section.
    size_t fn_desc_offset = fn_desc_address - opd_sheader->sh_addr;
    Elf_Data *elf_data = elf_rawdata(opd_section, NULL);
    if (elf_data->d_size <= fn_desc_offset)
      return fn_desc_address;

    // A pointer to the data of the .opd section, that we can actually
    // do something with.
    uint8_t * bytes = (uint8_t*) elf_data->d_buf;

    // The resulting address we are looking for is going to be formed
    // in this variable.
    GElf_Addr result = 0;

    if (is_big_endian)
      {
	// In Big Endian, the most significant byte is at the lowest
	// address.
	uint8_t* msb = bytes + fn_desc_offset;
	result = *msb;
	// Now read the remaining 7 least significant bytes.
	for (uint i = 1; i < 8; ++i)
	  result = (result << 8) + msb[i];
      }
    else
      {
	// In Little Endian, the least significant byte is at the
	// lowest address.
	uint8_t* lsb = bytes + fn_desc_offset;
	result = *lsb;
	// Now read the remaining 7 most significant bytes.
	for (uint i = 1; i < 8; ++i)
	  result = result + (lsb[i] << i * 8);
      }

    return result;
  }

  /// Given the address of the beginning of a function, lookup the
  /// symbol of the function, build an instance of @ref elf_symbol out
  /// of it and return it.
  ///
  /// @param symbol_start_addr the address of the beginning of the
  /// function to consider.
  ///
  /// @param sym the resulting symbol.  This is set iff the function
  /// returns true.
  ///
  /// @return the elf symbol found at address @p symbol_start_addr, or
  /// nil if none was found.
  elf_symbol_sptr
  lookup_elf_fn_symbol_from_address(GElf_Addr symbol_start_addr)
  {
    addr_elf_symbol_sptr_map_type::const_iterator i,
      nil = fun_entry_addr_sym_map().end();

    if ((i = fun_entry_addr_sym_map().find(symbol_start_addr)) == nil)
      return elf_symbol_sptr();

    return i->second;
  }

  /// Given the address of a global variable, lookup the symbol of the
  /// variable, build an instance of @ref elf_symbol out of it and
  /// return it.
  ///
  /// @param symbol_start_addr the address of the beginning of the
  /// variable to consider.
  ///
  /// @param the symbol found, iff the function returns true.
  ///
  /// @return the elf symbol found or nil if none was found.
  elf_symbol_sptr
  lookup_elf_var_symbol_from_address(GElf_Addr symbol_start_addr)
  {
    addr_elf_symbol_sptr_map_type::const_iterator i,
      nil = var_addr_sym_map().end();

    if ((i = var_addr_sym_map().find(symbol_start_addr)) == nil)
      return elf_symbol_sptr();

    return i->second;
  }

  /// Look in the symbol tables of the underying elf file and see if
  /// we find a symbol of a given name of function type.
  ///
  /// @param sym_name the name of the symbol to look for.
  ///
  /// @param syms the public function symbols that were found, with
  /// the name @p sym_name.
  ///
  /// @return true iff the symbol was found.
  bool
  lookup_public_function_symbol_from_elf(const string&			sym_name,
					 vector<elf_symbol_sptr>&	syms)
  {
    return dwarf_reader::lookup_public_function_symbol_from_elf(env(),
								elf_handle(),
								sym_name,
								syms);
  }

  /// Look in the symbol tables of the underying elf file and see if
  /// we find a symbol of a given name of variable type.
  ///
  /// @param sym_name the name of the symbol to look for.
  ///
  /// @param syms the variable symbols that were found, with the name
  /// @p sym_name.
  ///
  /// @return true iff the symbol was found.
  bool
  lookup_public_variable_symbol_from_elf(const string&			sym_name,
					 vector<elf_symbol_sptr>&	syms)
  {
    return dwarf_reader::lookup_public_variable_symbol_from_elf(env(),
								elf_handle(),
								sym_name,
								syms);
  }

  /// Getter for the map of function address -> symbol.
  ///
  /// @return the function address -> symbol map.
  const addr_elf_symbol_sptr_map_sptr
  fun_addr_sym_map_sptr() const
  {
    maybe_load_symbol_maps();
    return fun_addr_sym_map_;
  }

  /// Getter for the map of function address -> symbol.
  ///
  /// @return the function address -> symbol map.
  addr_elf_symbol_sptr_map_sptr
  fun_addr_sym_map_sptr()
  {
    maybe_load_symbol_maps();
    return fun_addr_sym_map_;
  }

  /// Getter for the map of function symbol address -> function symbol
  /// index.
  ///
  /// @return the map.  Note that this initializes the map once when
  /// its nedded.
  const addr_elf_symbol_sptr_map_type&
  fun_addr_sym_map() const
  {
    maybe_load_symbol_maps();
    return *fun_addr_sym_map_;
  }

  /// Getter for the map of function symbol address -> function symbol
  /// index.
  ///
  /// @return the map.  Note that this initializes the map once when
  /// its nedded.
  addr_elf_symbol_sptr_map_type&
  fun_addr_sym_map()
  {
    maybe_load_symbol_maps();
    return *fun_addr_sym_map_;
  }

  /// Getter for a pointer to the map that associates the address of
  /// an entry point of a function with the symbol of that function.
  ///
  /// Note that on non-"PPC64 ELFv1" binaries, this map is the same as
  /// the one that assciates the address of a function with the symbol
  /// of that function.
  ///
  /// @return a pointer to the map that associates the address of an
  /// entry point of a function with the symbol of that function.
  addr_elf_symbol_sptr_map_sptr&
  fun_entry_addr_sym_map_sptr()
  {
    maybe_load_symbol_maps();
    if (elf_architecture_is_ppc64())
      return fun_entry_addr_sym_map_;
    return fun_addr_sym_map_;
  }

  /// Getter for a pointer to the map that associates the address of
  /// an entry point of a function with the symbol of that function.
  ///
  /// Note that on non-"PPC64 ELFv1" binaries, this map is the same as
  /// the one that assciates the address of a function with the symbol
  /// of that function.
  ///
  /// @return a pointer to the map that associates the address of an
  /// entry point of a function with the symbol of that function.
  const addr_elf_symbol_sptr_map_sptr&
  fun_entry_addr_sym_map_sptr() const
  {return const_cast<read_context*>(this)->fun_entry_addr_sym_map_sptr();}


  /// Getter for the map that associates the address of an entry point
  /// of a function with the symbol of that function.
  ///
  /// Note that on non-"PPC64 ELFv1" binaries, this map is the same as
  /// the one that assciates the address of a function with the symbol
  /// of that function.
  ///
  /// @return the map that associates the address of an entry point of
  /// a function with the symbol of that function.
  addr_elf_symbol_sptr_map_type&
  fun_entry_addr_sym_map()
  {return *fun_entry_addr_sym_map_sptr();}

  /// Getter for the map that associates the address of an entry point
  /// of a function with the symbol of that function.
  ///
  /// Note that on non-"PPC64 ELFv1" binaries, this map is the same as
  /// the one that assciates the address of a function with the symbol
  /// of that function.
  ///
  /// @return the map that associates the address of an entry point of
  /// a function with the symbol of that function.
  const addr_elf_symbol_sptr_map_type&
  fun_entry_addr_sym_map() const
  { return *fun_entry_addr_sym_map_sptr();}

  /// Getter for the map of function symbols (name -> sym).
  ///
  /// @return a shared pointer to the map of function symbols.
  const string_elf_symbols_map_sptr&
  fun_syms_sptr() const
  {
    maybe_load_symbol_maps();
    return fun_syms_;
  }

  /// Getter for the map of function symbols (name -> sym).
  ///
  /// @return a shared pointer to the map of function symbols.
  string_elf_symbols_map_sptr&
  fun_syms_sptr()
  {
    maybe_load_symbol_maps();
    return fun_syms_;
  }

  /// Getter for the map of function symbols (name -> sym).
  ///
  /// @return a reference to the map of function symbols.
  const string_elf_symbols_map_type&
  fun_syms() const
  {
    maybe_load_symbol_maps();
    return *fun_syms_;
  }

  /// Getter for the map of function symbols (name -> sym).
  ///
  /// @return a reference to the map of function symbols.
  string_elf_symbols_map_type&
  fun_syms()
  {
    maybe_load_symbol_maps();
    return *fun_syms_;
  }

  /// Getter for the map of variable symbols (name -> sym)
  ///
  /// @return a shared pointer to the map of variable symbols.
  const string_elf_symbols_map_sptr
  var_syms_sptr() const
  {
    maybe_load_symbol_maps();
    return var_syms_;
  }

  /// Getter for the map of variable symbols (name -> sym)
  ///
  /// @return a shared pointer to the map of variable symbols.
  string_elf_symbols_map_sptr
  var_syms_sptr()
  {
    maybe_load_symbol_maps();
    return var_syms_;
  }

  /// Getter for the map of variable symbols (name -> sym)
  ///
  /// @return a reference to the map of variable symbols.
  const string_elf_symbols_map_type&
  var_syms() const
  {
    maybe_load_symbol_maps();
    return *var_syms_;
  }

  /// Getter for the map of variable symbols (name -> sym)
  ///
  /// @return a reference to the map of variable symbols.
  string_elf_symbols_map_type&
  var_syms()
  {
    maybe_load_symbol_maps();
    return *var_syms_;
  }

  /// Getter for the map of undefined function symbols (name -> vector
  /// of symbols).
  ///
  /// @return a (smart) pointer to the map of undefined function
  /// symbols.
  const string_elf_symbols_map_sptr&
  undefined_fun_syms_sptr() const
  {
    maybe_load_symbol_maps();
    return undefined_fun_syms_;
  }

  /// Getter for the map of undefined function symbols (name -> vector
  /// of symbols).
  ///
  /// @return a (smart) pointer to the map of undefined function
  /// symbols.
  string_elf_symbols_map_sptr&
  undefined_fun_syms_sptr()
  {
    maybe_load_symbol_maps();
    return undefined_fun_syms_;
  }

  /// Getter for the map of undefined function symbols (name -> vector
  /// of symbols).
  ///
  /// @return a reference to the map of undefined function symbols.
  const string_elf_symbols_map_type&
  undefined_fun_syms() const
  {
    maybe_load_symbol_maps();
    return *undefined_fun_syms_;
  }

  /// Getter for the map of undefined function symbols (name -> vector
  /// of symbols).
  ///
  /// @return a reference to the map of undefined function symbols.
  string_elf_symbols_map_type&
  undefined_fun_syms()
  {
    maybe_load_symbol_maps();
    return *undefined_fun_syms_;
  }

  /// Getter for the map of undefined variable symbols (name -> vector
  /// of symbols).
  ///
  /// @return a (smart) pointer to the map of undefined variable
  /// symbols.
  const string_elf_symbols_map_sptr&
  undefined_var_syms_sptr() const
  {
    maybe_load_symbol_maps();
    return undefined_var_syms_;
  }

  /// Getter for the map of undefined variable symbols (name -> vector
  /// of symbols).
  ///
  /// @return a (smart) pointer to the map of undefined variable
  /// symbols.
  string_elf_symbols_map_sptr&
  undefined_var_syms_sptr()
  {
    maybe_load_symbol_maps();
    return undefined_var_syms_;
  }

  /// Getter for the map of undefined variable symbols (name -> vector
  /// of symbols).
  ///
  /// @return a reference to the map of undefined variable symbols.
  const string_elf_symbols_map_type&
  undefined_var_syms() const
  {
    maybe_load_symbol_maps();
    return *undefined_var_syms_;
  }

  /// Getter for the map of undefined variable symbols (name -> vector
  /// of symbols).
  ///
  /// @return a reference to the map of undefined variable symbols.
  string_elf_symbols_map_type&
  undefined_var_syms()
  {
    maybe_load_symbol_maps();
    return *undefined_var_syms_;
  }

  /// Getter for the ELF dt_needed tag.
  const vector<string>&
  dt_needed() const
  {return dt_needed_;}

  /// Getter for the ELF dt_soname tag.
  const string&
  dt_soname() const
  {return dt_soname_;}

  /// Getter for the ELF architecture of the current file.
  const string&
  elf_architecture() const
  {return elf_architecture_;}

  /// Test if the architecture of the current binary is ppc64.
  ///
  /// @return true iff the architecture of the current binary is ppc64.
  bool
  elf_architecture_is_ppc64() const
  {
    GElf_Ehdr eh_mem;
    GElf_Ehdr* elf_header = gelf_getehdr(elf_handle(), &eh_mem);

    return (elf_header->e_machine == EM_PPC64);
  }

  /// Test if the endianness of the current binary is Big Endian.
  ///
  /// https://en.wikipedia.org/wiki/Endianness.
  ///
  /// @return true iff the current binary is Big Endian.
  bool
  elf_architecture_is_big_endian() const
  {
    GElf_Ehdr eh_mem;
    GElf_Ehdr* elf_header = gelf_getehdr(elf_handle(), &eh_mem);

    bool is_big_endian = (elf_header->e_ident[EI_DATA] == ELFDATA2MSB);
      return true;

    if (!is_big_endian)
      assert(elf_header->e_ident[EI_DATA] == ELFDATA2LSB);

    return false;
  }

  /// Test if the current elf file being read is an executable.
  ///
  /// @return true iff the current elf file being read is an
  /// executable.
  bool
  current_elf_file_is_executable() const
  {
    GElf_Ehdr eh_mem;
    GElf_Ehdr* elf_header = gelf_getehdr(elf_handle(), &eh_mem);
    return elf_header->e_type == ET_EXEC;
  }

  /// Test if the current elf file being read is a dynamic shared
  /// object.
  ///
  /// @return true iff the current elf file being read is a
  /// dynamic shared object.
  bool
  current_elf_file_is_dso() const
  {
    GElf_Ehdr eh_mem;
    GElf_Ehdr* elf_header = gelf_getehdr(elf_handle(), &eh_mem);
    return elf_header->e_type == ET_DYN;
  }

  /// Getter for the map of global variables symbol address -> global
  /// variable symbol index.
  ///
  /// @return the map.  Note that this initializes the map once when
  /// its nedded.
  const addr_elf_symbol_sptr_map_type&
  var_addr_sym_map() const
  {
    maybe_load_symbol_maps();
    return *var_addr_sym_map_;
  }

  /// Getter for the map of global variables symbol address -> global
  /// variable symbol index.
  ///
  /// @return the map.  Note that this initializes the map once when
  /// its nedded.
  addr_elf_symbol_sptr_map_type&
  var_addr_sym_map()
  {
    maybe_load_symbol_maps();
    return *var_addr_sym_map_;
  }

  /// Load the maps of function symbol address -> function symbol,
  /// global variable symbol address -> variable symbol and also the
  /// maps of function and variable undefined symbols.
  ///
  /// @return true iff everything went fine.
  bool
  load_symbol_maps()
  {
    bool load_fun_map = !fun_addr_sym_map_ || fun_addr_sym_map_->empty();
    bool load_var_map = !var_addr_sym_map_ || var_addr_sym_map_->empty();
    bool load_undefined_fun_map = (!undefined_fun_syms_
				    || undefined_fun_syms_->empty());
    bool load_undefined_var_map = (!undefined_var_syms_
				   || undefined_var_syms_->empty());

    if (!fun_syms_)
      fun_syms_.reset(new string_elf_symbols_map_type);

    if (!fun_addr_sym_map_)
      fun_addr_sym_map_.reset(new addr_elf_symbol_sptr_map_type);

    if (!fun_entry_addr_sym_map_ && elf_architecture_is_ppc64())
      fun_entry_addr_sym_map_.reset(new addr_elf_symbol_sptr_map_type);

    if (!var_syms_)
      var_syms_.reset(new string_elf_symbols_map_type);

    if (!var_addr_sym_map_)
      var_addr_sym_map_.reset(new addr_elf_symbol_sptr_map_type);

    if (!undefined_fun_syms_)
      undefined_fun_syms_.reset(new string_elf_symbols_map_type);

    if (!undefined_var_syms_)
      undefined_var_syms_.reset(new string_elf_symbols_map_type);

    Elf_Scn* symtab_section = find_symbol_table_section();
    if (!symtab_section)
      return false;

    GElf_Shdr header_mem;
    GElf_Shdr* symtab_sheader = gelf_getshdr(symtab_section,
					     &header_mem);
    size_t nb_syms = symtab_sheader->sh_size / symtab_sheader->sh_entsize;

    Elf_Data* symtab = elf_getdata(symtab_section, 0);
    assert(symtab);

    bool is_ppc64 = elf_architecture_is_ppc64();

    for (size_t i = 0; i < nb_syms; ++i)
      {
	GElf_Sym* sym, sym_mem;
	sym = gelf_getsym(symtab, i, &sym_mem);
	assert(sym);

	if ((load_fun_map || load_undefined_fun_map)
	    && (GELF_ST_TYPE(sym->st_info) == STT_FUNC
		|| GELF_ST_TYPE(sym->st_info) == STT_GNU_IFUNC))
	  {
	    elf_symbol_sptr symbol = lookup_elf_symbol_from_index(i);
	    assert(symbol);
	    assert(symbol->is_function());


	    if (load_fun_map && symbol->is_public())
	      {
		{
		  string_elf_symbols_map_type::iterator it =
		    fun_syms_->find(symbol->get_name());
		  if (it == fun_syms_->end())
		    {
		      (*fun_syms_)[symbol->get_name()] = elf_symbols();
		      it = fun_syms_->find(symbol->get_name());
		    }
		  string name = symbol->get_name();
		  it->second.push_back(symbol);
		}

		{
		  addr_elf_symbol_sptr_map_type::const_iterator it =
		    fun_addr_sym_map_->find(sym->st_value);
		  if (it == fun_addr_sym_map_->end())
		    (*fun_addr_sym_map_)[sym->st_value] = symbol;
		  else
		    it->second->get_main_symbol()->add_alias(symbol);

		  if (is_ppc64)
		    {
		      // For ppc64 ELFv1 binaries, we need to build a
		      // function entry point address -> function
		      // symbol map.  This is in addition to the
		      // function pointer -> symbol map.  This is
		      // because on ppc64 ELFv1, a function pointer is
		      // different from a function entry poinut
		      // address.
		      //
		      // On ppc64 ELFv1, the DWARF DIE of a function
		      // references the address of the entry point of
		      // the function symbol; whereas the value of the
		      // function symbol is the function pointer.  As
		      // these addresses are different, if I we want
		      // to get to the symbol of a function from its
		      // entry point address (as referenced by DWARF
		      // function DIEs) we must have the two maps I
		      // mentionned right above.
		      //
		      // In other words, we need a map that associates
		      // a function enty point address with the symbol
		      // of that function, to be able to get the
		      // function symbol that corresponds to a given
		      // function DIE, on ppc64.
		      //
		      // The value of the function pointer (the value
		      // of the symbol) usually refers to the offset
		      // of a table in the .opd section.  But
		      // sometimes, for a symbol named "foo", the
		      // corresponding symbol named ".foo" (note the
		      // dot before foo) which value is the entry
		      // point address of the function; that entry
		      // point address refers to a region in the .text
		      // section.
		      //
		      // So we are only interested in values of the
		      // symbol that are in the .opd section.
		      GElf_Addr fn_desc_addr = sym->st_value;
		      GElf_Addr fn_entry_point_addr =
			lookup_ppc64_elf_fn_entry_pointer_address(fn_desc_addr);
		      addr_elf_symbol_sptr_map_type::const_iterator it2 =
			fun_entry_addr_sym_map().find(fn_entry_point_addr);

		      if (it2 == fun_entry_addr_sym_map().end())
			fun_entry_addr_sym_map()[fn_entry_point_addr] = symbol;
		      else if (address_is_in_opd_section(fn_desc_addr))
			{
			  // Either
			  //
			  // 'symbol' must have been registered as an
			  // alias for it2->second->get_main_symbol(),
			  // right before the "if (ppc64)" statement.
			  //
			  // Or
			  //
			  // if the name of 'symbol' is foo, then the
			  // name of it2->second is ".foo".  That is,
			  // foo is the name of the symbol when it
			  // refers to the function descriptor in the
			  // .opd section and ".foo" is an internal
			  // name for the address of the entry point
			  // of foo.
			  //
			  // In the latter case, we just want to keep
			  // a refernce to "foo" as .foo is an
			  // internal name.

			  bool two_symbols_alias =
			    it2->second->get_main_symbol()->does_alias(*symbol);
			  bool symbol_is_foo_and_prev_symbol_is_dot_foo =
			    (it2->second->get_name()
			     == string(".") + symbol->get_name());

			  assert(two_symbols_alias
				 || symbol_is_foo_and_prev_symbol_is_dot_foo);

			  if (symbol_is_foo_and_prev_symbol_is_dot_foo)
			    // Let's just keep a reference of the
			    // symbol that the user sees in the source
			    // code (the one named foo).  The symbol
			    // which name is prefixed with a "dot" is
			    // an artificial one.
			    fun_entry_addr_sym_map()[fn_entry_point_addr] = symbol;
			}
		   }
		}
	      }
	    else if (load_undefined_fun_map && !symbol->is_defined())
	      {
		string_elf_symbols_map_type::iterator it =
		  undefined_fun_syms_->find(symbol->get_name());
		if (it == undefined_fun_syms_->end())
		  {
		    (*undefined_fun_syms_)[symbol->get_name()] = elf_symbols();
		    it = undefined_fun_syms_->find(symbol->get_name());
		  }
		it->second.push_back(symbol);
	      }
	  }
	else if ((load_var_map || load_undefined_var_map)
		 && (GELF_ST_TYPE(sym->st_info) == STT_OBJECT
		     || GELF_ST_TYPE(sym->st_info) == STT_TLS)
		 // If the symbol is for an OBJECT, the index of the
		 // section it refers to cannot be absolute.
		 // Otherwise that OBJECT is not a variable.
		 && (sym->st_shndx != SHN_ABS
		     || GELF_ST_TYPE(sym->st_info) != STT_OBJECT ))
	  {
	    elf_symbol_sptr symbol = lookup_elf_symbol_from_index(i);
	    assert(symbol);
	    assert(symbol->is_variable());

	    if (load_var_map && symbol->is_public())
	      {
		{
		  string_elf_symbols_map_type::iterator it =
		    var_syms_->find(symbol->get_name());
		  if (it == var_syms_->end())
		    {
		      (*var_syms_)[symbol->get_name()] = elf_symbols();
		      it = var_syms_->find(symbol->get_name());
		    }
		  string name = symbol->get_name();
		  it->second.push_back(symbol);
		}

		if (symbol->is_common_symbol())
		  {
		    string_elf_symbols_map_type::iterator it =
		      var_syms_->find(symbol->get_name());
		    assert(it != var_syms_->end());
		    const elf_symbols& common_sym_instances = it->second;
		    assert(!common_sym_instances.empty());
		    if (common_sym_instances.size() > 1)
		      {
			elf_symbol_sptr main_common_sym =
			  common_sym_instances[0];
			assert(main_common_sym->get_name()
			       == symbol->get_name());
			assert(main_common_sym->is_common_symbol());
			assert(symbol.get() != main_common_sym.get());
			main_common_sym->add_common_instance(symbol);
		      }
		  }
		else
		  {
		    addr_elf_symbol_sptr_map_type::const_iterator it =
		      var_addr_sym_map_->find(sym->st_value);
		    if (it == var_addr_sym_map_->end())
		      (*var_addr_sym_map_)[sym->st_value] = symbol;
		    else
		      it->second->get_main_symbol()->add_alias(symbol);
		  }
	      }
	    else if (load_undefined_var_map && !symbol->is_defined())
	      {
		string_elf_symbols_map_type::iterator it =
		  undefined_var_syms_->find(symbol->get_name());
		if (it == undefined_var_syms_->end())
		  {
		    (*undefined_var_syms_)[symbol->get_name()] = elf_symbols();
		    it = undefined_var_syms_->find(symbol->get_name());
		  }
		it->second.push_back(symbol);
	      }
	  }
      }

    return true;
  }

  /// Load the symbol maps if necessary.
  ///
  /// @return true iff the symbol maps has been loaded by this
  /// invocation.
  bool
  maybe_load_symbol_maps() const
  {
    if (!fun_addr_sym_map_ || fun_addr_sym_map_->empty()
	|| !var_addr_sym_map_ || var_addr_sym_map_->empty()
	|| !fun_syms_ || fun_syms_->empty()
	|| !var_syms_ || var_syms_->empty()
	|| !undefined_fun_syms_ || undefined_fun_syms_->empty()
	|| !undefined_var_syms_ || undefined_var_syms_->empty())
      return const_cast<read_context*>(this)->load_symbol_maps();
    return false;
  }

  /// Load the DT_NEEDED and DT_SONAME elf TAGS.
  ///
  void
  load_dt_soname_and_needed()
  {
    lookup_data_tag_from_dynamic_segment(elf_handle(), DT_NEEDED, dt_needed_);

    vector<string> dt_tag_data;
    lookup_data_tag_from_dynamic_segment(elf_handle(), DT_SONAME, dt_tag_data);
    if (!dt_tag_data.empty())
      dt_soname_ = dt_tag_data[0];
  }

  /// Read the string representing the architecture of the current ELF
  /// file.
  void
  load_elf_architecture()
  {
    if (!elf_handle())
      return;

    GElf_Ehdr eh_mem;
    GElf_Ehdr* elf_header = gelf_getehdr(elf_handle(), &eh_mem);

    elf_architecture_ = e_machine_to_string(elf_header->e_machine);
  }

  /// Load various ELF data.
  ///
  /// This function loads ELF data that are not symbol maps or debug
  /// info.  That is, things like various tags, elf architecture and
  /// so on.
  void
  load_elf_properties()
  {
    load_dt_soname_and_needed();
    load_elf_architecture();
  }

  /// This is a sub-routine of maybe_adjust_fn_sym_address and
  /// maybe_adjust_var_sym_address.
  ///
  /// Given an address that we got by looking at some debug
  /// information (e.g, a symbol's address referred to by a DWARF
  /// TAG), If the ELF file we are interested in is a shared library
  /// or an executable, then adjust the address to be coherent with
  /// where the executable (or shared library) is loaded.  That way,
  /// the address can be used to look for symbols in the executable or
  /// shared library.
  ///
  /// @return the adjusted address, or the same address as @p addr if
  /// it didn't need any adjustment.
  Dwarf_Addr
  maybe_adjust_address_for_exec_or_dyn(Dwarf_Addr addr) const
  {
    GElf_Ehdr eh_mem;
    GElf_Ehdr *elf_header = gelf_getehdr(elf_handle(), &eh_mem);

    if (elf_header->e_type == ET_DYN || elf_header->e_type == ET_EXEC)
      {
	Dwarf_Addr dwarf_elf_load_address = 0, elf_load_address = 0;
	assert(get_binary_load_address(dwarf_elf_handle(),
				       dwarf_elf_load_address));
	assert(get_binary_load_address(elf_handle(),
				       elf_load_address));
	if (dwarf_is_splitted()
	    && (dwarf_elf_load_address != elf_load_address))
	  // This means that in theory the DWARF an the executable are
	  // not loaded at the same address.  And addr is meaningful
	  // only in the context of the DWARF.
	  //
	  // So let's transform addr into an offset relative to where
	  // the DWARF is loaded, and let's add that relative offset
	  // to the load address of the executable.  That way, addr
	  // becomes meaningful in the context of the executable and
	  // can thus be used to compare against the address of
	  // symbols of the executable, for instance.
	  addr = addr - dwarf_elf_load_address + elf_load_address;
      }

    return addr;
  }

  /// For a relocatable (*.o) elf file, this function expects an
  /// absolute address, representing a function symbol.  It then
  /// extracts the address of the .text section from the symbol
  /// absolute address to get the relative address of the function
  /// from the beginning of the .text section.
  ///
  /// For executable or shared library, this function expects an
  /// address of a function symbol that was retrieved by looking at a
  /// DWARF "file".  The function thus adjusts the address to make it
  /// be meaningful in the context of the ELF file.
  ///
  /// In both cases, the address can then be compared against the
  /// st_value field of a function symbol from the ELF file.
  ///
  /// @param addr an adress for a function symbol that was retrieved
  /// from a DWARF file.
  ///
  /// @return the (possibly) adjusted address, or just @p addr if no
  /// adjustment took place.
  Dwarf_Addr
  maybe_adjust_fn_sym_address(Dwarf_Addr addr) const
  {
    Elf* elf = elf_handle();
    GElf_Ehdr eh_mem;
    GElf_Ehdr* elf_header = gelf_getehdr(elf, &eh_mem);

    if (elf_header->e_type == ET_REL)
      {
	Elf_Scn* text_section = find_text_section(elf);
	assert(text_section);

	GElf_Shdr sheader_mem;
	GElf_Shdr* text_sheader = gelf_getshdr(text_section, &sheader_mem);
	assert(text_sheader);
	addr = addr - text_sheader->sh_addr;
      }
    else
      addr = maybe_adjust_address_for_exec_or_dyn(addr);

    return addr;
  }

  /// Test if a given address is in a given section.
  ///
  /// @param addr the address to consider.
  ///
  /// @param section the section to consider.
  bool
  address_is_in_section(Dwarf_Addr addr, Elf_Scn* section) const
  {
    if (!section)
      return false;

    GElf_Shdr sheader_mem;
    GElf_Shdr* sheader = gelf_getshdr(section, &sheader_mem);

    if (sheader->sh_addr <= addr && addr <= sheader->sh_addr + sheader->sh_size)
      return true;

    return false;
  }

  /// Return true if an address is in the ".opd" section that is
  /// present on the ppc64 platform.
  ///
  /// @param addr the address to consider.
  ///
  /// @return true iff @p addr is designates a word that is in the
  /// ".opd" section.
  bool
  address_is_in_opd_section(Dwarf_Addr addr)
  {
    Elf_Scn * opd_section = find_opd_section();
    if (!opd_section)
      return false;
    if (address_is_in_section(addr, opd_section))
      return true;
    return false;
  }

  /// Get the section which a global variable address comes from.
  ///
  /// @param elf the elf handle to consider.
  ///
  /// @param var_addr the address for the variable.
  ///
  /// @return the ELF section the @p var_addr comes from, or nil if no
  /// section was found for that variable address.
  Elf_Scn*
  get_data_section_for_variable_address(Elf* elf, Dwarf_Addr var_addr) const
  {
    // There are several potential 'data sections" from which an
    // variable address can come from: .data, .data1 and .rodata.
    // Let's try to try them all in sequence.

    Elf_Scn* data_section = find_bss_section(elf);
    if (!address_is_in_section(var_addr, data_section))
      {
	data_section = find_data_section(elf);
	if (!address_is_in_section(var_addr, data_section))
	  {
	    data_section = find_data1_section(elf);
	    if (!address_is_in_section(var_addr, data_section))
	      {
		data_section = find_rodata_section(elf);
		if (!address_is_in_section(var_addr, data_section))
		  return 0;
	      }
	  }
      }
    return data_section;
  }

  /// For a relocatable (*.o) elf file, this function expects an
  /// absolute address, representing a global variable symbol.  It
  /// then extracts the address of the {.data,.data1,.rodata,.bss}
  /// section from the symbol absolute address to get the relative
  /// address of the variable from the beginning of the data section.
  ///
  /// For executable or shared library, this function expects an
  /// address of a variable symbol that was retrieved by looking at a
  /// DWARF "file".  The function thus adjusts the address to make it
  /// be meaningful in the context of the ELF file.
  ///
  /// In both cases, the address can then be compared against the
  /// st_value field of a function symbol from the ELF file.
  ///
  /// @param addr an address for a global variable symbol that was
  /// retrieved from a DWARF file.
  ///
  /// @return the (possibly) adjusted address, or just @p addr if no
  /// adjustment took place.
  Dwarf_Addr
  maybe_adjust_var_sym_address(Dwarf_Addr addr) const
  {
    Elf* elf = elf_handle();
    GElf_Ehdr eh_mem;
    GElf_Ehdr* elf_header = gelf_getehdr(elf, &eh_mem);

    if (elf_header->e_type == ET_REL)
      {
	Elf_Scn* data_section =
	  get_data_section_for_variable_address(elf, addr);
	if (!data_section)
	  // It's likely that this address doesn't come from any
	  // data section.
	  return addr;

	GElf_Shdr sheader_mem;
	GElf_Shdr* data_sheader = gelf_getshdr(data_section, &sheader_mem);
	assert(data_sheader);

	return addr - data_sheader->sh_addr;
      }
    else
      addr = maybe_adjust_address_for_exec_or_dyn(addr);

    return addr;
  }


  /// Get the address of the function.
  ///
  /// The address of the function is considered to be the value of the
  /// DW_AT_low_pc attribute, possibly adjusted (in relocatable files
  /// only) to not point to an absolute address anymore, but rather to
  /// the address of the function inside the .text segment.
  ///
  /// @param function_die the die of the function to consider.
  ///
  /// @param address the resulting address iff the function returns
  /// true.
  ///
  /// @return true if the function address was found.
  bool
  get_function_address(Dwarf_Die* function_die,
		       Dwarf_Addr& address)
  {
    Dwarf_Addr low_pc = 0;
    if (!die_address_attribute(function_die, DW_AT_low_pc, low_pc))
      return false;

    low_pc = maybe_adjust_fn_sym_address(low_pc);
    address = low_pc;
    return true;
  }

  /// Get the address of the global variable.
  ///
  /// The address of the global variable is considered to be the value
  /// of the DW_AT_location attribute, possibly adjusted (in
  /// relocatable files only) to not point to an absolute address
  /// anymore, but rather to the address of the global variable inside
  /// the data segment.
  ///
  /// @param variable_die the die of the function to consider.
  ///
  /// @param address the resulting address iff this function returns
  /// true.
  ///
  /// @return true if the variable address was found.
  bool
  get_variable_address(Dwarf_Die*	variable_die,
		       Dwarf_Addr&	address)
  {
    bool is_tls_address = false;
    if (!die_location_address(variable_die, address, is_tls_address))
      return false;
    if (!is_tls_address)
      address = maybe_adjust_var_sym_address(address);
    return true;
  }

  /// Tests if a suppression specification can match ABI artifacts
  /// coming from the binary being analyzed.
  ///
  /// This tests if the suppression matches the soname of and binary
  /// name of the ELF binary being analyzed.
  ///
  /// @param s the suppression specification to consider.
  bool
  suppression_can_match(const suppr::suppression_base& s) const
  {
    if (s.priv_->matches_soname(dt_soname())
	&& s.priv_->matches_binary_name(elf_path()))
      return true;
    return false;
  }

  /// Test whether if a given function suppression matches a function
  /// designated by a regular expression that describes its linkage
  /// name (symbol name).
  ///
  /// @param s the suppression specification to evaluate to see if it
  /// matches a given function linkage name
  ///
  /// @param fn_linkage_name the linkage name of the function of interest.
  ///
  /// @return true iff the suppression specification @p s matches the
  /// function whose linkage name is @p fn_linkage_name.
  bool
  suppression_matches_function_sym_name(const suppr::function_suppression_sptr& s,
					const string& fn_linkage_name) const
  {
    if (!s)
      return false;
    return suppression_matches_function_sym_name(*s,fn_linkage_name);
  }

  /// Test whether if a given function suppression matches a function
  /// designated by a regular expression that describes its linkage
  /// name (symbol name).
  ///
  /// @param s the suppression specification to evaluate to see if it
  /// matches a given function linkage name
  ///
  /// @param fn_linkage_name the linkage name of the function of interest.
  ///
  /// @return true iff the suppression specification @p s matches the
  /// function whose linkage name is @p fn_linkage_name.
  bool
  suppression_matches_function_sym_name(const suppr::function_suppression& s,
					const string& fn_linkage_name) const
  {
    if (!suppression_can_match(s))
      return false;

    return suppr::suppression_matches_function_sym_name(s, fn_linkage_name);
  }

  /// Test whether if a given function suppression matches a function
  /// designated by a regular expression that describes its name.
  ///
  /// @param s the suppression specification to evaluate to see if it
  /// matches a given function name.
  ///
  /// @param fn_name the name of the function of interest.  Note that
  /// this name must be *non* qualified.
  ///
  /// @return true iff the suppression specification @p s matches the
  /// function whose name is @p fn_name.
  bool
  suppression_matches_function_name(const suppr::function_suppression_sptr& s,
				    const string& fn_name) const
  {
    if (!s)
      return false;
    return suppression_matches_function_name(*s, fn_name);
  }

  /// Test whether if a given function suppression matches a function
  /// designated by a regular expression that describes its name.
  ///
  /// @param s the suppression specification to evaluate to see if it
  /// matches a given function name.
  ///
  /// @param fn_name the name of the function of interest.  Note that
  /// this name must be *non* qualified.
  ///
  /// @return true iff the suppression specification @p s matches the
  /// function whose name is @p fn_name.
  bool
  suppression_matches_function_name(const suppr::function_suppression& s,
				    const string& fn_name) const
  {
    if (!suppression_can_match(s))
      return false;

    return suppr::suppression_matches_function_name(s, fn_name);
  }

  /// Test whether if a given variable suppression specification
  /// matches a variable denoted by its name.
  ///
  /// @param s the variable suppression specification to consider.
  ///
  /// @param var_name the name of the variable to consider.
  ///
  /// @return true iff the suppression specification @p s matches the
  /// variable whose name is @p var_name.
  bool
  suppression_matches_variable_name(const suppr::variable_suppression& s,
				    const string& var_name) const
  {
    if (!suppression_can_match(s))
      return false;

    return suppr::suppression_matches_variable_name(s, var_name);
  }

  /// Test whether if a given variable suppression specification
  /// matches a variable denoted by its linkage name.
  ///
  /// @param s the variable suppression specification to consider.
  ///
  /// @param var_linkage_name the linkage name of the variable to consider.
  ///
  /// @return true iff variable suppression specification @p s matches
  /// the variable denoted by linkage name @p var_linkage_name.
  bool
  suppression_matches_variable_sym_name(const suppr::variable_suppression& s,
					const string& var_linkage_name) const
  {
    if (!suppression_can_match(s))
      return false;

    return suppr::suppression_matches_variable_sym_name(s, var_linkage_name);
  }

  /// Test if a given type suppression specification matches a type
  /// designated by its name and location.
  ///
  /// @param s the suppression specification to consider.
  ///
  /// @param type_name the fully qualified type name to consider.
  ///
  /// @param type_location the type location to consider.
  ///
  /// @return true iff the type suppression specification matches a
  /// type of a given name and location.
  bool
  suppression_matches_type_name_or_location(const suppr::type_suppression& s,
					    const string& type_name,
					    const location& type_location) const
  {
    if (!suppression_can_match(s))
      return false;

    return suppr::suppression_matches_type_name_or_location(s, type_name,
							    type_location);
  }

  /// Test if a type suppression specification matches the name of a
  /// type within a given scope.
  ///
  /// @param s the type suppression specification to consider.
  ///
  /// @param type_scope the type scope to consider.
  ///
  /// @param type the type to consider.
  ///
  /// @return true iff the type suppression specification matches a
  /// the name of type @p type.
  bool
  suppression_matches_type_name(const suppr::type_suppression&	s,
				const scope_decl*		type_scope,
				const type_base_sptr&		type) const
  {
    if (!suppression_can_match(s))
      return false;
    return suppr::suppression_matches_type_name(s, type_scope, type);
  }

  /// Getter of the exported decls builder object.
  ///
  /// @return the exported decls builder.
  corpus::exported_decls_builder*
  exported_decls_builder()
  {return exported_decls_builder_;}

  /// Setter of the exported decls builder object.
  ///
  /// Note that this @ref read_context is not responsible for the live
  /// time of the exported_decls_builder object.  The corpus is.
  ///
  /// @param b the new builder.
  void
  exported_decls_builder(corpus::exported_decls_builder* b)
  {exported_decls_builder_ = b;}

  /// Getter of the "load_all_types" flag.  This flag tells if all the
  /// types (including those not reachable by public declarations) are
  /// to be read and represented in the final ABI corpus.
  ///
  /// @return the load_all_types flag.
  bool
  load_all_types() const
  {return load_all_types_;}

  /// Setter of the "load_all_types" flag.  This flag tells if all the
  /// types (including those not reachable by public declarations) are
  /// to be read and represented in the final ABI corpus.
  ///
  /// @param f the new load_all_types flag.
  void
  load_all_types(bool f)
  {load_all_types_ = f;}

  /// Getter of the "show_stats" flag.
  ///
  /// This flag tells if we should emit statistics about various
  /// internal stuff.
  ///
  /// @return the value of the flag.
  bool
  show_stats() const
  {return show_stats_;}

  /// Setter of the "show_stats" flag.
  ///
  /// This flag tells if we should emit statistics about various
  /// internal stuff.
  ///
  /// @param f the value of the flag.
  void
  show_stats(bool f)
  {show_stats_ = f;}

  /// Getter of the "do_log" flag.
  ///
  /// This flag tells if we should log about various internal
  /// details.
  ///
  /// return the "do_log" flag.
  bool
  do_log() const
  {return do_log_;}

  /// Setter of the "do_log" flag.
  ///
  /// This flag tells if we should log about various internal details.
  ///
  /// @param f the new value of the flag.
  void
  do_log(bool f)
  {do_log_ = f;}

  /// If a given function decl is suitable for the set of exported
  /// functions of the current corpus, this function adds it to that
  /// set.
  ///
  /// @param fn the function to consider for inclusion into the set of
  /// exported functions of the current corpus.
  void
  maybe_add_fn_to_exported_decls(function_decl* fn)
  {
    if (fn)
      if (corpus::exported_decls_builder* b = exported_decls_builder())
	b->maybe_add_fn_to_exported_fns(fn);
  }

  /// If a given variable decl is suitable for the set of exported
  /// variables of the current corpus, this variable adds it to that
  /// set.
  ///
  /// @param fn the variable to consider for inclusion into the set of
  /// exported variables of the current corpus.
  void
  maybe_add_var_to_exported_decls(var_decl* var)
  {
    if (var)
      if (corpus::exported_decls_builder* b = exported_decls_builder())
	b->maybe_add_var_to_exported_vars(var);
  }

  /// Walk the DIEs under a given die and for each child, populate the
  /// die -> parent map to record the child -> parent relationship
  /// that
  /// exists between the child and the given die.
  ///
  /// The function also builds the vector of places where units are
  /// imported.
  ///
  /// This is done recursively as for each child DIE, this function
  /// walks its children as well.
  ///
  /// @param die the DIE whose children to walk recursively.
  ///
  /// @param source where the DIE @p die comes from.
  ///
  /// @param imported_units a vector containing all the offsets of the
  /// points where unit have been imported, under @p die.
  void
  build_die_parent_relations_under(Dwarf_Die*			die,
				   die_source			source,
				   imported_unit_points_type &	imported_units)
  {
    if (!die)
      return;

    offset_offset_map& parent_of = die_parent_map(source);

    Dwarf_Die child;
    if (dwarf_child(die, &child) != 0)
      return;

    do
      {
	parent_of[dwarf_dieoffset(&child)] = dwarf_dieoffset(die);
	if (dwarf_tag(&child) == DW_TAG_imported_unit)
	  {
	    Dwarf_Die imported_unit;
	    if (die_die_attribute(&child, DW_AT_import, imported_unit))
	      {
		die_source imported_unit_die_source = NO_DEBUG_INFO_DIE_SOURCE;
		assert(get_die_source(imported_unit, imported_unit_die_source));
		imported_units.push_back
		  (imported_unit_point(dwarf_dieoffset(&child),
				       imported_unit,
				       imported_unit_die_source));
	      }
	  }
	build_die_parent_relations_under(&child, source, imported_units);
      }
    while (dwarf_siblingof(&child, &child) == 0);

  }

  /// Walk all the DIEs accessible in the debug info (and in the
  /// alternate debug info as well) and build maps representing the
  /// relationship DIE -> parent.  That is, make it so that we can get
  /// the parent for a given DIE.
  void
  build_die_parent_maps()
  {
    // Build the DIE -> parent relation for DIEs coming from the
    // .debug_info section in the alternate debug info file.
    die_source source = ALT_DEBUG_INFO_DIE_SOURCE;
    uint8_t address_size = 0;
    size_t header_size = 0;
    for (Dwarf_Off offset = 0, next_offset = 0;
	 (dwarf_next_unit(alt_dwarf(), offset, &next_offset, &header_size,
			  NULL, NULL, &address_size, NULL, NULL, NULL) == 0);
	 offset = next_offset)
      {
	Dwarf_Off die_offset = offset + header_size;
	Dwarf_Die cu;
	if (!dwarf_offdie(alt_dwarf(), die_offset, &cu))
	  continue;
	cur_tu_die(&cu);

	imported_unit_points_type& imported_units =
	  tu_die_imported_unit_points_map(source)[die_offset] =
	  imported_unit_points_type();
	build_die_parent_relations_under(&cu, source, imported_units);
      }

    // Build the DIE -> parent relation for DIEs coming from the
    // .debug_info section of the main debug info file.
    source = PRIMARY_DEBUG_INFO_DIE_SOURCE;
    address_size = 0;
    header_size = 0;
    for (Dwarf_Off offset = 0, next_offset = 0;
	 (dwarf_next_unit(dwarf(), offset, &next_offset, &header_size,
			  NULL, NULL, &address_size, NULL, NULL, NULL) == 0);
	 offset = next_offset)
      {
	Dwarf_Off die_offset = offset + header_size;
	Dwarf_Die cu;
	if (!dwarf_offdie(dwarf(), die_offset, &cu))
	  continue;
	cur_tu_die(&cu);
	imported_unit_points_type& imported_units =
	  tu_die_imported_unit_points_map(source)[die_offset] =
	  imported_unit_points_type();
	build_die_parent_relations_under(&cu, source, imported_units);
      }

    // Build the DIE -> parent relation for DIEs coming from the
    // .debug_types section.
    source = TYPE_UNIT_DIE_SOURCE;
    address_size = 0;
    header_size = 0;
    uint64_t type_signature = 0;
    Dwarf_Off type_offset;
    for (Dwarf_Off offset = 0, next_offset = 0;
    (dwarf_next_unit(dwarf(), offset, &next_offset, &header_size,
      NULL, NULL, &address_size, NULL,
      &type_signature, &type_offset) == 0);
    offset = next_offset)
      {
	Dwarf_Off die_offset = offset + header_size;
	Dwarf_Die cu;

	if (!dwarf_offdie_types(dwarf(), die_offset, &cu))
	  continue;
	cur_tu_die(&cu);
	imported_unit_points_type& imported_units =
	  tu_die_imported_unit_points_map(source)[die_offset] =
	  imported_unit_points_type();
	build_die_parent_relations_under(&cu, source, imported_units);
      }
  }
};// end class read_context.

static type_or_decl_base_sptr
build_ir_node_from_die(read_context&	ctxt,
		       Dwarf_Die*	die,
		       scope_decl*	scope,
		       bool		called_from_public_decl,
		       size_t		where_offset);

static type_or_decl_base_sptr
build_ir_node_from_die(read_context&	ctxt,
		       Dwarf_Die*	die,
		       bool		called_from_public_decl,
		       size_t		where_offset);

static class_decl_sptr
build_class_type_and_add_to_ir(read_context&	ctxt,
			       Dwarf_Die*	die,
			       scope_decl*	scope,
			       bool		is_struct,
			       class_decl_sptr  klass,
			       bool		called_from_public_decl,
			       size_t		where_offset);

static union_decl_sptr
build_union_type_and_add_to_ir(read_context&	ctxt,
				Dwarf_Die*	die,
				scope_decl*	scope,
				union_decl_sptr  union_type,
				bool		called_from_public_decl,
				size_t		where_offset);

static decl_base_sptr
build_ir_node_for_void_type(read_context& ctxt);

static function_decl_sptr
build_function_decl(read_context&	ctxt,
		    Dwarf_Die*		die,
		    size_t		where_offset,
		    function_decl_sptr	fn);

static var_decl_sptr
build_var_decl(read_context&	ctxt,
	       Dwarf_Die	*die,
	       size_t		where_offset,
	       var_decl_sptr	result = var_decl_sptr());

static bool
variable_is_suppressed(const read_context& ctxt,
		       const scope_decl* scope,
		       Dwarf_Die *variable_die);

static void
finish_member_function_reading(Dwarf_Die*		die,
			       function_decl_sptr	f,
			       class_or_union_sptr	klass,
			       read_context&		ctxt);

/// Setter of the debug info root path for a dwarf reader context.
///
/// @param ctxt the dwarf reader context to consider.
///
/// @param path the new debug info root path.  This must be a pointer to a
/// character string which life time should be greater than the life
/// time of the read context.
void
set_debug_info_root_path(read_context& ctxt, char** path)
{ctxt.offline_callbacks()->debuginfo_path = path;}

/// Setter of the debug info root path for a dwarf reader context.
///
/// @param ctxt the dwarf reader context to consider.
///
/// @return a pointer to the the debug info root path.
///
/// time of the read context.
char**
get_debug_info_root_path(read_context& ctxt)
{return ctxt.offline_callbacks()->debuginfo_path;}

/// Getter of the "show_stats" flag.
///
/// This flag tells if we should emit statistics about various
/// internal stuff.
///
/// @param ctx the read context to consider for this flag.
///
/// @return the value of the flag.
bool
get_show_stats(read_context& ctxt)
{return ctxt.show_stats();}

/// Setter of the "show_stats" flag.
///
/// This flag tells if we should emit statistics about various
/// internal stuff.
///
/// @param ctxt the read context to consider for this flag.
///
/// @param f the value of the flag.
void
set_show_stats(read_context& ctxt, bool f)
{ctxt.show_stats(f);}

/// Setter of the "do_log" flag.
///
/// This flag tells if we should emit verbose logs for various
/// internal things related to DWARF reading.
///
/// @param ctxt the DWARF reading context to consider.
///
/// @param f the new value of the flag.
void
set_do_log(read_context& ctxt, bool f)
{ctxt.do_log(f);}

/// Get the value of an attribute that is supposed to be a string, or
/// an empty string if the attribute could not be found.
///
/// @param die the DIE to get the attribute value from.
///
/// @param attr_name the attribute name.  Must come from dwarf.h and
/// be an enumerator representing an attribute like, e.g, DW_AT_name.
///
/// @return the string representing the value of the attribute, or an
/// empty string if no string attribute could be found.
static string
die_string_attribute(Dwarf_Die* die, unsigned attr_name)
{
  if (!die)
    return "";

  Dwarf_Attribute attr;
  if (!dwarf_attr_integrate(die, attr_name, &attr))
    return "";

  const char* str = dwarf_formstring(&attr);
  return str ? str : "";
}

/// Get the value of an attribute that is supposed to be an unsigned
/// constant.
///
/// @param die the DIE to read the information from.
///
/// @param attr_name the DW_AT_* name of the attribute.  Must come
/// from dwarf.h and be an enumerator representing an attribute like,
/// e.g, DW_AT_decl_line.
///
///@param cst the output parameter that is set to the value of the
/// attribute @p attr_name.  This parameter is set iff the function
/// return true.
///
/// @return true if there was an attribute of the name @p attr_name
/// and with a value that is a constant, false otherwise.
static bool
die_unsigned_constant_attribute(Dwarf_Die*	die,
				unsigned	attr_name,
				uint64_t&	cst)
{
  if (!die)
    return false;

  Dwarf_Attribute attr;
  Dwarf_Word result = 0;
  if (!dwarf_attr_integrate(die, attr_name, &attr)
      || dwarf_formudata(&attr, &result))
    return false;

  cst = result;
  return true;
}

/// Get the value of a DIE attribute; that value is meant to be a
/// flag.
///
/// @param die the DIE to get the attribute from.
///
/// @param attr_name the DW_AT_* name of the attribute.  Must come
/// from dwarf.h and be an enumerator representing an attribute like,
/// e.g, DW_AT_external.
///
/// @param flag the output parameter to store the flag value into.
/// This is set iff the function returns true.
///
/// @return true if the DIE has a flag attribute named @p attr_name,
/// false otherwise.
static bool
die_flag_attribute(Dwarf_Die* die, unsigned attr_name, bool& flag)
{
  Dwarf_Attribute attr;
  bool f = false;
  if (!dwarf_attr_integrate(die, attr_name, &attr)
      || dwarf_formflag(&attr, &f))
    return false;

  flag = f;
  return true;
}

/// Get the mangled name from a given DIE.
///
/// @param die the DIE to read the mangled name from.
///
/// @return the mangled name if it's present in the DIE, or just an
/// empty string if it's not.
static string
die_linkage_name(Dwarf_Die* die)
{
  if (!die)
    return "";

  string linkage_name = die_string_attribute(die, DW_AT_linkage_name);
  if (linkage_name.empty())
    linkage_name = die_string_attribute(die, DW_AT_MIPS_linkage_name);
  return linkage_name;
}

/// Get the file path that is the value of the DW_AT_decl_file
/// attribute on a given DIE, if the DIE is a decl DIE having that
/// attribute.
///
/// @param die the DIE to consider.
///
/// @return a string containing the file path that is the logical
/// value of the DW_AT_decl_file attribute.  If the DIE @p die
/// doesn't have a DW_AT_decl_file attribute, then the return value is
/// just an empty string.
static string
die_decl_file_attribute(Dwarf_Die* die)
{
  if (!die)
    return "";

  const char* str = dwarf_decl_file(die);

  return str ? str : "";
}

/// Get the value of an attribute which value is supposed to be a
/// reference to a DIE.
///
/// @param die the DIE to read the value from.
///
/// @param die_is_in_alt_di true if @p die comes from alternate debug
/// info sections.
///
/// @param attr_name the DW_AT_* attribute name to read.
///
/// @param result the DIE resulting from reading the attribute value.
/// This is set iff the function returns true.
///
/// @param look_thru_abstract_origin if yes, the function looks
/// through the possible DW_AT_abstract_origin attribute all the way
/// down to the initial DIE that is cloned and look on that DIE to see
/// if it has the @p attr_name attribute.
///
/// @return true if the DIE @p die contains an attribute named @p
/// attr_name that is a DIE reference, false otherwise.
static bool
die_die_attribute(Dwarf_Die* die,
		  unsigned attr_name,
		  Dwarf_Die& result,
		  bool look_thru_abstract_origin)
{
  Dwarf_Attribute attr;
  if (look_thru_abstract_origin)
    {
      if (!dwarf_attr_integrate(die, attr_name, &attr))
	return false;
    }
  else
    {
      if (!dwarf_attr(die, attr_name, &attr))
	return false;
    }
  bool r = dwarf_formref_die(&attr, &result);
  return r;
}

/// Read and return a DW_FORM_addr attribute from a given DIE.
///
/// @param die the DIE to consider.
///
/// @param attr_name the name of the DW_FORM_addr attribute to read
/// the value from.
///
/// @param the resulting address.
///
/// @return true iff the attribute could be read, was of the expected
/// DW_FORM_addr and could thus be translated into the @p result.
static bool
die_address_attribute(Dwarf_Die* die, unsigned attr_name, Dwarf_Addr& result)
{
  Dwarf_Attribute attr;
  if (!dwarf_attr_integrate(die, attr_name, &attr))
    return false;
  return dwarf_formaddr(&attr, &result) == 0;
}

/// Returns the source location associated with a decl DIE.
///
/// @param ctxt the @ref read_context to use.
///
/// @param die the DIE the read the source location from.
///
/// @return the location associated with @p die.
static location
die_location(const read_context& ctxt, Dwarf_Die* die)
{
  if (!die)
    return location();

  string file = die_decl_file_attribute(die);
  uint64_t line = 0;
  die_unsigned_constant_attribute(die, DW_AT_decl_line, line);

  if (!file.empty() && line != 0)
    {
      translation_unit_sptr tu = ctxt.cur_transl_unit();
      location l = tu->get_loc_mgr().create_new_location(file, line, 1);
      return l;
    }
  return location();
}

/// Return the location, the name and the mangled name of a given DIE.
///
/// @param ctxt the read context to use.
///
/// @param die the DIE to read location and names from.
///
/// @param loc the location output parameter to set.
///
/// @param name the name output parameter to set.
///
/// @param linkage_name the linkage_name output parameter to set.
static void
die_loc_and_name(const read_context&	ctxt,
		 Dwarf_Die*		die,
		 location&		loc,
		 string&		name,
		 string&		linkage_name)
{
  loc = die_location(ctxt, die);
  name = die_string_attribute(die, DW_AT_name);
  linkage_name = die_linkage_name(die);
}

/// Get the size of a (type) DIE as the value for the parameter
/// DW_AT_byte_size or DW_AT_bit_size.
///
/// @param die the DIE to read the information from.
///
/// @param size the resulting size in bits.  This is set iff the
/// function return true.
///
/// @return true if the size attribute was found.
static bool
die_size_in_bits(Dwarf_Die* die, uint64_t& size)
{
  if (!die)
    return false;

  uint64_t byte_size = 0, bit_size = 0;

  if (!die_unsigned_constant_attribute(die, DW_AT_byte_size, byte_size))
    {
      if (!die_unsigned_constant_attribute(die, DW_AT_bit_size, bit_size))
	return false;
    }
  else
    bit_size = byte_size * 8;

  size = bit_size;

  return true;
}

/// Get the access specifier (from the DW_AT_accessibility attribute
/// value) of a given DIE.
///
/// @param die the DIE to consider.
///
/// @param access the resulting access.  This is set iff the function
/// returns true.
///
/// @return bool if the DIE contains the DW_AT_accessibility die.
static bool
die_access_specifier(Dwarf_Die * die, access_specifier& access)
{
  if (!die)
    return false;

  uint64_t a = 0;
  if (!die_unsigned_constant_attribute(die, DW_AT_accessibility, a))
    return false;

  access_specifier result = private_access;

  switch (a)
    {
    case private_access:
      result = private_access;
      break;

    case protected_access:
      result = protected_access;
      break;

    case public_access:
      result = public_access;
      break;

    default:
      break;
    }

  access = result;
  return true;
}

/// Test whether a given DIE represents a decl that is public.  That
/// is, one with the DW_AT_external attribute set.
///
/// @param die the DIE to consider for testing.
///
/// @return true if a DW_AT_external attribute is present and its
/// value is set to the true; return false otherwise.
static bool
die_is_public_decl(Dwarf_Die* die)
{
  bool is_public = false;
  die_flag_attribute(die, DW_AT_external, is_public);
  return is_public;
}

/// Test whether a given DIE represents a declaration-only DIE.
///
/// That is, if the DIE has the DW_AT_declaration flag set.
///
/// @param die the DIE to consider.
//
/// @return true if a DW_AT_declaration is present, false otherwise.
static bool
die_is_declaration_only(Dwarf_Die* die)
{
 bool is_declaration_only = false;
 die_flag_attribute(die, DW_AT_declaration, is_declaration_only);
 return is_declaration_only;
}

/// Tests whether a given DIE is artificial.
///
/// @param die the test to test for.
///
/// @return true if the DIE is artificial, false otherwise.
static bool
die_is_artificial(Dwarf_Die* die)
{
  bool is_artificial;
  return die_flag_attribute(die, DW_AT_artificial, is_artificial);
}

///@return true if a tag represents a type, false otherwise.
///
///@param tag the tag to consider.
static bool
is_type_tag(unsigned tag)
{
  bool result = false;

  switch (tag)
    {
    case DW_TAG_array_type:
    case DW_TAG_class_type:
    case DW_TAG_enumeration_type:
    case DW_TAG_pointer_type:
    case DW_TAG_reference_type:
    case DW_TAG_string_type:
    case DW_TAG_structure_type:
    case DW_TAG_subroutine_type:
    case DW_TAG_typedef:
    case DW_TAG_union_type:
    case DW_TAG_ptr_to_member_type:
    case DW_TAG_set_type:
    case DW_TAG_subrange_type:
    case DW_TAG_base_type:
    case DW_TAG_const_type:
    case DW_TAG_file_type:
    case DW_TAG_packed_type:
    case DW_TAG_thrown_type:
    case DW_TAG_volatile_type:
    case DW_TAG_restrict_type:
    case DW_TAG_interface_type:
    case DW_TAG_unspecified_type:
    case DW_TAG_shared_type:
    case DW_TAG_rvalue_reference_type:
      result = true;
      break;

    default:
      result = false;
      break;
    }

  return result;
}

/// Test if a DIE represents a type DIE.
///
/// @param die the DIE to consider.
///
/// @return true if @p die represents a type, false otherwise.
static bool
is_type_die(Dwarf_Die* die)
{
  if (!die)
    return false;
  return is_type_tag(dwarf_tag(die));
}

enum virtuality
{
  VIRTUALITY_NOT_VIRTUAL,
  VIRTUALITY_VIRTUAL,
  VIRTUALITY_PURE_VIRTUAL
};

/// Get the virtual-ness of a given DIE, that is, the value of the
/// DW_AT_virtuality attribute.
///
/// @param die the DIE to read from.
///
/// @param virt the resulting virtuality attribute.  This is set iff
/// the function returns true.
///
/// @return true if the virtual-ness could be determined.
static bool
die_virtuality(Dwarf_Die* die, virtuality& virt)
{
  if (!die)
    return false;

  uint64_t v = 0;
  die_unsigned_constant_attribute(die, DW_AT_virtuality, v);

  if (v == DW_VIRTUALITY_virtual)
    virt = VIRTUALITY_VIRTUAL;
  else if (v == DW_VIRTUALITY_pure_virtual)
    virt = VIRTUALITY_PURE_VIRTUAL;
  else
    virt = VIRTUALITY_NOT_VIRTUAL;

  return true;
}

/// Test whether the DIE represent either a virtual base or function.
///
/// @param die the DIE to consider.
///
/// @return bool if the DIE represents a virtual base or function,
/// false othersise.
static bool
die_is_virtual(Dwarf_Die* die)
{
  virtuality v;
  if (!die_virtuality(die, v))
    return false;

  return v == VIRTUALITY_PURE_VIRTUAL || v == VIRTUALITY_VIRTUAL;
}

/// Test if the DIE represents an entity that was declared inlined.
///
/// @param die the DIE to test for.
///
/// @return true if the DIE represents an entity that was declared
/// inlined.
static bool
die_is_declared_inline(Dwarf_Die* die)
{
  uint64_t inline_value = 0;
  if (!die_unsigned_constant_attribute(die, DW_AT_inline, inline_value))
    return false;
  return inline_value == DW_INL_declared_inlined;
}

/// Get the value of a given DIE attribute, knowing that it must be a
/// location expression.
///
/// @param die the DIE to read the attribute from.
///
/// @param attr_name the name of the attribute to read the value for.
///
/// @param expr the pointer to allocate and fill with the resulting
/// array of operators + operands forming a dwarf expression.  This is
/// set iff the function returns true.
///
/// @param expr_len the length of the resulting dwarf expression.
/// This is set iff the function returns true.
///
/// @return true if the attribute exists and has a dwarf expression as
/// value.  In that case the expr and expr_len arguments are set to
/// the resulting dwarf exprssion.
static bool
die_location_expr(Dwarf_Die* die,
		  unsigned attr_name,
		  Dwarf_Op** expr,
		  uint64_t* expr_len)
{
  if (!die)
    return false;

  Dwarf_Attribute attr;
  if (!dwarf_attr_integrate(die, attr_name, &attr))
    return false;

  size_t len = 0;
  bool result = (dwarf_getlocation(&attr, expr, &len) == 0);

  if (result)
    *expr_len = len;

  return result;
}

/// An abstraction of a value representing the result of the
/// evalutation of a dwarf expression.  This is abstraction represents
/// a partial view on the possible values because we are only
/// interested in extracting the latest and longuest constant
/// sub-expression of a given dwarf expression.
class expr_result
{
  bool is_const_;
  int64_t const_value_;

public:
  expr_result()
    : is_const_(true),
      const_value_(0)
  {}

  expr_result(bool is_const)
    : is_const_(is_const),
      const_value_(0)
  {}

  explicit expr_result(int64_t v)
    :is_const_(true),
     const_value_(v)
  {}

  /// @return true if the value is a constant.  Otherwise, return
  /// false, meaning the value represents a quantity for which we need
  /// inferior (a running program) state to determine the value.
  bool
  is_const() const
  {return is_const_;}


  /// @param f a flag saying if the value is set to a constant or not.
  void
  is_const(bool f)
  {is_const_ = f;}

  /// Get the current constant value iff this represents a
  /// constant.
  ///
  /// @param value the out parameter.  Is set to the constant value of
  /// the @ref expr_result.  This is set iff the function return true.
  ///
  ///@return true if this has a constant value, false otherwise.
  bool
  const_value(int64_t& value)
  {
    if (is_const())
      {
	value = const_value_;
	return true;
      }
    return false;
  }

  /// Getter of the constant value of the current @ref expr_result.
  ///
  /// Note that the current @ref expr_result must be constant,
  /// otherwise the current process is aborted.
  ///
  /// @return the constant value of the current @ref expr_result.
  int64_t
  const_value() const
  {
    assert(is_const());
    return const_value_;
  }

  operator int64_t() const
  {return const_value();}

  expr_result&
  operator=(const int64_t v)
  {
    const_value_ = v;
    return *this;
  }

  bool
  operator==(const expr_result& o) const
  {return const_value_ == o.const_value_ && is_const_ == o.is_const_;}

  bool
  operator>=(const expr_result& o) const
  {return const_value_ >= o.const_value_;}

  bool
  operator<=(const expr_result& o) const
  {return const_value_ <= o.const_value_;}

  bool
  operator>(const expr_result& o) const
  {return const_value_ > o.const_value_;}

  bool
  operator<(const expr_result& o) const
  {return const_value_ < o.const_value_;}

  expr_result
  operator+(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ += v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const_;
    return r;
  }

  expr_result&
  operator+=(int64_t v)
  {
    const_value_ += v;
    return *this;
  }

  expr_result
  operator-(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ -= v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const_;
    return r;
  }

  expr_result
  operator%(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ %= v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const();
    return r;
  }

  expr_result
  operator*(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ *= v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const();
    return r;
  }

  expr_result
  operator|(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ |= v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const_;
    return r;
  }

  expr_result
  operator^(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ ^= v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const_;
    return r;
  }

  expr_result
  operator>>(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ = r.const_value_ >> v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const_;
    return r;
  }

  expr_result
  operator<<(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ = r.const_value_ << v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const_;
    return r;
  }

  expr_result
  operator~() const
  {
    expr_result r(*this);
    r.const_value_ = ~r.const_value_;
    return r;
  }

  expr_result
  neg() const
  {
    expr_result r(*this);
    r.const_value_ = -r.const_value_;
    return r;
  }

  expr_result
  abs() const
  {
    expr_result r = *this;
    r.const_value_ = std::abs(static_cast<long double>(r.const_value()));
    return r;
  }

  expr_result
  operator&(const expr_result& o)
  {
    expr_result r(*this);
    r.const_value_ = *this & o;
    r.is_const_ = r.is_const_ && o.is_const_;
    return r;
  }

  expr_result
  operator/(const expr_result& o)
  {
    expr_result r(*this);
    r.is_const_ = r.is_const_ && o.is_const_;
    return r.const_value() / o.const_value();
  }
};// class end expr_result;


/// Abstraction of the evaluation context of a dwarf expression.
struct dwarf_expr_eval_context
{
  expr_result accum;
  deque<expr_result> stack;
  // Is set to true if the result of the expression that got evaluated
  // is a TLS address.
  bool set_tls_addr;

  dwarf_expr_eval_context()
    : accum(/*is_const=*/false),
      set_tls_addr(false)
  {
    stack.push_front(expr_result(true));
  }

  /// Set a flag to to tell that the result of the expression that got
  /// evaluated is a TLS address.
  ///
  /// @param f true iff the result of the expression that got
  /// evaluated is a TLS address, false otherwise.
  void
  set_tls_address(bool f)
  {set_tls_addr = f;}

  /// Getter for the flag that tells if the result of the expression
  /// that got evaluated is a TLS address.
  ///
  /// @return true iff the result of the expression that got evaluated
  /// is a TLS address.
  bool
  set_tls_address() const
  {return set_tls_addr;}

  expr_result
  pop()
  {
    expr_result r = stack.front();
    stack.pop_front();
    return r;
  }

  void
  push(const expr_result& v)
  {stack.push_front(v);}
};//end class dwarf_expr_eval_context

/// If the current operation in the dwarf expression represents a push
/// of a constant value onto the dwarf expr virtual machine (aka
/// DEVM), perform the operation and update the DEVM.
///
/// If the result of the operation is a constant, update the DEVM
/// accumulator with its value.  Otherwise, the DEVM accumulator is
/// left with its previous value.
///
/// @param ops the array of the dwarf expression operations to consider.
///
/// @param ops_len the lengths of @p ops array above.
///
/// @param index the index of the operation to interpret, in @p ops.
///
/// @param next_index the index of the operation to interpret at the
/// next step, after this function completed and returned.  This is
/// set an output parameter that is set iff the function returns true.
///
/// @param ctxt the DEVM evaluation context.
///
/// @return true if the current operation actually pushes a constant
/// value onto the DEVM stack, false otherwise.
static bool
op_pushes_constant_value(Dwarf_Op*			ops,
			 uint64_t			ops_len,
			 uint64_t			index,
			 uint64_t&			next_index,
			 dwarf_expr_eval_context&	ctxt)
{
  assert(index < ops_len);

  Dwarf_Op& op = ops[index];
  int64_t value = 0;

  switch (op.atom)
    {
    case DW_OP_addr:
      value = ops[index].number;
      break;

    case DW_OP_const1u:
    case DW_OP_const1s:
    case DW_OP_const2u:
    case DW_OP_const2s:
    case DW_OP_const4u:
    case DW_OP_const4s:
    case DW_OP_const8u:
    case DW_OP_const8s:
    case DW_OP_constu:
    case DW_OP_consts:
      value = ops[index].number;
      break;

    case DW_OP_lit0:
      value = 0;
      break;
    case DW_OP_lit1:
      value = 1;
      break;
    case DW_OP_lit2:
      value = 2;
      break;
    case DW_OP_lit3:
      value = 3;
      break;
    case DW_OP_lit4:
      value = 4;
      break;
    case DW_OP_lit5:
      value = 5;
      break;
    case DW_OP_lit6:
      value = 6;
      break;
    case DW_OP_lit7:
      value = 7;
      break;
    case DW_OP_lit8:
      value = 8;
      break;
    case DW_OP_lit9:
      value = 9;
      break;
    case DW_OP_lit10:
      value = 10;
      break;
    case DW_OP_lit11:
      value = 11;
      break;
    case DW_OP_lit12:
      value = 12;
      break;
    case DW_OP_lit13:
      value = 13;
      break;
    case DW_OP_lit14:
      value = 14;
      break;
    case DW_OP_lit15:
      value = 15;
      break;
    case DW_OP_lit16:
      value = 16;
      break;
    case DW_OP_lit17:
      value = 17;
      break;
    case DW_OP_lit18:
      value = 18;
      break;
    case DW_OP_lit19:
      value = 19;
      break;
    case DW_OP_lit20:
      value = 20;
      break;
    case DW_OP_lit21:
      value = 21;
      break;
    case DW_OP_lit22:
      value = 22;
      break;
    case DW_OP_lit23:
      value = 23;
      break;
    case DW_OP_lit24:
      value = 24;
      break;
    case DW_OP_lit25:
      value = 25;
      break;
    case DW_OP_lit26:
      value = 26;
      break;
    case DW_OP_lit27:
      value = 27;
      break;
    case DW_OP_lit28:
      value = 28;
      break;
    case DW_OP_lit29:
      value = 29;
      break;
    case DW_OP_lit30:
      value = 30;
      break;
    case DW_OP_lit31:
      value = 31;
      break;

    default:
      return false;
    }

  expr_result r(value);
  ctxt.stack.push_front(r);
  ctxt.accum = r;
  next_index = index + 1;

  return true;
}

/// If the current operation in the dwarf expression represents a push
/// of a non-constant value onto the dwarf expr virtual machine (aka
/// DEVM), perform the operation and update the DEVM.  A non-constant
/// is namely a quantity for which we need inferior (a running program
/// image) state to know the exact value.
///
/// Upon successful completion, as the result of the operation is a
/// non-constant the DEVM accumulator value is left to its state as of
/// before the invocation of this function.
///
/// @param ops the array of the dwarf expression operations to consider.
///
/// @param ops_len the lengths of @p ops array above.
///
/// @param index the index of the operation to interpret, in @p ops.
///
/// @param next_index the index of the operation to interpret at the
/// next step, after this function completed and returned.  This is
/// set an output parameter that is set iff the function returns true.
///
/// @param ctxt the DEVM evaluation context.
///
/// @return true if the current operation actually pushes a
/// non-constant value onto the DEVM stack, false otherwise.
static bool
op_pushes_non_constant_value(Dwarf_Op* ops,
			     uint64_t ops_len,
			     uint64_t index,
			     uint64_t& next_index,
			     dwarf_expr_eval_context& ctxt)
{
  assert(index < ops_len);
  Dwarf_Op& op = ops[index];

  switch (op.atom)
    {
    case DW_OP_reg0:
    case DW_OP_reg1:
    case DW_OP_reg2:
    case DW_OP_reg3:
    case DW_OP_reg4:
    case DW_OP_reg5:
    case DW_OP_reg6:
    case DW_OP_reg7:
    case DW_OP_reg8:
    case DW_OP_reg9:
    case DW_OP_reg10:
    case DW_OP_reg11:
    case DW_OP_reg12:
    case DW_OP_reg13:
    case DW_OP_reg14:
    case DW_OP_reg15:
    case DW_OP_reg16:
    case DW_OP_reg17:
    case DW_OP_reg18:
    case DW_OP_reg19:
    case DW_OP_reg20:
    case DW_OP_reg21:
    case DW_OP_reg22:
    case DW_OP_reg23:
    case DW_OP_reg24:
    case DW_OP_reg25:
    case DW_OP_reg26:
    case DW_OP_reg27:
    case DW_OP_reg28:
    case DW_OP_reg29:
    case DW_OP_reg30:
    case DW_OP_reg31:
      next_index = index + 1;
      break;

    case DW_OP_breg0:
    case DW_OP_breg1:
    case DW_OP_breg2:
    case DW_OP_breg3:
    case DW_OP_breg4:
    case DW_OP_breg5:
    case DW_OP_breg6:
    case DW_OP_breg7:
    case DW_OP_breg8:
    case DW_OP_breg9:
    case DW_OP_breg10:
    case DW_OP_breg11:
    case DW_OP_breg12:
    case DW_OP_breg13:
    case DW_OP_breg14:
    case DW_OP_breg15:
    case DW_OP_breg16:
    case DW_OP_breg17:
    case DW_OP_breg18:
    case DW_OP_breg19:
    case DW_OP_breg20:
    case DW_OP_breg21:
    case DW_OP_breg22:
    case DW_OP_breg23:
    case DW_OP_breg24:
    case DW_OP_breg25:
    case DW_OP_breg26:
    case DW_OP_breg27:
    case DW_OP_breg28:
    case DW_OP_breg29:
    case DW_OP_breg30:
    case DW_OP_breg31:
      next_index = index + 1;
      break;

    case DW_OP_regx:
      next_index = index + 2;
      break;

    case DW_OP_fbreg:
      next_index = index + 1;
      break;

    case DW_OP_bregx:
      next_index = index + 1;
      break;

    default:
      return false;
    }

  expr_result r(false);
  ctxt.stack.push_front(r);

  return true;
}

/// If the current operation in the dwarf expression represents a
/// manipulation of the stack of the DWARF Expression Virtual Machine
/// (aka DEVM), this function performs the operation and updates the
/// state of the DEVM.  If the result of the operation represents a
/// constant value, then the accumulator of the DEVM is set to that
/// result's value, Otherwise, the DEVM accumulator is left with its
/// previous value.
///
/// @param expr the array of the dwarf expression operations to consider.
///
/// @param expr_len the lengths of @p ops array above.
///
/// @param index the index of the operation to interpret, in @p ops.
///
/// @param next_index the index of the operation to interpret at the
/// next step, after this function completed and returned.  This is
/// set an output parameter that is set iff the function returns true.
///
/// @param ctxt the DEVM evaluation context.
///
/// @return true if the current operation actually manipulates the
/// DEVM stack, false otherwise.
static bool
op_manipulates_stack(Dwarf_Op* expr,
		     uint64_t expr_len,
		     uint64_t index,
		     uint64_t& next_index,
		     dwarf_expr_eval_context& ctxt)
{
  Dwarf_Op& op = expr[index];
  expr_result v;

  switch (op.atom)
    {
    case DW_OP_dup:
      v = ctxt.stack.front();
      ctxt.stack.push_front(v);
      break;

    case DW_OP_drop:
      v = ctxt.stack.front();
      ctxt.stack.pop_front();
      break;

    case DW_OP_over:
	assert(ctxt.stack.size() > 1);
	v = ctxt.stack[1];
	ctxt.stack.push_front(v);
      break;

    case DW_OP_pick:
	assert(index + 1 < expr_len);
	v = op.number;
	ctxt.stack.push_front(v);
      break;

    case DW_OP_swap:
	assert(ctxt.stack.size() > 1);
	v = ctxt.stack[1];
	ctxt.stack.erase(ctxt.stack.begin() + 1);
	ctxt.stack.push_front(v);
      break;

    case DW_OP_rot:
	assert(ctxt.stack.size() > 2);
	v = ctxt.stack[2];
	ctxt.stack.erase(ctxt.stack.begin() + 2);
	ctxt.stack.push_front(v);
      break;

    case DW_OP_deref:
    case DW_OP_deref_size:
      assert(ctxt.stack.size() > 0);
      ctxt.stack.pop_front();
      v.is_const(false);
      ctxt.stack.push_front(v);
      break;

    case DW_OP_xderef:
    case DW_OP_xderef_size:
      assert(ctxt.stack.size() > 1);
      ctxt.stack.pop_front();
      ctxt.stack.pop_front();
      v.is_const(false);
      ctxt.stack.push_front(v);
      break;

    case DW_OP_push_object_address:
      v.is_const(false);
      ctxt.stack.push_front(v);
      break;

    case DW_OP_form_tls_address:
    case DW_OP_GNU_push_tls_address:
      assert(ctxt.stack.size() > 0);
      v = ctxt.pop();
      if (op.atom == DW_OP_form_tls_address)
	v.is_const(false);
      ctxt.stack.push_front(v);
      break;

    case DW_OP_call_frame_cfa:
      v.is_const(false);
      ctxt.stack.push_front(v);
      break;

    default:
      return false;
    }

  if (v.is_const())
    ctxt.accum = v;

  if (op.atom == DW_OP_form_tls_address
      || op.atom == DW_OP_GNU_push_tls_address)
    ctxt.set_tls_address(true);
  else
    ctxt.set_tls_address(false);

  next_index = index + 1;

  return true;
}

/// If the current operation in the dwarf expression represents a push
/// of an arithmetic or logic operation onto the dwarf expr virtual
/// machine (aka DEVM), perform the operation and update the DEVM.
///
/// If the result of the operation is a constant, update the DEVM
/// accumulator with its value.  Otherwise, the DEVM accumulator is
/// left with its previous value.
///
/// @param expr the array of the dwarf expression operations to consider.
///
/// @param expr_len the lengths of @p expr array above.
///
/// @param index the index of the operation to interpret, in @p expr.
///
/// @param next_index the index of the operation to interpret at the
/// next step, after this function completed and returned.  This is
/// set an output parameter that is set iff the function returns true.
///
/// @param ctxt the DEVM evaluation context.
///
/// @return true if the current operation actually represent an
/// arithmetic or logic operation.
static bool
op_is_arith_logic(Dwarf_Op* expr,
		  uint64_t expr_len,
		  uint64_t index,
		  uint64_t& next_index,
		  dwarf_expr_eval_context& ctxt)
{
  assert(index < expr_len);

  Dwarf_Op& op = expr[index];
  expr_result val1, val2;

  switch (op.atom)
    {
    case DW_OP_abs:
      val1 = ctxt.pop();
      val1 = val1.abs();
      ctxt.push(val1);
      break;

    case DW_OP_and:
      assert(ctxt.stack.size() > 1);
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val1 & val2);
      break;

    case DW_OP_div:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      if (!val1.is_const())
	val1 = 1;
      ctxt.push(val2 / val1);
      break;

    case DW_OP_minus:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val2 - val1);
      break;

    case DW_OP_mod:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val2 % val1);
      break;

    case DW_OP_mul:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val2 * val1);
      break;

    case DW_OP_neg:
      val1 = ctxt.pop();
      ctxt.push(-val1);
      break;

    case DW_OP_not:
      val1 = ctxt.pop();
      ctxt.push(~val1);
      break;

    case DW_OP_or:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val1 | val2);
      break;

    case DW_OP_plus:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val2 + val1);
      break;

    case DW_OP_plus_uconst:
      val1 = ctxt.pop();
      val1 += op.number;
      ctxt.push(val1);
      break;

    case DW_OP_shl:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val2 << val1);
      break;

    case DW_OP_shr:
    case DW_OP_shra:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val2 >> val1);
      break;

    case DW_OP_xor:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val2 ^ val1);
      break;

    default:
      return false;
    }

  if (ctxt.stack.front().is_const())
    ctxt.accum = ctxt.stack.front();

  next_index = index + 1;
  return true;
}

/// If the current operation in the dwarf expression represents a push
/// of a control flow operation onto the dwarf expr virtual machine
/// (aka DEVM), perform the operation and update the DEVM.
///
/// If the result of the operation is a constant, update the DEVM
/// accumulator with its value.  Otherwise, the DEVM accumulator is
/// left with its previous value.
///
/// @param expr the array of the dwarf expression operations to consider.
///
/// @param expr_len the lengths of @p expr array above.
///
/// @param index the index of the operation to interpret, in @p expr.
///
/// @param next_index the index of the operation to interpret at the
/// next step, after this function completed and returned.  This is
/// set an output parameter that is set iff the function returns true.
///
/// @param ctxt the DEVM evaluation context.
///
/// @return true if the current operation actually represents a
/// control flow operation, false otherwise.
static bool
op_is_control_flow(Dwarf_Op* expr,
		   uint64_t expr_len,
		   uint64_t index,
		   uint64_t& next_index,
		   dwarf_expr_eval_context& ctxt)
{
    assert(index < expr_len);

  Dwarf_Op& op = expr[index];
  expr_result val1, val2;

  switch (op.atom)
    {
    case DW_OP_eq:
    case DW_OP_ge:
    case DW_OP_gt:
    case DW_OP_le:
    case DW_OP_lt:
    case DW_OP_ne:
      {
	bool value = true;
	val1 = ctxt.pop();
	val2 = ctxt.pop();
      if (op.atom == DW_OP_eq)
	value = val2 == val1;
      else if (op.atom == DW_OP_ge)
	value = val2 >= val1;
      else if (op.atom == DW_OP_gt)
	value = val2 > val1;
      else if (op.atom == DW_OP_le)
	value = val2 <= val1;
      else if (op.atom == DW_OP_lt)
	value = val2 < val1;
      else if (op.atom == DW_OP_ne)
	value = val2 != val1;

      val1 = value ? 1 : 0;
      ctxt.push(val1);
      }
      break;

    case DW_OP_skip:
      if (op.number > 0)
	index += op.number - 1;
      break;

    case DW_OP_bra:
      val1 = ctxt.pop();
      if (val1 != 0)
	  index += val1.const_value() - 1;
      break;

    case DW_OP_call2:
    case DW_OP_call4:
    case DW_OP_call_ref:
    case DW_OP_nop:
      break;

    default:
      return false;
    }

  if (ctxt.stack.front().is_const())
    ctxt.accum = ctxt.stack.front();

  next_index = index + 1;
  return true;
}

/// Evaluate the value of the last sub-expression that is a constant,
/// inside a given DWARF expression.
///
/// @param expr the DWARF expression to consider.
///
/// @param expr_len the length of the expression to consider.
///
/// @param value the resulting value of the last constant
/// sub-expression of the DWARF expression.  This is set iff the
/// function returns true.
///
/// @return true if the function could find a constant sub-expression
/// to evaluate, false otherwise.
static bool
eval_last_constant_dwarf_sub_expr(Dwarf_Op*	expr,
				  uint64_t	expr_len,
				  int64_t&	value,
				  bool&	is_tls_address)
{
  dwarf_expr_eval_context eval_ctxt;

  uint64_t index = 0, next_index = 0;
  do
    {
      if (op_is_arith_logic(expr, expr_len, index,
			    next_index, eval_ctxt)
	  || op_pushes_constant_value(expr, expr_len, index,
				      next_index, eval_ctxt)
	  || op_manipulates_stack(expr, expr_len, index,
				  next_index, eval_ctxt)
	  || op_pushes_non_constant_value(expr, expr_len, index,
					  next_index, eval_ctxt)
	  || op_is_control_flow(expr, expr_len, index,
				next_index, eval_ctxt))
	;
      else
	next_index = index + 1;

      assert(next_index > index);
      index = next_index;
    } while (index < expr_len);

  is_tls_address = eval_ctxt.set_tls_address();
  if (eval_ctxt.accum.is_const())
    {
      value = eval_ctxt.accum;
      return true;
    }
  return false;
}

/// Get the offset of a struct/class member as represented by the
/// value of the DW_AT_data_member_location attribute.
///
/// There is a huge gotcha in here.  The value of the
/// DW_AT_data_member_location is not necessarily a constant that one
/// would just read and be done with it.  Rather, it can be a DWARF
/// expression that one has to interpret.  In general, the offset can
/// be given by the DW_AT_bit_offset attribute.  In that case the
/// offset is a constant.  But it can also be given by the
/// DW_AT_data_member_location attribute.  In that case it's a DWARF
/// location expression.
///
/// When the it's the DW_AT_data_member_location that is present,
/// there are three cases to possibly take into account:
///
///     1/ The offset in the vtable where the offset of a virtual base
///        can be found, aka vptr offset.  Given the address of a
///        given object O, the vptr offset for B is given by the
///        (DWARF) expression:
///
///            address(O) + *(*address(0) - VIRTUAL_OFFSET)
///
///        where VIRTUAL_OFFSET is a constant value; In this case,
///        this function returns the constant VIRTUAL_OFFSET, as this
///        is enough to detect changes in a given virtual base
///        relative to the other virtual bases.
///
///     2/ The offset of a regular data member.  Given the address of
///        a struct object named O, the memory location for a
///        particular data member is given by the (DWARF) expression:
///
///            address(O) + OFFSET
///
///       where OFFSET is a constant.  In this case, this function
///       returns the OFFSET constant.
///
///     3/ The offset of a virtual member function in the virtual
///     pointer.  The DWARF expression is a constant that designates
///     the offset of the function in the vtable.  In this case this
///     function returns that constant.
///
///@param die the DIE to read the information from.
///
///@param offset the resulting constant offset, in bits.  This
///argument is set iff the function returns true.
static bool
die_member_offset(Dwarf_Die* die, int64_t& offset)
{
  Dwarf_Op* expr = NULL;
  uint64_t expr_len = 0;
  uint64_t off = 0;

  if (die_unsigned_constant_attribute(die, DW_AT_bit_offset, off))
    {
      // The DW_AT_bit_offset is present.  If it contains a non-zero
      // value, let's read that one.
      if (off != 0)
	{
	  offset = off;
	  return true;
	}
    }

  if (!die_location_expr(die, DW_AT_data_member_location, &expr, &expr_len))
    return false;

  // Otherwise, the DW_AT_data_member_location attribute is present.
  // In that case, let's evaluate it and get its constant
  // sub-expression and return that one.

  bool is_tls_address = false;
  if (!eval_last_constant_dwarf_sub_expr(expr, expr_len,
					 offset, is_tls_address))
    return false;

  offset *= 8;
  return true;
}

/// Read the value of the DW_AT_location attribute from a DIE,
/// evaluate the resulting DWARF expression and, if it's a constant
/// expression, return it.
///
/// @param die the DIE to consider.
///
/// @param address the resulting constant address.  This is set iff
/// the function returns true.
///
/// @return true iff the whole sequence of action described above
/// could be completed normally.
static bool
die_location_address(Dwarf_Die*	die,
		     Dwarf_Addr&	address,
		     bool&		is_tls_address)
{
  Dwarf_Op* expr = NULL;
  uint64_t expr_len = 0;

  is_tls_address = false;
  if (!die_location_expr(die, DW_AT_location, &expr, &expr_len))
    return false;

  int64_t addr = 0;
  if (!eval_last_constant_dwarf_sub_expr(expr, expr_len, addr, is_tls_address))
    return false;

  address = addr;
  return true;
}


/// Return the index of a function in its virtual table.  That is,
/// return the value of the DW_AT_vtable_elem_location attribute.
///
/// @param die the DIE of the function to consider.
///
/// @param vindex the resulting index.  This is set iff the function
/// returns true.
///
/// @return true if the DIE has a DW_AT_vtable_elem_location
/// attribute.
static bool
die_virtual_function_index(Dwarf_Die* die,
			   uint64_t& vindex)
{
  if (!die)
    return false;

  Dwarf_Op* expr = NULL;
  uint64_t expr_len = 0;
  if (!die_location_expr(die, DW_AT_vtable_elem_location,
			 &expr, &expr_len))
    return false;

  int64_t i = 0;
  bool is_tls_addr = false;
  if (!eval_last_constant_dwarf_sub_expr(expr, expr_len, i, is_tls_addr))
    return false;

  vindex = i;
  return true;
}

/// Get the point where a DW_AT_import DIE is used to import a given
/// (unit) DIE, between two DIEs.
///
/// @param ctxt the dwarf reading context to consider.
///
/// @param partial_unit_offset the imported unit for which we want to
/// know the insertion point.  This is usually a partial unit (with
/// tag DW_TAG_partial_unit) but it does not necessarily have to be
/// so.
///
/// @param first_die_offset the offset of the DIE from which this
/// function starts looking for the import point of
/// @partial_unit_offset.  Note that this offset is excluded from the
/// set of potential solutions.
///
/// @param first_die_cu_offset the offset of the (compilation) unit
/// that @p first_die_cu_offset belongs to.
///
/// @param source where the DIE of first_die_cu_offset unit comes
/// from.
///
/// @param last_die_offset the offset of the last DIE of the up to
/// which this function looks for the import point of @p
/// partial_unit_offset.  Note that this offset is excluded from the
/// set of potential solutions.
///
/// @param imported_point_offset.  The resulting
/// imported_point_offset.  Note that if the imported DIE @p
/// partial_unit_offset is not found between @p first_die_offset and
/// @p last_die_offset, this parameter is left untouched by this
/// function.
///
/// @return true iff an imported unit is found between @p
/// first_die_offset and @p last_die_offset.
static bool
find_import_unit_point_between_dies(const read_context& ctxt,
				    size_t		partial_unit_offset,
				    Dwarf_Off		first_die_offset,
				    Dwarf_Off		first_die_cu_offset,
				    die_source		source,
				    size_t		last_die_offset,
				    size_t&		imported_point_offset)
{
  const tu_die_imported_unit_points_map_type& tu_die_imported_unit_points_map =
    ctxt.tu_die_imported_unit_points_map(source);

  tu_die_imported_unit_points_map_type::const_iterator iter =
    tu_die_imported_unit_points_map.find(first_die_cu_offset);

  assert(iter != tu_die_imported_unit_points_map.end());

  const imported_unit_points_type& imported_unit_points = iter->second;
  if (imported_unit_points.empty())
    return false;

  imported_unit_points_type::const_iterator b = imported_unit_points.begin();
  imported_unit_points_type::const_iterator e = imported_unit_points.end();

  find_lower_bound_in_imported_unit_points(imported_unit_points,
					   first_die_offset,
					   b);

  if (last_die_offset != static_cast<size_t>(-1))
    find_lower_bound_in_imported_unit_points(imported_unit_points,
					     last_die_offset,
					     e);

  if (e != imported_unit_points.end())
    {
      for (imported_unit_points_type::const_iterator i = e; i >= b; --i)
	if (i->imported_unit_die_off == partial_unit_offset)
	  {
	    imported_point_offset = i->offset_of_import ;
	    return true;
	  }

      for (imported_unit_points_type::const_iterator i = e; i >= b; --i)
	{
	  if (find_import_unit_point_between_dies(ctxt,
						  partial_unit_offset,
						  i->imported_unit_child_off,
						  i->imported_unit_cu_off,
						  i->imported_unit_die_source,
						  /*(Dwarf_Off)*/-1,
						  imported_point_offset))
	    return true;
	}
    }
  else
    {
      for (imported_unit_points_type::const_iterator i = b; i != e; ++i)
	if (i->imported_unit_die_off == partial_unit_offset)
	  {
	    imported_point_offset = i->offset_of_import ;
	    return true;
	  }

      for (imported_unit_points_type::const_iterator i = b; i != e; ++i)
	{
	  if (find_import_unit_point_between_dies(ctxt,
						  partial_unit_offset,
						  i->imported_unit_child_off,
						  i->imported_unit_cu_off,
						  i->imported_unit_die_source,
						  /*(Dwarf_Off)*/-1,
						  imported_point_offset))
	    return true;
	}
    }

  return false;
}

/// In the current translation unit, get the last point where a
/// DW_AT_import DIE is used to import a given (unit) DIE, before a
/// given DIE is found.  That given DIE is called the limit DIE.
///
/// Said otherwise, this function returns the last import point of a
/// unit, before a limit.
///
/// @param ctxt the dwarf reading context to consider.
///
/// @param partial_unit_offset the imported unit for which we want to
/// know the insertion point of.  This is usually a partial unit (with
/// tag DW_TAG_partial_unit) but it does not necessarily have to be
/// so.
///
/// @param where_offset the offset of the limit DIE.
///
/// @param imported_point_offset.  The resulting imported_point_offset.
/// Note that if the imported DIE @p partial_unit_offset is not found
/// before @p die_offset, this is set to the last @p
/// partial_unit_offset found under @p parent_die.
///
/// @return true iff an imported unit is found before @p die_offset.
/// Note that if an imported unit is found after @p die_offset then @p
/// imported_point_offset is set and the function return false.
static bool
find_import_unit_point_before_die(const read_context&	ctxt,
				  size_t		partial_unit_offset,
				  size_t		where_offset,
				  size_t&		imported_point_offset)
{
  size_t import_point_offset = 0;
  Dwarf_Die first_die_of_tu;

  if (dwarf_child(const_cast<Dwarf_Die*>(ctxt.cur_tu_die()),
		  &first_die_of_tu) != 0)
    return false;

  Dwarf_Die cu_die_memory;
  Dwarf_Die *cu_die;

  cu_die = dwarf_diecu(const_cast<Dwarf_Die*>(&first_die_of_tu),
		       &cu_die_memory, 0, 0);

  if (find_import_unit_point_between_dies(ctxt, partial_unit_offset,
					  dwarf_dieoffset(&first_die_of_tu),
					  dwarf_dieoffset(cu_die),
					  /*source=*/PRIMARY_DEBUG_INFO_DIE_SOURCE,
					  where_offset,
					  import_point_offset))
    {
      imported_point_offset = import_point_offset;
      return true;
    }

  if (import_point_offset)
    {
      imported_point_offset = import_point_offset;
      return true;
    }

  return false;
}

/// Return the parent DIE for a given DIE.
///
/// Note that the function build_die_parent_map() must have been
/// called before this one can work.  This function either succeeds or
/// aborts the current process.
///
/// @param ctxt the read context to consider.
///
/// @param die the DIE for which we want the parent.
///
/// @param parent_die the output parameter set to the parent die of
/// @p die.  Its memory must be allocated and handled by the caller.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return true if the function could get a parent DIE, false
/// otherwise.
static bool
get_parent_die(const read_context&	ctxt,
	       Dwarf_Die*		die,
	       Dwarf_Die&		parent_die,
	       size_t			where_offset)
{
  assert(ctxt.dwarf());

  die_source source = NO_DEBUG_INFO_DIE_SOURCE;
  assert(ctxt.get_die_source(die, source));

  const offset_offset_map& m = ctxt.die_parent_map(source);
  offset_offset_map::const_iterator i = m.find(dwarf_dieoffset(die));

  if (i == m.end())
    return false;

  switch(source)
    {
    case PRIMARY_DEBUG_INFO_DIE_SOURCE:
      assert(dwarf_offdie(ctxt.dwarf(), i->second, &parent_die));
      break;
    case ALT_DEBUG_INFO_DIE_SOURCE:
      assert(dwarf_offdie(ctxt.alt_dwarf(), i->second, &parent_die));
      break;
    case TYPE_UNIT_DIE_SOURCE:
      assert(dwarf_offdie_types(ctxt.dwarf(), i->second, &parent_die));
      break;
    case NO_DEBUG_INFO_DIE_SOURCE:
    case NUMBER_OF_DIE_SOURCES:
      ABG_ASSERT_NOT_REACHED;
    }

  if (dwarf_tag(&parent_die) == DW_TAG_partial_unit)
    {
      assert(where_offset);
      size_t import_point_offset = 0;
      bool found =
	find_import_unit_point_before_die(ctxt,
					  dwarf_dieoffset(&parent_die),
					  where_offset,
					  import_point_offset);
      if (!found)
	// It looks like parent_die (which comes from the alternate
	// debug info file) hasn't been imported into this TU.  So,
	// Let's assume its logical parent is the DIE of the current
	// TU.
	parent_die = *ctxt.cur_tu_die();
      else
	{
	  assert(import_point_offset);
	  Dwarf_Die import_point_die;
	  assert(dwarf_offdie(ctxt.dwarf(),
			      import_point_offset,
			      &import_point_die));
	  return get_parent_die(ctxt, &import_point_die,
				parent_die, where_offset);
	}
    }

  return true;
}

/// Return the abigail IR node representing the scope of a given DIE.
///
/// Note that it is the logical scope that is returned.  That is, if
/// the DIE has a DW_AT_specification or DW_AT_abstract_origin
/// attribute, it's the scope of the referred-to DIE (via these
/// attributes) that is returned.
///
/// @param ctxt the dwarf reading context to use.
///
/// @param die the DIE to get the scope for.
///
/// @param called_from_public_decl is true if this function has been
/// initially called within the context of a public decl.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
static scope_decl_sptr
get_scope_for_die(read_context& ctxt,
		  Dwarf_Die*	die,
		  bool		called_for_public_decl,
		  size_t	where_offset)
{
  die_source source_of_die;
  assert(ctxt.get_die_source(die, source_of_die));

  Dwarf_Die cloned_die;
  if (die_die_attribute(die, DW_AT_specification, cloned_die, false)
      || die_die_attribute(die, DW_AT_abstract_origin, cloned_die, false))
    return get_scope_for_die(ctxt, &cloned_die,
			     called_for_public_decl,
			     where_offset);

  Dwarf_Die parent_die;

  if (!get_parent_die(ctxt, die, parent_die, where_offset))
    return scope_decl_sptr();

  if (dwarf_tag(&parent_die) == DW_TAG_compile_unit
      || dwarf_tag(&parent_die) == DW_TAG_partial_unit
      || dwarf_tag(&parent_die) == DW_TAG_type_unit)
    {
      if (dwarf_tag(&parent_die) == DW_TAG_partial_unit
	  || dwarf_tag(&parent_die) == DW_TAG_type_unit)
	{
	  assert(source_of_die == ALT_DEBUG_INFO_DIE_SOURCE
		 || source_of_die == TYPE_UNIT_DIE_SOURCE);
	  return ctxt.cur_transl_unit()->get_global_scope();
	}

      // For top level DIEs like DW_TAG_compile_unit, we just want to
      // return the global scope for the corresponding translation
      // unit.  This must have been set by
      // build_translation_unit_and_add_to_ir.
      die_tu_map_type::const_iterator i =
	ctxt.die_tu_map().find(dwarf_dieoffset(&parent_die));
      assert(i != ctxt.die_tu_map().end());
      return i->second->get_global_scope();
    }

  scope_decl_sptr s;
  type_or_decl_base_sptr d;
  if (dwarf_tag(&parent_die) == DW_TAG_subprogram)
    // this is an entity defined in a scope that is a function.
    // Normally, I would say that this should be dropped.  But I have
    // seen a case where a typedef DIE needed by a function parameter
    // was defined right before the parameter, under the scope of the
    // function.  Yeah, weird.  So if I drop the typedef DIE, I'd drop
    // the function parm too.  So for that case, let's say that the
    // scope is the scope of the function itself.
    return get_scope_for_die(ctxt, &parent_die,
			     called_for_public_decl, where_offset);
  else
    d = build_ir_node_from_die(ctxt, &parent_die,
			       called_for_public_decl,
			       where_offset);
  s =  dynamic_pointer_cast<scope_decl>(d);
  if (!s)
    // this is an entity defined in someting that is not a scope.
    // Let's drop it.
    return scope_decl_sptr();

  class_decl_sptr cl = dynamic_pointer_cast<class_decl>(d);
  if (cl && cl->get_is_declaration_only())
    {
      scope_decl_sptr scop (cl->get_definition_of_declaration());
      if (scop)
	s = scop;
      else
	s = cl;
    }
  return s;
}

/// Convert a DWARF constant representing the value of the
/// DW_AT_language property into the translation_unit::language
/// enumerator.
///
/// @param l the DWARF constant to convert.
///
/// @return the resulting translation_unit::language enumerator.
static translation_unit::language
dwarf_language_to_tu_language(size_t l)
{
  switch (l)
    {
    case DW_LANG_C89:
      return translation_unit::LANG_C89;
    case DW_LANG_C:
      return translation_unit::LANG_C;
    case DW_LANG_Ada83:
      return translation_unit::LANG_Ada83;
    case DW_LANG_C_plus_plus:
      return translation_unit::LANG_C_plus_plus;
    case DW_LANG_Cobol74:
      return translation_unit::LANG_Cobol74;
    case DW_LANG_Cobol85:
      return translation_unit::LANG_Cobol85;
    case DW_LANG_Fortran77:
      return translation_unit::LANG_Fortran77;
    case DW_LANG_Fortran90:
      return translation_unit::LANG_Fortran90;
    case DW_LANG_Pascal83:
      return translation_unit::LANG_Pascal83;
    case DW_LANG_Modula2:
      return translation_unit::LANG_Modula2;
    case DW_LANG_Java:
      return translation_unit::LANG_Java;
    case DW_LANG_C99:
      return translation_unit::LANG_C99;
    case DW_LANG_Ada95:
      return translation_unit::LANG_Ada95;
    case DW_LANG_Fortran95:
      return translation_unit::LANG_Fortran95;
    case DW_LANG_PL1:
      return translation_unit::LANG_PL1;
    case DW_LANG_ObjC:
      return translation_unit::LANG_ObjC;
    case DW_LANG_ObjC_plus_plus:
      return translation_unit::LANG_ObjC_plus_plus;
#ifdef DW_LANG_UPC
    case DW_LANG_UPC:
      return DW_LANG_UPC;
#endif

#ifdef DW_LANG_D
    case DW_LANG_D:
      return translation_unit::LANG_D;
#endif

#ifdef DW_LANG_Python
    case DW_LANG_Python:
      return translation_unit::LANG_Python;
#endif

#ifdef DW_LANG_Go
    case DW_LANG_Go:
      return translation_unit::LANG_Go;
#endif

#ifdef DW_LANG_C_plus_plus_11
    case DW_LANG_C_plus_plus_11:
      return translation_unit::LANG_C_plus_plus_11;
#endif

#ifdef DW_LANG_C11
    case DW_LANG_C11:
      return translation_unit::LANG_C11;
#endif

#ifdef DW_LANG_C_plus_plus_14
    case DW_LANG_C_plus_plus_14:
      return translation_unit::LANG_C_plus_plus_14;
#endif

#ifdef DW_LANG_Mips_Assembler
    case DW_LANG_Mips_Assembler:
      return translation_unit::LANG_Mips_Assembler;
#endif

    default:
      return translation_unit::LANG_UNKNOWN;
    }
}

/// Get the default array lower bound value as defined by the DWARF
/// specification, version 4, depending on the language of the
/// translation unit.
///
/// @param l the language of the translation unit.
///
/// @return the default array lower bound value.
static int
get_default_array_lower_bound(translation_unit::language l)
{
  int value = 0;
    switch (l)
    {
    case translation_unit::LANG_UNKNOWN:
      value = 0;
      break;
    case translation_unit::LANG_Cobol74:
    case translation_unit::LANG_Cobol85:
      value = 1;
      break;
    case translation_unit::LANG_C89:
    case translation_unit::LANG_C99:
    case translation_unit::LANG_C11:
    case translation_unit::LANG_C:
    case translation_unit::LANG_C_plus_plus_11:
    case translation_unit::LANG_C_plus_plus_14:
    case translation_unit::LANG_C_plus_plus:
    case translation_unit::LANG_ObjC:
    case translation_unit::LANG_ObjC_plus_plus:
      value = 0;
      break;
    case translation_unit::LANG_Fortran77:
    case translation_unit::LANG_Fortran90:
    case translation_unit::LANG_Fortran95:
    case translation_unit::LANG_Ada83:
    case translation_unit::LANG_Ada95:
    case translation_unit::LANG_Pascal83:
    case translation_unit::LANG_Modula2:
      value = 1;
      break;
    case translation_unit::LANG_Java:
      value = 0;
      break;
    case translation_unit::LANG_PL1:
      value = 1;
      break;
    case translation_unit::LANG_UPC:
    case translation_unit::LANG_D:
    case translation_unit::LANG_Python:
    case translation_unit::LANG_Go:
    case translation_unit::LANG_Mips_Assembler:
      value = 0;
      break;
    }

    return value;
}

/// For a given offset, find the lower bound of a sorted vector of
/// imported unit point offset.
///
/// The lower bound is the smallest point (the point with the smallest
/// offset) which is the greater than a given offset.
///
/// @param imported_unit_points_type the sorted vector  of imported
/// unit points.
///
/// @param val the offset to consider when looking for the lower
/// bound.
///
/// @param r an iterator to the lower bound found.  This parameter is
/// set iff the function returns true.
///
/// @return true iff the lower bound has been found.
static bool
find_lower_bound_in_imported_unit_points(const imported_unit_points_type& p,
					 Dwarf_Off val,
					 imported_unit_points_type::const_iterator& r)
{
  imported_unit_point v(val);
  imported_unit_points_type::const_iterator result =
    std::lower_bound(p.begin(), p.end(), v);

  bool is_ok = result != p.end();

  if (is_ok)
    r = result;

  return is_ok;
}

/// Given a DW_TAG_compile_unit, build and return the corresponding
/// abigail::translation_unit ir node.  Note that this function
/// recursively reads the children dies of the current DIE and
/// populates the resulting translation unit.
///
/// @param ctxt the read_context to use.
///
/// @param die the DW_TAG_compile_unit DIE to consider.
///
/// @param address_size the size of the addresses expressed in this
/// translation unit in general.
///
/// @return a pointer to the resulting translation_unit.
static translation_unit_sptr
build_translation_unit_and_add_to_ir(read_context&	ctxt,
				     Dwarf_Die*	die,
				     char		address_size)
{
  translation_unit_sptr result;

  if (!die)
    return result;
  assert(dwarf_tag(die) == DW_TAG_compile_unit);

  // Clear the part of the context that is dependent on the translation
  // unit we are reading.
  ctxt.clear_per_translation_unit_data();

  ctxt.cur_tu_die(die);

  string path = die_string_attribute(die, DW_AT_name);
  result.reset(new translation_unit(ctxt.env(),
				    path,
				    address_size));

  uint64_t l = 0;
  die_unsigned_constant_attribute(die, DW_AT_language, l);
  result->set_language(dwarf_language_to_tu_language(l));

  ctxt.current_corpus()->add(result);
  ctxt.cur_transl_unit(result);
  ctxt.die_tu_map()[dwarf_dieoffset(die)] = result;

  Dwarf_Die child;
  if (dwarf_child(die, &child) != 0)
    return result;

  do
    build_ir_node_from_die(ctxt, &child,
			   die_is_public_decl(&child),
			   dwarf_dieoffset(&child));
  while (dwarf_siblingof(&child, &child) == 0);

  if (!ctxt.var_decls_to_re_add_to_tree().empty())
    for (list<var_decl_sptr>::const_iterator v =
	   ctxt.var_decls_to_re_add_to_tree().begin();
	 v != ctxt.var_decls_to_re_add_to_tree().end();
	 ++v)
      {
	  if (is_member_decl(*v))
	  continue;

	assert((*v)->get_scope());
	string demangled_name =
	  demangle_cplus_mangled_name((*v)->get_linkage_name());
	if (!demangled_name.empty())
	  {
	    std::list<string> fqn_comps;
	    fqn_to_components(demangled_name, fqn_comps);
	    string mem_name = fqn_comps.back();
	    fqn_comps.pop_back();
	    decl_base_sptr ty_decl;
	    string ty_name;
	    if (!fqn_comps.empty())
	      {
		ty_name = components_to_type_name(fqn_comps);
		ty_decl =
		  lookup_type_in_translation_unit(ty_name,
						  *ctxt.cur_transl_unit());
	      }
	    if (class_decl_sptr cl = dynamic_pointer_cast<class_decl>(ty_decl))
	      {
		// So we are seeing a member variable for which there
		// is a global variable definition DIE not having a
		// reference attribute pointing back to the member
		// variable declaration DIE.  Thus remove the global
		// variable definition from its current non-class
		// scope ...
		decl_base_sptr d;
		if (d = lookup_var_decl_in_scope(mem_name,cl))
		  // This is the data member with the same name in cl.
		  // We just need to flag it as static.
		  ;
		else
		  {
		    // In this case there is no data member with the
		    // same name in cl already.  Let's add it there then
		    // ...
		    remove_decl_from_scope(*v);
		    d = add_decl_to_scope(*v, cl);
		  }

		assert(dynamic_pointer_cast<var_decl>(d));
		// Let's flag the data member as static.
		set_member_is_static(d, true);
	      }
	    if (ty_decl)
	      assert(ty_decl->get_scope());
	  }
      }
  ctxt.var_decls_to_re_add_to_tree().clear();

  result->set_is_constructed(true);

  return result;
}

/// Build a abigail::namespace_decl out of a DW_TAG_namespace or
/// DW_TAG_module (for fortran) DIE.
///
/// Note that this function connects the DW_TAG_namespace to the IR
/// being currently created, reads the children of the DIE and
/// connects them to the IR as well.
///
/// @param ctxt the read context to use.
///
/// @param die the DIE to read from.  Must be either DW_TAG_namespace
/// or DW_TAG_module.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return the resulting @ref abigail::namespace_decl or NULL if it
/// couldn't be created.
static namespace_decl_sptr
build_namespace_decl_and_add_to_ir(read_context&	ctxt,
				   Dwarf_Die*		die,
				   size_t		where_offset)
{
  namespace_decl_sptr result;

  if (!die)
    return result;

  die_source source;
  assert(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);
  if (tag != DW_TAG_namespace && tag != DW_TAG_module)
    return result;

  scope_decl_sptr scope = get_scope_for_die(ctxt, die,
					    /*called_for_public_decl=*/false,
					    where_offset);

  string name, linkage_name;
  location loc;
  die_loc_and_name(ctxt, die, loc, name, linkage_name);

  result.reset(new namespace_decl(ctxt.env(), name, loc));
  add_decl_to_scope(result, scope.get());
  ctxt.associate_die_to_decl(dwarf_dieoffset(die), source, result);

  Dwarf_Die child;
  if (dwarf_child(die, &child) != 0)
    return result;

  ctxt.scope_stack().push(result.get());
  do
    build_ir_node_from_die(ctxt, &child,
			   /*called_from_public_decl=*/false,
			   where_offset);
  while (dwarf_siblingof(&child, &child) == 0);
  ctxt.scope_stack().pop();

  return result;
}

/// Build a @ref type_decl out of a DW_TAG_base_type DIE.
///
/// @param ctxt the read context to use.
///
/// @param die the DW_TAG_base_type to consider.
///
/// @return the resulting decl_base_sptr.
static type_decl_sptr
build_type_decl(read_context& ctxt, Dwarf_Die* die)
{
  type_decl_sptr result;

  if (!die)
    return result;
  assert(dwarf_tag(die) == DW_TAG_base_type);

  die_source source;
  assert(ctxt.get_die_source(die, source));

  uint64_t byte_size = 0, bit_size = 0;
  if (!die_unsigned_constant_attribute(die, DW_AT_byte_size, byte_size))
    if (!die_unsigned_constant_attribute(die, DW_AT_bit_size, bit_size))
      return result;

  if (bit_size == 0 && byte_size != 0)
    // Update the bit size.
    bit_size = byte_size * 8;

  string type_name, linkage_name;
  location loc;
  die_loc_and_name(ctxt, die, loc, type_name, linkage_name);

  if (byte_size == 0)
    {
      // The size of the type is zero, that must mean that we are
      // looking at the definition of the void type.
      if (type_name == "void")
	result = is_type_decl(build_ir_node_for_void_type(ctxt));
      else
	// A type of size zero that is not void? Hmmh, I am not sure
	// what that means.  Return nil for now.
	return result;
    }

  if (!result)
    result.reset(new type_decl(ctxt.env(), type_name, bit_size,
			       /*alignment=*/0, loc, linkage_name));
  ctxt.associate_die_to_type(dwarf_dieoffset(die), source, result);
  return result;
}

/// Build an enum_type_decl from a DW_TAG_enumeration_type DIE.
///
/// @param ctxt the read context to use.
///
/// @param die the DIE to read from.
///
/// @return the built enum_type_decl or NULL if it could not be built.
static enum_type_decl_sptr
build_enum_type(read_context& ctxt, Dwarf_Die* die)
{
  enum_type_decl_sptr result;
  if (!die)
    return result;

  die_source source;
  assert(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);
  if (tag != DW_TAG_enumeration_type)
    return result;

  string name, linkage_name;
  location loc;
  die_loc_and_name(ctxt, die, loc, name, linkage_name);

  bool enum_is_anonymous = false;
  // If the enum is anonymous, let's give it a name.
  if (name.empty())
    {
      name = "__anonymous_enum__";
      // But we remember that the type is anonymous.
      enum_is_anonymous = true;
    }

  uint64_t size = 0;
  if (die_unsigned_constant_attribute(die, DW_AT_byte_size, size))
    size *= 8;

  // for now we consider that underlying types of enums are all anonymous
  bool enum_underlying_type_is_anonymous= true;
  string underlying_type_name;
  if (enum_underlying_type_is_anonymous)
    {
      underlying_type_name = "unnamed-enum";
      enum_underlying_type_is_anonymous = true;
    }
  else
    underlying_type_name = string("enum-") + name;
  underlying_type_name += "-underlying-type";

  enum_type_decl::enumerators enms;
  Dwarf_Die child;
  if (dwarf_child(die, &child) == 0)
    {
      do
	{
	  if (dwarf_tag(&child) != DW_TAG_enumerator)
	    continue;

	  string n, m;
	  location l;
	  die_loc_and_name(ctxt, &child, loc, n, m);
	  uint64_t val = 0;
	  die_unsigned_constant_attribute(&child, DW_AT_const_value, val);
	  enms.push_back(enum_type_decl::enumerator(ctxt.env(), n, val));
	}
      while (dwarf_siblingof(&child, &child) == 0);
    }

  // DWARF up to version 4 (at least) doesn't seem to carry the
  // underlying type, so let's create an artificial one here, which
  // sole purpose is to be passed to the constructor of the
  // enum_type_decl type.
  type_decl_sptr t(new type_decl(ctxt.env(), underlying_type_name,
				 size, size, location()));
  t->set_is_anonymous(enum_underlying_type_is_anonymous);
  translation_unit_sptr tu = ctxt.cur_transl_unit();
  decl_base_sptr d =
    add_decl_to_scope(t, tu->get_global_scope().get());
  canonicalize(t);

  t = dynamic_pointer_cast<type_decl>(d);
  assert(t);
  result.reset(new enum_type_decl(name, loc, t, enms, linkage_name));
  result->set_is_anonymous(enum_is_anonymous);
  ctxt.associate_die_to_type(dwarf_dieoffset(die), source, result);
  return result;
}

/// Once a function_decl has been built and added to a class as a
/// member function, this function updates the information of the
/// function_decl concerning the properties of its relationship with
/// the member class.  That is, it updates properties like
/// virtualness, access, constness, cdtorness, etc ...
///
/// @param die the DIE of the function_decl that has been just built.
///
/// @param f the function_decl that has just been built from @p die.
///
/// @param klass the @ref class_or_union that @p f belongs to.
///
/// @param ctxt the context used to read the ELF/DWARF information.
static void
finish_member_function_reading(Dwarf_Die*		die,
			       function_decl_sptr	f,
			       class_or_union_sptr	klass,
			       read_context&		ctxt)
{
  assert(klass);

  method_decl_sptr m = dynamic_pointer_cast<method_decl>(f);
  assert(m);

  bool is_ctor = (f->get_name() == klass->get_name());
  bool is_dtor = (!f->get_name().empty()
		  && static_cast<string>(f->get_name())[0] == '~');
  bool is_virtual = die_is_virtual(die);
  uint64_t vindex = 0;
  if (is_virtual)
    die_virtual_function_index(die, vindex);
  access_specifier access = private_access;
  if (class_decl_sptr c = is_class_type(klass))
    if (c->is_struct())
      access = public_access;
  die_access_specifier(die, access);

  bool is_static = false;
  {
    // Let's see if the first parameter is a pointer to an instance of
    // the same class type as the current class and has a
    // DW_AT_artificial attribute flag set.  We are not looking at
    // DW_AT_object_pointer (for DWARF 3) because it wasn't being
    // emitted in GCC 4_4, which was already DWARF 3.
    function_decl::parameter_sptr first_parm;
    if (!f->get_parameters().empty())
      first_parm = f->get_parameters()[0];

    bool is_artificial =
      first_parm && first_parm->get_artificial();;
    pointer_type_def_sptr this_ptr_type;
    type_base_sptr other_klass;

    if (is_artificial)
      this_ptr_type = is_pointer_type(first_parm->get_type());
    if (this_ptr_type)
      other_klass = this_ptr_type->get_pointed_to_type();
    // Sometimes, other_klass can be qualified; e.g, volatile.  In
    // that case, let's get the unqualified version of other_klass.
    if (qualified_type_def_sptr q = is_qualified_type(other_klass))
      other_klass = q->get_underlying_type();

    if (other_klass
	&& (ir::get_type_declaration(other_klass)->get_qualified_name()
	    == klass->get_qualified_name()))
      ;
    else
      is_static = true;
  }
  set_member_access_specifier(m, access);
  set_member_function_is_virtual(m, is_virtual);
  set_member_function_vtable_offset(m, vindex);
  set_member_is_static(m, is_static);
  set_member_function_is_ctor(m, is_ctor);
  set_member_function_is_dtor(m, is_dtor);
  set_member_function_is_const(m, false);

  assert(is_member_function(m));

  if (is_virtual)
    if (class_decl_sptr c = is_class_type(klass))
      c->sort_virtual_mem_fns();

  if (is_virtual && !f->get_linkage_name().empty() && !f->get_symbol())
    {
      // This is a virtual member function which has a linkage name
      // but has no underlying symbol set.
      //
      // The underlying elf symbol to set to this function can show up
      // later in the DWARF input or it can be that, because of some
      // compiler optimization, the relation between this function and
      // its underlying elf symbol is simply not emitted in the DWARF.
      //
      // Let's thus schedule this function for a later fixup pass
      // (performed by
      // read_context::fixup_functions_with_no_symbols()) that will
      // set its underlying symbol.
      //
      // Note that if the underying symbol is encountered later in the
      // DWARF input, then the part of build_function_decl() that
      // updates the function to set its underlying symbol will
      // de-schedule this function wrt fixup pass.
      Dwarf_Off die_offset = dwarf_dieoffset(die);
      die_function_decl_map_type &fns_with_no_symbol =
	ctxt.die_function_decl_with_no_symbol_map();
      die_function_decl_map_type::const_iterator i =
	fns_with_no_symbol.find(die_offset);
      if (i == fns_with_no_symbol.end())
	fns_with_no_symbol[die_offset] = f;
    }
}

/// Build a an IR node for class type from a DW_TAG_structure_type or
/// DW_TAG_class_type and
///
/// @param ctxt the read context to consider.
///
/// @param die the DIE to read information from.  Must be either a
/// DW_TAG_structure_type or a DW_TAG_class_type.
///
/// @param is_struct wheter the class was declared as a struct.
///
/// @param scope a pointer to the scope_decl* under which this class
/// is to be added to.
///
/// @param klass if non-null, this is a klass to append the members
/// too.  Otherwise, this function just builds the class from scratch.
///
/// @param called_from_public_decl set to true if this class is being
/// called from a "Public declaration like vars or public symbols".
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return the resulting class_type.
static class_decl_sptr
build_class_type_and_add_to_ir(read_context&	ctxt,
			       Dwarf_Die*	die,
			       scope_decl*	scope,
			       bool		is_struct,
			       class_decl_sptr  klass,
			       bool		called_from_public_decl,
			       size_t		where_offset)
{
  class_decl_sptr result;
  if (!die)
    return result;

  die_source source;
  assert(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);

  if (tag != DW_TAG_class_type && tag != DW_TAG_structure_type)
    return result;

  {
    die_class_or_union_map_type::const_iterator i =
      ctxt.die_wip_classes_map(source).find(dwarf_dieoffset(die));
    if (i != ctxt.die_wip_classes_map(source).end())
      {
	class_decl_sptr class_type = is_class_type(i->second);
	assert(class_type);
	return class_type;
      }
  }

  string name, linkage_name;
  location loc;
  die_loc_and_name(ctxt, die, loc, name, linkage_name);

  bool is_anonymous = false;
  if (name.empty())
    {
      // So we are looking at an anonymous struct.  Let's
      // give it a name.
      name = "__anonymous_struct__";
      // But we remember that the type is anonymous.
      is_anonymous = true;
    }

  uint64_t size = 0;
  die_size_in_bits(die, size);

  Dwarf_Die child;
  bool has_child = (dwarf_child(die, &child) == 0);
  bool is_declaration_only = die_is_declaration_only(die);

  decl_base_sptr res;
  if (klass)
    {
      res = result = klass;
      result->set_location(loc);
    }
  else
    {
      result.reset(new class_decl(ctxt.env(), name, size,
				  /*alignment=*/0, is_struct, loc,
				  decl_base::VISIBILITY_DEFAULT));
      result->set_is_anonymous(is_anonymous);

      if (is_declaration_only)
	result->set_is_declaration_only(true);

      res = add_decl_to_scope(result, scope);
      result = dynamic_pointer_cast<class_decl>(res);
      assert(result);
    }

  if (size)
    {
      result->set_size_in_bits(size);
      result->set_is_declaration_only(false);
    }

  ctxt.associate_die_to_type(dwarf_dieoffset(die), source, result);
  ctxt.maybe_schedule_declaration_only_class_for_resolution(result);

  if (!has_child)
    // TODO: set the access specifier for the declaration-only class
    // here.
    return result;

  ctxt.die_wip_classes_map(source)[dwarf_dieoffset(die)] = result;

  scope_decl_sptr scop =
    dynamic_pointer_cast<scope_decl>(res);
  assert(scop);
  ctxt.scope_stack().push(scop.get());

  if (has_child)
    {
      do
	{
	  tag = dwarf_tag(&child);

	  // Handle base classes.
	  if (tag == DW_TAG_inheritance)
	    {
	      result->set_is_declaration_only(false);

	      Dwarf_Die type_die;
	      if (!die_die_attribute(&child, DW_AT_type, type_die))
		continue;

	      decl_base_sptr base_type = is_decl(
		build_ir_node_from_die(ctxt, &type_die,
				       called_from_public_decl,
				       where_offset));
	      class_decl_sptr b = is_compatible_with_class_type(base_type);
	      if (!b)
		continue;
	      if (lookup_type_in_scope(base_type->get_name(), result))
		continue;

	      access_specifier access =
		is_struct
		? public_access
		: private_access;

	      die_access_specifier(&child, access);

	      bool is_virt= die_is_virtual(&child);
	      int64_t offset = 0;
	      bool is_offset_present =
		die_member_offset(&child, offset);

	      class_decl::base_spec_sptr base(new class_decl::base_spec
					      (b, access,
					       is_offset_present
					       ? offset
					       : -1,
					       is_virt));
	      if (b->get_is_declaration_only())
		assert(ctxt.is_decl_only_class_scheduled_for_resolution(b));
	      if (result->find_base_class(b->get_qualified_name()))
		continue;
	      result->add_base_specifier(base);
	    }
	  // Handle data members.
	  else if (tag == DW_TAG_member
		   || tag == DW_TAG_variable)
	    {
	      Dwarf_Die type_die;
	      if (!die_die_attribute(&child, DW_AT_type, type_die))
		continue;

	      string n, m;
	      location loc;
	      die_loc_and_name(ctxt, &child, loc, n, m);
	       /// For now, we skip the hidden vtable pointer.
	       /// Currently, we're looking for a member starting with
	       /// "_vptr[^0-9a-zA-Z_]", which is what Clang and GCC
	       /// use as a name for the hidden vtable pointer.
	      if (n.substr(0, 5) == "_vptr"
		  && !std::isalnum(n.at(5))
		  && n.at(5) != '_')
                continue;

	      if (lookup_var_decl_in_scope(n, result))
		continue;

	      int64_t offset_in_bits = 0;
	      bool is_laid_out = false;
	      is_laid_out = die_member_offset(&child, offset_in_bits);
	      // For now, is_static == !is_laid_out.  When we have
	      // templates, we'll try to be more specific.  For now,
	      // this approximation should do OK.
	      bool is_static = !is_laid_out;

	      if (is_static && variable_is_suppressed(ctxt,
						      result.get(),
						      &child))
		continue;

	      decl_base_sptr ty = is_decl(
		build_ir_node_from_die(ctxt, &type_die,
				       called_from_public_decl,
				       where_offset));
	      type_base_sptr t = is_type(ty);
	      if (!t)
		continue;

	      if (!is_static)
		// We have a non-static data member.  So this class
		// cannot be a declaration-only class anymore, even if
		// some DWARF emitters might consider it otherwise.
		result->set_is_declaration_only(false);
	      access_specifier access =
		is_struct
		? public_access
		: private_access;

	      die_access_specifier(&child, access);

	      var_decl_sptr dm(new var_decl(n, t, loc, m));
	      result->add_data_member(dm, access, is_laid_out,
				      is_static, offset_in_bits);
	      assert(has_scope(dm));
	      ctxt.associate_die_to_decl(dwarf_dieoffset(&child), source, dm);
	    }
	  // Handle member functions;
	  else if (tag == DW_TAG_subprogram)
	    {
	      decl_base_sptr r =
		is_decl(build_ir_node_from_die(ctxt, &child,
					       result.get(),
					       called_from_public_decl,
					       where_offset));
	      if (!r)
		continue;

	      function_decl_sptr f = dynamic_pointer_cast<function_decl>(r);
	      assert(f);

	      finish_member_function_reading(&child, f, result, ctxt);

	      ctxt.associate_die_to_decl(dwarf_dieoffset(&child), source, f);
	    }
	  // Handle member types
	  else if (is_type_die(&child))
	    decl_base_sptr td =
	      is_decl(build_ir_node_from_die(ctxt, &child, result.get(),
					     called_from_public_decl,
					     where_offset));
	} while (dwarf_siblingof(&child, &child) == 0);
    }

  ctxt.scope_stack().pop();

  {
    die_class_or_union_map_type::const_iterator i =
      ctxt.die_wip_classes_map(source).find(dwarf_dieoffset(die));
    if (i != ctxt.die_wip_classes_map(source).end())
      {
	if (is_member_type(i->second))
	  set_member_access_specifier(res,
				      get_member_access_specifier(i->second));
	ctxt.die_wip_classes_map(source).erase(i);
      }
  }

  ctxt.maybe_schedule_declaration_only_class_for_resolution(result);
  return result;
}

/// Build an @ref union_decl from a DW_TAG_union_type DIE.
///
/// @param ctxt the read context to use.
///
/// @param die the DIE to read from.
///
/// @param scope the scope the resulting @ref union_decl belongs to.
///
/// @param union_type if this parameter is non-nil, then this function
/// updates the @ref union_decl that it points to, rather than
/// creating a new @ref union_decl.
///
/// @param called_from_public_decl is true if this function has been
/// initially called within the context of a public decl.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
static union_decl_sptr
build_union_type_and_add_to_ir(read_context&	ctxt,
			       Dwarf_Die*	die,
			       scope_decl*	scope,
			       union_decl_sptr  union_type,
			       bool		called_from_public_decl,
			       size_t		where_offset)
{
  union_decl_sptr result;
  if (!die)
    return result;

  unsigned tag = dwarf_tag(die);

  if (tag != DW_TAG_union_type)
    return result;

  die_source source;
  assert(ctxt.get_die_source(die, source));
  {
    die_class_or_union_map_type::const_iterator i =
      ctxt.die_wip_classes_map(source).find(dwarf_dieoffset(die));
    if (i != ctxt.die_wip_classes_map(source).end())
      {
	union_decl_sptr u = is_union_type(i->second);
	assert(u);
	return u;
      }
  }

  string name, linkage_name;
  location loc;
  die_loc_and_name(ctxt, die, loc, name, linkage_name);

  bool is_anonymous = false;
  if (name.empty())
    {
      // So we are looking at an anonymous union.  Let's give it a
      // name.
      name = "__anonymous_union__";
      // But we remember that the type is anonymous.
      is_anonymous = true;
    }

  uint64_t size = 0;
  die_size_in_bits(die, size);

  bool is_declaration_only = die_is_declaration_only(die);

  if (union_type)
    {
      result = union_type;
      result->set_location(loc);
    }
  else
    {
      result.reset(new union_decl(ctxt.env(), name, size,
				  loc, decl_base::VISIBILITY_DEFAULT));
      result->set_is_anonymous(is_anonymous);
      if (is_declaration_only)
	result->set_is_declaration_only(true);
      result = is_union_type(add_decl_to_scope(result, scope));
      assert(result);
    }

  if (size)
    {
      result->set_size_in_bits(size);
      result->set_is_declaration_only(false);
    }

  ctxt.associate_die_to_type(dwarf_dieoffset(die), source, result);
  // TODO: maybe schedule declaration-only union for result like we do
  // for classes:
  // ctxt.maybe_schedule_declaration_only_class_for_resolution(result);

  Dwarf_Die child;
  bool has_child = (dwarf_child(die, &child) == 0);
  if (!has_child)
    return result;

  ctxt.die_wip_classes_map(source)[dwarf_dieoffset(die)] = result;

  scope_decl_sptr scop =
    dynamic_pointer_cast<scope_decl>(result);
  assert(scop);
  ctxt.scope_stack().push(scop.get());

  if (has_child)
    {
      do
	{
	  tag = dwarf_tag(&child);
	  // Handle data members.
	  if (tag == DW_TAG_member || tag == DW_TAG_variable)
	    {
	      Dwarf_Die type_die;
	      if (!die_die_attribute(&child, DW_AT_type, type_die))
		continue;

	      string n, m;
	      location loc;
	      die_loc_and_name(ctxt, &child, loc, n, m);

	      if (lookup_var_decl_in_scope(n, result))
		continue;

	      ssize_t offset_in_bits = 0;
	      decl_base_sptr ty =
		is_decl(build_ir_node_from_die(ctxt, &type_die,
					       called_from_public_decl,
					       where_offset));
	      type_base_sptr t = is_type(ty);
	      if (!t)
		continue;

	      // We have a non-static data member.  So this class
	      // cannot be a declaration-only class anymore, even if
	      // some DWARF emitters might consider it otherwise.
	      result->set_is_declaration_only(false);
	      access_specifier access = private_access;

	      die_access_specifier(&child, access);

	      var_decl_sptr dm(new var_decl(n, t, loc, m));
	      result->add_data_member(dm, access, /*is_laid_out=*/true,
				      /*is_static=*/false,
				      offset_in_bits);
	      assert(has_scope(dm));
	      ctxt.associate_die_to_decl(dwarf_dieoffset(&child), source, dm);
	    }
	  // Handle member functions;
	  else if (tag == DW_TAG_subprogram)
	    {
	      decl_base_sptr r =
		is_decl(build_ir_node_from_die(ctxt, &child,
					       result.get(),
					       called_from_public_decl,
					       where_offset));
	      if (!r)
		continue;

	      function_decl_sptr f = dynamic_pointer_cast<function_decl>(r);
	      assert(f);

	      finish_member_function_reading(&child, f, result, ctxt);

	      ctxt.associate_die_to_decl(dwarf_dieoffset(&child), source, f);
	    }
	  // Handle member types
	  else if (is_type_die(&child))
	    decl_base_sptr td =
	      is_decl(build_ir_node_from_die(ctxt, &child, result.get(),
					     called_from_public_decl,
					     where_offset));
	} while (dwarf_siblingof(&child, &child) == 0);
    }

  ctxt.scope_stack().pop();

  {
    die_class_or_union_map_type::const_iterator i =
      ctxt.die_wip_classes_map(source).find(dwarf_dieoffset(die));
    if (i != ctxt.die_wip_classes_map(source).end())
      {
	if (is_member_type(i->second))
	  set_member_access_specifier(result,
				      get_member_access_specifier(i->second));
	ctxt.die_wip_classes_map(source).erase(i);
      }
  }

  return result;
}

/// build a qualified type from a DW_TAG_const_type,
/// DW_TAG_volatile_type or DW_TAG_restrict_type DIE.
///
/// @param ctxt the read context to consider.
///
/// @param die the input DIE to read from.
///
/// @param called_from_public_decl true if this function was called
/// from a context where either a public function or a public variable
/// is being built.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return the resulting qualified_type_def.
static qualified_type_def_sptr
build_qualified_type(read_context&	ctxt,
		     Dwarf_Die*	die,
		     bool		called_from_public_decl,
		     size_t		where_offset)
{
  qualified_type_def_sptr result;
  if (!die)
    return result;

  die_source source;
  assert(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);

  if (tag != DW_TAG_const_type
      && tag != DW_TAG_volatile_type
      && tag != DW_TAG_restrict_type)
    return result;

  Dwarf_Die underlying_type_die;
  decl_base_sptr utype_decl;
  if (!die_die_attribute(die, DW_AT_type, underlying_type_die))
    // So, if no DW_AT_type is present, then this means (if we are
    // looking at a debug info emitted by GCC) that we are looking
    // at a qualified void type.
    utype_decl = build_ir_node_for_void_type(ctxt);

  if (!utype_decl)
    utype_decl = is_decl(build_ir_node_from_die(ctxt, &underlying_type_die,
						called_from_public_decl,
						where_offset));
  if (!utype_decl)
    return result;

  // The call to build_ir_node_from_die() could have triggered the
  // creation of the type for this DIE.  In that case, just return it.
  if (type_base_sptr t = ctxt.lookup_type_from_die_offset(dwarf_dieoffset(die),
							  source))
    {
      result = is_qualified_type(t);
      assert(result);
      return result;
    }

  type_base_sptr utype = is_type(utype_decl);
  assert(utype);

  if (tag == DW_TAG_const_type)
    result.reset(new qualified_type_def(utype,
					qualified_type_def::CV_CONST,
					location()));
  else if (tag == DW_TAG_volatile_type)
    result.reset(new qualified_type_def(utype,
					qualified_type_def::CV_VOLATILE,
					location()));
  else if (tag == DW_TAG_restrict_type)
    result.reset(new qualified_type_def(utype,
					qualified_type_def::CV_RESTRICT,
					location()));

  ctxt.associate_die_to_type(dwarf_dieoffset(die), source, result);

  return result;
}

/// Strip qualification from a qualified type, when it makes sense.
///
/// DWARF constructs "const reference".  This is redundant because a
/// reference is always const.  The issue is these redundant types then
/// leak into the IR and make for bad diagnostics.
///
/// This function thus strips the const qualifier from the type in
/// that case.  It might contain code to strip other cases like this
/// in the future.
///
/// @param t the type to strip const qualification from.
///
/// @return the stripped type or just return @p t.
static decl_base_sptr
maybe_strip_qualification(const qualified_type_def_sptr t)
{
  if (!t)
    return t;

  decl_base_sptr result = t;
  type_base_sptr u = t->get_underlying_type();
  environment* env = t->get_environment();

  if (t->get_cv_quals() & qualified_type_def::CV_CONST
      && (is_reference_type(u)))
    // Let's strip only the const qualifier.  To do that, the "const"
    // qualified is turned into a no-op "none" qualified.
    result.reset(new qualified_type_def
		 (u, t->get_cv_quals() & ~qualified_type_def::CV_CONST,
		  t->get_location()));
  else if (t->get_cv_quals() & qualified_type_def::CV_CONST
	   && env->is_void_type(u))
    // So this type is a "const void".  Let's strip the "const"
    // qualifier out and make this just be "void", so that a "const
    // void" type and a "void" type compare equal after going through
    // this function.
    result = is_decl(u);

  return result;
}

/// Build a pointer type from a DW_TAG_pointer_type DIE.
///
/// @param ctxt the read context to consider.
///
/// @param die the DIE to read information from.
///
/// @param called_from_public_decl true if this function was called
/// from a context where either a public function or a public variable
/// is being built.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return the resulting pointer to pointer_type_def.
static pointer_type_def_sptr
build_pointer_type_def(read_context&	ctxt,
		       Dwarf_Die*	die,
		       bool		called_from_public_decl,
		       size_t		where_offset)
{
  pointer_type_def_sptr result;

  if (!die)
    return result;

  die_source source;
  assert(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);
  if (tag != DW_TAG_pointer_type)
    return result;

  type_or_decl_base_sptr utype_decl;
  Dwarf_Die underlying_type_die;
  bool has_underlying_type_die = false;
  if (!die_die_attribute(die, DW_AT_type, underlying_type_die))
    // If the DW_AT_type attribute is missing, that means we are
    // looking at a pointer to "void".
    utype_decl = build_ir_node_for_void_type(ctxt);
  else
    has_underlying_type_die = true;

  if (!utype_decl && has_underlying_type_die)
    utype_decl = build_ir_node_from_die(ctxt, &underlying_type_die,
					called_from_public_decl,
					where_offset);
  if (!utype_decl)
    return result;

  // The call to build_ir_node_from_die() could have triggered the
  // creation of the type for this DIE.  In that case, just return it.
  if (type_base_sptr t = ctxt.lookup_type_from_die_offset(dwarf_dieoffset(die),
							  source))
    {
      result = is_pointer_type(t);
      assert(result);
      return result;
    }

  type_base_sptr utype = is_type(utype_decl);
  assert(utype);

  // if the DIE for the pointer type doesn't have a byte_size
  // attribute then we assume the size of the pointer is the address
  // size of the current translation unit.
  uint64_t size = ctxt.cur_transl_unit()->get_address_size();
  if (die_unsigned_constant_attribute(die, DW_AT_byte_size, size))
    // The size as expressed by DW_AT_byte_size is in byte, so let's
    // convert it to bits.
    size *= 8;

  // And the size of the pointer must be the same as the address size
  // of the current translation unit.
  assert((size_t) ctxt.cur_transl_unit()->get_address_size() == size);

  result.reset(new pointer_type_def(utype, size, /*alignment=*/0, location()));
  assert(result->get_pointed_to_type());
  ctxt.associate_die_to_type(dwarf_dieoffset(die), source, result);
  return result;
}

/// Build a reference type from either a DW_TAG_reference_type or
/// DW_TAG_rvalue_reference_type DIE.
///
/// @param ctxt the read context to consider.
///
/// @param die the DIE to read from.
///
/// @param called_from_public_decl true if this function was called
/// from a context where either a public function or a public variable
/// is being built.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return a pointer to the resulting reference_type_def.
static reference_type_def_sptr
build_reference_type(read_context&	ctxt,
		     Dwarf_Die*	die,
		     bool		called_from_public_decl,
		     size_t		where_offset)
{
  reference_type_def_sptr result;

  if (!die)
    return result;

  die_source source;
  assert(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);
  if (tag != DW_TAG_reference_type
      && tag != DW_TAG_rvalue_reference_type)
    return result;

  Dwarf_Die underlying_type_die;
  if (!die_die_attribute(die, DW_AT_type, underlying_type_die))
    return result;

  type_or_decl_base_sptr utype_decl =
    build_ir_node_from_die(ctxt, &underlying_type_die,
			   called_from_public_decl,
			   where_offset);
  if (!utype_decl)
    return result;

  // The call to build_ir_node_from_die() could have triggered the
  // creation of the type for this DIE.  In that case, just return it.
  if (type_base_sptr t = ctxt.lookup_type_from_die_offset(dwarf_dieoffset(die),
							  source))
    {
      result = is_reference_type(t);
      assert(result);
      return result;
    }

  type_base_sptr utype = is_type(utype_decl);
  assert(utype);

  // if the DIE for the reference type doesn't have a byte_size
  // attribute then we assume the size of the reference is the address
  // size of the current translation unit.
  uint64_t size = ctxt.cur_transl_unit()->get_address_size();
  if (die_unsigned_constant_attribute(die, DW_AT_byte_size, size))
    size *= 8;

  // And the size of the pointer must be the same as the address size
  // of the current translation unit.
  assert((size_t) ctxt.cur_transl_unit()->get_address_size() == size);

  bool is_lvalue = (tag == DW_TAG_reference_type) ? true : false;

  result.reset(new reference_type_def(utype, is_lvalue, size,
				      /*alignment=*/0,
				      location()));
  ctxt.associate_die_to_type(dwarf_dieoffset(die), source, result);
  return result;
}

/// Build a subroutine type from a DW_TAG_subroutine_type DIE.
///
/// @param ctxt the read context to consider.
///
/// @param die the DIE to read from.
///
/// @param is_method points to a class or union declaration iff we're
/// building the type for a method.  This is the enclosing class or
/// union of the method.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positioned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return a pointer to the resulting function_type_sptr.
static function_type_sptr
build_function_type(read_context&	ctxt,
		    Dwarf_Die*		die,
		    class_or_union_sptr is_method,
		    size_t		where_offset)
{
  function_type_sptr result;

  if (!die)
    return result;

  assert(dwarf_tag(die) == DW_TAG_subroutine_type
	 || dwarf_tag(die) == DW_TAG_subprogram);

  die_source source;
  assert(ctxt.get_die_source(die, source));

  decl_base_sptr type_decl;

  translation_unit_sptr tu = ctxt.cur_transl_unit();
  assert(tu);

  // The call to build_ir_node_from_die() could have triggered the
  // creation of the type for this DIE.  In that case, just return it.
  if (type_base_sptr t = ctxt.lookup_type_from_die_offset(dwarf_dieoffset(die),
							  source))
    {
      result = is_function_type(t);
      assert(result);
      return result;
    }

  // Let's create the type early and record it as being for the DIE
  // 'die'.  This way, when building the sub-type triggers the
  // creation of a type matching the same 'die', then we'll reuse this
  // one.
  result.reset(is_method
	       ? new method_type(is_method,
				 tu->get_address_size(),
				 /*alignment=*/0)
	       : new function_type(ctxt.env(), tu->get_address_size(),
				   /*alignment=*/0));
  tu->bind_function_type_life_time(result);
  ctxt.associate_die_to_type(dwarf_dieoffset(die),
			     source, result);
  ctxt.die_wip_function_types_map(source)[dwarf_dieoffset(die)] =
    result;

  decl_base_sptr return_type_decl;
  Dwarf_Die ret_type_die;
  if (die_die_attribute(die, DW_AT_type, ret_type_die))
    return_type_decl =
      is_decl(build_ir_node_from_die(ctxt, &ret_type_die,
				     /*called_from_public_decl=*/true,
				     where_offset));
  if (!return_type_decl)
    return_type_decl = build_ir_node_for_void_type(ctxt);
  result->set_return_type(is_type(return_type_decl));

  Dwarf_Die child;
  function_decl::parameters function_parms;

  if (dwarf_child(die, &child) == 0)
    do
      {
	int child_tag = dwarf_tag(&child);
	if (child_tag == DW_TAG_formal_parameter)
	  {
	    // This is a "normal" function parameter.
	    string name, linkage_name;
	    location loc;
	    die_loc_and_name(ctxt, &child, loc, name, linkage_name);
	    if (!tools_utils::string_is_ascii_identifier(name))
	      // Sometimes, bogus compiler emit names that are
	      // non-ascii garbage.  Let's just ditch that for now.
	      name.clear();
	    bool is_artificial = die_is_artificial(&child);
	    decl_base_sptr parm_type_decl;
	    Dwarf_Die parm_type_die;
	    if (die_die_attribute(&child, DW_AT_type, parm_type_die))
	      parm_type_decl =
		is_decl(build_ir_node_from_die(ctxt, &parm_type_die,
					       /*called_from_public_decl=*/true,
					       where_offset));
	    if (!parm_type_decl)
	      continue;
	    function_decl::parameter_sptr p
	      (new function_decl::parameter(is_type(parm_type_decl), name, loc,
					    /*variadic_marker=*/false,
					    is_artificial));
	    function_parms.push_back(p);
	  }
	else if (child_tag == DW_TAG_unspecified_parameters)
	  {
	    // This is a variadic function parameter.
	    bool is_artificial = die_is_artificial(&child);
	    ir::environment* env = ctxt.env();
	    assert(env);
	    type_base_sptr parm_type = env->get_variadic_parameter_type();
	    function_decl::parameter_sptr p
	      (new function_decl::parameter(parm_type,
					    /*name=*/"",
					    location(),
					    /*variadic_marker=*/true,
					    is_artificial));
	    function_parms.push_back(p);
	    // After a DW_TAG_unspecified_parameters tag, we shouldn't
	    // keep reading for parameters.  The
	    // unspecified_parameters TAG should be the last parameter
	    // that we record. For instance, if there are multiple
	    // DW_TAG_unspecified_parameters DIEs then we should care
	    // only for the first one.
	    break;
	  }
      }
  while (dwarf_siblingof(&child, &child) == 0);

  result->set_parameters(function_parms);

  {
    die_function_type_map_type::const_iterator i =
      ctxt.die_wip_function_types_map(source).
      find(dwarf_dieoffset(die));
    if (i != ctxt.die_wip_function_types_map(source).end())
      ctxt.die_wip_function_types_map(source).erase(i);
  }

  return result;
}

/// Build a array type from a DW_TAG_array_type DIE.
///
/// @param ctxt the read context to consider.
///
/// @param die the DIE to read from.
///
/// @param called_from_public_decl true if this function was called
/// from a context where either a public function or a public variable
/// is being built.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positioned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return a pointer to the resulting array_type_def.
static array_type_def_sptr
build_array_type(read_context&	ctxt,
		 Dwarf_Die*	die,
		 bool		called_from_public_decl,
		 size_t	where_offset)
{
  array_type_def_sptr result;

  if (!die)
    return result;

  die_source source;
  assert(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);
  if (tag != DW_TAG_array_type)
    return result;

  decl_base_sptr type_decl;
  Dwarf_Die type_die;

  if (die_die_attribute(die, DW_AT_type, type_die))
    type_decl = is_decl(build_ir_node_from_die(ctxt, &type_die,
					       called_from_public_decl,
					       where_offset));
  if (!type_decl)
    return result;

  // The call to build_ir_node_from_die() could have triggered the
  // creation of the type for this DIE.  In that case, just return it.
  if (type_base_sptr t = ctxt.lookup_type_from_die_offset(dwarf_dieoffset(die),
							  source))
    {
      result = is_array_type(t);
      assert(result);
      return result;
    }

  type_base_sptr type = is_type(type_decl);
  assert(type);

  Dwarf_Die child;
  array_type_def::subranges_type subranges;
  translation_unit::language language =
    ctxt.cur_transl_unit()->get_language();
  uint64_t upper_bound = 0;
  uint64_t lower_bound = get_default_array_lower_bound(language);
  uint64_t count = 0;

  if (dwarf_child(die, &child) == 0)
    {
      do
	{
	  int child_tag = dwarf_tag(&child);
	  if (child_tag == DW_TAG_subrange_type)
	    {
	      // The DWARF 4 specifications says, in [5.11 Subrange
	      // Type Entries]:
	      //
	      //     The subrange entry may have the attributes
	      //     DW_AT_lower_bound and DW_AT_upper_bound to
	      //     specify, respectively, the lower and upper bound
	      //     values of the subrange.
	      //
	      // So let's look for DW_AT_lower_bound first.
	      die_unsigned_constant_attribute(&child,
					      DW_AT_lower_bound,
					      lower_bound);

	      // Then, DW_AT_upper_bound.
	      if (!die_unsigned_constant_attribute(&child,
						   DW_AT_upper_bound,
						   upper_bound))
		{
		  // The DWARF 4 spec says, in [5.11 Subrange Type
		  // Entries]:
		  //
		  //   The DW_AT_upper_bound attribute may be replaced
		  //   by a DW_AT_count attribute, whose value
		  //   describes the number of elements in the
		  //   subrange rather than the value of the last
		  //   element."
		  //
		  // So, as DW_AT_upper_bound is not present in this
		  // case, let's see if there is a DW_AT_count.
		  if (!die_unsigned_constant_attribute(&child,
						       DW_AT_count,
						       count))
		    // We have no information about the number of
		    // elements of the array.  Let's bail out then.
		    return result;

		  // We can deduce the upper_bound from the
		  // lower_bound and the number of elements of the
		  // array:
		  if (size_t u = lower_bound + count)
		    upper_bound = u - 1;
		}

	      array_type_def::subrange_sptr s
		(new array_type_def::subrange_type(lower_bound,
						   upper_bound,
						   location()));
	      subranges.push_back(s);
	    }
	}
      while (dwarf_siblingof(&child, &child) == 0);
    }

  result.reset(new array_type_def(type, subranges, location()));

  return result;
}

/// Create a typedef_decl from a DW_TAG_typedef DIE.
///
/// @param ctxt the read context to consider.
///
/// @param die the DIE to read from.
///
/// @param called_from_public_decl true if this function was called
/// from a context where either a public function or a public variable
/// is being built.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return the newly created typedef_decl.
static typedef_decl_sptr
build_typedef_type(read_context&	ctxt,
		   Dwarf_Die*		die,
		   bool		called_from_public_decl,
		   size_t		where_offset)
{
  typedef_decl_sptr result;

  if (!die)
    return result;

  die_source source;
  assert(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);
  if (tag != DW_TAG_typedef)
    return result;

  Dwarf_Die underlying_type_die;
  if (!die_die_attribute(die, DW_AT_type, underlying_type_die))
    return result;

  decl_base_sptr utype_decl =
    is_decl(build_ir_node_from_die(ctxt, &underlying_type_die,
				   called_from_public_decl,
				   where_offset));
  if (!utype_decl)
    return result;

  // The call to build_ir_node_from_die() could have triggered the
  // creation of the type for this DIE.  In that case, just return it.
  if (type_base_sptr t = ctxt.lookup_type_from_die_offset(dwarf_dieoffset(die),
							  source))
    {
      result = is_typedef(t);
      assert(result);
      return result;
    }

  type_base_sptr utype = is_type(utype_decl);
  assert(utype);

  string name, linkage_name;
  location loc;
  die_loc_and_name(ctxt, die, loc, name, linkage_name);

  result.reset(new typedef_decl(name, utype, loc, linkage_name));
  ctxt.associate_die_to_type(dwarf_dieoffset(die), source, result);
  return result;
}

/// Build a @ref var_decl out of a DW_TAG_variable DIE iff the
/// variable denoted by the DIE is not suppressed by a suppression
/// specification associated to the current read context.
///
/// @param ctxt the read context to use.
///
/// @param die the DIE representing the variable we are looking at.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @param result if this is set to an existing var_decl, this means
/// that the function will append the new properties it sees on @p die
/// to that exising var_decl.  Otherwise, if this parameter is NULL, a
/// new var_decl is going to be allocated and returned.
///
/// @return a pointer to the newly created var_decl.  If the var_decl
/// could not be built, this function returns NULL.
static var_decl_sptr
build_var_decl_if_not_suppressed(read_context&	ctxt,
				 scope_decl	*scope,
				 Dwarf_Die	*die,
				 size_t	where_offset,
				 var_decl_sptr	result = var_decl_sptr())
{
  var_decl_sptr var;
  if (!variable_is_suppressed(ctxt, scope, die))
    var = build_var_decl(ctxt, die, where_offset, result);
  return var;
}

/// Build a @ref var_decl out of a DW_TAG_variable DIE.
///
/// @param ctxt the read context to use.
///
/// @param die the DIE representing the variable we are looking at.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @param result if this is set to an existing var_decl, this means
/// that the function will append the new properties it sees on @p die
/// to that exising var_decl.  Otherwise, if this parameter is NULL, a
/// new var_decl is going to be allocated and returned.
///
/// @return a pointer to the newly created var_decl.  If the var_decl
/// could not be built, this function returns NULL.
static var_decl_sptr
build_var_decl(read_context&	ctxt,
	       Dwarf_Die	*die,
	       size_t		where_offset,
	       var_decl_sptr	result)
{
  if (!die)
    return result;
  assert(dwarf_tag(die) == DW_TAG_variable);

  if (!die_is_public_decl(die))
    return result;

  die_source source;
  assert(ctxt.get_die_source(die, source));

  type_base_sptr type;
  Dwarf_Die type_die;
  if (die_die_attribute(die, DW_AT_type, type_die))
    {
      decl_base_sptr ty =
	is_decl(build_ir_node_from_die(ctxt, &type_die,
				       /*called_from_public_decl=*/true,
				       where_offset));
      if (!ty)
	return result;
      type = is_type(ty);
      assert(type);
    }

  if (!type)
    return result;

  string name, linkage_name;
  location loc;
  die_loc_and_name(ctxt, die, loc, name, linkage_name);

  if (!result)
    result.reset(new var_decl(name, type, loc, linkage_name));
  else
    {
      // We were called to append properties that might have been
      // missing from the first version of the variable.  And usually
      // that missing property is the mangled name.
      if (!linkage_name.empty())
	result->set_linkage_name(linkage_name);
    }

  // Check if a variable symbol with this name is exported by the elf
  // binary.
  if (!result->get_symbol())
    {
      Dwarf_Addr var_addr;
      if (ctxt.get_variable_address(die, var_addr))
	{
	  if (elf_symbol_sptr sym =
	      ctxt.lookup_elf_var_symbol_from_address(var_addr))
	    if (sym->is_variable() && sym->is_public())
	      {
		result->set_symbol(sym);
		// If the linkage name is not set or is wrong, set it to
		// the name of the underlying symbol.
		string linkage_name = result->get_linkage_name();
		if (linkage_name.empty()
		    || !sym->get_alias_from_name(linkage_name))
		  result->set_linkage_name(sym->get_name());
		result->set_is_in_public_symbol_table(true);
	      }
	}
    }

  return result;
}

/// Test if a given function denoted by its DIE and its scope is
/// suppressed by any of the suppression specifications associated to
/// a given context of ELF/DWARF reading.
///
/// @param ctxt the ELF/DWARF reading content of interest.
///
/// @param scope of the scope of the function.
///
/// @param function_die the DIE representing the function.
///
/// @return true iff @p function_die is suppressed by at least one
/// suppression specification attached to the @p ctxt.
static bool
function_is_suppressed(const read_context& ctxt,
		       const scope_decl* scope,
		       Dwarf_Die *function_die)
{
  if (function_die == 0
      || dwarf_tag(function_die) != DW_TAG_subprogram)
    return false;

  string fname = die_string_attribute(function_die, DW_AT_name);
  string flinkage_name = die_linkage_name(function_die);
  string qualified_name = build_qualified_name(scope, fname);

  return suppr::function_is_suppressed(ctxt, qualified_name,
				       flinkage_name,
				       /*require_drop_property=*/true);
}

/// Test if a given variable denoted by its DIE and its scope is
/// suppressed by any of the suppression specifications associated to
/// a given context of ELF/DWARF reading.
///
/// @param ctxt the ELF/DWARF reading content of interest.
///
/// @param scope of the scope of the variable.
///
/// @param variable_die the DIE representing the variable.
///
/// @return true iff @p variable_die is suppressed by at least one
/// suppression specification attached to the @p ctxt.
static bool
variable_is_suppressed(const read_context& ctxt,
		       const scope_decl* scope,
		       Dwarf_Die *variable_die)
{
  if (variable_die == 0
      || (dwarf_tag(variable_die) != DW_TAG_variable
	  && dwarf_tag(variable_die) != DW_TAG_member))
    return false;

  string name = die_string_attribute(variable_die, DW_AT_name);
  string linkage_name = die_linkage_name(variable_die);
  string qualified_name = build_qualified_name(scope, name);

  return suppr::variable_is_suppressed(ctxt, qualified_name,
				       linkage_name,
				       /*require_drop_property=*/true);
}

/// Test if a type (designated by a given DIE) in a given scope is
/// suppressed by the suppression specifications that are associated
/// to a given read context.
///
/// @param ctxt the read context to consider.
///
/// @param scope of the scope of the type DIE to consider.
///
/// @param type_die the DIE that designates the type to consider.
///
/// @return true iff the type designated by the DIE @p type_die, in
/// the scope @p scope is suppressed by at the suppression
/// specifications associated to the current read context.
static bool
type_is_suppressed(const read_context& ctxt,
		   const scope_decl* scope,
		   Dwarf_Die *type_die)
{
  if (type_die == 0
      || (dwarf_tag(type_die) != DW_TAG_enumeration_type
	  && dwarf_tag(type_die) != DW_TAG_class_type
	  && dwarf_tag(type_die) != DW_TAG_structure_type
	  && dwarf_tag(type_die) != DW_TAG_union_type))
    return false;

  string type_name, linkage_name;
  location type_location;
  die_loc_and_name(ctxt, type_die, type_location, type_name, linkage_name);
  string qualified_name = build_qualified_name(scope, type_name);

  return suppr::type_is_suppressed(ctxt, qualified_name,
				   type_location,
				   /*require_drop_property=*/true);
}

/// Build a @ref function_decl our of a DW_TAG_subprogram DIE.
///
/// @param ctxt the read context to use
///
/// @param die the DW_TAG_subprogram DIE to read from.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @param called_for_public_decl this is set to true if the function
/// was called for a public (function) decl.
static function_decl_sptr
build_function_decl(read_context&	ctxt,
		    Dwarf_Die*		die,
		    size_t		where_offset,
		    function_decl_sptr	fn)
{
  function_decl_sptr result = fn;
  if (!die)
    return result;
  assert(dwarf_tag(die) == DW_TAG_subprogram);

  die_source source;
  assert(ctxt.get_die_source(die, source));

 if (!die_is_public_decl(die))
   return result;

 translation_unit_sptr tu = ctxt.cur_transl_unit();
  assert(tu);

  string fname, flinkage_name;
  location floc;
  die_loc_and_name(ctxt, die, floc, fname, flinkage_name);

  size_t is_inline = die_is_declared_inline(die);
  class_or_union_sptr is_method =
    is_class_or_union_type(get_scope_for_die(ctxt, die, true, where_offset));

  if (result)
    {
      // Add the properties that might have been missing from the
      // first declaration of the function.  For now, it usually is
      // the mangled name that goes missing in the first declarations.
      //
      // Also note that if 'fn' has just been cloned, the current
      // linkage name (of the current DIE) might be different from the
      // linkage name of 'fn'.  In that case, update the linkage name
      // of 'fn' too.
      if (!flinkage_name.empty()
	  && result->get_linkage_name() != flinkage_name)
	result->set_linkage_name(flinkage_name);
    }
  else
    {
      function_type_sptr fn_type(build_function_type(ctxt, die, is_method,
						     where_offset));
      maybe_canonicalize_type(dwarf_dieoffset(die), source, ctxt);

      result.reset(is_method
		   ? new method_decl(fname, fn_type,
				     is_inline, floc,
				     flinkage_name)
		   : new function_decl(fname, fn_type,
				       is_inline, floc,
				       flinkage_name));
    }

  // Check if a function symbol with this name is exported by the elf
  // binary.
  bool symbol_updated = false;
  Dwarf_Addr fn_addr;
  if (ctxt.get_function_address(die, fn_addr))
    {
      if (elf_symbol_sptr sym = ctxt.lookup_elf_fn_symbol_from_address(fn_addr))
	if (sym->is_function() && sym->is_public())
	  {
	    result->set_symbol(sym);
	    symbol_updated = true;
	    // If the linkage name is not set or is wrong, set it to
	    // the name of the underlying symbol.
	    string linkage_name = result->get_linkage_name();
	    if (linkage_name.empty() || !sym->get_alias_from_name(linkage_name))
	      result->set_linkage_name(sym->get_name());
	    result->set_is_in_public_symbol_table(true);
	  }
    }

  Dwarf_Off die_offset = dwarf_dieoffset(die);
  ctxt.associate_die_to_type(die_offset, source, result->get_type());

  if (symbol_updated
      && fn
      && is_member_function(fn)
      && get_member_function_is_virtual(fn)
      && !result->get_linkage_name().empty())
    // This function is a virtual member function which has its
    // linkage name *and* and has its underlying symbol correctly set.
    // It thus doesn't need any fixup related to elf symbol.  So
    // remove it from the set of virtual member functions with linkage
    // names and no elf symbol that need to be fixed up.
    ctxt.die_function_decl_with_no_symbol_map().erase(die_offset);
  return result;
}

/// Read all @ref abigail::translation_unit possible from the debug info
/// accessible through a DWARF Front End Library handle, and stuff
/// them into a libabigail ABI Corpus.
///
/// @param ctxt the read context.
///
/// @return a pointer to the resulting corpus, or NULL if the corpus
/// could not be constructed.
static corpus_sptr
read_debug_info_into_corpus(read_context& ctxt)
{
  ctxt.clear_per_corpus_data();

  if (!ctxt.current_corpus())
    {
      corpus_sptr corp (new corpus(ctxt.env(), ctxt.elf_path()));
      ctxt.current_corpus(corp);
      if (!ctxt.env())
	ctxt.env(corp->get_environment());
    }

  // First set some mundane properties of the corpus gathered from
  // ELF.
  ctxt.current_corpus()->set_path(ctxt.elf_path());
  ctxt.current_corpus()->set_origin(corpus::DWARF_ORIGIN);
  ctxt.current_corpus()->set_soname(ctxt.dt_soname());
  ctxt.current_corpus()->set_needed(ctxt.dt_needed());
  ctxt.current_corpus()->set_architecture_name(ctxt.elf_architecture());

  // Set symbols information to the corpus.
  ctxt.current_corpus()->set_fun_symbol_map(ctxt.fun_syms_sptr());
  ctxt.current_corpus()->set_undefined_fun_symbol_map
    (ctxt.undefined_fun_syms_sptr());
  ctxt.current_corpus()->set_var_symbol_map(ctxt.var_syms_sptr());
  ctxt.current_corpus()->set_undefined_var_symbol_map
    (ctxt.undefined_var_syms_sptr());

  // Get out now if no debug info is found.
  if (!ctxt.dwarf())
    return ctxt.current_corpus();

  uint8_t address_size = 0;
  size_t header_size = 0;

  // Set the set of exported declaration that are defined.
  ctxt.exported_decls_builder
    (ctxt.current_corpus()->get_exported_decls_builder().get());

  // Walk all the DIEs of the debug info to build a DIE -> parent map
  // useful for get_die_parent() to work.
  if (ctxt.do_log())
    cerr << "building die -> parent maps ...";

  ctxt.build_die_parent_maps();

  if (ctxt.do_log())
    cerr << " DONE@" << ctxt.current_corpus()->get_path() << "\n";

  ctxt.env()->canonicalization_is_done(false);

  if (ctxt.do_log())
    cerr << "building the libabigail internal representation ...";
  // And now walk all the DIEs again to build the libabigail IR.
  Dwarf_Half dwarf_version = 0;
  for (Dwarf_Off offset = 0, next_offset = 0;
       (dwarf_next_unit(ctxt.dwarf(), offset, &next_offset, &header_size,
			&dwarf_version, NULL, &address_size, NULL,
			NULL, NULL) == 0);
       offset = next_offset)
    {
      Dwarf_Off die_offset = offset + header_size;
      Dwarf_Die unit;
      if (!dwarf_offdie(ctxt.dwarf(), die_offset, &unit)
	  || dwarf_tag(&unit) != DW_TAG_compile_unit)
	continue;

      ctxt.dwarf_version(dwarf_version);

      address_size *= 8;

      // Build a translation_unit IR node from cu; note that cu must
      // be a DW_TAG_compile_unit die.
      translation_unit_sptr ir_node =
	build_translation_unit_and_add_to_ir(ctxt, &unit, address_size);
      assert(ir_node);
    }
  if (ctxt.do_log())
    cerr << " DONE@" << ctxt.current_corpus()->get_path() << "\n";

  if (ctxt.do_log())
    cerr << "resolving declaration only classes ...";
  ctxt.resolve_declaration_only_classes();
  if (ctxt.do_log())
    cerr << " DONE@" << ctxt.current_corpus()->get_path() <<"\n";

  if (ctxt.do_log())
    cerr << "fixing up functions with linkage name but "
	 << "no advertised underlying symbols ....";
  ctxt.fixup_functions_with_no_symbols();
  if (ctxt.do_log())
    cerr << " DONE@" << ctxt.current_corpus()->get_path() <<"\n";

  /// Now, look at the types that needs to be canonicalized after the
  /// translation has been constructed (which is just now) and
  /// canonicalize them.
  ///
  /// These types need to be constructed at the end of the translation
  /// unit reading phase because some types are modified by some DIEs
  /// even after the principal DIE describing the type has been read;
  /// this happens for clones of virtual destructors (for instance) or
  /// even for some static data members.  We need to do that for types
  /// are in the alternate debug info section and for types that in
  /// the main debug info section.
  if (ctxt.do_log())
    cerr << "perform late type canonicalizing ...\n";
  ctxt.perform_late_type_canonicalizing();
  if (ctxt.do_log())
    cerr << "late type canonicalizing DONE@"
	 << ctxt.current_corpus()->get_path()
	 << "\n";

  ctxt.env()->canonicalization_is_done(true);

  if (ctxt.do_log())
    cerr << "sort functions and variables ...";
  ctxt.current_corpus()->sort_functions();
  ctxt.current_corpus()->sort_variables();
  if (ctxt.do_log())
    cerr << " DONE@" << ctxt.current_corpus()->get_path() <<" \n";

  return ctxt.current_corpus();
}

/// Canonicalize a type if it's suitable for early canonicalizing, or,
/// if it's not, schedule it for late canonicalization, after the
/// debug info of the current translation unit has been fully read.
///
/// A (composite) type is deemed suitable for early canonicalizing iff
/// all of its sub-types are canonicalized themselve.  Non composite
/// types are always deemed suitable for early canonicalization.
///
/// @param die_offset the offset of the type DIE to consider for
/// canonicalization.  Note that this DIE must have been associated
/// with its type using the function
/// read_context::associate_die_to_type() prior to calling this
/// function.
///
/// @param die_source where the DIE represented by @p die_offset comes
/// from.
///
/// @param ctxt the @ref read_context to use.
static void
maybe_canonicalize_type(Dwarf_Off	die_offset,
			die_source	source,
			read_context&	ctxt)
{
  type_base_sptr t = ctxt.lookup_type_from_die_offset(die_offset, source);
  assert(t);

  type_base_sptr peeled_type = peel_typedef_pointer_or_reference_type(t);
  if (is_class_type(peeled_type) || is_union_type(peeled_type))
    // We delay canonicalization of classes/unions or typedef,
    // pointers, references and array to classes/unions.  This is
    // because the (underlying) class might not be finished yet and we
    // might not be able to able detect it here (thinking about
    // classes that are work-in-progress, or classes that might be
    // later amended by some DWARF construct).  So we err on the safe
    // side.
    ctxt.schedule_type_for_late_canonicalization(die_offset, source);
  else if ((is_function_type(t)
	    && ctxt.is_wip_function_type_die_offset(die_offset, source))
	   || type_has_non_canonicalized_subtype(t))
    ctxt.schedule_type_for_late_canonicalization(die_offset, source);
  else
    canonicalize(t);
}

/// If a given decl is a member type declaration, set its access
/// specifier from the DIE that represents it.
///
/// @param member_type_declaration the member type declaration to
/// consider.
static void
maybe_set_member_type_access_specifier(decl_base_sptr member_type_declaration,
				       Dwarf_Die* die)
{
  if (is_type(member_type_declaration)
      && is_member_decl(member_type_declaration))
    {
      class_or_union* scope =
	is_class_or_union_type(member_type_declaration->get_scope());
      assert(scope);

      access_specifier access = private_access;
      if (class_decl* cl = is_class_type(scope))
	if (cl->is_struct())
	  access = public_access;

      die_access_specifier(die, access);
      set_member_access_specifier(member_type_declaration, access);
    }
}

/// Build an IR node from a given DIE and add the node to the current
/// IR being build and held in the read_context.  Doing that is called
/// "emitting an IR node for the DIE".
///
/// @param ctxt the read context.
///
/// @param die the DIE to consider.
///
/// @param scope the scope under which the resulting IR node has to be
/// added.
///
/// @param called_from_public_decl set to yes if this function is
/// called from the functions used to build a public decl (functions
/// and variables).  In that case, this function accepts building IR
/// nodes representing types.  Otherwise, this function only creates
/// IR nodes representing public decls (functions and variables).
/// This is done to avoid emitting IR nodes for types that are not
/// referenced by public functions or variables.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return the resulting IR node.
static type_or_decl_base_sptr
build_ir_node_from_die(read_context&	ctxt,
		       Dwarf_Die*	die,
		       scope_decl*	scope,
		       bool		called_from_public_decl,
		       size_t		where_offset)
{
  type_or_decl_base_sptr result;

  if (!die || !scope)
    return result;

  int tag = dwarf_tag(die);

  if (!called_from_public_decl)
    {
      if (ctxt.load_all_types() && is_type_die(die))
	/* We were instructed to load debug info for all types,
	   included those that are not reachable from a public
	   declaration.  So load the debug info for this type.  */;
      else if (tag != DW_TAG_subprogram
	       && tag != DW_TAG_variable
	       && tag != DW_TAG_member
	       && tag != DW_TAG_namespace)
	return result;
    }

  die_source source_of_die;
  assert(ctxt.get_die_source(die, source_of_die));

  if (result = ctxt.lookup_decl_from_die_offset(dwarf_dieoffset(die),
						source_of_die))
    return result;

  switch (tag)
    {
      // Type DIEs we support.
    case DW_TAG_base_type:
      if (type_decl_sptr t = build_type_decl(ctxt, die))
	  {
	    result =
	      add_decl_to_scope(t, ctxt.cur_transl_unit()->get_global_scope());
	    canonicalize(t);
	  }
      break;

    case DW_TAG_typedef:
      {
	typedef_decl_sptr t = build_typedef_type(ctxt, die,
						 called_from_public_decl,
						 where_offset);
	result = add_decl_to_scope(t, scope);
	if (result)
	  {
	    maybe_set_member_type_access_specifier(is_decl(result), die);
	    maybe_canonicalize_type(dwarf_dieoffset(die), source_of_die, ctxt);
	  }
      }
      break;

    case DW_TAG_pointer_type:
      {
	pointer_type_def_sptr p =
	  build_pointer_type_def(ctxt, die,
				 called_from_public_decl,
				 where_offset);
	if (p)
	  {
	    result =
	      add_decl_to_scope(p, ctxt.cur_transl_unit()->get_global_scope());
	    assert(result->get_translation_unit());
	    maybe_canonicalize_type(dwarf_dieoffset(die), source_of_die, ctxt);
	  }
      }
      break;

    case DW_TAG_reference_type:
    case DW_TAG_rvalue_reference_type:
      {
	reference_type_def_sptr r =
	  build_reference_type(ctxt, die,
			       called_from_public_decl,
			       where_offset);
	if (r)
	  {
	    result =
	      add_decl_to_scope(r, ctxt.cur_transl_unit()->get_global_scope());
	    ctxt.associate_die_to_type(dwarf_dieoffset(die), source_of_die, r);
	    maybe_canonicalize_type(dwarf_dieoffset(die), source_of_die, ctxt);
	  }
      }
      break;

    case DW_TAG_const_type:
    case DW_TAG_volatile_type:
    case DW_TAG_restrict_type:
      {
	qualified_type_def_sptr q =
	  build_qualified_type(ctxt, die,
			       called_from_public_decl,
			       where_offset);
	if (q)
	  {
	    // Strip some potentially redundant type qualifiers from
	    // the qualified type we just built.
	    decl_base_sptr d = maybe_strip_qualification(q);
	    type_base_sptr ty = is_type(d);
	    // Associate the die to type ty again because 'ty'might be
	    // different from 'q', because 'ty' is 'q' possibly
	    // stripped from some redundant type qualifier.
	    ctxt.associate_die_to_type(dwarf_dieoffset(die), source_of_die, ty);
	    result =
	      add_decl_to_scope(d, ctxt.cur_transl_unit()->get_global_scope());
	    maybe_canonicalize_type(dwarf_dieoffset(die), source_of_die, ctxt);
	  }
      }
      break;

    case DW_TAG_enumeration_type:
      {
	if (!type_is_suppressed(ctxt, scope, die))
	  {
	    enum_type_decl_sptr e = build_enum_type(ctxt, die);
	    result = add_decl_to_scope(e, scope);
	    if (result)
	      {
		maybe_set_member_type_access_specifier(is_decl(result), die);
		maybe_canonicalize_type(dwarf_dieoffset(die),
					source_of_die, ctxt);
	      }
	  }
      }
      break;

    case DW_TAG_class_type:
    case DW_TAG_structure_type:
      {
	if (!type_is_suppressed(ctxt, scope, die))
	  {
	    Dwarf_Die spec_die;
	    scope_decl_sptr scop;
	    class_decl_sptr klass;
	    if (die_die_attribute(die, DW_AT_specification, spec_die))
	      {
		scope_decl_sptr skope =
		  get_scope_for_die(ctxt, &spec_die,
				    called_from_public_decl,
				    where_offset);
		assert(skope);
		decl_base_sptr cl =
		  is_decl(build_ir_node_from_die(ctxt, &spec_die,
						 skope.get(),
						 called_from_public_decl,
						 where_offset));
		assert(cl);
		klass = dynamic_pointer_cast<class_decl>(cl);
		assert(klass);

		klass =
		  build_class_type_and_add_to_ir(ctxt, die,
						 skope.get(),
						 tag == DW_TAG_structure_type,
						 klass,
						 called_from_public_decl,
						 where_offset);
	      }
	    else
	      klass =
		build_class_type_and_add_to_ir(ctxt, die, scope,
					       tag == DW_TAG_structure_type,
					       class_decl_sptr(),
					       called_from_public_decl,
					       where_offset);
	    result = klass;
	    if (klass)
	      {
		maybe_set_member_type_access_specifier(klass, die);
		maybe_canonicalize_type(dwarf_dieoffset(die),
					source_of_die, ctxt);
	      }
	  }
      }
      break;
    case DW_TAG_union_type:
	if (!type_is_suppressed(ctxt, scope, die))
	  {
	    union_decl_sptr union_type =
	      build_union_type_and_add_to_ir(ctxt, die, scope,
					     union_decl_sptr(),
					     called_from_public_decl,
					     where_offset);
	    if (union_type)
	      {
		maybe_set_member_type_access_specifier(union_type, die);
		maybe_canonicalize_type(dwarf_dieoffset(die),
					source_of_die, ctxt);
	      }
	    result = union_type;
	  }
      break;
    case DW_TAG_string_type:
      break;
    case DW_TAG_subroutine_type:
      {
	function_type_sptr f =
	  build_function_type(ctxt, die,
			      /*called_from_public_decl,*/
			      class_decl_sptr(),
			      where_offset);
	if (f)
	  {
	    result = f;
	    maybe_canonicalize_type(dwarf_dieoffset(die), source_of_die, ctxt);
	  }
      }
      break;
    case DW_TAG_array_type:
      {
	array_type_def_sptr a = build_array_type(ctxt,
						 die,
						 called_from_public_decl,
						 where_offset);
	if (a)
	  {
	    result =
	      add_decl_to_scope(a, ctxt.cur_transl_unit()->get_global_scope());
	    ctxt.associate_die_to_type(dwarf_dieoffset(die), source_of_die, a);
	    maybe_canonicalize_type(dwarf_dieoffset(die), source_of_die, ctxt);
	  }
      }
      break;
    case DW_TAG_packed_type:
      break;
    case DW_TAG_set_type:
      break;
    case DW_TAG_file_type:
      break;
    case DW_TAG_ptr_to_member_type:
      break;
    case DW_TAG_subrange_type:
      /* we shouldn't get here as this part is handled by build_array_type */
      ABG_ASSERT_NOT_REACHED;
    case DW_TAG_thrown_type:
      break;
    case DW_TAG_interface_type:
      break;
    case DW_TAG_unspecified_type:
      break;
    case DW_TAG_shared_type:
      break;

      // Other declarations we intend to support someday, maybe.

    case DW_TAG_compile_unit:
      // We shouldn't reach this point b/c this should be handled by
      // build_translation_unit.
      ABG_ASSERT_NOT_REACHED;

    case DW_TAG_namespace:
    case DW_TAG_module:
      result = build_namespace_decl_and_add_to_ir(ctxt, die, where_offset);
      break;

    case DW_TAG_variable:
      {
	Dwarf_Die spec_die;
	bool var_is_cloned = false;
	if (die_die_attribute(die, DW_AT_specification, spec_die,false)
	    || (var_is_cloned = die_die_attribute(die, DW_AT_abstract_origin,
						  spec_die, false)))
	  {
	    scope_decl_sptr spec_scope = get_scope_for_die(ctxt, &spec_die,
							   called_from_public_decl,
							   where_offset);
	    if (spec_scope)
	      {
		decl_base_sptr d =
		  is_decl(build_ir_node_from_die(ctxt, &spec_die,
						 spec_scope.get(),
						 called_from_public_decl,
						 where_offset));
		if (d)
		  {
		    var_decl_sptr m =
		      dynamic_pointer_cast<var_decl>(d);
		    if (var_is_cloned)
		      m = m->clone();
		    m = build_var_decl(ctxt, die, where_offset, m);
		    if (is_data_member(m))
		      {
			set_member_is_static(m, true);
			ctxt.associate_die_to_decl(dwarf_dieoffset(die),
						   source_of_die, m);
		      }
		    else
		      {
			assert(has_scope(m));
			ctxt.var_decls_to_re_add_to_tree().push_back(m);
		      }
		    assert(m->get_scope());
		    ctxt.maybe_add_var_to_exported_decls(m.get());
		    return m;
		  }
	      }
	  }
	else if (var_decl_sptr v =
		 build_var_decl_if_not_suppressed(ctxt, scope, die,
						  where_offset))
	  {
	    result = add_decl_to_scope(v, scope);
	    assert(is_decl(result)->get_scope());
	    v = dynamic_pointer_cast<var_decl>(result);
	    assert(v);
	    assert(v->get_scope());
	    ctxt.var_decls_to_re_add_to_tree().push_back(v);
	    ctxt.maybe_add_var_to_exported_decls(v.get());
	  }
      }
      break;

    case DW_TAG_subprogram:
      {
	  Dwarf_Die spec_die;
	  Dwarf_Die abstract_origin_die;
	  Dwarf_Die *interface_die = 0, *origin_die = 0;
	  scope_decl_sptr interface_scope;
	  if (die_is_artificial(die))
	    break;

	  function_decl_sptr fn;
	  bool has_spec = die_die_attribute(die, DW_AT_specification,
					    spec_die, true);
	  bool has_abstract_origin =
	    die_die_attribute(die, DW_AT_abstract_origin,
			      abstract_origin_die, true);
	  if (has_spec || has_abstract_origin)
	    {
	      interface_die =
		has_spec
		? &spec_die
		: &abstract_origin_die;
	      origin_die =
		has_abstract_origin
		? &abstract_origin_die
		: &spec_die;

	      string linkage_name = die_linkage_name(die);
	      string spec_linkage_name = die_linkage_name(interface_die);

	      interface_scope = get_scope_for_die(ctxt, interface_die,
						  called_from_public_decl,
						  where_offset);
	      if (interface_scope)
		{
		  decl_base_sptr d =
		    is_decl(build_ir_node_from_die(ctxt,
						   origin_die,
						   interface_scope.get(),
						   called_from_public_decl,
						   where_offset));
		  if (d)
		    {
		      fn = dynamic_pointer_cast<function_decl>(d);
		      if (has_abstract_origin
			  && (linkage_name != spec_linkage_name))
			// The current DIE has 'd' as abstract orign,
			// and has a linkage name that is different
			// from from the linkage name of 'd'.  That
			// means, the current DIE represents a clone
			// of 'd'.
			fn = fn->clone();
		      ctxt.associate_die_to_decl(dwarf_dieoffset(die),
						 source_of_die, fn);
		    }
		}
	    }
	ctxt.scope_stack().push(scope);

	scope_decl* logical_scope =
	  interface_scope
	  ? interface_scope.get()
	  : scope;

	if (function_is_suppressed(ctxt, logical_scope, die))
	  // The function has been suppressed by a suppression
	  // specification, Do not try to build an internal
	  // representation for it.
	  ;
	else
	  result = build_function_decl(ctxt, die, where_offset, fn);

	if (result && !fn)
	  result = add_decl_to_scope(is_decl(result), scope);

	fn = dynamic_pointer_cast<function_decl>(result);
	if (fn && is_member_function(fn))
	  {
	    class_decl_sptr klass(static_cast<class_decl*>(scope),
				  sptr_utils::noop_deleter());
	    assert(klass);
	    finish_member_function_reading(die, fn, klass, ctxt);
	  }

	if (fn)
	  {
	    ctxt.maybe_add_fn_to_exported_decls(fn.get());
	    maybe_canonicalize_type(dwarf_dieoffset(die),
				    source_of_die, ctxt);
	  }

	ctxt.scope_stack().pop();
      }
      break;

    case DW_TAG_formal_parameter:
      // We should not read this case as it should have been dealt
      // with by build_function_decl above.
      ABG_ASSERT_NOT_REACHED;

    case DW_TAG_constant:
      break;
    case DW_TAG_enumerator:
      break;

    case DW_TAG_partial_unit:
    case DW_TAG_imported_unit:
      // For now, the DIEs under these are read lazily when they are
      // referenced by a public decl DIE that is under a
      // DW_TAG_compile_unit, so we shouldn't get here.
      ABG_ASSERT_NOT_REACHED;

      // Other declaration we don't really intend to support yet.
    case DW_TAG_dwarf_procedure:
    case DW_TAG_imported_declaration:
    case DW_TAG_entry_point:
    case DW_TAG_label:
    case DW_TAG_lexical_block:
    case DW_TAG_member:
    case DW_TAG_unspecified_parameters:
    case DW_TAG_variant:
    case DW_TAG_common_block:
    case DW_TAG_common_inclusion:
    case DW_TAG_inheritance:
    case DW_TAG_inlined_subroutine:
    case DW_TAG_with_stmt:
    case DW_TAG_access_declaration:
    case DW_TAG_catch_block:
    case DW_TAG_friend:
    case DW_TAG_namelist:
    case DW_TAG_namelist_item:
    case DW_TAG_template_type_parameter:
    case DW_TAG_template_value_parameter:
    case DW_TAG_try_block:
    case DW_TAG_variant_part:
    case DW_TAG_imported_module:
    case DW_TAG_condition:
    case DW_TAG_type_unit:
    case DW_TAG_template_alias:
    case DW_TAG_lo_user:
    case DW_TAG_MIPS_loop:
    case DW_TAG_format_label:
    case DW_TAG_function_template:
    case DW_TAG_class_template:
    case DW_TAG_GNU_BINCL:
    case DW_TAG_GNU_EINCL:
    case DW_TAG_GNU_template_template_param:
    case DW_TAG_GNU_template_parameter_pack:
    case DW_TAG_GNU_formal_parameter_pack:
    case DW_TAG_GNU_call_site:
    case DW_TAG_GNU_call_site_parameter:
    case DW_TAG_hi_user:
    default:
      break;
    }

  if (result && tag != DW_TAG_subroutine_type)
    ctxt.associate_die_to_decl(dwarf_dieoffset(die),
			       source_of_die,
			       is_decl(result));

  return result;
}

///  Build the IR node for a void type.
///
///  @param ctxt the read context to use.
///
///  @return the void type node.
static decl_base_sptr
build_ir_node_for_void_type(read_context& ctxt)
{
  ir::environment* env = ctxt.env();
  assert(env);
  type_base_sptr t = env->get_void_type();
  decl_base_sptr type_declaration = get_type_declaration(t);
  if (!has_scope(type_declaration))
    add_decl_to_scope(type_declaration,
		      ctxt.cur_transl_unit()->get_global_scope());
  canonicalize(t);
  return type_declaration;
}

/// Build an IR node from a given DIE and add the node to the current
/// IR being build and held in the read_context.  Doing that is called
/// "emitting an IR node for the DIE".
///
/// @param ctxt the read context.
///
/// @param die the DIE to consider.
///
/// @param called_from_public_decl set to yes if this function is
/// called from the functions used to build a public decl (functions
/// and variables).  In that case, this function accepts building IR
/// nodes representing types.  Otherwise, this function only creates
/// IR nodes representing public decls (functions and variables).
/// This is done to avoid emitting IR nodes for types that are not
/// referenced by public functions or variables.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return the resulting IR node.
static type_or_decl_base_sptr
build_ir_node_from_die(read_context&	ctxt,
		       Dwarf_Die*	die,
		       bool		called_from_public_decl,
		       size_t		where_offset)
{
  if (!die)
    return decl_base_sptr();

  scope_decl_sptr scope = get_scope_for_die(ctxt, die,
					    called_from_public_decl,
					    where_offset);
  return build_ir_node_from_die(ctxt, die, scope.get(),
				called_from_public_decl,
				where_offset);
}

status
operator|(status l, status r)
{
  return static_cast<status>(static_cast<unsigned>(l)
			     | static_cast<unsigned>(r));
}

status
operator&(status l, status r)
{
  return static_cast<status>(static_cast<unsigned>(l)
			     & static_cast<unsigned>(r));
}

status&
operator|=(status& l, status r)
{
  l = l | r;
  return l;
}

status&
operator&=(status& l, status r)
{
  l = l & r;
  return l;
}

/// Create a dwarf_reader::read_context.
///
/// @param elf_path the path to the elf file the context is to be used for.
///
/// @param a pointer to the path to the root directory under which the
/// debug info is to be found for @p elf_path.  Leave this to NULL if
/// the debug info is not in a split file.
///
/// @param environment the environment used by the current context.
/// This environment contains resources needed by the reader and by
/// the types and declarations that are to be created later.  Note
/// that ABI artifacts that are to be compared all need to be created
/// within the same environment.
///
/// Please also note that the life time of this environment object
/// must be greater than the life time of the resulting @ref
/// read_context the context uses resources that are allocated in the
/// environment.
///
/// @param load_all_types if set to false only the types that are
/// reachable from publicly exported declarations (of functions and
/// variables) are read.  If set to true then all types found in the
/// debug information are loaded.
///
/// @return a smart pointer to the resulting dwarf_reader::read_context.
read_context_sptr
create_read_context(const std::string&		elf_path,
		    char**			debug_info_root_path,
		    ir::environment*		environment,
		    bool			load_all_types)
{
  // Create a DWARF Front End Library handle to be used by functions
  // of that library.
  read_context_sptr result(new read_context(elf_path));
  result->create_default_dwfl(debug_info_root_path);
  result->load_all_types(load_all_types);
  result->env(environment);
  return result;
}

/// Add suppressions specifications to the set of suppressions to be
/// used during the construction of the ABI internal representation
/// (the ABI corpus) from ELF and DWARF.
///
/// During the construction of the ABI corpus, ABI artifacts that
/// match the a given suppression specification are dropped on the
/// floor; that is, they are discarded and won't be part of the final
/// ABI corpus.  This is a way to reduce the amount of data held by
/// the final ABI corpus.
///
/// Note that the suppression specifications provided to this function
/// are only considered during the construction of the ABI corpus.
/// For instance, they are not taken into account during e.g
/// comparisons of two ABI corpora that might happen later.  If you
/// want to apply suppression specificatins to the comparison (or
/// reporting) of ABI corpora please refer to the documentation of the
/// @ref diff_context type to learn how to set suppressions that are
/// to be used in that context.
///
/// @param ctxt the context that is going to be used by functions that
/// read ELF and DWARF information to construct and ABI corpus.
///
/// @param supprs the suppression specifications to be applied during
/// the construction of the ABI corpus.
void
add_read_context_suppressions(read_context& ctxt,
			      const suppr::suppressions_type& supprs)
{
  for (suppr::suppressions_type::const_iterator i = supprs.begin();
       i != supprs.end();
       ++i)
    if ((*i)->get_drops_artifact_from_ir())
      ctxt.get_suppressions().push_back(*i);
}

/// Read all @ref abigail::translation_unit possible from the debug info
/// accessible from an elf file, stuff them into a libabigail ABI
/// Corpus and return it.
///
/// @param ctxt the context to use for reading the elf file.
///
/// @param resulting_corp a pointer to the resulting abigail::corpus.
///
/// @return the resulting status.
corpus_sptr
read_corpus_from_elf(read_context& ctxt, status& status)
{
  status = STATUS_UNKNOWN;

  // Load debug info from the elf path.
  if (!ctxt.load_debug_info())
    status |= STATUS_DEBUG_INFO_NOT_FOUND;

  ctxt.load_elf_properties();

  // First read the symbols for publicly defined decls
  if (!ctxt.load_symbol_maps())
    status |= STATUS_NO_SYMBOLS_FOUND;

  if (status & STATUS_NO_SYMBOLS_FOUND)
    return corpus_sptr();

  // Read the variable and function descriptions from the debug info
  // we have, through the dwfl handle.
  corpus_sptr corp = read_debug_info_into_corpus(ctxt);

  status |= STATUS_OK;

  return corp;
}

/// Read all @ref abigail::translation_unit possible from the debug info
/// accessible from an elf file, stuff them into a libabigail ABI
/// Corpus and return it.
///
/// @param elf_path the path to the elf file.
///
/// @param debug_info_root_path a pointer to the root path under which
/// to look for the debug info of the elf files that are later handled
/// by the Dwfl.  This for cases where the debug info is split into a
/// different file from the binary we want to inspect.  On Red Hat
/// compatible systems, this root path is usually /usr/lib/debug by
/// default.  If this argument is set to NULL, then "./debug" and
/// /usr/lib/debug will be searched for sub-directories containing the
/// debug info file.
///
/// @param environment the environment used by the current context.
/// This environment contains resources needed by the reader and by
/// the types and declarations that are to be created later.  Note
/// that ABI artifacts that are to be compared all need to be created
/// within the same environment.  Also, the lifetime of the
/// environment must be greater than the lifetime of the resulting
/// corpus because the corpus uses resources that are allocated in the
/// environment.
///
/// @param load_all_types if set to false only the types that are
/// reachable from publicly exported declarations (of functions and
/// variables) are read.  If set to true then all types found in the
/// debug information are loaded.
///
/// @param resulting_corp a pointer to the resulting abigail::corpus.
///
/// @return the resulting status.
corpus_sptr
read_corpus_from_elf(const std::string& elf_path,
		     char**		debug_info_root_path,
		     ir::environment*	environment,
		     bool		load_all_types,
		     status&		status)
{
  read_context_sptr c = create_read_context(elf_path,
					    debug_info_root_path,
					    environment,
					    load_all_types);
  read_context& ctxt = *c;
  return read_corpus_from_elf(ctxt, status);
}

/// Look into the symbol tables of a given elf file and see if we find
/// a given symbol.
///
/// @param env the environment we are operating from.
///
/// @param elf_path the path to the elf file to consider.
///
/// @param symbol_name the name of the symbol to look for.
///
/// @param demangle if true, try to demangle the symbol name found in
/// the symbol table.
///
/// @param syms the vector of symbols found with the name @p symbol_name.
///
/// @return true iff the symbol was found among the publicly exported
/// symbols of the ELF file.
bool
lookup_symbol_from_elf(const environment*		env,
		       const string&			elf_path,
		       const string&			symbol_name,
		       bool				demangle,
		       vector<elf_symbol_sptr>&	syms)

{
  if (elf_version(EV_CURRENT) == EV_NONE)
    return false;

  int fd = open(elf_path.c_str(), O_RDONLY);
  if (fd < 0)
    return false;

  struct stat s;
  if (fstat(fd, &s))
    return false;

  Elf* elf = elf_begin(fd, ELF_C_READ, 0);
  if (elf == 0)
    return false;

  bool value = lookup_symbol_from_elf(env, elf, symbol_name,
				      demangle, syms);
  elf_end(elf);
  close(fd);

  return value;
}

/// Look into the symbol tables of an elf file to see if a public
/// function of a given name is found.
///
/// @param env the environment we are operating from.
///
/// @param elf_path the path to the elf file to consider.
///
/// @param symbol_name the name of the function to look for.
///
/// @param syms the vector of public function symbols found with the
/// name @p symname.
///
/// @return true iff a function with symbol name @p symbol_name is
/// found.
bool
lookup_public_function_symbol_from_elf(const environment*		env,
				       const string&			path,
				       const string&			symname,
				       vector<elf_symbol_sptr>&	syms)
{
  if (elf_version(EV_CURRENT) == EV_NONE)
    return false;

  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return false;

  struct stat s;
  if (fstat(fd, &s))
      return false;

  Elf* elf = elf_begin(fd, ELF_C_READ, 0);
  if (elf == 0)
    return false;

  bool value = lookup_public_function_symbol_from_elf(env, elf, symname, syms);
  elf_end(elf);
  close(fd);

  return value;
}

/// Check if the underlying elf file has an alternate debug info file
/// associated to it.
///
/// Note that "alternate debug info sections" is a GNU extension as
/// of DWARF4 and is described at
/// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1.
///
/// @param ctxt the read_context to use to handle the underlying elf file.
///
/// @param has_alt_di out parameter.  This is set to true upon
/// succesful completion of the function iff an alternate debug info
/// file was found, false otherwise.  Note thas this parameter is set
/// only if the function returns STATUS_OK.
///
/// @param alt_debug_info_path if the function returned STATUS_OK and
/// if @p has been set to true, then this parameter contains the path
/// to the alternate debug info file found.
///
/// return STATUS_OK upon successful completion, false otherwise.
status
has_alt_debug_info(read_context&	ctxt,
		   bool&		has_alt_di,
		   string&		alt_debug_info_path)
{
  // Load debug info from the elf path.
  if (!ctxt.load_debug_info())
    return STATUS_DEBUG_INFO_NOT_FOUND;

  if (ctxt.alt_dwarf())
    {
      has_alt_di = true;
      alt_debug_info_path = ctxt.alt_debug_info_path();
    }
  else
    has_alt_di = false;

  return STATUS_OK;
}

/// Check if a given elf file has an alternate debug info file
/// associated to it.
///
/// Note that "alternate debug info sections" is a GNU extension as
/// of DWARF4 and is described at
/// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1.
///
/// @param elf_path the path to the elf file to consider.
///
/// @param a pointer to the root directory under which the split debug info
/// file associated to elf_path is to be found.  This has to be NULL
/// if the debug info file is not in a split file.
///
/// @param has_alt_di out parameter.  This is set to true upon
/// succesful completion of the function iff an alternate debug info
/// file was found, false otherwise.  Note thas this parameter is set
/// only if the function returns STATUS_OK.
///
/// @param alt_debug_info_path if the function returned STATUS_OK and
/// if @p has been set to true, then this parameter contains the path
/// to the alternate debug info file found.
///
/// return STATUS_OK upon successful completion, false otherwise.
status
has_alt_debug_info(const string&	elf_path,
		   char**		debug_info_root_path,
		   bool&		has_alt_di,
		   string&		alt_debug_info_path)
{
  read_context_sptr c = create_read_context(elf_path, debug_info_root_path, 0);
  read_context& ctxt = *c;

  // Load debug info from the elf path.
  if (!ctxt.load_debug_info())
    return STATUS_DEBUG_INFO_NOT_FOUND;

  if (ctxt.alt_dwarf())
    {
      has_alt_di = true;
      alt_debug_info_path = ctxt.alt_debug_info_path();
    }
  else
    has_alt_di = false;

  return STATUS_OK;
}

/// Fetch the SONAME ELF property from an ELF binary file.
///
/// @param elf The handler of an ELF binary file.
///
/// @param soname out parameter. Set to the SONAME property of the
/// binary file, if it present in the ELF file.
///
/// return false if an error occured while looking for the SONAME
/// property in the binary, true otherwise.
bool
get_soname_of_elf_file(const string& path, string &soname)
{

  int fd = open(path.c_str(), O_RDONLY);
  if (fd == -1)
    return false;

  elf_version (EV_CURRENT);
  Elf* elf = elf_begin (fd, ELF_C_READ_MMAP, NULL);

  GElf_Ehdr ehdr_mem;
  GElf_Ehdr* ehdr = gelf_getehdr (elf, &ehdr_mem);
  if (ehdr == NULL)
    return false;

  for (int i = 0; i < ehdr->e_phnum; ++i)
    {
      GElf_Phdr phdr_mem;
      GElf_Phdr* phdr = gelf_getphdr (elf, i, &phdr_mem);

      if (phdr != NULL && phdr->p_type == PT_DYNAMIC)
        {
          Elf_Scn* scn = gelf_offscn (elf, phdr->p_offset);
          GElf_Shdr shdr_mem;
          GElf_Shdr* shdr = gelf_getshdr (scn, &shdr_mem);
          int maxcnt = (shdr != NULL
                        ? shdr->sh_size / shdr->sh_entsize : INT_MAX);
          assert (shdr == NULL || shdr->sh_type == SHT_DYNAMIC);
          Elf_Data* data = elf_getdata (scn, NULL);
          if (data == NULL)
            break;

          for (int cnt = 0; cnt < maxcnt; ++cnt)
            {
              GElf_Dyn dynmem;
              GElf_Dyn* dyn = gelf_getdyn (data, cnt, &dynmem);
              if (dyn == NULL)
                continue;

              if (dyn->d_tag == DT_NULL)
                break;

              if (dyn->d_tag != DT_SONAME)
                continue;

              soname = elf_strptr (elf, shdr->sh_link, dyn->d_un.d_val);
              break;
            }
          break;
        }
    }

  elf_end(elf);
  close(fd);

  return true;
}

/// Get the type of a given elf type.
///
/// @param path the absolute path to the ELF file to analyzed.
///
/// @param type the kind of the ELF file designated by @p path.
///
/// @param out parameter.  Is set to the type of ELF file of @p path.
/// This parameter is set iff the function returns true.
///
/// @return true iff the file could be opened and analyzed.
bool
get_type_of_elf_file(const string& path, elf_type& type)
{
  int fd = open(path.c_str(), O_RDONLY);
  if (fd == -1)
    return false;

  elf_version (EV_CURRENT);
  Elf *elf = elf_begin (fd, ELF_C_READ_MMAP, NULL);
  type = elf_file_type(elf);
  elf_end(elf);
  close(fd);

  return true;
}

}// end namespace dwarf_reader

}// end namespace abigail
