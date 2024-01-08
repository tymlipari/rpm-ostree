/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, In.c
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#pragma once

#include "libglnx.h"
#include "rpmostree-util.h"
#include "rust/cxx.h"
#include <gio/gio.h>
#include <libdnf/libdnf.h>
#include <memory>
#include <set>
#include <unordered_map>

G_BEGIN_DECLS

typedef struct
{
  gint refcount; /* atomic */
  rpmts ts;
  GLnxTmpDir tmpdir;
} RpmOstreeRefTs;

RpmOstreeRefTs *rpmostree_refts_new (rpmts ts, GLnxTmpDir *tmpdir);

RpmOstreeRefTs *rpmostree_refts_ref (RpmOstreeRefTs *rts);

void rpmostree_refts_unref (RpmOstreeRefTs *rts);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (RpmOstreeRefTs, rpmostree_refts_unref);

namespace rpmostreecxx
{

struct PackageMeta
{
  uint64_t _size;
  uint64_t _buildtime;
  rust::Vec<uint64_t> _changelogs;
  std::string _src_pkg;

  uint64_t
  size () const
  {
    return _size;
  };
  uint64_t
  buildtime () const
  {
    return _buildtime;
  };
  rust::Vec<uint64_t>
  changelogs () const
  {
    return _changelogs;
  };

  const std::string &
  src_pkg () const
  {
    return _src_pkg;
  };
};

struct RpmFileDb
{
  struct FilePackageInfo
  {
    rust::String pkg_nevra;
    std::string dirname;
    std::optional<ino_t> dir_inode;
  };
  std::unordered_map<std::string, std::vector<FilePackageInfo>> basename_to_pkginfo;

  bool use_fs_state;
  std::unordered_map<ino_t, std::set<std::string>> inode_to_path;
  mutable std::unordered_map<std::string, ino_t> path_to_inode;

  rust::Vec<rust::String>
  packages_for_file (rust::Str path) const;
};

// A simple C++ wrapper for a librpm C type, so we can expose it to Rust via cxx.rs.
class RpmTs
{
public:
  RpmTs (::RpmOstreeRefTs *ts);
  ~RpmTs ();
  rpmts get_ts () const;
  rust::Vec<rust::String> packages_providing_file (const rust::Str path) const;
  std::unique_ptr<PackageMeta> package_meta (const rust::Str package) const;
  std::unique_ptr<RpmFileDb> build_file_cache_from_rpmdb (bool use_fs_state) const;

private:
  ::RpmOstreeRefTs *_ts;
};

}

G_END_DECLS
