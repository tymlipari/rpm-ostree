/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "rpmostree-refts.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-util.h"
#include <rpm/header.h>
#include <rpm/rpmtag.h>
#include <string.h>
#include <unordered_set>

static inline void
cleanup_rpmtdFreeData (rpmtd *tdp)
{
  rpmtd td = *tdp;
  if (td)
    rpmtdFreeData (td);
}
#define _cleanup_rpmtddata_ __attribute__ ((cleanup (cleanup_rpmtdFreeData)))

/*
 * A wrapper for an `rpmts` that supports:
 *
 *  - Reference counting
 *  - Possibly holding a pointer to a tempdir, and cleaning it when unref'd
 */

RpmOstreeRefTs *
rpmostree_refts_new (rpmts ts, GLnxTmpDir *tmpdir)
{
  RpmOstreeRefTs *rts = g_new0 (RpmOstreeRefTs, 1);
  rts->ts = ts;
  rts->refcount = 1;
  if (tmpdir)
    {
      rts->tmpdir = *tmpdir;
      tmpdir->initialized = FALSE; /* Steal ownership */
    }
  return rts;
}

RpmOstreeRefTs *
rpmostree_refts_ref (RpmOstreeRefTs *rts)
{
  g_atomic_int_inc (&rts->refcount);
  return rts;
}

void
rpmostree_refts_unref (RpmOstreeRefTs *rts)
{
  if (!g_atomic_int_dec_and_test (&rts->refcount))
    return;
  rpmtsFree (rts->ts);
  (void)glnx_tmpdir_delete (&rts->tmpdir, NULL, NULL);
  g_free (rts);
}

namespace rpmostreecxx
{

rust::Vec<rust::String>
FileToPackageMap::packages_for_file (const OstreeRepoFile& file) const
{
  GFile* gfile = G_FILE (&file);
  g_autoptr(GFile) parentfile = g_file_get_parent (gfile);

  // Check to see if this file's directory was remapped
  // on the file system
  std::optional<std::string_view> remapped_dirname;
  if (parentfile != NULL)
    {
      std::string dirname = g_file_peek_path (parentfile);

      auto remapitr = _remapped_paths.find (dirname);
      if (remapitr != _remapped_paths.end ())
        remapped_dirname = std::string_view (remapitr->second);
    }

  // Determine the hash for the fs path, using the remapped parent
  // if necessary
  size_t path_hash;
  if (remapped_dirname)
    {
      g_autofree const char* basename = g_file_get_basename (gfile);
      auto remapped_path = std::string(*remapped_dirname) + "/" + basename;
      path_hash = std::hash<std::string>{} (remapped_path);
    }
  else
    {
      path_hash = std::hash<std::string_view>{} (g_file_peek_path (gfile));
    }

  // Return the contents of our cache hit
  rust::Vec<rust::String> ret_pkgs;
  auto cacheitr = _path_hash_to_pkgs.find (path_hash);
  if (cacheitr != _path_hash_to_pkgs.end ())
    {
      for (const rust::String& pkgid : cacheitr->second)
        ret_pkgs.emplace_back (pkgid);
    }
  return ret_pkgs;
}

RpmTs::RpmTs (RpmOstreeRefTs *ts) { _ts = ts; }

RpmTs::~RpmTs () { rpmostree_refts_unref (_ts); }

rust::Vec<rust::String>
RpmTs::packages_providing_file (const rust::Str path) const
{
  auto path_c = std::string (path);
  g_auto (rpmdbMatchIterator) mi
      = rpmtsInitIterator (_ts->ts, RPMDBI_INSTFILENAMES, path_c.c_str (), 0);
  if (mi == NULL)
    mi = rpmtsInitIterator (_ts->ts, RPMDBI_PROVIDENAME, path_c.c_str (), 0);
  rust::Vec<rust::String> ret;
  if (mi != NULL)
    {
      Header h;
      while ((h = rpmdbNextIterator (mi)) != NULL)
        {
          ret.push_back (rpmostreecxx::header_get_nevra (h));
        }
    }
  return ret;
}

std::unique_ptr<PackageMeta>
RpmTs::package_meta (const rust::Str name) const
{
  auto name_c = std::string (name);
  g_auto (rpmdbMatchIterator) mi = rpmtsInitIterator (_ts->ts, RPMDBI_NAME, name_c.c_str (), 0);
  if (mi == NULL)
    {
      g_autofree char *err = g_strdup_printf ("Package not found: %s", name_c.c_str ());
      throw std::runtime_error (err);
    }
  Header h;
  std::optional<rust::String> previous;
  auto retval = std::make_unique<PackageMeta> ();
  while ((h = rpmdbNextIterator (mi)) != NULL)
    {
      auto nevra = rpmostreecxx::header_get_nevra (h);
      if (!previous.has_value ())
        {
          previous = std::move (nevra);
          retval->_size = headerGetNumber (h, RPMTAG_LONGARCHIVESIZE);
          retval->_buildtime = headerGetNumber (h, RPMTAG_BUILDTIME);
          retval->_src_pkg = headerGetString (h, RPMTAG_SOURCERPM);

          // Get the changelogs
          struct rpmtd_s nchanges_date_s;
          _cleanup_rpmtddata_ rpmtd nchanges_date = NULL;
          nchanges_date = &nchanges_date_s;
          headerGet (h, RPMTAG_CHANGELOGTIME, nchanges_date, HEADERGET_MINMEM);
          int ncnum = rpmtdCount (nchanges_date);
          rust::Vec<uint64_t> epochs;
          for (int i = 0; i < ncnum; i++)
            {
              uint64_t nchange_date = 0;
              rpmtdNext (nchanges_date);
              nchange_date = rpmtdGetNumber (nchanges_date);
              epochs.push_back (nchange_date);
            }
          retval->_changelogs = std::move (epochs);
        }
      else
        {
          // TODO: Somehow we get two `libgcc-8.5.0-10.el8.x86_64` in current RHCOS, I don't
          // understand that.
          if (previous != nevra)
            {
              g_autofree char *buf
                  = g_strdup_printf ("Multiple installed '%s' (%s, %s)", name_c.c_str (),
                                     previous.value ().c_str (), nevra.c_str ());
              throw std::runtime_error (buf);
            }
        }
    }
  if (!previous)
    g_assert_not_reached ();
  return retval;
}

std::unique_ptr<FileToPackageMap>
RpmTs::build_file_to_pkg_map (const OstreeRepoFile& fsroot) const
{
  std::unique_ptr<FileToPackageMap> result = std::make_unique<FileToPackageMap> ();

  GFile* fsroot_g = G_FILE (&fsroot);

  g_auto (rpmdbMatchIterator) mi = rpmtsInitIterator (_ts->ts, RPMDBI_PACKAGES, NULL, 0);
  if (mi == NULL)
      throw std::runtime_error ("Failed to read rpmdb");

  Header h;
  while ((h = rpmdbNextIterator (mi)) != NULL)
    {
      rust::String pkg_nevra = rpmostreecxx::header_get_nevra (h);

      g_auto (rpmfi) fileitr = rpmfiNew (_ts->ts, h, 0, 0);
      if (fileitr == NULL)
        throw std::runtime_error ("Couldn't create file iterator");

      std::unordered_set<std::string> checked_paths;
      rpmfiInit (fileitr, 0);
      while (rpmfiNext (fileitr) >= 0)
        {
          rpm_mode_t fmode = rpmfiFMode (fileitr);

          if (RPMFILE_IS_INSTALLED (rpmfiFState (fileitr)) && !S_ISDIR (fmode))
            {
              // Check to see if the dirname is remapped on the system
              const char* dirname = rpmfiDN (fileitr);
              if (checked_paths.find (dirname) != checked_paths.end ())
                {
                  g_autoptr (GFile) child_path = g_file_get_child (fsroot_g, dirname);
                  g_autoptr (GFileInfo) child_info = g_file_query_info (child_path, "*", G_FILE_QUERY_INFO_NONE, NULL, NULL);
                  
                  if (child_info == NULL)
                    throw std::runtime_error ("Failed to get file info");

                  printf ("DEBUG -- FINFO -- %s: %d\n", dirname, g_file_info_get_file_type (child_info));

                  checked_paths.insert (dirname);
                }

              size_t path_hash = std::hash<std::string_view>{} (rpmfiFN (fileitr));
              result->_path_hash_to_pkgs[path_hash].insert (pkg_nevra);
            }
        }
    }

    return result;
}

}
