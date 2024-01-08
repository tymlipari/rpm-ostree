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

static inline std::pair<std::string_view, std::string_view>
split_filepath(std::string_view path)
{
  auto last_sep = path.rfind('/');
  if (last_sep == std::string_view::npos)
    return { std::string_view{}, path };
  else
    return { path.substr(0, last_sep), path.substr(last_sep) };
}

static inline std::pair<std::optional<ino_t>, std::string>
find_inode_for_dirname (std::string dirname, std::unordered_map<std::string, ino_t>* inode_cache)
{
  do
    {
      auto cache_itr = inode_cache->find (dirname);
      if (cache_itr != inode_cache->end ())
      {
        return { cache_itr->second, dirname };
      }

      struct stat finfo;
      if (stat (dirname.c_str (), &finfo) == 0)
        {
          (*inode_cache)[dirname] = finfo.st_ino;
          return std::pair { finfo.st_ino, dirname };
        }

      // Check the parent
      const auto& [newdirname, _] = split_filepath (dirname);
      dirname = newdirname;
    }
  while (!dirname.empty());

  return { std::nullopt , dirname };
}

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
RpmFileDb::packages_for_file (rust::Str path) const
{
  rust::Vec<rust::String> result;

  auto [dirname, basename] = split_filepath (std::string_view (path.data (), path.size ()));
  auto itr = basename_to_pkginfo.find (std::string (path.data (), path.size ()));
  if (itr != basename_to_pkginfo.end ())
    {
      std::optional<ino_t> dir_inode;

      if (use_fs_state)
        {
          const auto& [sel_inode, sel_dirname] = find_inode_for_dirname (std::string (dirname), &path_to_inode);
          dir_inode = sel_inode;
          dirname = sel_dirname;
        }

      for (const RpmFileDb::FilePackageInfo& info : itr->second)
        {
          if ((dir_inode && info.dir_inode == dir_inode) || (info.dirname == dirname))
            {
              result.push_back (info.pkg_nevra);
            }
        }
    }

  return result;
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

std::unique_ptr<RpmFileDb>
RpmTs::build_file_cache_from_rpmdb (bool use_fs_state) const
{
  std::unique_ptr<RpmFileDb> result = std::make_unique<RpmFileDb> ();
  result->use_fs_state = use_fs_state;

  // Walk the package set
  g_auto (rpmdbMatchIterator) mi = rpmtsInitIterator (_ts->ts, RPMDBI_PACKAGES, NULL, 0);
  if (mi == NULL)
    throw std::runtime_error ("Failed to read package set from rpmdb");

  Header h;
  while ((h = rpmdbNextIterator (mi)) != NULL)
    {
      auto pkg_nevra = rpmostreecxx::header_get_nevra (h);

      // Walk each file in the package and add it to the cache
      g_auto (rpmfi) fi = rpmfiNew (_ts->ts, h, 0, 0);
      if (fi == NULL)
        throw std::runtime_error ("Failed to create file iterator for package");

      rpmfiInit (fi, 0);
      while (rpmfiNext (fi) >= 0)
        {
          std::string basename = rpmfiBN (fi);
          std::string_view dirname = rpmfiDN (fi); 
          std::optional<ino_t> dirname_inode;

          if (use_fs_state && !dirname.empty())
            {
              auto const& [found_inode, found_path] = find_inode_for_dirname (std::string (dirname), &result->path_to_inode);
              dirname = found_path;
              dirname_inode = found_inode;

              // Log the path
              result->inode_to_path[*dirname_inode].emplace (dirname);
            }

          result->basename_to_pkginfo[basename].emplace_back(RpmFileDb::FilePackageInfo {
            pkg_nevra,
            std::string (dirname),
            dirname_inode,
          });
        }
    }

  return result;
}

}
