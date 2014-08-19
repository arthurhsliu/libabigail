// -*- Mode: C++ -*-
//
// Copyright (C) 2013-2014 Red Hat, Inc.
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

// Author: Dodji Seketeli

/// @file
///
/// This program tests that libabigail can handle alternate debug info
/// files as specified by http://www.dwarfstd.org/ShowIssue.php?issue=120604.1.

#include <iostream>
#include <cstdlib>
#include "abg-tools-utils.h"
#include "test-utils.h"

using std::cerr;
using std::string;

struct InOutSpec
{
  const char* in_elf_path;
  const char* debug_info_dir_path;
  const char* bidw_options;
  const char* in_report_path;
  const char* out_report_path;
};


InOutSpec in_out_specs[] =
{
  {
    "data/test-alt-dwarf-file/libtest0.so",
    "data/test-alt-dwarf-file/test0-debug-dir",
    "--check-alternate-debug-info-base-name",
    "data/test-alt-dwarf-file/test0-report.txt",
    "output/test-alt-dwarf-file/test0-report.txt"
  },
  {
    "data/test-alt-dwarf-file/libtest0-common.so",
    "data/test-alt-dwarf-file/test0-debug-dir",
    "--check-alternate-debug-info-base-name",
    "data/test-alt-dwarf-file/test0-report.txt",
    "output/test-alt-dwarf-file/test0-report.txt"
  },

  // This should always be the last entry
  {NULL, NULL, NULL, NULL, NULL}
};

int
main()
{
  using abigail::tests::get_src_dir;
  using abigail::tests::get_build_dir;
  using abigail::tools::ensure_parent_dir_created;

  bool is_ok = true;
  string in_elf_path, ref_report_path, out_report_path, debug_info_dir;
  string bidw, bidw_options;

  bidw = get_build_dir() + "/tools/bidw";
  for (InOutSpec* s = in_out_specs; s->in_elf_path; ++s)
    {
      bidw_options = s->bidw_options;
      in_elf_path = get_src_dir() + "/tests/" + s->in_elf_path;
      debug_info_dir = get_src_dir() + "/tests/" + s->debug_info_dir_path;
      ref_report_path = get_src_dir() + "/tests/" + s->in_report_path;
      out_report_path = get_build_dir() + "/tests/" + s->out_report_path;
      if (!ensure_parent_dir_created(out_report_path))
	{
	  cerr << "could not create parent directory for "
	       << out_report_path;
	  is_ok = false;
	  continue;
	}

      string cmd = bidw + " --debug-info-dir " + debug_info_dir
	+ " " + bidw_options + " " + in_elf_path + " > " + out_report_path;

      bool bidw_ok = true;
      if (system(cmd.c_str()))
	bidw_ok = false;

      if (bidw_ok)
	{
	  cmd = "diff -u " + ref_report_path + " " + out_report_path;
	  if (system(cmd.c_str()))
	    is_ok &=false;
	}
      else
	{
	  cerr << "command failed: " << cmd << "\n";
	  is_ok &= false;
	}
    }

  return !is_ok;
}