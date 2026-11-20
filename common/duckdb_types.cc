/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

#include "duckdb_types.h"
#include <stddef.h>
#include <cassert>
#include <cstring>

#include <my_global.h>
#include "m_string.h"
#include "sql_table.h"

static inline bool is_path_separator(char c)
{
  return c == '\\' || c == '/';
}

/* Refers to create_table_info_t::normalize_table_name */
DatabaseTableNames::DatabaseTableNames(const char *name)
{
  size_t total_len= strlen(name);
  size_t idx= total_len;

  /* seek to the last path separator */
  while (idx > 0 && !is_path_separator(name[idx - 1]))
    --idx;

  const char *name_ptr= name + idx;
  size_t name_len= total_len - idx;

  /* skip any number of path separators */
  while (idx > 0 && is_path_separator(name[idx - 1]))
    --idx;

  size_t db_end= idx;

  /* seek to the last but one path separator */
  while (idx > 0 && !is_path_separator(name[idx - 1]))
    --idx;

  const char *db_ptr= name + idx;
  size_t db_len= db_end - idx;

  std::string raw_table_name= std::string(name_ptr, name_len);
  std::string raw_db_name= std::string(db_ptr, db_len);

  /*
    When there are escape characters in the table name or database name
    (such as '-' is converted to '@002d'), we need to restore it to the
    original characters.

    Skip conversion for #sql- prefixed names (internal temp tables used
    during ALTER TABLE). filename_to_tablename() cannot handle the '#'
    prefix and would mangle the name. In AliSQL this is not an issue
    because dd::Table provides the clean name directly.
  */
  if (strncmp(raw_table_name.c_str(), "#sql", 4) == 0)
  {
    table_name= raw_table_name;
  }
  else
  {
    char ori_table_name[NAME_LEN + 1];
    size_t tbl_name_length= filename_to_tablename(
        raw_table_name.c_str(), ori_table_name, sizeof(ori_table_name));
    table_name= std::string(ori_table_name, tbl_name_length);
  }

  char ori_db_name[NAME_LEN + 1];
  size_t db_name_length= filename_to_tablename(
      raw_db_name.c_str(), ori_db_name, sizeof(ori_db_name));
  db_name= std::string(ori_db_name, db_name_length);
}

std::string quote_duckdb_identifier(const char *name, size_t length)
{
  std::string out;
  out.reserve(length + 2);
  out.push_back('"');
  for (size_t i= 0; i < length; i++)
  {
    if (name[i] == '"')
      out.push_back('"');
    out.push_back(name[i]);
  }
  out.push_back('"');
  return out;
}

Databasename::Databasename(const char *path_name)
{
  char dbname[FN_REFLEN];
  char tmp_buff[FN_REFLEN + 1];

  size_t len= strlen(path_name);
  size_t end_idx= len;
  while (end_idx > 0 && !is_path_separator(path_name[end_idx - 1]))
    --end_idx;
  if (end_idx > 0)
    --end_idx;
  size_t start_idx= end_idx;
  while (start_idx > 0 && !is_path_separator(path_name[start_idx - 1]))
    --start_idx;

  size_t name_len= end_idx - start_idx;
  if (name_len > FN_REFLEN)
    name_len= FN_REFLEN;
  if (name_len > 0)
    memcpy(tmp_buff, path_name + start_idx, name_len);
  tmp_buff[name_len]= '\0';

  size_t db_len= filename_to_tablename(tmp_buff, dbname, sizeof(tmp_buff) - 1);
  name= std::string(dbname, db_len);
}
